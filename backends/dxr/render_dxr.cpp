#include "render_dxr.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include "render_dxr.h"
#include "render_dxr_embedded_dxil.h"
#include "tonemap_embedded_dxil.h"
#include "util.h"
#include <glm/ext.hpp>

#ifdef ENABLE_PIX_RUNTIME
#include <pix3.h>
#endif

#define NUM_RAY_TYPES 2

using Microsoft::WRL::ComPtr;

namespace {

enum TimingQuery {
    TIMING_QUERY_FRAME_BEGIN = 0,
    TIMING_QUERY_RAYTRACING_BEGIN,
    TIMING_QUERY_RAYTRACING_END,
    TIMING_QUERY_DENOISE_BEGIN,
    TIMING_QUERY_TONEMAP_BEGIN,
    TIMING_QUERY_FRAME_END,
    TIMING_QUERY_COUNT
};

float elapsed_timestamp_ms(const uint64_t *timestamps,
                           uint64_t timestamp_freq,
                           TimingQuery begin,
                           TimingQuery end)
{
    const uint64_t delta = timestamps[end] - timestamps[begin];
    return static_cast<float>(static_cast<double>(delta) / timestamp_freq * 1e3);
}

} // namespace

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
bool RenderDXR::frame_diagnostics_enabled() const
{
    return frame_diagnostics_active && frame_id < 5;
}

void RenderDXR::log_frame_diagnostic(const std::string &event) const
{
    if (!frame_diagnostics_enabled()) {
        return;
    }

    std::cout << "[DXR frame diagnostic][frame " << frame_id << "] "
              << event << std::endl;
}
#define DXR_FRAME_DIAGNOSTIC(event) log_frame_diagnostic(event)
#else
#define DXR_FRAME_DIAGNOSTIC(event) do { } while (false)
#endif

RenderDXR::RenderDXR(Microsoft::WRL::ComPtr<ID3D12Device5> device)
    : device(device), native_display(true)
{
    create_device_objects();
}

RenderDXR::RenderDXR() : native_display(false)
{
    // Enable debugging for D3D12
#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> debug_controller;
        auto err = D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller));
        if (FAILED(err)) {
            std::cout << "Failed to enable debug layer!\n";
            throw std::runtime_error("get debug failed");
        }
        debug_controller->EnableDebugLayer();
    }
#endif

#ifdef _DEBUG
    uint32_t factory_flags = DXGI_CREATE_FACTORY_DEBUG;
#else
    uint32_t factory_flags = 0;
#endif
    CHECK_ERR(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)));

    device = dxr::create_dxr_device(factory);
    if (!device) {
        std::cout << "Failed to find DXR capable GPU!" << std::endl;
        throw std::runtime_error("Failed to find DXR capable device!");
    }
    create_device_objects();
}

RenderDXR::~RenderDXR()
{
    sync_gpu();
    CloseHandle(fence_evt);
}

std::string RenderDXR::name()
{
    return "DirectX Ray Tracing";
}

bool RenderDXR::supports_ray_stats() const
{
#ifdef REPORT_RAY_STATS
    return true;
#else
    return false;
#endif
}

#ifdef ENABLE_OIDN
namespace {

const char *oidn_interop_mode_name(RenderDXR::OIDNInteropMode mode)
{
    switch (mode) {
    case RenderDXR::OIDNInteropMode::HostBlocking:
        return "host_blocking";
    case RenderDXR::OIDNInteropMode::DeviceAsync:
        return "device_async";
    }

    return "Undefined";
}

} // namespace

std::string RenderDXR::get_oidn_interop_mode()
{
    return oidn_interop_mode_name(oidn_interop_mode);
}

bool RenderDXR::set_oidn_interop_mode(const std::string &mode)
{
    OIDNInteropMode new_mode;

    if (mode == "host_blocking") {
        new_mode = OIDNInteropMode::HostBlocking;
    } else if (mode == "device_async") {
        if (!oidn_device_async_supported) {
            std::cerr << "OIDN interop mode '" << mode
                      << "' is not supported by this DXR device.\n";
            return false;
        }
        new_mode = OIDNInteropMode::DeviceAsync;
    } else {
        std::cerr << "OIDN interop mode '" << mode
                  << "' is not supported by the DXR backend.\n";
        return false;
    }

    if (oidn_interop_mode != new_mode) {
        oidn_interop_mode = new_mode;
        std::cout << "OIDN interop mode changed to: "
                  << oidn_interop_mode_name(oidn_interop_mode) << "\n";
    }

    return true;
}

std::vector<std::string> RenderDXR::get_supported_oidn_interop_modes()
{
    std::vector<std::string> modes;
    modes.push_back("host_blocking");

    if (oidn_device_async_supported) {
        modes.push_back("device_async");
    }

    return modes;
}
#endif

