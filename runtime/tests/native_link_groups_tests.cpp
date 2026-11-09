// liveness-roots: reference-interpreter-only
// The layout manager's link-group lists (include/gekko/native_link_groups.h): the native owner of fn_800CAEAC against
// the ORIGINAL words of the game's DOL (fn_800CAEAC and the fn_800CAE24 it calls 256 times) in the reference
// interpreter, on the same seeded image and register file. Equal means: r0-r8, r30, r31, r1, LR, PC, CTR, CR, XER;
// every byte of the manager, the slot array, the entries and the stack around r1 (the frame's four words included).
// Layouts: realistic (enabled entries of random groups, shared entries, garbage in the slots' other words and in the
// heads), the last visit's four outcomes, and adversarial ones the single pass must not serve (an entry inside the
// slot array, an entry over the head table, the head table inside the slot array).
//
// usage: native_link_groups_tests <sys/main.dol>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_link_groups.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gekko;

namespace {

constexpr u32 kRebuildAll = 0x800CAEACu, kRebuildOne = 0x800CAE24u;
constexpr u32 kFinish = 0x817FF000u;  // the call's return address: never executed
constexpr u32 kStack = 0x817E0000u;
constexpr u32 kArena = 0x81000000u, kArenaSize = 0x00300000u;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        if (g_failures <= 40) std::printf("FAIL: %s\n", what.c_str());
    }
}
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}
std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    RecompiledCode code;
    std::vector<RecompiledCode::Entry> entries;
    std::unique_ptr<NativeLinkGroups> owner;

    template <u32 Address>
    static void served(Cpu& c) {
        c.try_hle(Address);
    }

    Machine(const std::vector<u8>& dol, bool native) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        // The words as recovered (native_link_groups.h).
        require(bus.read32(kRebuildAll) == 0x9421FFF0u && bus.read32(0x800CAECCu) == 0x4BFFFF59u &&
                    bus.read32(0x800CAED4u) == 0x2C1F0100u && bus.read32(kRebuildOne) == 0x5487103Au &&
                    bus.read32(0x800CAE30u) == 0x38000800u && bus.read32(0x800CAE58u) == 0x88050022u,
                "unexpected link-group instruction evidence");
        cpu.reset();
        if (native) {
            entries.push_back({kRebuildAll, 4u, served<kRebuildAll>, "link groups"});
            code.install(entries.data(), entries.size());
            cpu.attach_recompiled(&code);
            owner = std::make_unique<NativeLinkGroups>(cpu);
            owner->install();
        }
    }

    bool call(u64 seed, const std::function<void(GuestMemory&, std::mt19937_64&, u32&)>& layout) {
        std::mt19937_64 random(seed);
        std::vector<u8> fill(kArenaSize);
        for (auto& v : fill) v = static_cast<u8>(random());
        bus.write_block(kArena, fill.data(), kArenaSize);
        std::vector<u8> frame(0x200u);
        for (auto& v : frame) v = static_cast<u8>(random());
        bus.write_block(kStack - 0x100u, frame.data(), static_cast<u32>(frame.size()));
        u32 manager = 0;
        layout(bus, random, manager);
        for (u32 r = 0; r < 32u; ++r) cpu.gpr[r] = static_cast<u32>(random());
        cpu.gpr[1] = kStack;
        cpu.gpr[3] = manager;
        cpu.cr = static_cast<u32>(random());
        cpu.xer = static_cast<u32>(random()) & 0xE000007Fu;
        cpu.ctr = static_cast<u32>(random());
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
        cpu.lr = kFinish;
        cpu.pc = kRebuildAll;
        u64 steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 100'000'000ull) cpu.run_steps(1u);
        return cpu.pc == kFinish && !cpu.stopped();
    }
};

void compare(Machine& a, Machine& b, const std::string& where) {
    for (const u32 r : {0u, 1u, 3u, 4u, 5u, 6u, 7u, 8u, 30u, 31u}) {
        check(a.cpu.gpr[r] == b.cpu.gpr[r],
              where + ": r" + std::to_string(r) + " original " + hex(a.cpu.gpr[r]) + " owner " + hex(b.cpu.gpr[r]));
    }
    check(a.cpu.lr == b.cpu.lr && a.cpu.pc == b.cpu.pc, where + ": lr / pc");
    check(a.cpu.ctr == b.cpu.ctr, where + ": ctr");
    check(a.cpu.cr == b.cpu.cr, where + ": cr original " + hex(a.cpu.cr) + " owner " + hex(b.cpu.cr));
    check(a.cpu.xer == b.cpu.xer, where + ": xer");
    std::vector<u8> x(kArenaSize), y(kArenaSize);
    a.bus.peek_block(kArena, x.data(), kArenaSize);
    b.bus.peek_block(kArena, y.data(), kArenaSize);
    const auto diff = std::mismatch(x.begin(), x.end(), y.begin());
    check(diff.first == x.end(),
          where + ": the arena differs first at " + hex(kArena + static_cast<u32>(diff.first - x.begin())));
    std::vector<u8> s(0x200u), t(0x200u);
    a.bus.peek_block(kStack - 0x100u, s.data(), 0x200u);
    b.bus.peek_block(kStack - 0x100u, t.data(), 0x200u);
    check(s == t, where + ": the stack around r1");
}

