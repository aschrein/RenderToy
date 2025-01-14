#pragma once
#include "error_handling.hpp"
#include "random.hpp"
#include <algorithm>
#include <deque>
#include <fstream>
#include <glm/glm.hpp>
#include <memory>
#include <sparsehash/dense_hash_set>
#include <string>
#include <vector>
using namespace glm;

struct Oct_Item {
  vec3 min, max;
  u32 id;
};

struct Bit_Stream {
  u32 current_pos = 0u;
  u8 current_byte = 0u;
  std::vector<u8> bytes;
  float shannon_entropy() {
    u32 freq[0x100] = {};
    for (auto byte : bytes)
      freq[byte]++;
    float entr = 0.0f;
    for (auto f : freq)
      if (f)
        entr += float(f) * std::log2(float(f) / bytes.size());
    return -entr / bytes.size();
  }
  void decode_run_length8(Bit_Stream &out) {
    for (auto byte : bytes) {
      u32 run_value = byte & 1u;
      u32 run_length = byte >> 1u;
      for (u32 i = 0; i < run_length; i++)
        out.push_low_bit(run_value);
    }
    out.flush();
  }
  void push_byte(u8 byte) { bytes.push_back(byte); }
  void push_low_bit(u8 byte) {
    current_byte |= ((byte & 1u) << current_pos);
    current_pos++;
    if (current_pos == 8u) {
      bytes.push_back(current_byte);
      current_byte = 0u;
      current_pos = 0u;
    }
  }
  void flush() {
    if (current_byte)
      bytes.push_back(current_byte);
    current_byte = 0u;
    current_pos = 0u;
  }
  void run_length_encode4(Bit_Stream &out) {
    u32 run_length = 0u;
    u8 run_symbol = 0u;
    for (auto byte : bytes) {
      for (u32 bit = 0u; bit < 8u; bit++) {
        u8 cur_symbol = (byte >> bit) & 1u;
        if (cur_symbol == run_symbol) {
          run_length++;
          if (run_length == 0x7u) {
            out.push_low_bit(run_symbol);
            for (u32 j = 0; j < 3; j++)
              out.push_low_bit(1);
            run_length = 0;
          }
        } else {
          if (run_length != 0u) {
            out.push_low_bit(run_symbol);
            for (u32 j = 0; j < 3; j++)
              out.push_low_bit((run_length >> j) & 1);
          }
          run_symbol = cur_symbol;
          run_length = 1;
        }
      }
    }
    if (run_length != 0u) {
      out.push_low_bit(run_symbol);
      for (u32 j = 0; j < 3; j++)
        out.push_low_bit((run_length >> j) & 1);
    }
    out.flush();
  }
  void run_length_encode8(Bit_Stream &out) {
    u32 run_length = 0u;
    u8 run_symbol = 0u;
    for (auto byte : bytes) {
      for (u32 bit = 0u; bit < 8u; bit++) {
        u8 cur_symbol = (byte >> bit) & 1u;
        if (cur_symbol == run_symbol) {
          run_length++;
          if (run_length == 0x7fu) {
            out.push_low_bit(run_symbol);
            for (u32 j = 0; j < 7; j++)
              out.push_low_bit(1);
            run_length = 0;
          }
        } else {
          if (run_length != 0u) {
            out.push_low_bit(run_symbol);
            for (u32 j = 0; j < 7; j++)
              out.push_low_bit((run_length >> j) & 1);
          }
          run_symbol = cur_symbol;
          run_length = 1;
        }
      }
    }
    if (run_length != 0u) {
      out.push_low_bit(run_symbol);
      for (u32 j = 0; j < 7; j++)
        out.push_low_bit((run_length >> j) & 1);
    }
    out.flush();
  }
  void run_length_encode_zero_chunk8(Bit_Stream &out) {
    u32 run_length = 0u;
    u8 run_symbol = 0u;
    for (auto byte : bytes) {
      if (byte != 0u) {
        if (run_length != 0u) {
          out.push_byte(run_length);
          run_length = 0;
        }
        out.push_byte(0xffu);
        out.push_byte(byte);
      } else {
        run_length++;
        if (run_length == 0x7fu) {
          out.push_byte(run_length);
          run_length = 0;
        }
      }
    }
    if (run_length != 0u) {
      out.push_byte(run_length);
      run_length = 0;
    }
    out.flush();
  }
  void run_length_encode16(Bit_Stream &out) {
    u32 run_length = 0u;
    u8 run_symbol = 0u;
    for (auto byte : bytes) {
      for (u32 bit = 0u; bit < 8u; bit++) {
        u8 cur_symbol = (byte >> bit) & 1u;
        if (cur_symbol == run_symbol) {
          run_length++;
          if (run_length == 0x7fffu) {
            out.push_low_bit(run_symbol);
            for (u32 j = 0; j < 15; j++)
              out.push_low_bit(1);
            run_length = 0;
          }
        } else {
          if (run_length != 0u) {
            out.push_low_bit(run_symbol);
            for (u32 j = 0; j < 15; j++)
              out.push_low_bit((run_length >> j) & 1);
          }
          run_symbol = cur_symbol;
          run_length = 1;
        }
      }
    }
    if (run_length != 0u) {
      out.push_low_bit(run_symbol);
      for (u32 j = 0; j < 15; j++)
        out.push_low_bit((run_length >> j) & 1);
    }
    out.flush();
  }
};

