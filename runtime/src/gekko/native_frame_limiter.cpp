// The game's single-period wait point. Contract and rationale:
// include/gekko/native_frame_limiter.h and docs/FRAME_PACING.md.

#include "gekko/native_frame_limiter.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "gekko/cpu.h"
#include "gekko/profiling.h"
#include "gekko/recomp.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
constexpr u32 kLimiter = 0x801E1F2Cu;           // fn_801E1F2C, the whole function
// Continuations inside its extent (the original body is not in the image):
constexpr u32 kAfterMovieWait = 0x801E20ACu;    // the movie-mode refresh wait returned
constexpr u32 kWaitResume = 0x801E20C4u;        // the wait yielded to the guest and resumes
constexpr u32 kPublishResume = 0x801E2138u;     // the FPU fault before publishing returned
constexpr u32 kAfterSetter = 0x801E21D8u;       // 0x80203D20 returned

// The guest functions the limiter calls.
constexpr u32 kMovieRefreshWait = 0x804040ACu;  // -> 0x803C5730 -> VIWaitForRetrace
constexpr u32 kFrameStepSetter = 0x80203D20u;   // (obj + 0xC, n, f1 = dt)
constexpr u32 kFrameCounters = 0x80337DD4u;     // (obj + 0x88, byte r13+0x2D7C)

// The frame-rate object and the limiter's small-data words.
constexpr u32 kFrameObject = 0x8057E760u;
constexpr u32 kObjectInterval = 0x10u;
constexpr u32 kObjectBypass = 0x14u;
constexpr u32 kObjectCap = 0x34u;
constexpr u32 kObjectHz = 0x50u;
constexpr u32 kSdaDt = 0x4558u;
constexpr u32 kSdaRawDt = 0x455Cu;
constexpr u32 kSdaDtOverride = 0x4560u;
constexpr u32 kSdaFirst = 0x4575u;
constexpr u32 kSdaArmed = 0x4576u;
constexpr u32 kSdaLastHigh = 0x4578u;
constexpr u32 kSdaLastLow = 0x457Cu;
constexpr u32 kSdaMovie = 0x2C50u;
constexpr u32 kSdaCounterArgument = 0x2D7Cu;

// The limiter's guest stack frame (the original's own size), holding what the
// original kept in nonvolatile registers across its waits and calls.
constexpr u32 kFrameSize = 0x80u;
constexpr u32 kFrameTag = 0x4E464C4Du;   // 'NFLM'
constexpr u32 kTagOffset = 0x08u;
constexpr u32 kPeriodOffset = 0x0Cu;     // P (us)
constexpr u32 kLastGridOffset = 0x10u;   // u64, the frame index of "last" (frame_index)
constexpr u32 kMinDtOffset = 0x18u;      // single(P * 1e-6)
constexpr u32 kMaxDtOffset = 0x1Cu;      // single(min * cap)
constexpr u32 kNowOffset = 0x20u;        // u64, the latest sample
constexpr u32 kElapsedOffset = 0x28u;    // u64
constexpr u32 kWaitedOffset = 0x30u;     // this frame waited (stats); kZeroTimeWait: the event wait
constexpr u32 kZeroTimeWait = 2u;
// The zero-time event wait was cut by a stop before the clock moved (a savestate or development-link boundary): the
// resumed wait waits for the event again.
constexpr u32 kZeroTimeWaitStopped = 3u;
constexpr u32 kHzOffset = 0x38u;         // the frame rate the frame index counts in

const NativeFrameLimiter::Address kAddresses[] = {
    {kLimiter, "frame limiter (fn_801E1F2C)", false},
    {kAfterMovieWait, "frame limiter: movie refresh wait returned", true},
    {kWaitResume, "frame limiter: wait resumes", true},
    {kPublishResume, "frame limiter: publish resumes", true},
    {kAfterSetter, "frame limiter: frame-step setter returned", true},
};

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "frame limiter: " + why);
}

u64 read64(Cpu& c, u32 address) {
    return (static_cast<u64>(c.mem_read32(address)) << 32) | c.mem_read32(address + 4u);
}
void write64(Cpu& c, u32 address, u64 value) {
    c.mem_write32(address, static_cast<u32>(value >> 32));
    c.mem_write32(address + 4u, static_cast<u32>(value));
}

// The time-base divisor of 0x80051414: (bus clock / 4) / 125000 (324 for the
// 162 MHz bus the boot publishes).
u32 microsecond_divisor(Cpu& c) { return (c.mem_read32(0x800000F8u) >> 2) / 125000u; }

