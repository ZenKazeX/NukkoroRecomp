#pragma once

// __AXSyncPBs (0x80386BC0) with its callees __AXSyncPB (0x80386474), the depop accumulation (0x803863D8) and the callback stack push
// (0x80384914), owned natively (docs/agents/audio-ax.md, stage e: the native sync; step 2 keeps the DSP side's lagging copy of the
// parameter blocks in a native image (ax_pb_image.h) instead of the guest array at 0x8058FCA0).
//
// The original runs once per AI frame (from __AXOutNewFrame) and walks the voice priority stacks 31..1 and then the free stack:
//  - the "console DSP budget, preserved for identical output": every running voice (state +0x146 == 1, or a pending write ring
//    +0x28) adds its estimated DSP cycles (the table at 0x80563348, indexed by the source type +0x1DE, the mix flags +0x144, +0x140 and
//    +0x1F2) to a running total that starts at the command list's cycles (sda 0x5D30) + the call's argument + 0x10000 - 0x55F0; a
//    voice whose total reaches the budget (sda 0x5DA0 = bus clock * 0x51EB851F >> 7) is DROPPED: its PB state and ramps are cleared,
//    its AXVPB state is 0 and it is pushed on the callback stack (sda 0x5CE8), so __AXServiceCallbackStack runs its drop callback.
//    Measured on ~497 K AX frames (Westopolis, Final Haunt, Lethal Highway, the ramp tape, the 38 stage boots): no voice is ever
//    dropped; the arithmetic stays exact so the output is unchanged.
//  - __AXSyncPB: the AXVPB embedded PB (+0x138) <-> PB image exchange, driven by the dirty word +0x1C. Pushes AXVPB -> PB per dirty
//    bit; copies state and the current address back PB -> AXVPB when their bits are clear. The copy-back is the one-frame-late
//    observable behaviour: the DSP's result of the previous frame reaches the AXVPB (and so GCAX / CRI) only at this sync. Kept
//    exactly ("one-frame-late copy-back, preserved for identical output").
//  - the SDK's depop sums (sda 0x5D78..0x5D98, the DSP's click-removal ramps) are not accumulated any more: only the command list's
//    depop record read them, and that went with the list (step 3b).
//  - afterwards the dirty / write-ring words of every synced voice are cleared (+0x1C +0x20 +0x24 +0x28, +0x30 = self + 0x34).
// The cache invalidate / store calls of the original have no effect on guest memory here and are not performed.

#include <cstddef>
#include <string>

#include "gekko/ax_pb_image.h"
#include "gekko/cpu.h"
#include "gekko/types.h"

