// Mounting the game's asset tree. Contract and rationale: include/gekko/asset_mount.h.

#include "gekko/asset_mount.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <unordered_map>

#include "gekko/profiling.h"

namespace gekko::assets {

namespace {

using shadow::runtime::v1::Status;
using shadow::runtime::v1::StorageNode;

struct FileOf {
    u64 key = 0;
    u64 size = 0;
};

// The contract's tree as the namespace's tree, and every file's (key, size) by its namespace path.
// The contract's name rule: printable ASCII, no '/' or '\\', not "." or ".." (the root's name is empty).
bool contract_name(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (const char ch : name) {
        const auto b = static_cast<unsigned char>(ch);
        if (b < 0x20u || b > 0x7Eu || b == '/' || b == '\\') return false;
    }
    return true;
}

bool convert(const StorageNode& in, AssetNamespace::Node& out, const std::string& prefix,
             std::unordered_map<std::string, FileOf>& files, std::string& error) {
    if (!prefix.empty() || !in.name.empty()) {
        if (!contract_name(in.name)) {
            error = "the storage backend gave a name outside the contract's rule under '" + prefix + "'";
            return false;
        }
    }
    out.name = in.name;
    out.directory = in.directory;
    out.size = in.size;
    const std::string here = prefix.empty() ? in.name : prefix + "/" + in.name;
    if (!in.directory) {
        if (!files.emplace(here, FileOf{in.key, in.size}).second) {
            error = "the storage backend lists " + here + " twice";
            return false;
        }
        return true;
    }
    out.children.reserve(in.children.size());
    for (const StorageNode& child : in.children) {
        out.children.emplace_back();
        if (!convert(child, out.children.back(), here, files, error)) return false;
    }
    return true;
}

}  // namespace

ByteSpan StorageProvider::map(u64 key, u64 size) {
    if (storage_ == nullptr) return {};
    const shadow::runtime::v1::ConstBytes view = storage_->view(key);
    if (view.empty() || view.size != size) return {};
    return {reinterpret_cast<const u8*>(view.data), view.size};
}

bool StorageProvider::read(u64 key, u64 offset, u64 length, u8* dest, u64& got) {
    got = 0;
    if (storage_ == nullptr) return false;
    std::uint64_t n = 0;
    const Status s = storage_->read(key, offset, shadow::runtime::v1::Bytes{reinterpret_cast<std::byte*>(dest), static_cast<std::size_t>(length)}, n);
    got = n;
    return s == Status::Ok;
}

bool AssetSystem::mount(std::unique_ptr<shadow::runtime::v1::Storage> storage, std::vector<u8> identity,
                        std::string& error) {
    AssetNamespace names;
    {
        GEKKO_PROFILE_ZONE("res/mount-namespace");
        if (storage == nullptr) {
            error = "no storage backend";
            return false;
        }
        StorageNode tree;
        std::string message;
        if (const Status s = storage->enumerate(tree, message); s != Status::Ok) {
            error = std::string(storage->info().name) + ": " + shadow::runtime::v1::to_string(s) + ": " + message;
            return false;
        }
        AssetNamespace::Node root;
        std::unordered_map<std::string, FileOf> ignored;
        if (!convert(tree, root, "", ignored, error)) return false;
        root.name.clear();
        if (!AssetNamespace::from_tree(root, names, error)) return false;
        GEKKO_PROFILE_VALUE(names.count());
    }
    return mount(std::move(storage), std::move(names), std::move(identity), error);
}

bool AssetSystem::mount(std::unique_ptr<shadow::runtime::v1::Storage> storage, AssetNamespace names,
                        std::vector<u8> identity, std::string& error) {
    GEKKO_PROFILE_ZONE("res/mount");
    error.clear();
    mounted_ = false;
    report_ = MountReport{};
    asset_of_entry_.clear();
    registry_.clear();
    space_.clear();
    storage_provider_.attach(nullptr);
    storage_.reset();
    if (storage == nullptr) {
        error = "no storage backend";
        return false;
    }
    if (names.empty()) {
        error = "the asset namespace is empty";
        return false;
    }
    // every file's key in the backend, by namespace path (the backend's tree, enumerated once)
    std::unordered_map<std::string, FileOf> files;
    {
        StorageNode tree;
        std::string message;
        if (const Status s = storage->enumerate(tree, message); s != Status::Ok) {
            error = std::string(storage->info().name) + ": " + shadow::runtime::v1::to_string(s) + ": " + message;
            return false;
        }
        AssetNamespace::Node scratch;
        tree.name.clear();
        if (!convert(tree, scratch, "", files, error)) return false;
    }
    storage_ = std::move(storage);
    storage_provider_.attach(storage_.get());
    names_ = std::move(names);
    identity_ = std::move(identity);

    const auto file_of = [&files, this](u32 entry) -> const FileOf* {
        const auto it = files.find(names_.path(entry));
        return it == files.end() ? nullptr : &it->second;
    };
    // A namespace given by the caller may name a file the backend does not hold (a test of the failure path, a table
    // naming a file the medium lost): it is mounted with a key the backend never gave out, so reading it fails
    // (ReadStatus::Failed: "the asset exists and the host could not produce bytes"), as the host mount always did.
    // Its size is the namespace's. A namespace enumerated from the backend (the other overload) has no such file.
    constexpr u64 kAbsent = ~0ull;

    // Replacements first: a replaced file's size is the replacement's, and the
    // token space below is laid out from the sizes.
    std::vector<s64> replacement_of_entry(names_.count(), -1);
    replaced_count_ = 0;
    replacement_errors_.clear();
    const TreeReader read_tree = [this, &file_of](std::string_view path, std::vector<u8>& bytes) {
        const s32 entry = names_.resolve(path);
        if (entry <= 0 || names_.is_directory(static_cast<u32>(entry))) return false;
        const FileOf* f = file_of(static_cast<u32>(entry));
        if (f == nullptr) return false;
        bytes.assign(static_cast<std::size_t>(f->size), 0u);
        std::uint64_t got = 0;
        return storage_->read(f->key, 0, shadow::runtime::v1::Bytes{reinterpret_cast<std::byte*>(bytes.data()), bytes.size()},
                              got) == Status::Ok &&
               got == f->size;
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

    for (const Switchable& sw : pending_switchables_) {
        const s32 entry = names_.resolve(sw.path);
        if (entry <= 0 || names_.is_directory(static_cast<u32>(entry))) {
            replacement_errors_.push_back(sw.path + ": no such file in the asset tree");
            continue;
        }
        std::vector<std::vector<u8>> built;
        std::string build_error;
        bool ok = true;
        std::size_t largest = 0;
        for (const Replacement& variant : sw.variants) {
            std::vector<u8> bytes;
            if (!variant(read_tree, bytes, build_error)) {
                ok = false;
                break;
            }
            largest = std::max(largest, bytes.size());
            built.push_back(std::move(bytes));
        }
        if (!ok || built.empty()) {
            replacement_errors_.push_back(sw.path + ": " + build_error);
            continue;
        }
        names_.set_file_size(static_cast<u32>(entry), largest);
        replacement_of_entry[static_cast<std::size_t>(entry)] =
            static_cast<s64>(replacements_.add_switchable(std::move(built), sw.selector, sw.path));
        ++replaced_count_;
    }
    for (const Virtual& v : pending_virtuals_) {
        const s32 entry = names_.resolve(v.path);
        if (entry <= 0 || names_.is_directory(static_cast<u32>(entry))) {
            replacement_errors_.push_back(v.path + ": no such file in the asset tree");
            continue;
        }
        names_.set_file_size(static_cast<u32>(entry), v.size);
        replacement_of_entry[static_cast<std::size_t>(entry)] =
            static_cast<s64>(replacements_.add_virtual(v.size, v.reader, v.path));
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
                    : registry_.intern(path, e.size, ProviderKind::Storage,
                                       file_of(entry) != nullptr ? file_of(entry)->key : kAbsent);
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
    return true;
}

void AssetSystem::replace(std::string path, Replacement build) {
    pending_replacements_.emplace_back(std::move(path), std::move(build));
}

void AssetSystem::replace_switchable(std::string path, std::vector<Replacement> variants,
                                     std::function<std::size_t()> selector) {
    pending_switchables_.push_back(Switchable{std::move(path), std::move(variants), std::move(selector)});
}

void AssetSystem::replace_virtual(std::string path, u64 size, VirtualReader reader) {
    pending_virtuals_.push_back(Virtual{std::move(path), size, std::move(reader)});
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
