// The layout objects' range - where the game spawns them. Contract and
// rationale: include/gekko/native_object_range.h and docs/DRAW_DISTANCE.md.

#include "gekko/native_object_range.h"

#include <cstring>
#include <string>

#include "gekko/cpu.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
constexpr u32 kInRange = 0x800CA970u;
constexpr u32 kOutOfRange = 0x800C9D54u;
constexpr u32 kRangeRadius = 0x800C9D20u;
constexpr u32 kRange = 0x800CA200u;

// in_range's callers: the spawn test (0x800CA8D8) and the despawn tests of
// the two-player (0x800CAA2C) and one-player (0x800CAC08) manager updates.
constexpr u32 kSpawnReturn = 0x800CA914u;
constexpr u32 kLeaveReturns[] = {0x800CAB80u, 0x800CAB98u, 0x800CAD58u};

// A layout entry (0x2C bytes; the file's, byte-swapped by the loader).
constexpr u32 kEntryFlags = 0x18u;
constexpr u32 kEntryRange = 0x23u;
constexpr u32 kAlwaysInRange = 0x00080800u;  // in_range
constexpr u32 kNeverOutOfRange = 0x00000800u;  // out_of_range

// out_of_range's margin and the player list it measures against.
constexpr u32 kSdaMargin = 0x3270u;           // f32, 50 (0x800CA324)
constexpr u32 kSdaPlayers = 0x2B34u;          // the player list (0x80009548)
constexpr u32 kPlayersCount = 0x8u;           // its vector: count, data
constexpr u32 kPlayersData = 0xCu;            // 8-byte elements, object at +0
constexpr u32 kPlayerPosition = 0x8u;         // the object's position (0x80010244)
constexpr u32 kFrameObject = 0x8057E760u;
constexpr u32 kTwoPlayers = 0xB2u;            // byte: both players are measured

// The manager's per-type factories: +0x14 the table, a slot's +0x10 its index.
constexpr u32 kManagerFactories = 0x14u;
constexpr u32 kSlotFactoryIndex = 0x10u;

const NativeObjectRange::Address kAddresses[] = {
    {kInRange, "object range: in range (0x800CA970)"},
    {kOutOfRange, "object range: out of range (0x800C9D54)"},
    {kRangeRadius, "object range: radius (0x800C9D20)"},
    {kRange, "object range: range (0x800CA200)"},
};

float read_f32(Cpu& c, u32 address) {
    const u32 bits = c.mem_read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_f32(Cpu& c, u32 address, float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    c.mem_write32(address, bits);
}

// A single-precision result in f1, as the FPU leaves it in both halves.
void return_f32(Cpu& c, float value) {
    const double wide = static_cast<double>(value);
    u64 bits;
    std::memcpy(&bits, &wide, sizeof(bits));
    c.fpr[1].bits[0] = bits;
    c.fpr[1].bits[1] = bits;
}

// The authored distance: 100 * range, exact in single precision for a byte.
float distance(u32 range_byte) { return 100.0f * static_cast<float>(range_byte); }

// 0x8040DE54 then 0x8040DF7C: the component differences, then
// dz^2 + (dx^2 + dy^2), each step rounded to single.
float distance_squared(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return dz * dz + (dx * dx + dy * dy);
}

void read_position(Cpu& c, u32 address, float out[3]) {
    out[0] = read_f32(c, address);
    out[1] = read_f32(c, address + 4u);
    out[2] = read_f32(c, address + 8u);
}

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "object range: " + why);
}

}  // namespace

const NativeObjectRange::Address* NativeObjectRange::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

// The original multiplies 10000.0f by single(range * range); the same real
// value rounded once, so (100 * range)^2 is bit-identical.
float NativeObjectRange::in_range_limit(u32 range_byte) noexcept {
    const float d = distance(range_byte);
    return d * d;
}

// fmadds(100.0f, single(range), margin): the product is exact, so the fused
// result is the sum rounded once - which is what the separate add gives.
float NativeObjectRange::out_of_range_radius(u32 range_byte, float margin) noexcept {
    return distance(range_byte) + margin;
}

