// Tests of the renderer core (docs/lift-20261006/R_DESIGN.md section 6, stage R2a): the D3D12 device layer, bindless
// descriptors, the per-frame upload ring, copy-queue load batches, the frame graph (culling, validation,
// transitions, pooled targets), multisample resolve, determinism and presentation. Everything runs on WARP, the
// deterministic software rasterizer, with the D3D12 debug layer on; a debug-layer error fails the test.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <array>

#include "render/descriptors.h"
#include "render/frame.h"
#include "render/packet_queue.h"
#include "render/renderer.h"
#include "render/frame_graph.h"
#include "render/frame_loop.h"
#include "render/gpu_device.h"
#include "render/pipelines.h"
#include "render/resources.h"
#include "render/skin_cache.h"
#include "render/snapshot_reads.h"
#include "render/swap_chain.h"

#include "render_ps_solid.h"
#include "render_ps_texture.h"
#include "render_vs_fullscreen.h"
#include "render_vs_triangle.h"

namespace {

using namespace shadow::render;
namespace assets = shadow::assets;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void check_ok(bool ok, const std::string& error, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s: %s\n", what, error.c_str());
        ++g_failures;
    }
}

D3D12_SHADER_BYTECODE bytecode(const unsigned char* data, std::size_t size) {
    return D3D12_SHADER_BYTECODE{data, size};
}

struct Fixture {
    Device device;
    BindlessHeap heap;
    SamplerHeap samplers;
    TexturePool pool;
    FrameLoop frames;
    Com<ID3D12RootSignature> root;
    bool ready = false;

    bool create() {
        std::string error;
        DeviceOptions options;
        options.adapter = DeviceOptions::Adapter::Warp;
        options.debug_layer = true;
        bool ok = device.create(options, &error) && heap.create(device, 1024, 64, 2, &error) &&
                  samplers.create(device, 16, &error) && pool.create(device, heap, 64, 16, &error) &&
                  frames.create(device, heap, 2, 1u << 20, &error) && create_root_signature(device, &root, &error);
        check_ok(ok, error, "renderer core on WARP");
        ready = ok;
        return ok;
    }

    void expect_no_debug_errors(const char* test) {
        for (const std::string& message : device.take_debug_errors()) {
            std::printf("FAIL: %s: debug layer: %s\n", test, message.c_str());
            ++g_failures;
        }
    }
};

// The texture's bytes, read back through a buffer (the graph pass that copies them is a side effect).
struct Readback {
    GpuBuffer buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    std::uint32_t rows = 0;
    std::uint64_t row_bytes = 0;

    bool create(Device& device, const D3D12_RESOURCE_DESC& texture, std::string* error) {
        UINT64 total = 0;
        UINT row_count = 0;
        UINT64 row_size = 0;
        device.d3d()->GetCopyableFootprints(&texture, 0, 1, 0, &layout, &row_count, &row_size, &total);
        rows = row_count;
        row_bytes = row_size;
        return create_buffer(device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST, L"test.readback", &buffer, error);
    }

    void add_pass(FrameGraph& graph, ResourceId source) {
        ImportedBuffer imported;
        imported.resource = buffer.resource.Get();
        imported.state = D3D12_RESOURCE_STATE_COPY_DEST;
        imported.final_state = D3D12_RESOURCE_STATE_COPY_DEST;
        const ResourceId target = graph.import_buffer("readback", imported);
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = layout;
        ID3D12Resource* destination = buffer.resource.Get();
        graph.add_pass(
            "readback",
            [&](PassBuilder& pass) {
                pass.read(source, Access::CopySource);
                pass.write(target, Access::CopyDest, Load::Discard);
                pass.side_effect();
            },
            [=](PassContext& context) {
                D3D12_TEXTURE_COPY_LOCATION to{};
                to.pResource = destination;
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = footprint;
                D3D12_TEXTURE_COPY_LOCATION from{};
                from.pResource = context.resource(source);
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.SubresourceIndex = 0;
                context.list()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            });
    }

    // Tightly packed rows.
    std::vector<std::uint8_t> bytes() {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(row_bytes) * rows);
        void* mapped = nullptr;
        const D3D12_RANGE everything{0, static_cast<SIZE_T>(buffer.size)};
        if (FAILED(buffer.resource->Map(0, &everything, &mapped)))
            return {};
        for (std::uint32_t row = 0; row < rows; ++row) {
            std::memcpy(out.data() + row * row_bytes,
                        static_cast<const std::uint8_t*>(mapped) + layout.Offset + row * layout.Footprint.RowPitch,
                        static_cast<std::size_t>(row_bytes));
        }
        const D3D12_RANGE nothing_written{0, 0};
        buffer.resource->Unmap(0, &nothing_written);
        return out;
    }
};

// ---- device ---------------------------------------------------------------------------------------------------

void device_meets_the_floor(Fixture& f) {
    const DeviceCaps& caps = f.device.caps();
    check(caps.software, "WARP is the software adapter");
    check(caps.shader_model >= D3D_SHADER_MODEL_6_6, "Shader Model 6.6 or newer");
    check(caps.binding_tier >= D3D12_RESOURCE_BINDING_TIER_3, "resource binding tier 3");
    std::printf("render_core_tests: %s, shader model 0x%X, enhanced barriers %d, GPU upload heap %d, tearing %d\n",
                caps.adapter_name.c_str(), static_cast<unsigned>(caps.shader_model), caps.enhanced_barriers ? 1 : 0,
                caps.gpu_upload_heap ? 1 : 0, caps.tearing ? 1 : 0);
}

// A screenshot-sized readback must release its heap after its GPU use is complete.
// Allocation bytes alone miss the defect: the old allocator path left an empty
// 8 MiB block cached indefinitely after a one-shot capture.
void large_readback_releases_capacity(Fixture& f) {
    f.device.wait_idle();
    const auto readback_stats = [&] {
        D3D12MA::TotalStatistics stats{};
        f.device.allocator()->CalculateStatistics(&stats);
        return stats.HeapType[2].Stats;  // D3D12MA's READBACK heap index.
    };
    const auto before = readback_stats();
    for (unsigned capture = 0; capture != 3; ++capture) {
        GpuBuffer buffer;
        std::string error;
        constexpr std::uint64_t bytes = 4ull << 20;
        const bool made = create_buffer(f.device, bytes, D3D12_HEAP_TYPE_READBACK,
                                        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                                        L"test.large_readback", &buffer, &error);
        check_ok(made, error, "create screenshot-sized readback");
        if (!made)
            return;
        const auto live = readback_stats();
        check(live.AllocationBytes >= before.AllocationBytes + bytes,
              "live readback is included in allocator accounting");
        retire(f.device, buffer);
        f.device.wait_idle();
        const auto after = readback_stats();
        check(after.AllocationBytes == before.AllocationBytes &&
                  after.BlockBytes == before.BlockBytes && after.BlockCount == before.BlockCount,
              "completed readback leaves no cached heap capacity after release");
    }
    f.expect_no_debug_errors("large_readback_releases_capacity");
}

// ---- upload ring ------------------------------------------------------------------------------------------------

void upload_ring_segments(Fixture& f) {
    UploadRing ring;
    std::string error;
    check_ok(ring.create(f.device, 4096, 2, &error), error, "ring of two 4 KiB segments");
    ring.begin(0);
    const UploadAllocation a = ring.allocate(100, 256);
    const UploadAllocation b = ring.allocate(100, 256);
    check(a && a.offset == 0, "first allocation at the segment start");
    check(b && b.offset == 256, "second allocation aligned to 256");
    check(b.gpu == a.gpu + 256, "GPU addresses follow the offsets");
    const UploadAllocation c = ring.allocate(4000, 16);
    // F-M1 (HW_REPORT R): past the segment the frame continues in a spill chunk of that segment (counted), instead of
    // the allocation failing; the chunk is mapped, writable and released when the segment is begun again.
    check(c && c.buffer != nullptr && c.buffer != a.buffer && ring.overflow_count() == 0 && ring.spill_count() == 1,
          "an allocation past the segment continues in a spill chunk and is counted");
    if (c)
        std::memset(c.cpu, 0xA5, 4000);
    ring.begin(1);
    const UploadAllocation d = ring.allocate(64, 256);
    check(d && d.offset == 4096, "segment 1 starts after segment 0");
    ring.begin(0);
    const UploadAllocation e = ring.allocate(16, 16);
    check(e && e.offset == 0, "a segment restarts at its beginning");
    ring.destroy(f.device);
}

// ---- bindless descriptors -------------------------------------------------------------------------------------------

void bindless_indices_are_reused_after_the_gpu(Fixture& f) {
    BindlessHeap heap;
    std::string error;
    check_ok(heap.create(f.device, 4, 2, 2, &error), error, "bindless heap 4 + 2 x 2");
    std::uint32_t indices[4];
    for (std::uint32_t& index : indices)
        index = heap.allocate();
    check(indices[0] == 0 && indices[3] == 3, "persistent indices from 0");
    check(heap.allocate() == kNoDescriptor && heap.exhausted_count() == 1, "a full heap refuses and counts");
    // Freed at a point the GPU has not reached yet: not reusable.
    GpuMark future = f.device.mark();
    future.direct += 1;
    heap.free(indices[2], future);
    heap.reclaim(f.device);
    check(heap.allocate() == kNoDescriptor, "a freed index is not reused before the GPU passes its frame");
    f.device.direct().wait(f.device.direct().signal());
    heap.reclaim(f.device);
    check(heap.allocate() == indices[2], "the freed index is reused once the GPU has passed its frame");
    heap.begin_frame(1);
    check(heap.allocate_transient() == 4 + 2, "frame slot 1's transient range");
    check(heap.allocate_transient() == 4 + 3, "transient indices are linear");
    check(heap.allocate_transient() == kNoDescriptor, "a full transient range refuses");
    check(sampler_index(Filter::Anisotropic, Address::Mirror, Address::Mirror) == kSamplerCount - 1,
          "the sampler table covers every filter and address combination");
}

// ---- frame graph: culling, validation, transitions ------------------------------------------------------------------

void graph_culls_and_plans_transitions(Fixture& f) {
    FrameGraph graph;
    graph.reset(1);
    TextureDesc desc;
    desc.width = 16;
    desc.height = 16;
    desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    const ResourceId scene = graph.create_texture("scene", desc);
    const ResourceId blurred = graph.create_texture("blurred", desc);
    const ResourceId unused = graph.create_texture("unused", desc);
    Readback readback;
    std::string error;
    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = 16;
    texture_desc.Height = 16;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    check_ok(readback.create(f.device, texture_desc, &error), error, "readback buffer");
    graph.add_pass("draw scene", [&](PassBuilder& p) { p.write(scene, Access::RenderTarget, Load::Clear); }, nullptr);
    graph.add_pass(
        "blur",
        [&](PassBuilder& p) {
            p.read(scene, Access::ShaderRead);
            p.write(blurred, Access::RenderTarget, Load::Discard);
        },
        nullptr);
    graph.add_pass("nobody reads this", [&](PassBuilder& p) { p.write(unused, Access::RenderTarget, Load::Clear); },
                   nullptr);
    readback.add_pass(graph, blurred);
    check_ok(graph.compile(f.pool, &error), error, "graph compiles");
    const std::vector<std::string> kept = graph.kept_passes();
    check(kept.size() == 3 && kept[0] == "draw scene" && kept[1] == "blur" && kept[2] == "readback",
          "the pass nothing reads is culled; the others keep their order");
    // Pooled targets start as render targets: scene RT -> shader read (blur), blurred RT -> copy source.
    const auto& t = graph.transitions();
    check(t.size() == 2, "two transitions");
    if (t.size() == 2) {
        check(t[0].pass == 1 && t[0].resource == scene && t[0].before == D3D12_RESOURCE_STATE_RENDER_TARGET &&
                  t[0].after == D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
              "blur: scene becomes a shader resource");
        check(t[1].pass == 3 && t[1].resource == blurred && t[1].after == D3D12_RESOURCE_STATE_COPY_SOURCE,
              "readback: blurred becomes a copy source");
    }
    check(f.pool.size() == 2, "only the used textures are pooled");
    f.pool.end_frame(1);

    // The same frame again reuses the pooled textures, now starting from their last states.
    FrameGraph again;
    again.reset(2);
    const ResourceId scene2 = again.create_texture("scene", desc);
    again.add_pass("draw scene", [&](PassBuilder& p) {
        p.write(scene2, Access::RenderTarget, Load::Clear);
        p.side_effect();
    }, nullptr);
    check_ok(again.compile(f.pool, &error), error, "second frame compiles");
    check(f.pool.size() == 2, "pooled textures are reused, not recreated");
    f.pool.end_frame(2);
    // A pause of a few frames (the glare chain's) keeps them: releasing early only doubles the driver's commit.
    f.pool.end_frame(2 + 11);
    check(f.pool.size() == 2 && f.pool.released() == 0 && f.pool.created() == 2,
          "textures unused for a few frames stay pooled");

    // A pass that reads a texture nothing wrote is refused.
    FrameGraph invalid;
    invalid.reset(3);
    const ResourceId never_written = invalid.create_texture("never written", desc);
    invalid.add_pass("reader", [&](PassBuilder& p) {
        p.read(never_written, Access::ShaderRead);
        p.side_effect();
    }, nullptr);
    check(!invalid.compile(f.pool, &error) && error.find("before any pass wrote it") != std::string::npos,
          "reading an unwritten texture is refused");
    FrameGraph adds;
    adds.reset(3);
    const ResourceId blended = adds.create_texture("blended", desc);
    adds.add_pass("blend", [&](PassBuilder& p) {
        p.write(blended, Access::RenderTarget, Load::Keep);
        p.side_effect();
    }, nullptr);
    check(!adds.compile(f.pool, &error), "blending into an unwritten texture is refused");
    FrameGraph both;
    both.reset(3);
    const ResourceId feedback = both.create_texture("feedback", desc);
    both.add_pass("loop", [&](PassBuilder& p) {
        p.write(feedback, Access::RenderTarget, Load::Clear);
        p.read(feedback, Access::ShaderRead);
        p.side_effect();
    }, nullptr);
    check(!both.compile(f.pool, &error), "reading and writing one texture in one pass is refused");
    f.pool.end_frame(3);
    // Unused for the whole keep window: every pooled texture is released.
    f.pool.end_frame(3 + TexturePool::kKeepFrames + 1);
    check(f.pool.size() == 0 && f.pool.released() == f.pool.created(), "textures unused for kKeepFrames are released");
    retire(f.device, readback.buffer);
}

