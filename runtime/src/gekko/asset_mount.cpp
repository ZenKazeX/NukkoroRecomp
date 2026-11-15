// Mounting the game's asset tree. Contract and rationale: include/gekko/asset_mount.h.

#include "gekko/asset_mount.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "gekko/cs2_ram.h"
#include "gekko/profiling.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace gekko::assets {

bool pf_load_stamps_on() noexcept {
    static const bool on = [] {
        const char* e = std::getenv("GEKKO_PF_LOAD_STAMP");
        return e != nullptr && e[0] == '1';
    }();
    return on;
}

void pf_load_stamp(const char* label, u64 tick) noexcept {
    if (!pf_load_stamps_on()) return;
#if defined(_WIN32)
    const auto ms = [](FILETIME f) {
        return static_cast<double>((static_cast<u64>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000.0;
    };
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    FILETIME tc{}, te{}, tk{}, tu{};
    GetThreadTimes(GetCurrentThread(), &tc, &te, &tk, &tu);
    ULONG64 thread_cycles = 0, process_cycles = 0;  // exact (the times above count whole scheduler ticks)
    QueryThreadCycleTime(GetCurrentThread(), &thread_cycles);
    QueryProcessCycleTime(GetCurrentProcess(), &process_cycles);
    std::fprintf(stderr,
                 "[pf-load] %s uptime_ms %.1f thread_cpu_ms %.1f process_cpu_ms %.1f tick %llu thread_mcycles %.1f "
                 "process_mcycles %.1f\n",
                 label, ms(now) - ms(created), ms(tk) + ms(tu), ms(kernel) + ms(user), static_cast<unsigned long long>(tick),
                 static_cast<double>(thread_cycles) / 1.0e6, static_cast<double>(process_cycles) / 1.0e6);
#else
    (void)label;
    (void)tick;
#endif
}

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

// cs2/ram: the ranges where `bytes` differs from `original` (same size), runs closer than kGap bytes merged. Sorted and
// disjoint. A patch is the replacement's own bytes for its range.
std::vector<MemoryProvider::Patch> diff_patches(const std::vector<u8>& original, const std::vector<u8>& bytes,
                                                u64& patch_bytes) {
    constexpr std::size_t kGap = 64;
    std::vector<MemoryProvider::Patch> patches;
    patch_bytes = 0;
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i < n) {
        if (original[i] == bytes[i]) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        std::size_t last = i;
        for (std::size_t j = i + 1; j < n && j - last <= kGap; ++j)
            if (original[j] != bytes[j]) last = j;
        MemoryProvider::Patch patch;
        patch.offset = start;
        patch.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                           bytes.begin() + static_cast<std::ptrdiff_t>(last + 1));
        patch_bytes += patch.bytes.size();
        patches.push_back(std::move(patch));
        i = last + 1;
    }
    return patches;
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
        pf_load_stamp("mount_begin");
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
        const bool ok = storage_->read(f->key, 0, shadow::runtime::v1::Bytes{reinterpret_cast<std::byte*>(bytes.data()), bytes.size()},
                                       got) == Status::Ok &&
                        got == f->size;
        if (ok && cs2_ram::enabled()) {  // read once, whole: the mapped pages need not stay in the working set
            const shadow::runtime::v1::ConstBytes view = storage_->view(f->key);
            release_view_pages(view.data, view.size);
        }
        return ok;
    };
    // cs2/ram: a replacement is kept as patches over its original (mapped, shared with the file cache) when it is the
    // original's own size and its changed ranges are at most 7/8 of it; otherwise as a full private copy. The
    // delta is checked against the bytes it replaces before it is used (the original with the patches laid over it
    // must equal them), so a read returns exactly what a full copy would.
    // The delta decision of hold() below: the patches and the original's key when the content is kept as patches,
    // nothing when it is kept as a full copy. Reads only the original tree (callable from the prebuild's threads).
    const DeltaOf delta_of = [&](const std::string& path, const std::vector<u8>& bytes) -> std::optional<PreparedDelta> {
        if (cs2_ram::enabled() && !bytes.empty()) {
            const s32 entry = names_.resolve(path);
            const FileOf* f = entry > 0 ? file_of(static_cast<u32>(entry)) : nullptr;
            if (f != nullptr && f->size == bytes.size()) {
                std::vector<u8> original(bytes.size());
                std::uint64_t got = 0;
                if (storage_->read(f->key, 0,
                                   shadow::runtime::v1::Bytes{reinterpret_cast<std::byte*>(original.data()), original.size()},
                                   got) == Status::Ok &&
                    got == original.size()) {
                    const shadow::runtime::v1::ConstBytes view = storage_->view(f->key);
                    release_view_pages(view.data, view.size);
                    u64 patch_bytes = 0;
                    std::vector<MemoryProvider::Patch> patches = diff_patches(original, bytes, patch_bytes);
                    if (patch_bytes + 32u * patches.size() <= bytes.size() - bytes.size() / 8u) {
                        for (const MemoryProvider::Patch& p : patches)
                            std::copy(p.bytes.begin(), p.bytes.end(), original.begin() + static_cast<std::ptrdiff_t>(p.offset));
                        if (original == bytes) return PreparedDelta{f->key, std::move(patches)};
                    }
                }
            }
        }
        return std::nullopt;
    };
    // `prepared`: delta_of's answer for these bytes, made ahead by the prebuild (else made here).
    const auto hold = [&](const std::string& path, std::vector<u8>&& bytes,
                          std::optional<std::optional<PreparedDelta>> prepared = std::nullopt) -> u64 {
        std::optional<PreparedDelta> delta = prepared.has_value() ? std::move(*prepared) : delta_of(path, bytes);
        if (delta.has_value()) {
            const u64 key = replacements_.add_delta(&storage_provider_, delta->base_key, bytes.size(), std::move(delta->patches));
            bytes = std::vector<u8>();
            return key;
        }
        return replacements_.add(std::move(bytes));
    };
    // pf-load (GEKKO_PF_LOAD_MOUNT_JOBS, default 4 threads; =0 builds one after another as before): every replacement
    // is BUILT first, and its hold's delta decision made, on several host threads; then the
    // results are taken below in the original order, exactly as the sequential loop took them (sizes, holds, keys,
    // errors). The builds and the delta decisions read only the original tree; a build reads only the original
    // tree (read_tree: the backend's bytes, never a replacement) and its own captures, so its bytes do not depend on
    // which thread made them or when. Host timing only: all of this precedes the first guest instruction.
    std::vector<PrebuiltReplacement> prebuilt = prebuild_replacements(read_tree, delta_of);
    std::size_t prebuilt_index = 0;
    for (const auto& [path, build] : pending_replacements_) {
        PrebuiltReplacement* const ready = prebuilt.empty() ? nullptr : &prebuilt[prebuilt_index++];
        const s32 entry = names_.resolve(path);
        if (entry <= 0 || names_.is_directory(static_cast<u32>(entry))) {
            replacement_errors_.push_back(path + ": no such file in the asset tree");
            continue;
        }
        std::vector<u8> bytes;
        std::string build_error;
        std::optional<std::optional<PreparedDelta>> delta;
        if (ready != nullptr ? !ready->take(bytes, build_error, delta) : !build(read_tree, bytes, build_error)) {
            replacement_errors_.push_back(path + ": " + build_error);
            continue;
        }
        names_.set_file_size(static_cast<u32>(entry), bytes.size());
        replacement_of_entry[static_cast<std::size_t>(entry)] =
            static_cast<s64>(hold(path, std::move(bytes), std::move(delta)));
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
    pf_load_stamp("mount_end");
    return true;
}

