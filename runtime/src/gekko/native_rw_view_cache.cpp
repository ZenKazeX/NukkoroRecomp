// The driver camera view-matrix cache, native. Contract: include/gekko/native_rw_view_cache.h.

#include "gekko/native_rw_view_cache.h"

#include <cstdlib>
#include <cstring>

#include "gekko/native_guest_call.h"
#include "gekko/native_rw_refusal.h"
#include "gekko/ppc_ops.h"

namespace gekko::rw_view_cache {
namespace {

constexpr u32 kDevice = 0x805E2A10u;
constexpr u32 kIndex = 0x244u, kMatrixTable = 0x1C4u, kViewMatrix = 0x184u, kExtentA = 0x248u, kExtentB = 0x24Cu, kScaled = 0x258u,
              kInverse = 0x298u, kProduct = 0x2D8u, kDirty = 0x318u, kScale = 0x320u, kOtherFlag = 0x32Cu, kCamera = 0x34Cu;
constexpr u32 kSda2One = 0x1B48u, kSda2Zero = 0x1B4Cu, kSda2Half = 0x1B50u;  // r2-relative single constants

// RenderWare core callees
constexpr u32 kMultiply = 0x8047FC08u, kInvert = 0x80480100u, kScaleRow = 0x804803E8u, kTranslateRow = 0x804805ECu,
              kFrameGetLtm = 0x804882E8u;
// the rows' own addresses: the bases of the (never fetched) link addresses of the guest calls
constexpr u32 kSetOffsetEntry = 0x804759B4u + 0u, kRefreshEntry = 0x80475A1Cu + 0u;

bool servable(const Cpu& c) {
    const u32 gqr0 = c.spr[SPR_GQR0];
    return (c.msr() & 0x2000u) != 0u && ((gqr0 >> 16) & 7u) < 4u && (gqr0 & 7u) < 4u;
}

u64 lfs(Cpu& c, u32 address) { return single_to_double_bits(c.mem_read32(address)); }
u32 stfs_word(u64 value) { return double_to_single_bits(value); }
void settle(Cpu& c, u32 reg) { c.fpr[reg].bits[1] = single_to_double_bits(double_to_single_bits_flush(c.fpr[reg].bits[1])); }

// A guest call runs with r1 as the frame top, so temporaries live below it.
struct Frame {
    Cpu& c;
    u32 saved;
    Frame(Cpu& cpu, u32 bytes) : c(cpu), saved(cpu.gpr[1]) { c.gpr[1] = saved - bytes; }
    ~Frame() { c.gpr[1] = saved; }
    u32 at(u32 offset) const { return c.gpr[1] + offset; }
};

u32 current_matrix(Cpu& c) { return c.mem_read32(kDevice + (c.mem_read32(kDevice + kIndex) << 2) + kMatrixTable); }

}  // namespace

bool serve_set_offset(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "the driver camera view offset", "the FPU is off or GQR0 is not the float format");
    const u32 matrix = current_matrix(c);
    {
        Frame frame(c, 0x20u);
        c.mem_write32(frame.at(8u), stfs_word(c.fpr[1].bits[0]));
        c.mem_write32(frame.at(0xCu), stfs_word(c.fpr[2].bits[0]));
        c.mem_write32(frame.at(0x10u), stfs_word(lfs(c, c.gpr[2] + kSda2Zero)));
        u32 ignored = 0;
        if (!guest_call(c, kTranslateRow, kSetOffsetEntry + 4u, {matrix, frame.at(8u), 1u}, ignored)) return true;
    }
    c.mem_write32(kDevice + kDirty, 0u);
    c.mem_write32(kDevice + kOtherFlag, 0u);
    c.gpr[3] = 1u;
    return true;
}

