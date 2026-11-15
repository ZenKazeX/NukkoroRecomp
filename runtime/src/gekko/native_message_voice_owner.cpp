// The machine binding of the reconstructed message system and dialogue voice layer. Contract: include/gekko/native_message_voice_owner.h.

#include "gekko/native_message_voice_owner.h"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gekko/cpu.h"
#include "gekko/native_dialogue_voice.h"
#include "gekko/native_guest_call.h"
#include "gekko/native_message_system.h"
#include "gekko/voice_trace.h"

namespace gekko::message_voice_owner {
namespace {

namespace dv = dialogue_voice;
namespace ms = message_system;

constexpr u32 kAdxtCreate = 0x803C3D34u, kAdxtStop = 0x803C2F90u, kAdxtPause = 0x803C21C4u, kAdxtStartAfs = 0x803C3E40u;
constexpr u32 kAdxtGetStat = 0x803C2F38u, kAdxtSetOutVol = 0x803C25BCu;
constexpr u32 kCri5750 = 0x803C5750u, kCri5118 = 0x803C5118u, kCri72D8 = 0x803B72D8u;
constexpr u32 kTaskCtor = 0x8004F014u, kTaskDtor = 0x8004EF18u, kTaskParentBlock = 0x800461A0u;
constexpr u32 kSoundAlloc = 0x80049BB8u, kGlyphDraw = 0x8004A09Cu, kGuestNew = 0x803A1380u, kGuestDelete = 0x803A1334u;
constexpr u32 kRand = 0x8040FAE0u, kMgrCtor = 0x801FB490u, kStaticDtor = 0x803A1210u, kSoundMgr = 0x8005328Cu;
constexpr u32 kCountRelease = 0x8000945Cu;
constexpr u32 kGlyphReturn = 0x80049C98u;  // the return address in fn_80049BE8 (the Show subtitles hook checks it)

// Erratum E4's window lifetime (docs/GAME_ERRATA.md): native state, active only in owner mode with the switch on.
ms::WindowLifetime g_lifetime;
bool g_lifetime_enabled = false;

struct Regs {
    u32 r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0;
    double f1 = 0.0, f2 = 0.0;
};

Regs regs_of(Cpu& c) {
    Regs r;
    r.r3 = c.gpr[3];
    r.r4 = c.gpr[4];
    r.r5 = c.gpr[5];
    r.r6 = c.gpr[6];
    r.r7 = c.gpr[7];
    r.r8 = c.gpr[8];
    r.f1 = std::bit_cast<double>(c.fpr[1].bits[0]);
    r.f2 = std::bit_cast<double>(c.fpr[2].bits[0]);
    return r;
}

struct Fn {
    u32 address;
    const char* name;
    bool returns_value;
};

// Every entry owned, the public wrappers included. Order is the report's.
constexpr Fn kFns[] = {
    {0x8010A14Cu, "manager", true},
    {0x80049E44u, "find_record", true},
    {0x80049D60u, "record_text", true},
    {0x80049BE8u, "draw_message", true},
    {0x801FAEF4u, "reset", false},
    {0x801FB210u, "enqueue", true},
    {0x801FAF54u, "enqueue_random", true},
    {0x801FBA18u, "queue_erase", false},
    {0x801FADD8u, "is_finished", true},
    {0x801FB3CCu, "reinit", false},
    {0x801FB99Cu, "task_ctor", true},
    {0x801FB938u, "task_dtor", true},
    {0x801FB5ACu, "task_update", false},
    {0x801D7C64u, "choose_slot", true},
    {0x801D7DF0u, "play", true},
    {0x801D5540u, "request_play", true},
    {0x801D7BD0u, "stop_by_id", false},
    {0x801D551Cu, "request_stop", false},
    {0x801D7B4Cu, "stop_all", false},
    {0x801D54FCu, "request_stop_all", false},
    {0x801D8058u, "pause_all", false},
    {0x801D55ACu, "request_pause_all", false},
    {0x801D7F80u, "resume_all", false},
    {0x801D558Cu, "request_resume_all", false},
    {0x801D6F60u, "serif_reset", false},
    {0x801D6FECu, "serif_ctor", true},
    {0x801D6E14u, "serif_update", false},
    {0x801D874Cu, "allocate_work_areas", false},
};

struct FnCounters {
    u64 calls = 0;          // calls served / checked at this entry from translated code
    u64 nested_calls = 0;   // verify: calls the translated bodies made to it inside a checked call
    u64 bytes_compared = 0; // verify
    u64 boundary_calls = 0; // verify: boundary calls matched
    u64 differences = 0;    // verify
};
std::map<u32, FnCounters> g_counters;
u64 g_subtitle_draws = 0;
u64 g_subtitle_skips = 0;

// Runs one owned entry on the core.
template <class E>
u32 run_native(E& env, u32 address, const Regs& r) {
    switch (address) {
        case 0x8010A14Cu: return ms::manager(env);
        case 0x80049E44u:
            return ms::find_record(env, r.r3, static_cast<s32>(r.r4), static_cast<s32>(r.r5), static_cast<s32>(r.r6));
        case 0x80049D60u: {
            ms::OutWord frames, voice;
            const u32 text = ms::record_text(env, r.r3, static_cast<s32>(r.r4), static_cast<s32>(r.r5), static_cast<s32>(r.r6),
                                             r.r7 != 0u ? &frames : nullptr, r.r8 != 0u ? &voice : nullptr);
            if (frames.written) env.w32(r.r7, static_cast<u32>(frames.value));
            if (voice.written) env.w32(r.r8, static_cast<u32>(voice.value));
            return text;
        }
        case 0x80049BE8u: {
            ms::OutWord voice;
            const u32 record = ms::draw_message(env, r.r3, static_cast<s32>(r.r4), static_cast<s32>(r.r5), static_cast<s32>(r.r6),
                                                r.r7 != 0u ? &voice : nullptr, r.f1, r.f2);
            if (voice.written) env.w32(r.r7, static_cast<u32>(voice.value));
            return record;
        }
        case 0x801FAEF4u: ms::reset(env, r.r3); return 0u;
        case 0x801FB210u:
            return static_cast<u32>(ms::enqueue(env, r.r3, static_cast<s32>(r.r4), static_cast<s32>(r.r5), static_cast<s32>(r.r7)));
        case 0x801FAF54u: return static_cast<u32>(ms::enqueue_random(env, r.r3, static_cast<s32>(r.r4)));
        case 0x801FBA18u: ms::queue_erase(env, r.r3, r.r4); return 0u;
        case 0x801FADD8u: return ms::is_finished(env, r.r3, static_cast<s32>(r.r4));
        case 0x801FB3CCu: ms::reinit(env, r.r3, r.r4); return 0u;
        case 0x801FB99Cu: return ms::task_ctor(env, r.r3, r.r4);
        case 0x801FB938u: return ms::task_dtor(env, r.r3, r.r4);
        case 0x801FB5ACu: ms::task_update(env, r.r3, r.f1); return 0u;
        case 0x801D7C64u:
            return static_cast<u32>(dv::choose_slot(env, static_cast<s32>(r.r3), static_cast<s32>(static_cast<s8>(static_cast<u8>(r.r4)))));
        case 0x801D7DF0u: return static_cast<u32>(dv::play(env, static_cast<s32>(r.r3), static_cast<s32>(r.r4), static_cast<s32>(r.r5)));
        case 0x801D5540u: return static_cast<u32>(dv::request_play(env, static_cast<s32>(r.r4), r.r5, static_cast<s32>(r.r6)));
        case 0x801D7BD0u: dv::stop_by_id(env, static_cast<s32>(r.r3)); return 0u;
        case 0x801D551Cu: dv::request_stop(env, static_cast<s32>(r.r4)); return 0u;
        case 0x801D7B4Cu: dv::stop_all(env); return 0u;
        case 0x801D54FCu: dv::request_stop_all(env); return 0u;
        case 0x801D8058u:
        case 0x801D55ACu: dv::pause_all(env); return 0u;
        case 0x801D7F80u:
        case 0x801D558Cu: dv::resume_all(env); return 0u;
        case 0x801D6F60u: dv::serif_reset(env, r.r3); return 0u;
        case 0x801D6FECu: return dv::serif_ctor(env, r.r3, r.r4, static_cast<s32>(r.r5), r.r6, r.r7);
        case 0x801D6E14u: dv::serif_update(env, r.r3); return 0u;
        case 0x801D874Cu: dv::allocate_work_areas(env); return 0u;
        default: return 0u;
    }
}

// The real machine. Boundary calls run with a guest stack built so that the callee (and the voice trace watching it) sees the same
// return-address chain as the original had: the original saved each return address at its entry r1 + 4 before calling further.
class CpuEnv final : public ms::Env {
public:
    explicit CpuEnv(Cpu& c) : c_(c), entry_sp_(c.gpr[1]) { chain_[0] = c.lr; depth_ = 1; }

