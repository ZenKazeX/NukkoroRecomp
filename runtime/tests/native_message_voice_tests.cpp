// The reconstructed message system and dialogue voice layer on a fake guest. Contract: gekko/native_message_system.h,
// gekko/native_dialogue_voice.h; evidence: docs/voice_subtitle_timing/11_timing_reconstruction.md.
//
// These check the reconstruction against the behaviour read from the disassembly. They do not replace the differential
// run against the original bodies; they pin the behaviour of each branch and every float that the run can only sample.

#include <bit>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "gekko/native_dialogue_voice.h"
#include "gekko/native_message_system.h"

using namespace gekko;
using namespace gekko::message_system;
namespace dv = gekko::dialogue_voice;

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

constexpr u32 kFont = 0x80600000u, kTable = 0x80601000u, kControl = 0x80602000u, kTask = 0x80603000u, kQueue = 0x80606000u;
constexpr u32 kSerif0 = 0x80604000u, kSerif1 = 0x80604100u, kHandle0 = 0x80605000u, kHandle1 = 0x80605100u;
constexpr u32 kBgm0 = 0x80607000u, kBgm1 = 0x80607100u, kBgmHandle0 = 0x80608000u, kBgmHandle1 = 0x80608100u;
constexpr u32 kNew = 0x80700000u;

class Fake final : public message_system::Env {
public:
    std::map<u32, u8> bytes;
    std::vector<std::string> log;
    s32 next_rand = 0;
    bool lifetime_on = false;
    WindowLifetime lifetime;
    u64 ticks_now = 0;
    s32 samples_answer = 240000;
    s32 draws = 0;
    std::vector<std::string> lifetime_events;
    s32 stat_for_handle0 = 3, stat_for_handle1 = 3;
    u32 next_new = kNew;
    u32 parent_block = 0x80609000u;

    u8 r8(u32 a) override {
        auto it = bytes.find(a);
        return it == bytes.end() ? 0 : it->second;
    }
    u16 r16(u32 a) override { return static_cast<u16>((r8(a) << 8) | r8(a + 1)); }
    u32 r32(u32 a) override {
        return (static_cast<u32>(r8(a)) << 24) | (static_cast<u32>(r8(a + 1)) << 16) | (static_cast<u32>(r8(a + 2)) << 8) | r8(a + 3);
    }
    void w8(u32 a, u8 v) override { bytes[a] = v; }
    void w16(u32 a, u16 v) override {
        bytes[a] = static_cast<u8>(v >> 8);
        bytes[a + 1] = static_cast<u8>(v);
    }
    void w32(u32 a, u32 v) override {
        bytes[a] = static_cast<u8>(v >> 24);
        bytes[a + 1] = static_cast<u8>(v >> 16);
        bytes[a + 2] = static_cast<u8>(v >> 8);
        bytes[a + 3] = static_cast<u8>(v);
    }

    void note(const std::string& s) { log.push_back(s); }
    static std::string hex(u32 v) {
        char t[16];
        std::snprintf(t, sizeof(t), "%08X", v);
        return t;
    }

    u32 adxt_create(s32 ch, u32 work, u32 size) override {
        note("create " + std::to_string(ch) + " " + hex(work) + " " + hex(size));
        return create_result;
    }
    u32 create_result = kHandle0;
    void adxt_stop(u32 h, u32) override { note("stop " + hex(h)); }
    void adxt_pause(u32 h, u32 on) override { note("pause " + hex(h) + " " + std::to_string(on)); }
    void adxt_start_afs(u32 h, u32 part, u32 file) override {
        w8(h + 1u, 1);  // the real call sets decode-info status
        note("start " + hex(h) + " " + std::to_string(part) + " " + std::to_string(file));
    }
    s32 adxt_get_stat(u32 h) override {
        note("stat " + hex(h));
        return h == kHandle0 ? stat_for_handle0 : stat_for_handle1;
    }
    void adxt_set_out_vol(u32 h, s32 v) override { note("vol " + hex(h) + " " + std::to_string(v)); }
    void cri_setup_803C5750(u32 a) override { note("cri5750 " + std::to_string(a)); }
    void cri_setup_803C5118(u32 a) override { note("cri5118 " + std::to_string(a)); }
    void cri_setup_803B72D8() override { note("cri72D8"); }
    void task_base_ctor(u32 t, u32 p) override { note("task_ctor " + hex(t) + " " + hex(p)); }
    u32 sound_heap_alloc(u32 s, u32 tag) override {
        note("alloc " + hex(s) + " " + std::to_string(tag));
        return 0x80800000u + s;
    }
    WindowLifetime* window_lifetime() override { return lifetime_on ? &lifetime : nullptr; }
    u64 game_ticks() override { return ticks_now; }
    s32 adxt_num_samples(u32) override { return samples_answer; }
    void trace_lifetime(const char* event, const std::string&) override { lifetime_events.push_back(event); }

    void draw_text(u32 f, u32 t, double x, double y) override {
        ++draws;
        note("draw " + hex(f) + " " + hex(t) + " " + hex(std::bit_cast<u32>(static_cast<float>(x))) + " " +
             hex(std::bit_cast<u32>(static_cast<float>(y))));
    }
    u32 guest_new(u32 s) override {
        note("new " + hex(s));
        return next_new;
    }
    void guest_delete(u32 o) override { note("delete " + hex(o)); }
    void task_base_dtor(u32 t, s32 f) override { note("task_dtor " + hex(t) + " " + std::to_string(f)); }
    u32 task_parent_block() override {
        note("parent_block");
        return parent_block;
    }
    s32 rand_below(s32 limit) override {
        note("rand " + std::to_string(limit));
        return next_rand;
    }
    void manager_construct(u32 m) override { note("mgr_construct " + hex(m)); }
    void register_static_dtor(u32 o, u32 d, u32 b) override { note("atexit " + hex(o) + " " + hex(d) + " " + hex(b)); }
    void sound_manager_construct() override { note("snd_construct"); }
    void count_release_last(u32 c) override { note("release_last " + hex(c)); }
    void control_destroy(u32 c) override { note("control_destroy " + hex(c)); }

    // Setup.
    void wf(u32 a, float f) { w32(a, std::bit_cast<u32>(f)); }
    float rf(u32 a) { return std::bit_cast<float>(r32(a)); }
    void records(const std::vector<std::vector<s32>>& rows) {  // {key, priority, frames, voice}; text = 0x8100xxxx by index + 1
        w32(kTable, static_cast<u32>(rows.size()));
        for (size_t i = 0; i < rows.size(); ++i) {
            const u32 r = kTable + 4u + static_cast<u32>(i) * kRecSize;
            w32(r + kRecText, 0x81000000u + static_cast<u32>(i) + 1u);
            w32(r + kRecKey, static_cast<u32>(rows[i][0]));
            w32(r + kRecPriority, static_cast<u32>(rows[i][1]));
            w32(r + kRecFrames, static_cast<u32>(rows[i][2]));
            w32(r + kRecVoice, static_cast<u32>(rows[i][3]));
        }
    }
    u32 record_at(u32 i) { return kTable + 4u + i * kRecSize; }