// The target trace (resolution evidence, docs/RESOLUTION_AUDIT.md): each kept pass, the targets it binds as the D3D12
// resources report them, and the viewports it sets - at a size that is no multiple of the console's framebuffer.
void graph_target_trace(Fixture& f) {
    std::string error;
    TextureDesc colour;
    colour.width = 333;
    colour.height = 207;
    colour.format = DXGI_FORMAT_R16G16B16A16_UNORM;
    colour.samples = 4;
    TextureDesc depth = colour;
    depth.format = DXGI_FORMAT_D32_FLOAT;
    const FrameLoop::Frame frame = f.frames.begin();
    FrameGraph graph;
    graph.reset(frame.number);
    const ResourceId c_id = graph.create_texture("view colour", colour);
    const ResourceId d_id = graph.create_texture("view depth", depth);
    graph.add_pass("scene", [&](PassBuilder& p) {
        p.write(c_id, Access::RenderTarget, Load::Clear);
        p.write(d_id, Access::DepthWrite, Load::Clear);
        p.side_effect();
    }, [c_id](PassContext& c) {
        c.full_viewport(c_id);
        c.set_viewport(D3D12_VIEWPORT{10.0f, 20.0f, 100.0f, 50.0f, 0.0f, 1.0f}, D3D12_RECT{10, 20, 110, 70});
    });
    check_ok(graph.compile(f.pool, &error), error, "trace graph compiles");
    std::vector<std::string> lines;
    graph.set_target_trace(&lines);
    graph.execute(frame.list, f.heap, f.samplers, f.root.Get());
    graph.set_target_trace(nullptr);
    f.device.direct().wait(f.frames.submit());
    f.pool.end_frame(frame.number);
    const auto has = [&](const std::string& text) {
        for (const std::string& line : lines)
            if (line.find(text) != std::string::npos)
                return true;
        return false;
    };
    check(lines.size() == 5 && lines[0] == "pass scene", "one pass, two targets, two viewports");
    check(has("render-target 'view colour' 333x207 samples 4 format 11"),
          "the colour target as the D3D12 resource reports it (R16G16B16A16_UNORM, 4 samples)");
    check(has("depth-write 'view depth' 333x207 samples 4"), "the depth target as the resource reports it");
    check(has("viewport 0.0,0.0 333.0x207.0 scissor 0,0-333,207"), "full_viewport is the whole target");
    check(has("viewport 10.0,20.0 100.0x50.0 scissor 10,20-110,70"), "a set viewport is recorded as set");
    f.expect_no_debug_errors("graph_target_trace");
}

// A texture whose kept passes end before another's first hands its pooled texture on; the next holder starts in the
// state the first left it in. Overlapping lifetimes never share. The debug layer checks every barrier's before-state.
void graph_shares_textures_by_lifetime(Fixture& f) {
    std::string error;
    constexpr std::uint32_t kSize = 12;  // a size no other test pools
    TextureDesc red;
    red.width = kSize;
    red.height = kSize;
    red.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    red.clear_color = {1.0f, 0.0f, 0.0f, 1.0f};
    TextureDesc green = red;
    green.clear_color = {0.0f, 1.0f, 0.0f, 1.0f};
    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    readback_desc.Width = kSize;
    readback_desc.Height = kSize;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.Format = red.format;
    readback_desc.SampleDesc.Count = 1;
    Readback first_back, second_back;
    check_ok(first_back.create(f.device, readback_desc, &error), error, "readback (first)");
    check_ok(second_back.create(f.device, readback_desc, &error), error, "readback (second)");

    const std::size_t pooled_before = f.pool.size();
    const FrameLoop::Frame frame = f.frames.begin();
    FrameGraph graph;
    graph.reset(frame.number);
    const ResourceId first = graph.create_texture("first", red);
    const ResourceId second = graph.create_texture("second", green);
    graph.add_pass("clear first", [&](PassBuilder& p) { p.write(first, Access::RenderTarget, Load::Clear); }, nullptr);
    first_back.add_pass(graph, first);
    graph.add_pass("clear second", [&](PassBuilder& p) { p.write(second, Access::RenderTarget, Load::Clear); },
                   nullptr);
    second_back.add_pass(graph, second);
    check_ok(graph.compile(f.pool, &error), error, "sharing graph compiles");
    check(f.pool.size() == pooled_before + 1, "disjoint lifetimes share one pooled texture");
    bool chained = false;
    for (const FrameGraph::Transition& t : graph.transitions()) {
        if (t.resource == second && t.pass == 2)
            chained = t.before == D3D12_RESOURCE_STATE_COPY_SOURCE && t.after == D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    check(chained, "the second holder starts in the state the first left the texture in");
    graph.execute(frame.list, f.heap, f.samplers, f.root.Get());
    f.device.direct().wait(f.frames.submit());
    f.pool.end_frame(frame.number);
    const std::vector<std::uint8_t> a = first_back.bytes(), b = second_back.bytes();
    bool a_red = a.size() == kSize * kSize * 4, b_green = b.size() == kSize * kSize * 4;
    for (std::size_t i = 0; a_red && b_green && i < a.size(); i += 4) {
        a_red = a[i] == 255 && a[i + 1] == 0 && a[i + 2] == 0 && a[i + 3] == 255;
        b_green = b[i] == 0 && b[i + 1] == 255 && b[i + 2] == 0 && b[i + 3] == 255;
    }
    check(a_red && b_green, "each holder's readback has its own contents");

    // Overlapping lifetimes: the first is still read after the second is written.
    const std::size_t pooled_again = f.pool.size();
    FrameGraph overlap;
    overlap.reset(frame.number + 1);
    const ResourceId kept = overlap.create_texture("kept", red);
    const ResourceId later = overlap.create_texture("later", green);
    overlap.add_pass("clear kept", [&](PassBuilder& p) { p.write(kept, Access::RenderTarget, Load::Clear); }, nullptr);
    overlap.add_pass("clear later", [&](PassBuilder& p) { p.write(later, Access::RenderTarget, Load::Clear); },
                     nullptr);
    first_back.add_pass(overlap, kept);
    second_back.add_pass(overlap, later);
    check_ok(overlap.compile(f.pool, &error), error, "overlapping graph compiles");
    check(f.pool.size() == pooled_again + 1, "overlapping lifetimes hold two textures");
    f.pool.end_frame(frame.number + 1);
    retire(f.device, first_back.buffer);
    retire(f.device, second_back.buffer);
    f.expect_no_debug_errors("graph_shares_textures_by_lifetime");
}

// Textures of different descriptions whose lifetimes never meet share one render-target memory (memory aliasing):
// each is activated (aliasing barrier, discard) at its first pass and reads back its own contents; the same graph
// next frame reuses both without creating anything; overlapping lifetimes get memory of their own. The debug layer
// checks the activations and every barrier.
void graph_aliases_memory_by_lifetime(Fixture& f) {
    std::string error;
    TextureDesc red;  // 8 MiB, 16-bit
    red.width = 1024;
    red.height = 1024;
    red.format = DXGI_FORMAT_R16G16B16A16_UNORM;
    red.clear_color = {1.0f, 0.0f, 0.0f, 1.0f};
    TextureDesc green = red;  // 4 MiB: another description, placed in the larger memory
    green.height = 512;
    green.clear_color = {0.0f, 1.0f, 0.0f, 1.0f};
    check(f.pool.aliasable(red) && f.pool.aliasable(green), "1 MiB and larger RGBA16 render targets are placed");
    // The classes whose placement changed 4x output on the measured GPU keep committed memory of their own.
    TextureDesc eight_bit = red;
    eight_bit.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    TextureDesc depth = red;
    depth.format = DXGI_FORMAT_D32_FLOAT;
    depth.samples = 4;
    TextureDesc multisampled = red;
    multisampled.samples = 4;
    check(!f.pool.aliasable(eight_bit) && !f.pool.aliasable(depth) && f.pool.aliasable(multisampled),
          "8-bit single-sampled and depth targets stay committed; multisampled colour is placed");
    auto resource_desc = [](const TextureDesc& t) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = t.width;
        d.Height = t.height;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = t.format;
        d.SampleDesc.Count = 1;
        return d;
    };
    Readback red_back, green_back;
    check_ok(red_back.create(f.device, resource_desc(red), &error), error, "readback (red)");
    check_ok(green_back.create(f.device, resource_desc(green), &error), error, "readback (green)");
    auto contents_ok = [&]() {
        const std::vector<std::uint8_t> a = red_back.bytes(), b = green_back.bytes();
        bool a_red = a.size() == std::size_t{1024} * 1024 * 8, b_green = b.size() == std::size_t{1024} * 512 * 8;
        for (std::size_t i = 0; a_red && i < a.size(); i += 8)
            a_red = a[i] == 255 && a[i + 1] == 255 && a[i + 2] == 0 && a[i + 3] == 0 && a[i + 4] == 0 &&
                    a[i + 5] == 0 && a[i + 6] == 255 && a[i + 7] == 255;
        for (std::size_t i = 0; b_green && i < b.size(); i += 8)
            b_green = b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 255 && b[i + 3] == 255 && b[i + 4] == 0 &&
                      b[i + 5] == 0 && b[i + 6] == 255 && b[i + 7] == 255;
        return a_red && b_green;
    };
    auto disjoint_frame = [&](const char* what, std::size_t* activations) {
        const FrameLoop::Frame frame = f.frames.begin();
        FrameGraph graph;
        graph.reset(frame.number);
        const ResourceId a = graph.create_texture("aliased red", red);
        const ResourceId b = graph.create_texture("aliased green", green);
        graph.add_pass("clear red", [&](PassBuilder& p) { p.write(a, Access::RenderTarget, Load::Clear); }, nullptr);
        red_back.add_pass(graph, a);
        graph.add_pass("clear green", [&](PassBuilder& p) { p.write(b, Access::RenderTarget, Load::Clear); }, nullptr);
        green_back.add_pass(graph, b);
        check_ok(graph.compile(f.pool, &error), error, what);
        *activations = graph.activations().size();
        graph.execute(frame.list, f.heap, f.samplers, f.root.Get());
        f.device.direct().wait(f.frames.submit());
        f.pool.end_frame(frame.number);
    };

    const std::size_t memories_before = f.pool.memories(), pooled_before = f.pool.size();
    std::size_t activations = 0;
    disjoint_frame("aliasing graph compiles", &activations);
    check(f.pool.memories() == memories_before + 1, "disjoint lifetimes of two descriptions share one memory");
    check(f.pool.size() == pooled_before + 2, "each description has its own placed texture");
    check(activations == 2, "each placed texture is activated at its first pass");
    check(contents_ok(), "each texture in the shared memory reads back its own contents");

    disjoint_frame("aliasing graph compiles again", &activations);
    check(f.pool.memories() == memories_before + 1 && f.pool.size() == pooled_before + 2,
          "the next frame reuses the placed textures and their memory");
    check(activations == 2, "and activates them again");
    check(contents_ok(), "the reused textures read back their own contents");

    // Overlapping lifetimes: red is read back after green is cleared.
    {
        const FrameLoop::Frame frame = f.frames.begin();
        FrameGraph graph;
        graph.reset(frame.number);
        const ResourceId a = graph.create_texture("aliased red", red);
        const ResourceId b = graph.create_texture("aliased green", green);
        graph.add_pass("clear red", [&](PassBuilder& p) { p.write(a, Access::RenderTarget, Load::Clear); }, nullptr);
        graph.add_pass("clear green", [&](PassBuilder& p) { p.write(b, Access::RenderTarget, Load::Clear); }, nullptr);
        red_back.add_pass(graph, a);
        green_back.add_pass(graph, b);
        check_ok(graph.compile(f.pool, &error), error, "overlapping aliasing graph compiles");
        check(f.pool.memories() == memories_before + 2, "overlapping lifetimes never share memory");
        graph.execute(frame.list, f.heap, f.samplers, f.root.Get());
        f.device.direct().wait(f.frames.submit());
        f.pool.end_frame(frame.number);
        check(contents_ok(), "overlapping placed textures read back their own contents");
    }
    retire(f.device, red_back.buffer);
    retire(f.device, green_back.buffer);
    f.expect_no_debug_errors("graph_aliases_memory_by_lifetime");
}

// A texture placed alone (the view before the glare exists) moves into larger memory free in its lifetime (the glare
// sources') the frame that memory appears; its old texture and memory are released at that frame's end, not after
// kKeepFrames, and the moved texture reads back its own contents.
void graph_releases_superseded_placements(Fixture& f) {
    std::string error;
    TextureDesc view;  // 16 MiB: larger than any memory the earlier tests leave pooled
    view.width = 2048;
    view.height = 1024;
    view.format = DXGI_FORMAT_R16G16B16A16_UNORM;
    view.clear_color = {0.0f, 0.0f, 1.0f, 1.0f};
    TextureDesc big = view;  // 32 MiB, used before the view in the second frame
    big.height = 2048;
    big.clear_color = {1.0f, 1.0f, 0.0f, 1.0f};
    Readback back;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = view.width;
    d.Height = view.height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = view.format;
    d.SampleDesc.Count = 1;
    check_ok(back.create(f.device, d, &error), error, "readback (superseded)");
    const std::size_t memories_before = f.pool.memories(), pooled_before = f.pool.size();
    auto frame_with = [&](bool with_big, const char* what) {
        const FrameLoop::Frame frame = f.frames.begin();
        FrameGraph graph;
        graph.reset(frame.number);
        if (with_big) {
            const ResourceId b = graph.create_texture("superseded big", big);
            graph.add_pass("clear big", [&](PassBuilder& p) {
                p.write(b, Access::RenderTarget, Load::Clear);
                p.side_effect();
            }, nullptr);
        }
        const ResourceId v = graph.create_texture("superseded view", view);
        graph.add_pass("clear view", [&](PassBuilder& p) { p.write(v, Access::RenderTarget, Load::Clear); }, nullptr);
        back.add_pass(graph, v);
        check_ok(graph.compile(f.pool, &error), error, what);
        graph.execute(frame.list, f.heap, f.samplers, f.root.Get());
        f.device.direct().wait(f.frames.submit());
        f.pool.end_frame(frame.number);
    };
    auto blue = [&]() {
        const std::vector<std::uint8_t> b = back.bytes();
        bool ok = b.size() == std::size_t{2048} * 1024 * 8;
        for (std::size_t i = 0; ok && i < b.size(); i += 8)
            ok = b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 0 && b[i + 4] == 255 && b[i + 5] == 255 &&
                 b[i + 6] == 255 && b[i + 7] == 255;
        return ok;
    };
    frame_with(false, "the view alone compiles");
    check(f.pool.memories() == memories_before + 1 && f.pool.size() == pooled_before + 1,
          "the view alone gets memory of its own");
    frame_with(true, "the view after a larger texture compiles");
    check(f.pool.memories() == memories_before + 1 && f.pool.size() == pooled_before + 2,
          "the view moved into the larger memory; its old texture and memory are released that frame");
    check(blue(), "the moved view reads back its own contents");
    retire(f.device, back.buffer);
    f.expect_no_debug_errors("graph_releases_superseded_placements");
}

