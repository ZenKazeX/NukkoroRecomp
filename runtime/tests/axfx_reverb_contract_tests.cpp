// Original-DOL ownership and signal-path oracle for the active reverb migration.
// liveness-roots: reference-interpreter-only
// No generated reference body: every SDK instruction comes from the pinned DOL.
// The allocator is a guarded test fixture, not a proposed production heap.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_reverb_owner.h"
#include "audio/reverb.h"
#include "audio/output_stage.h"
#include "audio/effect_stage.h"
#include "support/audio_output_reference.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kState = 0x81000000u, kSamples = 0x81001000u, kArgs = 0x81002000u;
constexpr u32 kHeap = 0x81004000u, kHeapLimit = 0x81040000u;
constexpr u32 kReturn = 0x817FF000u, kAlloc = kReturn + 4, kFree = kReturn + 8;
constexpr u32 kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kKernel = 0x80387EB4u, kCallback = 0x803885A0u;
constexpr u32 kGuard = 0xA5A5A5A5u;
using Parameters = std::array<float, 6>; // color, level, time, damping, pre-delay, crossfeed
constexpr Parameters kLive{{0.5f, 1.0f, 1.0f, 0.8f, 0.01f, 0.3f}};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
u32 bits(float value) {
    u32 result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

struct Allocation { u32 address, bytes; bool live; };
struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    std::vector<Allocation> allocations;
    unsigned alloc_calls = 0, fail_at = 0, kernels = 0;
    u32 cursor = kHeap;
    u64 instructions = 0;
    bool cache_fixture = false;

    explicit Oracle(const char* path) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        // SHA-256: fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        reset();
    }
    void reset() {
        allocations.clear(); alloc_calls = fail_at = kernels = 0; cursor = kHeap; cache_fixture = false;
        const std::vector<u8> zero(kHeapLimit - kState);
        memory.write_block(kState, zero.data(), zero.size());
        memory.write32(kR13 + 0x27B8, kAlloc);
        memory.write32(kR13 + 0x27BC, kFree);
    }
    void returned() { cpu.pc = cpu.lr; cpu.npc = cpu.pc + 4; }
    void allocate() {
        const u32 bytes = cpu.gpr[3];
        if (++alloc_calls == fail_at) { cpu.gpr[3] = 0; returned(); return; }
        require(bytes && bytes < 32768 && cursor + bytes + 64 < kHeapLimit, "unbounded allocation");
        const u32 address = cursor + 16;
        allocations.push_back({address, bytes, true});
        for (u32 offset = 0; offset < 16; offset += 4) {
            memory.write32(address - 16 + offset, kGuard);
            memory.write32(address + bytes + offset, kGuard);
        }
        for (u32 offset = 0; offset < bytes; offset += 4) memory.write32(address + offset, 0xCDCDCDCDu);
        cursor = (address + bytes + 31) & ~15u;
        cpu.gpr[3] = address; returned();
    }
    void free() {
        const auto found = std::find_if(allocations.begin(), allocations.end(),
            [&](const Allocation& a) { return a.address == cpu.gpr[3] && a.live; });
        require(found != allocations.end(), "unknown allocation or double free");
        found->live = false; returned();
    }
    void guards() {
        for (const auto& a : allocations) for (u32 offset = 0; offset < 16; offset += 4)
            require(memory.peek32(a.address - 16 + offset) == kGuard &&
                    memory.peek32(a.address + a.bytes + offset) == kGuard, "allocation guard overwritten");
    }
    void call(u32 entry, u32 r3 = kState, u32 r4 = 0, u32 r5 = 0,
              const std::function<void()>& observe = {}, u32 floating_mode = 0) {
        cpu.reset(); cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn;
        cpu.fpscr = floating_mode;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3; cpu.gpr[4] = r4; cpu.gpr[5] = r5;
        const u64 tick = cpu.game_clock().now();
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 1000000) {
            if (observe) observe();
            if (cpu.pc == kAlloc) { allocate(); continue; }
            if (cpu.pc == kFree) { free(); continue; }
            // The isolated oracle has no OS syscall vector or DMA cache.
            // Check cache-maintenance arguments at the platform boundary;
            // execute the original builder/slot/callback instructions intact.
            if (cache_fixture && (cpu.pc == 0x80372508u || cpu.pc == 0x80372534u || cpu.pc == 0x80372594u)) {
                const u32 address = cpu.gpr[3], size = cpu.gpr[4];
                require((address == kArgs && size == 0x300) ||
                        (address >= 0x80587EC0u && u64(address) + size <= 0x8058ABC0u &&
                         (size == 0x780 || size == 0x280)), "unexpected cache-maintenance range");
                returned(); continue;
            }
            if (cpu.pc == kKernel) ++kernels;
            cpu.run_steps(1); ++instructions;
        }
        if (cpu.pc != kReturn || cpu.stopped())
            std::fprintf(stderr, "entry=%08X pc=%08X steps=%u stop=%s\n", entry, cpu.pc, steps,
                         cpu.stop().detail.c_str());
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack &&
                cpu.game_clock().now() == tick, "original call failed return/stack/time contract");
        guards();
    }
    bool initialize(const Parameters& parameters = kLive) {
        for (unsigned i = 0; i < parameters.size(); ++i) memory.write32(kState + 0x1C8 + i * 4, bits(parameters[i]));
        call(0x803884E4u);
        return cpu.gpr[3] != 0;
    }
    void release() {
        call(0x80388554u);
        require(std::none_of(allocations.begin(), allocations.end(), [](const Allocation& a) { return a.live; }),
                "original release leaked an allocation");
    }
    std::vector<u8> state() {
        std::vector<u8> result(cursor - kState);
        memory.read_block(kState, result.data(), result.size());
        return result;
    }
};

void compare_native_history(Oracle& o, const shadow::audio::Reverb& effect, unsigned frame, u32 fp_mode);

void lifecycle(Oracle& o) {
    o.reset(); require(o.initialize(), "live reverb initialization failed");
    require(o.allocations.size() == 21, "live initialization must allocate 18 lines and 3 pre-delays");
    const std::array<u32, 6> delay{{1789, 1999, 2333, 433, 149, 47}};
    for (unsigned channel = 0; channel < 3; ++channel) for (unsigned line = 0; line < 6; ++line) {
        const u32 descriptor = kState + channel * 60 + (line < 3 ? 0xB4 + line * 20 : (line - 3) * 20);
        const u32 length = line == 5 ? std::array<u32, 3>{{47, 73, 67}}[channel] : delay[line];
        require(o.memory.peek32(descriptor) == 0 && o.memory.peek32(descriptor + 4) == 8 &&
                o.memory.peek32(descriptor + 8) == (length + 2) * 4 &&
                o.memory.peek32(descriptor + 16) == 0, "initialized delay offsets/length/history differ");
    }
    for (unsigned channel = 0; channel < 3; ++channel)
        require(o.memory.peek32(kState + 0x1AC + 4 * channel) == o.memory.peek32(kState + 0x1B8 + 4 * channel),
                "pre-delay cursor does not start at base");
    require(o.memory.peek32(kState + 0x1A4) == 320, "live pre-delay is not 320 samples");
    o.release(); o.release();
    for (unsigned failure = 1; failure <= 21; ++failure) {
        o.reset(); o.fail_at = failure;
        require(!o.initialize() && o.alloc_calls == failure, "allocation failure was not propagated");
        require(std::none_of(o.allocations.begin(), o.allocations.end(), [](const Allocation& a) { return a.live; }),
                "partial initialization leaked an allocation");
        o.release();
    }
    // Invalid finite authored parameters must fail before allocating anything.
    for (unsigned field = 0; field < 6; ++field) for (float invalid : {-1.0f, 11.0f}) {
        o.reset(); Parameters parameters = kLive; parameters[field] = invalid;
        require(!o.initialize(parameters) && o.alloc_calls == 0, "invalid parameters allocated state");
    }
    std::puts("PASS original lifecycle: 21 allocation-failure points, release twice, 12 invalid parameters");
}

