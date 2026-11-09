#include "gekko/native_assets.h"

// SEGA PRS decompression, transcribed from the guest decompressor at
// 0x80043FE0 and verified against the whole-disc census.
//
// It lives in its own translation unit because it is a CODEC and nothing else.
// It used to sit inside the native texture manager, which meant that linking
// the decompressor also linked an executor, a memory system and a GX texture
// decoder - so an asset provider that wanted only to expand a compressed
// archive member could not do so without the whole graphics stack.
//
// The bitstream, as recovered: flag bits are LSB-first, eight per flag byte,
// and the first flag byte is read before the first bit. A set bit is a
// literal. A clear bit selects a match: the short form takes two MSB-first
// length bits plus 2, with distance = byte - 256; the long form reads a
// 16-bit little-endian word, with distance = (v >> 3) - 8192 and length
// (v & 7) + 2, or the next byte + 1 when those three bits are zero. A zero
// word terminates the stream. Matches may overlap, so the copy is byte-wise
// and must not be turned into a memcpy.

#include <cstddef>
#include <vector>

// A note for anyone reading the file comment above: this include pulls in the
// profiling vocabulary (and, with Tracy enabled, windows.h behind it), which is
// the one thing in here that is not the codec. It is the only way to price the
// expansion, and it compiles to nothing in the shipped build.
#include "gekko/profiling.h"

namespace gekko {

// --- PRS ---------------------------------------------------------------------

template<class A>
bool prs_expand_impl(const u8* src, std::size_t src_size, std::size_t expected_size,
                     std::vector<u8, A>& out) {
    // Per STREAM - never per byte or per match, which is the only rate hazard
    // this codec could pose. It is the most CPU-expensive thing the asset
    // pipeline does: a byte-at-a-time bitstream with overlapping copies that
    // cannot be vectorised. Each template instantiation plants its own source
    // location, so the statistics view shows one row per instantiation under
    // this one name.
    GEKKO_PROFILE_ZONE("res/prs-expand");
    out.clear();
    if (expected_size != 0u) out.reserve(expected_size + 256u);
    std::size_t i = 0;
    u32 flags = 0;
    u32 bits = 0;
    auto bit = [&]() -> int {
        if (bits == 0u) {
            if (i >= src_size) return -1;
            flags = src[i++];
            bits = 8u;
        }
        const int value = static_cast<int>(flags & 1u);
        flags >>= 1u;
        --bits;
        return value;
    };
    while (i < src_size && (expected_size == 0u || out.size() < expected_size)) {
        const int literal = bit();
        if (literal < 0) break;
        if (literal) {
            if (i >= src_size) return false;
            out.push_back(src[i++]);
            continue;
        }
        const int long_form = bit();
        if (long_form < 0) return false;
        if (long_form) {
            if (i + 1u >= src_size) return false;
            const u32 word = static_cast<u32>(src[i]) | (static_cast<u32>(src[i + 1u]) << 8u);
            i += 2u;
            if (word == 0u) break;  // stream terminator
            const std::ptrdiff_t distance =
                static_cast<std::ptrdiff_t>((word >> 3u) | 0xFFFFE000u) - 0x100000000;
            std::size_t length = word & 7u;
            if (length == 0u) {
                if (i >= src_size) return false;
                length = static_cast<std::size_t>(src[i]) + 1u;
                ++i;
            } else {
                length += 2u;
            }
            for (std::size_t k = 0; k < length; ++k) {
                const std::ptrdiff_t pos = static_cast<std::ptrdiff_t>(out.size()) + distance;
                if (pos < 0) return false;
                out.push_back(out[static_cast<std::size_t>(pos)]);
            }
        } else {
            const int high = bit();
            const int low = bit();
            if (high < 0 || low < 0) return false;
            const std::size_t length = (static_cast<std::size_t>((high << 1) | low)) + 2u;
            if (i >= src_size) return false;
            const std::ptrdiff_t distance = static_cast<std::ptrdiff_t>(src[i]) - 0x100;
            ++i;
            for (std::size_t k = 0; k < length; ++k) {
                const std::ptrdiff_t pos = static_cast<std::ptrdiff_t>(out.size()) + distance;
                if (pos < 0) return false;
                out.push_back(out[static_cast<std::size_t>(pos)]);
            }
        }
    }
    // Reached by both the terminator break and normal loop exit.
    GEKKO_PROFILE_VALUE(out.size());
    return true;
}

// The wrappers carry the OUTCOME, not the codec: prs_expand_impl has eight
// separate `return false` paths, and reporting from here cannot miss one or
// desynchronise from it later. A malformed stream means a member silently
// fails to load and its textures never register - until now, entirely silent.
bool prs_expand(const u8* src, std::size_t src_size, std::size_t expected_size, std::vector<u8>& out) {
    const bool ok = prs_expand_impl(src, src_size, expected_size, out);
    if (ok) {
        GEKKO_PROFILE_PLOT("res/prs-bytes", static_cast<double>(out.size()));
    } else {
        GEKKO_PROFILE_MESSAGE_LITERAL("res/prs-malformed");
    }
    return ok;
}

bool prs_expand(const u8* src, std::size_t src_size, std::size_t expected_size, DecompressionBytes& out) {
    const bool ok = prs_expand_impl(src, src_size, expected_size, out);
    if (ok) {
        GEKKO_PROFILE_PLOT("res/prs-bytes", static_cast<double>(out.size()));
    } else {
        GEKKO_PROFILE_MESSAGE_LITERAL("res/prs-malformed");
    }
    return ok;
}


}  // namespace gekko