    u8 r8(u32 a) override { return c_.mem_read8(a); }
    u16 r16(u32 a) override { return c_.mem_read16(a); }
    u32 r32(u32 a) override { return c_.mem_read32(a); }
    void w8(u32 a, u8 v) override { c_.mem_write8(a, v); }
    void w16(u32 a, u16 v) override { c_.mem_write16(a, v); }
    void w32(u32 a, u32 v) override { c_.mem_write32(a, v); }

    void enter_call(u32 return_address) override {
        if (depth_ >= kMaxChain) throw dv::UnportedPath{"native message call nesting deeper than the table"};
        chain_[depth_++] = return_address;
    }
    void leave_call() override { --depth_; }

    u32 adxt_create(s32 ch, u32 w, u32 s) override { return call(kAdxtCreate, kAdxtCreate + 4u, {static_cast<u32>(ch), w, s}); }
    void adxt_stop(u32 h, u32 link) override { call(kAdxtStop, link, {h}); }
    void adxt_pause(u32 h, u32 on) override { call(kAdxtPause, kAdxtPause + 4u, {h, on}); }
    void adxt_start_afs(u32 h, u32 part, u32 file) override { call(kAdxtStartAfs, 0x801D7E84u, {h, part, file}); }  // the return address in play, its only caller  // liveness-roots: value-only
    s32 adxt_get_stat(u32 h) override { return static_cast<s32>(call(kAdxtGetStat, kAdxtGetStat + 4u, {h})); }
    void adxt_set_out_vol(u32 h, s32 v) override { call(kAdxtSetOutVol, kAdxtSetOutVol + 4u, {h, static_cast<u32>(v)}); }
    void cri_setup_803C5750(u32 a) override { call(kCri5750, kCri5750 + 4u, {a}); }
    void cri_setup_803C5118(u32 a) override { call(kCri5118, kCri5118 + 4u, {a}); }
    void cri_setup_803B72D8() override { call(kCri72D8, kCri72D8 + 4u, {}); }
    void task_base_ctor(u32 t, u32 p) override { call(kTaskCtor, kTaskCtor + 4u, {t, p}); }
    u32 sound_heap_alloc(u32 s, u32 t) override { return call(kSoundAlloc, kSoundAlloc + 4u, {s, t}); }

