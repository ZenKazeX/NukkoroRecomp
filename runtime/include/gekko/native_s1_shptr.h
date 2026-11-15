#pragma once

// S1 core: the game's counted-handle family (boost::shared_ptr / weak_ptr, PAL main.dol), served natively. Map and evidence:
// docs/re/core.md section 8. Hand-written bodies (not re-emitted translated code).
//
// Control block (sp_counted_base): +0 vtable, +4 use count, +8 weak count (the strong group holds one weak); vtable +0xC dispose,
// +0x10 destroy. shared_count {pi}, shared_ptr {px, shared_count}, weak_count {pi}.
//
//   0x8000945C release(pi)              use--; zero: dispose (virtual), weak--, zero: destroy (virtual)
//   0x80009408 shared_count dtor(this, flag)   release(*this) when set; flag > 0 (s16): operator delete
//   0x800097E0 shared_count copy(dst, src)     *dst = *src; use++
//   0x80009E4C shared_count assign(this, rhs)  use++ of rhs, release of the old, store
//   0x80017454 weak_count dtor(this, flag)     weak--; zero: destroy (virtual); flag > 0: operator delete
//   0x80017930 / 0x8001AEE8 weak_count copy(dst, src)  *dst = *src; weak++
//   0x80016914 add_ref_lock(pi)         use == 0: throws bad_weak_ptr; else use++
//   0x80016838 shared_count(weak_count) (dst, src)     add_ref_lock of the weak's block (null block / expired: throws)
//   0x80028DB0 weak_ptr::lock(out, weak)       expired (use == 0): empty out; else out = shared_ptr(weak)
//   per-type clones (found by exact instruction pattern in the DOL, the hot ones installed): shared_ptr dtor wrappers
//   (this, flag): shared_count dtor(this + 4, -1), then operator delete for flag > 0; copy constructors (dst, src): px, then
//   shared_count copy; assignment (dst, src): px, then shared_count assign.
//
// Served: the paths that need no virtual call and no operator delete; every write is the translated code's, in its order: the control
// block counts AND the dead stack frames the translated chain leaves (state.json hashes them: the frame words of every level, with
// the return-address words of the nested calls and the callers' saved r28..r31). Handed back (nothing written, the translated body
// runs): the last strong release (dispose), the weak count reaching zero (destroy), operator delete (flag > 0), add_ref_lock of an
// expired block (throw), a null control block where the translated code throws, and any chain member that another hook (--watch,
// --context-hook, any foreign install_hle) covers, so those stay observable. Not kept (by the ABI): volatile registers, CR0, XER.
//
// Hooks: the chain entries (shared_count dtor, assign, shared_count-from-weak, lock, the per-type wrapper clones). The leaves, the weak_count
// dtor, release and add_ref_lock are served by serve() (the contract covers them) but NOT hooked unless GEKKO_S1_SHPTR_ALL=1: their guest
// bodies are as short as the dispatch (docs/agents/s1-core.md Round 6 counts).
//
// Switches: GEKKO_S1_SHPTR=1/0 at run time; CMake option SHADOW_S1_SHPTR (default OFF) makes it default-on; GEKKO_S1_SHPTR_STATS=1
// prints per-kind served / handed-back counts at exit.

#include "gekko/cpu.h"

namespace gekko::s1_shptr {

enum class Kind : u8 {
    Release, ScDtor, ScCopy, WeakCopy, Assign, WeakDtor, ScFromWeak, AddRefLock, Lock, DtorWrap, CopyWrap, AssignWrap, kCount
};

struct Stats {
    u64 served[static_cast<unsigned>(Kind::kCount)] = {};
    u64 declined[static_cast<unsigned>(Kind::kCount)] = {};
};

// One entry: address and kind (the LR constants of the clones derive from the address).
struct Entry {
    u32 address;
    Kind kind;
};

bool serve(Cpu& cpu, const Entry& entry);
const Stats& stats() noexcept;
void set_enabled(bool enabled) noexcept;
void install(Cpu& cpu);
void reset_chain_cache() noexcept;  // tests: forget the per-entry foreign-hook answers
const Entry* entries(unsigned& count) noexcept;  // every installed entry (the contract test installs the same set)

}  // namespace gekko::s1_shptr
