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
 * The Original Code is Copyright (C) 2017, Blender Foundation
 * This is a new part of Blender
 */

/** \file
 * \ingroup modifiers
 */

#include <stdio.h>

#include "BLI_utildefines.h"

#include "BLI_blenlib.h"
#include "BLI_math_vector.h"

#include "BLT_translation.h"

#include "DNA_collection_types.h"
#include "DNA_defaults.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_lineart_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "lineart/ED_lineart.h"

#include "BKE_collection.h"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_lib_query.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_screen.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "BKE_modifier.h"
#include "RNA_access.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "MOD_gpencil_modifiertypes.h"
#include "MOD_gpencil_ui_common.h"
#include "MOD_gpencil_util.h"

#include "WM_api.h"
#include "WM_types.h"

static void initData(GpencilModifierData *md)
{
  LineartGpencilModifierData *gpmd = (LineartGpencilModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(gpmd, modifier));

  MEMCPY_STRUCT_AFTER(gpmd, DNA_struct_default_get(LineartGpencilModifierData), modifier);
}

static void copyData(const GpencilModifierData *md, GpencilModifierData *target)
{
  BKE_gpencil_modifier_copydata_generic(md, target);
}

static void generate_strokes_actual(
    GpencilModifierData *md, Depsgraph *depsgraph, Object *ob, bGPDlayer *gpl, bGPDframe *gpf)
{
  LineartGpencilModifierData *lmd = (LineartGpencilModifierData *)md;

  if (G.debug_value == 4000) {
    printf("LRT: Generating from modifier.\n");
  }

  ED_lineart_gpencil_generate_with_type(
      lmd->render_buffer,
      depsgraph,
      ob,
      gpl,
      gpf,
      lmd->source_type,
      lmd->source_type == LRT_SOURCE_OBJECT ? (void *)lmd->source_object :
                                              (void *)lmd->source_collection,
      lmd->level_start,
      lmd->use_multiple_levels ? lmd->level_end : lmd->level_start,
      lmd->target_material ? BKE_gpencil_object_material_index_get(ob, lmd->target_material) : 0,
      lmd->line_types,
      lmd->transparency_flags,
      lmd->transparency_mask,
      lmd->thickness,
      lmd->opacity,
      lmd->pre_sample_length,
      lmd->source_vertex_group,
      lmd->vgname,
      lmd->flags);
}

static bool isModifierDisabled(GpencilModifierData *md)
{
  LineartGpencilModifierData *lmd = (LineartGpencilModifierData *)md;

  if ((lmd->target_layer[0] == '\0') || (lmd->target_material == NULL)) {
    return true;
  }

  if (lmd->source_type == LRT_SOURCE_OBJECT && !lmd->source_object) {
    return true;
  }

  if (lmd->source_type == LRT_SOURCE_COLLECTION && !lmd->source_collection) {
    return true;
  }

  return false;
}
static void generateStrokes(GpencilModifierData *md, Depsgraph *depsgraph, Object *ob)
{
  LineartGpencilModifierData *lmd = (LineartGpencilModifierData *)md;
  bGPdata *gpd = ob->data;

  /* Guard early, don't trigger calculation when no gpencil frame is present. Probably should
   * disable in the isModifierDisabled() function but we need addtional arg for depsgraph and
   * gpd. */
  bGPDlayer *gpl = BKE_gpencil_layer_get_by_name(gpd, lmd->target_layer, 1);
  if (gpl == NULL) {
    return;
  }
  /* Need to call this or we don't get active frame (user may haven't selected any one). */
  BKE_gpencil_frame_active_set(depsgraph, gpd);
  bGPDframe *gpf = gpl->actframe;
  if (gpf == NULL) {
    return;
  }

  /* Check all parameters required are filled. */
  if (isModifierDisabled(md)) {
    return;
  }

  ED_lineart_compute_feature_lines(depsgraph, lmd);

  generate_strokes_actual(md, depsgraph, ob, gpl, gpf);

  ED_lineart_destroy_render_data(lmd);

  WM_main_add_notifier(NA_EDITED | NC_GPENCIL, NULL);
}

