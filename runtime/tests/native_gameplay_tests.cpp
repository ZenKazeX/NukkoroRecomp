#include "../include/native_gameplay.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <thread>

using namespace shadow::gameplay;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct RecordedTask : Task {
    std::vector<int>& trace;
    int tag;
    std::function<void(UpdateContext&)> action;
    RecordedTask(std::vector<int>& out, int value, std::function<void(UpdateContext&)> work = {})
        : trace(out), tag(value), action(std::move(work)) {}
    ~RecordedTask() override { trace.push_back(-tag); }
    void update(UpdateContext& context) override {
        trace.push_back(tag);
        require(context.delta_seconds == 0.125f, "frame delta changed");
        if (action) action(context);
    }
};

void topology_and_order() {
    std::vector<int> trace;
    shadow::execution::Executor jobs(2);
    TaskSystem tasks(jobs);
    require(tasks.children(tasks.root()).size() == 6, "persistent layer topology is wrong");
    require(tasks.children(tasks.layer(Layer::Scene)).size() == 13, "scene layer topology is wrong");
    require(tasks.stats().live == 20, "native layer ownership count is wrong");
    for (unsigned i = 0; i < 19; ++i) {
        const auto layer = static_cast<Layer>(i);
        if (layer != Layer::Scene) tasks.create<RecordedTask>(tasks.layer(layer), "record", {}, trace, int(i + 1));
    }
    tasks.tick(0.125f);
    const std::vector<int> expected{1,2,3,7,8,9,10,11,12,13,15,14,16,17,18,19,5,6};
    require(trace == expected, "native hierarchy does not match original layer traversal");
    trace.clear();
    tasks.tick(0.125f);
    require(trace == expected && tasks.stats().frames == 2, "ordinary frame update rebuilt scene tasks");
}

void pause_and_deferred_removal() {
    std::vector<int> trace;
    shadow::execution::Executor jobs(1);
    TaskSystem tasks(jobs);
    const auto owner = tasks.layer(Layer::System);
    auto a = tasks.create<RecordedTask>(owner, "a", {}, trace, 1);
    auto b = tasks.create<RecordedTask>(a, "b", {true, true}, trace, 2);
    tasks.create<RecordedTask>(b, "c", {}, trace, 3);
    tasks.create<RecordedTask>(owner, "d", {true, true}, trace, 4);
    tasks.set_paused(true);
    tasks.tick(0.125f);
    require(trace == std::vector<int>({2,4}), "paused parent blocked eligible descendants");
    trace.clear();
    tasks.set_enabled(a, false);
    tasks.tick(0.125f);
    require(trace == std::vector<int>({4}), "disabled task did not suppress its subtree");
    trace.clear();
    tasks.request_destroy(a);
    tasks.tick(0.125f);
    require(trace == std::vector<int>({4}) && tasks.alive(a), "pause did not defer subtree destruction");
    trace.clear();
    tasks.set_paused(false);
    tasks.tick(0.125f);
    require(trace == std::vector<int>({-3,-2,-1,4}), "native subtree teardown was not bottom-up");
    require(!tasks.alive(a) && !tasks.find(b) && !tasks.request_destroy(a), "retired task handle resolved");
}

void self_removal_and_append() {
    std::vector<int> trace;
    shadow::execution::Executor jobs(1);
    TaskSystem tasks(jobs);
    auto owner = tasks.layer(Layer::System);
    bool first = true;
    auto a = tasks.create<RecordedTask>(owner, "a", {}, trace, 1, [&](UpdateContext& context) {
        if (first) {
            first = false;
            tasks.create<RecordedTask>(context.self, "child", {}, trace, 2);
            tasks.create<RecordedTask>(owner, "appended", {}, trace, 4);
            tasks.request_destroy(context.self);
        }
    });
    tasks.create<RecordedTask>(owner, "existing", {}, trace, 3);
    tasks.tick(0.125f);
    require(trace == std::vector<int>({1,2,3,4}) && tasks.alive(a), "in-frame creation or self-removal order changed");
    trace.clear();
    tasks.tick(0.125f);
    require(trace == std::vector<int>({-2,-1,3,4}), "self-removal did not wait until the next visit");
}

void scene_reset() {
    std::vector<int> trace;
    shadow::execution::Executor jobs(1);
    TaskSystem tasks(jobs);
    auto old_player = tasks.layer(Layer::Player);
    auto old_system = tasks.layer(Layer::System);
    auto old_render = tasks.layer(Layer::Render);
    tasks.create<RecordedTask>(old_player, "player", {}, trace, 1);
    tasks.create<RecordedTask>(tasks.layer(Layer::Enemy), "enemy", {}, trace, 2);
    auto persistent = tasks.create<RecordedTask>(old_system, "system", {}, trace, 3);
    tasks.reset_scene();
    require(trace == std::vector<int>({-2,-1}), "scene reset changed destruction order");
    require(tasks.alive(persistent) && tasks.layer(Layer::System) == old_system && tasks.layer(Layer::Render) == old_render,
            "scene reset destroyed persistent systems");
    require(!tasks.alive(old_player) && tasks.layer(Layer::Player) != old_player, "scene handles did not change generation");
    require(tasks.children(tasks.layer(Layer::Scene)).size() == 13 && tasks.stats().frames == 0,
            "scene reset was confused with a frame update");
}

