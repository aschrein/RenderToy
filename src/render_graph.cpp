#include "../include/assets.hpp"
#include "../include/device.hpp"
#include "../include/error_handling.hpp"
#include "../include/memory.hpp"
#include "../include/model_loader.hpp"
#include "../include/shader_compiler.hpp"
#include <../include/render_graph.hpp>
#include <sparsehash/dense_hash_map>
#include <sparsehash/dense_hash_set>

#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>

#include <shaders.h>

#include <imgui.h>

#include "examples/imgui_impl_vulkan.h"

using namespace render_graph;

struct Graphics_Pipeline_State {
  //  vk::PipelineCreateFlags flags;
  //  uint32_t stageCount;
  //  vk::PipelineShaderStageCreateInfo Stages;
  //  vk::PipelineVertexInputStateCreateInfo VertexInputState;
  //  vk::PipelineInputAssemblyStateCreateInfo InputAssemblyState;
  //  vk::PipelineTessellationStateCreateInfo TessellationState;
  //  vk::PipelineViewportStateCreateInfo ViewportState;
  //  vk::PipelineRasterizationStateCreateInfo RasterizationState;
  //  vk::PipelineMultisampleStateCreateInfo MultisampleState;
  //  vk::PipelineDepthStencilStateCreateInfo DepthStencilState;
  //  vk::PipelineColorBlendStateCreateInfo ColorBlendState;
  //  vk::PipelineDynamicStateCreateInfo DynamicState;
  //  vk::PipelineLayout layout;
  //  vk::RenderPass renderPass;
  //  uint32_t subpass;
  //  vk::Pipeline basePipelineHandle;
  //  int32_t basePipelineIndex;
  vk::CullModeFlags cull_mode;
  vk::FrontFace front_face;
  vk::PolygonMode polygon_mode;
  float line_width;
  bool enable_depth_test;
  vk::CompareOp cmp_op;
  bool enable_depth_write;
  float max_depth;
  vk::PrimitiveTopology topology;
  float depth_bias_const;
  u32 ps, vs;
  u32 pass;
  u64 dummy;
  bool operator==(const Graphics_Pipeline_State &that) const {
    return memcmp(this, &that, sizeof(*this)) == 0;
  }
  Graphics_Pipeline_State(u64 _dummy = 0) {
    memset(this, 0, sizeof(*this));
    dummy = _dummy;
  }
};

struct Graphics_Pipeline_State_Hash {
  u64 operator()(Graphics_Pipeline_State const &state) const {
    u64 out = 0ull;
    u8 *data = (u8 *)&state;
    ito(sizeof(Graphics_Pipeline_State)) {
      out = out ^ std::hash<u8>()(data[i]);
    }
    return out;
  }
};

struct Image_Layout {
  vk::ImageLayout layout;
  vk::AccessFlags access_flags;
};

struct RT_Details : public Slot {
  std::string name;
  u32 image_id;
};

enum class Pass_Type { Graphics, Compute };

struct Pass_Input {
  std::string name;
  bool history;
};

struct Pass_Details : public Slot {
  std::string name;
  Pass_Type type;
  std::vector<Pass_Input> input;
  std::vector<std::string> output;
  u32 width;
  u32 height;
  bool use_depth;
  u32 depth_target;
  vk::UniqueRenderPass pass;
  vk::UniqueFramebuffer fb;
  std::function<void()> on_exec;

  void destroy() {
    pass.reset();
    fb.reset();
    input.clear();
    output.clear();
  }
};
enum class Resource_Type { BUFFER, TEXTURE, RT, NONE };
struct Resource_Desc : public Slot {
  Resource_Type type;
  u32 ref;
};

// ...
// T: disable, is_alive, set_id, get_id
template <typename T> struct Slot_Machine {
  std::vector<T> slots;
  std::vector<u32> free_slots;
  // (timer, item)
  std::vector<std::pair<u32, u32>> limbo_slots;
  std::function<void(T &)> deleter;
  u32 push(T &&t) {
    if (free_slots.size()) {
      auto id = free_slots.back();
      free_slots.pop_back();
      slots[id - 1] = std::move(t);
      slots[id - 1].set_alive();
      slots[id - 1].set_id(id);
      return id;
    }
    slots.emplace_back(std::forward<T>(t));
    slots.back().set_alive();
    slots.back().set_id(slots.size());
    return slots.size();
  }
  T &operator[](u32 id) {
    ASSERT_PANIC(id && slots[id - 1].get_id() == id &&
                 slots[id - 1].is_alive());
    return slots[id - 1];
  }
  void remove(u32 id) {
    ASSERT_PANIC(id);
    slots[id - 1].disable();
    limbo_slots.push_back({3, id});
  }
  u32 count() { return slots.size(); }
  void for_each(std::function<void(T &)> fn) {
    for (auto &item : slots) {
      if (item.is_alive())
        fn(item);
    }
  }
  void tick() {
    std::vector<std::pair<u32, u32>> new_limbo_slots;
    for (auto &item : limbo_slots) {
      ASSERT_PANIC(item.first);
      item.first -= 1;
      if (item.first == 0) {
        deleter(slots[item.second - 1]);
        free_slots.push_back(item.second);
      } else {
        new_limbo_slots.push_back(item);
      }
    }
    limbo_slots = new_limbo_slots;
  }
  void ImGui_Emit_Stats(std::string const &name) {
    ImGui::Value((name + ":").c_str(), (u32)slots.size());
  }
};

// Used per frame
struct Descriptor_Frame {
  Device_Wrapper *device_wrapper;
  vk::UniqueDescriptorPool descset_pool;
  // Shader id -> Set group id
  google::dense_hash_map<u32, u32> descset_table;
  // Used to track update-after-bound error
  google::dense_hash_set<u64> dirty_set;
  // @TODO: Merge similar groups for different shaders
  std::vector<std::vector<vk::UniqueDescriptorSet>> descset_groups;
  google::dense_hash_map<std::string, vk::DescriptorSet> imgui_table;

  bool invalidate = false;

  void reset() { invalidate = true; }

  vk::DescriptorSet allocate_imgui(std::string const &name, vk::Sampler sampler,
                                   vk::ImageView image_view,
                                   vk::ImageLayout image_layout) {
    if (imgui_table.find(name) == imgui_table.end()) {
      imgui_table.insert(
          {name, (VkDescriptorSet)ImGui_ImplVulkan_AddTexture(
                     (VkSampler)sampler, (VkImageView)image_view,
                     (VkImageLayout)image_layout, descset_pool.get())});
    }
    return imgui_table.find(name)->second;
  }
  void allocate_descset(Pipeline_Wrapper &pwrap) {
    u32 pipe_id = pwrap.id;
    std::vector<vk::UniqueDescriptorSet> desc_sets;
    auto raw_set_layouts = pwrap.get_raw_descset_layouts();
    ASSERT_PANIC(raw_set_layouts.size());
    desc_sets = device_wrapper->device->allocateDescriptorSetsUnique(
        vk::DescriptorSetAllocateInfo()
            .setPSetLayouts(&raw_set_layouts[0])
            .setDescriptorPool(descset_pool.get())
            .setDescriptorSetCount(raw_set_layouts.size()));
    descset_groups.emplace_back(std::move(desc_sets));
    // Erase previous entry
    descset_table.erase(pipe_id);
    descset_table.insert({pipe_id, descset_groups.size()});
    // Something must go wrong to hit this
    ASSERT_PANIC(descset_table.size() < 128);
  }
  std::vector<vk::DescriptorSet>
  get_or_create_descsets(Pipeline_Wrapper &pwrap) {
    if (!descset_pool) {
      vk::DescriptorPoolSize aPoolSizes[] = {
          {vk::DescriptorType::eSampler, 1000},
          {vk::DescriptorType::eCombinedImageSampler, 1000},
          {vk::DescriptorType::eSampledImage, 4096},
          {vk::DescriptorType::eStorageImage, 1000},
          {vk::DescriptorType::eUniformTexelBuffer, 1000},
          {vk::DescriptorType::eStorageTexelBuffer, 1000},
          {vk::DescriptorType::eCombinedImageSampler, 1000},
          {vk::DescriptorType::eStorageBuffer, 1000},
          {vk::DescriptorType::eUniformBufferDynamic, 1000},
          {vk::DescriptorType::eStorageBufferDynamic, 1000},
          {vk::DescriptorType::eInputAttachment, 1000}};
      descset_pool = device_wrapper->device->createDescriptorPoolUnique(
          vk::DescriptorPoolCreateInfo(
              vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet |
                  vk::DescriptorPoolCreateFlagBits::eUpdateAfterBindEXT,
              1000 * 11, 11, aPoolSizes));
      invalidate = false;
    } else if (invalidate) {
      imgui_table.clear();
      descset_groups.clear();
      descset_table.clear();
      device_wrapper->device->resetDescriptorPool(descset_pool.get());
      dirty_set.clear();
      invalidate = false;
    }
    u32 pipe_id = pwrap.id;
    if (descset_table.find(pipe_id) == descset_table.end()) {
      allocate_descset(pwrap);
    }
    std::vector<vk::DescriptorSet> raw_desc_sets;
    std::vector<uint32_t> raw_desc_sets_offsets;
    u32 group_id = descset_table.find(pipe_id)->second;
    for (auto &uds : descset_groups[group_id - 1]) {
      raw_desc_sets.push_back(uds.get());
      raw_desc_sets_offsets.push_back(0);
    }
    // HACK
    // @Cleanup:
    // Here we check that if a descset is dirty we allocate a new one
    // But we don't keep any references to the old one so the only way
    // To invalidate it is to reset the whole pool
    bool dirty = false;
    for (auto set : raw_desc_sets) {
      if (dirty_set.find((u64)(VkDescriptorSet)set) != dirty_set.end()) {
        dirty = true;
      }
    }
    if (dirty) {
      allocate_descset(pwrap);
      std::vector<vk::DescriptorSet> raw_desc_sets;
      std::vector<uint32_t> raw_desc_sets_offsets;
      u32 group_id = descset_table.find(pipe_id)->second;
      for (auto &uds : descset_groups[group_id - 1]) {
        raw_desc_sets.push_back(uds.get());
        raw_desc_sets_offsets.push_back(0);
      }
      return raw_desc_sets;
    }
    // EOF HACK
    return raw_desc_sets;
  }
  Descriptor_Frame() {
    descset_table.set_empty_key(0u);
    dirty_set.set_empty_key(0u);
    imgui_table.set_empty_key("null");
    descset_table.set_deleted_key(u32(-1));
    dirty_set.set_deleted_key(u32(-1));
    imgui_table.set_deleted_key("deleted");
  }
  void end_frame() { dirty_set.clear(); }
  void bind_pipeline(vk::CommandBuffer &cmd, Pipeline_Wrapper &pwrap) {
    cmd.bindPipeline(pwrap.bind_point, pwrap.pipeline.get());
    if (pwrap.collect_sets().size() == 0)
      return;
    auto raw_descsets = get_or_create_descsets(pwrap);
    for (auto set : raw_descsets) {
      dirty_set.insert((u64)(VkDescriptorSet)set);
    }
    if (raw_descsets.size())
      cmd.bindDescriptorSets(pwrap.bind_point, pwrap.pipeline_layout.get(), 0,
                             raw_descsets, {});
  }
  void update_descriptor(
      Pipeline_Wrapper &pwrap, std::string const &name, vk::Buffer buffer,
      size_t origin, size_t size,
      vk::DescriptorType type = vk::DescriptorType::eStorageBuffer,
      u32 offset = 0u) {
    ASSERT_PANIC(pwrap.resource_slots.find(name) != pwrap.resource_slots.end());
    auto slot = pwrap.resource_slots[name];
    ASSERT_PANIC(
        slot.layout.descriptorType == vk::DescriptorType::eStorageBuffer ||
        slot.layout.descriptorType == vk::DescriptorType::eUniformBuffer);
    auto raw_descsets = get_or_create_descsets(pwrap);

    device_wrapper->device->updateDescriptorSets(
        {vk::WriteDescriptorSet()
             .setDstSet(raw_descsets[slot.set])
             .setDstBinding(slot.layout.binding)
             .setDescriptorCount(1)
             .setDstArrayElement(offset)
             .setDescriptorType(slot.layout.descriptorType)
             .setPBufferInfo(&vk::DescriptorBufferInfo()
                                  .setBuffer(buffer)
                                  .setRange(size)
                                  .setOffset(origin))},
        {});
  }
  void update_storage_image_descriptor(Pipeline_Wrapper &pwrap,
                                       std::string const &name,
                                       vk::ImageView image_view,
                                       u32 offset = 0u) {
    ASSERT_PANIC(pwrap.resource_slots.find(name) != pwrap.resource_slots.end());
    auto slot = pwrap.resource_slots[name];
    ASSERT_PANIC(slot.layout.descriptorType ==
                 vk::DescriptorType::eStorageImage);
    auto raw_descsets = get_or_create_descsets(pwrap);
    for (auto set : raw_descsets) {
      // If this fires it means that we're trying to modify a set that is
      // already bound
      ASSERT_PANIC(dirty_set.find((u64)(VkDescriptorSet)set) ==
                   dirty_set.end());
    }
    device_wrapper->device->updateDescriptorSets(
        {vk::WriteDescriptorSet()
             .setDstSet(raw_descsets[slot.set])
             .setDstBinding(slot.layout.binding)
             .setDescriptorCount(1)
             .setDstArrayElement(offset)
             .setDescriptorType(vk::DescriptorType::eStorageImage)
             .setPImageInfo(&vk::DescriptorImageInfo()
                                 .setImageView(image_view)
                                 .setImageLayout(vk::ImageLayout::eGeneral)
                                 .setSampler(vk::Sampler()))},
        {});
  }
  void update_sampled_image_descriptor(
      Pipeline_Wrapper &pwrap, std::string const &name,
      vk::ImageView image_view, vk::Sampler sampler, u32 offset = 0u,
      vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal) {
    ASSERT_PANIC(pwrap.resource_slots.find(name) != pwrap.resource_slots.end());
    auto slot = pwrap.resource_slots[name];
    ASSERT_PANIC(slot.layout.descriptorType ==
                 vk::DescriptorType::eCombinedImageSampler);
    auto raw_descsets = get_or_create_descsets(pwrap);
    for (auto set : raw_descsets) {
      ASSERT_PANIC(dirty_set.find((u64)(VkDescriptorSet)set) ==
                   dirty_set.end());
    }
    device_wrapper->device->updateDescriptorSets(
        {vk::WriteDescriptorSet()
             .setDstSet(raw_descsets[slot.set])
             .setDstBinding(slot.layout.binding)
             .setDescriptorCount(1)
             .setDstArrayElement(offset)
             .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
             .setPImageInfo(&vk::DescriptorImageInfo()
                                 .setImageView(image_view)
                                 .setImageLayout(layout)
                                 .setSampler(sampler))},
        {});
  }
};

