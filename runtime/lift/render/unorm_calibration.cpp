// The direct resolve's calibration (SHADOW_R2_DIRECT_RESOLVE; renderer.cpp, the resolve in Renderer::record).
//
// The compose path turns a view's resolved mean m (binary32, per channel) into an output byte through the GPU's own
// fixed-function conversions: the float -> R16G16B16A16_UNORM render-target store of the view texture, the compose's
// point sample of it, and the float -> 8-bit store of the output. D3D allows each conversion 0.6 ULP of its
// destination (a nondecreasing mapping, not one formula), and this GPU's R16 store is measurably not round-to-nearest
// (2026-11-04: a shader emulation left 88-216 pixels per 1080p frame one step high). So nothing is assumed here: the
// whole chain F(m) = byte is MEASURED on this device, through the same pipeline, formats and sampler the compose uses.
//
// F is nondecreasing in m, so it is fixed by its 255 transitions per channel: T[c][k] = the first nonnegative float
// (as bits; for nonnegative floats bit order is numeric order) whose byte is >= k. Measured in two steps:
//   1. g[c][v]: the compose's byte for every R16 code v (a 256 x 256 R16 texture holding every code, composed into
//      the output format and read back). It must be nondecreasing, g(0) = 0, g(65535) = 255.
//   2. For each k: v_k = the first code with g >= k. A bracket of floats one code either side of that code's ideal
//      lower edge ((v_k - 0.5) / 65535; the store may deviate 0.6 code) is narrowed pass by pass: kWindow evenly
//      spaced floats per bracket go through the same render-target store, are read back and mapped through g, and
//      the bracket becomes the two samples around the first that reaches k - until its ends are adjacent floats.
//      The upper end is T[c][k]. Every pass checks the transition lies inside and the samples are monotone.
// Then every representative k / 255 must store as byte k (the value the direct resolve writes). Any failure leaves
// the direct resolve off (the compose path runs): the table is evidence for this device and driver, not a formula.

#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <type_traits>
#include <vector>

#include "render/pipelines.h"
#include "render/resources.h"

