#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <vector>
#include <SDL.h>
#include "arcball_camera.h"
#include "imgui.h"
#include "scene.h"
#include "stb_image_write.h"
#include "util.h"
#include "util/display/display.h"
#include "util/display/gldisplay.h"
#include "util/display/imgui_impl_sdl.h"
#include "util/profiling.h"
#include "util/render_plugin.h"

const std::string USAGE =
    "Usage: <backend> <mesh.obj/gltf/glb> [options]\n"
    "Render backend libraries should be named following (lib)crt_<backend>.(dll|so)\n"
    "Options:\n"
    "\t-eye <x> <y> <z>       Set the camera position\n"
    "\t-center <x> <y> <z>    Set the camera focus point\n"
    "\t-up <x> <y> <z>        Set the camera up vector\n"
    "\t-fov <fovy>            Specify the camera field of view (in degrees)\n"
    "\t-camera <n>            If the scene contains multiple cameras, specify which\n"
    "\t                       should be used. Defaults to the first camera\n"
    "\t-img <x> <y>           Specify the window dimensions. Defaults to 1280x720\n"
#ifdef ENABLE_OIDN
    "\t-oidn-interop <mode>  Specify the OIDN interop mode\n"
#endif
    "\t-scene-report <path>   Write scene statistics to <path>.json and continue\n"
    "\t-benchmark-frames <n>  Render <n> frames then exit (for automated benchmarking)\n"
    "\t-profiling <base>      Write benchmark CSV/JSON to <base>.csv/.json (implies a\n"
    "\t                       bounded benchmark run)\n"
    "\t-profiling-fps <n>     Scales the default benchmark frame budget when\n"
    "\t                       -benchmark-frames is unset\n"
    "\n";

int win_width = 1280;
int win_height = 720;

void run_app(const std::vector<std::string> &args,
             SDL_Window *window,
             Display *display,
             RenderPlugin *render_plugin);

glm::vec2 transform_mouse(glm::vec2 in)
{
    return glm::vec2(in.x * 2.f / win_width - 1.f, 1.f - 2.f * in.y / win_height);
}

// Deterministic "random" color derived from a span's name, so each pass keeps a
// stable color across frames instead of flickering.
ImU32 timeline_color(const char *name)
{
    uint32_t hash = 2166136261u;  // FNV-1a
    for (const char *p = name; *p; ++p) {
        hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
    }
    const float hue = (hash % 360) / 360.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.6f, 0.9f, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

int main(int argc, const char **argv)
{
    const std::vector<std::string> args(argv, argv + argc);
    auto fnd_help = std::find_if(args.begin(), args.end(), [](const std::string &a) {
        return a == "-h" || a == "--help";
    });

    if (argc < 3 || fnd_help != args.end()) {
        std::cout << USAGE;
        return 1;
    }

    if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
        std::cerr << "Failed to init SDL: " << SDL_GetError() << "\n";
        return -1;
    }

    std::unique_ptr<RenderPlugin> render_plugin =
        std::make_unique<RenderPlugin>("crt_" + args[1]);
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "-img") {
            win_width = std::stoi(args[++i]);
            win_height = std::stoi(args[++i]);
            continue;
        }
    }

    const uint32_t window_flags = render_plugin->get_window_flags() | SDL_WINDOW_RESIZABLE;
    if (window_flags & SDL_WINDOW_OPENGL) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    }

    SDL_Window *window = SDL_CreateWindow("ChameleonRT",
                                          SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED,
                                          win_width,
                                          win_height,
                                          window_flags);

    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_Init(window);

    render_plugin->set_imgui_context(ImGui::GetCurrentContext());
    {
        std::unique_ptr<Display> display = render_plugin->make_display(window);
        run_app(args, window, display.get(), render_plugin.get());
    }

    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