void RenderDXR::initialize(const int fb_width, const int fb_height)
{
#ifdef ENABLE_OIDN
    // Get the LUID of the adapter
    LUID luid = device->GetAdapterLuid();

    // Initialize the denoiser device
    oidn_device = oidn::newDevice(oidn::LUID{luid.LowPart, luid.HighPart});
    if (oidn_device.getError() != oidn::Error::None)
        throw std::runtime_error("Failed to create OIDN device.");
    oidn_device.commit();
    if (oidn_device.getError() != oidn::Error::None)
        throw std::runtime_error("Failed to commit OIDN device.");

    // Find a compatible external memory handle type
    const auto oidn_external_mem_types = oidn_device.get<oidn::ExternalMemoryTypeFlags>("externalMemoryTypes");
    if (!(oidn_external_mem_types & oidn::ExternalMemoryTypeFlag::OpaqueWin32))
        throw std::runtime_error("failed to find compatible external memory type");
#endif

    frame_id = 0;
    frame_slot = 0;
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        slot_submitted[i] = false;
    }
    img.resize(fb_width * fb_height);

    render_target = dxr::Texture2D::device(device.Get(),
                                           glm::uvec2(fb_width, fb_height),
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                           DXGI_FORMAT_R8G8B8A8_UNORM,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

#ifdef ENABLE_OIDN
    accum_buffer = dxr::Buffer::device(device.Get(),
                                       3 * sizeof(glm::vec4) * fb_width * fb_height,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                       D3D12_HEAP_FLAG_SHARED);

    denoise_buffer = dxr::Buffer::device(device.Get(),
                                         sizeof(glm::vec4) * fb_width * fb_height,
                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                         D3D12_HEAP_FLAG_SHARED);
#else
     accum_buffer = dxr::Buffer::device(device.Get(),
                                        sizeof(glm::vec4) * fb_width * fb_height,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
#endif

    // Allocate the per-slot readback buffers so we can read the image back to the
    // CPU. They are kept persistently mapped to avoid per-frame Map/Unmap overhead.
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        img_readback_buf[i] =
            dxr::Buffer::readback(device.Get(),
                                  render_target.linear_row_pitch() * fb_height,
                                  D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_RANGE read_range;
        read_range.Begin = 0;
        read_range.End = img_readback_buf[i].size();
        img_readback_mapping[i] = static_cast<uint8_t *>(img_readback_buf[i].map(read_range));
    }

#ifdef REPORT_RAY_STATS
    ray_stats = dxr::Texture2D::device(device.Get(),
                                       glm::uvec2(fb_width, fb_height),
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                       DXGI_FORMAT_R16_UINT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    ray_counts.resize(ray_stats.dims().x * ray_stats.dims().y, 0);

    // Per-slot ray stats readback buffers, kept persistently mapped as well.
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        ray_stats_readback_buf[i] =
            dxr::Buffer::readback(device.Get(),
                                  ray_stats.linear_row_pitch() * fb_height,
                                  D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_RANGE read_range;
        read_range.Begin = 0;
        read_range.End = ray_stats_readback_buf[i].size();
        ray_stats_readback_mapping[i] =
            static_cast<uint8_t *>(ray_stats_readback_buf[i].map(read_range));
    }
#endif

    if (rt_pipeline.get()) {
        build_descriptor_heap();
        record_command_lists();
    }

#ifdef ENABLE_OIDN
    {
        // Initialize the denoiser filter
        oidn_filter = oidn_device.newFilter("RT");
        if (oidn_device.getError() != oidn::Error::None)
            throw std::runtime_error("Failed to create OIDN filter.");

        HANDLE accum_buffer_handle = nullptr;
        CHECK_ERR(device->CreateSharedHandle(
                    accum_buffer.get(),
                    nullptr,
                    GENERIC_ALL,
                    nullptr,
                    &accum_buffer_handle));
        auto input_buffer = oidn_device.newBuffer(oidn::ExternalMemoryTypeFlag::OpaqueWin32,
                                                  accum_buffer_handle, nullptr, accum_buffer.size());

        HANDLE denoise_buffer_handle = nullptr;
        CHECK_ERR(device->CreateSharedHandle(
                    denoise_buffer.get(),
                    nullptr,
                    GENERIC_ALL,
                    nullptr,
                    &denoise_buffer_handle));
        auto output_buffer = oidn_device.newBuffer(oidn::ExternalMemoryTypeFlag::OpaqueWin32,
                                                   denoise_buffer_handle, nullptr, denoise_buffer.size());

        oidn_filter.setImage("color",  input_buffer,  oidn::Format::Float3, fb_width, fb_height,
                             0 * sizeof(glm::vec4), 3 * sizeof(glm::vec4));
        oidn_filter.setImage("albedo", input_buffer,  oidn::Format::Float3, fb_width, fb_height,
                             1 * sizeof(glm::vec4), 3 * sizeof(glm::vec4));
        oidn_filter.setImage("normal", input_buffer,  oidn::Format::Float3, fb_width, fb_height,
                             2 * sizeof(glm::vec4), 3 * sizeof(glm::vec4));

        oidn_filter.setImage("output", output_buffer, oidn::Format::Float3, fb_width, fb_height,
                             0, sizeof(glm::vec4));

        oidn_filter.set("hdr", true);
        oidn_filter.set("quality", oidn::Quality::Balanced);
        
        oidn_filter.commit();
        if (oidn_device.getError() != oidn::Error::None)
            throw std::runtime_error("Failed to commit OIDN filter.");

        oidn_device_async_supported = false;

        // Register D3D fence for OIDN interop
        HANDLE win32_fence_handle;
        CHECK_ERR(device->CreateSharedHandle(
            fence.Get(), nullptr, GENERIC_ALL, nullptr, &win32_fence_handle));

        oidn_semaphore = oidn_device.newSemaphore(
            oidn::ExternalSemaphoreTypeFlag::D3D12Fence, win32_fence_handle, nullptr);

        oidn_device_async_supported = oidn_device.getError() == oidn::Error::None;
        if (oidn_interop_mode == OIDNInteropMode::DeviceAsync && !oidn_device_async_supported) {
            std::cerr << "OIDN interop mode '"
                      << oidn_interop_mode_name(oidn_interop_mode)
                      << "' is not supported by this DXR device; using 'host_blocking'.\n";
            oidn_interop_mode = OIDNInteropMode::HostBlocking;
        }

        if (!oidn_interop_mode_initialized) {
            std::cout << "OIDN interop mode initialized: "
                      << oidn_interop_mode_name(oidn_interop_mode) << "\n";
            oidn_interop_mode_initialized = true;
        }

    }
#endif
}

void RenderDXR::set_scene(const Scene &scene)
{
    frame_id = 0;

    // TODO: We can actually run all these uploads and BVH builds in parallel
    // using multiple command lists, as long as the BVH builds don't need so
    // much build + scratch that we run out of GPU memory.
    // Some helpers for managing the temp upload heap buf allocation and queuing of
    // the commands would help to make it easier to write the parallel load version
    for (const auto &mesh : scene.meshes) {
        std::vector<dxr::Geometry> geometries;
        for (const auto &geom : mesh.geometries) {
            // Upload the mesh to the vertex buffer, build accel structures
            // Place the data in an upload heap first, then do a GPU-side copy
            // into a default heap (resident in VRAM)
            dxr::Buffer upload_verts =
                dxr::Buffer::upload(device.Get(),
                                    geom.vertices.size() * sizeof(glm::vec3),
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
            dxr::Buffer upload_indices =
                dxr::Buffer::upload(device.Get(),
                                    geom.indices.size() * sizeof(glm::uvec3),
                                    D3D12_RESOURCE_STATE_GENERIC_READ);

            // Copy vertex and index data into the upload buffers
            std::memcpy(upload_verts.map(), geom.vertices.data(), upload_verts.size());
            std::memcpy(upload_indices.map(), geom.indices.data(), upload_indices.size());
            upload_verts.unmap();
            upload_indices.unmap();

            dxr::Buffer upload_uvs;
            if (!geom.uvs.empty()) {
                upload_uvs = dxr::Buffer::upload(device.Get(),
                                                 geom.uvs.size() * sizeof(glm::vec2),
                                                 D3D12_RESOURCE_STATE_GENERIC_READ);
                std::memcpy(upload_uvs.map(), geom.uvs.data(), upload_uvs.size());
                upload_uvs.unmap();
            }

            dxr::Buffer upload_normals;
            if (!geom.normals.empty()) {
                upload_normals = dxr::Buffer::upload(device.Get(),
                                                     geom.normals.size() * sizeof(glm::vec3),
                                                     D3D12_RESOURCE_STATE_GENERIC_READ);
                std::memcpy(upload_normals.map(), geom.normals.data(), upload_normals.size());
                upload_normals.unmap();
            }

            // Allocate GPU side buffers for the data so we can have it resident in VRAM
            dxr::Buffer vertex_buf = dxr::Buffer::device(
                device.Get(), upload_verts.size(), D3D12_RESOURCE_STATE_COPY_DEST);
            dxr::Buffer index_buf = dxr::Buffer::device(
                device.Get(), upload_indices.size(), D3D12_RESOURCE_STATE_COPY_DEST);

            CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), nullptr));

            // Enqueue the copy into GPU memory
            cmd_list->CopyResource(vertex_buf.get(), upload_verts.get());
            cmd_list->CopyResource(index_buf.get(), upload_indices.get());

            dxr::Buffer uv_buf;
            if (!geom.uvs.empty()) {
                uv_buf = dxr::Buffer::device(
                    device.Get(), upload_uvs.size(), D3D12_RESOURCE_STATE_COPY_DEST);
                cmd_list->CopyResource(uv_buf.get(), upload_uvs.get());
            }

            dxr::Buffer normal_buf;
            if (!geom.normals.empty()) {
                normal_buf = dxr::Buffer::device(
                    device.Get(), upload_normals.size(), D3D12_RESOURCE_STATE_COPY_DEST);
                cmd_list->CopyResource(normal_buf.get(), upload_normals.get());
            }

            // Barriers to wait for the copies to finish before building the accel. structs
            {
                std::vector<D3D12_RESOURCE_BARRIER> b;
                b.push_back(barrier_transition(
                    vertex_buf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                b.push_back(barrier_transition(
                    index_buf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                if (!geom.uvs.empty()) {
                    b.push_back(barrier_transition(
                        uv_buf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                };
                if (!geom.normals.empty()) {
                    b.push_back(barrier_transition(
                        normal_buf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                };
                cmd_list->ResourceBarrier(b.size(), b.data());
            }

            geometries.emplace_back(vertex_buf, index_buf, normal_buf, uv_buf);

            // TODO: Some possible perf improvements: We can run all the upload of
            // index data in parallel, and the BVH building in parallel for all the
            // geometries. This should help for some large scenes, though with the assumption
            // that the entire build space for all the bottom level stuff can fit on the GPU.
            // For large scenes it would be best to monitor the available space needed for
            // the queued builds vs. the available GPU memory and then run stuff and compact
            // when we start getting full.
            CHECK_ERR(cmd_list->Close());
            ID3D12CommandList *cmd_lists = cmd_list.Get();
            cmd_queue->ExecuteCommandLists(1, &cmd_lists);
            sync_gpu();
        }

        meshes.emplace_back(geometries);

        CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), nullptr));
        meshes.back().enqeue_build(device.Get(), cmd_list.Get());
        CHECK_ERR(cmd_list->Close());
        ID3D12CommandList *cmd_lists = cmd_list.Get();
        cmd_queue->ExecuteCommandLists(1, &cmd_lists);
        sync_gpu();

        CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), nullptr));

        meshes.back().enqueue_compaction(device.Get(), cmd_list.Get());
        CHECK_ERR(cmd_list->Close());
        cmd_queue->ExecuteCommandLists(1, &cmd_lists);
        sync_gpu();

        meshes.back().finalize();
    }

    parameterized_meshes = scene.parameterized_meshes;
    std::vector<uint32_t> parameterized_mesh_sbt_offsets;
    {
        // Compute the offsets each parameterized mesh will be written too in the SBT,
        // these are then the instance SBT offsets shared by each instance
        uint32_t offset = 0;
        for (const auto &pm : parameterized_meshes) {
            parameterized_mesh_sbt_offsets.push_back(offset);
            offset += meshes[pm.mesh_id].geometries.size();
        }
    }

    // TODO: May be best to move this into the top-level BVH build step,
    // and have it take the parameterized mesh info as well, similar to what
    // I have in the Metal backend
    auto upload_instance_buf = dxr::Buffer::upload(
        device.Get(),
        align_to(scene.instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                 D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT),
        D3D12_RESOURCE_STATE_GENERIC_READ);
    {
        // TODO: We want to keep some of the instance to BLAS mapping info for setting up the
        // hitgroups/sbt so the toplevel bvh can become something a bit higher-level to manage
        // this and filling out the instance buffers
        // Write the data about our instance
        D3D12_RAYTRACING_INSTANCE_DESC *buf =
            static_cast<D3D12_RAYTRACING_INSTANCE_DESC *>(upload_instance_buf.map());

        for (size_t i = 0; i < scene.instances.size(); ++i) {
            const auto &inst = scene.instances[i];
            buf[i].InstanceID = i;
            buf[i].InstanceContributionToHitGroupIndex =
                parameterized_mesh_sbt_offsets[inst.parameterized_mesh_id];
            buf[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
            buf[i].AccelerationStructure =
                meshes[parameterized_meshes[inst.parameterized_mesh_id].mesh_id]
                    ->GetGPUVirtualAddress();
            buf[i].InstanceMask = 0xff;

            // Note: D3D matrices are row-major
            std::memset(buf[i].Transform, 0, sizeof(buf[i].Transform));
            const glm::mat4 m = glm::transpose(inst.transform);
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 4; ++c) {
                    buf[i].Transform[r][c] = m[r][c];
                }
            }
        }
        upload_instance_buf.unmap();
    }

    // Copy instance data to the device heap
    instance_buf = dxr::Buffer::device(
        device.Get(),
        align_to(scene.instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                 D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT),
        D3D12_RESOURCE_STATE_COPY_DEST);
    {
        CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), nullptr));

        // Enqueue the copy into GPU memory
        cmd_list->CopyResource(instance_buf.get(), upload_instance_buf.get());

        auto b =
            barrier_transition(instance_buf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cmd_list->ResourceBarrier(1, &b);

        CHECK_ERR(cmd_list->Close());
        ID3D12CommandList *cmd_lists = cmd_list.Get();
        cmd_queue->ExecuteCommandLists(1, &cmd_lists);
        sync_gpu();
    }

    // Now build the top level acceleration structure on our instance
    scene_bvh = dxr::TopLevelBVH(instance_buf, scene.instances);

    CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), nullptr));
    scene_bvh.enqeue_build(device.Get(), cmd_list.Get());
    CHECK_ERR(cmd_list->Close());

    ID3D12CommandList *cmd_lists = cmd_list.Get();
    cmd_queue->ExecuteCommandLists(1, &cmd_lists);
    sync_gpu();

    scene_bvh.finalize();

    // Upload the textures
    for (const auto &t : scene.textures) {
        const DXGI_FORMAT format = t.color_space == SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                         : DXGI_FORMAT_R8G8B8A8_UNORM;

        dxr::Texture2D tex = dxr::Texture2D::device(device.Get(),
                                                    glm::uvec2(t.width, t.height),
                                                    D3D12_RESOURCE_STATE_COPY_DEST,
                                                    format);

        dxr::Buffer tex_upload = dxr::Buffer::upload(device.Get(),
                                                     tex.linear_row_pitch() * t.height,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ);

        // TODO: Some better texture upload handling here, and readback for handling the row
        // pitch stuff
        if (tex.linear_row_pitch() == t.width * tex.pixel_size()) {
            std::memcpy(tex_upload.map(), t.img.data(), tex_upload.size());
        } else {
            uint8_t *buf = static_cast<uint8_t *>(tex_upload.map());
            for (uint32_t y = 0; y < t.height; ++y) {
                std::memcpy(buf + y * tex.linear_row_pitch(),
                            t.img.data() + y * t.width * tex.pixel_size(),
                            t.width * tex.pixel_size());
            }
        }
        tex_upload.unmap();

        // TODO: We can upload these textures at once as well
        CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), nullptr));

        tex.upload(cmd_list.Get(), tex_upload);
        auto b = barrier_transition(tex, D3D12_RESOURCE_STATE_GENERIC_READ);
        cmd_list->ResourceBarrier(1, &b);

        CHECK_ERR(cmd_list->Close());
        cmd_queue->ExecuteCommandLists(1, &cmd_lists);
        sync_gpu();

        textures.push_back(tex);
    }

    // Upload the material data
    CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), nullptr));
    {
        dxr::Buffer mat_upload_buf =
            dxr::Buffer::upload(device.Get(),
                                scene.materials.size() * sizeof(DisneyMaterial),
                                D3D12_RESOURCE_STATE_GENERIC_READ);
        std::memcpy(mat_upload_buf.map(), scene.materials.data(), mat_upload_buf.size());
        mat_upload_buf.unmap();

        material_param_buf = dxr::Buffer::device(
            device.Get(), mat_upload_buf.size(), D3D12_RESOURCE_STATE_COPY_DEST);

        cmd_list->CopyResource(material_param_buf.get(), mat_upload_buf.get());
        auto b = barrier_transition(material_param_buf, D3D12_RESOURCE_STATE_GENERIC_READ);
        cmd_list->ResourceBarrier(1, &b);

        CHECK_ERR(cmd_list->Close());
        cmd_queue->ExecuteCommandLists(1, &cmd_lists);
        sync_gpu();
    }

    // Upload the light data
    CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), nullptr));
    {
        dxr::Buffer light_upload_buf =
            dxr::Buffer::upload(device.Get(),
                                scene.lights.size() * sizeof(QuadLight),
                                D3D12_RESOURCE_STATE_GENERIC_READ);
        std::memcpy(light_upload_buf.map(), scene.lights.data(), light_upload_buf.size());
        light_upload_buf.unmap();

        light_buf = dxr::Buffer::device(
            device.Get(), light_upload_buf.size(), D3D12_RESOURCE_STATE_COPY_DEST);

        cmd_list->CopyResource(light_buf.get(), light_upload_buf.get());
        auto b = barrier_transition(light_buf, D3D12_RESOURCE_STATE_GENERIC_READ);
        cmd_list->ResourceBarrier(1, &b);

        CHECK_ERR(cmd_list->Close());
        cmd_queue->ExecuteCommandLists(1, &cmd_lists);
        sync_gpu();
    }

    build_shader_resource_heap();
    build_raytracing_pipeline();
    build_shader_binding_table();
    build_descriptor_heap();
    record_command_lists();
}

