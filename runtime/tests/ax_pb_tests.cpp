#include "gekko/ax_pb_image.h"
#include "gekko/system.h"
#include "gekko/savestate.h"
#include "gekko/ax_adpcm_decode.h"
#include "gekko/ax_voice_select.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "test_scratch.h"

using namespace gekko;
namespace {
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
// The voices' samples: a native sound store over the sound addresses these tests use, 0..16 MiB - the range a state
// saved before ARAM was retired adopts (SoundArena::adopt, docs/ARAM.md "Old savestates") - zero-filled.
constexpr u32 kSampleBytes = 0x01000000u;
struct Samples {
    u8* bytes;
    u8& operator[](std::size_t index) const { return bytes[index]; }
    u8* begin() const { return bytes; }
    u8* end() const { return bytes + kSampleBytes; }
    u8* data() const { return bytes; }
    std::size_t size() const { return kSampleBytes; }
};
Samples samples_of(System& s) {
    SoundArena& store = s.sound_banks().arena();
    if (store.reserved() == 0u) {
        static const std::vector<u8> zeros(kSampleBytes);
        std::string why;
        require(store.adopt(0u, kSampleBytes, zeros.data(), why), "the tests' sample store could not be set up");
    }
    return {const_cast<u8*>(store.source().bytes)};
}
// AX reaches the mixer as the game's frame does: the frame's list (a command-list slot) is handed to the mixer. The tests write the
// list in the old command grammar into the first slot and the importer of old savestates (parse_ax_list) turns it into the native
// list; a list it refuses leaves the slot unbuilt and the mixer stops the run.
void post_list(System& s, u32 address) {
    s.import_ax_lists_from_guest();
    s.mix_ax_list(address);
}
// The AX task of the former native DSP no longer exists: nothing to set up (the call sites mark where the old protocol started).
void run_ax_task(System&) {}
// The hand-built list sits in the SDK's first command-list slot; the importer of old savestates (parse_ax_list) turns it into the native
// list the mixer takes (the grammar is checked there), exactly as a version-31 state's list is taken over.
constexpr u32 kList = AxLists::kBase, kOutput = 0x80060000u;
// Version 23 appends the room effect after the v22 mixer history: binding (context, configured), the six
// parameters, the effect's retained values, the pending blocks, the matrix surround sends, the slot and the
// awaiting flag. Offsets of the v22 fields are taken from the end of the record minus this.
constexpr std::size_t kEffectHistoryBytes =
    sizeof(u32) + 1u + 6u * sizeof(float) + sizeof(decltype(shadow::audio::Reverb::History::samples)) +
    sizeof(decltype(shadow::audio::Reverb::History::positions)) +
    sizeof(decltype(shadow::audio::Reverb::History::delayed)) +
    sizeof(decltype(shadow::audio::Reverb::History::low_pass)) +
    sizeof(decltype(shadow::audio::Reverb::History::previous_input)) +
    sizeof(decltype(shadow::audio::MixerHistory::pending)) +
    sizeof(decltype(shadow::audio::MixerHistory::spatial_right)) + 1u + 1u +
    // version 31 appends the native PB image and update rings after the effect history
    AxPbImage::kCount * (AxPbImage::kStride + AxPbImage::kRingStride) +
    // version 32 appends the two next-frame lists (13 bytes each)
    AxLists::kCount * 13u;
void submit(System& s, u32 head) {
    auto& c = s.cpu();
    c.mem_write16(kList, 2u); c.mem_write32(kList + 2u, head);
    c.mem_write16(kList + 6u, 3u); c.mem_write16(kList + 8u, 0xEu);
    c.mem_write32(kList + 10u, 0u); c.mem_write32(kList + 14u, kOutput);
    c.mem_write16(kList + 18u, 0xFu);
    const u64 lists = s.audio().stats().command_lists;
    post_list(s, kList);
    require(s.audio().stats().command_lists == lists + 1u && s.audio().last_command_list() == kList,
            "the AX list did not reach the mixer");
}
// The native AX sync owner's frame entry; the SDK's words hang off the small-data base r13 (zero in these fixtures).
inline bool sync_entry(Cpu& c) {
    c.gpr[13] = 0x805EC500u;
    return c.try_hle(0x80386BC0u);
}
void prime(System& s, u32 pb, u32 next, bool running) {
    samples_of(s);  // the voice's addresses name the sample store (zeros until a test writes samples)
    auto& c = s.cpu();
    c.mem_write32(pb, next);
    c.mem_write16(pb + 14u, running ? 1u : 0u);
    c.mem_write16(pb + 18u, 0x4000u); c.mem_write16(pb + 22u, 0x4000u);
    // Mixer control as AXSetVoiceMix (0x803871B0) derives it from the non-zero main L/R volumes: 0x1 | 0x2. The
    // mixer routes by these bits (05_DSP_MICROCODE_SEMANTICS.md 7.6), so a PB without them mixes nothing.
    c.mem_write16(pb + 12u, 0x0003u);
    c.mem_write16(pb + 100u, 0x7FFFu);
    c.mem_write16(pb + 110u, 1u); c.mem_write16(pb + 112u, 10u);
    c.mem_write32(pb + 114u, 0x100u); c.mem_write32(pb + 118u, 0x1FFFu);
    c.mem_write32(pb + 122u, 0x100u); c.mem_write32(pb + 166u, 0x10000u);
}
std::vector<u8> run(u32 first, u32 second) {
    auto s = std::make_unique<System>();
    s->cpu().reset(); s->start();
    run_ax_task(*s); // the AX task runs on the native DSP
    prime(*s, first, second, false); prime(*s, second, 0u, false);
    submit(*s, first); // publish the list before the next guest frame sync
    prime(*s, first, second, true); prime(*s, second, 0u, true);
    auto aram = samples_of(*s);
    for (u32 sample = 0x100u; sample < 0x2000u; ++sample) {
        aram[sample * 2u] = 0x12u; aram[sample * 2u + 1u] = static_cast<u8>(sample);
    }
    // The native sync owner (gekko/native_ax_sync.h) serves the frame entry; with no voices on the stacks it must leave the PBs alone.
    require(sync_entry(s->cpu()), "the native AX sync owner is installed at the SDK entry");
    require(s->cpu().mem_read32(first + 122u) == 0x100u &&
                s->cpu().mem_read32(second + 122u) == 0x100u,
            "sync entry acquired an unaccepted audio span");
    submit(*s, first);
    require(s->cpu().mem_read32(first + 122u) == 0x1A0u &&
                s->cpu().mem_read32(second + 122u) == 0x1A0u,
            "AX did not advance the published non-contiguous PB chain");
    const u8* data = s->guest_memory().host_pointer(kOutput, 640u);
    std::vector<u8> pcm(data, data + 640u);
    bool nonzero = false; for (u8 v : pcm) nonzero |= v != 0u;
    require(nonzero, "AX mixed silence from active PCM16 voices");
    require(!s->cpu().stopped(), "valid AX list stopped the CPU");
    return pcm;
}

void failures() {
    for (unsigned attack = 0; attack < 9; ++attack) {
        auto s = std::make_unique<System>();
        auto& c = s->cpu();
        c.reset(); s->start();
        run_ax_task(*s);
        constexpr u32 pb = 0x80F10000u;
        prime(*s, pb, 0u, false); submit(*s, pb);
        prime(*s, pb, 0u, true);
        if (attack < 4) {
            const u32 bad[] = {pb, pb + 2u, 0x817FFF80u, 0x80F10001u};
            c.mem_write32(pb, bad[attack]);
            submit(*s, pb); // synchronizing producer changed the accepted link
            require(c.mem_read32(pb + 122u) == 0x100u, "invalid chain partially advanced a voice");
        } else {
            c.mem_write16(kOutput, 0xCAFEu);
            if (attack == 4) c.mem_write32(kList + 14u, 0x817FFF00u); // output overrun
            if (attack == 5) c.mem_write16(kList + 18u, 0xDEADu); // no terminator / unknown
            if (attack == 6) c.mem_write16(kList, 7u); // no published PB head
            if (attack == 7) {
                for (u32 i = 0; i < 192; ++i) c.mem_write16(kList + i * 2u, 3u);
                c.mem_write16(kList + 382u, 2u); // truncated address at DMA end
            }
            post_list(*s, attack == 8 ? 0x817FFFF0u : kList); // list DMA overrun
            require(c.mem_read16(kOutput) == 0xCAFEu, "invalid list partially wrote PCM");
        }
        // The mixer's own diagnosis, not a DSP protocol stop (also an ExecutionFault).
        require(c.stopped() && c.stop().reason == StopReason::ExecutionFault &&
                    c.stop().detail.find("AX invalid") != std::string::npos,
                "malformed AX input was not diagnosed");
    }
    std::cout << "PASS 9 AX failure contracts (cycles, overlap, alignment, bounds, missing/truncated operands)\n";
}

void publication() {
    auto s = std::make_unique<System>();
    auto& c = s->cpu();
    c.reset(); s->start();
    constexpr u32 old = 0x8058FCA0u, pb = 0x80F10000u, other = 0x80F20000u;
    prime(*s, old, 0u, true);
    sync_entry(c);
    require(c.mem_read32(old + 122u) == 0x100u, "AX touched an unpublished compiled-in PB");
    run_ax_task(*s);
    prime(*s, pb, other, false); prime(*s, other, 0u, false);
    submit(*s, pb);
    prime(*s, pb, other, true); prime(*s, other, 0u, true);
    c.mem_write32(pb + 166u, 1u); c.mem_write32(other + 166u, 1u); // fractional cursor
    sync_entry(c);
    c.mem_write32(pb, 0u); c.mem_write32(other, pb);
    submit(*s, other); // same PB identities, different list order
    for (u32 i = 1; i < 410; ++i) submit(*s, other);
    require(c.mem_read32(pb + 122u) == 0x101u && c.mem_read32(other + 122u) == 0x101u,
            "reordering PBs lost per-address fractional history");
    submit(*s, 0u);
    sync_entry(c);
    require(c.mem_read32(pb + 122u) == 0x101u, "null PB head retained old voices");
    submit(*s, pb);
    s->reset_host_audio_state(); // explicit cold restart, also used for old v19 files
    c.mem_write32(pb + 166u, 0x10000u);
    sync_entry(c);
    require(c.mem_read32(pb + 122u) == 0x101u, "audio reset acquired an old unaccepted job");
    submit(*s, pb);
    require(c.mem_read32(pb + 122u) == 0x1A1u, "audio reset failed to consume the next accepted descriptor");
    require(c.mem_read32(old + 122u) == 0x100u, "relocated path touched the old data address");
    require(!c.stopped(), "valid publication/reordering/reset stopped CPU");
    std::cout << "PASS AX publication, null head, reordered voice history and host-audio reset\n";
}

void dma_mapping() {
    auto s = std::make_unique<System>();
    auto& c = s->cpu();
    c.reset(); s->start();
    run_ax_task(*s);
    constexpr u32 pb = 0x80F10000u;
    prime(*s, pb, 0u, false); submit(*s, pb);
    prime(*s, pb, 0u, true);
    sync_entry(c);  // the owner reads the SDK's words through the CPU's data translation: run it before the DR switch below
    // DSP DMA must not consult the CPU's MSR[DR]/DBAT state.
    c.set_msr(c.msr() & (~kMsrDR)); c.select_memory_window();
    require(s->guest_memory().read32(pb + 122u) == 0x100u,
            "sync entry advanced an unaccepted descriptor");
    post_list(*s, kList);
    require(s->guest_memory().read32(pb + 122u) == 0x1A0u,
            "accepted PB DMA incorrectly depends on CPU data translation");
    require(!c.stopped(), "DSP list DMA incorrectly depends on CPU data translation");
    std::cout << "PASS DSP PB/list DMA with CPU data translation disabled\n";
}

// A PCM16 voice at the top of a sound store: the host byte offset is position * 2 in 64 bits. ARAM's 16 MiB wrap
// is gone with ARAM (docs/ARAM.md step 5): a voice reaching past its store is a defect that stops the CPU.
void pcm16_store_top() {
    const auto mix = [](u32 end_sample, bool expect_stop) {
        auto s = std::make_unique<System>();
        auto& c = s->cpu();
        c.reset();
        s->start();
        run_ax_task(*s);
        constexpr u32 pb = 0x80F10000u;
        prime(*s, pb, 0u, false);
        submit(*s, pb);
        prime(*s, pb, 0u, true);
        // The store's last 16 samples. Envelope/volume at 0x7FFF scale 0x1234 -> 0x1232.
        constexpr u32 kIndex = 0x7FFFF0u;
        auto samples = samples_of(*s);
        samples[static_cast<std::size_t>(kIndex) * 2u] = 0x12u;
        samples[static_cast<std::size_t>(kIndex) * 2u + 1u] = 0x34u;
        c.mem_write16(pb + 18u, 0x7FFFu);
        c.mem_write16(pb + 22u, 0x7FFFu);
        c.mem_write32(pb + 122u, kIndex);
        c.mem_write32(pb + 118u, end_sample);
        c.mem_write32(pb + 114u, kIndex);
        sync_entry(c);
        submit(*s, pb);
        if (expect_stop) {
            require(c.stopped(), "a PCM16 voice reaching past its sound store did not stop the CPU");
            return;
        }
        // srcType (pb+8) is left 0 here (polyphase): that mode keeps the former two-tap arithmetic (the DSP ROM's
        // polyphase coefficients are not in the game binary), so output frame 0 is the first sample.
        const u8* data = s->guest_memory().host_pointer(kOutput, 4u);
        require(data != nullptr && data[0] == 0x12u && data[1] == 0x32u,
                "PCM16 mix missed the store-top host byte offset");
        require(!c.stopped(), "a PCM16 voice at the store's top stopped the CPU");
    };
    mix(0x7FFFFFu, false);  // the end sample is the store's last
    mix(0x800FF0u, true);   // past the store's end (ARAM used to wrap here)
    std::cout << "PASS PCM16 at the store's top (64-bit byte offset); past it the CPU stops\n";
}

void linear_src() {
    auto s = std::make_unique<System>();
    auto& c = s->cpu();
    c.reset();
    s->start();
    run_ax_task(*s);
    constexpr u32 pb = 0x80F10000u;
    prime(*s, pb, 0u, false);
    submit(*s, pb);
    prime(*s, pb, 0u, true);
    c.mem_write16(pb + 8u, 1u);  // srcType linear
    c.mem_write16(pb + 18u, 0x7FFFu);
    c.mem_write16(pb + 22u, 0x7FFFu);
    c.mem_write32(pb + 166u, 0x18000u);  // ratio 1.5
    auto aram = samples_of(*s);
    for (u8& b : aram) b = 0;
    for (u32 sample = 0x100u; sample < 0x200u; ++sample) {
        const s16 value = static_cast<s16>((sample - 0x100u) * 100);
        aram[sample * 2u] = static_cast<u8>(static_cast<u16>(value) >> 8);
        aram[sample * 2u + 1u] = static_cast<u8>(value);
    }
    sync_entry(c);
    submit(*s, pb);
    const u8* data = s->guest_memory().host_pointer(kOutput, 8u);
    require(data != nullptr, "linear SRC produced no output");
    const s16 y0 = static_cast<s16>((data[0] << 8) | data[1]);
    const s16 y1 = static_cast<s16>((data[4] << 8) | data[5]);
    // Envelope/volume at 0x7FFF scale ~identity. The microcode's linear SRC (05 section 7.2) advances the phase
    // before the output and reads the two newest taps of the 4-sample window (zero history, PB words 86..89):
    // output 0 at phase 1.5 = between history[3] (0) and sample 0 (0) = 0; output 1 at phase 3.0 = sample 1 (100),
    // then 98 after the two Q15 gains. The former stand-in sampled (2 - ratio) = 0.5 samples later (lerp at 1.5 = 150).
    require(y0 >= -2 && y0 <= 2, "linear SRC output 0 is not the first sample");
    require(y1 >= 95 && y1 <= 100, "linear SRC did not take the microcode's taps at ratio 1.5");
    require(!c.stopped(), "linear SRC mix stopped the CPU");
    std::cout << "PASS AX linear SRC at ratio 1.5 (y0=" << y0 << " y1=" << y1 << ")\n";
}

void capacity() {
    for (u32 count : {64u, 65u}) {
        auto s = std::make_unique<System>();
        auto& c = s->cpu();
        c.reset(); s->start();
        run_ax_task(*s);
        constexpr u32 base = 0x80F10000u;
        for (u32 i = 0; i < count; ++i)
            prime(*s, base + i * 244u, i + 1u == count ? 0u : base + (i + 1u) * 244u, false);
        submit(*s, base);
        require(c.stopped() == (count > 64u) &&
                    (count == 64u || c.stop().detail.find("AX invalid PB chain") != std::string::npos),
                "AX list capacity boundary is not exact");
        if (count == 64u) {
            for (u32 i = 0; i < count; ++i) c.mem_write16(base + i * 244u + 14u, 1u);
            sync_entry(c);
            submit(*s, base);
            for (u32 i = 0; i < count; ++i)
                require(c.mem_read32(base + i * 244u + 122u) == 0x1A0u, "64-voice list dropped a PB");
        }
    }
    std::cout << "PASS 64-voice capacity and loud rejection of a 65th PB\n";
}

// Independent sample-by-sample address oracle: headers are not samples, and
// every wrap starts at the loop's address. The production bug being diagnosed
// instead applies a single linear nibble delta modulo the nibble loop length.
// This opt-in diagnostic remains outside the normal gate until the position
// contract has been checked on real ADPCM voices.
void adpcm_loop_oracle() {
    struct Case { u32 current, loop, end, ratio; };
    const Case cases[] = {
        {2u, 2u, 15u, 0x10000u},
        {2u, 2u, 47u, 0x10000u},
        {18u, 18u, 47u, 0x18000u},
        {10u, 4u, 11u, 0x10000u},
        {26u, 18u, 47u, 0x40000u},
    };
    unsigned failures = 0;
    for (const auto& test : cases) {
        auto s = std::make_unique<System>();
        auto& c = s->cpu();
        c.reset(); s->start();
        run_ax_task(*s);
        constexpr u32 pb = 0x80F10000u;
        prime(*s, pb, 0u, false); submit(*s, pb);
        prime(*s, pb, 0u, true);
        c.mem_write16(pb + 112u, 0u);
        c.mem_write32(pb + 114u, test.loop);
        c.mem_write32(pb + 118u, test.end);
        c.mem_write32(pb + 122u, test.current);
        c.mem_write32(pb + 166u, test.ratio);
        u32 expected = test.current;
        for (u64 sample = 0; sample < (160ull * test.ratio >> 16u); ++sample) {
            if ((expected & 15u) < 2u) expected += 2u - (expected & 15u);
            if (expected > test.end) expected = test.loop;
            ++expected;
            if ((expected & 15u) == 0u) expected += 2u;
            if (expected > test.end) expected = test.loop;
        }
        sync_entry(c);
        const u32 actual = c.mem_read32(pb + 122u);
        if (actual != expected) ++failures;
        std::cout << "ADPCM sample-position oracle current=" << test.current
                  << " loop=" << test.loop << " end=" << test.end
                  << " ratio=" << test.ratio << " actual=" << actual
                  << " expected=" << expected << '\n';
        require(!c.stopped(), "valid ADPCM loop stopped the CPU");
    }
    require(failures == 0u, "ADPCM PB position disagrees with sample-read loop oracle");
}

// Muting the main gains changes the audible sum, not source playback or its
// dependent predictor state. Compare the resumed frame against the same source
// rendered continuously, with a synthetic nonzero ADPCM predictor coefficient.
void muted_history_oracle() {
    enum class Mute { None, MainGains, Envelope };
    const auto run_arm = [](Mute mute, u32 ratio) {
        auto s = std::make_unique<System>();
        auto& c = s->cpu();
        c.reset(); s->start();
        run_ax_task(*s);
        constexpr u32 pb = 0x80F10000u;
        prime(*s, pb, 0u, false); submit(*s, pb);
        prime(*s, pb, 0u, true);
        c.mem_write16(pb + 110u, 0u);
        c.mem_write16(pb + 112u, 0u);
        c.mem_write32(pb + 122u, 2u);
        c.mem_write32(pb + 118u, 0x1FFFu);
        c.mem_write32(pb + 166u, ratio);
        c.mem_write16(pb + 126u, 2047u);
        c.mem_write16(pb + 128u, 0u);
        auto aram = samples_of(*s);
        std::fill(aram.begin(), aram.begin() + 0x1000u, u8{0x11u});
        for (u32 offset = 0; offset < 0x1000u; offset += 8u) aram[offset] = 0u;
        for (u32 frame = 0; frame < 7u; ++frame) {
            const bool muted_frame = frame >= 1u && frame <= 5u;
            const u16 gain = mute == Mute::MainGains && muted_frame ? 0u : 0x7FFFu;
            c.mem_write16(pb + 18u, gain); c.mem_write16(pb + 22u, gain);
            c.mem_write16(pb + 100u, mute == Mute::Envelope && muted_frame ? 0u : 0x7FFFu);
            sync_entry(c);
            submit(*s, pb);
        }
        require(!c.stopped(), "valid muted ADPCM source stopped the CPU");
        const u8* data = s->guest_memory().host_pointer(kOutput, 640u);
        return std::make_pair(std::vector<u8>(data, data + 640u), c.mem_read32(pb + 122u));
    };
    for (const u32 ratio : {0x10000u, 0x18000u, 0x40000u}) {
        const auto control = run_arm(Mute::None, ratio);
        for (const auto mute : {Mute::MainGains, Mute::Envelope}) {
            const auto muted = run_arm(mute, ratio);
            const s16 control_first = static_cast<s16>(load_be16(control.first.data()));
            const s16 muted_first = static_cast<s16>(load_be16(muted.first.data()));
            std::cout << "ADPCM mute history oracle ratio=" << ratio
                      << " mute=" << static_cast<int>(mute)
                      << " resumed first sample control=" << control_first
                      << " muted=" << muted_first << '\n';
            require(control.second == muted.second, "muting changed the guest-visible source position");
            require(control.first == muted.first, "muting discarded ADPCM predictor history");
        }
    }
}
// Original AX synchronizes the voice descriptors before publishing a command
// list. Only the accepted work owns source advancement and sample generation.
// Mutations here deliberately stand for that synchronizing producer boundary.
void accepted_list_owner() {
    constexpr u32 pb = 0x80F10000u;
    bool all_equal = true;
    for (unsigned attack = 0; attack < 4; ++attack) {
        auto s = std::make_unique<System>();
        auto& c = s->cpu();
        c.reset(); s->start();
        run_ax_task(*s);
        prime(*s, pb, 0u, false); submit(*s, pb);
        auto aram = samples_of(*s);
        for (u32 i = 0x100u; i < 0x2000u; ++i) {
            const u16 sample = i < 0x400u ? 1000u : 2000u;
            aram[i * 2u] = static_cast<u8>(sample >> 8u);
            aram[i * 2u + 1u] = static_cast<u8>(sample);
        }
        prime(*s, pb, 0u, attack != 2u);
        c.mem_write16(pb + 8u, 1u);
        // Visiting the SDK sync entry is not accepting an audio job. It must
        // neither move the readback position nor acquire a stale source span.
        sync_entry(c);
        if (attack == 3u) sync_entry(c);
        const u32 before_acceptance = c.mem_read32(pb + 122u);
        if (attack == 0u) c.mem_write16(pb + 14u, 0u); // stop during sync
        if (attack == 1u) {
            c.mem_write32(pb + 114u, 0x400u);
            c.mem_write32(pb + 122u, 0x400u); // retarget during sync
        }
        if (attack == 2u) c.mem_write16(pb + 14u, 1u); // start during sync
        submit(*s, pb);
        const u32 expected_position = attack == 0u ? 0x100u :
                                      attack == 1u ? 0x4A0u : 0x1A0u;
        const s32 source = attack == 1u ? 2000 : 1000;
        const s16 expected_sample = attack == 0u ? 0 : static_cast<s16>(
            (((static_cast<s64>(source) * 0x7FFF) >> 15) * 0x4000) >> 15);
        const u8* output = s->guest_memory().host_pointer(kOutput, 640u);
        bool bytes_equal = true;
        // The microcode's linear SRC (05_DSP_MICROCODE_SEMANTICS.md 7.2) advances the phase before each output and
        // reads the two newest taps of its 4-sample window, which a (re)started voice takes from PB words 86..89
        // (zero here, as AXSetVoiceSrc leaves them): at ratio 1.0 output 0 is the last history sample (0) and the
        // source's first sample appears at output 1 - one sample later than the former stand-in's interpolation.
        for (u32 i = 0; i < 320u; ++i) {
            const s16 want = i < 2u ? 0 : expected_sample;  // frame 0 (L and R) comes from the zero history
            bytes_equal &= static_cast<s16>(load_be16(output + i * 2u)) == want;
        }
        std::cout << "accepted-list owner case=" << attack
                  << " before=" << before_acceptance
                  << " after=" << c.mem_read32(pb + 122u)
                  << " first=" << static_cast<s16>(load_be16(output))
                  << " expected=" << expected_sample << '\n';
        require(!c.stopped(), "valid accepted voice update stopped execution");
        all_equal &= before_acceptance == 0x100u &&
                     c.mem_read32(pb + 122u) == expected_position && bytes_equal;
    }
    require(all_equal, "accepted audio work violated source position/stop/start/retarget ownership");
}
// The two list shapes the game's SDK builds (0x803850AC), word for word, are accepted - mode 0 with the surround fold,
// aux A and the compressor, and mode 2 (the game's surround option: 0x13 + 0x10), which the former boundary rejected
// and halted on (AU_REPORT.md 3.4). Mode 2 mixes the Dolby matrix: aux B L/R return one frame later, main L += it,
// main R -= it (05_DSP_MICROCODE_SEMANTICS.md 5.1). Words the SDK never builds, or out of its order, are refused.
void submit_words(System& s, const std::vector<u32>& words) {
    auto& c = s.cpu();
    for (std::size_t i = 0; i < words.size(); ++i) {
        c.mem_write16(kList + static_cast<u32>(i) * 2u, static_cast<u16>(words[i]));
    }
    post_list(s, kList);
}
std::vector<u32> list_words(u32 head, int mode) {
    const auto addr = [](std::vector<u32>& w, u32 a) { w.push_back(a >> 16); w.push_back(a & 0xFFFFu); };
    std::vector<u32> w;
    constexpr u32 any = 0x80070000u;  // operand addresses the boundary does not dereference
    w.push_back(0x00); addr(w, any);
    if (mode == 0) { w.push_back(0x07); addr(w, any); }
    w.push_back(0x02); addr(w, head);
    w.push_back(0x03);
    if (mode == 2) {
        w.push_back(0x13); for (int k = 0; k < 6; ++k) addr(w, any);
        w.push_back(0x10); addr(w, any); addr(w, any);
    } else {
        w.push_back(0x04); addr(w, any); addr(w, any);
    }
    w.push_back(0x12); w.push_back(0x8000u); w.push_back(0x000Au); addr(w, 0x80563420u);
    w.push_back(0x0E); addr(w, any); addr(w, kOutput);
    w.push_back(0x0F);
    return w;
}
s16 out_sample(System& s, u32 index) {
    const u8* d = s.guest_memory().host_pointer(kOutput + index * 2u, 2u);
    return static_cast<s16>((d[0] << 8) | d[1]);
}
// The compressor gain table where the game's DOL has it (0x80563420, 21 x 160 big-endian u16): this test has no DOL,
// so it is written the way the loaded image would hold it.
void install_compressor_table(System& s) {
    const auto& table = shadow::audio::compressor_gain_table();
    for (u32 k = 0; k < table.size(); ++k) s.cpu().mem_write16(0x80563420u + k * 2u, table[k]);
}
// Command 0x12 (05_DSP_MICROCODE_SEMANTICS.md section 10): a frame whose main L/R reaches 0x8000 gets attack row 0
// (counter 0 before it), the next loud frame row 10, then each quiet frame walks rows 20 .. 11; L = floor(L*g/32768).
void compressor_contract() {
    auto s = std::make_unique<System>();
    auto& c = s->cpu();
    c.reset(); s->start();
    run_ax_task(*s);
    install_compressor_table(*s);
    constexpr u32 pb = 0x80F10000u;
    prime(*s, pb, 0u, false);
    submit_words(*s, list_words(pb, 0));
    prime(*s, pb, 0u, true);
    c.mem_write16(pb + 8u, 1u);
    c.mem_write16(pb + 12u, 0x0001u);           // main L only
    c.mem_write16(pb + 18u, 0xFFFFu);           // ~2.0: the bus exceeds s16
    c.mem_write16(pb + 22u, 0u);
    for (u32 k = 0; k < 4u; ++k) c.mem_write16(pb + 172u + k * 2u, 20000u);  // history = the DC level
    auto aram = samples_of(*s);
    const auto fill = [&](s16 level) {
        for (u32 sample = 0x100u; sample < 0x2000u; ++sample) {
            aram[sample * 2u] = static_cast<u8>(static_cast<u16>(level) >> 8);
            aram[sample * 2u + 1u] = static_cast<u8>(level);
        }
    };
    fill(20000);
    sync_entry(c);
    const auto& g = shadow::audio::compressor_gain_table();
    // DC 20000 x env 0x7FFF -> 19999; x vol 0xFFFF -> floor(19999 * 65535 / 32768) = 39997 on main L.
    const s64 bus = (static_cast<s64>((20000 * 0x7FFF) >> 15) * 0xFFFF) >> 15;
    submit_words(*s, list_words(pb, 0));   // loud frame 1: row 0
    require(out_sample(*s, 159u * 2u) == static_cast<s16>((bus * g[0 * 160 + 159]) >> 15),
            "compressor: first triggered frame must use attack row 0");
    submit_words(*s, list_words(pb, 0));   // loud frame 2: row 10 (counter was 10)
    require(out_sample(*s, 0) == static_cast<s16>((bus * g[10 * 160 + 0]) >> 15),
            "compressor: a frame triggered during the release must use row 10");
    fill(1000);                            // quiet: bus far below the threshold
    const s64 quiet = (static_cast<s64>((1000 * 0x7FFF) >> 15) * 0xFFFF) >> 15;
    // The first quiet frame still starts with the 4-sample SRC history of the loud level, so it triggers (row 10).
    submit_words(*s, list_words(pb, 0));
    require(out_sample(*s, 159u * 2u) == static_cast<s16>((quiet * g[10 * 160 + 159]) >> 15),
            "compressor: the transition frame (loud history) must trigger again");
    for (u32 frame = 0; frame < 10u; ++frame) {
        submit_words(*s, list_words(pb, 0));
        const u32 row = 20u - frame;
        // sample 159 is past the 4-sample history: the new level only
        require(out_sample(*s, 159u * 2u) == static_cast<s16>((quiet * g[row * 160 + 159]) >> 15),
                "compressor: release rows must walk 20 .. 11");
    }
    submit_words(*s, list_words(pb, 0));
    require(out_sample(*s, 159u * 2u) == static_cast<s16>(quiet), "compressor: after the release no gain applies");
    require(!c.stopped(), "compressor contract stopped the CPU");
    // A guest table that differs from the game's is refused before any output (a list naming a new table address
    // is checked again; 0x80563420 was verified above).
    const s16 sentinel = 0x0BAD;
    c.mem_write16(kOutput, static_cast<u16>(sentinel));
    std::vector<u32> w = list_words(pb, 0);
    for (std::size_t i = 0; i + 4 < w.size(); ++i) {
        if (w[i] == 0x12u && w[i + 1] == 0x8000u) { w[i + 3] = 0x8070u; w[i + 4] = 0x0000u; break; }
    }
    for (u32 k = 0; k < 21u * 160u; ++k) c.mem_write16(0x80700000u + k * 2u, g[k]);
    c.mem_write16(0x80700000u + 2u * 1000u, static_cast<u16>(g[1000] ^ 1u));
    submit_words(*s, w);
    require(c.stopped() && c.stop().detail.find("AX invalid") != std::string::npos &&  /* the importer refused the list, so no list was built */
                c.mem_read16(kOutput) == static_cast<u16>(sentinel),
            "a compressor table unlike the game's must be refused before any output");
    std::cout << "PASS AX compressor: rows 0, 10, 20..11, off; a foreign table refused\n";
}
void command_list_programs() {
    for (int mode : {0, 2}) {
        auto s = std::make_unique<System>();
        auto& c = s->cpu();
        c.reset(); s->start();
        run_ax_task(*s);
        install_compressor_table(*s);
        constexpr u32 pb = 0x80F10000u;
        prime(*s, pb, 0u, false);
        submit_words(*s, list_words(pb, mode));
        require(!c.stopped(), "an SDK-built AX list (mode 0 / mode 2) was refused");
        prime(*s, pb, 0u, true);
        // main L 0x4000 + aux B L/R 0x7FFF: mixer control 0x1 | 0x200 | 0x400 (AXSetVoiceMix's bits)
        c.mem_write16(pb + 12u, 0x0601u);
        c.mem_write16(pb + 22u, 0u);
        c.mem_write16(pb + 34u, 0x7FFFu); c.mem_write16(pb + 38u, 0x7FFFu);
        auto aram = samples_of(*s);
        for (u32 sample = 0x100u; sample < 0x2000u; ++sample) { aram[sample * 2u] = 0x03u; aram[sample * 2u + 1u] = 0xE8u; }
        sync_entry(c);
        submit_words(*s, list_words(pb, mode));
        const s16 l1 = out_sample(*s, 20), r1 = out_sample(*s, 21);
        submit_words(*s, list_words(pb, mode));
        const s16 l2 = out_sample(*s, 20), r2 = out_sample(*s, 21);
        require(!c.stopped(), "an SDK-built AX list stopped the CPU");
        // DC 1000 x env 0x7FFF: main L ~ 500 (0x4000); aux B ~ 999 per channel.
        require(l1 > 400 && l1 < 600 && r1 == 0, "main L/R routing by mixer control");
        if (mode == 0) {
            require(l2 == l1 && r2 == 0, "mode 0 must not return aux B (no aux B effect registered)");
        } else {
            require(l2 > l1 + 900 && r2 < -900, "mode 2: aux B L/R one frame later, main L += it, main R -= it");
        }
        std::cout << "PASS AX SDK list mode " << mode << " (L " << l1 << " -> " << l2 << ", R " << r1 << " -> " << r2
                  << ")\n";
    }
    for (u32 bad : {0x01u, 0x08u, 0x0Du, 0x14u}) {
        auto s = std::make_unique<System>();
        auto& c = s->cpu();
        c.reset(); s->start();
        run_ax_task(*s);
        std::vector<u32> w = list_words(0x80F10000u, 0);
        w.insert(w.begin() + 3, bad);  // after op 00 and its address
        prime(*s, 0x80F10000u, 0u, false);
        c.mem_write16(kOutput, 0xCAFEu);
        submit_words(*s, w);
        require(c.stopped() && c.stop().detail.find("AX invalid") != std::string::npos &&
                    c.mem_read16(kOutput) == 0xCAFEu,
                "a command the SDK never builds was not refused before any output");
    }
    std::cout << "PASS AX list grammar: commands 0x01 0x08 0x0D 0x14 refused\n";
}
}

