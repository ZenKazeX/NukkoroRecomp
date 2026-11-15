#pragma once

// Dialogue voice + subtitle window trace (docs/voice_subtitle_timing/08_runtime_baseline.md).
//
// Off unless GEKKO_VOICE_TRACE=<file> is set; with it unset nothing is installed and the game runs unchanged. Every
// hook only reads guest memory and always declines (returns false), so the guest body still runs and no guest state is
// written. Not compiled into the user's exe under GEKKO_SHIPPING.
//
// One tab-separated line per event: wall ms since the first event, game ticks (40.5 MHz game clock), game frame counter
// (0x8057E7F0), event name, then key=value fields. Events:
//   msg_enqueue    a subtitle/radio line is queued (fn_801FB210: group, number, variant, caller)
//   msg_pop        a queue entry is consumed or dropped (fn_801FBA18; reason became_current / dropped_lower_priority;
//                  the per-frame discard of queued variants that have no text is not logged)
//   win_open       the message manager's current message id (mgr+0x14) became valid or changed (polled each MessageTask
//                  update, fn_801FB5AC); fields give the message data (duration frames, voice id, priority)
//   win_close      the current message id left (it was set to -1 by the timer, by an empty queue, or by a replacement)
//   voice_request  TObjSERIF play request (fn_801D5540): voice id (index into PRS_VOICE_<lang>.afs), caller
//   voice_stop_req TObjSERIF stop-by-id request (fn_801D551C): voice id, caller
//   adx_start_afs  CRI ADXT_StartAfs (fn_803C3E40): handle, partition, file index (the dialogue voice file)
//   adx_start_name CRI ADXT_StartFname (fn_803C3D94): handle, file name (cutscene and music streams)
//   adx_stop       CRI ADXT_Stop (fn_803C2F90): handle, status before, 3 callers up the stack chain
//   adx_stat       an ADXT handle's status byte changed (0 stop, 1 decode info, 2 prepare, 3 playing, 4 decode end,
//                  5 play end), seen from the polls (every MessageTask update and every voice slot update)
//   voice_slot_end the voice slot update found its stream finished by itself (status 0 or 5) and frees the slot
#include "gekko/types.h"

namespace gekko {
class Cpu;
namespace voice_trace {
#if defined(GEKKO_SHIPPING)
inline void install(Cpu&) noexcept {}
inline bool enabled() noexcept { return false; }
inline void note(Cpu&, const char*, const char*) noexcept {}
inline void note_voice_stop(Cpu&, s32, u32, u32) noexcept {}
inline void note_voice_request(Cpu&, s32, s32, s32, u32, u32) noexcept {}
inline void note_queue_pop(Cpu&, u32, u32) noexcept {}
#else
// Installs the hooks when GEKKO_VOICE_TRACE is set; call after every other hook is installed (it chains to any earlier
// handler of the same address).
void install(Cpu& cpu);
bool enabled() noexcept;
// The same lines the function-entry observers would have written for a call the native code makes without going through the entry.
void note_voice_stop(Cpu& cpu, s32 voice, u32 caller, u32 caller2);
void note_voice_request(Cpu& cpu, s32 voice, s32 flag, s32 tag, u32 caller, u32 caller2);
void note_queue_pop(Cpu& cpu, u32 entry, u32 link);
// One extra trace line from another module (the E4 hold: hold_start / hold_end / hold_request / hold_release ...).
// A no-op unless the trace is on; never touches the guest.
void note(Cpu& cpu, const char* event, const char* text);
#endif
}  // namespace voice_trace
}  // namespace gekko