// ---- end to end: copy-queue upload, bindless sampling, readback ------------------------------------------------------

void bindless_texture_round_trip(Fixture& f) {
    std::string error;
    constexpr std::uint32_t kSize = 4;
    std::uint8_t texels[kSize * kSize * 4];
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            std::uint8_t* t = texels + (y * kSize + x) * 4;
            t[0] = static_cast<std::uint8_t>(x * 60 + 10);
            t[1] = static_cast<std::uint8_t>(y * 60 + 20);
            t[2] = static_cast<std::uint8_t>((x + y) * 16 + 5);
            t[3] = static_cast<std::uint8_t>(255 - x * y * 10);
        }
    }
    GpuTexture source;
    TextureInfo info;
    info.width = kSize;
    info.height = kSize;
    info.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.name = L"test.source";
    bool ok = create_texture_2d(f.device, info, &source, &error) &&
              create_texture_srv(f.device, f.heap, source, DXGI_FORMAT_UNKNOWN, &error);
    check_ok(ok, error, "source texture");
    if (!ok)
        return;
    {
        UploadBatch batch(f.device);
        SubresourceData data{texels, kSize * 4};
        check_ok(batch.upload_texture(source.resource.Get(), 0, 1, &data, &error), error, "texture staged");
        const std::uint64_t value = batch.submit();
        check(value != 0, "the batch was submitted on the copy queue");
        batch.make_visible(value);
    }

    Com<ID3D12PipelineState> pipeline;
    GraphicsPipelineInfo pso;
    pso.vs = bytecode(kRenderVSFullscreen, sizeof(kRenderVSFullscreen));
    pso.ps = bytecode(kRenderPSTexture, sizeof(kRenderPSTexture));
    pso.color_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.name = L"test.texture";
    check_ok(create_graphics_pipeline(f.device, f.root.Get(), pso, &pipeline, &error), error, "texture pipeline");

    TextureDesc desc;
    desc.width = kSize;
    desc.height = kSize;
    desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Readback readback;
    D3D12_RESOURCE_DESC target_desc = source.desc;
    check_ok(readback.create(f.device, target_desc, &error), error, "readback");

    const FrameLoop::Frame frame = f.frames.begin();
    FrameGraph graph;
    graph.reset(frame.number);
    const ResourceId target = graph.create_texture("target", desc);
    const std::uint32_t texture_index = source.srv;
    ID3D12PipelineState* state = pipeline.Get();
    graph.add_pass(
        "sample", [&](PassBuilder& p) { p.write(target, Access::RenderTarget, Load::Discard); },
        [=](PassContext& c) {
            ID3D12GraphicsCommandList* list = c.list();
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(target);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            c.full_viewport(target);
            list->SetPipelineState(state);
            const std::uint32_t constants[2] = {texture_index,
                                                sampler_index(Filter::Point, Address::Clamp, Address::Clamp)};
            list->SetGraphicsRoot32BitConstants(kRootConstants, 2, constants, 4);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
        });
    readback.add_pass(graph, target);
    check_ok(graph.compile(f.pool, &error), error, "sampling graph compiles");
    graph.execute(frame.list, f.heap, f.samplers, f.root.Get());
    f.device.direct().wait(f.frames.submit());
    f.pool.end_frame(frame.number);

    const std::vector<std::uint8_t> out = readback.bytes();
    check(out.size() == sizeof(texels) && std::memcmp(out.data(), texels, sizeof(texels)) == 0,
          "every texel arrives exactly: copy queue -> bindless SRV + sampler heap -> render target -> readback");
    retire(f.device, readback.buffer);
    retire(f.device, source, &f.heap);
    f.expect_no_debug_errors("bindless_texture_round_trip");
}

// ---- multisampling, resolve and determinism ----------------------------------------------------------------------------

std::vector<std::uint8_t> render_msaa_triangle(Fixture& f, ID3D12PipelineState* pipeline) {
    std::string error;
    constexpr std::uint32_t kSize = 8;
    TextureDesc scene;
    scene.width = kSize;
    scene.height = kSize;
    scene.format = DXGI_FORMAT_R16G16B16A16_UNORM;
    scene.samples = 4;
    TextureDesc resolved = scene;
    resolved.samples = 1;
    Readback readback;
    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    readback_desc.Width = kSize;
    readback_desc.Height = kSize;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    readback_desc.SampleDesc.Count = 1;
    if (!readback.create(f.device, readback_desc, &error)) {
        check_ok(false, error, "readback");
        return {};
    }
    const FrameLoop::Frame frame = f.frames.begin();
    FrameGraph graph;
    graph.reset(frame.number);
    const ResourceId color = graph.create_texture("scene (4x)", scene);
    const ResourceId output = graph.create_texture("resolved", resolved);
    graph.add_pass(
        "triangle", [&](PassBuilder& p) { p.write(color, Access::RenderTarget, Load::Clear); },
        [=](PassContext& c) {
            ID3D12GraphicsCommandList* list = c.list();
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(color);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            c.full_viewport(color);
            list->SetPipelineState(pipeline);
            const float colour[4] = {1.0f, 0.5f, 0.25f, 1.0f};
            const float corners[6] = {-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f};
            std::uint32_t constants[16]{};
            std::memcpy(constants, colour, sizeof(colour));
            std::memcpy(constants + 8, corners, sizeof(corners));
            list->SetGraphicsRoot32BitConstants(kRootConstants, 16, constants, 0);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
        });
    graph.add_pass(
        "resolve",
        [&](PassBuilder& p) {
            p.read(color, Access::ResolveSource);
            p.write(output, Access::ResolveDest, Load::Discard);
        },
        [=](PassContext& c) {
            c.list()->ResolveSubresource(c.resource(output), 0, c.resource(color), 0, DXGI_FORMAT_R16G16B16A16_UNORM);
        });
    readback.add_pass(graph, output);
    check_ok(graph.compile(f.pool, &error), error, "multisample graph compiles");
    graph.execute(frame.list, f.heap, f.samplers, f.root.Get());
    f.device.direct().wait(f.frames.submit());
    f.pool.end_frame(frame.number);
    std::vector<std::uint8_t> out = readback.bytes();
    retire(f.device, readback.buffer);
    return out;
}

void multisample_resolve_is_deterministic(Fixture& f) {
    std::string error;
    Com<ID3D12PipelineState> pipeline;
    GraphicsPipelineInfo pso;
    pso.vs = bytecode(kRenderVSTriangle, sizeof(kRenderVSTriangle));
    pso.ps = bytecode(kRenderPSSolid, sizeof(kRenderPSSolid));
    pso.color_format = DXGI_FORMAT_R16G16B16A16_UNORM;
    pso.samples = 4;
    pso.name = L"test.msaa";
    check_ok(create_graphics_pipeline(f.device, f.root.Get(), pso, &pipeline, &error), error, "4x pipeline");
    const std::vector<std::uint8_t> first = render_msaa_triangle(f, pipeline.Get());
    const std::vector<std::uint8_t> second = render_msaa_triangle(f, pipeline.Get());
    check(first.size() == 8 * 8 * 8, "8 x 8 R16G16B16A16 read back");
    if (first.size() != 8 * 8 * 8)
        return;
    auto texel = [&](std::uint32_t x, std::uint32_t y, std::uint32_t channel) {
        std::uint16_t v;
        std::memcpy(&v, first.data() + (y * 8 + x) * 8 + channel * 2, 2);
        return v;
    };
    check(texel(0, 7, 0) == 65535 && texel(0, 7, 3) == 65535 && (texel(0, 7, 1) == 32767 || texel(0, 7, 1) == 32768) &&
              (texel(0, 7, 2) == 16383 || texel(0, 7, 2) == 16384),
          "a covered pixel holds the colour at 16-bit precision");
    check(texel(7, 0, 0) == 0 && texel(7, 0, 3) == 0, "an uncovered pixel keeps the clear colour");
    bool partial = false;
    for (std::uint32_t i = 0; i < 8; ++i) {
        const std::uint16_t r = texel(i, i, 0);
        partial = partial || (r != 0 && r != 65535);
    }
    check(partial, "pixels on the edge are partially covered (4 samples resolved)");
    check(first == second, "the same frame renders to the same bytes");
    f.expect_no_debug_errors("multisample_resolve_is_deterministic");
}

// ---- the skin cache ----------------------------------------------------------------------------------------------------

void skin_cache_skins_once(Fixture& f) {
    std::string error;
    SkinCache cache;
    check_ok(cache.create(f.device, f.heap, 1024, 2, f.root.Get(), &error), error, "skin cache");
    // Bone 0: identity. Bone 1: a quarter turn about z, then 10 along x.
    BoneMatrix bones[2] = {};
    bones[0].rows[0][0] = bones[0].rows[1][1] = bones[0].rows[2][2] = 1.0f;
    bones[1].rows[0][1] = -1.0f;
    bones[1].rows[0][3] = 10.0f;
    bones[1].rows[1][0] = 1.0f;
    bones[1].rows[2][2] = 1.0f;
    std::vector<SkinVertex> vertices(3);
    for (SkinVertex& v : vertices) {
        v.position[0] = 1.0f;
        v.position[1] = 2.0f;
        v.position[2] = 3.0f;
        v.normal[0] = 1.0f;
        v.normal[1] = 0.0f;
        v.normal[2] = 0.0f;
    }
    vertices[0].weights[0] = 1.0f;  // bone 0 only
    vertices[0].indices = 0;
    vertices[1].weights[0] = 1.0f;  // bone 1 only
    vertices[1].indices = 1;
    vertices[2].weights[0] = 0.5f;  // half of each
    vertices[2].weights[1] = 0.5f;
    vertices[2].indices = 0u | (1u << 8);
    SkinSource source;
    {
        UploadBatch batch(f.device);
        check_ok(create_skin_source(f.device, f.heap, batch, vertices, &source, &error), error, "skin source");
        batch.make_visible(batch.submit());
    }
    GpuBuffer readback;
    check_ok(create_buffer(f.device, 3 * sizeof(SkinnedVertex), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"test.skin_readback", &readback, &error),
             error, "skin readback");

    const FrameLoop::Frame frame = f.frames.begin();
    cache.begin_frame(frame.slot);
    const std::uint32_t first = cache.allocate(3);
    const UploadAllocation palette = f.frames.ring().allocate(sizeof(bones), 256);
    std::memcpy(palette.cpu, bones, sizeof(bones));
    FrameGraph graph;
    graph.reset(frame.number);
    ImportedBuffer cache_buffer;
    cache_buffer.resource = cache.buffer();
    cache_buffer.state = D3D12_RESOURCE_STATE_COMMON;
    cache_buffer.final_state = D3D12_RESOURCE_STATE_COMMON;
    const ResourceId skinned = graph.import_buffer("skin cache", cache_buffer);
    ImportedBuffer out;
    out.resource = readback.resource.Get();
    out.state = D3D12_RESOURCE_STATE_COPY_DEST;
    out.final_state = D3D12_RESOURCE_STATE_COPY_DEST;
    const ResourceId target = graph.import_buffer("readback", out);
    graph.add_pass(
        "skin", [&](PassBuilder& p) { p.write(skinned, Access::UnorderedAccess, Load::Discard); },
        [&](PassContext& c) { cache.dispatch(c.list(), source, palette.gpu, first); });
    graph.add_pass(
        "readback",
        [&](PassBuilder& p) {
            p.read(skinned, Access::CopySource);
            p.write(target, Access::CopyDest, Load::Discard);
            p.side_effect();
        },
        [&](PassContext& c) {
            c.list()->CopyBufferRegion(readback.resource.Get(), 0, cache.buffer(),
                                       static_cast<UINT64>(first) * sizeof(SkinnedVertex), 3 * sizeof(SkinnedVertex));
        });
    check_ok(graph.compile(f.pool, &error), error, "skinning graph compiles");
    graph.execute(frame.list, f.heap, f.samplers, f.root.Get());
    f.device.direct().wait(f.frames.submit());
    f.pool.end_frame(frame.number);

    SkinnedVertex result[3]{};
    void* mapped = nullptr;
    const D3D12_RANGE everything{0, sizeof(result)};
    if (SUCCEEDED(readback.resource->Map(0, &everything, &mapped))) {
        std::memcpy(result, mapped, sizeof(result));
        const D3D12_RANGE nothing{0, 0};
        readback.resource->Unmap(0, &nothing);
    }
    auto equal3 = [](const float* v, float x, float y, float z) { return v[0] == x && v[1] == y && v[2] == z; };
    check(equal3(result[0].position, 1, 2, 3) && equal3(result[0].normal, 1, 0, 0), "bone 0 (identity) keeps the vertex");
    check(equal3(result[1].position, 8, 1, 3) && equal3(result[1].normal, 0, 1, 0),
          "bone 1 turns and moves the position, turns the normal");
    check(equal3(result[2].position, 4.5f, 1.5f, 3) && equal3(result[2].normal, 0.5f, 0.5f, 0),
          "two half weights blend the two poses");
    retire(f.device, readback);
    retire(f.device, source, f.heap);
    cache.destroy();
    f.expect_no_debug_errors("skin_cache_skins_once");
}

// ---- the hand-over and the renderer across threads -------------------------------------------------------------------

void packet_queue_keeps_the_newest() {
    PacketQueue queue(2);
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        FramePacket packet;
        packet.tick = tick;
        queue.push(std::move(packet));
    }
    check(queue.stats().dropped == 1, "a full queue drops its oldest packet");
    FramePacket out;
    check(queue.pop_newest(&out, std::chrono::milliseconds(0)) && out.tick == 3, "the render thread gets the newest");
    check(queue.stats().dropped == 2, "older unrendered packets are dropped and counted");
    check(!queue.pop_newest(&out, std::chrono::milliseconds(1)), "an empty queue times out");
    queue.close();
    check(!queue.pop_newest(&out, std::chrono::milliseconds(1000)), "a closed queue returns at once");
}

