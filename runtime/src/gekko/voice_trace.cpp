// Dialogue voice + subtitle window trace. Contract and event list: include/gekko/voice_trace.h.

#include "gekko/voice_trace.h"

#if !defined(GEKKO_SHIPPING)

#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>

#include "gekko/cpu.h"

namespace gekko::voice_trace {
namespace {

// Message manager singleton (fn_8010A14C returns it): +0x14 current message id (-1 none), +0x18 / +0x1C its two
// parameters, +0x20 the voice id it plays, +0x28 its priority.
constexpr u32 kManager = 0x80580B18u;
constexpr u32 kFrameCounter = 0x8057E7F0u;
// MessageTask (r3 of fn_801FB5AC): +0x28 float seconds shown, +0x30 the message data (+0x8 priority, +0xC duration in
// frames, 0 = 180, +0x10 voice id). fn_801FB5AC's f1 is the frame step in seconds.
constexpr u32 kMessageUpdate = 0x801FB5ACu;
constexpr u32 kMessageEnqueue = 0x801FB210u;
constexpr u32 kMessageQueuePop = 0x801FBA18u;
constexpr u32 kVoiceRequest = 0x801D5540u;
constexpr u32 kVoiceStopRequest = 0x801D551Cu;
constexpr u32 kVoiceSlotUpdate = 0x801D6E14u;
constexpr u32 kAdxStartAfs = 0x803C3E40u;
constexpr u32 kAdxStartName = 0x803C3D94u;
constexpr u32 kAdxStop = 0x803C2F90u;
// Small-data tables (r13-relative): two TObjSERIF slots at -0x2930 (ADXT handle at +0x3C), two TObjBGM at -0x2940
// (ADXT handle at +0x44). Both: +0x29 slot state, +0x2C voice id / track.
constexpr int kVoiceTable = -0x2930;
constexpr int kBgmTable = -0x2940;

struct Sink {
    std::FILE* file = nullptr;
    std::mutex mutex;
    std::chrono::steady_clock::time_point origin;
    bool started = false;
};

Sink& sink() {
    static Sink s;
    return s;
}

bool plausible(u32 a) noexcept { return a >= 0x80000000u && a < 0x81800000u; }

// One line; fields are "key=value" pairs formatted by the caller.
void emit(Cpu& cpu, const char* event, const char* fmt, ...) {
    Sink& s = sink();
    char body[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started) {
        s.started = true;
        s.origin = std::chrono::steady_clock::now();
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s.origin).count();
    std::fprintf(s.file, "%.3f\t%llu\t%u\t%s\t%s\n", ms, static_cast<unsigned long long>(cpu.game_clock().now()),
                 cpu.mem_read32(kFrameCounter), event, body);
    std::fflush(s.file);
}

// Up to three return addresses above the current function: lr is the caller's resume address; the next ones are read
// from the saved-LR slots of the stack frames (back chain at 0(r1), saved LR at 4(frame above)).
void callers(Cpu& cpu, u32 out[3]) {
    out[0] = cpu.lr;
    out[1] = out[2] = 0;
    u32 frame = cpu.gpr[1];
    for (int i = 1; i < 3; ++i) {
        if (!plausible(frame)) break;
        const u32 up = cpu.mem_read32(frame);  // back chain
        if (!plausible(up)) break;
        out[i] = cpu.mem_read32(up + 4u);
        frame = up;
    }
}

const char* handle_name(Cpu& cpu, u32 handle) {
    const u32 r13 = cpu.gpr[13];
    struct Entry {
        int table, index;
        u32 offset;
        const char* name;
    };
    static constexpr Entry entries[] = {{kVoiceTable, 0, 0x3Cu, "voice0"}, {kVoiceTable, 1, 0x3Cu, "voice1"},
                                        {kBgmTable, 0, 0x44u, "bgm0"},     {kBgmTable, 1, 0x44u, "bgm1"}};
    for (const Entry& e : entries) {
        const u32 object = cpu.mem_read32(r13 + static_cast<u32>(e.table) + 4u * static_cast<u32>(e.index));
        if (plausible(object) && cpu.mem_read32(object + e.offset) == handle) return e.name;
    }
    return "other";
}

std::string c_string(Cpu& cpu, u32 address) {
    std::string out;
    if (!plausible(address)) return "?";
    for (int i = 0; i < 96; ++i) {
        const char c = static_cast<char>(cpu.mem_read8(address + static_cast<u32>(i)));
        if (c == 0) break;
        out.push_back((c >= 32 && c < 127) ? c : '?');
    }
    return out;
}

// ---- polled state ---------------------------------------------------------------------------------------------

struct Window {
    bool open = false;
    s32 id = -1;
    u32 a = 0, b = 0;
    u64 open_ticks = 0;
    u32 open_frame = 0;
    float last_timer = 0.0f;
    u32 duration_frames = 0;
    s32 voice = -1;
};
Window g_window;

struct Watch {
    bool seen = false;
    u32 stat = 255, state = 255;
    s32 id = 0;
};
Watch g_watch[4];

void poll_streams(Cpu& cpu) {
    const u32 r13 = cpu.gpr[13];
    struct Entry {
        int table, index;
        u32 handle_offset;
        const char* name;
    };
    static constexpr Entry entries[] = {{kVoiceTable, 0, 0x3Cu, "voice0"}, {kVoiceTable, 1, 0x3Cu, "voice1"},
                                        {kBgmTable, 0, 0x44u, "bgm0"},     {kBgmTable, 1, 0x44u, "bgm1"}};
    for (int i = 0; i < 4; ++i) {
        const Entry& e = entries[i];
        const u32 object = cpu.mem_read32(r13 + static_cast<u32>(e.table) + 4u * static_cast<u32>(e.index));
        if (!plausible(object)) continue;
        const u32 handle = cpu.mem_read32(object + e.handle_offset);
        const u32 stat = plausible(handle) ? cpu.mem_read8(handle + 1u) : 255u;
        const u32 state = cpu.mem_read8(object + 0x29u);
        const s32 id = static_cast<s32>(cpu.mem_read32(object + 0x2Cu));
        Watch& w = g_watch[i];
        if (w.seen && w.stat == stat && w.state == state && w.id == id) continue;
        w = {true, stat, state, id};
        emit(cpu, "adx_stat", "slot=%s\tstat=%u\tslot_state=%u\tid=%d\thandle=0x%08X", e.name, stat, state, id, handle);
    }
}

void poll_window(Cpu& cpu, u32 task, bool entry) {
    const s32 id = static_cast<s32>(cpu.mem_read32(kManager + 0x14u));
    const u32 a = cpu.mem_read32(kManager + 0x18u), b = cpu.mem_read32(kManager + 0x1Cu);
    const u32 data = plausible(task) ? cpu.mem_read32(task + 0x30u) : 0u;
    const float timer = plausible(task) ? std::bit_cast<float>(cpu.mem_read32(task + 0x28u)) : 0.0f;
    u32 frames = 0;
    s32 voice = -1;
    s32 priority = 0;
    if (plausible(data)) {
        frames = cpu.mem_read32(data + 0xCu);
        voice = static_cast<s32>(cpu.mem_read32(data + 0x10u));
        priority = static_cast<s32>(cpu.mem_read32(data + 0x8u));
    }
    const bool now_open = id >= 0;
    const bool changed = g_window.open && (!now_open || g_window.id != id || g_window.a != a || g_window.b != b);
    if (changed) {
        const u64 ticks = cpu.game_clock().now();
        emit(cpu, "win_close",
             "id=%d\ta=%u\tb=%u\tlifetime_ticks=%llu\tlifetime_ms=%.1f\tlast_timer_s=%.4f\tduration_frames=%u\tvoice=%d\t%s",
             g_window.id, g_window.a, g_window.b, static_cast<unsigned long long>(ticks - g_window.open_ticks),
             1000.0 * static_cast<double>(ticks - g_window.open_ticks) / 40500000.0, g_window.last_timer,
             g_window.duration_frames, g_window.voice, now_open ? "replaced" : "ended");
        g_window.open = false;
    }
    if (now_open && !g_window.open) {
        g_window = {true, id, a, b, cpu.game_clock().now(), cpu.mem_read32(kFrameCounter), timer, frames, voice};
        emit(cpu, "win_open", "id=%d\ta=%u\tb=%u\tduration_frames=%u\tvoice=%d\tpriority=%d\tmgr_voice=%d", id, a, b,
             frames, voice, priority, static_cast<s32>(cpu.mem_read32(kManager + 0x20u)));
    } else if (now_open) {
        g_window.last_timer = timer;
        g_window.duration_frames = frames;
        g_window.voice = voice;
    }
    (void)entry;
}

// Run `observer` first, then whatever handler an earlier install left at the address (never replacing it).
void observe(Cpu& cpu, u32 address, std::function<void(Cpu&)> observer, const char* name) {
    Cpu::HleHandler previous;
    std::string previous_name;
    for (const auto& entry : cpu.hle_entries()) {
        if (entry.address == address) {
            previous = entry.handler;
            previous_name = entry.name;
        }
    }
    cpu.install_hle(
        address,
        [observer = std::move(observer), previous](Cpu& c) {
            observer(c);
            return previous ? previous(c) : false;
        },
        previous ? std::string(name) + " + " + previous_name : std::string(name));
}

}  // namespace

bool enabled() noexcept { return sink().file != nullptr; }

void note(Cpu& cpu, const char* event, const char* text) {
    if (sink().file == nullptr) return;
    emit(cpu, event, "%s", text);
}

void note_voice_stop(Cpu& cpu, s32 voice, u32 caller, u32 caller2) {
    if (sink().file != nullptr) emit(cpu, "voice_stop_req", "voice=%d\tcaller=0x%08X\tcaller2=0x%08X", voice, caller, caller2);
}

void note_voice_request(Cpu& cpu, s32 voice, s32 flag, s32 tag, u32 caller, u32 caller2) {
    if (sink().file != nullptr) {
        emit(cpu, "voice_request", "voice=%d\tflag=%d\ttag=%d\tcaller=0x%08X\tcaller2=0x%08X", voice, flag, tag, caller, caller2);
    }
}

void note_queue_pop(Cpu& cpu, u32 entry, u32 link) {
    if (sink().file == nullptr) return;
    // Return addresses inside the MessageTask update (values only, never entered).
    constexpr u32 kBecameCurrent = 0x801FB7A0u;  // liveness-roots: value-only
    constexpr u32 kDroppedLower = 0x801FB7B8u;   // liveness-roots: value-only
    constexpr u32 kDroppedNoText = 0x801FB7C8u;  // liveness-roots: value-only
    const char* why = link == kBecameCurrent ? "became_current"
                      : link == kDroppedLower ? "dropped_lower_priority"
                      : link == kDroppedNoText ? nullptr
                                               : "other";
    if (why == nullptr) return;
    emit(cpu, "msg_pop", "id=%d\ta=%u\tb=%u\tlr=0x%08X\t%s", static_cast<s32>(cpu.mem_read32(entry)), cpu.mem_read32(entry + 4u),
         cpu.mem_read32(entry + 8u), link, why);
}

void install(Cpu& cpu) {
    const char* path = std::getenv("GEKKO_VOICE_TRACE");
    if (path == nullptr || path[0] == '\0') return;
    sink().file = std::fopen(path, "w");
    if (sink().file == nullptr) {
        std::fprintf(stderr, "[voice-trace] cannot write %s\n", path);
        return;
    }
    std::fprintf(sink().file, "# ms\tgame_ticks\tframe\tevent\tfields (40.5 MHz game ticks; see gekko/voice_trace.h)\n");
    std::fprintf(stderr, "[voice-trace] writing %s\n", path);

    observe(cpu, kMessageUpdate,
            [](Cpu& c) {
                poll_window(c, c.gpr[3], true);
                poll_streams(c);
            },
            "voice trace: MessageTask update");
    observe(cpu, kMessageEnqueue,
            [](Cpu& c) {
                u32 who[3];
                callers(c, who);
                emit(c, "msg_enqueue", "group=%u\tnumber=%u\tvariant=%u\tcaller=0x%08X\tcaller2=0x%08X", c.gpr[4], c.gpr[5],
                     c.gpr[7], who[0], who[1]);
            },
            "voice trace: message enqueue");
    observe(cpu, kMessageQueuePop,
            [](Cpu& c) {
                const u32 entry = c.gpr[4];
                if (!plausible(entry)) return;
                // Return addresses inside the MessageTask update: 0x801FB7A0 the entry became the current message,
                // 0x801FB7B8 dropped (lower priority than the current one), 0x801FB7C8 dropped (its text does not
                // exist: a message queues 36 variants and the missing ones are discarded one per frame; not logged).
                const char* why = c.lr == 0x801FB7A0u   ? "became_current"  // liveness-roots: value-only
                                  : c.lr == 0x801FB7B8u ? "dropped_lower_priority"  // liveness-roots: value-only
                                  : c.lr == 0x801FB7C8u ? nullptr  // liveness-roots: value-only
                                                        : "other";
                if (why == nullptr) return;
                emit(c, "msg_pop", "id=%d\ta=%u\tb=%u\tlr=0x%08X\t%s", static_cast<s32>(c.mem_read32(entry)),
                     c.mem_read32(entry + 4u), c.mem_read32(entry + 8u), c.lr, why);
            },
            "voice trace: message queue pop");
    observe(cpu, kVoiceRequest,
            [](Cpu& c) {
                u32 who[3];
                callers(c, who);
                emit(c, "voice_request", "voice=%d\tflag=%d\ttag=%d\tcaller=0x%08X\tcaller2=0x%08X",
                     static_cast<s32>(c.gpr[4]), static_cast<int>(static_cast<s8>(c.gpr[5])), static_cast<s32>(c.gpr[6]),
                     who[0], who[1]);
            },
            "voice trace: voice request");
    observe(cpu, kVoiceStopRequest,
            [](Cpu& c) {
                u32 who[3];
                callers(c, who);
                emit(c, "voice_stop_req", "voice=%d\tcaller=0x%08X\tcaller2=0x%08X", static_cast<s32>(c.gpr[4]), who[0],
                     who[1]);
            },
            "voice trace: voice stop request");
    observe(cpu, kVoiceSlotUpdate,
            [](Cpu& c) {
                const u32 slot = c.gpr[3];
                if (!plausible(slot)) return;
                const u32 handle = c.mem_read32(slot + 0x3Cu);
                const u32 stat = plausible(handle) ? c.mem_read8(handle + 1u) : 255u;
                if (c.mem_read8(slot + 0x29u) == 1u && (stat == 0u || stat == 5u))
                    emit(c, "voice_slot_end", "voice=%d\tstat=%u\t%s", static_cast<s32>(c.mem_read32(slot + 0x2Cu)), stat,
                         stat == 5u ? "natural(play end)" : "stopped(status 0)");
                poll_streams(c);
            },
            "voice trace: voice slot update");
    observe(cpu, kAdxStartAfs,
            [](Cpu& c) {
                emit(c, "adx_start_afs", "slot=%s\thandle=0x%08X\tpartition=%d\tfile=%d", handle_name(c, c.gpr[3]), c.gpr[3],
                     static_cast<s32>(c.gpr[4]), static_cast<s32>(c.gpr[5]));
            },
            "voice trace: ADXT_StartAfs");
    observe(cpu, kAdxStartName,
            [](Cpu& c) {
                emit(c, "adx_start_name", "slot=%s\thandle=0x%08X\tname=%s", handle_name(c, c.gpr[3]), c.gpr[3],
                     c_string(c, c.gpr[4]).c_str());
            },
            "voice trace: ADXT_StartFname");
    observe(cpu, kAdxStop,
            [](Cpu& c) {
                u32 who[3];
                callers(c, who);
                const u32 handle = c.gpr[3];
                emit(c, "adx_stop", "slot=%s\thandle=0x%08X\tstat_before=%u\tcaller=0x%08X\tcaller2=0x%08X\tcaller3=0x%08X",
                     handle_name(c, handle), handle, plausible(handle) ? c.mem_read8(handle + 1u) : 255u, who[0], who[1],
                     who[2]);
            },
            "voice trace: ADXT_Stop");
}

}  // namespace gekko::voice_trace

#endif  // !GEKKO_SHIPPING
