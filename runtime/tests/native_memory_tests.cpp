#include "../include/native_memory.h"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace shadow::memory;

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void large_native_region() {
    MemorySystem memory(128ull << 20);
    const std::size_t capacity = (8ull << 30) + 12345u;
    const auto handle = memory.reserve(capacity, Domain::Assets);
    require(memory.snapshot().total.reserved >= capacity, "native reservation was truncated");
    require(memory.snapshot().total.committed == 0, "reservation eagerly committed RAM");
    require(!memory.map(handle, 0, 1), "uncommitted memory was exposed");
    require(memory.commit(handle, 32u << 20), "cannot commit more than console RAM");
    auto mapping = memory.map(handle, 0, 32u << 20);
    require(bool(mapping), "native mapping failed above 24 MiB");
    mapping.data()[0] = std::byte{0x41};
    mapping.data()[(32u << 20) - 1u] = std::byte{0x72};
    require(mapping.data()[(32u << 20) - 1u] == std::byte{0x72}, "native upper bytes are not writable");
    require(!memory.map(handle, 4ull << 30, 1u), "64-bit uncommitted offset wrapped");
    require(memory.allocations().front().capacity == capacity, "allocation metadata lost high size bits");
}

void lease_lifetime() {
    MemorySystem memory(1u << 20);
    void* pointer = memory.allocate(8192, 256, Domain::Streaming);
    require(reinterpret_cast<std::uintptr_t>(pointer) % 256u == 0, "native alignment incorrect");
    const auto handle = memory.handle_of(pointer);
    auto lease = memory.map(handle, 4096u, 4096u);
    lease.data()[0] = std::byte{0x5A};
    require(memory.deallocate(pointer), "owner release failed");
    require(!memory.map(handle, 0, 1), "stale allocation generation resolved");
    require(memory.snapshot().total.active == 0 && memory.snapshot().total.committed != 0,
            "mapping did not pin retired backing");
    require(lease.data()[0] == std::byte{0x5A}, "released owner invalidated outstanding mapping");
    lease = {};
    require(memory.snapshot().total.committed == 0 && memory.snapshot().total.reserved == 0,
            "retired allocation was not reclaimed");
    auto next = memory.reserve(4096u, Domain::Streaming);
    require(next.generation != handle.generation, "allocation generation reused");
}

void budget_failure() {
    MemorySystem memory(65536u);
    bool failed = false;
    try { memory.allocate(1u << 20, 16u, Domain::Renderer); }
    catch (const std::bad_alloc&) { failed = true; }
    const auto state = memory.snapshot();
    require(failed && state.total.failures == 1u, "budget failure not reported");
    require(state.total.live_allocations == 0 && state.total.committed == 0 && state.total.reserved == 0,
            "failed allocation leaked backing or accounting");
    auto small = memory.reserve(4096u, Domain::Renderer);
    require(!memory.commit(small, 8192u), "commit escaped reserved allocation");
    require(memory.commit(small, 4096u), "failure poisoned the memory system");
}

void mapping_bounds_and_identity() {
    MemorySystem a(65536u), b(65536u);
    auto handle = a.reserve(8192u);
    require(a.commit(handle, 4097u), "partial commit failed");
    require(!a.map(handle, 4097u, 1u), "mapping exposed allocation padding");
    require(!a.map(handle, std::size_t(-1), 8u), "mapping overflow accepted");
    require(!b.map(handle, 0u, 1u), "foreign allocation handle resolved");
    require(!b.release(handle), "foreign allocation handle released");
    require(a.release(handle) && !a.release(handle), "double release was not rejected");
}

void mappings_outlive_owner() {
    Mapping lease;
    {
        MemorySystem owner(65536u);
        auto handle = owner.reserve(4096u, Domain::Scratch);
        require(owner.commit(handle, 4096u), "commit failed");
        lease = owner.map(handle, 0u, 4096u);
        lease.data()[0] = std::byte{0x67};
    }
    require(lease.data()[0] == std::byte{0x67}, "mapping outlived its backing owner unsafely");
}

void concurrent_domains() {
    MemorySystem memory(32u << 20);
    std::atomic<unsigned> errors{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            try {
                for (unsigned i = 0; i < 100; ++i) {
                    auto pointer = memory.allocate(4096u + i, 64u, static_cast<Domain>(t % 7u));
                    auto lease = memory.map(memory.handle_of(pointer), 0u, 4096u);
                    require(bool(lease), "concurrent map failed");
                    lease.data()[0] = std::byte{0x31};
                    require(memory.deallocate(pointer), "concurrent release failed");
                }
            } catch (...) { ++errors; }
        });
    }
    for (auto& thread : threads) thread.join();
    const auto state = memory.snapshot();
    require(errors == 0 && state.total.allocations == 800 && state.mappings == 800,
            "concurrent allocation/mapping accounting differs");
    require(state.total.active == 0 && state.total.committed == 0 && state.total.reserved == 0 &&
            state.live_mappings == 0, "concurrent allocations leaked");
}

void allocator_integration() {
    const auto before = system().snapshot();
    {
        Vector<std::uint8_t, Domain::Assets> asset(32u << 20, 0x27u);
        require(asset.back() == 0x27u, "native vector exceeded an obsolete memory ceiling");
        const auto now = system().snapshot();
        require(now.total.active >= before.total.active + asset.size(), "container allocation unaccounted");
    }
    require(system().snapshot().total.active == before.total.active, "container ownership leaked");
}
}

int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"large native reservation and commitment", large_native_region},
        {"generation handles and mapping lifetime", lease_lifetime},
        {"budget failure rollback", budget_failure},
        {"mapping bounds and owner identity", mapping_bounds_and_identity},
        {"mapping outlives memory owner", mappings_outlive_owner},
        {"concurrent allocation domains", concurrent_domains},
        {"native container allocation", allocator_integration},
    };
    unsigned failures = 0;
    for (const auto& test : cases) {
        try { test.second(); std::printf("PASS %s\n", test.first); }
        catch (const std::exception& e) { ++failures; std::printf("FAIL %s: %s\n", test.first, e.what()); }
    }
    return failures ? 1 : 0;
}
