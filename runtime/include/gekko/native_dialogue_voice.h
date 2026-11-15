#pragma once

// The dialogue voice layer of the game (the two TObjSERIF players), reconstructed from the PAL main.dol.
// Meaning, addresses and evidence: docs/voice_subtitle_timing/11_timing_reconstruction.md.
//
// A TObjSERIF plays one PRS_VOICE clip through its own ADXT handle. There are two of them. A voice has no timer of its
// own: it ends when the ADXT reports stop / play end, or when somebody stops it. The stop, pause and start primitives
// live here; everything from the ADXT handle down stays where it is.
//
// The logic is pure: it reaches the machine only through Env, so the test drives it on a fake guest and the differential
// check drives it against the original body. The objects themselves still live in game memory because the task tree
// and the callers that hold them are still translated; that is what the guest addresses below name.

#include "gekko/types.h"

namespace gekko::dialogue_voice {

// A path this reconstruction does not cover. It stops the run (the binding turns it into an ExecutionFault); it never
// falls back to the translated body.
struct UnportedPath {
    const char* what;
};

// Small data of the sound system (r13 = 0x805EC500).
constexpr u32 kBgmObjects = 0x805E9BC0u;       // TObjBGM*[2], r13 - 0x2940 (out of scope: pause / resume only touch +0x29, +0x2B, +0x44)
constexpr u32 kBgmWork = 0x805E9BC8u;          // the ADXT work areas of the two BGM objects (0x389E4 bytes each)
constexpr u32 kSerifSlots = 0x805E9BD0u;       // TObjSERIF*[2], r13 - 0x2930
constexpr u32 kSerifWork = 0x805E9BD8u;        // their ADXT work areas (0x17124 bytes each)
constexpr u32 kLoadedVoiceLanguage = 0x805E9BE0u;  // the voice archive language that is loaded; 6 = none yet
constexpr u32 kSerifTaskName = 0x805E9BF0u;    // r13 - 0x2910: the word the TObjSERIF constructor stores in Task +0x00
constexpr u32 kSerifCount = 2u;

// TObjSERIF (0x48 bytes, a Task). Vtable 0x8053DFDC: +8 dtor 0x801D6EA4, +0xC update 0x801D6E14.
constexpr u32 kSerifVtable = 0x8053DFDCu;
constexpr u32 kSerifWorkSize = 0x17124u;
constexpr u32 kBgmWorkSize = 0x389E4u;
constexpr u32 kTaskName = 0x00u;       // Task: word, what the object is called
constexpr u32 kTaskFlags = 0x04u;      // Task: u16. bit 0 = kill request, 0x100 = updated while the game is paused
constexpr u32 kTaskVtable = 0x18u;     // Task: the vtable pointer
constexpr u32 kSlotIndex = 0x28u;      // s8: 0 or 1
constexpr u32 kSlotState = 0x29u;      // s8: 0 idle, 1 playing
constexpr u32 kSlotPriority = 0x2Au;   // s8: 3 = never replaced; <= 0 = anyone may replace it; else a strictly higher one may
constexpr u32 kSlotPauseFlags = 0x2Bu; // u8: bit 0 = paused
constexpr u32 kSlotVoice = 0x2Cu;      // AFS index of the clip, -1 none
constexpr u32 kSlotKind = 0x30u;       // the caller's "kind" of voice (same-kind replacement), -1 none
constexpr u32 kSlotStatus = 0x34u;     // the last ADXT status seen by the update
constexpr u32 kSlotVolume = 0x38u;     // out volume, -0x28 after a reset
constexpr u32 kSlotAdxt = 0x3Cu;       // the ADXT handle
constexpr u32 kSlotAdxtWork = 0x40u;   // its work area
constexpr u32 kSlotExtra = 0x44u;      // the constructor's last argument, stored and never read here (0 at the one call site)

// TObjBGM fields that the shared pause / resume loops touch (nothing else of TObjBGM is reconstructed).
constexpr u32 kBgmState = 0x29u;
constexpr u32 kBgmPauseFlags = 0x2Bu;
constexpr u32 kBgmAdxt = 0x44u;

constexpr s8 kPriorityNeverReplaced = 3;
constexpr u32 kSerifResetVolume = 0xFFFFFFD8u;  // -0x28

// ADXT status (CRI convention): 0 stop, 1 decode info, 2 prepare, 3 playing, 4 decode end, 5 play end, 6 error.
constexpr s32 kAdxtStop = 0;
constexpr s32 kAdxtPlayEnd = 5;

// The machine as this code sees it: game memory, and the systems outside its scope.
class Env {
public:
    virtual ~Env() = default;

