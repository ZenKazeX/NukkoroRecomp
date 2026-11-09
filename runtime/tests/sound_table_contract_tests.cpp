// The native sound-effect table lookup against the original instructions (gekko/native_sound_tables.h).
//
// Both sides get the same guest memory: a manager whose eight table slots hold the rows of the shipped
// AudioData*.bin files (System, Gadget, Weapon, Player, PlayerHero, PlayerDark, Enemy and each stage table in turn
// in slot 7), plus mutated copies that exercise disabled rows and instance limits. The original 0x801D88CC (with its
// helper 0x801D8838) runs from main.dol in the interpreter; the owner runs natively. After every call the result
// register and every byte of the manager and its rows must match - except the one documented erratum, which must
// change exactly the WARPHOLE row's program (to disabled, so that lookup is refused) and nothing else.

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_sound_tables.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kManager = 0x81000000u, kRows = kManager + 0x1000u, kRowsEnd = kManager + 0x100000u;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;

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
    explicit Side(const std::vector<u8>& dol) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
    }
};

struct Table {
    std::string name;
    std::vector<u8> rows;  // count * 0x18, as the parser lays them out (+0 is the relocated name; not read)
    u32 count = 0;
};

Table load_table(const std::filesystem::path& path) {
    const std::vector<u8> file = read_file(path);
    require(file.size() >= 0x28, "short table " + path.string());
    Table table;
    table.name = path.filename().string();
    table.count = load_be32(file.data() + 0x24);
    require(0x28u + u64(table.count) * 0x18u <= file.size(), "table rows past the file " + table.name);
    table.rows.assign(file.begin() + 0x28, file.begin() + 0x28 + table.count * 0x18);
    return table;
}

struct Harness {
    Side original, native;
    sound_tables::Report report;
    u64 calls = 0, instructions = 0, found = 0, errata = 0;
    explicit Harness(const std::vector<u8>& dol) : original(dol), native(dol) {}

    // Lays the tables into both sides: slot i at kManager + 16 i, rows packed from kRows.
    void install(const std::vector<const Table*>& slots) {
        for (Side* side : {&original, &native}) {
            const std::vector<u8> zero(kRowsEnd - kManager);
            side->memory.write_block(kManager, zero.data(), zero.size());
            u32 cursor = kRows;
            for (u32 slot = 0; slot < slots.size(); ++slot) {
                if (slots[slot] == nullptr) continue;
                const Table& t = *slots[slot];
                side->memory.write32(kManager + slot * 16, t.count);
                side->memory.write32(kManager + slot * 16 + 4, cursor);
                side->memory.write_block(cursor, t.rows.data(), t.rows.size());
                cursor += static_cast<u32>(t.rows.size());
                require(cursor < kRowsEnd, "tables do not fit");
            }
        }
    }

    std::vector<u8> image(Side& side) {
        std::vector<u8> bytes(kRowsEnd - kManager);
        side.memory.read_block(kManager, bytes.data(), bytes.size());
        return bytes;
    }

    // One lookup on both sides; returns the row (0 = none). An erratum may change the native side only by the
    // documented bytes; the original side is then brought to the same bytes so the comparison continues.
    u32 lookup(u32 id) {
        Cpu& cpu = original.cpu;
        cpu.reset();
        cpu.pc = sound_tables::kLookup; cpu.npc = cpu.pc + 4; cpu.lr = kReturn;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        cpu.gpr[3] = kManager; cpu.gpr[4] = id;
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 100000) { cpu.run_steps(1); ++instructions; }
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original lookup did not return");
        const u32 expected = cpu.gpr[3];

        Cpu& n = native.cpu;
        n.reset();
        n.gpr[3] = kManager; n.gpr[4] = id;
        const u64 errata_before = report.errata_applied;
        sound_tables::serve_lookup(n, report);

        std::vector<u8> a = image(original), b = image(native);
        if (report.errata_applied != errata_before) {
            // The WARPHOLE row becomes disabled (a deliberate fix of an original-game bug): the original returned it
            // (bank 0, program 0, the menu window-open sound), the owner refuses it.
            ++errata;
            require(expected != 0 && n.gpr[3] == 0, "the WARPHOLE erratum must turn a returned row into a refusal");
            const u32 at = expected - kManager;
            require(b[at + 4] == 0 && b[at + 5] == 0 && b[at + 6] == 0x71 && b[at + 7] == 0x60 &&
                        a[at + 8] == 0 && a[at + 9] == 0 && b[at + 8] == 0 && b[at + 9] == 0xFF,
                    "the WARPHOLE erratum changed something other than its program");
            for (u32 i = 0; i < a.size(); ++i)
                if (i != at + 9) require(a[i] == b[i], "an erratum touched another byte");
            original.memory.write8(expected + 9, 0xFF);
            a = image(original);
        } else {
            require(n.gpr[3] == expected, "lookup result differs for id " + std::to_string(id));
        }
        require(a == b, "table memory differs after id " + std::to_string(id));
        ++calls;
        if (n.gpr[3]) ++found;
        return n.gpr[3];
    }
};

