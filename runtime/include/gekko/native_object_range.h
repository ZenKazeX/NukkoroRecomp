#pragma once

// The layout objects' range - where the game spawns them - served natively,
// exactly as the original decides it.
//
// Every stage's set layout (stgNNNN_cmn/_nrm/_hrd/_ds1.dat, "sky2") gives each
// object an authored range byte (entry +0x23, 2..30 on Westopolis). The layout
// manager (0x800CAA2C two players / 0x800CAC08 one, each frame) spawns a
// dormant object when a player is within that range and asks a live one to
// leave once every player is outside it. Contract and original-code evidence:
// docs/DRAW_DISTANCE.md.
//
// The four guest functions that define the range, owned here:
//  * 0x800CA970 in_range(manager, slot, point): an entry flagged 0x80800 is
//    always in range; otherwise |entry.position - point|^2 <= (100 * range)^2
//    (single precision; NaN is out of range).
//  * 0x800C9D54 out_of_range(slot): 0 for an entry flagged 0x800; otherwise
//    1 iff every player's |position - entry.position|^2 is not below
//    (100 * range + margin)^2 (0x80169FC8; margin r13+0x3270, 50 from the
//    static initialiser 0x800CA324; two players when 0x8057E760+0xB2 != 0).
//  * 0x800C9D20 range_radius(slot) -> f1 = 100 * range + 100.
//  * 0x800CA200 range(slot, float* out): *out = 100 * range.
//
// The range is gameplay: what is alive, what thinks, what the game's heap
// holds. It stays the original's, bit for bit. Seeing farther is the native
// renderer's job (the far-object proxies, docs/DRAW_DISTANCE.md), not a
// larger range.

#include <cstddef>

#include "gekko/types.h"

namespace gekko {

class Cpu;

class NativeObjectRange {
public:
    explicit NativeObjectRange(Cpu& cpu) noexcept : cpu_(cpu) {}

    void install();

    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    // Told of each spawn the range admits (the layout entry and its type's
    // factory) and of the next range test, which the manager makes once that
    // spawn's factory has returned: everything the game created in between
    // belongs to the spawned object.
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void spawn_opened(u32 entry, u32 factory) = 0;
        virtual void range_tested() = 0;
    };
    void set_listener(Listener* listener) noexcept { listener_ = listener; }

    struct Stats {
        u64 spawn_tests = 0;     // in_range from the spawn path (0x800CA8D8)
        u64 spawns = 0;          // ... that admitted the object
        u64 leave_tests = 0;     // in_range from the despawn paths
        u64 leaves = 0;          // ... that found the object out of range
        u64 always_in_range = 0; // entries flagged 0x80800
        u64 out_of_range_tests = 0;
        u64 radius_reads = 0;    // 0x800C9D20 + 0x800CA200
    };
    const Stats& stats() const noexcept { return stats_; }

    // The original arithmetic, exposed for the tests.
    static float in_range_limit(u32 range_byte) noexcept;
    static float out_of_range_radius(u32 range_byte, float margin) noexcept;

private:
    void in_range(Cpu& c);
    void out_of_range(Cpu& c);
    void range_radius(Cpu& c);
    void range(Cpu& c);

    Cpu& cpu_;
    Listener* listener_ = nullptr;
    Stats stats_{};
};

}  // namespace gekko
