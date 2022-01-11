/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup edrend
 */

/* global includes */

#include <cmath>
#include <cstdlib>
#include <cstring>

#ifndef WIN32
#  include <unistd.h>
#else
#  include <io.h>
#endif
#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "PIL_time.h"

#include "BLO_readfile.h"

#include "DNA_brush_types.h"
#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_world_types.h"

#include "BKE_animsys.h"
#include "BKE_appdir.h"
#include "BKE_armature.h"
#include "BKE_brush.h"
#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_icons.h"
#include "BKE_idprop.h"
#include "BKE_image.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_light.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_node.h"
#include "BKE_object.h"
#include "BKE_scene.h"
#include "BKE_screen.h"
#include "BKE_texture.h"
#include "BKE_world.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"
#include "IMB_thumbs.h"

#include "BIF_glutil.h"

#include "GPU_shader.h"

#include "RE_engine.h"
#include "RE_pipeline.h"
#include "RE_texture.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_armature.h"
#include "ED_datafiles.h"
#include "ED_render.h"
#include "ED_screen.h"
#include "ED_view3d.h"
#include "ED_view3d_offscreen.h"

#include "UI_interface_icons.h"

#ifndef NDEBUG
/* Used for database init assert(). */
#  include "BLI_threads.h"
#endif

static void icon_copy_rect(ImBuf *ibuf, uint w, uint h, uint *rect);

/* -------------------------------------------------------------------- */
/** \name Local Structs
 * \{ */

struct ShaderPreview {
  /* from wmJob */
  void *owner;
  short *stop, *do_update;

  Scene *scene;
  ID *id, *id_copy;
  ID *parent;
  MTex *slot;

  /* datablocks with nodes need full copy during preview render, glsl uses it too */
  Material *matcopy;
  Tex *texcopy;
  Light *lampcopy;
  World *worldcopy;

  /** Copy of the active objects #Object.color */
  float color[4];

  int sizex, sizey;
  uint *pr_rect;
  ePreviewRenderMethod pr_method;
  bool own_id_copy;

  Main *bmain;
  Main *pr_main;
};

struct IconPreviewSize {
  struct IconPreviewSize *next, *prev;
  int sizex, sizey;
  uint *rect;
};

struct IconPreview {
  Main *bmain;
  Depsgraph *depsgraph; /* May be nullptr (see #WM_OT_previews_ensure). */
  Scene *scene;
  void *owner;
  ID *id,
      *id_copy; /* May be nullptr! (see ICON_TYPE_PREVIEW case in #ui_icon_ensure_deferred()) */
  ListBase sizes;

