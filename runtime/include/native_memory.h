#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace shadow::memory {

enum class Domain : std::size_t { Assets, Streaming, Renderer, Scratch, Decompression, Audio, General, Gameplay, Count };
const char* domain_name(Domain domain) noexcept;

struct Handle {
    std::uint64_t generation = 0;
    explicit operator bool() const noexcept { return generation != 0; }
};

struct DomainStats {
    std::uint64_t reserved = 0;
    std::uint64_t committed = 0;
    std::uint64_t active = 0;
    std::uint64_t peak_reserved = 0;
    std::uint64_t peak_committed = 0;
    std::uint64_t peak_active = 0;
    std::uint64_t live_allocations = 0;
    std::uint64_t allocations = 0;
    std::uint64_t failures = 0;
};

struct HostInfo {
    std::uint64_t physical_total = 0;
    std::uint64_t physical_available = 0;
    std::uint64_t commit_available = 0;
    std::uint64_t process_committed = 0;
    std::uint64_t process_resident = 0;
    std::uint64_t process_peak_resident = 0;
};
HostInfo host_info() noexcept;

struct Snapshot {
    DomainStats total;
    std::array<DomainStats, static_cast<std::size_t>(Domain::Count)> domains{};
    std::uint64_t commit_budget = 0;
    std::uint64_t mappings = 0;
    std::uint64_t live_mappings = 0;
    std::uint64_t rejected_mappings = 0;
    std::uint64_t invalid_releases = 0;
};

struct AllocationInfo {
    Handle handle;
    Domain domain = Domain::General;
    std::uintptr_t native_address = 0;
    std::size_t capacity = 0;
    std::size_t active = 0;
    std::size_t committed = 0;
    std::size_t reserved = 0;
};

class Mapping {
public:
    std::byte* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    explicit operator bool() const noexcept { return owner_ != nullptr; }
private:
    friend class MemorySystem;
    std::shared_ptr<void> owner_;
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

class MemorySystem {
public:
    explicit MemorySystem(std::uint64_t commit_budget = 0);
    ~MemorySystem();
    MemorySystem(const MemorySystem&) = delete;
    MemorySystem& operator=(const MemorySystem&) = delete;

    Handle reserve(std::size_t bytes, Domain domain = Domain::General,
                   std::size_t alignment = alignof(std::max_align_t));
    bool commit(Handle handle, std::size_t bytes);
    Mapping map(Handle handle, std::size_t offset, std::size_t bytes);
    bool release(Handle handle) noexcept;
    void* allocate(std::size_t bytes, std::size_t alignment, Domain domain);
    bool deallocate(void* pointer) noexcept;
    Handle handle_of(const void* pointer) const noexcept;
    Snapshot snapshot() const;
    std::vector<AllocationInfo> allocations() const;
    std::uint64_t budget() const noexcept;
    std::string report() const;
    std::string report_json() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

MemorySystem& system();

template<class T, Domain Owner> struct Allocator {
    using value_type = T;
    using is_always_equal = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    template<class U> struct rebind { using other = Allocator<U, Owner>; };
    Allocator() noexcept = default;
    template<class U> Allocator(const Allocator<U, Owner>&) noexcept {}
    T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
        return static_cast<T*>(system().allocate(count * sizeof(T), alignof(T), Owner));
    }
    void deallocate(T* pointer, std::size_t) noexcept { system().deallocate(pointer); }
    template<class U> bool operator==(const Allocator<U, Owner>&) const noexcept { return true; }
    template<class U> bool operator!=(const Allocator<U, Owner>&) const noexcept { return false; }
};

template<class T, Domain Owner>
using Vector = std::vector<T, Allocator<T, Owner>>;

}