    void boot() {
        w32(kManagerPointer, kManager);
        w8(kManagerGuard, 1);
        w32(kSoundManagerPointer, 0x80571C34u);
        w32(kManager + kMgrQueueCapacity, 0x40u);
        w32(kManager + kMgrQueueData, kQueue);
        w32(kManager + kMgrFont, kFont);
        w32(kManager + kMgrFontCount, kControl);
        w32(kControl + 4u, 1u);
        w32(kControl + 8u, 1u);
        w32(kFont + kFontTable, kTable);
        w32(kManager + kMgrCurrent, 0xFFFFFFFFu);
        w32(kManager + kMgrVoice, 0xFFFFFFFFu);
        w32(kManager + kMgrPriority, 0xFFFFFFFFu);
        w8(kManager + kMgrDrawEnable, 1);
        w8(kGlobalMessagesEnabled, 1);
        w32(kGlobalTextLanguage, 1u);
        // The voice slots, idle.
        w32(dv::kSerifSlots, kSerif0);
        w32(dv::kSerifSlots + 4u, kSerif1);
        for (u32 s : {kSerif0, kSerif1}) {
            w32(s + dv::kSlotVoice, 0xFFFFFFFFu);
            w32(s + dv::kSlotKind, 0xFFFFFFFFu);
            w8(s + dv::kSlotPriority, 0xFF);
            w8(s + dv::kSlotState, 0);
        }
        w32(kSerif0 + dv::kSlotAdxt, kHandle0);
        w32(kSerif1 + dv::kSlotAdxt, kHandle1);
    }
    u32 count() { return r32(kManager + kMgrQueueCount); }
    s32 current() { return static_cast<s32>(r32(kManager + kMgrCurrent)); }
    void queue(u32 i, s32 group, s32 number, s32 variant) {
        w32(kQueue + i * 12u, static_cast<u32>(group));
        w32(kQueue + i * 12u + 4u, static_cast<u32>(number));
        w32(kQueue + i * 12u + 8u, static_cast<u32>(variant));
    }
    bool log_has(const std::string& s) {
        for (const auto& l : log) {
            if (l == s) return true;
        }
        return false;
    }
};

// The idiom the original's loop computes with its xor / shift. Exhaustive equality with the plain comparison.
bool idiom_less(s32 a, s32 b) {
    const u32 x = static_cast<u32>(a) ^ static_cast<u32>(b);
    const u32 y = static_cast<u32>(static_cast<s32>(x) >> 1);
    const u32 z = x & static_cast<u32>(b);
    return ((y - z) >> 31) != 0u;
}

float accumulate(float limit_probe, u32 steps, double dt);

// A voice playing in slot 0: state 1, id, ADXT status 3 (the handle's byte +1).
void set_voice(Fake& f, u32 voice, u8 status = 3) {
    f.w8(kSerif0 + dv::kSlotState, 1);
    f.w32(kSerif0 + dv::kSlotVoice, voice);
    f.w8(kHandle0 + 1u, status);
}

void boot_english(Fake& f) {
    f.boot();
    f.w32(kVoiceLanguage, 1u);
    f.lifetime_on = true;
}

// Queues {group, 0, variant}; returns after opening it.
void open_part(Fake& f, s32 group, double dt) {
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, group, 0, 0);
    task_update(f, kTask, dt);
}

void test_linger() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    boot_english(f);
    f.records({{5000, 100, 2, 7}});
    open_part(f, 5, dt);
    set_voice(f, 7);
    task_update(f, kTask, dt);  // 2 frames: expires here (window closed; voice 7 from the first update)
    CHECK(f.current() == -1 && f.lifetime.linger_active);
    CHECK(f.lifetime.counters.lingers == 1u);
    const int base_draws = f.draws;
    const u32 scale_before = f.r32(kFont + kFontScale);
    const u32 record = f.r32(kTask + kTaskRecord);
    const u32 elapsed = f.r32(kTask + kTaskElapsed);
    task_update(f, kTask, dt);
    CHECK(f.draws == base_draws + 1);
    CHECK(f.r32(kTask + kTaskRecord) == record && f.r32(kTask + kTaskElapsed) == elapsed);
    CHECK(f.r32(kFont + kFontScale) == scale_before);  // restored: the idle path leaves the font scale alone
    CHECK(f.r32(kManager + kMgrPriority) == 0xFFFFFFFFu);
    // Voice end ends it in the update that sees it.
    f.w8(kHandle0 + 1u, 5);
    task_update(f, kTask, dt);
    CHECK(!f.lifetime.linger_active && f.lifetime.counters.linger_ended_voice == 1u);
    CHECK(f.draws == base_draws + 1);
}

void test_linger_reset_ends() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    boot_english(f);
    f.records({{5000, 100, 2, 7}});
    open_part(f, 5, dt);
    set_voice(f, 7);
    task_update(f, kTask, dt);
    CHECK(f.lifetime.linger_active);
    reset(f, kManager);
    CHECK(!f.lifetime.linger_active && f.lifetime.counters.linger_ended_reset == 1u);
}

// Manager words and the task's timer/record, compared byte for byte through a linger.
void test_linger_words_identical() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake on, off;
    boot_english(on);
    off.boot();
    for (Fake* f : {&on, &off}) f->records({{5000, 100, 2, 7}});
    for (int frame = 0; frame < 20; ++frame) {
        if (frame == 0) {
            open_part(on, 5, dt);
            open_part(off, 5, dt);
            set_voice(on, 7);
            set_voice(off, 7);
        } else {
            task_update(on, kTask, dt);
            task_update(off, kTask, dt);
        }
        for (u32 i = 0; i < 0x34u; ++i) CHECK(on.r8(kManager + i) == off.r8(kManager + i));
        CHECK(on.r32(kTask + kTaskElapsed) == off.r32(kTask + kTaskElapsed));
        CHECK(on.r32(kTask + kTaskRecord) == off.r32(kTask + kTaskRecord));
        CHECK(on.r32(kFont + kFontScale) == off.r32(kFont + kFontScale));
    }
    CHECK(on.lifetime.counters.lingers == 1u);
}

void test_gate() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    boot_english(f);
    f.records({{5000, 100, 2, 7}, {5001, 100, 60, 8}});
    open_part(f, 5, dt);
    set_voice(f, 7);
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 5, 0, 1);  // the next part with its own voice waits behind the open window (same group, equal or lower priority than the current)
    task_update(f, kTask, dt);  // expires
    CHECK(f.current() == -1);
    task_update(f, kTask, dt);  // the part would open: gated, because voice 7 still plays
    CHECK(f.lifetime.gate_active && f.lifetime.counters.gates == 1u);
    CHECK(f.current() == -1 && f.count() == 1u);  // not opened, not erased
    CHECK(f.r32(kManager + kMgrPriority) == 100u);  // the priority the part presents, not the idle -1
    task_update(f, kTask, dt);
    CHECK(f.count() == 1u);
    // The voice ends: it opens in the very next update.
    f.w8(kHandle0 + 1u, 5);
    task_update(f, kTask, dt);
    CHECK(f.current() == 5 && f.count() == 0u && !f.lifetime.gate_active);
    CHECK(f.r32(kManager + kMgrVoice) == 8u);
    // The timer follows the original's: 3 updates since the update the original would have opened the part (the 2 gated ones and this one).
    CHECK(std::bit_cast<u32>(f.rf(kTask + kTaskElapsed)) == std::bit_cast<u32>(accumulate(0, 3, dt)));
}

