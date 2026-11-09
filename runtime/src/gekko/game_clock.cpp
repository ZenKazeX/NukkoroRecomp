// The game's clock. Contract and rationale: include/gekko/game_clock.h and
// docs/native-time-20261005/DESIGN.md.

#include "gekko/game_clock.h"

namespace gekko {

const char* game_event_name(GameEvent event) noexcept {
    switch (event) {
        case GameEvent::Frame: return "frame";
        case GameEvent::Retrace: return "retrace";
        case GameEvent::Audio: return "audio";
        case GameEvent::Decrementer: return "decrementer";
        case GameEvent::Devices: return "devices";
        case GameEvent::Count: break;
    }
    return "none";
}

}  // namespace gekko
