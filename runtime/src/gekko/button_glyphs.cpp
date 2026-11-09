#include "gekko/button_glyphs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gekko/asset_mount.h"
#include "gekko/gx_texture_decode.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace gekko::button_glyphs {
namespace {

// What a cell means: the GameCube button whose glyph it shows, the Start prompt, or the white highlight mask of
// the vehicle arrow's X button (a silhouette, stored per position like a face cell).
enum class CellKind : u8 { A = 0, B, X, Y, Start, XMask };

struct SheetRow {
    u64 hash;         // FNV-1a 64 of the span: the palette through the end of the texels
    u32 format;       // 8 = C4, 9 = C8
    u32 width, height;
    u32 entries;      // palette entries: 16 or 256
    u32 span_bytes;
    u32 texel_at;     // the texels' offset in the span (after the palette and the u32 data length)
    u32 first_cell, cell_count;
    u32 first_family; // kFamilies row of xbox; playstation and switch follow
};
struct CellRow {
    u32 x0, y0, x1, y1;
    CellKind kind;
};
struct FamilyRow {
    u32 palette_at;   // kBlob: entries x u16 big-endian RGB5A3
    u32 remap_at;     // kBlob: entries bytes, the index each old entry becomes outside the cells
    u32 first_variant;
};
struct FileRow {
    const char* path;
    u32 sheet;
    u32 span_at;
};

#include "button_glyph_data.inc"

constexpr u32 kSheetCount = static_cast<u32>(sizeof(kSheets) / sizeof(kSheets[0]));
static_assert(sizeof(kBlob) == kBlobSize, "button glyph blob size");

u64 fnv1a64(const u8* data, std::size_t size) noexcept {
    u64 hash = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// The byte holding pixel (x, y) of a C4 (8 x 8 tiles) or C8 (8 x 4 tiles) texture, and the nibble's shift
// (C4: the even pixel is the high nibble).
std::size_t texel_byte(u32 format, u32 width, u32 x, u32 y, u32& shift) noexcept {
    const std::size_t tiles_x = width / 8u;
    if (format == 9u) {
        shift = 8u;  // a whole byte
        return ((y / 4u) * tiles_x + x / 8u) * 32u + (y % 4u) * 8u + x % 8u;
    }
    shift = (x % 2u) == 0u ? 4u : 0u;
    return ((y / 8u) * tiles_x + x / 8u) * 32u + (y % 8u) * 4u + (x % 8u) / 2u;
}

// A sheet rectangle on the sharp sheet: start rounded down, end rounded up (make_modern_buttons.py scaled_rect).
void sharp_rect(const CellRow& cell, u32 width, u32 height, u32& x0, u32& y0, u32& x1, u32& y1) noexcept {
    x0 = cell.x0 * kSharpNum / kSharpDen;
    y0 = cell.y0 * kSharpNum / kSharpDen;
    x1 = (std::min(cell.x1, width) * kSharpNum + kSharpDen - 1u) / kSharpDen;
    y1 = (std::min(cell.y1, height) * kSharpNum + kSharpDen - 1u) / kSharpDen;
}

// RGBA8 `src` (w x h) resized to dw x dh: Catmull-Rom, separable, on premultiplied alpha (so a transparent
// texel's colour does not bleed into its neighbours), edges clamped.
std::vector<u8> resize_rgba(const u8* src, u32 w, u32 h, u32 dw, u32 dh) {
    struct Tap {
        u32 at[4];
        float weight[4];
    };
    auto taps = [](u32 from, u32 to) {
        std::vector<Tap> out(to);
        for (u32 i = 0; i < to; ++i) {
            const double centre = (i + 0.5) * from / to - 0.5;
            const double base = std::floor(centre);
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                const double x = std::fabs(centre - (base - 1.0 + k));
                const double v = x < 1.0 ? (1.5 * x - 2.5) * x * x + 1.0
                                         : (x < 2.0 ? ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0 : 0.0);
                const long at = static_cast<long>(base) - 1 + k;
                out[i].at[k] = static_cast<u32>(std::clamp<long>(at, 0, static_cast<long>(from) - 1));
                out[i].weight[k] = static_cast<float>(v);
                sum += v;
            }
            for (float& v : out[i].weight) v = static_cast<float>(v / sum);
        }
        return out;
    };
    std::vector<float> pre(static_cast<std::size_t>(w) * h * 4u);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        const float a = src[i * 4u + 3u] / 255.0f;
        for (u32 c = 0; c < 3u; ++c) pre[i * 4u + c] = src[i * 4u + c] / 255.0f * a;
        pre[i * 4u + 3u] = a;
    }
    const std::vector<Tap> tx = taps(w, dw), ty = taps(h, dh);
    std::vector<float> rows(static_cast<std::size_t>(dw) * h * 4u, 0.0f);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < dw; ++x) {
            for (int k = 0; k < 4; ++k) {
                const float* s = &pre[(static_cast<std::size_t>(y) * w + tx[x].at[k]) * 4u];
                float* d = &rows[(static_cast<std::size_t>(y) * dw + x) * 4u];
                for (u32 c = 0; c < 4u; ++c) d[c] += s[c] * tx[x].weight[k];
            }
        }
    }
    std::vector<u8> out(static_cast<std::size_t>(dw) * dh * 4u);
    for (u32 y = 0; y < dh; ++y) {
        for (u32 x = 0; x < dw; ++x) {
            float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int k = 0; k < 4; ++k) {
                const float* s = &rows[(static_cast<std::size_t>(ty[y].at[k]) * dw + x) * 4u];
                for (u32 c = 0; c < 4u; ++c) v[c] += s[c] * ty[y].weight[k];
            }
            const float a = std::clamp(v[3], 0.0f, 1.0f);
            u8* d = &out[(static_cast<std::size_t>(y) * dw + x) * 4u];
            for (u32 c = 0; c < 3u; ++c) {
                const float straight = a > 0.0f ? v[c] / a : 0.0f;
                d[c] = static_cast<u8>(std::lround(std::clamp(straight, 0.0f, 1.0f) * 255.0f));
            }
            d[3] = static_cast<u8>(std::lround(a * 255.0f));
        }
    }
    return out;
}

