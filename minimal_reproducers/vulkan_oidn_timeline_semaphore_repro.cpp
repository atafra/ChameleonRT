// Minimal Vulkan/SYCL timeline semaphore reproducer for RenderVulkan-style hangs.
//
// Sequence:
//   Vulkan queue: submit cmd0 and signal timeline semaphore value N
//   SYCL queue:   wait_external_semaphore(N)
//                 signal_external_semaphore(N+1)
//   Vulkan queue: submit cmd1 waiting on timeline semaphore value N+1 and
//                 signal VkFence
//   CPU:          wait for VkFence with timeout
//
// This intentionally avoids OIDN, external memory, kernels, ray tracing,
// shaders, and swapchain work. It isolates Vulkan timeline semaphore import
// into a non-immediate Level Zero SYCL queue.
//
// Windows build example using the staged DPC++ runtime from the ChameleonRT build:
//   cd build\vs
//   call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
//   .\dpcpp\src\bin\clang++.exe -fsycl -std=c++17 -O2 ^
//      -I%VULKAN_SDK%\Include ^
//      ..\..\minimal_reproducers\vulkan_oidn_timeline_semaphore_repro.cpp ^
//      -o vulkan_oidn_timeline_semaphore_repro.exe ^
//      -L%VULKAN_SDK%\Lib -lvulkan-1

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-attributes"
#endif
#include <vulkan/vulkan.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <sycl/detail/core.hpp>
#include <sycl/ext/oneapi/bindless_images.hpp>
#include <sycl/properties/queue_properties.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace syclexp = sycl::ext::oneapi::experimental;

namespace {

static const uint64_t kWaitTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;

#ifdef _WIN32
static const VkExternalSemaphoreHandleTypeFlagBits kExternalSemaphoreHandleType =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
static const VkExternalSemaphoreHandleTypeFlagBits kExternalSemaphoreHandleType =
    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

const char *vk_result_string(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    default: return "<unknown VkResult>";
    }
}

const char *sycl_backend_string(sycl::backend backend)
{
    switch (backend) {
    case sycl::backend::ext_oneapi_level_zero: return "level_zero";
    case sycl::backend::opencl: return "opencl";
    case sycl::backend::ext_oneapi_cuda: return "cuda";
    case sycl::backend::ext_oneapi_hip: return "hip";
    default: return "unknown";
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

sycl::device select_sycl_device()
{
    std::vector<sycl::device> devices = sycl::device::get_devices();

    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].is_gpu() &&
            devices[i].get_backend() == sycl::backend::ext_oneapi_level_zero) {
            return devices[i];
        }
    }

    std::ostringstream out;
    out << "No Level Zero GPU SYCL device found. Available SYCL devices:";
    for (size_t i = 0; i < devices.size(); ++i) {
        out << "\n  [" << i << "] "
            << sycl_backend_string(devices[i].get_backend()) << ": "
            << devices[i].get_info<sycl::info::device::name>();
    }
    throw std::runtime_error(out.str());
}

struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
};

