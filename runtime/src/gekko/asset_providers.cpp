#include "gekko/asset_providers.h"
#include "gekko/thread_name.h"  // ES-T1

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "gekko/profiling.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gekko::assets {

// ---------------------------------------------------------------------------
// MappedFile
// ---------------------------------------------------------------------------

MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& o) noexcept
    : base_(o.base_), size_(o.size_), file_handle_(o.file_handle_),
      mapping_handle_(o.mapping_handle_) {
    o.base_ = nullptr;
    o.size_ = 0;
    o.file_handle_ = nullptr;
    o.mapping_handle_ = nullptr;
}

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
    if (this != &o) {
        close();
        base_ = o.base_;
        size_ = o.size_;
        file_handle_ = o.file_handle_;
        mapping_handle_ = o.mapping_handle_;
        o.base_ = nullptr;
        o.size_ = 0;
        o.file_handle_ = nullptr;
        o.mapping_handle_ = nullptr;
    }
    return *this;
}

bool MappedFile::open(const std::filesystem::path& path) {
    close();
#ifdef _WIN32
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0) {
        CloseHandle(file);
        return false;  // an empty file has nothing to map, and that is not a mapping
    }
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        CloseHandle(file);
        return false;
    }
    const void* base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (base == nullptr) {
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }
    base_ = static_cast<const u8*>(base);
    size_ = static_cast<u64>(length.QuadPart);
    // ES-M5 (2026-10-20, proposed by CH): the view is all this object needs. A mapped view keeps its own reference
    // to the file-mapping object, and the mapping object keeps the file open, until UnmapViewOfFile (MapViewOfFile /
    // CreateFileMapping documentation), so both handles can go now. Nothing else used them: the size is kept above,
    // and close() only closes what is still held. With the mount's pre-open this was two kernel handles per asset
    // file for the whole run (~6,960 of the ~7,600). The POSIX path already closes its descriptor after mmap.
    CloseHandle(mapping);
    CloseHandle(file);
    file_handle_ = nullptr;
    mapping_handle_ = nullptr;
    return true;
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return false;
    }
    void* base = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (base == MAP_FAILED) return false;
    base_ = static_cast<const u8*>(base);
    size_ = static_cast<u64>(st.st_size);
    mapping_handle_ = base;
    return true;
#endif
}

void MappedFile::prefault() const noexcept {
    if (base_ == nullptr || size_ == 0u) return;
#ifdef _WIN32
    // PrefetchVirtualMemory issues the reads asynchronously and never blocks
    // on them. It is Windows 8+, so resolve it dynamically and simply do
    // nothing on older systems: this is an optimisation, and failing to warm
    // must never fail a load.
    using PrefetchFn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
    static const PrefetchFn prefetch = []() -> PrefetchFn {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel == nullptr) return nullptr;
        return reinterpret_cast<PrefetchFn>(
            reinterpret_cast<void*>(GetProcAddress(kernel, "PrefetchVirtualMemory")));
    }();
    if (prefetch == nullptr) return;
    WIN32_MEMORY_RANGE_ENTRY range{};
    range.VirtualAddress = const_cast<u8*>(base_);
    range.NumberOfBytes = static_cast<SIZE_T>(size_);
    prefetch(GetCurrentProcess(), 1, &range, 0);
#else
    ::madvise(const_cast<u8*>(base_), static_cast<std::size_t>(size_), MADV_WILLNEED);
#endif
}

void MappedFile::close() noexcept {
#ifdef _WIN32
    if (base_ != nullptr) UnmapViewOfFile(base_);
    if (mapping_handle_ != nullptr) CloseHandle(static_cast<HANDLE>(mapping_handle_));
    if (file_handle_ != nullptr) CloseHandle(static_cast<HANDLE>(file_handle_));
#else
    if (mapping_handle_ != nullptr) ::munmap(mapping_handle_, static_cast<std::size_t>(size_));
#endif
    base_ = nullptr;
    size_ = 0;
    file_handle_ = nullptr;
    mapping_handle_ = nullptr;
}

