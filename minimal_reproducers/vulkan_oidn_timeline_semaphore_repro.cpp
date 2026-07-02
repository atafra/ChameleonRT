// Minimal Vulkan/OIDN timeline semaphore reproducer for RenderVulkan-style hangs.
//
// This intentionally mirrors the synchronization/submission ordering in
// backends/vulkan/render_vulkan.cpp for OIDNInteropMode::TimelineSemaphore:
//
//   Vulkan queue: render_cmd_buf submission signals timeline semaphore value N
//   OIDN/SYCL:    waitSemaphoreAsync(sem, N)
//                 executeAsync()
//                 signalSemaphoreAsync(sem, N+1)
//   Vulkan queue: tonemap_cmd_buf submission waits timeline semaphore value N+1
//                 and signals a CPU-visible VkFence
//   CPU:          waits for the VkFence with a timeout
//
// The command buffers contain only timestamp writes and the same external
// queue-family release/acquire barriers used by RenderVulkan around OIDN.  The
// OIDN filter is real and uses externally exported Vulkan buffers, so the
// semaphore path exercises the same Vulkan <-> OIDN/SYCL interop mechanism as
// the backend without needing a scene, ray tracing pipeline, or shaders.
//
// Windows build example:
//   cd minimal_reproducers
//   cl /std:c++14 /EHsc /I%VULKAN_SDK%\Include /I<OIDN include dir> ^
//      vulkan_oidn_timeline_semaphore_repro.cpp ^
//      /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib OpenImageDenoise.lib
//
// Linux build example:
//   cd minimal_reproducers
//   c++ -std=c++14 -O2 vulkan_oidn_timeline_semaphore_repro.cpp \
//       -I$VULKAN_SDK/include -lOpenImageDenoise -lvulkan -o repro

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <OpenImageDenoise/oidn.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

static const uint32_t kWidth = 64;
static const uint32_t kHeight = 64;
static const uint64_t kWaitTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;
static const uint32_t kMaxFramesInFlight = 2;

// Match RenderVulkan's timing query layout exactly.
enum TimingQuery {
    TIMING_QUERY_FRAME_BEGIN = 0,
    TIMING_QUERY_RAYTRACING_BEGIN,
    TIMING_QUERY_RAYTRACING_END,
    TIMING_QUERY_DENOISE_BEGIN,
    TIMING_QUERY_TONEMAP_BEGIN,
    TIMING_QUERY_FRAME_END,
    TIMING_QUERY_COUNT
};

#ifdef _WIN32
static const VkExternalSemaphoreHandleTypeFlagBits kExternalSemaphoreHandleType =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
static const VkExternalMemoryHandleTypeFlagBits kExternalMemoryHandleType =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
static const VkExternalSemaphoreHandleTypeFlagBits kExternalSemaphoreHandleType =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
static const VkExternalMemoryHandleTypeFlagBits kExternalMemoryHandleType =
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

const char *vk_result_string(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    default: return "<unknown VkResult>";
    }
}

void vk_check(VkResult r, const char *expr)
{
    if (r != VK_SUCCESS) {
        std::ostringstream out;
        out << expr << " failed: " << vk_result_string(r) << " (" << int(r) << ")";
        throw std::runtime_error(out.str());
    }
}

#define VK_CHECK(expr) vk_check((expr), #expr)

void log_step(const std::string &msg)
{
    std::cout << msg << std::endl;
}

template <typename Fn>
void timed_step(const std::string &label, Fn fn)
{
    std::cout << label << " begin" << std::endl;
    const auto begin = std::chrono::high_resolution_clock::now();
    fn();
    const auto end = std::chrono::high_resolution_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(end - begin).count();
    std::cout << label << " end; cpu_ms=" << ms << std::endl;
}

void check_oidn_error(oidn::DeviceRef &device, const char *message)
{
    if (device.getError() != oidn::Error::None) {
        throw std::runtime_error(message);
    }
}