VulkanContext create_vulkan_context()
{
    VulkanContext ctx;

    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Vulkan SYCL timeline semaphore hang reproducer";
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

    const std::array<const char *, 3> required_extensions = {{
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
#ifdef _WIN32
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME
#else
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME
#endif
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
        throw std::runtime_error("No Vulkan device supports required timeline/external semaphore features");
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

    VK_CHECK(vkCreateDevice(ctx.physical_device, &device_ci, nullptr, &ctx.device));
    vkGetDeviceQueue(ctx.device, ctx.queue_family, 0, &ctx.queue);

    return ctx;
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

uint64_t get_timeline_value(const VulkanContext &ctx, VkSemaphore semaphore)
{
    uint64_t value = 0;
    VkResult r = vkGetSemaphoreCounterValue(ctx.device, semaphore, &value);
    if (r != VK_SUCCESS) {
        return uint64_t(-1);
    }
    return value;
}

syclexp::external_semaphore import_sycl_timeline_semaphore(const VulkanContext &ctx,
                                                           VkSemaphore semaphore,
                                                           const sycl::device &sycl_device,
                                                           const sycl::context &sycl_context)
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

    auto sem_desc = syclexp::external_semaphore_descriptor<syclexp::resource_win32_handle>{
        handle,
        syclexp::external_semaphore_handle_type::timeline_win32_nt_handle};
    syclexp::external_semaphore sycl_sem =
        syclexp::import_external_semaphore(sem_desc, sycl_device, sycl_context);

    CloseHandle(handle);
    return sycl_sem;
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

    auto sem_desc = syclexp::external_semaphore_descriptor<syclexp::resource_fd>{
        fd,
        syclexp::external_semaphore_handle_type::timeline_fd};
    syclexp::external_semaphore sycl_sem =
        syclexp::import_external_semaphore(sem_desc, sycl_device, sycl_context);

    close(fd);
    return sycl_sem;
#endif
}

} // namespace

int main()
{
    try {
        std::cout << "Vulkan/SYCL async timeline semaphore hang reproducer\n";
        std::cout << "--------------------------------------------------\n";
        std::cout << "CPU final fence timeout: " << (kWaitTimeoutNs / 1000000000ull)
                  << " seconds\n" << std::flush;

        VulkanContext vk = create_vulkan_context();

        sycl::device sycl_device = select_sycl_device();
        sycl::queue q{sycl_device};
        sycl::context sycl_context = q.get_context();

        std::cout << "[SYCL] Device: "
                  << sycl_device.get_info<sycl::info::device::name>() << "\n";
        std::cout << "[SYCL] Backend: "
                  << sycl_backend_string(sycl_device.get_backend()) << "\n";

        VkSemaphore timeline_semaphore = create_exportable_timeline_semaphore(vk);
        syclexp::external_semaphore sycl_semaphore = import_sycl_timeline_semaphore(
            vk, timeline_semaphore, sycl_device, sycl_context);

        VkFence final_fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fence_ci = {};
        fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(vk.device, &fence_ci, nullptr, &final_fence));

        const uint64_t render_done = 1;
        const uint64_t sycl_done = 2;

        std::cout << "[Frame] render_done=" << render_done
                  << ", sycl_done=" << sycl_done << std::endl;

        VkTimelineSemaphoreSubmitInfo render_timeline_info = {};
        render_timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        render_timeline_info.signalSemaphoreValueCount = 1;
        render_timeline_info.pSignalSemaphoreValues = &render_done;

        VkSubmitInfo render_submit_info = {};
        render_submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        render_submit_info.pNext = &render_timeline_info;
        render_submit_info.commandBufferCount = 0;
        render_submit_info.pCommandBuffers = nullptr;
        render_submit_info.signalSemaphoreCount = 1;
        render_submit_info.pSignalSemaphores = &timeline_semaphore;

        std::cout << "[Vulkan] submit signal timeline " << render_done << "\n";
        VK_CHECK(vkQueueSubmit(vk.queue, 1, &render_submit_info, VK_NULL_HANDLE));

        std::cout << "[Diagnostics] timeline after signal submit: "
                  << get_timeline_value(vk, timeline_semaphore) << std::endl;

        std::cout << "[SYCL] wait_external_semaphore(" << render_done << ")\n";
        q.ext_oneapi_wait_external_semaphore(sycl_semaphore, render_done);

        std::cout << "[SYCL] signal_external_semaphore(" << sycl_done << ")\n";
        q.ext_oneapi_signal_external_semaphore(sycl_semaphore, sycl_done);

        std::cout << "[Diagnostics] timeline after SYCL async signal enqueue: "
                  << get_timeline_value(vk, timeline_semaphore) << std::endl;

        std::array<VkPipelineStageFlags, 1> wait_stages = {{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT}};

        VkTimelineSemaphoreSubmitInfo timeline_info = {};
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.waitSemaphoreValueCount = 1;
        timeline_info.pWaitSemaphoreValues = &sycl_done;

        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.pNext = &timeline_info;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &timeline_semaphore;
        submit_info.pWaitDstStageMask = wait_stages.data();
        submit_info.commandBufferCount = 0;
        submit_info.pCommandBuffers = nullptr;
        submit_info.signalSemaphoreCount = 0;
        submit_info.pSignalSemaphores = nullptr;

        std::cout << "[Vulkan] submit wait timeline " << sycl_done
                  << ", signal final fence\n";
        VK_CHECK(vkQueueSubmit(vk.queue, 1, &submit_info, final_fence));

        std::cout << "[Diagnostics] timeline after cmd1 submit: "
                  << get_timeline_value(vk, timeline_semaphore) << std::endl;

        const VkResult wait_result =
            vkWaitForFences(vk.device, 1, &final_fence, VK_TRUE, kWaitTimeoutNs);

        if (wait_result == VK_TIMEOUT) {
            std::cerr << "\n[FAIL] TIMEOUT waiting for final Vulkan fence.\n"
                      << "       Vulkan queue likely stuck waiting for timeline value "
                      << sycl_done << ".\n"
                      << "       Current timeline value: "
                      << get_timeline_value(vk, timeline_semaphore) << "\n";

            syclexp::release_external_semaphore(sycl_semaphore, sycl_device, sycl_context);
            vkDestroyFence(vk.device, final_fence, nullptr);
            vkDestroySemaphore(vk.device, timeline_semaphore, nullptr);
            vkDestroyDevice(vk.device, nullptr);
            vkDestroyInstance(vk.instance, nullptr);
            return 1;
        }

        VK_CHECK(wait_result);
        std::cout << "[PASS] Fence completed. Final timeline value: "
                  << get_timeline_value(vk, timeline_semaphore) << "\n";

        q.wait();
        syclexp::release_external_semaphore(sycl_semaphore, sycl_device, sycl_context);

        VK_CHECK(vkQueueWaitIdle(vk.queue));
        vkDestroyFence(vk.device, final_fence, nullptr);
        vkDestroySemaphore(vk.device, timeline_semaphore, nullptr);
        vkDestroyDevice(vk.device, nullptr);
        vkDestroyInstance(vk.instance, nullptr);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "\n[EXCEPTION] " << e.what() << std::endl;
        return 2;
    }
}