void renderer_creates_while_rendering() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    check_ok(renderer.create(options, &error), error, "renderer on WARP");
    // A quad mesh and a texture to draw while another thread creates and releases textures.
    assets::MeshData quad;
    quad.vertex_count = 4;
    quad.positions = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
    quad.uv0 = {0, 1, 1, 1, 0, 0, 1, 0};
    quad.colours.assign(16, 255);
    quad.indices = {0, 1, 2, 2, 1, 3};
    quad.submeshes.push_back(assets::MeshData::Submesh{0, 6, 0});
    const MeshHandle mesh = renderer.create_mesh(Scope::Stage, quad, &error);
    check(static_cast<bool>(mesh), "quad mesh");
    assets::TextureImage image;
    image.width = image.height = 4;
    image.mip_levels = 1;
    image.level_offsets = {0};
    image.rgba.assign(64, 200);
    const TextureHandle texture = renderer.create_texture(Scope::Stage, image, &error);
    check(static_cast<bool>(texture), "texture");

    // The loader creates textures the way the game's loads do (one at a time); half are released at once, the rest
    // with their scope.
    constexpr int kLoads = 300;
    std::atomic<bool> loading{true};
    std::atomic<int> created{0};
    std::thread loader([&] {
        std::string loader_error;
        for (int i = 0; i < kLoads; ++i) {
            const TextureHandle t = renderer.create_texture(Scope::Event, image, &loader_error);
            if (t)
                ++created;
            if (i % 2 == 0)
                renderer.release(t);
        }
        loading.store(false);
    });
    int frames = 0, frames_ok = 0, frames_while_loading = 0;
    while ((frames < 30 || loading.load()) && frames < 100000) {
        const bool during = loading.load();
        const int frame = frames++;
        FrameBuilder builder;
        builder.begin_frame(static_cast<std::uint64_t>(frame), Float4{0, 0, 0, 1});
        Camera camera;
        camera.frame = look_at(Float3{0, 0, -3}, Float3{0, 0, 0}, Float3{0, 1, 0});
        camera.parallel = false;
        camera.near_plane = 0.1f;
        camera.far_plane = 10.0f;
        const ViewId view = builder.view(camera, ViewRect{});
        Clear clear;
        clear.colour = clear.depth = true;
        builder.begin_pass(view, PassKind::Scene, clear);
        Material material;
        material.texture = texture;
        Recipe recipe;
        recipe.cull = Cull::None;
        builder.draw(mesh, 0, material, Float3x4{}, recipe);
        builder.end_pass();
        std::vector<std::uint8_t> pixels;
        if (renderer.render_to_image(builder.finish(), 32, 32, &pixels, &error)) {
            ++frames_ok;
            if (during)
                ++frames_while_loading;
        }
    }
    loader.join();
    check(frames_ok == frames, "every frame rendered while another thread created and released textures");
    check(created.load() == kLoads, "the loader thread created every texture meanwhile");
    renderer.release_scope(Scope::Event);
    check(renderer.stats().textures == 1, "the scope release left only the drawn texture");
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: renderer_creates_while_rendering: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
    std::printf("render_core_tests: %d frames (%d while loading) as %d textures were created on another thread\n",
                frames_ok, frames_while_loading, created.load());
}

// A skinned mesh is drawn where its palette puts it (the skinning pass runs before the draws that read its output),
// its draw's world matrix does not apply (skinned positions are world space), and a palette on a mesh without a
// skin is not drawn.
void renderer_skins_meshes() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    check_ok(renderer.create(options, &error), error, "renderer on WARP");
    assets::MeshData quad;
    quad.vertex_count = 4;
    quad.positions = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, -0.5f, 0.5f, 0, 0.5f, 0.5f, 0};
    quad.colours.assign(16, 255);
    quad.indices = {0, 1, 2, 2, 1, 3};
    quad.submeshes.push_back(assets::MeshData::Submesh{0, 6, 0});
    const MeshHandle plain = renderer.create_mesh(Scope::Stage, quad, &error);
    // The same quad bound to two bones: the left vertices to bone 0, the right ones to bone 1.
    quad.skin_indices = {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
    quad.skin_weights = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const MeshHandle skinned = renderer.create_mesh(Scope::Stage, quad, &error);
    check(plain && skinned, "plain and skinned quads");
    check(renderer.stats().skinned_meshes == 1, "one skinned mesh");

    // `palette` bones; `world` would move the quad off screen if it applied.
    auto render = [&](MeshHandle mesh, const std::vector<Float3x4>& palette, std::vector<std::uint8_t>* pixels) {
        FrameBuilder builder;
        builder.begin_frame(1, Float4{0, 0, 0, 1});
        Camera camera;
        camera.frame = look_at(Float3{0, 0, -3}, Float3{0, 0, 0}, Float3{0, 1, 0});
        camera.near_plane = 0.1f;
        camera.far_plane = 10.0f;
        const ViewId view = builder.view(camera, ViewRect{});
        Clear clear;
        clear.colour = clear.depth = true;
        builder.begin_pass(view, PassKind::Scene, clear);
        const BoneRange bones = builder.bones(palette.data(), static_cast<std::uint32_t>(palette.size()));
        Float3x4 world;
        world.m[1][3] = 5.0f;
        Recipe recipe;
        recipe.cull = Cull::None;
        builder.draw(mesh, 0, Material{}, world, recipe, kNoLights, bones);
        builder.end_pass();
        return renderer.render_to_image(builder.finish(), 32, 32, pixels, &error);
    };
    auto lit = [](const std::vector<std::uint8_t>& pixels, int x, int y) {
        return pixels[(static_cast<std::size_t>(y) * 32 + x) * 4] > 128;
    };
    std::vector<std::uint8_t> pixels;
    const std::vector<Float3x4> identity(2);
    check_ok(render(skinned, identity, &pixels), error, "skinned frame (bind pose)");
    check(lit(pixels, 16, 16) && !lit(pixels, 1, 1), "the bind pose covers the centre, not the corner");
    check(renderer.stats().skin_jobs == 1 && renderer.stats().skinned_vertices == 4, "one skinning job of 4 vertices");

    std::vector<Float3x4> raised(2);
    raised[0].m[1][3] = raised[1].m[1][3] = 2.0f;  // both bones 2 units up: off the top of the view
    check_ok(render(skinned, raised, &pixels), error, "skinned frame (raised)");
    check(!lit(pixels, 16, 16), "the raised pose leaves the centre");

    // Bone 1 (the right vertices) stretched along the world x axis: the right half widens towards one side only.
    std::vector<Float3x4> stretched(2);
    stretched[1].m[0][3] = 0.5f;
    std::vector<std::uint8_t> bind;
    check_ok(render(skinned, identity, &bind), error, "skinned frame (bind pose again)");
    check_ok(render(skinned, stretched, &pixels), error, "skinned frame (stretched)");
    int bind_lit = 0, stretched_lit = 0;
    for (int x = 0; x < 32; ++x) {
        bind_lit += lit(bind, x, 16) ? 1 : 0;
        stretched_lit += lit(pixels, x, 16) ? 1 : 0;
    }
    check(stretched_lit > bind_lit, "moving one bone moves only its vertices (the quad widens)");

    check_ok(render(plain, identity, &pixels), error, "palette on a plain mesh");
    check(renderer.stats().skipped_draws == 1 && !lit(pixels, 16, 16) &&
              renderer.stats().skipped_by_reason[static_cast<std::size_t>(Renderer::SkipReason::SkinMissing)] == 1,
          "a palette on a mesh without a skin is not drawn (counted as skin-missing)");
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: renderer_skins_meshes: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
}

// A delta-morphed mesh is drawn with each draw's own weights (base + sum w_t delta_t, blended by the deformation pass):
// two draws of one mesh in one frame keep their own shapes, a morphed shape still takes the draw's world matrix, and a
// skinned mesh is morphed before it is posed.
void renderer_morphs_meshes() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    check_ok(renderer.create(options, &error), error, "renderer on WARP");
    assets::MeshData quad;
    quad.vertex_count = 4;
    quad.positions = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, -0.5f, 0.5f, 0, 0.5f, 0.5f, 0};
    quad.colours.assign(16, 255);
    quad.indices = {0, 1, 2, 2, 1, 3};
    quad.submeshes.push_back(assets::MeshData::Submesh{0, 6, 0});
    // Target 0 lifts the whole quad by 2 (off the top of the view at weight 1); target 1 moves only the right
    // vertices right by 1 (it widens the quad).
    assets::MeshData::DeltaTarget lift, widen;
    lift.positions = {0, 2, 0, 0, 2, 0, 0, 2, 0, 0, 2, 0};
    widen.positions = {0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0};
    quad.delta_targets = {lift, widen};
    const MeshHandle morphed = renderer.create_mesh(Scope::Stage, quad, &error);
    quad.skin_indices = {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
    quad.skin_weights = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const MeshHandle skinned = renderer.create_mesh(Scope::Stage, quad, &error);
    check(morphed && skinned, "morphed and skinned-morphed quads");
    check(renderer.stats().morphed_meshes == 2 && renderer.stats().skinned_meshes == 1, "two morphed meshes, one skinned");

    struct Draw {
        MeshHandle mesh;
        std::vector<float> weights;
        float world_x = 0.0f;
        bool bones = false;
    };
    auto render = [&](const std::vector<Draw>& draws, std::vector<std::uint8_t>* pixels) {
        FrameBuilder builder;
        builder.begin_frame(1, Float4{0, 0, 0, 1});
        Camera camera;
        camera.frame = look_at(Float3{0, 0, -3}, Float3{0, 0, 0}, Float3{0, 1, 0});
        camera.near_plane = 0.1f;
        camera.far_plane = 10.0f;
        const ViewId view = builder.view(camera, ViewRect{});
        Clear clear;
        clear.colour = clear.depth = true;
        builder.begin_pass(view, PassKind::Scene, clear);
        const std::vector<Float3x4> identity(2);
        for (const Draw& d : draws) {
            const MorphRange morph = builder.morph(d.weights.data(), static_cast<std::uint32_t>(d.weights.size()));
            const BoneRange bones = d.bones ? builder.bones(identity.data(), 2) : BoneRange{};
            Float3x4 world;
            world.m[0][3] = d.world_x;
            Recipe recipe;
            recipe.cull = Cull::None;
            builder.draw(d.mesh, 0, Material{}, world, recipe, kNoLights, bones, morph);
        }
        builder.end_pass();
        return renderer.render_to_image(builder.finish(), 32, 32, pixels, &error);
    };
    auto lit = [](const std::vector<std::uint8_t>& pixels, int x, int y) {
        return pixels[(static_cast<std::size_t>(y) * 32 + x) * 4] > 128;
    };
    auto row_lit = [&](const std::vector<std::uint8_t>& pixels, int y) {
        int n = 0;
        for (int x = 0; x < 32; ++x)
            n += lit(pixels, x, y) ? 1 : 0;
        return n;
    };
    std::vector<std::uint8_t> base, pixels;
    check_ok(render({Draw{morphed, {0.0f, 0.0f}}}, &base), error, "morphed frame (weights 0)");
    check(lit(base, 16, 16) && renderer.stats().morph_jobs == 1, "weights 0 draw the base shape (one morph job)");
    check_ok(render({Draw{morphed, {1.0f, 0.0f}}}, &pixels), error, "morphed frame (lifted)");
    check(!lit(pixels, 16, 16), "target 0 at weight 1 lifts the quad off the centre");
    check_ok(render({Draw{morphed, {0.0f, 1.0f}}}, &pixels), error, "morphed frame (widened)");
    check(row_lit(pixels, 16) > row_lit(base, 16), "target 1 moves only the right vertices (the quad widens)");
    // Two draws of the mesh in one frame with different weights: each keeps its own shape (one job each).
    check_ok(render({Draw{morphed, {1.0f, 0.0f}}, Draw{morphed, {0.0f, 0.0f}}}, &pixels), error, "two weight sets");
    check(lit(pixels, 16, 16) && renderer.stats().morph_jobs == 2 && renderer.stats().skipped_draws == 0,
          "two draws of one morphed mesh with their own weights are both drawn");
    // A morphed shape alone stays in model space: the draw's world matrix moves it.
    check_ok(render({Draw{morphed, {0.0f, 0.0f}, 5.0f}}, &pixels), error, "morphed frame (moved)");
    check(!lit(pixels, 16, 16), "a morphed draw's world matrix applies");
    // Skinned and morphed: morphed first (lifted), then posed by the identity palette; the world matrix does not apply.
    check_ok(render({Draw{skinned, {0.0f, 0.0f}, 5.0f, true}}, &pixels), error, "skinned-morphed frame (base)");
    check(lit(pixels, 16, 16), "a skinned draw ignores the world matrix (base shape at the centre)");
    check_ok(render({Draw{skinned, {1.0f, 0.0f}, 5.0f, true}}, &pixels), error, "skinned-morphed frame (lifted)");
    check(!lit(pixels, 16, 16) && renderer.stats().skin_jobs == 1 && renderer.stats().morph_jobs == 1,
          "a skinned mesh is morphed before it is posed (one job, skinned and morphed)");
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: renderer_morphs_meshes: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
}

// A handle released while a packet that names it waits to be recorded stays valid until the packet is finished: the
// window records the newest packet after its back buffer is free, while the game may already rebuild the mesh.
void renderer_defers_releases() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    check_ok(renderer.create(options, &error), error, "renderer on WARP");
    assets::MeshData quad;
    quad.vertex_count = 4;
    quad.positions = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, -0.5f, 0.5f, 0, 0.5f, 0.5f, 0};
    quad.colours.assign(16, 255);
    quad.indices = {0, 1, 2, 2, 1, 3};
    quad.submeshes.push_back(assets::MeshData::Submesh{0, 6, 0});
    const MeshHandle mesh = renderer.create_mesh(Scope::Stage, quad, &error);
    auto packet = [&](std::uint64_t tick, MeshHandle m) {
        FrameBuilder builder;
        builder.begin_frame(tick, Float4{0, 0, 0, 1});
        Camera camera;
        camera.frame = look_at(Float3{0, 0, -3}, Float3{0, 0, 0}, Float3{0, 1, 0});
        camera.near_plane = 0.1f;
        camera.far_plane = 10.0f;
        const ViewId view = builder.view(camera, ViewRect{});
        Clear clear;
        clear.colour = clear.depth = true;
        builder.begin_pass(view, PassKind::Scene, clear);
        Recipe recipe;
        recipe.cull = Cull::None;
        builder.draw(m, 0, Material{}, Float3x4{}, recipe);
        builder.end_pass();
        return builder.finish();
    };
    // The game built packet 7 with the mesh, then (building packet 8) rebuilt the mesh and released the old one.
    const FramePacket queued = packet(7, mesh);
    renderer.release(mesh, 8);
    check(renderer.stats().pending_releases == 1 && renderer.stats().meshes == 1, "the release waits");
    std::vector<std::uint8_t> pixels;
    check_ok(renderer.render_to_image(queued, 32, 32, &pixels, &error), error, "the queued packet");
    check(pixels[(16 * 32 + 16) * 4] > 128 && renderer.stats().skipped_draws == 0,
          "the queued packet still draws the released mesh");
    renderer.finish_packets(7);
    check(renderer.stats().pending_releases == 1, "finishing packet 7 keeps a release named through packet 8");
    renderer.finish_packets(8);
    check(renderer.stats().pending_releases == 0 && renderer.stats().meshes == 0, "finishing packet 8 releases it");
    check_ok(renderer.render_to_image(packet(9, mesh), 32, 32, &pixels, &error), error, "a stale handle");
    check(renderer.stats().skipped_draws == 1 &&
              renderer.stats().skipped_by_reason[static_cast<std::size_t>(Renderer::SkipReason::MeshMissing)] == 1,
          "after the release the stale handle is not drawn (counted as mesh-missing)");
    // Releases naming finished packets (or no packet) take effect at once.
    const MeshHandle second = renderer.create_mesh(Scope::Stage, quad, &error);
    renderer.release(second, 8);
    const MeshHandle third = renderer.create_mesh(Scope::Stage, quad, &error);
    renderer.release(third);
    check(renderer.stats().pending_releases == 0 && renderer.stats().meshes == 0 &&
              renderer.stats().deferred_releases == 1,
          "releases naming finished packets or no packet are immediate");
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: renderer_defers_releases: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
}

