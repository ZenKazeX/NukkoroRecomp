#include "gekko/render/native_gpu_lifetime.h"

#include <cstdio>
#include <new>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char* text, int line) {
    if (!value) { ++failures; std::printf("FAIL line %d: %s\n", line, text); }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
struct AllocationControl { static bool fail; };
bool AllocationControl::fail = false;
template<class T> struct ThrowAllocator {
    using value_type = T;
    ThrowAllocator() = default;
    template<class U> ThrowAllocator(const ThrowAllocator<U>&) noexcept {}
    T* allocate(std::size_t size) {
        if (AllocationControl::fail) throw std::bad_alloc();
        return std::allocator<T>{}.allocate(size);
    }
    void deallocate(T* value, std::size_t size) noexcept { std::allocator<T>{}.deallocate(value, size); }
};
template<class T, class U> bool operator==(const ThrowAllocator<T>&, const ThrowAllocator<U>&) { return true; }
template<class T, class U> bool operator!=(const ThrowAllocator<T>&, const ThrowAllocator<U>&) { return false; }
struct Counts { int live = 0, destroyed = 0, unsafe = 0, mapped = 0; };
// Pure lifetime inputs represent accepted GPU use; no hardware is injected or emulated.
struct Destination {
    Counts& counts;
    bool queued = false, retired = false;
    explicit Destination(Counts& value) : counts(value) { ++counts.live; }
    ~Destination() { --counts.live; ++counts.destroyed; if (queued && !retired) ++counts.unsafe; }
    void map() { if (!retired) throw std::logic_error("read before retirement"); ++counts.mapped; }
};
struct Deferred { std::unique_ptr<Destination> first, second; };
using Owners = std::vector<Deferred, ThrowAllocator<Deferred>>;
using Published = std::vector<std::unique_ptr<Destination>, ThrowAllocator<std::unique_ptr<Destination>>>;
template<class Action> bool allocation_fails(Action&& action) {
    AllocationControl::fail = true;
    try { action(); } catch (const std::bad_alloc&) { AllocationControl::fail = false; return true; }
    AllocationControl::fail = false; return false;
}
struct Geometry { std::unique_ptr<Destination> vertex, index; };
}

int main() {
    using gekko::render::appendRetainedOwner;
    using gekko::render::PendingGpuDestination;
    {
        Counts counts;
        auto original = std::make_unique<Destination>(counts);
        original->queued = true;
        auto* identity = original.get();
        Owners owners;
        bool transferred = false;
        CHECK(allocation_fails([&] {
            appendRetainedOwner(owners, [&](Deferred& retained) noexcept {
                transferred = true; retained.first = std::move(original);
            });
        }));
        CHECK(!transferred && owners.empty() && original.get() == identity);
        CHECK(counts.live == 1 && counts.unsafe == 0);
        appendRetainedOwner(owners, [&](Deferred& retained) noexcept { retained.first = std::move(original); });
        CHECK(!original && owners.size() == 1 && owners[0].first.get() == identity);
        owners[0].first->retired = true;
        owners.clear();
        CHECK(counts.live == 0 && counts.destroyed == 1 && counts.unsafe == 0);
    }
    {
        // Negative control: the old local-then-push pattern loses an in-flight destination.
        Counts counts;
        auto original = std::make_unique<Destination>(counts);
        original->queued = true;
        Owners owners;
        CHECK(allocation_fails([&] {
            Deferred local;
            local.first = std::move(original);
            owners.push_back(std::move(local));
        }));
        CHECK(!original && owners.empty() && counts.live == 0 && counts.unsafe == 1);
    }
    for (int iteration = 0; iteration != 32; ++iteration) {
        Counts counts;
        PendingGpuDestination<Geometry> pending;
        auto asset = std::make_unique<Geometry>();
        asset->vertex = std::make_unique<Destination>(counts);
        asset->index = std::make_unique<Destination>(counts);
        auto* identity = asset.get();
        pending.adopt(std::move(asset));
        bool consumer_visible = false;
        try {
            pending.get()->vertex->queued = true;
            if (iteration & 1) pending.get()->index->queued = true;
            throw std::bad_alloc(); // next staging/metadata allocation after accepted COPY use
            consumer_visible = true;
        } catch (const std::bad_alloc&) {}
        CHECK(!consumer_visible && pending.get() == identity);
        CHECK(counts.live == 2 && counts.unsafe == 0);
        bool refused_reentry = false;
        try { pending.adopt(std::make_unique<Geometry>()); }
        catch (const std::logic_error&) { refused_reentry = true; }
        CHECK(refused_reentry && pending.get() == identity);
        pending.get()->vertex->retired = true;
        pending.get()->index->retired = true;
        pending.reset();
        CHECK(counts.live == 0 && counts.destroyed == 2 && counts.unsafe == 0);
    }
    {
        Counts counts;
        PendingGpuDestination<Destination> pending;
        auto& resource = pending.adopt(std::make_unique<Destination>(counts));
        resource.queued = true;
        Published published;
        CHECK(allocation_fails([&] { pending.publish(published); }));
        CHECK(pending.get() == &resource && published.empty() && counts.unsafe == 0);
        pending.publish(published);
        CHECK(!pending.get() && published.size() == 1 && published[0].get() == &resource);
        published[0]->retired = true;
        published.clear();
        CHECK(counts.live == 0 && counts.destroyed == 1 && counts.unsafe == 0);
    }
    {
        Counts counts;
        auto old = std::make_unique<Destination>(counts);
        old->queued = true;
        auto* old_identity = old.get();
        PendingGpuDestination<Destination> grown;
        grown.adopt(std::make_unique<Destination>(counts)).queued = true;
        auto* new_identity = grown.get();
        Owners retired_patterns;
        CHECK(allocation_fails([&] {
            appendRetainedOwner(retired_patterns, [&](Deferred& retained) noexcept { retained.first = std::move(old); });
        }));
        CHECK(old.get() == old_identity && grown.get() == new_identity && counts.live == 2);
        appendRetainedOwner(retired_patterns, [&](Deferred& retained) noexcept { retained.first = std::move(old); });
        grown.commit(old);
        CHECK(!grown.get() && old.get() == new_identity && retired_patterns[0].first.get() == old_identity);
        old->retired = true;
        retired_patterns[0].first->retired = true;
        old.reset(); retired_patterns.clear();
        CHECK(counts.live == 0 && counts.destroyed == 2 && counts.unsafe == 0);
    }
    {
        Counts counts;
        PendingGpuDestination<Destination> readback;
        auto& resource = readback.adopt(std::make_unique<Destination>(counts));
        resource.queued = true;
        bool early_map_refused = false;
        try { resource.map(); } catch (const std::logic_error&) { early_map_refused = true; }
        CHECK(early_map_refused && readback.get() == &resource && counts.live == 1);
        // A failed wait cannot reset this owner. Later actual retirement still retains it for CPU consumption.
        resource.retired = true;
        CHECK(readback.get() == &resource && counts.destroyed == 0);
        resource.map();
        CHECK(counts.mapped == 1 && counts.live == 1);
        readback.reset();
        CHECK(counts.live == 0 && counts.destroyed == 1 && counts.unsafe == 0);
    }
    std::printf("native retained destination ownership: %d failures\n", failures);
    return failures ? 1 : 0;
}
