#pragma once

// The keyboard and mouse for the input thread (GEKKO_INPUT_THREAD; docs/PERF_60FPS_2026-11-04.md, input latency).
//
// Before: the keyboard level came from the game window's key messages, which reach the program only when the UI
// thread pumps its window - once per displayed frame, after the present wait - so a key press waited up to ~16.7 ms
// before the game could latch it (a gamepad press, polled by the input thread, waits ~0.5 ms). Here the input thread
// owns a message-only window registered for raw keyboard and mouse input (RIDEV_INPUTSINK: delivered whichever window
// has focus) and keeps its own level of every virtual key and mouse button from WM_INPUT, the moment it changes.
//
// Keyboard semantics are the window path's (host_window.cpp, ES-W3): virtual-key codes as WM_KEYDOWN / WM_KEYUP carry
// them, and the caller counts the keyboard only while the game window is in the foreground. Because the sink also sees
// releases made while another window had focus, no key stays held after a focus change. A modifier also sets its left
// or right code (VK_LSHIFT ...), so bindings can tell them apart.
//
// The mouse is relative counts, as the device reports them: no pointer ballistics, no screen position, no display
// size. mouse_total_x / _y are running totals of the counts received while the mouse is accepted (accept_mouse): any
// thread may read them without a lock, and differences of two reads are what moved in between. Motion while the mouse
// is not accepted is dropped, so nothing is owed when focus returns.
//
// Windows only; elsewhere start() fails and the caller keeps the window path.

#include <array>
#include <atomic>
#include <cstdint>

namespace gekko {

class RawInput {
public:
    RawInput() = default;
    RawInput(const RawInput&) = delete;
    RawInput& operator=(const RawInput&) = delete;
    ~RawInput();

    // On the thread that will pump it: the message-only window and the registration. False: no raw input.
    bool start();
    void stop();
    bool running() const noexcept { return window_ != nullptr; }

    // Waits up to `timeout_ms` for raw input (or `extra` - a waitable handle, may be null - to be signalled), then
    // takes every pending event. Returns true when any key's or button's level changed.
    bool wait_and_pump(void* extra, std::uint32_t timeout_ms);

    // Clears every key and button the system no longer reports down (GetAsyncKeyState). A release made while this sink
    // saw nothing - the secure desktop (Win+L, UAC), an elevated window in front (UIPI) - would otherwise leave it held
    // when the game comes back to the front. Only clears; a key still down is reported again by autorepeat.
    void resync() noexcept;
    bool held(int virtual_key) const noexcept { return held_[static_cast<unsigned>(virtual_key) & 0xFFu] != 0; }
    std::uint8_t mouse_buttons() const noexcept { return mouse_buttons_; }
    std::uint64_t events() const noexcept { return events_; }

    // Whether mouse motion counts (the window is in front and the camera has the mouse). Any thread.
    void accept_mouse(bool accept) noexcept { accept_mouse_.store(accept, std::memory_order_relaxed); }
    std::int64_t mouse_total_x() const noexcept { return mouse_x_.load(std::memory_order_relaxed); }
    std::int64_t mouse_total_y() const noexcept { return mouse_y_.load(std::memory_order_relaxed); }

private:
    bool pump();

    void* window_ = nullptr;
    std::array<std::uint8_t, 256> held_{};
    std::uint8_t mouse_buttons_ = 0;  // bit = shadow::peripheral::MouseButton
    std::uint64_t events_ = 0;
    std::atomic<bool> accept_mouse_{false};
    std::atomic<std::int64_t> mouse_x_{0}, mouse_y_{0};
};

}  // namespace gekko
