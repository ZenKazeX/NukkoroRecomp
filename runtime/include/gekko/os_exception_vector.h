#pragma once

// __OSExceptionVector, the OS's exception entry (SDK address 0x80371620 in this image), as data: the 38
// instructions __OSExceptionInit copies to every exception vector with `li r3, <index>` patched at +0x68.
//
// The runtime performs the vector's work natively (gekko/os_exception_entry.cpp), so it never enters that code. It
// only checks that the guest installed this page before it takes an exception; the words it compares with are kept
// here as data, not read back from the SDK's own copy in the image, so the check does not name (and does not keep)
// the SDK function. A reference test compares this table with the original words of the pinned DOL.

#include <array>

#include "gekko/types.h"

namespace gekko {

constexpr u32 kOsVectorWords = 38u;
constexpr u32 kOsVectorPatchOffset = 0x68u;  // `li r3, <index>`

inline constexpr std::array<u32, kOsVectorWords> kOsExceptionVector = {
    0x7C9043A6u, 0x808000C0u, 0x9064000Cu, 0x7C7042A6u, 0x90640010u, 0x90A40014u, 0xA06401A2u,
    0x60630002u, 0xB06401A2u, 0x7C600026u, 0x90640080u, 0x7C6802A6u, 0x90640084u, 0x7C6902A6u,
    0x90640088u, 0x7C6102A6u, 0x9064008Cu, 0x7C7A02A6u, 0x90640198u, 0x7C7B02A6u, 0x9064019Cu,
    0x7C651B78u, 0x60000000u, 0x7C6000A6u, 0x60630030u, 0x7C7B03A6u, 0x38600000u, 0x808000D4u,
    0x54A507BDu, 0x40820014u, 0x3CA08037u, 0x38A516BCu, 0x7CBA03A6u, 0x4C000064u, 0x546515BAu,
    0x80A53000u, 0x7CBA03A6u, 0x4C000064u,
};

// The system-call vector (exception index 9): __OSInitSystemCall replaces the first seven instructions of its page with
// PPCSync's head (0x8037829C in this image): mfspr r9, HID0; ori r10, r9, 8; mtspr HID0, r10; isync; sync; mtspr HID0,
// r9; rfi. Kept as data for the same reason as the vector.
constexpr u32 kOsSystemCallHeadWords = 7u;
inline constexpr std::array<u32, kOsSystemCallHeadWords> kOsSystemCallHead = {
    0x7D30FAA6u, 0x612A0008u, 0x7D50FBA6u, 0x4C00012Cu, 0x7C0004ACu, 0x7D30FBA6u, 0x4C000064u,
};

// The word `word` of the vector for SDK exception `index` (the `li r3, <index>` patched in).
constexpr u32 os_exception_vector_word(u32 word, u32 index) noexcept {
    return word * 4u == kOsVectorPatchOffset ? (0x38600000u | index) : kOsExceptionVector[word];
}

// The word `word` of the page __OSExceptionInit and __OSInitSystemCall leave at the vector of exception `index` (for the
// system-call vector, index 9, PPCSync's head replaces the first seven words).
constexpr u32 os_exception_page_word(u32 word, u32 index) noexcept {
    return index == 9u && word < kOsSystemCallHeadWords ? kOsSystemCallHead[word] : os_exception_vector_word(word, index);
}

// Writes the page a guest's __OSExceptionInit would leave at the vector address `at` for exception `index`, into
// any memory with write32 (the guest memory of a test).
template <class Memory>
void write_os_exception_vector(Memory& memory, u32 at, u32 index) {
    for (u32 word = 0; word < kOsVectorWords; ++word) memory.write32(at + 4u * word, os_exception_vector_word(word, index));
}

}  // namespace gekko
