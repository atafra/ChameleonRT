#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "render_backend.h"

// GPU identity reported by the active display backend.
struct GpuInfo {
    std::string driver;  // Existing API/driver-provided device string
    std::string name;    // Human-readable physical GPU name
    std::string driver_version;
};

GpuInfo make_gpu_info(const std::string &driver, uint32_t vendor_id, uint32_t device_id);

struct Display {
    virtual ~Display() {}

    virtual std::string gpu_brand() = 0;

    // Defaults both fields to the existing driver-provided string; backends with
    // access to a more specific physical GPU name can override this.
    virtual GpuInfo gpu_info()
    {
        const std::string gpu = gpu_brand();
        return GpuInfo{gpu, gpu, std::string()};
    }

    virtual std::string name() = 0;

    virtual void resize(const int fb_width, const int fb_height) = 0;

    virtual void new_frame() = 0;

    virtual void display(RenderBackend *renderer) = 0;
};