u8 position_of_mask(u16 mask, u8 fallback) noexcept {
    switch (mask) {
        case shadow::peripheral::kGamepadSouth: return 0u;
        case shadow::peripheral::kGamepadEast: return 1u;
        case shadow::peripheral::kGamepadWest: return 2u;
        case shadow::peripheral::kGamepadNorth: return 3u;
        default: return fallback;  // not on a face button: the button of the same name
    }
}

}  // namespace

const char* family_name(Family family) noexcept {
    switch (family) {
        case Family::Original: return "original";
        case Family::Xbox: return "xbox";
        case Family::PlayStation: return "playstation";
        case Family::Switch: return "switch";
    }
    return "original";
}

Family family_of_vendors(const std::vector<u16>& vendors) noexcept {
    // A PlayStation or Nintendo pad is usually played through an XInput mapper (DS4Windows, Steam Input), which adds
    // a virtual Xbox pad beside the real one: the real one decides.
    constexpr u16 kSony = 0x054Cu, kNintendo = 0x057Eu;
    if (std::find(vendors.begin(), vendors.end(), kSony) != vendors.end()) return Family::PlayStation;
    if (std::find(vendors.begin(), vendors.end(), kNintendo) != vendors.end()) return Family::Switch;
    return Family::Xbox;
}