// A checkpoint must continue the accepted audio stream, including fractional
// source position, ADPCM predictor/ring and the mixer's SRC history. Compare
// uninterrupted output with both a reused and a fresh System, across wrap,
// muted frames, fractional ratios and PB chain reordering.
void checkpoint_audio_history() {
    const auto name = "ax-audio-checkpoint-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".gekkostate";
    const auto path = gekko::test_support::scratch_directory(name);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{path};
    unsigned cases = 0, reset_differences = 0;
    for (const u32 format : {0u, 10u, 25u}) {
        for (const u32 ratio : {0xB066u, 0x18001u, 0x40000u}) {
            auto s = std::make_unique<System>();
            auto& c = s->cpu(); c.reset(); s->start(); run_ax_task(*s);
            // Reuse the separated PB data locations from the relocation fixture below.
            constexpr u32 first = 0x80F10000u, second = 0x80F20000u;
            prime(*s, first, second, false); prime(*s, second, 0u, false); submit(*s, first);
            for (const u32 pb : {first, second}) {
                prime(*s, pb, pb == first ? second : 0u, true);
                c.mem_write16(pb + 8u, 1u); // linear SRC
                c.mem_write16(pb + 12u, 7u); // main L/R plus surround
                c.mem_write16(pb + 46u, 0x4000u); // surround gain
                c.mem_write16(pb + 112u, static_cast<u16>(format));
                c.mem_write32(pb + 114u, 2u);
                c.mem_write32(pb + 118u, 0x1FFu);
                c.mem_write32(pb + 122u, 2u);
                c.mem_write32(pb + 166u, ratio);
                c.mem_write16(pb + 126u, 1536u);
                c.mem_write16(pb + 128u, static_cast<u16>(-256));
                c.mem_write16(pb + 162u, 120u); c.mem_write16(pb + 164u, 80u);
                c.mem_write16(pb + 182u, 0u);
                c.mem_write16(pb + 184u, 70u); c.mem_write16(pb + 186u, 40u);
            }
            auto samples = samples_of(*s);
            for (u32 i = 0; i < 0x1000u; ++i) samples[i] = static_cast<u8>(i * 73u + 11u);
            if (format == 0u) for (u32 i = 0; i < 0x1000u; i += 8u) samples[i] = 3u;
            const auto next = [first, second](System& system, unsigned frame) {
                auto& cpu = system.cpu();
                const u32 head = frame & 1u ? second : first;
                const u32 tail = head == first ? second : first;
                cpu.mem_write32(head, tail); cpu.mem_write32(tail, 0u);
                const u16 gain = frame % 4u == 1u ? 0u : 0x4000u;
                cpu.mem_write16(first + 18u, gain); cpu.mem_write16(first + 22u, gain);
                cpu.mem_write16(kList, 7u); cpu.mem_write32(kList + 2u, 0u);
                cpu.mem_write16(kList + 6u, 2u); cpu.mem_write32(kList + 8u, head);
                cpu.mem_write16(kList + 12u, 3u); cpu.mem_write16(kList + 14u, 0xEu);
                cpu.mem_write32(kList + 16u, 0u); cpu.mem_write32(kList + 20u, kOutput);
                cpu.mem_write16(kList + 24u, 0xFu);
                post_list(system, kList);
                require(!cpu.stopped(), "checkpoint continuation stopped CPU");
                const u8* pcm = system.guest_memory().host_pointer(kOutput, 640u);
                std::vector<u8> result(pcm, pcm + 640u);
                state::put(result, cpu.mem_read32(first + 122u));
                state::put(result, cpu.mem_read32(second + 122u));
                system.save_host_audio_state(result);
                return result;
            };
            for (unsigned frame = 0; frame < 9; ++frame) next(*s, frame);
            std::string error;
            if (!save_state(*s, path, &error)) throw std::runtime_error(error);
            std::vector<std::vector<u8>> expected;
            for (unsigned frame = 9; frame < 25; ++frame) expected.push_back(next(*s, frame));
            for (bool fresh : {false, true}) {
                if (fresh) { s = std::make_unique<System>(); s->cpu().reset(); s->start(); }
                if (!load_state(*s, path, &error)) throw std::runtime_error(error);
                for (unsigned frame = 9; frame < 25; ++frame)
                    require(next(*s, frame) == expected[frame - 9], "checkpoint changed PCM, position or decoder/mixer history");
            }
            if (!load_state(*s, path, &error)) throw std::runtime_error(error);
            s->reset_host_audio_state();
            const auto discarded = next(*s, 9);
            reset_differences += !std::equal(discarded.begin(), discarded.begin() + 640, expected[0].begin());
            ++cases;
        }
    }
    require(reset_differences != 0, "checkpoint test did not expose the former reset-history bug");
    std::cout << "PASS " << cases << " checkpoint cases: reused/fresh owners, 16-frame exact continuations; "
              << reset_differences << " reset controls changed PCM\n";

    // Both nonzero matrix blocks and their phase must survive whole-file
    // restores across output-mode gaps. Incomplete state is a negative control.
    unsigned matrix_cases = 0, missing_bank_blocks = 0, wrong_phase_blocks = 0;
    constexpr int modes[] = {2, 2, 0, 2, 0, 0, 2, 2, 0, 2, 0, 0,
                             2, 0, 2, 2, 2, 0, 0, 2, 0, 2};
    for (unsigned checkpoint : {4u, 5u}) {
        auto owner = std::make_unique<System>();
        auto& cpu = owner->cpu(); cpu.reset(); owner->start(); run_ax_task(*owner);
        install_compressor_table(*owner);
        constexpr u32 voice = 0x80F10000u;
        prime(*owner, voice, 0u, false); submit_words(*owner, list_words(voice, 0));
        prime(*owner, voice, 0u, true);
        cpu.mem_write16(voice + 12u, 0x0607u); // main L/R/S and matrix L/R
        cpu.mem_write16(voice + 46u, 0x2000u);
        auto samples = samples_of(*owner);
        for (u32 i = 0x100u; i < 0x2000u; ++i)
            store_be16(samples.data() + i * 2u, static_cast<u16>((i * 37u) % 6000u + 200u));
        const auto next = [&](System& system, unsigned frame) {
            auto& c = system.cpu();
            c.mem_write16(voice + 34u, static_cast<u16>(0x2000u + frame * 211u));
            c.mem_write16(voice + 38u, static_cast<u16>(0x1800u + frame * 127u));
            submit_words(system, list_words(voice, modes[frame]));
            require(!c.stopped(), "matrix checkpoint continuation stopped CPU");
            const u8* pcm = system.guest_memory().host_pointer(kOutput, 640u);
            std::vector<u8> result(pcm, pcm + 640u);
            system.save_host_audio_state(result);
            return result;
        };
        for (unsigned frame = 0; frame < checkpoint; ++frame) next(*owner, frame);
        std::vector<u8> history; owner->save_host_audio_state(history);
        const auto v22_end = history.end() - static_cast<std::ptrdiff_t>(kEffectHistoryBytes);
        require(*(v22_end - 1) == ((checkpoint + 1u) & 1u), "matrix fixture did not cover both phases");
        const auto first = v22_end - 3713, alternate = v22_end - 1281;
        const auto nonzero = [](u8 value) { return value != 0; };
        require(std::any_of(first, first + 1280, nonzero) &&
                std::any_of(alternate, alternate + 1280, nonzero) &&
                !std::equal(first, first + 1280, alternate), "matrix history fixture is silent or identical");
        std::string error;
        if (!save_state(*owner, path, &error)) throw std::runtime_error(error);
        std::vector<std::vector<u8>> expected;
        for (unsigned frame = checkpoint; frame < checkpoint + 16u; ++frame)
            expected.push_back(next(*owner, frame));
        for (bool fresh : {false, true}) {
            if (fresh) { owner = std::make_unique<System>(); owner->cpu().reset(); owner->start(); }
            if (!load_state(*owner, path, &error)) throw std::runtime_error(error);
            for (unsigned frame = checkpoint; frame < checkpoint + 16u; ++frame)
                require(next(*owner, frame) == expected[frame - checkpoint],
                        "matrix checkpoint changed PCM or retained history across output modes");
        }
        for (bool wrong_phase : {false, true}) {
            if (!load_state(*owner, path, &error)) throw std::runtime_error(error);
            auto incomplete = history;
            if (wrong_phase) incomplete[incomplete.size() - kEffectHistoryBytes - 1u] ^= 1u;
            else incomplete.resize(incomplete.size() - kEffectHistoryBytes - 1281u);
            const u8* cursor = incomplete.data();
            require(owner->load_host_audio_state(cursor, incomplete.data() + incomplete.size(),
                    wrong_phase ? save_state_version() : 21u), "missing-state control failed to load");
            unsigned changed = 0;
            for (unsigned frame = checkpoint; frame < checkpoint + 16u; ++frame) {
                const auto actual = next(*owner, frame);
                changed += !std::equal(actual.begin(), actual.begin() + 640u, expected[frame - checkpoint].begin());
            }
            require(changed != 0, "matrix checkpoint control did not change PCM");
            (wrong_phase ? wrong_phase_blocks : missing_bank_blocks) += changed;
        }
        ++matrix_cases;
    }
    std::cout << "PASS " << matrix_cases << " nonzero matrix checkpoint phases: reused/fresh owners, 16-frame continuations; "
              << missing_bank_blocks << " missing-bank and " << wrong_phase_blocks << " wrong-phase PCM blocks differ\n";

    auto s = std::make_unique<System>(); s->cpu().reset(); s->start();
    std::vector<u8> block; s->save_host_audio_state(block);
    const auto load = [&](const std::vector<u8>& bytes, std::size_t length) {
        const u8* cursor = bytes.data();
        return s->load_host_audio_state(cursor, bytes.data() + length, save_state_version()) && cursor == bytes.data() + length;
    };
    require(load(block, block.size()), "empty audio history did not round-trip");
    const u8* const v22_end = block.data() + block.size() - kEffectHistoryBytes;
    const u8* old_cursor = block.data();
    require(s->load_host_audio_state(old_cursor, v22_end - 1921, 20u) && old_cursor == v22_end - 1921,
            "v20 audio prefix rejected");
    old_cursor = block.data();
    require(s->load_host_audio_state(old_cursor, v22_end - 1281, 21u) && old_cursor == v22_end - 1281,
            "v21 audio prefix rejected");
    old_cursor = block.data();
    require(s->load_host_audio_state(old_cursor, v22_end, 22u) && old_cursor == v22_end, "v22 audio prefix rejected");
    for (const auto length : {std::size_t{0}, std::size_t{1}, std::size_t{1297}, block.size() - 1})
        require(!load(block, length), "truncated audio history accepted");
    auto broken = block; broken[0] ^= 1u;
    require(!load(broken, broken.size()), "incompatible decoder mode accepted");
    broken = block; broken[1 + 256 + 2] = 1u;
    require(!load(broken, broken.size()), "invalid fractional position accepted");
    broken = block; broken[1297] = 2u;
    require(!load(broken, broken.size()), "invalid cursor flag accepted");
    const std::size_t v22_size = block.size() - kEffectHistoryBytes;
    broken = block; broken[v22_size - 3715] = 11u;
    require(!load(broken, broken.size()), "invalid compressor release accepted");
    broken = block; broken[v22_size - 1u] = 2u;
    require(!load(broken, broken.size()), "invalid matrix-return phase accepted");
    broken = block; broken[v22_size + 4u] = 2u;
    require(!load(broken, broken.size()), "invalid room effect binding flag accepted");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    broken = block; std::memcpy(broken.data() + v22_size + 5u, &nan, sizeof(nan));
    require(!load(broken, broken.size()), "invalid room effect parameter accepted");
    constexpr std::size_t kImageBytes = AxPbImage::kCount * (AxPbImage::kStride + AxPbImage::kRingStride) + AxLists::kCount * 13u;  // trails the effect history (version 31)
    broken = block; broken[block.size() - kImageBytes - 2u] = 3u;
    require(!load(broken, broken.size()), "invalid room effect slot accepted");
    broken = block; broken[block.size() - kImageBytes - 1u] = 2u;
    require(!load(broken, broken.size()), "invalid room effect awaiting flag accepted");
    std::cout << "PASS checkpoint rejects truncated or incompatible audio history\n";
}