std::vector<u8> build_mips(std::vector<u8> const &data, u32 width, u32 height,
                           vk::Format format, u32 &out_miplevels,
                           std::vector<u32> &mip_offsets,
                           std::vector<uvec2> &mip_sizes) {
  u32 big_dim = std::max(width, height);
  out_miplevels = 0u;
  ito(32u) {
    if ((big_dim & (1u << i)) != 0u) {
      out_miplevels = i + 1u;
    }
  }
  ito(out_miplevels) mip_sizes.push_back(
      uvec2(std::max(1u, width >> i), std::max(1u, height >> i)));

  // @TODO: Add more formats
  // Bytes per pixel
  u32 bpc = 4u;
  switch (format) {
  case vk::Format::eR8G8B8A8Unorm:
  case vk::Format::eR8G8B8A8Srgb:
    bpc = 4u;
    break;
  case vk::Format::eR32G32B32Sfloat:
    bpc = 12u;
    break;
  default:
    ASSERT_PANIC(false && "unsupported format");
  }
  u32 total_bytes = 0u;
  ito(out_miplevels) {
    mip_offsets.push_back(total_bytes);
    total_bytes += mip_sizes[i].x * mip_sizes[i].y * bpc;
  }
  std::vector<u8> out(total_bytes);
  memcpy(&out[0], &data[0], data.size());
  auto load_f32 = [&](uvec2 coord, u32 level, u32 component) {
    uvec2 size = mip_sizes[level];
    return *(f32 *)&out[mip_offsets[level] + coord.x * bpc +
                        coord.y * size.x * bpc + component * 4u];
  };
  auto load = [&](uvec2 coord, u32 level) {
    uvec2 size = mip_sizes[level];
    if (coord.x >= size.x)
      coord.x = size.x - 1;
    if (coord.y >= size.y)
      coord.y = size.y - 1;
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm: {
      u8 r = out[mip_offsets[level] + coord.x * bpc + coord.y * size.x * bpc];
      u8 g =
          out[mip_offsets[level] + coord.x * bpc + coord.y * size.x * bpc + 1u];
      u8 b =
          out[mip_offsets[level] + coord.x * bpc + coord.y * size.x * bpc + 2u];
      u8 a =
          out[mip_offsets[level] + coord.x * bpc + coord.y * size.x * bpc + 3u];
      return vec4(float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f,
                  float(a) / 255.0f);
    }
    case vk::Format::eR8G8B8A8Srgb: {
      u8 r = out[mip_offsets[level] + coord.x * bpc + coord.y * size.x * bpc];
      u8 g =
          out[mip_offsets[level] + coord.x * bpc + coord.y * size.x * bpc + 1u];
      u8 b =
          out[mip_offsets[level] + coord.x * bpc + coord.y * size.x * bpc + 2u];
      u8 a =
          out[mip_offsets[level] + coord.x * bpc + coord.y * size.x * bpc + 3u];

      auto out = vec4(float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f,
                      float(a) / 255.0f);
      out.r = std::pow(out.r, 2.2f);
      out.g = std::pow(out.g, 2.2f);
      out.b = std::pow(out.b, 2.2f);
      out.a = std::pow(out.a, 2.2f);
      return out;
    }
    case vk::Format::eR32G32B32Sfloat: {
      f32 r = load_f32(coord, level, 0u);
      f32 g = load_f32(coord, level, 1u);
      f32 b = load_f32(coord, level, 2u);
      return vec4(r, g, b, 0.0f);
    }
    default:
      ASSERT_PANIC(false && "unsupported format");
    }
  };
  auto write = [&](vec4 val, uvec2 coord, u32 level) {
    uvec2 size = mip_sizes[level];
    if (coord.x >= size.x)
      coord.x = size.x - 1;
    if (coord.y >= size.y)
      coord.y = size.y - 1;
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm: {
      auto size = mip_sizes[level];
      u8 r = u8(255.0f * val.x);
      u8 g = u8(255.0f * val.y);
      u8 b = u8(255.0f * val.z);
      u8 a = u8(255.0f * val.w);
      u8 *dst =
          &out[mip_offsets[level] + coord.x * bpc + coord.y * size.x * bpc];
      dst[0] = r;
      dst[1] = g;
      dst[2] = b;
      dst[3] = a;
      return;
    }
    case vk::Format::eR8G8B8A8Srgb: {
      auto size = mip_sizes[level];
      ito(4) val[i] = std::pow(val[i], 1.0f / 2.2f);
      u8 r = u8(255.0f * val.x);
      u8 g = u8(255.0f * val.y);
      u8 b = u8(255.0f * val.z);
      u8 a = u8(255.0f * val.w);

      u8 *dst =
          &out[mip_offsets[level] + coord.x * bpc + coord.y * size.x * bpc];
      dst[0] = r;
      dst[1] = g;
      dst[2] = b;
      dst[3] = a;
      return;
    }
    case vk::Format::eR32G32B32Sfloat: {
      f32 *dst = (f32 *)&out[mip_offsets[level] + coord.x * bpc +
                             coord.y * size.x * bpc];
      dst[0] = val.x;
      dst[1] = val.y;
      dst[2] = val.z;
      return;
    }
    default:
      ASSERT_PANIC(false && "unsupported format");
    }
  };
  for (u32 mip_level = 1u; mip_level < out_miplevels; mip_level++) {
    auto size = mip_sizes[mip_level];
    ito(size.y) {
      jto(size.x) {
        vec2 uv = vec2(float(j + 0.5f) / (size.x - 1u),
                       float(i + 0.5f) / (size.y - 1u));
        vec4 val_0 = load(uvec2(j * 2u, i * 2u), mip_level - 1u);
        vec4 val_1 = load(uvec2(j * 2u + 1, i * 2u), mip_level - 1u);
        vec4 val_2 = load(uvec2(j * 2u, i * 2u + 1), mip_level - 1u);
        vec4 val_3 = load(uvec2(j * 2u + 1, i * 2u + 1), mip_level - 1u);
        auto val = (val_0 + val_1 + val_2 + val_3) / 4.0f;
        write(val, uvec2(j, i), mip_level);
      }
    }
  }
  return out;
}