  /* May be nullptr, is used for rendering IDs that require some other object for it to be applied
   * on before the ID can be represented as an image, for example when rendering an Action. */
  struct Object *active_object;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preview for Buttons
 * \{ */

static Main *G_pr_main = nullptr;
static Main *G_pr_main_grease_pencil = nullptr;

#ifndef WITH_HEADLESS
static Main *load_main_from_memory(const void *blend, int blend_size)
{
  const int fileflags = G.fileflags;
  Main *bmain = nullptr;
  BlendFileData *bfd;

  G.fileflags |= G_FILE_NO_UI;
  bfd = BLO_read_from_memory(blend, blend_size, BLO_READ_SKIP_NONE, nullptr);
  if (bfd) {
    bmain = bfd->main;

    MEM_freeN(bfd);
  }
  G.fileflags = fileflags;

  return bmain;
}
#endif

void ED_preview_ensure_dbase()
{
#ifndef WITH_HEADLESS
  static bool base_initialized = false;
  BLI_assert(BLI_thread_is_main());
  if (!base_initialized) {
    G_pr_main = load_main_from_memory(datatoc_preview_blend, datatoc_preview_blend_size);
    G_pr_main_grease_pencil = load_main_from_memory(datatoc_preview_grease_pencil_blend,
                                                    datatoc_preview_grease_pencil_blend_size);
    base_initialized = true;
  }
#endif
}

static bool check_engine_supports_preview(Scene *scene)
{
  RenderEngineType *type = RE_engines_find(scene->r.engine);
  return (type->flag & RE_USE_PREVIEW) != 0;
}

static bool preview_method_is_render(const ePreviewRenderMethod pr_method)
{
  return ELEM(pr_method, PR_ICON_RENDER, PR_BUTS_RENDER);
}

void ED_preview_free_dbase()
{
  if (G_pr_main) {
    BKE_main_free(G_pr_main);
  }

  if (G_pr_main_grease_pencil) {
    BKE_main_free(G_pr_main_grease_pencil);
  }
}

static Scene *preview_get_scene(Main *pr_main)
{
  if (pr_main == nullptr) {
    return nullptr;
  }

  return static_cast<Scene *>(pr_main->scenes.first);
}

static const char *preview_collection_name(const ePreviewType pr_type)
{
  switch (pr_type) {
    case MA_FLAT:
      return "Flat";
    case MA_SPHERE:
      return "Sphere";
    case MA_CUBE:
      return "Cube";
    case MA_SHADERBALL:
      return "Shader Ball";
    case MA_CLOTH:
      return "Cloth";
    case MA_FLUID:
      return "Fluid";
    case MA_SPHERE_A:
      return "World Sphere";
    case MA_LAMP:
      return "Lamp";
    case MA_SKY:
      return "Sky";
    case MA_HAIR:
      return "Hair";
    case MA_ATMOS:
      return "Atmosphere";
    default:
      BLI_assert_msg(0, "Unknown preview type");
      return "";
  }
}

static bool render_engine_supports_ray_visibility(const Scene *sce)
{
  return !STREQ(sce->r.engine, RE_engine_id_BLENDER_EEVEE);
}

static void switch_preview_collection_visibilty(ViewLayer *view_layer, const ePreviewType pr_type)
{
  /* Set appropriate layer as visible. */
  LayerCollection *lc = static_cast<LayerCollection *>(view_layer->layer_collections.first);
  const char *collection_name = preview_collection_name(pr_type);

  for (lc = static_cast<LayerCollection *>(lc->layer_collections.first); lc; lc = lc->next) {
    if (STREQ(lc->collection->id.name + 2, collection_name)) {
      lc->collection->flag &= ~COLLECTION_HIDE_RENDER;
    }
    else {
      lc->collection->flag |= COLLECTION_HIDE_RENDER;
    }
  }
}

static const char *preview_floor_material_name(const Scene *scene,
                                               const ePreviewRenderMethod pr_method)
{
  if (pr_method == PR_ICON_RENDER && render_engine_supports_ray_visibility(scene)) {
    return "FloorHidden";
  }
  return "Floor";
}

static void switch_preview_floor_material(Main *pr_main,
                                          Mesh *me,
                                          const Scene *scene,
                                          const ePreviewRenderMethod pr_method)
{
  if (me->totcol == 0) {
    return;
  }

  const char *material_name = preview_floor_material_name(scene, pr_method);
  Material *mat = static_cast<Material *>(
      BLI_findstring(&pr_main->materials, material_name, offsetof(ID, name) + 2));
  if (mat) {
    me->mat[0] = mat;
  }
}

static void switch_preview_floor_visibility(Main *pr_main,
                                            const Scene *scene,
                                            ViewLayer *view_layer,
                                            const ePreviewRenderMethod pr_method)
{
  /* Hide floor for icon renders. */
  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    if (STREQ(base->object->id.name + 2, "Floor")) {
      base->object->visibility_flag &= ~OB_HIDE_RENDER;
      if (pr_method == PR_ICON_RENDER) {
        if (!render_engine_supports_ray_visibility(scene)) {
          base->object->visibility_flag |= OB_HIDE_RENDER;
        }
      }
      if (base->object->type == OB_MESH) {
        switch_preview_floor_material(
            pr_main, static_cast<Mesh *>(base->object->data), scene, pr_method);
      }
    }
  }
}

static void set_preview_visibility(Main *pr_main,
                                   Scene *scene,
                                   ViewLayer *view_layer,
                                   const ePreviewType pr_type,
                                   const ePreviewRenderMethod pr_method)
{
  switch_preview_collection_visibilty(view_layer, pr_type);
  switch_preview_floor_visibility(pr_main, scene, view_layer, pr_method);
  BKE_layer_collection_sync(scene, view_layer);
}

static World *preview_get_localized_world(ShaderPreview *sp, World *world)
{
  if (world == nullptr) {
    return nullptr;
  }
  if (sp->worldcopy != nullptr) {
    return sp->worldcopy;
  }

  ID *id_copy = BKE_id_copy_ex(nullptr,
                               &world->id,
                               nullptr,
                               LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE |
                                   LIB_ID_COPY_NO_ANIMDATA);
  sp->worldcopy = (World *)id_copy;
  BLI_addtail(&sp->pr_main->worlds, sp->worldcopy);
  return sp->worldcopy;
}

static ID *duplicate_ids(ID *id, const bool allow_failure)
{
  if (id == nullptr) {
    /* Non-ID preview render. */
    return nullptr;
  }

  switch (GS(id->name)) {
    case ID_OB:
    case ID_MA:
    case ID_TE:
    case ID_LA:
    case ID_WO: {
      BLI_assert(BKE_previewimg_id_supports_jobs(id));
      ID *id_copy = BKE_id_copy_ex(nullptr,
                                   id,
                                   nullptr,
                                   LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE |
                                       LIB_ID_COPY_NO_ANIMDATA);
      return id_copy;
    }
    /* These support threading, but don't need duplicating. */
    case ID_IM:
    case ID_BR:
      BLI_assert(BKE_previewimg_id_supports_jobs(id));
      return nullptr;
    default:
      if (!allow_failure) {
        BLI_assert_msg(0, "ID type preview not supported.");
      }
      return nullptr;
  }
}

static const char *preview_world_name(const Scene *sce,
                                      const ID_Type id_type,
                                      const ePreviewRenderMethod pr_method)
{
  /* When rendering material icons the floor will not be shown in the output. Cycles will use a
   * material trick to show the floor in the reflections, but hide the floor for camera rays. For
   * Eevee we use a transparent world that has a projected grid.
   *
   * In the future when Eevee supports VULKAN ray-tracing we can re-evaluate and perhaps remove
   * this approximation.
   */
  if (id_type == ID_MA && pr_method == PR_ICON_RENDER &&
      !render_engine_supports_ray_visibility(sce)) {
    return "WorldFloor";
  }
  return "World";
}

static World *preview_get_world(Main *pr_main,
                                const Scene *sce,
                                const ID_Type id_type,
                                const ePreviewRenderMethod pr_method)
{
  World *result = nullptr;
  const char *world_name = preview_world_name(sce, id_type, pr_method);
  result = static_cast<World *>(
      BLI_findstring(&pr_main->worlds, world_name, offsetof(ID, name) + 2));

  /* No world found return first world. */
  if (result == nullptr) {
    result = static_cast<World *>(pr_main->worlds.first);
  }

  BLI_assert_msg(result, "Preview file has no world.");
  return result;
}

static void preview_sync_exposure(World *dst, const World *src)
{
  BLI_assert(dst);
  BLI_assert(src);
  dst->exp = src->exp;
  dst->range = src->range;
}

static World *preview_prepare_world(Main *pr_main,
                                    const Scene *sce,
                                    const World *world,
                                    const ID_Type id_type,
                                    const ePreviewRenderMethod pr_method)
{
  World *result = preview_get_world(pr_main, sce, id_type, pr_method);
  if (world) {
    preview_sync_exposure(result, world);
  }
  return result;
}

/* call this with a pointer to initialize preview scene */
/* call this with nullptr to restore assigned ID pointers in preview scene */
static Scene *preview_prepare_scene(
    Main *bmain, Scene *scene, ID *id, int id_type, ShaderPreview *sp)
{
  Scene *sce;
  Main *pr_main = sp->pr_main;

  memcpy(pr_main->filepath, BKE_main_blendfile_path(bmain), sizeof(pr_main->filepath));

  sce = preview_get_scene(pr_main);
  if (sce) {
    ViewLayer *view_layer = static_cast<ViewLayer *>(sce->view_layers.first);

    /* Only enable the combined renderpass */
    view_layer->passflag = SCE_PASS_COMBINED;
    view_layer->eevee.render_passes = 0;

    /* this flag tells render to not execute depsgraph or ipos etc */
    sce->r.scemode |= R_BUTS_PREVIEW;
    BLI_strncpy(sce->r.engine, scene->r.engine, sizeof(sce->r.engine));

    sce->r.color_mgt_flag = scene->r.color_mgt_flag;
    BKE_color_managed_display_settings_copy(&sce->display_settings, &scene->display_settings);

    BKE_color_managed_view_settings_free(&sce->view_settings);
    BKE_color_managed_view_settings_copy(&sce->view_settings, &scene->view_settings);

    if ((id && sp->pr_method == PR_ICON_RENDER) && id_type != ID_WO) {
      sce->r.alphamode = R_ALPHAPREMUL;
    }
    else {
      sce->r.alphamode = R_ADDSKY;
    }

    sce->r.cfra = scene->r.cfra;

    /* Setup the world. */
    sce->world = preview_prepare_world(
        pr_main, sce, scene->world, static_cast<ID_Type>(id_type), sp->pr_method);

    if (id_type == ID_TE) {
      /* Texture is not actually rendered with engine, just set dummy value. */
      BLI_strncpy(sce->r.engine, RE_engine_id_BLENDER_EEVEE, sizeof(sce->r.engine));
    }

    if (id_type == ID_MA) {
      Material *mat = nullptr, *origmat = (Material *)id;

      if (origmat) {
        /* work on a copy */
        BLI_assert(sp->id_copy != nullptr);
        mat = sp->matcopy = (Material *)sp->id_copy;
        sp->id_copy = nullptr;
        BLI_addtail(&pr_main->materials, mat);

        /* Use current scene world for lighting. */
        if (mat->pr_flag == MA_PREVIEW_WORLD && sp->pr_method == PR_BUTS_RENDER) {
          /* Use current scene world to light sphere. */
          sce->world = preview_get_localized_world(sp, scene->world);
        }
        else if (sce->world && sp->pr_method != PR_ICON_RENDER) {
          /* Use a default world color. Using the current
           * scene world can be slow if it has big textures. */
          sce->world->use_nodes = false;
          sce->world->horr = 0.05f;
          sce->world->horg = 0.05f;
          sce->world->horb = 0.05f;
        }

        /* For grease pencil, always use sphere for icon renders. */
        const ePreviewType preview_type = static_cast<ePreviewType>(
            (sp->pr_method == PR_ICON_RENDER && sp->pr_main == G_pr_main_grease_pencil) ?
                MA_SPHERE_A :
                (ePreviewType)mat->pr_type);
        set_preview_visibility(pr_main, sce, view_layer, preview_type, sp->pr_method);
      }
      else {
        sce->display.render_aa = SCE_DISPLAY_AA_OFF;
      }

      LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
        if (base->object->id.name[2] == 'p') {
          /* copy over object color, in case material uses it */
          copy_v4_v4(base->object->color, sp->color);

          if (OB_TYPE_SUPPORT_MATERIAL(base->object->type)) {
            /* don't use BKE_object_material_assign, it changed mat->id.us, which shows in the UI
             */
            Material ***matar = BKE_object_material_array_p(base->object);
            int actcol = max_ii(base->object->actcol - 1, 0);

            if (matar && actcol < base->object->totcol) {
              (*matar)[actcol] = mat;
            }
          }
          else if (base->object->type == OB_LAMP) {
            base->flag |= BASE_VISIBLE_DEPSGRAPH;
          }
        }
      }
    }
    else if (id_type == ID_TE) {
      Tex *tex = nullptr, *origtex = (Tex *)id;

      if (origtex) {
        BLI_assert(sp->id_copy != nullptr);
        tex = sp->texcopy = (Tex *)sp->id_copy;
        sp->id_copy = nullptr;
        BLI_addtail(&pr_main->textures, tex);
      }
    }
    else if (id_type == ID_LA) {
      Light *la = nullptr, *origla = (Light *)id;

      /* work on a copy */
      if (origla) {
        BLI_assert(sp->id_copy != nullptr);
        la = sp->lampcopy = (Light *)sp->id_copy;
        sp->id_copy = nullptr;
        BLI_addtail(&pr_main->lights, la);
      }

      set_preview_visibility(pr_main, sce, view_layer, MA_LAMP, sp->pr_method);

      if (sce->world) {
        /* Only use lighting from the light. */
        sce->world->use_nodes = false;
        sce->world->horr = 0.0f;
        sce->world->horg = 0.0f;
        sce->world->horb = 0.0f;
      }

      LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
        if (base->object->id.name[2] == 'p') {
          if (base->object->type == OB_LAMP) {
            base->object->data = la;
          }
        }
      }
    }
    else if (id_type == ID_WO) {
      World *wrld = nullptr, *origwrld = (World *)id;

      if (origwrld) {
        BLI_assert(sp->id_copy != nullptr);
        wrld = sp->worldcopy = (World *)sp->id_copy;
        sp->id_copy = nullptr;
        BLI_addtail(&pr_main->worlds, wrld);
      }

      set_preview_visibility(pr_main, sce, view_layer, MA_SKY, sp->pr_method);
      sce->world = wrld;
    }

