// STEP-0 measurement, default off: guest entry probes for audit R5 (AX, AXRNA,
// Sofdec, movie) and R4 (DMorph re-emit), an AX parameter-block sampler at
// __AXSyncPBs entry, and per-VI MEM1 word samplers. See measure_probes.h for
// the observe-only contract; findings-2026-09-23/HARDWARE_ASSUMPTION_AUDIT.md
// sections 1 (R4, R5) and 7 (Route B HEADLESS-DSP / AXSEM, Route D, Route E)
// for what each counter answers.
//
// Every guest address below was read from sys/main.dol with tools/doldis.py
// (PAL image). Every probe address is a translated function entry
// (fn_<addr>_body in build-native/generated/fnindex.txt); install() re-checks
// that against the live RecompiledCode range table and refuses anything else.

#include "gekko/measure_probes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/numeric_probe.h"
#include "gekko/system.h"

namespace gekko {

static_assert(MeasureProbes::kMaxOrderedEvents == measurement::OrderedNumericEvidence::max_events);

namespace {

// ---------------------------------------------------------------------------
// Guest layout (PAL main.dol). r13 = 0x805EC500 (__start: lis r13,0x805E /
// ori r13,r13,0xC500).
// AXInit fn_80386E88: DSP PB array 0x8058FCA0, 64 x 0xF4; AXVPB array
// 0x805989A0, 64 x 0x22C. Each PB holds its own address at +4/+6
// (80387018..80387024), which validate_pb_table checks at run time.
constexpr u32 kPbBase = 0x8058FCA0u;
constexpr u32 kPbStride = 0xF4u;
constexpr u32 kVpbBase = 0x805989A0u;
constexpr u32 kVpbStride = 0x22Cu;
constexpr u32 kVoices = 64u;
constexpr u32 kDspBudget = 0x805F22A0u;     // r13+0x5DA0, DSP cycle budget
constexpr u32 kDspUsed = 0x805F22A4u;       // r13+0x5DA4, cycles used last sync
constexpr u32 kCallbackHead = 0x805F21E8u;  // r13+0x5CE8, dropped-voice stack
constexpr u32 kStackHeads = 0x80587DC0u;    // voice priority stack heads, 4*prio
constexpr u32 kStackTails = 0x80587E40u;    // ... tails
constexpr u32 kShowRaster = 0x80495CBCu;    // RwGC RasterShowRaster (guest frame)
constexpr u32 kDMorphCallback = 0x80444AC4u;

// PB field offsets (the same ones System::run_ax_frame uses).
constexpr u32 kPbState = 0x0Eu;
constexpr u32 kPbAuxAL = 0x1Au;
constexpr u32 kPbAuxAR = 0x1Eu;
constexpr u32 kPbAuxBL = 0x22u;
constexpr u32 kPbAuxBR = 0x26u;
constexpr u32 kPbVol = 0x64u;
constexpr u32 kPbVolDelta = 0x66u;
constexpr u32 kPbLoopFlag = 0x6Eu;
constexpr u32 kPbFormat = 0x70u;
constexpr u32 kPbLoopAddr = 0x72u;
constexpr u32 kPbEndAddr = 0x76u;
constexpr u32 kPbCurAddr = 0x7Au;
constexpr u32 kPbRatio = 0xA6u;
// AXVPB fields.
constexpr u32 kVpbNext = 0x08u;   // callback-stack link
constexpr u32 kVpbPrio = 0x0Cu;
constexpr u32 kVpbCallback = 0x10u;
constexpr u32 kVpbIndex = 0x18u;
constexpr u32 kVpbSync = 0x1Cu;
constexpr u32 kVpbState = 0x146u;   // 0x138 + PB state
constexpr u32 kVpbCurAddr = 0x1B2u;  // 0x138 + PB currentAddress

// fn_80386474 sync-flag bits. currentAddress is copied VPB->PB iff COPYALL
// (803864C8), or bit 0x10000 inside the 0x1E000 group (80386A48), or 0x1000
// with that group clear (80386A68); otherwise PB is copied into the VPB.
// PB state is copied VPB->PB iff COPYALL or 0x4 (80386738).
constexpr u32 kSyncCopyAll = 0x80000000u;
constexpr u32 kSyncCurAddr = 0x00010000u;
constexpr u32 kSyncAddrGroup = 0x0001E000u;
constexpr u32 kSyncAddrAll = 0x00001000u;
constexpr u32 kSyncState = 0x00000004u;

bool cur_from_vpb(u32 flags) {
    return (flags & kSyncCopyAll) != 0u || (flags & kSyncCurAddr) != 0u ||
           ((flags & kSyncAddrGroup) == 0u && (flags & kSyncAddrAll) != 0u);
}
bool state_from_vpb(u32 flags) { return (flags & kSyncCopyAll) != 0u || (flags & kSyncState) != 0u; }

enum class Kind : u8 {
    plain,
    show_raster,
    ax_frame,
    ax_sync_pbs,
    pb_sync,
    ax_force_drop,
    ax_cb_service,
    ax_acquire,
    axrna_frame,
    sofdec_clock,
    movie_getstat,
    dmorph_cb,
    geom_lock,
    reinstance,
    reemit,
};

struct BuiltIn {
    const char* set;
    const char* name;
    u32 address;
    Kind kind;
    u32 expected_lr[2];  // documented call-site return addresses, 0 = none
};

// "r5": the AX / AXRNA / Sofdec / movie decision points. Mid-function sites
// the audit names are replaced by their entries: the force drop at 80386D1C by
// its callee __AXPushCallbackStack (only caller, LR 80386D20); the steal blrl
// at 80384B3C by a prediction at AXAcquireVoice entry; the AXRNA refill block
// 803CF4EC by an exact prediction at fn_803CF408 entry.
constexpr BuiltIn kBuiltIns[] = {
    {"r5", "ai_dma_cb", 0x80385950u, Kind::plain, {0u, 0u}},
    {"r5", "ax_frame", 0x80385798u, Kind::ax_frame, {0x80385990u, 0x80385A54u}},  // idle, DSP-resume
    {"r5", "ax_sync_pbs", 0x80386BC0u, Kind::ax_sync_pbs, {0x803857CCu, 0u}},
    {"r5", "pb_sync", 0x80386474u, Kind::pb_sync, {0x80386CD4u, 0x80386D2Cu}},  // in budget, not running
    {"r5", "ax_force_drop", 0x80384914u, Kind::ax_force_drop, {0x80386D20u, 0u}},
    {"r5", "ax_cb_service", 0x80384718u, Kind::ax_cb_service, {0x80385808u, 0u}},
    {"r5", "ax_acquire", 0x80384A54u, Kind::ax_acquire, {0u, 0u}},
    {"r5", "reclaim_scan", 0x8043C64Cu, Kind::plain, {0x8043F620u, 0u}},
    {"r5", "voice_release", 0x8043CBA4u, Kind::plain, {0x8043C6F4u, 0u}},  // reclaim path
    {"r5", "axrna_frame", 0x803CF408u, Kind::axrna_frame, {0x803CEF4Cu, 0u}},
    {"r5", "axrna_free_query", 0x803CF5DCu, Kind::plain, {0x803C59B4u, 0u}},
    {"r5", "adxt_gettime", 0x803C2EE0u, Kind::plain, {0x803EEAC8u, 0u}},
    {"r5", "sofdec_clock", 0x803EEA5Cu, Kind::sofdec_clock, {0u, 0u}},
    {"r5", "movie_getstat", 0x804086D8u, Kind::movie_getstat, {0u, 0u}},
    {"r5", "movie_open", 0x80036D68u, Kind::plain, {0u, 0u}},
    {"r5", "movie_tick", 0x80036B0Cu, Kind::plain, {0u, 0u}},
    {"r5", "movie_frame_ready", 0x800367A8u, Kind::plain, {0u, 0u}},
    {"r5", "movie_blit", 0x80036570u, Kind::plain, {0u, 0u}},
    {"r5", "cri_get_frame", 0x80403E3Cu, Kind::plain, {0u, 0u}},
    {"r5", "sfd_exec_dec", 0x80408D80u, Kind::plain, {0u, 0u}},
    {"r5", "mpeg_dec", 0x803F92E0u, Kind::plain, {0u, 0u}},
    // "r4": DMorph in-place geometry re-emit.
    {"r4", "dmorph_cb", kDMorphCallback, Kind::dmorph_cb, {0u, 0u}},
    {"r4", "geom_lock", 0x8045A1BCu, Kind::geom_lock, {0x80444B74u, 0u}},  // from DMorph cb
    {"r4", "reinstance", 0x80464890u, Kind::reinstance, {0u, 0u}},
    {"r4", "reemit", 0x80464624u, Kind::reemit, {0x8046493Cu, 0u}},
};

// The r5 set's default per-VI words: DSP budget, cycles used, callback-stack
// head, free-voice stack head.
constexpr std::pair<u32, u32> kR5DefaultSamples[] = {
    {kDspBudget, 4u}, {kDspUsed, 4u}, {kCallbackHead, 4u}, {kStackHeads, 4u}};

constexpr std::size_t kLrSlots = 16;
constexpr u32 kViBlockSize = 50;
constexpr std::size_t kViBlocks = 2048;
constexpr std::size_t kGfBins = 7;  // 1, 2, 3-4, 5-8, 9-16, 17-64, 65+
constexpr std::size_t kSeriesCap = 4096;
constexpr std::size_t kObjSlots = 8;
constexpr std::size_t kCbTargetSlots = 8;
constexpr std::size_t kMovieHandles = 8;
constexpr std::size_t kMovieTransitionCap = 128;
constexpr u32 kCallbackWalkCap = 64;

std::size_t gf_bin(u64 count) {
    if (count <= 1) return 0;
    if (count == 2) return 1;
    if (count <= 4) return 2;
    if (count <= 8) return 3;
    if (count <= 16) return 4;
    if (count <= 64) return 5;
    return 6;
}

// Per-VI delta histogram: 0, 1, 2, 3, 4, 5-8, 9-16, 17+.
struct DeltaHist {
    std::array<u64, 8> bins{};
    u64 total = 0;
    void add(u64 delta) {
        total += delta;
        const std::size_t bin = delta <= 4 ? static_cast<std::size_t>(delta) : delta <= 8 ? 5 : delta <= 16 ? 6 : 7;
        ++bins[bin];
    }
};

std::atomic<MeasureProbes*> g_active{nullptr};

// ---------------------------------------------------------------------------
// Minimal JSON writer: commas are placed by a per-container "first" stack, so
// every value in the output is emitted through one of the typed calls below.
class JsonWriter {
public:
    void begin_object() { prefix(); out_ += '{'; first_.push_back(true); }
    void end_object() { out_ += '}'; first_.pop_back(); }
    void begin_array() { prefix(); out_ += '['; first_.push_back(true); }
    void end_array() { out_ += ']'; first_.pop_back(); }
    JsonWriter& key(const char* name) {
        comma();
        out_ += '"';
        out_ += name;
        out_ += "\":";
        after_key_ = true;
        return *this;
    }
    void u(u64 value) { prefix(); out_ += std::to_string(value); }
    void i(long long value) { prefix(); out_ += std::to_string(value); }
    void b(bool value) { prefix(); out_ += value ? "true" : "false"; }
    void hex(u32 value) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "\"0x%08X\"", value);
        prefix();
        out_ += buffer;
    }
    void str(const std::string& value) {
        prefix();
        out_ += '"';
        for (const unsigned char ch : value) {
            switch (ch) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (ch < 0x20 || ch >= 0x7F) {
                        char esc[8];
                        std::snprintf(esc, sizeof(esc), "\\u%04X", ch);
                        out_ += esc;
                    } else {
                        out_ += static_cast<char>(ch);
                    }
            }
        }
        out_ += '"';
    }
    const std::string& text() const { return out_; }