// The original callback against what the native owner does in its place: the parameters it reads from the
// game's object, the bypass byte it honours and the native effect. Samples and the complete retained history match.
void compare_callback(Oracle& original) {
    unsigned callbacks = 0, nonzero = 0, bypassed = 0;
    const std::array<Parameters, 4> parameters{{kLive, {{0, 0.5f, 0.01f, 0, 0, 0}},
        {{1, 1, 10, 1, 0.1f, 1}}, {{0.25f, 0, 2, 0.05f, 0.01f, 0.75f}}}};
    for (const auto& config : parameters) for (u32 fp_mode : {0u, 4u}) {
        original.reset();
        require(original.initialize(config), "valid configuration rejected");
        shadow::audio::Reverb::Parameters read;
        require(gekko::reverb::read_parameters(original.memory, kState, read) &&
                    read.color == config[0] && read.level == config[1] && read.time == config[2] &&
                    read.damping == config[3] && read.pre_delay == config[4] && read.crossfeed == config[5],
                "the owner read other parameters than the game wrote");
        shadow::audio::Reverb effect;
        require(effect.configure(read), "native effect refused the game's parameters");
        const auto floating = fp_mode ? shadow::math::SubnormalMode::Flush : shadow::math::SubnormalMode::Preserve;
        for (unsigned frame = 0; frame < 64; ++frame) {
            std::array<std::array<std::int32_t, 160>, 3> native{};
            for (unsigned channel = 0; channel < 3; ++channel) {
                const u32 address = kSamples + channel * 640;
                original.memory.write32(kArgs + channel * 4, address);
                for (unsigned i = 0; i < 160; ++i) {
                    // Impulses, opposite signs, integer->float rounding edges, then a long tail.
                    u32 sample = 0;
                    if (frame < 8) sample = u32(s32((frame * 173 + i * 917 + channel * 1103) % 65536) - 32768);
                    if (frame == 0 && i == 0) sample = 0x7FFFFFFFu;
                    if (frame == 0 && i == 1) sample = 0x80000000u;
                    original.memory.write32(address + i * 4, sample);
                    native[channel][i] = s32(sample);
                }
            }
            const bool bypass = frame == 31;
            original.memory.write8(kState + gekko::reverb::kBypassOffset, bypass ? 1 : 0);
            const auto before = original.state();
            original.call(kCallback, kArgs, kState, 0, {}, fp_mode);
            if (bypass) {
                require(original.state() == before, "bypass changed samples or delay state");
                ++bypassed;
            } else {
                effect.process({native[0].data(), native[1].data(), native[2].data()}, 160, floating);
            }
            for (unsigned channel = 0; channel < 3; ++channel)
                for (unsigned i = 0; i < 160; ++i) {
                    const u32 word = original.memory.peek32(kSamples + channel * 640 + i * 4);
                    require(word == u32(native[channel][i]), "native effect differs from the original callback");
                    nonzero += word != 0u;
                }
            compare_native_history(original, effect, frame, fp_mode);
            ++callbacks;
        }
        original.release();
    }
    require(nonzero > 0 && bypassed == 8, "callback oracle only exercised silence");
    std::printf("PASS original callback vs native owner: %u callbacks (%u bypassed), 4 configurations, 2 floating "
                "modes, %u nonzero samples, full retained history\n", callbacks, bypassed, nonzero);
}

void predelay(Oracle& o) {
    unsigned observed = 0;
    for (unsigned channel = 0; channel < 3; ++channel) {
        o.reset(); require(o.initialize(), "pre-delay initialization failed");
        const u32 base = o.memory.peek32(kState + 0x1AC + channel * 4);
        float previous = 0;
        for (unsigned frame = 0; frame < 4; ++frame) {
            std::array<float, 160> input{};
            for (unsigned i = 0; i < input.size(); ++i) {
                const s32 value = s32(frame * 160 + i + 1) * (channel == 1 ? -1 : 1);
                input[i] = float(value); o.memory.write32(kSamples + i * 4, u32(value));
            }
            unsigned sample = 0;
            o.call(kKernel, kSamples, kState, channel, [&] {
                if (o.cpu.pc != 0x80388030u && o.cpu.pc != 0x803881C8u) return;
                require(sample < input.size(), "extra pre-delay sample observed");
                double delayed; std::memcpy(&delayed, &o.cpu.fpr[13].bits[0], sizeof(delayed));
                require(delayed == double(previous), "pre-delay is not exactly one input sample");
                previous = input[sample++]; ++observed;
            });
            require(sample == 160 && o.memory.peek32(kState + 0x1B8 + channel * 4) == base &&
                    o.memory.peek32(base) == bits(previous) && o.memory.peek32(base + 0x1B8) == base + 4,
                    "original pre-delay cursor/stray-store contract differs");
            for (unsigned i = 1; i < 320; ++i) if (i != 110)
                require(o.memory.peek32(base + i * 4) == 0, "original unexpectedly used the rest of pre-delay line");
        }
        o.release();
    }
    // Negative control: repair only the two cursor-store base registers inside
    // this test's private DOL memory. It MUST break the one-sample contract.
    o.reset(); require(o.initialize(), "negative-control initialization failed");
    const u32 first = o.memory.peek32(0x8038802Cu), last = o.memory.peek32(0x803881C4u);
    require(first == 0x93BE01B8u && last == first, "cursor-store words differ");
    o.memory.write32(0x8038802Cu, 0x93BC01B8u); o.memory.write32(0x803881C4u, 0x93BC01B8u);
    for (unsigned i = 0; i < 160; ++i) o.memory.write32(kSamples + i * 4, i + 1);
    unsigned sample = 0, differences = 0;
    o.call(kKernel, kSamples, kState, 0, [&] {
        if (o.cpu.pc != 0x80388030u && o.cpu.pc != 0x803881C8u) return;
        double delayed; std::memcpy(&delayed, &o.cpu.fpr[13].bits[0], sizeof(delayed));
        if (delayed != double(sample)) ++differences;
        ++sample;
    });
    o.memory.write32(0x8038802Cu, first); o.memory.write32(0x803881C4u, last);
    require(sample == 160 && differences == 159, "negative control did not distinguish a moving cursor");
    o.release();
    std::printf("PASS original pre-delay: %u samples across calls/channels; moving-cursor control differs at 159/160\n",
                observed);
}

// Game control instructions execute intact. Unrelated voice/pool teardown calls
// are explicit fixtures: this proves publication/flag/call order, not device
// shutdown, allocator lifetime, interleavings or every possible object alias.
void game_effect_controls(Oracle& o) {
    constexpr u32 object = kArgs, audio_object = 0x81080000u;
    constexpr u32 effect = audio_object + 0xFF58;
    unsigned enables = 0, disables = 0, mode_calls = 0, shutdowns = 0;
    o.reset(); o.call(0x80384BC8u);
    o.memory.write32(kR13 + 0x5FAC, audio_object);
    o.memory.write32(audio_object, 1); o.memory.write32(audio_object + 8, 0);
    o.memory.write8(audio_object + 0x13A3B, 0);
    std::array<u8, 0x1E0> sentinel{}, after{};
    for (unsigned i=0; i<sentinel.size(); ++i) sentinel[i]=u8(i*37+11);
    o.memory.write_block(effect,sentinel.data(),sentinel.size());
    const auto registered = [&] { return o.memory.peek32(kR13 + 0x5CF0) != 0; };
    const auto observe = [&] {
        if (o.cpu.pc == 0x80385050u) {
            if (o.cpu.gpr[3]) {
                require(o.cpu.gpr[3] == kCallback && o.cpu.gpr[4] == effect,
                        "game enabled another effect or owner");
                ++enables;
            } else {
                require(o.cpu.gpr[4] == 0, "disable retained callback context"); ++disables;
            }
        }
        if (o.cpu.pc == 0x804402D4u) {
            require(o.cpu.gpr[3] == 2, "game control argument changed");
            ++mode_calls; // execute the original exclusion/update path intact
        }
    };
    for (unsigned cycle=0; cycle<3; ++cycle) {
        o.call(0x801F2134u,object+0x28,0,0,observe);
        require(registered() && o.memory.peek8(object+0x28)==1 &&
                o.memory.peek8(object+0x29)==0 && o.memory.peek8(object+0x2A)==0,
                "game enable flag/publication differs");
        const auto before_enables=enables, before_mode=mode_calls;
        o.call(0x801F2134u,object+0x28,0,0,observe);
        require(enables==before_enables && mode_calls==before_mode,"repeated enable was not inert");
        o.call(0x801F2A2Cu,object,0,0,observe);
        require(!registered() && o.memory.peek8(object+0x28)==0 &&
                o.memory.peek8(object+0x29)==0 && o.memory.peek8(object+0x2A)==0,
                "game disable flag/publication differs");
        const auto before_disables=disables;
        o.call(0x801F2A2Cu,object,0,0,observe);
        require(disables==before_disables,"repeated disable was not inert");
        o.memory.read_block(effect,after.data(),after.size());
        require(after==sentinel,"enable/disable reset or rewrote effect history");
    }
    for (bool enabled : {false,true}) {
        if (enabled) o.call(0x801F2134u,object+0x28,0,0,observe);
        unsigned phase=0;
        const auto shutdown = [&] {
            observe();
            if (o.cpu.pc == 0x804405FCu) {
                require(!registered() && o.memory.peek8(object+0x28)==0 && phase==0,
                        "voice stop precedes effect unregister/flag clear");
                phase=1; o.cpu.gpr[3]=0; o.returned();
            } else if (o.cpu.pc == 0x8043F674u) {
                require(phase==1 && !registered(),"resource teardown precedes voice stop/unregister");
                phase=2; ++shutdowns; o.cpu.gpr[3]=0; o.returned();
            } else if (o.cpu.pc == 0x8038A2BCu || o.cpu.pc == 0x803846CCu) {
                require(phase==2,"platform shutdown precedes owner teardown"); o.returned();
            }
        };
        // No owned heap here: object+4 and the deleting-destructor flag are zero.
        o.call(0x801F2DA8u,object,0,0,shutdown);
        require(phase==2 && o.cpu.gpr[3]==object,"destructor did not complete owner shutdown");
    }
    require(enables==4 && disables==4 && mode_calls==8 && shutdowns==2,
            "game control call counts differ");
    require(o.memory.peek32(audio_object + 8)==0 && o.memory.peek8(audio_object + 0x13A3B)==8,
            "control update did not preserve exclusion depth and reject nested work");
    // Explicit counterexample: a corrupted active flag does not itself protect
    // callback lifetime. Do not claim the destructor repairs arbitrary state.
    o.call(0x80440290u,0,0,0,observe);
    require(registered() && o.memory.peek8(object+0x28)==0,"counterexample setup failed");
    bool unsafe_teardown=false;
    o.call(0x801F2DA8u,object,0,0,[&] {
        if (o.cpu.pc == 0x8043F674u) unsafe_teardown=registered();
        if (o.cpu.pc == 0x804405FCu || o.cpu.pc == 0x8043F674u ||
            o.cpu.pc == 0x8038A2BCu || o.cpu.pc == 0x803846CCu) o.returned();
    });
    require(unsafe_teardown,"inconsistent flag counterexample was not distinguished");
    o.call(0x80384CACu);
    std::puts("PASS original game effect control: repeated enable/disable preserves history; 2 ordered shutdowns; inconsistent-flag counterexample");
}