// The room effect as the game drives it: each list schedules the aux A send/return, and after the block the SDK's
// aux processor calls the registered ReverbHi callback - here called the same way - which the native owner serves.
// The arithmetic is the original-word contract's (axfx_reverb_contract_tests); this covers the System path: the
// owner's binding, an audible return, checkpoint continuation of a nonzero tail, bypass and refusal.
void room_effect() {
    constexpr u32 voice = 0x80F10000u, context = 0x80F30000u, record = 0x80F31000u;
    constexpr float kParameters[6] = {0.5f, 1.0f, 1.0f, 0.8f, 0.01f, 0.3f};
    const auto boot = [&](System& s, u16 mixer_control) {
        auto& c = s.cpu();
        c.reset(); s.start(); run_ax_task(s); install_compressor_table(s);
        for (u32 i = 0; i < 6u; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &kParameters[i], sizeof(bits));
            c.mem_write32(context + reverb::kParametersOffset + i * 4u, bits);
        }
        c.mem_write8(context + reverb::kBypassOffset, 0u);
        c.gpr[3] = context; c.lr = 0x80004000u;
        c.mem_write8(context + reverb::kBypassOffset, 0xABu);
        require(c.try_hle(reverb::kInit) && c.gpr[3] == 1u && c.mem_read8(context + reverb::kBypassOffset) == 0u,
                "ReverbHiInit is served natively: bypass cleared, parameters accepted (r3 = 1)");
        prime(s, voice, 0u, false); submit_words(s, list_words(voice, 0));
        prime(s, voice, 0u, true);
        c.mem_write16(voice + 12u, mixer_control);
        c.mem_write16(voice + 26u, 0x6000u); c.mem_write16(voice + 30u, 0x5000u); c.mem_write16(voice + 50u, 0x4000u);
        auto samples = samples_of(s);
        for (u32 i = 0x100u; i < 0x2000u; ++i)
            store_be16(samples.data() + i * 2u, static_cast<u16>(i < 0x400u ? (i * 977u) % 12000u : 0u));
    };
    const auto callback = [&](System& s, u32 with_context) {
        auto& c = s.cpu();
        c.gpr[3] = record; c.gpr[4] = with_context; c.lr = 0x80004000u; c.fpscr = 0u;
        require(c.try_hle(reverb::kCallback), "the room reverb callback must be served natively");
    };
    // One block: the list (mixed at once), its PCM, then the aux processor's call of the callback.
    const auto block = [&](System& s, bool call) {
        submit_words(s, list_words(voice, 0));
        require(!s.cpu().stopped(), "room effect block stopped the CPU");
        const u8* pcm = s.guest_memory().host_pointer(kOutput, 640u);
        std::vector<u8> result(pcm, pcm + 640u);
        if (call) callback(s, context);
        return result;
    };
    auto wet = std::make_unique<System>(), dry = std::make_unique<System>();
    boot(*wet, 0x00B3u);  // main L/R and aux A L/R/S
    boot(*dry, 0x0003u);  // main L/R only
    unsigned audible = 0;
    for (unsigned frame = 0; frame < 24u; ++frame) {
        const auto a = block(*wet, true), b = block(*dry, true);
        for (u32 i = 0; i < 640u; i += 2u) audible += a[i] != b[i] || a[i + 1u] != b[i + 1u];
    }
    const auto& report = wet->ax_mixer_report();
    require(audible > 1000u && report.effect_processed == 24u && report.effect_scheduled >= 24u,
            "the room effect's return was not heard");

    // Checkpoint: a nonzero tail and pending blocks continue exactly in a reused and in a fresh owner.
    const auto path = (gekko::test_support::scratch_directory("ax_pb_room_effect.gekkostate")).string();
    std::string error;
    if (!save_state(*wet, path, &error)) throw std::runtime_error(error);
    std::vector<u8> saved_history; wet->save_host_audio_state(saved_history);
    std::vector<std::vector<u8>> expected;
    for (unsigned frame = 0; frame < 24u; ++frame) expected.push_back(block(*wet, true));
    for (bool fresh : {false, true}) {
        if (fresh) { wet = std::make_unique<System>(); wet->cpu().reset(); wet->start(); }
        if (!load_state(*wet, path, &error)) throw std::runtime_error(error);
        for (unsigned frame = 0; frame < 24u; ++frame)
            require(block(*wet, true) == expected[frame], "room effect checkpoint changed the continuation");
    }
    // Negative control: the same file without the effect history (version 22) restarts the tail.
    if (!load_state(*wet, path, &error)) throw std::runtime_error(error);
    {
        auto prefix = saved_history;
        prefix.resize(prefix.size() - kEffectHistoryBytes);
        const u8* cursor = prefix.data();
        require(wet->load_host_audio_state(cursor, prefix.data() + prefix.size(), 22u),
                "version 22 audio history refused");
    }
    unsigned restarted = 0;
    for (unsigned frame = 0; frame < 24u; ++frame) restarted += block(*wet, true) != expected[frame];
    require(restarted != 0u, "a missing effect history did not change the continuation");

    // Bypass (context + 0x1C4 set) is the original's "callback does nothing": the same as no call at all.
    if (!load_state(*wet, path, &error)) throw std::runtime_error(error);
    auto other = std::make_unique<System>(); other->cpu().reset(); other->start();
    if (!load_state(*other, path, &error)) throw std::runtime_error(error);
    for (unsigned frame = 0; frame < 12u; ++frame) {
        const bool skip = frame == 3u || frame == 4u;
        wet->cpu().mem_write8(context + reverb::kBypassOffset, skip ? 1u : 0u);
        require(block(*wet, true) == block(*other, !skip), "bypass differs from an absent callback");
    }
    require(wet->ax_mixer_report().effect_bypassed == 2u, "bypassed callbacks were not counted");

    // A callback for another object is refused loudly.
    callback(*wet, context + 0x400u);
    require(wet->cpu().stopped() && wet->cpu().stop().detail.find("room reverb callback") != std::string::npos,
            "a foreign effect context was not refused");
    std::filesystem::remove(path);
    std::cout << "PASS room effect: audible at " << audible << " samples, 24-block reused/fresh checkpoint continuation, "
              << restarted << " blocks differ without effect history, bypass = absent callback, foreign context refused\n";
}