static void bakeModifier(Main *UNUSED(bmain),
                         Depsgraph *depsgraph,
                         GpencilModifierData *md,
                         Object *ob)
{
  bGPdata *gpd = ob->data;
  LineartGpencilModifierData *lmd = (LineartGpencilModifierData *)md;

  bGPDlayer *gpl = BKE_gpencil_layer_get_by_name(gpd, lmd->target_layer, 1);
  if (gpl == NULL) {
    return;
  }
  bGPDframe *gpf = gpl->actframe;
  if (gpf == NULL) {
    return;
  }

  ED_lineart_compute_feature_lines(depsgraph, lmd);

  generate_strokes_actual(md, depsgraph, ob, gpl, gpf);

  ED_lineart_destroy_render_data(lmd);
}

static bool isDisabled(GpencilModifierData *md, int UNUSED(userRenderParams))
{
  return isModifierDisabled(md);
}

static void updateDepsgraph(GpencilModifierData *md,
                            const ModifierUpdateDepsgraphContext *ctx,
                            const int mode)
{
  DEG_add_object_relation(ctx->node, ctx->object, DEG_OB_COMP_TRANSFORM, "Line Art Modifier");

  LineartGpencilModifierData *lmd = (LineartGpencilModifierData *)md;
  if (lmd->source_type == LRT_SOURCE_OBJECT && lmd->source_object) {
    DEG_add_object_relation(
        ctx->node, lmd->source_object, DEG_OB_COMP_GEOMETRY, "Line Art Modifier");
    DEG_add_object_relation(
        ctx->node, lmd->source_object, DEG_OB_COMP_TRANSFORM, "Line Art Modifier");
  }
  else {
    FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_BEGIN (ctx->scene->master_collection, ob, mode) {
      if (ob->type == OB_MESH || ob->type == OB_MBALL || ob->type == OB_CURVE ||
          ob->type == OB_SURF || ob->type == OB_FONT) {
        if (!(ob->lineart.usage & COLLECTION_LRT_EXCLUDE)) {
          DEG_add_object_relation(ctx->node, ob, DEG_OB_COMP_GEOMETRY, "Line Art Modifier");
          DEG_add_object_relation(ctx->node, ob, DEG_OB_COMP_TRANSFORM, "Line Art Modifier");
        }
      }
    }
    FOREACH_COLLECTION_VISIBLE_OBJECT_RECURSIVE_END;
  }
  DEG_add_object_relation(
      ctx->node, ctx->scene->camera, DEG_OB_COMP_TRANSFORM, "Line Art Modifier");
}

static void foreachIDLink(GpencilModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  LineartGpencilModifierData *lmd = (LineartGpencilModifierData *)md;

  walk(userData, ob, (ID **)&lmd->target_material, IDWALK_CB_USER);
  walk(userData, ob, (ID **)&lmd->source_collection, IDWALK_CB_NOP);

  walk(userData, ob, (ID **)&lmd->source_object, IDWALK_CB_NOP);
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, &ob_ptr);

  PointerRNA obj_data_ptr = RNA_pointer_get(&ob_ptr, "data");

  int source_type = RNA_enum_get(ptr, "source_type");

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "source_type", 0, NULL, ICON_NONE);

  if (source_type == LRT_SOURCE_OBJECT) {
    uiItemR(layout, ptr, "source_object", 0, NULL, ICON_CUBE);
  }
  else if (source_type == LRT_SOURCE_COLLECTION) {
    uiItemR(layout, ptr, "source_collection", 0, NULL, ICON_OUTLINER_COLLECTION);
  }
  else {
    /* Source is Scene. */
  }

  uiLayout *column = uiLayoutColumn(layout, true);

  uiItemR(column, ptr, "use_contour", 0, NULL, ICON_NONE);

  bool use_crease = RNA_boolean_get(ptr, "use_crease");

  uiItemR(column, ptr, "use_crease", 0, "Crease", ICON_NONE);
  if (use_crease) {
    uiItemR(column, ptr, "crease_threshold", UI_ITEM_R_SLIDER, "Threshold", ICON_NONE);
  }

  uiItemR(column, ptr, "use_material", 0, "Material", ICON_NONE);
  uiItemR(column, ptr, "use_edge_mark", 0, "Edge Marks", ICON_NONE);
  uiItemR(column, ptr, "use_intersection", 0, "Intersection", ICON_NONE);

  uiItemPointerR(layout, ptr, "target_layer", &obj_data_ptr, "layers", NULL, ICON_GREASEPENCIL);
  uiItemPointerR(
      layout, ptr, "target_material", &obj_data_ptr, "materials", NULL, ICON_SHADING_TEXTURE);

  uiItemR(layout, ptr, "remove_doubles", 0, "Allow Overlapping Edges", ICON_NONE);

  gpencil_modifier_panel_end(layout, ptr);
}