void test_gate_expired_while_waiting() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    boot_english(f);
    f.records({{5000, 100, 2, 7}, {5001, 100, 2, 8}});
    open_part(f, 5, dt);
    set_voice(f, 7);
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 5, 0, 1);
    task_update(f, kTask, dt);  // the current part expires
    task_update(f, kTask, dt);  // gated, the timer runs from here (the original would have opened it here)
    CHECK(f.lifetime.gate_active && !f.lifetime.gate_expired);
    CHECK(f.r32(kManager + kMgrPriority) == 100u);
    task_update(f, kTask, dt);  // its own 2-frame window ends here
    CHECK(f.lifetime.gate_expired && f.lifetime.counters.gates_expired == 1u);
    task_update(f, kTask, dt);  // the original is idle now
    CHECK(f.r32(kManager + kMgrPriority) == 0xFFFFFFFFu);
    CHECK(f.current() == -1 && f.count() == 1u);
    // The poll sees the part as finished (the original finished it), though it waits in the queue.
    CHECK(is_finished(f, kManager, 5000) == 1u);
    // The voice ends: the part still opens so its voice plays, expires at once, and its window is the linger.
    f.w8(kHandle0 + 1u, 5);
    task_update(f, kTask, dt);
    CHECK(f.current() == -1 && f.count() == 0u);
    CHECK(f.r32(kManager + kMgrVoice) == 8u);
    CHECK(f.r32(kManager + kMgrPriority) == 0xFFFFFFFFu);
    CHECK(f.lifetime.linger_active && f.lifetime.linger_group == 5);
    CHECK(!f.lifetime.gate_active);
}

void test_gate_voiceless_part() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    boot_english(f);
    f.records({{5000, 100, 2, 7}, {5001, 100, 60, -1}});
    open_part(f, 5, dt);
    set_voice(f, 7);
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 5, 0, 1);
    task_update(f, kTask, dt);
    CHECK(f.current() == -1);
    task_update(f, kTask, dt);
    CHECK(f.current() == 5 && f.count() == 0u);  // opens at once, the voice plays on
    CHECK(f.lifetime.counters.gates == 0u);
}

void test_gate_only_with_no_window() {
    // A higher priority part while a window is showing is an interruption: not gated.
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    boot_english(f);
    f.records({{5000, 100, 600, 7}, {6000, 200, 60, 9}});
    open_part(f, 5, dt);
    set_voice(f, 7);
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 6, 0, 0);
    task_update(f, kTask, dt);
    CHECK(f.current() == 6 && f.lifetime.counters.gates == 0u);
}

void test_enqueue_during_linger() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    boot_english(f);
    f.records({{5000, 100, 2, 7}, {6000, 50, 60, -1}});
    open_part(f, 5, dt);
    set_voice(f, 7);
    task_update(f, kTask, dt);
    CHECK(f.lifetime.linger_active);
    // Priority -1 from the idle path (the original's state): a lower-priority line is admitted, and the reset of the admission ends the linger.
    task_update(f, kTask, dt);
    CHECK(f.r32(kManager + kMgrPriority) == 0xFFFFFFFFu);
    // Priority 50 is not above the voice's line (100): during the voice's overrun it waits rather than cutting it.
    CHECK(enqueue(f, kManager, 6, 0, 0) == 6000);
    CHECK(f.lifetime.linger_active && f.lifetime.hold_pending);
}

void test_japanese_identical() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake jp, off;
    jp.boot();
    off.boot();
    jp.lifetime_on = true;  // the switch is on; G+0x5C == 0
    jp.w32(kVoiceLanguage, 0u);
    for (Fake* f : {&jp, &off}) f->records({{5000, 100, 2, 7}, {5001, 100, 60, 8}});
    for (int frame = 0; frame < 15; ++frame) {
        if (frame == 0) {
            for (Fake* f : {&jp, &off}) {
                open_part(*f, 5, dt);
                set_voice(*f, 7);
                f->w32(kManager + kMgrQueueCount, 1u);
                f->queue(0, 5, 0, 1);
            }
        } else {
            task_update(jp, kTask, dt);
            task_update(off, kTask, dt);
        }
        for (u32 i = 0; i < 0x34u; ++i) CHECK(jp.r8(kManager + i) == off.r8(kManager + i));
        CHECK(jp.draws == off.draws);
    }
    CHECK(jp.lifetime.counters.lingers == 0u && jp.lifetime.counters.gates == 0u);
}

// A voice in overrun (linger): a line of lower or equal priority is held and starts when the voice ends.
void linger_setup(Fake& f, double dt, s32 samples = 240000) {
    boot_english(f);
    f.samples_answer = samples;
    f.records({{5000, 200, 2, 7}, {6000, 100, 60, 9}, {7000, 200, 60, -1}, {8000, 250, 60, -1}});
    open_part(f, 5, dt);
    set_voice(f, 7);
    task_update(f, kTask, dt);
    task_update(f, kTask, dt);  // the idle path sets the priority word to -1, as the original
    CHECK(f.lifetime.linger_active);
}

void test_hold_basic() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    linger_setup(f, dt);
    f.log.clear();
    CHECK(enqueue(f, kManager, 6, 0, 0) == 6000);  // lower than 200: held, the value of an accepted request
    CHECK(f.lifetime.hold_pending && f.lifetime.counters.holds == 1u);
    CHECK(f.count() == 0u);
    for (const auto& l : f.log) CHECK(l.rfind("stop", 0) != 0);  // nothing was reset or stopped
    CHECK(is_finished(f, kManager, 6000) == 0u);
    // The voice ends: released and accepted before the open decision.
    f.w8(kHandle0 + 1u, 5);
    task_update(f, kTask, dt);
    CHECK(!f.lifetime.hold_pending && f.lifetime.counters.holds_released == 1u);
    CHECK(f.current() == 6 && f.r32(kManager + kMgrVoice) == 9u);
}

void test_hold_higher_passes() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    linger_setup(f, dt);
    CHECK(enqueue(f, kManager, 6, 0, 0) == 6000);
    // Strictly higher than the voice's line: the original's accept, the reset cuts at once and drops the held request.
    CHECK(enqueue(f, kManager, 8, 0, 0) == 8000);
    CHECK(!f.lifetime.hold_pending && f.lifetime.counters.holds_dropped == 1u);
    CHECK(f.count() == 36u);
    CHECK(f.log_has("stop " + Fake::hex(kHandle0)));
}

void test_hold_replace_refuse() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    linger_setup(f, dt);
    CHECK(enqueue(f, kManager, 7, 0, 0) == 7000);       // 200 <= 200: held
    CHECK(enqueue(f, kManager, 6, 0, 0) == -1);         // 100 < 200 held: refused
    CHECK(f.lifetime.counters.holds_refused == 1u);
    CHECK(f.lifetime.hold_key == 7000);
    CHECK(enqueue(f, kManager, 7, 0, 3) == 7000);       // equal to the held one: replaces it
    CHECK(f.lifetime.counters.holds_replaced == 1u && f.lifetime.hold_first_variant == 3);
}

void test_hold_reset_drops() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    linger_setup(f, dt);
    CHECK(enqueue(f, kManager, 6, 0, 0) == 6000);
    reset(f, kManager);
    CHECK(!f.lifetime.hold_pending && f.lifetime.counters.holds_dropped == 1u);
}

void test_hold_lock_raised_while_held() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    linger_setup(f, dt);
    CHECK(enqueue(f, kManager, 6, 0, 0) == 6000);
    f.w32(kManager + kMgrLock, 1u);  // a lock raised while the request waits
    f.w8(kHandle0 + 1u, 5);
    task_update(f, kTask, dt);
    CHECK(f.lifetime.counters.holds_released == 1u && f.lifetime.counters.holds_release_refused == 0u);
    CHECK(f.current() == 6);
    CHECK(f.r32(kManager + kMgrLock) == 1u);  // restored
}