RenderStats RenderDXR::render(const glm::vec3 &pos,
                              const glm::vec3 &dir,
                              const glm::vec3 &up,
                              const float fovy,
                              const bool camera_changed,
                              const bool readback_framebuffer)
{
    using namespace std::chrono;
    RenderStats stats;

    // TODO: probably just pass frame_id directly
    if (camera_changed) {
        frame_id = 0;
    }

    // The in-flight frame slot whose command lists / readback buffers / timing
    // queries we use this frame.
    const uint32_t slot = frame_slot;
    const uint32_t prev_slot = (slot + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;

    // Reclaim this slot's resources. Its previous frame's work was submitted
    // MAX_FRAMES_IN_FLIGHT render() calls ago, so this wait is almost always
    // already satisfied and does not stall the host on work we are about to submit.
    if (slot_submitted[slot]) {
        wait_for_fence_value(slot_fence_value[slot]);
    }

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
    frame_diagnostics_active = frame_id < 5;
    {
        std::ostringstream msg;
        msg << "frame start; camera_changed=" << camera_changed
            << ", readback_framebuffer=" << readback_framebuffer
            << ", native_display=" << native_display
            << ", fence_value=" << fence_value;
#ifdef ENABLE_OIDN
        msg << ", oidn_interop_mode=" << oidn_interop_mode_name(oidn_interop_mode)
            << ", oidn_device_async_supported=" << oidn_device_async_supported;
#endif
        DXR_FRAME_DIAGNOSTIC(msg.str());
    }
#endif

    DXR_FRAME_DIAGNOSTIC("view parameter update begin");
    update_view_parameters(pos, dir, up, fovy);
    DXR_FRAME_DIAGNOSTIC("view parameter update end");

    ID3D12CommandList *render_cmds = render_cmd_list[slot].Get();
    DXR_FRAME_DIAGNOSTIC("ray tracing begin: ExecuteCommandLists(render_cmd_list)");
    cmd_queue->ExecuteCommandLists(1, &render_cmds);
    DXR_FRAME_DIAGNOSTIC("ray tracing command list submitted");

#ifdef ENABLE_OIDN
    // Denoise the frame
    if (oidn_interop_mode == OIDNInteropMode::HostBlocking) {
        DXR_FRAME_DIAGNOSTIC("denoising begin: host_blocking sync before OIDN execute");
        sync_gpu();
        DXR_FRAME_DIAGNOSTIC("denoising host_blocking sync complete; oidn_filter.execute begin");
        oidn_filter.execute();
        DXR_FRAME_DIAGNOSTIC("denoising end: oidn_filter.execute returned");
    } else if (oidn_interop_mode == OIDNInteropMode::DeviceAsync) {
        if (!oidn_device_async_supported) {
            throw(std::logic_error("Device async OIDN interop is not initialized"));
        }

        // signal fence and let OIDN wait to execute asynchronously
        const uint64_t oidn_fence_value = fence_value;
        fence_value += 2;

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
        {
            std::ostringstream msg;
            msg << "ID3D12Fence interaction begin: command queue Signal for OIDN, value="
                << oidn_fence_value << ", reserved_next_value=" << fence_value;
            DXR_FRAME_DIAGNOSTIC(msg.str());
        }
#endif
        cmd_queue->Signal(fence.Get(), oidn_fence_value);
        DXR_FRAME_DIAGNOSTIC("ID3D12Fence interaction end: command queue Signal for OIDN returned");

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
        {
            std::ostringstream msg;
            msg << "denoising begin: oidn_device.waitSemaphoreAsync waiting for fence value "
                << oidn_fence_value;
            DXR_FRAME_DIAGNOSTIC(msg.str());
        }
#endif
        oidn_device.waitSemaphoreAsync(oidn_semaphore, oidn_fence_value);
        DXR_FRAME_DIAGNOSTIC("denoising: oidn_device.waitSemaphoreAsync returned");

        DXR_FRAME_DIAGNOSTIC("denoising: oidn_filter.executeAsync begin");
        oidn_filter.executeAsync();
        DXR_FRAME_DIAGNOSTIC("denoising: oidn_filter.executeAsync returned");

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
        {
            std::ostringstream msg;
            msg << "ID3D12Fence interaction begin: oidn_device.signalSemaphoreAsync value="
                << oidn_fence_value + 1;
            DXR_FRAME_DIAGNOSTIC(msg.str());
        }
#endif
        oidn_device.signalSemaphoreAsync(oidn_semaphore, oidn_fence_value + 1);
        DXR_FRAME_DIAGNOSTIC("ID3D12Fence interaction end: oidn_device.signalSemaphoreAsync returned");

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
        {
            std::ostringstream msg;
            msg << "ID3D12Fence interaction begin: command queue Wait for OIDN fence value "
                << oidn_fence_value + 1;
            DXR_FRAME_DIAGNOSTIC(msg.str());
        }
#endif
        cmd_queue->Wait(fence.Get(), oidn_fence_value + 1);
        DXR_FRAME_DIAGNOSTIC("ID3D12Fence interaction end: command queue Wait for OIDN returned; denoising end");

    } else {
        throw(std::logic_error("Invalid OIDN sync method"));
    }

#endif

    // Tonemap the frame
    {
        ID3D12CommandList *tonemap_cmds = tonemap_cmd_list[slot].Get();
        DXR_FRAME_DIAGNOSTIC("tonemap begin: ExecuteCommandLists(tonemap_cmd_list)");
        cmd_queue->ExecuteCommandLists(1, &tonemap_cmds);
        DXR_FRAME_DIAGNOSTIC("tonemap command list submitted");
    }

#ifdef REPORT_RAY_STATS
    // Ray stats require reading the framebuffer/ray-count textures back, but only
    // pay that cost when the figure is actually being collected.
    const bool need_readback =
        collect_ray_stats || !native_display || readback_framebuffer;
#else
    const bool need_readback = !native_display || readback_framebuffer;
#endif

    if (need_readback) {
        ID3D12CommandList *readback_cmds = readback_cmd_list[slot].Get();
        DXR_FRAME_DIAGNOSTIC("readback begin: ExecuteCommandLists(readback_cmd_list)");
        cmd_queue->ExecuteCommandLists(1, &readback_cmds);
        DXR_FRAME_DIAGNOSTIC("readback command list submitted");
    }

    // Signal this slot's fence once all of its submissions complete. We do not
    // drain the queue here; the host only blocks when it actually needs this
    // slot's framebuffer, or later when reclaiming the slot / reading its stats.
    slot_fence_value[slot] = fence_value++;
    CHECK_ERR(cmd_queue->Signal(fence.Get(), slot_fence_value[slot]));
    slot_submitted[slot] = true;
    DXR_FRAME_DIAGNOSTIC("slot fence signaled");

    if (need_readback) {
        // The displayed / saved framebuffer must be current, so wait on this
        // slot's fence (rather than draining the whole queue) before copying it.
        DXR_FRAME_DIAGNOSTIC("framebuffer CPU readback begin");
        wait_for_fence_value(slot_fence_value[slot]);

        // Copy out the rendered image from the persistently mapped readback buffer.
        // We may have needed some padding for the readback buffer, so we might have to read
        // row by row.
        if (render_target.linear_row_pitch() ==
            render_target.dims().x * render_target.pixel_size()) {
            std::memcpy(img.data(), img_readback_mapping[slot], img_readback_buf[slot].size());
        } else {
            uint8_t *buf = img_readback_mapping[slot];
            for (uint32_t y = 0; y < render_target.dims().y; ++y) {
                std::memcpy(img.data() + y * render_target.dims().x,
                            buf + y * render_target.linear_row_pitch(),
                            render_target.dims().x * render_target.pixel_size());
            }
        }
        DXR_FRAME_DIAGNOSTIC("framebuffer CPU readback end");

#ifdef REPORT_RAY_STATS
        if (collect_ray_stats) {
            if (ray_stats.linear_row_pitch() == ray_stats.dims().x * ray_stats.pixel_size()) {
                std::memcpy(ray_counts.data(),
                            ray_stats_readback_mapping[slot],
                            ray_stats_readback_buf[slot].size());
            } else {
                uint8_t *buf = ray_stats_readback_mapping[slot];
                for (uint32_t y = 0; y < ray_stats.dims().y; ++y) {
                    std::memcpy(ray_counts.data() + y * ray_stats.dims().x,
                                buf + y * ray_stats.linear_row_pitch(),
                                ray_stats.dims().x * ray_stats.pixel_size());
                }
            }

            slot_total_rays[slot] = std::accumulate(
                ray_counts.begin(),
                ray_counts.end(),
                uint64_t(0),
                [](const uint64_t &total, const uint16_t &c) { return total + c; });
        } else {
            slot_total_rays[slot] = 0;
        }
#endif
    }

    // Read back the statistics for the previously completed frame slot. The
    // results lag the displayed frame by one render() call, which lets the host
    // avoid stalling on the GPU work that was just submitted.
    if (slot_submitted[prev_slot]) {
        DXR_FRAME_DIAGNOSTIC("timestamp readback begin");
        wait_for_fence_value(slot_fence_value[prev_slot]);
        const uint64_t *timestamps = query_resolve_mapping[prev_slot];

        stats.frame_time = elapsed_timestamp_ms(timestamps,
                                                timestamp_freq,
                                                TIMING_QUERY_FRAME_BEGIN,
                                                TIMING_QUERY_FRAME_END);
        stats.render_time = elapsed_timestamp_ms(timestamps,
                                                 timestamp_freq,
                                                 TIMING_QUERY_RAYTRACING_BEGIN,
                                                 TIMING_QUERY_RAYTRACING_END);
        stats.tonemap_time = elapsed_timestamp_ms(timestamps,
                                                  timestamp_freq,
                                                  TIMING_QUERY_TONEMAP_BEGIN,
                                                  TIMING_QUERY_FRAME_END);

        // Offset of a timestamp from the frame begin, in milliseconds. Used to
        // place the timeline spans relative to a common frame origin.
        auto offset_ms = [&](TimingQuery query) {
            return static_cast<float>(
                static_cast<double>(timestamps[query] - timestamps[TIMING_QUERY_FRAME_BEGIN]) /
                timestamp_freq * 1e3);
        };

        stats.timeline.clear();
        if (collect_timeline) {
            stats.timeline.push_back({"Ray Tracing",
                                      offset_ms(TIMING_QUERY_RAYTRACING_BEGIN),
                                      offset_ms(TIMING_QUERY_RAYTRACING_END)});
        }
#ifdef ENABLE_OIDN
        // OIDN runs asynchronously in a separate SYCL context, but in this
        // application the ray-tracing, denoise, and tonemap passes are
        // serialized by barriers on shared resources and cannot overlap.
        // Estimate the denoise cost conservatively as the GPU-visible gap
        // between the end of ray tracing and the start of tonemapping, which is
        // an upper bound on the denoise time. The passes are treated as serial.
        stats.denoise_time = elapsed_timestamp_ms(timestamps,
                                                  timestamp_freq,
                                                  TIMING_QUERY_RAYTRACING_END,
                                                  TIMING_QUERY_TONEMAP_BEGIN);
        stats.passes_overlap = false;

        if (collect_timeline) {
            stats.timeline.push_back({"Denoise (est.)",
                                      offset_ms(TIMING_QUERY_RAYTRACING_END),
                                      offset_ms(TIMING_QUERY_TONEMAP_BEGIN)});
        }
#endif
        if (collect_timeline) {
            stats.timeline.push_back({"Tonemap",
                                      offset_ms(TIMING_QUERY_TONEMAP_BEGIN),
                                      offset_ms(TIMING_QUERY_FRAME_END)});
        }

#ifdef REPORT_RAY_STATS
        stats.rays_per_second =
            stats.render_time > 0.f
                ? slot_total_rays[prev_slot] / (stats.render_time * 1.0e-3)
                : 0.0;
#endif
        DXR_FRAME_DIAGNOSTIC("timestamp readback end");
    }

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
    {
        std::ostringstream msg;
        msg << "frame end; render_time_ms=" << stats.render_time
            << ", frame_time_ms=" << stats.frame_time
            << ", denoise_time_ms=" << stats.denoise_time
            << ", tonemap_time_ms=" << stats.tonemap_time
            << ", next_frame_id=" << frame_id + 1
            << ", fence_value=" << fence_value;
        DXR_FRAME_DIAGNOSTIC(msg.str());
    }
    frame_diagnostics_active = false;
#endif

    ++frame_id;
    frame_slot = (frame_slot + 1) % MAX_FRAMES_IN_FLIGHT;
    return stats;
}

void RenderDXR::create_device_objects()
{
    device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
    fence_evt = CreateEvent(nullptr, false, false, nullptr);

    // Create the command queue and command allocator
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK_ERR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&cmd_queue)));
    CHECK_ERR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&cmd_allocator)));

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        CHECK_ERR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&render_cmd_allocator[i])));
    }

    // Make the command lists
    CHECK_ERR(device->CreateCommandList(0,
                                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        cmd_allocator.Get(),
                                        nullptr,
                                        IID_PPV_ARGS(&cmd_list)));
    CHECK_ERR(cmd_list->Close());

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        CHECK_ERR(device->CreateCommandList(0,
                                            D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            render_cmd_allocator[i].Get(),
                                            nullptr,
                                            IID_PPV_ARGS(&render_cmd_list[i])));
        CHECK_ERR(render_cmd_list[i]->Close());

        CHECK_ERR(device->CreateCommandList(0,
                                            D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            render_cmd_allocator[i].Get(),
                                            nullptr,
                                            IID_PPV_ARGS(&tonemap_cmd_list[i])));
        CHECK_ERR(tonemap_cmd_list[i]->Close());

        CHECK_ERR(device->CreateCommandList(0,
                                            D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            render_cmd_allocator[i].Get(),
                                            nullptr,
                                            IID_PPV_ARGS(&readback_cmd_list[i])));
        CHECK_ERR(readback_cmd_list[i]->Close());
    }

    // Allocate a constants buffer for the view parameters.
    // These are write once, read once (assumed to change each frame).
    // The params will be:
    // vec4 cam_pos
    // vec4 cam_du
    // vec4 cam_dv
    // vec4 cam_dir_top_left
    // uint32_t frame_id
    view_param_buf = dxr::Buffer::upload(
        device.Get(),
        align_to(5 * sizeof(glm::vec4), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT),
        D3D12_RESOURCE_STATE_GENERIC_READ);

    // Our query heap stores timestamps for the frame and its major GPU passes,
    // with one set of queries per in-flight frame slot.
    D3D12_QUERY_HEAP_DESC timing_query_heap_desc = {};
    timing_query_heap_desc.Count = TIMING_QUERY_COUNT * MAX_FRAMES_IN_FLIGHT;
    timing_query_heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    device->CreateQueryHeap(&timing_query_heap_desc, IID_PPV_ARGS(&timing_query_heap));

    // The timestamp frequency is fixed for the lifetime of the queue, so cache it
    // once here instead of querying it every frame.
    CHECK_ERR(cmd_queue->GetTimestampFrequency(&timestamp_freq));

    // Per-slot buffer to readback query results in to, kept persistently mapped to
    // avoid per-frame Map/Unmap overhead. The CPU only reads from it.
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        query_resolve_buffer[i] =
            dxr::Buffer::readback(device.Get(),
                                  sizeof(uint64_t) * TIMING_QUERY_COUNT,
                                  D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_RANGE read_range;
        read_range.Begin = 0;
        read_range.End = query_resolve_buffer[i].size();
        query_resolve_mapping[i] =
            static_cast<const uint64_t *>(query_resolve_buffer[i].map(read_range));
    }
}