// Particles are camera-facing quads expanded from their records (a rotation turns the quad in the plane of the batch's
// axes), drawn in the pass's order; fog pulls a draw to the fog colour with eye depth.
void renderer_draws_particles_and_fog() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    check_ok(renderer.create(options, &error), error, "renderer on WARP");
    assets::MeshData quad;
    quad.vertex_count = 4;
    quad.positions = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, -0.5f, 0.5f, 0, 0.5f, 0.5f, 0};
    quad.colours.assign(16, 255);
    quad.indices = {0, 1, 2, 2, 1, 3};
    quad.submeshes.push_back(assets::MeshData::Submesh{0, 6, 0});
    const MeshHandle mesh = renderer.create_mesh(Scope::Stage, quad, &error);
    check(static_cast<bool>(mesh), "quad mesh");

    auto frame = [&](FrameBuilder& builder) {
        builder.begin_frame(1, Float4{0, 0, 0, 1});
        Camera camera;
        camera.frame = look_at(Float3{0, 0, -3}, Float3{0, 0, 0}, Float3{0, 1, 0});
        camera.near_plane = 0.1f;
        camera.far_plane = 10.0f;
        Clear clear;
        clear.colour = clear.depth = true;
        builder.begin_pass(builder.view(camera, ViewRect{}), PassKind::Scene, clear);
    };
    auto channel = [](const std::vector<std::uint8_t>& pixels, int x, int y, int c) {
        return static_cast<int>(pixels[(static_cast<std::size_t>(y) * 32 + x) * 4 + c]);
    };
    Recipe recipe;
    recipe.cull = Cull::None;
    std::vector<std::uint8_t> pixels;

    // A long, thin red particle along the batch's right axis; then the same particle turned a quarter.
    Particle bar;
    bar.size[0] = 2.0f;
    bar.size[1] = 0.25f;
    bar.colour = 0xFF0000FFu;  // red, opaque
    ParticleBatch batch;
    batch.right = Float3{1, 0, 0};
    batch.up = Float3{0, 1, 0};
    for (int turn = 0; turn < 2; ++turn) {
        bar.rotation = turn == 0 ? 0.0f : 1.5707964f;
        FrameBuilder builder;
        frame(builder);
        builder.particles(batch, &bar, 1, recipe);
        builder.end_pass();
        check_ok(renderer.render_to_image(builder.finish(), 32, 32, &pixels, &error), error, "particle frame");
        const bool across = channel(pixels, 8, 16, 0) > 128 && channel(pixels, 24, 16, 0) > 128;
        const bool along = channel(pixels, 16, 8, 0) > 128 && channel(pixels, 16, 24, 0) > 128;
        check(channel(pixels, 16, 16, 0) > 128 && channel(pixels, 16, 16, 1) < 64, "the particle is red at its centre");
        check(turn == 0 ? (across && !along) : (along && !across), "the rotation turns the quad");
    }
    check(renderer.stats().particle_batches == 1 && renderer.stats().particles == 1, "one batch of one particle");

    // Fog: the quad at eye depth 3, fog complete from depth 1 on - the fog colour; without fog, white.
    for (int fogged = 0; fogged < 2; ++fogged) {
        Recipe fog = recipe;
        fog.fog = fogged != 0;
        fog.fog_start = 0.0f;
        fog.fog_end = 1.0f;
        fog.fog_colour = Float4{0, 1, 0, 1};
        FrameBuilder builder;
        frame(builder);
        builder.draw(mesh, 0, Material{}, Float3x4{}, fog);
        builder.end_pass();
        check_ok(renderer.render_to_image(builder.finish(), 32, 32, &pixels, &error), error, "fog frame");
        const bool green = channel(pixels, 16, 16, 0) < 64 && channel(pixels, 16, 16, 1) > 192;
        const bool white = channel(pixels, 16, 16, 0) > 192 && channel(pixels, 16, 16, 1) > 192;
        check(fogged != 0 ? green : white, "fog pulls the draw to the fog colour");
    }
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: renderer_draws_particles_and_fog: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
}

// The glare: glare sources drawn onto black over the scene's depth; at the Glare pass the scene gets the sources and a
// halo around them (level A's narrow blur, level B's wide plateau), nothing far away; with both levels off only the
// sources; a source behind scene geometry adds nothing.
void renderer_draws_the_glare() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    check_ok(renderer.create(options, &error), error, "renderer on WARP");
    // A small quad (the glare source) and a large one (an occluder).
    auto quad_mesh = [&](float half) {
        assets::MeshData quad;
        quad.vertex_count = 4;
        quad.positions = {-half, -half, 0, half, -half, 0, -half, half, 0, half, half, 0};
        quad.colours.assign(16, 255);
        quad.indices = {0, 1, 2, 2, 1, 3};
        quad.submeshes.push_back(assets::MeshData::Submesh{0, 6, 0});
        return renderer.create_mesh(Scope::Stage, quad, &error);
    };
    const MeshHandle source = quad_mesh(0.05f);
    const MeshHandle wall = quad_mesh(1.0f);
    check(source && wall, "glare test meshes");
    constexpr int kSize = 256;
    auto render = [&](const GlareParams& params, bool occluded, std::vector<std::uint8_t>* pixels) {
        FrameBuilder builder;
        builder.begin_frame(1, Float4{0, 0, 0, 1});
        Camera camera;
        camera.frame = look_at(Float3{0, 0, -3}, Float3{0, 0, 0}, Float3{0, 1, 0});
        camera.near_plane = 0.1f;
        camera.far_plane = 10.0f;
        const ViewId view = builder.view(camera, ViewRect{});
        Clear clear;
        clear.colour = clear.depth = true;
        Recipe recipe;
        recipe.cull = Cull::None;
        builder.begin_pass(view, PassKind::Scene, clear);
        if (occluded) {
            // A black wall in front of the source.
            Material black;
            black.colour = Float4{0, 0, 0, 1};
            black.modulate = true;
            Float3x4 nearer;
            nearer.m[2][3] = -1.0f;
            builder.draw(wall, 0, black, nearer, recipe);
        }
        builder.end_pass();
        builder.begin_pass(view, PassKind::GlareSources, Clear{});
        builder.draw(source, 0, Material{}, Float3x4{}, recipe);
        builder.end_pass();
        builder.glare(view, params);
        return renderer.render_to_image(builder.finish(), kSize, kSize, pixels, &error);
    };
    auto red = [](const std::vector<std::uint8_t>& pixels, int x, int y) {
        return static_cast<int>(pixels[(static_cast<std::size_t>(y) * kSize + x) * 4]);
    };
    std::vector<std::uint8_t> pixels;
    check_ok(render(GlareParams{}, false, &pixels), error, "glare frame");
    const int c = kSize / 2;
    // The source spans about +/- 5.7 px around the centre (114 px per unit at this camera); level A's texels are
    // 2.7 x 2 px (96 x 128 for a square view), so its halo reaches about 2.7 px past the source and level B's
    // plateau about 10.7 px horizontally and 8 px vertically.
    check(red(pixels, c, c) == 255, "the source itself is added (and saturates with its halo)");
    check(red(pixels, c + 9, c) > 16 && red(pixels, c, c + 9) > 16, "a halo spreads around the source");
    check(red(pixels, c + 60, c) == 0 && red(pixels, 4, 4) == 0, "nothing far from the source");
    check(renderer.stats().glares == 1, "one glare composite");

    GlareParams off;
    off.level_a = off.level_b = false;
    check_ok(render(off, false, &pixels), error, "glare frame without levels");
    check(red(pixels, c, c) == 255 && red(pixels, c + 9, c) == 0, "without levels only the sources are added");

    check_ok(render(GlareParams{}, true, &pixels), error, "occluded glare frame");
    check(red(pixels, c, c) == 0 && red(pixels, c + 9, c) == 0, "a source behind the scene adds nothing");
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: renderer_draws_the_glare: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
}

// Immediate geometry: indexed triangles, a strip and lines, coloured per vertex and placed by the draw's world matrix,
// in the pass's order with the mesh draws.
void renderer_draws_immediate_geometry() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    check_ok(renderer.create(options, &error), error, "renderer on WARP");
    constexpr int kSize = 64;
    auto pixel = [](const std::vector<std::uint8_t>& pixels, int x, int y, int c) {
        return static_cast<int>(pixels[(static_cast<std::size_t>(y) * kSize + x) * 4 + c]);
    };
    FrameBuilder builder;
    builder.begin_frame(1, Float4{0, 0, 0, 1});
    Camera camera;
    camera.frame = look_at(Float3{0, 0, -3}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    const ViewId view = builder.view(camera, ViewRect{});
    Clear clear;
    clear.colour = clear.depth = true;
    builder.begin_pass(view, PassKind::Scene, clear);
    Recipe recipe;
    recipe.cull = Cull::None;
    // A red quad (two indexed triangles) at the centre.
    const ImmediateVertex quad[4] = {{Float3{-0.3f, -0.3f, 0}, 0xFF0000FFu}, {Float3{0.3f, -0.3f, 0}, 0xFF0000FFu},
                                     {Float3{-0.3f, 0.3f, 0}, 0xFF0000FFu}, {Float3{0.3f, 0.3f, 0}, 0xFF0000FFu}};
    const std::uint32_t quad_indices[6] = {0, 1, 2, 2, 1, 3};
    ImmediateDraw triangles;
    builder.immediate(triangles, quad, 4, quad_indices, 6, Float3x4{}, recipe);
    // The same quad as a green strip, moved up by the world matrix.
    const ImmediateVertex strip[4] = {{Float3{-0.3f, -0.15f, 0}, 0xFF00FF00u}, {Float3{0.3f, -0.15f, 0}, 0xFF00FF00u},
                                      {Float3{-0.3f, 0.15f, 0}, 0xFF00FF00u}, {Float3{0.3f, 0.15f, 0}, 0xFF00FF00u}};
    ImmediateDraw strip_draw;
    strip_draw.topology = Topology::TriangleStrip;
    Float3x4 up;
    up.m[1][3] = 0.8f;
    builder.immediate(strip_draw, strip, 4, nullptr, 0, up, recipe);
    // A blue line across the bottom.
    const ImmediateVertex line[2] = {{Float3{-1.0f, -0.8f, 0}, 0xFFFF0000u}, {Float3{1.0f, -0.8f, 0}, 0xFFFF0000u}};
    ImmediateDraw lines;
    lines.topology = Topology::Lines;
    builder.immediate(lines, line, 2, nullptr, 0, Float3x4{}, recipe);
    builder.end_pass();
    std::vector<std::uint8_t> pixels;
    check_ok(renderer.render_to_image(builder.finish(), kSize, kSize, &pixels, &error), error, "immediate frame");
    const int c = kSize / 2;
    check(pixel(pixels, c, c, 0) > 200 && pixel(pixels, c, c, 1) < 50, "the indexed triangles: red at the centre");
    // 0.8 up is about 23 px above the centre at this camera (28.4 px per unit).
    check(pixel(pixels, c, c - 23, 1) > 200 && pixel(pixels, c, c - 23, 0) < 50, "the strip, moved up by its world");
    int blue = 0;
    for (int y = c + 18; y < c + 28; ++y)
        blue += pixel(pixels, c, y, 2) > 128 ? 1 : 0;
    check(blue >= 1, "the line is drawn");
    check(renderer.stats().immediates == 3, "three immediate draws");
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: renderer_draws_immediate_geometry: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
}

