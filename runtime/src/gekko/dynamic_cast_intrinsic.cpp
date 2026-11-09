#include "gekko/dynamic_cast_intrinsic.h"

#include "gekko/cpu.h"

namespace gekko {

namespace {

// The guest's name comparison: equal exactly when every byte up to and including the terminator is equal. A name
// longer than the bound is compared over the bound only, which no RTTI name approaches (the longest is ~150 bytes).
bool names_equal(Cpu& cpu, u32 a, u32 b) {
    if (a == b)
        return true;
    for (u32 i = 0; i < 4096u; ++i) {
        const u8 x = cpu.mem_read8(a + i);
        if (x != cpu.mem_read8(b + i))
            return false;
        if (x == 0)
            return true;
    }
    return true;
}

}  // namespace

DynamicCastIntrinsic::Result DynamicCastIntrinsic::walk(Cpu& cpu, u32 vtable, u32 rtti, u32 top, u32 to,
                                                        u32 from) const {
    (void)vtable;
    Result r;
    r.rtti = rtti;
    r.top = top;
    if (rtti == 0)
        return r;  // no RTTI: fail
    if (to == 0) {
        r.found = true;  // dynamic_cast<void*>: the complete object
        r.delta = top;
        return r;
    }
    const u32 to_name = cpu.mem_read32(to);
    if (names_equal(cpu, cpu.mem_read32(rtti), to_name)) {
        r.found = true;
        r.delta = top;
        return r;
    }
    u32 entry = cpu.mem_read32(rtti + 4u);
    if (entry == 0)
        return r;
    for (u32 base; (base = cpu.mem_read32(entry)) != 0;) {
        const u32 word = cpu.mem_read32(entry + 4u);
        if ((word & 0x80000000u) == 0u) {
            if (names_equal(cpu, cpu.mem_read32(base), to_name)) {
                r.found = true;
                r.delta = top + word;
                return r;
            }
            entry += 8u;
            continue;
        }
        // A repeated base: `count` {type, offset} pairs name the path from each source type.
        const s32 count = static_cast<s32>(cpu.mem_read32(entry + 8u));
        if (top + (word & 0x7FFFFFFFu) == 0u && names_equal(cpu, cpu.mem_read32(base), to_name)) {
            if (count <= 0)
                return r;
            const u32 from_name = cpu.mem_read32(from);
            for (s32 i = 0; i < count; ++i) {
                const u32 pair = entry + 12u + 8u * static_cast<u32>(i);
                const u32 offset = cpu.mem_read32(pair + 4u);
                if (top + offset != 0u)
                    continue;
                if (names_equal(cpu, cpu.mem_read32(cpu.mem_read32(pair)), from_name)) {
                    r.found = true;
                    r.delta = top + offset;
                    return r;
                }
            }
            return r;
        }
        entry += 12u + 8u * static_cast<u32>(count);
    }
    return r;
}

const DynamicCastIntrinsic::Result& DynamicCastIntrinsic::remembered(Cpu& cpu, u32 object, u32 vptr_offset, u32 to,
                                                                    u32 from) {
    const u32 vtable = cpu.mem_read32(object + vptr_offset);
    const u32 rtti = cpu.mem_read32(vtable);
    const u32 top = cpu.mem_read32(vtable + 4u);
    const Key key{vtable, vptr_offset, to, from};
    auto found = memo_.find(key);
    if (found == memo_.end() || found->second.rtti != rtti || found->second.top != top) {
        ++computed_;
        found = memo_.insert_or_assign(key, walk(cpu, vtable, rtti, top, to, from)).first;
    }
    return found->second;
}

u32 DynamicCastIntrinsic::cast(Cpu& cpu, u32 object, u32 vptr_offset, u32 to, u32 from) {
    if (object == 0u)
        return 0u;
    const Result& r = remembered(cpu, object, vptr_offset, to, from);
    return r.found ? object + r.delta : 0u;
}

bool DynamicCastIntrinsic::serve(Cpu& cpu) {
    const u32 object = cpu.gpr[3];
    const u32 vptr_offset = cpu.gpr[4];
    const u32 to = cpu.gpr[5];
    const u32 from = cpu.gpr[6];
    const bool reference = static_cast<s16>(cpu.gpr[7]) != 0;
    u32 result = 0;
    if (object != 0u) {
        const Result& found = remembered(cpu, object, vptr_offset, to, from);
        if (!found.found) {
            if (reference) {
                ++declines_;
                return false;  // bad_cast: the guest body throws it
            }
        } else {
            result = object + found.delta;
        }
    }
    ++calls_;
    // The guest prologue's stores and epilogue's reloads (stwu r1,-0x20; stw r0(LR),0x24; stw r31,0x1C).
    const u32 stack = cpu.gpr[1];
    cpu.mem_write32(stack - 0x20u, stack);
    cpu.mem_write32(stack + 4u, cpu.lr);
    cpu.mem_write32(stack - 4u, cpu.gpr[31]);
    cpu.gpr[0] = cpu.lr;
    cpu.gpr[3] = result;
    return true;
}

}  // namespace gekko