void test_hold_release_refused_counted() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    linger_setup(f, dt);
    CHECK(enqueue(f, kManager, 6, 0, 0) == 6000);
    f.records({{5000, 200, 2, 7}});  // the held line's record is gone: the release's own enqueue refuses it
    f.w8(kHandle0 + 1u, 5);
    task_update(f, kTask, dt);
    CHECK(f.lifetime.counters.holds_released == 0u && f.lifetime.counters.holds_release_refused == 1u);
}

void test_hold_table_window_unchanged() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    boot_english(f);
    f.records({{5000, 200, 600, 7}, {6000, 200, 60, 9}});
    open_part(f, 5, dt);
    set_voice(f, 7);
    // The window is still showing: an equal-priority request does what the original does, it resets at once.
    CHECK(enqueue(f, kManager, 6, 0, 0) == 6000);
    CHECK(!f.lifetime.hold_pending && f.count() == 36u);
}

// Devil Doom 414 (user 2026-11-14: Tails' "Go for it, Shadow!" must play). A line released late from a hold keeps its later parts:
// the next equal-priority line, arriving on its scripted frame while the late line is still showing, waits for it instead of
// cutting its last part.
void deferred_setup(Fake& f, double dt) {
    boot_english(f);
    f.samples_answer = 240000;
    // 5: the overrunning voice's line. 6: parts 0 (voice 9), 1 (no voice), 2 (voice 10, "Go for it"). 7: the next scripted line.
    // 8: strictly higher priority.
    f.records({{5000, 200, 2, 7}, {6000, 200, 2, 9}, {6001, 200, 60, -1}, {6002, 200, 2, 10}, {7000, 200, 60, 11}, {8000, 250, 60, 12},
               {9000, 100, 60, 13}});
    open_part(f, 5, dt);
    set_voice(f, 7);
    task_update(f, kTask, dt);
    task_update(f, kTask, dt);
    CHECK(f.lifetime.linger_active);
    CHECK(enqueue(f, kManager, 6, 0, 0) == 6000);  // during voice 7's overrun: held
    CHECK(f.lifetime.hold_pending);
    f.w8(kHandle0 + 1u, 5);  // voice 7 ends
    task_update(f, kTask, dt);  // released: line 6 opens late, part 0, voice 9
    CHECK(f.current() == 6 && f.r32(kManager + kMgrVoice) == 9u);
    CHECK(f.lifetime.deferred_group == 6 && f.lifetime.counters.deferred_lines == 1u);
    set_voice(f, 9);
}

void test_deferred_keeps_parts() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    deferred_setup(f, dt);
    // Part 0's window ends while voice 9 plays; the voiceless part 1 opens at once.
    for (int i = 0; i < 2; ++i) task_update(f, kTask, dt);
    CHECK(f.current() == 6 && f.r32(kManager + kMgrCurrentVariant) == 1u);
    // The next line arrives on its own frame. The original would reset here and drop part 2; the late line keeps it.
    f.log.clear();
    CHECK(enqueue(f, kManager, 7, 0, 0) == 7000);
    CHECK(f.lifetime.hold_pending && f.lifetime.counters.deferred_holds == 1u);
    for (const auto& l : f.log) CHECK(l.rfind("stop", 0) != 0);
    // Voice 9 ends; part 1 closes; part 2 opens with its own voice.
    f.w8(kHandle0 + 1u, 5);
    for (int i = 0; i < 200 && !(f.current() == 6 && f.r32(kManager + kMgrCurrentVariant) == 2u); ++i) task_update(f, kTask, dt);
    CHECK(f.current() == 6 && f.r32(kManager + kMgrCurrentVariant) == 2u && f.r32(kManager + kMgrVoice) == 10u);
    CHECK(f.lifetime.hold_pending);
    set_voice(f, 10);
    // Part 2's window ends with voice 10 still playing: it lingers and the line still holds the next one.
    for (int i = 0; i < 4; ++i) task_update(f, kTask, dt);
    CHECK(f.lifetime.hold_pending && f.current() != 7);
    // Voice 10 ends: line 6 is finished; line 7 is released and is itself a late line now.
    f.w8(kHandle0 + 1u, 5);
    for (int i = 0; i < 200 && f.current() != 7; ++i) task_update(f, kTask, dt);
    CHECK(f.current() == 7 && f.r32(kManager + kMgrVoice) == 11u);
    CHECK(!f.lifetime.hold_pending && f.lifetime.counters.holds_released == 2u);
    CHECK(f.lifetime.deferred_group == 7 && f.lifetime.counters.deferred_lines == 2u);
}

void test_deferred_higher_interrupts() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    deferred_setup(f, dt);
    CHECK(enqueue(f, kManager, 8, 0, 0) == 8000);  // strictly higher than the late line: the original's reset, at once
    CHECK(!f.lifetime.hold_pending && f.lifetime.deferred_group == -1 && f.count() == 36u);
    CHECK(f.log_has("stop " + Fake::hex(kHandle0)));
}

// A lower-priority request during a late line: refused while the original would still have been showing that line (its parts' table
// windows from the tick its request arrived), and waiting for the late line once the original would have been idle.
void test_deferred_admits_after_original_end() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    deferred_setup(f, dt);  // line 6 held at tick 0: parts 2 + 60 + 2 frames, original end at 64 frames
    CHECK(f.lifetime.deferred_original_end == 64u * kGameTicksPerSecond / 60u);
    f.ticks_now = 63u * kGameTicksPerSecond / 60u;
    CHECK(enqueue(f, kManager, 9, 0, 0) == -1);  // the original was still showing line 6: refused, as the original
    CHECK(!f.lifetime.hold_pending && f.lifetime.counters.deferred_admitted == 0u);
    f.ticks_now = 64u * kGameTicksPerSecond / 60u;
    CHECK(enqueue(f, kManager, 9, 0, 0) == 9000);  // the original was idle: accepted there, waits here
    CHECK(f.lifetime.hold_pending && f.lifetime.counters.deferred_admitted == 1u && f.count() != 36u);
}

void test_hold_failsafe_releases() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    linger_setup(f, dt, 24000);  // a header read: 1 s + 0.5 s margin
    CHECK(enqueue(f, kManager, 6, 0, 0) == 6000);
    f.ticks_now = kGameTicksPerSecond * 2u;
    task_update(f, kTask, dt);
    CHECK(f.lifetime.counters.failsafe_hits == 1u && f.lifetime.counters.holds_released == 1u);
}

void test_hold_japanese_identical() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake jp, off;
    for (Fake* f : {&jp, &off}) {
        f->boot();
        f->records({{5000, 200, 2, 7}, {6000, 100, 60, 9}});
        open_part(*f, 5, dt);
        set_voice(*f, 7);
        task_update(*f, kTask, dt);
    }
    jp.lifetime_on = true;
    jp.w32(kVoiceLanguage, 0u);
    CHECK(enqueue(jp, kManager, 6, 0, 0) == enqueue(off, kManager, 6, 0, 0));
    CHECK(!jp.lifetime.hold_pending);
    CHECK(jp.count() == off.count());
    for (u32 i = 0; i < 0x34u; ++i) CHECK(jp.r8(kManager + i) == off.r8(kManager + i));
}