void RenderDXR::build_raytracing_pipeline()
{
    dxr::ShaderLibrary shader_library(render_dxr_dxil,
                                      sizeof(render_dxr_dxil),
                                      {L"RayGen", L"Miss", L"ClosestHit", L"ShadowMiss"});

    dxr::RootSignature global_root_sig =
        dxr::RootSignatureBuilder::global().create(device.Get());

    // Create the root signature for our ray gen shader
    dxr::RootSignature raygen_root_sig =
        dxr::RootSignatureBuilder::local()
            .add_constants("SceneParams", 1, 1, 0)
            .add_desc_heap("cbv_srv_uav_heap", raygen_desc_heap)
            .add_desc_heap("sampler_heap", raygen_sampler_heap)
            .create(device.Get());

    // Create the root signature for our closest hit function
    dxr::RootSignature hitgroup_root_sig = dxr::RootSignatureBuilder::local()
                                               .add_srv("vertex_buf", 0, 1)
                                               .add_srv("index_buf", 1, 1)
                                               .add_srv("normal_buf", 2, 1)
                                               .add_srv("uv_buf", 3, 1)
                                               .add_constants("MeshData", 0, 3, 1)
                                               .create(device.Get());

    dxr::RTPipelineBuilder rt_pipeline_builder =
        dxr::RTPipelineBuilder()
            .set_global_root_sig(global_root_sig)
            .add_shader_library(shader_library)
            .set_ray_gen(L"RayGen")
            .add_miss_shader(L"Miss")
            .add_miss_shader(L"ShadowMiss")
            .set_shader_root_sig({L"RayGen"}, raygen_root_sig)
            .configure_shader_payload(
                shader_library.export_names(), 8 * sizeof(float), 2 * sizeof(float))
            .set_max_recursion(1);

    // Setup hit groups and shader root signatures for our instances.
    // For now this is also easy since they all share the same programs and root signatures,
    // but we just need different hitgroups to set the different params for the meshes
    std::vector<std::wstring> hg_names;
    for (size_t i = 0; i < parameterized_meshes.size(); ++i) {
        const auto &pm = parameterized_meshes[i];
        for (size_t j = 0; j < meshes[pm.mesh_id].geometries.size(); ++j) {
            const std::wstring hg_name =
                L"HitGroup_param_mesh" + std::to_wstring(i) + L"_geom" + std::to_wstring(j);
            hg_names.push_back(hg_name);

            rt_pipeline_builder.add_hit_group(
                {dxr::HitGroup(hg_name, D3D12_HIT_GROUP_TYPE_TRIANGLES, L"ClosestHit")});
        }
    }
    rt_pipeline_builder.set_shader_root_sig(hg_names, hitgroup_root_sig);

    rt_pipeline = rt_pipeline_builder.create(device.Get());

    // Tonemap
    tonemap_root_sig =
        dxr::RootSignatureBuilder::global()
            .add_desc_heap("cbv_srv_uav_heap", raygen_desc_heap)
            .create(device.Get());

    D3D12_COMPUTE_PIPELINE_STATE_DESC tonemap_pso = {};
    tonemap_pso.pRootSignature = tonemap_root_sig.get();
    tonemap_pso.CS.pShaderBytecode = tonemap_dxil;
    tonemap_pso.CS.BytecodeLength = sizeof(tonemap_dxil);
    tonemap_pso.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    CHECK_ERR(device->CreateComputePipelineState(&tonemap_pso, IID_PPV_ARGS(&tonemap_ps)));
}