// OSGetTime -> 0x80051414: (time base * 8) / divisor, a signed 64-bit divide
// of a non-negative dividend.
u64 now_us(Cpu& c, u32 divisor) { return (c.time_base() << 3) / divisor; }

// One time base (docs/FRAME_PACING.md): frame k of an hz game begins at time base k * (bus / 4) / hz - 675,000
// ticks (exactly 1/60 s) apart at 60 Hz, 810,000 (1/50 s) at 50 Hz - the ticks the video retrace and the audio
// passes are placed on too. The game's microsecond clock reads floor(k * 1e6 / hz) there, so a sample's frame index
// is the last k whose boundary it has reached.
u64 frame_index(u64 us, u64 hz) { return ((us + 1u) * hz - 1u) / 1000000u; }

// The first time base of frame k: ceil(k * (bus / 4) / hz), (bus / 4) = divisor * 125000.
u64 frame_start(u64 k, u32 divisor, u64 hz) { return (k * divisor * 125000u + hz - 1u) / hz; }

// fctiwz with the FPSCR's NI default results: NaN and below range give
// 0x80000000, above range 0x7FFFFFFF.
s32 truncate_to_s32(float value) {
    if (value != value || value < -2147483648.0f) return static_cast<s32>(0x80000000u);
    if (value >= 2147483648.0f) return 0x7FFFFFFF;
    return static_cast<s32>(value);
}

// __cvt_ull_flt (0x803A2380): the u64 rounded to the nearest double, then
// to the nearest single (frsp) - the host's two conversions in that order.
float u64_to_float(u64 value) { return static_cast<float>(static_cast<double>(value)); }

bool frame_ok(Cpu& c, u32 frame) {
    return (frame & 7u) == 0u && c.guest_memory().mapped(GcAddr(frame), kTagOffset + 4u) &&
           c.mem_read32(frame + kTagOffset) == kFrameTag;
}

}  // namespace

const NativeFrameLimiter::Address* NativeFrameLimiter::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeFrameLimiter::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("frame limiter: ") + name);
    };
    own(kLimiter, "fn_801E1F2C", [this](Cpu& c) { enter(c); });
    own(kAfterMovieWait, "movie refresh wait returned", [this](Cpu& c) { after_movie_wait(c); });
    own(kWaitResume, "wait resumes", [this](Cpu& c) { wait(c); });
    own(kPublishResume, "publish resumes", [this](Cpu& c) { publish(c); });
    own(kAfterSetter, "frame-step setter returned", [this](Cpu& c) { after_setter(c); });
}

NativeFrameLimiter::NativeFrameLimiter(Cpu& cpu, Advance advance, AdvanceEvent advance_event)
    : cpu_(cpu), advance_(std::move(advance)), advance_event_(std::move(advance_event)) {
    if (const char* path = std::getenv("GEKKO_FRAME_WORK"); path != nullptr && path[0] != '\0') {
        work_log_path_ = path;
        work_log_.reserve(2u * 60u * 60u * 30u);
    }
}

NativeFrameLimiter::~NativeFrameLimiter() {
    if (work_log_path_.empty()) return;
    std::FILE* out = std::fopen(work_log_path_.c_str(), "wb");
    if (out == nullptr) return;
    std::fputs("frame,host_ns,game_ticks\n", out);
    for (std::size_t i = 0; i + 1u < work_log_.size(); i += 2u)
        std::fprintf(out, "%zu,%llu,%llu\n", i / 2u, static_cast<unsigned long long>(work_log_[i]),
                     static_cast<unsigned long long>(work_log_[i + 1u]));
    std::fclose(out);
}

