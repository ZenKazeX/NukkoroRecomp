#pragma once

#include "gekko/render/native_gpu_lifetime.h"

#include <cassert>
#include <type_traits>

namespace gekko::render {

// Native COPY's synchronous owner reserves dependency storage before invoking
// the queue. Current Windows COPY has no queue waits and can throw C++ allocation
// failures only before ExecuteCommandLists. An unissued entry can therefore be
// removed on unwinding; unknown post-issue backend throws need a stronger attempt
// contract and are NOT certified by this storage primitive.
template<class Dependencies>
class CopyDependencyPublication {
public:
    using Pointer = typename Dependencies::value_type;
    static_assert(std::is_pointer_v<Pointer>);

    explicit CopyDependencyPublication(Dependencies& dependencies)
        : dependencies_(dependencies), index_(dependencies.size()) {
        appendRetainedOwner(dependencies_, [](Pointer& retained) noexcept { retained = nullptr; });
    }
    CopyDependencyPublication(const CopyDependencyPublication&) = delete;
    CopyDependencyPublication& operator=(const CopyDependencyPublication&) = delete;
    ~CopyDependencyPublication() noexcept {
        if (!published_) {
            assert(dependencies_.size() == index_ + 1u);
            assert(dependencies_[index_] == nullptr);
            dependencies_.pop_back();
        }
    }
    void publish(Pointer semaphore) noexcept {
        assert(!published_ && semaphore != nullptr);
        assert(dependencies_.size() == index_ + 1u);
        dependencies_[index_] = semaphore;
        published_ = true;
    }
private:
    Dependencies& dependencies_;
    typename Dependencies::size_type index_;
    bool published_ = false;
};

} // namespace gekko::render