// Bank selection is a game operation: disable the effect around the loader,
// then enable it even for a failed load or unmatched selector. Only asset I/O
// is a fixture here; selector, virtual dispatch and exclusion code run intact.
void game_bank_controls(Oracle& o) {
    constexpr u32 object=kArgs, audio_object=0x81080000u, effect=audio_object+0xFF58;
    unsigned cases=0, loads=0, stops=0;
    o.reset(); o.call(0x80384BC8u);
    o.memory.write32(kR13+0x5FAC,audio_object);
    o.memory.write32(audio_object,1); o.memory.write32(audio_object+8,0);
    o.memory.write8(audio_object+0x13A3B,0);
    std::array<u8,0x1E0> sentinel{},after{};
    for (unsigned i=0;i<sentinel.size();++i) sentinel[i]=u8(i*53+7);
    o.memory.write_block(effect,sentinel.data(),sentinel.size());
    const auto registered=[&] { return o.memory.peek32(kR13+0x5CF0)!=0; };
    for (unsigned variant=0;variant<2;++variant) for (unsigned bank=0;bank<10;++bank)
    for (bool active : {false,true}) for (u32 result : {0u,0x1234u}) {
        o.call(0x80385050u,0,0);
        o.memory.write32(object,0x8053F42Cu);
        o.memory.write32(object+0x28,0);
        o.memory.write32(0x8058E7A0u,variant);
        if (active) o.call(0x801F2134u,object+0x28);
        unsigned enabled=0,disabled=0,loaded=0;
        const unsigned selector=bank==9 ? 7 : bank;
        const u32 key=bank==9 ? 0xFFFFFFFFu : o.memory.peek32(0x8053F2C0u);
        o.call(0x801F2A80u,object,selector,key,[&] {
            if (o.cpu.pc==0x80385050u) {
                if(o.cpu.gpr[3]) {
                    require(o.cpu.gpr[3]==kCallback && o.cpu.gpr[4]==effect,
                            "bank switch enabled an unexpected effect"); ++enabled;
                } else ++disabled;
            }
            if (o.cpu.pc==0x801F2CE4u) {
                require(!registered() && o.memory.peek8(object+0x28)==0 && o.cpu.gpr[3]==object,
                        "bank asset load ran while effect remained enabled");
                require(o.cpu.gpr[4]!=0,"bank selection passed an empty asset name");
                ++loaded; ++loads; o.cpu.gpr[3]=result; o.returned();
            }
        });
        const bool has_load=bank<8;
        require(loaded==unsigned(has_load) && enabled==1 && disabled==unsigned(active) &&
                registered() && o.memory.peek8(object+0x28)==1 &&
                o.cpu.gpr[3]==(has_load ? result : 0),"bank switch control/return contract differs");
        require(o.memory.peek32(audio_object+8)==0,"bank switch leaked exclusion depth");
        o.memory.read_block(effect,after.data(),after.size());
        require(after==sentinel,"bank switch reset effect history"); ++cases;
    }
    for(bool active : {false,true}) {
        o.call(0x80385050u,0,0);o.memory.write8(object+0x28,0);
        if(active) o.call(0x801F2134u,object+0x28);
        o.call(0x801F28A4u,object,0,0,[&] {
            if(o.cpu.pc==0x80440340u) {
                require(!registered() && o.memory.peek8(object+0x28)==0,
                        "stop-all ran before effect unregister");
                ++stops;o.returned();
            }
        });
        require(!registered() && o.memory.peek8(object+0x28)==0,"stop-all re-enabled effect");
    }
    require(cases==80 && loads==64 && stops==2,"bank-control case coverage differs");
    o.call(0x80384CACu);
    std::printf("PASS original bank controls: %u cases, %u load boundaries, failed/unmatched loads re-enable; %u stop-all paths\n",
                cases,loads,stops);
}

