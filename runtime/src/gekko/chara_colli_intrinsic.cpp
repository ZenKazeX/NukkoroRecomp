#include "gekko/chara_colli_intrinsic.h"

#include "gekko/chara_colli_bone.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <unordered_map>

#include "gekko/cpu.h"
#include "gekko/dynamic_cast_intrinsic.h"
#include "gekko/guest_memory.h"
#include "gekko/ppc_ops.h"
#include "gekko/recomp.h"

namespace gekko {

namespace {

constexpr u32 kMsrFp = 0x2000u;

// The code the vtable slots must name (CharaColliBody / CharaColliGeneral and their CharInfo variants).
constexpr u32 kBodyUpdate = 0x800C1A74u;    // vf 0x88 (BodyCharInfo)
constexpr u32 kBodyNoUpdate = 0x800C0C9Cu;  // vf 0x88 (Body): blr
constexpr u32 kBodyShape = 0x800C0BD0u;     // vf 0x28: this + 0x5C (the shape)
constexpr u32 kBodyCentre = 0x800C0BB8u;    // vf 0x14: this + 0x40
constexpr u32 kGeneralUpdate = 0x800C409Cu; // vf 0x88 (GeneralCharInfo)
constexpr u32 kGeneralNoUpdate = 0x800C1D20u;  // vf 0x88 (General): blr
constexpr u32 kBoneUpdate = 0x800C3D6Cu;  // vf 0x88 (GeneralBone): 0x800C409C, vf 0x8C, vf 0x90, vf 0x84
constexpr u32 kGeneralCentre = 0x800C1CACu; // vf 0x14: this + 0x40

constexpr u32 kGroundFilter = 0x80099454u;  // the ground behaviour's ray / sweep filter
// The other filters on the tape (pair r3 as for 0x80099454; each returns 1 to test the object, 0 to skip it):
constexpr u32 kMaskAny1AB3 = 0x801A658Cu;   // (mask & 0x1AB3) != 0
constexpr u32 kMaskNone50008 = 0x801C80A4u; // (mask & 0x50008) == 0
constexpr u32 kMaskNone100108 = 0x801C80C0u;  // (mask & 0x100108) == 0
constexpr u32 kNotTheseTwo = 0x801C0910u;   // object neither arg[1] nor arg[3], and (mask & arg[0]) != 0
constexpr u32 kNotThatOwner = 0x8005B6C8u;  // (mask & 0xC0018) == 0, unless the object's owner's +0x54 is arg[0]

bool known_filter(u32 filter) noexcept {
    return filter == kGroundFilter || filter == kMaskAny1AB3 || filter == kMaskNone50008 ||
           filter == kMaskNone100108 || filter == kNotTheseTwo || filter == kNotThatOwner;
}

constexpr u32 kSegmentConstant = 0xFFFF8418u;  // r2 - 0x7BE8: the 0x8003C688 projection bound

u32 entry_of(CharaColliIntrinsic::Test test) noexcept {
    switch (test) {
    case CharaColliIntrinsic::Test::BodyRay: return CharaColliIntrinsic::kBodyRay;
    case CharaColliIntrinsic::Test::BodySweep: return CharaColliIntrinsic::kBodySweep;
    case CharaColliIntrinsic::Test::GeneralRay: return CharaColliIntrinsic::kGeneralRay;
    case CharaColliIntrinsic::Test::GeneralSweep: return CharaColliIntrinsic::kGeneralSweep;
    }
    return 0u;
}

bool is_body(CharaColliIntrinsic::Test test) noexcept {
    return test == CharaColliIntrinsic::Test::BodyRay || test == CharaColliIntrinsic::Test::BodySweep;
}
bool is_sweep(CharaColliIntrinsic::Test test) noexcept {
    return test == CharaColliIntrinsic::Test::BodySweep || test == CharaColliIntrinsic::Test::GeneralSweep;
}

// lfs and stfs: the guest's conversions at every float load and store.
GEKKO_ALWAYS_INLINE u64 lfs(u32 word) noexcept { return single_to_double_bits(word); }
GEKKO_ALWAYS_INLINE u32 stfs(u64 bits) noexcept { return double_to_single_bits(bits); }

// An RwV3d as three stored words (a helper's stfs results in the guest's dead frame).
struct V3 {
    u32 x, y, z;
};
GEKKO_ALWAYS_INLINE V3 read3(Cpu& c, u32 address) {
    const u32 x = c.mem_read32(address);
    const u32 y = c.mem_read32(address + 4u);
    return V3{x, y, c.mem_read32(address + 8u)};
}
// 0x800091EC (lfs, lfs, stfs, lfs, stfs, stfs): a copy through the FPRs.
GEKKO_ALWAYS_INLINE V3 copy(const V3& v) noexcept { return V3{stfs(lfs(v.x)), stfs(lfs(v.y)), stfs(lfs(v.z))}; }
// 0x8040DDC4 RwV3dSub(out, a, b): fsubs x, y, z.
GEKKO_ALWAYS_INLINE V3 sub(Cpu& c, const V3& a, const V3& b) {
    const u64 x = ppc::fps_sub(c, lfs(a.x), lfs(b.x));
    const u64 y = ppc::fps_sub(c, lfs(a.y), lfs(b.y));
    const u64 z = ppc::fps_sub(c, lfs(a.z), lfs(b.z));
    return V3{stfs(x), stfs(y), stfs(z)};
}
// 0x8040DE20 RwV3dAdd(out, a, b): fadds x, y, z.
GEKKO_ALWAYS_INLINE V3 add(Cpu& c, const V3& a, const V3& b) {
    const u64 x = ppc::fps_add(c, lfs(a.x), lfs(b.x));
    const u64 y = ppc::fps_add(c, lfs(a.y), lfs(b.y));
    const u64 z = ppc::fps_add(c, lfs(a.z), lfs(b.z));
    return V3{stfs(x), stfs(y), stfs(z)};
}
// 0x8040DDF8 RwV3dScale(out, v, f1): fmuls x, y, z.
GEKKO_ALWAYS_INLINE V3 scale(Cpu& c, const V3& v, u64 s) {
    const u64 x = ppc::fps_mul(c, lfs(v.x), s);
    const u64 y = ppc::fps_mul(c, lfs(v.y), s);
    const u64 z = ppc::fps_mul(c, lfs(v.z), s);
    return V3{stfs(x), stfs(y), stfs(z)};
}
// 0x8040DFE0 RwV3dDotProduct(a, b) -> f1: fmuls y, fmadds x, fmadds z.
GEKKO_ALWAYS_INLINE u64 dot(Cpu& c, const V3& a, const V3& b) {
    const u64 y = ppc::fps_mul(c, lfs(a.y), lfs(b.y));
    const u64 xy = ppc::fps_madd(c, lfs(a.x), lfs(b.x), y);
    return ppc::fps_madd(c, lfs(a.z), lfs(b.z), xy);
}
// 0x8040DF7C (|v|^2) -> f1: fmuls x, y, z; fadds x + y; fadds z + (x + y).
GEKKO_ALWAYS_INLINE u64 length_squared(Cpu& c, const V3& v) {
    const u64 x = ppc::fps_mul(c, lfs(v.x), lfs(v.x));
    const u64 y = ppc::fps_mul(c, lfs(v.y), lfs(v.y));
    const u64 z = ppc::fps_mul(c, lfs(v.z), lfs(v.z));
    const u64 xy = ppc::fps_add(c, x, y);
    return ppc::fps_add(c, z, xy);
}

// 0x8003C688 (centre, segment {a, b}, out = null) -> f1: the squared distance from the centre to the segment.
u64 distance_squared(Cpu& c, const V3& centre, const V3& a, const V3& b) {
    const V3 d = copy(sub(c, b, a));
    const V3 w = copy(sub(c, centre, a));
    const u64 along = dot(c, w, d);  // f31
    ppc::fp_compare(c, 0u, along, lfs(c.mem_read32(c.gpr[2] + kSegmentConstant)), true);
    if (!ppc::cr_bit(c, 1u))  // not past a: |w|^2
        return length_squared(c, w);
    const u64 span = dot(c, d, d);
    ppc::fp_compare(c, 0u, along, span, true);
    if (ppc::cr_bit(c, 1u) || ppc::cr_bit(c, 2u))  // past b: 0x8040E1D0 (centre, b)
        return length_squared(c, sub(c, centre, b));
    const u64 t = ppc::fps_div(c, along, span);
    const V3 nearest = copy(add(c, a, scale(c, d, t)));
    return length_squared(c, sub(c, centre, nearest));
}

// vf 0x88 (0x800C1A74 / 0x800C409C, the same code): the owner's position, rotation and word copied in when they
// differ. The weak_ptr copies and the lock change no count when done (each increment has its decrement).
void update(Cpu& c, u32 self) {
    const u32 counts = c.mem_read32(self + 0x34u);
    if (counts == 0u || c.mem_read32(counts + 4u) == 0u)  // expired
        return;
    const u32 owner = c.mem_read32(self + 0x30u);
    if (owner == 0u)
        return;
    const auto differs = [&c](u32 a, u32 b) {  // fcmpu cr0, lfs a, lfs b; "ne"
        ppc::fp_compare(c, 0u, lfs(c.mem_read32(a)), lfs(c.mem_read32(b)), false);
        return !ppc::cr_bit(c, 2u);
    };
    if (differs(owner, self + 0x40u) || differs(owner + 4u, self + 0x44u) || differs(owner + 8u, self + 0x48u)) {
        const u32 x = c.mem_read32(owner);  // 0x800091D0 (self + 0x40, owner)
        const u32 y = c.mem_read32(owner + 4u);
        c.mem_write32(self + 0x40u, stfs(lfs(x)));
        const u32 z = c.mem_read32(owner + 8u);
        c.mem_write32(self + 0x44u, stfs(lfs(y)));
        c.mem_write32(self + 0x48u, stfs(lfs(z)));
        c.mem_write8(self + 0x28u, 1u);
    }
    const u32 rx = c.mem_read32(owner + 0x0Cu);  // f2, kept for the store
    ppc::fp_compare(c, 0u, lfs(rx), lfs(c.mem_read32(self + 0x4Cu)), false);
    if (!ppc::cr_bit(c, 2u) || differs(owner + 0x10u, self + 0x50u) || differs(owner + 0x14u, self + 0x54u)) {
        c.mem_write32(self + 0x4Cu, stfs(lfs(rx)));
        c.mem_write32(self + 0x50u, stfs(lfs(c.mem_read32(owner + 0x10u))));
        c.mem_write32(self + 0x54u, stfs(lfs(c.mem_read32(owner + 0x14u))));
        c.mem_write8(self + 0x28u, 1u);
    }
    const u32 word = c.mem_read32(owner + 0x30u);
    if (word != c.mem_read32(self + 0x58u)) {
        c.mem_write32(self + 0x58u, word);
        c.mem_write8(self + 0x28u, 1u);
    }
}

// The bytes the update may write, for a rollback.
struct Written {
    u8 dirty;
    u32 words[7];  // self + 0x40 .. 0x5B
};
GEKKO_ALWAYS_INLINE void save(Cpu& c, u32 self, Written& w) {
    w.dirty = c.mem_read8(self + 0x28u);
    for (u32 i = 0; i < 7u; ++i)
        w.words[i] = c.mem_read32(self + 0x40u + 4u * i);
}
GEKKO_ALWAYS_INLINE void restore(Cpu& c, u32 self, const Written& w) {
    c.mem_write8(self + 0x28u, w.dirty);
    for (u32 i = 0; i < 7u; ++i)
        c.mem_write32(self + 0x40u + 4u * i, w.words[i]);
}

// A weak_ptr's sp_counted_base (use + 4, weak + 8) that the guest's copy / lock / release pairs leave as they found
// it: no count wraps and no release reaches zero.
bool counts_safe(Cpu& c, u32 counts) {
    if (counts == 0u)
        return true;
    const u32 use = c.mem_read32(counts + 4u);
    const u32 weak = c.mem_read32(counts + 8u);
    return weak != 0u && weak < 0x80000000u && use < 0x80000000u;
}

// ---- --verify-chara-colli ----

struct Window {
    u32 base = 0, size = 0;
};
constexpr u32 kWindows = 5;
constexpr u32 kWindowBytes = 0x60u;

struct Snapshot {
    u32 gpr[32], lr, ctr, xer, cr, fpscr;
    u64 fpr[32][2];
    u8 bytes[kWindows][kWindowBytes];
    bool mapped[kWindows];
};

void windows_of(Cpu& c, CharaColliIntrinsic::Test test, Window w[kWindows]) {
    const u32 self = c.gpr[3];
    w[0] = {self, 0x60u};                     // the object
    w[1] = {c.gpr[4], 0x18u};                 // the segment / sphere
    w[2] = {is_sweep(test) ? c.gpr[5] : 0u, is_sweep(test) ? 0x0Cu : 0u};  // the motion
    const GuestMemory& memory = c.guest_memory();
    const u8* link = memory.peek(GcAddr(self + 0x30u), 8u);
    u32 owner = 0u, counts = 0u;
    if (link != nullptr) {
        owner = (u32{link[0]} << 24) | (u32{link[1]} << 16) | (u32{link[2]} << 8) | link[3];
        counts = (u32{link[4]} << 24) | (u32{link[5]} << 16) | (u32{link[6]} << 8) | link[7];
    }
    w[3] = {owner, owner != 0u ? 0x34u : 0u};   // the owner's data
    w[4] = {counts, counts != 0u ? 0x10u : 0u}; // its sp_counted_base
}

void take(Cpu& c, const Window w[kWindows], Snapshot& s) {
    std::memcpy(s.gpr, c.gpr, sizeof(s.gpr));
    s.lr = c.lr;
    s.ctr = c.ctr;
    s.xer = c.xer;
    s.cr = c.cr;
    s.fpscr = c.fpscr;
    for (u32 i = 0; i < 32u; ++i) {
        s.fpr[i][0] = c.fpr[i].bits[0];
        s.fpr[i][1] = c.fpr[i].bits[1];
    }
    std::memset(s.bytes, 0, sizeof(s.bytes));
    for (u32 i = 0; i < kWindows; ++i) {
        const u8* p = w[i].size != 0u ? c.guest_memory().peek(GcAddr(w[i].base), w[i].size) : nullptr;
        s.mapped[i] = p != nullptr;
        if (p != nullptr)
            std::memcpy(s.bytes[i], p, w[i].size);
    }
}

void put(Cpu& c, const Window w[kWindows], const Snapshot& s) {
    std::memcpy(c.gpr, s.gpr, sizeof(s.gpr));
    c.lr = s.lr;
    c.ctr = s.ctr;
    c.xer = s.xer;
    c.cr = s.cr;
    c.fpscr = s.fpscr;
    for (u32 i = 0; i < 32u; ++i) {
        c.fpr[i].bits[0] = s.fpr[i][0];
        c.fpr[i].bits[1] = s.fpr[i][1];
    }
    // Host-side writes (nothing traced): the windows were all read from one state, so overlaps agree.
    for (u32 i = 0; i < kWindows; ++i)
        if (s.mapped[i])
            if (u8* p = const_cast<u8*>(c.guest_memory().peek(GcAddr(w[i].base), w[i].size)))
                std::memcpy(p, s.bytes[i], w[i].size);
}

// Every committed byte of every region, in region order.
void take_all(const GuestMemory& memory, std::vector<u8>& out) {
    out.clear();
    for (const GuestMemory::Region& region : memory.regions())
        out.insert(out.end(), region.host, region.host + region.committed);
}
void put_all(GuestMemory& memory, const std::vector<u8>& in) {
    std::size_t at = 0;
    for (const GuestMemory::Region& region : memory.regions()) {
        std::memcpy(region.host, in.data() + at, region.committed);
        at += region.committed;
    }
}
// The guest address of the first differing byte outside the dead stack [r1 - 0x10000, r1), or 0.
u32 first_difference(const GuestMemory& memory, const std::vector<u8>& a, const std::vector<u8>& b, u32 r1) {
    std::size_t at = 0;
    for (const GuestMemory::Region& region : memory.regions()) {
        for (u32 i = 0; i < region.committed; ++i) {
            if (a[at + i] != b[at + i]) {
                const u32 address = region.base + i;
                if (!(address < r1 && r1 - address <= 0x10000u))
                    return address;
            }
        }
        at += region.committed;
    }
    return 0u;
}

// The contract's registers: r1-r3, r13-r31, f14-f31, LR, CR2-4, FPSCR.
// FPSCR's control byte (VE OE UE ZE XE NI RN) is the contract: it decides every later result. Its status bits (FX
// FEX VX* OX UX ZX XX FR FI FPRF) are not: no game code reads them (main.dol has no mcrfs and no record-form FP
// instruction; mffs only in OSInit, OSSaveFPUContext and MetroTRK, docs/NATIVE_MSL_MATH.md), and the native math
// (math/msl.h) returns values without them.
bool same_fpscr(u32 a, u32 b) { return ((a ^ b) & 0x000000FFu) == 0u; }

bool same_contract(const Snapshot& a, const Snapshot& b) {
    for (u32 i = 1; i < 32u; ++i)
        if ((i <= 3u || i >= 13u) && a.gpr[i] != b.gpr[i])
            return false;
    for (u32 i = 14; i < 32u; ++i)
        if (a.fpr[i][0] != b.fpr[i][0] || a.fpr[i][1] != b.fpr[i][1])
            return false;
    return a.lr == b.lr && ((a.cr ^ b.cr) & 0x00FFF000u) == 0u && same_fpscr(a.fpscr, b.fpscr) &&
           std::memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;
}

}  // namespace

bool CharaColliIntrinsic::serve(Cpu& c, Test test) {
    if (reference_)
        return false;
    ++calls_;
    return verify_ ? verify(c, test) : execute(c, test);
}

bool CharaColliIntrinsic::execute(Cpu& c, Test test) {
    const u32 gqr = c.spr[SPR_GQR0];
    if ((c.msr() & kMsrFp) == 0u || (gqr & 7u) >= 4u || ((gqr >> 16) & 7u) >= 4u) {
        ++other_;
        ++other_by_[static_cast<u32>(test)][0];
        return false;
    }
    const bool body = is_body(test);
    const bool sweep = is_sweep(test);
    const u32 self = c.gpr[3];
    const u32 vtable = c.mem_read32(self);
    const u32 update_slot = c.mem_read32(vtable + 0x88u);
    const bool bone = !body && update_slot == kBoneUpdate;  // 0x800C409C, then the bones (bone_update)
    const bool updates = bone || update_slot == (body ? kBodyUpdate : kGeneralUpdate);
    if ((!updates && update_slot != (body ? kBodyNoUpdate : kGeneralNoUpdate)) ||
        (body ? (c.mem_read32(vtable + 0x28u) != kBodyShape || c.mem_read32(vtable + 0x14u) != kBodyCentre)
              : c.mem_read32(vtable + 0x14u) != kGeneralCentre)) {
        ++other_;
        ++other_by_[static_cast<u32>(test)][1];
        return false;
    }
    // A Body test calls its filter before the bounding test; the ground behaviour's (0x80099454) is served here.
    const u32 filter = body ? c.gpr[sweep ? 7 : 6] : 0u;
    if (filter != 0u && (!known_filter(filter) || cast_ == nullptr)) {
        ++filtered_;
        return false;
    }
    // The update's weak_ptr copies (weak + 2) and lock (use + 2) must not wrap, nor a release reach zero.
    if (updates && !counts_safe(c, c.mem_read32(self + 0x34u))) {
        ++other_;
        ++other_by_[static_cast<u32>(test)][2];
        return false;
    }

    const u32 cr = c.cr;
    const u32 fpscr = c.fpscr;
    Written written;
    save(c, self, written);
    const auto decline = [&](u64& counter) {
        restore(c, self, written);
        c.cr = cr;
        c.fpscr = fpscr;
        ++counter;
        return false;
    };
    u32 bone_cr = 0, bone_fpscr = 0;
    if (updates)
        update(c, self);
    if (bone) {
        const int done = bone_update(c, test, cr, fpscr);
        if (done < 0)
            return decline(other_);
        if (done == 0)
            return true;  // handed to the guest test at its return from vf 0x88
        bone_cr = c.cr;
        bone_fpscr = c.fpscr;
    }
    const u32 shape = body ? c.mem_read32(self + 0x5Cu) : 0u;  // vf 0x28
    const u64 radius = lfs(c.mem_read32(self + 8u));          // f31 (ray)
    u64 reach = 0;
    if (sweep)  // f31 = radius + sphere.r
        reach = ppc::fps_add(c, radius, lfs(c.mem_read32(c.gpr[4] + 0x0Cu)));
    if (filter != 0u) {
        const int keep = run_filter(c, filter, shape, self, c.gpr[sweep ? 8 : 7]);
        if (keep < 0) {
            ++other_by_[static_cast<u32>(test)][2];
            return decline(other_);
        }
        ++filter_runs_;
        if (keep == 0)
            return rejected(c, cr);
    }

    u64 d2, bound;
    if (!sweep) {
        const V3 segment_a = read3(c, c.gpr[4]);
        const V3 segment_b = read3(c, c.gpr[4] + 0x0Cu);
        d2 = distance_squared(c, read3(c, self + 0x40u), segment_a, segment_b);
        bound = ppc::fps_mul(c, radius, radius);
    } else {
        const u32 sphere = c.gpr[4];
        const V3 centre = read3(c, sphere);
        const V3 end = add(c, centre, read3(c, c.gpr[5]));
        d2 = distance_squared(c, read3(c, self + 0x40u), copy(centre), copy(end));  // 0x80020A20
        const u64 outer = ppc::fps_add(c, reach, lfs(c.mem_read32(sphere + 0x0Cu)));
        bound = ppc::fps_mul(c, outer, outer);
    }
    ppc::fp_compare(c, 0u, d2, bound, true);  // fcmpo f1, f0; ble: the narrow phase
    if (!ppc::cr_bit(c, 1u)) {
        if (!bone)
            return decline(passed_);
        // The update's writes stand: the guest test continues from its vf 0x88 return, as the update left it.
        c.cr = bone_cr;
        c.fpscr = bone_fpscr;
        c.leave_to_dispatcher(bone_test_frame(c, test));
        ++passed_;
        return true;
    }
    return rejected(c, cr);
}

bool CharaColliIntrinsic::rejected(Cpu& c, u32 cr) {
    // Rejected: return 0, f31 through its paired-single save and restore (psq_st / psq_l with GQR0, then lfd), and
    // the prologue's mflr / stw r0 into the caller's LR save word at r1 + 4.
    c.cr = cr;
    c.mem_write32(c.gpr[1] + 4u, c.lr);
    c.fpr[31].bits[1] = lfs(double_to_single_bits_flush(c.fpr[31].bits[1]));
    c.gpr[3] = 0u;
    ++served_;
    return true;
}

// 0x80099454 (pair r3: +4 shape, +8 object, +0xC shape+0x1C (its mask), +0x10 the filter's argument): 1 to test the
// object, 0 to skip it; -1 to decline (a count the guest's lock pair could drive to zero, a list that does not end).
//   (mask & arg[1]) -> 0
//   the object is in the list at arg + 8 (sentinel arg + 0xC, next +4, object +8) -> 0
//   arg[5] (a target) and the object dynamic_casts (vptr offset 0, to r13 - 0x6E80, from r13 - 0x6E88) to p, whose
//   owner weak_ptr (p + 0x30 / + 0x34) locks to q != 0 with team q+0x5C 1 or 2:
//     the target's team (arg[5] + 0x5C) neither q's nor 3 -> 0; q's flags word q+0xA8 bit 8 set -> 0
//   else (mask & arg[0]) != 0
// A Body test's filter (one of known_filter's): 1 to test the object, 0 to skip it, -1 to decline.
int CharaColliIntrinsic::run_filter(Cpu& c, u32 filter, u32 shape, u32 self, u32 arg) {
    const u32 mask = c.mem_read32(shape + 0x1Cu);  // the pair's +0xC
    switch (filter) {
    case kMaskAny1AB3: return (mask & 0x1AB3u) != 0u ? 1 : 0;
    case kMaskNone50008: return (mask & 0x50008u) == 0u ? 1 : 0;
    case kMaskNone100108: return (mask & 0x100108u) == 0u ? 1 : 0;
    case kNotTheseTwo: {  // 0x801C0910 (arg r5 = pair +0x10; arg[0] read first)
        const u32 bits = c.mem_read32(arg);
        if (self != 0u && (self == c.mem_read32(arg + 0x0Cu) || self == c.mem_read32(arg + 4u)))
            return 0;
        return (mask & bits) != 0u ? 1 : 0;
    }
    case kNotThatOwner: {  // 0x8005B6C8
        if ((mask & 0xC0018u) != 0u)
            return 0;
        if (self == 0u)
            return 1;
        const u32 p = cast_->cast(c, self, 0u, c.gpr[13] + 0xFFFF8688u, c.gpr[13] + 0xFFFF8680u);
        if (p == 0u)
            return 1;
        const u32 counts = c.mem_read32(p + 0x34u);  // its owner weak_ptr locked (0x80058814 / 0x8005223C), released
        if (!counts_safe(c, counts))
            return -1;
        const bool expired = counts == 0u || c.mem_read32(counts + 4u) == 0u;
        const u32 q = expired ? 0u : c.mem_read32(p + 0x30u);
        if (q != 0u) {
            const u32 id = c.mem_read32(arg);
            if (id != 0xFFFFFFFFu && id == c.mem_read32(q + 0x54u))
                return 0;
        }
        return 1;
    }
    default: return ground_filter(c, shape, self, arg);
    }
}

int CharaColliIntrinsic::ground_filter(Cpu& c, u32 shape, u32 self, u32 arg) {
    const u32 mask = c.mem_read32(shape + 0x1Cu);
    if ((mask & c.mem_read32(arg + 4u)) != 0u)
        return 0;
    if (self != 0u) {
        const u32 sentinel = arg + 0x0Cu;
        u32 node = c.mem_read32(sentinel + 4u);
        for (u32 steps = 0; node != sentinel; ++steps) {
            if (steps == (1u << 20))
                return -1;
            if (c.mem_read32(node + 8u) == self)
                return 0;
            node = c.mem_read32(node + 4u);
        }
        const u32 target = c.mem_read32(arg + 0x14u);
        if (target != 0u) {
            const u32 p = cast_->cast(c, self, 0u, c.gpr[13] + 0xFFFF9180u, c.gpr[13] + 0xFFFF9178u);
            if (p != 0u) {
                const u32 counts = c.mem_read32(p + 0x34u);
                if (!counts_safe(c, counts))
                    return -1;
                const bool expired = counts == 0u || c.mem_read32(counts + 4u) == 0u;
                const u32 q = expired ? 0u : c.mem_read32(p + 0x30u);
                if (q != 0u) {
                    const u32 team = c.mem_read32(q + 0x5Cu);
                    if (team == 1u || team == 2u) {
                        const u32 other = c.mem_read32(c.mem_read32(arg + 0x14u) + 0x5Cu);
                        if (other != team && other != 3u)
                            return 0;
                        if ((c.mem_read32(q + 0xA8u) & 0x100u) != 0u)  // 0x8006CEA0 (q + 0xA8, 8)
                            return 0;
                    }
                }
            }
        }
    }
    return (mask & c.mem_read32(arg)) != 0u ? 1 : 0;
}

bool CharaColliIntrinsic::verify(Cpu& c, Test test) {
    const u32 entry = entry_of(test);
    const RecompiledCode* code = c.recompiled();
    const RecompiledCode::Range* body = code != nullptr ? code->find_range(entry) : nullptr;
    if (body == nullptr)
        return execute(c, test);
    Window w[kWindows];
    windows_of(c, test, w);
    static Snapshot before, native, guest;
    take(c, w, before);
    // A GeneralBone test writes its children (outside the windows): every 8th is compared on all memory.
    const u32 self_vtable = c.mem_read32(c.gpr[3]);
    const bool bone = !is_body(test) && c.mem_read32(self_vtable + 0x88u) == kBoneUpdate;
    const bool full = (verified_ & 1023u) == 0u || (bone && (bone_verified_++ & 7u) == 0u);
    GuestMemory& memory = c.guest_memory();
    if (full)
        take_all(memory, full_before_);
    const u32 pc = c.pc;
    if (!execute(c, test)) {
        // Declined: nothing may have changed.
        take(c, w, native);
        ++verified_;
        if (std::memcmp(&before, &native, sizeof(before)) != 0 && ++differed_ <= 8u)
            std::fprintf(stderr, "[chara-colli] a declined test changed state: %08X (test %u)\n", c.gpr[3],
                         static_cast<unsigned>(test));
        return false;
    }
    if (c.exiting()) {  // handed to the guest test mid-way (GeneralBone): nothing to compare
        ++incomplete_;
        return true;
    }
    take(c, w, native);
    if (full)
        take_all(memory, full_native_);
    put(c, w, before);
    if (full)
        put_all(memory, full_before_);
    // The translated body itself (the entry thunk would come back to this hook), its published transfers driven
    // until it returns: the body is split at its calls.
    const u32 link = c.lr;
    body->at(c, entry);
    c.finish_transfers();
    if (c.pc != link) {
        // Cut short (an interrupt became deliverable inside it): nothing to compare. Keep the native result; the
        // pending exit is taken at the return address as after any served call.
        if (full)
            put_all(memory, full_native_);
        put(c, w, native);
        c.pc = pc;
        ++incomplete_;
        return true;
    }
    c.pc = pc;  // as a served call leaves it
    take(c, w, guest);
    ++verified_;
    u32 elsewhere = 0u;
    if (full) {
        ++full_checks_;
        take_all(memory, full_guest_);
        if (full_guest_.size() == full_native_.size())
            elsewhere = first_difference(memory, full_native_, full_guest_, before.gpr[1]);
    }
    if (!same_contract(native, guest) || elsewhere != 0u) {
        if (++differed_ <= 8u) {
            std::fprintf(stderr, "[chara-colli] native differs from the guest body: this %08X test %u\n", before.gpr[3],
                         static_cast<unsigned>(test));
            for (u32 i = 0; i < 32u; ++i)
                if (native.gpr[i] != guest.gpr[i])
                    std::fprintf(stderr, "  r%u native %08X guest %08X\n", i, native.gpr[i], guest.gpr[i]);
            for (u32 i = 14; i < 32u; ++i)
                if (native.fpr[i][0] != guest.fpr[i][0] || native.fpr[i][1] != guest.fpr[i][1])
                    std::fprintf(stderr, "  f%u native %016llX %016llX guest %016llX %016llX\n", i,
                                 static_cast<unsigned long long>(native.fpr[i][0]),
                                 static_cast<unsigned long long>(native.fpr[i][1]),
                                 static_cast<unsigned long long>(guest.fpr[i][0]),
                                 static_cast<unsigned long long>(guest.fpr[i][1]));
            if (!same_fpscr(native.fpscr, guest.fpscr))
                std::fprintf(stderr, "  fpscr native %08X guest %08X\n", native.fpscr, guest.fpscr);
            if (((native.cr ^ guest.cr) & 0x00FFF000u) != 0u)
                std::fprintf(stderr, "  cr native %08X guest %08X\n", native.cr, guest.cr);
            for (u32 i = 0; i < kWindows; ++i)
                if (std::memcmp(native.bytes[i], guest.bytes[i], kWindowBytes) != 0)
                    std::fprintf(stderr, "  memory window %u (%08X) differs\n", i, w[i].base);
            if (elsewhere != 0u) {
                std::fprintf(stderr, "  memory differs at %08X (r1 %08X):", elsewhere, before.gpr[1]);
                std::size_t at = 0;
                u32 shown = 0;
                for (const GuestMemory::Region& region : memory.regions()) {
                    for (u32 i = 0; i < region.committed && shown < 24u; ++i)
                        if (full_native_[at + i] != full_guest_[at + i] && region.base + i >= before.gpr[1]) {
                            std::fprintf(stderr, " %08X n%02X g%02X", region.base + i, full_native_[at + i],
                                         full_guest_[at + i]);
                            ++shown;
                        }
                    at += region.committed;
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    return true;
}

// ---- CharaColliGeneralBone's update ----

namespace {

// GeneralBone's own vtable slots the update dispatches through (0x800C3D6C, 0x800C3C3C).
constexpr u32 kBoneDirty = 0x800C34D8u;    // vf 0x8C: the children re-derived from the owner when dirty
constexpr u32 kBoneRepose = 0x800C3E00u;   // vf 0x90: each child re-posed on its bone (vf 0x54)
constexpr u32 kBoneRadius = 0x800C3C3Cu;   // vf 0x84: the bounding radius over the children
// The Range child classes on a bone and the code their slots name: 0x0C local position (from + 0x2C), 0x10 the
// Euler angles (+ 0x40), 0x14 their order (+ 0x4C), 0x54 the re-pose.
struct ChildClass {
    u32 vtable, local, angles, order, repose;
    bool oriented;  // capsule / cylinder: a matrix, its inverse and the axis finisher; a sphere: a point
};
constexpr ChildClass kChildClasses[] = {
    {0x8051DC88u, 0x8002DD6Cu, 0x8002DD9Cu, 0x8002DDA4u, 0x8002F664u, true},   // Range::RangeCapsule
    {0x8051DD04u, 0x80030BC0u, 0x80030BF0u, 0x80030BF8u, 0x8003291Cu, true},   // Range::RangeCylinder
    {0x8051DE14u, 0x80034AF4u, 0x80034B24u, 0x80028C94u, 0x800354A0u, false},  // Range::RangeSphere
};
constexpr u32 kMaxChildren = 32u;
constexpr u32 kFpscrControl = 0x000000FFu;     // VE OE UE ZE XE NI RN
constexpr u32 kFpscrExceptions = 0xFFF80700u;  // FX FEX VX OX UX ZX XX, the VX causes

const ChildClass* child_class(u32 vtable) noexcept {
    for (const ChildClass& k : kChildClasses)
        if (k.vtable == vtable)
            return &k;
    return nullptr;
}

}  // namespace

// The update's whole read set, in a fixed order, and the regions it writes. The update is 0x800C409C (served before
// this, here clean: nothing written), vf 0x8C (clean: nothing), vf 0x90 and vf 0x84:
//   vf 0x90: the bone array at this+0x6C walked with the child list (sentinel this+0x64, first this+0x68, next
//     node+4, child node+8); a child whose bone pointer is non-null is re-posed:
//       sphere: centre (+4) = bone x local (+0x2C)
//       capsule / cylinder: +0x50 = bone x translation(local); the Euler rotation (+0x40 angles, +0x4C order,
//         through the MSL sin / cos) applied to it; centre = its translation column; +0x80 = its inverse
//         (0x8040CF48); the axis (k, k, half length +0x38) rotated by it: ends centre -/+ axis (+0xB0 / +0xBC),
//         its direction normalised (+0xC8)
//     this+0x29 = 1
//   vf 0x84: this+8 = max over the children of |centre - this+0x40| + radius (+0x10) for the enabled (+0x19) ones,
//     the first child counted however enabled
// Inputs: r2 (the constants it reads), the FPSCR control bits, this's vtable slots, this+0x40 .. +0x48, the list,
// the bone pointers and matrices, each child's vtable and the slots it calls, local position, angles, order, half
// length, radius, enable byte, and for a child that is not re-posed its centre. Constants and code are the DOL's.
bool CharaColliIntrinsic::bone_inputs(Cpu& c, u32 self, std::vector<u32>& key, std::vector<u32>& outputs) {
    key.clear();
    outputs.clear();
    bone_oriented_ = false;
    const u32 vtable = c.mem_read32(self);
    if (c.mem_read32(vtable + 0x8Cu) != kBoneDirty || c.mem_read32(vtable + 0x90u) != kBoneRepose ||
        c.mem_read32(vtable + 0x84u) != kBoneRadius)
        return false;
    key.push_back(c.gpr[2]);
    key.push_back(c.fpscr & kFpscrControl);
    key.push_back(vtable);
    key.push_back(c.mem_read32(self + 0x40u));
    key.push_back(c.mem_read32(self + 0x44u));
    key.push_back(c.mem_read32(self + 0x48u));
    const u32 sentinel = self + 0x64u;
    const u32 bones = c.mem_read32(self + 0x6Cu);
    key.push_back(bones);
    u32 node = c.mem_read32(sentinel + 4u);
    if (node == sentinel)
        return false;  // vf 0x84 would read the sentinel's word as a child
    outputs.push_back(self + 8u);  // the radius (a word); this+0x29 is the one byte, written as 1
    for (u32 n = 0; node != sentinel; ++n, node = c.mem_read32(node + 4u)) {
        if (n == kMaxChildren)
            return false;
        const u32 child = c.mem_read32(node + 8u);
        const u32 bone = c.mem_read32(bones + 4u * n);
        const u32 cvt = c.mem_read32(child);
        const ChildClass* k = child_class(cvt);
        if (k == nullptr || c.mem_read32(cvt + 0x0Cu) != k->local || c.mem_read32(cvt + 0x54u) != k->repose ||
            (k->oriented && (c.mem_read32(cvt + 0x10u) != k->angles || c.mem_read32(cvt + 0x14u) != k->order)))
            return false;
        key.push_back(node);
        key.push_back(child);
        key.push_back(bone);
        key.push_back(cvt);
        for (u32 a = 0x2Cu; a <= 0x34u; a += 4u)
            key.push_back(c.mem_read32(child + a));
        key.push_back(c.mem_read32(child + 0x10u));
        key.push_back(c.mem_read8(child + 0x19u));
        if (bone == 0u) {
            for (u32 a = 4u; a <= 0x0Cu; a += 4u)  // not re-posed: vf 0x84 reads the centre as it is
                key.push_back(c.mem_read32(child + a));
            continue;
        }
        for (u32 a = 0; a < 0x30u; a += 4u)
            key.push_back(c.mem_read32(bone + a));
        for (u32 a = 4u; a <= 0x0Cu; a += 4u)
            outputs.push_back(child + a);
        if (k->oriented) {
            bone_oriented_ = true;  // 0x8040CF48 saves f21-f31 with psq_st
            for (u32 a = 0x38u; a <= 0x4Cu; a += 4u)  // half length, angles, order
                key.push_back(c.mem_read32(child + a));
            for (u32 a = 0x50u; a < 0xD4u; a += 4u)
                outputs.push_back(child + a);
        }
    }
    return true;
}

// 1: served from a recorded run (the test continues natively); 0: the update run by the guest (and recorded) and the
// test handed to the guest at vf 0x88's return; -1: decline.
int CharaColliIntrinsic::bone_update(Cpu& c, Test test, u32 cr, u32 fpscr) {
    const u32 self = c.gpr[3];
    const bool dirty = c.mem_read8(self + 0x28u) != 0u;
    // A clean object's update is a function of its gathered inputs: a recorded run is reused.
    const bool keyed = bone_memo_on_ && !dirty && bone_inputs(c, self, bone_key_, bone_outputs_);
    if (bone_memo_on_ && !dirty && !keyed)
        ++bone_unkeyed_;
    if (keyed) {
        auto found = bone_memo_.find(self);
        if (found != bone_memo_.end() && found->second.key == bone_key_ &&
            (found->second.exceptions & ~c.fpscr) == 0u) {
            // The guest's run would write these same values and leave FPCC as recorded; the FPRs its callees save
            // with psq_st come back with ps1 rounded to single.
            const BoneRun& run = found->second;
            for (std::size_t i = 0; i < bone_outputs_.size(); ++i)
                c.mem_write32(bone_outputs_[i], run.values[i]);
            c.mem_write8(self + 0x29u, 1u);
            c.fpscr = (c.fpscr & ~kFpscrFPCC) | run.fpcc;
            for (u32 f = run.wide ? 21u : 30u; f < 32u; ++f)
                c.fpr[f].bits[1] = lfs(double_to_single_bits_flush(c.fpr[f].bits[1]));
            ++bone_hits_;
            return 1;
        }
    }
    const auto record = [&](u32 before, bool wide) {
        // Recorded only when the run changed no FPSCR bit but FPCC: whatever it raised was already set. An
        // identical run raises the same, so it changes nothing while those exception bits are still set.
        if (!keyed || ((c.fpscr ^ before) & ~kFpscrFPCC) != 0u)
            return;
        BoneRun& run = bone_memo_[self];
        run.key = bone_key_;
        run.values.resize(bone_outputs_.size());
        for (std::size_t i = 0; i < bone_outputs_.size(); ++i)
            run.values[i] = c.mem_read32(bone_outputs_[i]);
        run.fpcc = c.fpscr & kFpscrFPCC;
        run.exceptions = before & kFpscrExceptions;
        run.wide = wide;
        ++bone_recorded_;
    };

    // Computed natively (gekko/chara_colli_bone.h).
    if (bone_native_) {
        const u32 before = c.fpscr;
        if (verify_)
            bone_snapshot(c, self, bone_pre_);
        bool wide = false;
        if (chara_colli_bone::update(c, self, &wide)) {
            ++(dirty ? bone_native_dirty_ : bone_native_clean_);
            if (verify_ && verify_bone(c, test, self) == 0)
                return 0;  // the guest reference was cut short: the guest test continues from inside it
            record(before, wide);
            return 1;
        }
        ++bone_native_declined_;
    }

    // Outside the native contract: the guest test's prologue up to its vf 0x88 call, then that call nested.
    c.cr = cr;
    c.fpscr = fpscr;
    u32 entry_gpr[32];
    std::memcpy(entry_gpr, c.gpr, sizeof(entry_gpr));
    const u32 entry_lr = c.lr;
    const u32 link = bone_test_frame(c, test);
    const u32 before = c.fpscr;
    ++bone_misses_;
    if (call_guest(c, kBoneUpdate, link) == 0) {
        c.leave_to_dispatcher(c.pc);  // cut short: the guest resumes inside it and returns into its test
        return 0;
    }
    // Returned: the test continues natively from vf 0x88's return, its registers as at its entry (the frame the
    // call needed is dead below r1 again).
    std::memcpy(c.gpr, entry_gpr, sizeof(entry_gpr));
    c.lr = entry_lr;
    record(before, bone_oriented_);
    return 1;
}

// Every word and byte the update can write (chara_colli_bone.h), FPSCR and the FPRs.
void CharaColliIntrinsic::bone_snapshot(Cpu& c, u32 self, BoneState& s) {
    s.addresses.clear();
    s.values.clear();
    s.addresses.push_back(self + 8u);
    const u32 sentinel = self + 0x64u;
    u32 node = c.mem_read32(sentinel + 4u);
    for (u32 n = 0; node != sentinel && n < 32u; ++n, node = c.mem_read32(node + 4u)) {
        const u32 child = c.mem_read32(node + 8u);
        for (u32 a = 4u; a <= 0x0Cu; a += 4u)
            s.addresses.push_back(child + a);
        if (c.mem_read32(child) != 0x8051DE14u)  // a capsule or cylinder: +0x50 .. +0xD3 too
            for (u32 a = 0x50u; a < 0xD4u; a += 4u)
                s.addresses.push_back(child + a);
    }
    for (const u32 a : s.addresses)
        s.values.push_back(c.mem_read32(a));
    s.dirty = c.mem_read8(self + 0x28u);
    s.posed = c.mem_read8(self + 0x29u);
    s.fpscr = c.fpscr;
    for (u32 i = 0; i < 32u; ++i) {
        s.fpr[i][0] = c.fpr[i].bits[0];
        s.fpr[i][1] = c.fpr[i].bits[1];
    }
}

// --verify-chara-colli: the native update against the guest's, run from the same state; the guest's is kept.
int CharaColliIntrinsic::verify_bone(Cpu& c, Test test, u32 self) {
    BoneState& pre = bone_pre_;
    BoneState& native = bone_native_state_;
    bone_snapshot(c, self, native);
    const bool full = (bone_checked_ & 15u) == 0u;
    GuestMemory& memory = c.guest_memory();
    // The test's own prologue stores the return address in its caller's LR save word (every path of the test does,
    // the reference's frame below included).
    c.mem_write32(c.gpr[1] + 4u, c.lr);
    if (full)
        take_all(memory, full_native_);
    // Back to the state the native update started from.
    for (std::size_t i = 0; i < pre.addresses.size(); ++i)
        c.mem_write32(pre.addresses[i], pre.values[i]);
    c.mem_write8(self + 0x28u, pre.dirty);
    c.mem_write8(self + 0x29u, pre.posed);
    c.fpscr = pre.fpscr;
    for (u32 i = 0; i < 32u; ++i) {
        c.fpr[i].bits[0] = pre.fpr[i][0];
        c.fpr[i].bits[1] = pre.fpr[i][1];
    }
    u32 entry_gpr[32];
    std::memcpy(entry_gpr, c.gpr, sizeof(entry_gpr));
    const u32 entry_lr = c.lr;
    const u32 entry_sp = c.gpr[1];
    const u32 link = bone_test_frame(c, test);
    reference_ = true;
    const int returned = call_guest(c, kBoneUpdate, link);
    reference_ = false;
    if (returned == 0) {
        c.leave_to_dispatcher(c.pc);
        ++incomplete_;
        return 0;
    }
    std::memcpy(c.gpr, entry_gpr, sizeof(entry_gpr));
    c.lr = entry_lr;
    BoneState& guest = bone_guest_state_;
    bone_snapshot(c, self, guest);
    ++bone_checked_;
    u32 elsewhere = 0u;
    if (full) {
        take_all(memory, full_guest_);
        elsewhere = full_guest_.size() == full_native_.size()
                        ? first_difference(memory, full_native_, full_guest_, entry_sp)
                        : 1u;
    }
    bool same = native.values == guest.values && native.dirty == guest.dirty && native.posed == guest.posed &&
                same_fpscr(native.fpscr, guest.fpscr) && elsewhere == 0u;
    for (u32 i = 14; i < 32u; ++i)
        same = same && native.fpr[i][0] == guest.fpr[i][0] && native.fpr[i][1] == guest.fpr[i][1];
    if (!same && ++bone_differed_ <= 8u) {
        std::fprintf(stderr, "[chara-colli] native GeneralBone update differs from the guest's: %08X (dirty %u)\n",
                     self, static_cast<unsigned>(pre.dirty));
        for (std::size_t i = 0; i < native.values.size() && i < guest.values.size(); ++i)
            if (native.values[i] != guest.values[i])
                std::fprintf(stderr, "  %08X native %08X guest %08X\n", native.addresses[i], native.values[i],
                             guest.values[i]);
        if (native.dirty != guest.dirty || native.posed != guest.posed)
            std::fprintf(stderr, "  bytes native %u %u guest %u %u\n", static_cast<unsigned>(native.dirty),
                         static_cast<unsigned>(native.posed), static_cast<unsigned>(guest.dirty),
                         static_cast<unsigned>(guest.posed));
        if (!same_fpscr(native.fpscr, guest.fpscr))
            std::fprintf(stderr, "  fpscr native %08X guest %08X\n", native.fpscr, guest.fpscr);
        for (u32 i = 14; i < 32u; ++i)
            if (native.fpr[i][0] != guest.fpr[i][0] || native.fpr[i][1] != guest.fpr[i][1])
                std::fprintf(stderr, "  f%u native %016llX %016llX guest %016llX %016llX\n", i,
                             static_cast<unsigned long long>(native.fpr[i][0]),
                             static_cast<unsigned long long>(native.fpr[i][1]),
                             static_cast<unsigned long long>(guest.fpr[i][0]),
                             static_cast<unsigned long long>(guest.fpr[i][1]));
        if (elsewhere != 0u)
            std::fprintf(stderr, "  memory differs at %08X\n", elsewhere);
    }
    if ((bone_checked_ & 63u) == 1u)
        fuzz_bone(c, test, self);
    return 1;
}

// --verify-chara-colli: unusual inputs. Each case changes input words of the object as it stands, runs the native
// update and then the guest's from that same changed state, compares them, and puts every word, byte, FPR and FPSCR
// back as they were. The guest reference runs with MSR[EE] clear, so no interrupt can cut it short (one becoming due
// stays latched and is taken after EE is set again).
void CharaColliIntrinsic::fuzz_bone(Cpu& c, Test test, u32 self) {
    struct Change {
        u32 address, value;
        bool byte;
    };
    constexpr u32 kNaN = 0x7FC00000u, kInf = 0x7F800000u, kMinusInf = 0xFF800000u, kDenormal = 0x00000001u,
                  kMinusZero = 0x80000000u, kLarge = 0x47C35000u, kSignalling = 0x7F800001u;
    const u32 sentinel = self + 0x64u;
    u32 children[32], vtables[32], count = 0;
    for (u32 node = c.mem_read32(sentinel + 4u); node != sentinel && count < 32u; node = c.mem_read32(node + 4u)) {
        children[count] = c.mem_read32(node + 8u);
        vtables[count] = c.mem_read32(children[count]);
        ++count;
    }
    if (count == 0u)
        return;
    const u32 pick = static_cast<u32>(bone_fuzz_rounds_++ % count);
    const u32 child = children[pick];
    const bool oriented = vtables[pick] != 0x8051DE14u;
    const u32 bone = c.mem_read32(c.mem_read32(self + 0x6Cu) + 4u * pick);
    std::vector<std::vector<Change>> cases;
    for (const u32 v : {kNaN, kInf, kMinusInf, kDenormal, kMinusZero, kLarge, kSignalling})
        for (u32 a = 0; a < 3u && oriented; ++a)
            cases.push_back({{child + 0x40u + 4u * a, v, false}});
    if (oriented) {
        cases.push_back({{child + 0x38u, 0u, false}});      // a zero axis: Normalize's fallback
        cases.push_back({{child + 0x38u, kNaN, false}});
        for (const u32 order : {1u, 2u, 7u, 0xFFFFFFFFu})
            cases.push_back({{child + 0x4Cu, order, false}});
    }
    cases.push_back({{child + 0x2Cu, kNaN, false}});
    cases.push_back({{child + 0x30u, kDenormal, false}});
    cases.push_back({{child + 0x10u, kNaN, false}});        // vf 0x84's unordered compare
    cases.push_back({{self + 0x40u, kInf, false}});
    if (bone != 0u) {
        std::vector<Change> singular;
        for (u32 i = 0; i < 12u; ++i)
            singular.push_back({bone + 4u * i, 0u, false});  // det 0: the inverse is a copy
        cases.push_back(singular);
        cases.push_back({{bone, kNaN, false}});
        cases.push_back({{bone + 0x0Cu, kMinusInf, false}});
    }
    // Dirty: vf 0x8C from the object's rotation and order.
    cases.push_back({{self + 0x28u, 1u, true}});
    cases.push_back({{self + 0x28u, 1u, true}, {self + 0x4Cu, kNaN, false}});
    cases.push_back({{self + 0x28u, 1u, true}, {self + 0x50u, kLarge, false}, {self + 0x58u, 2u, false}});
    for (const Change& k : {Change{child + 0x18u, 1u, true}})  // a sphere's rotated dirty path
        if (!oriented)
            cases.push_back({k, {self + 0x28u, 1u, true}, {self + 0x54u, kLarge, false}});

    BoneState& keep = bone_fuzz_keep_;
    bone_snapshot(c, self, keep);
    const u32 cr = c.cr;
    u32 entry_gpr[32];
    std::memcpy(entry_gpr, c.gpr, sizeof(entry_gpr));
    const u32 entry_lr = c.lr;
    const auto put_state = [&c, self](const BoneState& s) {
        for (std::size_t i = 0; i < s.addresses.size(); ++i)
            c.mem_write32(s.addresses[i], s.values[i]);
        c.mem_write8(self + 0x28u, s.dirty);
        c.mem_write8(self + 0x29u, s.posed);
        c.fpscr = s.fpscr;
        for (u32 i = 0; i < 32u; ++i) {
            c.fpr[i].bits[0] = s.fpr[i][0];
            c.fpr[i].bits[1] = s.fpr[i][1];
        }
    };
    for (const std::vector<Change>& changes : cases) {
        u32 old[16];
        for (std::size_t i = 0; i < changes.size(); ++i) {
            old[i] = changes[i].byte ? c.mem_read8(changes[i].address) : c.mem_read32(changes[i].address);
            if (changes[i].byte)
                c.mem_write8(changes[i].address, static_cast<u8>(changes[i].value));
            else
                c.mem_write32(changes[i].address, changes[i].value);
        }
        BoneState& start = bone_pre_;
        bone_snapshot(c, self, start);
        const bool native_ok = chara_colli_bone::update(c, self);
        if (native_ok) {
            BoneState& native = bone_native_state_;
            bone_snapshot(c, self, native);
            put_state(start);
            bone_test_frame(c, test);
            const u32 msr = c.msr();
            c.set_msr(msr & ~kMsrEE);
            // vf 0x8C, 0x90, 0x84 as 0x800C3D6C calls them, after its 0x800C409C: the owner sync is done, and run
            // again it would copy the owner over a changed position or rotation.
            reference_ = true;
            const u32 vt = c.mem_read32(self);
            int returned = 1;
            for (const auto [slot, link] : {std::pair{0x8Cu, 0x800C3D98u}, {0x90u, 0x800C3DACu}, {0x84u, 0x800C3DC0u}}) {
                c.gpr[3] = self;
                if (returned == 1)
                    returned = call_guest(c, c.mem_read32(vt + slot), link);
            }
            reference_ = false;
            c.set_msr(msr);
            std::memcpy(c.gpr, entry_gpr, sizeof(entry_gpr));
            c.lr = entry_lr;
            BoneState& guest = bone_guest_state_;
            bone_snapshot(c, self, guest);
            ++bone_fuzzed_;
            bool same = returned == 1 && native.values == guest.values && native.dirty == guest.dirty &&
                        native.posed == guest.posed && same_fpscr(native.fpscr, guest.fpscr);
            for (u32 i = 14; i < 32u; ++i)
                same = same && native.fpr[i][0] == guest.fpr[i][0] && native.fpr[i][1] == guest.fpr[i][1];
            if (!same && ++bone_fuzz_differed_ <= 8u) {
                std::fprintf(stderr, "[chara-colli] fuzzed GeneralBone update differs: %08X, change %08X = %08X%s\n",
                             self, changes[0].address, changes[0].value, returned == 1 ? "" : " (guest cut short)");
                for (std::size_t i = 0; i < native.values.size() && i < guest.values.size(); ++i)
                    if (native.values[i] != guest.values[i])
                        std::fprintf(stderr, "  %08X native %08X guest %08X\n", native.addresses[i],
                                     native.values[i], guest.values[i]);
                if (!same_fpscr(native.fpscr, guest.fpscr))
                    std::fprintf(stderr, "  fpscr native %08X guest %08X\n", native.fpscr, guest.fpscr);
                for (u32 i = 14; i < 32u; ++i)
                    if (native.fpr[i][1] != guest.fpr[i][1] || native.fpr[i][0] != guest.fpr[i][0])
                        std::fprintf(stderr, "  f%u differs\n", i);
            }
        } else {
            ++bone_fuzz_declined_;
        }
        // Everything back as the game left it.
        put_state(keep);
        for (std::size_t i = changes.size(); i-- > 0u;) {
            if (changes[i].byte)
                c.mem_write8(changes[i].address, static_cast<u8>(old[i]));
            else
                c.mem_write32(changes[i].address, old[i]);
        }
        put_state(keep);
        c.cr = cr;
    }
}

// The guest test (0x800C31EC / 0x800C2854) as it stands at its vf 0x88 call: its frame and saved registers, its
// argument registers moved, r0 the link. Returns the call's return address.
u32 CharaColliIntrinsic::bone_test_frame(Cpu& c, Test test) {
    u32* const g = c.gpr;
    const u32 caller_sp = g[1];
    const u32 link_in = c.lr;
    u32 link;
    if (test == Test::Manage) {  // 0x800C3DD4: stwu -0x10, mflr / stw r0 at 0x14
        const u32 f = caller_sp - 0x10u;
        c.mem_write32(f, caller_sp);
        c.mem_write32(caller_sp + 4u, link_in);
        g[1] = f;
        link = 0x800C3DF0u;
    } else if (test == Test::GeneralRay) {  // stwu -0xB0, stmw r24 at 0x90, r24-r28 = r3-r7
        const u32 f = caller_sp - 0xB0u;
        c.mem_write32(f, caller_sp);
        c.mem_write32(caller_sp + 4u, link_in);
        for (u32 i = 24; i < 32u; ++i)
            c.mem_write32(f + 0x90u + 4u * (i - 24u), g[i]);
        for (u32 i = 0; i < 5u; ++i)
            g[24u + i] = g[3u + i];
        g[1] = f;
        link = 0x800C3220u;
    } else {  // stwu -0xF0, f31 by stfd at 0xE0 and psq_st at 0xE8, stmw r23 at 0xBC, r23-r28 = r3-r8
        const u32 f = caller_sp - 0xF0u;
        c.mem_write32(f, caller_sp);
        c.mem_write32(caller_sp + 4u, link_in);
        c.mem_write64(f + 0xE0u, c.fpr[31].bits[0]);
        c.mem_write32(f + 0xE8u, double_to_single_bits_flush(c.fpr[31].bits[0]));
        c.mem_write32(f + 0xECu, double_to_single_bits_flush(c.fpr[31].bits[1]));
        for (u32 i = 23; i < 32u; ++i)
            c.mem_write32(f + 0xBCu + 4u * (i - 23u), g[i]);
        for (u32 i = 0; i < 6u; ++i)
            g[23u + i] = g[3u + i];
        g[1] = f;
        link = 0x800C2894u;
    }
    g[0] = link_in;
    const u32 vtable = c.mem_read32(g[3]);
    g[12] = c.mem_read32(vtable + 0x88u);
    c.ctr = g[12];
    return link;
}

int CharaColliIntrinsic::call_guest(Cpu& c, u32 target, u32 link) {
    c.gpr[12] = target;
    c.ctr = target;
    recomp::call_indirect(c, target, link);
    if (c.exiting())
        c.finish_transfers();
    return !c.exiting() && c.pc == link ? 1 : 0;
}

// ---- the query loops ----

namespace {

// 0x80039CC8 (bits r3, row r5, column r4): the manager's group-by-mask bit matrix.
GEKKO_ALWAYS_INLINE bool group_takes(Cpu& c, u32 bits, u32 mask, u32 group) {
    const u32 stride = c.mem_read32(bits + 0x0Cu);
    const u32 words = c.mem_read32(bits + 0x10u);
    const u32 word = c.mem_read32(words + (((mask >> 5) + group * stride) << 2));
    return (word & (1u << (mask & 31u))) != 0u;
}

// A weak_ptr {px, pi} copied into a frame slot: 0x80029338 (pi's weak count + 1).
GEKKO_ALWAYS_INLINE void weak_copy(Cpu& c, u32 slot, u32 px, u32 pi, bool count) {
    c.mem_write32(slot, px);
    c.mem_write32(slot + 4u, pi);
    if (count && pi != 0u)
        c.mem_write32(pi + 8u, c.mem_read32(pi + 8u) + 1u);
}
// 0x80028EA0 / 0x8005232C: expired (no counts, or a use count of 0).
GEKKO_ALWAYS_INLINE bool expired(Cpu& c, u32 pi) { return pi == 0u || c.mem_read32(pi + 4u) == 0u; }

CharaColliIntrinsic* stale_tracer = nullptr;  // --trace-stale-contacts

// The frame words a 0x8001FF00 result initialises (+0x2C 0, +0x30 -1, +0x34 1).
GEKKO_ALWAYS_INLINE void result_init(Cpu& c, u32 result) {
    if (stale_tracer != nullptr)
        stale_tracer->stale_note(c, result);
    c.mem_write32(result + 0x2Cu, 0u);
    c.mem_write32(result + 0x30u, 0xFFFFFFFFu);
    c.mem_write32(result + 0x34u, 1u);
}

// 0x800091EC through the FPRs (lfs, lfs, stfs, lfs, stfs, stfs).
GEKKO_ALWAYS_INLINE void copy3(Cpu& c, u32 to, u32 from) {
    const u32 x = c.mem_read32(from);
    const u32 y = c.mem_read32(from + 4u);
    c.mem_write32(to, stfs(lfs(x)));
    const u32 z = c.mem_read32(from + 8u);
    c.mem_write32(to + 4u, stfs(lfs(y)));
    c.mem_write32(to + 8u, stfs(lfs(z)));
}

bool test_of(u32 target, CharaColliIntrinsic::Test& test) noexcept {
    switch (target) {
    case CharaColliIntrinsic::kBodyRay: test = CharaColliIntrinsic::Test::BodyRay; return true;
    case CharaColliIntrinsic::kBodySweep: test = CharaColliIntrinsic::Test::BodySweep; return true;
    case CharaColliIntrinsic::kGeneralRay: test = CharaColliIntrinsic::Test::GeneralRay; return true;
    case CharaColliIntrinsic::kGeneralSweep: test = CharaColliIntrinsic::Test::GeneralSweep; return true;
    default: return false;
    }
}

}  // namespace

int CharaColliIntrinsic::call_test(Cpu& c, u32 target, u32 link) {
    // The guest's lwz r12 / mtctr r12 / bctrl.
    c.gpr[12] = target;
    c.ctr = target;
    c.lr = link;
    Test test;
    const bool ours = test_of(target, test);
    if (ours) {
        ++calls_;
        if (execute(c, test))
            return c.exiting() ? 0 : 1;  // a GeneralBone test may hand itself to the guest
    }
    ++nested_calls_;
    const RecompiledCode* code = c.recompiled();
    const RecompiledCode::Range* body = ours && code != nullptr ? code->find_range(target) : nullptr;
    if (body != nullptr)
        body->at(c, target);  // declined: its translated body (the entry thunk would come back to the hook)
    else
        recomp::call_indirect(c, target, link);
    if (c.exiting())
        c.finish_transfers();
    return !c.exiting() && c.pc == link ? 1 : 0;
}

bool CharaColliIntrinsic::hand_back(Cpu& c, u32 pc, u32 reason) {
    ++handed_back_[reason];
    c.leave_to_dispatcher(pc);
    return true;
}

bool CharaColliIntrinsic::serve_query(Cpu& c, Query query) {
    if (reference_ || (c.msr() & kMsrFp) == 0u)  // MSR[FP] clear: the guest's first lfs raises FP-unavailable
        return false;
    if (verify_)
        return verify_query(c, query);
    ++queries_;
    return run_query(c, query);
}

bool CharaColliIntrinsic::run_query(Cpu& c, Query query) {
    switch (query) {
    case Query::Ray: return run_ray(c);
    case Query::Sweep: return run_sweep(c);
    default: return run_enemy(c);
    }
}

// 0x800C5D20 (manager r3, segment r4, mask r5, with-result r6, filter r7, filter arg r8, r9): frame 0x130, r20-r31.
bool CharaColliIntrinsic::run_ray(Cpu& c) {
    u32* const g = c.gpr;
    const u32 caller_sp = g[1];
    const u32 f = caller_sp - 0x130u;
    const u32 link_in = c.lr;
    u32 saved[12];
    for (u32 i = 0; i < 12u; ++i)
        saved[i] = g[20u + i];
    // stwu r1,-0x130; mflr; stw r0,0x134; stmw r20,0x100.
    c.mem_write32(f, caller_sp);
    c.mem_write32(caller_sp + 4u, link_in);
    for (u32 i = 0; i < 12u; ++i)
        c.mem_write32(f + 0x100u + 4u * i, saved[i]);
    g[1] = f;
    g[30] = g[3];
    g[20] = g[4];
    g[22] = g[5];
    g[23] = g[6];
    g[24] = g[7];
    g[25] = g[8];
    g[31] = g[9];
    c.mem_write32(f + 0x64u, 0u);  // 0x80028E90: the hit's shared_ptr
    c.mem_write32(f + 0x68u, 0u);
    g[28] = 0u;
    result_init(c, f + 0xBCu);     // 0x8001FF00: the best result
    g[26] = 0u;
    copy3(c, f + 0x6Cu, g[20]);    // 0x80021DE4: the segment
    copy3(c, f + 0x78u, g[20] + 0x0Cu);
    g[29] = f + 0x78u;
    g[27] = 0u;
    g[21] = 0u;
    for (; static_cast<s32>(g[27]) < static_cast<s32>(c.mem_read32(g[30] + 4u)); ++g[27], g[21] += 0x10u) {
        if (c.mem_read32(c.mem_read32(g[30]) + g[21] + 4u) == 0u)
            continue;
        if (!group_takes(c, g[30] + 8u, g[22], g[27]))
            continue;
        c.mem_write32(f + 0x18u, 0u);
        c.mem_write32(f + 0x14u, 0u);
        const u32 first = c.mem_read32(c.mem_read32(g[30]) + g[21] + 8u);
        c.mem_write32(f + 0x10u, first);
        c.mem_write32(f + 0x18u, first);
        while (c.mem_read32(f + 0x18u) != c.mem_read32(c.mem_read32(g[30]) + g[21] + 0x0Cu)) {
            const u32 node = c.mem_read32(f + 0x18u);
            const u32 px = c.mem_read32(node);
            const u32 pi = c.mem_read32(node + 4u);
            if (!counts_safe(c, pi))
                return hand_back(c, 0x800C5DDCu, 2u);
            weak_copy(c, f + 0x54u, px, pi, false);  // copied and released: no count
            weak_copy(c, f + 0x5Cu, px, pi, true);   // held to the end of the object
            if (expired(c, pi))
                return hand_back(c, 0x800C5E10u, 1u);
            c.mem_write32(f + 0x4Cu, px);  // 0x80028DB0 lock, released at once
            c.mem_write32(f + 0x50u, pi);
            g[20] = px;
            c.mem_write32(f + 0x08u, node);  // 0x800C4A40: the iterator steps on
            c.mem_write32(f + 0x18u, node != 0u ? c.mem_read32(node + 0x10u) : 0u);
            if (c.mem_read8(g[20] + 0x2Au) != 0u) {
                const u32 target = c.mem_read32(c.mem_read32(g[20]) + 0x60u);
                g[3] = g[20];
                g[4] = f + 0x6Cu;
                u32 link;
                if ((g[23] & 0xFFu) != 0u) {
                    result_init(c, f + 0x84u);
                    g[5] = f + 0x84u;
                    g[6] = g[24];
                    g[7] = g[25];
                    link = 0x800C5ECCu;
                } else {
                    g[5] = 0u;
                    g[6] = 0u;
                    g[7] = 0u;
                    link = 0x800C5F88u;
                }
                if (call_test(c, target, link) == 0)
                    return hand_back(c, c.pc, 3u);
                if (g[3] != 0u)
                    return hand_back(c, link, 0u);
            }
            // 0x8001FC28: the held copy released (the guest destroys at zero).
            if (pi != 0u) {
                const u32 weak = c.mem_read32(pi + 8u);
                if (weak <= 1u)
                    return hand_back(c, 0x800C5FB8u, 2u);
                c.mem_write32(pi + 8u, weak - 1u);
            }
        }
    }
    // 0x800C5FF0 with no hit: the hit's empty shared_ptr released, r3 = r28 = 0, lmw r20, mtlr, r1 back.
    g[3] = g[28];
    for (u32 i = 0; i < 12u; ++i)
        g[20u + i] = saved[i];
    g[1] = caller_sp;
    c.lr = link_in;
    ++queries_finished_;
    return true;
}

// 0x800C51C8 (manager r3, sphere r4, motion r5, mask r6, with-result r7, list r8, filter r9, filter arg r10; stack
// words +8 and +0xC: r26 and the exclusion list r22): frame 0x110, r14-r31.
bool CharaColliIntrinsic::run_sweep(Cpu& c) {
    u32* const g = c.gpr;
    const u32 caller_sp = g[1];
    const u32 f = caller_sp - 0x110u;
    const u32 link_in = c.lr;
    u32 saved[18];
    for (u32 i = 0; i < 18u; ++i)
        saved[i] = g[14u + i];
    c.mem_write32(f, caller_sp);
    c.mem_write32(caller_sp + 4u, link_in);
    for (u32 i = 0; i < 18u; ++i)
        c.mem_write32(f + 0xC8u + 4u * i, saved[i]);
    g[1] = f;
    g[31] = g[8];
    g[26] = c.mem_read32(f + 0x118u);
    g[29] = g[3];
    g[22] = c.mem_read32(f + 0x11Cu);
    g[23] = g[4];
    g[25] = g[5];
    g[14] = g[6];
    g[21] = g[7];
    g[20] = g[9];
    g[15] = g[10];
    g[28] = 0u;
    if (g[31] != 0u && (g[21] & 0xFFu) != 0u) {
        const u32 owner = c.mem_read32(c.mem_read32(g[31] + 8u) + 0x14u);  // 0x8001EE04, +0x14
        if (owner != 0u && c.mem_read32(owner + 0x40u) == 1u)              // 0x8009944C
            g[28] = 1u;
    }
    g[24] = 0u;
    g[27] = 0u;
    for (; static_cast<s32>(g[24]) < static_cast<s32>(c.mem_read32(g[29] + 4u)); ++g[24], g[27] += 0x10u) {
        if (c.mem_read32(c.mem_read32(g[29]) + g[27] + 4u) == 0u)
            continue;
        if (!group_takes(c, g[29] + 8u, g[14], g[24]))
            continue;
        c.mem_write32(f + 0x24u, 0u);
        c.mem_write32(f + 0x20u, 0u);
        const u32 first = c.mem_read32(c.mem_read32(g[29]) + g[27] + 8u);
        c.mem_write32(f + 0x18u, first);
        c.mem_write32(f + 0x24u, first);
        while (c.mem_read32(f + 0x24u) != c.mem_read32(c.mem_read32(g[29]) + g[27] + 0x0Cu)) {
            const u32 node = c.mem_read32(f + 0x24u);
            const u32 px = c.mem_read32(node);
            const u32 pi = c.mem_read32(node + 4u);
            if (!counts_safe(c, pi))
                return hand_back(c, 0x800C529Cu, 2u);
            weak_copy(c, f + 0x70u, px, pi, false);
            weak_copy(c, f + 0x80u, px, pi, true);
            if (expired(c, pi))
                return hand_back(c, 0x800C52D0u, 1u);
            c.mem_write32(f + 0x68u, px);
            c.mem_write32(f + 0x6Cu, pi);
            g[30] = px;
            c.mem_write32(f + 0x10u, node);
            c.mem_write32(f + 0x24u, node != 0u ? c.mem_read32(node + 0x10u) : 0u);
            bool skip = c.mem_read8(g[30] + 0x2Au) == 0u;
            if (!skip && g[22] != 0u) {
                // The exclusion list: an entry whose weak_ptr locks to this object skips it.
                c.mem_write32(f + 0x1Cu, 0u);
                g[16] = 0u;
                const u32 begin = c.mem_read32(g[22] + 8u);
                c.mem_write32(f + 0x0Cu, begin);
                c.mem_write32(f + 0x1Cu, begin);
                while (c.mem_read32(f + 0x1Cu) != c.mem_read32(g[22] + 0x0Cu)) {
                    const u32 entry = c.mem_read32(f + 0x1Cu);
                    const u32 epx = c.mem_read32(entry);
                    const u32 epi = c.mem_read32(entry + 4u);
                    if (!counts_safe(c, epi)) {
                        g[4] = entry;
                        return hand_back(c, 0x800C538Cu, 2u);
                    }
                    weak_copy(c, f + 0x60u, epx, epi, false);
                    g[17] = expired(c, epi) ? 1u : 0u;
                    if (g[17] == 0u) {
                        weak_copy(c, f + 0x58u, epx, epi, false);
                        c.mem_write32(f + 0x78u, epx);  // 0x800C4468: locked, released
                        c.mem_write32(f + 0x7Cu, epi);
                        if (epx == g[30]) {
                            g[16] = 1u;
                            break;
                        }
                    }
                    c.mem_write32(f + 0x08u, entry);  // 0x80027374
                    c.mem_write32(f + 0x1Cu, entry != 0u ? c.mem_read32(entry + 0x1Cu) : 0u);
                }
                skip = (g[16] & 0xFFu) != 0u;
            }
            if (!skip) {
                g[3] = g[30];
                g[4] = g[23];
                const bool first_kind = (g[28] & 0xFFu) != 0u;  // vf 0x68, else vf 0x6C
                const u32 target = c.mem_read32(c.mem_read32(g[30]) + (first_kind ? 0x68u : 0x6Cu));
                u32 link;
                if ((g[21] & 0xFFu) != 0u) {
                    result_init(c, f + 0x88u);
                    if (first_kind) {
                        g[5] = f + 0x88u;
                        g[6] = g[20];
                        g[7] = g[15];
                        link = 0x800C547Cu;
                    } else {
                        g[5] = g[25];
                        g[6] = f + 0x88u;
                        g[7] = g[20];
                        g[8] = g[15];
                        link = 0x800C54A8u;
                    }
                } else if (first_kind) {
                    g[5] = 0u;
                    g[6] = 0u;
                    g[7] = 0u;
                    link = 0x800C5630u;
                } else {
                    g[5] = g[25];
                    g[6] = 0u;
                    g[7] = 0u;
                    g[8] = 0u;
                    link = 0x800C565Cu;
                }
                if (call_test(c, target, link) == 0)
                    return hand_back(c, c.pc, 3u);
                if (g[3] != 0u)
                    return hand_back(c, link, 0u);
            }
            if (pi != 0u) {
                const u32 weak = c.mem_read32(pi + 8u);
                if (weak <= 1u)
                    return hand_back(c, 0x800C5788u, 2u);
                c.mem_write32(pi + 8u, weak - 1u);
            }
        }
    }
    // 0x800C57C0: r3 = r31, lmw r14, mtlr, r1 back.
    g[3] = g[31];
    for (u32 i = 0; i < 18u; ++i)
        g[14u + i] = saved[i];
    g[1] = caller_sp;
    c.lr = link_in;
    ++queries_finished_;
    return true;
}

// 0x800C615C, the enemies' ray query (manager r3, segment r4 (used in place), mask r5, with-result r6, result list r7,
// filter r8, filter arg r9, r10): frame 0xE0, r17-r31. The ray loop without its segment copy and its stop at a hit on
// the segment's start; a hit builds the result objects in the guest. Returns the list argument.
bool CharaColliIntrinsic::run_enemy(Cpu& c) {
    u32* const g = c.gpr;
    const u32 caller_sp = g[1];
    const u32 f = caller_sp - 0xE0u;
    const u32 link_in = c.lr;
    u32 saved[15];
    for (u32 i = 0; i < 15u; ++i)
        saved[i] = g[17u + i];
    // stwu r1,-0xE0; mflr; stw r0,0xE4; stmw r17,0xA4.
    c.mem_write32(f, caller_sp);
    c.mem_write32(caller_sp + 4u, link_in);
    for (u32 i = 0; i < 15u; ++i)
        c.mem_write32(f + 0xA4u + 4u * i, saved[i]);
    g[1] = f;
    g[29] = g[3];
    g[26] = g[4];
    g[25] = g[5];
    g[23] = g[6];
    g[24] = g[7];
    g[22] = g[8];
    g[21] = g[9];
    g[30] = g[10];
    g[31] = 0u;
    g[28] = 0u;
    for (; static_cast<s32>(g[31]) < static_cast<s32>(c.mem_read32(g[29] + 4u)); ++g[31], g[28] += 0x10u) {
        if (c.mem_read32(c.mem_read32(g[29]) + g[28] + 4u) == 0u)
            continue;
        if (!group_takes(c, g[29] + 8u, g[25], g[31]))
            continue;
        c.mem_write32(f + 0x18u, 0u);
        c.mem_write32(f + 0x14u, 0u);
        const u32 first = c.mem_read32(c.mem_read32(g[29]) + g[28] + 8u);
        c.mem_write32(f + 0x10u, first);
        c.mem_write32(f + 0x18u, first);
        while (c.mem_read32(f + 0x18u) != c.mem_read32(c.mem_read32(g[29]) + g[28] + 0x0Cu)) {
            const u32 node = c.mem_read32(f + 0x18u);
            const u32 px = c.mem_read32(node);
            const u32 pi = c.mem_read32(node + 4u);
            if (!counts_safe(c, pi))
                return hand_back(c, 0x800C61F4u, 2u);
            weak_copy(c, f + 0x54u, px, pi, false);  // copied and released: no count
            weak_copy(c, f + 0x5Cu, px, pi, true);   // held to the end of the object
            if (expired(c, pi))
                return hand_back(c, 0x800C6228u, 1u);
            c.mem_write32(f + 0x4Cu, px);  // 0x80028DB0 lock, released at once
            c.mem_write32(f + 0x50u, pi);
            g[27] = px;
            c.mem_write32(f + 0x08u, node);  // 0x800C4A40: the iterator steps on
            c.mem_write32(f + 0x18u, node != 0u ? c.mem_read32(node + 0x10u) : 0u);
            if (c.mem_read8(g[27] + 0x2Au) != 0u) {
                const u32 target = c.mem_read32(c.mem_read32(g[27]) + 0x60u);
                g[3] = g[27];
                g[4] = g[26];
                u32 link;
                if ((g[23] & 0xFFu) != 0u) {
                    result_init(c, f + 0x64u);
                    g[5] = f + 0x64u;
                    g[6] = g[22];
                    g[7] = g[21];
                    link = 0x800C62E4u;
                } else {
                    g[5] = 0u;
                    g[6] = 0u;
                    g[7] = 0u;
                    link = 0x800C641Cu;
                }
                if (call_test(c, target, link) == 0)
                    return hand_back(c, c.pc, 3u);
                if (g[3] != 0u)
                    return hand_back(c, link, 0u);
            }
            if (pi != 0u) {  // 0x8001FC28: the held copy released (the guest destroys at zero)
                const u32 weak = c.mem_read32(pi + 8u);
                if (weak <= 1u)
                    return hand_back(c, 0x800C650Cu, 2u);
                c.mem_write32(pi + 8u, weak - 1u);
            }
        }
    }
    // 0x800C6544: r3 = r24, lmw r17, mtlr, r1 back.
    g[3] = g[24];
    for (u32 i = 0; i < 15u; ++i)
        g[17u + i] = saved[i];
    g[1] = caller_sp;
    c.lr = link_in;
    ++queries_finished_;
    return true;
}

bool CharaColliIntrinsic::verify_query(Cpu& c, Query query) {
    const u32 entry = query == Query::Ray ? kRayQuery : query == Query::Sweep ? kSweepQuery : kEnemyQuery;
    const RecompiledCode* code = c.recompiled();
    const RecompiledCode::Range* body = code != nullptr ? code->find_range(entry) : nullptr;
    ++queries_;
    if (body == nullptr)
        return run_query(c, query);
    GuestMemory& memory = c.guest_memory();
    static Snapshot before, native, guest;
    const Window none[kWindows] = {};
    take(c, none, before);
    take_all(memory, full_before_);
    const u32 pc = c.pc;
    const u64 finished = queries_finished_;
    const bool served = run_query(c, query);
    if (!served || queries_finished_ == finished)
        return served;  // declined, or handed back to the guest body mid-way: nothing to compare
    take(c, none, native);
    take_all(memory, full_native_);
    put(c, none, before);
    put_all(memory, full_before_);
    const u32 link = c.lr;
    reference_ = true;
    body->at(c, entry);
    c.finish_transfers();
    reference_ = false;
    if (c.pc != link) {
        put_all(memory, full_native_);
        put(c, none, native);
        c.pc = pc;
        ++incomplete_;
        return true;
    }
    c.pc = pc;
    take(c, none, guest);
    take_all(memory, full_guest_);
    ++queries_verified_;
    const u32 elsewhere = full_guest_.size() == full_native_.size()
                              ? first_difference(memory, full_native_, full_guest_, before.gpr[1])
                              : 1u;
    if (!same_contract(native, guest) || elsewhere != 0u) {
        if (++queries_differed_ <= 8u) {
            std::fprintf(stderr, "[chara-colli] native query %u differs from the guest body (r3 native %08X guest %08X)\n",
                         static_cast<unsigned>(query), native.gpr[3], guest.gpr[3]);
            for (u32 i = 1; i < 32u; ++i)
                if ((i <= 3u || i >= 13u) && native.gpr[i] != guest.gpr[i])
                    std::fprintf(stderr, "  r%u native %08X guest %08X\n", i, native.gpr[i], guest.gpr[i]);
            if (!same_fpscr(native.fpscr, guest.fpscr))
                std::fprintf(stderr, "  fpscr native %08X guest %08X\n", native.fpscr, guest.fpscr);
            if (native.lr != guest.lr)
                std::fprintf(stderr, "  lr native %08X guest %08X\n", native.lr, guest.lr);
            if (elsewhere != 0u) {
                std::fprintf(stderr, "  memory differs at %08X (r1 %08X):", elsewhere, before.gpr[1]);
                std::size_t at = 0;
                u32 shown = 0;
                for (const GuestMemory::Region& region : memory.regions()) {
                    for (u32 i = 0; i < region.committed && shown < 24u; ++i)
                        if (full_native_[at + i] != full_guest_[at + i] && region.base + i >= before.gpr[1]) {
                            std::fprintf(stderr, " %08X n%02X g%02X", region.base + i, full_native_[at + i],
                                         full_guest_[at + i]);
                            ++shown;
                        }
                    at += region.committed;
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    return true;
}

// ---- the manager pass's per-object update (vf 0x7C) ----

namespace {

constexpr u32 kBodyDirty = 0x800C1714u;  // Body vf 0x8C: its shape re-derived when dirty
constexpr u32 kManageEntry[3] = {0x800C3DD4u, 0x800C1788u, 0x800C3594u};

// The bytes a vf 0x7C call can write: the object and its shape or children (the Range shapes reach +0xD3).
void manage_windows(Cpu& c, u32 self, CharaColliIntrinsic::Manage kind, std::vector<std::pair<u32, u32>>& out) {
    out.clear();
    out.push_back({self, 0x70u});
    if (kind == CharaColliIntrinsic::Manage::Body) {
        out.push_back({c.mem_read32(self + 0x5Cu), 0xD4u});
        return;
    }
    const u32 sentinel = self + 0x64u;
    u32 node = c.mem_read32(sentinel + 4u);
    for (u32 n = 0; node != sentinel && n < 32u; ++n, node = c.mem_read32(node + 4u))
        out.push_back({c.mem_read32(node + 8u), 0xD4u});
}

void take_windows(const GuestMemory& memory, const std::vector<std::pair<u32, u32>>& windows, std::vector<u8>& out) {
    out.clear();
    for (const auto& [base, size] : windows) {
        const u8* p = memory.peek(GcAddr(base), size);
        if (p != nullptr)
            out.insert(out.end(), p, p + size);
        else
            out.insert(out.end(), size, u8{0});
    }
}
void put_windows(GuestMemory& memory, const std::vector<std::pair<u32, u32>>& windows, const std::vector<u8>& in) {
    std::size_t at = 0;
    for (const auto& [base, size] : windows) {
        if (u8* p = const_cast<u8*>(memory.peek(GcAddr(base), size)))
            std::memcpy(p, in.data() + at, size);
        at += size;
    }
}

}  // namespace

bool CharaColliIntrinsic::serve_manage(Cpu& c, Manage kind) {
    if (reference_)
        return false;
    ++manage_calls_;
    return verify_ ? verify_manage(c, kind) : run_manage(c, kind);
}

// vf 0x7C, called once per object per frame by the manager's pass (fn_800C7068):
//   GeneralBone 0x800C3DD4: vf 0x88 (the bone update, as for a bone test)
//   Body 0x800C1788 / General 0x800C3594: vf 0x88 (0x800C1A74 / 0x800C409C, or the plain classes' blr), then vf
//     0x8C - when dirty, Body's shape (0x800C1714) / General's children (0x800C34D8) re-derived from the owner.
// What a served call leaves: the object's and its shapes' writes, FPSCR, the FPRs the guest's callees round, the LR
// save word at r1 + 4; volatile registers are the ABI's (the pass reaches it only through the vtable).
bool CharaColliIntrinsic::run_manage(Cpu& c, Manage kind) {
    const u32 gqr = c.spr[SPR_GQR0];
    if ((c.msr() & kMsrFp) == 0u || (gqr & 7u) >= 4u || ((gqr >> 16) & 7u) >= 4u) {
        ++manage_declined_;
        return false;
    }
    const u32 self = c.gpr[3];
    const u32 vtable = c.mem_read32(self);
    const u32 update_slot = c.mem_read32(vtable + 0x88u);
    bool updates;
    if (kind == Manage::Bone) {
        updates = update_slot == kBoneUpdate && c.mem_read32(vtable + 0x14u) == kGeneralCentre;
        if (!updates) {
            ++manage_declined_;
            return false;
        }
    } else {
        const bool body = kind == Manage::Body;
        updates = update_slot == (body ? kBodyUpdate : kGeneralUpdate);
        if ((!updates && update_slot != (body ? kBodyNoUpdate : kGeneralNoUpdate)) ||
            c.mem_read32(vtable + 0x8Cu) != (body ? kBodyDirty : kBoneDirty) ||
            (body && c.mem_read32(vtable + 0x28u) != kBodyShape)) {
            ++manage_declined_;
            return false;
        }
    }
    if (updates && !counts_safe(c, c.mem_read32(self + 0x34u))) {
        ++manage_declined_;
        return false;
    }
    const u32 cr = c.cr;
    const u32 fpscr = c.fpscr;
    Written written;
    save(c, self, written);
    const auto decline = [&]() {
        restore(c, self, written);
        c.cr = cr;
        c.fpscr = fpscr;
        ++manage_declined_;
        return false;
    };
    if (updates)
        update(c, self);
    if (kind == Manage::Bone) {
        const int done = bone_update(c, Test::Manage, cr, fpscr);
        if (done < 0)
            return decline();
        if (done == 0)
            return true;  // handed to the guest inside the update; it returns into 0x800C3DD4
    } else if (c.mem_read8(self + 0x28u) != 0u) {
        const bool ok = kind == Manage::Body ? chara_colli_bone::rederive_shape(c, self)
                                             : chara_colli_bone::rederive_children(c, self);
        if (!ok)
            return decline();
        ++manage_rederived_;
    }
    c.mem_write32(c.gpr[1] + 4u, c.lr);  // the prologue's mflr / stw r0 into the caller's LR save word
    ++manage_served_[static_cast<u32>(kind)];
    return true;
}

// --verify-chara-colli: every served call also run by the guest's vf 0x7C from the same state, compared on the
// contract registers, FPSCR and the object's and its shapes' bytes; every 256th also on all committed memory.
bool CharaColliIntrinsic::verify_manage(Cpu& c, Manage kind) {
    const u32 entry = kManageEntry[static_cast<u32>(kind)];
    const RecompiledCode* code = c.recompiled();
    const RecompiledCode::Range* body = code != nullptr ? code->find_range(entry) : nullptr;
    if (body == nullptr)
        return run_manage(c, kind);
    GuestMemory& memory = c.guest_memory();
    static Snapshot before, native, guest;
    const Window none[kWindows] = {};
    manage_windows(c, c.gpr[3], kind, manage_windows_);
    take(c, none, before);
    take_windows(memory, manage_windows_, manage_before_);
    const bool full = (manage_verified_ & 255u) == 0u;
    if (full)
        take_all(memory, full_before_);
    const u32 pc = c.pc;
    if (!run_manage(c, kind)) {
        take(c, none, native);
        take_windows(memory, manage_windows_, manage_native_);
        if ((std::memcmp(&before, &native, sizeof(before)) != 0 || manage_before_ != manage_native_) &&
            ++manage_differed_ <= 8u)
            std::fprintf(stderr, "[chara-colli] a declined vf 0x7C changed state: %08X\n", before.gpr[3]);
        return false;
    }
    if (c.exiting()) {
        ++incomplete_;
        return true;
    }
    take(c, none, native);
    take_windows(memory, manage_windows_, manage_native_);
    if (full)
        take_all(memory, full_native_);
    put(c, none, before);
    put_windows(memory, manage_windows_, manage_before_);
    if (full)
        put_all(memory, full_before_);
    const u32 link = c.lr;
    reference_ = true;
    body->at(c, entry);
    c.finish_transfers();
    reference_ = false;
    if (c.pc != link) {
        put_windows(memory, manage_windows_, manage_native_);
        if (full)
            put_all(memory, full_native_);
        put(c, none, native);
        c.pc = pc;
        ++incomplete_;
        return true;
    }
    c.pc = pc;
    take(c, none, guest);
    take_windows(memory, manage_windows_, manage_guest_);
    ++manage_verified_;
    u32 elsewhere = 0u;
    if (full) {
        take_all(memory, full_guest_);
        elsewhere = full_guest_.size() == full_native_.size()
                        ? first_difference(memory, full_native_, full_guest_, before.gpr[1])
                        : 1u;
    }
    native.gpr[3] = guest.gpr[3];  // vf 0x7C returns nothing: r3 is volatile here
    if (!same_contract(native, guest) || manage_native_ != manage_guest_ || elsewhere != 0u) {
        if (++manage_differed_ <= 8u) {
            std::fprintf(stderr, "[chara-colli] native vf 0x7C (%u) differs from the guest's: %08X\n",
                         static_cast<unsigned>(kind), before.gpr[3]);
            std::size_t at = 0;
            for (const auto& [base, size] : manage_windows_) {
                for (u32 i = 0; i < size; ++i)
                    if (manage_native_[at + i] != manage_guest_[at + i]) {
                        std::fprintf(stderr, "  %08X native %02X guest %02X\n", base + i, manage_native_[at + i],
                                     manage_guest_[at + i]);
                        break;
                    }
                at += size;
            }
            if (!same_fpscr(native.fpscr, guest.fpscr))
                std::fprintf(stderr, "  fpscr native %08X guest %08X\n", native.fpscr, guest.fpscr);
            for (u32 i = 14; i < 32u; ++i)
                if (native.fpr[i][0] != guest.fpr[i][0] || native.fpr[i][1] != guest.fpr[i][1])
                    std::fprintf(stderr, "  f%u differs\n", i);
            if (elsewhere != 0u) {
                std::fprintf(stderr, "  memory differs at %08X (r1 %08X):", elsewhere, before.gpr[1]);
                std::size_t at = 0;
                u32 shown = 0;
                for (const GuestMemory::Region& region : memory.regions()) {
                    for (u32 i = 0; i < region.committed && shown < 24u; ++i)
                        if (full_native_[at + i] != full_guest_[at + i] && region.base + i >= before.gpr[1]) {
                            std::fprintf(stderr, " %08X n%02X g%02X", region.base + i, full_native_[at + i],
                                         full_guest_[at + i]);
                            ++shown;
                        }
                    at += region.committed;
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    return true;
}

// ---- the manager's pair pass (fn_800C7068) ----

namespace {

constexpr u32 kRadius = 0x80037380u;      // vf 0x38 (every class): lfs f1, 8(r3)
constexpr u32 kPassMargin = 0xFFFF99A0u;  // r2 - 0x6660: added to the two radii

GEKKO_ALWAYS_INLINE bool centre_getter(u32 fn) noexcept { return fn == kBodyCentre || fn == kGeneralCentre; }

// A bl to a guest function with the guest's own return address; 1 when it returned there.
int call_bl(Cpu& c, u32 target, u32 link) {
    recomp::call_indirect(c, target, link);
    if (c.exiting())
        c.finish_transfers();
    return !c.exiting() && c.pc == link ? 1 : 0;
}

// 0x8001FC28 / 0x80028D60 on a copy whose increment was real: false when the count would reach zero (the guest
// destroys there, so the caller hands back at the release).
GEKKO_ALWAYS_INLINE bool drop(Cpu& c, u32 pi, u32 field) {
    if (pi == 0u)
        return true;
    const u32 n = c.mem_read32(pi + field);
    if (n <= 1u)
        return false;
    c.mem_write32(pi + field, n - 1u);
    return true;
}
GEKKO_ALWAYS_INLINE bool drop_weak(Cpu& c, u32 pi) { return drop(c, pi, 8u); }
GEKKO_ALWAYS_INLINE bool drop_use(Cpu& c, u32 pi) { return drop(c, pi, 4u); }
GEKKO_ALWAYS_INLINE void hold_use(Cpu& c, u32 pi) {
    if (pi != 0u)
        c.mem_write32(pi + 4u, c.mem_read32(pi + 4u) + 1u);
}

}  // namespace

bool CharaColliIntrinsic::serve_pass(Cpu& c) {
    if (reference_ || (c.msr() & kMsrFp) == 0u)  // MSR[FP] clear: the guest's stfd raises FP-unavailable
        return false;
    ++pass_calls_;
    return verify_ ? verify_pass(c) : run_pass(c);
}

// The pass's bctrl to vf 0x7C: the native update when the slot names one, else the guest's.
int CharaColliIntrinsic::call_manage(Cpu& c, u32 self, u32 link) {
    const u32 target = c.mem_read32(c.mem_read32(self) + 0x7Cu);
    c.gpr[3] = self;
    c.gpr[12] = target;
    c.ctr = target;
    c.lr = link;
    for (u32 k = 0; k < 3u; ++k) {
        if (!manage_ || target != kManageEntry[k])
            continue;
        ++manage_calls_;
        if (run_manage(c, static_cast<Manage>(k)))
            return c.exiting() ? 0 : 1;  // a GeneralBone update may hand itself to the guest
        const RecompiledCode* code = c.recompiled();
        if (const RecompiledCode::Range* body = code != nullptr ? code->find_range(target) : nullptr) {
            body->at(c, target);  // declined: its translated body (the entry thunk would come back to the hook)
            if (c.exiting())
                c.finish_transfers();
            return !c.exiting() && c.pc == link ? 1 : 0;
        }
        break;
    }
    return call_bl(c, target, link);
}

// fn_800C7068 (manager r3): frame 0x100, f31 (stfd + psq_st), r17-r31. For every active object A (in group order)
// and every later object B whose group A's group takes: vf 0x7C on A (once) and on B; A's contact list searched
// for B (a contact still standing and neither moved: event 0x800C6A4C); a pair where one went inactive, or that
// moved apart beyond the two radii plus the margin, loses its contact (0x80026AAC, 0x800C77D8); a pair within
// reach runs the narrow phase (A's vf 0x80) and, on contact, builds the two contact records and their events.
// Native here: the loops, the weak_ptr copies and locks (an increment and its decrement with no guest code
// between them change nothing and are not made), vf 0x7C (run_manage), the radius and centre getters, the
// distance test (0x8040E1D0) and its fcmpo. Guest: contact erase / events / records and the narrow phase, called
// with the guest's own return addresses, every register and frame word as the guest's would be. Handed back to
// the guest body - at the instruction it stands at - when an entry is expired or a contact null (the guest erases
// it), a release would reach zero, a count could wrap, a nested call is cut short, or an object's getters are
// not the known ones.
bool CharaColliIntrinsic::run_pass(Cpu& c) {
    u32* const g = c.gpr;
    const u32 caller_sp = g[1];
    const u32 f = caller_sp - 0x100u;
    const u32 link_in = c.lr;
    // stwu r1,-0x100; mflr; stw r0,0x104; stfd f31,0xF0; psq_st f31,0xF8; stmw r17,0xB4.
    c.mem_write32(f, caller_sp);
    g[1] = f;
    c.mem_write32(caller_sp + 4u, link_in);
    c.mem_write64(f + 0xF0u, c.fpr[31].bits[0]);
    ppc::psq_store(c, 31u, f + 0xF8u, false, 0u);
    for (u32 i = 0; i < 15u; ++i)
        c.mem_write32(f + 0xB4u + 4u * i, g[17u + i]);
    g[31] = g[3];
    for (const u32 slot : {0x50u, 0x4Cu, 0x48u, 0x44u, 0x40u, 0x3Cu})
        c.mem_write32(f + slot, 0u);
    const u32 m = g[31];
    const auto back = [&](u32 pc, u32 reason) {
        ++pass_back_[reason];
        c.leave_to_dispatcher(pc);
        return true;
    };
    // 0x80026AAC (A's contact erased) and 0x800C77D8 (B's side): a pair losing its contact.
    const auto separate = [&](u32 erase_link, u32 event_link) {
        g[3] = g[28];
        g[4] = f + 0x40u;
        if (call_bl(c, 0x80026AACu, erase_link) == 0)
            return false;
        g[3] = g[31];
        g[4] = g[20];
        g[5] = g[29];
        return call_bl(c, 0x800C77D8u, event_link) != 0;
    };
    g[26] = 0u;
    g[30] = 0u;
    for (; static_cast<s32>(g[26]) < static_cast<s32>(c.mem_read32(m + 4u)); ++g[26], g[30] += 0x10u) {
        const u32 group = c.mem_read32(m) + g[30];
        if (c.mem_read32(group + 4u) == 0u)
            continue;
        const u32 first = c.mem_read32(group + 8u);  // 0x800C4ABC / 0x800C4AB4
        c.mem_write32(f + 0x28u, first);
        c.mem_write32(f + 0x50u, first);
        g[27] = g[26];
        while (c.mem_read32(f + 0x50u) != c.mem_read32(c.mem_read32(m) + g[30] + 0x0Cu)) {
            const u32 node = c.mem_read32(f + 0x50u);
            const u32 px = c.mem_read32(node);
            const u32 pi = c.mem_read32(node + 4u);
            if (!counts_safe(c, pi))
                return back(0x800C70E0u, 1u);
            weak_copy(c, f + 0x8Cu, px, pi, false);  // copied and released: no count
            weak_copy(c, f + 0xA4u, px, pi, true);   // held to the end of A
            g[24] = 0u;
            if (expired(c, pi))
                return back(0x800C7118u, 0u);
            c.mem_write32(f + 0x84u, px);  // 0x80028DB0 lock, released at once
            c.mem_write32(f + 0x88u, pi);
            g[29] = px;
            c.mem_write32(f + 0x20u, node);  // 0x800C4A40: the iterator steps on
            c.mem_write32(f + 0x50u, node != 0u ? c.mem_read32(node + 0x10u) : 0u);
            const u32 a = g[29];
            if (c.mem_read8(a + 0x2Au) == 0u && c.mem_read8(a + 0x2Bu) == 0u) {
                if (!drop_weak(c, pi))
                    return back(0x800C719Cu, 1u);
                continue;
            }
            g[28] = a + 0x0Cu;  // A's contact list
            c.mem_write32(f + 0x1Cu, c.mem_read32(g[28] + 8u));
            g[25] = g[26];
            c.mem_write32(f + 0x40u, c.mem_read32(f + 0x1Cu));
            if (c.mem_read32(f + 0x50u) == c.mem_read32(c.mem_read32(m) + g[30] + 0x0Cu))
                g[25] = g[26] + 1u;  // A was its group's last: B from the next group
            g[22] = g[25] << 4;
            for (; static_cast<s32>(g[25]) < static_cast<s32>(c.mem_read32(m + 4u)); ++g[25], g[22] += 0x10u) {
                if (c.mem_read32(c.mem_read32(m) + g[22] + 4u) == 0u)
                    continue;
                if (!group_takes(c, m + 8u, g[25], g[27]))
                    continue;
                if (g[25] == g[26]) {
                    c.mem_write32(f + 0x4Cu, c.mem_read32(f + 0x50u));  // B after A in A's group
                } else {
                    const u32 b_first = c.mem_read32(c.mem_read32(m) + g[22] + 8u);
                    c.mem_write32(f + 0x18u, b_first);
                    c.mem_write32(f + 0x4Cu, b_first);
                }
                while (c.mem_read32(f + 0x4Cu) != c.mem_read32(c.mem_read32(m) + g[22] + 0x0Cu)) {
                    const u32 b_node = c.mem_read32(f + 0x4Cu);
                    const u32 bpx = c.mem_read32(b_node);
                    const u32 bpi = c.mem_read32(b_node + 4u);
                    if (!counts_safe(c, bpi))
                        return back(0x800C7258u, 1u);
                    weak_copy(c, f + 0x7Cu, bpx, bpi, false);
                    weak_copy(c, f + 0x9Cu, bpx, bpi, true);  // held to the end of B
                    if (expired(c, bpi))
                        return back(0x800C728Cu, 0u);
                    c.mem_write32(f + 0x74u, bpx);
                    c.mem_write32(f + 0x78u, bpi);
                    g[21] = bpx;
                    c.mem_write32(f + 0x10u, b_node);
                    c.mem_write32(f + 0x4Cu, b_node != 0u ? c.mem_read32(b_node + 0x10u) : 0u);
                    const u32 b = g[21];
                    if (c.mem_read8(b + 0x2Au) == 0u && c.mem_read8(b + 0x2Bu) == 0u) {
                        if (!drop_weak(c, bpi))
                            return back(0x800C7310u, 1u);
                        continue;
                    }
                    if ((g[24] & 0xFFu) == 0u) {
                        if (call_manage(c, a, 0x800C733Cu) == 0)
                            return back(c.pc, 2u);
                        g[24] = 1u;
                    }
                    if (call_manage(c, b, 0x800C7354u) == 0)
                        return back(c.pc, 2u);
                    // A's contact list searched for B.
                    g[23] = 0u;
                    const u32 contacts = c.mem_read32(g[28] + 8u);
                    c.mem_write32(f + 0x0Cu, contacts);
                    c.mem_write32(f + 0x40u, contacts);
                    while (c.mem_read32(f + 0x40u) != c.mem_read32(g[28] + 0x0Cu)) {
                        const u32 entry = c.mem_read32(f + 0x40u);
                        const u32 epx = c.mem_read32(entry);
                        const u32 epi = c.mem_read32(entry + 4u);
                        if (!counts_safe(c, epi)) {
                            g[0] = entry;
                            return back(0x800C7378u, 1u);
                        }
                        c.mem_write32(f + 0x3Cu, entry != 0u ? c.mem_read32(entry + 0x1Cu) : 0u);  // 0x80027374
                        c.mem_write32(f + 0x08u, entry);
                        g[17] = entry;
                        weak_copy(c, f + 0x6Cu, epx, epi, false);  // 0x800588C8, released after the copy
                        const bool alive = !expired(c, epi);       // 0x80028DB0: a null lock when expired
                        const u32 lpx = alive ? epx : 0u;
                        const u32 lpi = alive ? epi : 0u;
                        c.mem_write32(f + 0x64u, lpx);
                        c.mem_write32(f + 0x68u, lpi);
                        c.mem_write32(f + 0x94u, lpx);  // 0x80028D20: the copy held over the event
                        c.mem_write32(f + 0x98u, lpi);
                        if (lpx == 0u) {
                            hold_use(c, lpi);
                            return back(0x800C73D8u, 0u);
                        }
                        if (lpx == b) {
                            g[23] = 1u;
                            if (c.mem_read8(a + 0x29u) == 0u && c.mem_read8(b + 0x29u) == 0u &&
                                c.mem_read8(a + 0x2Au) != 0u && c.mem_read8(b + 0x2Au) != 0u) {
                                hold_use(c, lpi);
                                g[3] = g[31];
                                g[4] = a;
                                g[5] = b;
                                g[6] = g[17];
                                g[7] = 0u;
                                g[8] = 0u;
                                if (call_bl(c, 0x800C6A4Cu, 0x800C744Cu) == 0)
                                    return back(c.pc, 2u);
                                if (!drop_use(c, lpi))
                                    return back(0x800C744Cu, 1u);
                            }
                            break;
                        }
                        c.mem_write32(f + 0x40u, c.mem_read32(f + 0x3Cu));
                    }
                    g[20] = b + 0x0Cu;  // B's contact list
                    const bool a_gone = c.mem_read8(a + 0x2Au) == 0u && c.mem_read8(a + 0x2Bu) != 0u;
                    const bool b_gone = c.mem_read8(b + 0x2Au) == 0u && c.mem_read8(b + 0x2Bu) != 0u;
                    if (a_gone || b_gone) {
                        if ((g[23] & 0xFFu) != 0u && !separate(0x800C74CCu, 0x800C74DCu))
                            return back(c.pc, 2u);
                        if (!drop_weak(c, c.mem_read32(f + 0xA0u)))
                            return back(0x800C74DCu, 1u);
                        continue;
                    }
                    if (c.mem_read8(a + 0x29u) == 0u && c.mem_read8(b + 0x29u) == 0u) {  // neither moved
                        if (!drop_weak(c, c.mem_read32(f + 0xA0u)))
                            return back(0x800C7504u, 1u);
                        continue;
                    }
                    const u32 avt = c.mem_read32(a);
                    const u32 bvt = c.mem_read32(b);
                    if (c.mem_read32(bvt + 0x38u) != kRadius || c.mem_read32(avt + 0x38u) != kRadius ||
                        !centre_getter(c.mem_read32(bvt + 0x14u)) || !centre_getter(c.mem_read32(avt + 0x14u)))
                        return back(0x800C7514u, 3u);
                    ++pass_pairs_;
                    // f31 = margin + (r(A) + r(B)); r17 = B's centre; 0x8040E1D0 (A's centre, B's) against f31^2.
                    const u64 reach = ppc::fps_add(c, lfs(c.mem_read32(g[2] + kPassMargin)),
                                                   ppc::fps_add(c, lfs(c.mem_read32(a + 8u)),
                                                                lfs(c.mem_read32(b + 8u))));
                    c.fpr[31].bits[0] = reach;
                    c.fpr[31].bits[1] = reach;
                    g[17] = b + 0x40u;
                    const u64 apart = length_squared(c, sub(c, read3(c, a + 0x40u), read3(c, g[17])));
                    ppc::fp_compare(c, 0u, apart, ppc::fps_mul(c, reach, reach), true);
                    if (!ppc::cr_bit(c, 0u) && !ppc::cr_bit(c, 2u)) {  // cror eq, lt, eq: beyond reach
                        if ((g[23] & 0xFFu) != 0u && !separate(0x800C75A4u, 0x800C75B4u))
                            return back(c.pc, 2u);
                        if (!drop_weak(c, c.mem_read32(f + 0xA0u)))
                            return back(0x800C75B4u, 1u);
                        continue;
                    }
                    // The narrow phase: A's vf 0x80 (A, B, four out words).
                    for (const u32 slot : {0x38u, 0x34u, 0x30u, 0x2Cu})
                        c.mem_write32(f + slot, 0u);
                    ++pass_narrow_;
                    g[3] = a;
                    g[4] = b;
                    g[5] = f + 0x38u;
                    g[6] = f + 0x34u;
                    g[7] = f + 0x30u;
                    g[8] = f + 0x2Cu;
                    if (call_guest(c, c.mem_read32(avt + 0x80u), 0x800C7600u) == 0)
                        return back(c.pc, 2u);
                    if ((g[3] & 0xFFu) == 0u) {
                        if ((g[23] & 0xFFu) != 0u && !separate(0x800C772Cu, 0x800C773Cu))
                            return back(c.pc, 2u);
                    } else {
                        // In contact: a record for each side (0x80026518 new 0x24, 0x800C57D8), A's replacing
                        // or joining A's list (0x80026B20 + 0x80026AAC / 0x800C78E0), B's joining B's, the event.
                        ++pass_contacts_;
                        g[17] = 0u;
                        g[3] = 0x24u;
                        if (call_bl(c, 0x80026518u, 0x800C7614u) == 0)
                            return back(c.pc, 2u);
                        g[18] = g[3];
                        if (g[18] != 0u) {
                            weak_copy(c, f + 0x5Cu, c.mem_read32(f + 0x9Cu), c.mem_read32(f + 0xA0u), true);
                            g[5] = c.mem_read32(f + 0x34u);
                            g[3] = g[18];
                            g[6] = c.mem_read32(f + 0x30u);
                            g[4] = f + 0x5Cu;
                            g[7] = c.mem_read32(f + 0x38u);
                            g[17] = 1u;
                            if (call_bl(c, 0x800C57D8u, 0x800C7644u) == 0)
                                return back(c.pc, 2u);
                            g[18] = g[3];
                        }
                        if ((g[17] & 0xFFu) != 0u && !drop_weak(c, c.mem_read32(f + 0x60u)))
                            return back(0x800C7650u, 1u);
                        g[19] = 0u;
                        g[3] = 0x24u;
                        if (call_bl(c, 0x80026518u, 0x800C7668u) == 0)
                            return back(c.pc, 2u);
                        g[17] = g[3];
                        if (g[17] != 0u) {
                            weak_copy(c, f + 0x54u, c.mem_read32(f + 0xA4u), c.mem_read32(f + 0xA8u), true);
                            g[5] = c.mem_read32(f + 0x38u);
                            g[3] = g[17];
                            g[6] = c.mem_read32(f + 0x2Cu);
                            g[4] = f + 0x54u;
                            g[7] = c.mem_read32(f + 0x34u);
                            g[19] = 1u;
                            if (call_bl(c, 0x800C57D8u, 0x800C7698u) == 0)
                                return back(c.pc, 2u);
                            g[17] = g[3];
                        }
                        if ((g[19] & 0xFFu) != 0u && !drop_weak(c, c.mem_read32(f + 0x58u)))
                            return back(0x800C76A4u, 1u);
                        if ((g[23] & 0xFFu) != 0u) {
                            g[3] = g[28];
                            g[5] = g[18];
                            g[4] = f + 0x40u;
                            if (call_bl(c, 0x80026B20u, 0x800C76C8u) == 0)
                                return back(c.pc, 2u);
                            g[3] = g[28];
                            g[4] = f + 0x40u;
                            if (call_bl(c, 0x80026AACu, 0x800C76D4u) == 0)
                                return back(c.pc, 2u);
                        } else {
                            g[3] = g[31];
                            g[4] = g[28];
                            g[5] = g[18];
                            if (call_bl(c, 0x800C78E0u, 0x800C76E8u) == 0)
                                return back(c.pc, 2u);
                        }
                        g[3] = g[31];
                        g[4] = g[20];
                        g[5] = g[17];
                        if (call_bl(c, 0x800C78E0u, 0x800C76F8u) == 0)
                            return back(c.pc, 2u);
                        g[3] = g[31];
                        g[4] = g[29];
                        g[5] = g[21];
                        g[6] = g[18];
                        g[7] = g[17];
                        g[8] = 0u;
                        if (call_bl(c, 0x800C6A4Cu, 0x800C7714u) == 0)
                            return back(c.pc, 2u);
                    }
                    if (!drop_weak(c, c.mem_read32(f + 0xA0u)))
                        return back(0x800C773Cu, 1u);
                }
            }
            // 0x800C7774: A's moved flag cleared, its activity remembered, its held copy released.
            if (!drop_weak(c, c.mem_read32(f + 0xA8u)))
                return back(0x800C7774u, 1u);
            c.mem_write8(a + 0x29u, 0u);
            c.mem_write8(a + 0x2Bu, c.mem_read8(a + 0x2Au));
        }
    }
    // 0x800C77BC: psq_l f31; lfd f31; lmw r17; mtlr; r1 back.
    ppc::psq_load(c, 31u, f + 0xF8u, false, 0u);
    c.fpr[31].bits[0] = c.mem_read64(f + 0xF0u);
    for (u32 i = 0; i < 15u; ++i)
        g[17u + i] = c.mem_read32(f + 0xB4u + 4u * i);
    g[0] = c.mem_read32(f + 0x104u);
    c.lr = g[0];
    g[1] = caller_sp;
    ++pass_finished_;
    return true;
}

// --verify-chara-colli: every pass served to its end also run by the guest body from the same state, compared on
// the contract registers, FPSCR and all committed memory at or above r1.
bool CharaColliIntrinsic::verify_pass(Cpu& c) {
    const RecompiledCode* code = c.recompiled();
    const RecompiledCode::Range* body = code != nullptr ? code->find_range(kPass) : nullptr;
    if (body == nullptr)
        return run_pass(c);
    GuestMemory& memory = c.guest_memory();
    static Snapshot before, native, guest;
    const Window none[kWindows] = {};
    take(c, none, before);
    take_all(memory, full_before_);
    const u32 pc = c.pc;
    const u64 finished = pass_finished_;
    const bool served = run_pass(c);
    if (!served || pass_finished_ == finished)
        return served;  // handed back to the guest body mid-way: nothing to compare
    take(c, none, native);
    take_all(memory, full_native_);
    put(c, none, before);
    put_all(memory, full_before_);
    const u32 link = c.lr;
    reference_ = true;
    body->at(c, kPass);
    c.finish_transfers();
    reference_ = false;
    if (c.pc != link) {
        put_all(memory, full_native_);
        put(c, none, native);
        c.pc = pc;
        ++incomplete_;
        return true;
    }
    c.pc = pc;
    take(c, none, guest);
    take_all(memory, full_guest_);
    ++pass_verified_;
    const u32 elsewhere = full_guest_.size() == full_native_.size()
                              ? first_difference(memory, full_native_, full_guest_, before.gpr[1])
                              : 1u;
    native.gpr[3] = guest.gpr[3];  // the pass returns nothing: r3 is volatile here
    if (!same_contract(native, guest) || elsewhere != 0u) {
        if (++pass_differed_ <= 8u) {
            std::fprintf(stderr, "[chara-colli] native pair pass differs from the guest body\n");
            for (u32 i = 1; i < 32u; ++i)
                if ((i <= 3u || i >= 13u) && native.gpr[i] != guest.gpr[i])
                    std::fprintf(stderr, "  r%u native %08X guest %08X\n", i, native.gpr[i], guest.gpr[i]);
            if (!same_fpscr(native.fpscr, guest.fpscr))
                std::fprintf(stderr, "  fpscr native %08X guest %08X\n", native.fpscr, guest.fpscr);
            if (native.lr != guest.lr)
                std::fprintf(stderr, "  lr native %08X guest %08X\n", native.lr, guest.lr);
            if (elsewhere != 0u) {
                std::fprintf(stderr, "  memory differs at %08X (r1 %08X):", elsewhere, before.gpr[1]);
                std::size_t at = 0;
                u32 shown = 0;
                for (const GuestMemory::Region& region : memory.regions()) {
                    for (u32 i = 0; i < region.committed && shown < 24u; ++i)
                        if (full_native_[at + i] != full_guest_[at + i] && region.base + i >= before.gpr[1]) {
                            std::fprintf(stderr, " %08X n%02X g%02X", region.base + i, full_native_[at + i],
                                         full_guest_[at + i]);
                            ++shown;
                        }
                    at += region.committed;
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    return true;
}

// ---- --trace-stale-contacts: who reads the contact result bytes no write reached ----

namespace {
constexpr u32 kStalePageBase = 0x80000000u;
constexpr u32 kStalePages = 0x02000000u >> 12;  // 32 MiB of the cached window, 4 KiB pages
GEKKO_ALWAYS_INLINE u32 stale_page(u32 address) noexcept { return (address - kStalePageBase) >> 12; }
}  // namespace

void CharaColliIntrinsic::trace_stale_contacts(Cpu& c) {
    stale_cpu_ = &c;
    stale_tracer = this;
    stale_pages_.assign(kStalePages, 0u);
    c.guest_memory().set_access_trace(
        [this](u32 address, std::size_t size, bool writing) { stale_access(address, static_cast<u32>(size), writing); });
}

void CharaColliIntrinsic::stale_mark(u32 address, int delta) {
    const u32 page = stale_page(address);
    if (page < kStalePages)
        stale_pages_[page] = static_cast<u32>(static_cast<s32>(stale_pages_[page]) + delta);
}

void CharaColliIntrinsic::stale_taint(u32 address, u8 offset) {
    if (stale_bytes_.insert_or_assign(address, offset).second)
        stale_mark(address, 1);
}

void CharaColliIntrinsic::stale_clean(u32 address) {
    const auto it = stale_bytes_.find(address);
    if (it == stale_bytes_.end())
        return;
    stale_bytes_.erase(it);
    stale_mark(address, -1);
}

bool CharaColliIntrinsic::stale_init(Cpu& c) {
    stale_note(c, c.gpr[3]);
    return false;
}

// A result R (0x38 bytes) on the stack initialised (0x8001FF00, or the native loops' copy of it): every byte is
// stale until written. Results in frames that have returned are forgotten.
void CharaColliIntrinsic::stale_note(Cpu& c, u32 r) {
    const u32 sp = c.gpr[1];
    for (auto it = stale_results_.begin(); it != stale_results_.end();) {
        if (*it < sp) {
            for (u32 i = 0; i < 0x38u; ++i)
                stale_clean(*it + i);
            it = stale_results_.erase(it);
        } else {
            ++it;
        }
    }
    if (r < sp || r - sp >= 0x400u)
        return;
    stale_results_.insert(r);
    ++stale_inits_;
    for (u32 i = 0; i < 0x38u; ++i)
        stale_taint(r + i, static_cast<u8>(i));
}

// A copy armed at its entry: the source's stale bytes are carried to the destination when its last store lands (the
// copy's own stores come after the entry and would otherwise clear them); its source reads are not counted.
void CharaColliIntrinsic::stale_arm(u32 dst, u32 src, u32 size, u32 last) {
    StaleCopy copy{src, size, last, {}};
    for (u32 i = 0; i < size; ++i) {
        const auto it = stale_bytes_.find(src + i);
        if (it != stale_bytes_.end())
            copy.carried.push_back({dst + i, it->second});
    }
    stale_copies_.push_back(std::move(copy));
    stale_mark(last, 1);
}

// A copy routine (dst r3, src r4, size bytes, its last store at +last): 0x800267F8 / 0x800C2B8C (a contact result,
// last +0x34), 0x800091D0 / 0x800091EC (an RwV3d, +8), 0x8040D198 (twelve words, +0x2C).
bool CharaColliIntrinsic::stale_copy_block(Cpu& c, u32 size, u32 last) {
    stale_arm(c.gpr[3], c.gpr[4], size, c.gpr[3] + last);
    return false;
}

// 0x8002686C (object r3, R r5): R copied to +0xC (0x800267F8), R +0xC to +0x44 (its last store).
bool CharaColliIntrinsic::stale_copy(Cpu& c) {
    ++stale_contacts_;
    stale_arm(c.gpr[3] + 0x44u, c.gpr[5] + 0x0Cu, 4u, c.gpr[3] + 0x44u);
    return false;
}

void CharaColliIntrinsic::stale_access(u32 address, u32 size, bool writing) {
    const u32 first = stale_page(address);
    const u32 last = stale_page(address + size - 1u);
    if (first >= kStalePages || last >= kStalePages || (stale_pages_[first] == 0u && stale_pages_[last] == 0u))
        return;
    if (!writing) {
        for (u32 i = 0; i < size; ++i) {
            const u32 at = address + i;
            const auto it = stale_bytes_.find(at);
            if (it == stale_bytes_.end())
                continue;
            bool copying = false;
            for (const StaleCopy& copy : stale_copies_)
                copying = copying || at - copy.src < copy.size;
            if (copying)
                continue;
            const u32 sp = stale_cpu_->gpr[1];
            const bool stack = at >= sp - 0x10000u && at < sp + 0x10000u;
            ++stale_reads_;
            ++stale_readers_[(static_cast<u64>(stale_cpu_->lr) << 9) | (u64{stack} << 8) | it->second];
        }
        return;
    }
    for (u32 i = 0; i < size; ++i)
        stale_clean(address + i);
    for (auto it = stale_copies_.begin(); it != stale_copies_.end();) {
        if (address <= it->last && address + size > it->last) {
            for (const auto& [at, offset] : it->carried) {
                stale_taint(at, offset);
                const u32 sp = stale_cpu_->gpr[1];
                if (!(at >= sp - 0x10000u && at < sp + 0x10000u))
                    ++stale_carried_[offset];
            }
            stale_mark(it->last, -1);
            it = stale_copies_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string CharaColliIntrinsic::stale_summary() const {
    std::string out;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "  Stale contact result bytes: %llu results initialised, %llu contact objects; %llu reads of a stale "
                  "byte (copies excluded), %zu bytes still stale\n",
                  static_cast<unsigned long long>(stale_inits_), static_cast<unsigned long long>(stale_contacts_),
                  static_cast<unsigned long long>(stale_reads_), stale_bytes_.size());
    out += line;
    out += "    stale bytes carried off the stack, per R word:";
    for (u32 off = 0; off < 0x38u; off += 4u) {
        u64 n = 0;
        for (u32 k = 0; k < 4u; ++k)
            n += stale_carried_[off + k];
        if (n != 0u) {
            std::snprintf(line, sizeof(line), " +%02X %llu", off, static_cast<unsigned long long>(n));
            out += line;
        }
    }
    out += "\n";
    std::vector<std::pair<u64, u64>> readers(stale_readers_.begin(), stale_readers_.end());
    std::sort(readers.begin(), readers.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < readers.size() && i < 32u; ++i) {
        std::snprintf(line, sizeof(line), "    read with lr %08X (%s) of R +%02X: %llu\n",
                      static_cast<unsigned>(readers[i].first >> 9), (readers[i].first >> 8) & 1u ? "stack" : "heap",
                      static_cast<unsigned>(readers[i].first & 0xFFu),
                      static_cast<unsigned long long>(readers[i].second));
        out += line;
    }
    return out;
}

}  // namespace gekko