void RenderDXR::build_shader_resource_heap()
{
    // The CBV/SRV/UAV resource heap has the pointers/views things to our output image buffer
    // and the top level acceleration structure, and any textures
    int uav_size = 2;
#ifdef ENABLE_OIDN
    uav_size++;
#endif
#if REPORT_RAY_STATS
    uav_size++;
#endif

    raygen_desc_heap = dxr::DescriptorHeapBuilder()
                           .add_uav_range(uav_size, 0, 0)
                           .add_srv_range(3, 0, 0)
                           .add_cbv_range(1, 0, 0)
                           .add_srv_range(!textures.empty() ? textures.size() : 1, 3, 0)
                           .create(device.Get());

    raygen_sampler_heap =
        dxr::DescriptorHeapBuilder().add_sampler_range(1, 0, 0).create(device.Get());
}

void RenderDXR::build_shader_binding_table()
{
    rt_pipeline.map_shader_table();
    {
        uint8_t *map = rt_pipeline.shader_record(L"RayGen");
        const dxr::RootSignature *sig = rt_pipeline.shader_signature(L"RayGen");

        const uint32_t num_lights = light_buf.size() / sizeof(QuadLight);
        std::memcpy(map + sig->offset("SceneParams"), &num_lights, sizeof(uint32_t));

        // Is writing the descriptor heap handle actually needed? It seems to not matter
        // if this is written or not
        D3D12_GPU_DESCRIPTOR_HANDLE desc_heap_handle =
            raygen_desc_heap->GetGPUDescriptorHandleForHeapStart();
        std::memcpy(map + sig->offset("cbv_srv_uav_heap"),
                    &desc_heap_handle,
                    sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));

        desc_heap_handle = raygen_sampler_heap->GetGPUDescriptorHandleForHeapStart();
        std::memcpy(map + sig->offset("sampler_heap"),
                    &desc_heap_handle,
                    sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));
    }
    for (size_t i = 0; i < parameterized_meshes.size(); ++i) {
        const auto &pm = parameterized_meshes[i];
        for (size_t j = 0; j < meshes[pm.mesh_id].geometries.size(); ++j) {
            const std::wstring hg_name =
                L"HitGroup_param_mesh" + std::to_wstring(i) + L"_geom" + std::to_wstring(j);

            auto &geom = meshes[pm.mesh_id].geometries[j];

            uint8_t *map = rt_pipeline.shader_record(hg_name);
            const dxr::RootSignature *sig = rt_pipeline.shader_signature(hg_name);

            D3D12_GPU_VIRTUAL_ADDRESS gpu_handle = geom.vertex_buf->GetGPUVirtualAddress();
            std::memcpy(map + sig->offset("vertex_buf"),
                        &gpu_handle,
                        sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));

            gpu_handle = geom.index_buf->GetGPUVirtualAddress();
            std::memcpy(map + sig->offset("index_buf"),
                        &gpu_handle,
                        sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));

            if (geom.normal_buf.size() != 0) {
                gpu_handle = geom.normal_buf->GetGPUVirtualAddress();
            } else {
                gpu_handle = 0;
            }
            std::memcpy(map + sig->offset("normal_buf"),
                        &gpu_handle,
                        sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));

            if (geom.uv_buf.size() != 0) {
                gpu_handle = geom.uv_buf->GetGPUVirtualAddress();
            } else {
                gpu_handle = 0;
            }
            std::memcpy(
                map + sig->offset("uv_buf"), &gpu_handle, sizeof(D3D12_GPU_DESCRIPTOR_HANDLE));

            const std::array<uint32_t, 3> mesh_data = {
                uint32_t(geom.normal_buf.size() / sizeof(glm::vec3)),
                uint32_t(geom.uv_buf.size() / sizeof(glm::vec2)),
                pm.material_ids[j]};
            std::memcpy(map + sig->offset("MeshData"),
                        mesh_data.data(),
                        mesh_data.size() * sizeof(uint32_t));
        }
    }
    rt_pipeline.unmap_shader_table();

    CHECK_ERR(cmd_list->Reset(cmd_allocator.Get(), nullptr));
    rt_pipeline.upload_shader_table(cmd_list.Get());
    std::array<ID3D12CommandList *, 1> cmd_lists = {cmd_list.Get()};
    CHECK_ERR(cmd_list->Close());
    cmd_queue->ExecuteCommandLists(cmd_lists.size(), cmd_lists.data());
    sync_gpu();
}