std::vector<u16> connected_controller_vendors() {
    std::vector<u16> vendors;
#ifdef _WIN32
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0u || count == 0u) return vendors;
    std::vector<RAWINPUTDEVICELIST> devices(count);
    const UINT got = GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (got == static_cast<UINT>(-1)) return vendors;
    for (UINT i = 0; i < got; ++i) {
        if (devices[i].dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT size = sizeof(info);
        if (GetRawInputDeviceInfoW(devices[i].hDevice, RIDI_DEVICEINFO, &info, &size) == static_cast<UINT>(-1)) {
            continue;
        }
        // Generic desktop page: 4 = joystick, 5 = game pad.
        if (info.hid.usUsagePage == 1u && (info.hid.usUsage == 4u || info.hid.usUsage == 5u)) {
            vendors.push_back(static_cast<u16>(info.hid.dwVendorId));
        }
    }
#endif
    return vendors;
}

void ButtonGlyphs::configure(ButtonStyle style, const shadow::peripheral::GamepadBinding& binding) {
    style_ = style;
    position_of_ = {position_of_mask(binding.a, 0u), position_of_mask(binding.b, 1u),
                    position_of_mask(binding.x, 2u), position_of_mask(binding.y, 3u)};
    for (auto& per_family : patched_) {
        for (auto& span : per_family) span.clear();
    }
    for (auto& per_family : served_) per_family = {};
}

static bool scaled_base(const SheetRow& sheet, u32 gx_format, u32 tlut_format, const std::vector<u8>& original,
                        std::vector<u8>& out);

u32 ButtonGlyphs::attach(assets::AssetSystem& assets) {
    report_ = Report{};
    files_.clear();
    scaled_bases_ = {};
    original_.assign(kSheetCount, {});
    patched_.assign(kSheetCount, {});
    header_.assign(kSheetCount, {});
    served_.assign(kSheetCount, {});
    if (style_ == ButtonStyle::Original) return 0u;
    std::vector<u8> span;
    u8 raster[16] = {};
    for (const FileRow& row : kFiles) {
        const SheetRow& sheet = kSheets[row.sheet];
        const s32 entry = assets.names().resolve(row.path);
        if (entry <= 0) continue;
        const assets::AssetId id = assets.asset_of(static_cast<u32>(entry));
        if (!id.valid()) continue;
        span.assign(sheet.span_bytes, 0u);
        u64 got = 0;
        if (!assets.resources().read(id, row.span_at, sheet.span_bytes, span.data(), got) ||
            got != sheet.span_bytes || fnv1a64(span.data(), span.size()) != sheet.hash) {
            ++report_.mismatched;
            continue;
        }
        if (original_[row.sheet].empty()) {
            // The raster header before the palette: u32 flags, u16 width, u16 height, u8 depth, u8 mip levels,
            // u8 GX format, u8 palette format (what the renderer's content hash folds in).
            got = 0;
            if (row.span_at < 16u || !assets.resources().read(id, row.span_at - 16u, 16u, raster, got) ||
                got != 16u) {
                ++report_.mismatched;
                continue;
            }
            header_[row.sheet] = RasterHeader{raster[10], raster[11], raster[8]};
            original_[row.sheet] = span;
            ++report_.sheets;
        }
        files_[id.value].push_back(Hit{row.sheet, row.span_at});
        ++report_.files;
    }
    if (!files_.empty()) {
        // The scaled art of every sheet found, off the game thread (inputs copied: they never change after this).
        scaled_bases_ = std::async(std::launch::async, [original = original_, header = header_]() {
                            std::vector<std::vector<u8>> made(original.size());
                            for (std::size_t s = 0; s < original.size(); ++s) {
                                if (!original[s].empty() &&
                                    !scaled_base(kSheets[s], header[s].gx_format, header[s].tlut_format, original[s],
                                                 made[s])) {
                                    made[s].clear();
                                }
                            }
                            return made;
                        }).share();
        assets.set_read_patch([this](assets::AssetId id, u64 offset, u8* dest, u64 length) {
            patch(id, offset, dest, length);
        });
    }
    report_.family = family();
    return report_.files;
}

