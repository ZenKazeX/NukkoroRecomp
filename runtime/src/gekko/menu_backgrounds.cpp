// Full-height menu backgrounds. Contract: include/gekko/menu_backgrounds.h. The project layout is
// tools/assets/gncp_read.py's (docs/2D_LAYOUT_ARCHITECTURE.md section 1.2).

#include "gekko/menu_backgrounds.h"

#include <bitset>
#include <cstring>
#include <optional>

namespace gekko::menu_backgrounds {

namespace {

constexpr float kOriginalScaleY = 0.94f;
constexpr float kFullScaleY = 1.0f;

u32 bits_of(float f) {
    u32 u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

float float_of(u32 u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// A project: every offset is relative to the "nCPJ" chunk, every value big-endian.
class Project {
public:
    explicit Project(const std::vector<u8>& bytes) : bytes_(bytes) {
        static constexpr char kMagic[] = {'n', 'C', 'P', 'J'};
        for (std::size_t i = 0; i + 4 <= bytes.size(); ++i) {
            if (std::memcmp(bytes.data() + i, kMagic, 4) == 0) {
                base_ = i;
                found_ = true;
                break;
            }
        }
    }
    bool found() const { return found_; }
    bool in(u32 offset, u32 size) const { return base_ + offset + size <= bytes_.size(); }
    u32 u32_at(u32 offset) const {
        if (!in(offset, 4)) return 0;
        const u8* p = bytes_.data() + base_ + offset;
        return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | static_cast<u32>(p[2]) << 8 | p[3];
    }
    float f32_at(u32 offset) const { return float_of(u32_at(offset)); }
    std::string_view name_at(u32 offset) const {
        if (offset == 0 || !in(offset, 1)) return {};
        const char* s = reinterpret_cast<const char*>(bytes_.data() + base_ + offset);
        return std::string_view(s, strnlen(s, bytes_.size() - base_ - offset));
    }
    std::size_t file_offset(u32 offset) const { return base_ + offset; }

    // The descriptor of a scene of the table {count, descriptor array, name array}: the names are {name, index}
    // pairs in alphabetical order, so the index (not the position) selects the descriptor.
    std::optional<u32> scene_in(u32 count, u32 descs, u32 names, std::string_view name) const {
        for (u32 i = 0; i < count && names != 0 && descs != 0; ++i) {
            if (name_at(u32_at(names + 8u * i)) != name) continue;
            const u32 index = u32_at(names + 8u * i + 4u);
            if (index >= count) return std::nullopt;
            return u32_at(descs + 4u * index);
        }
        return std::nullopt;
    }

    // "scene" in the flat table, or "group/scene" in the group table.
    std::optional<u32> scene(std::string_view path) const {
        const std::size_t slash = path.find('/');
        if (slash == std::string_view::npos) return scene_in(u32_at(0x20), u32_at(0x24), u32_at(0x28), path);
        const std::string_view group = path.substr(0, slash), name = path.substr(slash + 1);
        const u32 count = u32_at(0x2C), groups = u32_at(0x30), names = u32_at(0x34);
        for (u32 i = 0; i < count && groups != 0 && names != 0; ++i) {
            if (name_at(u32_at(names + 8u * i)) != group) continue;
            const u32 g = groups + 24u * u32_at(names + 8u * i + 4u);
            return scene_in(u32_at(g), u32_at(g + 4u), u32_at(g + 8u), name);
        }
        return std::nullopt;
    }

private:
    const std::vector<u8>& bytes_;
    std::size_t base_ = 0;
    bool found_ = false;
};

struct Cast {
    u32 record = 0;   // the cast record
    u32 block = 0;    // its authored property block
    u32 mask = 0;     // which property slots the block holds
};

// The cast named `name` that is cast 0 of group 0 of the scene at `desc`.
std::optional<Cast> first_cast(const Project& p, u32 desc, std::string_view name) {
    const u32 names = p.u32_at(desc + 0x30u), name_count = p.u32_at(desc + 0x2Cu);
    bool named = false;
    for (u32 i = 0; i < name_count; ++i) {
        const u32 o = names + 12u * i;
        if (p.u32_at(o + 4u) == 0u && p.u32_at(o + 8u) == 0u) named = p.name_at(p.u32_at(o)) == name;
    }
    if (!named || p.u32_at(desc + 0x24u) == 0u) return std::nullopt;
    const u32 group = p.u32_at(desc + 0x28u);
    if (p.u32_at(group) == 0u) return std::nullopt;
    Cast c;
    c.record = p.u32_at(p.u32_at(group + 4u));
    c.mask = p.u32_at(c.record + 0x2Cu);
    c.block = p.u32_at(c.record + 0x30u);
    if (c.record == 0u || c.block == 0u) return std::nullopt;
    return c;
}

// The offset of property slot `k` in the cast's block (the block holds the slots of its mask, in order).
std::optional<u32> slot(const Cast& c, u32 k) {
    if (((c.mask >> k) & 1u) == 0u) return std::nullopt;
    return c.block + 4u * static_cast<u32>(std::bitset<32>(c.mask & ((1u << k) - 1u)).count());
}

}  // namespace

bool background_scale_y(const std::vector<u8>& gncp, std::string_view scene, float& scale_y) {
    const Project p(gncp);
    if (!p.found()) return false;
    const std::optional<u32> desc = p.scene(scene);
    if (!desc) return false;
    const std::optional<Cast> cast = first_cast(p, *desc, "bg");
    if (!cast) return false;
    const std::optional<u32> sy = slot(*cast, 5u);
    if (!sy || !p.in(*sy, 4)) return false;
    scale_y = p.f32_at(*sy);
    return true;
}

bool fill(const std::vector<u8>& gncp, std::string_view scene, std::vector<u8>& out, std::string& error) {
    const Project p(gncp);
    const std::string where = "the background of scene " + std::string(scene);
    if (!p.found()) {
        error = "no nCPJ chunk";
        return false;
    }
    const std::optional<u32> desc = p.scene(scene);
    if (!desc) {
        error = "no scene " + std::string(scene);
        return false;
    }
    const std::optional<Cast> cast = first_cast(p, *desc, "bg");
    if (!cast) {
        error = "scene " + std::string(scene) + " has no cast bg at group 0, cast 0";
        return false;
    }
    const std::optional<u32> x = slot(*cast, 1u), y = slot(*cast, 2u), sx = slot(*cast, 4u), sy = slot(*cast, 5u);
    if (!x || !y || !sx || !sy || !p.in(*sy, 4)) {
        error = where + " has no authored position and scale";
        return false;
    }
    // The quad's corners about the cast's origin (cast + 0x0C, four normalised (x, y) pairs): the cast must cover the
    // canvas's width and hang from its top edge, so a larger scaleY grows it downward only.
    float left = 1e9f, right = -1e9f, top = 1e9f, bottom = -1e9f;
    for (u32 k = 0; k < 4u; ++k) {
        const float cx = p.f32_at(cast->record + 0x0Cu + 8u * k), cy = p.f32_at(cast->record + 0x10u + 8u * k);
        left = cx < left ? cx : left;
        right = cx > right ? cx : right;
        top = cy < top ? cy : top;
        bottom = cy > bottom ? cy : bottom;
    }
    const float ox = p.f32_at(*x);
    if (p.f32_at(*y) != 0.0f || top != 0.0f || bottom != 1.0f || ox + left != 0.0f || ox + right != 1.0f ||
        p.f32_at(*sx) != 1.0f || p.u32_at(*sy) != bits_of(kOriginalScaleY)) {
        error = where + " is not the retail one (the canvas's width from its top edge, scale 1.0 x 0.94)";
        return false;
    }
    out = gncp;
    const u32 full = bits_of(kFullScaleY);
    const std::size_t at = p.file_offset(*sy);
    out[at] = static_cast<u8>(full >> 24);
    out[at + 1] = static_cast<u8>(full >> 16);
    out[at + 2] = static_cast<u8>(full >> 8);
    out[at + 3] = static_cast<u8>(full);
    return true;
}

}  // namespace gekko::menu_backgrounds
