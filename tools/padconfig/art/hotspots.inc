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
