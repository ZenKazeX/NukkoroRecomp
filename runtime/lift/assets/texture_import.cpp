#include "assets/texture_import.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "assets/texel_decode.h"

namespace shadow::assets {

namespace {

// sRGB transfer: the texels are display-referred colours; averaging them is done on light, not on code values.
const std::array<float, 256>& decode_table() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            t[static_cast<std::size_t>(i)] =
                static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
        return t;
    }();
    return table;
}

std::uint8_t encode(float linear) {
    const double l = std::clamp(static_cast<double>(linear), 0.0, 1.0);
    const double c = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(std::lround(c * 255.0));
}

}  // namespace

void generate_mip_chain(TextureImage& image) {
    const std::array<float, 256>& to_linear = decode_table();
    std::uint32_t level = image.mip_levels - 1;
    while (image.level_width(level) > 1 || image.level_height(level) > 1) {
        const std::uint32_t sw = image.level_width(level);
        const std::uint32_t sh = image.level_height(level);
        const std::uint32_t dw = std::max<std::uint32_t>(sw / 2, 1);
        const std::uint32_t dh = std::max<std::uint32_t>(sh / 2, 1);
        const std::size_t source_offset = image.level_offsets[level];
        const std::size_t offset = image.rgba.size();
        image.rgba.resize(offset + static_cast<std::size_t>(dw) * dh * 4);
        const std::uint8_t* src = image.rgba.data() + source_offset;
        std::uint8_t* dst = image.rgba.data() + offset;
        for (std::uint32_t y = 0; y < dh; ++y) {
            for (std::uint32_t x = 0; x < dw; ++x) {
                double colour[3] = {0, 0, 0};
                double plain[3] = {0, 0, 0};
                double alpha = 0;
                int count = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy) {
                    for (std::uint32_t dx = 0; dx < 2; ++dx) {
                        const std::uint32_t sx = std::min(x * 2 + dx, sw - 1);
                        const std::uint32_t sy = std::min(y * 2 + dy, sh - 1);
                        const std::uint8_t* t = src + (static_cast<std::size_t>(sy) * sw + sx) * 4;
                        const double a = t[3] / 255.0;
                        for (int c = 0; c < 3; ++c) {
                            colour[c] += to_linear[t[c]] * a;
                            plain[c] += to_linear[t[c]];
                        }
                        alpha += a;
                        ++count;
                    }
                }
                std::uint8_t* out = dst + (static_cast<std::size_t>(y) * dw + x) * 4;
                for (int c = 0; c < 3; ++c) {
                    const double value = alpha > 0 ? colour[c] / alpha : plain[c] / count;
                    out[c] = encode(static_cast<float>(value));
                }
                out[3] = static_cast<std::uint8_t>(std::lround(alpha / count * 255.0));
            }
        }
        image.level_offsets.push_back(offset);
        ++image.mip_levels;
        ++level;
    }
}

bool import_texture(const TextureRecord& record, const ImportOptions& options, TextureImage* out, std::string* error) {
    *out = TextureImage{};
    if (record.width == 0 || record.height == 0) {
        if (error != nullptr)
            *error = "texture '" + record.name + "' has no size";
        return false;
    }
    out->width = record.width;
    out->height = record.height;
    out->mip_levels = 1;
    out->rgba.resize(static_cast<std::size_t>(record.width) * record.height * 4);
    out->level_offsets.push_back(0);
    const std::size_t level0 = level_bytes(record.encoding, record.width, record.height);
    if (!decode_level(record.encoding, record.width, record.height, record.texels.slice(0, level0), record.palette,
                      record.palette_encoding, out->rgba.data(), error))
        return false;
    for (std::size_t i = 3; i < out->rgba.size(); i += 4) {
        if (out->rgba[i] != 255) {
            out->texel_alpha = true;
            break;
        }
    }
    if (options.generate_mips)
        generate_mip_chain(*out);
    return true;
}

}  // namespace shadow::assets
