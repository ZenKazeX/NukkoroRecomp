#pragma once

// In-process stack sampler for the game thread (diagnostic; docs/GPU_FILL.md "stage start").
//
// GEKKO_SELF_SAMPLE=<path> (GEKKO_SELF_SAMPLE_US=<interval>, default 100) starts a sampler thread when the game thread
// creates a SelfSampler: every interval it suspends the game thread, unwinds its stack with the image's own unwind
// data (RtlLookupFunctionEntry / RtlVirtualUnwind, up to kDepth frames into a buffer allocated up front - nothing is
// allocated while the thread is suspended), stamps it with steady_clock (the clock GEKKO_FRAME_WORK and the GPU/present
// traces use) and resumes it. The file is written when the SelfSampler is destroyed: one line per sample, the time
// and the frames as image RVAs (absolute addresses outside the image are written with a leading '@').
// tools/self_sample_report.py names the frames from the link map and attributes samples to game frames.
// Without the variable nothing is created. Windows x64 only; elsewhere a no-op.

#include <memory>

namespace gekko {

class SelfSampler {
public:
    // Samples the calling thread, when GEKKO_SELF_SAMPLE is set; otherwise an inert object.
    SelfSampler();
    ~SelfSampler();
    SelfSampler(const SelfSampler&) = delete;
    SelfSampler& operator=(const SelfSampler&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gekko
