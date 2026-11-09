// The disc's directory table as a test oracle. Contract: fst_oracle.h.

#include "fst_oracle.h"

#include <cstring>

namespace gekko::test_support {

namespace {

u32 be32(const std::vector<u8>& bytes, std::size_t offset) {
    return (static_cast<u32>(bytes[offset]) << 24) | (static_cast<u32>(bytes[offset + 1]) << 16) |
           (static_cast<u32>(bytes[offset + 2]) << 8) | static_cast<u32>(bytes[offset + 3]);
}

void put_be32(std::vector<u8>& bytes, std::size_t offset, u32 value) {
    bytes[offset] = static_cast<u8>(value >> 24);
    bytes[offset + 1] = static_cast<u8>(value >> 16);
    bytes[offset + 2] = static_cast<u8>(value >> 8);
    bytes[offset + 3] = static_cast<u8>(value);
}

}  // namespace

bool namespace_from_fst(const std::vector<u8>& table, assets::AssetNamespace& out, std::string& error) {
    using assets::AssetNamespace;
    error.clear();
    if (table.size() < 12u) {
        error = "table is shorter than one entry";
        return false;
    }
    const u32 count = be32(table, 8u);
    const std::size_t strings = static_cast<std::size_t>(count) * 12u;
    if (count == 0u || (be32(table, 0u) & 0xFF000000u) == 0u || strings > table.size()) {
        error = "table root is not a directory or its entries overrun the table";
        return false;
    }
    if (be32(table, 4u) != AssetNamespace::kRoot) {
        error = "table root names a parent";
        return false;
    }
    std::vector<AssetNamespace::PreorderEntry> entries;
    entries.reserve(count - 1u);
    // (index, end) of each open directory: the nesting the table must agree with.
    std::vector<std::pair<u32, u32>> open{{AssetNamespace::kRoot, count}};
    for (u32 i = 1u; i < count; ++i) {
        while (open.size() > 1u && i >= open.back().second) open.pop_back();
        const u32 word0 = be32(table, static_cast<std::size_t>(i) * 12u);
        const u32 word1 = be32(table, static_cast<std::size_t>(i) * 12u + 4u);
        const u32 word2 = be32(table, static_cast<std::size_t>(i) * 12u + 8u);
        const std::size_t name_at = strings + (word0 & 0x00FFFFFFu);
        if (name_at >= table.size()) {
            error = "entry " + std::to_string(i) + " names a string outside the table";
            return false;
        }
        std::size_t name_end = name_at;
        while (name_end < table.size() && table[name_end] != 0u) ++name_end;
        if (name_end == table.size() || name_end == name_at) {
            error = "entry " + std::to_string(i) + " has an unterminated or empty name";
            return false;
        }
        AssetNamespace::PreorderEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(table.data() + name_at), name_end - name_at);
        entry.directory = (word0 & 0xFF000000u) != 0u;
        if (entry.directory) {
            // word1 is the parent the API's "../" follows; a table whose word1
            // disagrees with the nesting is not a tree and is refused.
            if (word1 != open.back().first || word2 <= i || word2 > open.back().second) {
                error = "directory entry " + std::to_string(i) + " is not nested in its parent";
                return false;
            }
            entry.end = word2;
            open.emplace_back(i, word2);
        } else {
            entry.size = word2;
        }
        entries.push_back(std::move(entry));
    }
    return AssetNamespace::from_preorder(entries, out, error);
}

std::vector<u8> fst_publication(const assets::AssetSystem& assets) {
    const assets::AssetNamespace& names = assets.names();
    std::vector<u8> image(static_cast<std::size_t>(names.reservation_bytes()), 0u);
    for (u32 entry = 0u; entry < names.count(); ++entry) {
        const assets::AssetNamespace::Entry& e = names.entry(entry);
        const std::size_t at = static_cast<std::size_t>(entry) * 12u;
        put_be32(image, at, (e.directory ? 0x01000000u : 0u) | (e.name_offset & 0x00FFFFFFu));
        put_be32(image, at + 4u, e.directory ? e.parent : assets.token_of(entry));
        put_be32(image, at + 8u, e.directory ? e.end : static_cast<u32>(e.size));
    }
    const std::string& pool = names.names();
    std::memcpy(image.data() + static_cast<std::size_t>(names.count()) * 12u, pool.data(), pool.size());
    return image;
}

}  // namespace gekko::test_support