bool serve_refresh(Cpu& c) {
    if (!servable(c)) return rw_refusal::refuse(c, "the driver camera view-matrix cache", "the FPU is off or GQR0 is not the float format");
    u32 ignored = 0;
    if (c.mem_read32(kDevice + kDirty) == 0u) {
        const u32 camera = c.mem_read32(kDevice + kCamera);
        const u64 f30 = c.mem_read32(camera + 0x14u) == 2u ? lfs(c, c.gpr[2] + kSda2One) : lfs(c, kDevice + kScale);
        const u32 current = current_matrix(c);
        Frame frame(c, 0xB0u);
        const u32 tmp = frame.at(0x28u), vec_a = frame.at(0x1Cu), vec_b = frame.at(0x10u);
        if (!guest_call(c, kMultiply, kRefreshEntry + 4u, {tmp, current, kDevice + kViewMatrix}, ignored)) return true;
        const u32 one = stfs_word(lfs(c, c.gpr[2] + kSda2One));
        c.mem_write32(vec_a, stfs_word(f30));
        c.mem_write32(vec_a + 4u, stfs_word(f30));
        c.mem_write32(vec_a + 8u, one);
        if (!guest_call(c, kScaleRow, kRefreshEntry + 8u, {tmp, vec_a, 2u}, ignored)) return true;
        const u64 extent_a = lfs(c, kDevice + kExtentA);
        const u64 f31 = lfs(c, kDevice + kExtentB);
        double da, db;
        std::memcpy(&da, &extent_a, 8);
        std::memcpy(&db, &f31, 8);
        const u64 smaller = da < db ? extent_a : f31;  // fcmpo; bge: not-less (unordered too) takes the second
        c.mem_write32(vec_a, stfs_word(smaller ^ 0x8000000000000000ull));
        c.mem_write32(vec_a + 4u, stfs_word(smaller));
        c.mem_write32(vec_a + 8u, one);
        if (!guest_call(c, kScaleRow, kRefreshEntry + 0xCu, {tmp, vec_a, 2u}, ignored)) return true;
        const u64 half = lfs(c, c.gpr[2] + kSda2Half);
        const u64 shift_y = ppc::fps_mul(c, half, f31), shift_x = ppc::fps_mul(c, half, extent_a);
        const u64 y_scaled = ppc::fps_mul(c, shift_y, f30), x_scaled = ppc::fps_mul(c, shift_x, f30);
        c.mem_write32(vec_b, stfs_word(x_scaled));
        c.mem_write32(vec_b + 4u, stfs_word(y_scaled ^ 0x8000000000000000ull));
        c.mem_write32(vec_b + 8u, stfs_word(lfs(c, c.gpr[2] + kSda2Zero)));
        if (!guest_call(c, kTranslateRow, kRefreshEntry + 0x10u, {tmp, vec_b, 2u}, ignored)) return true;
        for (u32 w = 0; w < 16u; ++w) c.mem_write32(kDevice + kScaled + 4u * w, c.mem_read32(tmp + 4u * w));
        if (!guest_call(c, kInvert, kRefreshEntry + 0x14u, {kDevice + kInverse, tmp}, ignored)) return true;
        c.mem_write32(kDevice + kDirty, 1u);
    }
    u32 ltm = 0;
    if (!guest_call(c, kFrameGetLtm, kRefreshEntry + 0x18u, {c.mem_read32(c.mem_read32(kDevice + kCamera) + 4u)}, ltm)) return true;
    if (!guest_call(c, kMultiply, kRefreshEntry + 0x1Cu, {kDevice + kProduct, kDevice + kScaled, ltm}, ignored)) return true;
    settle(c, 30);
    settle(c, 31);
    c.gpr[3] = kDevice + kProduct;
    return true;
}

void install(Cpu& cpu) {
    cpu.install_hle(kSetOffset, [](Cpu& c) { return serve_set_offset(c); }, "driver camera view offset");
    cpu.install_hle(kRefresh, [](Cpu& c) { return serve_refresh(c); }, "driver camera view-matrix cache");
}

}  // namespace gekko::rw_view_cache