// Split screen: two half-screen views drawn after a full-screen one, then the full-screen overlay (over_views). The
// overlay starts from the picture the views under it made (each half's colour survives where the overlay draws
// nothing), a HUD draw in a player's region is placed in that player's half, a draw across the split on the whole
// canvas; and a Continued draw is clipped to the view, not to the canvas's box.
void renderer_splits_the_screen() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    check_ok(renderer.create(options, &error), error, "renderer on WARP");
    constexpr int kWidth = 128, kHeight = 72;
    auto pixel = [](const std::vector<std::uint8_t>& pixels, int x, int y) {
        const std::size_t at = (static_cast<std::size_t>(y) * kWidth + x) * 4;
        return (static_cast<std::uint32_t>(pixels[at]) << 16) | (static_cast<std::uint32_t>(pixels[at + 1]) << 8) |
               pixels[at + 2];
    };
    FrameBuilder builder;
    Clear clear;
    clear.colour = clear.depth = true;
    Recipe recipe;
    recipe.cull = Cull::None;
    recipe.depth_test = false;
    recipe.depth_write = false;
    auto quad = [&](float x0, float y0, float x1, float y1, std::uint32_t colour, Anchor anchor, std::uint32_t region) {
        const ImmediateVertex v[4] = {{Float3{x0, y0, 0.0f}, colour}, {Float3{x1, y0, 0.0f}, colour},
                                      {Float3{x0, y1, 0.0f}, colour}, {Float3{x1, y1, 0.0f}, colour}};
        ImmediateDraw draw;
        draw.topology = Topology::TriangleStrip;
        draw.space = Space::Canvas;
        draw.anchor = anchor;
        draw.region = region;
        builder.immediate(draw, v, 4, nullptr, 0, Float3x4{}, recipe);
    };
    builder.begin_frame(1, Float4{0, 0, 0, 1});
    const ViewId screen = builder.view(Camera{}, ViewRect{}, Canvas{640, 480});
    builder.begin_pass(screen, PassKind::Scene, clear);  // the frame's clear: the whole screen
    builder.end_pass();
    const ViewId left = builder.view(Camera{}, ViewRect{0.0f, 0.0f, 0.5f, 1.0f}, Canvas{320, 480, 2.0f / 3.0f});
    builder.begin_pass(left, PassKind::Scene, clear);
    quad(0, 0, 320, 480, 0xFF0000FFu, Anchor::Fill, kNoRegion);  // red player 1
    builder.end_pass();
    const ViewId right = builder.view(Camera{}, ViewRect{0.5f, 0.0f, 0.5f, 1.0f}, Canvas{320, 480, 2.0f / 3.0f});
    builder.begin_pass(right, PassKind::Scene, clear);
    quad(0, 0, 320, 480, 0xFF00FF00u, Anchor::Fill, kNoRegion);  // green player 2
    builder.end_pass();
    const ViewId overlay = builder.view(Camera{}, ViewRect{}, Canvas{640, 480}, true);
    const std::uint32_t p1 = builder.canvas_region(CanvasRegion{0.0f, 0.0f, 320.0f, 480.0f});
    const std::uint32_t p2 = builder.canvas_region(CanvasRegion{320.0f, 0.0f, 320.0f, 480.0f});
    builder.begin_pass(overlay, PassKind::Scene, clear);
    // canvas units are 72 / 480 = 0.15 px: 0..64 -> 0..9.6 px at player 1's left edge; 576..640 -> 118.4..128 px.
    quad(0, 0, 64, 96, 0xFFFFFFFFu, Anchor::Left, p1);
    quad(576, 0, 640, 96, 0xFFFF0000u, Anchor::Right, p2);
    // Player 1's region centre (160) on its half's centre (32 px): 128..192 -> 27.2..36.8 px.
    quad(128, 200, 192, 280, 0xFFFFFF00u, Anchor::Center, p1);
    // Across the split, the whole canvas: 304..336 -> 61.6..66.4 px.
    quad(304, 400, 336, 470, 0xFF00FFFFu, Anchor::Center, kNoRegion);
    builder.end_pass();
    std::vector<std::uint8_t> pixels;
    check_ok(renderer.render_to_image(builder.finish(), kWidth, kHeight, &pixels, &error), error, "split frame");
    check(pixel(pixels, 40, 40) == 0xFF0000 && pixel(pixels, 100, 40) == 0x00FF00,
          "the overlay keeps both players' pictures under it");
    check(pixel(pixels, 4, 6) == 0xFFFFFF && pixel(pixels, 124, 6) == 0x0000FF,
          "a HUD draw in a player's region holds that viewport's edge");
    check(pixel(pixels, 32, 36) == 0x00FFFF && pixel(pixels, 24, 36) == 0xFF0000,
          "Center in a region: the player's viewport's centre");
    check(pixel(pixels, 64, 65) == 0xFFFF00 && pixel(pixels, 59, 65) == 0xFF0000,
          "a draw across the split is laid out on the whole canvas");
    check(renderer.stats().views_over_views == 1, "one view drawn over the views under it");

    // A Continued draw (a tiled strip's tile) is clipped to the view: -100..20 -> 1..19 px, all of it shown.
    builder.begin_frame(2, Float4{0, 0, 0, 1});
    const ViewId one = builder.view(Camera{}, ViewRect{}, Canvas{640, 480});
    builder.begin_pass(one, PassKind::Scene, clear);
    quad(-100, 400, 20, 440, 0xFFFFFFFFu, Anchor::Continued, kNoRegion);
    builder.end_pass();
    check_ok(renderer.render_to_image(builder.finish(), kWidth, kHeight, &pixels, &error), error, "continued frame");
    check(pixel(pixels, 8, 63) == 0xFFFFFF && pixel(pixels, 17, 63) == 0xFFFFFF,
          "a continued draw reaches past the canvas's box to the view's edge");
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: renderer_splits_the_screen: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
}

// Canvas geometry: a 640 x 480 canvas on a 16:9 view keeps its proportions (s = 0.75) and holds its left edge,
// centre or right edge on the view's; Fill stretches over the whole view; the canvas z is the depth.
void renderer_draws_canvas_geometry() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    check_ok(renderer.create(options, &error), error, "renderer on WARP");
    constexpr int kWidth = 128, kHeight = 72;
    auto pixel = [](const std::vector<std::uint8_t>& pixels, int x, int y) {
        const std::size_t at = (static_cast<std::size_t>(y) * kWidth + x) * 4;
        return (static_cast<std::uint32_t>(pixels[at]) << 16) | (static_cast<std::uint32_t>(pixels[at + 1]) << 8) |
               pixels[at + 2];
    };
    FrameBuilder builder;
    builder.begin_frame(1, Float4{0, 0, 0, 1});
    const ViewId view = builder.view(Camera{}, ViewRect{}, Canvas{640, 480});
    Clear clear;
    clear.colour = clear.depth = true;
    builder.begin_pass(view, PassKind::Scene, clear);
    Recipe recipe;
    recipe.cull = Cull::None;
    auto quad = [&](float x0, float y0, float x1, float y1, float z, std::uint32_t colour, Anchor anchor,
                    const Recipe& r) {
        const ImmediateVertex v[4] = {{Float3{x0, y0, z}, colour}, {Float3{x1, y0, z}, colour},
                                      {Float3{x0, y1, z}, colour}, {Float3{x1, y1, z}, colour}};
        ImmediateDraw draw;
        draw.topology = Topology::TriangleStrip;
        draw.space = Space::Canvas;
        draw.anchor = anchor;
        builder.immediate(draw, v, 4, nullptr, 0, Float3x4{}, r);
    };
    quad(0, 0, 640, 48, 0.0f, 0xFF00FFFFu, Anchor::Fill, recipe);         // yellow band over the whole width
    quad(0, 96, 64, 360, 0.0f, 0xFF0000FFu, Anchor::Left, recipe);        // red: canvas 0..48 -> 0..9.6 px
    quad(576, 96, 640, 360, 0.0f, 0xFF00FF00u, Anchor::Right, recipe);    // green: 592..640 -> 118.4..128 px
    quad(288, 96, 352, 360, 0.0f, 0xFFFF0000u, Anchor::Center, recipe);   // blue: 296..344 -> 59.2..68.8 px
    quad(160, 56, 480, 88, 0.25f, 0xFFFF00FFu, Anchor::Center, recipe);   // magenta, nearer
    quad(160, 56, 480, 88, 0.75f, 0xFFFFFF00u, Anchor::Center, recipe);   // cyan, farther: hidden
    // White, parked partly outside the canvas: -100..20 -> 5..95 (1..19 px), clipped to the canvas's 80..560 (16 px).
    quad(-100, 400, 20, 440, 0.0f, 0xFFFFFFFFu, Anchor::Center, recipe);
    builder.end_pass();
    std::vector<std::uint8_t> pixels;
    check_ok(renderer.render_to_image(builder.finish(), kWidth, kHeight, &pixels, &error), error, "canvas frame");
    check(pixel(pixels, 1, 3) == 0xFFFF00 && pixel(pixels, 64, 3) == 0xFFFF00 && pixel(pixels, 126, 3) == 0xFFFF00,
          "Fill: the band covers the whole width");
    check(pixel(pixels, 4, 40) == 0xFF0000 && pixel(pixels, 11, 40) == 0, "Left: the canvas's left edge stays");
    check(pixel(pixels, 123, 40) == 0x00FF00 && pixel(pixels, 115, 40) == 0, "Right: the canvas's right edge stays");
    check(pixel(pixels, 64, 40) == 0x0000FF && pixel(pixels, 58, 40) == 0, "Center: the centre stays");
    check(pixel(pixels, 64, 11) == 0xFF00FF, "the canvas z is the depth: the nearer quad stays in front");
    check(pixel(pixels, 8, 63) == 0 && pixel(pixels, 17, 63) == 0xFFFFFF,
          "a draw is clipped to its canvas: art parked outside it stays hidden");
    check(renderer.stats().immediates == 7, "seven canvas draws");

    // A canvas authored to be seen 4:3 although its units are not square (640 x 528): centred, it spans the 4:3
    // middle of the 16:9 view, 16..112 px (square units would give 20.4..107.6).
    builder.begin_frame(2, Float4{0, 0, 0, 1});
    const ViewId tall = builder.view(Camera{}, ViewRect{}, Canvas{640, 528, 4.0f / 3.0f});
    builder.begin_pass(tall, PassKind::Scene, clear);
    quad(0, 0, 640, 528, 0.0f, 0xFFFFFFFFu, Anchor::Center, recipe);
    builder.end_pass();
    check_ok(renderer.render_to_image(builder.finish(), kWidth, kHeight, &pixels, &error), error, "tall canvas frame");
    check(pixel(pixels, 14, 36) == 0 && pixel(pixels, 17, 36) == 0xFFFFFF && pixel(pixels, 110, 36) == 0xFFFFFF &&
              pixel(pixels, 113, 36) == 0,
          "a canvas keeps its authored aspect, not its units'");
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: renderer_draws_canvas_geometry: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
}

// Screen images: a snapshot of the scene colour taken mid-frame is sampled by a later draw of the frame (distortion);
// the view's history, saved in one frame, is sampled by the next (feedback); either one missing leaves its draw out.
void renderer_samples_snapshots_and_history() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    check_ok(renderer.create(options, &error), error, "renderer on WARP");
    constexpr int kWidth = 128, kHeight = 96;
    auto pixel = [](const std::vector<std::uint8_t>& pixels, int x, int y, int channel) {
        return static_cast<int>(pixels[(static_cast<std::size_t>(y) * kWidth + x) * 4 + channel]);
    };
    Clear clear;
    clear.colour = clear.depth = true;
    Recipe recipe;
    recipe.cull = Cull::None;
    recipe.depth_test = false;
    recipe.depth_write = false;
    Sampler linear_clamp;
    linear_clamp.filter = Filter::Linear;
    linear_clamp.u = linear_clamp.v = Address::Clamp;
    // A canvas quad (640 x 480 units, full height) of one colour, sampling `source` at the canvas points u0 .. u1
    // across its width (screen images take canvas points).
    auto quad = [&](FrameBuilder& builder, float x0, float x1, std::uint32_t colour, ImageSource source,
                    SnapshotId snapshot, float u0, float u1) {
        const ImmediateVertex v[4] = {{Float3{x0, 0, 0}, colour, {u0, 0}}, {Float3{x1, 0, 0}, colour, {u1, 0}},
                                      {Float3{x0, 480, 0}, colour, {u0, 480}}, {Float3{x1, 480, 0}, colour, {u1, 480}}};
        ImmediateDraw draw;
        draw.topology = Topology::TriangleStrip;
        draw.space = Space::Canvas;
        draw.anchor = Anchor::Fill;
        draw.source = source;
        draw.snapshot = snapshot;
        draw.sampler = linear_clamp;
        builder.immediate(draw, v, 4, nullptr, 0, Float3x4{}, recipe);
    };
    std::vector<std::uint8_t> pixels;
    FrameBuilder builder;

    // No history yet: the trail's draw is left out.
    builder.begin_frame(1, Float4{0, 0, 0, 1});
    ViewId view = builder.view(Camera{}, ViewRect{}, Canvas{640, 480});
    builder.begin_pass(view, PassKind::Scene, clear);
    quad(builder, 0, 640, 0xFFFFFFFFu, ImageSource::History, 0, 0, 640);
    builder.end_pass();
    check_ok(renderer.render_to_image(builder.finish(), kWidth, kHeight, &pixels, &error), error, "frame without history");
    check(pixel(pixels, 64, 48, 0) == 0 && renderer.stats().image_misses == 1, "a history that was never saved draws nothing");

    // Red on the left half; a snapshot; the right half shows the snapshot's left half; the history is saved.
    builder.begin_frame(2, Float4{0, 0, 0, 1});
    view = builder.view(Camera{}, ViewRect{}, Canvas{640, 480});
    builder.begin_pass(view, PassKind::Scene, clear);
    quad(builder, 0, 320, 0xFF0000FFu, ImageSource::Texture, 0, 0, 1);
    const SnapshotId snapshot = builder.snapshot(view);
    builder.begin_pass(view, PassKind::Scene, Clear{});
    quad(builder, 320, 640, 0xFFFFFFFFu, ImageSource::Snapshot, snapshot, 0, 320);
    quad(builder, 0, 640, 0xFFFFFFFFu, ImageSource::Snapshot, snapshot + 1, 0, 640);  // no such snapshot: left out
    builder.end_pass();
    builder.save_history(view);
    check_ok(renderer.render_to_image(builder.finish(), kWidth, kHeight, &pixels, &error), error, "snapshot frame");
    check(pixel(pixels, 32, 48, 0) == 255 && pixel(pixels, 96, 48, 0) == 255 && pixel(pixels, 96, 48, 1) == 0,
          "the snapshot's left half drawn on the right");
    check(renderer.stats().snapshots == 1 && renderer.stats().history_saves == 1 && renderer.stats().image_misses == 2,
          "one snapshot, one history save, one draw of a missing snapshot");

    // The next frame: black, then the history over it at half alpha (127 / 255; the colour is not written as one
    // literal, which the liveness scan would read as a game address).
    builder.begin_frame(3, Float4{0, 0, 0, 1});
    view = builder.view(Camera{}, ViewRect{}, Canvas{640, 480});
    builder.begin_pass(view, PassKind::Scene, clear);
    quad(builder, 0, 640, (127u << 24) | 0xFFFFFFu, ImageSource::History, 0, 0, 640);
    builder.end_pass();
    check_ok(renderer.render_to_image(builder.finish(), kWidth, kHeight, &pixels, &error), error, "history frame");
    const int red = pixel(pixels, 96, 48, 0);
    check(red >= 126 && red <= 130 && pixel(pixels, 96, 48, 1) == 0, "the history blended over the next frame");

    // On a 16:9 view a centred canvas draw that samples the snapshot at the canvas points it covers leaves the picture
    // as it was: the green stripe (canvas 300..340, at 61..67 px) is sampled where it lies.
    constexpr int kWide = 128, kWideHeight = 72;
    auto wide_green = [&](int x) {
        return static_cast<int>(pixels[(static_cast<std::size_t>(36) * kWide + x) * 4 + 1]);
    };
    auto centred = [&](float x0, float x1, std::uint32_t colour, ImageSource source, float u0, float u1) {
        const ImmediateVertex v[4] = {{Float3{x0, 0, 0}, colour, {u0, 0}}, {Float3{x1, 0, 0}, colour, {u1, 0}},
                                      {Float3{x0, 480, 0}, colour, {u0, 480}}, {Float3{x1, 480, 0}, colour, {u1, 480}}};
        ImmediateDraw draw;
        draw.topology = Topology::TriangleStrip;
        draw.space = Space::Canvas;
        draw.anchor = Anchor::Center;
        draw.source = source;
        draw.sampler = linear_clamp;
        builder.immediate(draw, v, 4, nullptr, 0, Float3x4{}, recipe);
    };
    builder.begin_frame(4, Float4{0, 0, 0, 1});
    view = builder.view(Camera{}, ViewRect{}, Canvas{640, 480});
    builder.begin_pass(view, PassKind::Scene, clear);
    centred(300, 340, 0xFF00FF00u, ImageSource::Texture, 0, 1);
    builder.snapshot(view);
    builder.begin_pass(view, PassKind::Scene, Clear{});
    centred(200, 440, 0xFFFFFFFFu, ImageSource::Snapshot, 200, 440);
    builder.end_pass();
    check_ok(renderer.render_to_image(builder.finish(), kWide, kWideHeight, &pixels, &error), error, "wide snapshot");
    check(wide_green(61) == 255 && wide_green(66) == 255 && wide_green(58) == 0 && wide_green(70) == 0,
          "a centred draw samples the snapshot where its canvas points lie");
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: renderer_samples_snapshots_and_history: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
}