void auxiliary_slots(Oracle& o) {
    constexpr u32 bus_a = 0x80587EC0u, bus_b = 0x80589540u;
    constexpr u32 callback_a = kReturn + 16, callback_b = kReturn + 20;
    constexpr std::array<unsigned, 20> words{{2, 5, 2, 0, 4, 4, 2, 2, 4, 2, 0, 0, 0, 3, 4, 0, 4, 2, 4, 12}};
    unsigned callback_count = 0, returned_frames = 0, matrix_returns = 0;
    for (u32 mode = 0; mode < 3; ++mode) {
        o.reset();
        o.cache_fixture = true;
        const std::vector<u8> zero(0x2D00);
        o.memory.write_block(bus_a, zero.data(), zero.size());
        o.call(0x80384BC8u); // original auxiliary initialization
        o.memory.write32(kR13 + 0x5D38, mode);
        o.call(0x80385050u, callback_a, kState);
        o.call(0x8038505Cu, callback_b, kState + 4);
        require(o.memory.peek32(kR13 + 0x5D10) == 0 && o.memory.peek32(kR13 + 0x5D14) == 1 &&
                o.memory.peek32(kR13 + 0x5D20) == 2, "initial auxiliary slot phases differ");
        std::array<u32, 2> sends{}, returns{};
        u32 matrix_send = 0, matrix_return = 0;
        const auto build = [&] {
            o.memory.write32(kR13 + 0x5D2C, kArgs);
            o.call(0x803850ACu, kSamples, kSamples + 640); // actual command-list builder
            sends.fill(0); returns.fill(0);
            matrix_send = matrix_return = 0;
            bool ended = false;
            for (u32 at = kArgs; at < kArgs + 384;) {
                const u16 command = o.memory.peek16(at); at += 2;
                require(command < words.size(), "original builder produced invalid command");
                if (command == 4 || command == 5) {
                    const unsigned bus = command - 4;
                    sends[bus] = o.memory.peek32(at); returns[bus] = o.memory.peek32(at + 4);
                } else if (command == 0x10) {
                    matrix_send = o.memory.peek32(at); matrix_return = o.memory.peek32(at + 4);
                } else if (command == 0x13) {
                    sends[0] = o.memory.peek32(at); returns[0] = o.memory.peek32(at + 8);
                    require(o.memory.peek32(at + 12) == returns[0] + 640 &&
                            o.memory.peek32(at + 16) == returns[0] + 1280,
                            "mode-2 A return channel layout differs");
                }
                if (command == 15) { ended = true; break; }
                at += words[command] * 2;
            }
            require(ended && sends[0] != 0 && returns[0] != 0, "missing auxiliary A command");
            require(mode == 2 ? sends[1] == 0 : sends[1] != 0, "auxiliary B mode contract differs");
        };
        // Original frame owner 0x80385798 submits the PREVIOUS list before
        // 0x80385814 calls ProcessAux, then builds the NEXT list at 0x80385860.
        // This marker DSP exchange tests that ordering/ownership, not DSP math.
        build();
        for (unsigned frame = 0; frame < 9; ++frame) {
            for (unsigned bus = 0; bus < (mode == 2 ? 1u : 2u); ++bus) {
                const u32 base = bus == 0 ? bus_a : bus_b;
                require(sends[bus] == base + (frame % 3) * 1920 &&
                        returns[bus] == base + ((frame + 1) % 3) * 1920, "builder slot rotation differs");
                for (unsigned channel = 0; channel < 3; ++channel) {
                    const u32 returned = o.memory.peek32(returns[bus] + channel * 640);
                    const u32 expected = frame == 0 ? 0 : frame == 1 ? 10000 :
                        10000 + (frame - 1) * 100 + bus * 10 + channel;
                    require(returned == expected, "auxiliary send did not return after two DSP frames");
                    o.memory.write32(sends[bus] + channel * 640, (frame + 1) * 100 + bus * 10 + channel);
                }
                if (frame >= 2) ++returned_frames;
            }
            if (mode == 2) {
                require(matrix_send == bus_b + (frame % 2) * 1920 &&
                        matrix_return == bus_b + ((frame + 1) % 2) * 1920,
                        "matrix channels do not alternate two independent blocks");
                for (unsigned channel = 0; channel < 2; ++channel) {
                    const u32 returned = o.memory.peek32(matrix_return + channel * 640);
                    const u32 expected = frame ? frame * 1000 + channel : 0;
                    require(returned == expected, "matrix return is not the preceding block");
                    o.memory.write32(matrix_send + channel * 640, (frame + 1) * 1000 + channel);
                }
                if (frame) ++matrix_returns;
            } else require(matrix_send == 0 && matrix_return == 0, "matrix command outside mode 2");
            unsigned calls = 0;
            o.call(0x80384E14u, 0, 0, 0, [&] {
                if (o.cpu.pc != callback_a && o.cpu.pc != callback_b) return;
                const unsigned bus = o.cpu.pc == callback_a ? 0 : 1;
                require(mode != 2 || bus == 0, "mode 2 invoked auxiliary B callback");
                require(o.cpu.gpr[4] == kState + bus * 4, "callback context changed");
                const u32 base = (bus == 0 ? bus_a : bus_b) + ((frame + 2) % 3) * 1920;
                for (unsigned channel = 0; channel < 3; ++channel) {
                    const u32 address = o.memory.peek32(o.cpu.gpr[3] + channel * 4);
                    require(address == base + channel * 640, "callback channel pointers differ");
                    const u32 value = o.memory.peek32(address);
                    require(value == (frame ? frame * 100 + bus * 10 + channel : 0),
                            "callback did not receive previous DSP send");
                    o.memory.write32(address, value + 10000);
                }
                if (mode == 2)
                    require(o.memory.peek32(o.cpu.gpr[3] + 12) == base + 0x1B80,
                            "mode-2 fourth channel does not name auxiliary B surround");
                ++calls; ++callback_count; o.returned();
            });
            require(calls == (mode == 2 ? 1u : 2u), "auxiliary callback count differs");
            build();
        }
        o.call(0x80384CACu); // disable callbacks
        o.call(0x80384E14u, 0, 0, 0, [&] {
            require(o.cpu.pc != callback_a && o.cpu.pc != callback_b, "disabled callback still executes");
        });
    }
    std::printf("PASS original auxiliary ownership: 3 output modes, %u callbacks, %u two-frame returns, %u preceding-block matrix returns, disable\n",
                callback_count, returned_frames, matrix_returns);
}

void compare_native_history(Oracle& o, const shadow::audio::Reverb::History& saved, unsigned frame, u32 fp_mode) {
    // Compare the entire future-readable delay signal after compacting
    // the original (D+2)-cell/two-cursor representation into D cells.
    std::size_t offset = 0;
    for (unsigned channel = 0; channel < 3; ++channel) {
        for (unsigned line = 0; line < 6; ++line) {
            const unsigned index = channel * 6 + line;
            const u32 descriptor = kState + channel * 60 +
                (line < 3 ? 0xB4 + line * 20 : (line - 3) * 20);
            const u32 buffer = o.memory.peek32(descriptor + 12);
            const u32 length = o.memory.peek32(descriptor + 8) / 4;
            const u32 cursor = o.memory.peek32(descriptor + 4) / 4;
            require(bits(saved.delayed[index]) == o.memory.peek32(descriptor + 16),
                    "native delay feedback state differs");
            for (u32 i = 0; i < length - 2; ++i) {
                const u32 actual = bits(saved.samples[offset + (saved.positions[index] + i) % (length - 2)]);
                const u32 expected = o.memory.peek32(buffer + (cursor + i) % length * 4);
                if (actual != expected) {
                    std::fprintf(stderr, "delay frame=%u fp=%u channel=%u line=%u pending=%u expected=%08X "
                        "actual=%08X original_cursor=%u native_cursor=%u\n", frame, fp_mode, channel, line,
                        i, expected, actual, cursor, unsigned(saved.positions[index]));
                    throw std::runtime_error("native pending delay signal differs");
                }
            }
            offset += length - 2;
        }
        require(bits(saved.low_pass[channel]) == o.memory.peek32(kState + 0x190 + channel * 4),
                "native low-pass state differs");
    }
    if (o.memory.peek32(kState + 0x1A4)) for (unsigned channel = 0; channel < 3; ++channel) {
        const u32 previous = o.memory.peek32(kState + 0x1B8 + channel * 4);
        require(bits(saved.previous_input[channel]) == o.memory.peek32(previous),
                "native pre-delay state differs");
    }
}

void compare_native_history(Oracle& o, const shadow::audio::Reverb& effect, unsigned frame, u32 fp_mode) {
    compare_native_history(o, effect.history(), frame, fp_mode);
}

