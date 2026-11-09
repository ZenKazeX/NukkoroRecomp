#pragma once

// Mounting the game's asset tree.
//
// The boot-time step that turns the extracted files/ directory into the three
// things the runtime serves the game's file API from:
//
//   AssetNamespace     the tree itself: names, nesting, entry numbers, 64-bit
//                      sizes, an O(1) per-component lookup (asset_namespace.h)
//   AssetRegistry      one AssetId per file, in entry order
//   AssetAddressSpace  the 32-bit token the guest carries for each file in
//                      DVDFileInfo.startAddr; the only arithmetic the guest
//                      ever performs on it is `token + offset`
//
// The source of truth is the runtime contract's storage backend
// (shadow/runtime/v1/runtime.h, runtime/backends/storage): the tree it
// enumerates and the bytes it reads. This file does no host I/O and knows no
// disc: whether the bytes come from the extracted tree on the host or from a
// GameCube disc image is the backend's business (docs/RUNTIME_CONTRACT.md).
// The core sorts the tree itself, so the namespace - and every guest token -
// is the same whichever backend serves it.
//
// See docs/native-asset-pipeline-20261005/README.md.

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gekko/asset_namespace.h"
#include "gekko/asset_providers.h"
#include "gekko/asset_runtime.h"
#include "gekko/types.h"
#include "shadow/runtime/v1/runtime.h"

namespace gekko::assets {

struct MountReport {
    u32 files = 0;            // file entries, each with an AssetId and a token
    u32 directories = 0;      // directory entries, the root included
    u64 declared_bytes = 0;   // sum of the file sizes
    u64 address_span = 0;     // bytes of 32-bit token space the assets occupy
    u64 table_bytes = 0;      // RAM the token decode table costs
};

// The contract's storage backend as an asset provider: map = the backend's
// view (empty when it has none), read = the backend's read (zero-fill past the
// end, as the contract specifies).
class StorageProvider final : public AssetProvider {
public:
    void attach(shadow::runtime::v1::Storage* storage) noexcept { storage_ = storage; }
    const char* name() const noexcept override { return "storage"; }
    ByteSpan map(u64 key, u64 size) override;
    bool read(u64 key, u64 offset, u64 length, u8* dest, u64& got) override;

private:
    shadow::runtime::v1::Storage* storage_ = nullptr;
};

// ---------------------------------------------------------------------------
// AssetSystem - the whole storage layer, assembled
// ---------------------------------------------------------------------------
//
// One object owns the namespace, identity, addressing, providers and
// lifetime, and exposes what the native file API needs: the namespace, the
// token of a file entry, and a read keyed by the token the guest carries.
//
// There is no disc source, no residency budget, no I/O engine and no second
// copy of anything. A read is a decode and one copy out of a mapped page into
// the destination the caller names.
class AssetSystem {
public:
    AssetSystem() : resources_(registry_) {
        resources_.set_provider(ProviderKind::Storage, &storage_provider_);
        resources_.set_provider(ProviderKind::Memory, &replacements_);
    }

    // Native content for a game file. At the next mount `build` produces the
    // bytes served under `path` (the namespace's path, ASCII case folded)
    // instead of the host file, with their size; `read` gives it any file of
    // the tree (the original included). Must precede the mount. A build that
    // fails, or a path the tree does not have, leaves the host file served and
    // is listed in replacement_errors().
    using TreeReader = std::function<bool(std::string_view path, std::vector<u8>& bytes)>;
    using Replacement = std::function<bool(const TreeReader& read, std::vector<u8>& bytes, std::string& error)>;
    void replace(std::string path, Replacement build);
    // A file with several native contents, one served at a time by `selector` (asked at every read; MemoryProvider::
    // add_switchable): every variant is built at the mount, the file's declared size is the largest one's and a
    // shorter variant reads as zeros past its end. The setting `selector` reads takes effect at the next read, with
    // no remount. Same failure rules as replace(): a variant that fails to build leaves the host file served.
    void replace_switchable(std::string path, std::vector<Replacement> variants, std::function<std::size_t()> selector);
    // A file whose bytes are produced at every read by `reader` (offset, length, dest, got) - nothing is held in
    // memory or built at the mount; `size` is its declared size. For a large composed file whose content follows a
    // runtime setting (the switchable dialogue-voice archive, gekko/voice_language.h).
    using VirtualReader = std::function<bool(u64 offset, u64 length, u8* dest, u64& got)>;
    void replace_virtual(std::string path, u64 size, VirtualReader reader);
    u32 replaced_count() const noexcept { return replaced_count_; }
    const std::vector<std::string>& replacement_errors() const noexcept { return replacement_errors_; }

    // Mounts the tree a storage backend enumerates, and keeps the backend for
    // the session (every read goes to it). `identity` is the 32-byte block the
    // game's drive-identity probe reads to learn which title is mounted
    // (DVDInquiryAsync / DVDReadDiskID): a game-visible datum, served directly,
    // never through the token space. A partial mount is never kept. Host
    // directory trees mount through runtime/backends/storage
    // (shadow::backends::mount_host_directory).
    bool mount(std::unique_ptr<shadow::runtime::v1::Storage> storage, std::vector<u8> identity, std::string& error);
    // The same with the namespace already built (tests, a filtered tree): every
    // file entry of `names` must exist in the backend's tree with its size.
    bool mount(std::unique_ptr<shadow::runtime::v1::Storage> storage, AssetNamespace names,
               std::vector<u8> identity, std::string& error);
    // The backend serving this mount (null before a mount).
    const shadow::runtime::v1::Storage* storage() const noexcept { return storage_.get(); }

