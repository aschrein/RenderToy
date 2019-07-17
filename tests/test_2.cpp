#include "../include/device.hpp"
#include "../include/error_handling.hpp"
#include "../include/gizmo.hpp"
#include "../include/memory.hpp"
#include "../include/particle_sim.hpp"
#include "../include/primitives.hpp"
#include "../include/shader_compiler.hpp"

#include "../include/random.hpp"
#include "imgui.h"

#include "examples/imgui_impl_vulkan.h"

#include "dir_monitor/include/dir_monitor/dir_monitor.hpp"
#include "gtest/gtest.h"
#include <boost/thread.hpp>
#include <chrono>
#include <cstring>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/ext.hpp>
#include <glm/glm.hpp>
using namespace glm;

template <int N> struct Time_Stack { f32 vals[N]; };
template <int N> struct Stack_Plot {
  std::string name;
  u32 max_values;
  std::vector<std::string> plot_names;

  std::unordered_map<std::string, u32> legend;
  Time_Stack<N> tmp_value;
  std::vector<Time_Stack<N>> values;
  void set_value(std::string const &name, f32 val) {
    if (legend.size() == 0) {
      u32 id = 0;
      for (auto const &name : plot_names) {
        legend[name] = id++;
      }
    }
    ASSERT_PANIC(legend.find(name) != legend.end());
    u32 id = legend[name];
    tmp_value.vals[id] = val;
  }
  void push_value() {
    if (values.size() == max_values) {
      for (int i = 0; i < max_values - 1; i++) {
        values[i] = values[i + 1];
      }
      values[max_values - 1] = tmp_value;
    } else {
      values.push_back(tmp_value);
    }
    tmp_value = {};
  }
};

struct CPU_timestamp {
  std::chrono::high_resolution_clock::time_point frame_begin_timestamp;
  CPU_timestamp() {
    frame_begin_timestamp = std::chrono::high_resolution_clock::now();
  }
  f32 end() {
    auto frame_end_timestamp = std::chrono::high_resolution_clock::now();
    auto frame_cpu_delta_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            frame_end_timestamp - frame_begin_timestamp)
            .count();
    return f32(frame_cpu_delta_ns) / 1000;
  }
};

struct Plot_Internal {
  std::string name;
  u32 max_values;
  std::vector<f32> values;
  std::chrono::high_resolution_clock::time_point frame_begin_timestamp;
  void cpu_timestamp_begin() {
    frame_begin_timestamp = std::chrono::high_resolution_clock::now();
  }
  void cpu_timestamp_end() {
    auto frame_end_timestamp = std::chrono::high_resolution_clock::now();
    auto frame_cpu_delta_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            frame_end_timestamp - frame_begin_timestamp)
            .count();
    push_value(f32(frame_cpu_delta_ns) / 1000);
  }
  void push_value(f32 value) {
    if (values.size() == max_values) {
      for (int i = 0; i < max_values - 1; i++) {
        values[i] = values[i + 1];
      }
      values[max_values - 1] = value;
    } else {
      values.push_back(value);
    }
  }
  void draw() {
    if (values.size() == 0)
      return;
    ImGui::PlotLines(name.c_str(), &values[0], values.size(), 0, NULL, FLT_MAX,
                     FLT_MAX, ImVec2(0, 100));
    ImGui::SameLine();
    ImGui::Text("%-3.1fuS", values[values.size() - 1]);
  }
};

struct Timestamp_Plot_Wrapper {
  std::string name;
  // 2 slots are needed
  u32 query_begin_id;
  u32 max_values;
  //
  bool timestamp_requested = false;
  Plot_Internal plot;
  void query_begin(vk::CommandBuffer &cmd, Device_Wrapper &device_wrapper) {
    cmd.resetQueryPool(device_wrapper.timestamp.pool.get(), query_begin_id, 2);
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eAllCommands,
                       device_wrapper.timestamp.pool.get(), query_begin_id);
  }
  void query_end(vk::CommandBuffer &cmd, Device_Wrapper &device_wrapper) {
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eAllCommands,
                       device_wrapper.timestamp.pool.get(), query_begin_id + 1);
    timestamp_requested = true;
  }
  void push_value(Device_Wrapper &device_wrapper) {
    if (timestamp_requested) {
      u64 query_results[] = {0, 0};
      device_wrapper.device->getQueryPoolResults(
          device_wrapper.timestamp.pool.get(), query_begin_id, 2,
          2 * sizeof(u64), (void *)query_results, sizeof(u64),
          vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
      u64 begin_ns = device_wrapper.timestamp.convert_to_ns(query_results[0]);
      u64 end_ns = device_wrapper.timestamp.convert_to_ns(query_results[1]);
      u64 diff_ns = end_ns - begin_ns;
      f32 us = f32(diff_ns) / 1000;
      timestamp_requested = false;
      plot.push_value(us);
    }
  }
  void draw() {
    plot.name = this->name;
    plot.max_values = this->max_values;
    plot.draw();
  }
};

