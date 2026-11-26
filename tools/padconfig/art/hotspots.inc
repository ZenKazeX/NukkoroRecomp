// Generated from the rendered controller views. Each button: its mask's resource id, and the mask's box in its picture
// (fractions of the picture's width and height).
struct Hotspot { const char* key; int id; float x, y, w, h; float cx, cy; };
inline constexpr Hotspot kTopHotspots[] = {
    {"a", 200, 0.7260f, 0.2853f, 0.1240f, 0.1866f, 0.7885f, 0.3786f},
    {"b", 201, 0.6570f, 0.3976f, 0.0590f, 0.0895f, 0.6868f, 0.4424f},
    {"x", 202, 0.8600f, 0.2640f, 0.0620f, 0.1578f, 0.8921f, 0.3425f},
    {"y", 203, 0.7170f, 0.1745f, 0.1040f, 0.0895f, 0.7701f, 0.2182f},
    {"start", 204, 0.4810f, 0.3384f, 0.0380f, 0.0577f, 0.5000f, 0.3673f},
    {"dpad_up", 205, 0.3360f, 0.6146f, 0.0390f, 0.0653f, 0.3549f, 0.6292f},
    {"dpad_down", 206, 0.3360f, 0.7314f, 0.0390f, 0.0653f, 0.3549f, 0.7832f},
    {"dpad_left", 207, 0.2950f, 0.6753f, 0.0430f, 0.0592f, 0.3044f, 0.7059f},
    {"dpad_right", 208, 0.3720f, 0.6753f, 0.0430f, 0.0592f, 0.4060f, 0.7059f},
    {"stick", 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.2065f, 0.3762f},
    {"cstick", 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.6463f, 0.7076f},
};
inline constexpr float kTopAspect = 1.51670f;  // width / height
inline constexpr Hotspot kTiltedHotspots[] = {
    {"l", 209, 0.1070f, 0.1915f, 0.1410f, 0.2177f, 0.1715f, 0.2625f},
    {"r", 210, 0.7520f, 0.1915f, 0.1410f, 0.2128f, 0.8285f, 0.2625f},
    {"z", 211, 0.7660f, 0.3535f, 0.1380f, 0.1162f, 0.8396f, 0.3873f},
};
inline constexpr float kTiltedAspect = 1.63636f;  // width / height
// The view from above in layers, for the sticks: the controller without its two stick caps (resource 103), and each
// cap turned on a bone at its stick's root (art/render_top.py, "sticks") as a GameCube stick moves, in its octagonal
// gate: a sheet of frames, the middle at the top left, then a row for each of kStickRings distances (1/kStickRings ..
// the gate), a column for each of kStickDirections directions (every 22.5 degrees from the right, anticlockwise). Each
// frame in two layers, two sheets: the dome with the shadows (drawn where it is) and the cap alone (drawn on the stick's
// top). Each cap: the two sheets' resource ids, its middle frame's box in the picture (fractions of the picture's width and height; the size
// drawn is the sheet's own), and the radius of its top (fraction of the picture's width: the input ring goes round it).
// kStickTops: where the cap's top is in each frame, in the picture's pixels (render_top.py "sticks-tops", printed by
// pack_sticks.ps1): the frames are drawn on it, so the top glides with the stick.
inline constexpr int kTopBodyId = 103;
inline constexpr int kStickDirections = 16, kStickRings = 4;
struct CapArt { const char* key; int id, cap_id; float x, y, w, h, top; };
inline constexpr CapArt kStickCaps[] = {
    {"stick", 104, 106, 0.0660f, 0.1593f, 0.2900f, 0.4203f, 0.0620f},
    {"cstick", 105, 107, 0.5490f, 0.5584f, 0.1980f, 0.2898f, 0.0340f},
};
inline constexpr float kStickTops[2][kStickRings + 1][kStickDirections][2] = {
    {
        {{204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}, {204.94f, 247.52f}},
        {{224.71f, 247.54f}, {221.82f, 240.58f}, {218.95f, 233.63f}, {211.98f, 230.74f}, {205.04f, 227.87f}, {198.06f, 230.74f}, {191.12f, 233.63f}, {188.22f, 240.58f}, {185.36f, 247.54f}, {188.22f, 254.50f}, {191.12f, 261.46f}, {198.06f, 264.34f}, {205.04f, 267.22f}, {211.98f, 264.34f}, {218.95f, 261.46f}, {221.82f, 254.50f}},
        {{244.40f, 247.62f}, {238.67f, 233.77f}, {232.95f, 219.99f}, {219.10f, 214.20f}, {205.32f, 208.54f}, {191.43f, 214.20f}, {177.68f, 219.99f}, {171.86f, 233.77f}, {166.24f, 247.62f}, {171.86f, 261.44f}, {177.68f, 275.26f}, {191.43f, 281.01f}, {205.32f, 286.70f}, {219.10f, 281.01f}, {232.95f, 275.26f}, {238.67f, 261.44f}},
        {{263.73f, 247.75f}, {255.27f, 227.17f}, {246.76f, 206.78f}, {226.21f, 198.10f}, {205.78f, 189.80f}, {185.11f, 198.10f}, {164.81f, 206.78f}, {156.05f, 227.17f}, {147.83f, 247.75f}, {156.05f, 268.27f}, {164.81f, 288.73f}, {185.11f, 297.33f}, {205.78f, 305.70f}, {226.21f, 297.33f}, {246.76f, 288.73f}, {255.27f, 268.27f}},
        {{282.45f, 247.93f}, {271.45f, 220.85f}, {260.18f, 194.17f}, {233.24f, 182.63f}, {206.42f, 171.91f}, {179.19f, 182.63f}, {152.67f, 194.17f}, {140.97f, 220.85f}, {130.40f, 247.93f}, {140.97f, 274.90f}, {152.67f, 301.69f}, {179.19f, 313.11f}, {206.42f, 323.95f}, {233.24f, 313.11f}, {260.18f, 301.69f}, {271.45f, 274.90f}},
    },
    {
        {{644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}, {644.28f, 465.41f}},
        {{661.07f, 465.37f}, {658.61f, 459.43f}, {656.14f, 453.48f}, {650.20f, 451.01f}, {644.24f, 448.55f}, {638.30f, 451.01f}, {632.34f, 453.48f}, {629.88f, 459.43f}, {627.42f, 465.37f}, {629.88f, 471.33f}, {632.34f, 477.27f}, {638.30f, 479.74f}, {644.24f, 482.20f}, {650.20f, 479.74f}, {656.14f, 477.27f}, {658.61f, 471.33f}},
        {{677.55f, 465.26f}, {672.71f, 453.45f}, {667.76f, 441.63f}, {655.98f, 436.72f}, {644.12f, 431.84f}, {632.31f, 436.72f}, {620.49f, 441.63f}, {615.58f, 453.45f}, {610.70f, 465.26f}, {615.58f, 477.12f}, {620.49f, 488.90f}, {632.31f, 493.85f}, {644.12f, 498.68f}, {655.98f, 493.85f}, {667.76f, 488.90f}, {672.71f, 477.12f}},
        {{693.50f, 465.08f}, {686.42f, 447.55f}, {678.98f, 430.03f}, {661.56f, 422.69f}, {643.93f, 415.51f}, {626.40f, 422.69f}, {608.88f, 430.03f}, {601.54f, 447.55f}, {594.36f, 465.08f}, {601.54f, 482.71f}, {608.88f, 500.13f}, {626.40f, 507.56f}, {643.93f, 514.65f}, {661.56f, 507.56f}, {678.98f, 500.13f}, {686.42f, 482.71f}},
        {{708.71f, 464.83f}, {699.57f, 441.79f}, {689.66f, 418.83f}, {666.87f, 409.09f}, {643.66f, 399.78f}, {620.63f, 409.09f}, {597.66f, 418.83f}, {587.93f, 441.79f}, {578.61f, 464.83f}, {587.93f, 488.03f}, {597.66f, 510.82f}, {620.63f, 520.73f}, {643.66f, 529.88f}, {666.87f, 520.73f}, {689.66f, 510.82f}, {699.57f, 488.03f}},
    },
};