#include "../include/native_memory.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace shadow::memory {
static_assert(sizeof(void*) == 8 && sizeof(std::size_t) == 8);

namespace {
std::atomic<std::uint64_t> next_generation{1};

std::uint64_t new_generation() {
    auto value = next_generation.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max()) throw std::bad_alloc();
        if (next_generation.compare_exchange_weak(value, value + 1u, std::memory_order_relaxed)) return value;
    }
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    if (value > std::numeric_limits<std::size_t>::max() - (alignment - 1u)) throw std::bad_alloc();
    return (value + alignment - 1u) & ~(alignment - 1u);
}

struct Counters {
    mutable std::mutex mutex;
    Snapshot values;
    template<class F> void update(Domain domain, F&& f) {
        f(values.total);
        f(values.domains[static_cast<std::size_t>(domain)]);
    }
};

struct Block {
    std::shared_ptr<Counters> counters;
    Domain domain = Domain::General;
    Handle handle;
    void* reservation = nullptr;
    std::byte* data = nullptr;
    std::size_t capacity = 0, reserved = 0, committed = 0, active = 0;
    bool counted = false;
    ~Block() {
        if (reservation) {
#ifdef _WIN32
            VirtualFree(reservation, 0, MEM_RELEASE);
#else
            munmap(reservation, reserved);
#endif
        }
        if (counted) {
            std::lock_guard lock(counters->mutex);
            counters->update(domain, [&](DomainStats& s) { s.reserved -= reserved; s.committed -= committed; });
        }
    }
};

struct Lease {
    std::shared_ptr<Block> block;
    explicit Lease(std::shared_ptr<Block> value) : block(std::move(value)) {
        std::lock_guard lock(block->counters->mutex);
        ++block->counters->values.mappings;
        ++block->counters->values.live_mappings;
    }
    ~Lease() {
        std::lock_guard lock(block->counters->mutex);
        --block->counters->values.live_mappings;
    }
};
}

const char* domain_name(Domain domain) noexcept {
    switch (domain) {
        case Domain::Assets: return "assets";
        case Domain::Streaming: return "streaming";
        case Domain::Renderer: return "renderer-host";
        case Domain::Scratch: return "scratch";
        case Domain::Decompression: return "decompression";
        case Domain::Audio: return "audio-host";
        case Domain::General: return "general";
        case Domain::Gameplay: return "gameplay";
        default: return "invalid";
    }
}

HostInfo host_info() noexcept {
    HostInfo result;
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        result.physical_total = memory.ullTotalPhys;
        result.physical_available = memory.ullAvailPhys;
        result.commit_available = memory.ullAvailPageFile;
    }
    PROCESS_MEMORY_COUNTERS_EX process{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process))) {
        result.process_committed = process.PrivateUsage;
        result.process_resident = process.WorkingSetSize;
        result.process_peak_resident = process.PeakWorkingSetSize;
    }
#else
    const long page = sysconf(_SC_PAGESIZE);
    const long pages = sysconf(_SC_PHYS_PAGES);
    if (page > 0 && pages > 0) result.physical_total = std::uint64_t(page) * std::uint64_t(pages);
#ifdef _SC_AVPHYS_PAGES
    const long available = sysconf(_SC_AVPHYS_PAGES);
    if (page > 0 && available > 0) result.physical_available = std::uint64_t(page) * std::uint64_t(available);
#endif
#endif
    return result;
}

struct MemorySystem::Impl {
    mutable std::mutex mutex;
    std::shared_ptr<Counters> counters = std::make_shared<Counters>();
    std::unordered_map<std::uint64_t, std::shared_ptr<Block>> blocks;
    std::unordered_map<const void*, std::uint64_t> pointers;
    std::size_t page = 4096u, granularity = 4096u;
    explicit Impl(std::uint64_t requested) {
#ifdef _WIN32
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        page = info.dwPageSize;
        granularity = info.dwAllocationGranularity;
#else
        const long value = sysconf(_SC_PAGESIZE);
        if (value > 0) page = granularity = static_cast<std::size_t>(value);
#endif
        const auto host = host_info();
        std::uint64_t available = host.physical_available;
        if (available == 0u) available = host.physical_total / 2u;
        if (host.commit_available != 0u) available = std::min(available, host.commit_available);
        counters->values.commit_budget = requested ? requested : available - available / 8u;
    }
};

MemorySystem::MemorySystem(std::uint64_t commit_budget) : impl_(std::make_unique<Impl>(commit_budget)) {}

MemorySystem::~MemorySystem() {
    while (!impl_->blocks.empty()) release({impl_->blocks.begin()->first});
}