private:
    void comma() {
        if (first_.empty()) return;
        if (!first_.back()) out_ += ',';
        first_.back() = false;
    }
    void prefix() {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        comma();
    }
    std::string out_;
    std::vector<bool> first_;
    bool after_key_ = false;
};

std::string hex_string(u32 value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

bool parse_number(const std::string& text, u32& out) {
    if (text.empty() || text[0] == '-' || text[0] == '+') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 0);
    if (errno != 0 || end != text.c_str() + text.size() || value > 0xFFFFFFFFull) return false;
    out = static_cast<u32>(value);
    return true;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(separator, start);
        const std::size_t stop = end == std::string::npos ? text.size() : end;
        if (stop > start) parts.push_back(text.substr(start, stop - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}


bool set_known(const std::string& name) {
    return name == "r5" || name == "r4" || name == "all" || name == "none";
}

}  // namespace

// ---------------------------------------------------------------------------

struct MeasureProbes::Impl {
    struct LrSlot {
        u32 lr = 0;
        u64 n = 0;
    };
    struct Event {
        u64 instr = 0;
        u32 vi = 0;
        u64 gf = 0;
        u32 lr = 0;
        std::array<u32, 5> r{};
        std::array<u32, 4> x{};
    };
    struct Probe {
        std::string name;
        u32 address = 0;
        Kind kind = Kind::plain;
        std::array<u32, 2> expected_lr{};
        bool entry_translated = false;
        bool installed = false;
        std::string reject;        // why it was not installed
        std::string chained_with;  // production handler at install time
        u64 hits = 0;
        bool observe_numeric = false;
        measurement::NumericEvidence<32> numeric;
        measurement::NumericEvidence<32, measurement::EntryArgumentKey> entry_r4;
        std::array<LrSlot, kLrSlots> lr{};
        std::size_t lr_used = 0;
        u64 lr_other = 0;
        std::vector<Event> first;
        std::vector<measurement::NumericKey> first_numeric; // allocated only for selected numeric probes
        // Per VI frame. Buckets close lazily on the next hit in a new frame.
        bool vi_open = false;
        u32 cur_vi = 0;
        u64 cur_vi_count = 0;
        u64 vi_frames = 0;
        u64 vi_max = 0;
        u32 first_vi = 0;
        u32 last_vi = 0;
        std::vector<u32> blocks;
        u64 blocks_overflow = 0;
        // Per guest frame (RwGC ShowRaster entries).
        bool gf_open = false;
        u64 cur_gf = 0;
        u64 cur_gf_count = 0;
        u64 gf_frames = 0;
        u64 gf_max = 0;
        std::array<u64, kGfBins> gf_hist{};
    };

    struct VoiceDump {
        u32 index = 0;
        u16 state = 0, loop = 0, format = 0, vol = 0, vdelta = 0, aux_al = 0, vstate = 0;
        u32 loop_addr = 0, end = 0, cur = 0, ratio = 0, vprio = 0, vsync = 0, vcur = 0, vcb = 0;
    };
    struct Dump {
        u32 target = 0;
        bool taken = false;
        u32 vi = 0;
        u64 instr = 0;
        std::vector<VoiceDump> voices;
    };
    struct SeriesRow {
        u32 vi = 0;
        u64 syncs = 0;
        u32 running = 0;
        u32 advanced = 0;
        u64 lists = 0;
    };
    struct Ax {
        u64 sync_calls = 0;
        bool pb_table_valid = false;
        bool pb_self_physical = false;
        u64 validation_failures = 0;
        bool have_prev = false;
        std::array<u16, kVoices> prev_state{};
        std::array<u32, kVoices> prev_cur{};
        std::array<u32, kVoices> prev_flags{};
        u64 drop_pending = 0;  // voices force-dropped since the last observation
        // Voice-frames (one voice at one __AXSyncPBs observation).
        u64 running = 0, running_both = 0, advanced_dsp = 0, guest_rewrite = 0, cur_rewrite_same = 0;
        u64 frozen = 0, dirty_no_cur = 0, starts = 0, stops_dsp = 0, stops_guest = 0, stops_drop = 0;
        u64 aux_nonzero = 0, vdelta_nonzero = 0;
        u32 budget_last = 0, used_last = 0, used_max = 0;
        u64 over_budget = 0;
        bool series_open = false;
        u32 series_vi = 0;
        u64 series_syncs = 0, series_running = 0, series_advanced = 0, series_lists_start = 0;
        std::vector<SeriesRow> series;
        u64 series_overflow = 0;
        u64 vi_regressions = 0;
        std::vector<Dump> dumps;
        // ax_frame / pb_sync / force drop / callback service decoders.
        u64 frame_r3_zero = 0, frame_r3_nonzero = 0;
        u64 pbsync_flags_nonzero = 0, pbsync_cur_from_vpb = 0, pbsync_bad = 0;
        u64 drops = 0, drop_bad_index = 0, drop_lr_mismatch = 0, drop_prio_other = 0;
        std::array<u64, 32> drop_prio{};
        u64 cb_nonempty = 0, cb_total = 0, cb_max = 0, cb_capped = 0, cb_bad_link = 0;
    };
    struct Acquire {
        u64 calls = 0, empty_free = 0, predicted_steal = 0, predicted_steal_cb = 0, victim_running = 0;
        u64 predicted_fail = 0, prio_out_of_range = 0, cb_other = 0;
        std::array<LrSlot, kCbTargetSlots> cb{};  // lr field = callback target
        std::size_t cb_used = 0;
    };
    struct RnaObject {
        u32 obj = 0;
        u64 entries = 0, no_voice = 0, refills = 0, refill_units = 0, full_ring = 0, trap_predicted = 0;
        u64 pos_changes = 0, writepos_init = 0, writepos_match = 0, writepos_mismatch = 0;
        bool have_pos = false;
        s32 last_pos = 0;
        bool have_expected = false;
        u32 expected_w = 0;
    };
    struct Rna {
        u64 entries = 0, no_voice = 0, bad_obj = 0, predicted_refills = 0, refill_units = 0, full_ring = 0;
        u64 trap_predicted = 0, pos_changes = 0, writepos_init = 0, writepos_match = 0, writepos_mismatch = 0;
        std::vector<RnaObject> objects;
        u64 objects_overflow = 0;
    };
    struct Sofdec {
        u64 queries = 0, playing_queries = 0, bad_handle = 0, sub_changes = 0, ncount_changes = 0;
        bool have_last = false;
        u32 last_sub = 0, last_ncount = 0, last_tscale = 0, first_change_vi = 0, last_change_vi = 0;
        bool changed = false;
    };
    struct MovieHandle {
        u32 handle = 0;
        u32 last_status = 0;
        u64 polls = 0;
    };
    struct MovieTransition {
        u32 vi = 0;
        u64 gf = 0;
        u32 handle = 0, from = 0, to = 0;
    };
    struct Movie {
        u64 polls = 0, bad_handle = 0, handles_overflow = 0, transitions_overflow = 0;
        std::array<u64, 5> status_hist{};  // 0..3, other
        bool ever_playend = false;
        std::vector<MovieHandle> handles;
        std::vector<MovieTransition> transitions;
    };
    struct DMorph {
        u64 cb_entries = 0, lock_entries = 0, lock_from_dmorph = 0;
        u64 reinstance = 0, locked = 0, locked_dmorph = 0, locked_other = 0, pending_unconsumed = 0;
        u64 reemit = 0, reemit_dmorph = 0, reemit_other = 0, reemit_unattributed = 0, reemit_lr_mismatch = 0;
        int pending = 0;  // 0 none, 1 DMorph, 2 other
    };
    struct Sampler {
        u32 ea = 0;
        u32 size = 4;
        u32 log_limit = kDefaultSampleLogEntries;
        u64 samples = 0, changes = 0, log_overflow = 0;
        bool have = false;
        u32 first = 0, last = 0;
        std::vector<std::pair<u32, u32>> log;
    };

    Impl(System& owner, Config cfg) : system(owner), config(std::move(cfg)),
        ordered_numeric(config.numeric_entries.empty() ? 0u : std::min(config.ordered_events, kMaxOrderedEvents)) {}

    System& system;
    Config config;
    measurement::OrderedNumericEvidence ordered_numeric;
    bool installed_once = false;
    std::vector<Probe> probes;
    u64 guest_frames = 0;
    u64 bad_peeks = 0;
    Ax ax;
    Acquire acquire;
    Rna rna;
    Sofdec sofdec;
    Movie movie;
    DMorph dmorph;
    // Host-service (per VI) sampler.
    u64 host_service_ticks = 0, vi_ticks = 0, vi_gaps = 0, host_vi_regressions = 0;
    bool have_vi = false;
    u32 last_vi = 0;
    std::vector<Sampler> samplers;
    u64 last_lists = 0, last_mixer_frames = 0, last_voice_advances = 0, list_address_changes = 0;
    u32 last_list_address = 0;
    DeltaHist lists_hist, mixer_frames_hist, voice_advance_hist;

    // --- side-effect-free reads of the game's address window --------------
    bool valid(u32 ea, u32 size) const { return system.guest_memory().mapped(GcAddr(ea), size); }
    u32 peek32(u32 ea) {
        if (!valid(ea, 4u)) {
            ++bad_peeks;
            return 0;
        }
        return system.guest_memory().peek32(ea);
    }
    u16 peek16(u32 ea) {
        if (!valid(ea, 2u)) {
            ++bad_peeks;
            return 0;
        }
        return system.guest_memory().peek16(ea);
    }
    u8 peek8(u32 ea) {
        if (!valid(ea, 1u)) {
            ++bad_peeks;
            return 0;
        }
        return system.guest_memory().peek8(ea);
    }
    u32 peek_sized(u32 ea, u32 size) {
        return size == 1u ? peek8(ea) : size == 2u ? peek16(ea) : peek32(ea);
    }

    // --- construction ------------------------------------------------------
    void add_probe(const std::string& name, u32 address, Kind kind, const u32 (&expected)[2]) {
        for (const Probe& existing : probes) {
            if (existing.address == address) return;  // first definition wins
        }
        Probe probe;
        probe.name = name;
        probe.address = address;
        probe.kind = kind;
        probe.expected_lr = {expected[0], expected[1]};
        probes.push_back(std::move(probe));
    }

    void build() {
        auto& c = config;
        if (c.extra_entries.size() > kMaxExtraEntries) c.extra_entries.resize(kMaxExtraEntries);
        if (c.numeric_entries.size() > kMaxExtraEntries) c.numeric_entries.resize(kMaxExtraEntries);
        if (c.samples.size() > kMaxSamples) c.samples.resize(kMaxSamples);
        if (c.pb_dump_frames.size() > kMaxPbDumpFrames) c.pb_dump_frames.resize(kMaxPbDumpFrames);
        c.first_hits = std::min(c.first_hits, kMaxFirstHits);
        c.ordered_events = std::min(c.ordered_events, kMaxOrderedEvents);
        if (c.route_label.size() > kMaxLabel) c.route_label.resize(kMaxLabel);

        auto selected = [&](const char* set) {
            for (const std::string& s : c.sets) {
                if (s == "all" || s == set) return true;
            }
            return false;
        };
        static constexpr u32 kNoLr[2] = {0u, 0u};
        add_probe("show_raster", kShowRaster, Kind::show_raster, kNoLr);  // guest frame marker
        for (const BuiltIn& entry : kBuiltIns) {
            if (selected(entry.set)) add_probe(entry.name, entry.address, entry.kind, entry.expected_lr);
        }
        for (const u32 address : c.extra_entries) add_probe("extra_" + hex_string(address), address, Kind::plain, kNoLr);
        for (const u32 address : c.numeric_entries) add_probe("numeric_" + hex_string(address), address, Kind::plain, kNoLr);
        for (Probe& probe : probes) {
            probe.observe_numeric = std::find(c.numeric_entries.begin(), c.numeric_entries.end(), probe.address) != c.numeric_entries.end();
            probe.first.reserve(c.first_hits);
            if (probe.observe_numeric) probe.first_numeric.reserve(c.first_hits);
            probe.blocks.assign(kViBlocks, 0u);
        }

        if (c.default_samples && selected("r5")) {
            for (const auto& sample : kR5DefaultSamples) {
                if (c.samples.size() < kMaxSamples) c.samples.push_back({sample.first, sample.second});
            }
        }
        for (const auto& sample : c.samples) {
            Sampler sampler;
            sampler.ea = sample.first;
            sampler.size = (sample.second == 1u || sample.second == 2u) ? sample.second : 4u;
            if (sample.log_capacity == 0u || sample.log_capacity > kMaxSampleLogEntries) {
                throw std::invalid_argument("sample change-log capacity must be 1..65536");
            }
            sampler.log_limit = sample.log_capacity;
            sampler.log.reserve(sampler.log_limit);
            samplers.push_back(std::move(sampler));
        }
        for (const u32 target : c.pb_dump_frames) {
            Dump dump;
            dump.target = target;
            ax.dumps.push_back(std::move(dump));
        }
        ax.series.reserve(kSeriesCap);
        rna.objects.reserve(kObjSlots);
        movie.handles.reserve(kMovieHandles);
        movie.transitions.reserve(kMovieTransitionCap);
    }

    // --- per-hit bookkeeping --------------------------------------------
    static void close_vi(Probe& probe) {
        if (!probe.vi_open) return;
        ++probe.vi_frames;
        probe.vi_max = std::max(probe.vi_max, probe.cur_vi_count);
    }
    static void close_gf(Probe& probe) {
        if (!probe.gf_open) return;
        ++probe.gf_frames;
        probe.gf_max = std::max(probe.gf_max, probe.cur_gf_count);
        ++probe.gf_hist[gf_bin(probe.cur_gf_count)];
    }

    void record(Probe& probe, const Cpu& cpu, u32 vi, const std::array<u32, 4>& x) {
        ++probe.hits;
        measurement::NumericKey numeric;
        if (probe.observe_numeric) {
            for (std::size_t i = 0; i < numeric.formats.size(); ++i) numeric.formats[i] = cpu.spr[SPR_GQR0 + i];
            numeric.status = cpu.fpscr;
            probe.numeric.observe(numeric);
            // All hits, independently bounded. A first-256 sample cannot prove
            // that a channel/selector combination never occurred later.
            probe.entry_r4.observe({cpu.gpr[4]});
            ordered_numeric.observe([&]() noexcept {
                measurement::OrderedNumericEvent event;
                event.instructions = cpu.game_clock().now();
                event.guest_frame = guest_frames;
                event.site = probe.address; event.lr = cpu.lr; event.vi = vi;
                event.msr = cpu.msr(); event.fpscr = cpu.fpscr;
                for (std::size_t i = 0; i < event.f18_f19.size(); ++i)
                    event.f18_f19[i] = cpu.fpr[18 + i / 2].bits[i % 2];
                for (std::size_t i = 0; i < event.roots.size(); ++i)
                    event.roots[i] = cpu.gpr[event.root_registers[i]];
                event.formats = numeric.formats;
                // Constant low-memory reference addresses, checked before
                // unobserved RAM reads. No source-root pointers are followed.
                const std::array<u32,3> addresses{0x800000D4u,0x800000D8u,0x800000E4u};
                std::array<u32*,3> values{&event.current_context,&event.fp_owner,&event.thread};
                for (std::size_t i = 0; i < addresses.size(); ++i) {
                    if (system.guest_memory().mapped(GcAddr(addresses[i]), 4u)) {
                        *values[i] = system.guest_memory().peek32(addresses[i]);
                        event.context_fields_observed |= 1u << i;
                    }
                }
                return event;
            });
        }
        const u32 lr = cpu.lr;
        bool placed = false;
        for (std::size_t index = 0; index < probe.lr_used; ++index) {
            if (probe.lr[index].lr == lr) {
                ++probe.lr[index].n;
                placed = true;
                break;
            }
        }
        if (!placed) {
            if (probe.lr_used < kLrSlots) {
                probe.lr[probe.lr_used++] = LrSlot{lr, 1};
            } else {
                ++probe.lr_other;
            }
        }
        if (probe.first.size() < config.first_hits) {
            Event event;
            event.instr = cpu.game_clock().now();
            event.vi = vi;
            event.gf = guest_frames;
            event.lr = lr;
            for (std::size_t index = 0; index < 5; ++index) event.r[index] = cpu.gpr[3 + index];
            event.x = x;
            if (probe.observe_numeric) probe.first_numeric.push_back(numeric);
            probe.first.push_back(event);
        }
        if (probe.hits == 1) probe.first_vi = vi;
        probe.last_vi = vi;
        if (!probe.vi_open || probe.cur_vi != vi) {
            close_vi(probe);
            probe.vi_open = true;
            probe.cur_vi = vi;
            probe.cur_vi_count = 0;
        }
        ++probe.cur_vi_count;
        const std::size_t block = vi / kViBlockSize;
        if (block < kViBlocks) {
            ++probe.blocks[block];
        } else {
            ++probe.blocks_overflow;
        }
        if (!probe.gf_open || probe.cur_gf != guest_frames) {
            close_gf(probe);
            probe.gf_open = true;
            probe.cur_gf = guest_frames;
            probe.cur_gf_count = 0;
        }
        ++probe.cur_gf_count;
    }

    void hit(std::size_t index, Cpu& cpu) {
        Probe& probe = probes[index];
        const u32 vi = static_cast<u32>(system.video().frames());
        std::array<u32, 4> x{};
        switch (probe.kind) {
            case Kind::plain:
            case Kind::show_raster: break;
            case Kind::ax_frame: decode_ax_frame(cpu, x); break;
            case Kind::ax_sync_pbs: sample_pbs(cpu, vi, x); break;
            case Kind::pb_sync: decode_pb_sync(cpu, x); break;
            case Kind::ax_force_drop: decode_force_drop(cpu, x); break;
            case Kind::ax_cb_service: decode_cb_service(x); break;
            case Kind::ax_acquire: decode_acquire(cpu, x); break;
            case Kind::axrna_frame: decode_axrna(cpu, x); break;
            case Kind::sofdec_clock: decode_sofdec(cpu, vi, x); break;
            case Kind::movie_getstat: decode_getstat(cpu, vi, x); break;
            case Kind::dmorph_cb: decode_dmorph_cb(cpu, x); break;
            case Kind::geom_lock: decode_geom_lock(cpu, x); break;
            case Kind::reinstance: decode_reinstance(cpu, x); break;
            case Kind::reemit: decode_reemit(cpu, x); break;
        }
        record(probe, cpu, vi, x);
        if (probe.kind == Kind::show_raster) ++guest_frames;
    }

    // --- decoders (all read-only) ----------------------------------------
    // fn_80385798 (AX frame driver): r3 = lessDspCycles, non-zero only on the
    // DSP-resume path (LR 80385A54).
    void decode_ax_frame(const Cpu& cpu, std::array<u32, 4>& x) {
        x[0] = cpu.gpr[3];
        if (cpu.gpr[3] != 0u) {
            ++ax.frame_r3_nonzero;
        } else {
            ++ax.frame_r3_zero;
        }
    }

    // fn_80386474 (per-voice PB<->VPB sync): r3 = VPB.
    void decode_pb_sync(const Cpu& cpu, std::array<u32, 4>& x) {
        const u32 vpb = cpu.gpr[3];
        if (!valid(vpb, kVpbStride)) {
            ++ax.pbsync_bad;
            return;
        }
        const u32 flags = peek32(vpb + kVpbSync);
        x[0] = peek32(vpb + kVpbIndex);
        x[1] = flags;
        if (flags != 0u) ++ax.pbsync_flags_nonzero;
        if (cur_from_vpb(flags)) ++ax.pbsync_cur_from_vpb;
    }

    // __AXPushCallbackStack, only reached from the over-budget drop in
    // __AXSyncPBs (80386D1C): r3 = VPB, r28 = priority, r29 = cycles.
    void decode_force_drop(const Cpu& cpu, std::array<u32, 4>& x) {
        ++ax.drops;
        if (cpu.lr != 0x80386D20u) ++ax.drop_lr_mismatch;
        const u32 vpb = cpu.gpr[3];
        const u32 index = valid(vpb, kVpbStride) ? peek32(vpb + kVpbIndex) : 0xFFFFFFFFu;
        x[0] = cpu.gpr[29];
        x[1] = cpu.gpr[28];
        x[2] = index;
        x[3] = peek32(kDspBudget);
        if (index < kVoices) {
            ax.drop_pending |= u64{1} << index;
        } else {
            ++ax.drop_bad_index;
        }
        if (cpu.gpr[28] < ax.drop_prio.size()) {
            ++ax.drop_prio[cpu.gpr[28]];
        } else {
            ++ax.drop_prio_other;
        }
    }

    // __AXServiceCallbackStack: length of the dropped-voice stack it drains
    // (head at r13+0x5CE8, link VPB+8).
    void decode_cb_service(std::array<u32, 4>& x) {
        u32 node = peek32(kCallbackHead);
        u32 length = 0;
        while (node != 0u && length < kCallbackWalkCap) {
            if (!valid(node, kVpbStride)) {
                ++ax.cb_bad_link;
                break;
            }
            ++length;
            node = peek32(node + kVpbNext);
        }
        if (node != 0u && length == kCallbackWalkCap) ++ax.cb_capped;
        x[0] = length;
        if (length != 0u) ++ax.cb_nonempty;
        ax.cb_total += length;
        ax.cb_max = std::max<u64>(ax.cb_max, length);
    }

    // AXAcquireVoice fn_80384A54 (r3 = priority). Replicates the steal scan
    // (80384A7C..80384B4C) to PREDICT the victim whose callback the blrl at
    // 80384B3C calls. A prediction: an interrupt recognised at the
    // OSDisableInterrupts call before the scan could change the stacks.
    void decode_acquire(const Cpu& cpu, std::array<u32, 4>& x) {
        ++acquire.calls;
        const u32 prio = cpu.gpr[3];
        const u32 free_head = peek32(kStackHeads);
        x[0] = prio;
        x[1] = free_head;
        if (free_head != 0u) return;
        ++acquire.empty_free;
        u32 victim = 0;
        if (prio > 1u) {  // cmplwi r28,1; ble
            if (prio > 32u) ++acquire.prio_out_of_range;
            const u32 last = std::min<u32>(prio, 64u);
            for (u32 p = 1; p < last; ++p) {
                const u32 head = peek32(kStackHeads + 4u * p);
                if (head == 0u) continue;
                const u32 tail = peek32(kStackTails + 4u * p);
                if (head == tail) {
                    victim = head;
                } else if (tail != 0u) {
                    victim = tail;
                } else {
                    continue;
                }
                break;
            }
        }
        if (victim == 0u) {
            ++acquire.predicted_fail;
            return;
        }
        ++acquire.predicted_steal;
        x[2] = victim;
        if (!valid(victim, kVpbStride)) return;
        if (peek16(victim + kVpbState) == 1u) ++acquire.victim_running;
        const u32 callback = peek32(victim + kVpbCallback);
        x[3] = callback;
        if (callback == 0u) return;
        ++acquire.predicted_steal_cb;
        for (std::size_t index = 0; index < acquire.cb_used; ++index) {
            if (acquire.cb[index].lr == callback) {
                ++acquire.cb[index].n;
                return;
            }
        }
        if (acquire.cb_used < kCbTargetSlots) {
            acquire.cb[acquire.cb_used++] = LrSlot{callback, 1};
        } else {
            ++acquire.cb_other;
        }
    }

    // AXRNA per-frame service fn_803CF408 (r3 = object). Exact entry-time
    // prediction of the refill block 803CF4EC (mid-function, not hookable):
    //   nch = extsb(obj+3); vpb = [obj+4+4*nch]; start = [obj+0x14+4*nch]
    //   pos = [vpb+0x1B2] - start; trap (b .) unless 0 <= pos <= [obj+0x20]
    //   w = [obj+4]: w == -1 && pos == 0 -> no refill; w == -1 && pos != 0 ->
    //   obj+4 = 0, w = 0 (803CF498..803CF4B8)
    //   amt = pos > w ? pos - w : 0x1000 - (w - pos), truncated to 0x800
    //   (srawi 11; addze; rlwinm 11,0,20); refill iff amt > 0, then
    //   obj+4 += amt with a 0x1000 wrap.
    // No call precedes the computation, so the prediction is exact; the next
    // entry cross-checks it against obj+4.
    RnaObject* rna_slot(u32 obj) {
        for (RnaObject& slot : rna.objects) {
            if (slot.obj == obj) return &slot;
        }
        if (rna.objects.size() >= kObjSlots) {
            ++rna.objects_overflow;
            return nullptr;
        }
        RnaObject slot;
        slot.obj = obj;
        rna.objects.push_back(slot);
        return &rna.objects.back();
    }

    void decode_axrna(const Cpu& cpu, std::array<u32, 4>& x) {
        ++rna.entries;
        const u32 obj = cpu.gpr[3];
        if (!valid(obj, 0x24u)) {
            ++rna.bad_obj;
            return;
        }
        RnaObject* slot = rna_slot(obj);
        if (slot != nullptr) ++slot->entries;
        const u32 w_raw = peek32(obj + 4u);
        if (slot != nullptr && slot->have_expected) {
            if (w_raw == slot->expected_w) {
                ++slot->writepos_match;
                ++rna.writepos_match;
            } else {
                ++slot->writepos_mismatch;
                ++rna.writepos_mismatch;
            }
            slot->have_expected = false;
        }
        const s32 nch = static_cast<s8>(peek8(obj + 3u));
        const u32 base = obj + static_cast<u32>(nch) * 4u;
        const u32 vpb = peek32(base + 4u);
        x[1] = w_raw;
        x[3] = vpb;
        if (vpb == 0u) {  // beq 803CF570: nothing happens
            ++rna.no_voice;
            if (slot != nullptr) {
                ++slot->no_voice;
                slot->expected_w = w_raw;
                slot->have_expected = true;
            }
            return;
        }
        const u32 start = peek32(base + 0x14u);
        const s32 pos = static_cast<s32>(peek32(vpb + kVpbCurAddr) - start);
        x[0] = static_cast<u32>(pos);
        if (slot != nullptr) {
            if (slot->have_pos && slot->last_pos != pos) {
                ++slot->pos_changes;
                ++rna.pos_changes;
            }
            slot->have_pos = true;
            slot->last_pos = pos;
        }
        const s32 limit = static_cast<s32>(peek32(obj + 0x20u));
        if (pos < 0 || pos > limit) {  // 803CF494 b . (the guest hangs)
            ++rna.trap_predicted;
            if (slot != nullptr) ++slot->trap_predicted;
            return;
        }
        u32 w = w_raw;
        u32 amount = 0;
        if (w == 0xFFFFFFFFu && pos != 0) {
            w = 0u;  // 803CF4B0..803CF4B8: obj+4 = 0
            ++rna.writepos_init;
            if (slot != nullptr) ++slot->writepos_init;
        }
        if (w != 0xFFFFFFFFu) {
            amount = pos > static_cast<s32>(w) ? static_cast<u32>(pos) - w
                                               : 0x1000u - (w - static_cast<u32>(pos));
        }
        const s32 quotient = static_cast<s32>(amount) / 2048;  // srawi 11; addze
        const s32 rounded = static_cast<s32>(static_cast<u32>(quotient) << 11);
        x[2] = static_cast<u32>(rounded);
        u32 next_w = w;
        if (rounded > 0) {
            ++rna.predicted_refills;
            rna.refill_units += static_cast<u32>(rounded);
            if (rounded == 0x1000) ++rna.full_ring;
            next_w = w + static_cast<u32>(rounded);
            if (static_cast<s32>(next_w) >= 0x1000) next_w -= 0x1000u;
            if (slot != nullptr) {
                ++slot->refills;
                slot->refill_units += static_cast<u32>(rounded);
                if (rounded == 0x1000) ++slot->full_ring;
            }
        }
        if (slot != nullptr) {
            slot->expected_w = next_w;
            slot->have_expected = true;
        }
    }

    // fn_803EEA5C (Sofdec clock, reached through a pointer): r3 = handle,
    // state [r3+0x48] (4 = playing path), sub = [r3+0x2004], the running max
    // time kept at sub+0x24 / sub+0x28 (the previous query's answer).
    void decode_sofdec(const Cpu& cpu, u32 vi, std::array<u32, 4>& x) {
        ++sofdec.queries;
        const u32 handle = cpu.gpr[3];
        if (!valid(handle, 0x2008u)) {
            ++sofdec.bad_handle;
            return;
        }
        const u32 state = peek32(handle + 0x48u);
        const u32 sub = peek32(handle + 0x2004u);
        x[0] = state;
        x[1] = sub;
        if (state == 4u) ++sofdec.playing_queries;
        if (!valid(sub, 0x2Cu)) return;
        const u32 ncount = peek32(sub + 0x24u);
        const u32 tscale = peek32(sub + 0x28u);
        x[2] = ncount;
        x[3] = tscale;
        if (sofdec.have_last && sub != sofdec.last_sub) {
            ++sofdec.sub_changes;
        } else if (sofdec.have_last && ncount != sofdec.last_ncount) {
            ++sofdec.ncount_changes;
            if (!sofdec.changed) sofdec.first_change_vi = vi;
            sofdec.changed = true;
            sofdec.last_change_vi = vi;
        }
        sofdec.have_last = true;
        sofdec.last_sub = sub;
        sofdec.last_ncount = ncount;
        sofdec.last_tscale = tscale;
    }

    // mwPlyGetStat 0x804086D8: handle+0 magic, handle+4 MwePlyStat
    // (1 PREP, 2 PLAYING, 3 PLAYEND). Read BEFORE the production handler,
    // i.e. before any PLAYEND stub write.
    void decode_getstat(const Cpu& cpu, u32 vi, std::array<u32, 4>& x) {
        ++movie.polls;
        const u32 handle = cpu.gpr[3];
        if (!valid(handle, 8u)) {
            ++movie.bad_handle;
            return;
        }
        const u32 status = peek32(handle + 4u);
        x[0] = peek32(handle);
        x[1] = status;
        ++movie.status_hist[status <= 3u ? status : 4u];
        if (status == 3u) movie.ever_playend = true;
        MovieHandle* entry = nullptr;
        for (MovieHandle& candidate : movie.handles) {
            if (candidate.handle == handle) entry = &candidate;
        }
        u32 from = 0xFFFFFFFFu;
        if (entry == nullptr) {
            if (movie.handles.size() < kMovieHandles) {
                movie.handles.push_back(MovieHandle{handle, status, 0});
                entry = &movie.handles.back();
            } else {
                ++movie.handles_overflow;
            }
        } else {
            from = entry->last_status;
        }
        if (entry != nullptr) {
            ++entry->polls;
            entry->last_status = status;
        }
        if (from != status) {
            if (movie.transitions.size() < kMovieTransitionCap) {
                movie.transitions.push_back(MovieTransition{vi, guest_frames, handle, from, status});
            } else {
                ++movie.transitions_overflow;
            }
        }
    }

    // DMorph software render callback 0x80444AC4: r3 = atomic.
    void decode_dmorph_cb(const Cpu& cpu, std::array<u32, 4>& x) {
        ++dmorph.cb_entries;
        x[0] = cpu.gpr[3];
        if (valid(cpu.gpr[3], 0x4Cu)) x[1] = peek32(cpu.gpr[3] + 0x18u);
    }

    // RpGeometryLock 0x8045A1BC: r3 = geometry, r4 = lock mode. Its only bl
    // caller is the DMorph callback (LR 80444B74).
    void decode_geom_lock(const Cpu& cpu, std::array<u32, 4>& x) {
        ++dmorph.lock_entries;
        if (cpu.lr == 0x80444B74u) ++dmorph.lock_from_dmorph;
        x[0] = cpu.gpr[3];
        x[1] = cpu.gpr[4];
    }

    // fn_80464890 (reinstance): r3 = atomic; geometry [r3+0x18]; re-emit
    // (fn_80464624, its only caller site 80464938) needs u16[geom+0xC]
    // (lockedSinceLastInst) != 0. The atomic's render callback [r3+0x48]
    // attributes the pending re-emit to DMorph or not.
    void decode_reinstance(const Cpu& cpu, std::array<u32, 4>& x) {
        ++dmorph.reinstance;
        if (dmorph.pending != 0) ++dmorph.pending_unconsumed;
        dmorph.pending = 0;
        const u32 atomic = cpu.gpr[3];
        if (!valid(atomic, 0x4Cu)) return;
        const u32 callback = peek32(atomic + 0x48u);
        const u32 geometry = peek32(atomic + 0x18u);
        const u32 locked = valid(geometry, 0x0Eu) ? peek16(geometry + 0x0Cu) : 0u;
        const bool is_dmorph = callback == kDMorphCallback;
        x[0] = callback;
        x[1] = geometry;
        x[2] = locked;
        x[3] = is_dmorph ? 1u : 0u;
        if (locked == 0u) return;
        ++dmorph.locked;
        if (is_dmorph) {
            ++dmorph.locked_dmorph;
            dmorph.pending = 1;
        } else {
            ++dmorph.locked_other;
            dmorph.pending = 2;
        }
    }

    void decode_reemit(const Cpu& cpu, std::array<u32, 4>& x) {
        ++dmorph.reemit;
        if (cpu.lr != 0x8046493Cu) ++dmorph.reemit_lr_mismatch;
        x[0] = static_cast<u32>(dmorph.pending);
        switch (dmorph.pending) {
            case 1: ++dmorph.reemit_dmorph; break;
            case 2: ++dmorph.reemit_other; break;
            default: ++dmorph.reemit_unattributed; break;
        }
        dmorph.pending = 0;
    }

    // --- AX PB sampler at __AXSyncPBs entry --------------------------------
    // Runs before the AX mixer's run_ax_frame (the production handler chained
    // behind this observer) and before the guest body. So the change seen
    // between two observations is: the mixer's advance at the previous call,
    // plus the guest sync at the previous call, which consumed the VPB sync
    // flags read at that previous observation (prev_flags), plus any force
    // drop inside it (drop_pending, from the ax_force_drop probe).
    void close_series(u64 lists_now) {
        if (!ax.series_open) return;
        if (ax.series.size() < kSeriesCap) {
            SeriesRow row;
            row.vi = ax.series_vi;
            row.syncs = ax.series_syncs;
            row.running = static_cast<u32>(std::bitset<64>(ax.series_running).count());
            row.advanced = static_cast<u32>(std::bitset<64>(ax.series_advanced).count());
            row.lists = lists_now - ax.series_lists_start;
            ax.series.push_back(row);
        } else {
            ++ax.series_overflow;
        }
        ax.series_open = false;
    }

    bool validate_pb_table() {
        bool physical = false;
        for (u32 index = 0; index < kVoices; ++index) {
            const u32 pb = kPbBase + index * kPbStride;
            const u32 self = peek32(pb + 4u);
            if (self == pb) continue;
            // The SDK's OSCachedToPhysical form of the address (the AX microcode contract).
            if (self == pb - GuestMemory::kConventionBase) {
                physical = true;
                continue;
            }
            return false;
        }
        ax.pb_self_physical = physical;
        return true;
    }

    void sample_pbs(const Cpu& cpu, u32 vi, std::array<u32, 4>& x) {
        ++ax.sync_calls;
        const u64 lists = system.audio().stats().command_lists;
        if (ax.series_open && vi < ax.series_vi) {  // state load: VI went back
            ++ax.vi_regressions;
            ax.have_prev = false;
            ax.drop_pending = 0;
        }
        if (!ax.series_open || vi != ax.series_vi) {
            close_series(lists);
            ax.series_open = true;
            ax.series_vi = vi;
            ax.series_syncs = 0;
            ax.series_running = 0;
            ax.series_advanced = 0;
            ax.series_lists_start = lists;
        }
        ++ax.series_syncs;
        if (!ax.pb_table_valid) {
            ax.pb_table_valid = validate_pb_table();
            if (!ax.pb_table_valid) {
                ++ax.validation_failures;
                return;
            }
        }
        const u32 budget = peek32(kDspBudget);
        const u32 used = peek32(kDspUsed);
        ax.budget_last = budget;
        ax.used_last = used;
        ax.used_max = std::max(ax.used_max, used);
        if (used > budget) ++ax.over_budget;

        const u64 dropped = ax.drop_pending;
        u32 running_now = 0;
        for (u32 index = 0; index < kVoices; ++index) {
            const u32 pb = kPbBase + index * kPbStride;
            const u32 vpb = kVpbBase + index * kVpbStride;
            const u16 state = peek16(pb + kPbState);
            const u32 cur = peek32(pb + kPbCurAddr);
            const u32 flags = peek32(vpb + kVpbSync);
            const u64 bit = u64{1} << index;
            if (ax.have_prev) {
                const u16 prev_state = ax.prev_state[index];
                const u32 prev_flags = ax.prev_flags[index];
                const bool changed = cur != ax.prev_cur[index];
                if (prev_state == 1u && state == 1u) {
                    ++ax.running_both;
                    if (cur_from_vpb(prev_flags)) {
                        if (changed) {
                            ++ax.guest_rewrite;
                        } else {
                            ++ax.cur_rewrite_same;
                        }
                    } else {
                        if (changed) {
                            ++ax.advanced_dsp;
                            ax.series_advanced |= bit;
                        } else {
                            ++ax.frozen;
                        }
                        if (prev_flags != 0u) ++ax.dirty_no_cur;
                    }
                } else if (prev_state == 0u && state == 1u) {
                    ++ax.starts;
                } else if (prev_state == 1u && state == 0u) {
                    if ((dropped & bit) != 0u) {
                        ++ax.stops_drop;
                    } else if (state_from_vpb(prev_flags)) {
                        ++ax.stops_guest;
                    } else {
                        ++ax.stops_dsp;
                    }
                }
            }
            if (state == 1u) {
                ++ax.running;
                ++running_now;
                ax.series_running |= bit;
                if (peek16(pb + kPbAuxAL) != 0u || peek16(pb + kPbAuxAR) != 0u || peek16(pb + kPbAuxBL) != 0u ||
                    peek16(pb + kPbAuxBR) != 0u) {
                    ++ax.aux_nonzero;
                }
                if (peek16(pb + kPbVolDelta) != 0u) ++ax.vdelta_nonzero;
            }
            ax.prev_state[index] = state;
            ax.prev_cur[index] = cur;
            ax.prev_flags[index] = flags;
        }
        ax.have_prev = true;
        ax.drop_pending = 0;

        for (Dump& dump : ax.dumps) {
            if (dump.taken || vi < dump.target) continue;
            dump.taken = true;
            dump.vi = vi;
            dump.instr = cpu.game_clock().now();
            for (u32 index = 0; index < kVoices; ++index) {
                const u32 pb = kPbBase + index * kPbStride;
                const u32 vpb = kVpbBase + index * kVpbStride;
                VoiceDump voice;
                voice.index = index;
                voice.state = peek16(pb + kPbState);
                voice.vprio = peek32(vpb + kVpbPrio);
                if (voice.state == 0u && voice.vprio == 0u) continue;
                voice.loop = peek16(pb + kPbLoopFlag);
                voice.format = peek16(pb + kPbFormat);
                voice.loop_addr = peek32(pb + kPbLoopAddr);
                voice.end = peek32(pb + kPbEndAddr);
                voice.cur = peek32(pb + kPbCurAddr);
                voice.ratio = peek32(pb + kPbRatio);
                voice.vol = peek16(pb + kPbVol);
                voice.vdelta = peek16(pb + kPbVolDelta);
                voice.aux_al = peek16(pb + kPbAuxAL);
                voice.vsync = peek32(vpb + kVpbSync);
                voice.vstate = peek16(vpb + kVpbState);
                voice.vcur = peek32(vpb + kVpbCurAddr);
                voice.vcb = peek32(vpb + kVpbCallback);
                dump.voices.push_back(voice);
            }
        }
        x[0] = running_now;
        x[1] = budget;
        x[2] = used;
        x[3] = static_cast<u32>(std::bitset<64>(dropped).count());
    }

    // --- per-VI host-service sampler ---------------------------------------
    void on_host_service() {
        ++host_service_ticks;
        const u32 vi = static_cast<u32>(system.video().frames());
        if (have_vi && vi == last_vi) return;
        if (have_vi && vi < last_vi) ++host_vi_regressions;
        if (have_vi && vi > last_vi + 1u) vi_gaps += vi - last_vi - 1u;
        const bool first = !have_vi;
        have_vi = true;
        last_vi = vi;
        ++vi_ticks;
        for (Sampler& sampler : samplers) {
            const u32 value = peek_sized(sampler.ea, sampler.size);
            ++sampler.samples;
            if (!sampler.have) {
                sampler.have = true;
                sampler.first = value;
                sampler.log.emplace_back(vi, value);
            } else if (value != sampler.last) {
                ++sampler.changes;
                if (sampler.log.size() < sampler.log_limit) {
                    sampler.log.emplace_back(vi, value);
                } else {
                    ++sampler.log_overflow;
                }
            }
            sampler.last = value;
        }
        const u64 lists = system.audio().stats().command_lists;
        const u32 list_address = system.audio().last_command_list();
        const System::AxMixerReport& mixer = system.ax_mixer_report();
        if (!first) {
            lists_hist.add(lists - last_lists);
            mixer_frames_hist.add(mixer.frames - last_mixer_frames);
            voice_advance_hist.add(mixer.voice_advances - last_voice_advances);
            if (list_address != last_list_address) ++list_address_changes;
        }
        last_lists = lists;
        last_list_address = list_address;
        last_mixer_frames = mixer.frames;
        last_voice_advances = mixer.voice_advances;
    }

    // --- output --------------------------------------------------------------
    bool still_installed(const Probe& probe) const {
        const std::string* name = system.cpu().hle_name(probe.address);
        if (name == nullptr) return false;
        const std::string token = "measure: " + probe.name;
        for (std::size_t at = name->find(token); at != std::string::npos; at = name->find(token, at + 1)) {
            const std::size_t end = at + token.size();
            const bool starts = at == 0 || (at >= 3 && name->compare(at - 3, 3, " + ") == 0);
            const bool ends = end == name->size() || name->compare(end, 3, " + ") == 0;
            if (starts && ends) return true;
        }
        return false;
    }

    static void write_hist(JsonWriter& j, const DeltaHist& hist) {
        j.begin_object();
        j.key("total").u(hist.total);
        j.key("bins_0_1_2_3_4_5to8_9to16_17plus").begin_array();
        for (const u64 bin : hist.bins) j.u(bin);
        j.end_array();
        j.end_object();
    }

    void write_decoded(JsonWriter& j, const Probe& probe) const {
        j.key("decoded").begin_object();
        switch (probe.kind) {
            case Kind::plain: break;
            case Kind::show_raster: j.key("guest_frames").u(guest_frames); break;
            case Kind::ax_frame:
                j.key("r3_zero").u(ax.frame_r3_zero);
                j.key("r3_nonzero").u(ax.frame_r3_nonzero);
                break;
            case Kind::ax_sync_pbs: j.key("section").str("ax"); break;
            case Kind::pb_sync:
                j.key("flags_nonzero").u(ax.pbsync_flags_nonzero);
                j.key("cur_from_vpb").u(ax.pbsync_cur_from_vpb);
                j.key("bad_vpb").u(ax.pbsync_bad);
                break;
            case Kind::ax_force_drop: {
                j.key("drops").u(ax.drops);
                j.key("bad_index").u(ax.drop_bad_index);
                j.key("lr_mismatch").u(ax.drop_lr_mismatch);
                j.key("by_priority").begin_array();
                for (const u64 n : ax.drop_prio) j.u(n);
                j.end_array();
                j.key("priority_other").u(ax.drop_prio_other);
                break;
            }
            case Kind::ax_cb_service:
                j.key("nonempty").u(ax.cb_nonempty);
                j.key("callbacks_total").u(ax.cb_total);
                j.key("max_length").u(ax.cb_max);
                j.key("capped").u(ax.cb_capped);
                j.key("bad_link").u(ax.cb_bad_link);
                break;
            case Kind::ax_acquire: {
                j.key("calls").u(acquire.calls);
                j.key("empty_free").u(acquire.empty_free);
                j.key("predicted_steal").u(acquire.predicted_steal);
                j.key("predicted_steal_cb").u(acquire.predicted_steal_cb);
                j.key("victim_running").u(acquire.victim_running);
                j.key("predicted_fail").u(acquire.predicted_fail);
                j.key("prio_out_of_range").u(acquire.prio_out_of_range);
                j.key("cb_targets").begin_array();
                for (std::size_t index = 0; index < acquire.cb_used; ++index) {
                    j.begin_object();
                    j.key("cb").hex(acquire.cb[index].lr);
                    j.key("n").u(acquire.cb[index].n);
                    j.end_object();
                }
                j.end_array();
                j.key("cb_other").u(acquire.cb_other);
                break;
            }
            case Kind::axrna_frame: j.key("section").str("axrna"); break;
            case Kind::sofdec_clock: j.key("section").str("sofdec"); break;
            case Kind::movie_getstat: j.key("section").str("movie"); break;
            case Kind::dmorph_cb:
            case Kind::geom_lock:
            case Kind::reinstance:
            case Kind::reemit: j.key("section").str("dmorph"); break;
        }
        j.end_object();
    }

    void write_probe(JsonWriter& j, const Probe& probe) const {
        j.begin_object();
        j.key("name").str(probe.name);
        j.key("address").hex(probe.address);
        j.key("entry_translated").b(probe.entry_translated);
        j.key("installed").b(probe.installed);
        if (!probe.reject.empty()) j.key("not_installed_reason").str(probe.reject);
        j.key("chained_with").str(probe.chained_with);
        j.key("still_installed").b(probe.installed && still_installed(probe));
        j.key("executed").b(probe.hits > 0);
        j.key("hits").u(probe.hits);
        if (probe.observe_numeric) {
            j.key("numeric_entry_evidence").begin_object();
            j.key("hits").u(probe.numeric.hits);
            j.key("unclassified").u(probe.numeric.unclassified);
            j.key("complete_for_observed_entries").b(probe.numeric.complete());
            j.key("whole_game_proof").b(false);
            j.key("capacity").u(probe.numeric.slots.size());
            j.key("states").begin_array();
            for (std::size_t i = 0; i < probe.numeric.used; ++i) {
                const auto& slot = probe.numeric.slots[i];
                j.begin_object();
                j.key("hits").u(slot.hits);
                j.key("fpscr").hex(slot.key.status);
                j.key("gqr").begin_array();
                for (const auto value : slot.key.formats) j.hex(value);
                j.end_array();
                j.end_object();
            }
            j.end_array();
            j.end_object();
            j.key("entry_r4_evidence").begin_object();
            j.key("hits").u(probe.entry_r4.hits);
            j.key("unclassified").u(probe.entry_r4.unclassified);
            j.key("complete_for_observed_entries").b(probe.entry_r4.complete());
            j.key("whole_game_proof").b(false);
            j.key("capacity").u(probe.entry_r4.slots.size());
            j.key("values").begin_array();
            for (std::size_t i = 0; i < probe.entry_r4.used; ++i) {
                const auto& slot = probe.entry_r4.slots[i];
                j.begin_object();
                j.key("r4").hex(slot.key.value);
                j.key("hits").u(slot.hits);
                j.end_object();
            }
            j.end_array();
            j.end_object();
        }
        j.key("expected_lr").begin_array();
        for (const u32 lr : probe.expected_lr) {
            if (lr != 0u) j.hex(lr);
        }
        j.end_array();
        j.key("lr").begin_array();
        for (std::size_t index = 0; index < probe.lr_used; ++index) {
            j.begin_object();
            j.key("lr").hex(probe.lr[index].lr);
            j.key("n").u(probe.lr[index].n);
            j.end_object();
        }
        j.end_array();
        j.key("lr_other").u(probe.lr_other);

        // Close the open buckets on a copy: json() never mutates state.
        Probe closed_counts;
        closed_counts.vi_open = probe.vi_open;
        closed_counts.cur_vi_count = probe.cur_vi_count;
        closed_counts.vi_frames = probe.vi_frames;
        closed_counts.vi_max = probe.vi_max;
        closed_counts.gf_open = probe.gf_open;
        closed_counts.cur_gf_count = probe.cur_gf_count;
        closed_counts.gf_frames = probe.gf_frames;
        closed_counts.gf_max = probe.gf_max;
        closed_counts.gf_hist = probe.gf_hist;
        close_vi(closed_counts);
        close_gf(closed_counts);

        j.key("per_vi").begin_object();
        j.key("frames_with_hits").u(closed_counts.vi_frames);
        j.key("max").u(closed_counts.vi_max);
        j.key("first").u(probe.hits > 0 ? probe.first_vi : 0u);
        j.key("last").u(probe.hits > 0 ? probe.last_vi : 0u);
        j.key("block_size").u(kViBlockSize);
        std::size_t used_blocks = probe.blocks.size();
        while (used_blocks > 0 && probe.blocks[used_blocks - 1] == 0u) --used_blocks;
        j.key("blocks").begin_array();
        for (std::size_t index = 0; index < used_blocks; ++index) j.u(probe.blocks[index]);
        j.end_array();
        j.key("blocks_overflow").u(probe.blocks_overflow);
        j.end_object();

        j.key("per_guest_frame").begin_object();
        j.key("frames_with_hits").u(closed_counts.gf_frames);
        j.key("max").u(closed_counts.gf_max);
        j.key("hist_1_2_3to4_5to8_9to16_17to64_65plus").begin_array();
        for (const u64 bin : closed_counts.gf_hist) j.u(bin);
        j.end_array();
        j.end_object();

        j.key("first").begin_array();
        std::size_t first_index = 0;
        for (const Event& event : probe.first) {
            j.begin_object();
            j.key("instr").u(event.instr);
            j.key("vi").u(event.vi);
            j.key("gf").u(event.gf);
            j.key("lr").hex(event.lr);
            j.key("r3").hex(event.r[0]);
            j.key("r4").hex(event.r[1]);
            j.key("r5").hex(event.r[2]);
            j.key("r6").hex(event.r[3]);
            j.key("r7").hex(event.r[4]);
            if (probe.observe_numeric) {
                const auto& numeric = probe.first_numeric[first_index];
                j.key("fpscr").hex(numeric.status);
                j.key("gqr").begin_array();
                for (const auto value : numeric.formats) j.hex(value);
                j.end_array();
            }
            j.key("x").begin_array();
            for (const u32 value : event.x) j.hex(value);
            j.end_array();
            j.end_object();
            ++first_index;
        }
        j.end_array();
        write_decoded(j, probe);
        j.end_object();
    }

    void write_ax(JsonWriter& j) const {
        j.key("ax").begin_object();
        j.key("sync_calls").u(ax.sync_calls);
        j.key("pb_table_valid").b(ax.pb_table_valid);
        j.key("pb_self_pointer_physical").b(ax.pb_self_physical);
        j.key("validation_failures").u(ax.validation_failures);
        j.key("pb_base").hex(kPbBase);
        j.key("pb_stride").u(kPbStride);
        j.key("vpb_base").hex(kVpbBase);
        j.key("vpb_stride").u(kVpbStride);
        j.key("voice_frames").begin_object();
        j.key("running").u(ax.running);
        j.key("running_both").u(ax.running_both);
        j.key("advanced_dsp").u(ax.advanced_dsp);
        j.key("guest_rewrite").u(ax.guest_rewrite);
        j.key("guest_rewrite_same_value").u(ax.cur_rewrite_same);
        j.key("frozen").u(ax.frozen);
        j.key("dirty_not_cur_rewriting").u(ax.dirty_no_cur);
        j.key("starts").u(ax.starts);
        j.key("stops_dsp").u(ax.stops_dsp);
        j.key("stops_guest").u(ax.stops_guest);
        j.key("stops_drop").u(ax.stops_drop);
        j.end_object();
        j.key("axsem").begin_object();
        j.key("aux_nonzero").u(ax.aux_nonzero);
        j.key("vdelta_nonzero").u(ax.vdelta_nonzero);
        j.end_object();
        j.key("budget").begin_object();
        j.key("last").u(ax.budget_last);
        j.key("used_last").u(ax.used_last);
        j.key("max_used").u(ax.used_max);
        j.key("over_budget").u(ax.over_budget);
        j.end_object();
        j.key("series_columns").begin_array();
        for (const char* column : {"vi", "syncs", "running", "advanced", "dsp_lists"}) j.str(column);
        j.end_array();
        j.key("series").begin_array();
        auto row_out = [&j](const SeriesRow& row) {
            j.begin_array();
            j.u(row.vi);
            j.u(row.syncs);
            j.u(row.running);
            j.u(row.advanced);
            j.u(row.lists);
            j.end_array();
        };
        for (const SeriesRow& row : ax.series) row_out(row);
        if (ax.series_open && ax.series.size() < kSeriesCap) {
            SeriesRow row;
            row.vi = ax.series_vi;
            row.syncs = ax.series_syncs;
            row.running = static_cast<u32>(std::bitset<64>(ax.series_running).count());
            row.advanced = static_cast<u32>(std::bitset<64>(ax.series_advanced).count());
            row.lists = system.audio().stats().command_lists - ax.series_lists_start;
            row_out(row);
        }
        j.end_array();
        j.key("series_overflow").u(ax.series_overflow);
        j.key("vi_regressions").u(ax.vi_regressions);
        j.key("dumps").begin_array();
        for (const Dump& dump : ax.dumps) {
            j.begin_object();
            j.key("target").u(dump.target);
            j.key("taken").b(dump.taken);
            j.key("vi").u(dump.vi);
            j.key("instr").u(dump.instr);
            j.key("voices").begin_array();
            for (const VoiceDump& v : dump.voices) {
                j.begin_object();
                j.key("i").u(v.index);
                j.key("state").u(v.state);
                j.key("loop").u(v.loop);
                j.key("fmt").u(v.format);
                j.key("loop_addr").hex(v.loop_addr);
                j.key("end").hex(v.end);
                j.key("cur").hex(v.cur);
                j.key("ratio").hex(v.ratio);
                j.key("vol").u(v.vol);
                j.key("vdelta").u(v.vdelta);
                j.key("aux_al").u(v.aux_al);
                j.key("vprio").u(v.vprio);
                j.key("vsync").hex(v.vsync);
                j.key("vstate").u(v.vstate);
                j.key("vcur").hex(v.vcur);
                j.key("vcb").hex(v.vcb);
                j.end_object();
            }
            j.end_array();
            j.end_object();
        }
        j.end_array();
        j.end_object();
    }

    void write_sections(JsonWriter& j) const {
        j.key("axrna").begin_object();
        j.key("entries").u(rna.entries);
        j.key("no_voice").u(rna.no_voice);
        j.key("bad_obj").u(rna.bad_obj);
        j.key("predicted_refills").u(rna.predicted_refills);
        j.key("refill_units").u(rna.refill_units);
        j.key("full_ring_refills").u(rna.full_ring);
        j.key("trap_predicted").u(rna.trap_predicted);
        j.key("pos_changes").u(rna.pos_changes);
        j.key("writepos_init").u(rna.writepos_init);
        j.key("writepos_match").u(rna.writepos_match);
        j.key("writepos_mismatch").u(rna.writepos_mismatch);
        j.key("objects").begin_array();
        for (const RnaObject& o : rna.objects) {
            j.begin_object();
            j.key("obj").hex(o.obj);
            j.key("entries").u(o.entries);
            j.key("no_voice").u(o.no_voice);
            j.key("refills").u(o.refills);
            j.key("refill_units").u(o.refill_units);
            j.key("full_ring").u(o.full_ring);
            j.key("trap_predicted").u(o.trap_predicted);
            j.key("pos_changes").u(o.pos_changes);
            j.key("writepos_init").u(o.writepos_init);
            j.key("writepos_match").u(o.writepos_match);
            j.key("writepos_mismatch").u(o.writepos_mismatch);
            j.key("last_pos").i(o.last_pos);
            j.end_object();
        }
        j.end_array();
        j.key("objects_overflow").u(rna.objects_overflow);
        j.end_object();

        j.key("sofdec").begin_object();
        j.key("queries").u(sofdec.queries);
        j.key("playing_queries").u(sofdec.playing_queries);
        j.key("bad_handle").u(sofdec.bad_handle);
        j.key("sub_changes").u(sofdec.sub_changes);
        j.key("ncount_changes").u(sofdec.ncount_changes);
        j.key("first_change_vi").u(sofdec.changed ? sofdec.first_change_vi : 0u);
        j.key("last_change_vi").u(sofdec.changed ? sofdec.last_change_vi : 0u);
        j.key("last_sub").hex(sofdec.last_sub);
        j.key("last_ncount").u(sofdec.last_ncount);
        j.key("last_tscale").u(sofdec.last_tscale);
        j.end_object();

        j.key("movie").begin_object();
        j.key("polls").u(movie.polls);
        j.key("bad_handle").u(movie.bad_handle);
        j.key("status_hist_0_1_2_3_other").begin_array();
        for (const u64 n : movie.status_hist) j.u(n);
        j.end_array();
        j.key("ever_playend").b(movie.ever_playend);
        j.key("handles").begin_array();
        for (const MovieHandle& h : movie.handles) {
            j.begin_object();
            j.key("handle").hex(h.handle);
            j.key("last_status").u(h.last_status);
            j.key("polls").u(h.polls);
            j.end_object();
        }
        j.end_array();
        j.key("handles_overflow").u(movie.handles_overflow);
        j.key("transitions").begin_array();
        for (const MovieTransition& t : movie.transitions) {
            j.begin_object();
            j.key("vi").u(t.vi);
            j.key("gf").u(t.gf);
            j.key("handle").hex(t.handle);
            j.key("from").hex(t.from);
            j.key("to").hex(t.to);
            j.end_object();
        }
        j.end_array();
        j.key("transitions_overflow").u(movie.transitions_overflow);
        j.end_object();

        j.key("dmorph").begin_object();
        j.key("dmorph_cb_entries").u(dmorph.cb_entries);
        j.key("geom_lock_entries").u(dmorph.lock_entries);
        j.key("geom_lock_from_dmorph").u(dmorph.lock_from_dmorph);
        j.key("reinstance_entries").u(dmorph.reinstance);
        j.key("reinstance_locked").u(dmorph.locked);
        j.key("reinstance_locked_dmorph").u(dmorph.locked_dmorph);
        j.key("reinstance_locked_other").u(dmorph.locked_other);
        j.key("pending_unconsumed").u(dmorph.pending_unconsumed);
        j.key("reemit_entries").u(dmorph.reemit);
        j.key("reemit_dmorph").u(dmorph.reemit_dmorph);
        j.key("reemit_other").u(dmorph.reemit_other);
        j.key("reemit_unattributed").u(dmorph.reemit_unattributed);
        j.key("reemit_lr_mismatch").u(dmorph.reemit_lr_mismatch);
        j.end_object();

        j.key("samplers").begin_array();
        for (const Sampler& s : samplers) {
            j.begin_object();
            j.key("address").hex(s.ea);
            j.key("size").u(s.size);
            j.key("log_capacity").u(s.log_limit);
            j.key("log_retained_capacity").u(s.log.capacity());
            j.key("log_retained_bytes").u(s.log.capacity() * sizeof(std::pair<u32, u32>));
            j.key("samples").u(s.samples);
            j.key("changes").u(s.changes);
            j.key("first").hex(s.first);
            j.key("last").hex(s.last);
            j.key("log").begin_array();
            for (const auto& entry : s.log) {
                j.begin_array();
                j.u(entry.first);
                j.hex(entry.second);
                j.end_array();
            }
            j.end_array();
            j.key("log_overflow").u(s.log_overflow);
            j.end_object();
        }
        j.end_array();

        j.key("per_vi_host").begin_object();
        j.key("dsp_command_lists").begin_object();
        j.key("total").u(system.audio().stats().command_lists);
        j.key("per_vi");
        write_hist(j, lists_hist);
        j.key("last_address").hex(system.audio().last_command_list());
        j.key("address_changes").u(list_address_changes);
        j.end_object();
        j.key("mixer_frames_per_vi");
        write_hist(j, mixer_frames_hist);
        j.key("mixer_voice_advances_per_vi");
        write_hist(j, voice_advance_hist);
        j.end_object();
    }
};

// ---------------------------------------------------------------------------

bool MeasureProbes::parse_spec(const std::string& spec, Config& out, std::string& error) {
    Config config;
    for (const std::string& clause : split(spec, ';')) {
        const std::size_t equals = clause.find('=');
        if (equals == std::string::npos) {
            error = "expected key=value, got '" + clause + "'";
            return false;
        }
        const std::string key = clause.substr(0, equals);
        const std::string value = clause.substr(equals + 1);
        if (key == "sets") {
            config.sets = split(value, ',');
            for (const std::string& set : config.sets) {
                if (!set_known(set)) {
                    error = "unknown probe set '" + set + "' (r5, r4, all, none)";
                    return false;
                }
            }
        } else if (key == "pb") {
            config.pb_dump_frames.clear();
            if (value != "none") {
                for (const std::string& item : split(value, ',')) {
                    u32 frame = 0;
                    if (!parse_number(item, frame)) {
                        error = "bad pb frame '" + item + "'";
                        return false;
                    }
                    config.pb_dump_frames.push_back(frame);
                }
            }
            if (config.pb_dump_frames.size() > kMaxPbDumpFrames) {
                error = "at most " + std::to_string(kMaxPbDumpFrames) + " pb dump frames";
                return false;
            }
        } else if (key == "sample") {
            config.samples.clear();
            config.default_samples = false;
            if (value != "none") {
                for (const std::string& item : split(value, ',')) {
                    const auto fields = split(item, ':');
                    u32 address = 0;
                    u32 size = 4;
                    u32 capacity = kDefaultSampleLogEntries;
                    if (fields.empty() || fields.size() > 3u ||
                        static_cast<std::size_t>(std::count(item.begin(), item.end(), ':')) + 1u != fields.size() ||
                        !parse_number(fields[0], address) ||
                        (fields.size() > 1u && !parse_number(fields[1], size)) ||
                        (fields.size() > 2u && !parse_number(fields[2], capacity))) {
                        error = "bad sample '" + item + "' (ea[:1|2|4[:log-capacity]])";
                        return false;
                    }
                    if (size != 1u && size != 2u && size != 4u) {
                        error = "sample size must be 1, 2 or 4: '" + item + "'";
                        return false;
                    }
                    if (address < GuestMemory::kImageBase) {
                        error = "sample address is below the game's address window: '" + item + "'";
                        return false;
                    }
                    if (capacity == 0u || capacity > kMaxSampleLogEntries) {
                        error = "sample change-log capacity must be 1.." + std::to_string(kMaxSampleLogEntries);
                        return false;
                    }
                    config.samples.push_back({address, size, capacity});
                }
            }
            if (config.samples.size() > kMaxSamples) {
                error = "at most " + std::to_string(kMaxSamples) + " samples";
                return false;
            }
        } else if (key == "extra" || key == "numeric") {
            auto& entries = key == "extra" ? config.extra_entries : config.numeric_entries;
            entries.clear();
            for (const std::string& item : split(value, ',')) {
                u32 address = 0;
                if (!parse_number(item, address) || address == 0u || (address & 3u) != 0u) {
                    error = "bad extra entry '" + item + "' (non-zero, 4-byte aligned)";
                    return false;
                }
                entries.push_back(address);
            }
            if (entries.size() > kMaxExtraEntries) {
                error = "at most " + std::to_string(kMaxExtraEntries) + " extra entries";
                return false;
            }
        } else if (key == "first") {
            u32 first = 0;
            if (!parse_number(value, first) || first > kMaxFirstHits) {
                error = "first must be 0.." + std::to_string(kMaxFirstHits);
                return false;
            }
            config.first_hits = first;
        } else if (key == "ordered") {
            if (!parse_number(value, config.ordered_events) || config.ordered_events > kMaxOrderedEvents) {
                error = "ordered must be 0.." + std::to_string(kMaxOrderedEvents);
                return false;
            }
        } else if (key == "label") {
            if (value.size() > kMaxLabel) {
                error = "label longer than " + std::to_string(kMaxLabel) + " characters";
                return false;
            }
            config.route_label = value;
        } else {
            error = "unknown key '" + key + "' (sets, pb, sample, extra, numeric, first, ordered, label)";
            return false;
        }
    }
    if (config.ordered_events != 0 && config.numeric_entries.empty()) {
        error = "ordered observations require explicit numeric entries";
        return false;
    }
    out = std::move(config);
    return true;
}

MeasureProbes::MeasureProbes(System& system, Config config)
    : impl_(std::make_shared<Impl>(system, std::move(config))) {
    impl_->build();
}

MeasureProbes::~MeasureProbes() {
    MeasureProbes* self = this;
    g_active.compare_exchange_strong(self, nullptr);
}

void MeasureProbes::install() {
    Impl& impl = *impl_;
    if (impl.installed_once) return;
    impl.installed_once = true;
    Cpu& cpu = impl.system.cpu();
    const RecompiledCode* code = cpu.recompiled();
    for (std::size_t index = 0; index < impl.probes.size(); ++index) {
        Impl::Probe& probe = impl.probes[index];
        if (code == nullptr) {
            probe.reject = "no recompiled code attached";
            continue;
        }
        // A declared entry that also BEGINS a translated range. lookup() alone
        // also accepts dispatcher continuation entries (whose thunks never call
        // try_hle) and overlapping pseudo-entries (hooking one would reroute
        // the dispatcher into a different translation), and a hooked address
        // with no range ends in the dispatcher's fatal stop.
        const RecompiledCode::Range* range = code->find_range(probe.address);
        probe.entry_translated =
            code->lookup(probe.address) != nullptr && range != nullptr && range->begin == probe.address;
        if (!probe.entry_translated) {
            probe.reject = "not a translated function entry";
            continue;
        }
        if (const std::string* existing = cpu.hle_name(probe.address)) probe.chained_with = *existing;
        std::shared_ptr<Impl> shared = impl_;
        impl.system.add_hle_observer(
            probe.address, [shared, index](Cpu& c) { shared->hit(index, c); }, "measure: " + probe.name);
        probe.installed = true;
    }
}

void MeasureProbes::on_host_service() { impl_->on_host_service(); }

std::string MeasureProbes::json() const {
    const Impl& impl = *impl_;
    JsonWriter j;
    j.begin_object();
    j.key("schema").u(1);
    j.key("route_label").str(impl.config.route_label);
    j.key("config").begin_object();
    j.key("sets").begin_array();
    for (const std::string& set : impl.config.sets) j.str(set);
    j.end_array();
    j.key("pb_dump_frames").begin_array();
    for (const u32 frame : impl.config.pb_dump_frames) j.u(frame);
    j.end_array();
    j.key("first_hits").u(impl.config.first_hits);
    j.key("ordered_events").u(impl.config.ordered_events);
    j.key("numeric_entries").begin_array();
    for (const u32 address : impl.config.numeric_entries) j.hex(address);
    j.end_array();
    j.key("samples").begin_array();
    for (const auto& sample : impl.config.samples) {
        j.begin_array();
        j.hex(sample.first);
        j.u(sample.second);
        j.u(sample.log_capacity);
        j.end_array();
    }
    j.end_array();
    j.end_object();

    j.key("env").begin_object();
    j.key("ax_mixer_enabled").b(impl.system.ax_mixer_report().enabled);
    j.key("vi_frames_final").u(impl.system.video().frames());
    j.key("guest_frames").u(impl.guest_frames);
    j.key("host_service_ticks").u(impl.host_service_ticks);
    j.key("vi_ticks_sampled").u(impl.vi_ticks);
    j.key("vi_gaps").u(impl.vi_gaps);
    j.key("host_vi_regressions").u(impl.host_vi_regressions);
    j.key("bad_peeks").u(impl.bad_peeks);
    j.key("installed").b(impl.installed_once);
    j.end_object();

    j.key("probes").begin_array();
    for (const Impl::Probe& probe : impl.probes) impl.write_probe(j, probe);
    j.end_array();
    j.key("ordered_numeric_events").begin_object();
    const auto& ordered = impl.ordered_numeric;
    j.key("schema").u(1);
    j.key("entry_observations_only").b(true);
    j.key("scope").str("Raw entry evidence; source tags not dereferenced; no producer output, interior hooks, semantic provenance or exception-to-producer proof");
    j.key("capacity").u(ordered.capacity());
    j.key("retained_bytes").u(ordered.retained_bytes());
    j.key("hits").u(ordered.hits()); j.key("stored").u(ordered.used());
    j.key("dropped").u(ordered.dropped());
    j.key("counter_overflow").b(ordered.counter_overflow());
    j.key("complete_for_observed_entries").b(ordered.complete());
    j.key("root_registers").begin_array();
    for (const auto reg : measurement::OrderedNumericEvent::root_registers) j.u(reg);
    j.end_array();
    j.key("lane_order").begin_array();
    for (const char* lane : {"f18.0", "f18.1", "f19.0", "f19.1"}) j.str(lane);
    j.end_array();
    j.key("events").begin_array();
    for (std::size_t i = 0; i < ordered.used(); ++i) {
        const auto& event = ordered[i];
        j.begin_object(); j.key("sequence").u(event.sequence);
        j.key("instructions").u(event.instructions); j.key("site").hex(event.site);
        j.key("lr").hex(event.lr); j.key("vi").u(event.vi); j.key("guest_frame").u(event.guest_frame);
        j.key("msr").hex(event.msr); j.key("fpscr").hex(event.fpscr);
        j.key("f18_f19_raw_u64").begin_array();
        for (const auto bits : event.f18_f19) {
            char raw[19];
            std::snprintf(raw, sizeof(raw), "0x%016llX", static_cast<unsigned long long>(bits));
            j.str(raw); // exact bits even in JSON readers with binary64 numbers
        }
        j.end_array(); j.key("roots").begin_array();
        for (const auto value : event.roots) j.hex(value);
        j.end_array(); j.key("gqr").begin_array();
        for (const auto value : event.formats) j.hex(value);
        j.end_array(); j.key("context_fields_observed").u(event.context_fields_observed);
        j.key("current_context").hex(event.current_context); j.key("fp_owner").hex(event.fp_owner);
        j.key("thread").hex(event.thread); j.end_object();
    }
    j.end_array(); j.end_object();
    impl.write_ax(j);
    impl.write_sections(j);
    j.key("vi_regressions").u(impl.ax.vi_regressions);
    j.end_object();
    return j.text();
}

std::string MeasureProbes::summary() const {
    const Impl& impl = *impl_;
    auto hits_of = [&impl](const char* name) -> u64 {
        for (const Impl::Probe& probe : impl.probes) {
            if (probe.name == name) return probe.hits;
        }
        return 0;
    };
    std::string text;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "r5probes [%s]: mixer %s, vi %llu, guest frames %llu, host-service ticks %llu, bad peeks %llu\n",
                  impl.config.route_label.c_str(), impl.system.ax_mixer_report().enabled ? "ON" : "off",
                  static_cast<unsigned long long>(impl.system.video().frames()),
                  static_cast<unsigned long long>(impl.guest_frames),
                  static_cast<unsigned long long>(impl.host_service_ticks),
                  static_cast<unsigned long long>(impl.bad_peeks));
    text += line;
    std::snprintf(line, sizeof(line),
                  "  AX: frames %llu, syncs %llu (pb table %s), force drops %llu, stolen-voice predictions %llu\n",
                  static_cast<unsigned long long>(hits_of("ax_frame")),
                  static_cast<unsigned long long>(impl.ax.sync_calls), impl.ax.pb_table_valid ? "valid" : "INVALID",
                  static_cast<unsigned long long>(impl.ax.drops),
                  static_cast<unsigned long long>(impl.acquire.predicted_steal));
    text += line;
    std::snprintf(line, sizeof(line),
                  "  voice-frames: running %llu, advanced %llu, frozen %llu, guest rewrite %llu, "
                  "stops dsp/guest/drop %llu/%llu/%llu\n",
                  static_cast<unsigned long long>(impl.ax.running),
                  static_cast<unsigned long long>(impl.ax.advanced_dsp),
                  static_cast<unsigned long long>(impl.ax.frozen),
                  static_cast<unsigned long long>(impl.ax.guest_rewrite),
                  static_cast<unsigned long long>(impl.ax.stops_dsp),
                  static_cast<unsigned long long>(impl.ax.stops_guest),
                  static_cast<unsigned long long>(impl.ax.stops_drop));
    text += line;
    std::snprintf(line, sizeof(line),
                  "  AXRNA: entries %llu, predicted refills %llu (%llu units), traps %llu, writepos mismatch %llu; "
                  "Sofdec clock queries %llu, ncount changes %llu\n",
                  static_cast<unsigned long long>(impl.rna.entries),
                  static_cast<unsigned long long>(impl.rna.predicted_refills),
                  static_cast<unsigned long long>(impl.rna.refill_units),
                  static_cast<unsigned long long>(impl.rna.trap_predicted),
                  static_cast<unsigned long long>(impl.rna.writepos_mismatch),
                  static_cast<unsigned long long>(impl.sofdec.queries),
                  static_cast<unsigned long long>(impl.sofdec.ncount_changes));
    text += line;
    u32 last_status = 0xFFFFFFFFu;
    if (!impl.movie.transitions.empty()) last_status = impl.movie.transitions.back().to;
    std::snprintf(line, sizeof(line), "  movie: GetStat polls %llu, last status %d, ever PLAYEND %s\n",
                  static_cast<unsigned long long>(impl.movie.polls), static_cast<int>(last_status),
                  impl.movie.ever_playend ? "yes" : "no");
    text += line;
    std::snprintf(line, sizeof(line),
                  "  DMorph: callback %llu, reinstance %llu (locked dmorph %llu), reemit dmorph/other/unattributed "
                  "%llu/%llu/%llu\n",
                  static_cast<unsigned long long>(impl.dmorph.cb_entries),
                  static_cast<unsigned long long>(impl.dmorph.reinstance),
                  static_cast<unsigned long long>(impl.dmorph.locked_dmorph),
                  static_cast<unsigned long long>(impl.dmorph.reemit_dmorph),
                  static_cast<unsigned long long>(impl.dmorph.reemit_other),
                  static_cast<unsigned long long>(impl.dmorph.reemit_unattributed));
    text += line;
    std::string not_executed;
    std::string not_installed;
    for (const Impl::Probe& probe : impl.probes) {
        if (!probe.installed) {
            not_installed += " " + probe.name;
        } else if (probe.hits == 0) {
            not_executed += " " + probe.name;
        }
    }
    if (!not_executed.empty()) text += "  NOT EXECUTED:" + not_executed + "\n";
    if (!not_installed.empty()) text += "  NOT INSTALLED:" + not_installed + "\n";
    return text;
}

#if !defined(GEKKO_SHIPPING)
MeasureProbes* MeasureProbes::active() noexcept { return g_active.load(std::memory_order_acquire); }
#endif

void MeasureProbes::set_active(MeasureProbes* probes) noexcept {
    g_active.store(probes, std::memory_order_release);
}

}  // namespace gekko
