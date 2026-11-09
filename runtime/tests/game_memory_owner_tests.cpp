// liveness-roots: reference-interpreter-only
//
// GameMemory's native owners of the C runtime's "memcpy" (0x800054F4, a byte-wise memmove) and the SEGA PRS
// expander (0x80043FE0) against the ORIGINAL words of the game's DOL in the reference interpreter
// (findings-20261019-EV/EV_REPORT.md section 7). Two machines start from the same memory and the same (random)
// register file; one interprets the original, the other runs the owner. Equal means: every GPR, CR, XER, CTR, LR,
// PC and MSR, and every byte of the arenas the calls touch. Covered:
//   * memcpy: lengths 0 .. 100000, disjoint both ways, overlapping both ways, equal pointers, 16 KiB page
//     crossings, a NULL source with n = 0;
//   * PRS: random valid streams (every token form at its limits, back-references before dst, in-place layouts);
//     every PRS member of every ONE archive on the disc decoded by the owner and compared with the host codec
//     (gekko::prs_expand), and register-exactly with the interpreter up to a byte budget (argument 2);
//   * both: with a native interrupt line held up (MSR[EE]), so Cpu::needs_dispatch() is true at every back edge:
//     the owner must hand back at exactly the back edges the translated loop polls, resume at the loop heads, and
//     end in the same state; and with an access trace installed (every access through the CPU accessors).
//
// usage: game_memory_owner_tests <sys/main.dol of the game data> [<interpreter byte budget for disc members>]

#include "gekko/cpu.h"
#include "gekko/game_memory.h"
#include "gekko/guest_memory.h"
#include "gekko/native_assets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using gekko::u8;
using gekko::u32;
using gekko::u64;

constexpr u32 kMemmove = 0x800054F4u;
constexpr u32 kPrs = 0x80043FE0u;
constexpr u32 kFinish = 0x817FF000u;  // the call's return address: never executed
constexpr u32 kStack = 0x817E0000u;

int g_failures = 0;
int g_not_inputs = 0;  // synthetic in-place layouts where the original never returns
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        if (g_failures <= 40) std::printf("FAIL: %s\n", what.c_str());
    }
}
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

// Back edges of the original loops and the heads they jump to (what the translated code polls).
struct Edge {
    u32 site;
    u32 head;
};
constexpr std::array<Edge, 2> kMemmoveEdges{{{0x80005518u, 0x8000550Cu}, {0x8000553Cu, 0x80005530u}}};
constexpr std::array<Edge, 5> kPrsEdges{{{0x80044028u, 0x80043FF0u},
                                         {0x800440D0u, 0x800440ACu},
                                         {0x800440F4u, 0x80043FF0u},
                                         {0x8004410Cu, 0x800440F8u},
                                         {0x80044110u, 0x80043FF0u}}};

struct Machine {
    gekko::GuestMemory bus;
    gekko::Cpu cpu{bus};
    std::unique_ptr<gekko::GameMemory> owner;