void NativeFrameLimiter::enter(Cpu& c) {
    GEKKO_PROFILE_ZONE("frame/limiter");
    if (!work_log_path_.empty()) {
        work_log_.push_back(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count()));
        work_log_.push_back(c.game_clock().now());
    }
    const u32 sda = c.gpr[13];
    // The frame (stwu r1,-0x80(r1); the caller's LR in its own frame's slot).
    const u32 caller_stack = c.gpr[1];
    const u32 frame = caller_stack - kFrameSize;
    c.mem_write32(frame, caller_stack);
    c.mem_write32(caller_stack + 4u, c.lr);
    c.mem_write32(frame + kTagOffset, kFrameTag);
    c.gpr[1] = frame;

    if (static_cast<s8>(c.mem_read8(sda + kSdaArmed)) == 0) {
        c.mem_write8(sda + kSdaFirst, 1u);
        c.mem_write8(sda + kSdaArmed, 1u);
    }
    const s32 hz = static_cast<s32>(c.mem_read32(kFrameObject + kObjectHz));
    const s32 cap = static_cast<s32>(c.mem_read32(kFrameObject + kObjectCap));
    if (hz <= 0) {
        stop(c, "the frame object's rate is " + std::to_string(hz) + " Hz");
        return;
    }
    // Single precision throughout, as fdivs/fmuls round: P = 20000 at 50 Hz
    // only because 1e6f * 0.02f rounds to 20000 in single precision.
    const float hz_single = static_cast<float>(static_cast<double>(hz));
    const float period_single = 1.0e6f * (1.0f / hz_single);
    const s32 period = truncate_to_s32(period_single);
    if (period <= 0) {
        stop(c, "hz " + std::to_string(hz) + " gives a frame period of " + std::to_string(period) + " us");
        return;
    }
    const float min_dt = 1.0e-6f * static_cast<float>(static_cast<double>(period));
    const float max_dt = min_dt * static_cast<float>(static_cast<double>(cap));
    const u32 divisor = microsecond_divisor(c);
    if (divisor == 0u) {
        stop(c, "the bus clock at 0x800000F8 gives a zero microsecond divisor");
        return;
    }
    if (c.mem_read8(sda + kSdaFirst) != 0u) {
        write64(c, sda + kSdaLastHigh, now_us(c, divisor));
        c.mem_write8(sda + kSdaFirst, 0u);
    }
    const u64 period64 = static_cast<u64>(static_cast<s64>(period));
    const u64 rate = static_cast<u64>(hz);
    const u64 last_frame = frame_index(read64(c, sda + kSdaLastHigh), rate);
    const u64 now = now_us(c, divisor);
    c.mem_write32(frame + kPeriodOffset, static_cast<u32>(period));
    c.mem_write32(frame + kHzOffset, static_cast<u32>(hz));
    write64(c, frame + kLastGridOffset, last_frame);
    c.mem_write32(frame + kMinDtOffset, float_to_bits(min_dt));
    c.mem_write32(frame + kMaxDtOffset, float_to_bits(max_dt));
    write64(c, frame + kNowOffset, now);
    write64(c, frame + kElapsedOffset, (frame_index(now, rate) - last_frame) * period64);
    c.mem_write32(frame + kWaitedOffset, 0u);

    if (c.mem_read32(sda + kSdaMovie) != 0u) {
        // Movie mode: one display-refresh wait (CRI's vsync wait) first.
        ++stats_.movie_waits;
        c.lr = kAfterMovieWait;
        c.leave_to_dispatcher(kMovieRefreshWait, FallbackReason::Yield, kAfterMovieWait);
        return;
    }
    after_movie_wait(c);
}

void NativeFrameLimiter::after_movie_wait(Cpu& c) {
    const u32 frame = c.gpr[1];
    if (!frame_ok(c, frame)) {
        stop(c, "the movie refresh wait returned without the limiter's frame (r1 " + hex(frame) + ")");
        return;
    }
    // Bypass publishes the elapsed time sampled before the refresh wait.
    if (c.mem_read8(kFrameObject + kObjectBypass) != 0u) {
        ++stats_.bypassed;
        publish_now(c);
        return;
    }
    // The original tests the entry sample first, then resamples per lap.
    // The in-engine events ask for two periods per frame (EventManager's
    // constructor 0x80173EF4): the console's render-cost accommodation. Every
    // frame here lasts one period; the game's elapsed-time step follows.
    const u64 period = static_cast<u64>(static_cast<s64>(static_cast<s32>(c.mem_read32(frame + kPeriodOffset))));
    stats_.last_interval = c.mem_read32(kFrameObject + kObjectInterval);
    if (stats_.last_interval > 1u) ++stats_.paced_at_one_period;
    // Zero is the reset/disc-error handler's no-wait mode. Every nonzero
    // value means one period; there is no render-load or half-rate policy.
    if (stats_.last_interval == 0u || read64(c, frame + kElapsedOffset) >= period) {
        ++stats_.immediate;
        publish_now(c);
        return;
    }
    wait(c);
}

