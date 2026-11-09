// Mounting the game's asset tree. Contract and rationale: include/gekko/asset_mount.h.

#include "gekko/asset_mount.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>

#include "gekko/profiling.h"

namespace gekko::assets {

bool AssetSystem::mount(const std::filesystem::path& files_root, std::vector<u8> identity,
                        std::string& error) {
    AssetNamespace names;
    {
        // The whole host walk: one directory enumeration per directory (the
        // sizes come with the listing), no per-file stat, no table.
        GEKKO_PROFILE_ZONE("res/mount-namespace");
        if (!AssetNamespace::from_directory(files_root, names, error)) return false;
        GEKKO_PROFILE_VALUE(names.count());
    }
    return mount(files_root, std::move(names), std::move(identity), error);
}

bool AssetSystem::mount(const std::filesystem::path& files_root, AssetNamespace names,
                        std::vector<u8> identity, std::string& error) {
    GEKKO_PROFILE_ZONE("res/mount");
    error.clear();
    mounted_ = false;
    report_ = MountReport{};
    asset_of_entry_.clear();
    registry_.clear();
    space_.clear();
    if (names.empty()) {
        error = "the asset namespace is empty";
        return false;
    }
    names_ = std::move(names);
    identity_ = std::move(identity);

    // Replacements first: a replaced file's size is the replacement's, and the
    // token space below is laid out from the sizes.
    std::vector<s64> replacement_of_entry(names_.count(), -1);
    replaced_count_ = 0;
    replacement_errors_.clear();
    const TreeReader read_tree = [this, &files_root](std::string_view path, std::vector<u8>& bytes) {
        const s32 entry = names_.resolve(path);
        if (entry <= 0 || names_.is_directory(static_cast<u32>(entry))) return false;
        std::ifstream input(files_root / names_.path(static_cast<u32>(entry)), std::ios::binary);
        if (!input) return false;
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        return !input.bad();
    };
    for (const auto& [path, build] : pending_replacements_) {
        const s32 entry = names_.resolve(path);
        if (entry <= 0 || names_.is_directory(static_cast<u32>(entry))) {
            replacement_errors_.push_back(path + ": no such file in the asset tree");
            continue;
        }
        std::vector<u8> bytes;
        std::string build_error;
        if (!build(read_tree, bytes, build_error)) {
            replacement_errors_.push_back(path + ": " + build_error);
            continue;
        }
        names_.set_file_size(static_cast<u32>(entry), bytes.size());
        replacement_of_entry[static_cast<std::size_t>(entry)] =
            static_cast<s64>(replacements_.add(std::move(bytes)));
        ++replaced_count_;
    }

    // One AssetId per file, in entry order: the token space is laid out in
    // that order, so a file's guest token depends only on the tree.
    asset_of_entry_.assign(names_.count(), AssetId{});
    {
        GEKKO_PROFILE_ZONE("res/mount-registry");
        for (u32 entry = 1u; entry < names_.count(); ++entry) {
            const AssetNamespace::Entry& e = names_.entry(entry);
            if (e.directory) continue;
            const std::string path = names_.path(entry);
            const s64 replacement = replacement_of_entry[entry];
            asset_of_entry_[entry] =
                replacement >= 0
                    ? registry_.intern(path, e.size, ProviderKind::Memory, static_cast<u64>(replacement))
                    : registry_.intern(path, e.size, ProviderKind::File, files_.add(files_root / path));
        }
    }
    if (!space_.build(registry_)) {
        error = "assets do not fit in 32 bits of guest token space (" +
                std::to_string(registry_.count()) + " assets)";
        asset_of_entry_.clear();
        registry_.clear();
        return false;
    }

    report_.files = names_.file_count();
    report_.directories = names_.directory_count();
    report_.declared_bytes = names_.total_bytes();
    report_.address_span = space_.span_bytes();
    report_.table_bytes = space_.table_bytes();
    GEKKO_PROFILE_PLOT("res/mount-files", static_cast<double>(report_.files));
    GEKKO_PROFILE_PLOT("res/mount-directories", static_cast<double>(report_.directories));
    GEKKO_PROFILE_PLOT("res/mount-declared-bytes", static_cast<double>(report_.declared_bytes));
    GEKKO_PROFILE_PLOT("res/mount-address-span", static_cast<double>(report_.address_span));
    GEKKO_PROFILE_PLOT("res/mount-table-bytes", static_cast<double>(report_.table_bytes));
    seen_.assign(registry_.count() + 1u, false);
    touched_.clear();
    touched_count_ = 0;
    mounted_ = true;
    // The host's file opens move off the game thread's reads (asset_providers.h FileProvider::preopen; nothing a
    // read returns changes). GEKKO_ASSET_PREOPEN=0 is the measurement's control.
    files_.preopen();
    return true;
}

void AssetSystem::replace(std::string path, Replacement build) {
    pending_replacements_.emplace_back(std::move(path), std::move(build));
}

AssetSystem::ReadStatus AssetSystem::read(u32 token, u64 length, u8* dest, u64& got) {
    got = 0;
    if (length != 0u && dest == nullptr) return ReadStatus::Failed;
    Located located;
    const ReadStatus status = locate(token, length, located);
    if (status != ReadStatus::Ok || length == 0u) return status;
    return copy(located, dest, got) ? ReadStatus::Ok : ReadStatus::Failed;
}

AssetSystem::ReadStatus AssetSystem::locate(u32 token, u64 length, Located& out) {
    // The decode half of a request: token -> (asset, offset), first-touch
    // bookkeeping, the registry lookup for the trace's name.
    out = Located{};
    last_path_.clear();
    last_offset_ = 0;
    if (length == 0u) return ReadStatus::Ok;

    AssetAddressSpace::Decoded decoded;
    switch (space_.decode(token, length, decoded)) {
        case AssetAddressSpace::Status::Unmapped:
            // MUST read zero: guest code derived a token from something other
            // than an opened file (Unmapped), or issued a request that would
            // have returned the NEXT file's bytes (OutOfRange).
            ++stats_.unmapped;
            GEKKO_PROFILE_MESSAGE_LITERAL("res/token-unmapped");
            GEKKO_PROFILE_PLOT("res/unmapped", static_cast<double>(stats_.unmapped));
            return ReadStatus::Unmapped;
        case AssetAddressSpace::Status::OutOfRange:
            ++stats_.out_of_range;
            GEKKO_PROFILE_MESSAGE_LITERAL("res/token-out-of-range");
            GEKKO_PROFILE_PLOT("res/out-of-range", static_cast<double>(stats_.out_of_range));
            return ReadStatus::OutOfRange;
        case AssetAddressSpace::Status::Ok:
            break;
    }

    if (decoded.id.value < seen_.size() && !seen_[decoded.id.value]) {
        seen_[decoded.id.value] = true;
        ++touched_count_;
        GEKKO_PROFILE_PLOT("res/assets-touched", static_cast<double>(touched_count_));
        if (track_touched_) {
            if (const AssetRecord* first = registry_.get(decoded.id)) touched_.push_back(first->path);
        }
    }
    if (const AssetRecord* record = registry_.get(decoded.id)) {
        last_path_ = record->path;
        if (last_path_.size() >= 4u) {
            const char* tail = last_path_.c_str() + last_path_.size() - 4u;
            if (tail[0] == '.' && (tail[1] | 0x20) == 's' && (tail[2] | 0x20) == 'f' &&
                (tail[3] | 0x20) == 'd') {
                last_movie_path_ = last_path_;
            }
        }
    }
    last_offset_ = decoded.offset;
    out.id = decoded.id;
    out.offset = decoded.offset;
    out.length = length;
    return ReadStatus::Ok;
}

bool AssetSystem::copy(const Located& located, u8* dest, u64& got) {
    // The copy half: one memcpy out of the provider's mapped view (zero-fill
    // past the end of the file) into `dest`.
    GEKKO_PROFILE_ZONE("res/read");
    GEKKO_PROFILE_VALUE(located.length);
    got = 0;
    if (located.length == 0u) return true;
    if (dest == nullptr || !located.id.valid()) {
        ++stats_.failed;
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    const bool ok = resources_.read(located.id, located.offset, located.length, dest, got);
    const auto elapsed = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    stats_.read_ns += elapsed;
    if (elapsed > stats_.read_ns_max) stats_.read_ns_max = elapsed;
    if (!ok) {
        ++stats_.failed;
        return false;
    }
    ++stats_.reads;
    stats_.bytes += got;
    if (read_patch_) read_patch_(located.id, located.offset, dest, got);
    return true;
}

}  // namespace gekko::assets