namespace {

// The one range rule, in one place. A read that runs past the end SUCCEEDS and
// zero-fills the tail: both live issuers round their length up to a 32-byte
// multiple, so the last read of every file always asks for bytes that are not
// there, and failing it would fail every file.
bool copy_with_zero_fill(const u8* base, u64 size, u64 offset, u64 length, u8* dest, u64& got) {
    got = 0;
    if (dest == nullptr) return false;
    if (length == 0u) return true;
    if (offset >= size) {
        std::memset(dest, 0, static_cast<std::size_t>(length));
        got = length;
        return true;
    }
    const u64 available = std::min<u64>(length, size - offset);
    std::memcpy(dest, base + offset, static_cast<std::size_t>(available));
    if (available < length) {
        std::memset(dest + available, 0, static_cast<std::size_t>(length - available));
    }
    got = length;
    return true;
}

}  // namespace

void release_view_pages(const void* base, u64 size) noexcept {
#ifdef _WIN32
    if (base == nullptr || size == 0u) return;
    // ERROR_NOT_LOCKED is the expected result: the call's effect on unlocked pages is the removal from the working set.
    VirtualUnlock(const_cast<void*>(base), static_cast<SIZE_T>(size));
#else
    (void)base;
    (void)size;
#endif
}

// ---------------------------------------------------------------------------
// FileProvider
// ---------------------------------------------------------------------------

u64 FileProvider::add(std::filesystem::path path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto slot = std::make_unique<Slot>();
    slot->path = std::move(path);
    slots_.push_back(std::move(slot));
    ++stats_.known;
    return static_cast<u64>(slots_.size() - 1u);
}

FileProvider::~FileProvider() {
    stop_.store(true, std::memory_order_release);
    if (preopener_.joinable()) preopener_.join();
}

void FileProvider::open_slot(Slot& slot, bool background) {
    // The only real host filesystem work in the serving path: CreateFileW +
    // GetFileSizeEx + CreateFileMappingW + MapViewOfFile. Runs once per slot
    // (the slot's once_flag), OUTSIDE mutex_: a background open of one file
    // never holds up the game's open of another. A cold NTFS open or a
    // network path shows here and nowhere else.
    GEKKO_PROFILE_ZONE("res/file-open");
    const bool opened = slot.file.open(slot.path);
    std::lock_guard<std::mutex> lock(mutex_);
    if (background) ++stats_.preopened;
    if (opened) {
        ++stats_.mapped;
        GEKKO_PROFILE_VALUE(slot.file.size());
        GEKKO_PROFILE_PLOT("res/files-mapped", static_cast<double>(stats_.mapped));
        stats_.mapped_bytes += slot.file.size();
        // VIRTUAL bytes mapped, NOT resident pages: an honest picture of the
        // address space the port costs, not a working-set figure. Nothing ever
        // unmaps, so this curve can only rise - which also makes it the leak
        // shape if one ever appears.
        GEKKO_PROFILE_PLOT("res/mapped-bytes", static_cast<double>(stats_.mapped_bytes));
    } else {
        // Every later read of this asset now returns false, surfacing far away
        // as a missing texture or a stalled loader; this puts the cause at the
        // open. MappedFile::open also fails a zero-length file, which is a
        // legitimate tree defect worth seeing.
        ++stats_.failures;
        GEKKO_PROFILE_MESSAGE_LITERAL("res/file-open-failed");
        GEKKO_PROFILE_PLOT("res/file-open-failures", static_cast<double>(stats_.failures));
    }
}

FileProvider::Slot* FileProvider::mapped_slot(u64 key) {
    Slot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (key >= slots_.size()) return nullptr;
        slot = slots_[static_cast<std::size_t>(key)].get();
    }
    if (slot == nullptr) return nullptr;
    // One open per distinct asset, never one per read. If the background
    // pre-opener is opening this very file, this waits for that one open
    // (which it would otherwise have done itself); the mapping it publishes
    // is visible here when call_once returns.
    std::call_once(slot->opened, [&] { open_slot(*slot, false); });
    return slot;
}

