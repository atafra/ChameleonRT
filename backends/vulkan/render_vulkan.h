#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <vulkan/vulkan.h>
#ifdef ENABLE_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif
#include "render_backend.h"
#include "vulkan_utils.h"
#include "vulkanrt_utils.h"

struct HitGroupParams {
    uint64_t vert_buf = 0;
    uint64_t idx_buf = 0;
    uint64_t normal_buf = 0;
    uint64_t uv_buf = 0;
    uint32_t num_normals = 0;
    uint32_t num_uvs = 0;
    uint32_t material_id = 0;
};

struct RenderVulkan : RenderBackend {

    enum class OIDNInteropMode {
        HostBlocking,
        TimelineSemaphore,
        BinarySemaphore };

    // Number of frames whose GPU work / timing queries may be in flight at once.
    // Double-buffering the per-frame resources lets us read back statistics from
    // a previous frame without blocking the host on the work just submitted.
    static const uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    std::shared_ptr<vkrt::Device> device;

    std::shared_ptr<vkrt::Buffer> view_param_buf, img_readback_buf, mat_params, light_params;

    std::shared_ptr<vkrt::Texture2D> render_target;
    std::shared_ptr<vkrt::Buffer> accum_buffer;

#ifdef ENABLE_OIDN
    std::shared_ptr<vkrt::Buffer> denoise_buffer;
    oidn::DeviceRef oidn_device;
    oidn::FilterRef oidn_filter;
    OIDNInteropMode oidn_interop_mode = OIDNInteropMode::BinarySemaphore;
    bool oidn_interop_mode_initialized = false;
#endif

#ifdef REPORT_RAY_STATS
    std::shared_ptr<vkrt::Texture2D> ray_stats;
    std::shared_ptr<vkrt::Buffer> ray_stats_readback_buf;
    std::vector<uint16_t> ray_counts;
#endif

    std::vector<std::unique_ptr<vkrt::TriangleMesh>> meshes;
    std::vector<ParameterizedMesh> parameterized_meshes;
    std::unique_ptr<vkrt::TopLevelBVH> scene_bvh;
    size_t total_geom = 0;

    std::vector<std::shared_ptr<vkrt::Texture2D>> textures;
    VkSampler sampler = VK_NULL_HANDLE;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;

    VkCommandPool render_cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer render_cmd_buf[MAX_FRAMES_IN_FLIGHT] = {};
    VkCommandBuffer tonemap_cmd_buf[MAX_FRAMES_IN_FLIGHT] = {};
    VkCommandBuffer readback_cmd_buf[MAX_FRAMES_IN_FLIGHT] = {};

    vkrt::RTPipeline rt_pipeline;
    VkPipeline tonemap_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout textures_desc_layout = VK_NULL_HANDLE;

    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    // We need a set per varying size array of things we're sending
    VkDescriptorSet desc_set = VK_NULL_HANDLE;
    VkDescriptorSet textures_desc_set = VK_NULL_HANDLE;

    vkrt::ShaderBindingTable shader_table;

    // One fence per in-flight frame slot. It is signaled by the final submission
    // of that slot's frame and guards reuse of the slot's command buffers and
    // timestamp queries.
    VkFence fence[MAX_FRAMES_IN_FLIGHT] = {};
    // Whether a given slot has been submitted at least once (so its query
    // results are valid to read back).
    bool slot_submitted[MAX_FRAMES_IN_FLIGHT] = {};
#ifdef REPORT_RAY_STATS
    uint64_t slot_total_rays[MAX_FRAMES_IN_FLIGHT] = {};
#endif

#ifdef ENABLE_OIDN
    VkSemaphore timeline_semaphore = VK_NULL_HANDLE;
    oidn::SemaphoreRef oidn_timeline_semaphore;
    VkSemaphore render_ready_semaphore = VK_NULL_HANDLE;
    VkSemaphore oidn_ready_semaphore = VK_NULL_HANDLE;
    oidn::SemaphoreRef oidn_wait_semaphore; // wait for render ready
    oidn::SemaphoreRef oidn_signal_semaphore; // signal OIDN ready
    uint64_t timeline_render_wait_value = 0;
    uint64_t timeline_render_signal_value = 1;
    uint64_t timeline_oidn_wait_value = 1;
    uint64_t timeline_oidn_signal_value = 2;
    uint64_t timeline_tonemap_wait_value = 2;
    uint64_t timeline_tonemap_signal_value = 3;
#endif

    VkQueryPool timing_query_pool;

    size_t frame_id = 0;
    // Index of the in-flight frame slot used for the next render() call.
    uint32_t frame_slot = 0;
    bool native_display = false;

    RenderVulkan(std::shared_ptr<vkrt::Device> device);

    RenderVulkan();

    virtual ~RenderVulkan();

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
    void build_raytracing_pipeline();

    void build_shader_descriptor_table();

    void build_shader_binding_table();

    void update_view_parameters(const glm::vec3 &pos,
                                const glm::vec3 &dir,
                                const glm::vec3 &up,
                                const float fovy);

    void record_command_buffers();
};