void test_failsafe() {
    const double dt = static_cast<double>(1.0f / 60.0f);
    Fake f;
    boot_english(f);
    f.records({{5000, 100, 2, 7}, {5001, 100, 60, 8}});
    open_part(f, 5, dt);
    set_voice(f, 7);
    // A short clip: 24000 samples = 1 s; the margin is 0.5 s.
    f.samples_answer = 24000;
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 5, 0, 1);
    task_update(f, kTask, dt);
    f.ticks_now = 1000u;
    task_update(f, kTask, dt);  // reads the header
    CHECK(f.lifetime.gate_active);
    f.ticks_now = kGameTicksPerSecond * 2u;  // past 1 s + 0.5 s from the voice start
    task_update(f, kTask, dt);  // the failsafe declares the stream dead: the part opens
    CHECK(f.lifetime.counters.failsafe_hits == 1u);
    CHECK(f.current() == 5 && f.count() == 0u);
}

void test_priority_idiom() {
    for (s32 a = -128; a <= 127; ++a) {
        for (s32 b = -128; b <= 127; ++b) CHECK(idiom_less(a, b) == (a < b));
    }
}

void test_record_search() {
    Fake f;
    f.boot();
    // Odd/even table sizes around the search's end conditions.
    for (s32 n = 0; n <= 9; ++n) {
        std::vector<std::vector<s32>> rows;
        for (s32 i = 0; i < n; ++i) rows.push_back({(i + 1) * 10, 100, 60, -1});
        f.records(rows);
        for (s32 i = 0; i < n; ++i) {
            CHECK(find_record(f, kFont, 0, 0, (i + 1) * 10) == f.record_at(static_cast<u32>(i)));
            CHECK(find_record(f, kFont, 0, 0, (i + 1) * 10 + 1) == 0u);
        }
        CHECK(find_record(f, kFont, 0, 0, 5) == 0u);
    }
    // Key arithmetic: group * 1000 + number * 100 + variant.
    f.records({{1234 * 1000 + 56 * 100 + 7, 1, 2, 3}});
    CHECK(find_record(f, kFont, 1234, 56, 7) == f.record_at(0));
    CHECK(find_record(f, kFont, 1234, 56, 8) == 0u);
    // No table.
    f.w32(kFont + kFontTable, 0u);
    CHECK(find_record(f, kFont, 1234, 56, 7) == 0u);
    OutWord frames, voice;
    CHECK(record_text(f, kFont, 1234, 56, 7, &frames, &voice) == 0u);
    CHECK(!frames.written && !voice.written);
}

void test_record_text_and_draw() {
    Fake f;
    f.boot();
    f.records({{1000, 5, 60, 7}, {1100, 6, 120, -1}});
    OutWord frames, voice;
    CHECK(record_text(f, kFont, 1, 0, 0, &frames, &voice) == 0x81000001u);
    CHECK(frames.written && frames.value == 60 && voice.written && voice.value == 7);

    voice = {};
    f.log.clear();
    CHECK(draw_message(f, kFont, 1, 1, 0, &voice, 1.5, 2.5) == f.record_at(1));
    CHECK(voice.written && voice.value == -1);
    CHECK(f.log.size() == 1u && f.log[0].rfind("draw ", 0) == 0);

    // number 3 has no text: the draw falls back to number 0 and returns that record.
    voice = {};
    f.log.clear();
    CHECK(draw_message(f, kFont, 1, 3, 0, &voice, 1.0, 1.0) == f.record_at(0));
    CHECK(voice.value == 7);
    CHECK(f.log.size() == 1u);

    // Already number 0: no retry and no draw.
    voice = {};
    f.log.clear();
    CHECK(draw_message(f, kFont, 9, 0, 0, &voice, 1.0, 1.0) == 0u);
    CHECK(f.log.empty() && !voice.written);

    // Messages disabled: nothing at all.
    f.w8(kGlobalMessagesEnabled, 0);
    CHECK(draw_message(f, kFont, 1, 0, 0, &voice, 1.0, 1.0) == 0u);
    CHECK(f.log.empty());
}

void test_enqueue() {
    Fake f;
    f.boot();
    f.records({{5000, 100, 60, 7}, {6000, 50, 60, 8}});
    // Locked.
    f.w32(kManager + kMgrLock, 1u);
    CHECK(enqueue(f, kManager, 5, 0, 0) == -1);
    f.w32(kManager + kMgrLock, 0u);
    // A line without a record is refused, not accepted.
    CHECK(enqueue(f, kManager, 77, 0, 0) == -1);
    CHECK(f.count() == 0u);
    // Accepted: 36 lines from variant 0.
    f.log.clear();
    CHECK(enqueue(f, kManager, 5, 0, 0) == 5000);
    CHECK(f.count() == 36u);
    CHECK(static_cast<s32>(f.r32(kQueue + 35u * 12u + 8u)) == 35);
    // The reset stops mgr + 0x20: here -1, which matches the idle slots (both stopped).
    CHECK(f.log_has("stop " + Fake::hex(kHandle0)) && f.log_has("stop " + Fake::hex(kHandle1)));
    // Lower priority than the current line: refused, nothing stopped.
    f.w32(kManager + kMgrPriority, 200u);
    f.log.clear();
    CHECK(enqueue(f, kManager, 6, 0, 0) == -1);
    CHECK(f.log.empty());
    // Equal priority is accepted and replaces.
    f.w32(kManager + kMgrPriority, 100u);
    CHECK(enqueue(f, kManager, 5, 0, 33) == 5000);
    CHECK(f.count() == 3u);
    // A dead font: the priority test is skipped.
    f.w32(kControl + 4u, 0u);
    CHECK(enqueue(f, kManager, 77, 1, 0) == 77100);
    // The use count returns to where it was.
    f.w32(kControl + 4u, 1u);
    CHECK(enqueue(f, kManager, 5, 0, 0) == 5000);
    CHECK(f.r32(kControl + 4u) == 1u);
}

void test_random_enqueue() {
    Fake f;
    f.boot();
    f.records({{5000, 100, 60, 7}, {5001, 100, 60, 7}, {5002, 100, 60, 7}});
    f.next_rand = 2;
    CHECK(enqueue_random(f, kManager, 5) == 5000);
    CHECK(f.log_has("rand 3"));
    CHECK(f.count() == 1u);
    CHECK(static_cast<s32>(f.r32(kQueue + 8u)) == 2);
    // No line: -1 (the first lookup failing leaves lines == 0, and the record test refuses).
    CHECK(enqueue_random(f, kManager, 6) == -1);
    // 36 lines: refused.
    std::vector<std::vector<s32>> rows;
    for (s32 i = 0; i < 36; ++i) rows.push_back({7000 + i, 100, 60, -1});
    f.records(rows);
    CHECK(enqueue_random(f, kManager, 7) == -1);
}

