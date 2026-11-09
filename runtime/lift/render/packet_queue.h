#pragma once

// The hand-over of frames from the game thread to the render thread (docs/lift-20261006/R2_DRIVER.md 4.2,
// R_DESIGN.md 2.1). The game never waits on rendering: a push never blocks, and when the render thread has fallen
// behind, the older unrendered packets are dropped (counted) - the render thread always draws the newest whole frame.
// Game time never depends on the GPU.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

#include "render/frame.h"
#include "render/present_trace.h"

namespace shadow::render {

// The phase lock's applied wall-anchor shift (gekko/phase_lock.h; written by the game thread, which also pushes):
// each packet carries the value it was produced under, so a hold measured later can be corrected for the shift
// applied since (the shared phase controller's target, not a lagging error).
inline std::atomic<std::int64_t>& phase_anchor_shift_ns() {
    static std::atomic<std::int64_t> value{0};
    return value;
}
class PacketQueue {
public:
    explicit PacketQueue(std::size_t depth = 2) : depth_(depth == 0 ? 1 : depth) {}

    // Render thread: reads the newest queued packet in place (under the queue's lock; f must be quick and keep no
    // reference). False when the queue is empty.
    template <typename F>
    bool with_newest(F&& f) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (packets_.empty())
            return false;
        f(static_cast<const FramePacket&>(packets_.back()));
        return true;
    }

    // Game thread. Does not wait for the consumer; false after close(), leaving packet owned by the caller.
    bool push(FramePacket&& packet) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_)
                return false;
            while (packets_.size() >= depth_) {
                present_trace::record(present_trace::Ev::DropOnPush, packets_.front().tick);
                packets_.pop_front();
                ++dropped_;
            }
            const std::uint64_t tick = packet.tick, game_ticks = packet.game_ticks;
            packet.pushed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
            packet.anchor_shift_ns = phase_anchor_shift_ns().load(std::memory_order_relaxed);
            packets_.push_back(std::move(packet));
            ++pushed_;
            present_trace::record(present_trace::Ev::Push, tick, game_ticks, packets_.size());
        }
        ready_.notify_one();
        return true;
    }

    // Render thread: true once a packet is waiting (up to `wait`), without taking it. The render thread then takes the
    // swap chain's credit and only after that the newest packet, which may be newer than this one.
    bool wait_for_packet(std::chrono::milliseconds wait) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait_for(lock, wait, [this] { return !packets_.empty() || closed_; });
        return !packets_.empty() && !closed_;
    }

    // Render thread: the newest packet (older ones are dropped), waiting up to `wait` for one. False on timeout or
    // after close().
    bool pop_newest(FramePacket* out, std::chrono::milliseconds wait) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait_for(lock, wait, [this] { return !packets_.empty() || closed_; });
        if (packets_.empty())
            return false;
        while (packets_.size() > 1) {
            present_trace::record(present_trace::Ev::DropOnPop, packets_.front().tick);
            packets_.pop_front();
            ++dropped_;
        }
        *out = std::move(packets_.front());
        packets_.pop_front();
        ++popped_;
        return true;
    }

    // No more rendering will consume this queue. Release pending packets and
    // deque storage before returning; cancellation is distinct from backlog drops.
    // Idempotent, and serialized with push/pop so accepted packets have one owner.
    void close() {
        std::deque<FramePacket> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            discarded_on_close_ += packets_.size();
            for (const FramePacket& packet : packets_)
                present_trace::record(present_trace::Ev::DropOnClose, packet.tick);
            discarded.swap(packets_);
        }
        ready_.notify_all();
        // Payload destruction occurs outside the queue lock.
    }

    struct Stats {
        std::uint64_t pushed = 0, popped = 0, dropped = 0, discarded_on_close = 0, pending = 0;
    };
    Stats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return Stats{pushed_, popped_, dropped_, discarded_on_close_, packets_.size()};
    }

private:
    const std::size_t depth_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<FramePacket> packets_;
    bool closed_ = false;
    std::uint64_t pushed_ = 0, popped_ = 0, dropped_ = 0, discarded_on_close_ = 0;
};

}  // namespace shadow::render