bool has_extension(const std::vector<VkExtensionProperties> &extensions,
                   const char *name)
{
    for (size_t i = 0; i < extensions.size(); ++i) {
        if (std::strcmp(extensions[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

uint32_t find_memory_type(VkPhysicalDevice physical_device,
                          uint32_t type_filter,
                          VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find a compatible Vulkan memory type");
}

struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
};

struct ExternalBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    size_t size = 0;
};

VulkanContext create_vulkan_context()
{
    VulkanContext ctx;

    log_step("[Vulkan setup] vkCreateInstance");
    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "RenderVulkan OIDN timeline semaphore reproducer";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "standalone repro";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instance_ci = {};
    instance_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_ci.pApplicationInfo = &app_info;
    VK_CHECK(vkCreateInstance(&instance_ci, nullptr, &ctx.instance));

    uint32_t device_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(ctx.instance, &device_count, nullptr));
    if (device_count == 0) {
        throw std::runtime_error("No Vulkan physical devices found");
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    VK_CHECK(vkEnumeratePhysicalDevices(ctx.instance, &device_count, devices.data()));

    const std::array<const char *, 6> required_extensions = {{
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
#ifdef _WIN32
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#else
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
#endif
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
#ifdef _WIN32
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
#else
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
#endif
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME
    }};

    for (size_t d = 0; d < devices.size(); ++d) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[d], &props);
        std::cout << "[Vulkan setup] considering device " << d << ": "
                  << props.deviceName << std::endl;

        uint32_t ext_count = 0;
        VK_CHECK(vkEnumerateDeviceExtensionProperties(devices[d], nullptr, &ext_count, nullptr));
        std::vector<VkExtensionProperties> extensions(ext_count);
        VK_CHECK(vkEnumerateDeviceExtensionProperties(
            devices[d], nullptr, &ext_count, extensions.data()));

        bool extensions_ok = true;
        for (size_t i = 0; i < required_extensions.size(); ++i) {
            if (!has_extension(extensions, required_extensions[i])) {
                std::cout << "[Vulkan setup]   missing extension: "
                          << required_extensions[i] << std::endl;
                extensions_ok = false;
            }
        }
        if (!extensions_ok) {
            continue;
        }

        VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features = {};
        timeline_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        VkPhysicalDeviceFeatures2 features2 = {};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &timeline_features;
        vkGetPhysicalDeviceFeatures2(devices[d], &features2);
        if (!timeline_features.timelineSemaphore) {
            std::cout << "[Vulkan setup]   timelineSemaphore feature is not supported" << std::endl;
            continue;
        }

        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &queue_count, queues.data());

        for (uint32_t q = 0; q < queue_count; ++q) {
            if (queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                ctx.physical_device = devices[d];
                ctx.queue_family = q;
                std::cout << "[Vulkan setup] selected device: " << props.deviceName
                          << ", queue_family=" << q << std::endl;
                break;
            }
        }
        if (ctx.physical_device != VK_NULL_HANDLE) {
            break;
        }
    }

    if (ctx.physical_device == VK_NULL_HANDLE) {
        throw std::runtime_error("No Vulkan device supports the required timeline/external extensions");
    }

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci = {};
    queue_ci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_ci.queueFamilyIndex = ctx.queue_family;
    queue_ci.queueCount = 1;
    queue_ci.pQueuePriorities = &queue_priority;

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features = {};
    timeline_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline_features.timelineSemaphore = VK_TRUE;

    VkDeviceCreateInfo device_ci = {};
    device_ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_ci.pNext = &timeline_features;
    device_ci.queueCreateInfoCount = 1;
    device_ci.pQueueCreateInfos = &queue_ci;
    device_ci.enabledExtensionCount = static_cast<uint32_t>(required_extensions.size());
    device_ci.ppEnabledExtensionNames = required_extensions.data();

    log_step("[Vulkan setup] vkCreateDevice");
    VK_CHECK(vkCreateDevice(ctx.physical_device, &device_ci, nullptr, &ctx.device));
    vkGetDeviceQueue(ctx.device, ctx.queue_family, 0, &ctx.queue);

    return ctx;
}