    Machine(const std::vector<u8>& dol, bool native) {
        const auto word = [&](std::size_t offset) { return gekko::load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        // The words the owners replace, as recovered (EV_REPORT.md 7.1).
        require(bus.read32(kMemmove) == 0x7C041840u && bus.read32(0x80005540u) == 0x4E800020u &&
                    bus.read32(kPrs) == 0x89230000u && bus.read32(0x80044110u) == 0x4BFFFEE0u,
                "unexpected memcpy / PRS instruction evidence");
        if (native) {
            owner = std::make_unique<gekko::GameMemory>(cpu);
            owner->install();
        }
    }

    // A fresh call state: the same seed gives both machines the same register file.
    void prepare(u64 seed, bool hold_interrupt, bool trace) {
        cpu.reset();
        cpu.attach_recompiled(nullptr);
        std::mt19937_64 random(seed);
        for (u32 r = 0; r < 32u; ++r) cpu.gpr[r] = static_cast<u32>(random());
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = 0x805FA780u;
        cpu.gpr[13] = 0x805EC500u;
        cpu.cr = static_cast<u32>(random());
        cpu.xer = static_cast<u32>(random()) & 0xE000007Fu;
        cpu.ctr = static_cast<u32>(random());
        cpu.set_msr(cpu.msr() | gekko::kMsrDR | gekko::kMsrIR | gekko::kMsrFP |
                    (hold_interrupt ? gekko::kMsrEE : 0u));
        if (hold_interrupt) {
            // A native interrupt line whose handler declines every delivery (pc stays): needs_dispatch() is true
            // at every back edge, so the owner must hand back at each one; nothing is ever delivered.
            cpu.set_native_interrupt_handler([](gekko::Cpu&) {});
            cpu.set_native_interrupt(true);
        }
        if (trace) {
            bus.set_access_trace([](u32, std::size_t, bool) {});
        } else {
            bus.set_access_trace({});
        }
    }

    // Runs the call to its return; counts the back edges the original takes (interpreter) and the hand-backs
    // (owner). Returns false when the step limit ran out.
    template <std::size_t N>
    bool call(u32 entry, u32 r3, u32 r4, u32 r5, const std::array<Edge, N>& edges, u64 limit, u64& back_edges) {
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.gpr[5] = r5;
        cpu.lr = kFinish;
        cpu.pc = entry;
        back_edges = 0;
        u64 steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < limit) {
            const u32 before = cpu.pc;
            cpu.run_steps(1u);
            for (const Edge& e : edges) {
                if (before == e.site && cpu.pc == e.head) ++back_edges;
            }
        }
        return cpu.pc == kFinish && !cpu.stopped();
    }
};

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

void compare(Machine& a, Machine& b, const std::vector<std::pair<u32, u32>>& arenas, const std::string& where) {
    for (u32 r = 0; r < 32u; ++r) {
        check(a.cpu.gpr[r] == b.cpu.gpr[r],
              where + ": r" + std::to_string(r) + " original " + hex(a.cpu.gpr[r]) + " owner " + hex(b.cpu.gpr[r]));
    }
    check(a.cpu.cr == b.cpu.cr, where + ": cr original " + hex(a.cpu.cr) + " owner " + hex(b.cpu.cr));
    check(a.cpu.xer == b.cpu.xer, where + ": xer original " + hex(a.cpu.xer) + " owner " + hex(b.cpu.xer));
    check(a.cpu.ctr == b.cpu.ctr, where + ": ctr original " + hex(a.cpu.ctr) + " owner " + hex(b.cpu.ctr));
    check(a.cpu.lr == b.cpu.lr && a.cpu.pc == b.cpu.pc && a.cpu.msr() == b.cpu.msr(), where + ": lr / pc / msr");
    for (const auto& [address, size] : arenas) {
        std::vector<u8> x(size), y(size);
        a.bus.peek_block(address, x.data(), size);
        b.bus.peek_block(address, y.data(), size);
        if (x != y) {
            const auto m = std::mismatch(x.begin(), x.end(), y.begin());
            check(false, where + ": memory differs first at " + hex(address + static_cast<u32>(m.first - x.begin())));
        }
    }
}

void fill(Machine& a, Machine& b, u32 address, u32 size, u64 seed) {
    std::vector<u8> bytes(size);
    std::mt19937_64 random(seed);
    for (auto& v : bytes) v = static_cast<u8>(random());
    a.bus.write_block(address, bytes.data(), size);
    b.bus.write_block(address, bytes.data(), size);
}

// ---- memcpy ---------------------------------------------------------------------------------------------------