// The former quadratic scans of System::select_ax_parameter_blocks, verbatim over plain arrays: the oracle for
// first_ax_block_overlap and assign_ax_voices (ax_voice_select.h).
u32 reference_first_overlap(const u32* addresses, u32 count) {
    for (u32 n = 0; n < count; ++n) {
        bool overlaps = false;
        for (u32 i = 0; i < n; ++i) overlaps |= addresses[n] < addresses[i] + 244u && addresses[i] < addresses[n] + 244u;
        if (overlaps) return n;
    }
    return count;
}
u64 reference_assign(const u32* addresses, u32 count, std::array<u32, 64>& voice_pb, std::array<u32, 64>& order) {
    u64 reset = 0;
    std::array<bool, 64> retained{};
    for (u32 voice = 0; voice < voice_pb.size(); ++voice) {
        for (u32 i = 0; i < count; ++i) retained[voice] |= voice_pb[voice] == addresses[i];
    }
    for (u32 i = 0; i < count; ++i) {
        u32 voice = 0;
        while (voice < voice_pb.size() && voice_pb[voice] != addresses[i]) ++voice;
        if (voice == voice_pb.size()) {
            voice = 0;
            while (retained[voice]) ++voice;
            reset |= u64(1) << voice;
            voice_pb[voice] = addresses[i];
            retained[voice] = true;
        }
        order[i] = voice;
    }
    for (u32 voice = 0; voice < voice_pb.size(); ++voice) {
        if (!retained[voice]) { reset |= u64(1) << voice; voice_pb[voice] = 0u; }
    }
    return reset;
}

