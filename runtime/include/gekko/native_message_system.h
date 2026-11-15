#pragma once

// The game's message system (subtitle windows and the voice lines they carry), reconstructed from the PAL main.dol.
// Meaning, addresses and evidence: docs/voice_subtitle_timing/11_timing_reconstruction.md.
//
// One manager singleton holds a queue of lines and the current one; one Task, the MessageTask, shows the current line
// and plays its voice. A line's window length is a field of its .fnt record, in frames: the task accumulates the frame
// step in seconds and closes the window at frames / 60. Closing the window touches no audio.
//
// The logic is pure: it reaches the machine only through Env (dialogue_voice::Env plus what is below), so the test drives
// it on a fake guest and the differential check drives it against the original body. The objects still live in game memory
// because the producers, the task tree and the font loader are still translated.

#include <string>

#include "gekko/native_dialogue_voice.h"
#include "gekko/types.h"

namespace gekko::message_system {

using dialogue_voice::UnportedPath;

// The singleton and its statics (r13 = 0x805EC500).
constexpr u32 kManager = 0x80580B18u;
constexpr u32 kManagerPointer = 0x805EFC40u;     // r13 + 0x3740
constexpr u32 kManagerGuard = 0x805EFC3Au;       // r13 + 0x373A
constexpr u32 kManagerDtor = 0x8010A1B8u;
constexpr u32 kManagerDtorBlock = 0x80580AFCu;
constexpr u32 kSoundManagerPointer = 0x805EF0B8u;  // r13 + 0x2BB8

// The message manager (0x34 bytes).
constexpr u32 kMgrQueueCapacity = 0x00u;   // std::vector<QueueEntry> of 12-byte entries: capacity, count, data
constexpr u32 kMgrQueueCount = 0x04u;
constexpr u32 kMgrQueueData = 0x08u;
constexpr u32 kMgrFont = 0x0Cu;            // boost::weak_ptr<Font>: pointer, shared count
constexpr u32 kMgrFontCount = 0x10u;
constexpr u32 kMgrCurrent = 0x14u;         // the current line's group, -1 = no window
constexpr u32 kMgrCurrentNumber = 0x18u;   // its number
constexpr u32 kMgrCurrentVariant = 0x1Cu;  // its variant
constexpr u32 kMgrVoice = 0x20u;           // the voice the system last started; never cleared by a reset
constexpr u32 kMgrLock = 0x24u;            // counter: enqueues refuse while it is nonzero
constexpr u32 kMgrPriority = 0x28u;        // the current line's priority, -1 when idle
constexpr u32 kMgrDrawEnable = 0x2Cu;      // u8: draw the text (else only the record lookup)
constexpr u32 kMgrAltLayout = 0x2Du;       // u8: selects the third layout row
constexpr u32 kMgrTask = 0x30u;            // the MessageTask

constexpr u32 kQueueEntrySize = 12u;
constexpr u32 kQueueLineLimit = 0x24u;     // an enqueue queues variants first .. 35

// The MessageTask (0x40 bytes, a Task). Vtable 0x805437D4: +8 dtor 0x801FB938, +0xC update 0x801FB5AC.
constexpr u32 kTaskSize = 0x40u;
constexpr u32 kMessageTaskVtable = 0x805437D4u;
constexpr u32 kTaskNameText = 0x804ECE58u;  // "MessageTask"
constexpr u32 kTaskElapsed = 0x28u;         // f32: seconds this line has been shown
constexpr u32 kTaskNewGroup = 0x2Cu;        // u8: the opening line's group differs from the last; passed to the draw, which ignores it
constexpr u32 kTaskRecord = 0x30u;          // the current line's record, 0 none
constexpr u32 kTaskTextX = 0x34u;           // f32
constexpr u32 kTaskTextY = 0x38u;           // f32
constexpr u32 kTaskTextScale = 0x3Cu;       // f32

// The .fnt message record (20 bytes). Sorted by key = group * 1000 + number * 100 + variant.
constexpr u32 kRecText = 0x00u;
constexpr u32 kRecKey = 0x04u;
constexpr u32 kRecPriority = 0x08u;
constexpr u32 kRecFrames = 0x0Cu;   // window length in 60 Hz frames; 0 = 180
constexpr u32 kRecVoice = 0x10u;    // PRS_VOICE index, -1 none
constexpr u32 kRecSize = 0x14u;
constexpr u32 kFontTable = 0x5Cu;   // Font: { u32 count; Record records[] }, may be 0
constexpr u32 kFontScale = 0x64u;   // Font: f32, the text scale

constexpr s32 kDefaultFrames = 0xB4;  // 180 frames for a record holding 0

// The game's global block, read by the update.
constexpr u32 kGlobals = 0x8057E760u;
constexpr u32 kGlobalMessagesEnabled = kGlobals + 0x0Au;  // u8: 0 = fn_80049BE8 draws and returns nothing
constexpr u32 kGlobalTextLanguage = kGlobals + 0x58u;     // word: 0 picks the fourth layout column
constexpr u32 kGlobalAltLayout = kGlobals + 0xB1u;        // u8
constexpr u32 kFreezeBlock = kGlobals + 0xA8u;            // the update does nothing while byte +0x21 or +0x23 is set
constexpr u32 kFreezeA = 0x21u;
constexpr u32 kFreezeB = 0x23u;

// Erratum E4 (docs/GAME_ERRATA.md), voice language English only: the state the display keeps past the original's window. Native
// data, no guest word. A line's subtitle stays visible until its voice ends (linger: display only, the manager words stay as the
// original left them), and the next part of a sequence that has its own voice waits for that voice instead of cutting it (gate). The
// game's interruptions are untouched.
constexpr u32 kVoiceLanguage = 0x8057E7BCu;     // G+0x5C: 1 = English
constexpr u32 kAdxtGetNumSmpl = 0x803C28B8u;
constexpr u64 kFailsafeMarginTicks = 40'500'000ull / 2u;    // 0.5 s of game time
constexpr u64 kFailsafeUnknownTicks = 120ull * 40'500'000ull;
constexpr u64 kGameTicksPerSecond = 40'500'000ull;
constexpr u32 kPrsVoiceRate = 24000u;

struct WindowLifetimeCounters {
    u64 lingers = 0;              // windows kept visible past their table limit
    u64 linger_updates = 0;
    u64 linger_ended_voice = 0;   // ... ended by the voice ending
    u64 linger_ended_reset = 0;   // ... by a manager reset
    u64 linger_ended_window = 0;  // ... by a newly opened window
    u64 linger_ended_failsafe = 0;
    u64 gates = 0;                // next parts that waited for a voice
    u64 gate_updates = 0;
    u64 gates_expired = 0;        // ... whose own window ended while they waited
    u64 holds = 0;                // requests of equal or lower priority held during a voice's overrun
    u64 holds_replaced = 0;       // ... replaced by a newer request
    u64 holds_refused = 0;        // newer requests lower than the held one, refused
    u64 holds_released = 0;       // released and accepted by the enqueue
    u64 holds_release_refused = 0;  // released but the enqueue refused it
    u64 holds_dropped = 0;        // dropped by a reset
    u64 deferred_lines = 0;       // released lines that kept their remaining parts (a later request of priority <= theirs held)
    u64 deferred_holds = 0;       // ... requests held because a deferred line was still playing out
    u64 deferred_admitted = 0;    // ... of them, lower priority than the deferred line, arriving after its original end (the original was idle)
    u64 failsafe_hits = 0;
};

struct WindowLifetime {
    bool linger_active = false;
    s32 linger_group = -1;
    s32 linger_number = 0;
    s32 linger_variant = 0;
    bool gate_active = false;
    float gate_elapsed = 0.0f;  // the gated part's timer, running from the update where the original would have opened it
    bool gate_expired = false;  // ... and it has reached the part's own limit while the part is still gated
    // The voice being followed (mgr+0x20 as the update started it).
    s32 voice = -1;
    u64 voice_start = 0u;
    s32 voice_samples = 0;
    bool samples_read = false;
    bool failsafe_counted = false;
    s32 voice_priority = -1;   // the priority of the line that started this voice (the one it would be interrupted against)
    // A request of priority <= the voice's line, accepted by the original during the voice's overrun, waiting for the voice to end.
    bool hold_pending = false;
    bool hold_releasing = false;
    s32 hold_group = 0;
    s32 hold_number = 0;
    s32 hold_first_variant = 0;
    s32 hold_priority = 0;
    s32 hold_key = 0;
    // A line released from a hold started late, off the game's script. Until its last part has finished (window closed, voice ended,
    // nothing of it queued), a request of priority <= its own waits for it as a hold does, so the delay the hold introduced can only
    // move the next line, never cut this one's later parts (Devil Doom 414-2, Tails' "Go for it, Shadow!", user 2026-11-14).
    s32 deferred_group = -1;
    s32 deferred_priority = 0;
    // When the original's timeline had finished the deferred line: the game tick its request arrived plus its parts' table windows. A
    // lower-priority request after it is judged as the original judged it then, against the idle state (accepted, so it waits too).
    u64 deferred_original_end = 0;
    u64 hold_ticks = 0;  // the game tick the held request arrived
    WindowLifetimeCounters counters;
};

// One word that a function fills only when it found a record.
struct OutWord {
    s32 value = 0;
    bool written = false;
    void set(s32 v) noexcept {
        value = v;
        written = true;
    }
};

class Env : public dialogue_voice::Env {
public:
    // The glyph layout and draw (fn_8004A09C, stays translated). The PC "Show subtitles" switch sits in its binding.
    virtual void draw_text(u32 font, u32 text, double x, double y) = 0;
    virtual u32 guest_new(u32 size) = 0;       // operator new 0x803A1380
    virtual void guest_delete(u32 object) = 0;  // operator delete 0x803A1334
    virtual void task_base_dtor(u32 task, s32 flags) = 0;  // Task::~Task 0x8004EF18
    virtual u32 task_parent_block() = 0;       // fn_800461A0 (its +0x54 is the MessageTask's parent)
    virtual s32 rand_below(s32 limit) = 0;     // 0x8040FAE0
    virtual void manager_construct(u32 manager) = 0;  // fn_801FB490 (reserves the queue through the game's allocator)
    virtual void register_static_dtor(u32 object, u32 dtor, u32 block) = 0;  // 0x803A1210
    virtual void sound_manager_construct() = 0;  // fn_8005328C when its pointer is still 0 (built on first use)
    // A boost shared count whose use count reaches zero (0x8000945C, the game's own dispose / destroy) and the
    // weak count's last release (its vtable +0x10).
    virtual void count_release_last(u32 count) = 0;
    virtual void control_destroy(u32 count) = 0;

