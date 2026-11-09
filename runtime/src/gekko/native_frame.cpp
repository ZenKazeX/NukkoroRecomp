#include "gekko/native_frame.h"

#include <algorithm>
#include <chrono>

#include "gekko/profiling.h"

namespace gekko::native {

const char* op_name(Op op) noexcept {
    switch (op) {
        case Op::Viewport: return "Viewport";
        case Op::Scissor: return "Scissor";
        case Op::ScissorOffset: return "ScissorOffset";
        case Op::GlareWindow: return "GlareWindow";
        case Op::Projection: return "Projection";
        case Op::PositionMatrix: return "PositionMatrix";
        case Op::NormalMatrix: return "NormalMatrix";
        case Op::CurrentMatrix: return "CurrentMatrix";
        case Op::TextureMatrix: return "TextureMatrix";
        case Op::Blend: return "Blend";
        case Op::DepthMode: return "DepthMode";
        case Op::DepthBeforeTexture: return "DepthBeforeTexture";
        case Op::AlphaCompare: return "AlphaCompare";
        case Op::Cull: return "Cull";
        case Op::Fog: return "Fog";
        case Op::ColorUpdate: return "ColorUpdate";
        case Op::AlphaUpdate: return "AlphaUpdate";
        case Op::DestinationAlpha: return "DestinationAlpha";
        case Op::ChannelCount: return "ChannelCount";
        case Op::ChannelControl: return "ChannelControl";
        case Op::MaterialColor: return "MaterialColor";
        case Op::AmbientColor: return "AmbientColor";
        case Op::Light: return "Light";
        case Op::TexGenCount: return "TexGenCount";
        case Op::TexGen: return "TexGen";
        case Op::TevRegisterColor: return "TevRegisterColor";
        case Op::TevKonstColor: return "TevKonstColor";
        case Op::BindTexture: return "BindTexture";
        case Op::TextureContent: return "TextureContent";
        case Op::ReleaseTexture: return "ReleaseTexture";
        case Op::GeometryContent: return "GeometryContent";
        case Op::GeometryDynamic: return "GeometryDynamic";
        case Op::ReleaseGeometry: return "ReleaseGeometry";
        case Op::DrawImmediate: return "DrawImmediate";
        case Op::DrawMesh: return "DrawMesh";
        case Op::Clear: return "Clear";
        case Op::CopyTexture: return "CopyTexture";
        case Op::CameraBegin: return "CameraBegin";
        case Op::CameraEnd: return "CameraEnd";
        case Op::Present: return "Present";
        case Op::ResetState: return "ResetState";
        case Op::HudGroup: return "HudGroup";
        case Op::HudScene: return "HudScene";
        case Op::Count: break;
    }
    return "?";
}

const char* reject_reason_name(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::ConvertFailed: return "convert-failed";
        case RejectReason::BufferCreate: return "buffer-create";
        case RejectReason::VertexStaging: return "vertex-staging";
        case RejectReason::IndexStaging: return "index-staging";
        case RejectReason::TextureCreate: return "texture-create";
        case RejectReason::TextureUpload: return "texture-upload";
        case RejectReason::Unknown:
        case RejectReason::Count:
            break;
    }
    return "unknown";
}

std::unique_ptr<FrameSegment> SegmentQueue::acquire() {
    std::lock_guard lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
    if (!primed_) {
        primed_ = true;
        const std::size_t ring = capacity_ + 2u;
        for (std::size_t i = 0; i < ring; ++i) pool_.push_back(std::make_unique<FrameSegment>());
        segments_allocated_ = ring;
    }
    // After a push at most capacity + 1 segments are out (queued, plus the one
    // the renderer is processing), so with a ring of capacity + 2 the oldest is
    // always back. Empty here means the producer acquired while still holding a
    // segment; growing the ring would hide that.
    if (pool_.empty()) throw std::logic_error("SegmentQueue::acquire: producer already holds a segment");
    auto segment = std::move(pool_.front());
    pool_.pop_front();
    return segment;
}

