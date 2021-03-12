/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "render/tile.h"

#include "util/util_algorithm.h"
#include "util/util_foreach.h"
#include "util/util_types.h"

CCL_NAMESPACE_BEGIN

#if 0
namespace {

class TileComparator {
 public:
  TileComparator(TileOrder order_, int2 center_, Tile *tiles_)
      : order(order_), center(center_), tiles(tiles_)
  {
  }

  bool operator()(int a, int b)
  {
    switch (order) {
      case TILE_CENTER: {
        float2 dist_a = make_float2(center.x - (tiles[a].x + tiles[a].w / 2),
                                    center.y - (tiles[a].y + tiles[a].h / 2));
        float2 dist_b = make_float2(center.x - (tiles[b].x + tiles[b].w / 2),
                                    center.y - (tiles[b].y + tiles[b].h / 2));
        return dot(dist_a, dist_a) < dot(dist_b, dist_b);
      }
      case TILE_LEFT_TO_RIGHT:
        return (tiles[a].x == tiles[b].x) ? (tiles[a].y < tiles[b].y) : (tiles[a].x < tiles[b].x);
      case TILE_RIGHT_TO_LEFT:
        return (tiles[a].x == tiles[b].x) ? (tiles[a].y < tiles[b].y) : (tiles[a].x > tiles[b].x);
      case TILE_TOP_TO_BOTTOM:
        return (tiles[a].y == tiles[b].y) ? (tiles[a].x < tiles[b].x) : (tiles[a].y > tiles[b].y);
      case TILE_BOTTOM_TO_TOP:
      default:
        return (tiles[a].y == tiles[b].y) ? (tiles[a].x < tiles[b].x) : (tiles[a].y < tiles[b].y);
    }
  }

 protected:
  TileOrder order;
  int2 center;
  Tile *tiles;
};

inline int2 hilbert_index_to_pos(int n, int d)
{
  int2 r, xy = make_int2(0, 0);
  for (int s = 1; s < n; s *= 2) {
    r.x = (d >> 1) & 1;
    r.y = (d ^ r.x) & 1;
    if (!r.y) {
      if (r.x) {
        xy = make_int2(s - 1, s - 1) - xy;
      }
      swap(xy.x, xy.y);
    }
    xy += r * make_int2(s, s);
    d >>= 2;
  }
  return xy;
}

enum SpiralDirection {
  DIRECTION_UP,
  DIRECTION_LEFT,
  DIRECTION_DOWN,
  DIRECTION_RIGHT,
};

} /* namespace */
#endif

TileManager::TileManager(bool progressive_,
                         int num_samples_,
#if 0
                          int2 tile_size_,
#endif
                         int start_resolution_,
#if 0
                          bool preserve_tile_device_,
                          bool background_,
                          TileOrder tile_order_,
                          int num_devices_,
#endif
                         int pixel_size_)
{
  progressive = progressive_;
#if 0
  tile_size = tile_size_;
  tile_order = tile_order_;
#endif
  start_resolution = start_resolution_;
  pixel_size = pixel_size_;
  slice_overlap = 0;
  num_samples = num_samples_;
#if 0
  num_devices = num_devices_;
  preserve_tile_device = preserve_tile_device_;
  background = background_;

  schedule_denoising = false;
#endif

  range_start_sample = 0;
  range_num_samples = -1;

  BufferParams buffer_params;
  reset(buffer_params, 0);
}

TileManager::~TileManager()
{
}

#if 0
void TileManager::device_free()
{
  if (schedule_denoising || progressive) {
    for (int i = 0; i < state.tiles.size(); i++) {
      delete state.tiles[i].buffers;
      state.tiles[i].buffers = NULL;
    }
  }

  state.tiles.clear();
}
#endif

static int get_divider(int w, int h, int start_resolution)
{
  int divider = 1;
  if (start_resolution != INT_MAX) {
    while (w * h > start_resolution * start_resolution) {
      w = max(1, w / 2);
      h = max(1, h / 2);

      divider <<= 1;
    }
  }
  return divider;
}