struct _Resource_View {
  u32 res_id = 0;
  u32 base_level = 0;
  u32 levels = 0;
  u32 base_layer = 0;
  u32 layers = 0;
  bool operator==(_Resource_View const &that) const {
    return memcmp(this, &that, sizeof(*this)) == 0;
  }
};

struct _Resource_View_Hash {
  u64 operator()(_Resource_View const &state) const {
    u64 out = 0ull;
    u8 *data = (u8 *)&state;
    ito(sizeof(_Resource_View)) { out = out ^ std::hash<u8>()(data[i]); }
    return out;
  }
};

struct Graphics_Utils_State {
  // #Definitions
  Device_Wrapper device_wrapper;
  boost::unordered_map<u32,
                       boost::unordered_map<_Resource_View, vk::UniqueImageView,
                                            _Resource_View_Hash>>
      image_view_table;
  Simple_Monitor simple_monitor;
  boost::unordered_map<Graphics_Pipeline_State, u32,
                       Graphics_Pipeline_State_Hash>
      gfx_pipelines;
  google::dense_hash_map<u32, u32> cs_pipelines;
  vk::UniqueSampler sampler;
  /////////////////////////////
  // Slot based resources
  Slot_Machine<Resource_Desc> resources;
  Slot_Machine<Pipeline_Wrapper> pipes;
  Slot_Machine<VmaImage> images;
  Slot_Machine<VmaBuffer> buffers;
  Slot_Machine<RT_Details> rts;
  Slot_Machine<Pass_Details> passes;

  // Single namespace for all gpu resources
  // Not the best way but whatever
  // Not every resource has a name
  // Also dummy targets have a name but no id
  google::dense_hash_map<std::string, u32> resource_name_table;
  google::dense_hash_map<u32, u32> resource_factory_table;
  google::dense_hash_map<std::string, u32> pass_name_table;
  google::dense_hash_map<std::string, u32> history_use;
  /////////////////////////////

  /////////////////////////////
  // Shader tables
  google::dense_hash_map<u32, std::string> shader_filenames;
  google::dense_hash_map<std::string, u32> shader_ids;
  /////////////////////////////

  // Descriptor allocation stuff
  std::vector<Descriptor_Frame> desc_frames;
  std::vector<std::pair<u32, std::function<void()>>> deferred_calls;

  /////////////////////////////
  // Immediate resource tracking
  // (name, slot) -> res_view
  boost::unordered_map<std::pair<std::string, u32>, _Resource_View>
      id_binding_table;
  Graphics_Pipeline_State cur_gfx_state;
  u32 cur_cs;
  google::dense_hash_map<u32, Image_Layout> cur_image_layouts;
  std::vector<Buffer_Info> vb_infos;
  u32 index_buffer;
  u32 index_offset;
  vk::Format index_format;
  u8 push_const[128];
  u32 push_const_size;

  u32 bound_pass;
  u32 bound_pipe;
  //////////////////////////////
  void reset_frame() {
    get_cur_descframe().end_frame();
    resources.tick();
    pipes.tick();
    rts.tick();
    passes.tick();
    images.tick();
    buffers.tick();

    std::vector<std::pair<u32, std::function<void()>>> new_deferred_list;
    for (auto &def_call : deferred_calls) {
      ASSERT_PANIC(def_call.first);
      def_call.first -= 1;
      if (def_call.first == 0u) {
        def_call.second();
      } else {
        new_deferred_list.push_back(def_call);
      }
    }
    deferred_calls = new_deferred_list;
  }
  void reset_pass() {
    bound_pass = 0;
    bound_pipe = 0;
    push_const_size = 0;
    index_offset = 0;
    index_buffer = 0;
    vb_infos.clear();
    cur_cs = 0;
    cur_gfx_state = Graphics_Pipeline_State{};
    id_binding_table.clear();
  }