void memcpy_cases(const std::vector<u8>& dol) {
    Machine original(dol, false), owner(dol, true);
    constexpr u32 A = 0x80800000u, kArena = 0x00100000u;
    struct Layout {
        const char* name;
        u32 dst, src;
    };
    const std::vector<u32> lengths{0u, 1u, 2u, 3u, 7u, 31u, 32u, 33u, 255u, 4096u, 16385u, 100000u};
    const std::vector<Layout> layouts{
        {"disjoint src>dst", A + 0x100u, A + 0x80005u},  {"disjoint src<dst", A + 0x80000u, A + 0x103u},
        {"overlap src=dst+1", A + 0x1000u, A + 0x1001u}, {"overlap src=dst+4096", A + 0x1000u, A + 0x2000u},
        {"overlap dst=src+1", A + 0x1001u, A + 0x1000u}, {"overlap dst=src+3", A + 0x1003u, A + 0x1000u},
        {"equal", A + 0x5000u, A + 0x5000u},             {"page-crossing", A + 0x3FF7u, A + 0x7FF3u},
    };
    int cases = 0, yields_checked = 0;
    for (int mode = 0; mode < 3; ++mode) {  // 0 plain, 1 interrupt held (hand-backs), 2 access trace
        for (const Layout& layout : layouts) {
            for (const u32 n : lengths) {
                if (mode == 1 && n > 4096u) continue;  // a hand-back per byte: keep the interpreter run short
                if (layout.src - A + n > kArena || layout.dst - A + n > kArena) continue;
                const u64 seed = 0x5EEDull * 1000003ull + static_cast<u64>(cases);
                fill(original, owner, A, kArena, seed);
                original.prepare(seed, mode == 1, mode == 2);
                owner.prepare(seed, mode == 1, mode == 2);
                u64 edges_original = 0, edges_owner = 0;
                const gekko::GameMemory::Stats before = owner.owner->stats();
                const bool a = original.call(kMemmove, layout.dst, layout.src, n, kMemmoveEdges, 50ull * n + 1000u,
                                             edges_original);
                const bool b = owner.call(kMemmove, layout.dst, layout.src, n, kMemmoveEdges, 50ull * n + 1000u,
                                          edges_owner);
                const std::string where = std::string("memcpy mode ") + std::to_string(mode) + " " + layout.name +
                                          " n=" + std::to_string(n);
                check(a && b, where + ": both return");
                compare(original, owner, {{A, kArena}}, where);
                const u64 yields = owner.owner->stats().move_yields - before.move_yields;
                check(yields == (mode == 1 ? edges_original : 0u),
                      where + ": hand-backs " + std::to_string(yields) + " vs original back edges " +
                          std::to_string(edges_original));
                if (mode == 1) ++yields_checked;
                ++cases;
            }
        }
    }
    // NULL source with n = 0 (the game makes one such call at boot): nothing is touched.
    fill(original, owner, A, kArena, 77u);
    original.prepare(77u, false, false);
    owner.prepare(77u, false, false);
    u64 e1 = 0, e2 = 0;
    check(original.call(kMemmove, A + 0x40u, 0u, 0u, kMemmoveEdges, 1000u, e1) &&
              owner.call(kMemmove, A + 0x40u, 0u, 0u, kMemmoveEdges, 1000u, e2),
          "memcpy NULL n=0 returns");
    compare(original, owner, {{A, kArena}}, "memcpy NULL n=0");
    ++cases;
    const auto& s = owner.owner->stats();
    std::printf("memcpy: %d cases (%d with hand-backs at every back edge); owner: %llu calls, %llu resumes, %llu "
                "hand-backs, %llu bulk bytes, %llu single bytes\n",
                cases, yields_checked, (unsigned long long)s.moves, (unsigned long long)s.move_resumes,
                (unsigned long long)s.move_yields, (unsigned long long)s.move_bytes_bulk,
                (unsigned long long)s.move_bytes_single);
}

// ---- PRS --------------------------------------------------------------------------------------------------------

