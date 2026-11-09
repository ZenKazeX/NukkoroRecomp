#pragma once

// Textures drawn in place of the game's own, found by the content the game's raster holds (its level-0 texels and
// its big-endian palette). The modern button sheets (gekko/button_glyphs.h) put their sharp, full-colour versions
// here; the lifted renderer's texture import (scaffold rw_driver build_texture) asks before it imports a paletted
// raster. Process-wide and thread-safe: the read patch adds on the game thread, the import reads on the game thread.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gekko::texture_overrides {

struct Override {
    std::uint32_t width = 0, height = 0;  // multiples of 4
    std::vector<std::uint8_t> gx_rgba8;   // GX RGBA8: 4 x 4 tiles, 16 (A, R) pairs then 16 (G, B) pairs
};

// FNV-1a 64 over the level-0 texels, then the palette bytes as stored (big-endian).
inline std::uint64_t content_key(const std::uint8_t* texels, std::size_t texel_bytes, const std::uint8_t* palette,
                                 std::size_t palette_bytes) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < texel_bytes; ++i) hash = (hash ^ texels[i]) * 0x100000001b3ULL;
    for (std::size_t i = 0; i < palette_bytes; ++i) hash = (hash ^ palette[i]) * 0x100000001b3ULL;
    return hash;
}

// Linear RGBA8 rows (R, G, B, A) to the GX RGBA8 tile layout.
inline std::vector<std::uint8_t> encode_gx_rgba8(const std::uint8_t* rgba, std::uint32_t width,
                                                 std::uint32_t height) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4u);
    std::size_t at = 0;
    for (std::uint32_t ty = 0; ty < height; ty += 4u) {
        for (std::uint32_t tx = 0; tx < width; tx += 4u) {
            for (int half = 0; half < 2; ++half) {
                for (std::uint32_t y = ty; y < ty + 4u; ++y) {
                    for (std::uint32_t x = tx; x < tx + 4u; ++x) {
                        const std::uint8_t* p = rgba + (static_cast<std::size_t>(y) * width + x) * 4u;
                        out[at++] = half == 0 ? p[3] : p[1];
                        out[at++] = half == 0 ? p[0] : p[2];
                    }
                }
            }
        }
    }
    return out;
}

class Registry {
public:
    static Registry& instance() {
        static Registry registry;
        return registry;
    }
    void put(std::uint64_t key, std::shared_ptr<const Override> value) {
        std::lock_guard<std::mutex> lock(mutex_);
        overrides_[key] = std::move(value);
        empty_ = false;
    }
    // Null when nothing is registered for `key`. Cheap while the registry is empty.
    std::shared_ptr<const Override> find(std::uint64_t key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = overrides_.find(key);
        return found != overrides_.end() ? found->second : nullptr;
    }
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return empty_;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<const Override>> overrides_;
    bool empty_ = true;
};

}  // namespace gekko::texture_overrides
