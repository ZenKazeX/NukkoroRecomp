#include "assets/archive.h"

#include <algorithm>

namespace shadow::assets {

namespace {

class BitReader {
public:
    BitReader(ByteView source, std::size_t& cursor) : source_(source), cursor_(cursor) {}

    bool next(std::uint32_t* bit) {
        if (bits_ == 0) {
            if (cursor_ >= source_.size)
                return false;
            flags_ = source_.data[cursor_++];
            bits_ = 8;
        }
        *bit = flags_ & 1u;
        flags_ >>= 1;
        --bits_;
        return true;
    }

private:
    ByteView source_;
    std::size_t& cursor_;
    std::uint32_t flags_ = 0;
    std::uint32_t bits_ = 0;
};

bool fail(std::string* error, const char* what) {
    if (error != nullptr)
        *error = what;
    return false;
}

constexpr std::size_t kOneEntryBytes = 56;
constexpr std::size_t kOneDataBase = 0x0C;

}  // namespace

bool prs_expand(ByteView source, std::vector<std::uint8_t>* out, std::size_t* consumed, bool* terminated,
                std::string* error) {
    out->clear();
    std::size_t i = 0;
    BitReader bits(source, i);
    bool ended = false;
    for (;;) {
        std::uint32_t literal = 0;
        if (i >= source.size || !bits.next(&literal))
            break;  // the bound: the stream ran out without its terminator
        if (literal != 0) {
            if (i >= source.size)
                return fail(error, "PRS literal past the end of the stream");
            out->push_back(source.data[i++]);
            continue;
        }
        std::uint32_t long_copy = 0;
        if (!bits.next(&long_copy))
            return fail(error, "PRS stream ends inside a copy flag");
        std::ptrdiff_t distance = 0;
        std::size_t length = 0;
        if (long_copy != 0) {
            if (i + 2 > source.size)
                return fail(error, "PRS long copy past the end of the stream");
            const std::uint32_t word = source.data[i] | (static_cast<std::uint32_t>(source.data[i + 1]) << 8);
            i += 2;
            if (word == 0) {
                ended = true;
                break;
            }
            distance = static_cast<std::ptrdiff_t>(word >> 3) - 8192;
            length = word & 7u;
            if (length == 0) {
                if (i >= source.size)
                    return fail(error, "PRS copy length past the end of the stream");
                length = static_cast<std::size_t>(source.data[i++]) + 1;
            } else {
                length += 2;
            }
        } else {
            std::uint32_t high = 0;
            std::uint32_t low = 0;
            if (!bits.next(&high) || !bits.next(&low))
                return fail(error, "PRS stream ends inside a short copy");
            length = ((high << 1) | low) + 2;
            if (i >= source.size)
                return fail(error, "PRS short copy past the end of the stream");
            distance = static_cast<std::ptrdiff_t>(source.data[i++]) - 256;
        }
        const std::ptrdiff_t from = static_cast<std::ptrdiff_t>(out->size()) + distance;
        if (from < 0)
            return fail(error, "PRS copy reaches before the start of the output");
        // Byte by byte: a copy may overlap the bytes it produces (run-length behaviour).
        for (std::size_t k = 0; k < length; ++k)
            out->push_back((*out)[static_cast<std::size_t>(from) + k]);
    }
    if (consumed != nullptr)
        *consumed = i;
    if (terminated != nullptr)
        *terminated = ended;
    return true;
}

bool is_one_archive(ByteView bytes) noexcept {
    return bytes.size >= 0x40 && std::memcmp(bytes.data + 0x0C, "One Ver ", 8) == 0;
}

bool parse_one_archive(ByteView bytes, OneArchive* out, std::string* error) {
    *out = OneArchive{};
    if (!is_one_archive(bytes))
        return fail(error, "not a ONE archive");
    out->version.assign(reinterpret_cast<const char*>(bytes.data + 0x0C), 12);
    out->library_id = load_le32(bytes.data + 0x08);
    out->legacy_layout = std::memcmp(bytes.data + 0x14, "0.5", 3) == 0;
    out->count = load_le32(bytes.data + 0x1C);
    const std::size_t size_at = out->legacy_layout ? 0x20 : 0x2C;
    const std::size_t offset_at = out->legacy_layout ? 0x24 : 0x30;
    const std::size_t flags_at = out->legacy_layout ? 0x28 : 0x34;
    // The table holds `count` members plus two trailing slots; empty slots are skipped.
    for (std::uint32_t slot = 0; slot < out->count + 2; ++slot) {
        const std::size_t entry = 0x40 + static_cast<std::size_t>(slot) * kOneEntryBytes;
        if (!bytes.contains(entry, kOneEntryBytes))
            break;
        const std::uint8_t* e = bytes.data + entry;
        if (e[0x1F] == 0 && load_le32(e) == 0)
            continue;
        OneMember member;
        member.index = slot;
        const auto* name = reinterpret_cast<const char*>(e);
        member.name.assign(name, std::find(name, name + 0x20, '\0'));
        member.size = load_le32(e + size_at);
        member.offset = load_le32(e + offset_at);
        member.flags = load_le32(e + flags_at);
        out->members.push_back(std::move(member));
    }
    return true;
}

bool read_one_member(ByteView bytes, const OneArchive& archive, const OneMember& member,
                     std::vector<std::uint8_t>* out, std::string* error) {
    const std::size_t start = static_cast<std::size_t>(member.offset) + kOneDataBase;
    if (start > bytes.size)
        return fail(error, "ONE member starts past the end of the archive");
    // A member's stored bytes end where the next member's begin (or at the end of the archive).
    std::size_t bound = bytes.size;
    for (const OneMember& other : archive.members) {
        const std::size_t other_start = static_cast<std::size_t>(other.offset) + kOneDataBase;
        if (other_start > start && other_start < bound)
            bound = other_start;
    }
    if ((member.flags & 1u) == 0) {
        if (!bytes.contains(start, member.size))
            return fail(error, "stored ONE member runs past the end of the archive");
        out->assign(bytes.data + start, bytes.data + start + member.size);
        return true;
    }
    if (!prs_expand(ByteView{bytes.data + start, bound - start}, out, nullptr, nullptr, error))
        return false;
    if (out->size() != member.size)
        return fail(error, "ONE member does not expand to its declared size");
    return true;
}

}  // namespace shadow::assets