// Compose original CPU scheduling/callback and original output microcode.
// The fixture supplies already-mixed voice buses and bounded memory transfers;
// voice decode, compressor, device latency and mode transitions are outside it.
void composed_effect_output(Oracle& o, audio_reference::OutputWords& dsp) {
    using shadow::audio::OutputStage;
    using shadow::audio::Reverb;
    using Buffer = OutputStage::Buffer;
    using Channels = std::array<Buffer,3>;
    constexpr u32 bus_a=0x80587EC0u, bus_b=0x80589540u;
    constexpr std::array<unsigned,20> words{{2,5,2,0,4,4,2,2,4,2,0,0,0,3,4,0,4,2,4,12}};
    unsigned frames=0, missing_return_differences=0;
    const auto initial_instructions=dsp.instructions;
    dsp.transfer=[&](bool write,u32 address,u16* data,unsigned count) {
        const u64 end=u64(address)+count*2u;
        require((address>=bus_a && end<=bus_b+3*1920u) ||
                (address>=kSamples && end<=kSamples+640u),"composition transfer outside owned fixture");
        for (unsigned i=0;i<count;++i) {
            if (write) o.memory.write16(address+i*2,data[i]);
            else data[i]=o.memory.peek16(address+i*2);
        }
    };
    for (unsigned mode=0;mode<3;++mode) for (u32 fp_mode : {0u,4u}) {
        o.reset(); require(o.initialize(),"composed output init failed"); o.cache_fixture=true;
        const std::vector<u8> zero(0x2D00); o.memory.write_block(bus_a,zero.data(),zero.size());
        o.call(0x80384BC8u); o.memory.write32(kR13+0x5D38,mode);
        o.call(0x80385050u,kCallback,kState);
        Reverb effect; require(effect.configure({kLive[0],kLive[1],kLive[2],kLive[3],kLive[4],kLive[5]}),
                               "composed native effect configuration failed");
        OutputStage output, dry;
        Channels previous_send{}, returned{};
        Buffer previous_bs{}, returned_bs{};
        const auto native_mode=mode==0 ? OutputStage::Mode::Stereo :
                               mode==1 ? OutputStage::Mode::Opposite : OutputStage::Mode::Matrix;
        const auto float_mode=fp_mode ? shadow::math::SubnormalMode::Flush : shadow::math::SubnormalMode::Preserve;
        u32 stream=0x52671DBFu;
        const auto sample=[&] { stream^=stream<<13;stream^=stream>>17;stream^=stream<<5;
            return s32(stream&32767u)-16384; };
        const auto build=[&] { o.memory.write32(kR13+0x5D2C,kArgs); o.call(0x803850ACu,kSamples,kSamples+640); };
        build();
        for (unsigned frame=0;frame<32;++frame) {
            dsp.data.fill(0);
            Buffer left{},right{},surround{},bl{},br{},bs{},dry_l{},dry_r{},dry_s{},dry_bl{},dry_br{};
            Channels sent{};
            output.begin(native_mode,left,right,surround); dry.begin(native_mode,dry_l,dry_r,dry_s);
            if (mode!=2) dsp.run(mode==0 ? 0x066A : 0x020B,{kSamples});
            for (unsigned i=0;i<160;++i) {
                const s32 dl=sample()/8,dr=sample()/8,ds=sample()/8;
                left[i]+=dl; right[i]+=dr; surround[i]=ds;
                dry_l[i]+=dl; dry_r[i]+=dr; dry_s[i]=ds;
                dsp.put32(i*2,dsp.get32(i*2)+dl); dsp.put32(0x140+i*2,dsp.get32(0x140+i*2)+dr);
                dsp.put32(0x280+i*2,ds);
                for (unsigned channel=0;channel<3;++channel) {
                    sent[channel][i]=frame<8 ? sample() : 0;
                    dsp.put32(0x400+channel*320+i*2,sent[channel][i]);
                }
                bl[i]=dry_bl[i]=sample()/8; br[i]=dry_br[i]=sample()/8; bs[i]=sample()/8;
                dsp.put32(0x7C0+i*2,bl[i]); dsp.put32(0x900+i*2,br[i]); dsp.put32(0xA40+i*2,bs[i]);
            }
            bool ended=false;
            for (u32 at=kArgs;at<kArgs+384;) {
                const u16 op=o.memory.peek16(at);at+=2;
                require(op<words.size(),"composed builder command changed");
                if (op==4 || op==0x13 || op==0x10) {
                    std::vector<u32> args;
                    for (unsigned i=0;i<words[op]/2;++i) args.push_back(o.memory.peek32(at+i*4));
                    dsp.run(op==4 ? 0x0509 : op==0x13 ? 0x0E17 : 0x0DA1,args);
                } else require(op!=5,"unexpected second effect bus in game output");
                if (op==15) { ended=true;break; }
                at+=words[op]*2;
            }
            require(ended,"composed list did not end");
            for (unsigned i=0;i<160;++i) {
                left[i]+=returned[0][i]; right[i]+=returned[1][i];
                if (mode==2) { bl[i]+=returned[2][i]; br[i]+=returned_bs[i]; }
                else surround[i]+=returned[2][i];
            }
            output.finish(native_mode,left,right,surround,bl,br);
            dry.finish(native_mode,dry_l,dry_r,dry_s,dry_bl,dry_br);
            for (unsigned i=0;i<160;++i) {
                require(left[i]==dsp.get32(i*2) && right[i]==dsp.get32(0x140+i*2) &&
                        surround[i]==dsp.get32(0x280+i*2),"composed native output differs from original CPU/DSP words");
                missing_return_differences+=(left[i]!=dry_l[i])+(right[i]!=dry_r[i]);
                // The final output command publishes main surround here. Its
                // following fold is independently original-word tested above.
                o.memory.write32(kSamples+i*4,u32(dsp.get32(0x280+i*2)));
            }
            o.call(0x80384E14u,0,0,0,{},fp_mode);
            effect.process({previous_send[0].data(),previous_send[1].data(),previous_send[2].data()},160,float_mode);
            returned=previous_send; previous_send=sent;
            returned_bs=previous_bs; previous_bs=bs;
            compare_native_history(o,effect,frame,fp_mode);
            build(); ++frames;
        }
        o.call(0x80384CACu); o.release();
    }
    dsp.transfer={};
    require(missing_return_differences>10000,"missing-return negative control did not change enough samples");
    std::printf("PASS composed native effect/output: %u blocks, 3 modes, 2 floating modes, full effect history; "
                "missing-return control differs at %u channel samples; %llu original output instructions\n",
                frames,missing_return_differences,static_cast<unsigned long long>(dsp.instructions-initial_instructions));
}

