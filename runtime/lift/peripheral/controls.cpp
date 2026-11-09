#include "peripheral/controls.h"

#include <cstring>

namespace shadow::peripheral {

namespace {

struct NamedControl {
    const char* name;
    Control control;
};

constexpr NamedControl kNames[] = {
    {"A", kControlA},         {"B", kControlB},       {"X", kControlX},         {"Y", kControlY},
    {"Z", kControlZ},         {"L", kControlL},       {"R", kControlR},         {"START", kControlStart},
    {"UP", kControlUp},       {"DOWN", kControlDown}, {"LEFT", kControlLeft},   {"RIGHT", kControlRight},
};

}  // namespace

bool control_from_name(const char* name, Control& out) noexcept {
    if (name == nullptr) return false;
    for (const NamedControl& entry : kNames) {
        if (std::strcmp(entry.name, name) == 0) {
            out = entry.control;
            return true;
        }
    }
    return false;
}

const char* control_name(std::uint32_t control) noexcept {
    for (const NamedControl& entry : kNames) {
        if (entry.control == control) return entry.name;
    }
    return nullptr;
}

}  // namespace shadow::peripheral
