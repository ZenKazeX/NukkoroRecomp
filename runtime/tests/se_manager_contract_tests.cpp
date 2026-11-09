// The game's SE manager, native against the original instructions (gekko/native_se_manager.h).
// liveness-roots: reference-interpreter-only
//
// Both sides load the same MEM1 image of the original game in a stage (a Dolphin dump: the real manager, channels,
// tables, sound object, GCAX context, listener actors and camera). A seeded random sequence of requests (PlaySimple,
// PlayFlags, Play3D with and without owners, PlayRing), frames (FrameSE), request-gate calls and state changes (pause
// mode, output mode, ring cooldown, gate bits) runs the original functions in the interpreter on one side and the
// native owners on the other. After every call r3 and every byte of MEM1 must match (the original's stack window is
// restored first). The GCAX rings are emptied before each call so every message is compared, not dropped.
//
// Also proven: no row of any shipped AudioData*.bin uses the special ring (row flags bit 1), whose channels above
// 0x26 would need GCAX ports 4..7 (not native yet).

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_gcax_midi.h"
#include "gekko/native_se_manager.h"
#include "gekko/native_sound_tables.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kMem1 = 0x80000000u, kMem1Size = 0x01800000u;
constexpr u32 kStackLow = 0x817F0000u, kStack = 0x817FC000u;  // the original's stack window (restored after calls)
constexpr u32 kVectors = 0x817FD000u;                        // request positions
constexpr u32 kReturn = 0x817FFF00u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kManager = se_manager::kManager;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}

struct Side {
    GuestMemory memory;
    Cpu cpu{memory};
    se_manager::Report report;
    gcax_midi::Report midi;
    sound_tables::Report tables;
    se_manager::Services services{report, midi, tables};
    explicit Side(const std::vector<u8>& mem1) { memory.write_block(kMem1, mem1.data(), mem1.size()); }
    void image(std::vector<u8>& out) const {
        out.resize(kMem1Size);
        memory.peek_block(kMem1, out.data(), out.size());
    }
};

struct Rng {
    u64 state;
    u32 next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(state >> 33);
    }
    u32 below(u32 n) { return next() % n; }
    bool chance(u32 percent) { return below(100) < percent; }
};

struct Harness {
    Side original, native;
    std::vector<u8> stack_window, image_a, image_b;
    u64 calls = 0, instructions = 0, op = 0;
    explicit Harness(const std::vector<u8>& mem1) : original(mem1), native(mem1) {
        stack_window.resize(kStack + 0x100u - kStackLow);
        original.memory.read_block(kStackLow, stack_window.data(), stack_window.size());
    }

    void both(const std::function<void(GuestMemory&)>& f) {
        f(original.memory);
        f(native.memory);
    }

    void empty_rings() {
        const u32 context = original.memory.peek32(gcax_midi::kContextPointer);
        if (context == 0u) return;
        for (u32 p = 0; p < gcax_midi::kPortCount; ++p) {
            const u32 port = context + gcax_midi::kPorts + p * gcax_midi::kPortBytes;
            const u32 write = static_cast<u32>((op * 7u + p * 131u) % gcax_midi::kRingWords);
            both([&](GuestMemory& m) {
                m.write32(port + gcax_midi::kCount, 0u);
                m.write32(port + gcax_midi::kWrite, write);
            });
        }
    }

    void compare(const std::string& what) {
        original.image(image_a);
        native.image(image_b);
        const u8* a = image_a.data();
        const u8* b = image_b.data();
        if (std::memcmp(a, b, kMem1Size) == 0) return;
        u32 first = 0;
        while (a[first] == b[first]) ++first;
        char text[160];
        std::snprintf(text, sizeof(text), " (op %llu): MEM1 differs first at 0x%08X: original %02X native %02X",
                      static_cast<unsigned long long>(op), kMem1 + first, a[first], b[first]);
        throw std::runtime_error(what + text);
    }

