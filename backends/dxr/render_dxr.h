#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#ifdef ENABLE_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif
#include "dx12_utils.h"
#include "dxr_utils.h"
#include "render_backend.h"

//#define ENABLE_DXR_FRAME_DIAGNOSTICS

struct RenderDXR : RenderBackend {
    enum class OIDNInteropMode {
        HostBlocking,
        DeviceAsync };

    // Number of frames whose GPU work / timing queries may be in flight at once.
    // Double-buffering the per-frame command lists, timing queries and readback
    // buffers lets a future change read back statistics from a previous frame
    // without blocking the host on the work just submitted.
    static const uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    Microsoft::WRL::ComPtr<ID3D12Device5> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> cmd_queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> cmd_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cmd_list;

    // Per in-flight frame slot command allocators and lists.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> render_cmd_allocator[MAX_FRAMES_IN_FLIGHT];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> render_cmd_list[MAX_FRAMES_IN_FLIGHT],
        tonemap_cmd_list[MAX_FRAMES_IN_FLIGHT], readback_cmd_list[MAX_FRAMES_IN_FLIGHT];

    dxr::Buffer view_param_buf, instance_buf, material_param_buf, light_buf;

    // The view parameter upload buffer is written once per frame, so it is kept
    // persistently mapped for the lifetime of the resource to avoid the per-frame
    // Map/Unmap overhead.
    uint8_t *view_param_mapping = nullptr;

    // Per-slot framebuffer / ray-stats readback buffers.
    dxr::Buffer img_readback_buf[MAX_FRAMES_IN_FLIGHT];
    dxr::Buffer ray_stats_readback_buf[MAX_FRAMES_IN_FLIGHT];

    dxr::Texture2D render_target, ray_stats;
    dxr::Buffer accum_buffer;
    std::vector<dxr::Texture2D> textures;

    std::vector<dxr::BottomLevelBVH> meshes;
    dxr::TopLevelBVH scene_bvh;

    std::vector<ParameterizedMesh> parameterized_meshes;

    dxr::RTPipeline rt_pipeline;
    dxr::DescriptorHeap raygen_desc_heap, raygen_sampler_heap;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> tonemap_ps;
    dxr::RootSignature tonemap_root_sig;

    uint64_t fence_value = 1;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE fence_evt;

    uint32_t frame_id = 0;
    // Index of the in-flight frame slot used for the next render() call.
    uint32_t frame_slot = 0;
    // Whether a given slot has been submitted at least once (so its readback
    // buffers and timestamp queries are valid to read back).
    bool slot_submitted[MAX_FRAMES_IN_FLIGHT] = {};
    // Fence value signaled once a slot's frame work has fully completed on the GPU.
    // Used to reclaim the slot and to gate reading back its timing/ray statistics.
    uint64_t slot_fence_value[MAX_FRAMES_IN_FLIGHT] = {};
    bool native_display = false;

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
    bool frame_diagnostics_active = false;
#endif

    // Query pool to measure GPU frame stage timings. The heap holds one set of
    // queries per in-flight frame slot.
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timing_query_heap;
    dxr::Buffer query_resolve_buffer[MAX_FRAMES_IN_FLIGHT];

    // The timestamp frequency is a fixed property of the command queue, so it is
    // queried once and cached instead of per-frame.
    uint64_t timestamp_freq = 0;

    // Readback buffers are kept persistently mapped for the lifetime of the
    // resource to avoid the per-frame Map/Unmap overhead.
    const uint64_t *query_resolve_mapping[MAX_FRAMES_IN_FLIGHT] = {};
    uint8_t *img_readback_mapping[MAX_FRAMES_IN_FLIGHT] = {};
#ifdef REPORT_RAY_STATS
    uint8_t *ray_stats_readback_mapping[MAX_FRAMES_IN_FLIGHT] = {};
#endif

#ifdef ENABLE_OIDN
    dxr::Buffer denoise_buffer;
    oidn::DeviceRef oidn_device;
    oidn::FilterRef oidn_filter;
    oidn::SemaphoreRef oidn_semaphore;
    // Dedicated fence used exclusively for OIDN<->SYCL semaphore sharing, kept
    // separate from the CPU<->GPU handshake fence (fence/fence_value) so the host
    // and the SYCL context do not contend on a single fence timeline.
    Microsoft::WRL::ComPtr<ID3D12Fence> oidn_fence;
    uint64_t oidn_fence_value = 1;
    OIDNInteropMode oidn_interop_mode = OIDNInteropMode::HostBlocking;
    bool oidn_interop_mode_initialized = false;
    bool oidn_device_async_supported = true;
#endif

#ifdef REPORT_RAY_STATS
    std::vector<uint16_t> ray_counts;
    // Total rays for each in-flight slot, read back lagged by one frame.
    uint64_t slot_total_rays[MAX_FRAMES_IN_FLIGHT] = {};
#endif

    RenderDXR(Microsoft::WRL::ComPtr<ID3D12Device5> device);

    RenderDXR();

    virtual ~RenderDXR();

    std::string name() override;

    bool supports_ray_stats() const override;

    #ifdef ENABLE_OIDN
    std::string get_oidn_interop_mode() override;
    bool set_oidn_interop_mode(const std::string &mode) override;
    std::vector<std::string> get_supported_oidn_interop_modes() override;
    #endif

    void initialize(const int fb_width, const int fb_height) override;

    void set_scene(const Scene &scene) override;

    RenderStats render(const glm::vec3 &pos,
                       const glm::vec3 &dir,
                       const glm::vec3 &up,
                       const float fovy,
                       const bool camera_changed,
                       const bool readback_framebuffer) override;

private:
    void create_device_objects();

    void build_raytracing_pipeline();

    void build_shader_resource_heap();

    void build_shader_binding_table();

    void update_view_parameters(const glm::vec3 &pos,
                                const glm::vec3 &dir,
                                const glm::vec3 &up,
                                const float fovy);

    void build_descriptor_heap();

    void record_command_lists();

    // Record the command lists for a single in-flight frame slot.
    void record_command_lists_for_slot(uint32_t slot);

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
    bool frame_diagnostics_enabled() const;
    void log_frame_diagnostic(const std::string &event) const;
#endif

    void sync_gpu();

    // Block the host until the GPU has signaled the fence with at least the given
    // value, without otherwise advancing the fence. Used to reclaim an in-flight
    // frame slot and to gate reading back its statistics.
    void wait_for_fence_value(uint64_t value);
};