void run_app(const std::vector<std::string> &args,
             SDL_Window *window,
             Display *display,
             RenderPlugin *render_plugin)
{
    ImGuiIO &io = ImGui::GetIO();

    std::string scene_file;
    bool got_camera_args = false;
    glm::vec3 eye(0, 0, 5);
    glm::vec3 center(0);
    glm::vec3 up(0, 1, 0);
    float fov_y = 65.f;
    size_t camera_id = 0;
    std::string validation_img_prefix;
    std::string oidn_interop_mode_arg;
    std::string scene_report_path;
    std::string profiling_output_base;
    size_t benchmark_frames = 0;
    size_t profiling_fps = 0;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-eye") {
            eye.x = std::stof(args[++i]);
            eye.y = std::stof(args[++i]);
            eye.z = std::stof(args[++i]);
            got_camera_args = true;
        } else if (args[i] == "-center") {
            center.x = std::stof(args[++i]);
            center.y = std::stof(args[++i]);
            center.z = std::stof(args[++i]);
            got_camera_args = true;
        } else if (args[i] == "-up") {
            up.x = std::stof(args[++i]);
            up.y = std::stof(args[++i]);
            up.z = std::stof(args[++i]);
            got_camera_args = true;
        } else if (args[i] == "-fov") {
            fov_y = std::stof(args[++i]);
            got_camera_args = true;
        } else if (args[i] == "-camera") {
            camera_id = std::stol(args[++i]);
        } else if (args[i] == "-validation") {
            validation_img_prefix = args[++i];
        } else if (args[i] == "-img") {
            i += 2;
        } else if (args[i] == "-oidn-interop") {
            oidn_interop_mode_arg = args[++i];
        } else if (args[i] == "-scene-report") {
            scene_report_path = args[++i];
        } else if (args[i] == "-profiling") {
            profiling_output_base = args[++i];
        } else if (args[i] == "-profiling-fps") {
            profiling_fps = std::stoul(args[++i]);
        } else if (args[i] == "-benchmark-frames") {
            benchmark_frames = std::stoul(args[++i]);
        } else if (args[i][0] != '-') {
            scene_file = args[i];
            canonicalize_path(scene_file);
        }
    }

    std::unique_ptr<RenderBackend> renderer = render_plugin->make_renderer(display);

    if (!renderer) {
        std::cout << "Error: No renderer backend or invalid backend name specified\n" << USAGE;
        std::exit(1);
    }
    if (scene_file.empty()) {
        std::cout << "Error: No model file specified\n" << USAGE;
        std::exit(1);
    }

#ifdef ENABLE_OIDN
    std::vector<std::string> supported_oidn_modes =
        renderer->get_supported_oidn_interop_modes();
    if (!oidn_interop_mode_arg.empty()) {
        const auto oidn_mode_it = std::find(supported_oidn_modes.begin(),
                                            supported_oidn_modes.end(),
                                            oidn_interop_mode_arg);
        if (oidn_mode_it == supported_oidn_modes.end()) {
            std::cerr << "Error: Invalid OIDN interop mode '" << oidn_interop_mode_arg << "'";
            if (!supported_oidn_modes.empty()) {
                std::cerr << ". Supported modes:";
                for (const std::string &mode : supported_oidn_modes) {
                    std::cerr << " " << mode;
                }
            }
            std::cerr << "\n";
            std::exit(1);
        }
        if (!renderer->set_oidn_interop_mode(oidn_interop_mode_arg)) {
            std::cerr << "Error: Failed to set OIDN interop mode '" << oidn_interop_mode_arg
                      << "'\n";
            std::exit(1);
        }
    }
#else
    if (!oidn_interop_mode_arg.empty()) {
        std::cerr << "Error: OIDN interop mode was specified, but OIDN support is disabled\n";
        std::exit(1);
    }
#endif

    display->resize(win_width, win_height);
    renderer->initialize(win_width, win_height);

    std::string scene_info;
    {
        Scene scene(scene_file);

        std::stringstream ss;
        ss << "Scene '" << scene_file << "':\n"
           << "# Unique Triangles: " << pretty_print_count(scene.unique_tris()) << "\n"
           << "# Total Triangles: " << pretty_print_count(scene.total_tris()) << "\n"
           << "# Geometries: " << scene.num_geometries() << "\n"
           << "# Meshes: " << scene.meshes.size() << "\n"
           << "# Parameterized Meshes: " << scene.parameterized_meshes.size() << "\n"
           << "# Instances: " << scene.instances.size() << "\n"
           << "# Materials: " << scene.materials.size() << "\n"
           << "# Textures: " << scene.textures.size() << "\n"
           << "# Lights: " << scene.lights.size() << "\n"
           << "# Cameras: " << scene.cameras.size();

        scene_info = ss.str();
        std::cout << scene_info << "\n";

        if (!scene_report_path.empty()) {
            SceneReport report;
            report.unique_tris = scene.unique_tris();
            report.total_tris = scene.total_tris();
            report.num_param_meshes = scene.parameterized_meshes.size();
            report.num_instances = scene.instances.size();
            report.num_lod_groups = 0;  // ChameleonRT has no LOD concept
            if (write_scene_report(scene_report_path, report)) {
                std::cout << "Scene report written to " << scene_report_path << ".json\n";
            }
        }

        renderer->set_scene(scene);

        if (!got_camera_args && !scene.cameras.empty()) {
            eye = scene.cameras[camera_id].position;
            center = scene.cameras[camera_id].center;
            up = scene.cameras[camera_id].up;
            fov_y = scene.cameras[camera_id].fov_y;
        }
    }

    ArcballCamera camera(eye, center, up);

    const std::string rt_backend = renderer->name();