static void style_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  uiLayout *layout = panel->layout;

  uiLayoutSetPropSep(layout, true);

  uiLayout *column = uiLayoutColumn(layout, true);

  uiItemR(column, ptr, "thickness", UI_ITEM_R_SLIDER, NULL, ICON_NONE);

  uiItemR(column, ptr, "opacity", UI_ITEM_R_SLIDER, NULL, ICON_NONE);
}

static void occlusion_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  uiLayout *layout = panel->layout;

  uiLayoutSetPropSep(layout, true);

  bool use_multiple_levels = RNA_boolean_get(ptr, "use_multiple_levels");
  bool use_transparency = RNA_boolean_get(ptr, "use_transparency");

  uiItemR(layout, ptr, "use_multiple_levels", 0, "Multiple Levels", ICON_NONE);

  if (use_multiple_levels) {
    uiLayout *col = uiLayoutColumn(layout, true);
    uiItemR(col, ptr, "level_start", 0, NULL, ICON_NONE);
    uiItemR(col, ptr, "level_end", 0, NULL, ICON_NONE);
  }
  else {
    uiItemR(layout, ptr, "level_start", 0, "Level", ICON_NONE);
  }

  uiItemR(layout, ptr, "use_transparency", 0, "Transparency", ICON_NONE);

  uiLayout *column = uiLayoutColumn(layout, true);

  if (use_transparency) {
    uiItemR(column, ptr, "transparency_match", 0, "Match", ICON_NONE);
  }

  if (use_transparency) {
    uiLayout *row = uiLayoutRow(column, true);
    uiItemR(row, ptr, "transparency_mask_0", UI_ITEM_R_TOGGLE, "0", ICON_NONE);
    uiItemR(row, ptr, "transparency_mask_1", UI_ITEM_R_TOGGLE, "1", ICON_NONE);
    uiItemR(row, ptr, "transparency_mask_2", UI_ITEM_R_TOGGLE, "2", ICON_NONE);
    uiItemR(row, ptr, "transparency_mask_3", UI_ITEM_R_TOGGLE, "3", ICON_NONE);
    uiItemR(row, ptr, "transparency_mask_4", UI_ITEM_R_TOGGLE, "4", ICON_NONE);
    uiItemR(row, ptr, "transparency_mask_5", UI_ITEM_R_TOGGLE, "5", ICON_NONE);
    uiItemR(row, ptr, "transparency_mask_6", UI_ITEM_R_TOGGLE, "6", ICON_NONE);
    uiItemR(row, ptr, "transparency_mask_7", UI_ITEM_R_TOGGLE, "7", ICON_NONE);
  }
}