    void draw_text(u32 font, u32 text, double x, double y) override {
        // The glyph draw; the Show subtitles hook at its entry skips it by reading the saved return address of fn_80049BE8's frame.
        const FpReg saved1 = c_.fpr[1], saved2 = c_.fpr[2];
        c_.fpr[1].bits[0] = c_.fpr[1].bits[1] = std::bit_cast<u64>(x);
        c_.fpr[2].bits[0] = c_.fpr[2].bits[1] = std::bit_cast<u64>(y);
        // fn_80049BE8's frame is 0x30 bytes: the callee finds the saved LR at r1 + 0x34, i.e. the entry r1 + 4 of the chain's top.
        call(kGlyphDraw, kGlyphReturn, {font, text}, 0x20u);
        c_.fpr[1] = saved1;
        c_.fpr[2] = saved2;
    }
    u32 guest_new(u32 s) override { return call(kGuestNew, kGuestNew + 4u, {s}); }
    void guest_delete(u32 o) override { call(kGuestDelete, kGuestDelete + 4u, {o}); }
    void task_base_dtor(u32 t, s32 f) override { call(kTaskDtor, kTaskDtor + 4u, {t, static_cast<u32>(f)}); }
    u32 task_parent_block() override { return call(kTaskParentBlock, kTaskParentBlock + 4u, {}); }
    s32 rand_below(s32 limit) override { return static_cast<s32>(call(kRand, kRand + 4u, {static_cast<u32>(limit)})); }
    void manager_construct(u32 m) override { call(kMgrCtor, kMgrCtor + 4u, {m}); }
    void register_static_dtor(u32 o, u32 d, u32 b) override { call(kStaticDtor, kStaticDtor + 4u, {o, d, b}); }
    void sound_manager_construct() override { call(kSoundMgr, kSoundMgr + 4u, {}); }
    void count_release_last(u32 count) override { call(kCountRelease, kCountRelease + 4u, {count}); }
    void control_destroy(u32 count) override {
        const u32 vtable = c_.mem_read32(count);
        const u32 target = c_.mem_read32(vtable + 0x10u);
        call(target, target + 4u, {count});
    }

    ms::WindowLifetime* window_lifetime() override { return g_lifetime_enabled ? &g_lifetime : nullptr; }
    u64 game_ticks() override { return c_.game_clock().now(); }
    s32 adxt_num_samples(u32 handle) override { return static_cast<s32>(call(ms::kAdxtGetNumSmpl, ms::kAdxtGetNumSmpl + 4u, {handle})); }
    void trace_lifetime(const char* event, const std::string& text) override {
        if (voice_trace::enabled()) voice_trace::note(c_, event, text.c_str());
    }

    void trace_voice_stop(s32 voice) override {
        if (voice_trace::enabled()) voice_trace::note_voice_stop(c_, voice, chain_[depth_ - 1], previous_caller());
    }
    void trace_voice_request(s32 voice, s32 flag, s32 tag) override {
        if (voice_trace::enabled()) voice_trace::note_voice_request(c_, voice, flag, tag, chain_[depth_ - 1], previous_caller());
    }
    void trace_queue_pop(u32 entry, u32 link) override {
        if (voice_trace::enabled()) voice_trace::note_queue_pop(c_, entry, link);
    }

private:
    static constexpr u32 kMaxChain = 12u;

    u32 previous_caller() {
        if (depth_ >= 2u) return chain_[depth_ - 2u];
        const u32 up = c_.mem_read32(entry_sp_);  // the real caller's frame
        return c_.mem_read32(up + 4u);
    }