ExternalBuffer create_external_buffer(const VulkanContext &ctx,
                                      size_t size,
                                      VkBufferUsageFlags usage)
{
    ExternalBuffer result;
    result.size = size;

    VkExternalMemoryBufferCreateInfo external_buffer_ci = {};
    external_buffer_ci.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external_buffer_ci.handleTypes = kExternalMemoryHandleType;

    VkBufferCreateInfo buffer_ci = {};
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.pNext = &external_buffer_ci;
    buffer_ci.size = size;
    buffer_ci.usage = usage;
    buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(ctx.device, &buffer_ci, nullptr, &result.buffer));

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(ctx.device, result.buffer, &requirements);

    VkExportMemoryAllocateInfo export_info = {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = kExternalMemoryHandleType;

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &export_info;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = find_memory_type(
        ctx.physical_device, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VK_CHECK(vkAllocateMemory(ctx.device, &allocate_info, nullptr, &result.memory));
    VK_CHECK(vkBindBufferMemory(ctx.device, result.buffer, result.memory, 0));

    return result;
}

void destroy_external_buffer(const VulkanContext &ctx, ExternalBuffer &buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(ctx.device, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(ctx.device, buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }
}

oidn::BufferRef import_oidn_buffer(const VulkanContext &ctx,
                                   oidn::DeviceRef &oidn_device,
                                   const ExternalBuffer &buffer,
                                   oidn::ExternalMemoryTypeFlag oidn_memory_type)
{
#ifdef _WIN32
    PFN_vkGetMemoryWin32HandleKHR get_memory_handle =
        reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
            vkGetDeviceProcAddr(ctx.device, "vkGetMemoryWin32HandleKHR"));
    if (!get_memory_handle) {
        throw std::runtime_error("Failed to load vkGetMemoryWin32HandleKHR");
    }

    HANDLE handle = nullptr;
    VkMemoryGetWin32HandleInfoKHR handle_info = {};
    handle_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    handle_info.memory = buffer.memory;
    handle_info.handleType = kExternalMemoryHandleType;
    VK_CHECK(get_memory_handle(ctx.device, &handle_info, &handle));

    oidn::BufferRef oidn_buffer =
        oidn_device.newBuffer(oidn_memory_type, handle, nullptr, buffer.size);
    CloseHandle(handle);
#else
    PFN_vkGetMemoryFdKHR get_memory_handle =
        reinterpret_cast<PFN_vkGetMemoryFdKHR>(
            vkGetDeviceProcAddr(ctx.device, "vkGetMemoryFdKHR"));
    if (!get_memory_handle) {
        throw std::runtime_error("Failed to load vkGetMemoryFdKHR");
    }

    int fd = -1;
    VkMemoryGetFdInfoKHR handle_info = {};
    handle_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    handle_info.memory = buffer.memory;
    handle_info.handleType = kExternalMemoryHandleType;
    VK_CHECK(get_memory_handle(ctx.device, &handle_info, &fd));

    oidn::BufferRef oidn_buffer = oidn_device.newBuffer(oidn_memory_type, fd, buffer.size);
    close(fd);
#endif
    check_oidn_error(oidn_device, "Failed to import Vulkan buffer into OIDN");
    return oidn_buffer;
}

VkSemaphore create_exportable_timeline_semaphore(const VulkanContext &ctx)
{
    VkSemaphoreTypeCreateInfo timeline_ci = {};
    timeline_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_ci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_ci.initialValue = 0;

    VkExportSemaphoreCreateInfo export_ci = {};
    export_ci.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    export_ci.pNext = &timeline_ci;
    export_ci.handleTypes = kExternalSemaphoreHandleType;

    VkSemaphoreCreateInfo semaphore_ci = {};
    semaphore_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore_ci.pNext = &export_ci;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSemaphore(ctx.device, &semaphore_ci, nullptr, &semaphore));
    return semaphore;
}

oidn::SemaphoreRef import_oidn_timeline_semaphore(const VulkanContext &ctx,
                                                  oidn::DeviceRef &oidn_device,
                                                  VkSemaphore semaphore)
{
#ifdef _WIN32
    PFN_vkGetSemaphoreWin32HandleKHR get_semaphore_handle =
        reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(ctx.device, "vkGetSemaphoreWin32HandleKHR"));
    if (!get_semaphore_handle) {
        throw std::runtime_error("Failed to load vkGetSemaphoreWin32HandleKHR");
    }

    HANDLE handle = nullptr;
    VkSemaphoreGetWin32HandleInfoKHR handle_info = {};
    handle_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
    handle_info.semaphore = semaphore;
    handle_info.handleType = kExternalSemaphoreHandleType;
    VK_CHECK(get_semaphore_handle(ctx.device, &handle_info, &handle));

    oidn::SemaphoreRef oidn_semaphore = oidn_device.newSemaphore(
        oidn::ExternalSemaphoreTypeFlag::TimelineSemaphoreWin32, handle, nullptr);
    CloseHandle(handle);