    return sce;
  }

  return nullptr;
}

/* new UI convention: draw is in pixel space already. */
/* uses UI_BTYPE_ROUNDBOX button in block to get the rect */
static bool ed_preview_draw_rect(ScrArea *area, int split, int first, rcti *rect, rcti *newrect)
{
  Render *re;
  RenderView *rv;
  RenderResult rres;
  char name[32];
  int offx = 0;
  int newx = BLI_rcti_size_x(rect);
  int newy = BLI_rcti_size_y(rect);
  bool ok = false;

  if (!split || first) {
    sprintf(name, "Preview %p", (void *)area);
  }
  else {
    sprintf(name, "SecondPreview %p", (void *)area);
  }

  if (split) {
    if (first) {
      offx = 0;
      newx = newx / 2;
    }
    else {
      offx = newx / 2;
      newx = newx - newx / 2;
    }
  }

  /* test if something rendered ok */
  re = RE_GetRender(name);

  if (re == nullptr) {
    return false;
  }

  RE_AcquireResultImageViews(re, &rres);

  if (!BLI_listbase_is_empty(&rres.views)) {
    /* material preview only needs monoscopy (view 0) */
    rv = RE_RenderViewGetById(&rres, 0);
  }
  else {
    /* possible the job clears the views but we're still drawing T45496 */
    rv = nullptr;
  }

  if (rv && rv->rectf) {

    if (abs(rres.rectx - newx) < 2 && abs(rres.recty - newy) < 2) {

      newrect->xmax = max_ii(newrect->xmax, rect->xmin + rres.rectx + offx);
      newrect->ymax = max_ii(newrect->ymax, rect->ymin + rres.recty);

      if (rres.rectx && rres.recty) {
        uchar *rect_byte = static_cast<uchar *>(
            MEM_mallocN(rres.rectx * rres.recty * sizeof(int), "ed_preview_draw_rect"));
        float fx = rect->xmin + offx;
        float fy = rect->ymin;

        /* material preview only needs monoscopy (view 0) */
        RE_AcquiredResultGet32(re, &rres, (uint *)rect_byte, 0);

        IMMDrawPixelsTexState state = immDrawPixelsTexSetup(GPU_SHADER_2D_IMAGE_COLOR);
        immDrawPixelsTex(&state,
                         fx,
                         fy,
                         rres.rectx,
                         rres.recty,
                         GPU_RGBA8,
                         false,
                         rect_byte,
                         1.0f,
                         1.0f,
                         nullptr);

        MEM_freeN(rect_byte);

        ok = true;
      }
    }
  }

  RE_ReleaseResultImageViews(re, &rres);

  return ok;
}

