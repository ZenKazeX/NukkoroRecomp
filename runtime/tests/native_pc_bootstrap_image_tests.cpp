#include "native_pc_bootstrap_image.h"
#include "native_pc_boot_constructor_audit.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {
using shadow::pc_bootstrap::InitializedImage;
using shadow::pc_bootstrap::load_initialized_image;
using shadow::pc_bootstrap::apply_verified_crt_initializers;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void put32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (24u - 8u * i));
}
template<class F> void rejects(F&& operation) {
    bool refused = false;
    try { operation(); } catch (const std::runtime_error&) { refused = true; }
    require(refused, "malformed/budget input was accepted");
}
void loader_contract() {
    std::vector<std::uint8_t> dol(0x104u);
    put32(dol, 0x1Cu, 0x100u); put32(dol, 0x64u, 0x805F2780u); put32(dol, 0xACu, 4u);
    put32(dol, 0xD8u, 0x805F2000u); put32(dol, 0xDCu, 0x1000u);
    put32(dol, 0x100u, 0x40490FDBu);
    auto image = load_initialized_image(dol, 0x1000u);
    require(image.storage.size() == 0x1000u && image.link_base == 0x805F2000u,
            "owner allocated more than image extent");
    require(image.word(0x805F2780u) == 0x40490FDBu, "BSS overlap erased initialized data");
    require(image.word(0x805F2000u) == 0u, "BSS backing was not zero initialized");
    rejects([&] { (void)load_initialized_image(dol, 0xFFFu); });
    rejects([&] { (void)image.range(0x805F1FFFu, 1u); });
    rejects([&] { (void)image.range(0x805F2FFFu, 2u); });
    auto malformed = dol; put32(malformed, 0x64u, 0xFFFFFFFFu);
    rejects([&] { (void)load_initialized_image(malformed, 0x1000u); });
    malformed = dol; put32(malformed, 0xACu, 8u);
    rejects([&] { (void)load_initialized_image(malformed, 0x1000u); });
    malformed = dol; put32(malformed, 0x1Cu, 0xF0u);
    rejects([&] { (void)load_initialized_image(malformed, 0x1000u); });
}
void constructor_observation_contract() {
    using shadow::pc_bootstrap::ConstructorAudit;
    namespace original = shadow::pc_bootstrap::original;
    const auto feed = [](ConstructorAudit& audit, std::size_t i, std::size_t slot) {
        audit.observe(original::kConstructorTable[i], ConstructorAudit::kTableAddress +
            static_cast<std::uint32_t>(slot * 4u), ConstructorAudit::kWalkerReturn, 100u + i * 10u);
    };
    ConstructorAudit audit;
    // Calls outside the original walker must not appear in its ordered census.
    feed(audit, 0u, 0u);
    require(audit.count() == 0u, "pre-walker constructor was attributed to bootstrap");
    audit.begin(true);
    audit.observe(original::kConstructorTable[0], ConstructorAudit::kTableAddress,
                  0x803796F4u, 1u);
    require(audit.count() == 0u, "nested/wrong-LR constructor entered walker census");
    for (std::size_t i = 0; i < ConstructorAudit::kExpected; ++i) feed(audit, i, i);
    require(!audit.valid(), "unsealed constructor walk certified complete");
    audit.seal(true);
    require(audit.valid(), "exact original constructor order was rejected");
    feed(audit, 0u, 0u);
    require(audit.valid() && audit.count() == ConstructorAudit::kExpected,
            "later gameplay constructor changed sealed original order");
    ConstructorAudit reordered; reordered.begin(true);
    for (std::size_t i = 0; i < ConstructorAudit::kExpected; ++i) feed(reordered, i, i == 0u ? 1u : i);
    reordered.seal(true);
    require(!reordered.valid(), "wrong original table-slot ownership accepted");
    ConstructorAudit missing; missing.begin(true);
    for (std::size_t i = 0; i + 1u < ConstructorAudit::kExpected; ++i) feed(missing, i, i);
    missing.seal(true);
    require(!missing.valid(), "missing original constructor accepted");
    ConstructorAudit growth; growth.begin(true);
    for (std::size_t i = 0; i < ConstructorAudit::kExpected; ++i) feed(growth, i, i);
    feed(growth, 0u, 0u); growth.seal(true);
    require(!growth.valid() && growth.count() == ConstructorAudit::kExpected && growth.overflow() == 1u,
            "duplicate constructor grew storage or was accepted");
    ConstructorAudit table_changed; table_changed.begin(true);
    for (std::size_t i = 0; i < ConstructorAudit::kExpected; ++i) feed(table_changed, i, i);
    table_changed.seal(false);
    require(!table_changed.valid(), "mutated constructor table accepted");
}
void original_crt_contract(const std::filesystem::path& dol_path) {
    std::ifstream in(dol_path, std::ios::binary);
    require(bool(in), "cannot read original DOL");
    const std::vector<std::uint8_t> dol{std::istreambuf_iterator<char>(in), {}};
    // Exact actual image extent: 80003100..805FC5EC, not a MEM1 allocation.
    auto image = load_initialized_image(dol, 0x5F94ECu);
    require(image.storage.size() == 0x5F94ECu && image.original_entry == 0x80003154u,
            "unexpected original image extent/entry");
    constexpr std::array<std::array<std::uint32_t, 2>, 3> zeros{{
        {0x8056FE00u, 0x74700u}, {0x805EF020u, 0x375Cu}, {0x805FC540u, 0xACu}}};
    // Earlier hardware initialization can write into BSS. Clearing must still
    // happen at this ownership boundary, rather than assuming loader zeros.
    for (const auto& row : zeros) std::fill(image.range(row[0], row[1]).begin(), image.range(row[0], row[1]).end(), 0xA5u);
    const auto before = image.storage;
    require(apply_verified_crt_initializers(image) == 491272u, "wrong CRT clear demand");
    for (std::size_t i = 0; i < image.storage.size(); ++i) {
        const auto address = std::uint64_t(image.link_base) + i;
        bool cleared = false;
        for (const auto& row : zeros) cleared |= address >= row[0] && address < std::uint64_t(row[0]) + row[1];
        require(image.storage[i] == (cleared ? 0u : before[i]), "CRT altered bytes outside proven zero ranges");
    }
    // Failure must be transactional: even a bad FINAL row cannot cause an
    // earlier clear, and an unknown code/relocation must never be executed.
    for (const auto address : {0x80003340u, 0x80005548u, 0x800055DCu, 0x800055E4u}) {
        auto bad = image;
        bad.range(0x80586CB4u, 1u)[0] = 0xA5u;
        put32(bad.range(address, 4u), 0, 0xDEADBEEFu);
        const auto unchanged = bad.storage;
        rejects([&] { (void)apply_verified_crt_initializers(bad); });
        require(bad.storage == unchanged, "refusal partially initialized image");
    }
}