// A snapshot resolves only what its readers sample (snapshot_reads.h): the bound of a canvas draw's texture
// coordinates, widened by the half pixel a partly covered pixel evaluates outside its triangle, the bilinear footprint
// and a texel of slack; what cannot be bounded is refused (the whole view is resolved).
void snapshot_reads_bound_the_sampled_texels() {
    // Powers of two, so the rows below are exact in float and the expected rectangles are too.
    constexpr std::uint32_t kW = 128, kH = 64;
    // Canvas units are the view's pixels: clip x = x / 64 - 1, clip y = 1 - y / 32, w = 1; uv fractions u / 128, v / 64.
    const float position[3][4] = {{2.0f / kW, 0, 0, -1}, {0, -2.0f / kH, 0, 1}, {0, 0, 0, 1}};
    const float uv[2][4] = {{1.0f / kW, 0, 0, 0}, {0, 1.0f / kH, 0, 0}};
    auto patch = [](float x0, float y0, float x1, float y1, float du, float dv) {
        return std::array<ImmediateVertex, 4>{{{Float3{x0, y0, 0}, 0xFFFFFFFFu, {x0 + du, y0 + dv}},
                                               {Float3{x1, y0, 0}, 0xFFFFFFFFu, {x1 + du, y0 + dv}},
                                               {Float3{x0, y1, 0}, 0xFFFFFFFFu, {x0 + du, y1 + dv}},
                                               {Float3{x1, y1, 0}, 0xFFFFFFFFu, {x1 + du, y1 + dv}}}};
    };
    // An undisplaced patch over pixels 10..20: one texel per pixel, so half a texel of extrapolation - coordinates
    // 9.5 .. 20.5 read texels 9 .. 21 (floor(t - 1/2) and the next), one slack texel each side: 8 .. 22.
    auto v = patch(10, 10, 20, 20, 0, 0);
    TexelRect rect;
    check(grow_sampled_texels(Topology::TriangleStrip, v.data(), 4, nullptr, 0, position, uv, kW, kH, &rect) &&
              rect.left == 8 && rect.top == 8 && rect.right == 23 && rect.bottom == 23,
          "an undisplaced patch reads the texels under it and their bilinear neighbours");
    // A second draw grows the same snapshot's rectangle.
    auto w = patch(40, 30, 44, 34, 0, 0);
    check(grow_sampled_texels(Topology::TriangleStrip, w.data(), 4, nullptr, 0, position, uv, kW, kH, &rect) &&
              rect.left == 8 && rect.top == 8 && rect.right == 47 && rect.bottom == 37,
          "the rectangle is the union of its readers'");
    // Coordinates past the right edge read the clamped edge column.
    TexelRect edge;
    auto off = patch(10, 10, 20, 20, 140, 0);
    check(grow_sampled_texels(Topology::TriangleStrip, off.data(), 4, nullptr, 0, position, uv, kW, kH, &edge) &&
              edge.left == kW - 1 && edge.right == static_cast<std::int32_t>(kW),
          "a draw sampling beyond the edge keeps the edge texels");
    // Indexed triangles; a triangle without area reads nothing.
    const std::uint32_t degenerate[3] = {0, 1, 1};
    TexelRect none;
    check(grow_sampled_texels(Topology::Triangles, v.data(), 4, degenerate, 3, position, uv, kW, kH, &none) &&
              none.empty(),
          "a triangle without area reads nothing");
    // A stretched texture coordinate widens the extrapolation: 3 texels per pixel across, 1.5 texels each side.
    std::array<ImmediateVertex, 4> stretched = patch(10, 10, 20, 20, 0, 0);
    for (ImmediateVertex& s : stretched)
        s.uv[0] = 3.0f * s.position.x;
    TexelRect wide;
    check(grow_sampled_texels(Topology::TriangleStrip, stretched.data(), 4, nullptr, 0, position, uv, kW, kH, &wide) &&
              wide.left == 27 && wide.right == 64,
          "the texel change per pixel widens the bound");
    // Refused: lines, and a placement whose w is not 1 (not interpolated linearly).
    TexelRect refused;
    check(!grow_sampled_texels(Topology::Lines, v.data(), 2, nullptr, 0, position, uv, kW, kH, &refused),
          "lines are not bounded");
    const float projective[3][4] = {{2.0f / kW, 0, 0, -1}, {0, -2.0f / kH, 0, 1}, {0, 0, 1, 1}};
    auto deep = patch(10, 10, 20, 20, 0, 0);
    deep[0].position.z = 0.5f;
    check(!grow_sampled_texels(Topology::TriangleStrip, deep.data(), 4, nullptr, 0, projective, uv, kW, kH, &refused),
          "a non-affine placement is not bounded");
}

// Without multisampling (msaa_samples 1) every resolve of the frame - the view's, the glare sources', a snapshot - is
// a copy, and the frame renders like the multisampled one.
void renderer_renders_without_multisampling() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    options.msaa_samples = 1;
    check_ok(renderer.create(options, &error), error, "single-sampled renderer on WARP");
    constexpr int kSize = 64;
    Recipe recipe;
    recipe.cull = Cull::None;
    recipe.depth_test = false;
    auto quad = [&](FrameBuilder& builder, float x0, float x1, std::uint32_t colour, ImageSource source) {
        const ImmediateVertex v[4] = {{Float3{x0, 0, 0}, colour, {x0, 0}}, {Float3{x1, 0, 0}, colour, {x1, 0}},
                                      {Float3{x0, 480, 0}, colour, {x0, 480}}, {Float3{x1, 480, 0}, colour, {x1, 480}}};
        ImmediateDraw draw;
        draw.topology = Topology::TriangleStrip;
        draw.space = Space::Canvas;
        draw.anchor = Anchor::Fill;
        draw.source = source;
        draw.sampler.filter = Filter::Point;
        draw.sampler.u = draw.sampler.v = Address::Clamp;
        builder.immediate(draw, v, 4, nullptr, 0, Float3x4{}, recipe);
    };
    FrameBuilder builder;
    builder.begin_frame(1, Float4{0, 0, 0, 1});
    const ViewId view = builder.view(Camera{}, ViewRect{}, Canvas{640, 480});
    Clear clear;
    clear.colour = clear.depth = true;
    builder.begin_pass(view, PassKind::Scene, clear);
    quad(builder, 0, 320, 0xFF0000FFu, ImageSource::Texture);  // red left half
    builder.snapshot(view);
    builder.begin_pass(view, PassKind::GlareSources, Clear{});
    quad(builder, 600, 640, 0xFF00FF00u, ImageSource::Texture);  // a green glare source at the right edge
    builder.glare(view, GlareParams{});
    builder.begin_pass(view, PassKind::Scene, Clear{});
    quad(builder, 320, 400, 0xFFFFFFFFu, ImageSource::Snapshot);  // the snapshot's own picture there: black
    builder.end_pass();
    std::vector<std::uint8_t> pixels;
    check_ok(renderer.render_to_image(builder.finish(), kSize, kSize, &pixels, &error), error, "single-sampled frame");
    auto pixel = [&](int x, int y, int c) {
        return static_cast<int>(pixels[(static_cast<std::size_t>(y) * kSize + x) * 4 + c]);
    };
    check(pixel(8, 32, 0) == 255 && pixel(36, 32, 0) == 0 && pixel(62, 32, 1) > 200,
          "single-sampled: the scene, a snapshot draw and the glare sources' addition");
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: renderer_renders_without_multisampling: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
}

// The game's loads stream resources one at a time through a ring far smaller than the data: the ring wraps, waits
// for the copy queue when full, gives an upload larger than itself a staging buffer of its own, and every byte
// arrives in place.
void upload_stream_wraps_and_delivers(Fixture& f) {
    std::string error;
    UploadStream stream;
    check_ok(stream.create(f.device, 64u << 10, &error), error, "upload stream");
    constexpr std::uint32_t kCount = 40;
    constexpr std::uint64_t kBytes = 12u << 10;
    constexpr std::uint64_t kLarge = 200u << 10;  // larger than the ring
    std::vector<GpuBuffer> targets(kCount + 1);
    std::vector<std::vector<std::uint8_t>> expected(kCount + 1);
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i <= kCount; ++i) {
        const std::uint64_t bytes = i == kCount ? kLarge : kBytes;
        expected[i].resize(bytes);
        for (std::uint64_t b = 0; b < bytes; ++b)
            expected[i][b] = static_cast<std::uint8_t>(i * 37u + b * 13u + (b >> 8));
        check_ok(create_buffer(f.device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COMMON, L"test.stream_target", &targets[i], &error),
                 error, "stream target");
        check_ok(stream.upload_buffer(targets[i].resource.Get(), 0, expected[i].data(), bytes, &error), error,
                 "streamed upload");
        if (i % 7 == 3)
            stream.flush();  // the render thread's per-frame flushes, at arbitrary points
        total += bytes;
    }
    const std::uint64_t value = stream.flush();
    check(value != 0 && !stream.pending(), "everything recorded was submitted");
    check(stream.stats().ring_waits > 0, "the uploads went round the ring and waited for the copy queue");
    check(stream.stats().dedicated == 1, "the upload larger than the ring had its own staging");

    GpuBuffer readback;
    check_ok(create_buffer(f.device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"test.stream_readback", &readback, &error),
             error, "stream readback");
    f.device.direct().wait_for(f.device.copy(), value);
    const FrameLoop::Frame frame = f.frames.begin();
    std::uint64_t at = 0;
    for (std::uint32_t i = 0; i <= kCount; ++i) {
        frame.list->CopyBufferRegion(readback.resource.Get(), at, targets[i].resource.Get(), 0, expected[i].size());
        at += expected[i].size();
    }
    f.device.direct().wait(f.frames.submit());
    void* mapped = nullptr;
    const D3D12_RANGE everything{0, static_cast<SIZE_T>(total)};
    bool same = SUCCEEDED(readback.resource->Map(0, &everything, &mapped));
    at = 0;
    for (std::uint32_t i = 0; same && i <= kCount; ++i) {
        same = std::memcmp(static_cast<const std::uint8_t*>(mapped) + at, expected[i].data(), expected[i].size()) == 0;
        at += expected[i].size();
    }
    if (mapped != nullptr) {
        const D3D12_RANGE nothing_written{0, 0};
        readback.resource->Unmap(0, &nothing_written);
    }
    check(same, "every streamed byte arrives in place");
    std::printf("render_core_tests: upload stream: %llu submissions, %llu ring waits, %llu bytes\n",
                static_cast<unsigned long long>(stream.stats().submissions),
                static_cast<unsigned long long>(stream.stats().ring_waits),
                static_cast<unsigned long long>(stream.stats().staged_bytes));
    stream.destroy();
    retire(f.device, readback);
    for (GpuBuffer& target : targets)
        retire(f.device, target);
    f.expect_no_debug_errors("upload_stream_wraps_and_delivers");
}

// ---- presentation --------------------------------------------------------------------------------------------------

LRESULT CALLBACK test_window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    return DefWindowProcW(window, message, w, l);
}

void swap_chain_presents_and_resizes(Fixture& f) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = test_window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"shadow.render_core_tests";
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"render core test", WS_OVERLAPPEDWINDOW, 0, 0, 64,
                                  64, nullptr, nullptr, window_class.hInstance, nullptr);
    check(window != nullptr, "hidden test window");
    if (window == nullptr)
        return;
    std::string error;
    {
        SwapChain swap_chain;
        check_ok(swap_chain.create(f.device, window, 64, 64, SwapChainOptions{}, &error), error, "swap chain");
        D3D12_RESOURCE_STATES states[4] = {D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
                                           D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT};
        for (int n = 0; n < 4; ++n) {
            if (n == 3)
                check_ok(swap_chain.resize(32, 32, &error), error, "resize to 32 x 32");
            swap_chain.wait_for_frame(1000);
            const FrameLoop::Frame frame = f.frames.begin();
            const std::uint32_t index = swap_chain.current();
            FrameGraph graph;
            graph.reset(frame.number);
            ImportedTexture back;
            back.resource = swap_chain.back_buffer(index);
            back.state = states[index];
            back.final_state = D3D12_RESOURCE_STATE_PRESENT;
            back.rtv = swap_chain.rtv(index);
            back.desc.width = swap_chain.width();
            back.desc.height = swap_chain.height();
            back.desc.format = swap_chain.format();
            back.desc.clear_color = {0.1f * static_cast<float>(n), 0.2f, 0.3f, 1.0f};
            const ResourceId target = graph.import_texture("back buffer", back);
            graph.add_pass("clear", [&](PassBuilder& p) { p.write(target, Access::RenderTarget, Load::Clear); },
                           nullptr);
            check_ok(graph.compile(f.pool, &error), error, "present graph compiles");
            graph.execute(frame.list, f.heap, f.samplers, f.root.Get());
            check(graph.state_after(target) == D3D12_RESOURCE_STATE_PRESENT, "the back buffer ends in PRESENT");
            states[index] = graph.state_after(target);
            f.frames.submit();
            f.pool.end_frame(frame.number);
            check_ok(swap_chain.present(PresentMode::VSync, &error), error, "present");
        }
        check(swap_chain.width() == 32 && swap_chain.height() == 32, "resized");
    }
    DestroyWindow(window);
    f.expect_no_debug_errors("swap_chain_presents_and_resizes");
}