#else
    PFN_vkGetSemaphoreFdKHR get_semaphore_handle =
        reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
            vkGetDeviceProcAddr(ctx.device, "vkGetSemaphoreFdKHR"));
    if (!get_semaphore_handle) {
        throw std::runtime_error("Failed to load vkGetSemaphoreFdKHR");
    }

    int fd = -1;
    VkSemaphoreGetFdInfoKHR handle_info = {};
    handle_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    handle_info.semaphore = semaphore;
    handle_info.handleType = kExternalSemaphoreHandleType;
    VK_CHECK(get_semaphore_handle(ctx.device, &handle_info, &fd));

    oidn::SemaphoreRef oidn_semaphore = oidn_device.newSemaphore(
        oidn::ExternalSemaphoreTypeFlag::TimelineSemaphoreFD, fd);
    close(fd);
#endif
    check_oidn_error(oidn_device, "Failed to import Vulkan timeline semaphore into OIDN");
    return oidn_semaphore;
}

void record_render_command_buffer(const VulkanContext &ctx,
                                  VkCommandBuffer cmd,
                                  VkQueryPool query_pool,
                                  const ExternalBuffer &accum_buffer,
                                  const ExternalBuffer &denoise_buffer)
{
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

    vkCmdResetQueryPool(cmd, query_pool, 0, TIMING_QUERY_COUNT * kMaxFramesInFlight);

    vkCmdWriteTimestamp(cmd,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        query_pool,
                        TIMING_QUERY_FRAME_BEGIN);
    vkCmdWriteTimestamp(cmd,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        query_pool,
                        TIMING_QUERY_RAYTRACING_BEGIN);

    // The real backend traces rays here.  This repro deliberately has no shader
    // work; it keeps only the barriers and submission/semaphore behavior.

    vkCmdWriteTimestamp(cmd,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        query_pool,
                        TIMING_QUERY_RAYTRACING_END);

    std::array<VkBufferMemoryBarrier, 2> external_release_barriers;
    std::memset(external_release_barriers.data(), 0,
                external_release_barriers.size() * sizeof(VkBufferMemoryBarrier));

    external_release_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    external_release_barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    external_release_barriers[0].dstAccessMask = 0;
    external_release_barriers[0].srcQueueFamilyIndex = ctx.queue_family;
    external_release_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    external_release_barriers[0].buffer = accum_buffer.buffer;
    external_release_barriers[0].offset = 0;
    external_release_barriers[0].size = VK_WHOLE_SIZE;

    external_release_barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    external_release_barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    external_release_barriers[1].dstAccessMask = 0;
    external_release_barriers[1].srcQueueFamilyIndex = ctx.queue_family;
    external_release_barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    external_release_barriers[1].buffer = denoise_buffer.buffer;
    external_release_barriers[1].offset = 0;
    external_release_barriers[1].size = VK_WHOLE_SIZE;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0,
                         0, nullptr,
                         static_cast<uint32_t>(external_release_barriers.size()),
                         external_release_barriers.data(),
                         0, nullptr);

    vkCmdWriteTimestamp(cmd,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        query_pool,
                        TIMING_QUERY_DENOISE_BEGIN);

    VK_CHECK(vkEndCommandBuffer(cmd));
}

void record_tonemap_command_buffer(const VulkanContext &ctx,
                                   VkCommandBuffer cmd,
                                   VkQueryPool query_pool,
                                   const ExternalBuffer &accum_buffer,
                                   const ExternalBuffer &denoise_buffer)
{
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

    std::array<VkBufferMemoryBarrier, 2> external_acquire_barriers;
    std::memset(external_acquire_barriers.data(), 0,
                external_acquire_barriers.size() * sizeof(VkBufferMemoryBarrier));

    external_acquire_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    external_acquire_barriers[0].srcAccessMask = 0;
    external_acquire_barriers[0].dstAccessMask = 0;
    external_acquire_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    external_acquire_barriers[0].dstQueueFamilyIndex = ctx.queue_family;
    external_acquire_barriers[0].buffer = accum_buffer.buffer;
    external_acquire_barriers[0].offset = 0;
    external_acquire_barriers[0].size = VK_WHOLE_SIZE;

    external_acquire_barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    external_acquire_barriers[1].srcAccessMask = 0;
    external_acquire_barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    external_acquire_barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    external_acquire_barriers[1].dstQueueFamilyIndex = ctx.queue_family;
    external_acquire_barriers[1].buffer = denoise_buffer.buffer;
    external_acquire_barriers[1].offset = 0;
    external_acquire_barriers[1].size = VK_WHOLE_SIZE;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         0, nullptr,
                         static_cast<uint32_t>(external_acquire_barriers.size()),
                         external_acquire_barriers.data(),
                         0, nullptr);

    vkCmdWriteTimestamp(cmd,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        query_pool,
                        TIMING_QUERY_TONEMAP_BEGIN);

    // The real backend dispatches the tonemap compute shader here.  The hang this
    // repro targets occurs before any useful tonemap work can complete if the
    // timeline wait is never satisfied.

    vkCmdWriteTimestamp(cmd,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        query_pool,
                        TIMING_QUERY_FRAME_END);

    VK_CHECK(vkEndCommandBuffer(cmd));
}

