// The player migration boundary (gekko/player_view.h) against its contract: one guest access per accessor, of the
// field's width, at base + offset, in call order, journalled and rolled back like a translated row's; float bits moved
// unchanged (a signalling NaN payload included); and the field table equal to the binary evidence
// (test_data/player/player_shadow_fields.csv, tools/dolrecon/player_layout.py): every field is an observed
// (offset, width, kind), every observed offset is a field, no two fields overlap, all inside the 0x29C object.
//
// usage: player_view_tests <test_data/player/player_shadow_fields.csv>

#include "gekko/guest_memory.h"
#include "gekko/player_view.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace gekko;
using player::FieldKind;
using player::kPlayerShadowFields;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

constexpr u32 kObject = 0x80F503D8u;  // where the Westopolis savestates hold the player (any aligned MEM1 address)

struct Access {
    u32 address;
    std::size_t size;
    bool write;
};

void test_one_access_per_accessor() {
    GuestMemory bus;
    std::vector<Access> trace;
    bus.set_access_trace([&](u32 a, std::size_t n, bool w) { trace.push_back({a, n, w}); });
    player::PlayerShadowView view(bus, kObject);
    view.set_word<0x64>(0x0000000Eu);
    view.set_float_bits<0x274>(0x40A00000u);
    const u32 id = view.word<0x64>();
    const u32 radius = view.float_bits<0x274>();
    check(id == 0x0000000Eu && radius == 0x40A00000u, "values read back");
    const std::vector<Access> expected = {
        {kObject + 0x64u, 4, true}, {kObject + 0x274u, 4, true}, {kObject + 0x64u, 4, false}, {kObject + 0x274u, 4, false}};
    check(trace.size() == expected.size(), "exactly one guest access per accessor call");
    for (std::size_t i = 0; i < expected.size() && i < trace.size(); ++i)
        check(trace[i].address == expected[i].address && trace[i].size == expected[i].size &&
                  trace[i].write == expected[i].write,
              "access " + std::to_string(i) + ": address, width and direction in call order");
    bus.set_access_trace({});
}

void test_big_endian_and_float_bits() {
    GuestMemory bus;
    player::PlayerShadowView view(bus, kObject);
    view.set_word<0x30>(0x11223344u);
    check(bus.read8(kObject + 0x30u) == 0x11u && bus.read8(kObject + 0x33u) == 0x44u, "words are stored big-endian");
    view.set_float_bits<0x0C>(0x7F800001u);  // signalling NaN: the view moves bits, never quiets them
    check(view.float_bits<0x0C>() == 0x7F800001u, "a signalling NaN payload survives a store and a load");
    view.set_float_value<0x10>(-0.0f);
    check(bus.read32(kObject + 0x10u) == 0x80000000u, "negative zero is stored as its bits");
}

void test_journal_and_rollback() {
    GuestMemory bus;
    player::PlayerShadowView view(bus, kObject);
    view.set_word<0x238>(0xAAAAAAAAu);
    bus.journal_begin();
    view.set_word<0x238>(0xBBBBBBBBu);
    view.set_float_bits<0x27C>(0x3F800000u);
    check(bus.journal().size() == 2u && bus.journal()[0].address == kObject + 0x238u &&
              bus.journal()[1].address == kObject + 0x27Cu,
          "the view's writes are journalled, in order");
    bus.journal_rollback();
    bus.journal_end();
    check(view.word<0x238>() == 0xAAAAAAAAu && view.float_bits<0x27C>() == 0u, "rollback restores the object");
}

void test_table_matches_evidence(const char* path) {
    std::ifstream csv(path);
    check(static_cast<bool>(csv), std::string("evidence file readable: ") + path);
    std::string line;
    std::getline(csv, line);  // header
    std::set<std::tuple<u32, u32, FieldKind>> observed;
    std::set<u32> offsets;
    while (std::getline(csv, line)) {
        std::stringstream row(line);
        std::string offset, width, kind;
        std::getline(row, offset, ',');
        std::getline(row, width, ',');
        std::getline(row, kind, ',');
        if (offset.empty() || offset[0] == '-') continue;
        const u32 o = static_cast<u32>(std::stoul(offset, nullptr, 16));
        const FieldKind k = kind == "f32" ? FieldKind::Float : FieldKind::Int;
        check(kind == "f32" || kind == "int" || kind == "sint", "evidence kind " + kind + " has a view kind");
        observed.insert({o, static_cast<u32>(std::stoul(width)), k});
        offsets.insert(o);
    }
    check(!observed.empty(), "evidence rows present");
    std::set<u32> fields;
    for (const auto& f : kPlayerShadowFields) {
        char at[16];
        std::snprintf(at, sizeof(at), "+0x%X", f.offset);
        check(observed.count({f.offset, f.width, f.kind}) == 1u, std::string("field ") + at + " is observed with this width and kind");
        check(f.offset % f.width == 0u, std::string("field ") + at + " is naturally aligned");
        check(f.offset + f.width <= player::kPlayerShadowSize, std::string("field ") + at + " lies inside the object");
        fields.insert(f.offset);
    }
    for (u32 o : offsets) {
        char at[16];
        std::snprintf(at, sizeof(at), "+0x%X", o);
        check(fields.count(o) == 1u, std::string("observed offset ") + at + " is a field of the view");
    }
    for (std::size_t i = 0; i < std::size(kPlayerShadowFields); ++i)
        for (std::size_t j = i + 1; j < std::size(kPlayerShadowFields); ++j) {
            const auto& a = kPlayerShadowFields[i];
            const auto& b = kPlayerShadowFields[j];
            check(a.offset + a.width <= b.offset || b.offset + b.width <= a.offset, "no two fields overlap");
        }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: player_view_tests <player_shadow_fields.csv>\n");
        return 2;
    }
    test_one_access_per_accessor();
    test_big_endian_and_float_bits();
    test_journal_and_rollback();
    test_table_matches_evidence(argv[1]);
    if (g_failures != 0) {
        std::printf("%d failures\n", g_failures);
        return 1;
    }
    std::printf("PASS player view: %zu evidenced fields, one access per accessor, journal + rollback, float bits\n",
                std::size(kPlayerShadowFields));
    return 0;
}