    virtual u8 r8(u32 address) = 0;
    virtual u16 r16(u32 address) = 0;
    virtual u32 r32(u32 address) = 0;
    virtual void w8(u32 address, u8 value) = 0;
    virtual void w16(u32 address, u16 value) = 0;
    virtual void w32(u32 address, u32 value) = 0;

    // CRI ADXT, unchanged.
    virtual u32 adxt_create(s32 max_channels, u32 work, u32 work_size) = 0;    // 0x803C3D34
    virtual void adxt_stop(u32 handle, u32 link) = 0;                          // 0x803C2F90 (link: the return address in the original)
    virtual void adxt_pause(u32 handle, u32 paused) = 0;                       // 0x803C21C4
    virtual void adxt_start_afs(u32 handle, u32 partition, u32 file) = 0;      // 0x803C3E40
    virtual s32 adxt_get_stat(u32 handle) = 0;                                 // 0x803C2F38 (-1 for a null handle)
    virtual void adxt_set_out_vol(u32 handle, s32 volume) = 0;                 // 0x803C25BC
    // The CRI library setup at the head of the work-area allocation; not reconstructed, called in the same order.
    virtual void cri_setup_803C5750(u32 argument) = 0;
    virtual void cri_setup_803C5118(u32 argument) = 0;
    virtual void cri_setup_803B72D8() = 0;

    // The task tree and the sound heap, unchanged.
    virtual void task_base_ctor(u32 task, u32 parent) = 0;  // Task::Task 0x8004F014
    virtual u32 sound_heap_alloc(u32 size, u32 tag) = 0;    // 0x80049BB8

    // The game's own call nesting, as far as the tools that read return addresses can see it (the voice trace, the Show
    // subtitles switch): each nested call of the original records its return address around the native code. No-ops when
    // nobody looks (see native_message_voice_owner.cpp).
    virtual void enter_call(u32 return_address) { (void)return_address; }
    virtual void leave_call() {}
};

class NestedCall {
public:
    NestedCall(Env& env, u32 return_address) : env_(env) { env_.enter_call(return_address); }
    ~NestedCall() { env_.leave_call(); }
    NestedCall(const NestedCall&) = delete;
    NestedCall& operator=(const NestedCall&) = delete;

private:
    Env& env_;
};

// fn_801D7C64: which slot would serve (kind, priority), -1 when none may be replaced.
s32 choose_slot(Env& env, s32 kind, s32 priority);

// fn_801D7DF0: a clip on the chosen slot (stopping what it played). Returns the slot or -1. `priority` is a sign-extended s8.
s32 play(Env& env, s32 voice, s32 priority, s32 kind);
// fn_801D5540: the public entry (the manager argument is unused by the original; the priority comes as the low byte of a word).
s32 request_play(Env& env, s32 voice, u32 priority_word, s32 kind);

// fn_801D7BD0: stops every slot whose +0x2C equals `voice`, including -1: an idle slot's id is -1, so a stop of -1 hits it.
void stop_by_id(Env& env, s32 voice);
// fn_801D551C: its public entry.
void request_stop(Env& env, s32 voice);
// fn_801D7B4C, and fn_801D54FC, its public entry.
void stop_all(Env& env);
void request_stop_all(Env& env);

// fn_801D8058 / fn_801D55AC and fn_801D7F80 / fn_801D558C. Both also walk the BGM objects (the two loops share these functions).
void pause_all(Env& env);
void resume_all(Env& env);

// fn_801D6F60: stop, pause cleared, fields cleared, volume -0x28.
void serif_reset(Env& env, u32 serif);
// fn_801D6FEC: TObjSERIF::TObjSERIF(parent, index, work, extra). Returns `serif`.
u32 serif_ctor(Env& env, u32 serif, u32 parent, s32 index, u32 work, u32 extra);
// fn_801D6E14: the per-frame status poll.
void serif_update(Env& env, u32 serif);

// fn_801D874C: the CRI setup and the four ADXT work areas (this is not where the TObjSERIF objects are made: fn_801D844C does that).
void allocate_work_areas(Env& env);

}  // namespace gekko::dialogue_voice
