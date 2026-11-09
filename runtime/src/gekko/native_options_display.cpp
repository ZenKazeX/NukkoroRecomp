// The Options menu's Display tile. Contract: include/gekko/native_options_display.h.

#include "gekko/native_options_display.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

#include "assets/chunk_stream.h"
#include "assets/texture_dictionary.h"
#include "assets/texture_import.h"
#include "gekko/cpu.h"
#include "gekko/game_clock.h"
#include "gekko/native_save_text.h"

namespace gekko::options_display {

namespace {

#include "options_display_tile.inc"

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

struct Presets {
    std::vector<Preset> list;
    FamilyOf family_of = nullptr;
};
Presets& presets() {
    static Presets p;
    return p;
}

std::u16string ascii16(std::string_view s) { return std::u16string(s.begin(), s.end()); }

constexpr const char* kRatios[] = {nullptr, "4:3", "5:4", "16:9", "16:10", "21:9", "32:10", "32:9"};
constexpr u32 kRatioCount = 8u;
constexpr u32 kPercents[] = {50u, 67u, 75u, 100u, 125u, 150u, 200u, 250u, 300u, 400u};
constexpr double kRates[] = {30.0, 60.0, 120.0, 144.0, 165.0, 240.0};
constexpr u32 kMaxRenderSide = 16384u;

const Texts kTexts[6] = {
    {"Change the screen and picture settings.",
     {"Display mode", "Screen ratio", "Resolution", "Render resolution", "Frame rate", "V-Sync", "Frame blending",
      "Draw distance", "FPS counter"},
     "Windowed", "Fullscreen", "Auto", "Unlocked", "Original", "Maximum"},
    {"Modifier les param\xC3\xA8tres de l'\xC3\xA9" "cran et de l'image.",
     {"Mode d'affichage", "Format d'\xC3\xA9" "cran", "R\xC3\xA9solution", "R\xC3\xA9solution de rendu",
      "Fr\xC3\xA9quence d'images", "V-Sync", "Fusion d'images", "Distance d'affichage", "Compteur FPS"},
     "Fen\xC3\xAAtr\xC3\xA9", "Plein \xC3\xA9" "cran", "Auto", "Illimit\xC3\xA9", "Original", "Maximum"},
    {"\xC3\x84ndere die Bildschirm- und Bildeinstellungen.",
     {"Anzeigemodus", "Bildformat", "Aufl\xC3\xB6sung", "Renderaufl\xC3\xB6sung", "Bildrate", "V-Sync",
      "Bildmischung", "Sichtweite", "FPS-Anzeige"},
     "Fenster", "Vollbild", "Auto", "Unbegrenzt", "Original", "Maximum"},
    {"Cambia i parametri dello schermo e dell'immagine.",
     {"Modalit\xC3\xA0 schermo", "Formato schermo", "Risoluzione", "Risoluzione render", "Frequenza fotogrammi",
      "V-Sync", "Fusione fotogrammi", "Distanza visiva", "Contatore FPS"},
     "Finestra", "Schermo intero", "Auto", "Sbloccato", "Originale", "Massimo"},
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
      "FPS\xE8\xA1\xA8\xE7\xA4\xBA"},
     "\xE3\x82\xA6\xE3\x82\xA3\xE3\x83\xB3\xE3\x83\x89\xE3\x82\xA6",
     "\xE3\x83\x95\xE3\x83\xAB\xE3\x82\xB9\xE3\x82\xAF\xE3\x83\xAA\xE3\x83\xBC\xE3\x83\xB3",
     "\xE8\x87\xAA\xE5\x8B\x95", "\xE7\x84\xA1\xE5\x88\xB6\xE9\x99\x90",
     "\xE3\x82\xAA\xE3\x83\xAA\xE3\x82\xB8\xE3\x83\x8A\xE3\x83\xAB", "\xE6\x9C\x80\xE5\xA4\xA7"},
    {"Cambiar la configuraci\xC3\xB3n de la pantalla y la imagen.",
     {"Modo de pantalla", "Proporci\xC3\xB3n", "Resoluci\xC3\xB3n", "Resoluci\xC3\xB3n de render", "Tasa de cuadros",
      "V-Sync", "Mezcla de cuadros", "Distancia de visi\xC3\xB3n", "Contador FPS"},
     "Ventana", "Pantalla completa", "Auto", "Desbloqueado", "Original", "M\xC3\xA1ximo"},
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

void set_resolution_presets(std::vector<Preset> list, FamilyOf family_of) {
    presets().list = std::move(list);
    presets().family_of = family_of;
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
// The model

std::vector<Model::Size> Model::resolutions() const {
    const char* family = kRatios[value[kScreenRatio] % kRatioCount];
    std::vector<Size> out;
    for (const Preset& p : presets().list)
        if (family == nullptr || std::strcmp(p.family, family) == 0) out.push_back({p.width, p.height});
    if (custom.width != 0u) {
        const char* own = presets().family_of != nullptr ? presets().family_of(custom.width, custom.height) : "other";
        if (family == nullptr || std::strcmp(own, family) == 0) out.push_back(custom);
    }
    std::sort(out.begin(), out.end(), [](const Size& a, const Size& b) {
        const u64 aa = static_cast<u64>(a.width) * a.height, bb = static_cast<u64>(b.width) * b.height;
        return aa != bb ? aa < bb : a.width < b.width;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Size& a, const Size& b) { return a.width == b.width && a.height == b.height; }),
              out.end());
    return out;
}

std::vector<u32> Model::render_percents() const {
    const std::vector<Size> sizes = resolutions();
    const Size s = sizes.empty() ? Size{1280u, 720u} : sizes[std::min<std::size_t>(value[kResolution], sizes.size() - 1)];
    std::vector<u32> out;
    for (const u32 p : kPercents) {
        const u32 w = static_cast<u32>(std::lround(s.width * p / 100.0)), h = static_cast<u32>(std::lround(s.height * p / 100.0));
        if (w <= kMaxRenderSide && h <= kMaxRenderSide) out.push_back(p);
    }
    return out;
}

u32 Model::count(u32 row) const {
    switch (row) {
    case kDisplayMode: return 2u;
    case kScreenRatio: return kRatioCount;
    case kResolution: return static_cast<u32>(resolutions().size());
    case kRenderResolution: return static_cast<u32>(render_percents().size());
    case kFrameRate: return static_cast<u32>(rates.size());
    case kVsync: case kFrameBlending: case kFpsCounter: return 2u;
    case kDrawDistance: return 8u;
    default: return 0u;
    }
}

Model Model::from(const DisplaySettings& s) {
    Model m;
    m.value[kDisplayMode] = s.fullscreen ? 1u : 0u;
    m.value[kScreenRatio] = 0u;
    const u32 w = static_cast<u32>(std::max(1, s.window_width)), h = static_cast<u32>(std::max(1, s.window_height));
    bool preset = false;
    for (const Preset& p : presets().list) preset = preset || (p.width == w && p.height == h);
    if (!preset) m.custom = {w, h};
    const std::vector<Size> sizes = m.resolutions();
    for (u32 i = 0; i < sizes.size(); ++i)
        if (sizes[i].width == w && sizes[i].height == h) m.value[kResolution] = i;
    // The render size as a percentage of the window (100 % when it follows the window or is no listed percentage).
    const std::vector<u32> percents = m.render_percents();
    u32 percent = 100u;
    if (s.render_width > 0 && s.render_height > 0) {
        for (const u32 p : percents)
            if (static_cast<int>(std::lround(w * p / 100.0)) == s.render_width &&
                static_cast<int>(std::lround(h * p / 100.0)) == s.render_height)
                percent = p;
    }
    for (u32 i = 0; i < percents.size(); ++i)
        if (percents[i] == percent) m.value[kRenderResolution] = i;
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
    return m;
}

void Model::step(u32 row, int delta) {
    if (row >= kRowCount) return;
    // The resolution and render percentage are kept by size and percentage across a ratio / resolution change.
    const std::vector<Size> before = resolutions();
    const Size size = before.empty() ? Size{0, 0} : before[std::min<std::size_t>(value[kResolution], before.size() - 1)];
    const std::vector<u32> percents_before = render_percents();
    const u32 percent = percents_before.empty() ? 100u
                                                : percents_before[std::min<std::size_t>(value[kRenderResolution],
                                                                                        percents_before.size() - 1)];
    const u32 n = count(row);
    if (n == 0u) return;
    const s64 next = std::clamp<s64>(static_cast<s64>(value[row]) + delta, 0, static_cast<s64>(n) - 1);
    value[row] = static_cast<u32>(next);
    if (row == kScreenRatio) {
        const std::vector<Size> sizes = resolutions();
        u32 best = 0;
        u64 best_gap = ~0ull;
        for (u32 i = 0; i < sizes.size(); ++i) {
            if (sizes[i].width == size.width && sizes[i].height == size.height) {
                best = i;
                best_gap = 0;
                break;
            }
            const u64 a = static_cast<u64>(sizes[i].width) * sizes[i].height, b = static_cast<u64>(size.width) * size.height;
            const u64 gap = a > b ? a - b : b - a;
            if (gap < best_gap) best = i, best_gap = gap;
        }
        value[kResolution] = best;
    }
    if (row == kScreenRatio || row == kResolution) {
        const std::vector<u32> percents = render_percents();
        u32 index = 0;
        for (u32 i = 0; i < percents.size(); ++i)
            if (percents[i] <= percent) index = i;
        value[kRenderResolution] = index;
    }
}

std::u16string Model::value_text(u32 row, const Texts& t, const std::u16string& on,
                                 const std::u16string& off) const {
    switch (row) {
    case kDisplayMode: return utf16(value[row] != 0u ? t.fullscreen : t.windowed);
    case kScreenRatio: return value[row] == 0u ? utf16(t.automatic) : ascii16(kRatios[value[row] % kRatioCount]);
    case kResolution: {
        const std::vector<Size> sizes = resolutions();
        if (sizes.empty()) return {};
        const Size s = sizes[std::min<std::size_t>(value[row], sizes.size() - 1)];
        return ascii16(std::to_string(s.width) + " x " + std::to_string(s.height));
    }
    case kRenderResolution: {
        const std::vector<u32> percents = render_percents();
        if (percents.empty()) return {};
        return ascii16(std::to_string(percents[std::min<std::size_t>(value[row], percents.size() - 1)]) + "%");
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
    DisplaySettings s = base;
    s.fullscreen = value[kDisplayMode] != 0u;
    const std::vector<Size> sizes = resolutions();
    if (!sizes.empty()) {
        const Size size = sizes[std::min<std::size_t>(value[kResolution], sizes.size() - 1)];
        s.window_width = static_cast<int>(size.width);
        s.window_height = static_cast<int>(size.height);
        const std::vector<u32> percents = render_percents();
        const u32 percent = percents.empty() ? 100u
                                             : percents[std::min<std::size_t>(value[kRenderResolution], percents.size() - 1)];
        if (percent == 100u) {
            s.render_width = s.render_height = 0;  // follows the window
        } else {
            s.render_width = static_cast<int>(std::lround(size.width * percent / 100.0));
            s.render_height = static_cast<int>(std::lround(size.height * percent / 100.0));
        }
    }
    if (!rates.empty()) s.render_rate = rates[std::min<std::size_t>(value[kFrameRate], rates.size() - 1)];
    s.vsync = value[kVsync] == 0u;
    s.frame_interpolation = value[kFrameBlending] != 0u;
    s.draw_distance = static_cast<float>(value[kDrawDistance] + 1u);
    s.fps_counter = value[kFpsCounter] != 0u;
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
// The tile

void Tile::set_font_files(Language language, std::vector<u8> txd, std::vector<std::vector<u8>> glyph_lists,
                          std::u16string on, std::u16string off) {
    FontFiles& f = files_[static_cast<u32>(language) % 6u];
    f.txd = std::move(txd);
    f.glyph_lists = std::move(glyph_lists);
    f.on = std::move(on);
    f.off = std::move(off);
}

u32 Tile::current_item(Cpu& cpu) const {
    const u32 row = cpu.mem_read32(kCursor), column = cpu.mem_read32(kCursor + 4u);
    if (row >= 3u || column >= 2u) return ~0u;
    return cpu.mem_read32(kGrid + row * 8u + column * 4u);
}

bool Tile::setup(Cpu& cpu) {
    cpu.mem_write8(cpu.gpr[3] + kHiddenFlag, 0u);
    ++report_.setups;
    open_ = false;
    return false;
}

bool Tile::page(Cpu&) {
    page_ran_ = true;
    return false;
}

bool Tile::draw_rows(Cpu& cpu) {
    if (cpu.gpr[4] != kItem) {
        hide(cpu);  // the panel shows another tile's rows from this frame on
        return false;
    }
    const u32 impl = cpu.gpr[3];
    ++report_.row_draws;
    const bool focused = page_ran_;
    page_ran_ = false;
    if (cpu.mem_read8(impl + kTextOn) == 0u) {  // the game draws no text now either
        hide(cpu);
        return true;
    }
    if (!focused) {
        open_ = false;
        const DisplaySettings now = display_settings().current();
        model_ = applied_ = Model::from(now);
        cursor_ = scroll_ = 0u;
    } else if (!open_) {
        open_ = true;
        model_ = applied_ = Model::from(display_settings().current());
        cursor_ = scroll_ = 0u;
    }
    publish(cpu, focused);
    return true;
}

bool Tile::repeat_press(Cpu& cpu) {
    if (cpu.lr != kPageUpQuery && cpu.lr != kPageDownQuery) return false;
    if (current_item(cpu) != kItem) return false;
    if (cpu.lr == kPageUpQuery) page_input(cpu, cpu.gpr[31]);
    cpu.gpr[3] = 0u;  // the game's own rows (the voice language) never move
    return true;
}

bool Tile::edge_press(Cpu& cpu) {
    if (cpu.lr != kPageAQuery || current_item(cpu) != kItem) return false;
    if (open_ && button_pressed(cpu, kButtonA)) apply(cpu);
    cpu.gpr[3] = 0u;  // the voice language's save path never runs for this tile
    return true;
}

// Called where the page handler asks for "up" (r31 = impl): the game's countdown has run out.
void Tile::page_input(Cpu& cpu, u32 impl) {
    if (!open_) return;
    int move = 0, change = 0;
    if (button_repeats(cpu, kButtonUp)) move = -1;
    else if (button_repeats(cpu, kButtonDown)) move = 1;
    else if (button_repeats(cpu, kButtonLeft)) change = -1;
    else if (button_repeats(cpu, kButtonRight)) change = 1;
    if (move != 0) {
        const s32 next = static_cast<s32>(cursor_) + move;
        if (next < 0 || next >= static_cast<s32>(kShownCount)) return;  // the game's lists stop at their ends
        cursor_ = static_cast<u32>(next);
        if (cursor_ < scroll_) scroll_ = cursor_;
        if (cursor_ >= scroll_ + kVisibleRows) scroll_ = cursor_ - kVisibleRows + 1u;
        ++report_.moves;
    } else if (change != 0) {
        const u32 row = kShownRows[cursor_];
        const u32 before = model_.value[row];
        model_.step(row, change);
        if (model_.value[row] == before) return;
        ++report_.changes;
    } else {
        return;
    }
    cpu.mem_write32(impl + kRepeatCountdown, 2u);  // as the game's own moves
    sound(cpu, kSoundCursor);
    publish(cpu, true);
}

void Tile::apply(Cpu& cpu) {
    const DisplaySettings before = display_settings().current();
    const DisplaySettings next = model_.settings(before);
    std::string error;
    if (next != before) {
        if (!display_settings().apply(next, &error)) {
            ++report_.apply_failures;
            last_error_ = error;
            return;
        }
        ++report_.applies;
        if (display_settings().save(&error)) {
            ++report_.saves;
        } else {
            ++report_.save_failures;
            last_error_ = error;
        }
    }
    model_ = applied_ = Model::from(display_settings().current());
    sound(cpu, kSoundDecide);
    publish(cpu, true);
}

void Tile::sound(Cpu& cpu, u32 id) {
    if (sound_ != nullptr) sound_(sound_context_, cpu, id);
}

bool Tile::ensure_font() {
    if (font_ready_ || font_failed_) return font_ready_;
    Font font;
    font.language = language_;
    // The language's sheets first, then English's for anything they lack (digits and Latin in the JP sheets).
    std::vector<Language> order{language_};
    if (language_ != Language::EN) order.push_back(Language::EN);
    std::set<char16_t> seen;
    for (const Language l : order) {
        const FontFiles& f = files_[static_cast<u32>(l)];
        if (f.txd.empty()) continue;
        namespace assets = shadow::assets;
        const assets::ByteView bytes{f.txd.data(), f.txd.size()};
        std::vector<assets::Chunk> top;
        if (!assets::read_stream(bytes, &top)) continue;
        const assets::Chunk* dict = assets::find(top, assets::chunk::kTextureDictionary);
        assets::TextureDictionary dictionary;
        std::string error;
        if (dict == nullptr || !assets::parse_texture_dictionary(bytes, *dict, &dictionary, &error)) continue;
        for (const std::vector<u8>& met : f.glyph_lists) {
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
            // The sheet as stored: grey glyphs on black, opaque. The game blends it by its colour (the overlay's
            // two passes), never by alpha.
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
    const FontFiles& own = files_[static_cast<u32>(language_)];
    on_off_[0] = own.on.empty() ? files_[0].on : own.on;
    on_off_[1] = own.off.empty() ? files_[0].off : own.off;
    if (font.glyphs.empty()) {
        font_failed_ = true;
        ++report_.font_failures;
        last_error_ = "the front-end glyph sheets could not be read";
        return false;
    }
    std::sort(font.glyphs.begin(), font.glyphs.end(),
              [](const std::pair<char16_t, Glyph>& a, const std::pair<char16_t, Glyph>& b) { return a.first < b.first; });
    std::lock_guard<std::mutex> lock(shared().mutex);
    font.serial = shared().font.serial + 1u;
    shared().font = std::move(font);
    font_ready_ = true;
    return true;
}

void Tile::hide(Cpu& cpu) {
    Page page;
    page.game_ticks = cpu.game_clock().now();
    std::lock_guard<std::mutex> lock(shared().mutex);
    const Page& last = shared().pages[(shared().next + Shared::kHistory - 1u) % Shared::kHistory];
    if (!last.visible) return;  // already hidden
    shared().push(std::move(page));
}

void Tile::publish(Cpu& cpu, bool focused) {
    if (!ensure_font()) return;
    const Texts& t = texts(language_);
    Page page;
    page.visible = true;
    page.focused = focused;
    page.cursor = cursor_;
    page.scroll = scroll_;
    page.game_ticks = cpu.game_clock().now();
    for (const u32 row : kShownRows) {
        Page::Line line;
        line.label = utf16(t.labels[row]);
        line.value = model_.value_text(row, t, on_off_[0], on_off_[1]);
        page.lines.push_back(std::move(line));
    }
    std::lock_guard<std::mutex> lock(shared().mutex);
    shared().push(std::move(page));
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
    // Cells 46 (icn_audio, normal) and 52 (selected): u 0.162109375 / 0.486328125, v 0.3203125, 82 x 82.
    const struct {
        u32 x, y;
        const unsigned char* art;
    } cells[] = {{83u, 164u, kTileNormal}, {249u, 164u, kTileSelected}};
    for (const auto& cell : cells) {
        for (u32 y = 0; y < kTileSide; ++y) {
            for (u32 x = 0; x < kTileSide; ++x) {
                const u32 tx = cell.x + x, ty = cell.y + y;
                const std::size_t at = (static_cast<std::size_t>(ty / 4u) * tiles_x + tx / 4u) * 32u +
                                       ((ty % 4u) * 4u + tx % 4u) * 2u;
                const u16 v = rgb5a3(cell.art + (static_cast<std::size_t>(y) * kTileSide + x) * 4u);
                texels[at] = static_cast<u8>(v >> 8);
                texels[at + 1] = static_cast<u8>(v & 0xFFu);
            }
        }
    }
    return true;
}

bool reword_help(const std::vector<u8>& table, const std::vector<std::vector<u8>>& glyph_lists, Language language,
                 std::vector<u8>& out, std::string& error) {
    const std::u16string help = utf16(texts(language).help);
    std::set<u32> codes;
    for (const std::vector<u8>& met : glyph_lists) {
        GlyphList list;
        if (!parse_glyph_list(met, list)) continue;
        for (const GlyphList::Entry& e : list.entries) codes.insert(e.code);
    }
    for (const char16_t unit : help) {
        if (unit != u' ' && codes.count(unit) == 0u) {
            error = std::string("the Display help line uses a glyph the ") +
                    std::string(kLanguageSuffix[static_cast<u32>(language)]) + " sheets lack: U+" +
                    std::to_string(static_cast<u32>(unit));
            return false;
        }
    }
    return save_text::replace_texts(table, {{0u, help}}, out, error);
}

}  // namespace gekko::options_display
