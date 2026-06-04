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

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    Microsoft::WRL::ComPtr<ID3D12Device5> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> cmd_queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> cmd_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cmd_list;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> render_cmd_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> render_cmd_list, tonemap_cmd_list, readback_cmd_list;

    dxr::Buffer view_param_buf, img_readback_buf, instance_buf, material_param_buf, light_buf,
        ray_stats_readback_buf;

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
    bool native_display = false;

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
    bool frame_diagnostics_active = false;
#endif

    // Query pool to measure GPU frame stage timings
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timing_query_heap;
    dxr::Buffer query_resolve_buffer;

#ifdef ENABLE_OIDN
    dxr::Buffer denoise_buffer;
    oidn::DeviceRef oidn_device;
    oidn::FilterRef oidn_filter;
    oidn::SemaphoreRef oidn_semaphore;
    OIDNInteropMode oidn_interop_mode = OIDNInteropMode::HostBlocking;
    bool oidn_interop_mode_initialized = false;
    bool oidn_device_async_supported = true;
#endif

#ifdef REPORT_RAY_STATS
    std::vector<uint16_t> ray_counts;
#endif

    RenderDXR(Microsoft::WRL::ComPtr<ID3D12Device5> device);

    RenderDXR();

    virtual ~RenderDXR();

    std::string name() override;

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

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
    bool frame_diagnostics_enabled() const;
    void log_frame_diagnostic(const std::string &event) const;
#endif

    void sync_gpu();
};
