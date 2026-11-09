#pragma once

// TextureUVAnimManagerTask's own time (docs/distant-rendering/uv_anim_start.md 1.3).
//
// The task (vtable 0x8053EDC4, execute 0x801EB790) advances every registered model's UV animation by the frame delta
// the task traversal hands it (RtAnimInterpolatorAddAnimTime 0x80478F24: nothing for dt <= 0, else time += dt, wrapped
// by the duration). So the sum of the deltas it was given is the game's UV animation time: it moves exactly when and as
// fast as the game's registered animations do, stands whenever the traversal skips the task, and holds every whole
// period of a long frame. (Measured 2026-11-05: the pause menu does not stop it - registered animations keep running
// under the menu in the original's code too.)
//
// The native gameplay scheduler (gameplay_scheduler.cpp, dispatch_execute) adds each delta it hands that execute; the
// renderer's shared UV clocks (rw_driver/pipelines.cpp, advance_uv_clocks) step by it. Both run on the CPU thread.
// Under --legacy-scheduler nothing feeds it (steps stays 0) and the renderer keeps its own frame time instead.

#include <atomic>
#include <cstdint>

namespace gekko {

struct UvAnimManagerTime {
    std::atomic<double> seconds{0.0};       // the sum of the positive deltas given to the execute
    std::atomic<std::uint64_t> steps{0};    // the executes counted
};

inline constexpr std::uint32_t kUvAnimManagerExecute = 0x801EB790u;
inline UvAnimManagerTime uv_anim_manager_time;

inline void note_uv_anim_manager_step(float dt) noexcept {
    uv_anim_manager_time.steps.fetch_add(1u, std::memory_order_relaxed);
    if (dt > 0.0f)
        uv_anim_manager_time.seconds.store(uv_anim_manager_time.seconds.load(std::memory_order_relaxed) + dt,
                                           std::memory_order_relaxed);
}

}  // namespace gekko
