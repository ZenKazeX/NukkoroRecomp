// The dialogue voice layer: see include/gekko/native_dialogue_voice.h and docs/voice_subtitle_timing/11_timing_reconstruction.md.

#include "gekko/native_dialogue_voice.h"

namespace gekko::dialogue_voice {

namespace {

constexpr u32 kNone = 0xFFFFFFFFu;

u32 slot_address(Env& env, u32 index) { return env.r32(kSerifSlots + index * 4u); }

s32 sext8(u8 value) noexcept { return static_cast<s32>(static_cast<s8>(value)); }

// The replacement rule, three times inline in the original. `old_priority < new_priority` is the compiler's
// ((a ^ b) >> 1) - ((a ^ b) & b) sign-bit form; the test proves the two equal for every s8 pair.
bool can_replace(s32 old_priority, s32 new_priority) noexcept {
    if (old_priority == kPriorityNeverReplaced) return false;
    if (old_priority <= 0) return true;
    return old_priority < new_priority;
}

// The field clear every stop path does, in store order.
void clear_playing(Env& env, u32 serif) {
    env.w32(serif + kSlotVoice, kNone);
    env.w32(serif + kSlotKind, kNone);
    env.w8(serif + kSlotPriority, 0xFFu);
    env.w8(serif + kSlotState, 0u);
    env.w8(serif + kSlotPauseFlags, static_cast<u8>(env.r8(serif + kSlotPauseFlags) & 0xFEu));
}

void stop_stream(Env& env, u32 serif, u32 link) {
    env.adxt_stop(env.r32(serif + kSlotAdxt), link);
    env.adxt_pause(env.r32(serif + kSlotAdxt), 0u);
}

}  // namespace

s32 choose_slot(Env& env, s32 kind, s32 priority) {
    if (kind >= 0) {
        for (u32 i = 0; i < kSerifCount; ++i) {
            const u32 serif = slot_address(env, i);
            if (serif == 0u) continue;
            if (static_cast<s32>(env.r32(serif + kSlotKind)) != kind) continue;
            // The first slot of the same kind decides, whatever the answer.
            return can_replace(sext8(env.r8(serif + kSlotPriority)), priority) ? static_cast<s32>(i) : -1;
        }
    }
    s32 best = -1;
    for (u32 i = 0; i < kSerifCount; ++i) {
        const u32 serif = slot_address(env, i);
        if (serif == 0u) continue;
        // Byte compared unsigned against 1; the first slot that is not playing is taken at once.
        if (env.r8(serif + kSlotState) != 1u) return static_cast<s32>(i);
        const s32 old_priority = sext8(env.r8(serif + kSlotPriority));
        if (!can_replace(old_priority, priority)) continue;
        if (best < 0) {
            best = static_cast<s32>(i);
            continue;
        }
        const u32 best_serif = env.r32(kSerifSlots + static_cast<u32>(best) * 4u);
        if (can_replace(old_priority, sext8(env.r8(best_serif + kSlotPriority)))) best = static_cast<s32>(i);
    }
    return best;
}

s32 play(Env& env, s32 voice, s32 priority, s32 kind) {
    const s32 slot = choose_slot(env, kind, static_cast<s32>(static_cast<s8>(priority)));
    if (slot < 0) return -1;
    const u32 serif = slot_address(env, static_cast<u32>(slot));
    stop_stream(env, serif, 0x801D7E34u);  // liveness-roots: value-only
    clear_playing(env, serif);
    env.w32(serif + kSlotVoice, static_cast<u32>(voice));
    env.w8(serif + kSlotPriority, static_cast<u8>(priority));
    env.w32(serif + kSlotKind, static_cast<u32>(kind));
    env.adxt_start_afs(env.r32(serif + kSlotAdxt), 0u, env.r32(serif + kSlotVoice));
    env.w8(serif + kSlotState, 1u);
    return slot;
}

s32 request_play(Env& env, s32 voice, u32 priority_word, s32 kind) {
    const NestedCall nested(env, 0x801D555Cu);  // liveness-roots: value-only
    return play(env, voice, static_cast<s32>(static_cast<s8>(static_cast<u8>(priority_word))), kind);
}

void stop_by_id(Env& env, s32 voice) {
    for (u32 i = 0; i < kSerifCount; ++i) {
        const u32 serif = slot_address(env, i);
        if (serif == 0u) continue;
        if (static_cast<s32>(env.r32(serif + kSlotVoice)) != voice) continue;
        stop_stream(env, serif, 0x801D7C10u);  // liveness-roots: value-only
        clear_playing(env, serif);
    }
}

void request_stop(Env& env, s32 voice) {
    const NestedCall nested(env, 0x801D5530u);  // liveness-roots: value-only
    stop_by_id(env, voice);
}

void stop_all(Env& env) {
    for (u32 i = 0; i < kSerifCount; ++i) {
        const u32 serif = slot_address(env, i);
        if (serif == 0u) continue;
        stop_stream(env, serif, 0x801D7B84u);  // liveness-roots: value-only
        clear_playing(env, serif);
    }
}

void request_stop_all(Env& env) {
    const NestedCall nested(env, 0x801D550Cu);  // liveness-roots: value-only
    stop_all(env);
}

void pause_all(Env& env) {
    for (u32 i = 0; i < 2u; ++i) {
        const u32 bgm = env.r32(kBgmObjects + i * 4u);
        if (bgm == 0u) continue;
        const s32 state = sext8(env.r8(bgm + kBgmState));
        if (state < 1 || state >= 7) continue;
        if (state != 1) env.adxt_pause(env.r32(bgm + kBgmAdxt), 1u);
        env.w8(bgm + kBgmPauseFlags, static_cast<u8>(env.r8(bgm + kBgmPauseFlags) | 1u));
    }
    for (u32 i = 0; i < kSerifCount; ++i) {
        const u32 serif = slot_address(env, i);
        if (serif == 0u) continue;
        if (env.r8(serif + kSlotState) != 1u) continue;
        env.adxt_pause(env.r32(serif + kSlotAdxt), 1u);
        env.w8(serif + kSlotPauseFlags, static_cast<u8>(env.r8(serif + kSlotPauseFlags) | 1u));
    }
}

void resume_all(Env& env) {
    for (u32 i = 0; i < 2u; ++i) {
        const u32 bgm = env.r32(kBgmObjects + i * 4u);
        if (bgm == 0u) continue;
        const s32 state = sext8(env.r8(bgm + kBgmState));
        if (state >= 2 && state < 7) env.adxt_pause(env.r32(bgm + kBgmAdxt), 0u);
        env.w8(bgm + kBgmPauseFlags, static_cast<u8>(env.r8(bgm + kBgmPauseFlags) & 0xFEu));
    }
    for (u32 i = 0; i < kSerifCount; ++i) {
        const u32 serif = slot_address(env, i);
        if (serif == 0u) continue;
        if (env.r8(serif + kSlotState) != 1u) continue;
        env.adxt_pause(env.r32(serif + kSlotAdxt), 0u);
        env.w8(serif + kSlotPauseFlags, static_cast<u8>(env.r8(serif + kSlotPauseFlags) & 0xFEu));
    }
}

void serif_reset(Env& env, u32 serif) {
    stop_stream(env, serif, 0x801D6F7Cu);  // liveness-roots: value-only
    clear_playing(env, serif);
    // The original then stores the same fields again and resets the status, the volume and the flags.
    env.w8(serif + kSlotState, 0u);
    env.w8(serif + kSlotPriority, 0xFFu);
    env.w32(serif + kSlotVoice, kNone);
    env.w32(serif + kSlotKind, kNone);
    env.w32(serif + kSlotStatus, kNone);
    env.w32(serif + kSlotVolume, kSerifResetVolume);
    env.w8(serif + kSlotPauseFlags, 0u);
    env.adxt_set_out_vol(env.r32(serif + kSlotAdxt), static_cast<s32>(env.r32(serif + kSlotVolume)));
}

u32 serif_ctor(Env& env, u32 serif, u32 parent, s32 index, u32 work, u32 extra) {
    env.task_base_ctor(serif, parent);
    env.w32(serif + kTaskVtable, kSerifVtable);
    env.w32(serif + kTaskName, env.r32(kSerifTaskName));
    env.w16(serif + kTaskFlags, static_cast<u16>(env.r16(serif + kTaskFlags) | 0x100u));
    env.w32(serif + kSlotAdxtWork, work);
    env.w32(serif + kSlotExtra, extra);
    const u32 handle = env.adxt_create(1, env.r32(serif + kSlotAdxtWork), kSerifWorkSize);
    env.w32(serif + kSlotAdxt, handle);
    if (env.r32(serif + kSlotAdxt) == 0u) {
        env.w16(serif + kTaskFlags, static_cast<u16>(env.r16(serif + kTaskFlags) | 1u));
        return serif;
    }
    env.w8(serif + kSlotIndex, static_cast<u8>(index));
    const NestedCall nested(env, 0x801D707Cu);  // liveness-roots: value-only
    serif_reset(env, serif);
    return serif;
}

void serif_update(Env& env, u32 serif) {
    const s32 state = sext8(env.r8(serif + kSlotState));
    if (state == 1) {
        env.w32(serif + kSlotStatus, static_cast<u32>(env.adxt_get_stat(env.r32(serif + kSlotAdxt))));
        const s32 status = static_cast<s32>(env.r32(serif + kSlotStatus));
        if (status == kAdxtStop || status == kAdxtPlayEnd) {
            env.w32(serif + kSlotVoice, kNone);
            env.w32(serif + kSlotKind, kNone);
            env.w8(serif + kSlotPriority, 0xFFu);
            env.w8(serif + kSlotState, 0u);
        }
    } else if (state == 0) {
        env.w32(serif + kSlotStatus, static_cast<u32>(env.adxt_get_stat(env.r32(serif + kSlotAdxt))));
    }
}

void allocate_work_areas(Env& env) {
    env.cri_setup_803C5750(0u);
    env.cri_setup_803C5118(0u);
    env.cri_setup_803B72D8();
    for (u32 i = 0; i < 2u; ++i) env.w32(kBgmWork + i * 4u, env.sound_heap_alloc(kBgmWorkSize, 2u));
    for (u32 i = 0; i < 2u; ++i) env.w32(kSerifWork + i * 4u, env.sound_heap_alloc(kSerifWorkSize, 2u));
    env.w32(kLoadedVoiceLanguage, 6u);
}

}  // namespace gekko::dialogue_voice