namespace shadow::render {

namespace {

constexpr std::uint32_t kWindow = 1024;  // floats sampled per open bracket per pass
constexpr std::uint32_t kLevels = 256;

bool fail(std::string* why, const std::string& what) {
    if (why != nullptr) *why = what;
    return false;
}

std::uint32_t float_bits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

}  // namespace

bool Renderer::calibrate_unorm_resolve(DXGI_FORMAT output_format, std::string* why) {
    if (output_format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return fail(why, "only an R8G8B8A8_UNORM output is calibrated");
    ID3D12Device8* d = device_.d3d();
    ID3D12PipelineState* to_r16 = screen_pipeline(Screen::Under, DXGI_FORMAT_R16G16B16A16_UNORM, 1u, why);
    ID3D12PipelineState* to_out = screen_pipeline(Screen::Under, output_format, 1u, why);
    if (to_r16 == nullptr || to_out == nullptr)
        return false;

    Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList> list;
    if (FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return fail(why, "calibration command list");
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = 4;
    Com<ID3D12DescriptorHeap> rtv_heap;
    if (FAILED(d->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap))))
        return fail(why, "calibration RTV heap");
    const std::uint32_t rtv_step = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    std::deque<GpuBuffer> buffers;  // uploads and readbacks, alive until the GPU is done (stable addresses)
    std::deque<GpuTexture> textures;
    const auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        list->ResourceBarrier(1, &b);
    };
    const auto footprint = [&](const D3D12_RESOURCE_DESC& desc, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp) {
        UINT rows = 0;
        UINT64 row_bytes = 0, total = 0;
        d->GetCopyableFootprints(&desc, 0, 1, 0, fp, &rows, &row_bytes, &total);
        return total;
    };
    // A sampled texture holding `data` (tightly packed rows of `texel` bytes), with a bindless SRV.
    const auto source = [&](DXGI_FORMAT format, std::uint32_t w, std::uint32_t h, const void* data,
                            std::uint32_t texel) -> GpuTexture* {
        TextureInfo info;
        info.width = w;
        info.height = h;
        info.format = format;
        info.initial_state = D3D12_RESOURCE_STATE_COPY_DEST;
        info.name = L"shadow.unorm-calibration.source";
        textures.emplace_back();
        GpuTexture& t = textures.back();
        if (!create_texture_2d(device_, info, &t, why) || !create_texture_srv(device_, heap_, t, format, why))
            return nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        const std::uint64_t total = footprint(t.desc, &fp);
        buffers.emplace_back();
        GpuBuffer& up = buffers.back();
        if (!create_buffer(device_, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"shadow.unorm-calibration.upload", &up, why))
            return nullptr;
        void* mapped = nullptr;
        if (FAILED(up.resource->Map(0, nullptr, &mapped)))
            return fail(why, "calibration upload map"), nullptr;
        for (std::uint32_t y = 0; y < h; ++y)
            std::memcpy(static_cast<std::uint8_t*>(mapped) + fp.Offset + std::size_t{y} * fp.Footprint.RowPitch,
                        static_cast<const std::uint8_t*>(data) + std::size_t{y} * w * texel, std::size_t{w} * texel);
        up.resource->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{t.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        D3D12_TEXTURE_COPY_LOCATION src{up.resource.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(t.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return &t;
    };
    // The compose's own copy (Screen::Under: point sample, no blend) of `from` into a new `format` target, then a
    // readback of that target. Returns the readback buffer index and its footprint.
    struct Readback {
        std::size_t buffer = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    };
    std::uint32_t rtv_used = 0;
    const auto copy_through = [&](const GpuTexture& from, DXGI_FORMAT format, ID3D12PipelineState* pipeline,
                                  Readback* out) -> bool {
        TextureInfo info;
        info.width = static_cast<std::uint32_t>(from.desc.Width);
        info.height = from.desc.Height;
        info.format = format;
        info.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        info.initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        info.name = L"shadow.unorm-calibration.target";
        textures.emplace_back();
        GpuTexture& target = textures.back();
        if (!create_texture_2d(device_, info, &target, why))
            return false;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += std::size_t{rtv_used++} * rtv_step;
        d->CreateRenderTargetView(target.resource.Get(), nullptr, rtv);
        ID3D12DescriptorHeap* heaps[] = {heap_.heap(), samplers_.heap()};
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootSignature(root_.Get());
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(info.width), static_cast<float>(info.height),
                                      0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(info.width), static_cast<LONG>(info.height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->SetPipelineState(pipeline);
        const std::uint32_t constants[2] = {from.srv, sampler_index(Filter::Point, Address::Clamp, Address::Clamp)};
        list->SetGraphicsRoot32BitConstants(kRootConstants, 2, constants, 4);
        list->DrawInstanced(3, 1, 0, 0);
        barrier(target.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const std::uint64_t total = footprint(target.desc, &out->fp);
        buffers.emplace_back();
        out->buffer = buffers.size() - 1;
        if (!create_buffer(device_, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"shadow.unorm-calibration.readback", &buffers.back(), why))
            return false;
        D3D12_TEXTURE_COPY_LOCATION dst{buffers.back().resource.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint = out->fp;
        D3D12_TEXTURE_COPY_LOCATION src{target.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        return true;
    };
    const auto run = [&]() -> bool {
        if (FAILED(list->Close()))
            return fail(why, "calibration list close");
        ID3D12CommandList* lists[] = {list.Get()};
        device_.direct().wait(device_.direct().submit(lists, 1));
        return true;
    };
    // texels[y * w + x] of a readback, `channels` values of type T each.
    const auto read = [&](const Readback& r, auto* sample, std::uint32_t channels) {
        using T = std::remove_pointer_t<decltype(sample)>;
        const std::uint32_t w = r.fp.Footprint.Width, h = r.fp.Footprint.Height;
        std::vector<T> out(std::size_t{w} * h * channels);
        void* mapped = nullptr;
        if (FAILED(buffers[r.buffer].resource->Map(0, nullptr, &mapped)))
            return std::vector<T>{};
        for (std::uint32_t y = 0; y < h; ++y)
            std::memcpy(out.data() + std::size_t{y} * w * channels,
                        static_cast<const std::uint8_t*>(mapped) + r.fp.Offset + std::size_t{y} * r.fp.Footprint.RowPitch,
                        std::size_t{w} * channels * sizeof(T));
        const D3D12_RANGE nothing{0, 0};
        buffers[r.buffer].resource->Unmap(0, &nothing);
        return out;
    };
    const auto reset_list = [&]() {
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        rtv_used = 0;
    };

    // ---- step 1: g, the compose's byte for every R16 code; and the representatives k / 255 ----------------------
    std::vector<std::uint16_t> codes(65536u * 4u);
    for (std::uint32_t v = 0; v < 65536u; ++v)
        for (std::uint32_t c = 0; c < 4u; ++c)
            codes[v * 4u + c] = static_cast<std::uint16_t>(v);
    std::vector<float> representatives(kLevels * 4u);
    for (std::uint32_t k = 0; k < kLevels; ++k)
        for (std::uint32_t c = 0; c < 4u; ++c)
            representatives[k * 4u + c] = static_cast<float>(k) / 255.0f;
    const GpuTexture* all_codes = source(DXGI_FORMAT_R16G16B16A16_UNORM, 256, 256, codes.data(), 8);
    const GpuTexture* reps = source(DXGI_FORMAT_R32G32B32A32_FLOAT, kLevels, 1, representatives.data(), 16);
    if (all_codes == nullptr || reps == nullptr)
        return false;
    Readback g_read, rep_read;
    if (!copy_through(*all_codes, output_format, to_out, &g_read) || !copy_through(*reps, output_format, to_out, &rep_read))
        return false;
    if (!run())
        return false;
    const std::vector<std::uint8_t> g_bytes = read(g_read, static_cast<std::uint8_t*>(nullptr), 4);
    const std::vector<std::uint8_t> rep_bytes = read(rep_read, static_cast<std::uint8_t*>(nullptr), 4);
    if (g_bytes.size() != 65536u * 4u || rep_bytes.size() != kLevels * 4u)
        return fail(why, "calibration readback");
    for (std::uint32_t k = 0; k < kLevels; ++k)
        for (std::uint32_t c = 0; c < 4u; ++c)
            if (rep_bytes[k * 4u + c] != k)
                return fail(why, "representative " + std::to_string(k) + " / 255 does not store as " + std::to_string(k));
    std::uint32_t first_code[4][kLevels] = {};  // v_k per channel
    for (std::uint32_t c = 0; c < 4u; ++c) {
        if (g_bytes[c] != 0u || g_bytes[65535u * 4u + c] != 255u)
            return fail(why, "compose byte of codes 0 / 65535 is not 0 / 255");
        std::uint32_t k = 1;
        for (std::uint32_t v = 1; v < 65536u; ++v) {
            const std::uint32_t now = g_bytes[v * 4u + c], before = g_bytes[(v - 1u) * 4u + c];
            if (now < before || now > before + 1u)
                return fail(why, "compose mapping of R16 codes is not a nondecreasing unit-step map");
            if (now == before + 1u)
                first_code[c][k++] = v;
        }
        if (k != kLevels)
            return fail(why, "compose mapping does not reach every byte");
    }

    // ---- step 2: the R16 render-target store around each transition, by bracketing ------------------------------
    // Each (channel, byte) starts with a bracket of floats one R16 code either side of the transition code's ideal
    // edge (a code is ~1.5e-5 wide: a few hundred float steps near 1, ~2^17 near byte 1). Every pass stores kWindow
    // evenly spaced floats of every open bracket through the R16 render-target store, maps the codes through g, and
    // keeps the two neighbouring samples around the first one that reaches the byte. The bracket shrinks ~1024x per
    // pass; when its ends are adjacent floats, the upper one is the threshold. Each pass also checks monotonicity.
    struct Bracket {
        std::uint32_t lo = 0, hi = 0;  // float bits: byte(lo) < k <= byte(hi), once checked
    };
    std::vector<Bracket> brackets(std::size_t{kLevels} * 4u);
    for (std::uint32_t c = 0; c < 4u; ++c)
        for (std::uint32_t k = 1; k < kLevels; ++k) {
            const double code = static_cast<double>(first_code[c][k]);
            const float low = static_cast<float>(std::max(0.0, (code - 1.5) / 65535.0));
            const float high = static_cast<float>(std::min(1.0, (code + 0.5) / 65535.0));
            brackets[std::size_t{c} * kLevels + k] = {float_bits(low), float_bits(high)};
        }
    std::vector<std::uint32_t> table(std::size_t{kLevels} * 4u, 0u);  // row c, column k: T[c][k] (T[c][0] = 0)
    bool first_pass = true;
    for (int pass = 0;; ++pass) {
        bool open = false;
        for (std::uint32_t c = 0; c < 4u && !open; ++c)
            for (std::uint32_t k = 1; k < kLevels && !open; ++k) {
                const Bracket& b = brackets[std::size_t{c} * kLevels + k];
                open = b.hi - b.lo > 1u || first_pass;
            }
        if (!open)
            break;
        if (pass == 8)
            return fail(why, "brackets did not close");
        reset_list();
        // Row k - 1, column i: sample i of byte k's bracket (each channel its own).
        std::vector<float> probe(std::size_t{kWindow} * (kLevels - 1u) * 4u);
        const auto sample_bits = [&](const Bracket& b, std::uint32_t i) {
            return b.lo + static_cast<std::uint32_t>((static_cast<std::uint64_t>(b.hi - b.lo) * i) / (kWindow - 1u));
        };
        for (std::uint32_t k = 1; k < kLevels; ++k)
            for (std::uint32_t c = 0; c < 4u; ++c)
                for (std::uint32_t i = 0; i < kWindow; ++i) {
                    const std::uint32_t bits = sample_bits(brackets[std::size_t{c} * kLevels + k], i);
                    float value;
                    std::memcpy(&value, &bits, sizeof(value));
                    probe[(std::size_t{k - 1u} * kWindow + i) * 4u + c] = value;
                }
        const GpuTexture* probes = source(DXGI_FORMAT_R32G32B32A32_FLOAT, kWindow, kLevels - 1u, probe.data(), 16);
        if (probes == nullptr)
            return false;
        Readback stored_read;
        if (!copy_through(*probes, DXGI_FORMAT_R16G16B16A16_UNORM, to_r16, &stored_read) || !run())
            return false;
        const std::vector<std::uint16_t> stored = read(stored_read, static_cast<std::uint16_t*>(nullptr), 4);
        if (stored.size() != probe.size())
            return fail(why, "calibration readback");
        for (std::uint32_t c = 0; c < 4u; ++c)
            for (std::uint32_t k = 1; k < kLevels; ++k) {
                Bracket& b = brackets[std::size_t{c} * kLevels + k];
                if (b.hi - b.lo <= 1u && !first_pass)
                    continue;
                std::uint32_t found = kWindow;
                for (std::uint32_t i = 0; i < kWindow; ++i) {
                    const std::uint32_t byte =
                        g_bytes[std::size_t{stored[(std::size_t{k - 1u} * kWindow + i) * 4u + c]} * 4u + c];
                    if (byte >= k) {
                        if (found == kWindow) found = i;
                    } else if (found != kWindow) {
                        return fail(why, "R16 store not monotone around byte " + std::to_string(k));
                    }
                }
                if (found == 0u || found == kWindow)
                    return fail(why, "transition to byte " + std::to_string(k) + " outside its bracket");
                const std::uint32_t lo = sample_bits(b, found - 1u), hi = sample_bits(b, found);
                b = {lo, hi};
                if (hi - lo <= 1u)
                    table[std::size_t{c} * kLevels + k] = hi;
            }
        first_pass = false;
    }
    // The shader decides most pixels without the table (direct_byte, utility.hlsl): that needs every threshold within
    // 0.1 step of its ideal (k - 0.5) / 255 (measured 2026-11-04: at most ~0.06, at byte 1).
    for (std::uint32_t c = 0; c < 4u; ++c)
        for (std::uint32_t k = 1; k < kLevels; ++k) {
            float t;
            std::memcpy(&t, &table[std::size_t{c} * kLevels + k], sizeof(t));
            if (std::fabs(static_cast<double>(t) * 255.0 - (static_cast<double>(k) - 0.5)) >= 0.1)
                return fail(why, "threshold for byte " + std::to_string(k) + " far from its ideal");
        }

    // ---- the table the shader reads (R32_UINT 256 x 4) -------------------------------------------------------
    reset_list();
    TextureInfo info;
    info.width = kLevels;
    info.height = 4;
    info.format = DXGI_FORMAT_R32_UINT;
    info.initial_state = D3D12_RESOURCE_STATE_COPY_DEST;
    info.name = L"shadow.unorm-calibration.table";
    GpuTexture result;
    if (!create_texture_2d(device_, info, &result, why) ||
        !create_texture_srv(device_, heap_, result, DXGI_FORMAT_R32_UINT, why))
        return false;
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        const std::uint64_t total = footprint(result.desc, &fp);
        buffers.emplace_back();
        if (!create_buffer(device_, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"shadow.unorm-calibration.upload", &buffers.back(), why))
            return false;
        void* mapped = nullptr;
        if (FAILED(buffers.back().resource->Map(0, nullptr, &mapped)))
            return fail(why, "calibration upload map");
        for (std::uint32_t y = 0; y < 4u; ++y)
            std::memcpy(static_cast<std::uint8_t*>(mapped) + fp.Offset + std::size_t{y} * fp.Footprint.RowPitch,
                        table.data() + std::size_t{y} * kLevels, kLevels * sizeof(std::uint32_t));
        buffers.back().resource->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{result.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        D3D12_TEXTURE_COPY_LOCATION src{buffers.back().resource.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(result.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (!run())
        return false;
    for (GpuTexture& t : textures)
        retire(device_, t, &heap_);
    unorm_table_ = std::move(result);
    unorm_table_format_ = output_format;

    // Diagnostics: how the measured store differs from round-to-nearest (SHADOW_R2_UNORM_TABLE=<file>).
    if (const char* path = std::getenv("SHADOW_R2_UNORM_TABLE"); path != nullptr && path[0] != '\0') {
        if (std::FILE* f = std::fopen(path, "w")) {
            std::fprintf(f, "# channel k first_code threshold_bits threshold ideal_nearest_bits ulp_from_ideal\n");
            for (std::uint32_t c = 0; c < 4u; ++c)
                for (std::uint32_t k = 1; k < kLevels; ++k) {
                    const std::uint32_t bits = table[std::size_t{c} * kLevels + k];
                    float t;
                    std::memcpy(&t, &bits, sizeof(t));
                    // Round-to-nearest of the exact product: the first float with x * 65535 >= v - 0.5.
                    const double edge = (static_cast<double>(first_code[c][k]) - 0.5) / 65535.0;
                    float ideal = static_cast<float>(edge);
                    if (static_cast<double>(ideal) < edge) ideal = std::nextafter(ideal, 2.0f);
                    std::fprintf(f, "%u %u %u %08X %.9g %08X %d\n", c, k, first_code[c][k], bits, t, float_bits(ideal),
                                 static_cast<int>(bits) - static_cast<int>(float_bits(ideal)));
                }
            std::fclose(f);
        }
    }
    return true;
}

void Renderer::ensure_unorm_calibration(DXGI_FORMAT output_format) {
    if (unorm_calibration_tried_ || output_format == unorm_table_format_)
        return;
    unorm_calibration_tried_ = true;
    std::string why;
    if (calibrate_unorm_resolve(output_format, &why)) {
        std::fprintf(stderr, "[render] direct resolve: calibrated the R16 -> compose -> 8-bit chain on this device\n");
    } else {
        std::fprintf(stderr, "[render] direct resolve stays off: calibration failed (%s)\n", why.c_str());
    }
}

}  // namespace shadow::render
