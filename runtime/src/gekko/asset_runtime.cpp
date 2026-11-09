#include "gekko/asset_runtime.h"

#include <algorithm>
#include <cstring>

#include "gekko/profiling.h"

namespace gekko::assets {

// ---------------------------------------------------------------------------
// AssetRegistry
// ---------------------------------------------------------------------------

AssetId AssetRegistry::intern(std::string path, u64 size, ProviderKind provider, u64 key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_path_.find(path);
    if (it != by_path_.end()) {
        AssetRecord& existing = records_[it->second - 1u];
        // Re-interning refreshes the locator but never the identity: a handle
        // taken earlier must keep meaning the same asset.
        existing.size = size;
        existing.provider = provider;
        existing.key = key;
        return existing.id;
    }
    const u32 value = static_cast<u32>(records_.size()) + 1u;  // 0 stays "none"
    AssetRecord record;
    record.id = AssetId{value};
    record.size = size;
    record.provider = provider;
    record.key = key;
    record.path = path;
    records_.push_back(std::move(record));
    by_path_.emplace(std::move(path), value);
    return AssetId{value};
}

AssetId AssetRegistry::find(std::string_view path) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_path_.find(std::string(path));
    return it == by_path_.end() ? AssetId{} : AssetId{it->second};
}

const AssetRecord* AssetRegistry::get(AssetId id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!id.valid() || id.value > records_.size()) return nullptr;
    return &records_[id.value - 1u];
}

void AssetRegistry::clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
    by_path_.clear();
}

// ---------------------------------------------------------------------------
// AssetAddressSpace
// ---------------------------------------------------------------------------

bool AssetAddressSpace::build(const AssetRegistry& registry) {
    // Two full passes over every record, the second writing page_owner_ once
    // per 64 KiB page owned - so the cost is set by the address-space LAYOUT,
    // not by the file count, which is unique among the mount steps. It is also
    // the one step that can FAIL the mount, and its failure path clears
    // everything it just built.
    GEKKO_PROFILE_ZONE("res/mount-space");
    clear();
    const std::vector<AssetRecord>& records = registry.records();
    base_.assign(records.size() + 1u, 0u);
    size_.assign(records.size() + 1u, 0u);

    // Page 0 of the space is reserved, so kSpaceBase itself never decodes.
    u64 next_page = 1u;
    for (const AssetRecord& record : records) {
        const u64 need = record.size + kTailAllowance;
        const u64 pages = (need + kPageSize - 1u) / kPageSize;
        const u64 base = kSpaceBase + (next_page << kPageShift);
        if (base + (pages << kPageShift) > 0x100000000ull) {
            // The assets do not fit in the 32 bits the guest has for a token.
            // Reported, never silently truncated: a truncated map is an
            // aliasing bug that would read one asset's bytes for another.
            clear();
            return false;
        }
        base_[record.id.value] = static_cast<u32>(base);
        size_[record.id.value] = record.size;
        next_page += pages;
    }
    end_ = kSpaceBase + (next_page << kPageShift);

    // The table is indexed from the base, not from zero, so raising the base
    // costs no memory.
    page_owner_.assign(static_cast<std::size_t>(next_page), 0u);
    for (const AssetRecord& record : records) {
        const u64 first = (static_cast<u64>(base_[record.id.value]) - kSpaceBase) >> kPageShift;
        const u64 pages = (record.size + kTailAllowance + kPageSize - 1u) / kPageSize;
        for (u64 page = first; page < first + pages; ++page) {
            page_owner_[static_cast<std::size_t>(page)] = record.id.value;
        }
    }
    // A PAGE count, not bytes; the byte figure is res/mount-table-bytes.
    GEKKO_PROFILE_VALUE(page_owner_.size());
    return true;
}

void AssetAddressSpace::clear() noexcept {
    page_owner_.clear();
    base_.clear();
    size_.clear();
    end_ = 0;
}

u32 AssetAddressSpace::base_of(AssetId id) const noexcept {
    if (!id.valid() || id.value >= base_.size()) return 0u;
    return base_[id.value];
}