    // Runs `entry` with r3..r9 in the interpreter and as the native owner, then compares r3 and MEM1.
    void call(u32 entry, std::vector<u32> args, const std::function<void(Cpu&, se_manager::Services&)>& owner,
              const std::string& what) {
        empty_rings();
        Cpu& cpu = original.cpu;
        cpu.reset();
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        cpu.fpscr = 0;
        for (std::size_t i = 0; i < args.size(); ++i) cpu.gpr[3 + i] = args[i];
        u64 steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 20000000u) cpu.run_steps(1);
        require(cpu.pc == kReturn && !cpu.stopped(), "original " + what + " did not return");
        instructions += steps;
        original.memory.write_block(kStackLow, stack_window.data(), stack_window.size());
        Cpu& n = native.cpu;
        n.reset();
        n.fpscr = 0;
        for (std::size_t i = 0; i < args.size(); ++i) n.gpr[3 + i] = args[i];
        owner(n, native.services);
        const bool returns = entry != se_manager::kFrame && entry != se_manager::kGateClear &&
                             entry != se_manager::kGateSet && entry != se_manager::kGateClearIfZero &&
                             entry != se_manager::kGateSetIfZero;
        if (returns && cpu.gpr[3] != n.gpr[3])
            throw std::runtime_error(what + " (op " + std::to_string(op) + "): result " + std::to_string(cpu.gpr[3]) +
                                     " vs native " + std::to_string(n.gpr[3]));
        compare(what);
        ++calls;
    }
};

std::vector<u32> table_ids(GuestMemory& m) {
    std::vector<u32> ids;
    const u32 tables = m.peek32(kManager + 8u);
    require(tables != 0u, "the image has no sound tables");
    for (u32 slot = 0; slot < 8u; ++slot) {
        const u32 count = m.peek32(tables + slot * 16u), rows = m.peek32(tables + slot * 16u + 4u);
        for (u32 r = 0; r < count && r < 4096u; ++r) {
            const u32 id = m.peek32(rows + r * sound_tables::kRowBytes + sound_tables::kRowId) & 0xFFFFu;
            if (id != 0x7160u) ids.push_back(id);  // the WARPHOLE erratum is sound_table_contract_tests' business
        }
    }
    return ids;
}

