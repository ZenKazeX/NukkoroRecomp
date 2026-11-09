#include "gekko/thread_sampler.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "gekko/thread_name.h"
#include "render/present_trace.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gekko {

namespace {

namespace trace = shadow::render::present_trace;

#if defined(_WIN32) && defined(_WIN64)
// NtQuerySystemInformation(SystemProcessInformation): one record per process, its threads after it. The layout is the
// x64 one every process viewer uses (the documented winternl.h type leaves these fields reserved): the record's next
// offset at 0, its thread count at 4, the process id at 0x50, the threads from 0x100, 0x50 bytes each with kernel time
// at 0, user time at 8, the thread id (CLIENT_ID.UniqueThread) at 0x30 and the context switch count at 0x40.
constexpr ULONG kSystemProcessInformation = 5;
constexpr std::size_t kProcessIdAt = 0x50, kThreadsAt = 0x100, kThreadSize = 0x50;
constexpr std::size_t kKernelTimeAt = 0x00, kUserTimeAt = 0x08, kThreadIdAt = 0x30, kContextSwitchesAt = 0x40;

using NtQuerySystemInformationFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

template <typename T>
T read_at(const std::uint8_t* base, std::size_t offset) {
    T value;
    std::memcpy(&value, base + offset, sizeof(T));
    return value;
}

struct Sampler {
    NtQuerySystemInformationFn query = nullptr;
    GetThreadDescriptionFn describe = nullptr;
    std::vector<std::uint8_t> buffer = std::vector<std::uint8_t>(1u << 20);
    std::unordered_map<DWORD, HANDLE> handles;  // thread id -> query handle (kept for the cycle counts)
    bool reported_layout = false;

    Sampler() {
        if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
            query = reinterpret_cast<NtQuerySystemInformationFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
        if (const HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
            describe = reinterpret_cast<GetThreadDescriptionFn>(
                reinterpret_cast<void*>(GetProcAddress(kernel, "GetThreadDescription")));
    }
    ~Sampler() {
        for (auto& h : handles)
            CloseHandle(h.second);
    }

    std::string name_of(HANDLE thread) const {
        std::string name;
        PWSTR wide = nullptr;
        if (describe != nullptr && SUCCEEDED(describe(thread, &wide)) && wide != nullptr) {
            for (const wchar_t* p = wide; *p != L'\0' && name.size() < 63; ++p)
                name.push_back(*p < 128 && *p >= 32 && *p != L',' ? static_cast<char>(*p) : '_');
            LocalFree(wide);
        }
        return name.empty() ? std::string("unnamed") : name;
    }

    void sample() {
        if (query == nullptr)
            return;
        ULONG needed = 0;
        LONG status = query(kSystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
        if (status == static_cast<LONG>(0xC0000004L) /* STATUS_INFO_LENGTH_MISMATCH */) {
            buffer.resize(static_cast<std::size_t>(needed) + (1u << 16));
            status = query(kSystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
        }
        if (status < 0)
            return;
        const auto self = static_cast<std::uintptr_t>(GetCurrentProcessId());
        const std::uint8_t* p = buffer.data();
        for (;;) {
            const auto next = read_at<ULONG>(p, 0);
            if (read_at<std::uintptr_t>(p, kProcessIdAt) == self) {
                const auto count = read_at<ULONG>(p, 4);
                bool found_self = false;
                for (ULONG i = 0; i < count; ++i) {
                    const std::uint8_t* t = p + kThreadsAt + i * kThreadSize;
                    const auto tid = static_cast<DWORD>(read_at<std::uintptr_t>(t, kThreadIdAt));
                    found_self = found_self || tid == GetCurrentThreadId();
                    const auto kernel = read_at<std::int64_t>(t, kKernelTimeAt);
                    const auto user = read_at<std::int64_t>(t, kUserTimeAt);
                    const auto switches = read_at<ULONG>(t, kContextSwitchesAt);
                    HANDLE& h = handles[tid];
                    if (h == nullptr) {
                        h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
                        if (h != nullptr)
                            trace::name_thread(tid, name_of(h));
                    }
                    ULONG64 cycles = 0;
                    if (h != nullptr && !QueryThreadCycleTime(h, &cycles))
                        cycles = 0;
                    trace::record(trace::Ev::ThreadCpu, tid, static_cast<std::uint64_t>(kernel + user), switches,
                                  cycles);
                }
                if (!found_self && !reported_layout) {
                    reported_layout = true;  // the layout assumption failed: say so, record nothing wrong silently
                    std::fprintf(stderr, "[thread-sampler] this thread is not in its own process's list: layout "
                                         "differs, per-thread rows are not trustworthy\n");
                }
                return;
            }
            if (next == 0)
                return;
            p += next;
        }
    }
};
#endif

}  // namespace

void ThreadSampler::start() {
#if defined(_WIN32) && defined(_WIN64)
    if (!trace::enabled() || thread_.joinable())
        return;
    stopping_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { run(); });
#endif
}

void ThreadSampler::stop() {
    if (!thread_.joinable())
        return;
    stopping_.store(true, std::memory_order_release);
    thread_.join();
}

void ThreadSampler::run() {
#if defined(_WIN32) && defined(_WIN64)
    name_this_thread("telemetry-sampler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    Sampler sampler;
    auto next = std::chrono::steady_clock::now();
    while (!stopping_.load(std::memory_order_acquire)) {
        sampler.sample();
        next += std::chrono::seconds(1);
        // Short sleeps so stop() does not wait a whole second; 10 wakes a second, nothing done in between.
        while (!stopping_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < next)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    sampler.sample();  // the last interval
#endif
}

}  // namespace gekko
