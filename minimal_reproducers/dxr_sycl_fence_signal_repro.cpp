// Minimal D3D12/SYCL fence reproducer for RenderDXR-style hangs.
//
// Sequence:
//   D3D12 queue: Signal(shared fence, N)
//   D3D12 queue: Wait(shared fence, N+1)
//   D3D12 queue: Signal(shared fence, N+2)
//   SYCL queue:  wait_external_semaphore(N)
//                signal_external_semaphore(N+1)
//   CPU:         wait for shared fence value N+2 with timeout
//
// This intentionally avoids OIDN, ray tracing, shaders, command lists, and
// external memory. It isolates the D3D12 fence <-> SYCL external semaphore path
// used by RenderDXR's async OIDN interop mode.
//
// Windows build example using the staged DPC++ runtime from the ChameleonRT build:
//   cd build\vs
//   call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
//   .\dpcpp\src\bin\clang++.exe -fsycl -std=c++17 -O2 ^
//      ..\..\minimal_reproducers\dxr_sycl_fence_signal_repro.cpp ^
//      -o dxr_sycl_fence_signal_repro.exe -ld3d12 -ldxgi

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include <sycl/detail/core.hpp>
#include <sycl/ext/oneapi/bindless_images.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace syclexp = sycl::ext::oneapi::experimental;
using Microsoft::WRL::ComPtr;

namespace {

static const DWORD kWaitTimeoutMs = 10000;

void throw_if_failed(HRESULT hr, const char *message)
{
    if (FAILED(hr)) {
        std::ostringstream out;
        out << message << " failed with HRESULT 0x" << std::hex << hr;
        throw std::runtime_error(out.str());
    }
}

std::string wide_to_utf8(const wchar_t *text)
{
    if (!text || !text[0]) {
        return std::string();
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return std::string();
    }

    std::vector<char> result(size);
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return std::string(result.data());
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

struct D3D12Context {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
};

D3D12Context create_d3d12_context()
{
    D3D12Context ctx;

    throw_if_failed(CreateDXGIFactory2(0, IID_PPV_ARGS(&ctx.factory)),
                    "CreateDXGIFactory2");

    for (uint32_t i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = ctx.factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        throw_if_failed(hr, "EnumAdapters1");

        DXGI_ADAPTER_DESC1 desc;
        throw_if_failed(adapter->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }

        ComPtr<ID3D12Device> device;
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) {
            ctx.adapter = adapter;
            ctx.device = device;
            std::cout << "[D3D12 setup] selected adapter: "
                      << wide_to_utf8(desc.Description) << "\n";
            break;
        }
    }

    if (!ctx.device) {
        throw std::runtime_error("No D3D12-capable hardware adapter found");
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    throw_if_failed(ctx.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&ctx.queue)),
                    "ID3D12Device::CreateCommandQueue");

    return ctx;
}

} // namespace

