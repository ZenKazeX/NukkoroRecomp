#pragma once
// F1c test-only host probes (Windows). Kept out of the driver translation units so <windows.h>
// never meets the gekko headers. Nothing here is linked into any shipped target.
#include <cstddef>
#include <cstdint>
#include <string>

namespace f1c {

// NT_TIB of the calling thread: StackBase (highest address, exclusive) and StackLimit (lowest
// committed address; it only moves down, as the guard page is touched).
std::uintptr_t stack_base() noexcept;
std::uintptr_t stack_limit() noexcept;

// Runs fn(arg) on a new thread whose stack RESERVE is `reserve` bytes (committed on demand), joins
// it, and returns the thread's exit code (0 when fn returned; the exception code when it died of an
// unhandled SEH exception such as STATUS_STACK_OVERFLOW 0xC00000FD, which is caught and reported).
unsigned long run_on_thread(std::size_t reserve, void (*fn)(void*), void* arg);

// VirtualQuery facts about the page immediately below the current StackLimit: whether it is still
// the PAGE_GUARD page the OS keeps under the committed stack.
bool guard_page_below_limit() noexcept;

double now_ms() noexcept;

// Return addresses of the calling thread's stack (RtlCaptureStackBackTrace, .pdata unwinding), as RVAs of
// the main executable where they fall inside it (others are reported as 0). Returns the frame count.
std::size_t capture_backtrace_rvas(std::uintptr_t* rvas, std::size_t max_frames) noexcept;

}  // namespace f1c