    bool mounted() const noexcept { return mounted_; }
    const AssetNamespace& names() const noexcept { return names_; }
    const std::vector<u8>& identity_block() const noexcept { return identity_; }
    const MountReport& report() const noexcept { return report_; }

    // The registry identity and the guest token of a file entry; an invalid id
    // and 0 for a directory or an unknown entry.
    AssetId asset_of(u32 entry) const noexcept {
        return entry < asset_of_entry_.size() ? asset_of_entry_[entry] : AssetId{};
    }
    u32 token_of(u32 entry) const noexcept { return space_.base_of(asset_of(entry)); }

    enum class ReadStatus : u8 {
        Ok = 0,
        Unmapped,     // the token names no asset
        OutOfRange,   // inside an asset, but past what a legal read can reach
        Failed,       // the asset exists and the host could not produce bytes
    };

    // The bridge. `token` is fileInfo->startAddr + caller offset, exactly as
    // the guest computed it. Bytes past the asset's size read as zero, which
    // is the contract every issuer depends on (each rounds its length up to a
    // multiple of 32). `dest` may be guest memory itself: this is the only
    // copy a read makes.
    ReadStatus read(u32 token, u64 length, u8* dest, u64& got);

    // read() in its two halves, for a caller that must know the request is
    // good before it hands out a destination (the file API writes guest
    // memory in place): locate() decodes and counts a refusal; copy() moves
    // the bytes of a located request. A Located is valid only until the next
    // locate().
    struct Located {
        AssetId id{};
        u64 offset = 0;
        u64 length = 0;
    };
    ReadStatus locate(u32 token, u64 length, Located& out);
    bool copy(const Located& located, u8* dest, u64& got);

    // A rewrite of what a read hands the guest, after the copy (gekko/button_glyphs.h): the asset, the file
    // offset of dest[0] and the bytes copied. It runs on the reading thread and must keep the size. Unset: none.
    using ReadPatch = std::function<void(AssetId id, u64 offset, u8* dest, u64 length)>;
    void set_read_patch(ReadPatch patch) { read_patch_ = std::move(patch); }

    // What the most recent read resolved to. Reporting only.
    const std::string& last_path() const noexcept { return last_path_; }
    u64 last_offset() const noexcept { return last_offset_; }

    // The most recent Sofdec stream any read touched (one suffix test per
    // read; its consumer used to rescan every touched file per poll).
    const std::string& last_movie_path() const noexcept { return last_movie_path_; }

    // How many distinct assets this session has read (one indexed write per
    // read into a bitset sized at mount).
    u32 touched_count() const noexcept { return touched_count_; }
    void set_track_touched(bool on) noexcept { track_touched_ = on; }
    const std::vector<std::string>& touched() const noexcept { return touched_; }

    struct Stats {
        u64 reads = 0;
        u64 bytes = 0;
        u64 unmapped = 0;      // MUST read zero: a token that names no asset
        u64 out_of_range = 0;  // MUST read zero: a request leaving its asset
        u64 failed = 0;
        // Wall time inside read(), on whatever thread called it: the copy
        // plus whatever page fault a first touch takes.
        u64 read_ns = 0;
        u64 read_ns_max = 0;
    };
    Stats stats() const noexcept { return stats_; }

    AssetRegistry& registry() noexcept { return registry_; }
    const AssetRegistry& registry() const noexcept { return registry_; }
    ResourceManager& resources() noexcept { return resources_; }
    const AssetAddressSpace& space() const noexcept { return space_; }

private:
    AssetNamespace names_;
    AssetRegistry registry_;
    std::unique_ptr<shadow::runtime::v1::Storage> storage_;
    StorageProvider storage_provider_;
    MemoryProvider replacements_;
    ReadPatch read_patch_;
    std::vector<std::pair<std::string, Replacement>> pending_replacements_;
    struct Switchable {
        std::string path;
        std::vector<Replacement> variants;
        std::function<std::size_t()> selector;
    };
    std::vector<Switchable> pending_switchables_;
    struct Virtual {
        std::string path;
        u64 size;
        VirtualReader reader;
    };
    std::vector<Virtual> pending_virtuals_;
    std::vector<std::string> replacement_errors_;
    u32 replaced_count_ = 0;
    ResourceManager resources_;
    AssetAddressSpace space_;
    std::vector<AssetId> asset_of_entry_;  // index = entry number
    std::vector<u8> identity_;
    MountReport report_;
    bool mounted_ = false;
    std::string last_path_;
    std::string last_movie_path_;
    u64 last_offset_ = 0;
    std::vector<bool> seen_;  // index = AssetId::value
    std::vector<std::string> touched_;
    u32 touched_count_ = 0;
    bool track_touched_ = false;
    Stats stats_{};
};

}  // namespace gekko::assets