int main()
{
    HANDLE shared_fence_handle = nullptr;
    HANDLE fence_event = nullptr;

    try {
        std::cout << "D3D12/SYCL async fence-signal hang reproducer\n";
        std::cout << "----------------------------------------------\n";
        std::cout << "CPU final fence timeout: " << kWaitTimeoutMs << " ms\n";

        D3D12Context d3d = create_d3d12_context();

        sycl::device sycl_device = select_sycl_device();
        sycl::queue q{sycl_device};
        sycl::context sycl_context = q.get_context();

        std::cout << "[SYCL] Device: "
                  << sycl_device.get_info<sycl::info::device::name>() << "\n";
        std::cout << "[SYCL] Backend: "
                  << sycl_backend_string(sycl_device.get_backend()) << "\n";

        ComPtr<ID3D12Fence> shared_fence;
        throw_if_failed(d3d.device->CreateFence(0,
                                                D3D12_FENCE_FLAG_SHARED,
                                                IID_PPV_ARGS(&shared_fence)),
                        "ID3D12Device::CreateFence(shared)");

        throw_if_failed(d3d.device->CreateSharedHandle(shared_fence.Get(),
                                                       nullptr,
                                                       GENERIC_ALL,
                                                       nullptr,
                                                       &shared_fence_handle),
                        "ID3D12Device::CreateSharedHandle(shared fence)");

        auto sem_desc = syclexp::external_semaphore_descriptor<syclexp::resource_win32_handle>{
            shared_fence_handle,
            syclexp::external_semaphore_handle_type::win32_nt_dx12_fence};
        syclexp::external_semaphore sycl_semaphore =
            syclexp::import_external_semaphore(sem_desc, sycl_device, sycl_context);

        fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) {
            throw std::runtime_error("CreateEvent failed");
        }

        const uint64_t render_done = 1;
        const uint64_t sycl_done = 2;
        const uint64_t final_done = 3;

        std::cout << "[Frame] render_done=" << render_done
                  << ", sycl_done=" << sycl_done
                  << ", final_done=" << final_done << "\n";

        std::cout << "[D3D12] queue Signal(shared fence, " << render_done << ")\n";
        throw_if_failed(d3d.queue->Signal(shared_fence.Get(), render_done),
                        "ID3D12CommandQueue::Signal(render_done)");

        std::cout << "[Diagnostics] shared fence after render signal submit: "
                  << shared_fence->GetCompletedValue() << "\n";

        std::cout << "[D3D12] queue Wait(shared fence, " << sycl_done
                  << ") [GPU-side wait]\n";
        throw_if_failed(d3d.queue->Wait(shared_fence.Get(), sycl_done),
                        "ID3D12CommandQueue::Wait(sycl_done)");

        std::cout << "[D3D12] queue Signal(shared fence, " << final_done << ")\n";
        throw_if_failed(d3d.queue->Signal(shared_fence.Get(), final_done),
                        "ID3D12CommandQueue::Signal(final_done)");

        std::cout << "[SYCL] wait_external_semaphore(" << render_done << ")\n";
        q.ext_oneapi_wait_external_semaphore(sycl_semaphore, render_done);

        std::cout << "[SYCL] signal_external_semaphore(" << sycl_done << ")\n";
        q.ext_oneapi_signal_external_semaphore(sycl_semaphore, sycl_done);

        std::cout << "[Diagnostics] shared fence after SYCL async signal enqueue: "
                  << shared_fence->GetCompletedValue() << "\n";

        std::cout << "[CPU] Wait for shared fence value " << final_done
                  << " with timeout\n";
        throw_if_failed(shared_fence->SetEventOnCompletion(final_done, fence_event),
                        "ID3D12Fence::SetEventOnCompletion(final_done)");

        const DWORD wait_result = WaitForSingleObject(fence_event, kWaitTimeoutMs);
        if (wait_result == WAIT_TIMEOUT) {
            std::cerr << "\n[FAIL] TIMEOUT waiting for final D3D12 fence.\n"
                      << "       D3D12 queue likely stuck waiting for fence value "
                      << sycl_done << ".\n"
                      << "       Current shared fence value: "
                      << shared_fence->GetCompletedValue() << "\n";

            syclexp::release_external_semaphore(sycl_semaphore, sycl_device, sycl_context);
            CloseHandle(fence_event);
            CloseHandle(shared_fence_handle);
            return 1;
        }

        if (wait_result != WAIT_OBJECT_0) {
            throw std::runtime_error("WaitForSingleObject returned unexpected result");
        }

        std::cout << "[PASS] Fence completed. Final shared fence value: "
                  << shared_fence->GetCompletedValue() << "\n";

        q.wait();
        syclexp::release_external_semaphore(sycl_semaphore, sycl_device, sycl_context);
        CloseHandle(fence_event);
        CloseHandle(shared_fence_handle);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "\n[EXCEPTION] " << e.what() << "\n";
        if (fence_event) {
            CloseHandle(fence_event);
        }
        if (shared_fence_handle) {
            CloseHandle(shared_fence_handle);
        }
        return 2;
    }
}