// A PRS encoder for test streams, emitting bits in the order the decoder consumes them (a flag byte is placed
// where the decoder will be when it needs its ninth bit).
struct PrsWriter {
    std::vector<u8> out{0u};
    std::size_t flag = 0;
    int used = 0;
    void bit(int b) {
        if (used == 8) {
            flag = out.size();
            out.push_back(0u);
            used = 0;
        }
        out[flag] = static_cast<u8>(out[flag] | (b << used));
        ++used;
    }
    void literal(u8 v) {
        bit(1);
        out.push_back(v);
    }
    void short_match(u32 length, u32 distance) {  // length 2..5, distance 1..256
        bit(0);
        bit(0);
        const u32 l = length - 2u;
        bit(static_cast<int>((l >> 1) & 1u));
        bit(static_cast<int>(l & 1u));
        out.push_back(static_cast<u8>(0x100u - distance));
    }
    void long_match(u32 length, u32 distance) {  // distance 1..8192; length 3..9 inline, else 1..256 extended
        bit(0);
        bit(1);
        const u32 high = (0x2000u - distance) << 3;
        if (length >= 3u && length <= 9u) {
            const u32 w = high | (length - 2u);
            out.push_back(static_cast<u8>(w));
            out.push_back(static_cast<u8>(w >> 8));
        } else {
            out.push_back(static_cast<u8>(high));
            out.push_back(static_cast<u8>(high >> 8));
            out.push_back(static_cast<u8>(length - 1u));
        }
    }
    void end() {
        bit(0);
        bit(1);
        out.push_back(0u);
        out.push_back(0u);
    }
};

// A random valid stream: `tokens` tokens, back-references up to `before` bytes before the output start allowed.
std::vector<u8> random_stream(u64 seed, int tokens, u32 before) {
    std::mt19937_64 random(seed);
    PrsWriter w;
    u32 produced = 0;
    for (int t = 0; t < tokens; ++t) {
        const u32 reach = produced + before;
        const int kind = static_cast<int>(random() % 4u);
        if (kind == 0 || reach == 0u) {
            w.literal(static_cast<u8>(random()));
            produced += 1u;
        } else if (kind == 1) {
            const u32 d = 1u + static_cast<u32>(random() % std::min<u32>(reach, 256u));
            const u32 l = 2u + static_cast<u32>(random() % 4u);
            w.short_match(l, d);
            produced += l;
        } else {
            u32 d = 1u + static_cast<u32>(random() % std::min<u32>(reach, 8192u));
            u32 l = (random() & 1u) ? 3u + static_cast<u32>(random() % 7u) : 1u + static_cast<u32>(random() % 256u);
            if (l >= 3u && l <= 9u && (random() & 3u) == 0u) l = 1u + static_cast<u32>(random() % 2u);  // 1..2 extended
            if (d == 0x2000u && !(l >= 3u && l <= 9u)) d = 0x1FFFu;  // the zero word is the terminator
            w.long_match(l, d);
            produced += l;
        }
    }
    w.end();
    return w.out;
}

struct PrsCase {
    std::string name;
    std::vector<u8> stream;
    u32 src, dst;
};

void prs_run_pair(Machine& original, Machine& owner, const PrsCase& c, int mode, u64 seed,
                  const std::vector<std::pair<u32, u32>>& arenas, u64 limit, int& yields_checked) {
    original.prepare(seed, mode == 1, mode == 2);
    owner.prepare(seed, mode == 1, mode == 2);
    original.bus.write_block(c.src, c.stream.data(), static_cast<u32>(c.stream.size()));
    owner.bus.write_block(c.src, c.stream.data(), static_cast<u32>(c.stream.size()));
    u64 edges_original = 0, edges_owner = 0;
    const gekko::GameMemory::Stats before = owner.owner->stats();
    const bool a = original.call(kPrs, c.src, c.dst, 0u, kPrsEdges, limit, edges_original);
    const std::string where = "prs mode " + std::to_string(mode) + " " + c.name;
    if (!a) {
        // Not an input: the original itself never returns (an in-place layout whose output overwrote the source
        // before it was read - typically the terminator). Counted and reported, never compared: the owner would
        // run on unbounded just the same. Only the synthetic in-place layouts may do this.
        check(c.name.rfind("in place", 0) == 0, where + ": the original did not return within the step limit");
        ++g_not_inputs;
        std::printf("not an input (the original does not return): %s\n", where.c_str());
        return;
    }
    const bool b = owner.call(kPrs, c.src, c.dst, 0u, kPrsEdges, limit, edges_owner);
    check(b, where + ": the owner returns");
    compare(original, owner, arenas, where);
    const u64 yields = owner.owner->stats().expansion_yields - before.expansion_yields;
    check(yields == (mode == 1 ? edges_original : 0u),
          where + ": hand-backs " + std::to_string(yields) + " vs original back edges " + std::to_string(edges_original));
    if (mode == 1) ++yields_checked;
}