boost::asio::io_service io_service;

std::atomic<bool> shaders_updated = false;
void dir_event_handler(boost::asio::dir_monitor &dm,
                       const boost::system::error_code &ec,
                       const boost::asio::dir_monitor_event &ev) {
  if (ev.type == boost::asio::dir_monitor_event::event_type::modified)
    shaders_updated = true;
  dm.async_monitor([&](const boost::system::error_code &ec,
                       const boost::asio::dir_monitor_event &ev) {
    dir_event_handler(dm, ec, ev);
  });
}

TEST(graphics, vulkan_graphics_test_1) {
  auto device_wrapper = init_device(true);
  auto &device = device_wrapper.device;

  boost::asio::dir_monitor dm(io_service);
  dm.add_directory("../shaders");
  dm.async_monitor([&](const boost::system::error_code &ec,
                       const boost::asio::dir_monitor_event &ev) {
    dir_event_handler(dm, ec, ev);
  });

  boost::asio::io_service::work workload(io_service);
  boost::thread dm_thread = boost::thread(
      boost::bind(&boost::asio::io_service::run, boost::ref(io_service)));

  // Some shader data structures
  struct Particle_Vertex {
    vec3 position;
  };
  struct Compute_UBO {
    vec3 camera_pos;
    int pad_0;
    vec3 camera_look;
    int pad_1;
    vec3 camera_up;
    int pad_2;
    vec3 camera_right;
    float camera_fov;
    float ug_size;
    uint ug_bins_count;
    float ug_bin_size;
    uint rendering_flags;
    uint raymarch_iterations;
    float hull_radius;
    float step_radius;
  };
  struct Particle_UBO {
    mat4 world;
    mat4 view;
    mat4 proj;
  };
  // Viewport for this sample's rendering
  vk::Rect2D example_viewport({0, 0}, {32, 32});
  ///////////////////////////
  // Particle system state //
  ///////////////////////////
  Random_Factory frand;
  Simulation_State particle_system;

  // Initialize the system
  particle_system.restore_or_default("simulation_state_dump");

  // Rendering state
  // @TODO: Proper serialization with protocol buffers or smth
  Stack_Plot<3> cpu_frametime_stack{
    name : "CPU frame time",
    max_values : 256,
    plot_names : {"grid baking", "simulation", "full frame"}
  };
  CPU_Image cpu_time =
      CPU_Image::create(device_wrapper, 256, 128, vk::Format::eR8G8B8A8Unorm);
  Timestamp_Plot_Wrapper raymarch_timestamp_graph{
    name : "raymarch time",
    query_begin_id : 0,
    max_values : 100
  };
  Timestamp_Plot_Wrapper fullframe_gpu_graph{
    name : "full frame GPU time",
    query_begin_id : 2,
    max_values : 100
  };
  bool raymarch_flag_render_hull = true;
  bool raymarch_flag_render_cells = true;
  u32 GRID_DIM = 32;
  uint raymarch_iterations = 32;
  f32 rendering_radius = 0.1f;
  f32 rendering_step = 0.2f;
  f32 debug_grid_flood_radius = 0.325f;
  f32 rendering_grid_size =
      particle_system.system_size + debug_grid_flood_radius;
  Framebuffer_Wrapper framebuffer_wrapper{};
  Storage_Image_Wrapper storage_image_wrapper{};
  Pipeline_Wrapper fullscreen_pipeline;
  Pipeline_Wrapper particles_pipeline;
  Pipeline_Wrapper links_pipeline;
  Pipeline_Wrapper compute_pipeline_wrapped;
  auto recreate_resources = [&] {
    // Raymarching kernel
    compute_pipeline_wrapped = Pipeline_Wrapper::create_compute(
        device_wrapper, "../shaders/raymarch.comp.1.glsl",
        {{"GROUP_DIM", "16"}});
    framebuffer_wrapper = Framebuffer_Wrapper::create(
        device_wrapper, example_viewport.extent.width,
        example_viewport.extent.height, vk::Format::eR32G32B32A32Sfloat);
    storage_image_wrapper = Storage_Image_Wrapper::create(
        device_wrapper, example_viewport.extent.width,
        example_viewport.extent.height, vk::Format::eR32G32B32A32Sfloat);
    // @TODO: Squash all this pipeline creation boilerplate
    // Fullscreen pass
    fullscreen_pipeline = Pipeline_Wrapper::create_graphics(
        device_wrapper, "../shaders/tests/bufferless_triangle.vert.glsl",
        "../shaders/tests/simple_1.frag.glsl",
        vk::GraphicsPipelineCreateInfo().setRenderPass(
            framebuffer_wrapper.render_pass.get()),
        {}, {}, {});
  };
  Alloc_State *alloc_state = device_wrapper.alloc_state.get();

  // Shared sampler
  vk::UniqueSampler sampler =
      device->createSamplerUnique(vk::SamplerCreateInfo().setMaxLod(1));
  // Init device stuff
  {
    vk::UniqueFence transfer_fence =
        device->createFenceUnique(vk::FenceCreateInfo());
    auto &cmd = device_wrapper.graphics_cmds[0].get();
    cmd.reset(vk::CommandBufferResetFlagBits::eReleaseResources);
    cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlags()));
    cpu_time.transition_layout_to_general(device_wrapper, cmd);
    cmd.end();
    device_wrapper.graphics_queue.submit(
        vk::SubmitInfo(
            0, nullptr,
            &vk::PipelineStageFlags(vk::PipelineStageFlagBits::eAllCommands), 1,
            &cmd),
        transfer_fence.get());
    while (vk::Result::eTimeout ==
           device->waitForFences(transfer_fence.get(), VK_TRUE, 0xffffffffu))
      ;
  }
  //
  //////////////////
  // Camera state //
  //////////////////
  float camera_phi = 0.0;
  float camera_theta = M_PI / 2.0f;
  float camera_distance = 10.0f;

  //////////////////////
  // Render offscreen //
  //////////////////////
  VmaBuffer compute_ubo_buffer;
  VmaBuffer bins_buffer;
  VmaBuffer particles_buffer;

  device_wrapper.pre_tick = [&](vk::CommandBuffer &cmd) {
    CPU_timestamp __full_frame;
    fullframe_gpu_graph.push_value(device_wrapper);
    fullframe_gpu_graph.query_begin(cmd, device_wrapper);
    // Update backbuffer if the viewport size has changed
    bool expected = true;
    if (shaders_updated.compare_exchange_weak(expected, false) ||
        framebuffer_wrapper.width != example_viewport.extent.width ||
        framebuffer_wrapper.height != example_viewport.extent.height) {
      recreate_resources();
    }

    ////////////// SIMULATION //////////////////
    // Perform fixed step iteration on the particle system
    // Fill the uniform grid

    Packed_UG packed;
    {
      CPU_timestamp __timestamp;
      UG ug(rendering_grid_size, GRID_DIM);

      for (u32 i = 0; i < particle_system.particles.size(); i++) {
        ug.put(particle_system.particles[i],
               debug_grid_flood_radius, // rendering_radius + rendering_step
                                        // * 4.0f,
               i);
      }
      packed = ug.pack();
      cpu_frametime_stack.set_value("grid baking", __timestamp.end());
    }
    ///////////// RENDERING ////////////////////

    // Create new GPU visible buffers
    // @TODO: Track usage of the old buffers
    // Right now there is no overlapping of cpu and gpu work
    // With overlapping this will invalidate used buffers
    compute_ubo_buffer = alloc_state->allocate_buffer(
        vk::BufferCreateInfo()
            .setSize(sizeof(Compute_UBO))
            .setUsage(vk::BufferUsageFlagBits::eUniformBuffer |
                      vk::BufferUsageFlagBits::eTransferDst),
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    bins_buffer = alloc_state->allocate_buffer(
        vk::BufferCreateInfo()
            .setSize(sizeof(u32) * packed.arena_table.size())
            .setUsage(vk::BufferUsageFlagBits::eStorageBuffer |
                      vk::BufferUsageFlagBits::eTransferDst),
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    particles_buffer = alloc_state->allocate_buffer(
        vk::BufferCreateInfo()
            .setSize(sizeof(f32) * 3 * packed.ids.size())
            .setUsage(vk::BufferUsageFlagBits::eStorageBuffer |
                      vk::BufferUsageFlagBits::eTransferDst),
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Update gpu visible buffers
    {
      vec3 camera_pos =
          vec3(sinf(camera_theta) * cosf(camera_phi),
               sinf(camera_theta) * sinf(camera_phi), cos(camera_theta)) *
          camera_distance;

      {
        void *data = bins_buffer.map();
        u32 *typed_data = (u32 *)data;
        for (u32 i = 0; i < packed.arena_table.size(); i++) {
          typed_data[i] = packed.arena_table[i];
        }
        bins_buffer.unmap();
      }
      {
        void *data = particles_buffer.map();
        vec3 *typed_data = (vec3 *)data;
        std::vector<vec3> particles_packed;
        for (u32 pid : packed.ids) {
          particles_packed.push_back(particle_system.particles[pid]);
        }
        memcpy(typed_data, &particles_packed[0],
               particles_packed.size() * sizeof(vec3));
        particles_buffer.unmap();
      }
      {
        void *data = compute_ubo_buffer.map();
        Compute_UBO *typed_data = (Compute_UBO *)data;
        Compute_UBO tmp_ubo;
        tmp_ubo.camera_fov = float(example_viewport.extent.width) /
                             example_viewport.extent.height;

        tmp_ubo.camera_pos = camera_pos;
        tmp_ubo.camera_look = normalize(-camera_pos);
        tmp_ubo.camera_right =
            normalize(cross(tmp_ubo.camera_look, vec3(0.0f, 0.0f, 1.0f)));
        tmp_ubo.camera_up =
            normalize(cross(tmp_ubo.camera_right, tmp_ubo.camera_look));
        tmp_ubo.ug_size = rendering_grid_size;
        tmp_ubo.ug_bins_count = GRID_DIM;
        tmp_ubo.ug_bin_size = 2.0f * rendering_grid_size / GRID_DIM;
        tmp_ubo.rendering_flags = 0;
        tmp_ubo.rendering_flags |= (raymarch_flag_render_hull ? 1 : 0);
        tmp_ubo.rendering_flags |= (raymarch_flag_render_cells ? 1 : 0) << 1;
        tmp_ubo.hull_radius = rendering_radius;
        tmp_ubo.step_radius = rendering_step;
        tmp_ubo.raymarch_iterations = raymarch_iterations;
        *typed_data = tmp_ubo;
        compute_ubo_buffer.unmap();
      }
    }
    // Update descriptor tables
    compute_pipeline_wrapped.update_descriptor(
        device.get(), "Bins", bins_buffer.buffer, 0,
        sizeof(uint) * packed.arena_table.size(),
        vk::DescriptorType::eStorageBuffer);
    compute_pipeline_wrapped.update_descriptor(
        device.get(), "Particles", particles_buffer.buffer, 0,
        sizeof(float) * 3 * packed.ids.size());
    compute_pipeline_wrapped.update_descriptor(
        device.get(), "UBO", compute_ubo_buffer.buffer, 0, sizeof(Compute_UBO),
        vk::DescriptorType::eUniformBuffer);

    compute_pipeline_wrapped.update_storage_image_descriptor(
        device.get(), "resultImage", storage_image_wrapper.image_view.get());

    /*------------------------------*/
    /* Spawn the raymarching kernel */
    /*------------------------------*/

    raymarch_timestamp_graph.push_value(device_wrapper);
    storage_image_wrapper.transition_layout_to_write(device_wrapper, cmd);
    compute_pipeline_wrapped.bind_pipeline(device.get(), cmd);
    raymarch_timestamp_graph.query_begin(cmd, device_wrapper);
    cmd.dispatch((example_viewport.extent.width + 15) / 16,
                 (example_viewport.extent.height + 15) / 16, 1);
    raymarch_timestamp_graph.query_end(cmd, device_wrapper);
    storage_image_wrapper.transition_layout_to_read(device_wrapper, cmd);

    /*----------------------------------*/
    /* Update the offscreen framebuffer */
    /*----------------------------------*/
    framebuffer_wrapper.transition_layout_to_write(device_wrapper, cmd);
    framebuffer_wrapper.begin_render_pass(cmd);
    cmd.setViewport(0,
                    {vk::Viewport(0, 0, example_viewport.extent.width,
                                  example_viewport.extent.height, 0.0f, 1.0f)});
    cmd.setScissor(
        0, {{{0, 0},
             {example_viewport.extent.width, example_viewport.extent.height}}});

    fullscreen_pipeline.bind_pipeline(device.get(), cmd);
    fullscreen_pipeline.update_sampled_image_descriptor(
        device.get(), "tex", storage_image_wrapper.image_view.get(),
        sampler.get());

    cmd.draw(3, 1, 0, 0);

    framebuffer_wrapper.end_render_pass(cmd);
    framebuffer_wrapper.transition_layout_to_read(device_wrapper, cmd);
    fullframe_gpu_graph.query_end(cmd, device_wrapper);
    cpu_frametime_stack.set_value("full frame", __full_frame.end());
    cpu_frametime_stack.push_value();
  };

  /////////////////////
  // Render the image
  /////////////////////
  device_wrapper.on_tick = [&](vk::CommandBuffer &cmd) {

  };

  /////////////////////
  // Render the GUI
  /////////////////////
  device_wrapper.on_gui = [&] {
    static bool show_demo = true;
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |=
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    window_flags |= ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(-1.0f);
    ImGui::Begin("DockSpace Demo", nullptr, window_flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleVar(2);
    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    ImGui::SetNextWindowBgAlpha(-1.0f);
    ImGui::Begin("dummy window");

    /*---------------------------------------*/
    /* Update the viewport for the rendering */
    /*---------------------------------------*/
    auto wpos = ImGui::GetWindowPos();
    example_viewport.offset.x = wpos.x;
    example_viewport.offset.y = wpos.y;
    auto wsize = ImGui::GetWindowSize();
    example_viewport.extent.width = wsize.x;
    float height_diff = 40;
    if (wsize.y < height_diff) {
      example_viewport.extent.height = 1;

    } else {
      example_viewport.extent.height = wsize.y - height_diff;
    }
    /*-------------------*/
    /* Update the camera */
    /*-------------------*/
    if (ImGui::IsWindowHovered()) {
      static ImVec2 old_mpos{};
      auto eps = 1.0e-4f;
      auto mpos = ImGui::GetMousePos();
      if (ImGui::GetIO().MouseDown[0]) {
        if (mpos.x != old_mpos.x || mpos.y != old_mpos.y) {
          auto dx = mpos.x - old_mpos.x;
          auto dy = mpos.y - old_mpos.y;

          camera_phi -= dx * 1.0e-2f;
          camera_theta -= dy * 1.0e-2f;
          if (camera_phi > M_PI * 2.0f) {
            camera_phi -= M_PI * 2.0f;
          } else if (camera_phi < 0.0f) {
            camera_phi += M_PI * 2.0;
          }
          if (camera_theta > M_PI - eps) {
            camera_theta = M_PI - eps;
          } else if (camera_theta < eps) {
            camera_theta = eps;
          }
        }
      }
      old_mpos = mpos;
      auto scroll_y = ImGui::GetIO().MouseWheel;
      camera_distance += camera_distance * 1.e-1 * scroll_y;
      camera_distance = clamp(camera_distance, eps, 100.0f);
    }
    // ImGui::Button("Press me");

    ImGui::Image(
        ImGui_ImplVulkan_AddTexture(
            sampler.get(), framebuffer_wrapper.image_view.get(),
            VkImageLayout::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        ImVec2(example_viewport.extent.width, example_viewport.extent.height),
        ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

    ImGui::End();
    ImGui::ShowDemoWindow(&show_demo);
    ImGui::Begin("Simulation parameters");

    ImGui::End();
    ImGui::Begin("Rendering configuration");

    ImGui::Checkbox("raymarch render hull", &raymarch_flag_render_hull);
    ImGui::Checkbox("raymarch render iterations", &raymarch_flag_render_cells);
    ImGui::DragFloat("raymarch hull radius", &rendering_radius, 0.025f, 0.025f,
                     10.0f);
    ImGui::DragFloat("raymarch step radius", &rendering_step, 0.025f, 0.025f,
                     10.0f);
    ImGui::DragFloat("[Debug] grid flood radius", &debug_grid_flood_radius,
                     0.025f, 0.0f, 10.0f);
    ImGui::DragFloat("[Debug] rendering grid size", &rendering_grid_size,
                     0.025f, 0.025f, 10.0f);
    u32 step = 1;
    ImGui::InputScalar("raymarch grid dimension", ImGuiDataType_U32, &GRID_DIM,
                       &step);
    ImGui::SliderInt("raymarch max iterations", (i32 *)&raymarch_iterations, 1,
                     64);
    ImGui::End();
    ImGui::Begin("Metrics");
    {
      u32 colors[] = {
          0x6a4740ff, 0xe6fdabff, 0xb7be0bff, 0x8fe5beff,
          0x03bcd8ff, 0xed3e0eff, 0xa90b0cff,
      };
      auto bswap = [](u32 val) {
        return ((val >> 24) & 0xff) | ((val << 8) & 0xff0000) |
               ((val >> 8) & 0xff00) | ((val << 24) & 0xff000000);
      };
      f32 max = 0.0f;
      for (auto const &item : cpu_frametime_stack.values) {
        for (u32 i = 0; i < __ARRAY_SIZE(item.vals); i++) {
          max = std::max(max, item.vals[i]);
        }
      }
      void *data = cpu_time.image.map();
      u32 *typed_data = (u32 *)data;
      // typed_data[0] = 0xffu;
      for (u32 x = 0; x < cpu_time.width; x++) {
        for (u32 y = 0; y < cpu_time.height; y++) {
          typed_data[x + y * cpu_time.width] = bswap(0x000000ffu);
        }
      }
      for (u32 x = 0; x < cpu_time.width; x++) {
        if (x == cpu_frametime_stack.values.size())
          break;
        auto item = cpu_frametime_stack.values[x];
        for (u32 i = 1; i < __ARRAY_SIZE(item.vals) - 1; i++) {
          item.vals[i] += item.vals[i - 1];
        }
        for (auto &val : item.vals) {
          val *= f32(cpu_time.height) / max;
        }
        for (u32 y = 0; y < cpu_time.height; y++) {

          for (u32 i = 0; i < __ARRAY_SIZE(item.vals); i++) {
            if (y <= u32(item.vals[i])) {
              typed_data[x + y * cpu_time.width] = bswap(colors[i]);
              break;
            }
          }
        }
      }
      cpu_time.image.unmap();
    }
    if (cpu_frametime_stack.values.size()) {
      ImGui::Image(
          ImGui_ImplVulkan_AddTexture(sampler.get(), cpu_time.image_view.get(),
                                      VkImageLayout::VK_IMAGE_LAYOUT_GENERAL),
          ImVec2(cpu_time.width, cpu_time.height), ImVec2(0.0f, 1.0f),
          ImVec2(1.0f, 0.0f));
      ImGui::SameLine();
      ImGui::Text(
          "%s:%-3.1fuS", cpu_frametime_stack.name.c_str(),
          cpu_frametime_stack.values[cpu_frametime_stack.values.size() - 1]
              .vals[__ARRAY_SIZE(cpu_frametime_stack.values[0].vals) - 1]);
    }
    raymarch_timestamp_graph.draw();
    fullframe_gpu_graph.draw();
    // fullframe_cpu_graph.draw();
    // ug_cpu_graph.draw();
    // sim_cpu_graph.draw();
    ImGui::End();
  };
  device_wrapper.window_loop();
  particle_system.dump("simulation_state_dump");
}

TEST(graphics, vulkan_graphics_test_gizmo) {
  auto device_wrapper = init_device(true);
  auto &device = device_wrapper.device;

  // boost::asio::dir_monitor dm(io_service);
  // dm.add_directory("../shaders");
  // dm.async_monitor([&](const boost::system::error_code &ec,
  //                      const boost::asio::dir_monitor_event &ev) {
  //   dir_event_handler(dm, ec, ev);
  // });

  // boost::asio::io_service::work workload(io_service);
  // boost::thread dm_thread = boost::thread(
  //     boost::bind(&boost::asio::io_service::run, boost::ref(io_service)));

  // Some shader data structures
  Gizmo_Layer gizmo_layer{};

  ///////////////////////////
  // Particle system state //
  ///////////////////////////
  Framebuffer_Wrapper framebuffer_wrapper{};
  Pipeline_Wrapper fullscreen_pipeline;

  auto recreate_resources = [&] {
    framebuffer_wrapper = Framebuffer_Wrapper::create(
        device_wrapper, gizmo_layer.example_viewport.extent.width,
        gizmo_layer.example_viewport.extent.height,
        vk::Format::eR32G32B32A32Sfloat);

    fullscreen_pipeline = Pipeline_Wrapper::create_graphics(
        device_wrapper, "../shaders/tests/bufferless_triangle.vert.glsl",
        "../shaders/tests/simple_1.frag.glsl",
        vk::GraphicsPipelineCreateInfo()
            .setPInputAssemblyState(
                &vk::PipelineInputAssemblyStateCreateInfo().setTopology(
                    vk::PrimitiveTopology::eTriangleList))
            .setRenderPass(framebuffer_wrapper.render_pass.get()),
        {}, {}, {});
    gizmo_layer.init_vulkan_state(device_wrapper,
                                  framebuffer_wrapper.render_pass.get());
  };
  Alloc_State *alloc_state = device_wrapper.alloc_state.get();

  // Shared sampler
  vk::UniqueSampler sampler =
      device->createSamplerUnique(vk::SamplerCreateInfo().setMaxLod(1));
  // Init device stuff
  {

    auto &cmd = device_wrapper.graphics_cmds[0].get();
    cmd.reset(vk::CommandBufferResetFlagBits::eReleaseResources);
    cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlags()));
    cmd.end();
    device_wrapper.sumbit_and_flush(cmd);
  }

  device_wrapper.pre_tick = [&](vk::CommandBuffer &cmd) {
    // Update backbuffer if the viewport size has changed
    bool expected = true;
    if (shaders_updated.compare_exchange_weak(expected, false) ||
        framebuffer_wrapper.width !=
            gizmo_layer.example_viewport.extent.width ||
        framebuffer_wrapper.height !=
            gizmo_layer.example_viewport.extent.height) {
      recreate_resources();
    }

    ///////////// RENDERING ////////////////////

    /*----------------------------------*/
    /* Update the offscreen framebuffer */
    /*----------------------------------*/
    framebuffer_wrapper.transition_layout_to_write(device_wrapper, cmd);
    framebuffer_wrapper.begin_render_pass(cmd);
    cmd.setViewport(
        0,
        {vk::Viewport(0, 0, gizmo_layer.example_viewport.extent.width,
                      gizmo_layer.example_viewport.extent.height, 0.0f, 1.0f)});

    cmd.setScissor(0, {{{0, 0},
                        {gizmo_layer.example_viewport.extent.width,
                         gizmo_layer.example_viewport.extent.height}}});
    gizmo_layer.draw(device_wrapper, cmd);
    fullscreen_pipeline.bind_pipeline(device.get(), cmd);

    framebuffer_wrapper.end_render_pass(cmd);
    framebuffer_wrapper.transition_layout_to_read(device_wrapper, cmd);
  };

  /////////////////////
  // Render the image
  /////////////////////
  device_wrapper.on_tick = [&](vk::CommandBuffer &cmd) {

  };

  /////////////////////
  // Render the GUI
  /////////////////////
  device_wrapper.on_gui = [&] {
    static bool show_demo = true;
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |=
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    window_flags |= ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(-1.0f);
    ImGui::Begin("DockSpace Demo", nullptr, window_flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleVar(2);
    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    ImGui::SetNextWindowBgAlpha(-1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("dummy window");
    ImGui::PopStyleVar(3);
    gizmo_layer.on_imgui_viewport();
    // ImGui::Button("Press me");

    ImGui::Image(ImGui_ImplVulkan_AddTexture(
                     sampler.get(), framebuffer_wrapper.image_view.get(),
                     VkImageLayout::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                 ImVec2(gizmo_layer.example_viewport.extent.width,
                        gizmo_layer.example_viewport.extent.height),
                 ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

    ImGui::End();
    ImGui::ShowDemoWindow(&show_demo);
    ImGui::Begin("Simulation parameters");

    ImGui::End();
    ImGui::Begin("Rendering configuration");

    ImGui::End();
    ImGui::Begin("Metrics");
    // ImGui::InputFloat("mx", &mx, 0.1f, 0.1f, 2);
    // ImGui::InputFloat("my", &my, 0.1f, 0.1f, 2);
    ImGui::End();
  };
  device_wrapper.window_loop();
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}