Handle MemorySystem::reserve(std::size_t bytes, Domain domain, std::size_t alignment) {
    const bool valid_domain = static_cast<std::size_t>(domain) < static_cast<std::size_t>(Domain::Count);
    if (!valid_domain || bytes == 0u || alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
        std::lock_guard lock(impl_->counters->mutex);
        if (valid_domain) impl_->counters->update(domain, [](DomainStats& s) { ++s.failures; });
        else ++impl_->counters->values.total.failures;
        throw std::invalid_argument("invalid native allocation request");
    }
    try {
        const auto padding = alignment > impl_->granularity ? alignment - 1u : 0u;
        if (bytes > std::size_t(std::numeric_limits<std::ptrdiff_t>::max()) - padding) throw std::bad_alloc();
        auto block = std::make_shared<Block>();
        block->counters = impl_->counters;
        block->domain = domain;
        block->capacity = bytes;
        block->reserved = align_up(bytes + padding, impl_->granularity);
        block->handle = {new_generation()};
#ifdef _WIN32
        block->reservation = VirtualAlloc(nullptr, block->reserved, MEM_RESERVE, PAGE_NOACCESS);
#else
        block->reservation = mmap(nullptr, block->reserved, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (block->reservation == MAP_FAILED) block->reservation = nullptr;
#endif
        if (!block->reservation) throw std::bad_alloc();
        const auto address = align_up(reinterpret_cast<std::uintptr_t>(block->reservation), alignment);
        block->data = reinterpret_cast<std::byte*>(address);
        {
            std::lock_guard lock(impl_->counters->mutex);
            impl_->counters->update(domain, [&](DomainStats& s) {
                s.reserved += block->reserved;
                s.peak_reserved = std::max(s.peak_reserved, s.reserved);
            });
            block->counted = true;
        }
        std::lock_guard lock(impl_->mutex);
        impl_->pointers.emplace(block->data, block->handle.generation);
        try { impl_->blocks.emplace(block->handle.generation, block); }
        catch (...) { impl_->pointers.erase(block->data); throw; }
        {
            std::lock_guard counters_lock(impl_->counters->mutex);
            impl_->counters->update(domain, [](DomainStats& s) { ++s.live_allocations; ++s.allocations; });
        }
        return block->handle;
    } catch (const std::bad_alloc&) {
        std::lock_guard lock(impl_->counters->mutex);
        impl_->counters->update(domain, [](DomainStats& s) { ++s.failures; });
        throw;
    }
}

bool MemorySystem::commit(Handle handle, std::size_t bytes) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->blocks.find(handle.generation);
    if (found == impl_->blocks.end()) return false;
    Block& block = *found->second;
    std::lock_guard counters_lock(impl_->counters->mutex);
    const auto fail = [&] {
        impl_->counters->update(block.domain, [](DomainStats& s) { ++s.failures; });
        return false;
    };
    if (bytes > block.capacity) return fail();
    const auto target = align_up(bytes, impl_->page);
    const auto extra = target > block.committed ? target - block.committed : 0u;
    const auto& total = impl_->counters->values;
    if (extra > total.commit_budget || total.total.committed > total.commit_budget - extra) return fail();
    if (extra != 0u) {
#ifdef _WIN32
        if (!VirtualAlloc(block.data + block.committed, extra, MEM_COMMIT, PAGE_READWRITE)) return fail();
#else
        if (mprotect(block.data + block.committed, extra, PROT_READ | PROT_WRITE) != 0) return fail();
#endif
    }
    const auto active_delta = bytes > block.active ? bytes - block.active : 0u;
    block.committed += extra;
    block.active += active_delta;
    impl_->counters->update(block.domain, [&](DomainStats& s) {
        s.committed += extra;
        s.active += active_delta;
        s.peak_committed = std::max(s.peak_committed, s.committed);
        s.peak_active = std::max(s.peak_active, s.active);
    });
    return true;
}

Mapping MemorySystem::map(Handle handle, std::size_t offset, std::size_t bytes) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->blocks.find(handle.generation);
    if (found == impl_->blocks.end() || offset > found->second->active || bytes > found->second->active - offset) {
        std::lock_guard counters_lock(impl_->counters->mutex);
        ++impl_->counters->values.rejected_mappings;
        return {};
    }
    Mapping result;
    result.owner_ = std::make_shared<Lease>(found->second);
    result.data_ = found->second->data + offset;
    result.size_ = bytes;
    return result;
}

bool MemorySystem::release(Handle handle) noexcept {
    std::shared_ptr<Block> block;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->blocks.find(handle.generation);
        if (found == impl_->blocks.end()) {
            std::lock_guard counters_lock(impl_->counters->mutex);
            ++impl_->counters->values.invalid_releases;
            return false;
        }
        block = found->second;
        impl_->pointers.erase(block->data);
        impl_->blocks.erase(found);
        std::lock_guard counters_lock(impl_->counters->mutex);
        impl_->counters->update(block->domain, [&](DomainStats& s) {
            s.active -= block->active;
            --s.live_allocations;
        });
        block->active = 0u;
    }
    return true;
}

