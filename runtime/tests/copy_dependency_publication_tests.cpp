#include "gekko/render/copy_dependency_publication.h"

#include <cstdio>
#include <memory>
#include <new>
#include <optional>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char* expression, int line) {
    if (!value) { ++failures; std::printf("FAIL line %d: %s\n", line, expression); }
}
#define CHECK(value) check(bool(value), #value, __LINE__)
struct AllocationControl {
    static bool deny;
    static unsigned requests, denied;
};
bool AllocationControl::deny = false;
unsigned AllocationControl::requests = 0, AllocationControl::denied = 0;
template<class T> struct ThrowAllocator {
    using value_type = T;
    ThrowAllocator() = default;
    template<class U> ThrowAllocator(const ThrowAllocator<U>&) noexcept {}
    T* allocate(std::size_t size) {
        ++AllocationControl::requests;
        if (AllocationControl::deny) { ++AllocationControl::denied; throw std::bad_alloc(); }
        return std::allocator<T>{}.allocate(size);
    }
    void deallocate(T* pointer, std::size_t size) noexcept {
        std::allocator<T>{}.deallocate(pointer, size);
    }
};
template<class T, class U> bool operator==(const ThrowAllocator<T>&, const ThrowAllocator<U>&) noexcept { return true; }
template<class T, class U> bool operator!=(const ThrowAllocator<T>&, const ThrowAllocator<U>&) noexcept { return false; }
// Addresses are dependency identities only. These tests exercise the actual
// storage/ordering primitive, not GPU execution or emulated fence semantics.
using Dependencies = std::vector<const int*, ThrowAllocator<const int*>>;
using Publication = gekko::render::CopyDependencyPublication<Dependencies>;
struct BeforeExecutionFailure { unsigned owner = 71; };
Dependencies full_prefix(const int* prefix) {
    Dependencies dependencies;
    dependencies.reserve(1);
    // Fill the actual capacity; do not assume reserve() chooses exactly one.
    while (dependencies.size() != dependencies.capacity()) dependencies.push_back(prefix);
    return dependencies;
}
bool prefix_preserved(const Dependencies& dependencies, std::size_t count, const int* prefix) {
    if (dependencies.size() != count) return false;
    for (auto* value : dependencies) if (value != prefix) return false;
    return true;
}
}

int main() {
    const int older = 1, next = 2;
    {
        auto dependencies = full_prefix(&older);
        const auto prefix_count = dependencies.size();
        unsigned executions = 0;
        bool caught = false;
        AllocationControl::deny = true;
        try {
            Publication publication(dependencies);
            ++executions; // Corresponds to the caller's later, unwrapped queue call.
            publication.publish(&next);
        } catch (const std::bad_alloc&) { caught = true; }
        AllocationControl::deny = false;
        CHECK(caught && executions == 0);
        CHECK(prefix_preserved(dependencies, prefix_count, &older));
    }
    {
        // Exact old ordering is the negative control: the accepted work occurs
        // before the retained dependency vector acquires its next entry.
        auto dependencies = full_prefix(&older);
        const auto prefix_count = dependencies.size();
        unsigned executions = 0;
        bool caught = false;
        AllocationControl::deny = true;
        try { ++executions; dependencies.push_back(&next); }
        catch (const std::bad_alloc&) { caught = true; }
        AllocationControl::deny = false;
        CHECK(caught && executions == 1);
        CHECK(prefix_preserved(dependencies, prefix_count, &older));
    }
    {
        // Only a proven pre-execution C++ failure is covered by rollback.
        // A backend that can throw after acceptance requires an attempt token.
        auto dependencies = full_prefix(&older);
        const auto prefix_count = dependencies.size();
        unsigned executions = 0;
        bool caught_exact = false;
        try {
            Publication publication(dependencies);
            CHECK(dependencies.size() == prefix_count + 1 && dependencies.back() == nullptr);
            throw BeforeExecutionFailure{};
            ++executions;
            publication.publish(&next);
        } catch (const BeforeExecutionFailure& error) { caught_exact = error.owner == 71; }
        CHECK(caught_exact && executions == 0);
        CHECK(prefix_preserved(dependencies, prefix_count, &older));
    }
    {
        auto dependencies = full_prefix(&older);
        const auto prefix_count = dependencies.size();
        unsigned executions = 0;
        {
            Publication publication(dependencies);
            ++executions;
            const auto requests_before = AllocationControl::requests;
            const auto denied_before = AllocationControl::denied;
            AllocationControl::deny = true;
            publication.publish(&next);
            CHECK(AllocationControl::requests == requests_before);
            CHECK(AllocationControl::denied == denied_before);
            CHECK(dependencies.size() == prefix_count + 1 && dependencies.back() == &next);
            // Destructor also runs under allocation denial after publication.
        }
        AllocationControl::deny = false;
        CHECK(executions == 1 && dependencies.size() == prefix_count + 1);
        for (std::size_t i = 0; i != prefix_count; ++i) CHECK(dependencies[i] == &older);
    }
    {
        // Exercise the exact optional integration shape; no dependency means
        // no placeholder, and no publication to a null semaphore is invented.
        Dependencies dependencies;
        std::optional<Publication> publication;
        const int* semaphore = nullptr;
        if (semaphore != nullptr) publication.emplace(dependencies);
        CHECK(!publication && dependencies.empty());
    }
    {
        Dependencies dependencies;
        for (unsigned iteration = 0; iteration != 1024; ++iteration) {
            const auto count = dependencies.size();
            {
                Publication publication(dependencies);
                CHECK(dependencies.size() == count + 1);
                if (iteration % 3 != 0) publication.publish(&next);
            }
            CHECK(dependencies.size() == count + (iteration % 3 != 0 ? 1 : 0));
            for (auto* value : dependencies) CHECK(value == &next);
        }
        const auto capacity = dependencies.capacity();
        dependencies.clear();
        const auto requests_before = AllocationControl::requests;
        for (unsigned iteration = 0; iteration != 1024; ++iteration) {
            Publication publication(dependencies); // Unissued attempt; exact rollback.
        }
        CHECK(dependencies.empty() && dependencies.capacity() == capacity);
        CHECK(AllocationControl::requests == requests_before);
    }
    std::printf("COPY dependency publication design: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
