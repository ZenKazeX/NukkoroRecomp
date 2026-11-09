#pragma once

// The Metrowerks C++ runtime's __dynamic_cast (0x803A1AFC, 612 bytes; docs/CPU_FINAL_HAUNT.md), served natively.
//
// The guest function walks the object's RTTI and compares class-name strings byte by byte for every candidate base:
// on Final Haunt it was the game thread's largest self-time function (4.9 % of samples, 4.3 % from the ground
// behaviour's fn_80099454 alone). Its answer depends only on constant data - the object's vtable (its RTTI record and
// offset to the complete object) and the type_info records of the target and source types, all in the DOL's data -
// so the native walk below, the same algorithm, is computed once per (vtable, vptr offset, target, source) and kept:
//
//   void* __dynamic_cast(void* obj r3, long vptr_offset r4, type_info* to r5, type_info* from r6, short ref r7)
//     obj == 0 -> 0
//     vt = *(obj + vptr_offset); rtti = vt[0] (none: fail); top = vt[1] (offset to the complete object)
//     to == 0 -> obj + top                                      (dynamic_cast<void*>)
//     rtti->name == to->name -> obj + top                       (the exact type)
//     for each entry {base, word} of rtti->bases (until base == 0):
//       word bit 31 clear: base->name == to->name -> obj + top + word; else the next 8-byte entry
//       word bit 31 set (a repeated base, `count` = the entry's third word, then `count` {type, offset} pairs):
//         top + (word & 0x7FFFFFFF) == 0 and base->name == to->name:
//           count <= 0 -> fail; the pair with top + offset == 0 whose type's name == from->name -> obj + top + offset;
//           none -> fail
//         otherwise the next entry, 12 + 8 count bytes on
//     fail: ref != 0 -> throw bad_cast (served by the guest body: the hook declines); else 0
//   Names compare equal exactly when their bytes are equal up to the terminator (the guest's loop compares
//   sign-extended bytes and tests only for zero).
//
// What a served call leaves, as the guest body does on every non-throwing path: r3 the result, r0 the return address
// (its epilogue reloads it), r31 and r1 unchanged, and the three stack words its prologue writes - the back chain at
// r1 - 0x20, the return address at r1 + 4, r31 at r1 - 4. Other volatile registers are the ABI's (no caller reads them).
// A cached entry also keeps the vtable's two words it was computed from and is used only while they still read the same.

#include <cstdint>
#include <unordered_map>

#include "gekko/types.h"

namespace gekko {

class Cpu;

class DynamicCastIntrinsic {
public:
    // The hook: true when the call was served (r3 and the stack words written), false to run the guest body (the
    // throwing failure of a reference cast).
    bool serve(Cpu& cpu);

    // A pointer cast (ref = 0) for host code that serves a caller of __dynamic_cast natively: the served call's r3,
    // from the same remembered walks; nothing is written.
    u32 cast(Cpu& cpu, u32 object, u32 vptr_offset, u32 to, u32 from);

    u64 calls() const noexcept { return calls_; }
    u64 computed() const noexcept { return computed_; }  // walks made (the rest were remembered)
    u64 declines() const noexcept { return declines_; }

private:
    struct Key {
        u32 vtable, vptr_offset, to, from;
        bool operator==(const Key& o) const noexcept {
            return vtable == o.vtable && vptr_offset == o.vptr_offset && to == o.to && from == o.from;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            std::uint64_t h = (std::uint64_t{k.vtable} << 32) ^ k.to;
            h ^= (std::uint64_t{k.from} << 17) ^ (std::uint64_t{k.vptr_offset} << 3);
            h *= 0x9E3779B97F4A7C15ull;
            return static_cast<std::size_t>(h ^ (h >> 29));
        }
    };
    struct Result {
        u32 rtti = 0, top = 0;  // the vtable's two words the walk read
        bool found = false;
        u32 delta = 0;          // found: added to the object pointer
    };
    Result walk(Cpu& cpu, u32 vtable, u32 rtti, u32 top, u32 to, u32 from) const;
    const Result& remembered(Cpu& cpu, u32 object, u32 vptr_offset, u32 to, u32 from);

    std::unordered_map<Key, Result, KeyHash> memo_;
    u64 calls_ = 0, computed_ = 0, declines_ = 0;
};

}  // namespace gekko