void TileManager::reset(BufferParams &params_, int num_samples_)
{
  params = params_;

  set_samples(num_samples_);

  state.buffer = BufferParams();
  state.sample = range_start_sample - 1;

#if 0
  state.num_tiles = 0;
#endif

  state.num_samples = 0;
  state.resolution_divider = get_divider(params.width, params.height, start_resolution);

#if 0
  state.render_tiles.clear();
  state.denoising_tiles.clear();
  device_free();
#endif
}

void TileManager::set_samples(int num_samples_)
{
  num_samples = num_samples_;

  /* No real progress indication is possible when using unlimited samples. */
  if (num_samples == INT_MAX) {
    state.total_pixel_samples = 0;
  }
  else {
    uint64_t pixel_samples = 0;
    /* While rendering in the viewport, the initial preview resolution is increased to the native
     * resolution before the actual rendering begins. Therefore, additional pixel samples will be
     * rendered. */
    int divider = max(get_divider(params.width, params.height, start_resolution) / 2, pixel_size);
    while (divider > pixel_size) {
      int image_w = max(1, params.width / divider);
      int image_h = max(1, params.height / divider);
      pixel_samples += image_w * image_h;
      divider >>= 1;
    }

    int image_w = max(1, params.width / divider);
    int image_h = max(1, params.height / divider);
    state.total_pixel_samples = pixel_samples +
                                (uint64_t)get_num_effective_samples() * image_w * image_h;

#if 0
    if (schedule_denoising) {
      state.total_pixel_samples += params.width * params.height;
    }
#endif
  }
}

#if 0
/* If sliced is false, splits image into tiles and assigns equal amount of tiles to every render
 * device. If sliced is true, slice image into as much pieces as how many devices are rendering
 * this image. */