uint64_t get_timeline_value(const VulkanContext &ctx, VkSemaphore semaphore)
{
    uint64_t value = 0;
    VkResult r = vkGetSemaphoreCounterValue(ctx.device, semaphore, &value);
    if (r != VK_SUCCESS) {
        return uint64_t(-1);
    }
    return value;
}

} // namespace

int main()
{
    try {
        std::cout << "RenderVulkan/OIDN timeline semaphore hang reproducer\n";
        std::cout << "----------------------------------------------------\n";
        std::cout << "Framebuffer size used for OIDN buffers: "
                  << kWidth << "x" << kHeight << "\n";
        std::cout << "CPU final fence timeout: " << (kWaitTimeoutNs / 1000000000ull)
                  << " seconds\n" << std::flush;

        VulkanContext vk = create_vulkan_context();

        log_step("[OIDN setup] Query Vulkan device UUID and create matching OIDN device");
        VkPhysicalDeviceIDProperties id_properties = {};
        id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 properties = {};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &id_properties;
        vkGetPhysicalDeviceProperties2(vk.physical_device, &properties);

        oidn::UUID uuid;
        std::memcpy(uuid.bytes, id_properties.deviceUUID, sizeof(uuid.bytes));
        oidn::DeviceRef oidn_device = oidn::newDevice(uuid);
        if (oidn_device.getError() != oidn::Error::None) {
            throw std::runtime_error("Failed to create OIDN device from Vulkan UUID");
        }
        oidn_device.commit();
        check_oidn_error(oidn_device, "Failed to commit OIDN device");

        oidn::ExternalMemoryTypeFlag oidn_external_mem_type;
        const oidn::ExternalMemoryTypeFlags oidn_external_mem_types =
            oidn_device.get<oidn::ExternalMemoryTypeFlags>("externalMemoryTypes");
#ifdef _WIN32
        if (!(oidn_external_mem_types & oidn::ExternalMemoryTypeFlag::OpaqueWin32)) {
            throw std::runtime_error("OIDN device does not support OpaqueWin32 external memory");
        }
        oidn_external_mem_type = oidn::ExternalMemoryTypeFlag::OpaqueWin32;
#else
        if (!(oidn_external_mem_types & oidn::ExternalMemoryTypeFlag::OpaqueFD)) {
            throw std::runtime_error("OIDN device does not support OpaqueFD external memory");
        }
        oidn_external_mem_type = oidn::ExternalMemoryTypeFlag::OpaqueFD;
#endif

        const size_t accum_size = 3ull * sizeof(float) * 4ull * kWidth * kHeight;
        const size_t denoise_size = sizeof(float) * 4ull * kWidth * kHeight;
        log_step("[Vulkan setup] Create exportable accum_buffer and denoise_buffer");
        ExternalBuffer accum_buffer = create_external_buffer(
            vk, accum_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        ExternalBuffer denoise_buffer = create_external_buffer(
            vk, denoise_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        log_step("[OIDN setup] Import Vulkan buffers into OIDN");
        oidn::BufferRef oidn_input_buffer =
            import_oidn_buffer(vk, oidn_device, accum_buffer, oidn_external_mem_type);
        oidn::BufferRef oidn_output_buffer =
            import_oidn_buffer(vk, oidn_device, denoise_buffer, oidn_external_mem_type);

        log_step("[OIDN setup] Create and commit RT filter");
        oidn::FilterRef oidn_filter = oidn_device.newFilter("RT");
        check_oidn_error(oidn_device, "Failed to create OIDN RT filter");
        oidn_filter.setImage("color", oidn_input_buffer, oidn::Format::Float3,
                             kWidth, kHeight, 0 * sizeof(float) * 4,
                             3 * sizeof(float) * 4);
        oidn_filter.setImage("albedo", oidn_input_buffer, oidn::Format::Float3,
                             kWidth, kHeight, 1 * sizeof(float) * 4,
                             3 * sizeof(float) * 4);
        oidn_filter.setImage("normal", oidn_input_buffer, oidn::Format::Float3,
                             kWidth, kHeight, 2 * sizeof(float) * 4,
                             3 * sizeof(float) * 4);
        oidn_filter.setImage("output", oidn_output_buffer, oidn::Format::Float3,
                             kWidth, kHeight, 0, sizeof(float) * 4);
        oidn_filter.set("hdr", true);
        oidn_filter.set("quality", oidn::Quality::Balanced);
        oidn_filter.commit();
        check_oidn_error(oidn_device, "Failed to commit OIDN RT filter");

        log_step("[Vulkan setup] Create exportable timeline semaphore and import into OIDN");
        VkSemaphore timeline_semaphore = create_exportable_timeline_semaphore(vk);
        oidn::SemaphoreRef oidn_timeline_semaphore =
            import_oidn_timeline_semaphore(vk, oidn_device, timeline_semaphore);

        log_step("[Vulkan setup] Create command pool, command buffers, query pool, and final fence");
        VkCommandPoolCreateInfo pool_ci = {};
        pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_ci.queueFamilyIndex = vk.queue_family;
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VK_CHECK(vkCreateCommandPool(vk.device, &pool_ci, nullptr, &command_pool));

        std::array<VkCommandBuffer, 2> command_buffers;
        VkCommandBufferAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers.size());
        VK_CHECK(vkAllocateCommandBuffers(vk.device, &alloc_info, command_buffers.data()));

        VkQueryPool timing_query_pool = VK_NULL_HANDLE;
        VkQueryPoolCreateInfo query_ci = {};
        query_ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_ci.queryCount = TIMING_QUERY_COUNT * kMaxFramesInFlight;
        VK_CHECK(vkCreateQueryPool(vk.device, &query_ci, nullptr, &timing_query_pool));

        VkFence final_fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fence_ci = {};
        fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(vk.device, &fence_ci, nullptr, &final_fence));

        log_step("[Vulkan setup] Record render_cmd_buf with RenderVulkan external release barriers");
        record_render_command_buffer(vk,
                                     command_buffers[0],
                                     timing_query_pool,
                                     accum_buffer,
                                     denoise_buffer);
        log_step("[Vulkan setup] Record tonemap_cmd_buf with RenderVulkan external acquire barriers");
        record_tonemap_command_buffer(vk,
                                      command_buffers[1],
                                      timing_query_pool,
                                      accum_buffer,
                                      denoise_buffer);

        // Match RenderVulkan::render for TimelineSemaphore mode.
        const uint32_t slot = 0;
        uint64_t oidn_timeline_value = 1;
        const uint64_t timeline_render_done_value = oidn_timeline_value++;
        const uint64_t timeline_denoise_done_value = oidn_timeline_value++;

        std::cout << "[Frame] slot=" << slot
                  << ", render_done=" << timeline_render_done_value
                  << ", denoise_done=" << timeline_denoise_done_value << std::endl;

        std::array<VkPipelineStageFlags, 1> wait_stages = {{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT}};
        VkTimelineSemaphoreSubmitInfo timeline_info = {};
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.waitSemaphoreValueCount = 0;
        timeline_info.pWaitSemaphoreValues = nullptr;
        timeline_info.signalSemaphoreValueCount = 1;
        timeline_info.pSignalSemaphoreValues = &timeline_render_done_value;

        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.pNext = &timeline_info;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffers[0];
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &timeline_semaphore;

        timed_step("[Vulkan queue] vkQueueSubmit(render_cmd_buf, signal timeline render_done)", [&]() {
            VK_CHECK(vkQueueSubmit(vk.queue, 1, &submit_info, VK_NULL_HANDLE));
        });
        std::cout << "[Diagnostics] timeline semaphore current value after render submit return: "
                  << get_timeline_value(vk, timeline_semaphore) << std::endl;

        timed_step("[OIDN/SYCL] oidn_device.waitSemaphoreAsync(timeline, render_done)", [&]() {
            oidn_device.waitSemaphoreAsync(oidn_timeline_semaphore, timeline_render_done_value);
            check_oidn_error(oidn_device, "OIDN waitSemaphoreAsync failed");
        });

        timed_step("[OIDN/SYCL] oidn_filter.executeAsync()", [&]() {
            oidn_filter.executeAsync();
            check_oidn_error(oidn_device, "OIDN executeAsync failed");
        });

        timed_step("[OIDN/SYCL] oidn_device.signalSemaphoreAsync(timeline, denoise_done)", [&]() {
            oidn_device.signalSemaphoreAsync(oidn_timeline_semaphore, timeline_denoise_done_value);
            check_oidn_error(oidn_device, "OIDN signalSemaphoreAsync failed");
        });
        std::cout << "[Diagnostics] timeline semaphore current value after OIDN async signal enqueue: "
                  << get_timeline_value(vk, timeline_semaphore) << std::endl;

        // Mutate the same VkSubmitInfo/VkTimelineSemaphoreSubmitInfo exactly as
        // RenderVulkan does before submitting tonemap_cmd_buf.
        submit_info.pCommandBuffers = &command_buffers[1];
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &timeline_semaphore;
        submit_info.pWaitDstStageMask = wait_stages.data();
        submit_info.signalSemaphoreCount = 0;
        submit_info.pSignalSemaphores = nullptr;

        timeline_info.waitSemaphoreValueCount = 1;
        timeline_info.pWaitSemaphoreValues = &timeline_denoise_done_value;
        timeline_info.signalSemaphoreValueCount = 0;
        timeline_info.pSignalSemaphoreValues = nullptr;

        timed_step("[Vulkan queue] vkQueueSubmit(tonemap_cmd_buf, wait timeline denoise_done, signal final fence)", [&]() {
            VK_CHECK(vkQueueSubmit(vk.queue, 1, &submit_info, final_fence));
        });
        std::cout << "[Diagnostics] timeline semaphore current value after tonemap submit return: "
                  << get_timeline_value(vk, timeline_semaphore) << std::endl;

        log_step("[CPU] vkWaitForFences(final_fence, timeout) begin");
        const VkResult wait_result = vkWaitForFences(
            vk.device, 1, &final_fence, VK_TRUE, kWaitTimeoutNs);

        if (wait_result == VK_TIMEOUT) {
            std::cerr << "\n[FAIL] TIMEOUT waiting for final Vulkan fence.\n"
                      << "       This means tonemap_cmd_buf did not complete.\n"
                      << "       Most likely the Vulkan queue is stuck waiting for timeline value "
                      << timeline_denoise_done_value << ".\n"
                      << "       Current Vulkan timeline semaphore value: "
                      << get_timeline_value(vk, timeline_semaphore) << "\n"
                      << "       Expected values: render_done=" << timeline_render_done_value
                      << ", denoise_done=" << timeline_denoise_done_value << "\n"
                      << "       Last completed diagnostic step was printed above.\n";
            return 1;
        }
        VK_CHECK(wait_result);

        std::cout << "[CPU] vkWaitForFences returned VK_SUCCESS" << std::endl;
        std::cout << "[Diagnostics] final timeline semaphore value: "
                  << get_timeline_value(vk, timeline_semaphore) << std::endl;
        std::cout << "[PASS] RenderVulkan-style OIDN timeline semaphore sequence completed.\n";

        VK_CHECK(vkQueueWaitIdle(vk.queue));
        vkDestroyFence(vk.device, final_fence, nullptr);
        vkDestroyQueryPool(vk.device, timing_query_pool, nullptr);
        vkDestroyCommandPool(vk.device, command_pool, nullptr);
        vkDestroySemaphore(vk.device, timeline_semaphore, nullptr);
        destroy_external_buffer(vk, accum_buffer);
        destroy_external_buffer(vk, denoise_buffer);
        vkDestroyDevice(vk.device, nullptr);
        vkDestroyInstance(vk.instance, nullptr);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "\n[EXCEPTION] " << e.what() << std::endl;
        return 2;
    }
}
