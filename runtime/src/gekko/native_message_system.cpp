// The message system: see include/gekko/native_message_system.h and docs/voice_subtitle_timing/11_timing_reconstruction.md.

#include "gekko/native_message_system.h"

#include <bit>

namespace gekko::message_system {

namespace {

constexpr u32 kNone = 0xFFFFFFFFu;
constexpr float kFramesPerSecond = 60.0f;  // 0x805F7794
constexpr u32 kZeroFloatBits = 0u;         // 0x805F7790

// The text position and scale table at 0x80543798: x, y, scale, scale when the text language is 0.
struct Layout {
    u32 x, y, scale, scale_language0;
};
constexpr Layout kLayoutNormal{0x3F0F5C29u, 0x3E3851ECu, 0x3D1BA5E3u, 0x3D23D70Au};      // +0x00
constexpr Layout kLayoutGlobalAlt{0x3F0CCCCDu, 0x3E6147AEu, 0x3D1BA5E3u, 0x3D23D70Au};   // +0x10
constexpr Layout kLayoutManagerAlt{0x3F1C28F6u, 0x3E3851ECu, 0x3D23D70Au, 0x3D23D70Au};  // +0x20

u32 text_key(s32 group, s32 number, s32 variant) noexcept {
    return static_cast<u32>(group) * 1000u + (static_cast<u32>(number) * 100u + static_cast<u32>(variant));
}

// The table search common to fn_80049E44, fn_80049D60 and fn_80049BE8. It reads the two end records even for a table that
// is too short (count 0 gives indices 0 and -1), as the original does.
u32 search_table(Env& env, u32 table, u32 key) {
    s32 low = 0;
    s32 high = static_cast<s32>(env.r32(table) - 1u);
    const u32 base = table + 4u;
    const auto record = [&](s32 index) { return base + static_cast<u32>(index) * kRecSize; };
    while (static_cast<s32>(static_cast<u32>(high) - static_cast<u32>(low)) > 1) {
        const s32 middle = (low + high) / 2;  // srawi + addze: rounds toward zero
        const u32 at = record(middle);
        const s32 found = static_cast<s32>(env.r32(at + kRecKey));
        if (found < static_cast<s32>(key)) {
            low = middle;
        } else if (found == static_cast<s32>(key)) {
            return at;
        } else {
            high = middle;
        }
    }
    u32 at = record(low);
    if (static_cast<s32>(key) == static_cast<s32>(env.r32(at + kRecKey))) return at;
    at = record(high);
    if (static_cast<s32>(key) == static_cast<s32>(env.r32(at + kRecKey))) return at;
    return 0u;
}

// boost::weak_ptr<Font>::lock() held for the scope of a call (the original's lock, copy and release of the temporary).
class FontLock {
public:
    FontLock(Env& env, u32 mgr) : env_(env) {
        const u32 count = env.r32(mgr + kMgrFontCount);
        if (count == 0u || env.r32(count + 4u) == 0u) return;  // expired: an empty pointer
        env.w32(count + 4u, env.r32(count + 4u) + 1u);
        font_ = env.r32(mgr + kMgrFont);
        count_ = count;
    }
    FontLock(const FontLock&) = delete;
    FontLock& operator=(const FontLock&) = delete;
    ~FontLock() {
        if (count_ == 0u) return;
        const u32 use = env_.r32(count_ + 4u);
        if (use == 1u) {
            env_.count_release_last(count_);
        } else {
            env_.w32(count_ + 4u, use - 1u);
        }
    }
    u32 get() const noexcept { return font_; }

private:
    Env& env_;
    u32 font_ = 0u;
    u32 count_ = 0u;
};

u32 queue_end(Env& env, u32 mgr) {
    return env.r32(mgr + kMgrQueueData) + env.r32(mgr + kMgrQueueCount) * kQueueEntrySize;
}

// std::vector::push_back. The growth path of fn_801FBAF4 is not reconstructed: the queue is reserved to 0x40 entries and an
// enqueue resets it first, so it is only reachable by repeated random enqueues without any update running.
void queue_push(Env& env, u32 mgr, s32 group, s32 number, s32 variant) {
    const u32 count = env.r32(mgr + kMgrQueueCount);
    if (count >= env.r32(mgr + kMgrQueueCapacity)) throw UnportedPath{"message queue growth (fn_801FBAF4 reallocation path)"};
    const u32 entry = env.r32(mgr + kMgrQueueData) + count * kQueueEntrySize;
    env.w32(entry, static_cast<u32>(group));
    env.w32(entry + 4u, static_cast<u32>(number));
    env.w32(entry + 8u, static_cast<u32>(variant));
    env.w32(mgr + kMgrQueueCount, count + 1u);
}

// Built on first use only; afterwards the call has no effect.
void ensure_sound_manager(Env& env) {
    if (env.r32(kSoundManagerPointer) == 0u) env.sound_manager_construct();
}

void weak_count_release(Env& env, u32 count) {
    if (count == 0u) return;
    const u32 weak = env.r32(count + 8u) - 1u;
    env.w32(count + 8u, weak);
    if (weak == 0u) env.control_destroy(count);
}

}  // namespace

u32 manager(Env& env) {
    if (env.r32(kManagerPointer) == 0u) {
        if (env.r8(kManagerGuard) == 0u) {
            env.manager_construct(kManager);
            env.register_static_dtor(kManager, kManagerDtor, kManagerDtorBlock);
            env.w8(kManagerGuard, 1u);
        }
        env.w32(kManagerPointer, kManager);
    }
    return env.r32(kManagerPointer);
}

u32 find_record(Env& env, u32 font, s32 group, s32 number, s32 variant) {
    const u32 table = env.r32(font + kFontTable);
    if (table == 0u) return 0u;
    return search_table(env, table, text_key(group, number, variant));
}

u32 record_text(Env& env, u32 font, s32 group, s32 number, s32 variant, OutWord* frames, OutWord* voice) {
    const u32 record = find_record(env, font, group, number, variant);
    if (record == 0u) return 0u;
    if (frames != nullptr) frames->set(static_cast<s32>(env.r32(record + kRecFrames)));
    if (voice != nullptr) voice->set(static_cast<s32>(env.r32(record + kRecVoice)));
    return env.r32(record + kRecText);
}

u32 draw_message(Env& env, u32 font, s32 group, s32 number, s32 variant, OutWord* voice, double x, double y) {
    if (env.r8(kGlobalMessagesEnabled) == 0u) return 0u;
    u32 text = record_text(env, font, group, number, variant, nullptr, voice);
    if (text == 0u && number != 0) {
        number = 0;
        text = record_text(env, font, group, 0, variant, nullptr, voice);
    }
    if (text == 0u) return 0u;
    env.draw_text(font, text, x, y);
    return find_record(env, font, group, number, variant);
}

namespace {

// The manager's voice is really playing: a slot holds it, is in state 1, and its ADXT reports status 1..4. A stuck stream cannot hold
// anything longer than the clip's own length plus the margin (the failsafe).
bool voice_really_playing(Env& env, WindowLifetime& wl, u32 mgr) {
    const s32 voice = static_cast<s32>(env.r32(mgr + kMgrVoice));
    if (voice < 0) return false;
    u32 handle = 0u;
    for (u32 i = 0; i < dialogue_voice::kSerifCount && handle == 0u; ++i) {
        const u32 serif = env.r32(dialogue_voice::kSerifSlots + i * 4u);
        if (serif == 0u || env.r8(serif + dialogue_voice::kSlotState) != 1u) continue;
        if (static_cast<s32>(env.r32(serif + dialogue_voice::kSlotVoice)) != voice) continue;
        const u32 candidate = env.r32(serif + dialogue_voice::kSlotAdxt);
        if (candidate == 0u) continue;
        const u32 status = env.r8(candidate + 1u);
        if (status >= 1u && status <= 4u) handle = candidate;
    }
    if (handle == 0u) return false;
    if (wl.voice != voice) {  // a voice this code did not see start: its clock starts now
        wl.voice = voice;
        wl.voice_start = env.game_ticks();
        wl.voice_samples = 0;
        wl.samples_read = false;
        wl.failsafe_counted = false;
    }
    if (!wl.samples_read) {
        const s32 samples = env.adxt_num_samples(handle);
        if (samples > 0) {
            wl.voice_samples = samples;
            wl.samples_read = true;
        }
    }
    const u64 length = wl.samples_read
                           ? static_cast<u64>(wl.voice_samples) * kGameTicksPerSecond / kPrsVoiceRate + kFailsafeMarginTicks
                           : kFailsafeUnknownTicks;
    if (env.game_ticks() > wl.voice_start + length) {
        if (!wl.failsafe_counted) {
            wl.failsafe_counted = true;
            ++wl.counters.failsafe_hits;
            env.trace_lifetime("hold_failsafe", "voice=" + std::to_string(voice));
        }
        return false;
    }
    return true;
}

void end_deferred(Env& env, WindowLifetime& wl, const char* reason) {
    if (wl.deferred_group < 0) return;
    env.trace_lifetime("deferred_end", std::string("reason=") + reason + "\tid=" + std::to_string(wl.deferred_group));
    wl.deferred_group = -1;
}

void end_linger(Env& env, WindowLifetime& wl, const char* reason) {
    if (!wl.linger_active) return;
    wl.linger_active = false;
    env.trace_lifetime("linger_end", std::string("reason=") + reason + "\tid=" + std::to_string(wl.linger_group));
}

void drop_hold(Env& env, WindowLifetime& wl, const char* reason) {
    if (!wl.hold_pending) return;
    wl.hold_pending = false;
    ++wl.counters.holds_dropped;
    env.trace_lifetime("hold_dropped", std::string("reason=") + reason + "\tkey=" + std::to_string(wl.hold_key));
}

// Whether the deferred line is still playing out: it is the current window, a queued entry of it still has a record (a part still to
// show; the entries past its last part have none and are dropped one per update), or its last voice is still really playing past its
// window (linger or gate). Ends the state the first time none of these holds.
bool deferred_active(Env& env, WindowLifetime& wl, u32 mgr, u32 font) {
    if (wl.deferred_group < 0) return false;
    bool active = static_cast<s32>(env.r32(mgr + kMgrCurrent)) == wl.deferred_group;
    if (!active && font != 0u) {
        const u32 end = queue_end(env, mgr);
        for (u32 entry = env.r32(mgr + kMgrQueueData); entry != end && !active; entry += kQueueEntrySize) {
            active = static_cast<s32>(env.r32(entry)) == wl.deferred_group &&
                     find_record(env, font, wl.deferred_group, static_cast<s32>(env.r32(entry + 4u)),
                                 static_cast<s32>(env.r32(entry + 8u))) != 0u;
        }
    }
    if (!active && (wl.linger_active || wl.gate_active)) active = voice_really_playing(env, wl, mgr);
    if (!active) end_deferred(env, wl, "finished");
    return active;
}

void end_gate(Env& env, WindowLifetime& wl, const char* reason) {
    if (!wl.gate_active) return;
    wl.gate_active = false;
    wl.gate_expired = false;
    env.trace_lifetime("gate_end", std::string("reason=") + reason);
}

// One update of a line past its window: the voice decides, the text is drawn, nothing the game can read changes.
void linger_tick(Env& env, WindowLifetime& wl, u32 task, u32 font, u32 mgr) {
    if (!wl.linger_active) return;
    if (!voice_really_playing(env, wl, mgr)) {
        const bool failsafe = wl.failsafe_counted;
        if (failsafe) {
            ++wl.counters.linger_ended_failsafe;
        } else {
            ++wl.counters.linger_ended_voice;
        }
        end_linger(env, wl, failsafe ? "failsafe" : "voice_ended");
        return;
    }
    ++wl.counters.linger_updates;
    if (env.r8(mgr + kMgrDrawEnable) == 0u) return;
    const u32 saved_scale = env.r32(font + kFontScale);
    env.w32(font + kFontScale, env.r32(task + kTaskTextScale));
    {
        const dialogue_voice::NestedCall nested(env, 0x801FB82Cu);
        const double x = static_cast<double>(std::bit_cast<float>(env.r32(task + kTaskTextX)));
        const double y = static_cast<double>(std::bit_cast<float>(env.r32(task + kTaskTextY)));
        OutWord ignored;
        draw_message(env, font, wl.linger_group, wl.linger_number, wl.linger_variant, &ignored, x, y);
    }
    env.w32(font + kFontScale, saved_scale);
}

}  // namespace

void reset(Env& env, u32 mgr) {
    if (WindowLifetime* wl = env.window_lifetime()) {
        if (wl->linger_active) ++wl->counters.linger_ended_reset;
        end_linger(env, *wl, "reset");
        end_gate(env, *wl, "reset");
        drop_hold(env, *wl, "reset");
        end_deferred(env, *wl, "reset");
    }
    env.w32(mgr + kMgrQueueCount, 0u);  // fn_801FAF34 -> fn_8010B670 (the element destructors are empty)
    ensure_sound_manager(env);
    {
        const dialogue_voice::NestedCall nested(env, 0x801FAF18u);  // liveness-roots: value-only
        const s32 voice = static_cast<s32>(env.r32(mgr + kMgrVoice));
        env.trace_voice_stop(voice);
        dialogue_voice::request_stop(env, voice);
    }
    env.w32(mgr + kMgrCurrent, kNone);
}

s32 enqueue(Env& env, u32 mgr, s32 group, s32 number, s32 first_variant) {
    if (static_cast<s32>(env.r32(mgr + kMgrLock)) != 0) return -1;
    const FontLock font(env, mgr);
    s32 incoming_priority = 0;
    bool have_record = false;
    if (font.get() != 0u) {
        const u32 record = find_record(env, font.get(), group, number, 0);
        if (record == 0u) return -1;
        incoming_priority = static_cast<s32>(env.r32(record + kRecPriority));
        have_record = true;
    }
    WindowLifetime* wl = env.window_lifetime();
    if (wl != nullptr && env.r32(kVoiceLanguage) != 1u) wl = nullptr;
    bool original_idle = false;
    if (have_record && incoming_priority < static_cast<s32>(env.r32(mgr + kMgrPriority))) {
        // Erratum E4: refused against a deferred line that the original's timeline had already finished. The original was idle then
        // and accepted it, so it waits for the deferred line instead.
        if (wl == nullptr || wl->hold_releasing || wl->deferred_group < 0 || env.game_ticks() < wl->deferred_original_end ||
            !deferred_active(env, *wl, mgr, font.get())) {
            return -1;
        }
        original_idle = true;
    }
    // Erratum E4: the original accepts this request (the tests above passed). During a voice's overrun, a request not strictly above the
    // voice's line waits for that voice instead of cutting it.
    // Also while a deferred line (released late from a hold) is still playing out, against that line's priority.
    bool deferred_hold = original_idle;
    if (original_idle) ++wl->counters.deferred_admitted;
    if (!original_idle && wl != nullptr && !wl->hold_releasing && have_record && !((wl->linger_active || wl->gate_active) &&
                                                                 voice_really_playing(env, *wl, mgr) &&
                                                                 incoming_priority <= wl->voice_priority)) {
        deferred_hold = deferred_active(env, *wl, mgr, font.get()) && incoming_priority <= wl->deferred_priority;
    }
    if (wl != nullptr && !wl->hold_releasing && have_record &&
        (deferred_hold || ((wl->linger_active || wl->gate_active) && voice_really_playing(env, *wl, mgr) &&
                           incoming_priority <= wl->voice_priority))) {
        if (deferred_hold) ++wl->counters.deferred_holds;
        const s32 key = static_cast<s32>(static_cast<u32>(group) * 1000u + static_cast<u32>(number) * 100u);
        const std::string text = "group=" + std::to_string(group) + "\tnumber=" + std::to_string(number) + "\tprio=" + std::to_string(incoming_priority);
        if (wl->hold_pending) {
            if (incoming_priority < wl->hold_priority) {
                ++wl->counters.holds_refused;
                env.trace_lifetime("hold_refused", text);
                return -1;
            }
            ++wl->counters.holds_replaced;
            env.trace_lifetime("hold_replaced", text + "\told_key=" + std::to_string(wl->hold_key));
        } else {
            ++wl->counters.holds;
            env.trace_lifetime("hold_request", text);
        }
        wl->hold_pending = true;
        wl->hold_group = group;
        wl->hold_number = number;
        wl->hold_first_variant = first_variant;
        wl->hold_priority = incoming_priority;
        wl->hold_key = key;
        wl->hold_ticks = env.game_ticks();
        return key;  // the value the original returns for an accepted request
    }
    {
        const dialogue_voice::NestedCall nested(env, 0x801FB2B8u);  // liveness-roots: value-only
        reset(env, mgr);
    }
    for (s32 variant = first_variant; variant < static_cast<s32>(kQueueLineLimit); ++variant) {
        queue_push(env, mgr, group, number, variant);
    }
    return static_cast<s32>(static_cast<u32>(group) * 1000u + static_cast<u32>(number) * 100u);
}

s32 enqueue_random(Env& env, u32 mgr, s32 group) {
    if (static_cast<s32>(env.r32(mgr + kMgrLock)) != 0) return -1;
    const FontLock font(env, mgr);
    if (font.get() == 0u) return -1;
    s32 lines = 0;
    for (; lines < static_cast<s32>(kQueueLineLimit); ++lines) {
        if (find_record(env, font.get(), group, 0, lines) == 0u) break;
    }
    if (lines == static_cast<s32>(kQueueLineLimit)) return -1;
    const u32 record = find_record(env, font.get(), group, 0, 0);
    if (record == 0u) return -1;
    if (static_cast<s32>(env.r32(record + kRecPriority)) < static_cast<s32>(env.r32(mgr + kMgrPriority))) return -1;
    const s32 variant = env.rand_below(lines);
    queue_push(env, mgr, group, 0, variant);
    return static_cast<s32>(static_cast<u32>(group) * 1000u);
}

void queue_erase(Env& env, u32 mgr, u32 entry) {
    const u32 end = queue_end(env, mgr);
    const s32 remaining = static_cast<s32>(end - entry) / static_cast<s32>(kQueueEntrySize);
    if (remaining - 1 != 0) {
        // A forward copy of the tail, entry by entry (fn_801FBA8C).
        u32 from = entry + kQueueEntrySize;
        u32 to = entry;
        for (; from < end; from += kQueueEntrySize, to += kQueueEntrySize) {
            env.w32(to, env.r32(from));
            env.w32(to + 4u, env.r32(from + 4u));
            env.w32(to + 8u, env.r32(from + 8u));
        }
    }
    env.w32(mgr + kMgrQueueCount, env.r32(mgr + kMgrQueueCount) - 1u);
}

u32 is_finished(Env& env, u32 mgr, s32 key) {
    if (key < 0) return env.r32(mgr + kMgrQueueCount) != 0u ? 1u : 0u;
    if (WindowLifetime* held = env.window_lifetime(); held != nullptr && held->hold_pending && held->hold_key == key) return 0u;
    const auto line_key = [](u32 group, u32 number) { return group * 1000u + number * 100u; };
    if (static_cast<u32>(key) == line_key(env.r32(mgr + kMgrCurrent), env.r32(mgr + kMgrCurrentNumber))) return 0u;
    u32 begin = env.r32(mgr + kMgrQueueData);
    const u32 end = queue_end(env, mgr);
    // Erratum E4: the original had shown this part and finished it at its own limit, so the waiting head entry no longer counts.
    if (WindowLifetime* wl = env.window_lifetime(); wl != nullptr && wl->gate_expired && begin != end) begin += kQueueEntrySize;
    for (u32 entry = begin; entry != end; entry += kQueueEntrySize) {
        if (static_cast<u32>(key) == line_key(env.r32(entry), env.r32(entry + 4u))) return 0u;
    }
    return 1u;
}

void reinit(Env& env, u32 mgr, u32 font) {
    // fn_801FB30C: the old task is told to die; its destructor runs later, from the task walker.
    const u32 old_task = env.r32(mgr + kMgrTask);
    if (old_task != 0u) {
        env.w16(old_task + dialogue_voice::kTaskFlags, static_cast<u16>(env.r16(old_task + dialogue_voice::kTaskFlags) | 1u));
        env.w32(mgr + kMgrTask, 0u);
    }
    // The manager's weak_ptr is cleared, releasing the old count.
    const u32 old_count = env.r32(mgr + kMgrFontCount);
    env.w32(mgr + kMgrFont, 0u);
    env.w32(mgr + kMgrFontCount, 0u);
    weak_count_release(env, old_count);
    // ... and assigned from the shared_ptr (fn_80017390: take the weak ref, then drop the previous one, which is now empty).
    env.w32(mgr + kMgrFont, env.r32(font));
    const u32 count = env.r32(font + 4u);
    if (count != 0u) env.w32(count + 8u, env.r32(count + 8u) + 1u);
    weak_count_release(env, env.r32(mgr + kMgrFontCount));
    env.w32(mgr + kMgrFontCount, count);

    u32 task = env.guest_new(kTaskSize);
    if (task != 0u) {
        const u32 parent = env.r32(env.task_parent_block() + 0x54u);
        const dialogue_voice::NestedCall nested(env, 0x801FB418u);  // liveness-roots: value-only
        task = task_ctor(env, task, parent);
    }
    env.w32(mgr + kMgrTask, task);
    env.w32(mgr + kMgrCurrent, kNone);
    env.w32(mgr + kMgrVoice, kNone);
    env.w32(mgr + kMgrLock, 0u);
    env.w8(mgr + kMgrAltLayout, 0u);
    env.w32(mgr + kMgrPriority, kNone);
}

u32 task_ctor(Env& env, u32 task, u32 parent) {
    env.task_base_ctor(task, parent);
    env.w32(task + dialogue_voice::kTaskVtable, kMessageTaskVtable);
    env.w32(task + kTaskElapsed, kZeroFloatBits);
    env.w8(task + kTaskNewGroup, 0u);
    env.w32(task + kTaskRecord, 0u);
    env.w32(task + dialogue_voice::kTaskName, kTaskNameText);
    const u32 mgr = manager(env);
    {
        const dialogue_voice::NestedCall nested(env, 0x801FB9E8u);  // liveness-roots: value-only
        reset(env, mgr);
    }
    env.w8(mgr + kMgrDrawEnable, 1u);
    env.w16(task + dialogue_voice::kTaskFlags, static_cast<u16>(env.r16(task + dialogue_voice::kTaskFlags) | 0x100u));
    return task;
}

u32 task_dtor(Env& env, u32 task, u32 flags) {
    if (task == 0u) return task;
    env.w32(task + dialogue_voice::kTaskVtable, kMessageTaskVtable);
    {
        const u32 mgr = manager(env);
        const dialogue_voice::NestedCall nested(env, 0x801FB968u);  // liveness-roots: value-only
        reset(env, mgr);
    }
    env.task_base_dtor(task, 0);
    if (static_cast<s16>(static_cast<u16>(flags)) > 0) env.guest_delete(task);
    return task;
}

void task_update(Env& env, u32 task, double dt) {
    if (env.r8(kFreezeBlock + kFreezeA) != 0u || env.r8(kFreezeBlock + kFreezeB) != 0u) return;

    const u32 mgr = manager(env);  // the later accessor calls of the original are pure once this one has run
    const Layout& layout = env.r8(mgr + kMgrAltLayout) != 0u ? kLayoutManagerAlt
                           : env.r8(kGlobalAltLayout) != 0u  ? kLayoutGlobalAlt
                                                             : kLayoutNormal;
    env.w32(task + kTaskTextX, layout.x);
    env.w32(task + kTaskTextY, layout.y);
    env.w32(task + kTaskTextScale, env.r32(kGlobalTextLanguage) == 0u ? layout.scale_language0 : layout.scale);

    const FontLock font_lock(env, mgr);
    const u32 font = font_lock.get();
    if (font == 0u) return;

    // The window-lifetime state (erratum E4). Null: the switch is off or under verification, or the voice language is not English:
    // from here on the code below is the original.
    WindowLifetime* wl = env.window_lifetime();
    if (wl != nullptr && env.r32(kVoiceLanguage) != 1u) {
        end_linger(env, *wl, "language");
        end_gate(env, *wl, "language");
        wl = nullptr;
    }

    // Erratum E4: the voice a held request waits for has ended (or the failsafe gave up on it): the request now does what the original
    // did when it arrived, against the idle state and a free lock. The lock is restored afterward; a refusal is counted, never hidden.
    if (wl != nullptr && wl->hold_pending && !voice_really_playing(env, *wl, mgr) && !deferred_active(env, *wl, mgr, font)) {
        if (wl->linger_active) {
            ++wl->counters.linger_ended_voice;
            end_linger(env, *wl, "voice_ended");
        }
        const s32 group = wl->hold_group, number = wl->hold_number, variant = wl->hold_first_variant, key = wl->hold_key;
        const s32 held_priority = wl->hold_priority;
        const u64 held_ticks = wl->hold_ticks;
        wl->hold_pending = false;
        const u32 saved_lock = env.r32(mgr + kMgrLock);
        env.w32(mgr + kMgrLock, 0u);
        env.w32(mgr + kMgrPriority, kNone);
        wl->hold_releasing = true;
        const s32 result = enqueue(env, mgr, group, number, variant);
        wl->hold_releasing = false;
        env.w32(mgr + kMgrLock, saved_lock);
        if (result < 0) {
            ++wl->counters.holds_release_refused;
            env.trace_lifetime("hold_release_refused", "key=" + std::to_string(key));
        } else {
            ++wl->counters.holds_released;
            env.trace_lifetime("hold_release", "key=" + std::to_string(key));
            // Started late: its remaining parts keep their turn (after the enqueue, whose reset ends any earlier deferred line).
            wl->deferred_group = group;
            wl->deferred_priority = held_priority;
            // Its parts' table windows from the tick its request arrived: when the original had finished it.
            u64 frames_total = 0u;
            for (s32 part = variant; part < static_cast<s32>(kQueueLineLimit); ++part) {
                const u32 rec = find_record(env, font, group, number, part);
                if (rec == 0u) continue;
                const s32 frames_part = static_cast<s32>(env.r32(rec + kRecFrames));
                frames_total += static_cast<u64>(frames_part == 0 ? kDefaultFrames : frames_part);
            }
            wl->deferred_original_end = held_ticks + frames_total * kGameTicksPerSecond / 60u;
            ++wl->counters.deferred_lines;
            env.trace_lifetime("deferred_start", "id=" + std::to_string(group) + "\tprio=" + std::to_string(held_priority));
        }
    }

    bool opened_line = false;
    bool gated_expired_part = false;  // E4: this part's window ended while it was gated; it opens only so its voice plays
    if (env.r32(mgr + kMgrQueueCount) == 0u && static_cast<s32>(env.r32(mgr + kMgrCurrent)) < 0) {
        env.w32(mgr + kMgrPriority, kNone);
        if (wl != nullptr) linger_tick(env, *wl, task, font, mgr);
        return;
    }
    const u32 head = env.r32(mgr + kMgrQueueData);
    if (head != queue_end(env, mgr)) {
        const u32 queued = find_record(env, font, static_cast<s32>(env.r32(head)), static_cast<s32>(env.r32(head + 4u)),
                                       static_cast<s32>(env.r32(head + 8u)));
        if (queued == 0u) {
            env.trace_queue_pop(head, 0x801FB7C8u);  // liveness-roots: value-only
            queue_erase(env, mgr, head);  // no such line: dropped, one entry per update
        } else {
            const s32 current = static_cast<s32>(env.r32(mgr + kMgrCurrent));
            const u32 current_record = env.r32(task + kTaskRecord);
            bool open = current == -1 || current_record == 0u;
            s32 queued_priority = 0, current_priority = 0;
            if (!open) {
                queued_priority = static_cast<s32>(env.r32(queued + kRecPriority));
                current_priority = static_cast<s32>(env.r32(current_record + kRecPriority));
                open = queued_priority > current_priority ||
                       (current != static_cast<s32>(env.r32(head)) && queued_priority == current_priority);
            }
            // Erratum E4's next-part gate. It applies only to the "no window" case (current == -1): every other reason to open here is
            // an interruption (a higher priority, or an equal priority of another group, replacing a window that is still showing)
            // and stays as it is, and the drop of a lower-priority part below never reaches here with current == -1.
            if (open && wl != nullptr && current == -1 && static_cast<s32>(env.r32(queued + kRecVoice)) >= 0 &&
                voice_really_playing(env, *wl, mgr)) {
                if (!wl->gate_active) {
                    wl->gate_active = true;
                    wl->gate_elapsed = 0.0f;
                    wl->gate_expired = false;
                    ++wl->counters.gates;
                    env.trace_lifetime("gate_start", "id=" + std::to_string(static_cast<s32>(env.r32(head))) + "\tb=" +
                                                         std::to_string(static_cast<s32>(env.r32(head + 8u))) + "\tvoice=" +
                                                         std::to_string(static_cast<s32>(env.r32(mgr + kMgrVoice))));
                }
                ++wl->counters.gate_updates;
                if (wl->gate_expired) {
                    // The original had closed this part: its word is the idle state.
                    env.w32(mgr + kMgrPriority, kNone);
                } else {
                    // The word the part would have shown had it opened: admission of a new request is judged as it would have been.
                    env.w32(mgr + kMgrPriority, env.r32(queued + kRecPriority));
                    // Its timer runs as the original's did, from this update (the fadds of the update's elapsed; the original opens it at 0).
                    s32 frames_gated = static_cast<s32>(env.r32(queued + kRecFrames));
                    if (frames_gated == 0) frames_gated = kDefaultFrames;
                    const float limit_gated = static_cast<float>(frames_gated) / kFramesPerSecond;
                    wl->gate_elapsed = static_cast<float>(static_cast<double>(wl->gate_elapsed) + dt);
                    if (wl->gate_elapsed >= limit_gated) {
                        wl->gate_expired = true;
                        ++wl->counters.gates_expired;
                        env.trace_lifetime("gate_expired", "id=" + std::to_string(static_cast<s32>(env.r32(head))));
                    }
                }
                linger_tick(env, *wl, task, font, mgr);
                return;
            }
            const bool gated_part = wl != nullptr && wl->gate_active;
            const float gate_elapsed = gated_part ? wl->gate_elapsed : 0.0f;
            gated_expired_part = gated_part && wl->gate_expired;
            if (wl != nullptr) end_gate(env, *wl, "opened");
            if (open) {
                if (wl != nullptr && wl->linger_active) {
                    ++wl->counters.linger_ended_window;
                    end_linger(env, *wl, "new_window");
                }
                const u32 head_group = env.r32(head);
                env.w8(task + kTaskNewGroup, static_cast<u8>(static_cast<s32>(env.r32(mgr + kMgrCurrent)) != static_cast<s32>(head_group) ? 1 : 0));
                env.w32(mgr + kMgrCurrent, head_group);
                opened_line = true;
                env.w32(mgr + kMgrCurrentNumber, env.r32(head + 4u));
                env.w32(mgr + kMgrCurrentVariant, env.r32(head + 8u));
                env.w32(task + kTaskElapsed, gated_part ? std::bit_cast<u32>(gate_elapsed) : kZeroFloatBits);
                env.trace_queue_pop(head, 0x801FB7A0u);  // liveness-roots: value-only
                queue_erase(env, mgr, head);
            } else if (queued_priority < current_priority) {
                env.trace_queue_pop(head, 0x801FB7B8u);  // liveness-roots: value-only
                queue_erase(env, mgr, head);  // lower priority: dropped. Equal priority and the same group waits in the queue.
            }
        }
    }

    if (static_cast<s32>(env.r32(mgr + kMgrCurrent)) < 0) {
        env.w32(mgr + kMgrPriority, kNone);
        if (wl != nullptr) linger_tick(env, *wl, task, font, mgr);
        return;
    }

    env.w32(font + kFontScale, env.r32(task + kTaskTextScale));

    const s32 group = static_cast<s32>(env.r32(mgr + kMgrCurrent));
    const s32 number = static_cast<s32>(env.r32(mgr + kMgrCurrentNumber));
    const s32 variant = static_cast<s32>(env.r32(mgr + kMgrCurrentVariant));
    OutWord voice;
    if (env.r8(mgr + kMgrDrawEnable) != 0u) {
        const dialogue_voice::NestedCall nested(env, 0x801FB82Cu);
        const double x = static_cast<double>(std::bit_cast<float>(env.r32(task + kTaskTextX)));
        const double y = static_cast<double>(std::bit_cast<float>(env.r32(task + kTaskTextY)));
        env.w32(task + kTaskRecord, draw_message(env, font, group, number, variant, &voice, x, y));
    } else {
        const u32 record = find_record(env, font, group, number, variant);
        env.w32(task + kTaskRecord, record);
        // The original reads record + 0x10 unchecked here: a null record is an access fault on the console.
        if (record == 0u) throw UnportedPath{"MessageTask update: the current line has no record while drawing is off"};
        voice.set(static_cast<s32>(env.r32(record + kRecVoice)));
    }

    const u32 record = env.r32(task + kTaskRecord);
    if (record == 0u) {
        env.w32(mgr + kMgrCurrent, kNone);
        return;
    }

    s32 frames = static_cast<s32>(env.r32(record + kRecFrames));
    if (frames == 0) frames = kDefaultFrames;
    const u32 priority = env.r32(record + kRecPriority);
    env.w32(mgr + kMgrPriority, priority);

    const float limit = static_cast<float>(frames) / kFramesPerSecond;
    const float elapsed =
        static_cast<float>(static_cast<double>(std::bit_cast<float>(env.r32(task + kTaskElapsed))) + dt);  // fadds
    env.w32(task + kTaskElapsed, std::bit_cast<u32>(elapsed));
    const bool expired = elapsed >= limit;
    if (expired) env.w32(mgr + kMgrCurrent, kNone);  // the window closes; no audio is touched
    if (gated_expired_part) env.w32(mgr + kMgrPriority, kNone);  // the original was idle by now

    if (!opened_line || !voice.written || voice.value < 0) {  // a silent line leaves the previous voice playing
        if (expired && wl != nullptr && !wl->linger_active && voice_really_playing(env, *wl, mgr)) {
            wl->linger_active = true;
            wl->linger_group = group;
            wl->linger_number = number;
            wl->linger_variant = variant;
            ++wl->counters.lingers;
            env.trace_lifetime("linger_start", "id=" + std::to_string(group) + "\ta=" + std::to_string(number) + "\tb=" +
                                                    std::to_string(variant) + "\tvoice=" +
                                                    std::to_string(static_cast<s32>(env.r32(mgr + kMgrVoice))));
        }
        return;
    }
    ensure_sound_manager(env);
    {
        const dialogue_voice::NestedCall nested(env, 0x801FB8E8u);  // liveness-roots: value-only
        const s32 previous = static_cast<s32>(env.r32(mgr + kMgrVoice));
        env.trace_voice_stop(previous);
        dialogue_voice::request_stop(env, previous);
    }
    env.w32(mgr + kMgrVoice, static_cast<u32>(voice.value));
    ensure_sound_manager(env);
    const dialogue_voice::NestedCall nested(env, 0x801FB904u);  // liveness-roots: value-only
    env.trace_voice_request(voice.value, -1, -1);
    dialogue_voice::request_play(env, voice.value, 0xFFFFFFFFu, -1);
    if (wl != nullptr) {
        // The clock of the voice just requested (the failsafe's), and a window that expires in the update that opened it lingers too.
        wl->voice_priority = static_cast<s32>(priority);
        wl->voice = voice.value;
        wl->voice_start = env.game_ticks();
        wl->voice_samples = 0;
        wl->samples_read = false;
        wl->failsafe_counted = false;
        if (expired && !wl->linger_active && voice_really_playing(env, *wl, mgr)) {
            wl->linger_active = true;
            wl->linger_group = group;
            wl->linger_number = number;
            wl->linger_variant = variant;
            ++wl->counters.lingers;
            env.trace_lifetime("linger_start", "id=" + std::to_string(group) + "\ta=" + std::to_string(number) + "\tb=" +
                                                    std::to_string(variant) + "\tvoice=" + std::to_string(voice.value));
        }
    }
}

}  // namespace gekko::message_system