int TileManager::gen_tiles(bool sliced)
{
  int resolution = state.resolution_divider;
  int image_w = max(1, params.width / resolution);
  int image_h = max(1, params.height / resolution);
  int2 center = make_int2(image_w / 2, image_h / 2);

  int num = preserve_tile_device || sliced ? min(image_h, num_devices) : 1;
  int slice_num = sliced ? num : 1;
  int tile_w = (tile_size.x >= image_w) ? 1 : divide_up(image_w, tile_size.x);

  device_free();
  state.render_tiles.clear();
  state.denoising_tiles.clear();
  state.render_tiles.resize(num);
  state.denoising_tiles.resize(num);
  state.tile_stride = tile_w;
  vector<list<int>>::iterator tile_list;
  tile_list = state.render_tiles.begin();

  if (tile_order == TILE_HILBERT_SPIRAL) {
    assert(!sliced && slice_overlap == 0);

    int tile_h = (tile_size.y >= image_h) ? 1 : divide_up(image_h, tile_size.y);
    state.tiles.resize(tile_w * tile_h);

    /* Size of blocks in tiles, must be a power of 2 */
    const int hilbert_size = (max(tile_size.x, tile_size.y) <= 12) ? 8 : 4;

    int tiles_per_device = divide_up(tile_w * tile_h, num);
    int cur_device = 0, cur_tiles = 0;

    int2 block_size = tile_size * make_int2(hilbert_size, hilbert_size);
    /* Number of blocks to fill the image */
    int blocks_x = (block_size.x >= image_w) ? 1 : divide_up(image_w, block_size.x);
    int blocks_y = (block_size.y >= image_h) ? 1 : divide_up(image_h, block_size.y);
    int n = max(blocks_x, blocks_y) | 0x1; /* Side length of the spiral (must be odd) */
    /* Offset of spiral (to keep it centered) */
    int2 offset = make_int2((image_w - n * block_size.x) / 2, (image_h - n * block_size.y) / 2);
    offset = (offset / tile_size) * tile_size; /* Round to tile border. */

    int2 block = make_int2(0, 0); /* Current block */
    SpiralDirection prev_dir = DIRECTION_UP, dir = DIRECTION_UP;
    for (int i = 0;;) {
      /* Generate the tiles in the current block. */
      for (int hilbert_index = 0; hilbert_index < hilbert_size * hilbert_size; hilbert_index++) {
        int2 tile, hilbert_pos = hilbert_index_to_pos(hilbert_size, hilbert_index);
        /* Rotate block according to spiral direction. */
        if (prev_dir == DIRECTION_UP && dir == DIRECTION_UP) {
          tile = make_int2(hilbert_pos.y, hilbert_pos.x);
        }
        else if (dir == DIRECTION_LEFT || prev_dir == DIRECTION_LEFT) {
          tile = hilbert_pos;
        }
        else if (dir == DIRECTION_DOWN) {
          tile = make_int2(hilbert_size - 1 - hilbert_pos.y, hilbert_size - 1 - hilbert_pos.x);
        }
        else {
          tile = make_int2(hilbert_size - 1 - hilbert_pos.x, hilbert_size - 1 - hilbert_pos.y);
        }

        int2 pos = block * block_size + tile * tile_size + offset;
        /* Only add tiles which are in the image (tiles outside of the image can be generated since
         * the spiral is always square). */
        if (pos.x >= 0 && pos.y >= 0 && pos.x < image_w && pos.y < image_h) {
          int w = min(tile_size.x, image_w - pos.x);
          int h = min(tile_size.y, image_h - pos.y);
          int2 ipos = pos / tile_size;
          int idx = ipos.y * tile_w + ipos.x;
          state.tiles[idx] = Tile(idx, pos.x, pos.y, w, h, cur_device, Tile::RENDER);
          tile_list->push_front(idx);
          cur_tiles++;

          if (cur_tiles == tiles_per_device) {
            tile_list++;
            cur_tiles = 0;
            cur_device++;
          }
        }
      }

      /* Stop as soon as the spiral has reached the center block. */
      if (block.x == (n - 1) / 2 && block.y == (n - 1) / 2)
        break;

      /* Advance to next block. */
      prev_dir = dir;
      switch (dir) {
        case DIRECTION_UP:
          block.y++;
          if (block.y == (n - i - 1)) {
            dir = DIRECTION_LEFT;
          }
          break;
        case DIRECTION_LEFT:
          block.x++;
          if (block.x == (n - i - 1)) {
            dir = DIRECTION_DOWN;
          }
          break;
        case DIRECTION_DOWN:
          block.y--;
          if (block.y == i) {
            dir = DIRECTION_RIGHT;
          }
          break;
        case DIRECTION_RIGHT:
          block.x--;
          if (block.x == i + 1) {
            dir = DIRECTION_UP;
            i++;
          }
          break;
      }
    }
    return tile_w * tile_h;
  }

  int idx = 0;
  for (int slice = 0; slice < slice_num; slice++) {
    int slice_y = (image_h / slice_num) * slice;
    int slice_h = (slice == slice_num - 1) ? image_h - slice * (image_h / slice_num) :
                                             image_h / slice_num;

    if (slice_overlap != 0) {
      int slice_y_offset = max(slice_y - slice_overlap, 0);
      slice_h = min(slice_y + slice_h + slice_overlap, image_h) - slice_y_offset;
      slice_y = slice_y_offset;
    }

    int tile_h = (tile_size.y >= slice_h) ? 1 : divide_up(slice_h, tile_size.y);

    int tiles_per_device = divide_up(tile_w * tile_h, num);
    int cur_device = 0, cur_tiles = 0;

    for (int tile_y = 0; tile_y < tile_h; tile_y++) {
      for (int tile_x = 0; tile_x < tile_w; tile_x++, idx++) {
        int x = tile_x * tile_size.x;
        int y = tile_y * tile_size.y;
        int w = (tile_x == tile_w - 1) ? image_w - x : tile_size.x;
        int h = (tile_y == tile_h - 1) ? slice_h - y : tile_size.y;

        state.tiles.push_back(
            Tile(idx, x, y + slice_y, w, h, sliced ? slice : cur_device, Tile::RENDER));
        tile_list->push_back(idx);

        if (!sliced) {
          cur_tiles++;

          if (cur_tiles == tiles_per_device) {
            /* Tiles are already generated in Bottom-to-Top order, so no sort is necessary in that
             * case. */
            if (tile_order != TILE_BOTTOM_TO_TOP) {
              tile_list->sort(TileComparator(tile_order, center, &state.tiles[0]));
            }
            tile_list++;
            cur_tiles = 0;
            cur_device++;
          }
        }
      }
    }
    if (sliced) {
      tile_list++;
    }
  }

  return idx;
}

