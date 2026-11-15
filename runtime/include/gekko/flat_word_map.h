#pragma once

// A map from a non-zero 32-bit word to a 32-bit value: open addressing over two flat arrays (power-of-two size,
// linear probing, key 0 = empty slot). For a lookup that runs once per draw or per call, where the node-based
// std::unordered_map's hash and pointer chase were measurable. Insertion order does not matter; the later set() of a
// word replaces its value (the behaviour of the map's operator[] that it replaces). Entries are never erased:
// rebuild with clear() / reserve().

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gekko {

class FlatWordMap {
public:
    void clear() noexcept {
        key_.clear();
        value_.clear();
        mask_ = 0;
    }
    // Room for `count` distinct words (the table is at least twice that, so every probe ends).
    void reserve(std::size_t count) {
        std::size_t size = 16;
        while (size < count * 2u) size <<= 1;
        key_.assign(size, 0u);
        value_.assign(size, 0u);
        mask_ = static_cast<std::uint32_t>(size - 1u);
    }
    // `word` must not be 0 and fewer than half of the slots may be in use (reserve() with the final count first).
    void set(std::uint32_t word, std::uint32_t value) noexcept {
        for (std::uint32_t i = slot_of(word) & mask_;; i = (i + 1u) & mask_) {
            if (key_[i] == 0u || key_[i] == word) {
                key_[i] = word;
                value_[i] = value;
                return;
            }
        }
    }
    const std::uint32_t* find(std::uint32_t word) const noexcept {
        if (key_.empty()) return nullptr;
        for (std::uint32_t i = slot_of(word) & mask_;; i = (i + 1u) & mask_) {
            if (key_[i] == word) return &value_[i];
            if (key_[i] == 0u) return nullptr;
        }
    }

private:
    static std::uint32_t slot_of(std::uint32_t word) noexcept { return ((word >> 2) * 2654435761u) >> 7; }
    std::vector<std::uint32_t> key_, value_;
    std::uint32_t mask_ = 0;
};

}  // namespace gekko