// Zero game time is the one thing a frame cannot take. The game loops frames
// while it waits for asynchronous work (the stg0100 load bypasses the limiter
// and waits for the CRI server the retrace wakes); on hardware every frame
// cost its CPU time, so retraces kept coming, but guest code takes no game
// time here, and a loop of zero-time frames would never let one happen. A
// frame that would publish at once therefore does so only if game time has
// passed since the previous frame (the game's own last-time word); otherwise
// it lasts until the next event - the earliest moment anything it could be
// waiting for can change - and publishes with the new sample.
void NativeFrameLimiter::publish_now(Cpu& c) {
    const u32 frame = c.gpr[1];
    if (read64(c, frame + kNowOffset) != read64(c, c.gpr[13] + kSdaLastHigh)) {
        publish(c);
        return;
    }
    ++stats_.zero_time;
    c.mem_write32(frame + kWaitedOffset, kZeroTimeWait);
    const u64 before = c.game_clock().now();
    const bool reached = advance_event_();
    stats_.waited_ticks += c.game_clock().now() - before;
    if (!reached) {
        // An interrupt the event made deliverable comes first, and the wait
        // continuation publishes when it returns. A stop comes before the clock
        // moves at all: then the continuation waits for the event again.
        if (c.stopped() || c.stop_requested()) c.mem_write32(frame + kWaitedOffset, kZeroTimeWaitStopped);
        ++stats_.yields;
        c.leave_to_dispatcher(kWaitResume, FallbackReason::Yield, kWaitResume);
        return;
    }
    resample(c, frame);
    publish(c);
}

// The rate the frame's indices count in (a limiter frame from before the slot existed: the object's).
u64 NativeFrameLimiter::frame_rate(Cpu& c, u32 frame) {
    const u32 hz = c.mem_read32(frame + kHzOffset);
    return hz != 0u ? hz : c.mem_read32(kFrameObject + kObjectHz);
}

void NativeFrameLimiter::resample(Cpu& c, u32 frame) {
    const u32 divisor = microsecond_divisor(c);
    const u64 period = static_cast<u64>(static_cast<s64>(static_cast<s32>(c.mem_read32(frame + kPeriodOffset))));
    const u64 now = now_us(c, divisor);
    write64(c, frame + kNowOffset, now);
    write64(c, frame + kElapsedOffset,
            (frame_index(now, frame_rate(c, frame)) - read64(c, frame + kLastGridOffset)) * period);
}

// Resample after each interrupt and advance to the start of the next frame
// (one period). A reset may cancel this wait by setting the interval word to zero.
void NativeFrameLimiter::wait(Cpu& c) {
    const u32 frame = c.gpr[1];
    if (!frame_ok(c, frame)) {
        stop(c, "the wait resumed without the limiter's frame (r1 " + hex(frame) + ")");
        return;
    }
    if (c.mem_read32(frame + kWaitedOffset) == kZeroTimeWaitStopped) {
        // The zero-time frame's event wait was cut by a stop before the event:
        // it waits for the event now, as it would have without the stop.
        c.mem_write32(frame + kWaitedOffset, kZeroTimeWait);
        const u64 before = c.game_clock().now();
        const bool reached = advance_event_();
        stats_.waited_ticks += c.game_clock().now() - before;
        if (!reached) {
            if (c.stopped() || c.stop_requested()) c.mem_write32(frame + kWaitedOffset, kZeroTimeWaitStopped);
            ++stats_.yields;
            c.leave_to_dispatcher(kWaitResume, FallbackReason::Yield, kWaitResume);
            return;
        }
        resample(c, frame);
        publish(c);
        return;
    }
    if (c.mem_read32(frame + kWaitedOffset) == kZeroTimeWait) {
        // A zero-time frame's event wait resumed after an interrupt: the event
        // happened, so the frame publishes with the new sample.
        resample(c, frame);
        publish(c);
        return;
    }
    const u32 divisor = microsecond_divisor(c);
    const u64 period = static_cast<u64>(static_cast<s64>(static_cast<s32>(c.mem_read32(frame + kPeriodOffset))));
    const u64 rate = frame_rate(c, frame);
    const u64 last_frame = read64(c, frame + kLastGridOffset);
    for (;;) {
        const u64 now = now_us(c, divisor);
        const u64 elapsed = (frame_index(now, rate) - last_frame) * period;
        write64(c, frame + kNowOffset, now);
        write64(c, frame + kElapsedOffset, elapsed);
        if (c.mem_read32(kFrameObject + kObjectInterval) == 0u || elapsed >= period) break;
        if (c.mem_read32(frame + kWaitedOffset) == 0u) {
            ++stats_.waits;
            c.mem_write32(frame + kWaitedOffset, 1u);
        }
        // The next frame begins on its time-base tick, where the retrace is.
        const u64 target_tb = frame_start(last_frame + 1u, divisor, rate);
        const u64 target_clock = target_tb - (c.time_base() - c.game_clock().now());
        const u64 before = c.game_clock().now();
        const bool reached = advance_(target_clock);
        stats_.waited_ticks += c.game_clock().now() - before;
        if (!reached) {
            // An interrupt (or a stop) has to be taken first: hand back at the
            // wait, which the interrupt's return resumes.
            ++stats_.yields;
            c.leave_to_dispatcher(kWaitResume, FallbackReason::Yield, kWaitResume);
            return;
        }
    }
    publish(c);
}