void* MemorySystem::allocate(std::size_t bytes, std::size_t alignment, Domain domain) {
    bytes = std::max<std::size_t>(bytes, 1u);
    const auto handle = reserve(bytes, domain, alignment);
    if (!commit(handle, bytes)) {
        release(handle);
        throw std::bad_alloc();
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->blocks.at(handle.generation)->data;
}

bool MemorySystem::deallocate(void* pointer) noexcept {
    if (!pointer) return true;
    return release(handle_of(pointer));
}

Handle MemorySystem::handle_of(const void* pointer) const noexcept {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->pointers.find(pointer);
    return found == impl_->pointers.end() ? Handle{} : Handle{found->second};
}

Snapshot MemorySystem::snapshot() const {
    std::lock_guard lock(impl_->counters->mutex);
    return impl_->counters->values;
}

std::vector<AllocationInfo> MemorySystem::allocations() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<AllocationInfo> result;
    result.reserve(impl_->blocks.size());
    for (const auto& item : impl_->blocks) {
        const auto& b = *item.second;
        result.push_back({b.handle, b.domain, reinterpret_cast<std::uintptr_t>(b.data),
                          b.capacity, b.active, b.committed, b.reserved});
    }
    return result;
}

std::uint64_t MemorySystem::budget() const noexcept { return impl_->counters->values.commit_budget; }

std::string MemorySystem::report() const {
    const auto s = snapshot();
    const auto host = host_info();
    std::ostringstream out;
    out << "[native-memory] physical-total=" << host.physical_total << " physical-available=" << host.physical_available
        << " commit-available=" << host.commit_available << " commit-budget=" << s.commit_budget
        << " process-private=" << host.process_committed
        << " process-resident=" << host.process_resident << " process-peak-resident=" << host.process_peak_resident << '\n';
    out << "[native-memory] tracked reserved=" << s.total.reserved << " committed=" << s.total.committed
        << " active-requested=" << s.total.active << " peak-active=" << s.total.peak_active
        << " peak-committed=" << s.total.peak_committed << " peak-reserved=" << s.total.peak_reserved
        << " live=" << s.total.live_allocations << " allocations=" << s.total.allocations
        << " failures=" << s.total.failures << " mappings=" << s.mappings << " live-mappings=" << s.live_mappings
        << " rejected-mappings=" << s.rejected_mappings << " invalid-releases=" << s.invalid_releases << '\n';
    for (std::size_t i = 0; i < s.domains.size(); ++i) {
        const auto& d = s.domains[i];
        out << "[native-memory] " << domain_name(static_cast<Domain>(i)) << " reserved=" << d.reserved
            << " committed=" << d.committed << " active-requested=" << d.active << " peak-active=" << d.peak_active
            << " peak-committed=" << d.peak_committed << " peak-reserved=" << d.peak_reserved
            << " live=" << d.live_allocations << " allocations=" << d.allocations << " failures=" << d.failures << '\n';
    }
    out << "[native-memory] scope=registered-native-allocations; GPU heaps and legacy game storage reported separately\n";
    return out.str();
}

std::string MemorySystem::report_json() const {
    const auto s = snapshot();
    const auto host = host_info();
    std::ostringstream out;
    const auto domain = [&](const DomainStats& d) {
        out << "{\"reserved\":" << d.reserved << ",\"committed\":" << d.committed
            << ",\"active_requested\":" << d.active << ",\"peak_reserved\":" << d.peak_reserved
            << ",\"peak_committed\":" << d.peak_committed << ",\"peak_active\":" << d.peak_active
            << ",\"live_allocations\":" << d.live_allocations << ",\"allocations\":" << d.allocations
            << ",\"failures\":" << d.failures << '}';
    };
    out << "{\"schema\":1,\"units\":\"bytes\",\"scope\":\"registered-native-allocations\",\"commit_budget\":"
        << s.commit_budget << ",\"physical_total\":" << host.physical_total
        << ",\"physical_available\":" << host.physical_available << ",\"commit_available\":" << host.commit_available
        << ",\"process_private\":" << host.process_committed << ",\"process_resident\":" << host.process_resident
        << ",\"process_peak_resident\":" << host.process_peak_resident << ",\"total\":";
    domain(s.total);
    out << ",\"mappings\":" << s.mappings << ",\"live_mappings\":" << s.live_mappings
        << ",\"rejected_mappings\":" << s.rejected_mappings << ",\"invalid_releases\":" << s.invalid_releases
        << ",\"domains\":{";
    for (std::size_t i = 0; i < s.domains.size(); ++i) {
        if (i != 0u) out << ',';
        out << '\"' << domain_name(static_cast<Domain>(i)) << "\":";
        domain(s.domains[i]);
    }
    out << "}}";
    return out.str();
}

MemorySystem& system() {
    static MemorySystem memory;
    return memory;
}

}
