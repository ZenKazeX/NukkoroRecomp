#include "gekko/asset_providers.h"
#include "gekko/thread_name.h"  // ES-T1

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gekko/native_assets.h"  // prs_expand, the verified decompressor
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
// ArchiveProvider
// ---------------------------------------------------------------------------

u64 ArchiveProvider::add_container(std::filesystem::path path) {
    std::lock_guard<std::mutex> lock(mutex_);
    container_paths_.push_back(std::move(path));
    containers_.push_back(nullptr);
    return static_cast<u64>(containers_.size() - 1u);
}

u64 ArchiveProvider::add_member(const Member& member) {
    std::lock_guard<std::mutex> lock(mutex_);
    members_.push_back(member);
    return static_cast<u64>(members_.size() - 1u);
}

const ArchiveProvider::Member* ArchiveProvider::member(u64 key) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (key >= members_.size()) return nullptr;
    return &members_[static_cast<std::size_t>(key)];
}

ByteSpan ArchiveProvider::stored(u64 key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (key >= members_.size()) return ByteSpan{};
    const Member m = members_[static_cast<std::size_t>(key)];
    if (m.container >= containers_.size()) return ByteSpan{};
    auto& container = containers_[static_cast<std::size_t>(m.container)];
    if (container == nullptr) {
        container = std::make_unique<MappedFile>();
        if (!container->open(container_paths_[static_cast<std::size_t>(m.container)])) {
            return ByteSpan{};
        }
    }
    if (!container->valid()) return ByteSpan{};
    if (m.offset > container->size()) return ByteSpan{};
    const u64 usable = std::min<u64>(m.size, container->size() - m.offset);
    return ByteSpan{container->data() + m.offset, usable};
}

ByteSpan ArchiveProvider::map(u64 key, u64 size) {
    const Member* m = member(key);
    if (m == nullptr || m->expanded != 0u) return ByteSpan{};  // compressed: cannot serve in place
    const ByteSpan span = stored(key);
    if (!span.valid()) return ByteSpan{};
    return ByteSpan{span.data, size == 0u ? span.size : std::min<u64>(size, span.size)};
}

bool ArchiveProvider::read(u64 key, u64 offset, u64 length, u8* dest, u64& got) {
    const ByteSpan span = stored(key);
    if (!span.valid()) {
        got = 0;
        return false;
    }
    return copy_with_zero_fill(span.data, span.size, offset, length, dest, got);
}

// ---------------------------------------------------------------------------
// PrsProvider
// ---------------------------------------------------------------------------

bool PrsProvider::read(u64 key, u64 offset, u64 length, u8* dest, u64& got) {
    got = 0;
    if (dest == nullptr) return false;
    const ArchiveProvider::Member* m = archive_.member(key);
    if (m == nullptr || m->expanded == 0u) return false;

    const ByteSpan compressed = archive_.stored(key);
    if (!compressed.valid()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.failures;
        return false;
    }

    // Expanded straight out of the mapped container: the compressed bytes are
    // never copied before being decoded.
    std::vector<u8> expanded;
    // A stream that ends (truncated) before the member's declared size is a failure, not zeros.
    if (!prs_expand(compressed.data, static_cast<std::size_t>(compressed.size),
                    static_cast<std::size_t>(m->expanded), expanded) ||
        expanded.size() < static_cast<std::size_t>(m->expanded)) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.failures;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.expansions;
        stats_.expanded_bytes += expanded.size();
    }
    return copy_with_zero_fill(expanded.data(), expanded.size(), offset, length, dest, got);
}

PrsProvider::Stats PrsProvider::stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// ---------------------------------------------------------------------------
// MemoryProvider
// ---------------------------------------------------------------------------

u64 MemoryProvider::add(std::vector<u8> bytes) {
    files_.push_back(std::move(bytes));
    return files_.size() - 1u;
}

u64 MemoryProvider::add_switchable(std::vector<std::vector<u8>> variants, std::function<std::size_t()> selector,
                                   std::string name) {
    Group group;
    group.selector = std::move(selector);
    group.name = std::move(name);
    for (std::vector<u8>& v : variants) group.files.push_back(add(std::move(v)));
    groups_.push_back(std::move(group));
    return kGroupBit | (groups_.size() - 1u);
}

const std::vector<u8>* MemoryProvider::file_of(u64 key) const {
    if ((key & kGroupBit) != 0u) {
        const u64 index = key & ~kGroupBit;
        if (index >= groups_.size() || groups_[static_cast<std::size_t>(index)].files.empty()) return nullptr;
        const Group& group = groups_[static_cast<std::size_t>(index)];
        const std::size_t chosen = group.selector ? std::min(group.selector(), group.files.size() - 1u) : 0u;
        // A read after the setting changed: say once which variant is now served (the live-switch evidence).
        const std::size_t before = group.chosen->exchange(chosen, std::memory_order_relaxed);
        if (before != chosen && before != ~std::size_t{0})
            std::fprintf(stderr, "[assets] %s: now serving variant %zu (was %zu)\n", group.name.c_str(), chosen, before);
        return &files_[static_cast<std::size_t>(group.files[chosen])];
    }
    return key < files_.size() ? &files_[static_cast<std::size_t>(key)] : nullptr;
}

u64 MemoryProvider::add_virtual(u64 size, VirtualReader reader, std::string name) {
    virtuals_.push_back(Virtual{size, std::move(reader), std::move(name)});
    return kVirtualBit | (virtuals_.size() - 1u);
}

ByteSpan MemoryProvider::map(u64 key, u64 size) {
    if ((key & kVirtualBit) != 0u) return ByteSpan{};  // produced per read, never mapped
    const std::vector<u8>* file = file_of(key);
    if (file == nullptr) return ByteSpan{};
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
    const std::vector<u8>* file = file_of(key);
    if (file == nullptr) {
        got = 0;
        return false;
    }
    return copy_with_zero_fill(file->data(), file->size(), offset, length, dest, got);
}

}  // namespace gekko::assets