void test_queue_erase_and_finished() {
    Fake f;
    f.boot();
    f.w32(kManager + kMgrQueueCount, 3u);
    f.queue(0, 1, 2, 3);
    f.queue(1, 4, 5, 6);
    f.queue(2, 7, 8, 9);
    queue_erase(f, kManager, kQueue);
    CHECK(f.count() == 2u);
    CHECK(f.r32(kQueue) == 4u && f.r32(kQueue + 12u + 8u) == 9u);
    // The last entry: nothing moved.
    queue_erase(f, kManager, kQueue + 12u);
    CHECK(f.count() == 1u);

    // is_finished: a negative key answers "the queue is not empty".
    CHECK(is_finished(f, kManager, -1) == 1u);
    f.w32(kManager + kMgrQueueCount, 0u);
    CHECK(is_finished(f, kManager, -1) == 0u);
    // The current line, by group and number only.
    f.w32(kManager + kMgrCurrent, 5u);
    f.w32(kManager + kMgrCurrentNumber, 1u);
    CHECK(is_finished(f, kManager, 5100) == 0u);
    CHECK(is_finished(f, kManager, 5000) == 1u);
    // A queued line.
    f.w32(kManager + kMgrQueueCount, 2u);
    f.queue(0, 9, 0, 0);
    f.queue(1, 5, 0, 0);
    CHECK(is_finished(f, kManager, 5000) == 0u);
    CHECK(is_finished(f, kManager, 9000) == 0u);
    CHECK(is_finished(f, kManager, 9001) == 1u);
}

void test_growth_fails_closed() {
    Fake f;
    f.boot();
    f.records({{5000, 100, 60, 7}});
    f.w32(kManager + kMgrQueueCapacity, 2u);
    bool stopped = false;
    try {
        enqueue(f, kManager, 5, 0, 0);
    } catch (const UnportedPath&) {
        stopped = true;
    }
    CHECK(stopped);
}

float accumulate(float limit_probe, u32 steps, double dt) {
    float elapsed = 0.0f;
    (void)limit_probe;
    for (u32 i = 0; i < steps; ++i) elapsed = static_cast<float>(static_cast<double>(elapsed) + dt);
    return elapsed;
}

void test_window_timing() {
    Fake f;
    f.boot();
    f.records({{5000, 100, 3, 7}});
    const double dt = static_cast<double>(1.0f / 60.0f);
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 5, 0, 0);
    // Opens: the same update counts the first step and requests the voice.
    f.log.clear();
    task_update(f, kTask, dt);
    CHECK(f.current() == 5);
    CHECK(f.count() == 0u);
    CHECK(f.r32(kTask + kTaskRecord) == f.record_at(0));
    CHECK(f.r8(kTask + kTaskNewGroup) == 1u);
    CHECK(f.r32(kManager + kMgrPriority) == 100u);
    CHECK(std::bit_cast<u32>(f.rf(kTask + kTaskElapsed)) == std::bit_cast<u32>(accumulate(0, 1, dt)));
    CHECK(f.r32(kManager + kMgrVoice) == 7u);
    CHECK(f.log_has("start " + Fake::hex(kHandle0) + " 0 7"));
    // Stop first, then play: the stop of the previous voice comes before the start.
    size_t stop_at = f.log.size(), start_at = f.log.size();
    for (size_t i = 0; i < f.log.size(); ++i) {
        if (f.log[i].rfind("stop", 0) == 0 && stop_at == f.log.size()) stop_at = i;
        if (f.log[i].rfind("start", 0) == 0) start_at = i;
    }
    CHECK(stop_at < start_at);

    // Updates 2 and 3: limit = 3 / 60 = 0.05 f, reached exactly when the float sum says so.
    task_update(f, kTask, dt);
    CHECK(f.current() == 5);
    const float limit = 3.0f / 60.0f;
    task_update(f, kTask, dt);
    const float sum3 = accumulate(0, 3, dt);
    CHECK(std::bit_cast<u32>(f.rf(kTask + kTaskElapsed)) == std::bit_cast<u32>(sum3));
    CHECK(f.current() == (sum3 >= limit ? -1 : 5));
    if (f.current() != -1) {
        task_update(f, kTask, dt);
        CHECK(f.current() == -1);
    }
    // The close touches no audio.
    f.log.clear();
    task_update(f, kTask, dt);
    CHECK(f.r32(kManager + kMgrPriority) == 0xFFFFFFFFu);  // idle
    for (const auto& l : f.log) CHECK(l.rfind("stop", 0) != 0 && l.rfind("start", 0) != 0);
}

void test_update_selection() {
    Fake f;
    f.boot();
    f.records({{1000, 100, 60, 1}, {2000, 100, 60, 2}, {3000, 50, 60, 3}, {4000, 200, 60, -1}});
    const double dt = static_cast<double>(1.0f / 60.0f);
    // Frozen: nothing at all.
    f.w8(kFreezeBlock + kFreezeB, 1);
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 1, 0, 0);
    task_update(f, kTask, dt);
    CHECK(f.count() == 1u && f.r32(kTask + kTaskTextX) == 0u);
    f.w8(kFreezeBlock + kFreezeB, 0);
    task_update(f, kTask, dt);
    CHECK(f.current() == 1);
    // Equal priority, same group: waits.
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 1, 0, 0);
    f.w32(kManager + kMgrCurrent, 1u);
    task_update(f, kTask, dt);
    CHECK(f.count() == 1u);
    // Equal priority, other group: replaces.
    f.queue(0, 2, 0, 0);
    task_update(f, kTask, dt);
    CHECK(f.current() == 2 && f.count() == 0u);
    CHECK(f.r32(kManager + kMgrVoice) == 2u);
    // Lower: dropped.
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 3, 0, 0);
    task_update(f, kTask, dt);
    CHECK(f.current() == 2 && f.count() == 0u);
    // Higher and silent: opens, the previous voice is not stopped.
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 4, 0, 0);
    f.log.clear();
    task_update(f, kTask, dt);
    CHECK(f.current() == 4 && f.r32(kManager + kMgrVoice) == 2u);
    for (const auto& l : f.log) CHECK(l.rfind("stop", 0) != 0);
    // A missing line: dropped, one per update.
    f.w32(kManager + kMgrQueueCount, 2u);
    f.queue(0, 9, 0, 0);
    f.queue(1, 9, 0, 1);
    task_update(f, kTask, dt);
    CHECK(f.count() == 1u);
    // Draw off: the lookup path gives the same voice and timing, and no draw call.
    f.w8(kManager + kMgrDrawEnable, 0);
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 1, 0, 0);
    f.w32(kManager + kMgrCurrent, 0xFFFFFFFFu);
    f.log.clear();
    task_update(f, kTask, dt);
    CHECK(f.current() == 1 && f.r32(kManager + kMgrVoice) == 1u);
    for (const auto& l : f.log) CHECK(l.rfind("draw", 0) != 0);
    f.w8(kManager + kMgrDrawEnable, 1);
    // Messages disabled globally: the window closes at once.
    f.w8(kGlobalMessagesEnabled, 0);
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 1, 0, 0);
    f.w32(kManager + kMgrCurrent, 0xFFFFFFFFu);
    task_update(f, kTask, dt);
    CHECK(f.current() == -1 && f.r32(kTask + kTaskRecord) == 0u);
    // A dead font: the update stops after the layout.
    f.w8(kGlobalMessagesEnabled, 1);
    f.w32(kControl + 4u, 0u);
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 2, 0, 0);
    task_update(f, kTask, dt);
    CHECK(f.count() == 1u);
}