// The native effect stage owns pending signal and control values. Original PPC
// controls/list/callback and DSP math supply the independent expected result.
// This does not claim these interleavings occur on every gameplay route or that
// the stage has been connected to the game's voice producer yet.
void effect_control_transitions(Oracle& o, audio_reference::OutputWords& dsp) {
    using shadow::audio::OutputStage;
    using Buffer=OutputStage::Buffer;
    using Channels=std::array<Buffer,3>;
    using Stage=shadow::audio::EffectStage;
    constexpr u32 bus_a=0x80587EC0u,bus_b=0x80589540u;
    constexpr std::array<unsigned,20> words{{2,5,2,0,4,4,2,2,4,2,0,0,0,3,4,0,4,2,4,12}};
    unsigned blocks=0,callbacks=0,late_controls=0,cleared_differences=0,restores=0;
    const auto initial_instructions=dsp.instructions;
    dsp.transfer=[&](bool write,u32 address,u16* data,unsigned count) {
        const u64 end=u64(address)+count*2u;
        require((address>=bus_a && end<=bus_b+3*1920u) ||
                (address>=kSamples && end<=kSamples+640u),"dynamic transfer outside fixture");
        for(unsigned i=0;i<count;++i) {
            if(write)o.memory.write16(address+i*2,data[i]);else data[i]=o.memory.peek16(address+i*2);
        }
    };
    for(unsigned initial_mode=0;initial_mode<3;++initial_mode) for(u32 fp_mode : {0u,4u}) {
        o.reset();require(o.initialize(),"dynamic original effect init failed");o.cache_fixture=true;
        const std::vector<u8> zero(0x2D00);o.memory.write_block(bus_a,zero.data(),zero.size());
        o.call(0x80384BC8u);o.memory.write32(kR13+0x5FAC,kState-0xFF58u);
        o.call(0x80440290u);
        Stage native,clear_on_disable,owned;
        gekko::reverb::Binding binding;
        const auto kind=[](unsigned mode) {return mode==0 ? Stage::Mode::Stereo :
            mode==1 ? Stage::Mode::Opposite : Stage::Mode::Matrix;};
        require(native.reset({}, {kind(initial_mode),true}) && clear_on_disable.reset({}, {kind(initial_mode),true}) &&
                owned.reset({}, {kind(initial_mode),true}),"native effect stage initialization failed");
        // The production owner's view of the game's start-up: ReverbHiInit's context and parameters.
        o.cpu.gpr[3]=kState;require(gekko::reverb::serve_init(o.cpu,binding,owned),"owner refused a readable context");
        require(binding.configured && binding.context==kState,"owner did not bind the initialized effect");
        auto floating=fp_mode ? shadow::math::SubnormalMode::Flush : shadow::math::SubnormalMode::Preserve;
        unsigned scheduled_mode=initial_mode;bool scheduled_enabled=true,previous_enabled=true;
        o.memory.write32(kR13+0x5D38,scheduled_mode);
        const auto build=[&] {o.memory.write32(kR13+0x5D2C,kArgs);o.call(0x803850ACu,kSamples,kSamples+640);};
        build();
        u32 stream=0x23C417D5u;
        const auto sample=[&] {stream^=stream<<13;stream^=stream>>17;stream^=stream<<5;return s32(stream&16383u)-8192;};
        for(unsigned frame=0;frame<60;++frame) {
            const unsigned cycle=frame%24;
            const bool live_enabled=cycle<6 || cycle==9 || cycle==10 || cycle>=19;
            const unsigned live_mode=(initial_mode+frame/5)%3;
            // The command list already exists. A control changes callback
            // publication now, without rewriting the accepted block's work.
            o.call(live_enabled ? 0x80440290u : 0x8044024Cu);
            o.memory.write32(kR13+0x5D38,live_mode);
            if(scheduled_enabled!=live_enabled)++late_controls;
            if(previous_enabled && !live_enabled) {
                auto cleared=clear_on_disable.history();cleared.pending={};cleared.spatial_right={};
                require(clear_on_disable.restore(cleared),"cleared-pending control restore failed");
            }
            previous_enabled=live_enabled;
            Channels main{},sent{};Buffer ml{},mr{},ms{};
            dsp.data.fill(0);
            if(scheduled_mode!=2)dsp.run(scheduled_mode==0 ? 0x066A : 0x020B,{kSamples});
            for(unsigned i=0;i<160;++i) {
                for(unsigned c=0;c<3;++c) {
                    main[c][i]=sample()/8;sent[c][i]=sample();
                    const unsigned offset=c*320+i*2;
                    dsp.put32(offset,dsp.get32(offset)+main[c][i]);
                    dsp.put32(0x400+offset,sent[c][i]);
                }
                ml[i]=sample()/8;mr[i]=sample()/8;ms[i]=sample()/8;
                dsp.put32(0x7C0+i*2,ml[i]);dsp.put32(0x900+i*2,mr[i]);dsp.put32(0xA40+i*2,ms[i]);
            }
            bool ended=false,sent_effect=false;
            for(u32 at=kArgs;at<kArgs+384;) {
                const u16 op=o.memory.peek16(at);at+=2;
                require(op<words.size(),"dynamic builder command changed");
                if(op==4 || op==0x13 || op==0x10) {
                    std::vector<u32> args;
                    for(unsigned i=0;i<words[op]/2;++i)args.push_back(o.memory.peek32(at+i*4));
                    dsp.run(op==4 ? 0x0509 : op==0x13 ? 0x0E17 : 0x0DA1,args);
                    if(op!=0x10)sent_effect=true;
                } else require(op!=5,"dynamic game unexpectedly used effect B");
                if(op==15){ended=true;break;}at+=words[op]*2;
            }
            require(ended && sent_effect==scheduled_enabled,"live control changed already-built send/return work");
            require(native.control({kind(live_mode),live_enabled}) &&
                    clear_on_disable.control({kind(live_mode),live_enabled}),"native live control rejected");
            const Stage::Input input{main,sent,{ml,mr,ms}};
            const auto actual=native.render(input,floating);
            const auto cleared=clear_on_disable.render(input,floating);
            // The mixer's order with the list's own schedule: fold, voices, send/return and matrix.
            Channels mixed{};
            require(owned.begin(kind(scheduled_mode),mixed[0],mixed[1],mixed[2]),"owned begin refused");
            for(unsigned c=0;c<3;++c) for(unsigned i=0;i<160;++i) mixed[c][i]+=main[c][i];
            require(owned.finish({kind(scheduled_mode),sent_effect},mixed[0],mixed[1],mixed[2],sent,{ml,mr,ms}),
                    "owned finish refused");
            for(unsigned c=0;c<3;++c) for(unsigned i=0;i<160;++i) {
                const auto expected=dsp.get32(c*320+i*2);
                if(actual[c][i]!=expected) {
                    std::fprintf(stderr,"effect control frame=%u mode=%u scheduled=%u live=%u channel=%u sample=%u actual=%d expected=%d\n",
                                 frame,scheduled_mode,scheduled_enabled,live_enabled,c,i,actual[c][i],expected);
                    throw std::runtime_error("pending effect ownership differs from original words");
                }
                cleared_differences+=cleared[c][i]!=expected;
                require(mixed[c][i]==expected,"owner-driven effect output differs from original words");
                if(c==2)o.memory.write32(kSamples+i*4,u32(expected));
            }
            unsigned calls=0;
            unsigned owner_calls=0;
            o.call(0x80384E14u,0,0,0,[&] {
                if(o.cpu.pc!=kCallback) return;
                ++calls;
                // The original aux processor reached the callback: the production owner serves the call here
                // (the original callback still runs, producing the expected words).
                const auto result=gekko::reverb::serve_callback(o.cpu,binding,owned);
                require(result==gekko::reverb::CallbackResult::Processed,"owner did not process a dispatched callback");
                ++owner_calls;
            },fp_mode);
            require(owner_calls==calls,"owner and original callback counts differ");
            require(calls==unsigned(live_enabled),"callback did not follow live control");callbacks+=calls;
            compare_native_history(o,native.history().effect,frame,fp_mode);
            compare_native_history(o,owned.history().effect,frame,fp_mode);
            if(frame%11==7) {
                Stage restored;
                require(restored.restore(native.history()),"dynamic effect stage restore failed");
                native=restored;++restores;
                Stage owned_restored;
                require(owned_restored.restore(owned.history()),"owned effect stage restore failed");
                owned=owned_restored;
            }
            scheduled_mode=live_mode;scheduled_enabled=live_enabled;build();++blocks;
        }
        o.call(0x80384CACu);o.release();
    }
    dsp.transfer={};
    require(late_controls>=24 && cleared_differences>1000,"dynamic controls did not distinguish pending samples");
    std::printf("PASS dynamic effect ownership (render and the production owner path): %u blocks, %u callbacks, "
                "%u control changes after scheduling, %u restores; "
                "cleared-pending control differs at %u channel samples; %llu output instructions\n",
                blocks,callbacks,late_controls,restores,cleared_differences,
                static_cast<unsigned long long>(dsp.instructions-initial_instructions));
}

void output_mode_transitions(Oracle& o, audio_reference::OutputWords& dsp) {
    using shadow::audio::OutputStage;
    constexpr u32 bus_a=0x80587EC0u, bus_b=0x80589540u;
    constexpr std::array<unsigned,20> words{{2,5,2,0,4,4,2,2,4,2,0,0,0,3,4,0,4,2,4,12}};
    constexpr std::array<unsigned,18> modes{{2,2,0,2,1,0,2,2,0,0,2,1,2,0,1,2,2,2}};
    o.reset(); o.cache_fixture=true;
    const std::vector<u8> zero(0x2D00);o.memory.write_block(bus_a,zero.data(),zero.size());
    o.call(0x80384BC8u);
    dsp.transfer=[&](bool write,u32 address,u16* data,unsigned count) {
        require((address>=bus_b && u64(address)+count*2<=bus_b+3*1920u) ||
                (address>=kSamples && u64(address)+count*2<=kSamples+640u),
                "mode transition transfer outside fixture");
        for(unsigned i=0;i<count;++i) {
            if(write) o.memory.write16(address+i*2,data[i]); else data[i]=o.memory.peek16(address+i*2);
        }
    };
    OutputStage output;
    for(unsigned frame=0;frame<modes.size();++frame) {
        const unsigned mode=modes[frame];
        o.memory.write32(kR13+0x5D38,mode);o.memory.write32(kR13+0x5D2C,kArgs);
        o.call(0x803850ACu,kSamples,kSamples+640);
        const auto kind=mode==0 ? OutputStage::Mode::Stereo : mode==1 ? OutputStage::Mode::Opposite : OutputStage::Mode::Matrix;
        OutputStage::Buffer left{},right{},surround{},bl{},br{};
        output.begin(kind,left,right,surround); dsp.data.fill(0);
        if(mode!=2) dsp.run(mode==0 ? 0x066A : 0x020B,{kSamples});
        for(unsigned i=0;i<160;++i) {
            left[i]+=100;right[i]-=200;surround[i]=s32(frame*13+i);
            dsp.put32(i*2,dsp.get32(i*2)+100);dsp.put32(0x140+i*2,dsp.get32(0x140+i*2)-200);
            dsp.put32(0x280+i*2,surround[i]);
            bl[i]=s32((frame+1)*1000+i);br[i]=-bl[i];
            dsp.put32(0x7C0+i*2,bl[i]);dsp.put32(0x900+i*2,br[i]);
        }
        bool ended=false;
        for(u32 at=kArgs;at<kArgs+384;) {
            const auto command=o.memory.peek16(at);at+=2;
            require(command<words.size(),"transition builder command changed");
            if(command==0x10) dsp.run(0x0DA1,{o.memory.peek32(at),o.memory.peek32(at+4)});
            if(command==15) { ended=true;break; } at+=words[command]*2;
        }
        require(ended,"transition list did not end");
        output.finish(kind,left,right,surround,bl,br);
        for(unsigned i=0;i<160;++i) {
            if(left[i]!=dsp.get32(i*2) || right[i]!=dsp.get32(0x140+i*2)) {
                std::fprintf(stderr,"output transition frame=%u mode=%u sample=%u left=%d expected=%d right=%d expected=%d\n",
                    frame,mode,i,left[i],dsp.get32(i*2),right[i],dsp.get32(0x140+i*2));
                throw std::runtime_error("mode change lost retained matrix history");
            }
            o.memory.write32(kSamples+i*4,u32(surround[i]));
        }
        if(frame%3==0) { OutputStage restored;restored.restore(output.history());output=restored; }
        o.call(0x80384E14u);
    }
    dsp.transfer={};
    std::printf("PASS original/native output mode transitions: %zu blocks, history restore every third block\n",modes.size());
}