void FileProvider::preopen() {
    // Diagnostic control for the before/after stall measurement (ES_REPORT):
    // GEKKO_ASSET_PREOPEN=0 leaves every open to the first read, as before.
    if (const char* setting = std::getenv("GEKKO_ASSET_PREOPEN"); setting != nullptr && setting[0] == '0') return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (preopener_.joinable()) return;
    const std::size_t end = slots_.size();
    preopener_ = std::thread([this, end] {
        name_this_thread("asset-preopen");  // ES-T1
#ifdef _WIN32
        // Below the game's threads for the CPU; the I/O priority stays normal,
        // so a read that has to wait for this thread's open of the same file
        // never waits behind background-priority I/O.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
        for (std::size_t key = 0; key < end && !stop_.load(std::memory_order_acquire); ++key) {
            Slot* slot = nullptr;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                slot = slots_[key].get();
            }
            if (slot != nullptr) std::call_once(slot->opened, [&] { open_slot(*slot, true); });
        }
    });
}

ByteSpan FileProvider::map(u64 key, u64 size) {
    Slot* slot = mapped_slot(key);
    if (slot == nullptr || !slot->file.valid()) return ByteSpan{};
    // The registry's size is what the game believes; the mapping is the truth.
    // Serve the smaller so a consumer can never read past the file.
    const u64 usable = size == 0u ? slot->file.size() : std::min<u64>(size, slot->file.size());
    return ByteSpan{slot->file.data(), usable};
}

bool FileProvider::read(u64 key, u64 offset, u64 length, u8* dest, u64& got) {
    Slot* slot = mapped_slot(key);
    if (slot == nullptr || !slot->file.valid()) {
        got = 0;
        return false;
    }
    // The memcpy out of the mapped view - and, on first touch of a page, the
    // point at which the host page cache actually faults the file in. A major
    // fault lands inside this zone, which is what would falsify the standing
    // "disc I/O is not a bottleneck" result if it ever stopped being true.
    // Placed here rather than in copy_with_zero_fill, which is shared with the
    // archive and PRS providers (neither of which is ever registered).
    GEKKO_PROFILE_ZONE("res/file-copy");
    GEKKO_PROFILE_VALUE(length);
    return copy_with_zero_fill(slot->file.data(), slot->file.size(), offset, length, dest, got);
}

FileProvider::Stats FileProvider::stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// ---------------------------------------------------------------------------
// MemoryProvider
// ---------------------------------------------------------------------------

u64 MemoryProvider::add(std::vector<u8> bytes) {
    files_.push_back(std::move(bytes));
    file_seq_.push_back(next_seq_++);
    return files_.size() - 1u;
}

u64 MemoryProvider::add_delta(AssetProvider* base, u64 base_key, u64 size, std::vector<Patch> patches) {
    Delta delta;
    delta.base = base;
    delta.base_key = base_key;
    delta.size = size;
    delta.patches = std::move(patches);
    delta.seq = next_seq_++;
    deltas_.push_back(std::move(delta));
    return kDeltaBit | (deltas_.size() - 1u);
}

const std::vector<u8>* MemoryProvider::file_of(u64 key) const {
    if ((key & (kDeltaBit | kVirtualBit)) != 0u) return nullptr;
    return key < files_.size() ? &files_[static_cast<std::size_t>(key)] : nullptr;
}

bool MemoryProvider::read_delta(const Delta& delta, u64 offset, u64 length, u8* dest, u64& got) {
    got = 0;
    if (dest == nullptr) return false;
    if (length == 0u) return true;
    if (offset >= delta.size) {
        std::memset(dest, 0, static_cast<std::size_t>(length));
        got = length;
        return true;
    }
    const u64 inside = std::min<u64>(length, delta.size - offset);
    u64 base_got = 0;
    if (delta.base == nullptr || !delta.base->read(delta.base_key, offset, inside, dest, base_got)) return false;
    if (base_got < inside) std::memset(dest + base_got, 0, static_cast<std::size_t>(inside - base_got));
    // The patches that reach [offset, offset + inside): the first whose end passes `offset`, then forward.
    const auto first = std::partition_point(delta.patches.begin(), delta.patches.end(), [offset](const Patch& p) {
        return p.offset + p.bytes.size() <= offset;
    });
    for (auto it = first; it != delta.patches.end() && it->offset < offset + inside; ++it) {
        const u64 begin = std::max<u64>(it->offset, offset);
        const u64 end = std::min<u64>(it->offset + it->bytes.size(), offset + inside);
        std::memcpy(dest + (begin - offset), it->bytes.data() + (begin - it->offset),
                    static_cast<std::size_t>(end - begin));
    }
    if (inside < length) std::memset(dest + inside, 0, static_cast<std::size_t>(length - inside));
    got = length;
    return true;
}

