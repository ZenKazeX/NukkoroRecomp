#pragma once

// The native asset namespace: the game's file tree as an immutable, indexed,
// 64-bit database.
//
// ---------------------------------------------------------------------------
// WHAT THIS REPLACES
// ---------------------------------------------------------------------------
//
// The GameCube "FST": a 12-byte-per-entry table plus a string table that the
// apploader parked at the top of MEM1, which the Nintendo DVD SDK walked in
// guest memory to turn a path string into an entry number. Here the tree is
// built once, on the host, from the host filesystem (the extracted files/
// directory) - no table is read, no table is published into guest memory, and
// nothing in the game walks guest memory to find a file.
//
// ---------------------------------------------------------------------------
// WHAT IS PRESERVED, AND WHY
// ---------------------------------------------------------------------------
//
// Only what the game can observe through the file API it calls:
//
//  * LOOKUP. resolve() implements exactly the path grammar the API defines
//    (docs/dvd-ownership-20261005/CONTRACTS.md, DVDConvertPathToEntrynum):
//    '/' restarts at the root, "./" and "../" step, "." and ".." end, every
//    other run up to '/' or NUL is one name; a name followed by '/' matches
//    only a directory; a final name matches a file or a directory, first in
//    entry order. Names compare with ASCII A-Z folded and every other byte
//    exact - the original's tolower over MSL's __lower_map, which the
//    reference test proves equal to fold() for all 256 bytes.
//
//  * ENTRY NUMBERS. They are stable: a pre-order walk where each directory's
//    children are sorted by folded name, then by raw bytes. That ordering
//    reproduces the disc's own numbering for every one of its 3,615 entries
//    (asset_namespace_tests, against sys/fst.bin as an oracle).
//
//  * NAMES. Original case, NUL-terminated, laid out in entry order (the
//    first child of the root at offset 0). The game's only directory scan
//    reads them from here natively (docs/DISC_SYSTEM_MAP.md cut 2); none is
//    ever written into guest memory.
//
//  * SIZES. 64-bit natively. The guest's DVDFileInfo.length is 32-bit; the
//    API boundary refuses a file it cannot describe instead of truncating.
//
// What is NOT preserved: a table layout, a string-table address, a "disc
// offset", the FST's word1 overload (parent for a directory, offset for a
// file), any 32-bit limit on the host side, any sector or alignment rule.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gekko/types.h"

namespace gekko::assets {

class AssetNamespace {
public:
    static constexpr u32 kRoot = 0u;
    static constexpr s32 kNotFound = -1;

    struct Entry {
        u32 parent = 0;       // containing directory (the root's own is kRoot)
        u32 end = 0;          // one past the last entry of this subtree; a file: index + 1
        u32 name_offset = 0;  // into names(); the root's name is empty
        u32 name_length = 0;
        u64 size = 0;         // files: bytes; directories: 0
        bool directory = false;
    };

    // One node of a tree description: what a builder is given. Children are
    // ordered by the builder, never by the caller.
    struct Node {
        std::string name;
        bool directory = false;
        u64 size = 0;
        std::vector<Node> children;
    };

    // The production source: a host directory tree. Every regular file is an
    // entry with its host size; every directory is an entry. Names must be
    // printable ASCII without '/' (the game's names are; anything else is
    // refused rather than transcoded).
    static bool from_directory(const std::filesystem::path& root, AssetNamespace& out,
                               std::string& error);

    // A tree description (tests; any future packed index).
    static bool from_tree(const Node& root, AssetNamespace& out, std::string& error);

    // Entries 1..n of a namespace already in pre-order, taken exactly in the
    // order given (the root is implied as entry 0 and must not be listed).
    // A directory's `end` is one past the last entry of its subtree; a file's
    // is ignored. Nothing is reordered, so a caller can state any order the
    // API's "first in entry order" rule must honour - the tests use it to
    // hand the native API the very tables the original SDK walked, decoded
    // by their own oracle (runtime/tests/support/fst_oracle.h). No table
    // format is known here.
    struct PreorderEntry {
        std::string name;
        bool directory = false;
        u32 end = 0;
        u64 size = 0;
    };
    static bool from_preorder(const std::vector<PreorderEntry>& entries, AssetNamespace& out,
                              std::string& error);

    u32 count() const noexcept { return static_cast<u32>(entries_.size()); }
    bool empty() const noexcept { return entries_.empty(); }
    const Entry& entry(u32 index) const noexcept { return entries_[index]; }
    bool valid(u32 index) const noexcept { return index < entries_.size(); }
    bool is_directory(u32 index) const noexcept {
        return index < entries_.size() && entries_[index].directory;
    }
    std::string_view name(u32 index) const noexcept {
        const Entry& e = entries_[index];
        return std::string_view(names_.data() + e.name_offset, e.name_length);
    }
    // "dir/sub/file" (no leading '/'); the root is "".
    std::string path(u32 index) const;

    // A file entry's size, for content the runtime serves in place of the
    // host file. False for a directory or an unknown entry.
    bool set_file_size(u32 index, u64 size) noexcept {
        if (index >= entries_.size() || entries_[index].directory) return false;
        total_bytes_ = total_bytes_ - entries_[index].size + size;
        entries_[index].size = size;
        return true;
    }

    // The API's path grammar from directory `current` (the guest API's
    // current directory, always the root). kNotFound when nothing matches.
    // `path` stops at its first NUL, as the guest string does.
    s32 resolve(std::string_view path, u32 current = kRoot) const;

    // The first child of `directory` named `name` (folded), in entry order,
    // optionally restricted to directories. kNotFound when there is none.
    s32 find_child(u32 directory, std::string_view name, bool directories_only) const;

    // Every name, NUL-terminated, in entry order (the root contributes none).
    const std::string& names() const noexcept { return names_; }
    // The size the disc's directory table had: one 12-byte record per entry
    // plus every name. The boot keeps the arena that far below BI2 (the
    // boundary the game was mastered with); nothing is stored there.
    u64 reservation_bytes() const noexcept {
        return static_cast<u64>(entries_.size()) * 12u + static_cast<u64>(names_.size());
    }

    u32 file_count() const noexcept { return files_; }
    u32 directory_count() const noexcept { return directories_; }
    u64 total_bytes() const noexcept { return total_bytes_; }

    // The name equality the API defines: ASCII A-Z folded, every other byte
    // exact.
    static constexpr u8 fold(u8 byte) noexcept {
        return (byte >= 'A' && byte <= 'Z') ? static_cast<u8>(byte + ('a' - 'A')) : byte;
    }

private:
    struct Key {
        u32 directory;
        std::string folded;
        bool operator==(const Key& o) const noexcept {
            return directory == o.directory && folded == o.folded;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept;
    };

    void clear() noexcept;
    bool finish(std::string& error);  // validates the pre-order and builds the index

    std::vector<Entry> entries_;
    std::string names_;
    // (directory, folded name) -> matching children in entry order. One
    // element for every name of a case-insensitively unique tree.
    std::unordered_map<Key, std::vector<u32>, KeyHash> children_;
    u32 files_ = 0;
    u32 directories_ = 0;
    u64 total_bytes_ = 0;
};

}  // namespace gekko::assets
