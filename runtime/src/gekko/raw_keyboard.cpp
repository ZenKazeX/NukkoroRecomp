#include "gekko/raw_keyboard.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace gekko {
namespace {

constexpr wchar_t kClassName[] = L"ShadowRawKeyboard";

LRESULT CALLBACK raw_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

RawKeyboard::~RawKeyboard() { stop(); }

bool RawKeyboard::start() {
    if (window_ != nullptr)
        return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = raw_window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // fails harmlessly when already registered (a second start)
    HWND window = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (window == nullptr)
        return false;
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;  // generic desktop
    device.usUsage = 0x06;      // keyboard
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = window;
    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
        DestroyWindow(window);
        return false;
    }
    window_ = window;
    held_.fill(0);
    return true;
}

void RawKeyboard::stop() {
    if (window_ == nullptr)
        return;
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = RIDEV_REMOVE;
    device.hwndTarget = nullptr;
    RegisterRawInputDevices(&device, 1, sizeof(device));
    DestroyWindow(static_cast<HWND>(window_));
    window_ = nullptr;
    held_.fill(0);
}

bool RawKeyboard::wait_and_pump(void* extra, std::uint32_t timeout_ms) {
    if (window_ == nullptr)
        return false;
    HANDLE handles[1] = {static_cast<HANDLE>(extra)};
    const DWORD count = extra != nullptr ? 1u : 0u;
    // MWMO_INPUTAVAILABLE: also wake for input that arrived before this call and is still queued.
    MsgWaitForMultipleObjectsEx(count, count != 0 ? handles : nullptr, timeout_ms, QS_RAWINPUT, MWMO_INPUTAVAILABLE);
    return pump();
}

void RawKeyboard::resync() noexcept {
    for (int key = 1; key < 0xFF; ++key) {
        if (held_[key] != 0u && (GetAsyncKeyState(key) & 0x8000) == 0) held_[key] = 0u;
    }
}

bool RawKeyboard::pump() {
    bool changed = false;
    MSG message;
    // Only this thread's messages for the raw window; anything else on this thread is dispatched as usual.
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_INPUT) {
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER));
            if (size >= sizeof(RAWINPUTHEADER) && size <= 1024) {
                alignas(8) std::uint8_t buffer[1024];
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT, buffer, &size,
                                    sizeof(RAWINPUTHEADER)) == size) {
                    const RAWINPUT* input = reinterpret_cast<const RAWINPUT*>(buffer);
                    if (input->header.dwType == RIM_TYPEKEYBOARD) {
                        const RAWKEYBOARD& kb = input->data.keyboard;
                        // 0xFF: a fake key, part of an escaped sequence (Pause, Print Screen); no virtual key.
                        // E0 2A / E0 36: the fake Shifts the keyboard wraps around a grey navigation key (NumLock
                        // on, or Shift held) - reported here as VK_SHIFT, never seen as WM_KEYDOWN by the window path.
                        // Counting them made Enter + an arrow read as Shift+Enter (Start) for a moment.
                        const bool fake_shift =
                            (kb.MakeCode == 0x2A || kb.MakeCode == 0x36) && (kb.Flags & RI_KEY_E0) != 0;
                        if (kb.VKey != 0 && kb.VKey < 0xFF && !fake_shift) {
                            const std::uint8_t down = (kb.Flags & RI_KEY_BREAK) == 0 ? 1u : 0u;
                            std::uint8_t& level = held_[kb.VKey & 0xFFu];
                            if (level != down) {
                                level = down;
                                changed = true;
                            }
                            ++events_;
                        }
                    }
                }
            }
        }
        DispatchMessageW(&message);  // WM_INPUT must reach DefWindowProc for the system to free the input
    }
    return changed;
}

}  // namespace gekko

#else

namespace gekko {
RawKeyboard::~RawKeyboard() = default;
bool RawKeyboard::start() { return false; }
void RawKeyboard::stop() {}
bool RawKeyboard::wait_and_pump(void*, std::uint32_t) { return false; }
bool RawKeyboard::pump() { return false; }
void RawKeyboard::resync() noexcept {}
}  // namespace gekko

#endif
