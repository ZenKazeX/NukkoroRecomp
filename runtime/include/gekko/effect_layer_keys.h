#pragma once

// The effect layer keys (docs/EFFECTS_INVENTORY.md "Layer switches"), as the window reports them
// (HostWindow::take_effect_keys) and the overlay reads them (src/gekko/effect_layer_overlay.h). On the numeric keypad
// (Num Lock on): Num / shows / hides the layer list; while it shows, Num 8 / Num 2 move, Num 5 switches the layer,
// Num 0 solos it, Num . hides the world in a solo, Num - switches every layer on. The same on F6, Page Up / Page Down,
// Insert, Home, End, Delete. Num * shows / hides the FPS counter (any time). None of these keys reaches the game.

namespace gekko {

enum EffectLayerKey : unsigned {
    kEffectKeyMenu = 1u,
    kEffectKeyUp = 2u,
    kEffectKeyDown = 4u,
    kEffectKeyToggle = 8u,
    kEffectKeySolo = 16u,
    kEffectKeyIsolate = 32u,
    kEffectKeyAllOn = 64u,
    kEffectKeyFps = 128u,  // Num *: the FPS counter on / off (any time)
};

}  // namespace gekko
