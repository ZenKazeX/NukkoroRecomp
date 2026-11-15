#pragma once

// Binds the reconstructed message system and dialogue voice layer (native_message_system.h, native_dialogue_voice.h) to the machine
// as function-entry owners. See docs/voice_subtitle_timing/11_timing_reconstruction.md.
//
// SHADOW_NATIVE_MESSAGE selects the mode:
//   off / 0          : the translated bodies run (no owner); erratum E4 does nothing, and says so.
//   unset / owner / 1: the native code serves every call (the default). Erratum E4's window lifetime (SHADOW_ERRATA_E4=0 turns it off)
//                      needs the native owner.
//   verify           : the translated body runs for every call, then the native code is replayed over a snapshot of the state
//                      the call started from, with every call into the systems outside its scope answered from what the original
//                      did; the words it wrote and the call sequence are compared (per-call differential, no effect on the run).
//
// SCAFFOLDING (R2): the HLE entries, the guest-memory views and the rest of this file exist because the callers, the task tree and
// CRI are still translated. Deleted with those callers. Nothing here falls back to the translated body: a declined path is a stop.

#include <cstdio>

#include "gekko/types.h"

namespace gekko {
class Cpu;
namespace message_voice_owner {

enum class Mode { Off, Owner, Verify };

// Reads SHADOW_NATIVE_MESSAGE and installs the owners. Call after every other hook is installed so nothing replaces them later;
// the voice trace (which chains) is installed after this.
Mode install(Cpu& cpu);

// The exit report: per-function call counts (and in verify mode the comparisons made).
void print_report(std::FILE* out);

}  // namespace message_voice_owner
}  // namespace gekko