AssetAddressSpace::Status AssetAddressSpace::decode(u32 address, u64 length,
                                                    Decoded& out) const noexcept {
    out = Decoded{};
    if (address < kSpaceBase) return Status::Unmapped;
    const u64 page = (static_cast<u64>(address) - kSpaceBase) >> kPageShift;
    if (page >= page_owner_.size()) return Status::Unmapped;
    const u32 slot = page_owner_[static_cast<std::size_t>(page)];
    if (slot == 0u) return Status::Unmapped;

    const u64 offset = static_cast<u64>(address) - base_[slot];
    const u64 size = size_[slot];
    // The two conditions the GUEST asserts for itself before it computes
    // startAddr + offset, checked here against the asset rather than against a
    // page:
    //
    //     offset <= length                    (0x8037AF34 / 0x8037B020)
    //     offset + request < length + 0x20    (0x8037AF64 / 0x8037B050)
    //
    // Page granularity would be a weaker test - a 4 KiB asset owns a whole
    // 64 KiB page, so a 8 KiB read of it lands inside the region and would
    // pass. It is still malformed, and on hardware it would have returned the
    // NEXT file's bytes. Checking the asset's own contract refuses it exactly.
    // (One byte looser than the guest, which uses a strict <, so nothing legal
    // is ever refused.)
    if (offset > size) return Status::OutOfRange;
    if (offset + length > size + kTailAllowance) return Status::OutOfRange;

    out.id = AssetId{slot};
    out.offset = offset;
    out.size = size;
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// AssetHandle
// ---------------------------------------------------------------------------

AssetHandle::~AssetHandle() { reset(); }

AssetHandle::AssetHandle(AssetHandle&& other) noexcept
    : manager_(other.manager_), id_(other.id_), span_(other.span_) {
    other.manager_ = nullptr;
    other.id_ = AssetId{};
    other.span_ = ByteSpan{};
}

AssetHandle& AssetHandle::operator=(AssetHandle&& other) noexcept {
    if (this != &other) {
        reset();
        manager_ = other.manager_;
        id_ = other.id_;
        span_ = other.span_;
        other.manager_ = nullptr;
        other.id_ = AssetId{};
        other.span_ = ByteSpan{};
    }
    return *this;
}

void AssetHandle::reset() noexcept {
    if (manager_ != nullptr && id_.valid()) manager_->release(id_);
    manager_ = nullptr;
    id_ = AssetId{};
    span_ = ByteSpan{};
}

// ---------------------------------------------------------------------------
// ResourceManager
// ---------------------------------------------------------------------------

void ResourceManager::set_provider(ProviderKind kind, AssetProvider* provider) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < providers_.size()) providers_[index] = provider;
}

AssetProvider* ResourceManager::provider(ProviderKind kind) const noexcept {
    const auto index = static_cast<std::size_t>(kind);
    std::lock_guard<std::mutex> lock(mutex_);
    return index < providers_.size() ? providers_[index] : nullptr;
}

