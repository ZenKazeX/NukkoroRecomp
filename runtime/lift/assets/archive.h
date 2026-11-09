#pragma once

// The game's archive formats: PRS compression and ONE archives (docs/lift-20261006/R2_DRIVER.md 2.3).
//
// A ONE archive is a table of named members followed by their bytes; a member is stored either as is or PRS
// compressed (an LZ77 variant with a bit-flag stream). Stage, character, effect and event data arrive this way.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/bytes.h"

namespace shadow::assets {

// Expands the PRS stream that starts at `source.data` until its terminator, without reading past `source.size`.
// `consumed` (optional) receives the bytes read; `terminated` (optional) whether the stream ended with its own
// terminator rather than at the bound. False with a reason on a malformed stream.
bool prs_expand(ByteView source, std::vector<std::uint8_t>* out, std::size_t* consumed, bool* terminated,
                std::string* error);

struct OneMember {
    std::uint32_t index = 0;   // table slot
    std::string name;
    std::uint32_t size = 0;    // expanded size
    std::uint32_t offset = 0;  // relative to the data area
    std::uint32_t flags = 0;   // bit 0: PRS compressed
};

struct OneArchive {
    std::string version;       // "One Ver 0.60" (or the older "0.5x" layout)
    bool legacy_layout = false;
    std::uint32_t library_id = 0;
    std::uint32_t count = 0;
    std::vector<OneMember> members;
};

// True when the bytes carry a ONE archive signature.
bool is_one_archive(ByteView bytes) noexcept;
bool parse_one_archive(ByteView bytes, OneArchive* out, std::string* error);
// The member's expanded bytes (exactly `member.size` of them; anything else is an error).
bool read_one_member(ByteView bytes, const OneArchive& archive, const OneMember& member,
                     std::vector<std::uint8_t>* out, std::string* error);

}  // namespace shadow::assets