  // #GetPipeline
  Pipeline_Wrapper &get_current_gfx_pipeline() {
    if (gfx_pipelines.find(cur_gfx_state) == gfx_pipelines.end()) {

      ASSERT_PANIC(cur_gfx_state.ps);
      ASSERT_PANIC(cur_gfx_state.vs);
      ASSERT_PANIC(cur_gfx_state.pass);
      auto &pass = passes[cur_gfx_state.pass];
      auto vs_filename = shader_filenames[cur_gfx_state.vs];
      auto ps_filename = shader_filenames[cur_gfx_state.ps];
      std::unordered_map<std::string, Vertex_Input> bindings;
      std::vector<std::pair<size_t, bool>> strides;
      if (g_binding_table.find(vs_filename) != g_binding_table.end()) {
        ASSERT_PANIC(g_binding_table.find(vs_filename) !=
                     g_binding_table.end());
        ASSERT_PANIC(g_binding_strides.find(vs_filename) !=
                     g_binding_strides.end());
        bindings = g_binding_table.find(vs_filename)->second;
        strides = g_binding_strides.find(vs_filename)->second;
      }
      std::vector<vk::VertexInputBindingDescription> descs;
      ito(strides.size()) {
        descs.push_back(vk::VertexInputBindingDescription()
                            .setStride(strides[i].first)
                            .setBinding(i)
                            .setInputRate(strides[i].second
                                              ? vk::VertexInputRate::eInstance
                                              : vk::VertexInputRate::eVertex));
      }
      // @TODO: Enable blending
      std::vector<vk::PipelineColorBlendAttachmentState> blend_atts;
      ito(pass.output.size()) {
        auto res_id = get_resource_id(pass.output[i]);
        auto &res = resources[res_id];
        if (res.type == Resource_Type::RT) {
          auto &rt = rts[res.ref];
          auto &img = images[rt.image_id];
          if (img.aspect == vk::ImageAspectFlagBits::eColor) {
            blend_atts.push_back(
                vk::PipelineColorBlendAttachmentState(false).setColorWriteMask(
                    vk::ColorComponentFlagBits::eR |
                    vk::ColorComponentFlagBits::eG |
                    vk::ColorComponentFlagBits::eB |
                    vk::ColorComponentFlagBits::eA));
          }
        }
      }
      u32 pipe_id = pipes.push(Pipeline_Wrapper::create_graphics(
          device_wrapper, "shaders/" + vs_filename, "shaders/" + ps_filename,
          vk::GraphicsPipelineCreateInfo()
              .setPInputAssemblyState(
                  &vk::PipelineInputAssemblyStateCreateInfo().setTopology(
                      cur_gfx_state.topology))
              .setPColorBlendState(&vk::PipelineColorBlendStateCreateInfo()
                                        .setAttachmentCount(blend_atts.size())
                                        .setLogicOpEnable(false)
                                        .setPAttachments(&blend_atts[0]))
              .setPDepthStencilState(
                  &vk::PipelineDepthStencilStateCreateInfo()
                       .setDepthTestEnable(cur_gfx_state.enable_depth_test)
                       .setDepthCompareOp(cur_gfx_state.cmp_op)
                       .setDepthWriteEnable(cur_gfx_state.enable_depth_write)
                       .setMaxDepthBounds(cur_gfx_state.max_depth)

                      )
              .setPRasterizationState(
                  &vk::PipelineRasterizationStateCreateInfo()
                       .setCullMode(cur_gfx_state.cull_mode)
                       .setFrontFace(cur_gfx_state.front_face)
                       .setPolygonMode(cur_gfx_state.polygon_mode)
                       .setLineWidth(cur_gfx_state.line_width)
                       .setDepthBiasEnable(cur_gfx_state.depth_bias_const !=
                                           0.0f)
                       .setDepthBiasConstantFactor(
                           cur_gfx_state.depth_bias_const))
              .setRenderPass(pass.pass.get()),
          bindings, descs, {}));
      // #Debug
      auto &pipe = pipes[pipe_id];
      auto pipe_name = vs_filename + "#" + ps_filename;
      device_wrapper.name_pipe(pipe.pipeline.get(), pipe_name.c_str());
      gfx_pipelines.insert({cur_gfx_state, pipe_id});
    }
    return pipes[gfx_pipelines[cur_gfx_state]];
  }
  Pipeline_Wrapper &get_current_compute_pipeline() {
    if (cs_pipelines.find(cur_cs) == cs_pipelines.end()) {
      ASSERT_PANIC(cur_cs);
      auto cs_filename = shader_filenames[cur_cs];

      u32 pipe_id = pipes.push(Pipeline_Wrapper::create_compute(
          device_wrapper, "shaders/" + cs_filename, {}));
      // #Debug
      auto &pipe = pipes[pipe_id];
      device_wrapper.name_pipe(pipe.pipeline.get(), cs_filename.c_str());
      cs_pipelines.insert({cur_cs, pipe_id});
    }
    return pipes[cs_pipelines[cur_cs]];
  }
  u32 get_resource_id(std::string const &name) {
    ASSERT_PANIC(resource_name_table.find(name) != resource_name_table.end());
    return resource_name_table.find(name)->second;
  }
  // #Constructor
  Graphics_Utils_State()
      : device_wrapper(init_device(true)), simple_monitor("shaders") {
    //    gfx_pipelines.set_empty_key(Graphics_Pipeline_State());
    //    gfx_pipelines.set_deleted_key(Graphics_Pipeline_State(u32(-1)));
    cs_pipelines.set_empty_key(0u);
    cs_pipelines.set_deleted_key(u32(-1));
    shader_filenames.set_empty_key(0u);
    shader_filenames.set_deleted_key(u32(-1));
    resource_factory_table.set_empty_key(0u);
    resource_factory_table.set_deleted_key(u32(-1));
    // @WTF
    shader_ids.set_empty_key("null");
    history_use.set_empty_key("null");
    shader_ids.set_deleted_key("deleted");
    history_use.set_deleted_key("deleted");
    resource_name_table.set_empty_key("null");
    resource_name_table.set_deleted_key("deleted");
    pass_name_table.set_empty_key("null");
    pass_name_table.set_deleted_key("deleted");
    //    named_binding_table.set_empty_key("null");
    //    id_binding_table.set_empty_key({"null", 0u});
    //    id_binding_table.set_deleted_key({"deleted", 0u});
    //
    cur_image_layouts.set_empty_key(0u);
    cur_image_layouts.set_deleted_key(u32(-1));
    sampler = device_wrapper.device->createSamplerUnique(
        vk::SamplerCreateInfo()
            .setMinFilter(vk::Filter::eLinear)
            .setMagFilter(vk::Filter::eLinear)
            .setAddressModeU(vk::SamplerAddressMode::eRepeat)
            .setAddressModeV(vk::SamplerAddressMode::eRepeat)
            .setMaxLod(100));
    desc_frames.resize(3);
    for (auto &frame : desc_frames)
      // @Cleanup?
      // It's safe cuz pImpl does not change memory location
      frame.device_wrapper = &device_wrapper;
    passes.deleter = [this](Pass_Details &pass) { pass.destroy(); };
    pipes.deleter = [](Pipeline_Wrapper &pipe) { pipe.destroy(); };
    images.deleter = [](VmaImage &img) { img.destroy(); };
    buffers.deleter = [](VmaBuffer &buf) { buf.destroy(); };
    rts.deleter = [this](RT_Details &rt) { images.remove(rt.image_id); };
    resources.deleter = [this](Resource_Desc &res) {
      if (res.type == Resource_Type::RT) {
        rts.remove(res.ref);
      } else if (res.type == Resource_Type::TEXTURE) {
        images.remove(res.ref);
      } else if (res.type == Resource_Type::BUFFER) {
        buffers.remove(res.ref);
      } else {
        ASSERT_PANIC(false);
      }
      auto res_id = res.id;
      // Invalidate views
      if (image_view_table.find(res_id) != image_view_table.end()) {
        auto &entry = image_view_table.find(res_id)->second;
        for (auto &view : entry) {
          view.second.reset(vk::ImageView());
        }
        entry.clear();
        image_view_table.erase(res_id);
      }
      res.type = Resource_Type::NONE;
      res.ref = 0;
    };
  }
  u32 create_texture2D(Image_Raw const &image_raw, bool build_mip = true) {
    u32 mip_levels = get_mip_levels(image_raw.width, image_raw.height);
    if (!build_mip)
      mip_levels = 1u;
    std::vector<uvec2> mip_sizes;
    std::vector<u32> mip_offsets;
    Alloc_State *alloc_state = device_wrapper.alloc_state.get();
    //    std::vector<u8> with_mips =
    //        build_mips(image_raw.data, image_raw.width, image_raw.height,
    //                   image_raw.format, mip_levels, mip_offsets, mip_sizes);
    ito(mip_levels)
        mip_sizes.push_back(uvec2(std::max(1u, image_raw.width >> i),
                                  std::max(1u, image_raw.height >> i)));
    u32 total_bytes = 0u;
    // @TODO: Add more formats
    // Bytes per pixel
    u32 bpc = 4u;
    u32 divisor = 4u;
    u32 components = 1u;
    switch (image_raw.format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
      bpc = 4u;
      divisor = 4u;
      components = 4u;
      break;
    case vk::Format::eR32G32B32Sfloat:
      bpc = 12u;
      divisor = 3u;
      components = 3u;
      break;
    case vk::Format::eR32G32B32A32Sfloat:
      bpc = 16u;
      divisor = 4u;
      components = 4u;
      break;
    case vk::Format::eR32Sfloat:
      bpc = 4u;
      divisor = 1u;
      components = 1u;
      break;
    default:
      ASSERT_PANIC(false && "unsupported format");
    }
    ito(mip_levels) {
      mip_offsets.push_back(total_bytes);
      total_bytes += mip_sizes[i].x * mip_sizes[i].y * bpc;
    }

    auto image_id = images.push(device_wrapper.alloc_state->allocate_image(
        vk::ImageCreateInfo()
            .setArrayLayers(1)
            .setExtent(vk::Extent3D(image_raw.width, image_raw.height, 1))
            .setFormat(image_raw.format)
            .setMipLevels(mip_levels)
            .setImageType(vk::ImageType::e2D)
            .setInitialLayout(vk::ImageLayout::eUndefined)
            .setPQueueFamilyIndices(&device_wrapper.graphics_queue_family_id)
            .setQueueFamilyIndexCount(1)
            .setSamples(vk::SampleCountFlagBits::e1)
            .setSharingMode(vk::SharingMode::eExclusive)
            .setTiling(vk::ImageTiling::eLinear)
            .setUsage(vk::ImageUsageFlagBits::eSampled |
                      vk::ImageUsageFlagBits::eTransferDst),
        VMA_MEMORY_USAGE_GPU_ONLY));
    auto &img = images[image_id];
    auto res_id = create_buffer(
        Buffer{.usage_bits = vk::BufferUsageFlagBits::eStorageBuffer |
                             vk::BufferUsageFlagBits::eTransferSrc,
               .size = total_bytes},
        nullptr);
    auto &buf = buffers[resources[res_id].ref];
    // Copy the top mip level
    {
      void *data = buf.map();
      memcpy(data, &image_raw.data[0], image_raw.data.size());
      buf.unmap();
    }
    {
      auto &cmd = device_wrapper.cur_cmd();
      auto &pass = passes[cur_gfx_state.pass];
      // @Cleanup
      _end_pass(cmd, pass);
      if (mip_levels > 1) {
        // @TODO: Restore state
        // For now let the texture creation be at the beginning of a pass
        ASSERT_PANIC(!bound_pipe);
        ASSERT_PANIC(!cur_cs && !push_const_size);
        bind_resource("Mip_Chain_U8", res_id, 0);
        CS_set_shader("mip_build.comp.glsl");
        bind_resource("Mip_Chain_F32", res_id, 0);
        ito(mip_levels - 1) {
          sh_mip_build_comp::pc pc{};
          pc.src_width = mip_sizes[i].x;
          pc.src_height = mip_sizes[i].y;
          pc.src_offset = mip_offsets[i] / bpc;
          pc.dst_width = mip_sizes[i + 1].x;
          pc.dst_height = mip_sizes[i + 1].y;
          pc.dst_offset = mip_offsets[i + 1] / bpc;
          pc.components = components;
          const uint R8G8B8A8_SRGB = 0;
          const uint R8G8B8A8_UNORM = 1;
          const uint R32G32B32_FLOAT = 2;
          const uint R32_FLOAT = 3;
          const uint R32G32B32A32_FLOAT = 4;
          switch (image_raw.format) {
          case vk::Format::eR8G8B8A8Unorm:
            pc.format = R8G8B8A8_UNORM;
            break;
          case vk::Format::eR8G8B8A8Srgb:
            pc.format = R8G8B8A8_SRGB;
            break;
          case vk::Format::eR32G32B32Sfloat:
            pc.format = R32G32B32_FLOAT;
            break;
          case vk::Format::eR32G32B32A32Sfloat:
            pc.format = R32G32B32A32_FLOAT;
            break;
          case vk::Format::eR32Sfloat:
            pc.format = R32_FLOAT;
            break;
          default:
            ASSERT_PANIC(false && "unsupported format");
          }
          buf.barrier(cmd, device_wrapper.graphics_queue_family_id,
                      vk::AccessFlagBits::eShaderWrite |
                          vk::AccessFlagBits::eShaderRead);
          push_constants(&pc, sizeof(pc));
          dispatch((mip_sizes[i + 1].x + 15) / 16,
                   (mip_sizes[i + 1].y + 15) / 16, 1);
        }
        cur_cs = 0u;
        bound_pipe = 0u;
      }

      buf.barrier(cmd, device_wrapper.graphics_queue_family_id,
                  vk::AccessFlagBits::eMemoryRead);
      img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                  vk::ImageLayout::eTransferDstOptimal,
                  vk::AccessFlagBits::eColorAttachmentWrite);

      ito(mip_levels) cmd.copyBufferToImage(
          buf.buffer, img.image, vk::ImageLayout::eTransferDstOptimal,
          vk::ArrayProxy<const vk::BufferImageCopy>{
              vk::BufferImageCopy()
                  .setBufferOffset(mip_offsets[i])
                  .setImageSubresource(vk::ImageSubresourceLayers(
                      vk::ImageAspectFlagBits::eColor, i, 0u, 1u))
                  .setImageOffset(vk::Offset3D(0u, 0u, 0u))
                  .setImageExtent(
                      vk::Extent3D(mip_sizes[i].x, mip_sizes[i].y, 1u))});

      //      cmd.copyBufferToImage(
      //          buf.buffer, img.image, vk::ImageLayout::eTransferDstOptimal,
      //          vk::ArrayProxy<const vk::BufferImageCopy>{
      //              vk::BufferImageCopy()
      //                  .setBufferOffset(0)
      //                  .setImageSubresource(vk::ImageSubresourceLayers(
      //                      vk::ImageAspectFlagBits::eColor, 0u, 0u, 1u))
      //                  .setImageOffset(vk::Offset3D(0u, 0u, 0u))
      //                  .setImageExtent(
      //                      vk::Extent3D(image_raw.width, image_raw.height,
      //                      1u))});
      img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                  vk::ImageLayout::eShaderReadOnlyOptimal,
                  vk::AccessFlagBits::eShaderRead);
      // @Cleanup
      _begin_pass(cmd, pass);
    }
    // Transient buffer so schedule the removal right away
    resources.remove(res_id);
    return resources.push({.type = Resource_Type::TEXTURE, .ref = image_id});
  }
  u32 create_uav_image(u32 width, u32 height, vk::Format format, u32 levels,
                       u32 layers) {
    ASSERT_PANIC(false);
  }
  u32 create_buffer(Buffer info, void const *initial_data) {
    ASSERT_PANIC(info.size);
    auto buf_id = buffers.push(device_wrapper.alloc_state->allocate_buffer(
        vk::BufferCreateInfo().setSize(info.size).setUsage(info.usage_bits),
        VMA_MEMORY_USAGE_CPU_TO_GPU));
    if (initial_data) {
      auto &buffer = buffers[buf_id];
      void *data = buffer.map();
      memcpy(data, initial_data, info.size);
      buffer.unmap();
    }
    auto res_id =
        resources.push({.type = Resource_Type::BUFFER, .ref = buf_id});
    return res_id;
  }
  u32 create_render_pass(std::string const &name,
                         std::vector<std::string> const &input,
                         std::vector<Resource> const &output, u32 width,
                         u32 height, std::function<void()> on_exec,
                         Pass_Type type = Pass_Type::Graphics) {
    ASSERT_PANIC(width && height);
    // @TODO: partial invalidation
    if (pass_name_table.find(name) != pass_name_table.end()) {
      auto pass_id = pass_name_table.find(name)->second;
      auto &pass = passes[pass_id];
      ASSERT_PANIC(pass.alive);
      bool invalidate = false;
      if (pass.width != width || pass.height != height)
        invalidate |= true;
      if (pass.output.size() != output.size() ||
          pass.input.size() != input.size())
        invalidate |= true;
      ito(output.size()) {
        if (output[i].type == Type::RT) {
          ASSERT_PANIC(output[i].type == Type::RT);
          auto &rt_info = output[i].rt_info;
          auto res_id = get_resource_id(pass.output[i]);
          auto &res = resources[res_id];
          // Type mismatch
          if (res.type != Resource_Type::RT) {
            invalidate |= true;
            break;
          }
          auto &rt = rts[res.ref];
          auto &img = images[rt.image_id];
          if (rt_info.format != img.create_info.format)
            invalidate |= true;
        } else if (output[i].type == Type::Image) {
          auto &image_info = output[i].image_info;
          auto res_id = get_resource_id(pass.output[i]);
          auto &res = resources[res_id];
          // Type mismatch
          if (res.type != Resource_Type::RT) {
            invalidate |= true;
            break;
          }
          auto &rt = rts[res.ref];
          auto &img = images[rt.image_id];
          if (img.create_info.format != image_info.format ||
              img.create_info.extent.width != image_info.width ||
              img.create_info.extent.height != image_info.height ||
              img.create_info.extent.depth != image_info.depth ||
              img.create_info.mipLevels != image_info.levels ||
              img.create_info.arrayLayers != image_info.layers)
            invalidate |= true;
        } else {
          ASSERT_PANIC(false);
        }
      }
      if (!invalidate) {
        return pass_id;
      } else {
        _invalidate_pass(pass_id);
      }
    }
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> refs;
    u32 pass_id = passes.push(Pass_Details());
    auto &pass_details = passes[pass_id];
    pass_details.alive = true;
    pass_details.name = name;
    pass_details.width = width;
    pass_details.height = height;
    pass_details.on_exec = on_exec;
    pass_details.type = type;
    ito(input.size()) {
      auto res_name = input[i];
      bool history = false;
      if (res_name[0] == '~') {
        res_name = res_name.substr(1);
        history = true;
      }
      pass_details.input.push_back({.name = res_name, .history = history});
    }
    i32 depth_attachment_id = -1;
    ito(output.size()) {
      // @TODO: invalidate resource if create info has changed
      if (resource_name_table.find(output[i].name) !=
          resource_name_table.end()) {
        // Shouldn't have happened in the current implementation
        ASSERT_PANIC(false);
      }
      if (output[i].type == Type::RT) {
        ASSERT_PANIC(type == Pass_Type::Graphics);
        RT rt_info = output[i].rt_info;
        RT_Details details;
        details.name = output[i].name;
        u32 image_id = 0;
        if (rt_info.target == Render_Target::Color) {
          image_id = images.push(device_wrapper.alloc_state->allocate_image(
              vk::ImageCreateInfo()
                  .setArrayLayers(1)
                  .setExtent(vk::Extent3D(width, height, 1))
                  .setFormat(rt_info.format)
                  .setMipLevels(1)
                  .setImageType(vk::ImageType::e2D)
                  .setInitialLayout(vk::ImageLayout::eUndefined)
                  .setPQueueFamilyIndices(
                      &device_wrapper.graphics_queue_family_id)
                  .setQueueFamilyIndexCount(1)
                  .setSamples(vk::SampleCountFlagBits::e1)
                  .setSharingMode(vk::SharingMode::eExclusive)
                  .setTiling(vk::ImageTiling::eOptimal)
                  .setUsage(vk::ImageUsageFlagBits::eColorAttachment |
                            vk::ImageUsageFlagBits::eTransferDst |
                            vk::ImageUsageFlagBits::eTransferSrc |
                            vk::ImageUsageFlagBits::eSampled),
              VMA_MEMORY_USAGE_GPU_ONLY));
        } else {
          image_id = images.push(device_wrapper.alloc_state->allocate_image(
              vk::ImageCreateInfo()
                  .setArrayLayers(1)
                  .setExtent(vk::Extent3D(width, height, 1))
                  .setFormat(rt_info.format)
                  .setMipLevels(1)
                  .setImageType(vk::ImageType::e2D)
                  .setInitialLayout(vk::ImageLayout::eUndefined)
                  .setPQueueFamilyIndices(
                      &device_wrapper.graphics_queue_family_id)
                  .setQueueFamilyIndexCount(1)
                  .setSamples(vk::SampleCountFlagBits::e1)
                  .setSharingMode(vk::SharingMode::eExclusive)
                  .setTiling(vk::ImageTiling::eOptimal)
                  .setUsage(vk::ImageUsageFlagBits::eDepthStencilAttachment |
                            vk::ImageUsageFlagBits::eTransferDst |
                            vk::ImageUsageFlagBits::eTransferSrc |
                            vk::ImageUsageFlagBits::eSampled),
              VMA_MEMORY_USAGE_GPU_ONLY, vk::ImageAspectFlagBits::eDepth));
        }
        details.image_id = image_id;
        auto rts_id = rts.push(std::move(details));
        auto res_id =
            resources.push({.type = Resource_Type::RT, .ref = rts_id});
        resource_name_table.insert({output[i].name, res_id});
        // Insert factory reference
        resource_factory_table.insert({res_id, pass_id});
        pass_details.output.push_back(output[i].name);
        {
          // #Debug
          auto &img = images[image_id];
          device_wrapper.name_image(img.image, output[i].name.c_str());
        }
        VkAttachmentDescription attachment = {};
        if (rt_info.target == Render_Target::Color) {
          attachment.format = VkFormat(rt_info.format);
          attachment.samples = VK_SAMPLE_COUNT_1_BIT;
          attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
          attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
          attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        } else {
          attachment.format = VkFormat(rt_info.format);
          attachment.samples = VK_SAMPLE_COUNT_1_BIT;
          attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
          attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
          attachment.initialLayout =
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
          attachment.finalLayout =
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
          pass_details.use_depth = true;
          pass_details.depth_target = image_id;
          depth_attachment_id = i;
        }
        attachments.push_back(attachment);
        // @TODO: Reorder
        if (rt_info.target == Render_Target::Color) {
          VkAttachmentReference color_attachment = {};
          color_attachment.attachment = i;
          color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          refs.push_back(color_attachment);
        }
      } else if (output[i].type == Type::Image) {
        auto info = output[i].image_info;
        auto type = vk::ImageType::e1D;
        if (info.height != 1)
          type = vk::ImageType::e2D;
        if (info.depth != 1)
          type = vk::ImageType::e3D;
        auto image_id = images.push(device_wrapper.alloc_state->allocate_image(
            vk::ImageCreateInfo()
                .setArrayLayers(info.layers)
                .setExtent(vk::Extent3D(info.width, info.height, info.depth))
                .setFormat(info.format)
                .setMipLevels(info.levels)
                .setImageType(type)
                .setInitialLayout(vk::ImageLayout::eUndefined)
                .setPQueueFamilyIndices(
                    &device_wrapper.graphics_queue_family_id)
                .setQueueFamilyIndexCount(1)
                .setSamples(vk::SampleCountFlagBits::e1)
                .setSharingMode(vk::SharingMode::eExclusive)
                .setTiling(vk::ImageTiling::eOptimal)
                .setUsage(vk::ImageUsageFlagBits::eStorage |
                          vk::ImageUsageFlagBits::eTransferDst |
                          vk::ImageUsageFlagBits::eTransferSrc |
                          vk::ImageUsageFlagBits::eSampled),
            VMA_MEMORY_USAGE_GPU_ONLY, vk::ImageAspectFlagBits::eColor));
        auto rt_id =
            rts.push(RT_Details{.name = output[i].name, .image_id = image_id});
        auto res_id = resources.push({.type = Resource_Type::RT, .ref = rt_id});
        resource_name_table.insert({output[i].name, res_id});
        // Insert factory reference
        resource_factory_table.insert({res_id, pass_id});
        pass_details.output.push_back(output[i].name);
        {
          // #Debug
          auto &img = images[image_id];
          device_wrapper.name_image(img.image, output[i].name.c_str());
        }

      }
      // @TODO Named buffers
      else {
        // Stub
        ASSERT_PANIC(false);
      }
    }
    if (type == Pass_Type::Graphics) {
      VkAttachmentReference depth_attachment = {};
      // Using simple subpass without tile based crap
      VkSubpassDescription subpass = {};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = refs.size();
      subpass.pColorAttachments = &refs[0];
      // @TODO: Reorder
      if (pass_details.use_depth) {
        depth_attachment.attachment = depth_attachment_id;
        depth_attachment.layout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        subpass.pDepthStencilAttachment = &depth_attachment;
      }
      VkSubpassDependency dependency = {};
      dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
      dependency.dstSubpass = 0;
      dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependency.srcAccessMask = 0;
      dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      VkRenderPassCreateInfo info = {};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info.attachmentCount = attachments.size();
      info.pAttachments = &attachments[0];
      info.subpassCount = 1;
      info.pSubpasses = &subpass;
      info.dependencyCount = 1;
      info.pDependencies = &dependency;
      pass_details.pass = device_wrapper.device->createRenderPassUnique(
          vk::RenderPassCreateInfo(info));
      std::vector<vk::ImageView> views;
      for (auto res_name : pass_details.output) {
        auto res_id = get_resource_id(res_name);
        auto &res = resources[res_id];
        if (res.type == Resource_Type::RT) {
          auto &rt = rts[res.ref];
          auto &img = images[rt.image_id];
          views.push_back(_get_view(_Resource_View{.res_id = res_id}));
        }
      }
      pass_details.fb = device_wrapper.device->createFramebufferUnique(
          vk::FramebufferCreateInfo()
              .setAttachmentCount(views.size())
              .setHeight(height)
              .setWidth(width)
              .setLayers(1)
              .setPAttachments(&views[0])
              .setRenderPass(pass_details.pass.get()));
    }
    pass_name_table.insert({name, pass_id});
    return pass_id;
  }
  void release_resource(u32 id) {
    //    auto &res = resources[id];
    resource_factory_table.erase(id);
    resources.remove(id);
  }

  void IA_set_topology(vk::PrimitiveTopology topology) {
    cur_gfx_state.topology = topology;
  }

  void _invalidate() {
    passes.for_each([this](Pass_Details &pass) { _invalidate_pass(pass.id); });
  }

  void _invalidate_pipes() {
    // Invalidate descriptor sets because they depend on pipelines
    for (auto &frame : desc_frames)
      frame.reset();
    gfx_pipelines.clear();
    cs_pipelines.clear();

    pipes.for_each([this](Pipeline_Wrapper &pipe) { pipes.remove(pipe.id); });
  }

  void _invalidate_pass(u32 pass_id) {
    // Total invalidation
    // @TODO: Track dependencies?
    auto &pass = passes[pass_id];
    // Remove pipelines as they may depend on the pass
    for (auto &pipe : gfx_pipelines) {
      pipes.remove(pipe.second);
    }
    // Invalidate descriptor sets because they depend on pipelines
    for (auto &frame : desc_frames)
      frame.reset();

    // Invalidate tables
    for (auto res_name : pass.output) {
      auto res_id = get_resource_id(res_name);
      auto &res = resources[res_id];
      resource_factory_table.erase(res_id);
      resource_name_table.erase(res_name);
      resources.remove(res_id);
      if (history_use.find(res_name) != history_use.end()) {
        resources.remove(history_use.find(res_name)->second);
        history_use.erase(res_name);
      }
    }
    gfx_pipelines.clear();
    pass_name_table.erase(pass.name);
    // Remove pass
    passes.remove(pass_id);
  }

  void
  IA_set_layout(std::unordered_map<std::string, Vertex_Input> const &layout) {
    ASSERT_PANIC(false);
  }
  void IA_set_index_buffer(u32 id, u32 offset, vk::IndexType format) {
    auto &cmd = device_wrapper.cur_cmd();
    auto &res = resources[id];
    if (res.type == Resource_Type::BUFFER) {
      auto &buf = buffers[res.ref];
      cmd.bindIndexBuffer(buf.buffer, offset, format);
    } else {
      ASSERT_PANIC(false);
    }
  }
  void IA_set_vertex_buffers(std::vector<Buffer_Info> const &infos,
                             u32 offset) {
    auto &cmd = device_wrapper.cur_cmd();
    std::vector<vk::Buffer> arg_buffers;
    std::vector<vk::DeviceSize> offsets;
    ito(infos.size()) {
      auto &res = resources[infos[i].buf_id];
      if (res.type == Resource_Type::BUFFER) {
        auto &buf = buffers[res.ref];
        arg_buffers.push_back(buf.buffer);
        offsets.push_back(infos[i].offset);
      } else {
        ASSERT_PANIC(false);
      }
    }
    cmd.bindVertexBuffers(offset, arg_buffers, offsets);
  }
  void IA_set_cull_mode(vk::CullModeFlags cull_mode, vk::FrontFace front_face,
                        vk::PolygonMode polygon_mode, float line_width) {
    cur_gfx_state.cull_mode = cull_mode;
    cur_gfx_state.front_face = front_face;
    cur_gfx_state.polygon_mode = polygon_mode;
    cur_gfx_state.line_width = line_width;
  }
  u32 _set_or_create_shader(std::string const &filename) {
    u32 id = 0;
    if (shader_ids.find(filename) == shader_ids.end()) {
      id = shader_ids.size() + 1;
      shader_ids.insert({filename, id});
      shader_filenames.insert({id, filename});
    }
    return shader_ids.find(filename)->second;
  }
  void VS_set_shader(std::string const &filename) {

    cur_gfx_state.vs = _set_or_create_shader(filename);
  }
  void PS_set_shader(std::string const &filename) {
    cur_gfx_state.ps = _set_or_create_shader(filename);
  }
  void CS_set_shader(std::string const &filename) {
    cur_cs = _set_or_create_shader(filename);
  }
  void RS_set_depth_stencil_state(bool enable_depth_test, vk::CompareOp cmp_op,
                                  bool enable_depth_write, float max_depth,
                                  float depth_bias) {
    cur_gfx_state.enable_depth_test = enable_depth_test;
    cur_gfx_state.cmp_op = cmp_op;
    cur_gfx_state.enable_depth_write = enable_depth_write;
    cur_gfx_state.max_depth = max_depth;
    cur_gfx_state.depth_bias_const = depth_bias;
  }
  vk::ImageView _get_view(_Resource_View _view) {
    auto &res = resources[_view.res_id];
    u32 img_id = 0;
    if (res.type == Resource_Type::RT) {
      auto &rt = rts[res.ref];
      img_id = rt.image_id;
    } else if (res.type == Resource_Type::TEXTURE) {
      img_id = res.ref;
    } else {
      ASSERT_PANIC(false);
    }
    // @Warning: Creates an entry
    auto &entry = image_view_table[_view.res_id];
    if (entry.find(_view) == entry.end()) {
      auto &img = images[img_id];
      // Fix the default image view
      auto c_view = _view;
      if (c_view.layers == 0) {
        ASSERT_PANIC(c_view.base_layer < img.create_info.arrayLayers);
        c_view.layers = img.create_info.arrayLayers - c_view.base_layer;
      }
      if (c_view.levels == 0) {
        ASSERT_PANIC(c_view.base_level < img.create_info.mipLevels);
        c_view.levels = img.create_info.mipLevels - c_view.base_level;
      }
      entry.insert({_view, img.create_view(device_wrapper.device.get(),
                                           c_view.base_level, c_view.levels,
                                           c_view.base_layer, c_view.layers)});
    }
    return entry.find(_view)->second.get();
  }
  void bind_image(std::string const &name, u32 res_id, u32 index,
                  Image_View view) {
    // @TODO: Allow modifications after descriptors set binding
    ASSERT_PANIC(!bound_pipe);
    _Resource_View _view;
    _view.res_id = res_id;
    _view.layers = view.layers;
    _view.levels = view.levels;
    _view.base_layer = view.base_layer;
    _view.base_level = view.base_level;
    id_binding_table[{name, index}] = _view;
  }
  void bind_image(std::string const &name, std::string const &res_name,
                  u32 index, Image_View view) {
    ASSERT_PANIC(res_name[0] != '~');
    u32 res_id = get_resource_id(res_name);
    bind_image(name, res_id, index, view);
  }
  void bind_resource(std::string const &name, u32 id, u32 index) {
    // @TODO: Allow modifications after descriptors set binding
    ASSERT_PANIC(!bound_pipe);
    id_binding_table[{name, index}] = _Resource_View{.res_id = id};
  }
  u32 init_history(std::string const &res_name) {
    std::string id = "~" + res_name;
    auto res_id = resource_name_table.find(res_name)->second;
    auto &res = resources[res_id];
    if (res.type == Resource_Type::RT) {
      auto &rt = rts[res.ref];
      auto &img = images[rt.image_id];
      u32 new_image_id = images.push(device_wrapper.alloc_state->allocate_image(
          img.create_info, VMA_MEMORY_USAGE_GPU_ONLY));
      // @Cleanup: Clear image upon creation

      auto &cmd = device_wrapper.cur_cmd();
      bool resume_pass = false;
      if (bound_pass) {
        auto &pass = passes[cur_gfx_state.pass];
        _end_pass(cmd, pass);
        resume_pass = true;
      }
      auto &new_img = images[new_image_id];
      new_img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                      vk::ImageLayout::eTransferDstOptimal,
                      vk::AccessFlagBits::eColorAttachmentWrite);
      cmd.clearColorImage(
          new_img.image, vk::ImageLayout::eTransferDstOptimal,
          vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}),
          {vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0u, 1u,
                                     0u, 1u)});
      if (resume_pass) {
        auto &pass = passes[cur_gfx_state.pass];
        _begin_pass(cmd, pass);
      }

      //
      RT_Details new_rt{};
      new_rt.name = id;
      new_rt.image_id = new_image_id;
      u32 new_rt_id = rts.push(std::move(new_rt));
      auto new_res_id =
          resources.push({.type = Resource_Type::RT, .ref = new_rt_id});
      history_use.insert({res_name, new_res_id});
    } else {
      ASSERT_PANIC(false);
    }
  }
  void bind_resource(std::string const &name, std::string const &id,
                     u32 index) {
    std::string res_name = id;
    if (res_name[0] == '~') {
      res_name = res_name.substr(1);
      if (history_use.find(res_name) == history_use.end()) {
        init_history(res_name);
      }

      // Images only for now
      bind_resource(name, history_use.find(res_name)->second, index);
    } else {
      bind_resource(name, get_resource_id(id), index);
    }
  }

  void *map_buffer(u32 id) {
    auto &res = resources[id];
    if (res.type == Resource_Type::BUFFER) {
      auto &buf = buffers[res.ref];
      return buf.map();
    } else {
      ASSERT_PANIC(false);
    }
  }
  void unmap_buffer(u32 id) {
    auto &res = resources[id];
    if (res.type == Resource_Type::BUFFER) {
      auto &buf = buffers[res.ref];
      buf.unmap();
      return;
    } else {
      ASSERT_PANIC(false);
    }
  }
  std::vector<std::string> get_img_list() {
    std::vector<std::string> out;
    for (auto &item : resource_name_table) {
      auto &res = resources[item.second];
      if (res.type == Resource_Type::RT || res.type == Resource_Type::TEXTURE)
        out.push_back(item.first);
    }
    return out;
  }
  void push_constants(void *data, size_t size) {
    ASSERT_PANIC(size <= 128);
    memcpy(push_const, data, size);
    push_const_size = size;
  }
  void clear_color(vec4 value) {
    ASSERT_PANIC(cur_gfx_state.pass);
    auto &pass = passes[cur_gfx_state.pass];
    auto &cmd = device_wrapper.cur_cmd();
    // @Cleanup
    _end_pass(cmd, pass);
    for (auto res_name : pass.output) {
      auto res_id = get_resource_id(res_name);
      auto &res = resources[res_id];
      if (res.type == Resource_Type::RT) {

        auto &rt = rts[res.ref];
        auto &img = images[rt.image_id];
        if (img.aspect == vk::ImageAspectFlagBits::eColor) {

          img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                      vk::ImageLayout::eTransferDstOptimal,
                      vk::AccessFlagBits::eColorAttachmentWrite);
          cmd.clearColorImage(
              img.image, vk::ImageLayout::eTransferDstOptimal,
              vk::ClearColorValue(
                  std::array<float, 4>{value.x, value.y, value.z, value.w}),
              {vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0u,
                                         1u, 0u, 1u)});
        } else if (img.aspect == vk::ImageAspectFlagBits::eDepth) {
        } else {
          // Stub
          ASSERT_PANIC(false);
        }
      }
    }
    // @Cleanup
    _begin_pass(cmd, pass);
  }
  void clear_depth(float value) {
    ASSERT_PANIC(cur_gfx_state.pass);
    auto &pass = passes[cur_gfx_state.pass];

    auto &cmd = device_wrapper.cur_cmd();
    // @Cleanup
    _end_pass(cmd, pass);
    for (auto res_name : pass.output) {
      auto res_id = get_resource_id(res_name);
      auto &res = resources[res_id];
      if (res.type == Resource_Type::RT) {
        auto &rt = rts[res.ref];
        auto &img = images[rt.image_id];
        if (img.aspect == vk::ImageAspectFlagBits::eColor) {

        } else if (img.aspect == vk::ImageAspectFlagBits::eDepth) {
          img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                      vk::ImageLayout::eTransferDstOptimal,
                      vk::AccessFlagBits::eDepthStencilAttachmentWrite);
          cmd.clearDepthStencilImage(
              img.image, vk::ImageLayout::eTransferDstOptimal,
              vk::ClearDepthStencilValue(value),
              {vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, 0u,
                                         1u, 0u, 1u)});
        } else {
          // Stub
          ASSERT_PANIC(false);
        }
      }
    }

    // @Cleanup
    _begin_pass(cmd, pass);
  }
  Descriptor_Frame &get_cur_descframe() {
    return desc_frames[device_wrapper.get_frame_id()];
  }
  void draw(u32 indices, u32 instances, u32 first_index, u32 first_instance,
            i32 vertex_offset) {
    _setup_bindings();
    auto &cmd = device_wrapper.cur_cmd();
    cmd.drawIndexed(indices, instances, first_index, vertex_offset,
                    first_instance);
  }
  void draw(u32 vertices, u32 instances, u32 first_vertex, u32 first_instance) {
    _setup_bindings();
    auto &cmd = device_wrapper.cur_cmd();
    cmd.draw(vertices, instances, first_vertex, first_instance);
  }
  void _setup_bindings(bool draw = true) {
    auto &cmd = device_wrapper.cur_cmd();
    auto &pass = passes[cur_gfx_state.pass];
    auto &pipeline =
        draw ? get_current_gfx_pipeline() : get_current_compute_pipeline();
    if (push_const_size) {
      cmd.pushConstants(pipeline.pipeline_layout.get(),
                        vk::ShaderStageFlagBits::eAll, 0, push_const_size,
                        push_const);
      push_const_size = 0;
    }
    if (bound_pipe == pipeline.id)
      return;
    auto &dframe = get_cur_descframe();
    // @Cleanup
    _end_pass(cmd, pass);
    // Detect simultaneous read-write bindings
    boost::unordered_set<u32> storage_images;
    for (auto &item : id_binding_table) {
      if (!pipeline.has_descriptor(item.first.first))
        continue;
      auto type = pipeline.get_type(item.first.first);
      if (type == vk::DescriptorType::eStorageImage) {
        storage_images.insert(item.second.res_id);
      } else if (type == vk::DescriptorType::eCombinedImageSampler) {

      } else if (type == vk::DescriptorType::eUniformBuffer) {

      } else if (type == vk::DescriptorType::eStorageBuffer) {
      } else {
        ASSERT_PANIC(false);
      }
    }
    for (auto &item : id_binding_table) {
      if (!pipeline.has_descriptor(item.first.first))
        continue;
      auto &_view = item.second;
      // @TODO: Check for valid ids
      auto &res = resources[_view.res_id];
      u32 img_id = 0;
      u32 buf_id = 0;
      if (res.type == Resource_Type::RT) {
        auto &rt = rts[res.ref];
        img_id = rt.image_id;
      } else if (res.type == Resource_Type::TEXTURE) {
        img_id = res.ref;
      } else if (res.type == Resource_Type::BUFFER) {
        buf_id = res.ref;
      } else {
        // @TODO
        ASSERT_PANIC(false);
      }

      auto type = pipeline.get_type(item.first.first);
      if (type == vk::DescriptorType::eStorageImage) {
        ASSERT_PANIC(img_id);
        auto &img = images[img_id];
        // @Cleanup
        img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eShaderRead |
                        vk::AccessFlagBits::eShaderWrite);
        dframe.update_storage_image_descriptor(
            pipeline, item.first.first, _get_view(_view), item.first.second);
      } else if (type == vk::DescriptorType::eCombinedImageSampler) {
        auto &img = images[img_id];
        // @Cleanup
        if (storage_images.find(_view.res_id) == storage_images.end()) {
          img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                      vk::ImageLayout::eShaderReadOnlyOptimal,
                      vk::AccessFlagBits::eShaderRead);
        } else {
          img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                      vk::ImageLayout::eGeneral,
                      vk::AccessFlagBits::eShaderRead |
                          vk::AccessFlagBits::eShaderWrite,
                      true);
        }
        dframe.update_sampled_image_descriptor(pipeline, item.first.first,
                                               _get_view(_view), sampler.get(),
                                               item.first.second, img.layout);
      } else if (type == vk::DescriptorType::eUniformBuffer) {
        auto &buf = buffers[buf_id];
        dframe.update_descriptor(pipeline, item.first.first, buf.buffer, 0,
                                 buf.create_info.size, type, item.first.second);
      } else if (type == vk::DescriptorType::eStorageBuffer) {
        auto &buf = buffers[buf_id];
        dframe.update_descriptor(pipeline, item.first.first, buf.buffer, 0,
                                 buf.create_info.size, type, item.first.second);
      } else {
        ASSERT_PANIC(false);
      }
    }

    dframe.bind_pipeline(cmd, pipeline);
    bound_pipe = pipeline.id;
    // @Cleanup
    _begin_pass(cmd, pass, true);
  }
  void dispatch(u32 dim_x, u32 dim_y, u32 dim_z) {
    _setup_bindings(false);
    auto &cmd = device_wrapper.cur_cmd();
    cmd.dispatch(dim_x, dim_y, dim_z);
  }

  void set_on_gui(std::function<void()> fn) {
    device_wrapper.on_gui = [=]() { fn(); };
  }
  void _begin_pass(vk::CommandBuffer &cmd, Pass_Details &pass,
                   bool force = false) {
    if (!bound_pass && !force)
      return;
    if (bound_pass == pass.id)
      return;
    ASSERT_PANIC(pass.alive);
    if (pass.type != Pass_Type::Graphics)
      return;
    if (pass.use_depth) {
      ASSERT_PANIC(pass.depth_target);
      auto &depth = images[pass.depth_target];
    }
    for (auto res_name : pass.output) {
      auto res_id = get_resource_id(res_name);
      auto &res = resources[res_id];
      if (res.type == Resource_Type::RT) {
        auto &rt = rts[res.ref];
        auto &img = images[rt.image_id];
        if (img.aspect == vk::ImageAspectFlagBits::eColor) {

          img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                      vk::ImageLayout::eColorAttachmentOptimal,
                      vk::AccessFlagBits::eColorAttachmentWrite);
        } else if (img.aspect == vk::ImageAspectFlagBits::eDepth) {
          img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                      vk::ImageLayout::eDepthStencilAttachmentOptimal,
                      vk::AccessFlagBits::eDepthStencilAttachmentWrite);
        } else {
          // Stub
          ASSERT_PANIC(false);
        }
      }
    }

    cmd.beginRenderPass(vk::RenderPassBeginInfo()
                            .setFramebuffer(pass.fb.get())
                            .setRenderPass(pass.pass.get())
                            .setRenderArea(vk::Rect2D(
                                {
                                    0,
                                    0,
                                },
                                {pass.width, pass.height})),
                        vk::SubpassContents::eInline);
    cmd.setViewport(0,
                    {vk::Viewport(0, 0, pass.width, pass.height, 0.0f, 1.0f)});

    cmd.setScissor(0, {{{0, 0}, {pass.width, pass.height}}});
    bound_pass = pass.id;
  }
  void _end_pass(vk::CommandBuffer &cmd, Pass_Details &pass,
                 bool force = false) {
    if (!bound_pass && !force)
      return;
    if (pass.type != Pass_Type::Graphics)
      return;
    cmd.endRenderPass();
    bound_pass = 0;
  }

  void run_loop(std::function<void()> fn) {
    device_wrapper.pre_tick = [=](vk::CommandBuffer &cmd) {
      reset_frame();
      if (simple_monitor.is_updated())
        _invalidate_pipes();

      fn();

      boost::unordered_set<u32> history_needed;
      passes.for_each([&](Pass_Details &pass) {
        auto pass_id = pass.get_id();
        for (auto &input : pass.input) {
          if (input.history) {
            ASSERT_PANIC(resource_name_table.find(input.name) !=
                         resource_name_table.end());
            history_needed.insert(resource_name_table.find(input.name)->second);
          }
        }
      });

      // Poor man's dependency graph
      // pass_id -> list of pass_ids on which this pass depends
      boost::unordered_map<u32, boost::unordered_set<u32>> dep_graph;
      boost::unordered_map<u32, boost::unordered_set<u32>> inv_dep_graph;
      std::deque<u32> passes_queue;
      passes.for_each([&](Pass_Details &pass) {
        auto pass_id = pass.get_id();
        boost::unordered_set<u32> deps;
        for (auto &input : pass.input) {
          // Dependency on the  previous frame is automatic
          if (input.history)
            continue;
          auto res_id = get_resource_id(input.name);
          ASSERT_PANIC(resource_factory_table.find(res_id) !=
                       resource_factory_table.end());
          auto dep_id = resource_factory_table.find(res_id)->second;
          deps.insert(dep_id);
          inv_dep_graph[dep_id].insert(pass_id);
        }
        if (deps.size())
          dep_graph.insert({pass_id, deps});
        passes_queue.push_back(pass_id);
      });
      if (false) {
        std::ofstream out("pass_dep.gv");
        out << "digraph dep_graph {\n";
        for (auto id0 : dep_graph) {
          auto pass_0_name = passes[id0.first].name;
          for (auto id1 : id0.second) {
            auto pass_1_name = passes[id1].name;
            out << pass_0_name << " -> " << pass_1_name << ";\n";
          }
        }
        out << "}";
        out << "digraph inv_dep_graph {\n";
        for (auto id0 : inv_dep_graph) {
          auto pass_0_name = passes[id0.first].name;
          for (auto id1 : id0.second) {
            auto pass_1_name = passes[id1].name;
            out << pass_0_name << " -> " << pass_1_name << ";\n";
          }
        }
        out << "}";
      }
      while (passes_queue.size()) {
        u32 begin = passes_queue.front();
        passes_queue.pop_front();
        if (dep_graph.count(begin) == 0) {
          auto &pass = passes[begin];
          reset_pass();
          cur_gfx_state.pass = begin;
          // #Debug
          device_wrapper.marker_begin(pass.name.c_str());
          _begin_pass(cmd, pass);
          pass.on_exec();
          _end_pass(cmd, pass);
          // #Debug
          device_wrapper.marker_end();
          // Notify all dependent passes that this pass has finished
          if (inv_dep_graph.find(begin) != inv_dep_graph.end()) {
            for (auto &id : inv_dep_graph[begin]) {
              dep_graph[id].erase(begin);
              if (dep_graph[id].size() == 0u)
                dep_graph.erase(id);
            }
          }
        } else {
          // There must be some pass to resolve the dependency
          ASSERT_PANIC(passes_queue.size());
          passes_queue.push_back(begin);
        }
      }
      reset_pass();
      // #Debug
      device_wrapper.marker_begin("copy_to_history");
      // Copy to history
      for (u32 res_id : history_needed) {
        _copy_to_history(res_id);
      }
      // #Debug
      device_wrapper.marker_end();
    };
    device_wrapper.window_loop();
  }
  void _copy_to_history(u32 res_id) {
    // Outside of renderpass
    ASSERT_PANIC(bound_pass == 0);
    auto &res = resources[res_id];
    if (res.type == Resource_Type::RT) {
      u32 image_id = res.ref;
      auto &rt = rts[res.ref];
      image_id = rt.image_id;
      auto &img = images[image_id];
      if (history_use.find(rt.name) == history_use.end()) {
        init_history(rt.name);
      }
      auto &res_1 = resources[history_use.find(rt.name)->second];
      auto &rt_1 = rts[res_1.ref];
      auto &img_1 = images[rt_1.image_id];
      auto &cmd = device_wrapper.cur_cmd();
      img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                  vk::ImageLayout::eTransferSrcOptimal,
                  vk::AccessFlagBits::eMemoryRead);
      img_1.barrier(cmd, device_wrapper.graphics_queue_family_id,
                    vk::ImageLayout::eTransferDstOptimal,
                    vk::AccessFlagBits::eMemoryWrite);
      cmd.copyImage(
          img.image, img.layout, img_1.image, img_1.layout, 1,
          &vk::ImageCopy()
               .setExtent(img.create_info.extent)
               .setSrcSubresource(
                   vk::ImageSubresourceLayers().setLayerCount(1).setAspectMask(
                       img.aspect))
               .setDstSubresource(
                   vk::ImageSubresourceLayers().setLayerCount(1).setAspectMask(
                       img.aspect)));
    } else {
      ASSERT_PANIC(false);
    }
  }
  u32 create_compute_pass(std::string const &name,
                          std::vector<std::string> const &input,
                          std::vector<Resource> const &output,
                          std::function<void()> on_exec) {
    return create_render_pass(name, input, output, 1, 1, on_exec,
                              Pass_Type::Compute);
  }
  void ImGui_Image(std::string const &name, u32 width, u32 height) {

    auto &cmd = device_wrapper.cur_cmd();
    ASSERT_PANIC(resource_name_table.find(name) != resource_name_table.end());
    auto res_id = resource_name_table[name];
    auto &res = resources[res_id];
    vk::ImageView view;
    if (res.type == Resource_Type::RT) {
      auto &rt = rts[res.ref];
      auto &img = images[rt.image_id];
      img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                  vk::ImageLayout::eShaderReadOnlyOptimal,
                  vk::AccessFlagBits::eShaderRead);
      view = _get_view(_Resource_View{.res_id = res_id});
    } else if (res.type == Resource_Type::TEXTURE) {
      auto &img = images[res.ref];
      img.barrier(cmd, device_wrapper.graphics_queue_family_id,
                  vk::ImageLayout::eShaderReadOnlyOptimal,
                  vk::AccessFlagBits::eShaderRead);
      view = _get_view(_Resource_View{.res_id = res_id});
    } else {
      ASSERT_PANIC(false);
    }
    auto desc = get_cur_descframe().allocate_imgui(
        name, sampler.get(), view, vk::ImageLayout::eShaderReadOnlyOptimal);
    ImGui::Image((ImTextureID)desc, ImVec2(width, height), ImVec2(0.0f, 1.0f),
                 ImVec2(1.0f, 0.0f));
  }
  void ImGui_Emit_Stats() {
    passes.ImGui_Emit_Stats("Passes");
    rts.ImGui_Emit_Stats("RTS");
    images.ImGui_Emit_Stats("Images");
    buffers.ImGui_Emit_Stats("Buffers");
  }
};