void native_effect(Oracle& o) {
    using shadow::audio::Reverb;
    unsigned frames = 0, continuation_frames = 0;
    const std::array<Parameters, 6> configs{{kLive, {{0, 0.5f, 0.01f, 0, 0, 0}},
        {{1, 1, 10, 1, 0.1f, 1}}, {{0.25f, 0, 2, 0.05f, 0.01f, 0.75f}},
        {{0.8125f, 0.375f, 0.231f, 0.061f, 0, 0.625f}},
        {{0.171f, 0.924f, 3.741f, 0.736f, 0.007f, 0.196f}}}};
    for (const auto& p : configs) for (u32 fp_mode : {0u, 4u}) {
        o.reset(); require(o.initialize(p), "native oracle initialization failed");
        Reverb effect;
        require(effect.configure({p[0], p[1], p[2], p[3], p[4], p[5]}), "native configure rejected valid parameters");
        std::array<std::array<s32, 160>, 3> samples{};
        u32 stream = 0xC179A24Du;
        const auto mode = fp_mode ? shadow::math::SubnormalMode::Flush : shadow::math::SubnormalMode::Preserve;
        for (unsigned frame = 0; frame < 64; ++frame) {
            for (unsigned channel = 0; channel < 3; ++channel) {
                o.memory.write32(kArgs + channel * 4, kSamples + channel * 640);
                for (unsigned i = 0; i < 160; ++i) {
                    stream ^= stream << 13; stream ^= stream >> 17; stream ^= stream << 5;
                    // Alternate ordinary audio amplitude and full signed-range
                    // samples after feedback has become nonzero as well.
                    s32 value = frame < 8 ? ((frame & 1) ? s32(stream) :
                        s32((frame * 173 + i * 917 + channel * 1103) % 65536) - 32768) : 0;
                    if (frame == 0 && i == 0) value = INT32_MAX;
                    if (frame == 0 && i == 1) value = INT32_MIN;
                    samples[channel][i] = value;
                    o.memory.write32(kSamples + channel * 640 + i * 4, u32(value));
                }
            }
            o.call(kCallback, kArgs, kState, 0, {}, fp_mode);
            // Variable native block lengths must preserve the continuous signal.
            effect.process({nullptr, nullptr, nullptr}, 0, mode);
            effect.process({samples[0].data(), samples[1].data(), samples[2].data()}, 7, mode);
            effect.process({samples[0].data() + 7, samples[1].data() + 7, samples[2].data() + 7}, 153, mode);
            for (unsigned channel = 0; channel < 3; ++channel) for (unsigned i = 0; i < 160; ++i) {
                const u32 expected = o.memory.peek32(kSamples + channel * 640 + i * 4);
                if (u32(samples[channel][i]) != expected) {
                    std::fprintf(stderr, "native frame=%u fp=%u channel=%u sample=%u expected=%08X actual=%08X\n",
                                 frame, fp_mode, channel, i, expected, u32(samples[channel][i]));
                    throw std::runtime_error("native reverb output differs from original instructions");
                }
            }
            compare_native_history(o, effect, frame, fp_mode);
            if (frame == 23) {
                const auto checkpoint = effect.history();
                Reverb restored;
                require(restored.restore(checkpoint), "native effect history was not restorable");
                effect = restored;
            }
            if (frame > 23) ++continuation_frames;
            ++frames;
        }
        o.release();
    }
    std::printf("PASS owned native reverb: %u frames, %u restored continuations, split blocks and complete delay state\n",
                frames, continuation_frames);
}

void native_history_edges(Oracle& o) {
    using shadow::audio::Reverb;
    const std::array<u32, 8> tiny{{0, 0x80000000u, 1, 0x80000001u,
        0x007FFFFFu, 0x00800000u, 0x00800001u, 0x80800000u}};
    const auto value = [&](unsigned i) { float result; const u32 word = tiny[i % tiny.size()];
        std::memcpy(&result, &word, sizeof(result)); return result; };
    unsigned frames = 0;
    for (u32 fp_mode : {0u, 4u}) {
        o.reset(); require(o.initialize(), "tiny-history original init failed");
        Reverb effect;
        auto saved = effect.history();
        std::size_t offset = 0;
        for (unsigned channel = 0; channel < 3; ++channel) {
            for (unsigned line = 0; line < 6; ++line) {
                const unsigned index = channel * 6 + line;
                const u32 descriptor = kState + channel * 60 +
                    (line < 3 ? 0xB4 + line * 20 : (line - 3) * 20);
                const u32 buffer = o.memory.peek32(descriptor + 12);
                const u32 length = o.memory.peek32(descriptor + 8) / 4;
                const u32 cursor = o.memory.peek32(descriptor + 4) / 4;
                saved.positions[index] = std::uint16_t((index * 31) % (length - 2));
                saved.delayed[index] = value(index);
                o.memory.write32(descriptor + 16, bits(saved.delayed[index]));
                for (u32 i = 0; i < length - 2; ++i) {
                    const float sample = value(i + index);
                    saved.samples[offset + (saved.positions[index] + i) % (length - 2)] = sample;
                    o.memory.write32(buffer + (cursor + i) % length * 4, bits(sample));
                }
                offset += length - 2;
            }
            saved.low_pass[channel] = value(channel + 5);
            saved.previous_input[channel] = value(channel + 4);
            o.memory.write32(kState + 0x190 + channel * 4, bits(saved.low_pass[channel]));
            o.memory.write32(o.memory.peek32(kState + 0x1B8 + channel * 4), bits(saved.previous_input[channel]));
        }
        require(effect.restore(saved), "finite tiny history rejected");
        compare_native_history(o, effect, 0, fp_mode);
        const auto mode = fp_mode ? shadow::math::SubnormalMode::Flush : shadow::math::SubnormalMode::Preserve;
        for (unsigned frame = 0; frame < 16; ++frame) {
            std::array<std::array<s32, 160>, 3> samples{};
            for (unsigned channel = 0; channel < 3; ++channel) {
                o.memory.write32(kArgs + channel * 4, kSamples + channel * 640);
                for (unsigned i = 0; i < 160; ++i) o.memory.write32(kSamples + channel * 640 + i * 4, 0);
            }
            o.call(kCallback, kArgs, kState, 0, {}, fp_mode);
            effect.process({samples[0].data(), samples[1].data(), samples[2].data()}, 160, mode);
            for (unsigned channel = 0; channel < 3; ++channel) for (unsigned i = 0; i < 160; ++i)
                require(u32(samples[channel][i]) == o.memory.peek32(kSamples + channel * 640 + i * 4),
                        "tiny-history PCM differs");
            compare_native_history(o, effect, frame, fp_mode);
            ++frames;
        }
        o.release();
    }
    std::printf("PASS native reverb tiny history: %u frames, signed zero and subnormal boundaries, both modes\n", frames);
}

void effect_stage_rejections() {
    using Stage=shadow::audio::EffectStage;
    Stage owner,reference;
    Stage::Input input;
    for(unsigned c=0;c<3;++c) for(unsigned i=0;i<160;++i) {
        input.main[c][i]=s32(i*13+c*31)-2000;
        input.room[c][i]=s32(i*79+c*431)-6000;
        input.spatial[c][i]=s32(i*19+c*47)-1500;
    }
    for(unsigned frame=0;frame<8;++frame) {
        require(owner.control({frame%2 ? Stage::Mode::Matrix : Stage::Mode::Stereo,frame%3!=1}),
                "effect-stage warmup control rejected");
        owner.render(input);
    }
    require(owner.control({Stage::Mode::Opposite,true}),"pending live control rejected");
    const auto saved=owner.history();
    require(reference.restore(saved),"effect-stage control history rejected");
    unsigned rejected=0;
    // Direct (a list without a fold) is a valid output mode; anything outside the four is not.
    const auto invalid=static_cast<Stage::Mode>(9);
    require(!owner.control({invalid,false}),"unsupported output mode accepted");++rejected;
    auto parameters=saved.effect.parameters;parameters.level=std::numeric_limits<float>::quiet_NaN();
    require(!owner.reset(parameters,{Stage::Mode::Stereo,false}),"invalid reset parameters accepted");++rejected;
    require(!owner.reset({}, {invalid,false}),"invalid reset control accepted");++rejected;
    for(unsigned attack=0;attack<5;++attack) {
        auto bad=saved;
        if(attack==0)bad.next=3;
        if(attack==1)bad.scheduled.mode=invalid;
        if(attack==2)bad.live.mode=invalid;
        if(attack==3)bad.effect.samples[0]=std::numeric_limits<float>::infinity();
        if(attack==4)bad.effect.positions[0]=UINT16_MAX;
        require(!owner.restore(bad),"invalid effect-stage history accepted");++rejected;
    }
    for(unsigned frame=0;frame<32;++frame) {
        require(owner.render(input)==reference.render(input),"rejected effect-stage operation changed future output");
        const Stage::Control control{frame%3 ? Stage::Mode::Matrix : Stage::Mode::Opposite,frame%5<3};
        require(owner.control(control) && reference.control(control),"continuation control rejected");
    }
    Stage fresh;
    require(owner.reset({}, {Stage::Mode::Stereo,true}),"valid effect-stage reset rejected");
    for(unsigned frame=0;frame<32;++frame)
        require(owner.render(input)==fresh.render(input),"effect-stage reset retained old signal/control history");
    std::printf("PASS native effect stage: %u rejected operations preserve continuation; reset matches fresh owner\n",rejected);
}