void prs_synthetic(const std::vector<u8>& dol) {
    Machine original(dol, false), owner(dol, true);
    constexpr u32 A = 0x80800000u, kArena = 0x00200000u;
    std::vector<PrsCase> cases;
    {
        PrsWriter w;
        w.end();
        cases.push_back({"immediate terminator", w.out, A + 0x100000u, A + 0x10000u});
    }
    {
        PrsWriter w;  // every limit: short 2..5 at distance 1 and 256, long inline 3..9, extended 1, 2, 256,
        for (int i = 0; i < 300; ++i) w.literal(static_cast<u8>(i * 7));  // distance 8192 inline
        for (u32 l = 2; l <= 5; ++l) w.short_match(l, 1u), w.short_match(l, 256u);
        for (u32 l = 3; l <= 9; ++l) w.long_match(l, 1u), w.long_match(l, 8192u);
        for (u32 l : {1u, 2u, 3u, 9u, 10u, 255u, 256u}) w.long_match(l, 300u), w.long_match(l, 8191u);
        w.end();
        cases.push_back({"limits (reaching 8192 before dst)", w.out, A + 0x100000u, A + 0x10000u});
    }
    for (int i = 0; i < 40; ++i) {
        const std::vector<u8> s = random_stream(1000u + static_cast<u64>(i), 50 + 37 * i, 8192u);
        cases.push_back({"random " + std::to_string(i), s, A + 0x100000u + static_cast<u32>(i) * 13u, A + 0x10000u});
    }
    // In place: the stream sits inside the output range, ending `margin` bytes past the output's own end (the
    // classic in-place expansion: the output chases the bytes it has consumed; at small margins it may catch up
    // with ones it has not, and then the original's byte order decides - which the owner must keep).
    for (int i = 0; i < 6; ++i) {
        const std::vector<u8> s = random_stream(5000u + static_cast<u64>(i), 400, 0u);
        std::vector<u8> expanded;
        require(gekko::prs_expand(s.data(), s.size(), 0u, expanded), "in-place stream decodes");
        const u32 dst = A + 0x40000u;
        // At small margins the original may overwrite its own unread source and never return (reported as "not an input").
        const u32 margins[] = {0u, 1u, 2u, 16u, 512u, 4096u};
        const u32 src = dst + static_cast<u32>(expanded.size()) + margins[i] - static_cast<u32>(s.size());
        cases.push_back({"in place margin " + std::to_string(margins[i]) + " (output " +
                             std::to_string(expanded.size()) + ", stream " + std::to_string(s.size()) + ")",
                         s, src, dst});
    }
    int yields_checked = 0, runs = 0;
    for (std::size_t k = 0; k < cases.size(); ++k) {
        for (int mode = 0; mode < 3; ++mode) {
            const u64 seed = 0xC0DEull + k * 31u + static_cast<u64>(mode);
            fill(original, owner, A, kArena, seed);
            prs_run_pair(original, owner, cases[k], mode, seed, {{A, kArena}}, 40000000ull, yields_checked);
            ++runs;
        }
    }
    const auto& s = owner.owner->stats();
    std::printf("prs synthetic: %zu streams x 3 modes = %d runs (%d not inputs; %d with hand-backs at every back edge); owner: %llu "
                "calls, %llu resumes, %llu hand-backs, %llu in place\n",
                cases.size(), runs, g_not_inputs, yields_checked, (unsigned long long)s.expansions,
                (unsigned long long)s.expansion_resumes, (unsigned long long)s.expansion_yields,
                (unsigned long long)s.expansions_in_place);
}