void NativeObjectRange::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, name);
    };
    own(kInRange, kAddresses[0].name, [this](Cpu& c) { in_range(c); });
    own(kOutOfRange, kAddresses[1].name, [this](Cpu& c) { out_of_range(c); });
    own(kRangeRadius, kAddresses[2].name, [this](Cpu& c) { range_radius(c); });
    own(kRange, kAddresses[3].name, [this](Cpu& c) { range(c); });
}

// (r3 manager, r4 slot, r5 point) -> r3.
void NativeObjectRange::in_range(Cpu& c) {
    const u32 entry = c.mem_read32(c.gpr[4]);
    const u32 point = c.gpr[5];
    const bool spawn = c.lr == kSpawnReturn;
    bool leave = false;
    for (u32 ret : kLeaveReturns) leave = leave || c.lr == ret;
    if (spawn) ++stats_.spawn_tests;
    if (leave) ++stats_.leave_tests;
    // The previous spawn's factory has returned: its window closes.
    if (listener_ != nullptr) listener_->range_tested();

    if ((c.mem_read32(entry + kEntryFlags) & kAlwaysInRange) != 0u) {
        ++stats_.always_in_range;
        c.gpr[3] = 1u;
        return;
    }
    float position[3], at[3];
    read_position(c, entry, position);
    read_position(c, point, at);
    // fcmpu, then cror eq <- lt | eq: within or on the boundary; NaN is out.
    const bool within = distance_squared(position, at) <= in_range_limit(c.mem_read8(entry + kEntryRange));
    if (spawn && within) {
        ++stats_.spawns;
        if (listener_ != nullptr) {
            const u32 table = c.mem_read32(c.gpr[3] + kManagerFactories);
            const u32 factory = table != 0u ? c.mem_read32(table + c.mem_read32(c.gpr[4] + kSlotFactoryIndex) * 4u) : 0u;
            listener_->spawn_opened(entry, factory);
        }
    }
    if (leave && !within) ++stats_.leaves;
    c.gpr[3] = within ? 1u : 0u;
}

// (r3 slot) -> r3: 1 when no player is within the range plus the margin.
void NativeObjectRange::out_of_range(Cpu& c) {
    ++stats_.out_of_range_tests;
    const u32 entry = c.mem_read32(c.gpr[3]);
    if ((c.mem_read32(entry + kEntryFlags) & kNeverOutOfRange) != 0u) {
        c.gpr[3] = 0u;
        return;
    }
    // 0x80009548 creates the player list on first use; by the time any
    // object asks, the stage has made it.
    const u32 players = c.mem_read32(c.gpr[13] + kSdaPlayers);
    if (players == 0u) {
        stop(c, "an object asked before the player list exists");
        return;
    }
    const u32 count = c.mem_read32(players + kPlayersCount);
    const u32 measured = c.mem_read8(kFrameObject + kTwoPlayers) != 0u ? 2u : 1u;
    // 0x8000DFD0's bounds check (the original stops in its assert).
    if (count < measured) {
        stop(c, "the player list holds " + std::to_string(count) + " of the " + std::to_string(measured) +
                    " players measured");
        return;
    }
    float position[3];
    read_position(c, entry, position);
    const float radius =
        out_of_range_radius(c.mem_read8(entry + kEntryRange), read_f32(c, c.gpr[13] + kSdaMargin));
    const float limit = radius * radius;
    const u32 data = c.mem_read32(players + kPlayersData);
    bool out = true;
    for (u32 index = 0; index < measured; ++index) {
        float player[3];
        read_position(c, c.mem_read32(data + index * 8u) + kPlayerPosition, player);
        // 0x80169FC8: a player strictly within the radius keeps the object.
        if (distance_squared(player, position) < limit) out = false;
    }
    c.gpr[3] = out ? 1u : 0u;
}

// (r3 slot) -> f1 = fmadds(100.0f, single(range), 100.0f).
void NativeObjectRange::range_radius(Cpu& c) {
    ++stats_.radius_reads;
    const u32 entry = c.mem_read32(c.gpr[3]);
    return_f32(c, distance(c.mem_read8(entry + kEntryRange)) + 100.0f);
}

// (r3 slot, r4 float*): *r4 = 100.0f * single(range).
void NativeObjectRange::range(Cpu& c) {
    ++stats_.radius_reads;
    const u32 entry = c.mem_read32(c.gpr[3]);
    write_f32(c, c.gpr[4], distance(c.mem_read8(entry + kEntryRange)));
}

}  // namespace gekko