#ifdef ENABLE_OIDN
    std::string oidn_interop_mode = renderer->get_oidn_interop_mode();
#endif
    const std::string cpu_brand = get_cpu_brand();
    const std::string gpu_brand = display->gpu_brand();
    const std::string image_output = "chameleonrt.png";
    const std::string display_frontend = display->name();

    const size_t stats_window = 120;
    std::array<RenderStats, 120> stats_history;
    size_t stats_history_count = 0;
    size_t stats_history_index = 0;
    size_t frame_id = 0;
    float render_time = 0.f;
    float frame_time = 0.f;
    float denoise_time = 0.f;
    float tonemap_time = 0.f;
    float rays_per_second = 0.f;
    glm::vec2 prev_mouse(-2.f);
    bool done = false;
    // A profiling run is a bounded, non-interactive benchmark. It is requested
    // either explicitly via -benchmark-frames or implicitly via -profiling (in
    // which case a default frame budget is used, optionally scaled by
    // -profiling-fps).
    const bool profiling_active = !profiling_output_base.empty();
    if (profiling_active && benchmark_frames == 0) {
        const size_t default_profiling_frames = 200;
        benchmark_frames =
            profiling_fps > 1 ? profiling_fps * default_profiling_frames
                              : default_profiling_frames;
    }
    // When a frame budget is set the run is a non-interactive benchmark: camera
    // input is frozen for determinism and the loop exits after the budget.
    const bool benchmark_active = benchmark_frames > 0;
    // CSV/JSON recorder. Only created when -profiling is requested; otherwise the
    // pointer stays null and the per-frame record() call is skipped.
    std::unique_ptr<BenchmarkRecorder> benchmark_recorder;
    if (profiling_active) {
        benchmark_recorder.reset(new BenchmarkRecorder(profiling_output_base));
    }
    bool camera_changed = true;
    bool save_image = false;
    bool resizing = false;
    bool show_timeline = true;
    bool average_timeline = false;
    bool show_ray_stats = true;
    // Whether the active backend can produce ray statistics at all. When it
    // cannot, the ray stats controls are hidden and collection stays disabled.
    const bool ray_stats_supported = renderer->supports_ray_stats();
    if (!ray_stats_supported) {
        show_ray_stats = false;
    }
    if (benchmark_active) {
        std::cout << "Benchmark mode: rendering " << benchmark_frames
                  << " frames then exiting\n";
    }
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                done = true;
            }
            if (!io.WantCaptureKeyboard && event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    done = true;
                } else if (event.key.keysym.sym == SDLK_p) {
                    auto eye = camera.eye();
                    auto center = camera.center();
                    auto up = camera.up();
                    std::cout << "-eye " << eye.x << " " << eye.y << " " << eye.z
                              << " -center " << center.x << " " << center.y << " " << center.z
                              << " -up " << up.x << " " << up.y << " " << up.z << " -fov "
                              << fov_y << "\n";
                } else if (event.key.keysym.sym == SDLK_s) {
                    save_image = true;
                }
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                done = true;
            }
            if (!io.WantCaptureMouse && !benchmark_active) {
                if (event.type == SDL_MOUSEMOTION) {
                    const glm::vec2 cur_mouse =
                        transform_mouse(glm::vec2(event.motion.x, event.motion.y));
                    if (prev_mouse != glm::vec2(-2.f)) {
                        if (event.motion.state & SDL_BUTTON_LMASK) {
                            camera.rotate(prev_mouse, cur_mouse);
                            camera_changed = true;
                        } else if (event.motion.state & SDL_BUTTON_RMASK) {
                            camera.pan(cur_mouse - prev_mouse);
                            camera_changed = true;
                        }
                    }
                    prev_mouse = cur_mouse;
                } else if (event.type == SDL_MOUSEWHEEL) {
                    camera.zoom(event.wheel.y * 0.1);
                    camera_changed = true;
                }
            }
            if (event.type == SDL_WINDOWEVENT) {

            }
            if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                frame_id = 0;
                win_width = event.window.data1;
                win_height = event.window.data2;
                io.DisplaySize.x = win_width;
                io.DisplaySize.y = win_height;
                display->resize(win_width, win_height);
                renderer->initialize(win_width, win_height);
                resizing = true;
            }
        }

        if (camera_changed) {
            frame_id = 0;
        }

        const bool need_readback = save_image || !validation_img_prefix.empty();
        if (renderer->collect_timeline != show_timeline) {
            std::cout << "Frame timeline collection "
                      << (show_timeline ? "enabled" : "disabled") << "\n";
        }
        renderer->collect_timeline = show_timeline;
        if (ray_stats_supported) {
            if (renderer->collect_ray_stats != show_ray_stats) {
                std::cout << "Ray stats collection "
                          << (show_ray_stats ? "enabled" : "disabled") << "\n";
            }
            renderer->collect_ray_stats = show_ray_stats;
        }
        RenderStats stats = renderer->render(
            camera.eye(), camera.dir(), camera.up(), fov_y, camera_changed, need_readback);

        ++frame_id;
        camera_changed = false;

        if (benchmark_active && frame_id >= benchmark_frames) {
            done = true;
        }

        if (save_image) {
            save_image = false;
            std::cout << "Image saved to " << image_output << "\n";
            stbi_write_png(image_output.c_str(),
                           win_width,
                           win_height,
                           4,
                           renderer->img.data(),
                           4 * win_width);
        }
        if (!validation_img_prefix.empty()) {
            const std::string img_name = validation_img_prefix + render_plugin->get_name() +
                                         "-f" + std::to_string(frame_id) + ".png";
            stbi_write_png(img_name.c_str(),
                           win_width,
                           win_height,
                           4,
                           renderer->img.data(),
                           4 * win_width);
        }

        if (frame_id == 1) {
            stats_history_count = 0;
            stats_history_index = 0;
            render_time = 0.f;
            frame_time = 0.f;
            denoise_time = 0.f;
            tonemap_time = 0.f;
            rays_per_second = 0.f;
        }

        if (stats_history_count == stats_window) {
            const RenderStats &old_stats = stats_history[stats_history_index];
            render_time -= old_stats.render_time;
            frame_time -= old_stats.frame_time;
            denoise_time -= old_stats.denoise_time;
            tonemap_time -= old_stats.tonemap_time;
            rays_per_second -= old_stats.rays_per_second;
        } else {
            ++stats_history_count;
        }

        stats_history[stats_history_index] = stats;
        stats_history_index = (stats_history_index + 1) % stats_window;

        render_time += stats.render_time;
        frame_time += stats.frame_time;
        denoise_time += stats.denoise_time;
        tonemap_time += stats.tonemap_time;
        rays_per_second += stats.rays_per_second;

        const float stats_sample_count = static_cast<float>(stats_history_count);
        const float avg_render_time = render_time / stats_sample_count;
        const float avg_frame_time = frame_time / stats_sample_count;
        const float avg_denoise_time = denoise_time / stats_sample_count;
        const float avg_tonemap_time = tonemap_time / stats_sample_count;
        const float avg_rays_per_second = rays_per_second / stats_sample_count;

        if (benchmark_recorder) {
            BenchmarkFrameStats frame_stats;
            frame_stats.render_time_ms = stats.render_time;
            // frame_time is the authoritative per-frame total, recorded as the
            // app_time_ms column.
            frame_stats.app_time_ms = stats.frame_time;
            frame_stats.denoise_time_ms = stats.denoise_time;
            frame_stats.tonemap_time_ms = stats.tonemap_time;
            frame_stats.rays_per_second = stats.rays_per_second;
            // frame_id resets to 0 on camera movement; in a frozen benchmark run
            // it is the progressive accumulation count.
            frame_stats.frames_accumulated = frame_id;
            benchmark_recorder->record(frame_stats);
        }

        display->new_frame();

        ImGui_ImplSDL2_NewFrame(window);
        ImGui::NewFrame();

        ImGui::Begin("Render Info");
        ImGui::Text("Total Application Time: %.3f ms/frame (%.1f FPS)",
                    1000.0f / ImGui::GetIO().Framerate,
                    ImGui::GetIO().Framerate);

        ImGui::Text("RT Backend: %s", rt_backend.c_str());
        ImGui::Text("CPU: %s", cpu_brand.c_str());
        ImGui::Text("GPU: %s", gpu_brand.c_str());
        ImGui::Text("Accumulated Frames: %zu", frame_id);
        ImGui::Text("Display Frontend: %s (%dx%d)",
                    display_frontend.c_str(),
                    win_width,
                    win_height);
    #ifdef ENABLE_OIDN
        ImGui::Text("Denoiser: Intel(R) Open Image Denoise");
    #endif
        ImGui::Text("%s", scene_info.c_str());

        if (ImGui::Button("Save Image")) {
            save_image = true;
        }
        ImGui::End();

        ImGui::Begin("Profiling");

        // Frame timeline: one horizontal bar per span, placed by its offset from
        // the frame begin and scaled to the GPU frame time. Overlapping async
        // passes appear on separate rows. The spans are only collected while the
        // timeline is shown, so the checkbox lives outside the data check.
        ImGui::Checkbox("Show Frame Timeline", &show_timeline);
        if (show_timeline) {
            ImGui::Checkbox("Average Timeline", &average_timeline);
        }

        const bool timeline_visible = show_timeline && !stats.timeline.empty();

        if (!timeline_visible) {
            // Textual per-pass timings. Hidden when the timeline is shown, since the
            // bars carry the same information (with the timing in each row's label).
            ImGui::Text("GPU Frame Time: %.3f ms", avg_frame_time);
            ImGui::Text("Render Time: %.3f ms", avg_render_time);
            if (avg_denoise_time > 0.f) {
                ImGui::Text("Denoise Time: %.3f ms", avg_denoise_time);
            }
            if (avg_tonemap_time > 0.f) {
                ImGui::Text("Tonemap Time: %.3f ms", avg_tonemap_time);
            }
            if (stats.passes_overlap) {
                // In async denoiser modes the render/denoise/tonemap passes run
                // concurrently on the device, so their times overlap and should not
                // be summed. GPU Frame Time is the authoritative end-to-end cost.
                ImGui::TextDisabled(
                    "(passes overlap on device; see GPU Frame Time for total cost)");
            } else {
                // GPU frame time not attributed to any of the finer scoped markers
                // (barriers, queue gaps, work between passes).
                const float avg_unscoped_time =
                    avg_frame_time - avg_render_time - avg_denoise_time - avg_tonemap_time;
                ImGui::Text("Unscoped GPU Time: %.3f ms", avg_unscoped_time);
            }
        }

        if (timeline_visible) {
            // Either show the most recent frame's spans, or the per-span average
            // over the history window (matched by name) for a steadier view.
            std::vector<RenderTimelineSpan> timeline = stats.timeline;
            float timeline_extent = stats.frame_time;
            if (average_timeline && stats_history_count > 0) {
                std::vector<float> sum_start(timeline.size(), 0.f);
                std::vector<float> sum_end(timeline.size(), 0.f);
                std::vector<size_t> counts(timeline.size(), 0);
                float sum_extent = 0.f;
                size_t extent_count = 0;
                for (size_t i = 0; i < stats_history_count; ++i) {
                    const RenderStats &h = stats_history[i];
                    if (h.timeline.empty()) {
                        // Skip frames captured while the timeline was disabled.
                        continue;
                    }
                    sum_extent += h.frame_time;
                    ++extent_count;
                    for (size_t s = 0; s < timeline.size(); ++s) {
                        // Match spans by name; backends emit them in a stable order.
                        for (const RenderTimelineSpan &hs : h.timeline) {
                            if (std::strcmp(hs.name, timeline[s].name) == 0) {
                                sum_start[s] += hs.start_ms;
                                sum_end[s] += hs.end_ms;
                                ++counts[s];
                                break;
                            }
                        }
                    }
                }
                for (size_t s = 0; s < timeline.size(); ++s) {
                    if (counts[s] > 0) {
                        timeline[s].start_ms = sum_start[s] / counts[s];
                        timeline[s].end_ms = sum_end[s] / counts[s];
                    }
                }
                if (extent_count > 0) {
                    timeline_extent = sum_extent / extent_count;
                }
            }

            if (timeline_extent > 0.f) {
                ImGui::Text("Frame Timeline (%.3f ms)", timeline_extent);

                // Build each row's "Name: X.XXX ms" label and size the name column
                // to the widest label so the bars line up in a separate column.
                std::vector<std::string> labels(timeline.size());
                float label_width = 0.0f;
                for (size_t s = 0; s < timeline.size(); ++s) {
                    const float duration = timeline[s].end_ms - timeline[s].start_ms;
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%s: %.3f ms",
                                  timeline[s].name, duration);
                    labels[s] = buf;
                    label_width = std::max(label_width, ImGui::CalcTextSize(buf).x);
                }
                label_width += 8.0f;  // padding between the label and bar columns

                ImDrawList *draw_list = ImGui::GetWindowDrawList();
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                const float total_width = ImGui::GetContentRegionAvail().x;
                const float bar_area = std::max(total_width - label_width, 1.0f);
                const float row_height = ImGui::GetTextLineHeight();
                const float row_spacing = row_height + 4.0f;
                const float scale = bar_area / timeline_extent;
                const float bar_origin_x = origin.x + label_width;

                int row = 0;
                for (size_t s = 0; s < timeline.size(); ++s) {
                    const RenderTimelineSpan &span = timeline[s];
                    const float y0 = origin.y + row * row_spacing;
                    const float y1 = y0 + row_height;

                    draw_list->AddText(
                        ImVec2(origin.x, y0), IM_COL32_WHITE, labels[s].c_str());

                    const float x0 = bar_origin_x + span.start_ms * scale;
                    const float x1 = bar_origin_x + span.end_ms * scale;
                    draw_list->AddRectFilled(
                        ImVec2(x0, y0), ImVec2(std::max(x1, x0 + 1.0f), y1),
                        timeline_color(span.name), 2.0f);
                    ++row;
                }

                ImGui::Dummy(ImVec2(total_width, row * row_spacing));
            }
        }

        if (stats.rays_per_second > 0) {
            const std::string rays_per_sec = pretty_print_count(avg_rays_per_second);
            ImGui::Text("Rays per-second: %sRay/s", rays_per_sec.c_str());
        }
        // Collecting ray stats requires a full-resolution readback and per-pixel
        // reduction each frame, so let the user disable it when not needed. Only
        // shown when the backend can actually produce ray statistics.
        if (ray_stats_supported) {
            ImGui::Checkbox("Collect Ray Stats", &show_ray_stats);
        }
        ImGui::End();

