#pragma once

#include <vector>
#include "scene.h"
#include <glm/glm.hpp>

// A single named interval of the frame, expressed in milliseconds relative to
// the frame's begin timestamp. Overlapping spans (e.g. async OIDN interop) are
// allowed and simply share time ranges.
struct RenderTimelineSpan {
    const char *name = "";
    float start_ms = 0;
    float end_ms = 0;
};

struct RenderStats {
    float render_time = 0;
    float frame_time = 0;
    float denoise_time = 0;
    float tonemap_time = 0;
    float rays_per_second = 0;
    // When true, the render/denoise/tonemap passes run concurrently on the device
    // (e.g. async OIDN interop), so their individual times overlap and must not be
    // summed as disjoint intervals. frame_time remains the authoritative total.
    bool passes_overlap = false;
    // Ordered spans relative to frame begin, used to visualize the frame timeline.
    // Empty if the backend does not provide timeline data.
    std::vector<RenderTimelineSpan> timeline;
};

struct RenderBackend {
    std::vector<uint32_t> img;

    // When true, the backend populates RenderStats::timeline with per-pass spans.
    // The app toggles this so timeline data is only gathered while it is displayed.
    bool collect_timeline = false;

    virtual ~RenderBackend() {}

    virtual std::string name() = 0;

    #ifdef ENABLE_OIDN
    virtual std::string get_oidn_interop_mode()
    {
        return "Undefined";
    }

    virtual bool set_oidn_interop_mode(const std::string& mode)
    {
        return false;
    }

    virtual std::vector<std::string> get_supported_oidn_interop_modes()
    {
        return {};
    }
    #endif

    virtual void initialize(const int fb_width, const int fb_height) = 0;

    // TODO Probably should take the scene through a shared_ptr
    virtual void set_scene(const Scene &scene) = 0;

    // Returns the rays per-second achieved, or -1 if this is not tracked
    virtual RenderStats render(const glm::vec3 &pos,
                               const glm::vec3 &dir,
                               const glm::vec3 &up,
                               const float fovy,
                               const bool camera_changed,
                               const bool readback_framebuffer) = 0;
};