// Every PRS member of every ONE archive on the disc: the owner's output and count against the host codec, and,
// within the byte budget, against the original in the interpreter, register-exactly.
void prs_disc(const std::vector<u8>& dol, const std::filesystem::path& game, u64 interpreter_budget) {
    Machine original(dol, false), owner(dol, true), ahead(dol, true);
    constexpr u32 kSrc = 0x80700000u, kSrcLimit = 0x80FF0000u, kDst = 0x81000000u, kDstLimit = 0x817C0000u;
    std::size_t archives = 0, members = 0, skipped = 0, size_mismatch = 0, interpreted = 0;
    // The expand call served from a stream decoded ahead (gekko/prs_ahead.h) on a third machine, against the owner:
    // every register, the output and the bytes after it, and the source block; and a stream changed in guest memory
    // after decoding is refused (the call decodes) with the same result.
    std::size_t ahead_qualified = 0, ahead_served = 0, ahead_refused_checked = 0;
    std::shared_ptr<const gekko::PrsDecoded> ahead_stream;
    ahead.owner->set_prs_ahead(
        [&](const gekko::u8*, const std::function<bool(const gekko::PrsDecoded&)>& accept)
            -> std::shared_ptr<const gekko::PrsDecoded> {
            return ahead_stream != nullptr && accept(*ahead_stream) ? ahead_stream : nullptr;
        });
    u64 bytes = 0, interpreted_bytes = 0;
    int yields_checked = 0;
    const auto start = std::chrono::steady_clock::now();
    double owner_seconds = 0.0;  // the owner's calls alone (measurement only)
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(game / "files")) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](char ch) { return static_cast<char>(std::tolower(ch)); });
            if (ext == ".one") files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        const std::vector<u8> data = read_file(path);
        gekko::OneArchive archive;
        if (!gekko::OneArchive::parse(data.data(), data.size(), archive)) continue;
        ++archives;
        for (const auto& member : archive.members()) {
            if ((member.flags & 1u) == 0u) continue;
            ++members;
            const std::size_t start_offset = static_cast<std::size_t>(member.offset) + 0x0Cu;
            if (start_offset >= data.size()) {
                ++skipped;
                continue;
            }
            const std::size_t available = std::min<std::size_t>(data.size() - start_offset, kSrcLimit - kSrc);
            std::vector<u8> expected;
            const bool ok = gekko::prs_expand(data.data() + start_offset, available, 0u, expected);
            if (!ok || expected.size() > kDstLimit - kDst) {
                ++skipped;
                continue;
            }
            const std::string where = path.filename().string() + ":" + member.name;
            // The owner (no interpreter): output and count against the host codec.
            owner.prepare(members, false, false);
            owner.bus.write_block(kSrc, data.data() + start_offset, static_cast<u32>(available));
            u64 e = 0;
            const auto owner_start = std::chrono::steady_clock::now();
            check(owner.call(kPrs, kSrc, kDst, 0u, kPrsEdges, 4u, e), where + ": owner returns");
            owner_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - owner_start).count();
            const u32 written = owner.cpu.gpr[3];
            check(written == expected.size(), where + ": count " + std::to_string(written) + " vs host codec " +
                                                  std::to_string(expected.size()));
            std::vector<u8> got(expected.size());
            if (!got.empty()) owner.bus.peek_block(kDst, got.data(), static_cast<u32>(got.size()));
            check(got == expected, where + ": output equals the host codec's");
            if (written != member.size) ++size_mismatch;
            bytes += written;
            {
                auto decoded = std::make_shared<gekko::PrsDecoded>();
                if (gekko::prs_decode_ahead(data.data() + start_offset, available, member.size, *decoded)) {
                    ++ahead_qualified;
                    check(decoded->output == expected, where + ": decoded ahead = the host codec");
                    const bool corrupt = ahead_qualified % 97u == 5u && decoded->consumed > 2u;
                    ahead_stream = decoded;
                    ahead.prepare(members, false, false);
                    ahead.bus.write_block(kSrc, data.data() + start_offset, static_cast<u32>(available));
                    if (corrupt) {
                        // One stream byte changed in guest memory after the decode: refused, decoded as the owner
                        // would decode those bytes (the owner gets the same change).
                        const u32 at = kSrc + decoded->consumed / 2u;
                        const u8 v = static_cast<u8>(ahead.bus.read8(at) ^ 0x5Au);
                        ahead.bus.write8(at, v);
                        owner.prepare(members, false, false);
                        owner.bus.write_block(kSrc, data.data() + start_offset, static_cast<u32>(available));
                        owner.bus.write8(at, v);
                        u64 e2 = 0;
                        owner.call(kPrs, kSrc, kDst, 0u, kPrsEdges, 4u, e2);
                    }
                    // The bytes after the output: the ahead machine's own, which no call may touch.
                    const u32 out_n = owner.cpu.gpr[3];
                    std::vector<u8> after_before(64u), after_after(64u);
                    ahead.bus.peek_block(kDst + out_n, after_before.data(), 64u);
                    const u64 served_before = ahead.owner->prs_ahead_served();
                    u64 e3 = 0;
                    check(ahead.call(kPrs, kSrc, kDst, 0u, kPrsEdges, 4u, e3), where + ": ahead returns");
                    const bool served = ahead.owner->prs_ahead_served() != served_before;
                    check(served != corrupt, where + (corrupt ? ": a changed stream is refused" : ": served ahead"));
                    if (served) ++ahead_served;
                    if (corrupt) ++ahead_refused_checked;
                    compare(owner, ahead, {{kDst, out_n}, {kSrc, static_cast<u32>(available)}}, where + " ahead");
                    ahead.bus.peek_block(kDst + out_n, after_after.data(), 64u);
                    check(after_before == after_after, where + " ahead: nothing written after the output");
                    ahead_stream.reset();
                }
            }
            // The original, register-exactly, while the budget lasts (alternating plain / held interrupt).
            if (interpreted_bytes + written <= interpreter_budget) {
                const int mode = (interpreted % 7u == 3u && written <= 65536u) ? 1 : 0;
                fill(original, owner, kDst, static_cast<u32>(std::min<std::size_t>(expected.size() + 64u, kDstLimit - kDst)),
                     members);
                PrsCase c{where, std::vector<u8>(data.begin() + static_cast<std::ptrdiff_t>(start_offset),
                                                 data.begin() + static_cast<std::ptrdiff_t>(start_offset + available)),
                          kSrc, kDst};
                prs_run_pair(original, owner, c, mode, members,
                             {{kDst, static_cast<u32>(expected.size() + 64u)}}, 40ull * written + 100000u,
                             yields_checked);
                ++interpreted;
                interpreted_bytes += written;
            }
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("prs disc: %zu ONE archives, %zu PRS members (%zu skipped: too large / not decodable by the host "
                "codec), %llu bytes expanded by the owner = the host codec; %zu members (%llu bytes) also against "
                "the original in the interpreter (%d with hand-backs); %zu members whose count differs from the "
                "archive's declared size; %.1f s (the owner's calls %.3f s)\n",
                archives, members, skipped, (unsigned long long)bytes, interpreted,
                (unsigned long long)interpreted_bytes, yields_checked, size_mismatch, seconds, owner_seconds);
    std::printf("prs ahead: %zu members decoded ahead, %zu served register- and byte-equal to the owner, %zu changed "
                "streams refused\n",
                ahead_qualified, ahead_served, ahead_refused_checked);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: game_memory_owner_tests <sys/main.dol> [<interpreter byte budget>]\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256u, "missing original DOL");
        memcpy_cases(dol);
        prs_synthetic(dol);
        // The game data root holds the DOL at sys/main.dol (as asset_import_tests derives it).
        const std::filesystem::path game = std::filesystem::path(argv[1]).parent_path().parent_path();
        const u64 budget = argc >= 3 ? std::strtoull(argv[2], nullptr, 10) : 8ull * 1024u * 1024u;
        if (std::filesystem::is_directory(game / "files")) {
            prs_disc(dol, game, budget);
        } else {
            check(false, "prs disc: no files/ beside the DOL's sys/ folder");
        }
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    std::printf(g_failures == 0 ? "game_memory_owner_tests: PASS\n" : "game_memory_owner_tests: %d FAILURES\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
