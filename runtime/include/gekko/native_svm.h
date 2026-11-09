#pragma once

// CRI's server manager - "SVM/GC Ver.1.58" (its build string at 0x805185A8) - as native C++20: the server table, its
// dispatch, registration, the lock pair and the test-and-set. docs/CRI_SERVERS.md section 1; reconstruction step 1.
//
// The original is eighteen functions (0x803CBFB8..0x803CCD50) over one .bss block S = 0x805C0E20; a byte scan of the
// image finds no other code addressing S, so S becomes this object's state and the eighteen bodies are pruned
// (extract/native-owners.txt). Its interface, as the callers (ADXT, ADXGC, ADXF, Sofdec, the CRI thread loops) use it:
//   SVM_Init / SVM_Finish (0x803CC194 / 0x803CC0CC): a reference count; the first Init and the last Finish clear the
//     "inside a server" flags, both lock hooks and the pass counters of types 0..5 - NOT the server table or the user
//     hooks (the original's memsets; the table is .bss, zero at boot).
//   servers 0x803CC3FC / 0x803CC36C / 0x803CC2DC / 0x803CC24C (types 2 / 4 / 5 / 6): for slot 0..5 of the type, a
//     non-null function is called with its argument, the type's "inside" flag set around the call; the results are
//     ORed and returned; then the type's pass counter is incremented. The servers take no lock.
//   SVM_SetCbSvr (0x803CC970, type, fn, arg, name) -> the first free slot's id, or -1 (type outside 0..7, or no free
//     slot); SVM_SetCbSvrId (0x803CC5F4, type, id, fn, arg, name): that slot (an occupied slot is overwritten, as
//     the original only warns); SVM_DelCbSvr (0x803CC7D0, type, id): the slot cleared. A null name is stored as
//     0x805186FC ("Unknown"). Each runs under the lock.
//   SetCbLock / SetCbUnlock (0x803CC49C / 0x803CC48C, fn, arg): the lock hooks (ADXGC installs its own).
//   svm_lock (0x803CCCEC): only when a lock hook is installed - the hook is called, then a depth of 0 records lock
//     type 1 and the depth is incremented. svm_unlock (0x803CCC5C): only when an unlock hook is installed - the depth
//     is decremented; at 0 a type other than 1 is reported ("svm_unlock:lock type miss match") and the type
//     cleared; then the hook is called.
//   user hooks: set (0x803CC4F0, index, fn, arg; under the lock) and call (0x803CC4AC, index): ADXGC sets index 6 to
//     adxm_goto_mwidle_border, Sofdec calls it.
//   test-and-set (0x803CBFB8, word*) under the lock: the word becomes 1; returns 1 unless it already was 1.
//   error reports (0x803CCB2C message, 0x803CCB84 printf format): the original formats into S+0x40 for an error
//     callback at S+0xC0 - which nothing in the image ever sets (S is .bss; no instruction writes S+0xC0). The
//     reports have no observable effect; here they are logged (non-shipping builds).
// Not kept: the test-and-set override at S+0x3C (no writer in the image) and the words S+0x2C / S+0x34 (cleared by
// Init / Finish, read by nothing).
//
// Guest calls (server callbacks, lock / unlock hooks, user hooks) may block - an ADX callback waits for an
// interrupt - so they are not nested host calls: each owner keeps its progress in a frame on the guest stack (like
// the original's own frame; reentrant: a callback may call the SVM again) and calls through the continuation entry
// kReturn, which resumes it. Remaining guest dependencies: those calls (the callbacks are CRI's translated code
// until reconstructed - steps 2-5 of docs/CRI_SERVERS.md), the continuation entry, the frames on the guest stack.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;

class NativeSvm {
public:
    explicit NativeSvm(Cpu& cpu) : cpu_(cpu) {}
    void install();

    static constexpr u32 kTypes = 8u, kSlots = 6u, kUserHooks = 8u;
    struct Slot {
        u32 function = 0, argument = 0, name = 0;
    };
    struct Hook {
        u32 function = 0, argument = 0;
    };
    const Slot& slot(u32 type, u32 id) const { return table_[type][id]; }
    const Hook& user_hook(u32 index) const { return user_hooks_[index]; }
    const Hook& lock_hook() const { return lock_hook_; }
    const Hook& unlock_hook() const { return unlock_hook_; }
    u32 refcount() const noexcept { return refcount_; }
    u32 lock_depth() const noexcept { return lock_depth_; }
    u32 lock_type() const noexcept { return lock_type_; }
    u32 inside(u32 type) const { return inside_[type]; }
    u32 passes(u32 type) const { return passes_[type]; }

    struct Stats {
        u64 server_calls = 0, callbacks = 0, locks = 0, unlocks = 0, registrations = 0, test_and_sets = 0,
            user_hook_calls = 0, errors = 0;
        std::array<u64, 8> server_calls_by_type{};
    };
    const Stats& stats() const noexcept { return stats_; }

    // The error report (0x803CCB2C) for a native caller: CRI's other modules (SJ, docs/CRI_SERVERS.md 4b) report here.
    void error_report(const std::string& text) { report(text.c_str()); }

    // The addresses the generator must route here (extract/native-owners.txt; CTest native_svm_tests).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    // Savestate (version 27). A state saved before the SVM was native holds it in guest memory: import_guest reads S.
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);
    void import_guest(Cpu& c);

    // The block the original kept the state in, and the continuation entry (inside SVM_Finish's pruned extent).
    static constexpr u32 kBlock = 0x805C0E20u;
    static constexpr u32 kReturn = 0x803CC0D0u;

private:
    enum class Op : u32;
    // One owner call: its frame is pushed on the guest stack and run until it calls the guest or finishes.
    void begin(Cpu& c, Op op, std::initializer_list<u32> args);
    void resume(Cpu& c);  // the continuation entry
    void run(Cpu& c, u32 frame);
    void call(Cpu& c, u32 frame, u32 phase, u32 function, u32 argument);
    void finish(Cpu& c, u32 frame, u32 result);
    void report(const char* message, u32 a = 0, u32 b = 0);

    Cpu& cpu_;
    std::array<std::array<Slot, kSlots>, kTypes> table_{};
    std::array<Hook, kUserHooks> user_hooks_{};
    Hook lock_hook_, unlock_hook_;
    u32 refcount_ = 0, lock_depth_ = 0, lock_type_ = 0;
    std::array<u32, kTypes> inside_{}, passes_{};
    Stats stats_;
};

}  // namespace gekko