void RenderDXR::update_view_parameters(const glm::vec3 &pos,
                                       const glm::vec3 &dir,
                                       const glm::vec3 &up,
                                       const float fovy)
{
    glm::vec2 img_plane_size;
    img_plane_size.y = 2.f * std::tan(glm::radians(0.5f * fovy));
    img_plane_size.x =
        img_plane_size.y * static_cast<float>(render_target.dims().x) / render_target.dims().y;

    const glm::vec3 dir_du = glm::normalize(glm::cross(dir, up)) * img_plane_size.x;
    const glm::vec3 dir_dv = -glm::normalize(glm::cross(dir_du, dir)) * img_plane_size.y;
    const glm::vec3 dir_top_left = dir - 0.5f * dir_du - 0.5f * dir_dv;

    uint8_t *buf = static_cast<uint8_t *>(view_param_buf.map());
    {
        glm::vec4 *vecs = reinterpret_cast<glm::vec4 *>(buf);
        vecs[0] = glm::vec4(pos, 0.f);
        vecs[1] = glm::vec4(dir_du, 0.f);
        vecs[2] = glm::vec4(dir_dv, 0.f);
        vecs[3] = glm::vec4(dir_top_left, 0.f);
    }
    {
        uint32_t *fid = reinterpret_cast<uint32_t *>(buf + 4 * sizeof(glm::vec4));
        *fid = frame_id;
    }

    view_param_buf.unmap();
}