// The resource worker (render/resource_worker.h, ES 2026-10-19). Asking for a resource returns its handle at once and
// a job makes it; three things must hold for that to change nothing but who pays for it:
//   * handle values are those the synchronous path gives, for the same asks and releases in the same order (they are
//     written into guest memory) - also when a resource is released before its job ran;
//   * a packet naming a resource still being made draws it: the picture is the synchronous path's, bit for bit
//     (the same decode, mip chain and upload, recorded after the job);
//   * nothing is left behind: releases of pending resources retire what their jobs made, the counts return.
void renderer_makes_resources_on_its_worker() {
    std::string error;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    Renderer synchronous, asynchronous;
    check_ok(synchronous.create(options, &error), error, "renderer on WARP (synchronous asks)");
    check_ok(asynchronous.create(options, &error), error, "renderer on WARP (asks made by the worker)");

    // A stored 16 x 16 RGB565 texture with a mip chain, as the game hands one over: big-endian texels in tiles.
    assets::TextureRecord record;
    record.width = record.height = 16;
    record.encoding = assets::TexelEncoding::RGB565;
    std::vector<std::uint8_t> stored(assets::level_bytes(record.encoding, 16, 16));
    std::uint32_t seed = 0x1234567u;
    for (std::uint8_t& b : stored) {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<std::uint8_t>(seed >> 24);
    }
    const auto source_of = [&] {
        Renderer::TextureSource source;
        source.record = record;
        source.texels = stored;
        source.options.generate_mips = true;
        return source;
    };
    assets::TextureImage decoded;
    {
        assets::TextureRecord view = record;
        view.texels = assets::ByteView{stored.data(), stored.size()};
        assets::ImportOptions import_options;
        import_options.generate_mips = true;
        check_ok(assets::import_texture(view, import_options, &decoded, &error), error, "the record decodes");
    }
    assets::MeshData quad;
    quad.vertex_count = 4;
    quad.positions = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
    quad.uv0 = {0, 1, 1, 1, 0, 0, 1, 0};
    quad.colours.assign(16, 255);
    quad.indices = {0, 1, 2, 2, 1, 3};
    quad.submeshes.push_back(assets::MeshData::Submesh{0, 6, 0});

    // 1. Handle values: the same asks and releases, synchronous vs made by the worker. Many asks queue behind each
    //    other, so most releases below hit a resource whose job has not run yet.
    std::vector<std::uint32_t> sync_handles, async_handles;
    for (int round = 0; round < 3; ++round) {
        std::vector<TextureHandle> s_textures, a_textures;
        std::vector<MeshHandle> s_meshes, a_meshes;
        for (int i = 0; i < 40; ++i) {
            s_textures.push_back(synchronous.create_texture(Scope::Event, decoded, &error));
            a_textures.push_back(asynchronous.request_texture(Scope::Event, source_of(), &error));
            s_meshes.push_back(synchronous.create_mesh(Scope::Event, quad, &error));
            a_meshes.push_back(asynchronous.request_mesh(Scope::Event, quad, &error));
        }
        for (int i = 0; i < 40; i += 3) {
            synchronous.release(s_textures[i]);
            asynchronous.release(a_textures[i]);
            synchronous.release(s_meshes[i + 1 < 40 ? i + 1 : i]);
            asynchronous.release(a_meshes[i + 1 < 40 ? i + 1 : i]);
        }
        for (int i = 0; i < 40; ++i) {
            sync_handles.push_back(s_textures[i].value);
            sync_handles.push_back(s_meshes[i].value);
            async_handles.push_back(a_textures[i].value);
            async_handles.push_back(a_meshes[i].value);
        }
    }
    check(sync_handles == async_handles, "the worker's handles are the synchronous path's, release before make included");
    synchronous.release_scope(Scope::Event);
    asynchronous.release_scope(Scope::Event);
    check(synchronous.stats().textures == 0 && asynchronous.stats().textures == 0 &&
              synchronous.stats().meshes == 0 && asynchronous.stats().meshes == 0,
          "every resource released, made or still being made");

    // 2. Pictures: a packet that names a texture and a mesh asked for just before draws them as the synchronous path.
    const auto picture = [&](Renderer& renderer, MeshHandle mesh, TextureHandle texture, std::uint64_t tick) {
        FrameBuilder builder;
        builder.begin_frame(tick, Float4{0, 0, 0, 1});
        Camera camera;
        camera.frame = look_at(Float3{0, 0, -3}, Float3{0, 0, 0}, Float3{0, 1, 0});
        camera.parallel = false;
        camera.near_plane = 0.1f;
        camera.far_plane = 10.0f;
        const ViewId view = builder.view(camera, ViewRect{});
        Clear clear;
        clear.colour = clear.depth = true;
        builder.begin_pass(view, PassKind::Scene, clear);
        Material material;
        material.texture = texture;
        Recipe recipe;
        recipe.cull = Cull::None;
        builder.draw(mesh, 0, material, Float3x4{}, recipe);
        builder.end_pass();
        std::vector<std::uint8_t> pixels;
        std::string render_error;
        check_ok(renderer.render_to_image(builder.finish(), 64, 64, &pixels, &render_error), render_error,
                 "the frame renders");
        return pixels;
    };
    for (int attempt = 0; attempt < 4; ++attempt) {
        const TextureHandle s_texture = synchronous.create_texture(Scope::Stage, decoded, &error);
        const MeshHandle s_mesh = synchronous.create_mesh(Scope::Stage, quad, &error);
        // Queue work ahead of the drawn resources, so the frame below is recorded while they are still being made.
        for (int i = 0; i < 20 * attempt; ++i)
            asynchronous.release(asynchronous.request_texture(Scope::Event, source_of(), &error));
        const TextureHandle a_texture = asynchronous.request_texture(Scope::Stage, source_of(), &error);
        const MeshHandle a_mesh = asynchronous.request_mesh(Scope::Stage, quad, &error);
        const std::vector<std::uint8_t> expected = picture(synchronous, s_mesh, s_texture, 1u + attempt);
        const std::vector<std::uint8_t> drawn = picture(asynchronous, a_mesh, a_texture, 1u + attempt);
        check(!expected.empty() && expected == drawn,
              "a resource still being made when its packet records is drawn as the synchronous path draws it");
        synchronous.finish_packets(1u + attempt);
        asynchronous.finish_packets(1u + attempt);
    }
    asynchronous.release_scope(Scope::Event);
    std::printf("render_core_tests: resource worker - %llu jobs, %llu run by waiting threads, %llu draws recorded "
                "while their resource was made\n",
                static_cast<unsigned long long>(asynchronous.worker_stats().submitted),
                static_cast<unsigned long long>(asynchronous.worker_stats().run_by_waiters),
                static_cast<unsigned long long>(asynchronous.stats().resolve_waits));
    check(asynchronous.stats().resources_failed == 0, "no job failed");
    for (Renderer* r : {&synchronous, &asynchronous}) {
        for (const std::string& message : r->take_debug_errors()) {
            std::printf("FAIL: renderer_makes_resources_on_its_worker: debug layer: %s\n", message.c_str());
            ++g_failures;
        }
    }
}

// C3 (HW_REPORT S): a rendered packet handed back to its builder (FrameBuilder::recycle) gives the next frame its
// vectors' capacity and nothing else. The next packet holds only what its frame described (sizes as a fresh
// builder's), renders the same pixels as a fresh builder's packet, the renderer reads no packet once
// render_to_image has returned, discard() keeps a handed-back packet, and packets handed back from another thread
// while frames are built (the UI thread / game thread pattern) never leave a frame with another frame's contents.
void frame_builder_recycles_rendered_packets() {
    std::string error;
    Renderer renderer;
    RendererOptions options;
    options.device.adapter = DeviceOptions::Adapter::Warp;
    options.device.debug_layer = true;
    check_ok(renderer.create(options, &error), error, "renderer on WARP");
    constexpr int kSize = 32;
    auto describe = [](FrameBuilder& builder, std::uint64_t tick, std::uint32_t colour, int quads) {
        builder.begin_frame(tick, Float4{0, 0, 0, 1});
        Camera camera;
        camera.frame = look_at(Float3{0, 0, -3}, Float3{0, 0, 0}, Float3{0, 1, 0});
        camera.near_plane = 0.1f;
        camera.far_plane = 10.0f;
        const ViewId view = builder.view(camera, ViewRect{});
        Clear clear;
        clear.colour = clear.depth = true;
        builder.begin_pass(view, PassKind::Scene, clear);
        Recipe recipe;
        recipe.cull = Cull::None;
        for (int q = 0; q < quads; ++q) {
            const float x = -0.7f + 0.4f * static_cast<float>(q);
            const ImmediateVertex v[4] = {{Float3{x, -0.2f, 0}, colour}, {Float3{x + 0.3f, -0.2f, 0}, colour},
                                          {Float3{x, 0.2f, 0}, colour}, {Float3{x + 0.3f, 0.2f, 0}, colour}};
            const std::uint32_t indices[6] = {0, 1, 2, 2, 1, 3};
            ImmediateDraw draw;
            builder.immediate(draw, v, 4, indices, 6, Float3x4{}, recipe);
        }
        builder.end_pass();
        return builder.finish();
    };
    auto sizes = [](const FramePacket& p) {
        return std::vector<std::size_t>{p.views.size(), p.light_sets.size(), p.passes.size(), p.draws.size(),
                                        p.bones.size(), p.morph_weights.size(), p.particles.size(),
                                        p.particle_batches.size(), p.immediate_vertices.size(),
                                        p.immediate_indices.size(), p.immediates.size(), p.canvas_regions.size()};
    };
    auto render = [&](const FramePacket& packet, const char* what) {
        std::vector<std::uint8_t> pixels;
        check_ok(renderer.render_to_image(packet, kSize, kSize, &pixels, &error), error, what);
        check(!renderer.reading(packet), "the renderer reads no packet once render_to_image has returned");
        return pixels;
    };
    // The reference: fresh builders' packets.
    FrameBuilder fresh_big, fresh_small;
    const FramePacket four_quads = describe(fresh_big, 1, 0xFF0000FFu, 4);
    const FramePacket one_quad = describe(fresh_small, 2, 0xFF00FF00u, 1);
    const std::vector<std::uint8_t> big_pixels = render(four_quads, "fresh packet, 4 quads");
    const std::vector<std::uint8_t> small_pixels = render(one_quad, "fresh packet, 1 quad");
    check(big_pixels != small_pixels, "the two reference frames differ");

    FrameBuilder builder;
    FramePacket first = describe(builder, 1, 0xFF0000FFu, 4);
    check(render(first, "frame 1") == big_pixels, "frame 1: a fresh builder's pixels");
    const void* storage = first.immediate_vertices.data();
    const std::size_t capacity = first.immediate_vertices.capacity();
    builder.recycle(std::move(first));
    FramePacket second = describe(builder, 2, 0xFF00FF00u, 1);
    check(second.immediate_vertices.data() == storage && second.immediate_vertices.capacity() == capacity,
          "frame 2 reuses the rendered packet's storage");
    check(sizes(second) == sizes(one_quad) && second.tick == 2u, "frame 2 holds only what it described");
    check(render(second, "frame 2") == small_pixels, "frame 2: a fresh builder's pixels");
    // A handed-back packet survives discard() (a restored state dropping the frame being described).
    const void* storage2 = second.immediates.data();
    builder.recycle(std::move(second));
    builder.discard();
    FramePacket third = describe(builder, 3, 0xFF00FF00u, 1);
    check(third.immediates.data() == storage2, "a handed-back packet survives discard()");
    check(sizes(third) == sizes(one_quad) && render(third, "frame 3") == small_pixels, "frame 3 as a fresh builder's");

    // Cross-thread: another thread hands each finished packet back while the next frames are built.
    std::mutex slot_mutex;
    std::vector<FramePacket> slot;
    std::atomic<bool> done{false};
    std::thread hand_back([&] {
        for (;;) {
            std::vector<FramePacket> taken;
            {
                std::lock_guard<std::mutex> lock(slot_mutex);
                taken.swap(slot);
            }
            for (FramePacket& packet : taken)
                builder.recycle(std::move(packet));
            if (taken.empty()) {
                if (done.load(std::memory_order_acquire))
                    break;
                std::this_thread::yield();
            }
        }
    });
    bool all_sound = true;
    for (int frame = 0; frame < 400; ++frame) {
        const int quads = 1 + frame % 4;
        FramePacket packet = describe(builder, 10u + static_cast<std::uint64_t>(frame), 0xFF0000FFu, quads);
        all_sound = all_sound && packet.immediates.size() == static_cast<std::size_t>(quads) &&
                    packet.immediate_vertices.size() == static_cast<std::size_t>(4 * quads) &&
                    packet.immediate_indices.size() == static_cast<std::size_t>(6 * quads) &&
                    packet.views.size() == 1u && packet.passes.size() == 1u;
        std::lock_guard<std::mutex> lock(slot_mutex);
        slot.push_back(std::move(packet));
    }
    done.store(true, std::memory_order_release);
    hand_back.join();
    check(all_sound, "cross-thread hand-back: every frame holds only its own contents");
    FramePacket last = describe(builder, 1000, 0xFF0000FFu, 4);
    check(sizes(last) == sizes(four_quads) && render(last, "frame after the cross-thread run") == big_pixels,
          "after the cross-thread run: a fresh builder's packet and pixels");
    for (const std::string& message : renderer.take_debug_errors()) {
        std::printf("FAIL: frame_builder_recycles_rendered_packets: debug layer: %s\n", message.c_str());
        ++g_failures;
    }
}

}  // namespace

int main() {
    Fixture fixture;
    if (fixture.create()) {
        device_meets_the_floor(fixture);
        large_readback_releases_capacity(fixture);
        upload_ring_segments(fixture);
        bindless_indices_are_reused_after_the_gpu(fixture);
        graph_culls_and_plans_transitions(fixture);
        graph_shares_textures_by_lifetime(fixture);
        graph_aliases_memory_by_lifetime(fixture);
        graph_releases_superseded_placements(fixture);
        graph_target_trace(fixture);
        bindless_texture_round_trip(fixture);
        upload_stream_wraps_and_delivers(fixture);
        multisample_resolve_is_deterministic(fixture);
        skin_cache_skins_once(fixture);
        swap_chain_presents_and_resizes(fixture);
        packet_queue_keeps_the_newest();
        renderer_creates_while_rendering();
        renderer_makes_resources_on_its_worker();
        renderer_skins_meshes();
        renderer_morphs_meshes();
        renderer_defers_releases();
        renderer_draws_particles_and_fog();
        renderer_draws_the_glare();
        renderer_draws_immediate_geometry();
        renderer_draws_canvas_geometry();
        renderer_splits_the_screen();
        renderer_samples_snapshots_and_history();
        snapshot_reads_bound_the_sampled_texels();
        renderer_renders_without_multisampling();
        frame_builder_recycles_rendered_packets();
        fixture.device.wait_idle();
        fixture.expect_no_debug_errors("teardown");
    }
    if (g_failures == 0) {
        std::printf("render_core_tests: all passed\n");
        return 0;
    }
    std::printf("render_core_tests: %d failure(s)\n", g_failures);
    return 1;
}