void run(Harness& h, u64 operations) {
    GuestMemory& m = h.original.memory;
    require(m.peek32(se_manager::kManagerPointer) == kManager, "GetSoundManager's singleton is not initialised");
    require(m.peek32(kManager) != 0u && m.peek32(kManager + 4u) != 0u, "the image has no sound object / channels");
    std::vector<u32> ids = table_ids(m);
    require(ids.size() > 50u, "too few table rows");
    for (u32 extra : {0xFFFFu, 0x1FFFu, 0xE00Bu, 0xE000u, 0xE004u, 0xE009u, 0xE00Au, 0x0123u}) ids.push_back(extra);
    // Position bases: every positional channel in the image, else the origin.
    std::vector<std::array<float, 3>> bases;
    const u32 channels = m.peek32(kManager + 4u);
    for (u32 i = 0; i < se_manager::kChannels; ++i) {
        const u32 c = channels + i * se_manager::kChannelBytes;
        if (m.peek32(c + 0x30u) != 0u && (m.peek32(c + 0x14u) & 0x10u)) {
            std::array<float, 3> v;
            for (u32 k = 0; k < 3; ++k) {
                const u32 bits = m.peek32(c + 0x1Cu + 4u * k);
                std::memcpy(&v[k], &bits, 4);
            }
            bases.push_back(v);
        }
    }
    if (bases.empty()) bases.push_back({0.0f, 0.0f, 0.0f});
    std::printf("image: %zu table ids, %zu positional channels as position bases\n", ids.size(), bases.size());

    Rng rng{0x5E5E5E5Eull};
    const u32 owners[] = {0u, 0x80500000u, 0x80500010u, 0x80500020u, 0x80500030u};
    const u32 flags3d[] = {1u, 2u, 3u, 1u | 4u, 3u | 0x20u, 1u | 0x20u, 3u | 4u, 0x41u, 0u};
    const u32 flags_play[] = {1u, 0x41u, 0x40u, 5u, 0x21u, 0x43u, 0u};
    const s32 durations[] = {0, 2, 3, 5, 0, 30, -1, 300};
    auto position = [&](u32 slot) -> u32 {
        if (rng.chance(10)) return 0u;
        const auto& base = bases[rng.below(static_cast<u32>(bases.size()))];
        const float scale = rng.chance(50) ? 150.0f : (rng.chance(50) ? 600.0f : 2500.0f);
        const u32 address = kVectors + slot * 12u;
        for (u32 k = 0; k < 3; ++k) {
            const float offset = (static_cast<float>(rng.below(20001)) / 10000.0f - 1.0f) * scale;
            const float value = base[k] + (k == 1 ? offset * 0.25f : offset);
            u32 bits;
            std::memcpy(&bits, &value, 4);
            h.both([&](GuestMemory& g) { g.write32(address + 4u * k, bits); });
        }
        return address;
    };
    // Ids whose rows cull by distance (row flags 0x10), and the last id each owner played (keep-alive).
    // Lethal Highway's tables have no such row (Ark1, LastBKBoss and LastEggBoss ship them) and no shipped row uses
    // flags 0x4 / 0x8, so some rows get them on both sides: distance cull, no pan tracking, request flags from the row.
    std::vector<u32> culling;
    {
        const u32 tables = m.peek32(kManager + 8u);
        u32 n = 0;
        for (u32 slot = 0; slot < 8u; ++slot) {
            const u32 count = m.peek32(tables + slot * 16u), rows = m.peek32(tables + slot * 16u + 4u);
            for (u32 r = 0; r < count && r < 4096u; ++r, ++n) {
                const u32 flags_at = rows + r * sound_tables::kRowBytes + 0xEu;
                const u32 add = (n % 5u == 0u ? 0x10u : 0u) | (n % 11u == 0u ? 0x08u : 0u) | (n % 13u == 0u ? 0x04u : 0u);
                h.both([&](GuestMemory& g) { g.write8(flags_at, static_cast<u8>(g.peek8(flags_at) | add)); });
            }
        }
    }
    {
        const u32 tables = m.peek32(kManager + 8u);
        for (u32 slot = 0; slot < 8u; ++slot) {
            const u32 count = m.peek32(tables + slot * 16u), rows = m.peek32(tables + slot * 16u + 4u);
            for (u32 r = 0; r < count && r < 4096u; ++r) {
                const u32 row = rows + r * sound_tables::kRowBytes;
                const u32 sound = m.peek32(row + sound_tables::kRowId) & 0xFFFFu;
                if ((m.peek8(row + 0xEu) & 0x10u) && sound != 0x7160u) culling.push_back(sound);
            }
        }
    }
    std::printf("image: %zu distance-culled ids\n", culling.size());
    u32 last_id[5] = {};
    auto id = [&]() {
        if (!culling.empty() && rng.chance(25)) return culling[rng.below(static_cast<u32>(culling.size()))];
        return ids[rng.below(static_cast<u32>(ids.size()))];
    };
    auto owned_id = [&](u32 owner_slot) {
        if (owner_slot != 0u && last_id[owner_slot] != 0u && rng.chance(60)) return last_id[owner_slot];
        const u32 sound = id();
        last_id[owner_slot] = sound;
        return sound;
    };

    for (h.op = 0; h.op < operations; ++h.op) {
        const u32 roll = rng.below(100);
        if (roll < 34) {
            const u32 slot = rng.below(5), owner = owners[slot], pos = position(h.op % 64u);
            const u32 fl = rng.chance(5) ? rng.below(0x10000u) : flags3d[rng.below(9)];
            const s32 vol = rng.chance(60) ? 0 : static_cast<s8>(rng.below(256));
            const s32 dur = rng.chance(5) ? static_cast<s16>(rng.below(0x10000u)) : durations[rng.below(8)];
            const u32 sound = owned_id(slot);
            h.call(se_manager::kPlay3D, {kManager, sound, pos, owner, fl, static_cast<u32>(vol), static_cast<u32>(dur)},
                   [](Cpu& c, se_manager::Services& s) { se_manager::serve_play_3d(c, s); }, "Play3D");
        } else if (roll < 56) {
            const u32 slot = rng.below(5), owner = owners[slot], pos = position(h.op % 64u);
            const u32 fl = rng.chance(5) ? rng.below(0x10000u) : flags_play[rng.below(7)];
            const u32 sound = owned_id(slot);
            h.call(se_manager::kPlayFlags, {kManager, sound, pos, owner, fl},
                   [](Cpu& c, se_manager::Services& s) { se_manager::serve_play_flags(c, s); }, "PlayFlags");
        } else if (roll < 68) {
            const s32 vol = rng.chance(80) ? 0 : static_cast<s8>(rng.below(256));
            const u32 owner = rng.chance(80) ? 0u : owners[rng.below(5)];
            const u32 sound = id();
            h.call(se_manager::kPlaySimple, {kManager, sound, static_cast<u32>(vol), owner},
                   [](Cpu& c, se_manager::Services& s) { se_manager::serve_play_simple(c, s); }, "PlaySimple");
        } else if (roll < 72) {
            h.call(se_manager::kPlayRing, {kManager},
                   [](Cpu& c, se_manager::Services& s) { se_manager::serve_play_ring(c, s); }, "PlayRing");
        } else if (roll < 94) {
            const u32 frames = 1u + rng.below(rng.chance(10) ? 150u : 4u);
            for (u32 f = 0; f < frames; ++f)
                h.call(se_manager::kFrame, {kManager},
                       [](Cpu& c, se_manager::Services& s) { se_manager::serve_frame(c, s); }, "FrameSE");
        } else if (roll < 97) {
            const u32 entries[] = {se_manager::kGateClearIfZero, se_manager::kGateSetIfZero, se_manager::kGateClear,
                                   se_manager::kGateSet};
            const u32 entry = entries[rng.below(4)];
            const u32 x = rng.chance(50) ? 0u : 1u;
            h.call(entry, {kManager, x}, [entry](Cpu& c, se_manager::Services&) { se_manager::serve_gate(c, entry); },
                   "gate");
        } else {
            // State the translated control functions own: pause mode, pause gate, output mode, ring cooldown.
            const u32 what = rng.below(5);
            const u32 value = rng.below(100);
            h.both([&](GuestMemory& g) {
                if (what == 0) g.write32(kManager + 0x10u, value < 70 ? 0u : (value < 95 ? 1u : 2u));
                if (what == 1) g.write32(kManager + 0x14u, (g.peek32(kManager + 0x14u) & ~1u) | (value < 60 ? 0u : 1u));
                if (what == 2) g.write32(gcax_midi::kOutputConfig + 0x78u, value < 40 ? 0u : (value < 70 ? 1u : 2u));
                if (what == 3) g.write32(kManager + 0x18u, value < 50 ? 0u : (value < 75 ? 0x3CA3D70Au : 0xBC23D70Au));
            });
            if (what == 4) {
                // Every normal-ring channel busy at the top priority for one request: it finds no channel. The
                // channels are restored afterwards (on both sides, after the comparison).
                const u32 block = m.peek32(kManager + 4u), bytes = 0x20u * se_manager::kChannelBytes;
                std::vector<u8> saved(bytes);
                m.read_block(block, saved.data(), bytes);
                h.both([&](GuestMemory& g) {
                    for (u32 i = 0; i < 0x20u; ++i) {
                        const u32 c = block + i * se_manager::kChannelBytes;
                        if (g.peek16(c + 0x12u) == 0u) g.write16(c + 0x12u, 0x0001u);
                        g.write8(c + 3u, 0xFFu);
                    }
                });
                const u32 sound = id();
                h.call(se_manager::kPlaySimple, {kManager, sound, 0u, 0u},
                       [](Cpu& c, se_manager::Services& s) { se_manager::serve_play_simple(c, s); }, "PlaySimple (busy)");
                h.both([&](GuestMemory& g) { g.write_block(block, saved.data(), bytes); });
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        require(argc >= 4, "usage: se_manager_contract_tests <main.dol> <game files/sound folder> <MEM1 image> [ops]");
        const std::vector<u8> dol = read_file(argv[1]);
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        // SHA-256: fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        // No shipped table uses the special ring.
        u64 rows = 0, files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(argv[2])) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("AudioData", 0) != 0) continue;
            const std::vector<u8> file = read_file(entry.path());
            require(file.size() >= 0x28u, "short table " + name);
            const u32 count = load_be32(file.data() + 0x24);
            require(0x28u + u64(count) * 0x18u <= file.size(), "rows past the file " + name);
            for (u32 r = 0; r < count; ++r)
                require((file[0x28 + r * 0x18 + 0xE] & 2u) == 0u, "special-ring row in " + name);
            rows += count;
            ++files;
        }
        require(files == 23u, "expected the 23 shipped AudioData tables");
        std::vector<u8> mem1 = read_file(argv[3]);
        require(mem1.size() == kMem1Size, "the MEM1 image must be 24 MiB");
        // main.dol's text over the image (the running game patched a few words of it).
        u64 patched = 0;
        for (unsigned i = 0; i < 7; ++i) {
            const u32 offset = load_be32(dol.data() + i * 4), address = load_be32(dol.data() + 0x48 + i * 4),
                      size = load_be32(dol.data() + 0x90 + i * 4);
            for (u32 b = 0; b < size; ++b) {
                u8& byte = mem1[address - kMem1 + b];
                if (byte != dol[offset + b]) ++patched;
                byte = dol[offset + b];
            }
        }
        require(patched < 64u, "the image's text is not main.dol's");
        const u64 operations = argc >= 5 ? std::stoull(argv[4]) : 2500u;
        Harness h(mem1);
        run(h, operations);
        const se_manager::Report& r = h.native.report;
        std::printf("PASS native SE manager: %llu calls identical to %llu original instructions (r3 and all of MEM1) on "
                    "a real stage image; native: %llu requests, %llu started, %llu kept alive, %llu gated, %llu culled, "
                    "%llu without a channel, %llu rings, %llu frames, %llu channel updates, %llu expired, %llu "
                    "retriggered; MIDI pushed %llu, dropped %llu, refused %llu; no special-ring row in %llu rows of %llu "
                    "tables\n",
                    static_cast<unsigned long long>(h.calls), static_cast<unsigned long long>(h.instructions),
                    static_cast<unsigned long long>(r.requests), static_cast<unsigned long long>(r.started),
                    static_cast<unsigned long long>(r.kept_alive), static_cast<unsigned long long>(r.gated),
                    static_cast<unsigned long long>(r.culled), static_cast<unsigned long long>(r.no_channel),
                    static_cast<unsigned long long>(r.rings), static_cast<unsigned long long>(r.frames),
                    static_cast<unsigned long long>(r.channel_updates), static_cast<unsigned long long>(r.expired),
                    static_cast<unsigned long long>(r.retriggered),
                    static_cast<unsigned long long>(h.native.midi.pushed),
                    static_cast<unsigned long long>(h.native.midi.dropped),
                    static_cast<unsigned long long>(h.native.midi.refused), static_cast<unsigned long long>(rows),
                    static_cast<unsigned long long>(files));
        require(r.special_ring == 0u && r.unsupported_port == 0u, "special ring reached");
        require(r.started && r.kept_alive && r.gated && r.culled && r.no_channel && r.rings && r.expired && r.retriggered,
                "a manager path was not exercised");
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