// Layout parameters: the manager, its slot array (word +0x18), and the entries the slots point at.
constexpr u32 kManager = 0x81000000u, kSlots = 0x81010000u, kEntries = 0x81100000u;

// Realistic: `entries` entries of 0x2C bytes, each slot pointing at entry (i % entries), enabled with probability
// `enabled_percent`, link groups drawn from `groups` (0 = uniform 0..255).
auto realistic(u32 entries, u32 enabled_percent, std::vector<u32> groups) {
    return [=](GuestMemory& bus, std::mt19937_64& random, u32& manager) {
        manager = kManager;
        bus.write32(kManager + 0x18u, kSlots);
        for (u32 e = 0; e < entries; ++e) {
            const u32 at = kEntries + e * 0x2Cu;
            const u32 flags = static_cast<u32>(random());
            bus.write32(at + 0x18u, (random() % 100u) < enabled_percent ? (flags | 1u) : (flags & ~1u));
            bus.write8(at + 0x22u, groups.empty() ? static_cast<u8>(random())
                                                  : static_cast<u8>(groups[random() % groups.size()]));
        }
        for (u32 i = 0; i < 0x800u; ++i) bus.write32(kSlots + i * 0x20u, kEntries + (i % entries) * 0x2Cu);
    };
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: native_link_groups_tests <sys/main.dol>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        struct Case {
            std::string name;
            std::function<void(GuestMemory&, std::mt19937_64&, u32&)> layout;
            bool single_pass;  // the owner may serve it with the single pass
        };
        std::vector<Case> cases{
            {"Westopolis-like (1111 entries, groups 0..30)", realistic(1111u, 85u, {0, 0, 0, 0, 1, 2, 3, 7, 30}), true},
            {"2048 entries, uniform groups", realistic(2048u, 60u, {}), true},
            {"shared entries (37 entries)", realistic(37u, 90u, {}), true},
            {"nothing enabled", realistic(500u, 0u, {}), true},
            {"everything group 255", realistic(2048u, 100u, {255}), true},
            {"everything group 0", realistic(2048u, 100u, {0}), true},
            {"groups 254 and 255", realistic(1500u, 70u, {254, 255}), true},
            {"slot 2047 first of group 255",
             [](GuestMemory& bus, std::mt19937_64& random, u32& manager) {
                 realistic(2048u, 100u, {3})(bus, random, manager);
                 bus.write8(kEntries + 2047u * 0x2Cu + 0x22u, 255u);
             },
             true},
            {"slot 2047 disabled",
             [](GuestMemory& bus, std::mt19937_64& random, u32& manager) {
                 realistic(2048u, 100u, {255})(bus, random, manager);
                 bus.write32(kEntries + 2047u * 0x2Cu + 0x18u, 0x10u);
             },
             true},
            {"an entry inside the slot array",
             [](GuestMemory& bus, std::mt19937_64& random, u32& manager) {
                 realistic(1000u, 80u, {0, 1, 2})(bus, random, manager);
                 bus.write32(kSlots + 5u * 0x20u, kSlots + 40u * 0x20u - 0x18u + 4u);  // flags = slot 40's prev word
             },
             false},
            {"an entry over the head table",
             [](GuestMemory& bus, std::mt19937_64& random, u32& manager) {
                 realistic(1000u, 80u, {0, 1, 2})(bus, random, manager);
                 bus.write32(kSlots + 900u * 0x20u, kManager + 0x1Cu + 4u * 2u - 0x18u);  // flags = head of group 2
             },
             false},
            {"the head table inside the slot array",
             [](GuestMemory& bus, std::mt19937_64& random, u32& manager) {
                 realistic(800u, 80u, {0, 1})(bus, random, manager);
                 manager = kSlots + 0x100u;  // its heads cover slots 9.. (its own +0x18 word points at the array)
                 bus.write32(manager + 0x18u, kSlots);
             },
             false},
        };
        int index = 0;
        u64 served_single = 0, served_walk = 0;
        for (const Case& c : cases) {
            for (u32 rep = 0; rep < 2u; ++rep) {
                Machine original(dol, false), owner(dol, true);
                const u64 seed = 0x11AB0000ull + static_cast<u64>(index++);
                const std::string where = c.name + " #" + std::to_string(rep);
                const bool a = original.call(seed, c.layout);
                const bool b = owner.call(seed, c.layout);
                check(a, where + ": the original did not return (" + original.cpu.stop().detail + ")");
                check(b, where + ": the owner did not return (" + owner.cpu.stop().detail + ")");
                if (a && b) compare(original, owner, where);
                const auto& stats = owner.owner->stats();
                check(stats.rebuilds == 1u, where + ": the owner served the call");
                check((stats.one_pass == 1u) == c.single_pass,
                      where + (c.single_pass ? ": served by the single pass" : ": served by the original's walk"));
                served_single += stats.one_pass;
                served_walk += stats.walked;
            }
        }
        std::printf("native link groups: %zu layouts x 2 seeds against the original in the interpreter "
                    "(%llu single pass, %llu walked), %d failure(s)\n",
                    cases.size(), static_cast<unsigned long long>(served_single),
                    static_cast<unsigned long long>(served_walk), g_failures);
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
