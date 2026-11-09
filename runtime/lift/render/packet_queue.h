#pragma once

// The hand-over of frames from the game thread to the render thread (docs/lift-20261006/R2_DRIVER.md 4.2,
// R_DESIGN.md 2.1). The game never waits on rendering: a push never blocks, and when the render thread has fallen
// behind, the older unrendered packets are dropped (counted) - the render thread always draws the newest whole frame.
// Game time never depends on the GPU.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

#include "render/frame.h"
#include "render/present_trace.h"

namespace shadow::render {

class PacketQueue {
public:
    explicit PacketQueue(std::size_t depth = 2) : depth_(depth == 0 ? 1 : depth) {}

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
            packets_.push_back(std::move(packet));
            ++pushed_;
            present_trace::record(present_trace::Ev::Push, tick, game_ticks, packets_.size());
        }
        ready_.notify_one();
        return true;
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