void voice_selection_oracle() {
    u64 state = 0x9E3779B97F4A7C15ull;
    const auto next = [&state] { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
    // Overlap: dense, sparse, repeated (cycles) and 32-bit wrapping addresses.
    u64 overlap_cases = 0, overlaps_found = 0;
    for (u32 round = 0; round < 200000u; ++round) {
        std::array<u32, 64> a{};
        const u32 count = u32(next() % 65u);
        const u32 kind = round % 4u;
        const u32 base = kind == 3u ? 0xFFFFF000u : 0x80000000u + u32(next() % 0x100000u) * 2u;
        const u32 spread = kind == 0u ? 64u * 244u : (kind == 1u ? 0x400000u : 4096u);
        for (u32 i = 0; i < count; ++i) a[i] = (base + u32(next() % spread)) & ~1u;
        if (kind == 2u && count > 2u) a[count - 1u] = a[u32(next() % (count - 1u))];  // a cycle back
        const u32 expected = reference_first_overlap(a.data(), count);
        require(first_ax_block_overlap(a.data(), count) == expected, "voice selection: first overlap differs");
        ++overlap_cases;
        overlaps_found += expected != count;
    }
    // Assignment: frame sequences over a persistent slot table, as the game uses it, plus restored tables holding
    // empty slots and blocks foreign to the chain. Both implementations need what a checkpoint load enforces
    // (System::load_host_audio_state): the nonzero slots hold distinct blocks, so a free slot always exists.
    u64 assign_cases = 0, resets_seen = 0;
    for (u32 sequence = 0; sequence < 4000u; ++sequence) {
        std::array<u32, 64> pool{};
        for (u32 i = 0; i < 64u; ++i) pool[i] = 0x80100000u + i * 256u;  // 64 non-overlapping blocks
        std::array<u32, 64> slots_old{}, slots_new{};
        if (sequence % 5u == 4u) {
            std::array<u32, 128> candidates{};
            for (u32 i = 0; i < 64u; ++i) candidates[i] = pool[i];
            for (u32 i = 64u; i < 128u; ++i) candidates[i] = 0x80900000u + i * 256u;  // never chained
            for (u32 i = 127u; i > 0u; --i) std::swap(candidates[i], candidates[u32(next() % (i + 1u))]);
            for (u32 v = 0; v < 64u; ++v) slots_old[v] = next() % 3u == 0u ? 0u : candidates[v];
        }
        slots_new = slots_old;
        for (u32 frame = 0; frame < 40u; ++frame) {
            for (u32 i = 63u; i > 0u; --i) std::swap(pool[i], pool[u32(next() % (i + 1u))]);
            const u32 count = (frame % 7u == 6u) ? 64u : u32(next() % 65u);
            std::array<u32, 64> order_old{}, order_new{};
            const u64 reset_old = reference_assign(pool.data(), count, slots_old, order_old);
            const u64 reset_new = assign_ax_voices(pool.data(), count, slots_new, order_new);
            require(reset_old == reset_new && slots_old == slots_new &&
                        std::equal(order_old.begin(), order_old.begin() + count, order_new.begin()),
                    "voice selection: slot assignment differs");
            ++assign_cases;
            resets_seen += reset_old != 0u;
        }
    }
    // The path System::select_ax_parameter_blocks takes: the unchanged chain first, then the full assignment. The
    // former scans run on the same chains with no state but the slot table; a checkpoint load (slots restored,
    // previous count zero) interrupts some sequences.
    u64 select_frames = 0, unchanged_frames = 0, loads = 0;
    for (u32 sequence = 0; sequence < 4000u; ++sequence) {
        std::array<u32, 64> pool{};
        for (u32 i = 0; i < 64u; ++i) pool[i] = 0x80100000u + i * 256u;
        std::array<u32, 64> slots_old{}, slots_new{}, order_new{}, chain{};
        u32 previous_count = 0, count = 0;
        for (u32 frame = 0; frame < 60u; ++frame) {
            const u64 what = next() % 20u;
            if (what == 0u) {  // a checkpoint load
                std::array<u32, 128> candidates{};
                for (u32 i = 0; i < 64u; ++i) candidates[i] = pool[i];
                for (u32 i = 64u; i < 128u; ++i) candidates[i] = 0x80900000u + i * 256u;
                for (u32 i = 127u; i > 0u; --i) std::swap(candidates[i], candidates[u32(next() % (i + 1u))]);
                for (u32 v = 0; v < 64u; ++v) slots_old[v] = next() % 3u == 0u ? 0u : candidates[v];
                slots_new = slots_old;
                order_new.fill(0u);
                previous_count = 0;
                ++loads;
            }
            if (what >= 13u || frame == 0u) {  // a new chain
                for (u32 i = 63u; i > 0u; --i) std::swap(pool[i], pool[u32(next() % (i + 1u))]);
                count = what == 19u ? u32(next() % 65u) : 64u;
                std::copy(pool.begin(), pool.begin() + count, chain.begin());
            } else if (what >= 10u && count > 1u) {  // the same blocks reordered
                std::swap(chain[u32(next() % count)], chain[u32(next() % count)]);
            }                                         // otherwise the previous chain again
            std::array<u32, 64> order_old{};
            const u64 reset_old = reference_assign(chain.data(), count, slots_old, order_old);
            const bool unchanged = ax_chain_unchanged(chain.data(), count, slots_new, order_new, previous_count);
            const u64 reset_new = unchanged ? ax_empty_slots(slots_new)
                                            : assign_ax_voices(chain.data(), count, slots_new, order_new);
            previous_count = count;
            require(reset_old == reset_new && slots_old == slots_new &&
                        std::equal(order_old.begin(), order_old.begin() + count, order_new.begin()),
                    "voice selection: the unchanged-chain path differs from the former scans");
            ++select_frames;
            unchanged_frames += unchanged;
        }
    }
    std::cout << "PASS voice selection oracle: " << overlap_cases << " overlap cases (" << overlaps_found
              << " with an overlap), " << assign_cases << " assignment frames (" << resets_seen
              << " with resets), " << select_frames << " selection frames (" << unchanged_frames
              << " unchanged chains, " << loads << " checkpoint loads) identical to the former scans\n";

    // Paired timing (diagnostic): 64 blocks, the game's chain length, repeated or reordered every frame.
    for (const bool reorder : {false, true}) {
        std::array<u32, 64> chain{};
        for (u32 i = 0; i < 64u; ++i) chain[i] = 0x80100000u + i * 256u;
        std::array<u32, 64> slots_old{}, slots_new{}, order_old{}, order_new{};
        u32 previous_count = 0;
        double best_old = 1e30, best_new = 1e30;
        u64 sink = 0;
        for (u32 rep = 0; rep < 7u; ++rep) {
            for (u32 arm = 0; arm < 2u; ++arm) {
                const auto t0 = std::chrono::steady_clock::now();
                for (u32 frame = 0; frame < 20000u; ++frame) {
                    if (reorder) std::swap(chain[frame % 64u], chain[(frame * 7u + 3u) % 64u]);
                    if (arm == 0u) {
                        sink += reference_first_overlap(chain.data(), 64u) +
                                reference_assign(chain.data(), 64u, slots_old, order_old);
                    } else {
                        const bool unchanged =
                            ax_chain_unchanged(chain.data(), 64u, slots_new, order_new, previous_count);
                        if (!unchanged) sink += first_ax_block_overlap(chain.data(), 64u);
                        sink += unchanged ? ax_empty_slots(slots_new)
                                          : assign_ax_voices(chain.data(), 64u, slots_new, order_new);
                        previous_count = 64u;
                    }
                }
                const double ns =
                    std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 20000.0;
                (arm == 0u ? best_old : best_new) = std::min(arm == 0u ? best_old : best_new, ns);
            }
        }
        std::cout << "  timing (diagnostic, best of 7 alternating, 64 blocks, chain " << (reorder ? "reordered" : "repeated")
                  << "): former " << best_old << " ns, current " << best_new << " ns per frame (sink " << (sink & 1u)
                  << ")\n";
    }
}

// The former per-sample decoder of System::run_ax_mix, verbatim: the oracle for adpcm_decode_forward.
void reference_adpcm_decode(AxAdpcmVoiceCursor& cursor, s16* ring, hw::AudioFrameIndex64 ring_mask,
                            const AdpcmDecodeContext& ctx, hw::AudioFrameIndex64 count, AdpcmDecodeStats& stats) {
    for (u64 produced = 0; produced < count; ++produced) {
        s32 value = 0;
        for (u32 pass = 0; pass < 4u; ++pass) {
            if (cursor.nibble > ctx.end_address) {
                if (!ctx.looping) {
                    break;
                }
                cursor.nibble = ctx.loop_address;
                cursor.predictor = (ctx.loop_pred_scale >> 4) & 7u;
                cursor.scale = ctx.loop_pred_scale & 0xfu;
                cursor.yn1 = ctx.loop_yn1;
                cursor.yn2 = ctx.loop_yn2;
                ++stats.loop_context_reloads;
            }
            const u32 within = cursor.nibble & (kAdpcmNibblesPerFrame - 1u);
            if (within < kAdpcmHeaderNibbles) {
                const u8 header = ctx.samples[(cursor.nibble / 2u) & ctx.sample_mask];
                cursor.predictor = (header >> 4) & 7u;
                cursor.scale = header & 0xfu;
                cursor.nibble += kAdpcmHeaderNibbles - within;
                continue;
            }
            const u8 byte = ctx.samples[(cursor.nibble / 2u) & ctx.sample_mask];
            const s32 raw = ((cursor.nibble & 1u) == 0u) ? (byte >> 4) : (byte & 0xfu);
            const s32 nibble_value = (raw ^ 8) - 8;
            const s32 predicted = ctx.coefficients[cursor.predictor * 2u] * cursor.yn1 +
                                  ctx.coefficients[cursor.predictor * 2u + 1u] * cursor.yn2;
            value = ((nibble_value * (1 << cursor.scale)) * 2048 + 1024 + predicted) >> 11;
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            cursor.yn2 = cursor.yn1;
            cursor.yn1 = value;
            ++cursor.nibble;
            break;
        }
        ring[static_cast<std::size_t>(cursor.decoded & ring_mask)] = static_cast<s16>(value);
        ++cursor.decoded;
        ++stats.samples;
    }
}

void adpcm_decode_oracle() {
    u64 state = 0xD1B54A32D192ED03ull;
    const auto next = [&state] { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
    constexpr hw::AudioFrameIndex64 kRingMask = 1023u;
    std::vector<u8> aram(1u << 16);
    std::vector<s16> ring_old(1024), ring_new(1024);
    u64 cases = 0, samples = 0, reloads = 0, one_shot_ends = 0;
    for (u32 voice = 0; voice < 30000u; ++voice) {
        for (u8& b : aram) b = static_cast<u8>(next());
        s16 coefficients[16];
        for (s16& k : coefficients) {
            const u64 r = next() % 8u;
            k = r == 0u ? s16(-32768) : (r == 1u ? s16(32767) : static_cast<s16>(next()));
        }
        AdpcmDecodeContext ctx;
        ctx.samples = aram.data();
        ctx.sample_mask = u32(aram.size()) - 1u;
        ctx.coefficients = coefficients;
        const u32 base = u32(next() % 0x20000u);  // the masked ARAM index wraps too
        const u32 length = u32(next() % 900u);
        ctx.end_address = base + length;
        ctx.looping = next() % 3u != 0u;
        const u64 loop_kind = next() % 4u;  // anywhere, a header nibble, the end itself, past the end
        ctx.loop_address = loop_kind == 0u ? base + u32(next() % (length + 1u))
                         : loop_kind == 1u ? ((base + u32(next() % (length + 1u))) & ~15u) + u32(next() % 2u)
                         : loop_kind == 2u ? ctx.end_address : ctx.end_address + 1u + u32(next() % 40u);
        ctx.loop_pred_scale = u32(next() & 0xffffu);
        ctx.loop_yn1 = static_cast<s16>(next());
        ctx.loop_yn2 = static_cast<s16>(next());
        AxAdpcmVoiceCursor a;
        a.nibble = base + u32(next() % (length + 20u));
        a.predictor = u32(next() % 8u);
        a.scale = u32(next() % 16u);
        a.yn1 = static_cast<s16>(next());
        a.yn2 = static_cast<s16>(next());
        a.decoded = next();
        a.valid = true;
        AxAdpcmVoiceCursor b = a;
        for (std::size_t i = 0; i < ring_old.size(); ++i) ring_old[i] = ring_new[i] = static_cast<s16>(next());
        AdpcmDecodeStats sa, sb;
        for (u32 call = 0; call < 6u; ++call) {  // continuing calls, as successive AX frames make them
            const u64 count = next() % 700u;
            reference_adpcm_decode(a, ring_old.data(), kRingMask, ctx, count, sa);
            adpcm_decode_forward(b, ring_new.data(), kRingMask, ctx, count, sb);
            require(a.nibble == b.nibble && a.predictor == b.predictor && a.scale == b.scale && a.yn1 == b.yn1 &&
                        a.yn2 == b.yn2 && a.decoded == b.decoded && sa.samples == sb.samples &&
                        sa.loop_context_reloads == sb.loop_context_reloads && ring_old == ring_new,
                    "ADPCM decode: the run decoder differs from the per-sample loop");
            ++cases;
        }
        samples += sa.samples;
        reloads += sa.loop_context_reloads;
        one_shot_ends += !ctx.looping && a.nibble > ctx.end_address;
    }
    std::cout << "PASS ADPCM decode oracle: " << cases << " continuing calls, " << samples << " samples, " << reloads
              << " loop context reloads, " << one_shot_ends << " finished one-shots identical to the per-sample loop\n";

    // Paired timing (diagnostic): a looping voice, 160 output samples at ratio 1.0 per AX frame.
    s16 coefficients[16] = {2048, -1024, 3000, -1500, 1200, 300, 4000, -2000, 512, 256, 1800, -900, 2600, -1300, 100, 50};
    for (u8& b : aram) b = static_cast<u8>(next());
    AdpcmDecodeContext ctx;
    ctx.samples = aram.data();
    ctx.sample_mask = u32(aram.size()) - 1u;
    ctx.coefficients = coefficients;
    ctx.end_address = 0x8000u + 16u * 2000u - 1u;
    ctx.looping = true;
    ctx.loop_address = 0x8002u;
    double best_old = 1e30, best_new = 1e30;
    for (u32 rep = 0; rep < 7u; ++rep) {
        for (u32 arm = 0; arm < 2u; ++arm) {
            AxAdpcmVoiceCursor cursor;
            cursor.nibble = 0x8000u;
            AdpcmDecodeStats stats;
            const auto t0 = std::chrono::steady_clock::now();
            for (u32 frame = 0; frame < 20000u; ++frame) {
                if (arm == 0u) reference_adpcm_decode(cursor, ring_old.data(), kRingMask, ctx, 161u, stats);
                else adpcm_decode_forward(cursor, ring_new.data(), kRingMask, ctx, 161u, stats);
            }
            const double ns =
                std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 20000.0;
            (arm == 0u ? best_old : best_new) = std::min(arm == 0u ? best_old : best_new, ns);
        }
    }
    require(ring_old == ring_new, "ADPCM decode timing arms decoded different samples");
    std::cout << "  timing (diagnostic, best of 7 alternating, 161 samples per call): former " << best_old
              << " ns, run decoder " << best_new << " ns per call\n";
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--room-effect") == 0) {
            room_effect();
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--adpcm-decode-oracle") == 0) {
            adpcm_decode_oracle();
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--voice-selection-oracle") == 0) {
            voice_selection_oracle();
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--checkpoint-history") == 0) {
            checkpoint_audio_history();
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--accepted-list-owner") == 0) {
            accepted_list_owner();
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--adpcm-loop-oracle") == 0) {
            adpcm_loop_oracle();
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--muted-history-oracle") == 0) {
            muted_history_oracle();
            return 0;
        }
        // PBs at the SDK array's addresses live in the native image since stage e step 2 (guest memory is not their store), so the
        // "same identities" runs use another pair of guest addresses with the same stride; the relocated run below uses far ones.
        const auto a = run(0x80F20000u, 0x80F200F4u);
        const auto control = run(0x80F20000u, 0x80F200F4u);
        const auto relocated = run(0x80F10000u, 0x80F20000u);
        require(a == control, "same-configuration AX control diverged");
        require(a == relocated, "relocating PBs changed PCM bytes");
        std::cout << "PASS AX published PB chain: control and relocated PCM byte-identical\n";
        checkpoint_audio_history();
        failures();
        publication();
        dma_mapping();
        capacity();
        pcm16_store_top();
        linear_src();
        muted_history_oracle();
        accepted_list_owner();
        command_list_programs();
        compressor_contract();
        voice_selection_oracle();
        adpcm_decode_oracle();
        room_effect();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n'; return 1;
    }
}