void test_layout() {
    Fake f;
    f.boot();
    const double dt = 1.0 / 60.0;
    task_update(f, kTask, dt);
    CHECK(f.r32(kTask + kTaskTextX) == 0x3F0F5C29u && f.r32(kTask + kTaskTextY) == 0x3E3851ECu);
    CHECK(f.r32(kTask + kTaskTextScale) == 0x3D1BA5E3u);
    f.w32(kGlobalTextLanguage, 0u);
    task_update(f, kTask, dt);
    CHECK(f.r32(kTask + kTaskTextScale) == 0x3D23D70Au);
    f.w8(kGlobalAltLayout, 1);
    task_update(f, kTask, dt);
    CHECK(f.r32(kTask + kTaskTextX) == 0x3F0CCCCDu && f.r32(kTask + kTaskTextY) == 0x3E6147AEu);
    f.w8(kManager + kMgrAltLayout, 1);
    task_update(f, kTask, dt);
    CHECK(f.r32(kTask + kTaskTextX) == 0x3F1C28F6u);
    // The default length.
    f.records({{1000, 1, 0, -1}});
    f.w32(kManager + kMgrQueueCount, 1u);
    f.queue(0, 1, 0, 0);
    task_update(f, kTask, dt);
    for (int i = 0; i < 400 && f.current() == 1; ++i) {
        CHECK(!(f.rf(kTask + kTaskElapsed) >= 3.0f));
        task_update(f, kTask, dt);
    }
    CHECK(f.current() == -1);
}

void test_ctor_dtor_reinit() {
    Fake f;
    f.boot();
    f.w32(dv::kSerifSlots, kSerif0);
    CHECK(task_ctor(f, kTask, 0x8080A000u) == kTask);
    CHECK(f.r32(kTask + dv::kTaskVtable) == 0x805437D4u);
    CHECK(f.r32(kTask + dv::kTaskName) == 0x804ECE58u);
    CHECK(f.r16(kTask + dv::kTaskFlags) == 0x100u);
    CHECK(f.r8(kManager + kMgrDrawEnable) == 1u);
    CHECK(f.current() == -1);

    f.log.clear();
    CHECK(task_dtor(f, kTask, 1u) == kTask);
    CHECK(f.log_has("task_dtor " + Fake::hex(kTask) + " 0"));
    CHECK(f.log_has("delete " + Fake::hex(kTask)));
    f.log.clear();
    task_dtor(f, kTask, 0u);
    CHECK(!f.log_has("delete " + Fake::hex(kTask)));
    CHECK(task_dtor(f, 0u, 1u) == 0u);

    // Reinit: the old task is killed, a new one made, the old weak count released, the new one taken.
    const u32 kNewCount = 0x8080B000u, kShared = 0x8080C000u;
    f.w32(kManager + kMgrTask, kTask);
    f.w16(kTask + dv::kTaskFlags, 0x100u);
    f.w32(kShared, 0x8080D000u);
    f.w32(kShared + 4u, kNewCount);
    f.w32(kNewCount + 8u, 5u);
    f.w32(kControl + 8u, 1u);  // the last weak reference: destroyed
    f.w32(f.parent_block + 0x54u, 0x8080E000u);
    f.log.clear();
    reinit(f, kManager, kShared);
    CHECK((f.r16(kTask + dv::kTaskFlags) & 1u) == 1u);
    CHECK(f.log_has("control_destroy " + Fake::hex(kControl)));
    CHECK(f.r32(kManager + kMgrFont) == 0x8080D000u && f.r32(kManager + kMgrFontCount) == kNewCount);
    CHECK(f.r32(kNewCount + 8u) == 6u);
    CHECK(f.log_has("new 00000040"));
    CHECK(f.log_has("task_ctor " + Fake::hex(kNew) + " 8080E000"));
    CHECK(f.r32(kManager + kMgrTask) == kNew);
    CHECK(f.r32(kManager + kMgrVoice) == 0xFFFFFFFFu && f.r32(kManager + kMgrPriority) == 0xFFFFFFFFu);
    CHECK(f.r8(kManager + kMgrAltLayout) == 0u);

    // A failed allocation still stores the null task.
    f.next_new = 0u;
    reinit(f, kManager, kShared);
    CHECK(f.r32(kManager + kMgrTask) == 0u);
}

void test_lazy_singletons() {
    Fake f;
    f.boot();
    f.w32(kManagerPointer, 0u);
    f.w8(kManagerGuard, 0);
    CHECK(manager(f) == kManager);
    CHECK(f.log.size() == 2u);
    CHECK(f.log_has("mgr_construct 80580B18"));
    CHECK(f.log_has("atexit 80580B18 8010A1B8 80580AFC"));
    CHECK(f.r8(kManagerGuard) == 1u);
    f.log.clear();
    f.w32(kManagerPointer, 0u);
    CHECK(manager(f) == kManager);
    CHECK(f.log.empty());  // the guard is already set: no second construction
    // The sound manager is built by the first reset that needs it.
    f.w32(kSoundManagerPointer, 0u);
    f.log.clear();
    reset(f, kManager);
    CHECK(f.log_has("snd_construct"));
}

void test_slot_choice() {
    Fake f;
    f.boot();
    using dv::kSlotPriority;
    using dv::kSlotState;
    // Idle: the first one.
    CHECK(dv::choose_slot(f, -1, -1) == 0);
    // Slot 0 playing, 1 idle.
    f.w8(kSerif0 + kSlotState, 1);
    CHECK(dv::choose_slot(f, -1, -1) == 1);
    // Both playing, low priorities: the later slot.
    f.w8(kSerif1 + kSlotState, 1);
    CHECK(dv::choose_slot(f, -1, -1) == 1);
    // Slot 1 refuses (priority 3), slot 0 serves.
    f.w8(kSerif1 + kSlotPriority, 3);
    f.w8(kSerif0 + kSlotPriority, static_cast<u8>(-1));
    CHECK(dv::choose_slot(f, -1, -1) == 0);
    // Both refuse: -1.
    f.w8(kSerif0 + kSlotPriority, 3);
    CHECK(dv::choose_slot(f, -1, -1) == -1);
    // Positive priority: only a strictly higher request.
    f.w8(kSerif0 + kSlotPriority, 5);
    f.w8(kSerif1 + kSlotPriority, 5);
    CHECK(dv::choose_slot(f, -1, 5) == -1);
    CHECK(dv::choose_slot(f, -1, 6) == 0);  // both eligible; the later one wins only if it can replace the best (5 < 5: no)
    // Same kind: the first such slot decides.
    f.w32(kSerif0 + dv::kSlotKind, 9u);
    f.w8(kSerif0 + kSlotPriority, 3);
    CHECK(dv::choose_slot(f, 9, 7) == -1);
    f.w8(kSerif0 + kSlotPriority, 0);
    CHECK(dv::choose_slot(f, 9, 7) == 0);
}

void test_play_and_stop() {
    Fake f;
    f.boot();
    f.log.clear();
    CHECK(dv::request_play(f, 42, 0xFFFFFFFFu, -1) == 0);
    CHECK(static_cast<s32>(f.r32(kSerif0 + dv::kSlotVoice)) == 42);
    CHECK(f.r8(kSerif0 + dv::kSlotState) == 1u);
    CHECK(f.r8(kSerif0 + dv::kSlotPriority) == 0xFFu);
    CHECK(f.log.size() == 3u);
    CHECK(f.log[0] == "stop " + Fake::hex(kHandle0));
    CHECK(f.log[1] == "pause " + Fake::hex(kHandle0) + " 0");
    CHECK(f.log[2] == "start " + Fake::hex(kHandle0) + " 0 42");

    f.log.clear();
    dv::stop_by_id(f, 42);
    CHECK(static_cast<s32>(f.r32(kSerif0 + dv::kSlotVoice)) == -1 && f.r8(kSerif0 + dv::kSlotState) == 0u);
    CHECK(f.log.size() == 2u);  // slot 1's id is -1, not 42: untouched
    // Stop of -1 hits every idle slot.
    f.log.clear();
    dv::stop_by_id(f, -1);
    CHECK(f.log.size() == 4u);

    f.w32(kSerif0 + dv::kSlotVoice, 5u);
    f.w8(kSerif1 + dv::kSlotPauseFlags, 3);
    f.log.clear();
    dv::stop_all(f);
    CHECK(f.log.size() == 4u);
    CHECK(f.r8(kSerif1 + dv::kSlotPauseFlags) == 2u);  // only bit 0 is cleared
}