u64 SegmentQueue::push(std::unique_ptr<FrameSegment> segment) {
    // This zone lands on the GUEST thread, and its duration IS the renderer's
    // back-pressure on the guest. The session scalars below already hold the
    // total; the zone is the per-frame shape they cannot show.
    GEKKO_PROFILE_ZONE("gpu/segment enqueue");
    std::unique_lock lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
    u64 waited = 0;
    if (queue_.size() >= capacity_ && !closed_.load(std::memory_order_relaxed)) {
        const auto begin = std::chrono::steady_clock::now();
        not_full_.wait(lock, [this] {
            return queue_.size() < capacity_ || closed_.load(std::memory_order_relaxed);
        });
        waited = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - begin)
                                      .count());
        ++waits_;
        wait_ns_ += waited;
        wait_ns_max_ = std::max(wait_ns_max_, waited);
        // Measured 1 in 680 frames and 2 in 65,438: rare, individually
        // interesting, and a 60 Hz plot sample steps straight over it.
        GEKKO_PROFILE_MESSAGE_LITERAL("gpu/segment queue full: the guest waited");
    }
    // Both of these sit after the wait block rather than before a return: push
    // has two exits, and a value attached on only one of them loses half the
    // samples. A flat zero line is the proof that back-pressure is absent,
    // which a plot sampled only when it waits cannot give.
    GEKKO_PROFILE_VALUE(waited);
    GEKKO_PROFILE_PLOT("gpu/producer wait ns", static_cast<double>(waited));
    if (failure_) std::rethrow_exception(failure_);
    if (closed_.load(std::memory_order_relaxed)) {
        // Nobody will consume it; keep the allocation for reuse.
        segment->clear();
        pool_.push_back(std::move(segment));
        return waited;
    }
    queue_.push_back(std::move(segment));
    ++pushed_;
    depth_max_ = std::max<u64>(depth_max_, queue_.size());
    // The class keeps only depth_max_ for the whole session, which cannot tell
    // "touched the cap twice" from "sat at the cap for twenty seconds". Read
    // here, under the lock already held: depth() and stats() both take mutex_
    // and std::mutex is not recursive, so calling either from in here would
    // deadlock on the spot. Published once the lock is released.
    GEKKO_PROFILE_ONLY(const std::size_t depth_after_push = queue_.size();)
    lock.unlock();
    GEKKO_PROFILE_PLOT("gpu/segment queue depth", static_cast<double>(depth_after_push));
    not_empty_.notify_one();
    return waited;
}

std::unique_ptr<FrameSegment> SegmentQueue::pop(u32 timeout_ms) {
    // Deliberately covers not_empty_.wait_for: a render thread parked 15 ms per
    // frame reads directly as "the guest is the bottleneck", and that is the
    // measurement, not an accident of where the zone was opened.
    GEKKO_PROFILE_ZONE("gpu/segment wait");
    std::unique_lock lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
    if (queue_.empty()) {
        if (timeout_ms == 0u) return nullptr;
        not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
            return !queue_.empty() || closed_.load(std::memory_order_relaxed);
        });
        if (failure_) std::rethrow_exception(failure_);
        if (queue_.empty()) return nullptr;
    }
    auto segment = std::move(queue_.front());
    queue_.erase(queue_.begin());
    ++popped_;
    // Same local-under-lock rule as push. The two early returns above leave the
    // plot unsampled, which is correct - a stepped plot holds its last value.
    GEKKO_PROFILE_ONLY(const std::size_t depth_after_pop = queue_.size();)
    lock.unlock();
    GEKKO_PROFILE_PLOT("gpu/segment queue depth", static_cast<double>(depth_after_pop));
    not_full_.notify_one();
    return segment;
}

void SegmentQueue::close() {
    {
        std::lock_guard lock(mutex_);
        closed_.store(true, std::memory_order_relaxed);
    }
    not_full_.notify_all();
    not_empty_.notify_all();
}

void SegmentQueue::fail(std::exception_ptr failure) {
    if (!failure) throw std::invalid_argument("SegmentQueue::fail needs a native failure");
    {
        std::lock_guard lock(mutex_);
        if (!failure_) failure_ = std::move(failure);
        closed_.store(true, std::memory_order_relaxed);
    }
    not_full_.notify_all();
    not_empty_.notify_all();
}

SegmentQueue::Stats SegmentQueue::stats() const {
    std::lock_guard lock(mutex_);
    Stats stats;
    stats.pushed = pushed_;
    stats.popped = popped_;
    stats.producer_waits = waits_;
    stats.producer_wait_ns = wait_ns_;
    stats.producer_wait_ns_max = wait_ns_max_;
    stats.segments_allocated = segments_allocated_;
    stats.depth_max = depth_max_;
    return stats;
}

std::size_t SegmentQueue::depth() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

}  // namespace gekko::native