Graphics_Utils Graphics_Utils::create() {
  Graphics_Utils out{};
  out.pImpl = new Graphics_Utils_State();
  return out;
}
Graphics_Utils::~Graphics_Utils() {
  delete ((Graphics_Utils_State *)this->pImpl);
}

u32 Graphics_Utils::create_texture2D(Image_Raw const &image_raw,
                                     bool build_mip) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->create_texture2D(image_raw, build_mip);
}
u32 Graphics_Utils::create_uav_image(u32 width, u32 height, vk::Format format,
                                     u32 levels, u32 layers) {

  return ((Graphics_Utils_State *)this->pImpl)
      ->create_uav_image(width, height, format, levels, layers);
}
u32 Graphics_Utils::create_buffer(Buffer info, void const *initial_data) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->create_buffer(info, initial_data);
}

u32 Graphics_Utils::create_render_pass(std::string const &name,
                                       std::vector<std::string> const &input,
                                       std::vector<Resource> const &output,
                                       u32 width, u32 height,
                                       std::function<void()> on_exec) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->create_render_pass(name, input, output, width, height, on_exec);
}
u32 Graphics_Utils::create_compute_pass(std::string const &name,
                                        std::vector<std::string> const &input,
                                        std::vector<Resource> const &output,
                                        std::function<void()> on_exec) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->create_compute_pass(name, input, output, on_exec);
}
void Graphics_Utils::release_resource(u32 id) {
  return ((Graphics_Utils_State *)this->pImpl)->release_resource(id);
}
// void Graphics_Utils::IA_set_layout(
//    std::unordered_map<std::string, Vertex_Input> const &layout) {
//  return ((Graphics_Utils_State *)this->pImpl)->IA_set_layout(layout);
//}
void Graphics_Utils::IA_set_topology(vk::PrimitiveTopology topology) {
  return ((Graphics_Utils_State *)this->pImpl)->IA_set_topology(topology);
}
void Graphics_Utils::IA_set_index_buffer(u32 id, u32 offset,
                                         vk::IndexType format) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->IA_set_index_buffer(id, offset, format);
}
void Graphics_Utils::IA_set_vertex_buffers(
    std::vector<Buffer_Info> const &infos, u32 offset) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->IA_set_vertex_buffers(infos, offset);
}
void Graphics_Utils::IA_set_cull_mode(vk::CullModeFlags cull_mode,
                                      vk::FrontFace front_face,
                                      vk::PolygonMode polygon_mode,
                                      float line_width) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->IA_set_cull_mode(cull_mode, front_face, polygon_mode, line_width);
}
void Graphics_Utils::VS_set_shader(std::string const &filename) {
  return ((Graphics_Utils_State *)this->pImpl)->VS_set_shader(filename);
}
void Graphics_Utils::PS_set_shader(std::string const &filename) {
  return ((Graphics_Utils_State *)this->pImpl)->PS_set_shader(filename);
}
void Graphics_Utils::CS_set_shader(std::string const &filename) {
  return ((Graphics_Utils_State *)this->pImpl)->CS_set_shader(filename);
}
void Graphics_Utils::RS_set_depth_stencil_state(bool enable_depth_test,
                                                vk::CompareOp cmp_op,
                                                bool enable_depth_write,
                                                float max_depth,
                                                float depth_bias) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->RS_set_depth_stencil_state(enable_depth_test, cmp_op,
                                   enable_depth_write, max_depth, depth_bias);
}

