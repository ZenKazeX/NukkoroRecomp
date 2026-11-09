#include "peripheral/keyboard_layout.h"

#include <algorithm>

namespace shadow::peripheral {

namespace {

// Windows virtual-key codes (stable values; WinUser.h).
constexpr int kKeyEnter = 0x0D;
constexpr int kKeyShift = 0x10;
constexpr int kKeySpace = 0x20;
constexpr int kKeyLeft = 0x25;
constexpr int kKeyUp = 0x26;
constexpr int kKeyRight = 0x27;
constexpr int kKeyDown = 0x28;

}  // namespace

ControllerSample sample_from_keyboard(KeyHeld key_held, void* context) noexcept {
    const auto down = [&](int key) { return key_held != nullptr && key_held(key, context); };
    ControllerSample sample;
    sample.connected = true;
    std::uint32_t controls = 0;
    if (down('X') || (down(kKeyEnter) && !down(kKeyShift))) controls |= kControlA;
    if (down('Z')) controls |= kControlB;
    if (down('S')) controls |= kControlX;
    if (down('A')) controls |= kControlY;
    if (down(kKeySpace) || (down(kKeyEnter) && down(kKeyShift))) controls |= kControlStart;
    if (down('Q')) controls |= kControlL;
    if (down('E')) controls |= kControlR;
    if (down('C')) controls |= kControlZ;
    if (down(kKeyLeft)) controls |= kControlLeft;
    if (down(kKeyRight)) controls |= kControlRight;
    if (down(kKeyUp)) controls |= kControlUp;
    if (down(kKeyDown)) controls |= kControlDown;
    sample.controls = controls;
    int x = 0;
    int y = 0;
    if (down('J') || down(kKeyLeft)) x -= kStickFull;
    if (down('L') || down(kKeyRight)) x += kStickFull;
    if (down('I') || down(kKeyUp)) y += kStickFull;
    if (down('K') || down(kKeyDown)) y -= kStickFull;
    sample.stick_x = static_cast<std::int8_t>(std::clamp(x, -kStickFull, kStickFull));
    sample.stick_y = static_cast<std::int8_t>(std::clamp(y, -kStickFull, kStickFull));
    return sample;
}

ControllerSample combine(const ControllerSample& keyboard, const ControllerSample& gamepad) noexcept {
    if (!gamepad.connected) return keyboard;
    ControllerSample sample = keyboard;
    sample.connected = true;
    sample.controls |= gamepad.controls;
    if (gamepad.stick_x != 0) sample.stick_x = gamepad.stick_x;
    if (gamepad.stick_y != 0) sample.stick_y = gamepad.stick_y;
    sample.substick_x = gamepad.substick_x;
    sample.substick_y = gamepad.substick_y;
    return sample;
}

}  // namespace shadow::peripheral
