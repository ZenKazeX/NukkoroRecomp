// F1c test-only host probes (Windows). See f1c_host.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "f1c_host.h"

#include <chrono>

namespace f1c {

std::uintptr_t stack_base() noexcept {
    const NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    return reinterpret_cast<std::uintptr_t>(tib->StackBase);
}

std::uintptr_t stack_limit() noexcept {
    const NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    return reinterpret_cast<std::uintptr_t>(tib->StackLimit);
}

bool guard_page_below_limit() noexcept {
    MEMORY_BASIC_INFORMATION info{};
    const std::uintptr_t probe = stack_limit() - 1u;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(probe), &info, sizeof(info)) == 0) return false;
    return info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) != 0;
}

namespace {
struct ThreadJob {
    void (*fn)(void*);
    void* arg;
    DWORD code;
};

DWORD filter(DWORD code, ThreadJob* job) {
    job->code = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

void invoke(ThreadJob* job) {
    __try {
        job->fn(job->arg);
    } __except (filter(GetExceptionCode(), job)) {
    }
}

DWORD WINAPI thread_main(LPVOID parameter) {
    auto* job = static_cast<ThreadJob*>(parameter);
    job->code = 0;
    invoke(job);
    return job->code;
}
}  // namespace

unsigned long run_on_thread(std::size_t reserve, void (*fn)(void*), void* arg) {
    ThreadJob job{fn, arg, 0};
    HANDLE thread = CreateThread(nullptr, reserve, &thread_main, &job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr) return 0xFFFFFFFFul;
    WaitForSingleObject(thread, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    return code;
}

std::size_t capture_backtrace_rvas(std::uintptr_t* rvas, std::size_t max_frames) noexcept {
    void* frames[62];
    const std::size_t wanted = max_frames < 62u ? max_frames : 62u;
    const USHORT got = RtlCaptureStackBackTrace(1, static_cast<DWORD>(wanted), frames, nullptr);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<std::uintptr_t>(dos->e_lfanew));
    const std::uintptr_t size = nt->OptionalHeader.SizeOfImage;
    for (USHORT i = 0; i < got; ++i) {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
        rvas[i] = address >= base && address < base + size ? address - base : 0u;
    }
    return got;
}

double now_ms() noexcept {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

}  // namespace f1c
