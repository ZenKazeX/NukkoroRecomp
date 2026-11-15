#include "gekko/raw_input.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "peripheral/action_map.h"

namespace gekko {
namespace {

constexpr wchar_t kClassName[] = L"ShadowRawInput";

LRESULT CALLBACK raw_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// The left or right code of a generic modifier in this event; 0 when it is not a modifier.
int sided_modifier(const RAWKEYBOARD& kb) noexcept {
    const bool extended = (kb.Flags & RI_KEY_E0) != 0;
    switch (kb.VKey) {
        case VK_SHIFT: return kb.MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
        case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
        case VK_MENU: return extended ? VK_RMENU : VK_LMENU;
        default: return 0;
    }
}

struct ButtonFlags {
    USHORT down, up;
    int vk;
    shadow::peripheral::MouseButton button;
};
constexpr ButtonFlags kButtons[] = {
    {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, VK_LBUTTON, shadow::peripheral::kMouseLeft},
    {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, VK_RBUTTON, shadow::peripheral::kMouseRight},
    {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, VK_MBUTTON, shadow::peripheral::kMouseMiddle},
    {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, VK_XBUTTON1, shadow::peripheral::kMouseX1},
    {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, VK_XBUTTON2, shadow::peripheral::kMouseX2},
};

}  // namespace

RawInput::~RawInput() { stop(); }

bool RawInput::start() {
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
    RAWINPUTDEVICE devices[2]{};
    devices[0].usUsagePage = 0x01;  // generic desktop
    devices[0].usUsage = 0x06;      // keyboard
    devices[0].dwFlags = RIDEV_INPUTSINK;
    devices[0].hwndTarget = window;
    devices[1] = devices[0];
    devices[1].usUsage = 0x02;  // mouse
    if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) {
        DestroyWindow(window);
        return false;
    }
    window_ = window;
    held_.fill(0);
    mouse_buttons_ = 0;
    return true;
}

void RawInput::stop() {
    if (window_ == nullptr)
        return;
    RAWINPUTDEVICE devices[2]{};
    devices[0].usUsagePage = 0x01;
    devices[0].usUsage = 0x06;
    devices[0].dwFlags = RIDEV_REMOVE;
    devices[0].hwndTarget = nullptr;
    devices[1] = devices[0];
    devices[1].usUsage = 0x02;
    RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
    DestroyWindow(static_cast<HWND>(window_));
    window_ = nullptr;
    held_.fill(0);
    mouse_buttons_ = 0;
}

bool RawInput::wait_and_pump(void* extra, std::uint32_t timeout_ms) {
    if (window_ == nullptr)
        return false;
    HANDLE handles[1] = {static_cast<HANDLE>(extra)};
    const DWORD count = extra != nullptr ? 1u : 0u;
    // MWMO_INPUTAVAILABLE: also wake for input that arrived before this call and is still queued.
    MsgWaitForMultipleObjectsEx(count, count != 0 ? handles : nullptr, timeout_ms, QS_RAWINPUT, MWMO_INPUTAVAILABLE);
    return pump();
}

void RawInput::resync() noexcept {
    for (int key = 1; key < 0xFF; ++key) {
        if (held_[key] != 0u && (GetAsyncKeyState(key) & 0x8000) == 0) held_[key] = 0u;
    }
    for (const ButtonFlags& row : kButtons) {
        if ((GetAsyncKeyState(row.vk) & 0x8000) == 0) mouse_buttons_ &= static_cast<std::uint8_t>(~(1u << row.button));
    }
}

bool RawInput::pump() {
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
                            if (const int sided = sided_modifier(kb); sided != 0) held_[sided & 0xFF] = down;
                            ++events_;
                        }
                    } else if (input->header.dwType == RIM_TYPEMOUSE) {
                        const RAWMOUSE& mouse = input->data.mouse;
                        if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && accept_mouse_.load(std::memory_order_relaxed)) {
                            mouse_x_.fetch_add(mouse.lLastX, std::memory_order_relaxed);
                            mouse_y_.fetch_add(mouse.lLastY, std::memory_order_relaxed);
                        }
                        for (const ButtonFlags& row : kButtons) {
                            const auto bit = static_cast<std::uint8_t>(1u << row.button);
                            if ((mouse.usButtonFlags & row.down) != 0 && (mouse_buttons_ & bit) == 0) {
                                mouse_buttons_ |= bit;
                                changed = true;
                            } else if ((mouse.usButtonFlags & row.up) != 0 && (mouse_buttons_ & bit) != 0) {
                                mouse_buttons_ &= static_cast<std::uint8_t>(~bit);
                                changed = true;
                            }
                        }
                        ++events_;
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
RawInput::~RawInput() = default;
bool RawInput::start() { return false; }
void RawInput::stop() {}
bool RawInput::wait_and_pump(void*, std::uint32_t) { return false; }
bool RawInput::pump() { return false; }
void RawInput::resync() noexcept {}
}  // namespace gekko

#endif