    u32 call(u32 target, u32 link, std::initializer_list<u32> args, u32 extra_frame = 0u) {
        // Lay down the frames the original would have had: each level saved its return address at its entry r1 + 4 and pushed a frame.
        u32 sp = entry_sp_;
        for (u32 level = 0; level + 1u < depth_; ++level) {
            c_.mem_write32(sp + 4u, chain_[level]);
            const u32 next = sp - 0x20u;
            c_.mem_write32(next, sp);
            sp = next;
        }
        c_.mem_write32(sp + 4u, chain_[depth_ - 1u]);
        if (extra_frame != 0u) {
            const u32 next = sp - extra_frame;
            c_.mem_write32(next, sp);
            sp = next;
            // The Show subtitles hook reads the saved LR of fn_80049BE8's frame at its entry r1 + 0x34 (guest_call's frame, 16-aligned).
            c_.mem_write32(((sp - 0x10u) & ~0xFu) + 0x34u, chain_[depth_ - 1u]);
        }
        const u32 saved_sp = c_.gpr[1];
        c_.gpr[1] = sp;
        u32 result = 0u;
        (void)guest_call(c_, target, link, args, result);  // a failure stops the machine
        c_.gpr[1] = saved_sp;
        return result;
    }

    Cpu& c_;
    u32 entry_sp_;
    u32 chain_[kMaxChain]{};
    u32 depth_ = 0u;
};

// ---- verify mode -----------------------------------------------------------------------------------------------------------

struct Recorded {
    std::string key;
    u32 result = 0u;
    std::vector<std::pair<u32, u8>> capture;  // memory the call wrote that the core reads afterwards (object headers)
};

struct Verifier {
    bool in_check = false;
    int boundary_depth = 0;
    std::vector<Recorded> calls;
    std::string first_difference;
    u64 total_differences = 0u;
    bool self_pending = false;                       // the first re-entry after the check's own guest_call is not a nested call
    u32 self_address = 0u;
    std::vector<std::pair<u32, u32>> link_ranges;    // Task tree links of objects in the snapshot (+0x08 .. +0x17): the task framework's
};
Verifier g_v;

std::string hex(u32 v) {
    char t[16];
    std::snprintf(t, sizeof(t), "%08X", v);
    return t;
}
std::string join_key(const char* name, std::initializer_list<u32> args) {
    std::string key = name;
    for (u32 a : args) key += " " + hex(a);
    return key;
}

class ReplayEnv final : public ms::Env {
public:
    ReplayEnv(Cpu& c, std::map<u32, u8> pre, std::vector<Recorded>& calls) : c_(c), pre_(std::move(pre)), calls_(calls) {}

    u8 r8(u32 a) override {
        auto it = written_.find(a);
        if (it != written_.end()) return it->second;
        it = pre_.find(a);
        if (it != pre_.end()) return it->second;
        return c_.mem_read8(a);
    }
    u16 r16(u32 a) override { return static_cast<u16>((r8(a) << 8) | r8(a + 1u)); }
    u32 r32(u32 a) override { return (static_cast<u32>(r8(a)) << 24) | (r8(a + 1u) << 16) | (r8(a + 2u) << 8) | r8(a + 3u); }
    void w8(u32 a, u8 v) override { written_[a] = v; }
    void w16(u32 a, u16 v) override {
        written_[a] = static_cast<u8>(v >> 8);
        written_[a + 1u] = static_cast<u8>(v);
    }
    void w32(u32 a, u32 v) override {
        written_[a] = static_cast<u8>(v >> 24);
        written_[a + 1u] = static_cast<u8>(v >> 16);
        written_[a + 2u] = static_cast<u8>(v >> 8);
        written_[a + 3u] = static_cast<u8>(v);
    }

    u32 boundary(const std::string& key) {
        if (next_ >= calls_.size()) {
            difference("the native code made an extra call: " + key);
            return 0u;
        }
        Recorded& rec = calls_[next_++];
        if (rec.key != key) difference("call " + std::to_string(next_ - 1u) + ": original [" + rec.key + "], native [" + key + "]");
        for (const auto& [a, v] : rec.capture) written_[a] = v;
        return rec.result;
    }
    void difference(const std::string& text) {
        if (first_difference.empty()) first_difference = text;
    }