    // Erratum E4's window-lifetime state, or nullptr when the switch is off or the code is under differential verification.
    virtual WindowLifetime* window_lifetime() { return nullptr; }
    virtual u64 game_ticks() { return 0u; }
    virtual s32 adxt_num_samples(u32 handle) { (void)handle; return 0; }  // the clip's header sample count, <= 0 unknown
    virtual void trace_lifetime(const char* event, const std::string& text) {
        (void)event;
        (void)text;
    }

    // The trace's view of what would have gone through a function entry (GEKKO_VOICE_TRACE); no-ops otherwise.
    virtual void trace_voice_stop(s32 voice) { (void)voice; }                  // fn_801D551C reached from native code
    virtual void trace_voice_request(s32 voice, s32 flag, s32 tag) {           // fn_801D5540 reached from native code
        (void)voice;
        (void)flag;
        (void)tag;
    }
    virtual void trace_queue_pop(u32 entry, u32 link) {                        // fn_801FBA18 reached from native code
        (void)entry;
        (void)link;
    }
};

// fn_8010A14C: the singleton (built on first use).
u32 manager(Env& env);

// fn_801FAEF4: the queue is emptied, the voice the system started is stopped, no window. mgr +0x20 and +0x28 stay.
void reset(Env& env, u32 mgr);

// fn_801FB210: asks to show lines first_variant .. 35 of (group, number). -1 when refused: locked, or the font is alive and
// the line's record is missing or has a lower priority than the current line. Otherwise everything else is dropped first
// (reset) and the lines are queued. Returns group * 1000 + number * 100. A missing record is refused here, it does not
// fall through to accept.
s32 enqueue(Env& env, u32 mgr, s32 group, s32 number, s32 first_variant);

// fn_801FAF54: the same admission for one random line of (group, number 0), queued without a reset. Returns group * 1000.
s32 enqueue_random(Env& env, u32 mgr, s32 group);

// fn_801FBA18: removes a queue entry.
void queue_erase(Env& env, u32 mgr, u32 entry);

// fn_801FADD8: 0 while `key` (group * 1000 + number * 100) is showing or queued, else 1. A negative key answers "the queue is
// not empty" (the original returns !empty for it; written as it is).
u32 is_finished(Env& env, u32 mgr, s32 key);

// fn_801FB3CC: a new font and a new MessageTask (`font` is the address of the caller's shared_ptr: pointer, count).
void reinit(Env& env, u32 mgr, u32 font);

// fn_801FB99C: MessageTask::MessageTask(parent). Returns `task`.
u32 task_ctor(Env& env, u32 task, u32 parent);
// fn_801FB938: vf0, the destructor; `flags` is its s16 delete argument. Returns `task`.
u32 task_dtor(Env& env, u32 task, u32 flags);
// fn_801FB5AC: vf1, one update; `dt` is the task walker's step.
void task_update(Env& env, u32 task, double dt);

// The 20-byte record search.
u32 find_record(Env& env, u32 font, s32 group, s32 number, s32 variant);                                 // fn_80049E44
u32 record_text(Env& env, u32 font, s32 group, s32 number, s32 variant, OutWord* frames, OutWord* voice);  // fn_80049D60
// fn_80049BE8: draws a line (falling back to number 0 when `number` has none) and returns its record, 0 if there is none or
// messages are disabled. The caller's eighth argument (r8) is overwritten at entry, so it is not a parameter here.
u32 draw_message(Env& env, u32 font, s32 group, s32 number, s32 variant, OutWord* voice, double x, double y);

}  // namespace gekko::message_system