Family ButtonGlyphs::family() {
    switch (style_) {
        case ButtonStyle::Original: return Family::Original;
        case ButtonStyle::Xbox: return Family::Xbox;
        case ButtonStyle::PlayStation: return Family::PlayStation;
        case ButtonStyle::Switch: return Family::Switch;
        case ButtonStyle::Auto: break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!detected_once_ || now >= next_detect_) {
        const Family found = family_of_vendors(connected_controller_vendors());
        if (detected_once_ && found != detected_) ++report_.family_changes;
        detected_ = found;
        detected_once_ = true;
        next_detect_ = now + std::chrono::seconds(1);
    }
    return detected_;
}

const std::vector<u8>& ButtonGlyphs::patched_span(u32 sheet_index, Family family) {
    static const std::vector<u8> kNone;
    if (family == Family::Original || sheet_index >= kSheetCount || sheet_index >= original_.size() ||
        original_[sheet_index].empty()) {
        return kNone;
    }
    std::vector<u8>& out = patched_[sheet_index][static_cast<u32>(family) - 1u];
    if (!out.empty()) return out;

    const SheetRow& sheet = kSheets[sheet_index];
    const FamilyRow& fam = kFamilies[sheet.first_family + static_cast<u32>(family) - 1u];
    out = original_[sheet_index];
    // The palette, then every texel through the remap (entries merged to make room keep their colour).
    std::memcpy(out.data(), kBlob + fam.palette_at, static_cast<std::size_t>(sheet.entries) * 2u);
    const u8* remap = kBlob + fam.remap_at;
    u8* texels = out.data() + sheet.texel_at;
    const std::size_t texel_bytes = sheet.span_bytes - sheet.texel_at;
    for (std::size_t i = 0; i < texel_bytes; ++i) {
        const u8 v = texels[i];
        texels[i] = sheet.format == 9u ? remap[v]
                                       : static_cast<u8>((remap[v >> 4u] << 4u) | (remap[v & 0x0Fu] & 0x0Fu));
    }
    // The cells: each in the variant its meaning and the binding call for.
    u32 variant = fam.first_variant;
    for (u32 c = 0; c < sheet.cell_count; ++c) {
        const CellRow& cell = kCells[sheet.first_cell + c];
        u32 pick = 0u, stored = 4u;
        switch (cell.kind) {
            case CellKind::Start: stored = 1u; break;
            case CellKind::XMask: pick = position_of_[2]; break;
            default: pick = position_of_[static_cast<u32>(cell.kind)]; break;
        }
        const u8* block = kBlob + kVariants[variant + pick];
        variant += stored;
        const u32 w = cell.x1 - cell.x0;
        for (u32 y = cell.y0; y < cell.y1; ++y) {
            for (u32 x = cell.x0; x < cell.x1; ++x) {
                const u8 index = block[(y - cell.y0) * w + (x - cell.x0)];
                u32 shift = 0;
                const std::size_t at = texel_byte(sheet.format, sheet.width, x, y, shift);
                if (shift == 8u) {
                    texels[at] = index;
                } else {
                    texels[at] = static_cast<u8>((texels[at] & ~(0x0Fu << shift)) | ((index & 0x0Fu) << shift));
                }
            }
        }
    }
    return out;
}

// The disc's own art of a sheet (the original span, not the refitted palette), decoded and scaled to the sharp size.
static bool scaled_base(const SheetRow& sheet, u32 gx_format, u32 tlut_format, const std::vector<u8>& original,
                 std::vector<u8>& out) {
    std::vector<u8> base(static_cast<std::size_t>(sheet.width) * sheet.height * 4u);
    if (!gx_decode_texture(gx_format, sheet.width, sheet.height, original.data() + sheet.texel_at,
                           sheet.span_bytes - sheet.texel_at, original.data(), tlut_format, sheet.entries,
                           base.data())) {
        return false;
    }
    out = resize_rgba(base.data(), sheet.width, sheet.height, sheet.width * kSharpNum / kSharpDen,
                      sheet.height * kSharpNum / kSharpDen);
    return true;
}