struct Oct_Node {
  static const u32 COUNT_THRESHOLD = 100;
  static const u32 DEPTH_THRESHOLD = 5;
  vec3 min, max;
  bool leaf;
  u32 depth;
  std::vector<std::unique_ptr<Oct_Node>> children;
  std::vector<Oct_Item> items;
  Oct_Node(vec3 _min, vec3 _max, u32 _depth)
      : min(_min), max(_max), leaf(true), depth(_depth) {}
  void push(Oct_Item const &item) {
    float EPS = 1.0e-5f;
    if (item.min.x > this->max.x * (1.0f + EPS) ||
        item.min.y > this->max.y * (1.0f + EPS) ||
        item.min.z > this->max.z * (1.0f + EPS) ||
        item.max.x < this->min.x * (1.0f - EPS) ||
        item.max.y < this->min.y * (1.0f - EPS) ||
        item.max.z < this->min.z * (1.0f - EPS)) {
      return;
    }
    if (leaf) {
      items.push_back(item);
      if (items.size() > COUNT_THRESHOLD && depth < DEPTH_THRESHOLD) {
        leaf = false;
        vec3 dim = max - min;
        vec3 new_dim = (max - min) * 0.5f;
        for (u32 i = 0; i < 8; i++) {
          u32 dx = i & 1;
          u32 dy = (i >> 1) & 1;
          u32 dz = (i >> 2) & 1;
          vec3 new_min =
              min + 0.5f * dim * vec3(float(dx), float(dy), float(dz));
          vec3 new_max = new_min + new_dim;
          children.emplace_back(new Oct_Node(new_min, new_max, depth + 1));
          for (auto const &item : items) {
            children[i]->push(item);
          }
        }
        items.clear();
      }
    } else {
      for (u32 i = 0; i < 8; i++) {
        children[i]->push(item);
      }
    }
  }
  void fill_lines_render(std::vector<vec3> &lines) {
    auto push_cube = [&lines](float bin_idx, float bin_idy, float bin_idz,
                              float bin_size_x, float bin_size_y,
                              float bin_size_z) {
      {
        const u32 iter_x[] = {0, 0, 1, 1, 0, 0, 1, 1, 0, 0};
        const u32 iter_y[] = {0, 1, 1, 0, 0, 0, 0, 1, 1, 0};
        const u32 iter_z[] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
        ito(9) {
          lines.push_back(vec3{bin_idx + bin_size_x * f32(iter_x[i]),
                               bin_idy + bin_size_y * f32(iter_y[i]),
                               bin_idz + bin_size_z * f32(iter_z[i])});
          lines.push_back(vec3{bin_idx + bin_size_x * f32(iter_x[i + 1]),
                               bin_idy + bin_size_y * f32(iter_y[i + 1]),
                               bin_idz + bin_size_z * f32(iter_z[i + 1])});
        }
      }
      {
        const u32 iter_x[] = {
            0, 0, 1, 1, 1, 1,
        };
        const u32 iter_y[] = {
            1, 1, 1, 1, 0, 0,
        };
        const u32 iter_z[] = {
            0, 1, 0, 1, 0, 1,
        };
        ito(3) {
          lines.push_back(vec3{bin_idx + bin_size_x * f32(iter_x[i * 2]),
                               bin_idy + bin_size_y * f32(iter_y[i * 2]),
                               bin_idz + bin_size_z * f32(iter_z[i * 2])});
          lines.push_back(vec3{bin_idx + bin_size_x * f32(iter_x[i * 2 + 1]),
                               bin_idy + bin_size_y * f32(iter_y[i * 2 + 1]),
                               bin_idz + bin_size_z * f32(iter_z[i * 2 + 1])});
        }
      }
    };
    push_cube(min.x, min.y, min.z, max.x - min.x, max.y - min.y, max.z - min.z);
    if (!leaf) {
      for (u32 i = 0; i < 8; i++) {
        children[i]->fill_lines_render(lines);
      }
    }
  }
  void to_bit_table(Bit_Stream &bitstream) {}
};

