#pragma once

// STEP-0 measurement for audit R5 ("headless routes run a DSP that does
// nothing") and the R4 DMorph entry counts
// (findings-2026-09-23/HARDWARE_ASSUMPTION_AUDIT.md, sections 1 and 7).
//
// DEFAULT OFF, OBSERVE ONLY. Nothing is constructed unless the integrator's
// --measure list names "r5probes". When enabled it:
//  - counts guest FUNCTION ENTRIES (by LR, per VI frame, per guest frame, with
//    the first N hits' r3..r7) through System::add_hle_observer, which chains:
//    the observer runs, then any production handler, whose return value is
//    kept, so every served/declined decision is exactly production's;
//  - samples the AX parameter-block table at every __AXSyncPBs entry and a few
//    configured MEM1 words once per VI frame.
// It never writes a Cpu field, guest memory or a device, never retires
// instructions, never leaves to the dispatcher and never calls guest code.
// Every guest read goes through GuestMemory::peek{8,16,32} at a window
// address (host-side: no guest access, nothing recorded).
//
// Deliberately NOT included by cpu.h, recomp.h or anything a generated shard
// includes (the shard include closure is recomp_decls.h -> recomp.h -> cpu.h,
// ppc_ops.h, memory.h, types.h): adding it costs a relink, not a shard rebuild.
//
// Install it AFTER System::start(): Cpu::install_hle replaces by address, so a
// production hook installed later would silently remove a probe (audit R8).
// json() reports, per probe, whether its observer is still in the chain.

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class System;
class Cpu;

class MeasureProbes {
public:
    // Bounds on every user-configurable list; parse_spec rejects larger values
    // and the constructor clamps a programmatic Config to the same limits, so
    // every table is fixed-capacity once install() has run.
    static constexpr std::size_t kMaxExtraEntries = 32;
    static constexpr std::size_t kMaxSamples = 64;
    static constexpr std::size_t kMaxPbDumpFrames = 16;
    static constexpr u32 kMaxFirstHits = 256;
    static constexpr std::size_t kMaxLabel = 128;
    static constexpr u32 kMaxOrderedEvents = 65536;
    static constexpr u32 kDefaultSampleLogEntries = 256;
    static constexpr u32 kMaxSampleLogEntries = 65536;

    struct Sample {
        u32 first = 0;  // effective observation address
        u32 second = 4; // byte width
        u32 log_capacity = kDefaultSampleLogEntries;
    };

    struct Config {
        std::vector<std::string> sets{"r5"};              // "r5", "r4", "all", "none"
        std::vector<u32> extra_entries;                   // additional function entries
        std::vector<u32> numeric_entries;                 // raw numeric entry evidence, never execution state
        std::vector<u32> pb_dump_frames{200, 220, 240};   // VI frames for PB-table dumps
        std::vector<Sample> samples;                     // {ea, size 1|2|4, bounded change-log capacity}
        bool default_samples = true;                      // add the r5 set's words when "r5" is on
        u32 first_hits = 50;
        u32 ordered_events = 0;                         // opt-in ordered numeric entry observations
        std::string route_label;
    };

    // "sets=r5,r4;pb=200,220,240;sample=0x805F22A0:4,0x805F22A4:4;extra=0x80385950;
    //  numeric=0x8044C260;first=50;label=hl-mixoff". Numeric entries are also
    // installed entries; independent 32-key format and r4 argument histograms,
    // all hits counted, explicit overflow. r4 is raw evidence, not an inferred
    // flag/format contract at arbitrary entries. ordered=0..65536 records one
    // bounded sequence across numeric entries; default 0 does no extra reads.
    // Each sample may specify ea:width:change-log-capacity, measured per owner;
    // omitted capacity keeps 256 entries. Explicit capacity is 1..65536. Logs
    // reserve once; they never grow or wrap over retained observations. The
    // worst explicit request is 64 * 65536 * 8 = 32 MiB of diagnostic history.
    // Keys are optional; "sample=none" / "pb=none"
    // turn those off. Returns false with a message on any malformed or
    // out-of-bounds value.
    static bool parse_spec(const std::string& spec, Config& out, std::string& error);

    MeasureProbes(System& system, Config config);
    ~MeasureProbes();
    MeasureProbes(const MeasureProbes&) = delete;
    MeasureProbes& operator=(const MeasureProbes&) = delete;

    // Once, immediately after System::start(). Probes whose address is not a
    // translated function entry (range begin) are recorded, never installed.
    void install();
    // From any host-service callback on the guest thread; samples once per new
    // VI frame and returns at once otherwise.
    void on_host_service();

    // ONE JSON object (the value of the "r5probes" key). Call once the guest
    // thread has stopped.
    std::string json() const;
    // A few human-readable lines, newline terminated.
    std::string summary() const;

    // The instance the host-service taps feed; null when the item is off.
#if defined(GEKKO_SHIPPING)
    // GEKKO_SHIPPING (ES): no probes in the user's exe; every `if (MeasureProbes* p = active())` folds away.
    static MeasureProbes* active() noexcept { return nullptr; }
#else
    static MeasureProbes* active() noexcept;
#endif
    static void set_active(MeasureProbes* probes) noexcept;

    struct Impl;

private:
    // Shared with the installed observers, so a handler left in the Cpu's HLE
    // table can never reach freed state even if this object dies first.
    std::shared_ptr<Impl> impl_;
};

}  // namespace gekko
