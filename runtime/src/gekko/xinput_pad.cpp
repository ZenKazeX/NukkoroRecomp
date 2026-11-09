#include "gekko/xinput_pad.h"

#include <array>
#include <atomic>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace gekko {
namespace {

#ifdef _WIN32

// XINPUT_GAMEPAD and XINPUT_STATE / XINPUT_VIBRATION, without xinput.h (the DLL is loaded at run time so a
// machine without it still runs on the keyboard).
struct NativeXInputGamepad {
    unsigned short buttons;
    unsigned char left_trigger;
    unsigned char right_trigger;
    short thumb_lx;
    short thumb_ly;
    short thumb_rx;
    short thumb_ry;
};
struct NativeXInputState {
    unsigned long packet;
    NativeXInputGamepad gamepad;
};
struct NativeXInputVibration {
    unsigned short left_motor_speed;
    unsigned short right_motor_speed;
};
static_assert(sizeof(NativeXInputGamepad) == 12u, "XInput gamepad layout must match Windows");
static_assert(sizeof(NativeXInputState) == 16u, "XInput state layout must match Windows");

using XInputGetStateFn = unsigned long(__stdcall*)(unsigned long, NativeXInputState*);
using XInputSetStateFn = unsigned long(__stdcall*)(unsigned long, NativeXInputVibration*);

template <typename Fn>
Fn resolve_xinput(const char* name) {
    for (const wchar_t* library : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        if (HMODULE module = LoadLibraryW(library)) {
            if (FARPROC address = GetProcAddress(module, name)) {
                return reinterpret_cast<Fn>(address);
            }
            FreeLibrary(module);
        }
    }
    return nullptr;
}

XInputGetStateFn xinput_get_state() {
    static const XInputGetStateFn function = resolve_xinput<XInputGetStateFn>("XInputGetState");
    return function;
}

XInputSetStateFn xinput_set_state() {
    static const XInputSetStateFn function = resolve_xinput<XInputSetStateFn>("XInputSetState");
    return function;
}

#endif

shadow::peripheral::GamepadBinding g_binding{};

}  // namespace

void set_gamepad_binding(const shadow::peripheral::GamepadBinding& binding) { g_binding = binding; }
const shadow::peripheral::GamepadBinding& gamepad_binding() noexcept { return g_binding; }

bool poll_xinput_gamepad(unsigned user_index, shadow::peripheral::GamepadState& state) {
#ifdef _WIN32
    const XInputGetStateFn get_state = xinput_get_state();
    if (get_state == nullptr) {
        return false;
    }
    // XInputGetState against an absent user index is the documented slow path - it re-enumerates rather
    // than answering from a cached table - and this runs on the thread whose loop period sets the delivered
    // frame rate, for all four ports, on every iteration. Remember which ports answered
    // ERROR_DEVICE_NOT_CONNECTED and re-probe each about once a second, staggered, so a controller plugged in
    // mid-session is still picked up within a second.
    static std::array<std::atomic<long long>, 4> dead_until_ms{};
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const bool tracked = user_index < dead_until_ms.size();
    if (tracked && now_ms < dead_until_ms[user_index].load(std::memory_order_relaxed)) {
        return false;
    }
    NativeXInputState input{};
    if (get_state(user_index, &input) != 0u) {
        if (tracked) {
            dead_until_ms[user_index].store(now_ms + 1000 + 250 * static_cast<long long>(user_index),
                                            std::memory_order_relaxed);
        }
        return false;
    }
    if (tracked) {
        dead_until_ms[user_index].store(0, std::memory_order_relaxed);
    }
    state.buttons = input.gamepad.buttons;
    state.left_trigger = input.gamepad.left_trigger;
    state.right_trigger = input.gamepad.right_trigger;
    state.left_x = input.gamepad.thumb_lx;
    state.left_y = input.gamepad.thumb_ly;
    state.right_x = input.gamepad.thumb_rx;
    state.right_y = input.gamepad.thumb_ry;
    return true;
#else
    (void)user_index;
    (void)state;
    return false;
#endif
}

shadow::peripheral::ControllerSample poll_xinput_controller(unsigned user_index) {
    shadow::peripheral::GamepadState state;
    if (!poll_xinput_gamepad(user_index, state)) return shadow::peripheral::ControllerSample{};
    return shadow::peripheral::sample_from_gamepad(state, g_binding);
}

void set_xinput_rumble(unsigned user_index, bool rumble) {
#ifdef _WIN32
    const XInputSetStateFn set_state = xinput_set_state();
    if (set_state == nullptr) {
        return;
    }
    // One on / off request per port; a strong but not maximal duty on both motors.
    NativeXInputVibration vibration{};
    if (rumble) {
        vibration.left_motor_speed = 0xc000u;
        vibration.right_motor_speed = 0xc000u;
    }
    set_state(user_index, &vibration);
#else
    (void)user_index;
    (void)rumble;
#endif
}

}  // namespace gekko