void AssetSystem::release_pages(AssetId id) noexcept {
    if (!cs2_ram::enabled() || storage_ == nullptr) return;
    const AssetRecord* record = registry_.get(id);
    if (record == nullptr || record->provider != ProviderKind::Storage) return;
    const shadow::runtime::v1::ConstBytes view = storage_->view(record->key);
    release_view_pages(view.data, view.size);
}

bool AssetSystem::PrebuiltReplacement::take(std::vector<u8>& out, std::string& build_error,
                                             std::optional<std::optional<PreparedDelta>>& out_delta) {
    if (built) {
        out = std::move(bytes);
        out_delta = std::move(delta);
        return true;
    }
    if (exception != nullptr) std::rethrow_exception(exception);
    build_error = error;
    return false;
}

std::vector<AssetSystem::PrebuiltReplacement> AssetSystem::prebuild_replacements(const TreeReader& read,
                                                                                 const DeltaOf& delta_of) {
    std::vector<PrebuiltReplacement> out;
    // GEKKO_PF_LOAD_MOUNT_JOBS: 0 = the sequential loop; N = N threads (the mount's own and N - 1 helpers);
    // unset = kDefaultMountThreads.
    constexpr std::size_t kDefaultMountThreads = 4;  // 2026-11-14 sweep: 8 saves no wall time over 4 and costs cycles
    std::size_t wanted = kDefaultMountThreads;
    if (const char* e = std::getenv("GEKKO_PF_LOAD_MOUNT_JOBS"); e != nullptr && e[0] != '\0') {
        wanted = static_cast<std::size_t>(std::strtoul(e, nullptr, 10));
        if (wanted == 0u) return out;
    }
    const std::size_t count = pending_replacements_.size();
    if (count == 0u) return out;
    out.resize(count);
    // The builds of the entries that resolve to a file (the sequential loop builds no other); names_ is only read here.
    std::vector<std::size_t> jobs;
    jobs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const s32 entry = names_.resolve(pending_replacements_[i].first);
        if (entry > 0 && !names_.is_directory(static_cast<u32>(entry))) jobs.push_back(i);
    }
    const auto run = [&](std::size_t i) {
        PrebuiltReplacement& r = out[i];
        const auto& [path, build] = pending_replacements_[i];
        std::vector<u8> bytes;
        try {
            if (!build(read, bytes, r.error)) return;
        } catch (...) {
            r.exception = std::current_exception();
            return;
        }
        // hold()'s delta decision, made here too (it reads only the original tree); hold() takes it as is.
        try {
            r.delta.emplace(delta_of(path, bytes));
        } catch (...) {
            r.delta.reset();  // hold() decides again on the mount's thread, as before
        }
        r.bytes = std::move(bytes);
        r.built = true;
    };
    // The jobs are taken from both ends of the list (even workers from the front, odd from the back), so the heavy
    // builds registered last do not all start last. Which thread builds what changes nothing in the bytes.
    std::mutex take_mutex;
    std::size_t front = 0, back = jobs.size();
    const auto take = [&](bool from_back) -> std::optional<std::size_t> {
        std::lock_guard<std::mutex> lock(take_mutex);
        if (front >= back) return std::nullopt;
        return from_back ? jobs[--back] : jobs[front++];
    };
    std::atomic<unsigned> worker_ids{0};
    const auto worker = [&] {
        const unsigned me = worker_ids.fetch_add(1u);
        for (std::optional<std::size_t> i = take(me % 2u == 1u); i.has_value(); i = take(me % 2u == 1u)) {
            run(*i);
        }
    };
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t helpers =
        std::min<std::size_t>({wanted - 1u, std::size_t{hardware} - 1u, jobs.size() > 0u ? jobs.size() - 1u : 0u});
    std::vector<std::thread> threads;
    threads.reserve(helpers);
    for (std::size_t t = 0; t < helpers; ++t) threads.emplace_back(worker);
    worker();
    for (std::thread& t : threads) t.join();
    return out;
}

void AssetSystem::replace(std::string path, Replacement build) {
    pending_replacements_.emplace_back(std::move(path), std::move(build));
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
