#include "gekko/native_frame.h"
#include "plume_fence_retirement.h"

#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>

namespace {
using namespace gekko::native;
using plume::RenderFenceWait;
int failures = 0;
void check(bool pass, const char* text, int line) {
    if (!pass) { ++failures; std::printf("FAIL line %d: %s\n", line, text); }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
template<class Action> bool throws_owner(Action&& action) {
    try { action(); }
    catch (const plume::FenceRetirementError& error) {
        return error.outcome() == RenderFenceWait::WAIT_ERROR;
    }
    return false;
}
std::exception_ptr fault() {
    return std::make_exception_ptr(plume::FenceRetirementError(RenderFenceWait::WAIT_ERROR, "test renderer"));
}
}

int main() {
    {
        SegmentQueue queue(1);
        auto first = queue.acquire();
        first->append_empty(Op::ResetState);
        queue.push(std::move(first));
        auto second = queue.acquire();
        second->append_empty(Op::ResetState);
        std::promise<void> entering;
        auto entered = entering.get_future();
        auto blocked = std::async(std::launch::async, [&] {
            entering.set_value();
            return throws_owner([&] { queue.push(std::move(second)); });
        });
        entered.wait();
        CHECK(blocked.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        queue.fail(fault());
        CHECK(blocked.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(blocked.get());
        CHECK(queue.stats().pushed == 1);
        CHECK(queue.stats().popped == 0);
        CHECK(throws_owner([&] { queue.acquire(); }));
        CHECK(throws_owner([&] { queue.pop(0); }));
        // Graceful close never erases a terminal error; first fault wins.
        queue.close();
        queue.fail(std::make_exception_ptr(std::runtime_error("later fault")));
        CHECK(throws_owner([&] { queue.pop(0); }));
    }
    {
        SegmentQueue queue;
        std::promise<void> entering;
        auto entered = entering.get_future();
        auto consumer = std::async(std::launch::async, [&] {
            entering.set_value();
            return throws_owner([&] { queue.pop(5000); });
        });
        entered.wait();
        queue.fail(fault());
        CHECK(consumer.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        CHECK(consumer.get());
    }
    {
        SegmentQueue queue;
        bool refused = false;
        try { queue.fail({}); } catch (const std::invalid_argument&) { refused = true; }
        CHECK(refused);
        CHECK(!queue.closed());
        // Existing graceful close still drains queued ownership.
        queue.push(queue.acquire());
        queue.close();
        auto drained = queue.pop(0);
        CHECK(drained != nullptr);
        queue.recycle(std::move(drained));
        CHECK(queue.pop(0) == nullptr);
    }
    std::printf("native terminal segment failure: %d failures\n", failures);
    return failures ? 1 : 0;
}