void ED_preview_draw(const bContext *C, void *idp, void *parentp, void *slotp, rcti *rect)
{
  if (idp) {
    wmWindowManager *wm = CTX_wm_manager(C);
    ScrArea *area = CTX_wm_area(C);
    ID *id = (ID *)idp;
    ID *parent = (ID *)parentp;
    MTex *slot = (MTex *)slotp;
    SpaceProperties *sbuts = CTX_wm_space_properties(C);
    ShaderPreview *sp = static_cast<ShaderPreview *>(WM_jobs_customdata(wm, area));
    rcti newrect;
    bool ok;
    int newx = BLI_rcti_size_x(rect);
    int newy = BLI_rcti_size_y(rect);

    newrect.xmin = rect->xmin;
    newrect.xmax = rect->xmin;
    newrect.ymin = rect->ymin;
    newrect.ymax = rect->ymin;

    if (parent) {
      ok = ed_preview_draw_rect(area, 1, 1, rect, &newrect);
      ok &= ed_preview_draw_rect(area, 1, 0, rect, &newrect);
    }
    else {
      ok = ed_preview_draw_rect(area, 0, 0, rect, &newrect);
    }

    if (ok) {
      *rect = newrect;
    }

    /* start a new preview render job if signaled through sbuts->preview,
     * if no render result was found and no preview render job is running,
     * or if the job is running and the size of preview changed */
    if ((sbuts != nullptr && sbuts->preview) ||
        (!ok && !WM_jobs_test(wm, area, WM_JOB_TYPE_RENDER_PREVIEW)) ||
        (sp && (abs(sp->sizex - newx) >= 2 || abs(sp->sizey - newy) > 2))) {
      if (sbuts != nullptr) {
        sbuts->preview = 0;
      }
      ED_preview_shader_job(C, area, id, parent, slot, newx, newy, PR_BUTS_RENDER);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Preview
 * \{ */

struct ObjectPreviewData {
  /* The main for the preview, not of the current file. */
  Main *pr_main;
  /* Copy of the object to create the preview for. The copy is for thread safety (and to insert
   * it into an own main). */
  Object *object;
  /* Current frame. */
  int cfra;
  int sizex;
  int sizey;
};

static bool object_preview_is_type_supported(const Object *ob)
{
  return OB_TYPE_IS_GEOMETRY(ob->type);
}

static Object *object_preview_camera_create(Main *preview_main,
                                            ViewLayer *view_layer,
                                            Object *preview_object)
{
  Object *camera = BKE_object_add(preview_main, view_layer, OB_CAMERA, "Preview Camera");

  float rotmat[3][3];
  float dummyscale[3];
  mat4_to_loc_rot_size(camera->loc, rotmat, dummyscale, preview_object->obmat);

  /* Camera is Y up, so needs additional rotations to obliquely face the front. */
  float drotmat[3][3];
  const float eul[3] = {M_PI * 0.4f, 0.0f, M_PI * 0.1f};
  eul_to_mat3(drotmat, eul);
  mul_m3_m3_post(rotmat, drotmat);

  camera->rotmode = ROT_MODE_QUAT;
  mat3_to_quat(camera->quat, rotmat);

  /* Nice focal length for close portraiture. */
  ((Camera *)camera->data)->lens = 85;

  return camera;
}

static Scene *object_preview_scene_create(const struct ObjectPreviewData *preview_data,
                                          Depsgraph **r_depsgraph)
{
  Scene *scene = BKE_scene_add(preview_data->pr_main, "Object preview scene");
  /* Preview need to be in the current frame to get a thumbnail similar of what
   * viewport displays. */
  CFRA = preview_data->cfra;

  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  Depsgraph *depsgraph = DEG_graph_new(
      preview_data->pr_main, scene, view_layer, DAG_EVAL_VIEWPORT);

  BLI_assert(preview_data->object != nullptr);
  BLI_addtail(&preview_data->pr_main->objects, preview_data->object);

  BKE_collection_object_add(preview_data->pr_main, scene->master_collection, preview_data->object);

  Object *camera_object = object_preview_camera_create(
      preview_data->pr_main, view_layer, preview_data->object);

  scene->camera = camera_object;
  scene->r.xsch = preview_data->sizex;
  scene->r.ysch = preview_data->sizey;
  scene->r.size = 100;

  Base *preview_base = BKE_view_layer_base_find(view_layer, preview_data->object);
  /* For 'view selected' below. */
  preview_base->flag |= BASE_SELECTED;

  DEG_graph_build_from_view_layer(depsgraph);
  DEG_evaluate_on_refresh(depsgraph);

  ED_view3d_camera_to_view_selected(preview_data->pr_main, depsgraph, scene, camera_object);

  BKE_scene_graph_update_tagged(depsgraph, preview_data->pr_main);

  *r_depsgraph = depsgraph;
  return scene;
}

static void object_preview_render(IconPreview *preview, IconPreviewSize *preview_sized)
{
  Main *preview_main = BKE_main_new();
  char err_out[256] = "unknown";

  BLI_assert(preview->id_copy && (preview->id_copy != preview->id));

  struct ObjectPreviewData preview_data = {};
  preview_data.pr_main = preview_main;
  /* Act on a copy. */
  preview_data.object = (Object *)preview->id_copy;
  preview_data.cfra = preview->scene->r.cfra;
  preview_data.sizex = preview_sized->sizex;
  preview_data.sizey = preview_sized->sizey;

  Depsgraph *depsgraph;
  Scene *scene = object_preview_scene_create(&preview_data, &depsgraph);

  /* Ownership is now ours. */
  preview->id_copy = nullptr;

  View3DShading shading;
  BKE_screen_view3d_shading_init(&shading);
  /* Enable shadows, makes it a bit easier to see the shape. */
  shading.flag |= V3D_SHADING_SHADOW;

  ImBuf *ibuf = ED_view3d_draw_offscreen_imbuf_simple(
      depsgraph,
      DEG_get_evaluated_scene(depsgraph),
      &shading,
      OB_TEXTURE,
      DEG_get_evaluated_object(depsgraph, scene->camera),
      preview_sized->sizex,
      preview_sized->sizey,
      IB_rect,
      V3D_OFSDRAW_OVERRIDE_SCENE_SETTINGS,
      R_ALPHAPREMUL,
      nullptr,
      nullptr,
      err_out);
  /* TODO: color-management? */

  if (ibuf) {
    icon_copy_rect(ibuf, preview_sized->sizex, preview_sized->sizey, preview_sized->rect);
    IMB_freeImBuf(ibuf);
  }

  DEG_graph_free(depsgraph);
  BKE_main_free(preview_main);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Action Preview
 * \{ */

static struct PoseBackup *action_preview_render_prepare(IconPreview *preview)
{
  Object *object = preview->active_object;
  if (object == nullptr) {
    WM_report(RPT_WARNING, "No active object, unable to apply the Action before rendering");
    return nullptr;
  }
  if (object->pose == nullptr) {
    WM_reportf(RPT_WARNING,
               "Object %s has no pose, unable to apply the Action before rendering",
               object->id.name + 2);
    return nullptr;
  }

  /* Create a backup of the current pose. */
  struct bAction *action = (struct bAction *)preview->id;
  struct PoseBackup *pose_backup = ED_pose_backup_create_all_bones(object, action);

  /* Apply the Action as pose, so that it can be rendered. This assumes the Action represents a
   * single pose, and that thus the evaluation time doesn't matter. */
  AnimationEvalContext anim_eval_context = {preview->depsgraph, 0.0f};
  BKE_pose_apply_action_all_bones(object, action, &anim_eval_context);

  /* Force evaluation of the new pose, before the preview is rendered. */
  DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  DEG_evaluate_on_refresh(preview->depsgraph);

  return pose_backup;
}

static void action_preview_render_cleanup(IconPreview *preview, struct PoseBackup *pose_backup)
{
  if (pose_backup == nullptr) {
    return;
  }
  ED_pose_backup_restore(pose_backup);
  ED_pose_backup_free(pose_backup);

  DEG_id_tag_update(&preview->active_object->id, ID_RECALC_GEOMETRY);
}

/* Render a pose from the scene camera. It is assumed that the scene camera is
 * capturing the pose. The pose is applied temporarily to the current object
 * before rendering. */
static void action_preview_render(IconPreview *preview, IconPreviewSize *preview_sized)
{
  char err_out[256] = "";

  Depsgraph *depsgraph = preview->depsgraph;
  /* Not all code paths that lead to this function actually provide a depsgraph.
   * The "Refresh Asset Preview" button (ED_OT_lib_id_generate_preview) does,
   * but WM_OT_previews_ensure does not. */
  BLI_assert(depsgraph != nullptr);
  BLI_assert(preview->scene == DEG_get_input_scene(depsgraph));

  /* Apply the pose before getting the evaluated scene, so that the new pose is evaluated. */
  struct PoseBackup *pose_backup = action_preview_render_prepare(preview);

  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Object *camera_eval = scene_eval->camera;
  if (camera_eval == nullptr) {
    printf("Scene has no camera, unable to render preview of %s without it.\n",
           preview->id->name + 2);
    return;
  }

  /* This renders with the Workbench engine settings stored on the Scene. */
  ImBuf *ibuf = ED_view3d_draw_offscreen_imbuf_simple(depsgraph,
                                                      scene_eval,
                                                      nullptr,
                                                      OB_SOLID,
                                                      camera_eval,
                                                      preview_sized->sizex,
                                                      preview_sized->sizey,
                                                      IB_rect,
                                                      V3D_OFSDRAW_NONE,
                                                      R_ADDSKY,
                                                      nullptr,
                                                      nullptr,
                                                      err_out);

  action_preview_render_cleanup(preview, pose_backup);

  if (err_out[0] != '\0') {
    printf("Error rendering Action %s preview: %s\n", preview->id->name + 2, err_out);
  }

  if (ibuf) {
    icon_copy_rect(ibuf, preview_sized->sizex, preview_sized->sizey, preview_sized->rect);
    IMB_freeImBuf(ibuf);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name New Shader Preview System
 * \{ */

/* inside thread, called by renderer, sets job update value */
static void shader_preview_update(void *spv,
                                  RenderResult *UNUSED(rr),
                                  volatile struct rcti *UNUSED(rect))
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(spv);

  *(sp->do_update) = true;
}

/* called by renderer, checks job value */
static int shader_preview_break(void *spv)
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(spv);

  return *(sp->stop);
}

static void shader_preview_updatejob(void *UNUSED(spv))
{
}

/* Renders texture directly to render buffer. */
static void shader_preview_texture(ShaderPreview *sp, Tex *tex, Scene *sce, Render *re)
{
  /* Setup output buffer. */
  int width = sp->sizex;
  int height = sp->sizey;

  /* This is needed otherwise no RenderResult is created. */
  sce->r.scemode &= ~R_BUTS_PREVIEW;
  RE_InitState(re, nullptr, &sce->r, &sce->view_layers, nullptr, width, height, nullptr);
  RE_SetScene(re, sce);

  /* Create buffer in empty RenderView created in the init step. */
  RenderResult *rr = RE_AcquireResultWrite(re);
  RenderView *rv = (RenderView *)rr->views.first;
  rv->rectf = static_cast<float *>(
      MEM_callocN(sizeof(float[4]) * width * height, "texture render result"));
  RE_ReleaseResult(re);

  /* Get texture image pool (if any) */
  struct ImagePool *img_pool = BKE_image_pool_new();
  BKE_texture_fetch_images_for_pool(tex, img_pool);

  /* Fill in image buffer. */
  float *rect_float = rv->rectf;
  float tex_coord[3] = {0.0f, 0.0f, 0.0f};
  bool color_manage = true;

  for (int y = 0; y < height; y++) {
    /* Tex coords between -1.0f and 1.0f. */
    tex_coord[1] = ((float)y / (float)height) * 2.0f - 1.0f;

    for (int x = 0; x < width; x++) {
      tex_coord[0] = ((float)x / (float)height) * 2.0f - 1.0f;

      /* Evaluate texture at tex_coord. */
      TexResult texres = {0};
      BKE_texture_get_value_ex(sce, tex, tex_coord, &texres, img_pool, color_manage);

      rect_float[0] = texres.tr;
      rect_float[1] = texres.tg;
      rect_float[2] = texres.tb;
      rect_float[3] = texres.talpha ? texres.ta : 1.0f;

      rect_float += 4;
    }

    /* Check if we should cancel texture preview. */
    if (shader_preview_break(sp)) {
      break;
    }
  }

  BKE_image_pool_free(img_pool);
}

static void shader_preview_render(ShaderPreview *sp, ID *id, int split, int first)
{
  Render *re;
  Scene *sce;
  float oldlens;
  short idtype = GS(id->name);
  char name[32];
  int sizex;
  Main *pr_main = sp->pr_main;

  /* in case of split preview, use border render */
  if (split) {
    if (first) {
      sizex = sp->sizex / 2;
    }
    else {
      sizex = sp->sizex - sp->sizex / 2;
    }
  }
  else {
    sizex = sp->sizex;
  }

  /* we have to set preview variables first */
  sce = preview_get_scene(pr_main);
  if (sce) {
    sce->r.xsch = sizex;
    sce->r.ysch = sp->sizey;
    sce->r.size = 100;
  }

  /* get the stuff from the builtin preview dbase */
  sce = preview_prepare_scene(sp->bmain, sp->scene, id, idtype, sp);
  if (sce == nullptr) {
    return;
  }

  if (!split || first) {
    sprintf(name, "Preview %p", sp->owner);
  }
  else {
    sprintf(name, "SecondPreview %p", sp->owner);
  }
  re = RE_GetRender(name);

  /* full refreshed render from first tile */
  if (re == nullptr) {
    re = RE_NewRender(name);
  }

  /* sce->r gets copied in RE_InitState! */
  sce->r.scemode &= ~(R_MATNODE_PREVIEW | R_TEXNODE_PREVIEW);
  sce->r.scemode &= ~R_NO_IMAGE_LOAD;

  if (sp->pr_method == PR_ICON_RENDER) {
    sce->r.scemode |= R_NO_IMAGE_LOAD;
    sce->display.render_aa = SCE_DISPLAY_AA_SAMPLES_8;
  }
  else { /* PR_BUTS_RENDER */
    sce->display.render_aa = SCE_DISPLAY_AA_SAMPLES_8;
  }

  /* Callbacks are cleared on GetRender(). */
  if (sp->pr_method == PR_BUTS_RENDER) {
    RE_display_update_cb(re, sp, shader_preview_update);
  }
  /* set this for all previews, default is react to G.is_break still */
  RE_test_break_cb(re, sp, shader_preview_break);

  /* lens adjust */
  oldlens = ((Camera *)sce->camera->data)->lens;
  if (sizex > sp->sizey) {
    ((Camera *)sce->camera->data)->lens *= (float)sp->sizey / (float)sizex;
  }

  /* entire cycle for render engine */
  if (idtype == ID_TE) {
    shader_preview_texture(sp, (Tex *)id, sce, re);
  }
  else {
    /* Render preview scene */
    RE_PreviewRender(re, pr_main, sce);
  }

  ((Camera *)sce->camera->data)->lens = oldlens;

  /* handle results */
  if (sp->pr_method == PR_ICON_RENDER) {
    // char *rct = (char *)(sp->pr_rect + 32 * 16 + 16);

    if (sp->pr_rect) {
      RE_ResultGet32(re, sp->pr_rect);
    }
  }

  /* unassign the pointers, reset vars */
  preview_prepare_scene(sp->bmain, sp->scene, nullptr, GS(id->name), sp);

  /* XXX bad exception, end-exec is not being called in render, because it uses local main. */
#if 0
  if (idtype == ID_TE) {
    Tex *tex = (Tex *)id;
    if (tex->use_nodes && tex->nodetree)
      ntreeEndExecTree(tex->nodetree);
  }
#endif
}

/* runs inside thread for material and icons */
static void shader_preview_startjob(void *customdata, short *stop, short *do_update)
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(customdata);

  sp->stop = stop;
  sp->do_update = do_update;

  if (sp->parent) {
    shader_preview_render(sp, sp->id, 1, 1);
    shader_preview_render(sp, sp->parent, 1, 0);
  }
  else {
    shader_preview_render(sp, sp->id, 0, 0);
  }

  *do_update = true;
}

static void preview_id_copy_free(ID *id)
{
  struct IDProperty *properties;
  /* get rid of copied ID */
  properties = IDP_GetProperties(id, false);
  if (properties) {
    IDP_FreePropertyContent_ex(properties, false);
    MEM_freeN(properties);
  }
  BKE_libblock_free_datablock(id, 0);
  MEM_freeN(id);
}

static void shader_preview_free(void *customdata)
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(customdata);
  Main *pr_main = sp->pr_main;
  ID *main_id_copy = nullptr;
  ID *sub_id_copy = nullptr;

  if (sp->matcopy) {
    main_id_copy = (ID *)sp->matcopy;
    BLI_remlink(&pr_main->materials, sp->matcopy);
  }
  if (sp->texcopy) {
    BLI_assert(main_id_copy == nullptr);
    main_id_copy = (ID *)sp->texcopy;
    BLI_remlink(&pr_main->textures, sp->texcopy);
  }
  if (sp->worldcopy) {
    /* worldcopy is also created for material with `Preview World` enabled */
    if (main_id_copy) {
      sub_id_copy = (ID *)sp->worldcopy;
    }
    else {
      main_id_copy = (ID *)sp->worldcopy;
    }
    BLI_remlink(&pr_main->worlds, sp->worldcopy);
  }
  if (sp->lampcopy) {
    BLI_assert(main_id_copy == nullptr);
    main_id_copy = (ID *)sp->lampcopy;
    BLI_remlink(&pr_main->lights, sp->lampcopy);
  }
  if (sp->own_id_copy) {
    if (sp->id_copy) {
      preview_id_copy_free(sp->id_copy);
    }
    if (main_id_copy) {
      preview_id_copy_free(main_id_copy);
    }
    if (sub_id_copy) {
      preview_id_copy_free(sub_id_copy);
    }
  }

  MEM_freeN(sp);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Icon Preview
 * \{ */

static ImBuf *icon_preview_imbuf_from_brush(Brush *brush)
{
  static const int flags = IB_rect | IB_multilayer | IB_metadata;

  char path[FILE_MAX];
  const char *folder;

  if (!(brush->icon_imbuf)) {
    if (brush->flag & BRUSH_CUSTOM_ICON) {

      if (brush->icon_filepath[0]) {
        /* First use the path directly to try and load the file. */

        BLI_strncpy(path, brush->icon_filepath, sizeof(brush->icon_filepath));
        BLI_path_abs(path, ID_BLEND_PATH_FROM_GLOBAL(&brush->id));

        /* Use default color-spaces for brushes. */
        brush->icon_imbuf = IMB_loadiffname(path, flags, nullptr);

        /* otherwise lets try to find it in other directories */
        if (!(brush->icon_imbuf)) {
          folder = BKE_appdir_folder_id(BLENDER_DATAFILES, "brushicons");

          BLI_make_file_string(
              BKE_main_blendfile_path_from_global(), path, folder, brush->icon_filepath);

          if (path[0]) {
            /* Use default color spaces. */
            brush->icon_imbuf = IMB_loadiffname(path, flags, nullptr);
          }
        }

        if (brush->icon_imbuf) {
          BKE_icon_changed(BKE_icon_id_ensure(&brush->id));
        }
      }
    }
  }

  if (!(brush->icon_imbuf)) {
    brush->id.icon_id = 0;
  }

  return brush->icon_imbuf;
}

static void icon_copy_rect(ImBuf *ibuf, uint w, uint h, uint *rect)
{
  struct ImBuf *ima;
  uint *drect, *srect;
  float scaledx, scaledy;
  short ex, ey, dx, dy;

  /* paranoia test */
  if (ibuf == nullptr || (ibuf->rect == nullptr && ibuf->rect_float == nullptr)) {
    return;
  }

  /* Waste of cpu cycles... but the imbuf API has no other way to scale fast (ton). */
  ima = IMB_dupImBuf(ibuf);

  if (!ima) {
    return;
  }

  if (ima->x > ima->y) {
    scaledx = (float)w;
    scaledy = ((float)ima->y / (float)ima->x) * (float)w;
  }
  else {
    scaledx = ((float)ima->x / (float)ima->y) * (float)h;
    scaledy = (float)h;
  }

  /* Scaling down must never assign zero width/height, see: T89868. */
  ex = MAX2(1, (short)scaledx);
  ey = MAX2(1, (short)scaledy);

  dx = (w - ex) / 2;
  dy = (h - ey) / 2;

  IMB_scalefastImBuf(ima, ex, ey);

  /* if needed, convert to 32 bits */
  if (ima->rect == nullptr) {
    IMB_rect_from_float(ima);
  }

  srect = ima->rect;
  drect = rect;

  drect += dy * w + dx;
  for (; ey > 0; ey--) {
    memcpy(drect, srect, ex * sizeof(int));
    drect += w;
    srect += ima->x;
  }

  IMB_freeImBuf(ima);
}

static void set_alpha(char *cp, int sizex, int sizey, char alpha)
{
  int a, size = sizex * sizey;

  for (a = 0; a < size; a++, cp += 4) {
    cp[3] = alpha;
  }
}

static void icon_preview_startjob(void *customdata, short *stop, short *do_update)
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(customdata);

  if (sp->pr_method == PR_ICON_DEFERRED) {
    PreviewImage *prv = static_cast<PreviewImage *>(sp->owner);
    ImBuf *thumb;
    char *deferred_data = static_cast<char *>(PRV_DEFERRED_DATA(prv));
    ThumbSource source = static_cast<ThumbSource>(deferred_data[0]);
    char *path = &deferred_data[1];

    // printf("generating deferred %d×%d preview for %s\n", sp->sizex, sp->sizey, path);

    thumb = IMB_thumb_manage(path, THB_LARGE, source);

    if (thumb) {
      /* PreviewImage assumes premultiplied alhpa... */
      IMB_premultiply_alpha(thumb);

      icon_copy_rect(thumb, sp->sizex, sp->sizey, sp->pr_rect);
      IMB_freeImBuf(thumb);
    }
  }
  else {
    ID *id = sp->id;
    short idtype = GS(id->name);

    BLI_assert(id != nullptr);

    if (idtype == ID_IM) {
      Image *ima = (Image *)id;
      ImBuf *ibuf = nullptr;
      ImageUser iuser;
      BKE_imageuser_default(&iuser);

      if (ima == nullptr) {
        return;
      }

      /* setup dummy image user */
      iuser.framenr = 1;
      iuser.scene = sp->scene;

      /* NOTE(@elubie): this needs to be changed: here image is always loaded if not
       * already there. Very expensive for large images. Need to find a way to
       * only get existing `ibuf`. */
      ibuf = BKE_image_acquire_ibuf(ima, &iuser, nullptr);
      if (ibuf == nullptr || (ibuf->rect == nullptr && ibuf->rect_float == nullptr)) {
        BKE_image_release_ibuf(ima, ibuf, nullptr);
        return;
      }

      icon_copy_rect(ibuf, sp->sizex, sp->sizey, sp->pr_rect);

      *do_update = true;

      BKE_image_release_ibuf(ima, ibuf, nullptr);
    }
    else if (idtype == ID_BR) {
      Brush *br = (Brush *)id;

      br->icon_imbuf = icon_preview_imbuf_from_brush(br);

      memset(sp->pr_rect, 0x88, sp->sizex * sp->sizey * sizeof(uint));

      if (!(br->icon_imbuf) || !(br->icon_imbuf->rect)) {
        return;
      }

      icon_copy_rect(br->icon_imbuf, sp->sizex, sp->sizey, sp->pr_rect);

      *do_update = true;
    }
    else if (idtype == ID_SCR) {
      bScreen *screen = (bScreen *)id;

      ED_screen_preview_render(screen, sp->sizex, sp->sizey, sp->pr_rect);
      *do_update = true;
    }
    else {
      /* re-use shader job */
      shader_preview_startjob(customdata, stop, do_update);

      /* world is rendered with alpha=0, so it wasn't displayed
       * this could be render option for sky to, for later */
      if (idtype == ID_WO) {
        set_alpha((char *)sp->pr_rect, sp->sizex, sp->sizey, 255);
      }
    }
  }
}

/* use same function for icon & shader, so the job manager
 * does not run two of them at the same time. */

static void common_preview_startjob(void *customdata,
                                    short *stop,
                                    short *do_update,
                                    float *UNUSED(progress))
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(customdata);

  if (ELEM(sp->pr_method, PR_ICON_RENDER, PR_ICON_DEFERRED)) {
    icon_preview_startjob(customdata, stop, do_update);
  }
  else {
    shader_preview_startjob(customdata, stop, do_update);
  }
}

/**
 * Some ID types already have their own, more focused rendering (only objects right now). This is
 * for the other ones, which all share #ShaderPreview and some functions.
 */
static void other_id_types_preview_render(IconPreview *ip,
                                          IconPreviewSize *cur_size,
                                          const ePreviewRenderMethod pr_method,
                                          short *stop,
                                          short *do_update,
                                          float *progress)
{
  ShaderPreview *sp = MEM_cnew<ShaderPreview>("Icon ShaderPreview");

  /* These types don't use the ShaderPreview mess, they have their own types and functions. */
  BLI_assert(!ip->id || !ELEM(GS(ip->id->name), ID_OB));

  /* Construct shader preview from image size and preview custom-data. */
  sp->scene = ip->scene;
  sp->owner = ip->owner;
  sp->sizex = cur_size->sizex;
  sp->sizey = cur_size->sizey;
  sp->pr_method = pr_method;
  sp->pr_rect = cur_size->rect;
  sp->id = ip->id;
  sp->id_copy = ip->id_copy;
  sp->bmain = ip->bmain;
  sp->own_id_copy = false;
  Material *ma = nullptr;

  if (sp->pr_method == PR_ICON_RENDER) {
    BLI_assert(ip->id);

    /* grease pencil use its own preview file */
    if (GS(ip->id->name) == ID_MA) {
      ma = (Material *)ip->id;
    }

    if ((ma == nullptr) || (ma->gp_style == nullptr)) {
      sp->pr_main = G_pr_main;
    }
    else {
      sp->pr_main = G_pr_main_grease_pencil;
    }
  }

  common_preview_startjob(sp, stop, do_update, progress);
  shader_preview_free(sp);
}

/* exported functions */

/**
 * Find the index to map \a icon_size to data in \a preview_image.
 */
static int icon_previewimg_size_index_get(const IconPreviewSize *icon_size,
                                          const PreviewImage *preview_image)
{
  for (int i = 0; i < NUM_ICON_SIZES; i++) {
    if ((preview_image->w[i] == icon_size->sizex) && (preview_image->h[i] == icon_size->sizey)) {
      return i;
    }
  }

  BLI_assert_msg(0, "The searched icon size does not match any in the preview image");
  return -1;
}

static void icon_preview_startjob_all_sizes(void *customdata,
                                            short *stop,
                                            short *do_update,
                                            float *progress)
{
  IconPreview *ip = (IconPreview *)customdata;
  IconPreviewSize *cur_size;

  for (cur_size = static_cast<IconPreviewSize *>(ip->sizes.first); cur_size;
       cur_size = cur_size->next) {
    PreviewImage *prv = static_cast<PreviewImage *>(ip->owner);
    /* Is this a render job or a deferred loading job? */
    const ePreviewRenderMethod pr_method = (prv->tag & PRV_TAG_DEFFERED) ? PR_ICON_DEFERRED :
                                                                           PR_ICON_RENDER;

    if (*stop) {
      break;
    }

    if (prv->tag & PRV_TAG_DEFFERED_DELETE) {
      /* Non-thread-protected reading is not an issue here. */
      continue;
    }

    /* check_engine_supports_preview() checks whether the engine supports "preview mode" (think:
     * Material Preview). This check is only relevant when the render function called below is
     * going to use such a mode. Object and Action render functions use Solid mode, though, so
     * they can skip this test. */
    /* TODO: Decouple the ID-type-specific render functions from this function, so that it's not
     * necessary to know here what happens inside lower-level functions. */
    const bool use_solid_render_mode = (ip->id != nullptr) && ELEM(GS(ip->id->name), ID_OB, ID_AC);
    if (!use_solid_render_mode && preview_method_is_render(pr_method) &&
        !check_engine_supports_preview(ip->scene)) {
      continue;
    }

#ifndef NDEBUG
    {
      int size_index = icon_previewimg_size_index_get(cur_size, prv);
      BLI_assert(!BKE_previewimg_is_finished(prv, size_index));
    }
#endif

    if (ip->id != nullptr) {
      switch (GS(ip->id->name)) {
        case ID_OB:
          if (object_preview_is_type_supported((Object *)ip->id)) {
            /* Much simpler than the ShaderPreview mess used for other ID types. */
            object_preview_render(ip, cur_size);
            continue;
          }
          break;
        case ID_AC:
          action_preview_render(ip, cur_size);
          continue;
        default:
          /* Fall through to the same code as the `ip->id == nullptr` case. */
          break;
      }
    }
    other_id_types_preview_render(ip, cur_size, pr_method, stop, do_update, progress);
  }
}

static void icon_preview_add_size(IconPreview *ip, uint *rect, int sizex, int sizey)
{
  IconPreviewSize *cur_size = static_cast<IconPreviewSize *>(ip->sizes.first);

  while (cur_size) {
    if (cur_size->sizex == sizex && cur_size->sizey == sizey) {
      /* requested size is already in list, no need to add it again */
      return;
    }

    cur_size = cur_size->next;
  }

  IconPreviewSize *new_size = MEM_cnew<IconPreviewSize>("IconPreviewSize");
  new_size->sizex = sizex;
  new_size->sizey = sizey;
  new_size->rect = rect;

  BLI_addtail(&ip->sizes, new_size);
}

static void icon_preview_endjob(void *customdata)
{
  IconPreview *ip = static_cast<IconPreview *>(customdata);

  if (ip->id) {

    if (GS(ip->id->name) == ID_BR) {
      WM_main_add_notifier(NC_BRUSH | NA_EDITED, ip->id);
    }
#if 0
    if (GS(ip->id->name) == ID_MA) {
      Material *ma = (Material *)ip->id;
      PreviewImage *prv_img = ma->preview;
      int i;

      /* signal to gpu texture */
      for (i = 0; i < NUM_ICON_SIZES; i++) {
        if (prv_img->gputexture[i]) {
          GPU_texture_free(prv_img->gputexture[i]);
          prv_img->gputexture[i] = nullptr;
          WM_main_add_notifier(NC_MATERIAL | ND_SHADING_DRAW, ip->id);
        }
      }
    }
#endif
  }

  if (ip->owner) {
    PreviewImage *prv_img = static_cast<PreviewImage *>(ip->owner);
    prv_img->tag &= ~PRV_TAG_DEFFERED_RENDERING;

    LISTBASE_FOREACH (IconPreviewSize *, icon_size, &ip->sizes) {
      int size_index = icon_previewimg_size_index_get(icon_size, prv_img);
      BKE_previewimg_finish(prv_img, size_index);
    }

    if (prv_img->tag & PRV_TAG_DEFFERED_DELETE) {
      BLI_assert(prv_img->tag & PRV_TAG_DEFFERED);
      BKE_previewimg_deferred_release(prv_img);
    }
  }
}

static void icon_preview_free(void *customdata)
{
  IconPreview *ip = (IconPreview *)customdata;

  if (ip->id_copy) {
    preview_id_copy_free(ip->id_copy);
  }

  BLI_freelistN(&ip->sizes);
  MEM_freeN(ip);
}

bool ED_preview_id_is_supported(const ID *id)
{
  if (id == nullptr) {
    return false;
  }
  if (GS(id->name) == ID_NT) {
    /* Node groups don't support standard preview generation. */
    return false;
  }
  if (GS(id->name) == ID_OB) {
    return object_preview_is_type_supported((const Object *)id);
  }
  return BKE_previewimg_id_get_p(id) != nullptr;
}

void ED_preview_icon_render(
    const bContext *C, Scene *scene, ID *id, uint *rect, int sizex, int sizey)
{
  IconPreview ip = {nullptr};
  short stop = false, update = false;
  float progress = 0.0f;

  ED_preview_ensure_dbase();

  ip.bmain = CTX_data_main(C);
  ip.scene = scene;
  ip.depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ip.owner = BKE_previewimg_id_ensure(id);
  ip.id = id;
  /* Control isn't given back to the caller until the preview is done. So we don't need to copy
   * the ID to avoid thread races. */
  ip.id_copy = duplicate_ids(id, true);
  ip.active_object = CTX_data_active_object(C);

  icon_preview_add_size(&ip, rect, sizex, sizey);

  icon_preview_startjob_all_sizes(&ip, &stop, &update, &progress);

  icon_preview_endjob(&ip);

  BLI_freelistN(&ip.sizes);
  if (ip.id_copy != nullptr) {
    preview_id_copy_free(ip.id_copy);
  }
}

void ED_preview_icon_job(
    const bContext *C, void *owner, ID *id, uint *rect, int sizex, int sizey, const bool delay)
{
  wmJob *wm_job;
  IconPreview *ip, *old_ip;

  ED_preview_ensure_dbase();

  /* suspended start means it starts after 1 timer step, see WM_jobs_timer below */
  wm_job = WM_jobs_get(CTX_wm_manager(C),
                       CTX_wm_window(C),
                       owner,
                       "Icon Preview",
                       WM_JOB_EXCL_RENDER,
                       WM_JOB_TYPE_RENDER_PREVIEW);

  ip = MEM_cnew<IconPreview>("icon preview");

  /* render all resolutions from suspended job too */
  old_ip = static_cast<IconPreview *>(WM_jobs_customdata_get(wm_job));
  if (old_ip) {
    BLI_movelisttolist(&ip->sizes, &old_ip->sizes);
  }

  /* customdata for preview thread */
  ip->bmain = CTX_data_main(C);
  ip->depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ip->scene = DEG_get_input_scene(ip->depsgraph);
  ip->active_object = CTX_data_active_object(C);
  ip->owner = owner;
  ip->id = id;
  ip->id_copy = duplicate_ids(id, false);

  icon_preview_add_size(ip, rect, sizex, sizey);

  /* Special threading hack:
   * warn main code that this preview is being rendered and cannot be freed... */
  {
    PreviewImage *prv_img = static_cast<PreviewImage *>(owner);
    if (prv_img->tag & PRV_TAG_DEFFERED) {
      prv_img->tag |= PRV_TAG_DEFFERED_RENDERING;
    }
  }

  /* setup job */
  WM_jobs_customdata_set(wm_job, ip, icon_preview_free);
  WM_jobs_timer(wm_job, 0.1, NC_WINDOW, NC_WINDOW);
  /* Wait 2s to start rendering icon previews, to not bog down user interaction.
   * Particularly important for heavy scenes and Eevee using OpenGL that blocks
   * the user interface drawing. */
  WM_jobs_delay_start(wm_job, (delay) ? 2.0 : 0.0);
  WM_jobs_callbacks(
      wm_job, icon_preview_startjob_all_sizes, nullptr, nullptr, icon_preview_endjob);

  WM_jobs_start(CTX_wm_manager(C), wm_job);
}

void ED_preview_shader_job(const bContext *C,
                           void *owner,
                           ID *id,
                           ID *parent,
                           MTex *slot,
                           int sizex,
                           int sizey,
                           ePreviewRenderMethod method)
{
  Object *ob = CTX_data_active_object(C);
  wmJob *wm_job;
  ShaderPreview *sp;
  Scene *scene = CTX_data_scene(C);
  const ID_Type id_type = GS(id->name);

  BLI_assert(BKE_previewimg_id_supports_jobs(id));

  /* Use workspace render only for buttons Window,
   * since the other previews are related to the datablock. */

  if (preview_method_is_render(method) && !check_engine_supports_preview(scene)) {
    return;
  }

  ED_preview_ensure_dbase();

  wm_job = WM_jobs_get(CTX_wm_manager(C),
                       CTX_wm_window(C),
                       owner,
                       "Shader Preview",
                       WM_JOB_EXCL_RENDER,
                       WM_JOB_TYPE_RENDER_PREVIEW);
  sp = MEM_cnew<ShaderPreview>("shader preview");

  /* customdata for preview thread */
  sp->scene = scene;
  sp->owner = owner;
  sp->sizex = sizex;
  sp->sizey = sizey;
  sp->pr_method = method;
  sp->id = id;
  sp->id_copy = duplicate_ids(id, false);
  sp->own_id_copy = true;
  sp->parent = parent;
  sp->slot = slot;
  sp->bmain = CTX_data_main(C);
  Material *ma = nullptr;

  /* hardcoded preview .blend for Eevee + Cycles, this should be solved
   * once with custom preview .blend path for external engines */

  /* grease pencil use its own preview file */
  if (id_type == ID_MA) {
    ma = (Material *)id;
  }

  if ((ma == nullptr) || (ma->gp_style == nullptr)) {
    sp->pr_main = G_pr_main;
  }
  else {
    sp->pr_main = G_pr_main_grease_pencil;
  }

  if (ob && ob->totcol) {
    copy_v4_v4(sp->color, ob->color);
  }
  else {
    ARRAY_SET_ITEMS(sp->color, 0.0f, 0.0f, 0.0f, 1.0f);
  }

  /* setup job */
  WM_jobs_customdata_set(wm_job, sp, shader_preview_free);
  WM_jobs_timer(wm_job, 0.1, NC_MATERIAL, NC_MATERIAL);
  WM_jobs_callbacks(wm_job, common_preview_startjob, nullptr, shader_preview_updatejob, nullptr);

  WM_jobs_start(CTX_wm_manager(C), wm_job);
}

void ED_preview_kill_jobs(wmWindowManager *wm, Main *UNUSED(bmain))
{
  if (wm) {
    /* This is called to stop all preview jobs before scene data changes, to
     * avoid invalid memory access. */
    WM_jobs_kill(wm, nullptr, common_preview_startjob);
    WM_jobs_kill(wm, nullptr, icon_preview_startjob_all_sizes);
  }
}

struct PreviewRestartQueueEntry {
  struct PreviewRestartQueueEntry *next, *prev;

  enum eIconSizes size;
  ID *id;
};

static ListBase /* #PreviewRestartQueueEntry */ G_restart_previews_queue;

void ED_preview_restart_queue_free()
{
  BLI_freelistN(&G_restart_previews_queue);
}

void ED_preview_restart_queue_add(ID *id, enum eIconSizes size)
{
  PreviewRestartQueueEntry *queue_entry = MEM_new<PreviewRestartQueueEntry>(__func__);
  queue_entry->size = size;
  queue_entry->id = id;
  BLI_addtail(&G_restart_previews_queue, queue_entry);
}

void ED_preview_restart_queue_work(const bContext *C)
{
  LISTBASE_FOREACH_MUTABLE (PreviewRestartQueueEntry *, queue_entry, &G_restart_previews_queue) {
    PreviewImage *preview = BKE_previewimg_id_get(queue_entry->id);
    if (!preview) {
      continue;
    }
    if (preview->flag[queue_entry->size] & PRV_USER_EDITED) {
      /* Don't touch custom previews. */
      continue;
    }

    BKE_previewimg_clear_single(preview, queue_entry->size);
    UI_icon_render_id(C, nullptr, queue_entry->id, queue_entry->size, true);

    BLI_freelinkN(&G_restart_previews_queue, queue_entry);
  }
}

/** \} */
