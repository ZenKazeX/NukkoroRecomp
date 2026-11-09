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

namespace {
// The player's controller preferences (Options > Controls / Gameplay, gekko/pc_settings.h), any thread.
std::atomic<unsigned> g_rumble_percent{100u};
std::atomic<bool> g_invert_camera_x{false}, g_invert_camera_y{false};
}  // namespace

void set_xinput_preferences(unsigned rumble_percent, bool invert_camera_x, bool invert_camera_y) noexcept {
    g_rumble_percent.store(rumble_percent > 100u ? 100u : rumble_percent, std::memory_order_relaxed);
    g_invert_camera_x.store(invert_camera_x, std::memory_order_relaxed);
    g_invert_camera_y.store(invert_camera_y, std::memory_order_relaxed);
}

std::int8_t invert_stick_axis(std::int8_t value) noexcept {
    // -(-128) does not fit an int8: saturate to +127 instead of wrapping back to -128.
    return value == -128 ? static_cast<std::int8_t>(127) : static_cast<std::int8_t>(-value);
}

shadow::peripheral::ControllerSample apply_camera_inversion(shadow::peripheral::ControllerSample sample, bool invert_x,
                                                            bool invert_y) noexcept {
    if (invert_x) sample.substick_x = invert_stick_axis(sample.substick_x);
    if (invert_y) sample.substick_y = invert_stick_axis(sample.substick_y);
    return sample;
}

unsigned short xinput_rumble_speed(bool rumble, unsigned percent) noexcept {
    if (!rumble) return 0u;
    return static_cast<unsigned short>(0xc000u * (percent > 100u ? 100u : percent) / 100u);
}

shadow::peripheral::ControllerSample poll_xinput_controller(unsigned user_index) {
    shadow::peripheral::GamepadState state;
    if (!poll_xinput_gamepad(user_index, state)) return shadow::peripheral::ControllerSample{};
    shadow::peripheral::ControllerSample sample = shadow::peripheral::sample_from_gamepad(state, g_binding);
    // Camera inversion: the camera stick as the player asked for it, before the sample is latched (an input tape
    // records what the game read, so a replay needs no setting). Off by default: the original stick.
    sample = apply_camera_inversion(sample, g_invert_camera_x.load(std::memory_order_relaxed),
                                    g_invert_camera_y.load(std::memory_order_relaxed));
    return sample;
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
        // 100 % (the default) is the port's original duty, 0xC000.
        const unsigned short speed = xinput_rumble_speed(true, g_rumble_percent.load(std::memory_order_relaxed));
        vibration.left_motor_speed = static_cast<decltype(vibration.left_motor_speed)>(speed);
        vibration.right_motor_speed = static_cast<decltype(vibration.right_motor_speed)>(speed);
    }
    set_state(user_index, &vibration);
#else
    (void)user_index;
    (void)rumble;
#endif
}

}  // namespace gekko