static void chaining_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  uiLayout *layout = panel->layout;

  uiLayoutSetPropSep(layout, true);

  uiLayout *column = uiLayoutColumn(layout, true);
  uiItemR(column, ptr, "chaining_geometry_threshold", 0, NULL, ICON_NONE);
  uiItemR(column, ptr, "chaining_image_threshold", 0, NULL, ICON_NONE);

  uiItemR(layout, ptr, "pre_sample_length", UI_ITEM_R_SLIDER, NULL, ICON_NONE);

  uiItemR(layout, ptr, "angle_splitting_threshold", UI_ITEM_R_SLIDER, NULL, ICON_NONE);
}

static void vgroup_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  PointerRNA ob_ptr;
  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, &ob_ptr);

  uiLayout *layout = panel->layout, *row;

  uiLayoutSetPropSep(layout, true);

  uiLayout *column = uiLayoutColumn(layout, true);

  row = uiLayoutRow(column, true);
  uiItemR(row, ptr, "source_vertex_group", 0, "Filter Source", ICON_GROUP_VERTEX);
  uiItemR(row, ptr, "invert_source_vertex_group", UI_ITEM_R_TOGGLE, "", ICON_ARROW_LEFTRIGHT);

  uiItemR(column, ptr, "match_output_vertex_group", 0, NULL, ICON_NONE);

  bool match_output = RNA_boolean_get(ptr, "match_output_vertex_group");
  if (!match_output) {
    uiItemPointerR(column, ptr, "vertex_group", &ob_ptr, "vertex_groups", "Target", ICON_NONE);
  }

  uiItemR(layout, ptr, "soft_selection", 0, NULL, ICON_NONE);
}

static void baking_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  uiLayoutSetPropSep(layout, true);

  // TODO come up with better names for these buttons!
  uiItemO(layout, IFACE_("Bake Current Object"), ICON_NONE, "OBJECT_OT_lineart_bake_strokes");
  uiItemO(layout,
          IFACE_("Bake All Line Art Objects"),
          ICON_NONE,
          "OBJECT_OT_lineart_bake_strokes_all_targets");
  uiItemO(layout, IFACE_("Clear Current Object"), ICON_NONE, "OBJECT_OT_lineart_clear_strokes");
  uiItemO(layout, IFACE_("Clear All Objects"), ICON_NONE, "OBJECT_OT_lineart_clear_strokes_all");
}

static void panelRegister(ARegionType *region_type)
{
  PanelType *panel_type = gpencil_modifier_panel_register(
      region_type, eGpencilModifierType_Lineart, panel_draw);

  gpencil_modifier_subpanel_register(
      region_type, "style", "Style", NULL, style_panel_draw, panel_type);
  gpencil_modifier_subpanel_register(
      region_type, "occlusion", "Occlusion", NULL, occlusion_panel_draw, panel_type);
  gpencil_modifier_subpanel_register(
      region_type, "chaining", "Chaining", NULL, chaining_panel_draw, panel_type);
  gpencil_modifier_subpanel_register(
      region_type, "vgroup", "Vertex Weight Transfer", NULL, vgroup_panel_draw, panel_type);
  gpencil_modifier_subpanel_register(
      region_type, "baking", "Baking", NULL, baking_panel_draw, panel_type);
}

GpencilModifierTypeInfo modifierType_Gpencil_Lineart = {
    /* name. */ "Line Art",
    /* structName. */ "LineartGpencilModifierData",
    /* structSize. */ sizeof(LineartGpencilModifierData),
    /* type. */ eGpencilModifierTypeType_Gpencil,
    /* flags. */ eGpencilModifierTypeFlag_SupportsEditmode,

    /* copyData. */ copyData,

    /* deformStroke. */ NULL,
    /* generateStrokes. */ generateStrokes,
    /* bakeModifier. */ bakeModifier,
    /* remapTime. */ NULL,

    /* initData. */ initData,
    /* freeData. */ NULL,
    /* isDisabled. */ isDisabled,
    /* updateDepsgraph. */ updateDepsgraph,
    /* dependsOnTime. */ NULL,
    /* foreachIDLink. */ foreachIDLink,
    /* foreachTexLink. */ NULL,
    /* panelRegister. */ panelRegister,
};