struct Oct_Tree {
  std::unique_ptr<Oct_Node> root;
};

struct Packed_UG {
  // (arena_origin, arena_size)
  std::vector<uint> arena_table;
  // [point_id..]
  std::vector<uint> ids;
  vec3 min, max;
  uvec3 bin_count;
  f32 bin_size;
};

// Uniform Grid
//  ____
// |    |}size
// |____|}size
//
struct UG {
  vec3 min, max;
  uvec3 bin_count;
  u32 total_bin_count;
  f32 bin_size;
  std::vector<std::vector<uint>> bins;
  std::vector<uint> bins_indices;
  UG(float size, u32 bin_count)
      : UG(-vec3{size, size, size}, {size, size, size},
           2.0f * size / bin_count) {}
  UG(vec3 _min, vec3 _max, f32 _bin_size) : bin_size(_bin_size) {
    vec3 fbin_count = (_max - _min) / bin_size;
    fbin_count = vec3(std::ceil(fbin_count.x + 1.0e-7f),
                      std::ceil(fbin_count.y + 1.0e-7f),
                      std::ceil(fbin_count.z + 1.0e-7f));
    this->bin_count = uvec3(fbin_count);
    this->min = _min;
    this->max = _min + fbin_count * bin_size;
    bins.push_back({});
    this->total_bin_count = bin_count.x * bin_count.y * bin_count.z;
    for (u32 i = 0; i < total_bin_count; i++)
      bins_indices.push_back(0);
  }
  void to_bit_table(Bit_Stream &bitstream) {

    std::vector<u32> flag_table(total_bin_count);
    // First pass mark all boundary cells
    for (int dx = 0; dx < bin_count.x; dx++) {
      for (int dy = 0; dy < bin_count.y; dy++) {
        for (int dz = 0; dz < bin_count.z; dz++) {
          const auto flat_id = dx + dy * this->bin_count.x +
                               dz * this->bin_count.x * this->bin_count.y;
          const auto bin_id = this->bins_indices[flat_id];
          if (bin_id != 0) {
            flag_table[flat_id] = 1;
          }
        }
      }
    }
    // Now run the flood pass
    // We know that the volume does not have holes
    // And that it does not have inner empty bubbles
    // Having all that we just mark islands of disconnected cells
    // And then mark those islands that touch the boundary
    // The inner volume is those islands that does not touch the boundary
    u32 flag_counter = 1;
    while (true) {
      i32 x, y, z;
      bool picked = false;
      for (int dz = 0; dz < bin_count.z; dz++) {
        for (int dy = 0; dy < bin_count.y; dy++) {
          for (int dx = 0; dx < bin_count.x; dx++) {
            const auto flat_id = dx + dy * this->bin_count.x +
                                 dz * this->bin_count.x * this->bin_count.y;
            u8 flag = flag_table[flat_id];
            if (!flag) {
              x = dx;
              y = dy;
              z = dz;
              picked = true;
              break;
            }
          }
          if (picked)
            break;
        }
        if (picked)
          break;
      }
      if (!picked)
        break;
      flag_counter++;
      std::deque<uvec3> queue;
      queue.push_back({x, y, z});
      while (queue.size()) {
        auto item = queue.back();
        queue.pop_back();
        if (item.x < 0 || item.y < 0 || item.z < 0 ||
            item.x >= int(this->bin_count.x) ||
            item.y >= int(this->bin_count.y) ||
            item.z >= int(this->bin_count.z))
          continue;
        const auto flat_id = item.x + item.y * this->bin_count.x +
                             item.z * this->bin_count.x * this->bin_count.y;
        if (flag_table[flat_id] != 0)
          continue;
        flag_table[flat_id] = flag_counter;
        queue.push_back({item.x + 1, item.y, item.z});
        queue.push_back({item.x - 1, item.y, item.z});
        queue.push_back({item.x, item.y + 1, item.z});
        queue.push_back({item.x, item.y - 1, item.z});
        queue.push_back({item.x, item.y, item.z + 1});
        queue.push_back({item.x, item.y, item.z - 1});
      }
    }
    google::dense_hash_set<u32> boundary_set;
    boundary_set.set_empty_key(UINT32_MAX);
    // Now mark outer islands
    for (int dx = 0; dx < bin_count.x; dx++) {
      for (int dy = 0; dy < bin_count.y; dy++) {
        for (int dz = 0; dz < bin_count.z; dz++) {
          if (dx == 0 || dx == this->bin_count.x - 1 || dy == 0 ||
              dy == this->bin_count.y - 1 || dz == 0 ||
              dz == this->bin_count.z - 1) {
            const auto flat_id = dx + dy * this->bin_count.x +
                                 dz * this->bin_count.x * this->bin_count.y;
            const auto bin_id = this->bins_indices[flat_id];
            if (flag_table[flat_id] != 1) {
              boundary_set.insert(flag_table[flat_id]);
            }
          }
        }
      }
    }

    for (int dz = 0; dz < bin_count.z; dz++) {
      for (int dy = 0; dy < bin_count.y; dy++) {
        for (int dx = 0; dx < bin_count.x; dx++) {
          const auto flat_id = dx + dy * this->bin_count.x +
                               dz * this->bin_count.x * this->bin_count.y;
          // We put 1 if that cell is mesh boundary or inner volume
          if (flag_table[flat_id] &&
              boundary_set.find(flag_table[flat_id]) == boundary_set.end()) {
            bitstream.push_low_bit(1);
          } else {
            bitstream.push_low_bit(0);
          }
        }
      }
    }
    bitstream.flush();
  }
  Packed_UG pack() {
    Packed_UG out;
    out.min = min;
    out.max = max;
    out.bin_count = bin_count;
    out.bin_size = bin_size;
    out.ids.push_back(0);
    for (auto &bin_index : bins_indices) {
      if (bin_index > 0) {
        auto &bin = bins[bin_index];
        out.arena_table.push_back(out.ids.size());
        out.arena_table.push_back(bin.size());
        for (auto &id : bin) {
          out.ids.push_back(id);
        }
      } else {
        out.arena_table.push_back(0);
        out.arena_table.push_back(0);
      }
    }
    return out;
  }
  void put(vec3 const &pos, float radius, uint index) {
    put(pos, {radius, radius, radius}, index);
  }
  void put(vec3 const &pos, vec3 const &extent, uint index) {
    float EPS = 1.0e-1f;
    //    if (pos.x > this->max.x + extent.x * (1.0f + EPS) ||
    //        pos.y > this->max.y + extent.y * (1.0f + EPS) ||
    //        pos.z > this->max.z + extent.z * (1.0f + EPS) ||
    //        pos.x < this->min.x - extent.x * (1.0f + EPS) ||
    //        pos.y < this->min.y - extent.y * (1.0f + EPS) ||
    //        pos.z < this->min.z - extent.z * (1.0f + EPS)) {
    //      panic("");
    //      return;
    //    }
    ivec3 min_ids =
        ivec3((pos - min - vec3(EPS, EPS, EPS) - extent) / bin_size);
    ivec3 max_ids =
        ivec3((pos - min + vec3(EPS, EPS, EPS) + extent) / bin_size);
    for (int ix = min_ids.x; ix <= max_ids.x; ix++) {
      for (int iy = min_ids.y; iy <= max_ids.y; iy++) {
        for (int iz = min_ids.z; iz <= max_ids.z; iz++) {
          // Boundary check
          if (ix < 0 || iy < 0 || iz < 0 || ix >= int(this->bin_count.x) ||
              iy >= int(this->bin_count.y) || iz >= int(this->bin_count.z)) {
            continue;
          }
          u32 flat_id = ix + iy * this->bin_count.x +
                        iz * this->bin_count.x * this->bin_count.y;
          auto *bin_id = &this->bins_indices[flat_id];
          if (*bin_id == 0) {
            this->bins.push_back({});
            *bin_id = this->bins.size() - 1;
          }
          this->bins[*bin_id].push_back(index);
        }
      }
    }
  }
  bool intersect_box(vec3 ray_invdir, vec3 ray_origin, float &hit_min,
                     float &hit_max) {
    vec3 tbot = ray_invdir * (this->min - ray_origin);
    vec3 ttop = ray_invdir * (this->max - ray_origin);
    vec3 tmin = glm::min(ttop, tbot);
    vec3 tmax = glm::max(ttop, tbot);
    vec2 t = vec2(std::max(tmin.x, tmin.y), std::max(tmin.x, tmin.z));
    float t0 = std::max(t.x, t.y);
    t = vec2(std::min(tmax.x, tmax.y), std::min(tmax.x, tmax.z));
    float t1 = std::min(t.x, t.y);
    hit_min = t0;
    hit_max = t1;
    return t1 > std::max(t0, 0.0f);
  }
  // on_hit returns false to early-out the traversal
  void
  iterate(vec3 ray_dir, vec3 ray_origin,
          std::function<bool(std::vector<u32> const &, float t_max)> on_hit) {
          // @Cleanup: Fix devision by zero
    ito(3) if (std::abs(ray_dir[i]) < 1.0e-7f) ray_dir[i] =
        (std::signbit(ray_dir[i]) ? -1.0f : 1.0f) * 1.0e-7f;
    vec3 ray_invdir = 1.0f / ray_dir;
    float hit_min;
    float hit_max;
    if (!intersect_box(ray_invdir, ray_origin, hit_min, hit_max))
      return;
    hit_min = std::max(0.0f, hit_min);
    vec3 hit_pos = ray_origin + ray_dir * hit_min;
    ivec3 exit, step, cell_id;
    vec3 axis_delta, axis_distance;
    for (uint i = 0; i < 3; ++i) {
      // convert ray starting point to cell_id coordinates
      float ray_offset = hit_pos[i] - this->min[i];
      cell_id[i] = int(glm::clamp(floor(ray_offset / this->bin_size), 0.0f,
                                  float(this->bin_count[i]) - 1.0f));
      // hit_normal[i] = cell_id[i];
      if (std::abs(ray_dir[i]) < 1.0e-5f) {
        axis_delta[i] = 0.0f;
        axis_distance[i] = 1.0e10f;
        step[i] = 0;
      } else if (ray_dir[i] < 0) {
        axis_delta[i] = -this->bin_size * ray_invdir[i];
        axis_distance[i] =
            (cell_id[i] * this->bin_size - ray_offset) * ray_invdir[i];
        // exit[i] = -1;
        step[i] = -1;
      } else {
        axis_delta[i] = this->bin_size * ray_invdir[i];
        axis_distance[i] =
            ((cell_id[i] + 1) * this->bin_size - ray_offset) * ray_invdir[i];
        // exit[i] = int();
        step[i] = 1;
      }
    }
    int cell_delta[3] = {step[0], step[1], step[2]};
    while (true) {
      uint k = (uint(axis_distance[0] < axis_distance[1]) << 2) +
               (uint(axis_distance[0] < axis_distance[2]) << 1) +
               (uint(axis_distance[1] < axis_distance[2]));
      const uint map[8] = {2, 1, 2, 1, 2, 2, 0, 0};
      uint axis = map[k];
      float t_max = axis_distance[axis];
      uint cell_id_offset = cell_id[2] * bin_count[0] * bin_count[1] +
                            cell_id[1] * bin_count[0] + cell_id[0];
      uint o = cell_id_offset;
      uint bin_offset = this->bins_indices[o];
      // If the current node has items
      if (bin_offset > 0) {
        if (!on_hit(this->bins[bin_offset],
                    (t_max + hit_min) * (1.0f + 1.0e-5f)))
          return;
      }
      axis_distance[axis] += axis_delta[axis];
      cell_id[axis] += cell_delta[axis];
      if (cell_id[axis] < 0 || cell_id[axis] >= bin_count[axis])
        break;
    }
  }
  static void push_cube(std::vector<vec3> &lines, float bin_idx, float bin_idy,
                        float bin_idz, float bin_size_x, float bin_size_y,
                        float bin_size_z){
      {const u32 iter_x[] = {0, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  const u32 iter_y[] = {0, 1, 1, 0, 0, 0, 0, 1, 1, 0};
  const u32 iter_z[] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
  ito(9) {
    lines.push_back(vec3{bin_idx + bin_size_x * f32(iter_x[i]),
                         bin_idy + bin_size_y * f32(iter_y[i]),
                         bin_idz + bin_size_z * f32(iter_z[i])});
    lines.push_back(vec3{bin_idx + bin_size_x * f32(iter_x[i + 1]),
                         bin_idy + bin_size_y * f32(iter_y[i + 1]),
                         bin_idz + bin_size_z * f32(iter_z[i + 1])});
  }
} {
  const u32 iter_x[] = {
      0, 0, 1, 1, 1, 1,
  };
  const u32 iter_y[] = {
      1, 1, 1, 1, 0, 0,
  };
  const u32 iter_z[] = {
      0, 1, 0, 1, 0, 1,
  };
  ito(3) {
    lines.push_back(vec3{bin_idx + bin_size_x * f32(iter_x[i * 2]),
                         bin_idy + bin_size_y * f32(iter_y[i * 2]),
                         bin_idz + bin_size_z * f32(iter_z[i * 2])});
    lines.push_back(vec3{bin_idx + bin_size_x * f32(iter_x[i * 2 + 1]),
                         bin_idy + bin_size_y * f32(iter_y[i * 2 + 1]),
                         bin_idz + bin_size_z * f32(iter_z[i * 2 + 1])});
  }
}
}
;
void fill_lines_render(std::vector<vec3> &lines) {

  push_cube(lines, min.x, min.y, min.z, max.x - min.x, max.y - min.y,
            max.z - min.z);
  for (int dx = 0; dx < bin_count.x; dx++) {
    for (int dy = 0; dy < bin_count.y; dy++) {
      for (int dz = 0; dz < bin_count.z; dz++) {
        const auto flat_id = dx + dy * this->bin_count.x +
                             dz * this->bin_count.x * this->bin_count.y;
        const auto bin_id = this->bins_indices[flat_id];
        if (bin_id != 0) {
          const auto bin_idx = bin_size * f32(dx) + this->min.x;
          const auto bin_idy = bin_size * f32(dy) + this->min.y;
          const auto bin_idz = bin_size * f32(dz) + this->min.z;
          push_cube(lines, bin_idx, bin_idy, bin_idz, bin_size, bin_size,
                    bin_size);
        }
      }
    }
  }
}
std::vector<u32> traverse(vec3 const &pos, f32 radius) {
  if (pos.x > this->max.x + radius || pos.y > this->max.y + radius ||
      pos.z > this->max.z + radius || pos.x < this->min.x - radius ||
      pos.y < this->min.y - radius || pos.z < this->min.z - radius) {
    panic("");
    return;
  }
  ivec3 min_ids = ivec3((pos + min - vec3(radius, radius, radius)) / bin_size);
  ivec3 max_ids = ivec3((pos + min + vec3(radius, radius, radius)) / bin_size);
  google::dense_hash_set<u32> set;
  set.set_empty_key(UINT32_MAX);
  for (int ix = min_ids.x; ix <= max_ids.x; ix++) {
    for (int iy = min_ids.y; iy <= max_ids.y; iy++) {
      for (int iz = min_ids.z; iz <= max_ids.z; iz++) {
        // Boundary check
        if (ix < 0 || iy < 0 || iz < 0 || ix >= int(this->bin_count.x) ||
            iy >= int(this->bin_count.y) || iz >= int(this->bin_count.z)) {
          continue;
        }
        u32 flat_id = ix + iy * this->bin_count.x +
                      iz * this->bin_count.x * this->bin_count.y;
        auto bin_id = this->bins_indices[flat_id];
        if (bin_id != 0) {
          for (auto const &item : this->bins[bin_id]) {
            set.insert(item);
          }
        }
      }
    }
  }
  std::vector<u32> out;
  out.reserve(set.size());
  for (auto &i : set)
    out.push_back(i);
  return out;
}
}
;

struct Pair_Hash {
  u64 operator()(std::pair<u32, u32> const &pair) {
    return std::hash<u32>()(pair.first) ^ std::hash<u32>()(pair.second);
  }
};

struct Simulation_State {
  // Static constants
  f32 rest_length;
  f32 spring_factor;
  f32 repell_factor;
  f32 planar_factor;
  f32 bulge_factor;
  f32 cell_radius;
  f32 cell_mass;
  f32 domain_radius;
  u32 birth_rate;
  // Dynamic state
  std::vector<vec3> particles;
  google::dense_hash_set<std::pair<u32, u32>, Pair_Hash> links;
  f32 system_size;
  Random_Factory rf;
  // Methods
  void dump(std::string const &filename) {
    std::ofstream out(filename, std::ios::binary | std::ios::out);
    out << rest_length << "\n";
    out << spring_factor << "\n";
    out << repell_factor << "\n";
    out << planar_factor << "\n";
    out << bulge_factor << "\n";
    out << cell_radius << "\n";
    out << cell_mass << "\n";
    out << domain_radius << "\n";
    out << birth_rate << "\n";
    out << particles.size() << "\n";
    for (u32 i = 0; i < particles.size(); i++) {
      out << particles[i].x << "\n";
      out << particles[i].y << "\n";
      out << particles[i].z << "\n";
    }
    out << links.size() << "\n";
    for (auto link : links) {
      out << link.first << "\n";
      out << link.second << "\n";
    }
  }
  void restore_or_default(std::string const &filename) {
    std::ifstream is(filename, std::ios::binary | std::ios::in);
    if (is.is_open()) {
      links.set_empty_key({UINT32_MAX, UINT32_MAX});
      is >> rest_length;
      is >> spring_factor;
      is >> repell_factor;
      is >> planar_factor;
      is >> bulge_factor;
      is >> cell_radius;
      is >> cell_mass;
      is >> domain_radius;
      is >> birth_rate;
      u32 particles_count;
      is >> particles_count;
      particles.resize(particles_count);
      for (u32 i = 0; i < particles_count; i++) {
        is >> particles[i].x;
        is >> particles[i].y;
        is >> particles[i].z;
      }
      u32 links_count;
      is >> links_count;
      for (u32 k = 0; k < particles_count; k++) {
        u32 i, j;
        is >> i;
        is >> j;
        links.insert({i, j});
      }
      update_size();
    } else {
      init_default();
    }
  }
  void init_default() {
    *this = Simulation_State{.rest_length = 0.35f,
                             .spring_factor = 100.f,
                             .repell_factor = 3.0e-1f,
                             .planar_factor = 10.0f,
                             .bulge_factor = 10.0f,
                             .cell_radius = 0.025f,
                             .cell_mass = 10.0f,
                             .domain_radius = 10.0f,
                             .birth_rate = 100u};
    links.set_empty_key({UINT32_MAX, UINT32_MAX});
    links.insert({0, 1});
    particles.push_back({0.0f, 0.0f, -cell_radius});
    particles.push_back({0.0f, 0.0f, cell_radius});
    system_size = cell_radius;
  }
  void update_size() {
    system_size = 0.0f;
    for (auto const &pnt : particles) {
      system_size = std::max(
          system_size, std::max(std::abs(pnt.x),
                                std::max(std::abs(pnt.y), std::abs(pnt.z))));
      ;
    }
    system_size += rest_length;
  }
  void step(float dt) {
    auto ug = UG(system_size, system_size / rest_length);
    {
      u32 i = 0;
      for (auto const &pnt : particles) {
        ug.put(pnt, 0.0f, i);
        i++;
      }
    }
    std::vector<f32> force_table(particles.size());
    std::vector<vec3> new_particles = particles;
    // Repell
    {
      u32 i = 0;
      for (auto const &old_pos_0 : particles) {
        auto close_points = ug.traverse(old_pos_0, rest_length);
        vec3 new_pos_0 = new_particles[i];
        float acc_force = 0.0f;
        for (u32 j : close_points) {
          if (j <= i)
            continue;
          vec3 const old_pos_1 = particles[j];
          vec3 new_pos_1 = new_particles[j];
          f32 const dist = glm::distance(old_pos_0, old_pos_1);
          if (dist < rest_length * 0.9) {
            links.insert({i, j});
          }
          f32 const force = repell_factor * cell_mass / (dist * dist + 1.0f);
          acc_force += std::abs(force);
          auto const vforce =
              (old_pos_0 - old_pos_1) / (dist + 1.0f) * force * dt;
          new_pos_0 += vforce;
          new_pos_1 -= vforce;
          new_particles[j] = new_pos_1;
          force_table[j] += std::abs(force);
        }
        new_particles[i] = new_pos_0;
        force_table[i] += acc_force;
        i++;
      }
    }
    // Attract
    for (auto const &link : links) {
      ASSERT_PANIC(link.first < link.second);
      u32 i = link.first;
      u32 j = link.second;
      vec3 const old_pos_0 = particles[i];
      vec3 const new_pos_0 = new_particles[i];
      vec3 const old_pos_1 = particles[j];
      vec3 const new_pos_1 = new_particles[j];
      f32 const dist = glm::distance(old_pos_0, old_pos_1);
      f32 const force = spring_factor * (rest_length - dist) / dist;
      vec3 const vforce = (old_pos_0 - old_pos_1) * (force * dt);
      new_particles[i] = new_pos_0 + vforce;
      new_particles[j] = new_pos_1 - vforce;
      force_table[i] += std::abs(force);
      force_table[j] += std::abs(force);
    }

    // Planarization
    struct Planar_Target {
      vec3 target;
      u32 n_divisor;
    };
    std::vector<Planar_Target> spring_target(particles.size());
    {
      u32 i = 0;
      for (auto const &old_pos_0 : particles) {
        spring_target[i] =
        Planar_Target{target : vec3(0.0f, 0.0f, 0.0f), n_divisor : 0};
        i++;
      }
    }
    for (auto const &link : links) {
      u32 i = link.first;
      u32 j = link.second;
      vec3 const old_pos_0 = particles[i];
      vec3 const old_pos_1 = particles[j];
      spring_target[i].target += old_pos_1;
      spring_target[i].n_divisor += 1;
      spring_target[j].target += old_pos_0;
      spring_target[j].n_divisor += 1;
    }
    {
      u32 i = 0;
      for (auto const &old_pos_0 : particles) {
        auto const st = spring_target[i];
        if (st.n_divisor == 0) {
          i++;
          continue;
        }
        auto const average_target = st.target / float(st.n_divisor);
        auto const dist = distance(old_pos_0, average_target);
        auto const force = spring_factor * dist;
        auto const vforce = dt * (average_target - old_pos_0) * force;
        force_table[i] += std::abs(force);
        new_particles[i] += vforce;
        i++;
      }
    }

    // Division
    {
      u32 i = 0;
      for (auto const &old_pos_0 : particles) {
        if (rf.uniform(0, birth_rate) == 0 && force_table[i] < 120.0f) {
          new_particles.push_back(old_pos_0 + rf.rand_unit_cube() * 1.0e-3f);
        }
        i++;
      }
    }
    // Force into the domain
    {
      u32 i = 0;
      for (auto &new_pos_0 : new_particles) {
        new_pos_0.z -= new_pos_0.z * dt;
        if (new_pos_0.z < 0.0f) {
          new_pos_0.z = 0.0f;
        }
        i++;
      }
    }

    // Apply the changes
    particles = new_particles;
    update_size();
  }
};
