// gekko::FlatWordMap against std::unordered_map (the container it replaced in the RenderWare driver's texture index).
#include "gekko/flat_word_map.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <unordered_map>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// Every key of `reference` finds its value; `absent` keys find nothing.
bool agrees(const gekko::FlatWordMap& map, const std::unordered_map<std::uint32_t, std::uint32_t>& reference,
            const std::vector<std::uint32_t>& absent) {
    for (const auto& [word, value] : reference) {
        const std::uint32_t* found = map.find(word);
        if (found == nullptr || *found != value) return false;
    }
    for (const std::uint32_t word : absent)
        if (reference.count(word) == 0 && map.find(word) != nullptr) return false;
    return true;
}
}  // namespace

int main() {
    {
        gekko::FlatWordMap empty;  // never reserved: every lookup is a miss
        check(empty.find(0x80001234u) == nullptr, "an unreserved map finds nothing");
    }
    std::mt19937 rng(7);
    for (int round = 0; round < 400; ++round) {
        const std::size_t count = 1 + rng() % 300;
        std::unordered_map<std::uint32_t, std::uint32_t> reference;
        std::vector<std::uint32_t> words;
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t word;
            switch (round % 4) {
                case 0: word = 0x80000000u + (rng() % 5000u) * 4u; break;       // dense guest words
                case 1: word = (rng() % 64u + 1u) * 0x10000u + (rng() % 4) * 4u; break;  // few distinct high bits
                case 2: word = 0xFFFFFFFCu - (rng() % 400u) * 4u; break;        // the top of the range
                default: word = (rng() | 1u) & ~3u; if (word == 0u) word = 4u; break;
            }
            words.push_back(word);
        }
        gekko::FlatWordMap map;
        map.reserve(words.size());
        for (const std::uint32_t word : words) {
            const std::uint32_t value = static_cast<std::uint32_t>(rng());
            map.set(word, value);          // a repeated word: the later value wins
            reference[word] = value;
        }
        std::vector<std::uint32_t> absent;
        for (int i = 0; i < 400; ++i) absent.push_back(static_cast<std::uint32_t>(rng()) & ~3u);
        check(agrees(map, reference, absent), "random set agrees with unordered_map");
        // Negative control: change one stored value behind the reference's back; the comparison must notice.
        if (round == 0) {
            map.set(words.front(), reference[words.front()] ^ 1u);
            check(!agrees(map, reference, absent), "negative control: a changed value is caught");
        }
        map.clear();
        check(map.find(words.front()) == nullptr, "a cleared map finds nothing");
    }
    std::puts(failures == 0 ? "flat_word_map_tests: ok" : "flat_word_map_tests: FAILED");
    return failures == 0 ? 0 : 1;
}
