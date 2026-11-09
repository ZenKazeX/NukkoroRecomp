#include "peripheral/manager_imp_pc.h"

#include <utility>

namespace shadow::peripheral {

namespace {

// A stick axis as the engine reads it: stick units, a negative deflection one unit closer to centre.
std::int16_t engine_axis(std::int8_t value) noexcept {
    return static_cast<std::int16_t>(value < 0 ? value + 1 : value);
}

constexpr std::uint8_t kSaveFormat = 1;

void put_u8(std::vector<std::uint8_t>& out, std::uint8_t value) { out.push_back(value); }

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool get_u8(const std::uint8_t*& cursor, const std::uint8_t* end, std::uint8_t& value) {
    if (end - cursor < 1) return false;
    value = *cursor++;
    return true;
}

bool get_u32(const std::uint8_t*& cursor, const std::uint8_t* end, std::uint32_t& value) {
    if (end - cursor < 4) return false;
    value = 0;
    for (int shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(*cursor++) << shift;
    return true;
}

}  // namespace

ManagerImpPc::ManagerImpPc() noexcept { reset(); }

void ManagerImpPc::reset() noexcept {
    samples_ = {};
    samples_[0].connected = true;
    for (std::size_t port = 0; port < kPorts; ++port) set_rumble(port, false);
    stats_ = Stats{};
}

void ManagerImpPc::latch(std::size_t port, const ControllerSample& sample) noexcept {
    if (port < kPorts) samples_[port] = sample;
}

const ControllerSample& ManagerImpPc::sample(std::size_t port) const noexcept {
    static const ControllerSample kNone{};
    return port < kPorts ? samples_[port] : kNone;
}

void ManagerImpPc::read(PortReading* out, std::size_t count) noexcept {
    ++stats_.frames_read;
    if (live_source_) live_source_(*this);
    for (std::size_t port = 0; port < count; ++port) {
        PortReading reading;
        if (port < kPorts && samples_[port].connected) {
            const ControllerSample& sample = samples_[port];
            reading.state = PortState::Ready;
            reading.controls = sample.controls & kAllControls;
            reading.left_x = engine_axis(sample.stick_x);
            reading.left_y = static_cast<std::int16_t>(-engine_axis(sample.stick_y));
            reading.right_x = engine_axis(sample.substick_x);
            reading.right_y = static_cast<std::int16_t>(-engine_axis(sample.substick_y));
        }
        out[port] = reading;
    }
}

ManagerImpPc::StickRange ManagerImpPc::stick_range(std::size_t) const noexcept { return StickRange{}; }

void ManagerImpPc::set_rumble_listener(RumbleListener listener) { rumble_listener_ = std::move(listener); }

void ManagerImpPc::set_rumble(std::size_t port, bool on) {
    if (port >= kPorts || rumble_[port] == on) return;
    rumble_[port] = on;
    ++stats_.rumble_changes;
    if (rumble_listener_) rumble_listener_(port, on);
}

bool ManagerImpPc::rumble(std::size_t port) const noexcept { return port < kPorts && rumble_[port]; }

std::uint32_t ManagerImpPc::connected_mask() const noexcept {
    std::uint32_t mask = 0;
    for (std::size_t port = 0; port < kPorts; ++port) {
        if (samples_[port].connected) mask |= 1u << port;
    }
    return mask;
}

void ManagerImpPc::save(std::vector<std::uint8_t>& out) const {
    put_u8(out, kSaveFormat);
    for (std::size_t port = 0; port < kPorts; ++port) {
        const ControllerSample& sample = samples_[port];
        put_u8(out, sample.connected ? 1u : 0u);
        put_u32(out, sample.controls);
        put_u8(out, static_cast<std::uint8_t>(sample.stick_x));
        put_u8(out, static_cast<std::uint8_t>(sample.stick_y));
        put_u8(out, static_cast<std::uint8_t>(sample.substick_x));
        put_u8(out, static_cast<std::uint8_t>(sample.substick_y));
        put_u8(out, rumble_[port] ? 1u : 0u);
    }
}

bool ManagerImpPc::load(const std::uint8_t*& cursor, const std::uint8_t* end) {
    std::uint8_t format = 0;
    if (!get_u8(cursor, end, format) || format != kSaveFormat) return false;
    std::array<ControllerSample, kPorts> samples{};
    std::array<bool, kPorts> rumble{};
    for (std::size_t port = 0; port < kPorts; ++port) {
        std::uint8_t connected = 0, x = 0, y = 0, sx = 0, sy = 0, on = 0;
        std::uint32_t controls = 0;
        if (!get_u8(cursor, end, connected) || !get_u32(cursor, end, controls) || !get_u8(cursor, end, x) ||
            !get_u8(cursor, end, y) || !get_u8(cursor, end, sx) || !get_u8(cursor, end, sy) ||
            !get_u8(cursor, end, on)) {
            return false;
        }
        if (connected > 1u || on > 1u || (controls & ~kAllControls) != 0u) return false;
        samples[port].connected = connected != 0u;
        samples[port].controls = controls;
        samples[port].stick_x = static_cast<std::int8_t>(x);
        samples[port].stick_y = static_cast<std::int8_t>(y);
        samples[port].substick_x = static_cast<std::int8_t>(sx);
        samples[port].substick_y = static_cast<std::int8_t>(sy);
        rumble[port] = on != 0u;
    }
    samples_ = samples;
    for (std::size_t port = 0; port < kPorts; ++port) set_rumble(port, rumble[port]);
    return true;
}

}  // namespace shadow::peripheral