void NativeFrameLimiter::publish(Cpu& c) {
    const u32 frame = c.gpr[1];
    if (!frame_ok(c, frame)) {
        stop(c, "publish without the limiter's frame (r1 " + hex(frame) + ")");
        return;
    }
    // dt reaches the frame-step setter in f1: this thread must own the FPU,
    // which the original made sure of with its first FP instruction.
    if ((c.msr() & kMsrFP) == 0u) {
        recomp::fp_unavailable(c, kPublishResume);
        return;
    }
    const u32 sda = c.gpr[13];
    const u64 period = static_cast<u64>(static_cast<s64>(static_cast<s32>(c.mem_read32(frame + kPeriodOffset))));
    const u64 now = read64(c, frame + kNowOffset);
    const u64 elapsed = read64(c, frame + kElapsedOffset);
    const float min_dt = bits_to_float(c.mem_read32(frame + kMinDtOffset));
    const float max_dt = bits_to_float(c.mem_read32(frame + kMaxDtOffset));

    const float dt = 1.0e-6f * u64_to_float(elapsed);
    c.mem_write32(sda + kSdaDt, float_to_bits(dt));
    const u64 last = read64(c, sda + kSdaLastHigh);
    const float raw_dt = 1.0e-6f * u64_to_float(now - last);
    const float override_dt = bits_to_float(c.mem_read32(sda + kSdaDtOverride));
    write64(c, sda + kSdaLastHigh, now);
    c.mem_write32(sda + kSdaRawDt, float_to_bits(raw_dt));
    if (override_dt > 0.0f) {
        c.mem_write32(sda + kSdaDt, float_to_bits(override_dt));
        c.mem_write32(sda + kSdaRawDt, float_to_bits(override_dt));
    }
    const float stored = bits_to_float(c.mem_read32(sda + kSdaDt));
    if (stored > max_dt) {
        c.mem_write32(sda + kSdaDt, float_to_bits(max_dt));
    } else if (stored < min_dt) {
        c.mem_write32(sda + kSdaDt, float_to_bits(min_dt));
    }
    // 0x80203D20(obj + 0xC, n = low word of elapsed / P, f1 = dt as lfs loads it).
    const u64 steps = elapsed / period;
    const u64 dt_bits = single_to_double_bits(c.mem_read32(sda + kSdaDt));
    c.fpr[1].bits[0] = dt_bits;
    c.fpr[1].bits[1] = dt_bits;
    c.gpr[3] = kFrameObject + 0xCu;
    c.gpr[4] = static_cast<u32>(steps);
    c.lr = kAfterSetter;
    c.leave_to_dispatcher(kFrameStepSetter, FallbackReason::Yield, kAfterSetter);
}

void NativeFrameLimiter::after_setter(Cpu& c) {
    const u32 frame = c.gpr[1];
    if (!frame_ok(c, frame)) {
        stop(c, "the frame-step setter returned without the limiter's frame (r1 " + hex(frame) + ")");
        return;
    }
    ++stats_.frames;
    // 0x80337DD4(obj + 0x88, byte r13+0x2D7C) is the last thing the original
    // does before restoring nonvolatile registers it never changed, so it is
    // entered as a tail call: the frame is popped and it returns to our caller.
    const u32 caller_stack = c.mem_read32(frame);
    c.gpr[3] = kFrameObject + 0x88u;
    c.gpr[4] = c.mem_read8(c.gpr[13] + kSdaCounterArgument);
    c.mem_write32(frame + kTagOffset, 0u);
    c.gpr[1] = caller_stack;
    c.lr = c.mem_read32(caller_stack + 4u);
    c.leave_to_dispatcher(kFrameCounters, FallbackReason::Yield, kAfterSetter);
}

}  // namespace gekko