#ifdef ENABLE_OIDN
        ImGui::Begin("OIDN");
        // List supported interop modes and allow switching between them at runtime
        const auto current_it = std::find(supported_oidn_modes.begin(), supported_oidn_modes.end(), oidn_interop_mode);
        int current_idx = current_it != supported_oidn_modes.end() ? static_cast<int>(current_it - supported_oidn_modes.begin()) : 0;
        ImGui::Text("Interop Mode");
        if (ImGui::BeginCombo("##interop_mode", oidn_interop_mode.c_str())) {
            for (int i = 0; i < static_cast<int>(supported_oidn_modes.size()); ++i) {
                const bool selected = (i == current_idx);
                if (ImGui::Selectable(supported_oidn_modes[i].c_str(), selected)) {
                    oidn_interop_mode = supported_oidn_modes[i];
                    renderer->set_oidn_interop_mode(oidn_interop_mode);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::End();
#endif

        ImGui::Render();

        if (!resizing)
            display->display(renderer.get());
        resizing = false;
    }

    if (benchmark_recorder) {
        BenchmarkEnvironment env;
        env.cpu_brand = cpu_brand;
        env.gpu_brand = gpu_brand;
        env.display_frontend = display_frontend;
        env.rt_backend = rt_backend;
        env.cmdline = args;
        env.render_width = win_width;
        env.render_height = win_height;
        env.display_width = win_width;
        env.display_height = win_height;
        benchmark_recorder->finish(env);
        std::cout << "Benchmark data written to " << profiling_output_base << ".csv and "
                  << profiling_output_base << ".json\n";
    }
}
