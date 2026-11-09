#pragma once

// Original-game bugs fixed on purpose - DELIBERATE DEVIATIONS from the original, each chosen by the user and recorded
// in docs/GAME_ERRATA.md, kept apart from the exact-equivalence results (an A/B against the original measures them
// separately: SHADOW_ERRATA=0 restores the original behaviour for that comparison only).
//
// E1 (user 2026-11-02) EnemyBaseAI's state-machine lock byte is never initialised.
//   EnemyBaseAI::EnemyBaseAI (0x8019F1D4; base 0x8019B72C writes +0x0C..+0x18) zeroes its state machine at +0x28
//   (+0x28 owner, +0x2C current state, +0x30 previous state) but not the lock byte at +0x34, then sets the initial
//   state (0x8019F528 = StateMachine::Start, this + 0x28). Start makes no transition while the lock byte is nonzero
//   (it then only clears the byte, at 0x8019F5C8). A nonzero byte - heap memory is not cleared, the byte is whatever
//   an earlier user (a freed thread stack) left - makes that enemy's AI skip its initial state: it starts with no
//   state instead of EnemyAIState_EnemyBaseAI_None (whose enter, 0x801A0000, records the owner), until its first
//   ChangeState (0x8019FD00), which then runs without a None exit. The sibling state machine at +0x38 (init
//   0x8019FC18) zeroes its lock byte, and Start leaves it 0: 0 is the intended initial value. Westopolis replay,
//   original game: 17 of 30 AIs skip it (first two at 22.2 s and 23.5 s: 0x00, 0x21). Fix: the byte is 0 when the constructor starts (no
//   constructor in the chain writes it before Start reads it - checked for the base, EnemyBaseAI and its 10 derived
//   constructors).

//
// E2 (user 2026-11-03) Player::PlayerMotion's counter at +0x3EC is never initialised.
//   ShadowMotion::update sub0 (0x800B4B3C) increments +0x3EC whenever bit 0x18 of the motion's flag set (+0xA8,
//   fn_8006CEA0) is set, and the new value's parity selects ShadowMotion vf13 (0x800B54C8) or vf14 (0x800B541C): a
//   matched pair that sets / clears flag 3 and switches the player model's atomics with RpClumpForAllAtomics
//   (callbacks 0x80039298 / 0x800393A8) - a toggle of the model. The base constructor Player::PlayerMotion
//   (0x800AAE18) memsets +0xD0..+0x3DF and stores +0x3E0, +0x3E8, +0x3F0 but not +0x3EC; ShadowMotion's constructor
//   (0x800B5844) does not write it either; the increment is its only store. The object (0x868 bytes) is allocated over
//   a freed loader thread's stack, so the toggle's phase comes from that stack's leftover bytes (Westopolis:
//   0x810132E0, even) - and a native loader changes them: 9 of 40 stages start in the other phase with the native
//   background read (docs/NATIVE_WORKERS.md 5a, W0b). Fix: +0x3EC is 0 when the base constructor starts (its first
//   write is the increment). +0x3E4, also skipped by that constructor, is not proven to be read uninitialised and is
//   not touched.
//
// E3 (user 2026-11-05) The landscape's texture-pattern animations run one clock per streamed block.
//   Every land block's display layer (TObjLand slot layer, docs: spec_TBlockManage 3.4/3.5) gets its own
//   TTextureChange when its world finishes loading: the RG1 callback 0x80046438 calls 0x8004FBD0, which makes the set
//   (0x8004F6D0 -> constructor 0x8004F4A8 -> 0x8004F284 stores its clock +0x34 = 0) as a child of task group 3, and
//   stores the set's handle {u16, s16} in the slot layer (+0x0C + 12 k). The set's update (0x8004F3C4) adds 60 dt
//   to its own clock; the frame choice (0x8004F0FC) reads that clock (the event clock 0x805E4A60 only while a
//   cutscene sets it >= 0). So a pattern's phase is the time since ITS BLOCK finished streaming in: GUN Fortress's
//   corridor lights (s0600_zn055, period 64) blink out of step from block to block, by however long apart the blocks
//   loaded - an artefact of the console's block streaming, not of the authored pulse. Fix: when a land layer's set
//   has just been made (0x80038148, whose only caller is that callback, right after the handle was stored), every
//   live land set - a TTextureChange in group 3 whose handle a loaded TObjLand slot holds - takes the clock of the
//   oldest one (group 3 appends children at its tail, 0x8004F014). The blocks there since the stage started keep
//   their original clock; every later block joins it. Sets of objects (0x8004FA40) are untouched. Read from guest
//   memory only, so a savestate needs nothing extra.