void RenderDXR::build_descriptor_heap()
{
    D3D12_CPU_DESCRIPTOR_HANDLE heap_handle = raygen_desc_heap.cpu_desc_handle();

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {0};

    // Render target
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(render_target.get(), nullptr, &uav_desc, heap_handle);
    heap_handle.ptr +=
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Accum buffer
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = DXGI_FORMAT_UNKNOWN;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_desc.Buffer.FirstElement = 0;
    #ifdef ENABLE_OIDN
         uav_desc.Buffer.StructureByteStride = 3 * sizeof(glm::vec4);
    #else
         uav_desc.Buffer.StructureByteStride = sizeof(glm::vec4);
    #endif
        uav_desc.Buffer.NumElements = accum_buffer.size() / uav_desc.Buffer.StructureByteStride;
        uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        device->CreateUnorderedAccessView(accum_buffer.get(), nullptr, &uav_desc, heap_handle);
        heap_handle.ptr +=
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

#ifdef ENABLE_OIDN
    // Denoise buffer
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = DXGI_FORMAT_UNKNOWN;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_desc.Buffer.FirstElement = 0;
        uav_desc.Buffer.StructureByteStride = sizeof(glm::vec4);
        uav_desc.Buffer.NumElements = denoise_buffer.size() / uav_desc.Buffer.StructureByteStride;
        uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        device->CreateUnorderedAccessView(denoise_buffer.get(), nullptr, &uav_desc, heap_handle);
        heap_handle.ptr +=
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
#endif

#ifdef REPORT_RAY_STATS
    // Ray stats buffer
    device->CreateUnorderedAccessView(ray_stats.get(), nullptr, &uav_desc, heap_handle);
    heap_handle.ptr +=
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
#endif

    // Write the TLAS after the output image in the heap
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC tlas_desc = {0};
        tlas_desc.Format = DXGI_FORMAT_UNKNOWN;
        tlas_desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        tlas_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tlas_desc.RaytracingAccelerationStructure.Location = scene_bvh->GetGPUVirtualAddress();
        device->CreateShaderResourceView(nullptr, &tlas_desc, heap_handle);
        heap_handle.ptr +=
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // Write the material params buffer view
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
        srv_desc.Format = DXGI_FORMAT_UNKNOWN;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Buffer.FirstElement = 0;
        srv_desc.Buffer.NumElements = material_param_buf.size() / sizeof(DisneyMaterial);
        srv_desc.Buffer.StructureByteStride = sizeof(DisneyMaterial);
        srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(material_param_buf.get(), &srv_desc, heap_handle);
        heap_handle.ptr +=
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // Write the light params buffer view
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
        srv_desc.Format = DXGI_FORMAT_UNKNOWN;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Buffer.FirstElement = 0;
        srv_desc.Buffer.NumElements = light_buf.size() / sizeof(QuadLight);
        srv_desc.Buffer.StructureByteStride = sizeof(QuadLight);
        srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(light_buf.get(), &srv_desc, heap_handle);
        heap_handle.ptr +=
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // Write the view params constants buffer
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {0};
    cbv_desc.BufferLocation = view_param_buf->GetGPUVirtualAddress();
    cbv_desc.SizeInBytes = view_param_buf.size();
    device->CreateConstantBufferView(&cbv_desc, heap_handle);
    heap_handle.ptr +=
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Write the SRVs for the textures
    for (auto &t : textures) {
        D3D12_SHADER_RESOURCE_VIEW_DESC tex_desc = {0};
        tex_desc.Format = t.pixel_format();
        tex_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        tex_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tex_desc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(t.get(), &tex_desc, heap_handle);
        heap_handle.ptr +=
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // Write the sampler to the sampler heap
    D3D12_SAMPLER_DESC sampler_desc = {0};
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = 0;
    sampler_desc.MipLODBias = 0.0f;
    sampler_desc.MaxAnisotropy = 1;
    device->CreateSampler(&sampler_desc, raygen_sampler_heap.cpu_desc_handle());
}

void RenderDXR::record_command_lists()
{
    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        record_command_lists_for_slot(slot);
    }
}

