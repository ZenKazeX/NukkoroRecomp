// Original-game bugs fixed on purpose. Contract and evidence: include/gekko/game_errata.h, docs/GAME_ERRATA.md.

#include "gekko/game_errata.h"

#include <cstdlib>

#include "gekko/cpu.h"

namespace gekko::errata {

bool enemy_base_ai_constructor_entry(Cpu& c, Stats& stats) {
    const u32 lock = c.gpr[3] + kEnemyBaseAiLockOffset;
    ++stats.ai_constructed;
    if (c.mem_read8(lock) != 0u) {
        ++stats.ai_lock_cleared;
        c.mem_write8(lock, 0u);
    }
    return false;
}

bool player_motion_constructor_entry(Cpu& c, Stats& stats) {
    const u32 counter = c.gpr[3] + kPlayerMotionToggleOffset;
    ++stats.motion_constructed;
    if (c.mem_read32(counter) != 0u) {
        ++stats.motion_counter_cleared;
        c.mem_write32(counter, 0u);
    }
    return false;
}

namespace {

bool guest_object(u32 address) noexcept {
    return address >= 0x80000000u && address < 0x81800000u && (address & 3u) == 0u;
}

}  // namespace

bool land_layer_ready_entry(Cpu& c, Stats& stats) {
    ++stats.land_layers_ready;
    // The handles the loaded blocks' layers hold (0 = no world, {0, -1} = 0x0000FFFF = no set: 0x8004F988).
    u32 handles[kObjLandSlots * kObjLandLayers];
    u32 count = 0;
    for (u32 slot = 0; slot < kObjLandSlots; ++slot) {
        const u32 base = kObjLand + slot * kObjLandSlotSize;
        if (c.mem_read32(base) == 0u) continue;  // free
        for (u32 layer = 0; layer < kObjLandLayers; ++layer) {
            const u32 handle = c.mem_read32(base + kObjLandLayerHandle + layer * kObjLandLayerStride);
            if (handle != 0u && handle != 0x0000FFFFu) handles[count++] = handle;
        }
    }
    const u32 group = c.mem_read32(kTaskGroups + 4u + 4u * kPatternGroup);
    if (count == 0u || !guest_object(group)) return false;
    // Group 3's children in creation order; the first land set is the oldest.
    u32 clock = 0;
    bool first = true;
    u32 task = c.mem_read32(group + kTaskFirstChild);
    for (u32 guard = 0; guest_object(task) && guard < 0x10000u; ++guard, task = c.mem_read32(task + kTaskNext)) {
        if (c.mem_read32(task + kTaskVtable) != kTextureChangeVtable) continue;
        const u32 handle = c.mem_read32(task + kTextureChangeHandle);
        bool land = false;
        for (u32 i = 0; i < count && !land; ++i) land = handles[i] == handle;
        if (!land) continue;
        ++stats.land_pattern_sets;
        if (first) {
            clock = c.mem_read32(task + kTextureChangeClock);
            first = false;
        } else if (c.mem_read32(task + kTextureChangeClock) != clock) {
            c.mem_write32(task + kTextureChangeClock, clock);
            ++stats.land_clocks_aligned;
        }
    }
    return false;
}

bool install(Cpu& cpu, Stats& stats) {
    const char* env = std::getenv("SHADOW_ERRATA");
    if (env != nullptr && env[0] == '0') return false;
    // SHADOW_ERRATA_E3=0 leaves E3 out (an A/B of E3 alone).
    const char* e3 = std::getenv("SHADOW_ERRATA_E3");
    if (e3 == nullptr || e3[0] != '0')
        cpu.install_hle(
            kLandLayerReady, [&stats](Cpu& c) { return land_layer_ready_entry(c, stats); },
            "errata E3: land pattern sets take the oldest land set's clock (declines)");
    cpu.install_hle(
        kEnemyBaseAiConstructor, [&stats](Cpu& c) { return enemy_base_ai_constructor_entry(c, stats); },
        "errata E1: EnemyBaseAI state-machine lock byte initialised to 0 (declines)");
    cpu.install_hle(
        kPlayerMotionConstructor, [&stats](Cpu& c) { return player_motion_constructor_entry(c, stats); },
        "errata E2: PlayerMotion toggle counter +0x3EC initialised to 0 (declines)");
    return true;
}

}  // namespace gekko::errata
