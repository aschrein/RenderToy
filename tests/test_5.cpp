#include "../include/assets.hpp"
#include "../include/device.hpp"
#include "../include/ecs.hpp"
#include "../include/error_handling.hpp"
#include "../include/gizmo.hpp"
#include "../include/memory.hpp"
#include "../include/model_loader.hpp"
#include "../include/particle_sim.hpp"
#include "../include/path_tracing.hpp"
#include "../include/render_graph.hpp"
#include "../include/shader_compiler.hpp"
#include "f32_f16.hpp"

#include "../include/random.hpp"
#include "imgui.h"

#include "dir_monitor/include/dir_monitor/dir_monitor.hpp"
#include "gtest/gtest.h"
#include <boost/thread.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
namespace fs = std::filesystem;

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/ext.hpp>
#include <glm/glm.hpp>
using namespace glm;

#include <exception>
#include <omp.h>

#include "shaders.h"

void iterate_folder(std::string const &folder, std::vector<std::string> &models,
                    std::string const &ext) {
  for (const auto &entry : fs::directory_iterator(folder))
    if (entry.path().filename().string().find(ext) != std::string::npos) {
      models.push_back(folder +
                       entry.path().relative_path().filename().string());
    } else if (entry.is_directory()) {
      iterate_folder(entry.path().string() + "/", models, ext);
    }
}

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
    // Return ms
    return f32(frame_cpu_delta_ns / 1000000);
  }
};

