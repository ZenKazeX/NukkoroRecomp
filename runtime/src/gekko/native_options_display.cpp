// The Options menu's Display tile. Contract: include/gekko/native_options_display.h.

#include "gekko/native_options_display.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

#include "assets/chunk_stream.h"
#include "assets/texture_dictionary.h"
#include "assets/texture_import.h"
#include "gekko/cpu.h"
#include "gekko/display_geometry.h"
#include "gekko/game_clock.h"
#include "gekko/native_save_text.h"

namespace gekko::options_display {

namespace {

#include "options_menu_tiles.inc"

// r13 / r2 of main.dol and the input globals the button queries read.
constexpr u32 kR13 = 0x805EC500u;
constexpr u32 kR2 = 0x805FA780u;
constexpr u32 kFadeHolder = kR13 + 0x386Cu;  // 0x801178FC's object: (*(*holder)) + 0x18 = the fade's level
constexpr u32 kFadeIdle = kR2 - 0x590u;      // the level of no fade (0x802E5D88 compares with it)
constexpr u32 kButtonTable = kR13 + 0x59E4u; // u32[id][3]: the pad masks of a menu button
constexpr u32 kPressPort = kR13 + 0x59E0u;   // the port that pressed A (0x80357068)
constexpr u32 kInputHolder = kR13 + 0x2D9Cu; // 0x8005185C's object
// 0x8034A608's held-button counters: one s16 per mask, per port.
constexpr u32 kHoldMasks[8] = {0x40u, 0x80u, 0x200u, 0x100u, 0x40000u, 0x80000u, 0x200000u, 0x100000u};
constexpr u32 kDpad = 0x3C0u, kStick = 0x3C0000u;
constexpr u32 kRepeatDelay = 0xCu, kRepeatPeriod = 1u;  // 0x80356F98's arguments

// Menu sounds (0x801D59AC ids the page plays).
constexpr u32 kSoundCursor = 0x1E002u;
constexpr u32 kSoundDecide = 0x1E003u;

constexpr u32 kButtonA = 1u, kButtonUp = 3u, kButtonDown = 4u, kButtonLeft = 5u, kButtonRight = 6u;

float f32_at(Cpu& cpu, u32 address) {
    const u32 bits = cpu.mem_read32(address);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

bool all_of(u32 mask, u32 edges) { return (mask & edges) == mask; }

// 0x8034A608's test of one port's edges: the whole mask, or its d-pad part, or its stick part.
bool edge_rule(u32 mask, u32 edges) {
    if (all_of(mask, edges)) return true;
    const u32 dpad = mask & kDpad;
    if (dpad != 0u && all_of(dpad, edges)) return true;
    const u32 stick = mask & kStick;
    return stick != 0u && all_of(stick, edges);
}

// 0x8034A608(input, port, mask, delay, period).
bool repeat_rule(Cpu& cpu, u32 input, u32 port, u32 mask, u32 delay, u32 period) {
    if (mask == 0u) return false;
    if (port == 4u) {
        const u32 active = cpu.mem_read32(input + 0x11Cu);
        if (active < 2u) port = active;
    } else if (port == 5u) {
        port = cpu.mem_read32(input + 0x120u);
    }
    if (port >= 2u) {
        return repeat_rule(cpu, input, 0u, mask, delay, period) || repeat_rule(cpu, input, 1u, mask, delay, period);
    }
    if (edge_rule(mask, cpu.mem_read32(input + 0x30u + port * 0x28u))) return true;
    for (u32 slot = 0; slot < 8u; ++slot) {
        if ((mask & kHoldMasks[slot]) == 0u) continue;
        const s32 held = static_cast<s16>(cpu.mem_read16(input + port * 16u + slot * 2u));
        const s32 past = held - static_cast<s32>(delay + 1u);
        if (past >= 0 && past % static_cast<s32>(period) == 0) return true;
    }
    return false;
}

// 0x802B810C / 0x802B8168's gate: no query is answered while a fade runs.
bool queries_open(Cpu& cpu, u32& input, u32& table) {
    const u32 holder = cpu.mem_read32(kFadeHolder);
    if (holder == 0u) return false;
    const u32 fade = cpu.mem_read32(holder);
    if (fade == 0u || f32_at(cpu, fade + 0x18u) != f32_at(cpu, kFadeIdle)) return false;
    input = cpu.mem_read32(kInputHolder);
    table = cpu.mem_read32(kButtonTable);
    return input != 0u && table != 0u;
}

std::u16string ascii16(std::string_view s) { return std::u16string(s.begin(), s.end()); }

constexpr double kRates[] = {30.0, 60.0, 120.0, 144.0, 165.0, 240.0};

const Texts kTexts[6] = {
    {"Change the screen and picture settings.",
     {"Display mode", "Screen ratio", "Resolution", "Render resolution", "Frame rate", "V-Sync", "Frame blending",
      "Draw distance", "FPS counter", "Anti-aliasing (MSAA)", "Anisotropic filtering"},
     "Windowed", "Fullscreen", "Auto", "Unlocked", "Original", "Maximum", "Exclusive"},
    {"Modifier les param\xC3\xA8tres de l'\xC3\xA9" "cran et de l'image.",
     {"Mode d'affichage", "Format d'\xC3\xA9" "cran", "R\xC3\xA9solution", "R\xC3\xA9solution de rendu",
      "Fr\xC3\xA9quence d'images", "V-Sync", "Fusion d'images", "Distance d'affichage", "Compteur FPS", "Anticr\xC3\xA9nelage (MSAA)", "Filtrage anisotrope"},
     "Fen\xC3\xAAtr\xC3\xA9", "Plein \xC3\xA9" "cran", "Auto", "Illimit\xC3\xA9", "Original", "Maximum", "Exclusif"},
    {"\xC3\x84ndere die Bildschirm- und Bildeinstellungen.",
     {"Anzeigemodus", "Bildformat", "Aufl\xC3\xB6sung", "Renderaufl\xC3\xB6sung", "Bildrate", "V-Sync",
      "Bildmischung", "Sichtweite", "FPS-Anzeige", "Kantengl\xC3\xA4ttung (MSAA)", "Anisotrope Filterung"},
     "Fenster", "Vollbild", "Auto", "Unbegrenzt", "Original", "Maximum", "Exklusiv"},
    {"Cambia i parametri dello schermo e dell'immagine.",
     {"Modalit\xC3\xA0 schermo", "Formato schermo", "Risoluzione", "Risoluzione render", "Frequenza fotogrammi",
      "V-Sync", "Fusione fotogrammi", "Distanza visiva", "Contatore FPS", "Antialiasing (MSAA)", "Filtro anisotropico"},
     "Finestra", "Schermo intero", "Auto", "Sbloccato", "Originale", "Massimo", "Esclusivo"},
    {"\xE7\x94\xBB\xE9\x9D\xA2\xE3\x81\xA8\xE6\x98\xA0\xE5\x83\x8F\xE3\x81\xAE\xE8\xA8\xAD\xE5\xAE\x9A\xE3\x82\x92"
     "\xE5\xA4\x89\xE6\x9B\xB4\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x99\xE3\x80\x82",
     {"\xE7\x94\xBB\xE9\x9D\xA2\xE3\x83\xA2\xE3\x83\xBC\xE3\x83\x89",
      "\xE3\x82\xA2\xE3\x82\xB9\xE3\x83\x9A\xE3\x82\xAF\xE3\x83\x88",
      "\xE8\xA7\xA3\xE5\x83\x8F\xE5\xBA\xA6",
      "\xE5\x86\x85\xE9\x83\xA8\xE8\xA7\xA3\xE5\x83\x8F\xE5\xBA\xA6",
      "\xE3\x83\x95\xE3\x83\xAC\xE3\x83\xBC\xE3\x83\xA0\xE3\x83\xAC\xE3\x83\xBC\xE3\x83\x88",
      "V-Sync",
      "\xE3\x83\x95\xE3\x83\xAC\xE3\x83\xBC\xE3\x83\xA0\xE3\x83\x96\xE3\x83\xAC\xE3\x83\xB3\xE3\x83\x89",
      "\xE9\x81\xA0\xE6\x99\xAF\xE8\xA1\xA8\xE7\xA4\xBA",
      "FPS\xE8\xA1\xA8\xE7\xA4\xBA", "MSAA", "AF"},
     "\xE3\x82\xA6\xE3\x82\xA3\xE3\x83\xB3\xE3\x83\x89\xE3\x82\xA6",
     "\xE3\x83\x95\xE3\x83\xAB\xE3\x82\xB9\xE3\x82\xAF\xE3\x83\xAA\xE3\x83\xBC\xE3\x83\xB3",
     "\xE8\x87\xAA\xE5\x8B\x95", "\xE7\x84\xA1\xE5\x88\xB6\xE9\x99\x90",
     "\xE3\x82\xAA\xE3\x83\xAA\xE3\x82\xB8\xE3\x83\x8A\xE3\x83\xAB", "\xE6\x9C\x80\xE5\xA4\xA7", "\xE3\x82\xA8\xE3\x82\xAF\xE3\x82\xB9\xE3\x82\xAF\xE3\x83\xAB\xE3\x83\xBC\xE3\x82\xB7\xE3\x83\x96"},
    {"Cambiar la configuraci\xC3\xB3n de la pantalla y la imagen.",
     {"Modo de pantalla", "Proporci\xC3\xB3n", "Resoluci\xC3\xB3n", "Resoluci\xC3\xB3n de render", "Tasa de cuadros",
      "V-Sync", "Mezcla de cuadros", "Distancia de visi\xC3\xB3n", "Contador FPS", "Antialiasing (MSAA)", "Filtrado anisotr\xC3\xB3pico"},
     "Ventana", "Pantalla completa", "Auto", "Desbloqueado", "Original", "M\xC3\xA1ximo", "Exclusivo"},
};

// "code x0 y0 x1 y1 #" lines of a .met after "METRICS1", its sheet's name and a small number.
struct GlyphList {
    std::string sheet;
    struct Entry {
        u32 code, x0, y0, x1, y1;
    };
    std::vector<Entry> entries;
};

bool parse_glyph_list(const std::vector<u8>& met, GlyphList& out) {
    std::string text(met.begin(), met.end());
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        at = end + 1;
    }
    if (lines.size() < 3u || lines[0].rfind("METRICS1", 0) != 0) return false;
    out.sheet = lines[1];
    while (!out.sheet.empty() && (out.sheet.back() == ' ' || out.sheet.back() == '\t')) out.sheet.pop_back();
    while (!out.sheet.empty() && (out.sheet.front() == ' ' || out.sheet.front() == '\t')) out.sheet.erase(0, 1);
    for (std::size_t i = 3; i < lines.size(); ++i) {
        u32 v[5];
        const char* p = lines[i].c_str();
        u32 got = 0;
        for (; got < 5u; ++got) {
            char* end = nullptr;
            const unsigned long n = std::strtoul(p, &end, 10);
            if (end == p) break;
            v[got] = static_cast<u32>(n);
            p = end;
        }
        if (got == 5u) out.entries.push_back({v[0], v[1], v[2], v[3], v[4]});
    }
    return !out.entries.empty();
}

u16 rgb5a3(const u8* p) {
    const u32 r = p[0], g = p[1], b = p[2], a = p[3];
    if (a >= 0xE0u)  // opaque: 5:5:5 (the 3-bit alpha's top level is opaque too)
        return static_cast<u16>(0x8000u | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
    return static_cast<u16>((a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
}

}  // namespace

std::u16string utf16(std::string_view s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 code = c, extra = 0;
        if (c >= 0xF0u) code = c & 0x07u, extra = 3;
        else if (c >= 0xE0u) code = c & 0x0Fu, extra = 2;
        else if (c >= 0xC0u) code = c & 0x1Fu, extra = 1;
        ++i;
        for (u32 k = 0; k < extra && i < s.size(); ++k, ++i) code = code << 6 | (static_cast<u8>(s[i]) & 0x3Fu);
        if (code >= 0x10000u) {
            code -= 0x10000u;
            out.push_back(static_cast<char16_t>(0xD800u + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00u + (code & 0x3FFu)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
    }
    return out;
}


namespace {
std::atomic<u64>& screen_size() {
    static std::atomic<u64> size{0};
    return size;
}
std::atomic<u64>& work_area_size() {
    static std::atomic<u64> size{0};
    return size;
}

// The resolution scales offered (percent per side of the picture, gekko/display_geometry.h, 25..200). A scale in
// effect that is not one of them is listed as itself.
constexpr double kScales[] = {25.0, 33.0, 50.0, 67.0, 75.0, 100.0, 125.0, 150.0, 175.0, 200.0};

// The window sizes offered. Which of them a ratio may use is decided by shape (kRatioSnap), so a monitor size a little
// off its named ratio (1366 x 768 for 16:9, 3440 x 1440 for 21:9) is still listed under it.
constexpr Model::Size kResolutions[] = {
    {640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 960},  {1400, 1050}, {1600, 1200}, {1920, 1440}, {2048, 1536},  // 4:3
    {1280, 1024}, {1800, 1440},                                                                                                  // 5:4
    {1280, 800},  {1440, 900},  {1680, 1050}, {1920, 1200}, {2560, 1600}, {3840, 2400},                                          // 16:10
    {854, 480},   {1280, 720},  {1366, 768},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}, {5120, 2880},  // 16:9
    {2560, 1080}, {3440, 1440}, {3840, 1600}, {5120, 2160},                                                                      // 21:9
    {1920, 600},  {2560, 800},  {3840, 1200}, {5120, 1600},                                                                      // 32:10
    {3840, 1080}, {5120, 1440},                                                                                                  // 32:9
};
}  // namespace

void set_screen_size(u32 width, u32 height) noexcept {
    screen_size().store(static_cast<u64>(width) << 32 | height, std::memory_order_relaxed);
}
void set_work_area(u32 width, u32 height) noexcept {
    work_area_size().store(static_cast<u64>(width) << 32 | height, std::memory_order_relaxed);
}
Model::Size work_area() noexcept {
    const u64 work = work_area_size().load(std::memory_order_relaxed);
    const Model::Size s{static_cast<u32>(work >> 32), static_cast<u32>(work)};
    return s.width != 0u && s.height != 0u ? s : Model::Size{0u, 0u};
}

namespace {
std::atomic<u32>& msaa_mask() {
    static std::atomic<u32> mask{0x0Fu};  // 1, 2, 4, 8 until the renderer says
    return mask;
}
constexpr u32 kAnisotropyLevels[] = {1u, 2u, 4u, 8u, 16u};
}  // namespace

void set_msaa_supported(u32 mask) noexcept { msaa_mask().store(mask | 1u, std::memory_order_relaxed); }

std::vector<u32> Model::msaa_values() const {
    std::vector<u32> out;
    const u32 mask = msaa_mask().load(std::memory_order_relaxed);
    for (u32 bit = 0; bit <= 5u; ++bit)
        if ((mask & (1u << bit)) != 0u) out.push_back(1u << bit);
    if (std::find(out.begin(), out.end(), msaa_setting) == out.end()) out.push_back(msaa_setting);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<u32> Model::anisotropy_values() const {
    return std::vector<u32>(std::begin(kAnisotropyLevels), std::end(kAnisotropyLevels));
}

const Texts& texts(Language language) noexcept { return kTexts[static_cast<u32>(language) % 6u]; }

Shared& shared() noexcept {
    static Shared s;
    return s;
}

void Shared::push(Page page) {
    pages[next] = std::move(page);
    next = (next + 1u) % kHistory;
}

const Page* Shared::at(u64 game_ticks) const {
    const Page* best = nullptr;
    for (const Page& p : pages)
        if (p.game_ticks != 0u && p.game_ticks <= game_ticks && (best == nullptr || p.game_ticks >= best->game_ticks))
            best = &p;
    // The screen draws its rows every frame: nothing published within the last two 60 Hz frames means it is gone.
    constexpr u64 kTwoFrames = kGameTicksPerSecond / 30u;
    if (best == nullptr || !best->visible || game_ticks - best->game_ticks > kTwoFrames) return nullptr;
    return best;
}

const Glyph* Font::find(char16_t code) const noexcept {
    const auto it = std::lower_bound(glyphs.begin(), glyphs.end(), code,
                                     [](const std::pair<char16_t, Glyph>& g, char16_t c) { return g.first < c; });
    return it != glyphs.end() && it->first == code ? &it->second : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// The model (gekko/display_geometry.h): every setting held as its value; a list only says where a step goes. No row
// changes another row's value.

std::vector<double> Model::aspects() const {
    std::vector<double> out{0.0};  // Auto
    for (const NamedAspect& a : kNamedAspects) out.push_back(static_cast<double>(a.w) / a.h);
    if (aspect > 0.0 && std::find(out.begin(), out.end(), aspect) == out.end()) out.push_back(aspect);
    return out;  // Auto, then the named ratios in their order, then a custom one in effect (display.cfg aspect=2.37)
}

std::vector<double> Model::scales() const {
    std::vector<double> out(std::begin(kScales), std::end(kScales));
    if (std::find(out.begin(), out.end(), scale) == out.end()) out.push_back(scale);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Model::Size> Model::resolutions() const {
    std::vector<Size> out;
    if (value[kDisplayMode] != 0u) {  // borderless fullscreen shows at the desktop's size: one picture, not a choice
        const PixelSize picture = geometry().picture;
        out.push_back({picture.width, picture.height});
        return out;
    }
    const auto listed = [&](const Size& s) {
        return std::any_of(out.begin(), out.end(), [&](const Size& o) { return o.width == s.width && o.height == s.height; });
    };
    for (const Size& s : kResolutions) {
        if (work.width != 0u && (s.width > work.width || s.height > work.height)) continue;
        if (aspect > 0.0 && std::fabs(static_cast<double>(s.width) / s.height / aspect - 1.0) > kRatioSnap) continue;
        out.push_back(s);
    }
    if (!listed(window)) out.push_back(window);
    std::sort(out.begin(), out.end(), [](const Size& a, const Size& b) {
        const u64 aa = static_cast<u64>(a.width) * a.height, bb = static_cast<u64>(b.width) * b.height;
        return aa != bb ? aa < bb : a.width < b.width;
    });
    return out;
}

PixelSize Model::output() const {
    if (value[kDisplayMode] != 0u && screen.width != 0u) return {screen.width, screen.height};
    Size s = window;
    if (work.width != 0u && (s.width > work.width || s.height > work.height)) {
        // The window owner shrinks a client the desktop cannot hold to the largest of its shape inside it.
        const PixelSize fit = fit_aspect({work.width, work.height}, static_cast<double>(s.width) / s.height);
        s = {fit.width, fit.height};
    }
    return {s.width, s.height};
}

DisplayGeometry Model::geometry() const { return display_geometry(output(), aspect, scale); }

Model::Size Model::viewport() const {
    const DisplayGeometry g = geometry();
    return {g.picture.width, g.picture.height};
}

u32 Model::count(u32 row) const {
    switch (row) {
    case kDisplayMode: return 2u;  // windowed, fullscreen
    case kScreenRatio: return static_cast<u32>(aspects().size());
    case kResolution: return static_cast<u32>(resolutions().size());
    case kRenderResolution: return static_cast<u32>(scales().size());
    case kFrameRate: return static_cast<u32>(rates.size());
    case kVsync: case kFrameBlending: case kFpsCounter: return 2u;
    case kDrawDistance: return 8u;
    case kMsaa: return static_cast<u32>(msaa_values().size());
    case kAnisotropy: return static_cast<u32>(anisotropy_values().size());
    default: return 0u;
    }
}

u32 Model::index(u32 row) const {
    switch (row) {
    case kScreenRatio: {
        const std::vector<double> list = aspects();
        return static_cast<u32>(std::find(list.begin(), list.end(), aspect) - list.begin());
    }
    case kResolution: {
        if (value[kDisplayMode] != 0u) return 0u;
        const std::vector<Size> list = resolutions();
        for (u32 i = 0; i < list.size(); ++i)
            if (list[i].width == window.width && list[i].height == window.height) return i;
        return 0u;
    }
    case kRenderResolution: {
        const std::vector<double> list = scales();
        return static_cast<u32>(std::find(list.begin(), list.end(), scale) - list.begin());
    }
    default: return row < kRowCount ? value[row] : 0u;
    }
}

Model Model::from(const DisplaySettings& s) {
    Model m;
    m.value[kDisplayMode] = s.fullscreen ? 1u : 0u;
    m.window = {static_cast<u32>(std::max(1, s.window_width)), static_cast<u32>(std::max(1, s.window_height))};
    const u64 screen = screen_size().load(std::memory_order_relaxed);
    m.screen = {static_cast<u32>(screen >> 32), static_cast<u32>(screen)};
    if (m.screen.width == 0u || m.screen.height == 0u) m.screen = {0u, 0u};
    m.work = work_area();
    m.aspect = s.aspect > 0.0 ? s.aspect : 0.0;
    m.scale = s.render_scale;
    m.rates.assign(std::begin(kRates), std::end(kRates));
    if (s.render_rate > 0.0 && std::find(m.rates.begin(), m.rates.end(), s.render_rate) == m.rates.end())
        m.rates.push_back(s.render_rate);
    std::sort(m.rates.begin(), m.rates.end());
    m.rates.push_back(0.0);  // unlocked, last
    for (u32 i = 0; i < m.rates.size(); ++i)
        if (m.rates[i] == s.render_rate) m.value[kFrameRate] = i;
    m.value[kVsync] = s.vsync ? 0u : 1u;  // ON, OFF
    m.value[kFrameBlending] = s.frame_interpolation ? 1u : 0u;  // OFF, ON
    m.value[kDrawDistance] = static_cast<u32>(std::clamp(std::lround(s.draw_distance), 1L, 8L) - 1);
    m.value[kFpsCounter] = s.fps_counter ? 1u : 0u;  // OFF, ON
    m.msaa_setting = static_cast<u32>(std::max(1, s.msaa));
    {
        const std::vector<u32> counts = m.msaa_values();
        for (u32 i = 0; i < counts.size(); ++i)
            if (counts[i] == m.msaa_setting) m.value[kMsaa] = i;
        const std::vector<u32> levels = m.anisotropy_values();
        for (u32 i = 0; i < levels.size(); ++i)
            if (static_cast<int>(levels[i]) == s.anisotropy) m.value[kAnisotropy] = i;
    }
    return m;
}

void Model::step(u32 row, int delta) {
    if (row >= kRowCount || delta == 0) return;
    switch (row) {
    case kScreenRatio: {
        // By place in the list (it is in the named order, not sorted); the ratio in effect is always in it.
        const std::vector<double> list = aspects();
        const int at = static_cast<int>(index(kScreenRatio)) + delta;
        aspect = list[static_cast<std::size_t>(std::clamp(at, 0, static_cast<int>(list.size()) - 1))];
        return;
    }
    case kResolution: {
        if (value[kDisplayMode] != 0u) return;  // fullscreen has one picture: nothing to move to
        const std::vector<Size> list = resolutions();
        const int at = static_cast<int>(index(kResolution)) + delta;
        window = list[static_cast<std::size_t>(std::clamp(at, 0, static_cast<int>(list.size()) - 1))];
        return;
    }
    case kRenderResolution: {
        const std::vector<double> list = scales();
        const int at = static_cast<int>(index(kRenderResolution)) + delta;
        scale = list[static_cast<std::size_t>(std::clamp(at, 0, static_cast<int>(list.size()) - 1))];
        return;
    }
    default: {
        const u32 n = count(row);
        if (n == 0u) return;
        value[row] = static_cast<u32>(std::clamp<s64>(static_cast<s64>(value[row]) + delta, 0, static_cast<s64>(n) - 1));
        return;
    }
    }
}

std::u16string Model::value_text(u32 row, const Texts& t, const std::u16string& on,
                                 const std::u16string& off) const {
    switch (row) {
    case kDisplayMode: return utf16(value[row] == 0u ? t.windowed : t.fullscreen);
    case kScreenRatio:
        if (!(aspect > 0.0)) return utf16(t.automatic);
        return ascii16(aspect_label(aspect));
    case kResolution: {
        const std::vector<Size> list = resolutions();
        const Size s = list[std::min<std::size_t>(index(kResolution), list.size() - 1)];
        return ascii16(std::to_string(s.width) + "x" + std::to_string(s.height));
    }
    case kRenderResolution: {
        char text[48];
        std::snprintf(text, sizeof(text), scale == std::floor(scale) ? "%.0f%%" : "%.1f%%", scale);
        const DisplayGeometry g = geometry();
        if (g.scale_capped) {
            char capped[48];
            std::snprintf(capped, sizeof(capped), " (%.0f%% in use)", g.effective_scale);
            return ascii16(std::string(text) + capped);
        }
        return ascii16(text);
    }
    case kFrameRate: {
        if (rates.empty()) return {};
        const double r = rates[std::min<std::size_t>(value[row], rates.size() - 1)];
        if (r <= 0.0) return utf16(t.unlocked);
        char text[32];
        std::snprintf(text, sizeof(text), r == std::floor(r) ? "%.0f" : "%.2f", r);
        return ascii16(text);
    }
    case kVsync: return value[row] == 0u ? on : off;
    case kFrameBlending: case kFpsCounter: return value[row] != 0u ? on : off;
    case kMsaa: {
        const std::vector<u32> counts = msaa_values();
        const u32 n = counts[std::min<std::size_t>(value[row], counts.size() - 1)];
        if (n <= 1u) return off;
        const bool supported = (msaa_mask().load(std::memory_order_relaxed) & n) != 0u;
        return ascii16(std::to_string(n) + "x" + (supported ? "" : " (n/a)"));
    }
    case kAnisotropy: {
        const std::vector<u32> levels = anisotropy_values();
        const u32 n = levels[std::min<std::size_t>(value[row], levels.size() - 1)];
        return n <= 1u ? off : ascii16(std::to_string(n) + "x");
    }
    case kDrawDistance: {
        std::u16string v = ascii16("x" + std::to_string(value[row] + 1u));
        if (value[row] == 0u) v += u' ' + utf16(t.original);
        if (value[row] == 7u) v += u' ' + utf16(t.maximum);
        return v;
    }
    default: return {};
    }
}

DisplaySettings Model::settings(const DisplaySettings& base) const {
    DisplaySettings s = base;  // the window size, the exclusive keys and the gameplay aspect are not the page's
    s.fullscreen = value[kDisplayMode] != 0u;
    s.aspect = aspect;
    s.render_scale = scale;
    if (!rates.empty()) s.render_rate = rates[std::min<std::size_t>(value[kFrameRate], rates.size() - 1)];
    s.vsync = value[kVsync] == 0u;
    s.frame_interpolation = value[kFrameBlending] != 0u;
    s.draw_distance = static_cast<float>(value[kDrawDistance] + 1u);
    s.fps_counter = value[kFpsCounter] != 0u;
    {
        const std::vector<u32> counts = msaa_values();
        s.msaa = static_cast<int>(counts[std::min<std::size_t>(value[kMsaa], counts.size() - 1)]);
        const std::vector<u32> levels = anisotropy_values();
        s.anisotropy = static_cast<int>(levels[std::min<std::size_t>(value[kAnisotropy], levels.size() - 1)]);
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// The game's input state

bool button_repeats(Cpu& cpu, u32 button) {
    u32 input = 0, table = 0;
    if (!queries_open(cpu, input, table)) return false;
    for (u32 k = 0; k < 3u; ++k) {
        const u32 mask = cpu.mem_read32(table + button * 12u + k * 4u);
        if (mask != 0u && repeat_rule(cpu, input, 4u, mask, kRepeatDelay, kRepeatPeriod)) return true;
    }
    return false;
}

bool button_pressed(Cpu& cpu, u32 button) {
    u32 input = 0, table = 0;
    if (!queries_open(cpu, input, table)) return false;
    for (u32 k = 0; k < 3u; ++k) {
        const u32 mask = cpu.mem_read32(table + button * 12u + k * 4u);
        if (mask == 0u || !edge_rule(mask, cpu.mem_read32(input + 0x30u + 4u * 0x28u))) continue;
        if (button == kButtonA) {
            // 0x80357068 notes which port pressed A.
            const u32 first = cpu.mem_read32(input + 0x11Cu), second = cpu.mem_read32(input + 0x120u);
            if (first < 3u) {
                cpu.mem_write32(kPressPort, first);
            } else {
                for (u32 port = 0; port < 2u; ++port) {
                    if (port == first || port == second) continue;
                    if (edge_rule(mask, cpu.mem_read32(input + 0x30u + port * 0x28u))) {
                        cpu.mem_write32(kPressPort, port);
                        break;
                    }
                }
            }
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// The font

bool build_font(Language language,
                const std::vector<std::pair<const std::vector<u8>*, const std::vector<std::vector<u8>>*>>& files,
                Font& font) {
    font = Font{};
    font.language = language;
    std::set<char16_t> seen;
    for (const auto& [txd, lists] : files) {
        if (txd == nullptr || lists == nullptr || txd->empty()) continue;
        namespace assets = shadow::assets;
        const assets::ByteView bytes{txd->data(), txd->size()};
        std::vector<assets::Chunk> top;
        if (!assets::read_stream(bytes, &top)) continue;
        const assets::Chunk* dict = assets::find(top, assets::chunk::kTextureDictionary);
        assets::TextureDictionary dictionary;
        std::string error;
        if (dict == nullptr || !assets::parse_texture_dictionary(bytes, *dict, &dictionary, &error)) continue;
        for (const std::vector<u8>& met : *lists) {
            GlyphList list;
            if (!parse_glyph_list(met, list)) continue;
            const auto record = std::find_if(dictionary.textures.begin(), dictionary.textures.end(),
                                             [&](const assets::TextureRecord& r) { return r.name == list.sheet; });
            if (record == dictionary.textures.end()) continue;
            assets::TextureImage image;
            assets::ImportOptions options;
            options.generate_mips = false;
            if (!assets::import_texture(*record, options, &image, &error)) continue;
            Font::Sheet sheet;
            sheet.width = image.width;
            sheet.height = image.height;
            sheet.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4u);
            // The sheet as stored: grey glyphs on black, opaque. The game blends it by its colour (the overlay's two
            // passes), never by alpha.
            for (std::size_t i = 0; i < sheet.rgba.size(); i += 4u) {
                sheet.rgba[i] = image.rgba[i];
                sheet.rgba[i + 1] = image.rgba[i + 1];
                sheet.rgba[i + 2] = image.rgba[i + 2];
                sheet.rgba[i + 3] = 255u;
            }
            const u32 index = static_cast<u32>(font.sheets.size());
            for (const GlyphList::Entry& e : list.entries) {
                if (e.code > 0xFFFFu || e.x1 >= sheet.width || e.y1 >= sheet.height || e.x1 < e.x0 || e.y1 < e.y0)
                    continue;
                const char16_t code = static_cast<char16_t>(e.code);
                // Ink columns: a sheet can list a character it has no ink for (EN ':'), which counts as missing.
                u32 left = ~0u, right = 0u;
                for (u32 x = e.x0; x <= e.x1; ++x)
                    for (u32 y = e.y0; y <= e.y1; ++y)
                        if (sheet.rgba[(static_cast<std::size_t>(y) * sheet.width + x) * 4u + 1u] > 40u) {
                            left = std::min(left, x - e.x0);
                            right = std::max(right, x - e.x0 + 1u);
                        }
                if (left == ~0u && code != u' ') continue;
                if (!seen.insert(code).second) continue;
                Glyph g;
                g.sheet = index;
                g.x = static_cast<u16>(e.x0);
                g.y = static_cast<u16>(e.y0);
                g.width = static_cast<u16>(e.x1 - e.x0 + 1u);
                g.height = static_cast<u16>(e.y1 - e.y0 + 1u);
                g.ink_left = static_cast<u16>(left == ~0u ? 0u : left);
                g.ink_right = static_cast<u16>(right);
                font.glyphs.emplace_back(code, g);
            }
            font.sheets.push_back(std::move(sheet));
        }
    }
    if (font.glyphs.empty()) return false;
    std::sort(font.glyphs.begin(), font.glyphs.end(),
              [](const std::pair<char16_t, Glyph>& a, const std::pair<char16_t, Glyph>& b) { return a.first < b.first; });
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Files

namespace {

// Options2 of an Options_<lang>.txd: its record, checked (512 x 512 RGB5A3).
bool options2(const std::vector<u8>& txd, shadow::assets::TextureRecord& out, std::string& error) {
    namespace assets = shadow::assets;
    const assets::ByteView bytes{txd.data(), txd.size()};
    std::vector<assets::Chunk> top;
    if (!assets::read_stream(bytes, &top)) {
        error = "the Options dictionary is not a RenderWare stream";
        return false;
    }
    const assets::Chunk* dict = assets::find(top, assets::chunk::kTextureDictionary);
    assets::TextureDictionary dictionary;
    if (dict == nullptr || !assets::parse_texture_dictionary(bytes, *dict, &dictionary, &error)) {
        if (error.empty()) error = "the Options stream holds no texture dictionary";
        return false;
    }
    const auto record = std::find_if(dictionary.textures.begin(), dictionary.textures.end(),
                                     [](const assets::TextureRecord& r) { return r.name == "Options2"; });
    if (record == dictionary.textures.end() || record->encoding != assets::TexelEncoding::RGB5A3 ||
        record->width != 512u || record->height != 512u) {
        error = "the Options dictionary has no 512 x 512 RGB5A3 Options2";
        return false;
    }
    if (record->texels.size < assets::level_bytes(record->encoding, record->width, record->height)) {
        error = "Options2's texels are short";
        return false;
    }
    out = *record;
    return true;
}

}  // namespace

bool place_tile_art(const std::vector<u8>& txd, std::vector<u8>& out, std::string& error) {
    shadow::assets::TextureRecord record;
    if (!options2(txd, record, error)) return false;
    out = txd;
    u8* texels = out.data() + (record.texels.data - txd.data());
    const u32 tiles_x = record.width / 4u;
    // Cells 41..46 (items 0..5, normal) and 47..52 (selected): 82 x 82 at x 0 / 83 (+ 166 selected), y 0 / 82 / 164.
    for (u32 item = 0; item < 6u; ++item) {
        for (u32 state = 0; state < 2u; ++state) {
            const u32 cx = (item % 2u) * 83u + state * 166u, cy = (item / 2u) * 82u;
            const unsigned char* art = kTileArt[item][state];
            for (u32 y = 0; y < kTileSide; ++y) {
                for (u32 x = 0; x < kTileSide; ++x) {
                    const u32 tx = cx + x, ty = cy + y;
                    const std::size_t at = (static_cast<std::size_t>(ty / 4u) * tiles_x + tx / 4u) * 32u +
                                           ((ty % 4u) * 4u + tx % 4u) * 2u;
                    const u16 v = rgb5a3(art + (static_cast<std::size_t>(y) * kTileSide + x) * 4u);
                    texels[at] = static_cast<u8>(v >> 8);
                    texels[at + 1] = static_cast<u8>(v & 0xFFu);
                }
            }
        }
    }
    return true;
}

bool insert_messages(const std::vector<u8>& table, const std::vector<std::vector<u8>>& glyph_lists,
                     const std::vector<std::pair<u32, std::u16string>>& messages, u32 template_id,
                     std::vector<u8>& out, u32& missing, std::string& error) {
    std::set<u32> codes;
    for (const std::vector<u8>& met : glyph_lists) {
        GlyphList list;
        if (!parse_glyph_list(met, list)) continue;
        for (const GlyphList::Entry& e : list.entries) codes.insert(e.code);
    }
    missing = 0;
    std::vector<std::pair<u32, std::u16string>> checked;
    for (const auto& [id, text] : messages) {
        std::u16string t;
        for (const char16_t unit : text) {
            if (unit == u' ' || unit == u'\n' || codes.count(unit) != 0u) t.push_back(unit);
            else ++missing;  // the game would draw nothing there; dropped and reported
        }
        checked.emplace_back(id, std::move(t));
    }
    return save_text::insert_texts(table, checked, template_id, out, error);
}

bool message_by_id(const std::vector<u8>& table, u32 id, std::u16string& text) {
    return save_text::text_by_id(table, id, text);
}

}  // namespace gekko::options_display