void RenderDXR::record_command_lists_for_slot(uint32_t slot)
{
    // Base index of this slot's set of timestamp queries within the shared heap.
    const uint32_t query_base = slot * TIMING_QUERY_COUNT;

    CHECK_ERR(render_cmd_allocator[slot]->Reset());
    CHECK_ERR(render_cmd_list[slot]->Reset(render_cmd_allocator[slot].Get(), nullptr));

   // TODO: We'll need a second desc. heap for the sampler and bind both of them here
    std::array<ID3D12DescriptorHeap *, 2> desc_heaps = {raygen_desc_heap.get(),
                                                        raygen_sampler_heap.get()};
    render_cmd_list[slot]->SetDescriptorHeaps(desc_heaps.size(), desc_heaps.data());
    render_cmd_list[slot]->SetPipelineState1(rt_pipeline.get());
    render_cmd_list[slot]->SetComputeRootSignature(rt_pipeline.global_sig());

    render_cmd_list[slot]->EndQuery(timing_query_heap.Get(),
                                    D3D12_QUERY_TYPE_TIMESTAMP,
                                    query_base + TIMING_QUERY_FRAME_BEGIN);
    render_cmd_list[slot]->EndQuery(timing_query_heap.Get(),
                                    D3D12_QUERY_TYPE_TIMESTAMP,
                                    query_base + TIMING_QUERY_RAYTRACING_BEGIN);

    D3D12_DISPATCH_RAYS_DESC dispatch_rays = rt_pipeline.dispatch_rays(render_target.dims());
    render_cmd_list[slot]->DispatchRays(&dispatch_rays);

    render_cmd_list[slot]->EndQuery(timing_query_heap.Get(),
                                    D3D12_QUERY_TYPE_TIMESTAMP,
                                    query_base + TIMING_QUERY_RAYTRACING_END);

    D3D12_RESOURCE_BARRIER barrier = barrier_uav(accum_buffer);
    render_cmd_list[slot]->ResourceBarrier(1, &barrier);

    render_cmd_list[slot]->EndQuery(timing_query_heap.Get(),
                                    D3D12_QUERY_TYPE_TIMESTAMP,
                                    query_base + TIMING_QUERY_DENOISE_BEGIN);

    CHECK_ERR(render_cmd_list[slot]->Close());

    // Tonemap
    CHECK_ERR(tonemap_cmd_list[slot]->Reset(render_cmd_allocator[slot].Get(), nullptr));
    tonemap_cmd_list[slot]->SetDescriptorHeaps(desc_heaps.size(), desc_heaps.data());
    tonemap_cmd_list[slot]->SetPipelineState(tonemap_ps.Get());
    tonemap_cmd_list[slot]->SetComputeRootSignature(tonemap_root_sig.get());
    tonemap_cmd_list[slot]->SetComputeRootDescriptorTable(0, raygen_desc_heap.gpu_desc_handle());

    tonemap_cmd_list[slot]->EndQuery(timing_query_heap.Get(),
                                     D3D12_QUERY_TYPE_TIMESTAMP,
                                     query_base + TIMING_QUERY_TONEMAP_BEGIN);

    glm::uvec2 dispatch_dim = render_target.dims();
    glm::uvec2 workgroup_dim(16, 16);
    dispatch_dim = (dispatch_dim + workgroup_dim - glm::uvec2(1)) / workgroup_dim;
    tonemap_cmd_list[slot]->Dispatch(dispatch_dim.x, dispatch_dim.y, 1);

    tonemap_cmd_list[slot]->EndQuery(timing_query_heap.Get(),
                                     D3D12_QUERY_TYPE_TIMESTAMP,
                                     query_base + TIMING_QUERY_FRAME_END);

    tonemap_cmd_list[slot]->ResolveQueryData(timing_query_heap.Get(),
                                             D3D12_QUERY_TYPE_TIMESTAMP,
                                             query_base,
                                             TIMING_QUERY_COUNT,
                                             query_resolve_buffer[slot].get(),
                                             0);

    CHECK_ERR(tonemap_cmd_list[slot]->Close());

    // Now copy the rendered image into our readback heap so we can give it back
    // to our simple window to blit the image (TODO: Maybe in the future keep this on the GPU?
    // would we be able to share with GL or need a separate DX window backend?)
    CHECK_ERR(readback_cmd_list[slot]->Reset(render_cmd_allocator[slot].Get(), nullptr));
    {
        // Render target from UA -> Copy Source
        auto b = barrier_transition(render_target, D3D12_RESOURCE_STATE_COPY_SOURCE);
        readback_cmd_list[slot]->ResourceBarrier(1, &b);
#ifdef REPORT_RAY_STATS
        b = barrier_transition(ray_stats, D3D12_RESOURCE_STATE_COPY_SOURCE);
        readback_cmd_list[slot]->ResourceBarrier(1, &b);
#endif

        render_target.readback(readback_cmd_list[slot].Get(), img_readback_buf[slot]);
#ifdef REPORT_RAY_STATS
        ray_stats.readback(readback_cmd_list[slot].Get(), ray_stats_readback_buf[slot]);
#endif

        // Transition the render target back to UA so we can write to it in the next frame
        b = barrier_transition(render_target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        readback_cmd_list[slot]->ResourceBarrier(1, &b);
#ifdef REPORT_RAY_STATS
        b = barrier_transition(ray_stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        readback_cmd_list[slot]->ResourceBarrier(1, &b);
#endif
    }
    CHECK_ERR(readback_cmd_list[slot]->Close());
}

void RenderDXR::sync_gpu()
{
    const uint64_t signal_val = fence_value++;
#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
    {
        std::ostringstream msg;
        msg << "ID3D12Fence interaction begin: sync_gpu Signal value=" << signal_val
            << ", next_fence_value=" << fence_value;
        DXR_FRAME_DIAGNOSTIC(msg.str());
    }
#endif
    CHECK_ERR(cmd_queue->Signal(fence.Get(), signal_val));

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
    {
        std::ostringstream msg;
        msg << "ID3D12Fence interaction end: sync_gpu Signal returned; completed_value="
            << fence->GetCompletedValue();
        DXR_FRAME_DIAGNOSTIC(msg.str());
    }
#endif

    const uint64_t completed_value = fence->GetCompletedValue();
#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
    {
        std::ostringstream msg;
        msg << "ID3D12Fence interaction: sync_gpu GetCompletedValue returned "
            << completed_value << " for target " << signal_val;
        DXR_FRAME_DIAGNOSTIC(msg.str());
    }
#endif

    if (completed_value < signal_val) {
#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
        {
            std::ostringstream msg;
            msg << "ID3D12Fence interaction begin: SetEventOnCompletion value="
                << signal_val;
            DXR_FRAME_DIAGNOSTIC(msg.str());
        }
#endif
        CHECK_ERR(fence->SetEventOnCompletion(signal_val, fence_evt));
#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
        DXR_FRAME_DIAGNOSTIC("ID3D12Fence interaction end: SetEventOnCompletion returned; WaitForSingleObject begin");
#endif
        WaitForSingleObject(fence_evt, INFINITE);
#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
        {
            std::ostringstream msg;
            msg << "ID3D12Fence interaction end: WaitForSingleObject returned; completed_value="
                << fence->GetCompletedValue();
            DXR_FRAME_DIAGNOSTIC(msg.str());
        }
#endif
    } else {
        DXR_FRAME_DIAGNOSTIC("ID3D12Fence interaction: sync_gpu wait skipped because fence already completed");
    }
}

void RenderDXR::wait_for_fence_value(uint64_t value)
{
    if (fence->GetCompletedValue() >= value) {
        DXR_FRAME_DIAGNOSTIC("ID3D12Fence interaction: wait_for_fence_value skipped (already completed)");
        return;
    }

#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
    {
        std::ostringstream msg;
        msg << "ID3D12Fence interaction begin: wait_for_fence_value SetEventOnCompletion value="
            << value;
        DXR_FRAME_DIAGNOSTIC(msg.str());
    }
#endif
    CHECK_ERR(fence->SetEventOnCompletion(value, fence_evt));
    WaitForSingleObject(fence_evt, INFINITE);
#ifdef ENABLE_DXR_FRAME_DIAGNOSTICS
    {
        std::ostringstream msg;
        msg << "ID3D12Fence interaction end: wait_for_fence_value returned; completed_value="
            << fence->GetCompletedValue();
        DXR_FRAME_DIAGNOSTIC(msg.str());
    }
#endif
}
