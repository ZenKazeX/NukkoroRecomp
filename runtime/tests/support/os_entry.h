#pragma once

// The OS's exception entry as the game's own OS leaves it, for reference tests
// that run the original SDK words of a loaded DOL in the interpreter.
//
// __OSExceptionInit copies __OSExceptionVector (38 instructions, gekko/os_exception_vector.h)
// to every exception vector with `li r3, <index>` patched at +0x68 and the
// handler registered in __OSExceptionTable (0x80003000); the system-call
// vector's first 7 instructions are PPCSync's head (0x8037829C) instead.
// OSSetCurrentContext keeps the current OSContext at low memory 0xD4. The CPU
// performs the vector's work natively (gekko/os_exception_entry.cpp) once it
// finds the vector built this way, so a test that takes an exception or runs
// `sc` installs exactly this state.

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/os_exception_vector.h"

namespace test_support {

constexpr gekko::u32 kOsExceptionTable = 0x80003000u;  // __OSExceptionTable
constexpr gekko::u32 kOsCurrentContext = 0x800000D4u;  // OS_CURRENTCONTEXT

// The vector for `vector` (SDK index `index`) built from the template, the
// handler registered for it.
inline void install_os_exception(gekko::GuestMemory& memory, gekko::Exception vector, gekko::u32 index,
                                 gekko::u32 handler) {
    const gekko::u32 at = 0x80000000u | static_cast<gekko::u32>(vector);
    gekko::write_os_exception_vector(memory, at, index);
    memory.write32(kOsExceptionTable + 4u * index, handler);
}

// The system-call vector (index 9): PPCSync's head, then the template's
// instructions 7..37 with the index patch.
inline void install_os_system_call(gekko::GuestMemory& memory) {
    const gekko::u32 at = 0x80000000u | static_cast<gekko::u32>(gekko::Exception::SystemCall);
    for (gekko::u32 word = 0; word < gekko::kOsVectorWords; ++word)
        memory.write32(at + 4u * word, gekko::os_exception_page_word(word, 9u));
}

}  // namespace test_support
