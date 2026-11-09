#pragma once

// A replay's gameplay checkpoint (gekko/input_tape.h `check` records, docs/tools/RECORDER.md): five hashes over guest
// state that is the game's own play and nothing of the display - so a replay drawn at any resolution, aspect, render
// scale, MSAA or filtering must reproduce every one of them, and the first group that differs names what diverged.
//
//   stage    the running stage, its state, the game frame counter, the mission timer, the pause flag
//   rng      the MT19937 state (the random stream: every random choice the game made)
//   player   the P1 game record (rings, scores) and the player object's position / rotation / velocity
//   mission  the mission manager, the hero / dark gauges, the checkpoint reached, the enemies defeated
//   camera   the game camera's frame (right / up / at / eye) - its aim, not its field of view (which the widescreen
//            picture may widen)
//
// Addresses: tools/comparator/comparator/probes.py (the PAL build). Each group is FNV-1a 32 over its words in order; an
// unreadable pointer path contributes a fixed marker, so a missing player hashes the same in both runs.

#include <cstdint>
#include <cstdio>
#include <string>

namespace gekko::replay_check {

inline std::uint32_t fnv_word(std::uint32_t h, std::uint32_t w) noexcept {
    for (int i = 0; i < 4; ++i) {
        h ^= (w >> (8 * i)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

inline bool guest_pointer(std::uint32_t p) noexcept { return p >= 0x80000000u && p < 0x81800000u && (p & 3u) == 0u; }

// `read32(address)`: a guest word.
template <typename Read32>
std::string compute(Read32&& read32) {
    const auto range = [&](std::uint32_t h, std::uint32_t base, std::uint32_t bytes) {
        for (std::uint32_t o = 0; o < bytes; o += 4u) h = fnv_word(h, read32(base + o));
        return h;
    };
    // A pointer path: read32 at each step plus its offset; 0 when a step is not a guest pointer.
    const auto follow = [&](std::uint32_t base, std::initializer_list<std::uint32_t> offsets) -> std::uint32_t {
        std::uint32_t p = base;
        for (const std::uint32_t off : offsets) {
            const std::uint32_t next = read32(p);
            if (!guest_pointer(next)) return 0u;
            p = next + off;
        }
        return p;
    };
    constexpr std::uint32_t kBasis = 2166136261u;
    std::uint32_t stage = kBasis;
    stage = range(stage, 0x8057E808u, 8u);    // stage number, table index
    stage = range(stage, 0x80577040u, 4u);    // StageState +0x8
    stage = range(stage, 0x8057E7F0u, 4u);    // game frame counter
    stage = range(stage, 0x8057E800u, 4u);    // mission timer
    stage = range(stage, 0x805EF27Cu, 4u);    // pause flag (its word)
    std::uint32_t rng = range(kBasis, 0x805E1894u, 2504u);
    std::uint32_t player = range(kBasis, 0x805777A4u, 0x4Cu);  // the P1 record
    const std::uint32_t shadow = follow(0x80571CA4u, {0x14u, 0x14u, 0xCu, 0x28u, 0x0u});
    player = shadow != 0u ? range(player, shadow, 0x24u) : fnv_word(player, 0xDEADu);  // position, rotation, velocity
    std::uint32_t mission = range(kBasis, 0x80576FBCu, 0x40u);
    mission = range(mission, 0x80577788u, 24u);  // hero, dark gauges
    mission = range(mission, 0x8057707Cu, 4u);    // checkpoint reached
    mission = range(mission, 0x80580C0Cu, 16u);   // enemies defeated (3 teams + by the player)
    std::uint32_t camera = kBasis;
    const std::uint32_t frame = follow(0x8056FF1Cu, {0x4u, 0x0u});
    camera = frame != 0u ? range(camera, frame + 0x10u, 0x40u) : fnv_word(camera, 0xDEADu);  // right, up, at, eye rows
    char text[128];
    std::snprintf(text, sizeof(text), "stage=%08x rng=%08x player=%08x mission=%08x camera=%08x", stage, rng, player,
                  mission, camera);
    return text;
}

// The groups whose hashes differ between a recorded and a replayed checkpoint ("" when equal).
inline std::string differing_groups(const std::string& recorded, const std::string& replayed) {
    std::string out;
    std::size_t a = 0, b = 0;
    while (a < recorded.size() && b < replayed.size()) {
        const std::size_t ea = recorded.find(' ', a), eb = replayed.find(' ', b);
        const std::string fa = recorded.substr(a, ea == std::string::npos ? std::string::npos : ea - a);
        const std::string fb = replayed.substr(b, eb == std::string::npos ? std::string::npos : eb - b);
        if (fa != fb) {
            const std::size_t eq = fa.find('=');
            out += (out.empty() ? "" : ",") + fa.substr(0, eq);
        }
        if (ea == std::string::npos || eb == std::string::npos) break;
        a = ea + 1;
        b = eb + 1;
    }
    return out;
}

}  // namespace gekko::replay_check