void TileManager::gen_render_tiles()
{
  /* Regenerate just the render tiles for progressive render. */
  foreach (Tile &tile, state.tiles) {
    tile.state = Tile::RENDER;
    state.render_tiles[tile.device].push_back(tile.index);
  }
}
#endif

void TileManager::set_tiles()
{
  int resolution = state.resolution_divider;
  int image_w = max(1, params.width / resolution);
  int image_h = max(1, params.height / resolution);

#if 0
  state.num_tiles = gen_tiles(!background);
#endif

  state.buffer.width = image_w;
  state.buffer.height = image_h;

  state.buffer.full_x = params.full_x / resolution;
  state.buffer.full_y = params.full_y / resolution;
  state.buffer.full_width = max(1, params.full_width / resolution);
  state.buffer.full_height = max(1, params.full_height / resolution);
}

#if 0
int TileManager::get_neighbor_index(int index, int neighbor)
{
  /* Neighbor indices:
   *   0 1 2
   *   3 4 5
   *   6 7 8
   */
  static const int dx[] = {-1, 0, 1, -1, 0, 1, -1, 0, 1};
  static const int dy[] = {-1, -1, -1, 0, 0, 0, 1, 1, 1};

  int resolution = state.resolution_divider;
  int image_w = max(1, params.width / resolution);
  int image_h = max(1, params.height / resolution);

  int num = min(image_h, num_devices);
  int slice_num = !background ? num : 1;
  int slice_h = image_h / slice_num;

  int tile_w = (tile_size.x >= image_w) ? 1 : divide_up(image_w, tile_size.x);
  int tile_h = (tile_size.y >= slice_h) ? 1 : divide_up(slice_h, tile_size.y);

  /* Tiles in the state tile list are always indexed from left to right, top to bottom. */
  int nx = (index % tile_w) + dx[neighbor];
  int ny = (index / tile_w) + dy[neighbor];
  if (nx < 0 || ny < 0 || nx >= tile_w || ny >= tile_h * slice_num)
    return -1;

  return ny * state.tile_stride + nx;
}

/* Checks whether all neighbors of a tile (as well as the tile itself) are at least at state
 * min_state. */
bool TileManager::check_neighbor_state(int index, Tile::State min_state)
{
  if (index < 0 || state.tiles[index].state < min_state) {
    return false;
  }
  for (int neighbor = 0; neighbor < 9; neighbor++) {
    int nindex = get_neighbor_index(index, neighbor);
    /* Out-of-bounds tiles don't matter. */
    if (nindex >= 0 && state.tiles[nindex].state < min_state) {
      return false;
    }
  }

  return true;
}

/* Returns whether the tile should be written (and freed if no denoising is used) instead of
 * updating. */