    u32 adxt_create(s32 ch, u32 w, u32 s) override { return boundary(join_key("adxt_create", {static_cast<u32>(ch), w, s})); }
    void adxt_stop(u32 h, u32 link) override { (void)link; boundary(join_key("adxt_stop", {h})); }
    void adxt_pause(u32 h, u32 on) override { boundary(join_key("adxt_pause", {h, on})); }
    void adxt_start_afs(u32 h, u32 p, u32 f) override { boundary(join_key("adxt_start_afs", {h, p, f})); }
    s32 adxt_get_stat(u32 h) override { return static_cast<s32>(boundary(join_key("adxt_get_stat", {h}))); }
    void adxt_set_out_vol(u32 h, s32 v) override { boundary(join_key("adxt_set_out_vol", {h, static_cast<u32>(v)})); }
    void cri_setup_803C5750(u32 a) override { boundary(join_key("cri5750", {a})); }
    void cri_setup_803C5118(u32 a) override { boundary(join_key("cri5118", {a})); }
    void cri_setup_803B72D8() override { boundary(join_key("cri72D8", {})); }
    void task_base_ctor(u32 t, u32 p) override { boundary(join_key("task_ctor", {t, p})); }
    u32 sound_heap_alloc(u32 s, u32 t) override { return boundary(join_key("sound_alloc", {s, t})); }
    void draw_text(u32 font, u32 text, double x, double y) override {
        boundary(join_key("glyph", {font, text, std::bit_cast<u32>(static_cast<float>(x)), std::bit_cast<u32>(static_cast<float>(y))}));
    }
    u32 guest_new(u32 s) override { return boundary(join_key("new", {s})); }
    void guest_delete(u32 o) override {
        boundary(join_key("delete", {o}));
        excluded_.emplace_back(o, 0x40u);
    }
    void task_base_dtor(u32 t, s32 f) override { boundary(join_key("task_dtor", {t, static_cast<u32>(f)})); }
    u32 task_parent_block() override { return boundary(join_key("parent", {})); }
    s32 rand_below(s32 l) override { return static_cast<s32>(boundary(join_key("rand", {static_cast<u32>(l)}))); }
    void manager_construct(u32 m) override { boundary(join_key("mgr_ctor", {m})); }
    void register_static_dtor(u32 o, u32 d, u32 b) override { boundary(join_key("atexit", {o, d, b})); }
    void sound_manager_construct() override { boundary(join_key("snd", {})); }
    void count_release_last(u32 c) override { boundary(join_key("release", {c})); excluded_.emplace_back(c, 12u); }
    void control_destroy(u32 count) override { excluded_.emplace_back(count, 12u); }