void test_serif_update() {
    Fake f;
    f.boot();
    f.w8(kSerif0 + dv::kSlotState, 1);
    f.w32(kSerif0 + dv::kSlotVoice, 3u);
    f.w32(kSerif0 + dv::kSlotKind, 7u);
    f.w8(kSerif0 + dv::kSlotPriority, 0);
    f.stat_for_handle0 = 3;
    dv::serif_update(f, kSerif0);
    CHECK(f.r8(kSerif0 + dv::kSlotState) == 1u && f.r32(kSerif0 + dv::kSlotStatus) == 3u);
    for (s32 stat : {0, 5}) {
        f.w8(kSerif0 + dv::kSlotState, 1);
        f.stat_for_handle0 = stat;
        dv::serif_update(f, kSerif0);
        CHECK(f.r8(kSerif0 + dv::kSlotState) == 0u);
        CHECK(static_cast<s32>(f.r32(kSerif0 + dv::kSlotVoice)) == -1);
        CHECK(f.r8(kSerif0 + dv::kSlotPriority) == 0xFFu);
    }
    // Status 6 (error) does not end the state: the slot stays busy.
    f.w8(kSerif0 + dv::kSlotState, 1);
    f.stat_for_handle0 = 6;
    dv::serif_update(f, kSerif0);
    CHECK(f.r8(kSerif0 + dv::kSlotState) == 1u);
    // Idle: the status is still read.
    f.w8(kSerif0 + dv::kSlotState, 0);
    f.stat_for_handle0 = 2;
    dv::serif_update(f, kSerif0);
    CHECK(f.r32(kSerif0 + dv::kSlotStatus) == 2u);
    // Any other state: nothing.
    f.w8(kSerif0 + dv::kSlotState, 2);
    f.log.clear();
    dv::serif_update(f, kSerif0);
    CHECK(f.log.empty());
}

void test_pause_resume() {
    Fake f;
    f.boot();
    f.w32(dv::kBgmObjects, kBgm0);
    f.w32(dv::kBgmObjects + 4u, kBgm1);
    f.w32(kBgm0 + dv::kBgmAdxt, kBgmHandle0);
    f.w32(kBgm1 + dv::kBgmAdxt, kBgmHandle1);
    f.w8(kBgm0 + dv::kBgmState, 1);  // state 1: flag only
    f.w8(kBgm1 + dv::kBgmState, 3);  // 2..6: paused too
    f.w8(kSerif0 + dv::kSlotState, 1);
    dv::pause_all(f);
    CHECK(f.r8(kBgm0 + dv::kBgmPauseFlags) == 1u && f.r8(kBgm1 + dv::kBgmPauseFlags) == 1u);
    CHECK(f.r8(kSerif0 + dv::kSlotPauseFlags) == 1u && f.r8(kSerif1 + dv::kSlotPauseFlags) == 0u);
    CHECK(f.log_has("pause " + Fake::hex(kBgmHandle1) + " 1"));
    CHECK(!f.log_has("pause " + Fake::hex(kBgmHandle0) + " 1"));
    CHECK(f.log_has("pause " + Fake::hex(kHandle0) + " 1"));
    f.log.clear();
    dv::resume_all(f);
    CHECK(f.r8(kBgm0 + dv::kBgmPauseFlags) == 0u && f.r8(kBgm1 + dv::kBgmPauseFlags) == 0u);
    CHECK(f.log_has("pause " + Fake::hex(kBgmHandle1) + " 0"));
    CHECK(!f.log_has("pause " + Fake::hex(kBgmHandle0) + " 0"));
    CHECK(f.log_has("pause " + Fake::hex(kHandle0) + " 0"));
    CHECK(f.r8(kSerif0 + dv::kSlotPauseFlags) == 0u);
}

void test_serif_ctor_alloc() {
    Fake f;
    f.boot();
    f.w32(dv::kSerifTaskName, 0x804D15F0u);
    CHECK(dv::serif_ctor(f, kSerif0, 0x80900000u, 1, 0x80910000u, 0u) == kSerif0);
    CHECK(f.r32(kSerif0 + dv::kTaskVtable) == 0x8053DFDCu);
    CHECK(f.r32(kSerif0 + dv::kTaskName) == 0x804D15F0u);
    CHECK((f.r16(kSerif0 + dv::kTaskFlags) & 0x100u) != 0u);
    CHECK(f.r8(kSerif0 + dv::kSlotIndex) == 1u);
    CHECK(f.r32(kSerif0 + dv::kSlotVolume) == 0xFFFFFFD8u);
    CHECK(f.log_has("create 1 80910000 00017124"));
    CHECK(f.log_has("vol " + Fake::hex(kHandle0) + " -40"));

    f.create_result = 0u;
    f.bytes.erase(kSerif1 + dv::kTaskFlags);
    f.bytes.erase(kSerif1 + dv::kTaskFlags + 1u);
    dv::serif_ctor(f, kSerif1, 0x80900000u, 0, 0x80920000u, 0u);
    CHECK((f.r16(kSerif1 + dv::kTaskFlags) & 1u) == 1u);

    f.log.clear();
    dv::allocate_work_areas(f);
    CHECK(f.log[0] == "cri5750 0" && f.log[1] == "cri5118 0" && f.log[2] == "cri72D8");
    CHECK(f.r32(dv::kBgmWork) == 0x80800000u + 0x389E4u);
    CHECK(f.r32(dv::kSerifWork + 4u) == 0x80800000u + 0x17124u);
    CHECK(f.r32(dv::kLoadedVoiceLanguage) == 6u);
}

}  // namespace

int main() {
    test_priority_idiom();
    test_record_search();
    test_record_text_and_draw();
    test_enqueue();
    test_random_enqueue();
    test_queue_erase_and_finished();
    test_growth_fails_closed();
    test_window_timing();
    test_update_selection();
    test_layout();
    test_ctor_dtor_reinit();
    test_lazy_singletons();
    test_slot_choice();
    test_play_and_stop();
    test_serif_update();
    test_pause_resume();
    test_serif_ctor_alloc();
    test_linger();
    test_linger_reset_ends();
    test_linger_words_identical();
    test_gate();
    test_gate_expired_while_waiting();
    test_gate_voiceless_part();
    test_gate_only_with_no_window();
    test_enqueue_during_linger();
    test_japanese_identical();
    test_failsafe();
    test_hold_basic();
    test_hold_higher_passes();
    test_hold_replace_refuse();
    test_hold_reset_drops();
    test_hold_lock_raised_while_held();
    test_hold_release_refused_counted();
    test_hold_table_window_unchanged();
    test_hold_failsafe_releases();
    test_hold_japanese_identical();
    test_deferred_keeps_parts();
    test_deferred_higher_interrupts();
    test_deferred_admits_after_original_end();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