void Graphics_Utils::bind_resource(std::string const &name, u32 id, u32 index) {
  return ((Graphics_Utils_State *)this->pImpl)->bind_resource(name, id, index);
}
void Graphics_Utils::bind_resource(std::string const &name,
                                   std::string const &id, u32 index) {
  return ((Graphics_Utils_State *)this->pImpl)->bind_resource(name, id, index);
}
void Graphics_Utils::bind_image(std::string const &name,
                                std::string const &res_name, u32 index,
                                Image_View view) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->bind_image(name, res_name, index, view);
}
void *Graphics_Utils::map_buffer(u32 id) {
  return ((Graphics_Utils_State *)this->pImpl)->map_buffer(id);
}
void Graphics_Utils::unmap_buffer(u32 id) {
  return ((Graphics_Utils_State *)this->pImpl)->unmap_buffer(id);
}
void Graphics_Utils::push_constants(void *data, size_t size) {
  return ((Graphics_Utils_State *)this->pImpl)->push_constants(data, size);
}
std::vector<std::string> Graphics_Utils::get_img_list() {
  return ((Graphics_Utils_State *)this->pImpl)->get_img_list();
}
void Graphics_Utils::clear_color(vec4 value) {
  return ((Graphics_Utils_State *)this->pImpl)->clear_color(value);
}
void Graphics_Utils::clear_depth(float value) {
  return ((Graphics_Utils_State *)this->pImpl)->clear_depth(value);
}
void Graphics_Utils::draw(u32 indices, u32 instances, u32 first_index,
                          u32 first_instance, i32 vertex_offset) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->draw(indices, instances, first_index, first_instance, vertex_offset);
}
void Graphics_Utils::draw(u32 vertices, u32 instances, u32 first_vertex,
                          u32 first_instance) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->draw(vertices, instances, first_vertex, first_instance);
}
void Graphics_Utils::dispatch(u32 dim_x, u32 dim_y, u32 dim_z) {
  return ((Graphics_Utils_State *)this->pImpl)->dispatch(dim_x, dim_y, dim_z);
}

void Graphics_Utils::set_on_gui(std::function<void()> fn) {
  return ((Graphics_Utils_State *)this->pImpl)->set_on_gui(fn);
}
void Graphics_Utils::run_loop(std::function<void()> fn) {
  return ((Graphics_Utils_State *)this->pImpl)->run_loop(fn);
}

void Graphics_Utils::ImGui_Image(std::string const &name, u32 width,
                                 u32 height) {
  return ((Graphics_Utils_State *)this->pImpl)
      ->ImGui_Image(name, width, height);
}

void Graphics_Utils::ImGui_Emit_Stats() {
  return ((Graphics_Utils_State *)this->pImpl)->ImGui_Emit_Stats();
}
