// The layout manager's link-group lists. Contract and the original's words: include/gekko/native_link_groups.h.

#include "gekko/native_link_groups.h"

#include <array>
#include <string>
#include <utility>

#include "gekko/cpu.h"

namespace gekko {

namespace {

constexpr u32 kRebuildAll = 0x800CAEACu;  // fn_800CAEAC(manager): every group (fn_800CAE24 stays guest code)

const NativeLinkGroups::Address kAddresses[] = {
    {kRebuildAll, "the layout manager's link-group lists (all groups)", false},
};

constexpr u32 kSlotsWord = 0x18u;   // manager + 0x18: the slot array
constexpr u32 kHeads = 0x1Cu;       // manager + 0x1C: 256 list heads
constexpr u32 kGroups = 0x100u;
constexpr u32 kSlots = 0x800u;
constexpr u32 kSlotBytes = 0x20u;
constexpr u32 kSlotPrev = 4u, kSlotNext = 8u;
constexpr u32 kEntryFlags = 0x18u;  // bit 0: enabled
constexpr u32 kEntryLink = 0x22u;   // u8: the link group
constexpr u32 kCrEq = 0x2u;         // CR0 bits as cr >> 28: LT 8, GT 4, EQ 2, SO 1

// [lo, hi) of a read field overlaps [wlo, whi) of a written range (u64: no wrap).
bool overlaps(u64 lo, u64 hi, u64 wlo, u64 whi) noexcept { return lo < whi && wlo < hi; }

}  // namespace

const NativeLinkGroups::Address* NativeLinkGroups::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeLinkGroups::install() {
    Stats& stats = stats_;
    cpu_.install_hle(kRebuildAll, [&stats](Cpu& c) {
        ++stats.rebuilds;
        const u32 manager = c.gpr[3];
        const u32 sp = c.gpr[1];
        const u32 lr = c.lr;
        // The prologue's words: back chain, the caller's LR in the caller's frame, r30 and r31.
        c.mem_write32(sp - 0x10u, sp);
        c.mem_write32(sp + 4u, lr);
        c.mem_write32(sp - 8u, c.gpr[30]);
        c.mem_write32(sp - 4u, c.gpr[31]);

        // The written words: the slot array's list words (a range through every slot) and the head table. The read
        // fields: the array word, each slot's entry word (never written: +0), each entry's flags word and link byte.
        u32 r5 = 0, r8 = 0;
        const u32 slots = c.mem_read32(manager + kSlotsWord);
        const u64 slots_lo = slots, slots_hi = u64{slots} + u64{kSlots} * kSlotBytes;
        const u64 heads_lo = u64{manager} + kHeads, heads_hi = heads_lo + 4u * kGroups;
        bool safe = !overlaps(u64{manager} + kSlotsWord, u64{manager} + kSlotsWord + 4u, slots_lo, slots_hi) &&
                    !overlaps(u64{manager} + kSlotsWord, u64{manager} + kSlotsWord + 4u, heads_lo, heads_hi) &&
                    !overlaps(heads_lo, heads_hi, slots_lo, slots_hi);
        std::array<u32, kSlots> entry{};
        for (u32 i = 0; i < kSlots && safe; ++i) {
            entry[i] = c.mem_read32(slots + i * kSlotBytes);
            const u64 e = entry[i];
            for (const auto& [lo, hi] : {std::pair<u64, u64>{e + kEntryFlags, e + kEntryFlags + 4u},
                                         std::pair<u64, u64>{e + kEntryLink, e + kEntryLink + 1u}}) {
                if (overlaps(lo, hi, slots_lo, slots_hi) || overlaps(lo, hi, heads_lo, heads_hi)) safe = false;
            }
        }
        // Slot i of the walk visit (group g): the array word re-read, the slot, its entry, and the outcome's r5.
        const auto append = [&c, manager](u32 g, u32 slot) -> u32 {
            const u32 head_at = manager + kHeads + 4u * g;
            const u32 head = c.mem_read32(head_at);
            if (head == 0u) {
                c.mem_write32(head_at, slot);
                c.mem_write32(slot + kSlotPrev, slot);
                c.mem_write32(slot + kSlotNext, 0u);
                return head;  // r5 = the head word read: 0
            }
            const u32 tail = c.mem_read32(head + kSlotPrev);
            c.mem_write32(tail + kSlotNext, slot);
            c.mem_write32(slot + kSlotPrev, tail);
            c.mem_write32(slot + kSlotNext, 0u);
            c.mem_write32(head + kSlotPrev, slot);
            return c.mem_read32(head_at);  // r5 = the head word, re-read
        };
        if (safe) {
            ++stats.one_pass;
            for (u32 g = 0; g < kGroups; ++g) c.mem_write32(manager + kHeads + 4u * g, 0u);
            for (u32 i = 0; i < kSlots; ++i) {
                const u32 slot = slots + i * kSlotBytes;
                const u32 e = entry[i];
                r8 = slot;
                r5 = e;
                if ((c.mem_read32(e + kEntryFlags) & 1u) == 0u) continue;
                const u32 g = c.mem_read8(e + kEntryLink);
                const u32 r5_after = append(g, slot);
                ++stats.linked;
                // The original's last visit is group 255's: there r5 is the append's only for a group-255 slot.
                if (g == kGroups - 1u) r5 = r5_after;
            }
        } else {
            ++stats.walked;
            for (u32 g = 0; g < kGroups; ++g) {
                c.mem_write32(manager + kHeads + 4u * g, 0u);
                for (u32 i = 0; i < kSlots; ++i) {
                    const u32 slot = c.mem_read32(manager + kSlotsWord) + i * kSlotBytes;
                    r8 = slot;
                    r5 = c.mem_read32(slot);
                    if ((c.mem_read32(r5 + kEntryFlags) & 1u) == 0u) continue;
                    if (c.mem_read8(r5 + kEntryLink) != g) continue;
                    r5 = append(g, slot);
                    ++stats.linked;
                }
            }
        }
        // The registers the original returns with.
        c.gpr[0] = lr;
        c.gpr[3] = manager;
        c.gpr[4] = kGroups - 1u;
        c.gpr[5] = r5;
        c.gpr[6] = kSlots * kSlotBytes;
        c.gpr[7] = kHeads + 4u * (kGroups - 1u);
        c.gpr[8] = r8;
        c.ctr = 0u;
        const u32 so = (c.xer >> 31) & 1u;
        c.cr = (c.cr & 0x0FFFFFFFu) | ((kCrEq | so) << 28);
        return true;
    }, "native link groups: the layout manager's link-group lists");
}

}  // namespace gekko