// ReverbHiInit / ReverbHiShutdown served natively (docs/GC_REMOVAL_PLAN.md step 2) against the original bodies on the
// same memory: for every parameter at its bounds, one ulp outside them, signed zeros, infinities and NaN, the return
// value and the bypass byte are the original's, and the owner writes no other byte of the object (the descriptors and
// delay lines the original builds are retired state: no surviving reader, guest watch). A NaN parameter passes the
// original's ordered compares and then sizes a heap request from the NaN, so its result is the game heap's, not a
// contract: the owner refuses it (r3 0, unbound), and both callers are shown to discard r3.
void owned_init_shutdown(Oracle& o) {
    // The two call sites overwrite r3 right after the call: lwz r3, 0x5FAC(r13).
    for (u32 after_call : {0x8043F9E4u, 0x8043F6D0u})
        require(o.memory.peek32(after_call) == 0x806D5FACu, "a caller of ReverbHiInit/Shutdown reads its r3");
    using shadow::audio::EffectStage;
    unsigned cases = 0, accepted = 0, nan_cases = 0;
    const auto run = [&](const Parameters& parameters) {
        const bool has_nan = std::any_of(parameters.begin(), parameters.end(), [](float v) { return v != v; });
        u32 original_r3 = 0u;
        u8 original_bypass = 0u;
        if (!has_nan) {
        // Original.
        o.reset();
        o.memory.write8(kState + gekko::reverb::kBypassOffset, 0xABu);
        const bool original_ok = o.initialize(parameters);
        original_r3 = o.cpu.gpr[3];
        original_bypass = o.memory.peek8(kState + gekko::reverb::kBypassOffset);
        if (original_ok) { o.call(0x80388554u); require(o.cpu.gpr[3] == 1u, "original shutdown did not return 1"); }
        }
        // Native on a fresh object with the same parameters and bypass byte.
        o.reset();
        for (unsigned i = 0; i < parameters.size(); ++i) o.memory.write32(kState + 0x1C8 + i * 4, bits(parameters[i]));
        o.memory.write8(kState + gekko::reverb::kBypassOffset, 0xABu);
        std::vector<u8> before(0x200), after(0x200);
        o.memory.read_block(kState, before.data(), before.size());
        EffectStage stage;
        require(stage.reset({}, {EffectStage::Mode::Stereo, true}), "effect stage reset failed");
        gekko::reverb::Binding binding;
        o.cpu.gpr[3] = kState;
        require(gekko::reverb::serve_init(o.cpu, binding, stage), "native init refused a readable object");
        const u32 native_r3 = o.cpu.gpr[3];
        o.memory.read_block(kState, after.data(), after.size());
        require(has_nan || native_r3 == original_r3, "native init return value differs from the original");
        require(after[gekko::reverb::kBypassOffset] == 0u && (has_nan || original_bypass == 0u),
                "native init bypass byte differs from the original");
        after[gekko::reverb::kBypassOffset] = before[gekko::reverb::kBypassOffset];
        require(after == before, "native init wrote a byte the original's game-visible contract does not");
        require(binding.configured == (native_r3 == 1u) && binding.context == kState,
                "native binding does not follow its return value");
        if (has_nan) require(native_r3 == 0u, "a NaN parameter was accepted natively");
        o.cpu.gpr[3] = kState;
        gekko::reverb::serve_shutdown(o.cpu, binding);
        require(o.cpu.gpr[3] == 1u && !binding.configured, "native shutdown result differs");
        ++cases; if (native_r3 == 1u) ++accepted;
        if (has_nan) ++nan_cases;
    };
    run(kLive);
    const std::array<std::array<float, 2>, 6> bounds{{{0, 1}, {0, 1}, {0.01f, 10}, {0, 1}, {0, 0.1f}, {0, 1}}};
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    for (unsigned field = 0; field < 6; ++field) {
        const float low = bounds[field][0], high = bounds[field][1];
        for (float value : {low, std::nextafter(low, -inf), high, std::nextafter(high, inf), -0.0f, 0.0f,
                            (low + high) / 2, inf, -inf, nan}) {
            Parameters p = kLive; p[field] = value; run(p);
        }
    }
    require(accepted > 0 && accepted < cases && nan_cases == 6, "the sweep did not reach both verdicts and NaN");
    std::printf("PASS owned init/shutdown: %u parameter sets (%u accepted; %u NaN refused, callers discard r3), return value and bypass "
                "byte equal the original's, no other byte written\n", cases, accepted, nan_cases);
}

void native_rejections() {
    using shadow::audio::Reverb;
    Reverb effect;
    std::array<std::array<s32, 160>, 3> samples{};
    samples[0][0] = 1234567; samples[1][1] = -7654321; samples[2][159] = 31337;
    effect.process({samples[0].data(), samples[1].data(), samples[2].data()}, 160);
    const auto before = effect.history();
    // Compare only against a byte copy of THIS instance, preserving its padding.
    std::array<unsigned char, sizeof(Reverb::History)> image{};
    std::memcpy(image.data(), &effect.history(), image.size());
    const auto unchanged = [&] { require(std::memcmp(image.data(), &effect.history(), image.size()) == 0,
                                       "rejected operation changed effect history"); };
    unsigned rejected = 0;
    for (unsigned field = 0; field < 6; ++field) for (float invalid : {
        -1.0f, 11.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto p = before.parameters;
        std::array<float*, 6> fields{{&p.color, &p.level, &p.time, &p.damping, &p.pre_delay, &p.crossfeed}};
        *fields[field] = invalid;
        require(!effect.configure(p), "invalid native configuration accepted"); unchanged(); ++rejected;
        auto bad = before; bad.parameters = p;
        require(!effect.restore(bad), "invalid history parameters accepted"); unchanged(); ++rejected;
    }
    auto bad = before;
    bad.positions.back() = UINT16_MAX;
    require(!effect.restore(bad), "invalid ring position accepted"); unchanged(); ++rejected;
    for (unsigned group = 0; group < 4; ++group) {
        bad = before;
        std::array<float*, 4> fields{{&bad.samples.back(), &bad.delayed.back(),
            &bad.low_pass.back(), &bad.previous_input.back()}};
        *fields[group] = std::numeric_limits<float>::infinity();
        require(!effect.restore(bad), "nonfinite history accepted"); unchanged(); ++rejected;
    }
    // Rejection must preserve coefficients as well as serialized history.
    Reverb control; require(control.restore(before), "control restore failed");
    std::array<std::array<s32, 160>, 3> expected{};
    samples = expected;
    for (unsigned frame = 0; frame < 24; ++frame) {
        effect.process({samples[0].data(), samples[1].data(), samples[2].data()}, 160);
        control.process({expected[0].data(), expected[1].data(), expected[2].data()}, 160);
        require(samples == expected, "rejection changed future output");
        samples = {}; expected = {};
    }
    effect.reset();
    const auto zero = [](const auto& values) { return std::all_of(values.begin(), values.end(),
        [](auto v) { return v == 0; }); };
    const auto& reset = effect.history();
    require(zero(reset.samples) && zero(reset.positions) && zero(reset.delayed) && zero(reset.low_pass) &&
            zero(reset.previous_input), "reset retained signal history");
    std::printf("PASS native reverb rejection: %u invalid operations, unchanged continuation and reset\n", rejected);
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: axfx_reverb_contract_tests main.dol");
        Oracle original(argv[1]);
        lifecycle(original);
        compare_callback(original);
        predelay(original);
        game_effect_controls(original);
        game_bank_controls(original);
        auxiliary_slots(original);
        audio_reference::OutputWords output_words(argv[1]);
        composed_effect_output(original, output_words);
        effect_control_transitions(original, output_words);
        output_mode_transitions(original, output_words);
        native_effect(original);
        native_history_edges(original);
        native_rejections();
        owned_init_shutdown(original);
        effect_stage_rejections();
        std::printf("PASS reverb contract: %llu original instructions\n",
                    static_cast<unsigned long long>(original.instructions));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL reverb contract: %s\n", error.what());
        return 1;
    }
}