bool TileManager::finish_tile(const int index, const bool need_denoise, bool &delete_tile)
{
  delete_tile = false;

  switch (state.tiles[index].state) {
    case Tile::RENDER: {
      if (!(schedule_denoising && need_denoise)) {
        state.tiles[index].state = Tile::DONE;
        delete_tile = !progressive;
        return true;
      }
      state.tiles[index].state = Tile::RENDERED;
      /* For each neighbor and the tile itself, check whether all of its neighbors have been
       * rendered. If yes, it can be denoised. */
      for (int neighbor = 0; neighbor < 9; neighbor++) {
        int nindex = get_neighbor_index(index, neighbor);
        if (check_neighbor_state(nindex, Tile::RENDERED)) {
          state.tiles[nindex].state = Tile::DENOISE;
          state.denoising_tiles[state.tiles[nindex].device].push_back(nindex);
        }
      }
      return false;
    }
    case Tile::DENOISE: {
      state.tiles[index].state = Tile::DENOISED;
      /* For each neighbor and the tile itself, check whether all of its neighbors have been
       * denoised. If yes, it can be freed. */
      for (int neighbor = 0; neighbor < 9; neighbor++) {
        int nindex = get_neighbor_index(index, neighbor);
        if (check_neighbor_state(nindex, Tile::DENOISED)) {
          state.tiles[nindex].state = Tile::DONE;
          /* Do not delete finished tiles in progressive mode. */
          if (!progressive) {
            /* It can happen that the tile just finished denoising and already can be freed here.
             * However, in that case it still has to be written before deleting, so we can't delete
             * it yet. */
            if (neighbor == 4) {
              delete_tile = true;
            }
            else {
              delete state.tiles[nindex].buffers;
              state.tiles[nindex].buffers = NULL;
            }
          }
        }
      }
      return true;
    }
    default:
      assert(false);
      return true;
  }
}

bool TileManager::next_tile(Tile *&tile, int device, uint tile_types)
{
  /* Preserve device if requested, unless this is a separate denoising device that just wants to
   * grab any available tile. */
  const bool preserve_device = preserve_tile_device && device < num_devices;

  if (tile_types & RenderTile::DENOISE) {
    int tile_index = -1;
    int logical_device = preserve_device ? device : 0;

    while (logical_device < state.denoising_tiles.size()) {
      if (state.denoising_tiles[logical_device].empty()) {
        if (preserve_device) {
          break;
        }
        else {
          logical_device++;
          continue;
        }
      }

      tile_index = state.denoising_tiles[logical_device].front();
      state.denoising_tiles[logical_device].pop_front();
      break;
    }

    if (tile_index >= 0) {
      tile = &state.tiles[tile_index];
      return true;
    }
  }

  if (tile_types & RenderTile::PATH_TRACE) {
    int tile_index = -1;
    int logical_device = preserve_device ? device : 0;

    while (logical_device < state.render_tiles.size()) {
      if (state.render_tiles[logical_device].empty()) {
        if (preserve_device) {
          break;
        }
        else {
          logical_device++;
          continue;
        }
      }

      tile_index = state.render_tiles[logical_device].front();
      state.render_tiles[logical_device].pop_front();
      break;
    }

    if (tile_index >= 0) {
      tile = &state.tiles[tile_index];
      return true;
    }
  }

  return false;
}
#endif

bool TileManager::done()
{
  int end_sample = (range_num_samples == -1) ? num_samples :
                                               range_start_sample + range_num_samples;
  return (state.resolution_divider == pixel_size) &&
         (state.sample + state.num_samples >= end_sample);
}

#if 0
bool TileManager::has_tiles()
{
  foreach (Tile &tile, state.tiles) {
    if (tile.state != Tile::DONE) {
      return true;
    }
  }
  return false;
}
#endif

bool TileManager::next()
{
  if (done())
    return false;

  if (progressive && state.resolution_divider > pixel_size) {
    state.sample = 0;
    state.resolution_divider = max(state.resolution_divider / 2, pixel_size);
    state.num_samples = 1;
    set_tiles();
  }
  else {
    state.sample++;

    if (progressive)
      state.num_samples = 1;
    else if (range_num_samples == -1)
      state.num_samples = num_samples;
    else
      state.num_samples = range_num_samples;

    state.resolution_divider = pixel_size;

    if (state.sample == range_start_sample) {
      set_tiles();
    }
#if 0
    else {
      gen_render_tiles();
    }
#endif
  }

  return true;
}

int TileManager::get_num_effective_samples()
{
  return (range_num_samples == -1) ? num_samples : range_num_samples;
}

CCL_NAMESPACE_END