AssetHandle ResourceManager::acquire(AssetId id) {
    const AssetRecord* record = registry_.get(id);
    if (record == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.misses;
        return AssetHandle{};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[id.value];
    if (entry.resident) {
        ++entry.refs;
        ++stats_.acquires;
        ++stats_.live;
        stats_.peak_live = std::max(stats_.peak_live, stats_.live);
        return AssetHandle{this, id, entry.span};
    }

    const auto index = static_cast<std::size_t>(record->provider);
    AssetProvider* source = index < providers_.size() ? providers_[index] : nullptr;
    if (source == nullptr) {
        ++stats_.misses;
        return AssetHandle{};
    }

    // Prefer the view. For a mapped file this is the whole cost of a load: no
    // copy, no staging buffer, and the bytes are the page cache's own pages.
    const ByteSpan mapped = source->map(record->key, record->size);
    if (mapped.valid()) {
        entry.span = mapped;
        entry.resident = true;
        ++stats_.map_hits;
    } else {
        // A provider that cannot serve in place - an archive member, or a
        // compressed one that has to be expanded. Materialise once and keep it
        // for as long as anything holds a handle.
        entry.owned.resize(static_cast<std::size_t>(record->size));
        u64 got = 0;
        if (!source->read(record->key, 0, record->size, entry.owned.data(), got)) {
            entries_.erase(id.value);
            ++stats_.misses;
            return AssetHandle{};
        }
        entry.span = ByteSpan{entry.owned.data(), got};
        entry.resident = true;
        ++stats_.materialised;
    }

    entry.refs = 1;
    ++stats_.acquires;
    ++stats_.live;
    stats_.peak_live = std::max(stats_.peak_live, stats_.live);
    return AssetHandle{this, id, entry.span};
}

void ResourceManager::release(AssetId id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(id.value);
    if (it == entries_.end()) return;
    if (it->second.refs != 0u) --it->second.refs;
    ++stats_.releases;
    if (stats_.live != 0u) --stats_.live;
    if (it->second.refs == 0u) {
        // A mapped asset costs nothing to keep - the pages are evictable and
        // shared - so only materialised bytes are actually handed back.
        if (!it->second.owned.empty()) entries_.erase(it);
    }
}

bool ResourceManager::read(AssetId id, u64 offset, u64 length, u8* dest, u64& got) {
    got = 0;
    const AssetRecord* record = registry_.get(id);
    if (record == nullptr || dest == nullptr) return false;
    const auto index = static_cast<std::size_t>(record->provider);
    AssetProvider* source = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source = index < providers_.size() ? providers_[index] : nullptr;
    }
    if (source == nullptr) return false;

    // AN ASSET IS EXACTLY record->size BYTES, AND EVERYTHING PAST IT IS ZERO.
    //
    // Enforced here rather than left to the provider, because the provider
    // knows only how big its backing object happens to be. A host file longer
    // than the asset it backs would otherwise leak its tail into a guest read
    // that the guest believes cannot reach it (its own bounds check uses the
    // declared size, 0x8037AF30). Measured on the shipping tree all 3,480
    // files match their declared size exactly, so this changes no byte today -
    // it makes the contract independent of the tree staying that way.
    //
    // The provider is called even when nothing is visible, so that an asset
    // whose backing object cannot be opened still FAILS a read wholly past its
    // end instead of quietly returning zeroes.
    const u64 visible = offset >= record->size ? 0u : std::min(length, record->size - offset);
    if (!source->read(record->key, offset, visible, dest, got)) return false;
    if (visible < length) {
        std::memset(dest + visible, 0, static_cast<std::size_t>(length - visible));
    }
    got = length;
    return true;
}

ResourceManager::Stats ResourceManager::stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// ---------------------------------------------------------------------------
// StreamingScheduler
// ---------------------------------------------------------------------------

bool StreamingScheduler::submit(const Request& request) {
    // REJECT AT SUBMIT, not at completion. The guest checks that a file exists
    // before it allocates a node and returns false on the spot; a replacement
    // that accepted and failed later would drive error paths the game has no
    // code for.
    if (!request.id.valid() || request.dest == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.rejected;
        return false;
    }
    const auto priority = static_cast<std::size_t>(request.priority);
    if (priority >= static_cast<std::size_t>(Priority::Count)) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.rejected;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    queues_[priority].push_back(request);
    ++stats_.submitted;
    u64 total = 0;
    for (const auto& q : queues_) total += q.size();
    stats_.peak_pending = std::max(stats_.peak_pending, total);
    return true;
}

std::size_t StreamingScheduler::pump(std::vector<Completion>& out, std::size_t budget) {
    std::size_t retired = 0;
    for (;;) {
        if (budget != 0u && retired >= budget) break;

        Request request;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Strict priority, then FIFO inside a level. Taking the head of
            // the most urgent non-empty queue IS the recovered rule.
            std::size_t chosen = static_cast<std::size_t>(Priority::Count);
            for (std::size_t p = 0; p < static_cast<std::size_t>(Priority::Count); ++p) {
                if (!queues_[p].empty()) {
                    chosen = p;
                    break;
                }
            }
            if (chosen == static_cast<std::size_t>(Priority::Count)) break;
            request = queues_[chosen].front();
            queues_[chosen].erase(queues_[chosen].begin());
        }

        // The bytes are produced HERE, on the thread that called pump, and the
        // completion is appended only once they are all present. There is no
        // state in which a consumer can observe this request half-done.
        Completion completion;
        completion.id = request.id;
        completion.user = request.user;
        u64 got = 0;
        completion.ok = resources_.read(request.id, request.offset, request.length,
                                        request.dest, got);
        completion.transferred = got;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (completion.ok) {
                ++stats_.completed;
                stats_.bytes += got;
            } else {
                ++stats_.failed;
            }
        }
        out.push_back(completion);
        ++retired;
    }
    return retired;
}

std::size_t StreamingScheduler::pending() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t total = 0;
    for (const auto& q : queues_) total += q.size();
    return total;
}

}  // namespace gekko::assets
