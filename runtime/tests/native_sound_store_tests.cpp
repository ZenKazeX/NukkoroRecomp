// The native stores that replaced ARAM (docs/ARAM.md steps 2-4): the contracts the lowered sites keep, checked on
// a bare System. The in-game evidence (byte-identical audio over the tapes) is in docs/ARAM.md; this covers what play
// cannot reach - the releases (the game never frees its banks or ring in play).

#include "gekko/system.h"

#include <iostream>
#include <memory>
#include <stdexcept>

using namespace gekko;

namespace {

void require(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}

// Fixture memory past the DOL's sections, so no literal here roots game code (extract/liveness-roots.txt).
constexpr u32 kSizeOut = 0x80F40100u;  // ARFree's size word

std::unique_ptr<System> boot() {
    auto s = std::make_unique<System>();
    s->cpu().reset();
    s->start();
    return s;
}

// The banks' and the ring's releases (never reached in play): the block's base back, the size ARFree reports
// written where r3 points, the arena unmapped and reservable again.
void releases_report_the_size() {
    auto s = boot();
    Cpu& c = s->cpu();
    NativeSoundBanks& banks = s->sound_banks();
    require(banks.reserve(c, 0x800000u) == NativeSoundBanks::kSoundBase, "the banks' address is their base");
    require(banks.release(c, kSizeOut) == NativeSoundBanks::kSoundBase && c.mem_read32(kSizeOut) == 0x800000u,
            "the banks' release reports their size");
    require(banks.arena().source().bytes == nullptr, "the released banks hold nothing");
    NativeStreamRing& ring = s->stream_ring();
    require(ring.reserve(c, 0x40000u) == NativeStreamRing::kSoundBase, "the ring's address is its base");
    require(ring.release(c, kSizeOut) == NativeStreamRing::kSoundBase && c.mem_read32(kSizeOut) == 0x40000u,
            "the ring's release reports its size (fn_803D0530 compares it)");
    require(ring.reserve(c, 0x40000u) == NativeStreamRing::kSoundBase, "a released ring can be set up again");
    require(!c.stopped(), "the releases stopped the CPU");
    std::cout << "PASS banks and ring: releases report the size and free the arena\n";
}

}  // namespace

int main() {
    try {
        releases_report_the_size();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << "\n";
        return 1;
    }
    return 0;
}