namespace gekko::ax_sync {

constexpr u32 kSyncPBs = 0x80386BC0u;
constexpr u32 kStackHeads = 0x80587DC0u;
constexpr u32 kCostTable = 0x80563348u;
constexpr u32 kSdaCallbackStack = 0x5CE8u, kSdaCycles = 0x5D30u, kSdaBudget = 0x5DA0u, kSdaTotalCost = 0x5DA4u, kSdaSyncCount = 0x5DA8u;
constexpr u32 kEmbedded = 0x138u;  // the PB copy inside the AXVPB

struct Owners {
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kSyncPBs, "__AXSyncPBs", false}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// One voice's PB in the image. An index outside the 64 voices (never produced by the SDK) reads zero and drops writes.
struct Pb {
    AxPbImage& image;
    u32 index;
    bool ok() const noexcept { return index < AxPbImage::kCount; }
    u16 r16(u32 off) const noexcept { return ok() ? image.r16(index, off) : u16{0}; }
    u32 r32(u32 off) const noexcept { return ok() ? image.r32(index, off) : 0u; }
    void w16(u32 off, u16 v) const noexcept { if (ok()) image.w16(index, off, v); }
    void w32(u32 off, u32 v) const noexcept { if (ok()) image.w32(index, off, v); }
};

// AXVPB embedded block -> PB, halfword range.
inline void copy16(Cpu& c, u32 e, const Pb& pb, u32 first, u32 last) {
    for (u32 off = first; off <= last; off += 2u) pb.w16(off, c.mem_read16(e + off));
}
inline void push32(Cpu& c, u32 e, const Pb& pb, u32 offset) { pb.w32(offset, c.mem_read32(e + offset)); }

// The pending-write ring: vpb+0x28 words between vpb+0x34 and the DSP's update list. With the dirty bit 0x80 the guest's list goes to the
// DSP; with the whole-block bit 0x80000000 the original copies the list back the other way (DSP side -> AXVPB), which is what the first
// arm of __AXSyncPB does.
inline void copy_write_ring(Cpu& c, AxPbImage& image, u32 vpb, u32 index, bool to_dsp) {
    const u32 count = c.mem_read32(vpb + 0x28u);
    if (count == 0u || index >= AxPbImage::kCount) return;
    for (u32 i = 0; i < count; ++i) {
        if (to_dsp) image.set_ring_word(index, i, c.mem_read32(vpb + 0x34u + 4u * i));
        else c.mem_write32(vpb + 0x34u + 4u * i, image.ring_word(index, i));
    }
}

// 0x80386474 __AXSyncPB.
inline void sync_pb(Cpu& c, AxPbImage& image, u32 vpb) {
    const u32 sda = c.gpr[13];
    c.mem_write32(sda + kSdaSyncCount, c.mem_read32(sda + kSdaSyncCount) + 1u);
    const u32 e = vpb + kEmbedded;
    const u32 index = c.mem_read32(vpb + 0x18u);
    const Pb pb{image, index};
    const u32 flags = c.mem_read32(vpb + 0x1Cu);
    if (flags == 0u) {  // nothing pending: the DSP's state and position come back
        c.mem_write16(e + 0xE, pb.r16(0xE));
        c.mem_write16(e + 0x64, pb.r16(0x64));
        c.mem_write16(e + 0x7A, pb.r16(0x7A));
        c.mem_write16(e + 0x7C, pb.r16(0x7C));
        return;
    }
    if ((flags & 0x80000000u) != 0u) {  // a whole new parameter block
        for (u32 off = 0; off <= 0xCCu; off += 4u) push32(c, e, pb, off);
        copy_write_ring(c, image, vpb, index, false);
        return;
    }
    if ((flags & 0x1u) != 0u) {
        pb.w16(8, c.mem_read16(e + 8));
        pb.w16(0xA, c.mem_read16(e + 0xA));
    }
    if ((flags & 0x2u) != 0u) pb.w16(0xC, c.mem_read16(e + 0xC));
    if ((flags & 0x4u) != 0u) pb.w16(0xE, c.mem_read16(e + 0xE));
    else c.mem_write16(e + 0xE, pb.r16(0xE));
    if ((flags & 0x8u) != 0u) pb.w16(0x10, c.mem_read16(e + 0x10));
    if ((flags & 0x10u) != 0u) copy16(c, e, pb, 0x12, 0x34);
    if ((flags & 0x40u) != 0u) {
        copy16(c, e, pb, 0x40, 0x42);
    } else if ((flags & 0x20u) != 0u) {
        copy16(c, e, pb, 0x36, 0x42);
        const u32 scratch = c.mem_read32(vpb + 0x134u);
        for (u32 off = 0; off <= 0x3Cu; off += 4u) c.mem_write32(scratch + off, 0u);
    }
    if ((flags & 0x80u) != 0u) {
        copy16(c, e, pb, 0x44, 0x4C);
        copy_write_ring(c, image, vpb, index, true);
    }
    if ((flags & 0x100u) != 0u) copy16(c, e, pb, 0x52, 0x62);
    if ((flags & 0x400u) != 0u) {
        c.mem_write16(e + 0x64, pb.r16(0x64));
        pb.w16(0x66, c.mem_read16(e + 0x66));
    } else if ((flags & 0x200u) != 0u) {
        pb.w16(0x64, c.mem_read16(e + 0x64));
        pb.w16(0x66, c.mem_read16(e + 0x66));
    }
    if ((flags & 0x800u) != 0u) copy16(c, e, pb, 0x68, 0x6C);
    if ((flags & 0x1E000u) != 0u) {
        if ((flags & 0x2000u) != 0u) pb.w16(0x6E, c.mem_read16(e + 0x6E));
        if ((flags & 0x4000u) != 0u) push32(c, e, pb, 0x72);
        if ((flags & 0x8000u) != 0u) push32(c, e, pb, 0x76);
        if ((flags & 0x10000u) != 0u) push32(c, e, pb, 0x7A);
        else c.mem_write32(e + 0x7A, pb.r32(0x7A));
    } else if ((flags & 0x1000u) != 0u) {
        for (const u32 off : {0x6Eu, 0x72u, 0x76u, 0x7Au}) push32(c, e, pb, off);
    } else {
        c.mem_write16(e + 0x7A, pb.r16(0x7A));
        c.mem_write16(e + 0x7C, pb.r16(0x7C));
    }
    if ((flags & 0x20000u) != 0u)
        for (u32 off = 0x7Eu; off <= 0xA2u; off += 4u) push32(c, e, pb, off);
    if ((flags & 0x80000u) != 0u) copy16(c, e, pb, 0xA6, 0xA8);
    else if ((flags & 0x40000u) != 0u) copy16(c, e, pb, 0xA6, 0xB2);
    if ((flags & 0x100000u) != 0u) copy16(c, e, pb, 0xB4, 0xB8);
    if ((flags & 0x400000u) != 0u) copy16(c, e, pb, 0xBE, 0xC0);
    else if ((flags & 0x200000u) != 0u) copy16(c, e, pb, 0xBA, 0xC0);
}

inline void push_callback_stack(Cpu& c, u32 vpb) {  // 0x80384914
    const u32 sda = c.gpr[13];
    c.mem_write32(vpb + 8u, c.mem_read32(sda + kSdaCallbackStack));
    c.mem_write32(sda + kSdaCallbackStack, vpb);
}

// The fields a free or dropped voice's PB loses (the SDK's ramps and the state).
inline void clear_pb_ramps(const Pb& pb) {
    for (const u32 off : {0x4Cu, 0x4Au, 0x48u, 0x46u, 0x44u, 0xEu}) pb.w16(off, 0);
}

// 0x80386BC0 __AXSyncPBs(cycles): see the header comment.
inline void sync_pbs(Cpu& c, AxPbImage& image) {
    const u32 sda = c.gpr[13];
    c.mem_write32(sda + kSdaSyncCount, 0u);
    u32 total = c.mem_read32(sda + kSdaCycles) + 0x10000u + c.gpr[3] - 0x55F0u;
    const u32 budget = c.mem_read32(sda + kSdaBudget);
    const auto clear_after_sync = [&](u32 vpb) {
        c.mem_write32(vpb + 0x1Cu, 0u);
        c.mem_write32(vpb + 0x20u, 0u);
        c.mem_write32(vpb + 0x28u, 0u);
        c.mem_write32(vpb + 0x24u, 0u);
        c.mem_write32(vpb + 0x30u, vpb + 0x34u);
    };
    for (u32 priority = 0x1F; priority >= 1u; --priority) {
        u32 vpb = c.mem_read32(kStackHeads + (priority << 2));
        while (vpb != 0u) {
            const Pb pb{image, c.mem_read32(vpb + 0x18u)};
            const bool running = c.mem_read16(vpb + 0x146u) == 1u || c.mem_read32(vpb + 0x28u) != 0u;
            if (!running) {
                sync_pb(c, image, vpb);
            } else {
                if (c.mem_read16(vpb + 0x140u) != 2u)
                    total += c.mem_read32(kCostTable + (static_cast<u32>(c.mem_read16(vpb + 0x1DEu)) << 2));
                if (c.mem_read16(vpb + 0x1F2u) != 0u) total += 0x22Bu;
                const u32 mix = c.mem_read16(vpb + 0x144u);
                const u32 a = c.mem_read32(kCostTable + 0x54u + ((mix >> 7) & 0x7Cu));
                const u32 b = c.mem_read32(kCostTable + 0x54u + ((mix >> 2) & 0x7Cu));
                const u32 d = c.mem_read32(kCostTable + 0x14u + ((mix << 2) & 0x3Cu));
                total += (b + a) + 0x8Cu;
                total += d;
                if (budget > total) {
                    sync_pb(c, image, vpb);
                } else {  // dropped: the console DSP could not have mixed it
                    clear_pb_ramps(pb);
                    c.mem_write16(vpb + 0x146u, 0);
                    push_callback_stack(c, vpb);
                }
            }
            clear_after_sync(vpb);
            vpb = c.mem_read32(vpb);
        }
    }
    c.mem_write32(sda + kSdaTotalCost, total);
    for (u32 vpb = c.mem_read32(kStackHeads); vpb != 0u; vpb = c.mem_read32(vpb)) {  // the free stack
        const Pb pb{image, c.mem_read32(vpb + 0x18u)};
        c.mem_write32(vpb + 0x20u, 0u);
        clear_pb_ramps(pb);
    }
}

inline void install(Cpu& cpu, AxPbImage& image) {
    cpu.install_hle(kSyncPBs, [&image](Cpu& c) {
        sync_pbs(c, image);
        return true;  // an owner never declines
    }, std::string("AX sync: __AXSyncPBs"));
}

}  // namespace gekko::ax_sync
