#pragma once

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace gekko::render {

// Acquire retained storage while the original resource is still owned. Once
// allocation succeeds, ownership transfer itself must be nonthrowing.
template<class Container, class Transfer>
void appendRetainedOwner(Container& owners, Transfer&& transfer) {
    static_assert(std::is_nothrow_invocable_v<Transfer, typename Container::value_type&>);
    owners.emplace_back();
    std::forward<Transfer>(transfer)(owners.back());
}

// One synchronous native construction can be pending on this owner. Resource
// lifetime starts before staging can submit; consumer visibility starts only
// after commit. Failed construction remains owned for the final GPU drain.
template<class Resource>
class PendingGpuDestination {
public:
    Resource* get() const noexcept { return resource_.get(); }
    Resource& adopt(std::unique_ptr<Resource> resource) {
        if (resource_) throw std::logic_error("native GPU destination construction already pending");
        if (!resource) throw std::invalid_argument("native GPU destination is null");
        resource_ = std::move(resource);
        return *resource_;
    }
    void commit(std::unique_ptr<Resource>& destination) noexcept {
        destination = std::move(resource_);
    }
    template<class Container>
    void publish(Container& destinations) {
        appendRetainedOwner(destinations, [&](auto& destination) noexcept {
            destination = std::move(resource_);
        });
    }
    void reset() noexcept { resource_.reset(); }
private:
    std::unique_ptr<Resource> resource_;
};

} // namespace gekko::render