std::uint64_t le(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) value |= std::uint64_t(bytes[offset + i]) << (8u * i);
    return value;
}
InitializedImage snapshot_linked_image(const std::filesystem::path& path, std::uint32_t expected_pc,
                                      bool allow_proven_dbinit_memory_boundary = false) {
    // Index fixed canonical CPU/memory extents before allocating the linked
    // span. A variable device owner length is only seeked, never allocated.
    std::ifstream source(path, std::ios::binary | std::ios::ate);
    require(bool(source), "cannot open original contract");
    const auto file_bytes = static_cast<std::streamoff>(source.tellg());
    require(file_bytes >= 176, "truncated original contract extent");
    std::array<std::uint8_t, 176> prefix{};
    source.seekg(0); source.read(reinterpret_cast<char*>(prefix.data()), prefix.size());
    require(bool(source), "truncated original contract prefix");
    std::streamoff mem1_offset = 0;
    const bool fragment = std::equal(prefix.begin(), prefix.begin() + 8, "GFBCON01");
    const bool legacy = std::equal(prefix.begin(), prefix.begin() + 8, "GFSNAP01");
    if (legacy) {
        require(file_bytes == 168 + 16 + (24 << 20) + (16 << 20), "invalid GFSNAP01 file extent");
        require(le(prefix, 16, 4) == expected_pc, "snapshot is not the exact required program boundary");
        source.seekg(168);
    } else {
        std::uint32_t declared = 0, actual = 0;
        if (fragment) {
            require(le(prefix, 8, 4) == 1u && le(prefix, 12, 4) == 9u, "unknown boot fragment version");
            declared = static_cast<std::uint32_t>(le(prefix, 16, 4));
            actual = static_cast<std::uint32_t>(le(prefix, 20, 4));
            const auto flags = le(prefix, 32, 4);
            require((flags & ~1u) == 0u, "unknown fragment flags");
            const bool observed_post = allow_proven_dbinit_memory_boundary && declared == 0x80370BF0u && flags == 1u;
            require(declared == expected_pc || observed_post, "fragment is not the required memory boundary");
            require(flags == 0u || declared == 0x80370BF0u, "post memory proof at wrong boundary");
            require(le(prefix, 36, 8) == 5456u, "unknown canonical CPU fragment extent");
            source.seekg(44);
        } else {
            require(std::equal(prefix.begin(), prefix.begin() + 4, "SKGK") && le(prefix, 4, 4) == 9u,
                    "unknown contract schema/version");
            source.seekg(16);
        }
        std::array<std::uint8_t, 8> length{};
        for (const auto bytes : {4872u, 528u, 32u}) {
            source.read(reinterpret_cast<char*>(length.data()), length.size());
            require(bool(source) && le(length, 0, 8) == bytes, "unknown canonical CPU block extent");
            const auto begin = static_cast<std::streamoff>(source.tellg());
            require(begin <= file_bytes && bytes <= file_bytes - begin, "truncated canonical CPU block");
            if (bytes == 4872u) {
                std::array<std::uint8_t, 4> pc{};
                source.seekg(begin + 640); source.read(reinterpret_cast<char*>(pc.data()), pc.size());
                require(bool(source) && le(pc, 0, 4) == (fragment ? actual : expected_pc),
                        "canonical CPU actual PC disagrees with declared contract");
            }
            source.seekg(begin + bytes);
        }
    }
    const auto block = [&](std::uint64_t expected, bool fixed) {
        std::array<std::uint8_t, 8> length{};
        source.read(reinterpret_cast<char*>(length.data()), length.size());
        require(bool(source), "truncated contract domain prefix");
        const auto size = le(length, 0, 8);
        const auto begin = static_cast<std::streamoff>(source.tellg());
        require((!fixed || size == expected) && begin <= file_bytes && size <= std::uint64_t(file_bytes - begin),
                "invalid/truncated contract domain extent");
        source.seekg(begin + static_cast<std::streamoff>(size));
        return begin;
    };
    mem1_offset = block(24u << 20u, true);
    (void)block(16u << 20u, true);
    if (!legacy) (void)block(16u << 10u, true);
    if (fragment) {
        for (unsigned i = 0; i < 7u; ++i) (void)block(0u, false);
        require(source.tellg() == file_bytes, "unknown boot fragment trailing bytes");
    }
    InitializedImage image{0x80003100u, 0x80003154u, std::vector<std::uint8_t>(0x5F94ECu)};
    source.seekg(mem1_offset + 0x3100u);
    source.read(reinterpret_cast<char*>(image.storage.data()), image.storage.size());
    require(bool(source), "snapshot linked image truncated");
    return image;
}
void original_crt_snapshot_contract(const std::filesystem::path& before,
                                    const std::filesystem::path& after) {
    auto candidate = snapshot_linked_image(before, 0x80003340u);
    const auto oracle = snapshot_linked_image(after, 0x80003170u, true);
    (void)apply_verified_crt_initializers(candidate);
    std::size_t differences = 0;
    for (std::size_t i = 0; i < candidate.storage.size(); ++i) {
        if (candidate.storage[i] == oracle.storage[i]) continue;
        if (differences < 32u) std::cerr << "linked-byte-difference 0x" << std::hex
                                       << candidate.link_base + i << std::dec << '\n';
        ++differences;
    }
    std::cout << "CRT original-snapshot linked-span different-bytes=" << differences
              << " compared=" << candidate.storage.size()
              << " ignored-in-scope=0 full-handoff-proven=false\n";
    require(differences == 0, "CRT differs from actual original post memory boundary");
}
}
int main(int argc, char** argv) {
    try {
        loader_contract();
        constructor_observation_contract();
        if (argc == 4 && std::string_view(argv[1]) == "--crt-pre-post") {
            original_crt_snapshot_contract(argv[2], argv[3]);
        } else if (argc == 2) {
            original_crt_contract(argv[1]);
        } else {
            require(argc == 1, "usage: [DOL] or --crt-pre-post exact-preCRT exact-postCRT");
        }
        std::cout << "native PC image contracts passed; full boot/handoff unproven\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
