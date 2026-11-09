#pragma once

// The keyboard for the input thread (GEKKO_INPUT_THREAD; docs/PERF_60FPS_2026-11-04.md, input latency).
//
// Before: the keyboard level came from the game window's key messages, which reach the program only when the UI
// thread pumps its window - once per displayed frame, after the present wait - so a key press waited up to ~16.7 ms
// before the game could latch it (a gamepad press, polled by the input thread, waits ~0.5 ms). Here the input thread
// owns a message-only window registered for raw keyboard input (RIDEV_INPUTSINK: delivered whichever window has
// focus) and keeps its own level of every virtual key from WM_INPUT, the moment the key changes.
//
// Same semantics as the window path (host_window.cpp, ES-W3): virtual-key codes as WM_KEYDOWN / WM_KEYUP carry them,
// and the caller counts the keyboard only while the game window is in the foreground. Because the sink also sees
// releases made while another window had focus, no key stays held after a focus change (the window path releases
// every key on WM_KILLFOCUS / WM_ACTIVATEAPP for the same reason).
//
// Windows only; elsewhere start() fails and the caller keeps the window path.

#include <array>
#include <cstdint>

namespace gekko {

class RawKeyboard {
public:
    RawKeyboard() = default;
    RawKeyboard(const RawKeyboard&) = delete;
    RawKeyboard& operator=(const RawKeyboard&) = delete;
    ~RawKeyboard();

    // On the thread that will pump it: the message-only window and the registration. False: no raw keyboard.
    bool start();
    void stop();
    bool running() const noexcept { return window_ != nullptr; }

    // Waits up to `timeout_ms` for raw input (or `extra` - a waitable handle, may be null - to be signalled), then
    // takes every pending keyboard event. Returns true when any key's level changed.
    bool wait_and_pump(void* extra, std::uint32_t timeout_ms);

    // Clears every key the system no longer reports down (GetAsyncKeyState). A release made while this sink saw
    // nothing - the secure desktop (Win+L, UAC), an elevated window in front (UIPI) - would otherwise leave the key
    // held when the game comes back to the front. Only clears; a key still down is reported again by autorepeat.
    void resync() noexcept;
    bool held(int virtual_key) const noexcept { return held_[static_cast<unsigned>(virtual_key) & 0xFFu] != 0; }
    std::uint64_t events() const noexcept { return events_; }

private:
    bool pump();

    void* window_ = nullptr;
    std::array<std::uint8_t, 256> held_{};
    std::uint64_t events_ = 0;
};

}  // namespace gekko