void paused_reset() {
    std::vector<int> trace;
    shadow::execution::Executor jobs(1);
    TaskSystem tasks(jobs);
    const auto old = tasks.layer(Layer::Player);
    auto actor = tasks.create<RecordedTask>(old, "actor", {true, true}, trace, 1);
    tasks.set_paused(true);
    tasks.reset_scene();
    require(tasks.alive(actor) && tasks.children(tasks.layer(Layer::Scene)).size() == 26,
            "paused reset lost pending scene ownership");
    tasks.tick(0.125f);
    require(trace.empty(), "pending removal executed while paused");
    tasks.set_paused(false);
    tasks.tick(0.125f);
    require(trace == std::vector<int>({-1}) && !tasks.alive(old), "pending scene was not retired after unpause");
    require(tasks.children(tasks.layer(Layer::Scene)).size() == 13, "old scene layers leaked");
}

void reparent_and_manual_pump() {
    std::vector<int> trace;
    shadow::execution::Executor jobs(2);
    TaskSystem tasks(jobs);
    auto a = tasks.create<RecordedTask>(tasks.layer(Layer::System), "a", {}, trace, 1,
        [&](UpdateContext& context) { tasks.update_children(context.self, context.delta_seconds); });
    auto b = tasks.create<RecordedTask>(a, "b", {}, trace, 2);
    require(!tasks.reparent(a, b), "native ownership cycle accepted");
    tasks.tick(0.125f);
    require(trace == std::vector<int>({1,2,2}), "explicit subtree pump did not compose with automatic traversal");
    require(tasks.reparent(b, tasks.layer(Layer::Audio)), "native reparent failed");
    require(tasks.parent(b) == tasks.layer(Layer::Audio), "native parent handle is wrong");
    trace.clear();
    tasks.tick(0.125f);
    require(trace == std::vector<int>({1,2}), "native reparent left duplicate ownership");
}

void native_jobs_and_large_state() {
    struct Compute : Task {
        shadow::memory::Vector<std::uint8_t, shadow::memory::Domain::Gameplay> bytes;
        std::array<unsigned, 32> results{};
        unsigned total = 0;
        Compute() : bytes(32u << 20, 0x37u) {}
        void update(UpdateContext& context) override {
            shadow::execution::TaskGraph graph;
            std::vector<shadow::execution::TaskGraph::TaskId> inputs;
            for (unsigned i = 0; i < results.size(); ++i) inputs.push_back(graph.add([&, i] { results[i] = i * i; }));
            graph.add([&] { for (auto value : results) total += value; }, inputs);
            context.jobs.execute(graph);
        }
    };
    shadow::execution::Executor jobs(3);
    TaskSystem tasks(jobs);
    auto id = tasks.create<Compute>(tasks.layer(Layer::Player), "native state", {});
    tasks.tick(0.125f);
    auto* state = dynamic_cast<Compute*>(tasks.find(id));
    require(state && state->bytes.size() == (32u << 20) && state->bytes.back() == 0x37u && state->total == 10416u,
            "native gameplay state or dependency computation failed");
    require(shadow::memory::system().snapshot().domains[static_cast<std::size_t>(shadow::memory::Domain::Gameplay)].active > (32u << 20),
            "gameplay ownership was not charged to native memory");
}

void owner_and_exception_guards() {
    std::vector<int> trace;
    shadow::execution::Executor jobs(1);
    TaskSystem tasks(jobs);
    std::atomic<bool> rejected{false};
    auto parent = tasks.layer(Layer::System);
    std::thread other([&] {
        try { tasks.create<RecordedTask>(parent, "wrong-thread", {}, trace, 99); }
        catch (const std::logic_error&) { rejected = true; }
    });
    other.join();
    require(rejected && trace.empty(), "cross-thread mutation ran a task constructor/destructor");
    auto id = tasks.create<RecordedTask>(parent, "throws", {}, trace, 1, [](UpdateContext&) { throw std::runtime_error("task failure"); });
    bool threw = false;
    try { tasks.tick(0.125f); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "gameplay exception was hidden");
    tasks.set_enabled(id, false);
    tasks.tick(0.125f);
    require(tasks.stats().frames == 2, "failed callback left traversal locked");
}
}

int main() {
    const std::pair<const char*, void(*)()> tests[] = {
        {"verified hierarchy and frame order", topology_and_order},
        {"pause and deferred bottom-up removal", pause_and_deferred_removal},
        {"self-removal and same-frame append", self_removal_and_append},
        {"scene lifecycle distinct from frame pump", scene_reset},
        {"paused scene retirement", paused_reset},
        {"reparenting and explicit subtree pumps", reparent_and_manual_pump},
        {"native jobs and state beyond 24 MiB", native_jobs_and_large_state},
        {"owner-thread and exception boundaries", owner_and_exception_guards},
    };
    unsigned failed = 0;
    for (const auto& test : tests) {
        try { test.second(); std::printf("PASS %s\n", test.first); }
        catch (const std::exception& e) { ++failed; std::printf("FAIL %s: %s\n", test.first, e.what()); }
    }
    return failed ? 1 : 0;
}