MemoryProvider::Held MemoryProvider::held() const noexcept {
    Held h;
    h.files = files_.size();
    for (const std::vector<u8>& f : files_) h.file_bytes += f.size();
    h.deltas = deltas_.size();
    for (const Delta& d : deltas_) {
        h.delta_logical_bytes += d.size;
        for (const Patch& p : d.patches) h.delta_bytes += p.bytes.size();
    }
    return h;
}

void MemoryProvider::digests(const std::function<void(const std::string&, u64, u64)>& sink) {
    const auto fnv = [](const u8* data, u64 n) {
        u64 h = 1469598103934665603ull;
        for (u64 i = 0; i < n; ++i) {
            h ^= data[i];
            h *= 1099511628211ull;
        }
        return h;
    };
    const auto of_key = [&](u64 key, const std::string& label) {
        if ((key & kDeltaBit) != 0u) {
            const u64 index = key & ~kDeltaBit;
            if (index >= deltas_.size()) return;
            const Delta& d = deltas_[static_cast<std::size_t>(index)];
            std::vector<u8> bytes(static_cast<std::size_t>(d.size));
            u64 got = 0;
            if (d.size != 0u && !read_delta(d, 0, d.size, bytes.data(), got)) return;
            sink(label, d.size, fnv(bytes.data(), d.size));
        } else if (key < files_.size()) {
            const std::vector<u8>& f = files_[static_cast<std::size_t>(key)];
            sink(label, f.size(), fnv(f.data(), f.size()));
        }
    };
    // Every content in creation order (the same order in any build, whether a content is held plain or as a delta).
    std::vector<std::pair<u64, u64>> order;  // (creation number, key)
    for (std::size_t i = 0; i < files_.size(); ++i) order.emplace_back(file_seq_[i], i);
    for (std::size_t i = 0; i < deltas_.size(); ++i) order.emplace_back(deltas_[i].seq, kDeltaBit | i);
    std::sort(order.begin(), order.end());
    for (const auto& [seq, key] : order) {
        of_key(key, "content " + std::to_string(seq));
    }
}

u64 MemoryProvider::add_virtual(u64 size, VirtualReader reader, std::string name) {
    virtuals_.push_back(Virtual{size, std::move(reader), std::move(name)});
    return kVirtualBit | (virtuals_.size() - 1u);
}

ByteSpan MemoryProvider::map(u64 key, u64 size) {
    if ((key & kVirtualBit) != 0u) return ByteSpan{};  // produced per read, never mapped
    const std::vector<u8>* file = file_of(key);
    if (file == nullptr) return ByteSpan{};  // a delta exists only as base + patches: the caller materialises
    return ByteSpan{file->data(), size == 0u ? file->size() : std::min<u64>(size, file->size())};
}

bool MemoryProvider::read(u64 key, u64 offset, u64 length, u8* dest, u64& got) {
    if ((key & kVirtualBit) != 0u) {
        const u64 index = key & ~kVirtualBit;
        got = 0;
        if (index >= virtuals_.size()) return false;
        const Virtual& v = virtuals_[static_cast<std::size_t>(index)];
        // The same contract as a held file: exactly `size` bytes, zeros past it.
        const u64 inside = offset >= v.size ? 0u : std::min(length, v.size - offset);
        u64 produced = 0;
        if (inside != 0u && (!v.reader || !v.reader(offset, inside, dest, produced))) return false;
        if (produced < length) std::memset(dest + produced, 0, static_cast<std::size_t>(length - produced));
        got = length;
        return true;
    }
    if ((key & kDeltaBit) != 0u) {
        const u64 index = key & ~kDeltaBit;
        if (index >= deltas_.size()) {
            got = 0;
            return false;
        }
        return read_delta(deltas_[static_cast<std::size_t>(index)], offset, length, dest, got);
    }
    const std::vector<u8>* file = file_of(key);
    if (file == nullptr) {
        got = 0;
        return false;
    }
    return copy_with_zero_fill(file->data(), file->size(), offset, length, dest, got);
}

}  // namespace gekko::assets