void run(Harness& h, const std::vector<Table>& fixed, const std::vector<Table>& stages) {
    // Slot order: System (0xE ids), Gadget 1, Weapon 2, Player 3, PlayerHero 4, PlayerDark 5, Enemy 6, stage 7.
    for (const Table& stage : stages) {
        std::vector<const Table*> slots(8, nullptr);
        for (u32 i = 0; i < fixed.size(); ++i) slots[i] = &fixed[i];
        slots[7] = &stage;
        h.install(slots);
        // Every shipped id (each table's own nibble), twice: rows with an instance limit count up.
        for (int pass = 0; pass < 2; ++pass)
            for (const Table* t : slots)
                for (u32 r = 0; r < t->count; ++r) h.lookup(load_be32(t->rows.data() + r * 0x18 + 4));
        // Every nibble, absent ids, and bits above the 16-bit id.
        for (u32 nibble = 0; nibble < 16; ++nibble) {
            h.lookup(nibble << 12);
            h.lookup((nibble << 12) | 0x0FFFu);
            h.lookup(0x00010000u | (nibble << 12) | 0x0001u);
            h.lookup(0xFFFF0000u | (nibble << 12));
        }
    }
    // Disabled rows and instance limits: mutate the System table and drive each limited row past its limit.
    Table mutated = fixed[0];
    for (u32 r = 0; r < mutated.count; ++r) {
        u8* row = mutated.rows.data() + r * 0x18;
        if (r % 3 == 0) row[9] = static_cast<u8>(0x80 | r);                     // disabled
        if (r % 3 == 1) store_be32(row + 0x14, 1 + (r % 4));                    // limit 1..4
        if (r % 5 == 2) { store_be32(row + 0x14, 2); store_be32(row + 0x10, 5); }  // already past its limit
    }
    std::vector<const Table*> slots(8, nullptr);
    slots[0] = &mutated;
    h.install(slots);
    for (int pass = 0; pass < 6; ++pass)
        for (u32 r = 0; r < mutated.count; ++r) h.lookup(load_be32(mutated.rows.data() + r * 0x18 + 4));
    // An empty slot (count 0) and a negative count.
    Table empty = fixed[0];
    empty.count = 0;
    slots[0] = &empty;
    h.install(slots);
    h.lookup(0xE000u);
    for (Side* side : {&h.original, &h.native}) side->memory.write32(kManager, 0xFFFFFFFFu);
    h.lookup(0xE000u);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 3, "usage: sound_table_contract_tests <main.dol> <game files/sound folder>");
        const std::vector<u8> dol = read_file(argv[1]);
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        // SHA-256: fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const std::filesystem::path sound = argv[2];
        std::vector<Table> fixed;
        for (const char* name : {"AudioDataSystem.bin", "AudioDataGadget.bin", "AudioDataWeapon.bin",
                                 "AudioDataPlayer.bin", "AudioDataPlayerHero.bin", "AudioDataPlayerDark.bin",
                                 "AudioDataEnemy.bin"})
            fixed.push_back(load_table(sound / name));
        std::vector<Table> stages;
        for (const auto& entry : std::filesystem::directory_iterator(sound)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("AudioDataStage", 0) == 0) stages.push_back(load_table(entry.path()));
        }
        require(stages.size() == 16, "expected the 16 shipped stage tables");
        Harness h(dol);
        run(h, fixed, stages);
        require(h.errata == 1 && h.report.errata_applied == 1,
                "the WARPHOLE erratum must apply exactly once (City table, first lookup of 0x7160)");
        std::printf("PASS native sound-table lookup: %llu calls (%llu found) identical to %llu original instructions "
                    "over 7 fixed + %zu stage tables, mutations and limits; 1 erratum (City 0x7160 "
                    "disabled: silent, a deliberate fix of the original's menu sound)\n",
                    static_cast<unsigned long long>(h.calls), static_cast<unsigned long long>(h.found),
                    static_cast<unsigned long long>(h.instructions), stages.size());
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
