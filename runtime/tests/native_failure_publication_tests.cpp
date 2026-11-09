#include "gekko/native_frame.h"
#include "gekko/native_failure_publication.h"
#include "gekko/native_renderer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <new>
#include <stdexcept>

namespace {
int failures = 0;
void check(bool value, const char* text, int line) {
    if (!value) { ++failures; std::printf("FAIL line %d: %s\n", line, text); }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
struct OwnerFault : std::exception {
    std::shared_ptr<const unsigned> owner;
    unsigned sequence;
    OwnerFault(std::shared_ptr<const unsigned> identity, unsigned number) noexcept
        : owner(std::move(identity)), sequence(number) {}
    const char* what() const noexcept override { return "native owner fault"; }
};
template<class Action> bool receives(Action&& action, const OwnerFault& original) {
    try { action(); }
    catch (const OwnerFault& received) {
        // MSVC current_exception makes a fresh exception copy; reference-field
        // address equality is not transport identity. Exact type, immutable
        // ownership token and sequence prove that the first fault survived.
        return received.owner.get() == original.owner.get() && received.sequence == original.sequence;
    }
    catch (...) { return false; }
    return false;
}
}

int main() {
    using namespace gekko::native;
    {
        SegmentQueue queue(1);
        queue.push(queue.acquire());
        auto second = queue.acquire();
        const OwnerFault expected(std::make_shared<const unsigned>(0xA117u), 1);
        const auto original = std::make_exception_ptr(expected);
        CHECK(receives([&] { std::rethrow_exception(original); }, expected));
        std::atomic<bool> failed{false};
        std::promise<void> entering;
        auto entered = entering.get_future();
        auto producer = std::async(std::launch::async, [&] {
            entering.set_value();
            return receives([&] { queue.push(std::move(second)); }, expected);
        });
        entered.wait();
        CHECK(producer.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        bool diagnostic_called = false;
        publishFailureBeforeDiagnostics([&] {
            failed.store(true, std::memory_order_release);
            queue.fail(original);
        }, [&] {
            diagnostic_called = true;
            CHECK(failed.load(std::memory_order_acquire) && queue.closed());
            // Publication must let the real blocked producer finish BEFORE diagnostic allocation.
            CHECK(producer.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
            throw std::bad_alloc();
        });
        CHECK(diagnostic_called && failed.load(std::memory_order_acquire));
        CHECK(producer.get());
        CHECK(queue.stats().pushed == 1 && queue.stats().popped == 0);
        queue.fail(std::make_exception_ptr(OwnerFault(std::make_shared<const unsigned>(0xB228u), 2)));
        CHECK(receives([&] { queue.acquire(); }, expected));
        CHECK(receives([&] { queue.pop(0); }, expected));
    }
    {
        SegmentQueue queue;
        const OwnerFault expected(std::make_shared<const unsigned>(0xC339u), 3);
        const auto original = std::make_exception_ptr(expected);
        std::atomic<bool> failed{false};
        std::promise<void> entering;
        auto entered = entering.get_future();
        auto consumer = std::async(std::launch::async, [&] {
            entering.set_value();
            return receives([&] { queue.pop(5000); }, expected);
        });
        entered.wait();
        publishFailureBeforeDiagnostics([&] {
            failed.store(true, std::memory_order_release);
            queue.fail(original);
        }, [&] {
            CHECK(failed.load(std::memory_order_acquire) && queue.closed());
            CHECK(consumer.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
            throw std::bad_alloc();
        });
        CHECK(consumer.get() && failed.load(std::memory_order_acquire));
        CHECK(queue.stats().pushed == 0 && queue.stats().popped == 0);
    }
    {
        int publication = 0, diagnosis = 0;
        publishFailureBeforeDiagnostics([&] { ++publication; }, [&] { CHECK(publication == 1); ++diagnosis; });
        CHECK(publication == 1 && diagnosis == 1);
    }
    {
        gekko::NativeRenderer renderer;
        gekko::NativeRendererConfig config;
        config.gpu = false;
        CHECK(!renderer.failed());
        CHECK(renderer.start(config));
        const OwnerFault expected(std::make_shared<const unsigned>(0xD44Au), 4);
        renderer.queue().fail(std::make_exception_ptr(expected));
        renderer.queue().fail(std::make_exception_ptr(OwnerFault(std::make_shared<const unsigned>(0xE55Bu), 5)));
        renderer.stop(); // actual worker catches its queue fault; owning join completes
        CHECK(renderer.failed());
        CHECK(renderer.last_error() == expected.what());
        CHECK(receives([&] { renderer.queue().pop(0); }, expected));
        // start explicitly refuses a retained terminal owner; no implicit device restart.
        CHECK(!renderer.start(config));
        renderer.stop();
        CHECK(renderer.failed());
        CHECK(renderer.report().segments == 0);
    }
    {
        gekko::NativeRenderer renderer;
        gekko::NativeRendererConfig config;
        config.gpu = false;
        CHECK(renderer.start(config));
        renderer.queue().push(renderer.queue().acquire());
        renderer.stop();
        CHECK(!renderer.failed() && renderer.last_error().empty());
        CHECK(renderer.report().segments == 1);
    }
    std::printf("native first-fault publication before diagnostics: %d failures\n", failures);
    return failures ? 1 : 0;
}
