#pragma once

// The layout manager's link-group lists, rebuilt natively (docs/GPU_FILL.md "stage start";
// docs/distant-rendering/OBJECT_SPAWN_SYSTEM.md "The data" for the manager).
//
// The original, fn_800CAEAC(manager), runs fn_800CAE24(manager, g) for every link group g = 0..255, and each of those
// walks all 2,048 spawn slots: 524,288 slot visits, ~3 ms of translated code in the stage-start frame (16 % of it).
//
//   fn_800CAEAC: stwu r1,-0x10(r1); LR -> 0x14(r1); stmw r30,8(r1); r30 = manager;
//                for r31 = 0 .. 0xFF: fn_800CAE24(r30, r31); cmpwi r31,0x100;  then lmw, r0 = saved LR, mtlr, blr.
//   fn_800CAE24(r3 = manager, r4 = g): head[g] (manager + 0x1C + 4g) = 0; for each slot s at
//                [manager + 0x18] + 32 i, i = 0 .. 2047 (the array word re-read every visit), entry = [s]:
//                if ([entry + 0x18] & 1) and byte [entry + 0x22] == g, append s to group g's circular list:
//                  head 0:  head = s; [s + 4] = s;    [s + 8] = 0
//                  else:    t = [head + 4]; [t + 8] = s; [s + 4] = t; [s + 8] = 0; [head + 4] = s
//                (+4 the previous slot - the head's is the tail - and +8 the next, 0 at the tail).
//
// What this owner does instead: every head cleared, then ONE pass over the slots, appending each enabled slot to its
// own group's list. That is the original's result whenever no word the walk reads is one it writes: the visits of
// group g write only group g's slots (+4, +8), group g's head and the slots' list words, and read the array word,
// each slot's entry pointer and each entry's flags word and link byte. When an entry's fields, or the manager's
// array word, could lie on a written word (an entry pointer into the slot array or the head table - nothing the game
// builds, but nothing forbids it), the owner runs the original's 256 x 2,048 walk itself, in the original's order,
// so the result is the original's for every input. Either way the call leaves the original's registers: r0 = LR =
// the caller's return address, r3 = the manager, r4 = 0xFF, r5-r8 from the last visit (group 255, slot 2047),
// CTR 0, CR0 = EQ (cmpwi 256, 0x100) | SO; r30, r31, r1 restored; and its frame's four words below r1.
//
// tests/native_link_groups_tests.cpp: the owner against the original words in the reference interpreter.

#include <cstddef>

#include "gekko/types.h"

namespace gekko {

class Cpu;

class NativeLinkGroups {
public:
    explicit NativeLinkGroups(Cpu& cpu) noexcept : cpu_(cpu) {}

    void install();

    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    struct Stats {
        u64 rebuilds = 0;   // calls
        u64 one_pass = 0;   // served by the single pass
        u64 walked = 0;     // served by the original's walk (a read field could be written)
        u64 linked = 0;     // slots linked
    };
    const Stats& stats() const noexcept { return stats_; }

private:
    Cpu& cpu_;
    Stats stats_;
};

}  // namespace gekko