TEST(graphics, vulkan_graphics_test_render_graph) try {
  Gizmo_Layer gizmo_layer{};
  PT_Manager pt_manager;
  /////////////////
  // Input Files //
  /////////////////
  bool reload_env = true;
  bool reload_model = true;
  Scene scene;
  std::string models_path = "models/";
  std::vector<std::string> model_filenames;
  std::vector<std::string> env_filenames;

  //  scene.load_env("spheremaps/lythwood_field.hdr");
  scene.init_black_env();
  //  scene.load_model("models/demon_in_thought_3d_print/scene.gltf");
  //  scene.load_model("models/MetalRoughSpheres/MetalRoughSpheres.gltf");
  scene.load_model("models/ssr_test_0.gltf");
  scene.push_light(Light_Source{
      .type = Light_Type::PLANE,
      .power = vec3(0.0f), // vec3(2.2f, 2.2f, 2.2f),
      .plane_light = Plane_Light{.position = vec3(24.3f, 13.2f, 1.2f),
                                 .up = vec3(0.0f, 10.0f, 0.0f),
                                 .right = vec3(0.0f, 0.0f, 10.0f)}});
  scene.push_light(Light_Source{
      .type = Light_Type::POINT,
      .power = vec3(0.0f), // 5.0f * vec3(10.0f, 10.0f, 10.0f),
      .point_light = Point_Light{.position = vec3(-1.4f, -12.1f, 11.6f)}});
  // @Cleanup
  const u32 sun_id = 2;
  scene.push_light(Light_Source{
      .type = Light_Type::DIRECTIONAL,
      .power = vec3(3.0f, 2.6f, 2.1f),
      .dir_light = Direction_Light{
          .direction = glm::normalize(vec3(-0.5f, -0.8f, -0.2f))}});
  iterate_folder("models/", model_filenames, ".gltf");
  iterate_folder("spheremaps/", env_filenames, ".hdr");
  gizmo_layer.camera.update();
  // Benchmark
  //  {
  //    CPU_timestamp __timestamp;
  //    pt_manager.reset_path_tracing_state(gizmo_layer.camera, 512, 512);
  //    while (pt_manager.path_tracing_queue.has_job())
  //      pt_manager.path_tracing_iteration(scene);
  //    std::cout << "Time ms:" << __timestamp.end() << "\n";
  //  }
  //  exit(0);
  //

  struct Path_Tracing_Plane_Push {
    mat4 viewprojmodel;
  };

  ImVec2 wsize(512, 512);
  render_graph::Graphics_Utils gu = render_graph::Graphics_Utils::create();
  float drag_val = 0.0;

  // #IMGUI
  bool display_gizmo_layer = false;
  bool enable_ao = false;
  bool display_ug = false;
  bool display_wire = false;
  bool denoise = false;
  bool display_lpv = false;
  bool display_shadow = false;
  gu.set_on_gui([&] {
    gizmo_layer.on_imgui_begin();
    if (gizmo_layer.mouse_click[0]) {
      pt_manager.eval_debug_ray(scene);
    }
    static bool show_demo = true;
    ImGui::Begin("Rasterizer");
    ImGui::PopStyleVar(3);
    gizmo_layer.on_imgui_viewport();
    //       gu.ImGui_Emit_Stats();
    wsize = ImVec2(gizmo_layer.example_viewport.extent.width,
                   gizmo_layer.example_viewport.extent.height);
    gu.ImGui_Image("postprocess.HDR", wsize.x, wsize.y);

    static int selected_fish = -1;
    const char *names[] = {"Bream", "Haddock", "Mackerel", "Pollock",
                           "Tilefish"};
    static bool toggles[] = {true, false, false, false, false};

    ImGui::OpenPopupOnItemClick("my_toggle_popup", 1);
    if (ImGui::BeginPopup("my_toggle_popup")) {
      for (int i = 0; i < IM_ARRAYSIZE(names); i++)
        ImGui::MenuItem(names[i], "", &toggles[i]);
      if (ImGui::BeginMenu("Sub-menu")) {
        ImGui::MenuItem("Click me");
        ImGui::EndMenu();
      }
      if (ImGui::Button("Exit"))
        std::exit(0);
      ImGui::Separator();
      ImGui::Text("Tooltip here");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("I am a tooltip over a popup");

      if (ImGui::Button("Stacked Popup"))
        ImGui::OpenPopup("another popup");
      if (ImGui::BeginPopup("another popup")) {
        for (int i = 0; i < IM_ARRAYSIZE(names); i++)
          ImGui::MenuItem(names[i], "", &toggles[i]);
        if (ImGui::BeginMenu("Sub-menu")) {
          ImGui::MenuItem("Click me");
          if (ImGui::Button("Stacked Popup"))
            ImGui::OpenPopup("another popup");
          if (ImGui::BeginPopup("another popup")) {
            ImGui::Text("I am the last one here.");
            ImGui::EndPopup();
          }
          ImGui::EndMenu();
        }
        ImGui::EndPopup();
      }
      ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::ShowDemoWindow(&show_demo);
    ImGui::Begin("Simulation parameters");

    ImGui::End();
    ImGui::Begin("Raytracer");
    if (ImGui::Button("Render with path tracer")) {
      pt_manager.reset_path_tracing_state(gizmo_layer.camera, 512, 512);
    }
    if (ImGui::Button("Add primary rays")) {
      pt_manager.add_primary_rays();
    }
    if (ImGui::Button("Reset Path tracer")) {
      pt_manager.path_tracing_queue.reset();
    }
    ImGui::InputInt("Samples per pixel", (int *)&pt_manager.samples_per_pixel);
    ImGui::InputInt("Max path depth", (int *)&pt_manager.max_depth);
    ImGui::Checkbox("Camera jitter", &gizmo_layer.jitter_on);
    ImGui::Checkbox("Gizmo layer", &display_gizmo_layer);
    ImGui::Checkbox("Denoise", &denoise);
    ImGui::Checkbox("Enable Raster AO", &enable_ao);
    ImGui::Checkbox("Enable LPV", &display_lpv);
    ImGui::Checkbox("Enable Shadows", &display_shadow);
    ImGui::Checkbox("Display UG", &display_ug);
    ImGui::Checkbox("Display Wire", &display_wire);
    ImGui::Checkbox("Use ISPC", &pt_manager.trace_ispc);
    ImGui::Checkbox("Use MT", &pt_manager.use_jobs);
    if (ImGui::TreeNode("Scene nodes")) {
      ito(scene.light_sources.size()) scene.light_sources[i].imgui_edit(i);
      ImGui::TreePop();
    }
    // Select in the list of named images available for display
    auto images = gu.get_img_list();

    static int image_item_current = 0;
    static int model_item_current = 0;
    static int env_item_current = 0;

    {
      std::vector<const char *> _model_filenames;
      for (auto const &filename : model_filenames)
        _model_filenames.push_back(filename.c_str());

      if (ImGui::Combo("Select model", &model_item_current,
                       &_model_filenames[0], _model_filenames.size())) {
        scene.reset_model();
        scene.load_model(_model_filenames[model_item_current]);
        reload_model = true;
      }
    }
    {
      std::vector<const char *> _env_filenames;
      for (auto const &filename : env_filenames)
        _env_filenames.push_back(filename.c_str());

      if (ImGui::Combo("Select env", &env_item_current, &_env_filenames[0],
                       _env_filenames.size())) {
        scene.load_env(_env_filenames[env_item_current]);
        reload_env = true;
      }
    }
    {
      std::vector<char const *> images_;
      int i = 0;
      for (auto &img_name : images) {
        if (img_name == "path_traced_scene")
          image_item_current = i;
        images_.push_back(img_name.c_str());
        i++;
      }
      ImGui::Combo("Select Image", &image_item_current, &images_[0],
                   images_.size());
    }
    auto wsize = ImGui::GetWindowSize();
    // @TODO: Select mip level
    gu.ImGui_Image(images[image_item_current], wsize.x - 2, wsize.x - 2);
    ImGui::End();
    ImGui::Begin("Metrics");
    ImGui::End();
    if (ImGui::GetIO().KeysDown[GLFW_KEY_ESCAPE]) {
      std::exit(0);
    }
  });
  u32 spheremap_id = 0;
  u32 ltc_invm_id = 0;
  u32 ltc_amp_id = 0;
  auto ltc_data = load_ltc_data();
  std::vector<Raw_Mesh_Opaque_Wrapper> models;
  std::vector<PBR_Material> materials;
  std::vector<u32> textures;
  std::function<void(u32)> traverse_node = [&](u32 node_id) {
    if (scene.pbr_model.nodes.size() <= node_id)
      return;
    auto &node = scene.pbr_model.nodes[node_id];

    for (auto i : node.meshes) {
      auto &model = models[i];
      auto &material = materials[i];
      sh_gltf_vert::push_constants pc;
      pc.transform = node.transform_cache;
      pc.albedo_id = material.albedo_id;
      pc.normal_id = material.normal_id;
      pc.arm_id = material.arm_id;
      pc.albedo_factor = material.albedo_factor;
      pc.metal_factor = material.metal_factor;
      pc.roughness_factor = material.roughness_factor;
      gu.push_constants(&pc, sizeof(pc));
      model.draw(gu);
    }
    for (auto child_id : node.children) {
      traverse_node(child_id);
    }
  };
  // Light propagation volume
  struct LPV {
    // AABB
    vec3 volume_min, volume_max, volume_cell_size;
    u32 volume_width, volume_height, volume_depth;
    // Reflective shadow map
    u32 rsm_size;
    // Camera config
    float nearz, farz;
    float width, height;
    // Sun
    u32 dir_light_id;
    mat4 sun_proj, sun_view;
    // Scaled:
    vec3 rsm_y;
    vec3 rsm_x;
    vec3 rsm_z;
    vec3 rsm_pos;
  } lpv = {};

  {
    lpv.volume_width = 32;
    lpv.volume_height = 32;
    lpv.volume_depth = 32;
    lpv.dir_light_id = sun_id;
    lpv.farz = 200.0f;
    lpv.nearz = 0.0f;
    lpv.width = 32.0f;
    lpv.height = 32.0f;
    lpv.volume_min = vec3(-32.0f);
    lpv.volume_max = vec3(32.0f);
    lpv.volume_cell_size = (lpv.volume_max - lpv.volume_min) / 32.0f;
  }
  auto update_lpv_state = [&] {
    lpv.sun_proj = mat4(1.0f);
    lpv.sun_proj[0][0] = 1.0f / lpv.width;
    lpv.sun_proj[1][1] = 1.0f / lpv.height;
    lpv.sun_proj[2][2] = 1.0f / lpv.farz;
    auto &dir_light = scene.get_ligth(lpv.dir_light_id).dir_light;
    vec3 light_pos = -100.f * dir_light.direction;
    vec3 up = std::abs(dir_light.direction.y) > 0.99f ? vec3(0.0f, 0.0f, 1.0f)
                                                      : vec3(0.0f, 1.0f, 0.0f);
    vec3 light_right = -glm::normalize(glm::cross(dir_light.direction, up));
    vec3 light_up = -glm::cross(light_right, dir_light.direction);
    lpv.rsm_y = light_up * lpv.height;
    lpv.rsm_x = light_right * lpv.width;
    lpv.rsm_z = dir_light.direction * lpv.farz;
    lpv.rsm_pos = light_pos;
    lpv.sun_view = glm::transpose(
        mat4(light_right.x, light_right.y, light_right.z,
             -dot(light_pos, light_right), light_up.x, light_up.y, light_up.z,
             -dot(light_pos, light_up), dir_light.direction.x,
             dir_light.direction.y, dir_light.direction.z,
             -dot(light_pos, dir_light.direction), 0.0f, 0.0f, 0.0f, 1.0f));
  };
  // @TODO: Derive structures from shader reflection
  struct GPU_Point_Light {
    vec4 position;
    vec4 power;
  };
  struct GPU_Dir_Light {
    vec4 dir;
    vec4 power;
  };
  struct GPU_Plane_Light {
    vec4 position;
    vec4 up;
    vec4 right;
    vec4 power;
  };
  std::vector<GPU_Point_Light> point_lights;
  std::vector<GPU_Plane_Light> plane_lights;
  std::vector<GPU_Dir_Light> dir_lights;
  gu.run_loop([&] {
    update_lpv_state();
    point_lights.clear();
    plane_lights.clear();
    dir_lights.clear();
    ito(scene.light_sources.size()) {
      auto &light = scene.light_sources[i];
      if (light.type == Light_Type::POINT) {
        point_lights.push_back(
            {.position = vec4(light.point_light.position, 0.0f),
             .power = vec4(light.power, 0.0f)});
        gizmo_layer.push_line_sphere(light.point_light.position, 1.0f,
                                     light.power);
      } else if (light.type == Light_Type::PLANE) {
        plane_lights.push_back(
            {.position = vec4(light.plane_light.position, 0.0f),
             .up = vec4(light.plane_light.up, 0.0f),
             .right = vec4(light.plane_light.right, 0.0f),
             .power = vec4(light.power, 0.0f)});
        gizmo_layer.push_line_plane(light.plane_light.position,
                                    light.plane_light.up,
                                    light.plane_light.right, light.power);
      } else if (light.type == Light_Type::DIRECTIONAL) {
        dir_lights.push_back({.dir = vec4(light.dir_light.direction, 1.0f),
                              .power = vec4(light.power, 5.0f)});
      }
    }

    scene.get_ligth(0).plane_light.position = gizmo_layer.gizmo_drag_state.pos;

    scene.update_transforms();
    pt_manager.update_debug_ray(scene, gizmo_layer.camera.pos,
                                gizmo_layer.mouse_ray);
    pt_manager.path_tracing_iteration(scene);
    u32 spheremap_mip_levels =
        get_mip_levels(scene.spheremap.width, scene.spheremap.height);
    gu.create_compute_pass(
        "LPV_pass",
        {"sun_rsm.radiant_flux", "sun_rsm.normal", "sun_rsm.depth", "~LPV.R",
         "~LPV.G", "~LPV.B"},
        {
            render_graph::Resource{
                .name = "LPV.R",
                .type = render_graph::Type::Image,
                .image_info =
                    render_graph::Image{.format =
                                            vk::Format::eR16G16B16A16Sfloat,
                                        .use = render_graph::Use::UAV,
                                        .width = lpv.volume_width,
                                        .height = lpv.volume_height,
                                        .depth = lpv.volume_depth,
                                        .levels = 1,
                                        .layers = 1}},
            render_graph::Resource{
                .name = "LPV.G",
                .type = render_graph::Type::Image,
                .image_info =
                    render_graph::Image{.format =
                                            vk::Format::eR16G16B16A16Sfloat,
                                        .use = render_graph::Use::UAV,
                                        .width = lpv.volume_width,
                                        .height = lpv.volume_height,
                                        .depth = lpv.volume_depth,
                                        .levels = 1,
                                        .layers = 1}},
            render_graph::Resource{
                .name = "LPV.B",
                .type = render_graph::Type::Image,
                .image_info =
                    render_graph::Image{.format =
                                            vk::Format::eR16G16B16A16Sfloat,
                                        .use = render_graph::Use::UAV,
                                        .width = lpv.volume_width,
                                        .height = lpv.volume_height,
                                        .depth = lpv.volume_depth,
                                        .levels = 1,
                                        .layers = 1}},

        },
        [&] {
          if (!display_lpv)
            return;
          sh_lpv_comp::UBO ubo{};
          ubo.lpv_min = lpv.volume_min;
          ubo.lpv_max = lpv.volume_max;
          ubo.lpv_cell_size = lpv.volume_cell_size;
          ubo.lpv_size =
              uvec3(lpv.volume_width, lpv.volume_height, lpv.volume_depth);
          ubo.rsm_x = lpv.rsm_x;
          ubo.rsm_y = lpv.rsm_y;
          ubo.rsm_z = lpv.rsm_z;
          ubo.rsm_pos = lpv.rsm_pos;
          ubo.rsm_viewproj = lpv.sun_proj * lpv.sun_view;
          u32 buf_id = gu.create_buffer(
              render_graph::Buffer{.usage_bits =
                                       vk::BufferUsageFlagBits::eUniformBuffer,
                                   .size = u32(sizeof(ubo))},
              &ubo);
          gu.bind_resource("UBO", buf_id);
          gu.bind_resource("LPV_R", "LPV.R");
          gu.bind_resource("LPV_G", "LPV.G");
          gu.bind_resource("LPV_B", "LPV.B");
          gu.bind_resource("rsm_radiant_flux", "sun_rsm.radiant_flux");
          gu.bind_resource("rsm_normal", "sun_rsm.normal");
          gu.bind_resource("rsm_depth", "sun_rsm.depth");
          gu.bind_resource("s_LPV_R", "~LPV.R");
          gu.bind_resource("s_LPV_G", "~LPV.G");
          gu.bind_resource("s_LPV_B", "~LPV.B");
          gu.bind_resource("n_LPV_R", "LPV.R");
          gu.bind_resource("n_LPV_G", "LPV.G");
          gu.bind_resource("n_LPV_B", "LPV.B");
          gu.CS_set_shader("lpv.comp.glsl");
          // Injection
          gu.dispatch((lpv.volume_width + 7) / 8, (lpv.volume_height + 7) / 8,
                      (lpv.volume_depth + 7) / 8);

          gu.CS_set_shader("lpv_prop.comp.glsl");
          // Propagation
          gu.dispatch((lpv.volume_width + 7) / 8, (lpv.volume_height + 7) / 8,
                      (lpv.volume_depth + 7) / 8);
          gu.release_resource(buf_id);
        });
    gu.create_compute_pass(
        "init_pass", {},
        {
            render_graph::Resource{
                .name = "IBL.specular",
                .type = render_graph::Type::Image,
                .image_info =
                    render_graph::Image{.format =
                                            vk::Format::eR32G32B32A32Sfloat,
                                        .use = render_graph::Use::UAV,
                                        .width = scene.spheremap.width,
                                        .height = scene.spheremap.height,
                                        .depth = 1,
                                        .levels = spheremap_mip_levels,
                                        .layers = 1}},
            render_graph::Resource{
                .name = "IBL.diffuse",
                .type = render_graph::Type::Image,
                .image_info =
                    render_graph::Image{
                        .format = vk::Format::eR32G32B32A32Sfloat,
                        .use = render_graph::Use::UAV,
                        .width = std::max(scene.spheremap.width / 8u, 2u),
                        .height = std::max(scene.spheremap.height / 8u, 2u),
                        .depth = 1,
                        .levels = 1,
                        .layers = 1}},
            render_graph::Resource{
                .name = "IBL.LUT",
                .type = render_graph::Type::Image,
                .image_info =
                    render_graph::Image{.format =
                                            vk::Format::eR32G32B32A32Sfloat,
                                        .use = render_graph::Use::UAV,
                                        .width = 128,
                                        .height = 128,
                                        .depth = 1,
                                        .levels = 1,
                                        .layers = 1}},

        },
        [&, spheremap_mip_levels] {
          if (!ltc_invm_id) {
            ltc_invm_id = gu.create_texture2D(ltc_data.inv, false);
            ltc_amp_id = gu.create_texture2D(ltc_data.ampl, false);
          }
          if (reload_env || reload_model) {
            if (reload_model) {
              for (auto &model : models) {
                gu.release_resource(model.index_buffer);
                gu.release_resource(model.vertex_buffer);
              }
              models.clear();
              materials.clear();
              for (auto &tex : textures) {
                gu.release_resource(tex);
              }
              textures.clear();
              ito(scene.pbr_model.meshes.size()) {
                auto &model = scene.pbr_model.meshes[i];
                materials.push_back(scene.pbr_model.materials[i]);
                models.push_back(Raw_Mesh_Opaque_Wrapper::create(gu, model));
              }
              ito(scene.pbr_model.images.size()) {
                auto &img = scene.pbr_model.images[i];
                textures.push_back(gu.create_texture2D(img, true));
              }
              reload_model = false;
            }
            if (reload_env) {
              if (spheremap_id)
                gu.release_resource(spheremap_id);
              spheremap_id = gu.create_texture2D(scene.spheremap, true);
              reload_env = false;
              gu.CS_set_shader("ibl_integrator.comp.glsl");
              gu.bind_resource("in_image", spheremap_id);
              gu.bind_resource("out_image", "IBL.diffuse", 0);
              gu.bind_resource("out_image", "IBL.LUT", 1);
              ito(spheremap_mip_levels) {
                gu.bind_image(
                    "out_image", "IBL.specular", i + 2,
                    render_graph::Image_View{.base_level = i, .levels = 1});
              }
              const uint DIFFUSE = 0;
              const uint SPECULAR = 1;
              const uint LUT = 2;
              {
                sh_ibl_integrator_comp::push_constants pc{};
                pc.level = 0;
                pc.max_level = spheremap_mip_levels;
                pc.mode = LUT;
                gu.push_constants(&pc, sizeof(pc));
                gu.dispatch((128 + 15) / 16, (128 + 15) / 16, 1);
              }
              u32 width = scene.spheremap.width;
              u32 height = scene.spheremap.height;
              {
                sh_ibl_integrator_comp::push_constants pc{};
                pc.level = 0;
                pc.max_level = spheremap_mip_levels;
                pc.mode = DIFFUSE;
                gu.push_constants(&pc, sizeof(pc));
                gu.dispatch((width / 8 + 15) / 16, (height / 8 + 15) / 16, 1);
              }
              ito(spheremap_mip_levels) {
                sh_ibl_integrator_comp::push_constants pc{};
                pc.level = i;
                pc.max_level = spheremap_mip_levels;
                pc.mode = SPECULAR;
                gu.push_constants(&pc, sizeof(pc));
                gu.dispatch((width + 15) / 16, (height + 15) / 16, 1);
                width = std::max(1u, width / 2);
                height = std::max(1u, height / 2);
              }
            }

          } else {
            return;
          }
        });
    if (pt_manager.path_tracing_image.data.size()) {
      gu.create_compute_pass(
          "fill_images", {},
          {render_graph::Resource{
              .name = "path_traced_scene",
              .type = render_graph::Type::Image,
              .image_info =
                  render_graph::Image{
                      .format = vk::Format::eR32G32B32A32Sfloat,
                      .use = render_graph::Use::UAV,
                      .width = u32(pt_manager.path_tracing_image.width),
                      .height = u32(pt_manager.path_tracing_image.height),
                      .depth = 1,
                      .levels = 1,
                      .layers = 1}}},
          [&] {
            void *data = nullptr;
            if (denoise) {
              pt_manager.path_tracing_image.denoise();
              data = &pt_manager.path_tracing_image.denoised_data[0];
            } else {
              data = &pt_manager.path_tracing_image.data[0];
            }
            u32 buf_id = gu.create_buffer(
                render_graph::Buffer{
                    .usage_bits = vk::BufferUsageFlagBits::eStorageBuffer,
                    .size = u32(pt_manager.path_tracing_image.data.size() *
                                sizeof(pt_manager.path_tracing_image.data[0]))},
                data);

            gu.bind_resource("Bins", buf_id);
            gu.bind_resource("out_image", "path_traced_scene");
            gu.CS_set_shader("swap_image.comp.glsl");
            gu.dispatch(u32(pt_manager.path_tracing_image.width + 15) / 16,
                        u32(pt_manager.path_tracing_image.height + 15) / 16, 1);
            gu.release_resource(buf_id);
          });
    }
    gu.create_compute_pass(
        "postprocess", {"shading.HDR", "SSR.UV"},
        {render_graph::Resource{
            .name = "postprocess.HDR",
            .type = render_graph::Type::Image,
            .image_info =
                render_graph::Image{.format = vk::Format::eR32G32B32A32Sfloat,
                                    .use = render_graph::Use::UAV,
                                    .width = u32(wsize.x),
                                    .height = u32(wsize.y),
                                    .depth = 1,
                                    .levels = 1,
                                    .layers = 1}}},
        [&] {
          sh_postprocess_comp::push_constants pc{};
          pc.offset = vec4(drag_val, drag_val, drag_val, drag_val);

          gu.push_constants(&pc, sizeof(pc));
          gu.bind_resource("out_image", "postprocess.HDR");
          gu.bind_resource("in_image", "shading.HDR");
          gu.bind_resource("in_ssr_uv", "SSR.UV");
          gu.CS_set_shader("postprocess.comp.glsl");
          gu.dispatch(u32(wsize.x + 15) / 16, u32(wsize.y + 15) / 16, 1);
        });
        gu.create_compute_pass(
        "SSR",
        {"g_pass.normal", "g_pass.metal", "depth_mips"},
        {render_graph::Resource{
            .name = "SSR.UV",
            .type = render_graph::Type::Image,
            .image_info =
                render_graph::Image{.format = vk::Format::eR32G32B32A32Sfloat,
                                    .use = render_graph::Use::UAV,
                                    .width = u32(wsize.x),
                                    .height = u32(wsize.y),
                                    .depth = 1,
                                    .levels = 1,
                                    .layers = 1}}},
        [&] {
          sh_ssr_comp::UBO ubo{};
          ubo.camera_up = gizmo_layer.camera.up;
          ubo.camera_pos = gizmo_layer.camera.pos;
          ubo.camera_right = gizmo_layer.camera.right;
          ubo.camera_look = gizmo_layer.camera.look;
          ubo.camera_inv_tan = 1.0f / std::tan(gizmo_layer.camera.fov / 2.0f);
          ubo.camera_jitter = gizmo_layer.camera_jitter;
          ubo.viewproj = gizmo_layer.camera.viewproj();
          ubo.view = gizmo_layer.camera.view;

          u32 ubo_id = gu.create_buffer(
              render_graph::Buffer{.usage_bits =
                                       vk::BufferUsageFlagBits::eUniformBuffer,
                                   .size = sizeof(ubo)},
              &ubo);
          gu.bind_resource("UBO", ubo_id);

          gu.bind_resource("out_image", "SSR.UV");
          gu.bind_resource("g_normal", "g_pass.normal");
          gu.bind_resource("g_metal", "g_pass.metal");
          gu.bind_resource("g_depth", "depth_mips");

          gu.CS_set_shader("ssr.comp.glsl");
          gu.dispatch(u32(wsize.x + 15) / 16, u32(wsize.y + 15) / 16, 1);
          gu.release_resource(ubo_id);

        });

    gu.create_compute_pass(
        "shading",
        {"g_pass.albedo", "g_pass.normal", "g_pass.metal", "depth_mips",
         "~shading.HDR", "IBL.specular", "IBL.LUT", "IBL.diffuse",
         "g_pass.gizmo", "LPV.R", "LPV.G", "LPV.B"
         },
        {render_graph::Resource{
            .name = "shading.HDR",
            .type = render_graph::Type::Image,
            .image_info =
                render_graph::Image{.format = vk::Format::eR32G32B32A32Sfloat,
                                    .use = render_graph::Use::UAV,
                                    .width = u32(wsize.x),
                                    .height = u32(wsize.y),
                                    .depth = 1,
                                    .levels = 1,
                                    .layers = 1}}},
        [&] {
          static bool prev_cam_moved = false;

          u32 point_lights_buffer_id = 0u;
          u32 plane_lights_buffer_id = 0u;
          u32 dir_lights_buffer_id = 0u;
          if (point_lights.size()) {
            point_lights_buffer_id = gu.create_buffer(
                render_graph::Buffer{
                    .usage_bits = vk::BufferUsageFlagBits::eStorageBuffer,
                    .size = u32(point_lights.size() * sizeof(GPU_Point_Light))},
                &point_lights[0]);
            gu.bind_resource("PointLightList", point_lights_buffer_id);
          }
          if (plane_lights.size()) {
            plane_lights_buffer_id = gu.create_buffer(
                render_graph::Buffer{
                    .usage_bits = vk::BufferUsageFlagBits::eStorageBuffer,
                    .size = u32(plane_lights.size() * sizeof(plane_lights[0]))},
                &plane_lights[0]);
            gu.bind_resource("PlaneLightList", plane_lights_buffer_id);
          }
          if (dir_lights.size()) {
            dir_lights_buffer_id = gu.create_buffer(
                render_graph::Buffer{
                    .usage_bits = vk::BufferUsageFlagBits::eStorageBuffer,
                    .size = u32(dir_lights.size() * sizeof(dir_lights[0]))},
                &dir_lights[0]);
            gu.bind_resource("DirLightList", dir_lights_buffer_id);
          }
          mat4 sun_viewproj = lpv.sun_proj * lpv.sun_view;
          u32 matrixlist_buffer_id = gu.create_buffer(
              render_graph::Buffer{.usage_bits =
                                       vk::BufferUsageFlagBits::eStorageBuffer,
                                   .size = u32(1 * sizeof(mat4))},
              &sun_viewproj);
          gu.bind_resource("MatrixList", matrixlist_buffer_id);
          sh_pbr_shading_comp::UBO ubo{};
          ubo.point_lights_count = point_lights.size();
          ubo.plane_lights_count = plane_lights.size();
          ubo.dir_lights_count = dir_lights.size();
          ubo.camera_up = gizmo_layer.camera.up;
          ubo.camera_pos = gizmo_layer.camera.pos;
          ubo.camera_right = gizmo_layer.camera.right;
          ubo.camera_look = gizmo_layer.camera.look;
          ubo.camera_inv_tan = 1.0f / std::tan(gizmo_layer.camera.fov / 2.0f);

          ubo.camera_jitter = gizmo_layer.camera_jitter;
          ubo.taa_weight = (gizmo_layer.camera_moved || prev_cam_moved ||
                            !gizmo_layer.jitter_on)
                               ? 0.0f
                               : 0.95f;
          prev_cam_moved = gizmo_layer.camera_moved;
          const uint DISPLAY_GIZMO = 1;
          const uint DISPLAY_AO = 2;
          const uint ENABLE_SHADOW = 4;
          const uint ENABLE_LPV = 8;
          ubo.mask = 0;
          ubo.mask |= display_gizmo_layer ? DISPLAY_GIZMO : 0;
          ubo.mask |= enable_ao ? DISPLAY_AO : 0;
          ubo.mask |= display_lpv ? ENABLE_LPV : 0;
          ubo.mask |= display_shadow ? ENABLE_SHADOW : 0;
          ubo.lpv_max = lpv.volume_max;
          ubo.lpv_min = lpv.volume_min;
          ubo.lpv_cell_size = lpv.volume_cell_size;

          u32 ubo_id = gu.create_buffer(
              render_graph::Buffer{.usage_bits =
                                       vk::BufferUsageFlagBits::eUniformBuffer,
                                   .size = sizeof(ubo)},
              &ubo);
          gu.bind_resource("UBO", ubo_id);

          gu.bind_resource("out_image", "shading.HDR");
          gu.bind_resource("g_albedo", "g_pass.albedo");
          gu.bind_resource("g_normal", "g_pass.normal");
          gu.bind_resource("g_metal", "g_pass.metal");
          gu.bind_resource("g_gizmo", "g_pass.gizmo");
          gu.bind_resource("history", "~shading.HDR");
          gu.bind_resource("g_depth", "depth_linear");
          gu.bind_resource("textures", "IBL.diffuse", 0);
          gu.bind_resource("textures", "IBL.specular", 1);
          gu.bind_resource("textures", "IBL.LUT", 2);
          gu.bind_resource("textures", ltc_invm_id, 3);
          gu.bind_resource("textures", ltc_amp_id, 4);
          gu.bind_resource("textures", "sun_rsm.depth", 5);
//          gu.bind_resource("textures", "SSR.UV", 6);
          gu.bind_resource("g_lpv_r", "LPV.R");
          gu.bind_resource("g_lpv_g", "LPV.G");
          gu.bind_resource("g_lpv_b", "LPV.B");
          gu.CS_set_shader("pbr_shading.comp.glsl");
          gu.dispatch(u32(wsize.x + 15) / 16, u32(wsize.y + 15) / 16, 1);
          gu.release_resource(ubo_id);
          if (point_lights_buffer_id)
            gu.release_resource(point_lights_buffer_id);
          if (plane_lights_buffer_id)
            gu.release_resource(plane_lights_buffer_id);
          if (dir_lights_buffer_id)
            gu.release_resource(dir_lights_buffer_id);
        });
    u32 bb_miplevels = get_mip_levels(u32(wsize.x), u32(wsize.y));
    gu.create_compute_pass(
        "depth_mips_build", {"depth_linear"},
        {render_graph::Resource{
            .name = "depth_mips",
            .type = render_graph::Type::Image,
            .image_info = render_graph::Image{.format = vk::Format::eR32Sfloat,
                                              .use = render_graph::Use::UAV,
                                              .width = u32(wsize.x),
                                              .height = u32(wsize.y),
                                              .depth = 1,
                                              .levels = bb_miplevels,
                                              .layers = 1}}},
        [&gu, wsize, bb_miplevels] {
          u32 width = u32(wsize.x);
          u32 height = u32(wsize.y);
          gu.CS_set_shader("zpyramid_build.comp.glsl");
          gu.bind_image("in_image", "depth_linear", 0,
                        render_graph::Image_View{});
          ito(bb_miplevels) {
            gu.bind_image(
                "in_image", "depth_mips", i + 1,
                render_graph::Image_View{.base_level = i, .levels = 1});
            gu.bind_image(
                "out_image", "depth_mips", i,
                render_graph::Image_View{.base_level = i, .levels = 1});
          }
          ito(bb_miplevels) {
            sh_zpyramid_build_comp::push_constants pc{};
            if (i == 0) {
              pc.copy = 1;
            } else {
              pc.copy = 0;
            }
            pc.src_level = i;
            pc.dst_level = i;
            gu.push_constants(&pc, sizeof(pc));
            gu.dispatch((width + 15) / 16, (height + 15) / 16, 1);
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
          }
        });
    gu.create_compute_pass(
        "depth_linearize", {"g_pass.depth"},
        {render_graph::Resource{
            .name = "depth_linear",
            .type = render_graph::Type::Image,
            .image_info = render_graph::Image{.format = vk::Format::eR32Sfloat,
                                              .use = render_graph::Use::UAV,
                                              .width = u32(wsize.x),
                                              .height = u32(wsize.y),
                                              .depth = 1,
                                              .levels = 1,
                                              .layers = 1}}},
        [&] {
          sh_linearize_depth_comp::push_constants pc{};
          pc.zfar = gizmo_layer.camera.zfar;
          pc.znear = gizmo_layer.camera.znear;
          gu.push_constants(&pc, sizeof(pc));
          gu.bind_resource("out_image", "depth_linear");
          gu.bind_resource("in_depth", "g_pass.depth");
          gu.CS_set_shader("linearize_depth.comp.glsl");
          gu.dispatch(u32(wsize.x + 15) / 16, u32(wsize.y + 15) / 16, 1);
        });
    gu.create_render_pass(
        "sun_rsm", {},
        {
            render_graph::Resource{
                .name = "sun_rsm.radiant_flux",
                .type = render_graph::Type::RT,
                .rt_info =
                    render_graph::RT{.format = vk::Format::eR32G32B32A32Sfloat,
                                     .target =
                                         render_graph::Render_Target::Color}},

            render_graph::Resource{
                .name = "sun_rsm.normal",
                .type = render_graph::Type::RT,
                .rt_info =
                    render_graph::RT{.format = vk::Format::eR32G32B32A32Sfloat,
                                     .target =
                                         render_graph::Render_Target::Color}},
            render_graph::Resource{
                .name = "sun_rsm.depth",
                .type = render_graph::Type::RT,
                .rt_info =
                    render_graph::RT{.format = vk::Format::eD32Sfloat,
                                     .target =
                                         render_graph::Render_Target::Depth}},

        },
        2048, 2048, [&] {
          if (!display_shadow)
            return;
          gu.clear_color({0.0f, 0.0f, 0.0f, 0.0f});
          gu.clear_depth(1.0f);
          gu.VS_set_shader("glrf.rsm.vert.glsl");
          gu.PS_set_shader("rsm.frag.glsl");
          auto &dir_light = scene.get_ligth(lpv.dir_light_id).dir_light;
          sh_glrf_rsm_vert::UBO ubo{};
          ubo.proj = lpv.sun_proj;
          ubo.view = lpv.sun_view;

          ubo.L = -dir_light.direction;
          ubo.power = scene.get_ligth(lpv.dir_light_id).power;
          u32 ubo_id = gu.create_buffer(
              render_graph::Buffer{.usage_bits =
                                       vk::BufferUsageFlagBits::eUniformBuffer,
                                   .size = sizeof(ubo)},
              &ubo);
          gu.bind_resource("UBO", ubo_id);
          gu.IA_set_topology(vk::PrimitiveTopology::eTriangleList);
          gu.IA_set_cull_mode(vk::CullModeFlagBits::eNone,
                              vk::FrontFace::eClockwise, vk::PolygonMode::eFill,
                              1.0f);
          gu.RS_set_depth_stencil_state(true, vk::CompareOp::eLessOrEqual, true,
                                        1.0f);
          ito(textures.size()) {
            auto &tex = textures[i];
            gu.bind_resource("textures", tex, i);
          }

          traverse_node(0);
          gu.release_resource(ubo_id);
        });
    gu.create_render_pass(
        "g_pass", {"LPV.R", "LPV.G", "LPV.B"},
        {
            render_graph::Resource{
                .name = "g_pass.albedo",
                .type = render_graph::Type::RT,
                .rt_info =
                    render_graph::RT{.format = vk::Format::eR32G32B32A32Sfloat,
                                     .target =
                                         render_graph::Render_Target::Color}},

            render_graph::Resource{
                .name = "g_pass.normal",
                .type = render_graph::Type::RT,
                .rt_info =
                    render_graph::RT{.format = vk::Format::eR32G32B32A32Sfloat,
                                     .target =
                                         render_graph::Render_Target::Color}},
            render_graph::Resource{
                .name = "g_pass.metal",
                .type = render_graph::Type::RT,
                .rt_info =
                    render_graph::RT{.format = vk::Format::eR32G32B32A32Sfloat,
                                     .target =
                                         render_graph::Render_Target::Color}},
            render_graph::Resource{
                .name = "g_pass.gizmo",
                .type = render_graph::Type::RT,
                .rt_info =
                    render_graph::RT{.format = vk::Format::eR32G32B32A32Sfloat,
                                     .target =
                                         render_graph::Render_Target::Color}},
            render_graph::Resource{
                .name = "g_pass.depth",
                .type = render_graph::Type::RT,
                .rt_info =
                    render_graph::RT{.format = vk::Format::eD32Sfloat,
                                     .target =
                                         render_graph::Render_Target::Depth}},

        },
        wsize.x, wsize.y, [&] {
          // Bindings
          gu.bind_resource("s_LPV_R", "LPV.R");
          gu.bind_resource("s_LPV_G", "LPV.G");
          gu.bind_resource("s_LPV_B", "LPV.B");
          sh_gltf_vert::UBO ubo{};
          ubo.proj = gizmo_layer.camera.proj;
          ubo.view = gizmo_layer.camera.view;
          ubo.camera_pos = gizmo_layer.camera.pos;
          u32 ubo_id = gu.create_buffer(
              render_graph::Buffer{.usage_bits =
                                       vk::BufferUsageFlagBits::eUniformBuffer,
                                   .size = sizeof(sh_gltf_vert::UBO)},
              &ubo);
          gu.bind_resource("UBO", ubo_id);
          ito(textures.size()) {
            auto &tex = textures[i];
            gu.bind_resource("textures", tex, i);
          }

          //
          gu.clear_color({0.0f, 0.0f, 0.0f, 0.0f});
          gu.clear_depth(1.0f);
          gu.VS_set_shader("gltf.vert.glsl");
          gu.PS_set_shader("gltf.frag.glsl");

          gu.IA_set_topology(vk::PrimitiveTopology::eTriangleList);
          gu.IA_set_cull_mode(vk::CullModeFlagBits::eBack,
                              vk::FrontFace::eClockwise, vk::PolygonMode::eFill,
                              1.0f);
          gu.RS_set_depth_stencil_state(true, vk::CompareOp::eLessOrEqual, true,
                                        1.0f);

          traverse_node(0);
          if (display_gizmo_layer) {
            {
              sh_lpv_debug_vert::g_ubo pc{};
              pc.viewproj = gizmo_layer.camera.viewproj();
              pc.lpv_min = lpv.volume_min;
              pc.lpv_max = lpv.volume_max;
              pc.lpv_cell_size = lpv.volume_cell_size;
              pc.lpv_size =
                  uvec3(lpv.volume_width, lpv.volume_height, lpv.volume_depth);
              gu.push_constants(&pc, sizeof(pc));
              gu.IA_set_topology(vk::PrimitiveTopology::eLineList);
              gu.IA_set_cull_mode(vk::CullModeFlagBits::eBack,
                                  vk::FrontFace::eClockwise,
                                  vk::PolygonMode::eLine, 1.0f);
              gu.RS_set_depth_stencil_state(true, vk::CompareOp::eLessOrEqual,
                                            false, 1.0f, -0.1f);

              gu.VS_set_shader("lpv.debug.vert.glsl");
              gu.PS_set_shader("gizmo.frag.glsl");
              gu.draw(pc.lpv_size.x * pc.lpv_size.y * pc.lpv_size.z * 2 * 3, 1,
                      0, 0);
            }
            if (display_wire) {
              gu.VS_set_shader("gltf.vert.glsl");
              gu.PS_set_shader("red.frag.glsl");

              gu.IA_set_topology(vk::PrimitiveTopology::eTriangleList);
              gu.IA_set_cull_mode(vk::CullModeFlagBits::eBack,
                                  vk::FrontFace::eClockwise,
                                  vk::PolygonMode::eLine, 1.0f);
              gu.RS_set_depth_stencil_state(true, vk::CompareOp::eLessOrEqual,
                                            false, 1.0f, -0.1f);
              traverse_node(0);
            }
            int N = 16;
            float dx = 10.0f;
            float half = ((N - 1) * dx) / 2.0f;
            ito(N) {
              float x = i * dx - half;
              gizmo_layer.push_line(vec3(x, 0.0f, -half), vec3(x, 0.0f, half),
                                    vec3(0.0f, 0.0f, 0.0f));
              gizmo_layer.push_line(vec3(-half, 0.0f, x), vec3(half, 0.0f, x),
                                    vec3(0.0f, 0.0f, 0.0f));
            }
            {
              gizmo_layer.push_line(pt_manager.path_tracing_camera.pos,
                                    pt_manager.path_tracing_camera._debug_pos,
                                    vec3(1.0f, 0.0f, 0.0f));
            }
            if (display_ug) {
              std::vector<vec3> ug_lines;
              for (auto &snode : scene.scene_nodes) {
                std::vector<vec3> ug_lines_t;
                snode.ug.fill_lines_render(ug_lines_t);
                for (auto &p : ug_lines_t) {
                  vec4 t = snode.transform * vec4(p, 1.0f);
                  ug_lines.push_back(vec3(t.x, t.y, t.z));
                }
              }
              UG::push_cube(ug_lines, lpv.volume_min.x, lpv.volume_min.y,
                            lpv.volume_min.z,
                            lpv.volume_max.x - lpv.volume_min.x,
                            lpv.volume_max.y - lpv.volume_min.y,
                            lpv.volume_max.z - lpv.volume_min.z);
              ito(ug_lines.size() / 2) {
                gizmo_layer.push_line(ug_lines[i * 2], ug_lines[i * 2 + 1],
                                      vec3(1.0f, 1.0f, 1.0f));
              }
            }
            if (pt_manager.path_tracing_camera._debug_path.size() > 1) {
              ito(pt_manager.path_tracing_camera._debug_path.size() / 2) {
                auto p0 = pt_manager.path_tracing_camera._debug_path[i * 2];
                auto p1 = pt_manager.path_tracing_camera._debug_path[i * 2 + 1];
                gizmo_layer.push_line(p0, p1, vec3(1.0f, 1.0f, 0.0f));
              }
            }
            gizmo_layer.draw(gu);
          }
          gu.release_resource(ubo_id);

          //          u32 i = 0;
          //          for (auto &model : models) {
          //            auto &material = materials[i];
          //            sh_gltf_frag::push_constants pc;
          //            pc.transform = mat4(1.0f);
          //            pc.albedo_id = material.albedo_id;
          //            pc.ao_id = material.ao_id;
          //            pc.normal_id = material.normal_id;
          //            pc.metalness_roughness_id =
          //            material.metalness_roughness_id; gu.push_constants(&pc,
          //            sizeof(pc)); model.draw(gu); i++;
          //          }
        });
  });
} catch (std::exception const &exc) {
  std::cerr << exc.what() << "\n";
  // @TODO: Disable exceptions
  // ASSERT_PANIC(false);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
