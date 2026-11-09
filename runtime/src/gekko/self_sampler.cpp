// In-process game-thread stack sampler (diagnostic). Contract: include/gekko/self_sampler.h.

#include "gekko/self_sampler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) && defined(_M_X64)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace gekko {

#if defined(_WIN32) && defined(_M_X64)

namespace {
constexpr std::size_t kDepth = 24;
constexpr std::size_t kRecord = kDepth + 2;  // time, frame count, frames
}  // namespace

struct SelfSampler::Impl {
    std::string path;
    HANDLE target = nullptr;
    std::uint64_t interval_us = 100;
    std::vector<std::uint64_t> buffer;  // kRecord words per sample
    std::size_t samples = 0, capacity = 0;
    std::uint64_t failures = 0;
    std::atomic<bool> stop{false};
    std::thread thread;

    static std::uint64_t now_ns() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }

    void sample_once() {
        if (samples >= capacity)
            return;
        std::uint64_t* record = &buffer[samples * kRecord];
        if (SuspendThread(target) == static_cast<DWORD>(-1)) {
            ++failures;
            return;
        }
        record[0] = now_ns();
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        std::size_t depth = 0;
        if (GetThreadContext(target, &context)) {
            for (; depth < kDepth && context.Rip != 0; ++depth) {
                record[2 + depth] = context.Rip;
                DWORD64 image_base = 0;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
                if (function == nullptr) {
                    // A leaf: the return address is on top of the stack.
                    if (context.Rsp == 0)
                        break;
                    context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                    continue;
                }
                void* handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                                 &establisher, nullptr);
            }
        } else {
            ++failures;
        }
        ResumeThread(target);
        record[1] = depth;
        ++samples;
    }

    void run() {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        while (!stop.load(std::memory_order_relaxed) && samples < capacity) {
            if (timer != nullptr) {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(interval_us * 10u);  // 100 ns units, relative
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(interval_us));
            }
            sample_once();
        }
        if (timer != nullptr)
            CloseHandle(timer);
    }

    void write() {
        std::FILE* out = std::fopen(path.c_str(), "wb");
        if (out == nullptr)
            return;
        const auto base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        std::fprintf(out, "# gekko self sample 1, interval %llu us, image base %llX, failures %llu\n",
                     static_cast<unsigned long long>(interval_us), static_cast<unsigned long long>(base),
                     static_cast<unsigned long long>(failures));
        for (std::size_t s = 0; s < samples; ++s) {
            const std::uint64_t* record = &buffer[s * kRecord];
            std::fprintf(out, "%llu", static_cast<unsigned long long>(record[0]));
            for (std::size_t d = 0; d < record[1]; ++d) {
                const std::uint64_t pc = record[2 + d];
                if (pc >= base && pc - base < 0x80000000ull)
                    std::fprintf(out, ",%llX", static_cast<unsigned long long>(pc - base));
                else
                    std::fprintf(out, ",@%llX", static_cast<unsigned long long>(pc));
            }
            std::fputc('\n', out);
        }
        std::fclose(out);
    }
};

SelfSampler::SelfSampler() {
    const char* path = std::getenv("GEKKO_SELF_SAMPLE");
    if (path == nullptr || path[0] == 0)
        return;
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    if (const char* us = std::getenv("GEKKO_SELF_SAMPLE_US"); us != nullptr && us[0] != 0)
        impl->interval_us = std::max<std::uint64_t>(20u, std::strtoull(us, nullptr, 10));
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &impl->target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    impl->capacity = 600000;  // 60 s at 100 us
    impl->buffer.assign(impl->capacity * kRecord, 0u);
    Impl* raw = impl.get();
    impl->thread = std::thread([raw] { raw->run(); });
    impl_ = std::move(impl);
}

SelfSampler::~SelfSampler() {
    if (!impl_)
        return;
    impl_->stop.store(true, std::memory_order_relaxed);
    if (impl_->thread.joinable())
        impl_->thread.join();
    impl_->write();
    CloseHandle(impl_->target);
}

#else

struct SelfSampler::Impl {};
SelfSampler::SelfSampler() = default;
SelfSampler::~SelfSampler() = default;

#endif

}  // namespace gekko