    const std::map<u32, u8>& pre() const { return pre_; }
    const std::map<u32, u8>& written() const { return written_; }
    size_t consumed() const { return next_; }
    bool is_excluded(u32 a) const {
        for (const auto& [base, size] : excluded_) {
            if (a >= base && a < base + size) return true;
        }
        for (const auto& [base, size] : g_v.link_ranges) {
            if (a >= base && a < base + size) return true;
        }
        return false;
    }
    std::string first_difference;

private:
    Cpu& c_;
    std::map<u32, u8> pre_;
    std::map<u32, u8> written_;
    std::vector<Recorded>& calls_;
    size_t next_ = 0u;
    std::vector<std::pair<u32, u32>> excluded_;
};

void snapshot_range(Cpu& c, std::map<u32, u8>& pre, u32 base, u32 size) {
    for (u32 i = 0; i < size; ++i) pre[base + i] = c.mem_read8(base + i);
}

// Everything the owned code can write, read before the original runs.
std::map<u32, u8> take_snapshot(Cpu& c, u32 address, const Regs& r) {
    std::map<u32, u8> pre;
    const u32 mgr = c.mem_read32(ms::kManagerPointer);
    if (mgr != 0u) {
        snapshot_range(c, pre, mgr, 0x34u);
        const u32 data = c.mem_read32(mgr + ms::kMgrQueueData);
        const u32 capacity = c.mem_read32(mgr + ms::kMgrQueueCapacity);
        if (data != 0u && capacity <= 0x400u) snapshot_range(c, pre, data, capacity * ms::kQueueEntrySize);
        const u32 font = c.mem_read32(mgr + ms::kMgrFont);
        const u32 count = c.mem_read32(mgr + ms::kMgrFontCount);
        if (count != 0u) snapshot_range(c, pre, count, 12u);
        if (font != 0u) snapshot_range(c, pre, font + ms::kFontScale, 4u);
    }
    snapshot_range(c, pre, ms::kManagerPointer, 4u);
    snapshot_range(c, pre, ms::kManagerGuard, 1u);
    snapshot_range(c, pre, ms::kSoundManagerPointer, 4u);
    snapshot_range(c, pre, dv::kBgmWork, dv::kLoadedVoiceLanguage + 4u - dv::kBgmWork);
    g_v.link_ranges.clear();
    const auto task_links = [&](u32 task) { g_v.link_ranges.emplace_back(task + 8u, 0x10u); };
    for (u32 i = 0; i < 2u; ++i) {
        const u32 serif = c.mem_read32(dv::kSerifSlots + i * 4u);
        if (serif != 0u) {
            snapshot_range(c, pre, serif, 0x48u);
            task_links(serif);
        }
        const u32 bgm = c.mem_read32(dv::kBgmObjects + i * 4u);
        if (bgm != 0u) snapshot_range(c, pre, bgm + 0x28u, 8u);
    }
    switch (address) {
        case 0x801FB5ACu:
        case 0x801FB938u:
        case 0x801FB99Cu:
            snapshot_range(c, pre, r.r3, ms::kTaskSize);
            task_links(r.r3);
            break;
        case 0x801D6FECu:
        case 0x801D6F60u:
        case 0x801D6E14u:
            snapshot_range(c, pre, r.r3, 0x48u);
            task_links(r.r3);
            break;
        case 0x801FB3CCu: {
            const u32 task = c.mem_read32(r.r3 + ms::kMgrTask);
            if (task != 0u) {
                snapshot_range(c, pre, task, ms::kTaskSize);
                task_links(task);
            }
            const u32 new_count = c.mem_read32(r.r4 + 4u);  // the shared_ptr's count receives a weak reference
            if (new_count != 0u) snapshot_range(c, pre, new_count, 12u);
            break;
        }
        case 0x80049D60u:
            if (r.r7 != 0u) snapshot_range(c, pre, r.r7, 4u);
            if (r.r8 != 0u) snapshot_range(c, pre, r.r8, 4u);
            break;
        case 0x80049BE8u:
            if (r.r7 != 0u) snapshot_range(c, pre, r.r7, 4u);
            break;
        default: break;
    }
    return pre;
}

void compare_call(Cpu& c, u32 address, const char* name, const Regs& regs, std::map<u32, u8> pre, bool returns_value, u32 original_result) {
    FnCounters& fc = g_counters[address];
    ReplayEnv env(c, std::move(pre), g_v.calls);
    u32 native_result = 0u;
    bool stopped = false;
    try {
        native_result = run_native(env, address, regs);
    } catch (const dv::UnportedPath& p) {
        stopped = true;
        env.difference(std::string("fail-closed path taken: ") + p.what);
    }
    if (!stopped && env.consumed() != g_v.calls.size()) {
        env.difference("the native code made " + std::to_string(env.consumed()) + " boundary calls, the original " +
                       std::to_string(g_v.calls.size()) + "; next original: " + g_v.calls[env.consumed()].key);
    }
    if (returns_value && native_result != original_result) {
        env.difference("result: original " + hex(original_result) + ", native " + hex(native_result));
    }
    u64 compared = 0u;
    const auto check = [&](u32 a, u8 expected) {
        if (env.is_excluded(a)) return;
        ++compared;
        const u8 actual = c.mem_read8(a);
        if (expected != actual) {
            env.difference("byte " + hex(a) + ": original wrote " + std::to_string(actual) + ", native " + std::to_string(expected));
        }
    };
    for (const auto& [a, v] : env.pre()) {
        const auto w = env.written().find(a);
        check(a, w != env.written().end() ? w->second : v);
    }
    for (const auto& [a, v] : env.written()) {
        if (env.pre().find(a) == env.pre().end()) check(a, v);
    }
    fc.bytes_compared += compared;
    fc.boundary_calls += env.consumed();
    if (!env.first_difference.empty()) {
        ++fc.differences;
        ++g_v.total_differences;
        if (g_v.total_differences <= 16u) {
            std::fprintf(stderr, "[native-message verify] %s frame %u: %s\n", name, c.mem_read32(0x8057E7F0u), env.first_difference.c_str());
        }
        if (g_v.first_difference.empty()) {
            g_v.first_difference = std::string(name) + " at frame " + std::to_string(c.mem_read32(0x8057E7F0u)) + ": " + env.first_difference;
            std::fprintf(stderr, "[native-message verify] FIRST DIFFERENCE: %s\n", g_v.first_difference.c_str());
        }
    }
}

// Boundary recorder: runs the real function (the guard makes its re-entry decline), records key + result.
Cpu::HleHandler previous_handler(Cpu& cpu, u32 address) {
    for (const auto& entry : cpu.hle_entries()) {
        if (entry.address == address) return entry.handler;
    }
    return nullptr;
}

using Capture = std::function<void(Cpu&, const Regs&, Recorded&)>;

void capture_range(Cpu& c, Recorded& rec, u32 base, u32 size) {
    for (u32 i = 0; i < size; ++i) rec.capture.emplace_back(base + i, c.mem_read8(base + i));
}

void install_recorder(Cpu& cpu, u32 address, const char* key_name, int nargs,
                      std::function<bool(Cpu&, const Regs&)> relevant = nullptr, Capture capture = nullptr,
                      std::function<std::string(Cpu&, const Regs&)> key_override = nullptr) {
    Cpu::HleHandler previous = previous_handler(cpu, address);
    cpu.install_hle(
        address,
        [=](Cpu& c) {
            if (!g_v.in_check || g_v.boundary_depth > 0) return previous ? previous(c) : false;
            const Regs regs = regs_of(c);
            if (relevant && !relevant(c, regs)) return previous ? previous(c) : false;
            Recorded rec;
            if (key_override) {
                rec.key = key_override(c, regs);
            } else {
                std::initializer_list<u32> all{regs.r3, regs.r4, regs.r5};
                rec.key = key_name;
                int i = 0;
                for (u32 a : all) {
                    if (i++ < nargs) rec.key += " " + hex(a);
                }
            }
            ++g_v.boundary_depth;
            u32 result = 0u;
            (void)guest_call(c, address, address + 4u, {regs.r3, regs.r4, regs.r5, regs.r6}, result);
            --g_v.boundary_depth;
            rec.result = result;
            if (capture) capture(c, regs, rec);
            g_v.calls.push_back(std::move(rec));
            c.gpr[3] = result;
            return true;
        },
        std::string("native message verify: recorder"));
}

void install_verify(Cpu& cpu) {
    install_recorder(cpu, kAdxtCreate, "adxt_create", 3);
    install_recorder(cpu, kAdxtStop, "adxt_stop", 1);
    install_recorder(cpu, kAdxtPause, "adxt_pause", 2);
    install_recorder(cpu, kAdxtStartAfs, "adxt_start_afs", 3);
    install_recorder(cpu, kAdxtGetStat, "adxt_get_stat", 1);
    install_recorder(cpu, kAdxtSetOutVol, "adxt_set_out_vol", 2);
    install_recorder(cpu, kCri5750, "cri5750", 1);
    install_recorder(cpu, kCri5118, "cri5118", 1);
    install_recorder(cpu, kCri72D8, "cri72D8", 0);
    install_recorder(cpu, kTaskCtor, "task_ctor", 2, nullptr,
                     [](Cpu& c, const Regs& r, Recorded& rec) { capture_range(c, rec, r.r3, 0x28u); });
    install_recorder(cpu, kTaskDtor, "task_dtor", 2, nullptr,
                     [](Cpu& c, const Regs& r, Recorded& rec) { capture_range(c, rec, r.r3, 0x28u); });
    install_recorder(cpu, kSoundAlloc, "sound_alloc", 2);
    install_recorder(cpu, kGuestNew, "new", 1);
    install_recorder(cpu, kGuestDelete, "delete", 1);
    install_recorder(cpu, kTaskParentBlock, "parent", 0);
    install_recorder(cpu, kRand, "rand", 1);
    install_recorder(cpu, kMgrCtor, "mgr_ctor", 1, nullptr, [](Cpu& c, const Regs& r, Recorded& rec) {
        capture_range(c, rec, r.r3, 0x34u);
    });
    install_recorder(cpu, kStaticDtor, "atexit", 3);
    install_recorder(cpu, kSoundMgr, "snd", 0, [](Cpu& c, const Regs&) { return c.mem_read32(ms::kSoundManagerPointer) == 0u; });
    install_recorder(cpu, kCountRelease, "release", 1,
                     [](Cpu& c, const Regs& r) { return r.r3 != 0u && c.mem_read32(r.r3 + 4u) == 1u; });
    install_recorder(cpu, kGlyphDraw, "glyph", 0, nullptr, nullptr, [](Cpu&, const Regs& r) {
        return join_key("glyph", {r.r3, r.r4, std::bit_cast<u32>(static_cast<float>(r.f1)), std::bit_cast<u32>(static_cast<float>(r.f2))});
    });

    for (const Fn& fn : kFns) {
        const u32 address = fn.address;
        const char* name = fn.name;
        const bool returns_value = fn.returns_value;
        Cpu::HleHandler previous = previous_handler(cpu, address);
        cpu.install_hle(
            address,
            [=](Cpu& c) {
                if (g_v.in_check) {
                    if (g_v.self_pending && g_v.self_address == address) {
                        g_v.self_pending = false;
                    } else {
                        ++g_counters[address].nested_calls;
                    }
                    return previous ? previous(c) : false;
                }
                // Observers installed earlier run on the original path (the guest_call below re-enters this handler with in_check set).
                ++g_counters[address].calls;
                const Regs regs = regs_of(c);
                std::map<u32, u8> pre = take_snapshot(c, address, regs);
                g_v.calls.clear();
                g_v.in_check = true;
                g_v.self_pending = true;
                g_v.self_address = address;
                u32 result = 0u;
                const u32 saved_lr = c.lr;
                const bool ok = guest_call(c, address, address + 4u, {regs.r3, regs.r4, regs.r5, regs.r6, regs.r7, regs.r8}, result);
                g_v.in_check = false;
                if (!ok) return true;  // the run has been stopped
                compare_call(c, address, name, regs, std::move(pre), returns_value, result);
                c.gpr[3] = result;
                c.lr = saved_lr;
                return true;
            },
            std::string("native message verify: ") + name);
    }
}

void install_owner(Cpu& cpu) {
    for (const Fn& fn : kFns) {
        const u32 address = fn.address;
        const bool returns_value = fn.returns_value;
        Cpu::HleHandler previous = previous_handler(cpu, address);
        cpu.install_hle(
            address,
            [=](Cpu& c) {
                (void)previous;
                ++g_counters[address].calls;
                CpuEnv env(c);
                const Regs regs = regs_of(c);
                try {
                    const u32 result = run_native(env, address, regs);
                    if (returns_value) c.gpr[3] = result;
                    if (address == 0x801FADD8u && voice_trace::enabled()) {
                        // When each polled key reports finished: the game logic that waits on it (HintRing and the message action object).
                        static std::map<u32, u32> last_answer;
                        u32& last = last_answer[regs.r4];
                        if (result != last) {
                            voice_trace::note(c, result != 0u ? "finished_poll_end" : "finished_poll_wait", ("key=" + std::to_string(static_cast<s32>(regs.r4))).c_str());
                            last = result;
                        }
                    }
                } catch (const dv::UnportedPath& p) {
                    c.request_stop(StopReason::ExecutionFault, std::string("native message system: ") + p.what);
                }
                return true;
            },
            "native message system");
    }
    // Counts the glyph draws and the Show subtitles skips (the hook at this entry returns true when it skips).
    Cpu::HleHandler glyph = previous_handler(cpu, kGlyphDraw);
    cpu.install_hle(
        kGlyphDraw,
        [glyph](Cpu& c) {
            if (glyph && glyph(c)) {
                ++g_subtitle_skips;
                return true;
            }
            ++g_subtitle_draws;
            return false;
        },
        "native message system: glyph draw counter");
}

Mode g_mode = Mode::Off;
bool g_report_table = false;

}  // namespace

Mode install(Cpu& cpu) {
    const char* value = std::getenv("SHADOW_NATIVE_MESSAGE");
    g_mode = Mode::Off;
    const char* errata_all = std::getenv("SHADOW_ERRATA");
    const char* errata_e4 = std::getenv("SHADOW_ERRATA_E4");
    const bool e4_wanted = !(errata_all != nullptr && errata_all[0] == '0') && !(errata_e4 != nullptr && errata_e4[0] == '0');
    g_report_table = value != nullptr;
    if (value != nullptr && std::strcmp(value, "verify") == 0) {
        g_mode = Mode::Verify;
        install_verify(cpu);
    } else if (value == nullptr || std::strcmp(value, "owner") == 0 || std::strcmp(value, "1") == 0) {
        g_mode = Mode::Owner;
        install_owner(cpu);
    }
    g_lifetime_enabled = g_mode == Mode::Owner && e4_wanted;
    std::fprintf(stderr, "[native-message] mode %s; E4 window lifetime %s\n",
                 g_mode == Mode::Verify ? "verify" : g_mode == Mode::Owner ? "owner" : "off",
                 g_lifetime_enabled ? "on" : (e4_wanted ? "off (needs the native owner)" : "off (SHADOW_ERRATA_E4=0)"));
    return g_mode;
}

void print_report(std::FILE* out) {
    const ms::WindowLifetimeCounters& e4 = g_lifetime.counters;
    std::fprintf(out, "[native-message] mode %s; E4 window lifetime %s; lingers %" PRIu64 " (%" PRIu64 " updates; ended by voice %" PRIu64
                      ", reset %" PRIu64 ", new window %" PRIu64 ", failsafe %" PRIu64 "); gates %" PRIu64 " (%" PRIu64
                      " updates); failsafe hits %" PRIu64 "; holds %" PRIu64 ", replaced %" PRIu64 ", refused %" PRIu64 ", released %" PRIu64 ", release refused %" PRIu64 ", dropped %" PRIu64
                      "; deferred lines %" PRIu64 " (holds %" PRIu64 ", admitted after the original end %" PRIu64 ")\n",
                 g_mode == Mode::Verify ? "verify" : g_mode == Mode::Owner ? "owner" : "off", g_lifetime_enabled ? "on" : "off", e4.lingers,
                 e4.linger_updates, e4.linger_ended_voice, e4.linger_ended_reset, e4.linger_ended_window, e4.linger_ended_failsafe, e4.gates,
                 e4.gate_updates, e4.failsafe_hits, e4.holds, e4.holds_replaced, e4.holds_refused, e4.holds_released, e4.holds_release_refused,
                 e4.holds_dropped, e4.deferred_lines, e4.deferred_holds, e4.deferred_admitted);
    if (g_mode == Mode::Off || !g_report_table) return;
    u64 total_differences = 0u;
    for (const Fn& fn : kFns) {
        const FnCounters& fc = g_counters[fn.address];
        if (g_mode == Mode::Verify) {
            std::fprintf(out, "[native-message] %-20s 0x%08X calls %" PRIu64 " nested %" PRIu64 " bytes compared %" PRIu64 " boundary calls %" PRIu64 " differences %" PRIu64 "\n",
                         fn.name, fn.address, fc.calls, fc.nested_calls, fc.bytes_compared, fc.boundary_calls, fc.differences);
        } else {
            std::fprintf(out, "[native-message] %-20s 0x%08X calls %" PRIu64 "\n", fn.name, fn.address, fc.calls);
        }
        total_differences += fc.differences;
    }
    if (g_mode == Mode::Verify) {
        std::fprintf(out, "[native-message] differences total %" PRIu64 "; first: %s\n", total_differences,
                     g_v.first_difference.empty() ? "none" : g_v.first_difference.c_str());
    } else {
        std::fprintf(out, "[native-message] glyph draws %" PRIu64 ", skipped by Show subtitles %" PRIu64 "\n", g_subtitle_draws, g_subtitle_skips);
    }
}

}  // namespace gekko::message_voice_owner