#include "gekko/types.h"

namespace gekko {

class Cpu;

namespace errata {

constexpr u32 kEnemyBaseAiConstructor = 0x8019F1D4u;
constexpr u32 kEnemyBaseAiLockOffset = 0x34u;  // the state machine at +0x28, its lock byte at +0xC

constexpr u32 kPlayerMotionConstructor = 0x800AAE18u;
constexpr u32 kPlayerMotionToggleOffset = 0x3ECu;  // ShadowMotion::update sub0's counter

// E3.
constexpr u32 kLandLayerReady = 0x80038148u;      // (world): called only by the RG1 callback 0x80046438
constexpr u32 kTaskGroups = 0x80571C6Cu;          // the task manager (getter 0x8001C4F8); group k = [+4 + 4 k]
constexpr u32 kPatternGroup = 3u;                 // 0x8004F6D0's parent for a set made without one (0x801EB614)
constexpr u32 kTaskNext = 0x0Cu;                  // next sibling, 0-terminated (0x8004F014)
constexpr u32 kTaskFirstChild = 0x14u;
constexpr u32 kTaskVtable = 0x18u;
constexpr u32 kTextureChangeVtable = 0x8051E78Cu;
constexpr u32 kTextureChangeHandle = 0x2Cu;       // {u16 id, s16 gen}: its +0x28 task part's +4 (0x80039EFC)
constexpr u32 kTextureChangeClock = 0x34u;        // f32 frames: 0 at 0x8004F284, += 60 dt at 0x8004F3C4
constexpr u32 kObjLand = 0x80572560u;             // TObjLand (getter 0x8003A868)
constexpr u32 kObjLandSlots = 32u;
constexpr u32 kObjLandSlotSize = 0xC0u;           // +0 state (0 free)
constexpr u32 kObjLandLayers = 15u;
constexpr u32 kObjLandLayerHandle = 0x0Cu;        // + 12 k: the layer's set handle (0x80046438)
constexpr u32 kObjLandLayerStride = 0x0Cu;

struct Stats {
    u64 ai_constructed = 0;   // EnemyBaseAI constructors entered
    u64 ai_lock_cleared = 0;  // ... whose lock byte held garbage (nonzero) and was cleared
    u64 motion_constructed = 0;      // Player::PlayerMotion constructors entered (E2)
    u64 motion_counter_cleared = 0;  // ... whose +0x3EC held garbage (nonzero) and was cleared
    u64 land_layers_ready = 0;       // land layers loaded (0x80038148 entered, E3)
    u64 land_pattern_sets = 0;       // ... live land pattern sets seen there, summed
    u64 land_clocks_aligned = 0;     // ... whose clock differed from the oldest set's and was set to it
};

// The fix's effect at the constructor's entry (r3 = this): the lock byte becomes 0. Declines (false): the
// constructor's own body runs.
bool enemy_base_ai_constructor_entry(Cpu& c, Stats& stats);

// E2 at Player::PlayerMotion's constructor entry (r3 = this): +0x3EC becomes 0. Declines.
bool player_motion_constructor_entry(Cpu& c, Stats& stats);

// E3 at 0x80038148's entry: every live land pattern set's clock becomes the oldest one's. Declines.
bool land_layer_ready_entry(Cpu& c, Stats& stats);

// Installs every fix unless SHADOW_ERRATA=0 (an A/B against the original only). Returns whether they are installed.
bool install(Cpu& cpu, Stats& stats);

}  // namespace errata
}  // namespace gekko