bool ButtonGlyphs::sharp_sheet(u32 sheet_index, Family family, SharpSheet& out) {
    const std::vector<u8>& span = patched_span(sheet_index, family);
    if (span.empty()) return false;
    const SheetRow& sheet = kSheets[sheet_index];
    const RasterHeader& header = header_[sheet_index];
    out = SharpSheet{};
    out.gx_format = header.gx_format;
    out.tlut_format = header.tlut_format;
    out.depth = header.depth;
    out.width = sheet.width;
    out.height = sheet.height;
    out.palette = span.data();
    out.palette_bytes = static_cast<std::size_t>(sheet.entries) * 2u;
    out.texels = span.data() + sheet.texel_at;
    out.texel_bytes = sheet.span_bytes - sheet.texel_at;

    // Everything but the cells: the disc's own art, scaled - made by attach()'s worker (waited for if it is still
    // running), or here when there is none.
    out.sharp_width = sheet.width * kSharpNum / kSharpDen;
    out.sharp_height = sheet.height * kSharpNum / kSharpDen;
    const std::vector<std::vector<u8>>* made = scaled_bases_.valid() ? &scaled_bases_.get() : nullptr;
    if (made != nullptr && sheet_index < made->size() && !(*made)[sheet_index].empty()) {
        out.rgba8 = (*made)[sheet_index];
    } else if (!scaled_base(sheet, header.gx_format, header.tlut_format, original_[sheet_index], out.rgba8)) {
        return false;
    }

    // The cells, each in the variant patched_span chose.
    const FamilyRow& fam = kFamilies[sheet.first_family + static_cast<u32>(family) - 1u];
    u32 variant = fam.first_variant;
    for (u32 c = 0; c < sheet.cell_count; ++c) {
        const CellRow& cell = kCells[sheet.first_cell + c];
        u32 pick = 0u, stored = 4u;
        switch (cell.kind) {
            case CellKind::Start: stored = 1u; break;
            case CellKind::XMask: pick = position_of_[2]; break;
            default: pick = position_of_[static_cast<u32>(cell.kind)]; break;
        }
        const u8* block = kBlob + kSharpVariants[variant + pick];
        variant += stored;
        u32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        sharp_rect(cell, sheet.width, sheet.height, x0, y0, x1, y1);
        const std::size_t row_bytes = static_cast<std::size_t>(x1 - x0) * 4u;
        for (u32 y = y0; y < y1; ++y) {
            std::memcpy(out.rgba8.data() + (static_cast<std::size_t>(y) * out.sharp_width + x0) * 4u,
                        block + (y - y0) * row_bytes, row_bytes);
        }
    }
    return true;
}

bool ButtonGlyphs::table_sheet_of(const char* path, u32& sheet, u64& span_at) {
    for (const FileRow& row : kFiles) {
        if (std::strcmp(row.path, path) == 0) {
            sheet = row.sheet;
            span_at = row.span_at;
            return true;
        }
    }
    return false;
}

void ButtonGlyphs::patch(assets::AssetId id, u64 offset, u8* dest, u64 length) {
    const auto found = files_.find(id.value);
    if (found == files_.end() || length == 0u) return;
    const Family now = family();
    report_.family = now;
    if (now == Family::Original) return;
    bool touched = false;
    for (const Hit& hit : found->second) {
        const std::vector<u8>& span = patched_span(hit.sheet, now);
        if (span.empty()) continue;
        const u64 begin = std::max(offset, hit.at);
        const u64 end = std::min(offset + length, hit.at + span.size());
        if (begin >= end) continue;
        std::memcpy(dest + (begin - offset), span.data() + (begin - hit.at), static_cast<std::size_t>(end - begin));
        touched = true;
        // The sharp version, once per sheet and family (and binding): the renderer finds it by the content above.
        bool& served = served_[hit.sheet][static_cast<u32>(now) - 1u];
        if (!served && sharp_sink_) {
            SharpSheet sharp;
            if (sharp_sheet(hit.sheet, now, sharp) && sharp_sink_(std::move(sharp))) {
                served = true;
                ++report_.sharp_served;
            }
        }
    }
    if (touched) ++report_.patched_reads;
}

}  // namespace gekko::button_glyphs
