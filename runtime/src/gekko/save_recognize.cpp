// The universal save recognizer (gekko/save_recognize.h, docs/SAVE_FORMATS.md).

#include "gekko/save_recognize.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

namespace gekko::save_recognize {

namespace {

using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;

constexpr std::size_t kMaxSourceBytes = 64u << 20;  // a save container is far smaller; larger files are skipped

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}
void put_be16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
void put_be32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}

// A NUL-terminated field of at most n bytes.
std::string field(const std::uint8_t* p, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n && p[i] != 0; ++i) s.push_back(static_cast<char>(p[i]));
    return s;
}
bool printable(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 0x20 && u < 0x7F;
    });
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool fail(std::string* error, std::string why) {
    if (error != nullptr) *error = std::move(why);
    return false;
}

// ---- CRC-32 (IEEE, reflected 0xEDB88320) --------------------------------------------------------------------------

const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    return table;
}

// ---- Inflate (RFC 1951) ---------------------------------------------------------------------------------------------

struct BitIn {
    View in;
    std::size_t pos = 0;
    std::uint32_t buf = 0;
    int count = 0;
    bool over = false;
    std::uint32_t bits(int n) {
        while (count < n) {
            if (pos >= in.size()) {
                over = true;
                return 0;
            }
            buf |= static_cast<std::uint32_t>(in[pos++]) << count;
            count += 8;
        }
        const std::uint32_t v = buf & ((1u << n) - 1u);
        buf >>= n;
        count -= n;
        return v;
    }
};

struct Huffman {
    std::array<std::uint16_t, 16> count{};
    std::array<std::uint16_t, 320> symbol{};
};

// Canonical code from lengths; false when over-subscribed.
bool build(Huffman& h, const std::uint8_t* lengths, int n) {
    h.count.fill(0);
    for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
    if (h.count[0] == n) return true;  // no codes: only an error if used
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    std::array<std::uint16_t, 16> offs{};
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<std::uint16_t>(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
        if (lengths[i] != 0) h.symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
    return true;
}

int decode_symbol(BitIn& b, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= static_cast<int>(b.bits(1));
        if (b.over) return -1;
        const int c = h.count[len];
        if (code - c < first) return h.symbol[index + (code - first)];
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

constexpr std::uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_codes(BitIn& b, Bytes& out, const Huffman& lit, const Huffman& dist, std::size_t limit) {
    for (;;) {
        const int sym = decode_symbol(b, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= limit) return false;
            out.push_back(static_cast<std::uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            const int li = sym - 257;
            if (li >= 29) return false;
            const std::size_t len = kLenBase[li] + b.bits(kLenExtra[li]);
            const int ds = decode_symbol(b, dist);
            if (ds < 0 || ds >= 30 || b.over) return false;
            const std::size_t d = kDistBase[ds] + b.bits(kDistExtra[ds]);
            if (b.over || d > out.size() || out.size() + len > limit) return false;
            const std::size_t from = out.size() - d;
            for (std::size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    }
}

// ---- LZARI (Haruhiko Okumura, 1989), as Action Replay MAX uses it -----------------------------------------------------
//
// The arithmetic coder with an adaptive frequency model; ring buffer 4096 filled with spaces, matches 3..60 bytes,
// bits most significant first. Ported from the published algorithm.

constexpr int kN = 4096, kF = 60, kThreshold = 2;
constexpr std::uint32_t kM = 15, kQ1 = 1u << kM, kQ2 = 2 * kQ1, kQ3 = 3 * kQ1, kQ4 = 4 * kQ1, kMaxCum = kQ1 - 1;
constexpr int kNChar = 256 - kThreshold + kF;

struct LzariModel {
    std::array<int, kNChar> char_to_sym{};
    std::array<int, kNChar + 1> sym_to_char{};
    std::array<std::uint32_t, kNChar + 1> sym_freq{}, sym_cum{};
    std::array<std::uint32_t, kN + 1> position_cum{};

    LzariModel() {
        sym_cum[kNChar] = 0;
        for (int sym = kNChar; sym >= 1; --sym) {
            const int ch = sym - 1;
            char_to_sym[ch] = sym;
            sym_to_char[sym] = ch;
            sym_freq[sym] = 1;
            sym_cum[sym - 1] = sym_cum[sym] + sym_freq[sym];
        }
        sym_freq[0] = 0;
        position_cum[kN] = 0;
        for (int i = kN; i >= 1; --i) position_cum[i - 1] = position_cum[i] + 10000u / static_cast<std::uint32_t>(i + 200);
    }
    void update(int sym) {
        if (sym_cum[0] >= kMaxCum) {
            std::uint32_t c = 0;
            for (int i = kNChar; i > 0; --i) {
                sym_cum[i] = c;
                c += (sym_freq[i] = (sym_freq[i] + 1) >> 1);
            }
            sym_cum[0] = c;
        }
        int i = sym;
        while (sym_freq[i] == sym_freq[i - 1]) --i;
        if (i < sym) {
            const int ch_i = sym_to_char[i], ch_sym = sym_to_char[sym];
            sym_to_char[i] = ch_sym;
            sym_to_char[sym] = ch_i;
            char_to_sym[ch_i] = sym;
            char_to_sym[ch_sym] = i;
        }
        ++sym_freq[i];
        while (--i >= 0) ++sym_cum[i];
    }
};

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) {
    const auto& t = crc_table();
    crc = ~crc;
    for (const std::uint8_t b : data) crc = t[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

bool inflate_raw(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t expected_size,
                 std::string* error) {
    out.clear();
    out.reserve(expected_size);
    BitIn b{in};
    const std::size_t limit = expected_size;
    for (;;) {
        const std::uint32_t last = b.bits(1);
        const std::uint32_t type = b.bits(2);
        if (b.over) return fail(error, "deflate: the stream ends inside a block header");
        if (type == 0) {
            b.buf = 0;
            b.count = 0;  // to the byte boundary
            if (b.pos + 4 > in.size()) return fail(error, "deflate: a stored block's length is cut");
            const std::uint16_t len = le16(in.data() + b.pos), nlen = le16(in.data() + b.pos + 2);
            b.pos += 4;
            if (static_cast<std::uint16_t>(~nlen) != len) return fail(error, "deflate: a stored block's length check fails");
            if (b.pos + len > in.size() || out.size() + len > limit) return fail(error, "deflate: a stored block overruns");
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(b.pos),
                       in.begin() + static_cast<std::ptrdiff_t>(b.pos + len));
            b.pos += len;
        } else if (type == 1) {
            static const std::pair<Huffman, Huffman> fixed = [] {
                std::pair<Huffman, Huffman> f;
                std::uint8_t l[288];
                for (int i = 0; i < 144; ++i) l[i] = 8;
                for (int i = 144; i < 256; ++i) l[i] = 9;
                for (int i = 256; i < 280; ++i) l[i] = 7;
                for (int i = 280; i < 288; ++i) l[i] = 8;
                build(f.first, l, 288);
                std::uint8_t d[30];
                std::fill_n(d, 30, std::uint8_t{5});
                build(f.second, d, 30);
                return f;
            }();
            if (!inflate_codes(b, out, fixed.first, fixed.second, limit)) return fail(error, "deflate: a fixed block is corrupt");
        } else if (type == 2) {
            const int nlen = static_cast<int>(b.bits(5)) + 257, ndist = static_cast<int>(b.bits(5)) + 1,
                      ncode = static_cast<int>(b.bits(4)) + 4;
            if (b.over || nlen > 286 || ndist > 30) return fail(error, "deflate: a dynamic block header is corrupt");
            static constexpr std::uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = static_cast<std::uint8_t>(b.bits(3));
            Huffman lencode;
            if (b.over || !build(lencode, lengths, 19)) return fail(error, "deflate: the code-length code is corrupt");
            int index = 0;
            std::fill_n(lengths, 320, std::uint8_t{0});
            while (index < nlen + ndist) {
                int sym = decode_symbol(b, lencode);
                if (sym < 0) return fail(error, "deflate: a code length is corrupt");
                if (sym < 16) {
                    lengths[index++] = static_cast<std::uint8_t>(sym);
                } else {
                    std::uint8_t len = 0;
                    int repeat = 0;
                    if (sym == 16) {
                        if (index == 0) return fail(error, "deflate: a repeat with no previous length");
                        len = lengths[index - 1];
                        repeat = 3 + static_cast<int>(b.bits(2));
                    } else if (sym == 17) {
                        repeat = 3 + static_cast<int>(b.bits(3));
                    } else {
                        repeat = 11 + static_cast<int>(b.bits(7));
                    }
                    if (b.over || index + repeat > nlen + ndist) return fail(error, "deflate: too many code lengths");
                    while (repeat-- > 0) lengths[index++] = len;
                }
            }
            if (lengths[256] == 0) return fail(error, "deflate: no end-of-block code");
            Huffman lit, dist;
            if (!build(lit, lengths, nlen) || !build(dist, lengths + nlen, ndist))
                return fail(error, "deflate: a dynamic code is over-subscribed");
            if (!inflate_codes(b, out, lit, dist, limit)) return fail(error, "deflate: a dynamic block is corrupt");
        } else {
            return fail(error, "deflate: block type 3");
        }
        if (last != 0u) break;
    }
    if (out.size() != expected_size)
        return fail(error, "deflate: " + std::to_string(out.size()) + " bytes, " + std::to_string(expected_size) +
                               " expected");
    return true;
}

bool lzari_decode(std::span<const std::uint8_t> in, std::size_t out_size, std::vector<std::uint8_t>& out,
                  std::string* error) {
    out.clear();
    out.reserve(out_size);
    std::size_t pos = 0, overrun = 0;
    unsigned buffer = 0, mask = 0;
    // Past the end the coder reads zero bits; a few bytes of look-ahead are normal (the value register is M + 2
    // bits ahead), more means the stream is cut.
    const auto bit = [&]() -> std::uint32_t {
        if ((mask >>= 1) == 0) {
            if (pos < in.size()) buffer = in[pos];
            else buffer = 0, ++overrun;
            ++pos;
            mask = 128;
        }
        return (buffer & mask) != 0 ? 1u : 0u;
    };
    auto model = std::make_unique<LzariModel>();
    LzariModel& m = *model;
    std::uint32_t low = 0, high = kQ4, value = 0;
    for (std::uint32_t i = 0; i < kM + 2; ++i) value = 2 * value + bit();
    const auto renormalise = [&] {
        for (;;) {
            if (low >= kQ2) {
                value -= kQ2, low -= kQ2, high -= kQ2;
            } else if (low >= kQ1 && high <= kQ3) {
                value -= kQ1, low -= kQ1, high -= kQ1;
            } else if (high > kQ2) {
                break;
            }
            low += low;
            high += high;
            value = 2 * value + bit();
        }
    };
    std::array<std::uint8_t, kN> text;
    text.fill(' ');
    int r = kN - kF;
    while (out.size() < out_size) {
        const std::uint64_t range = high - low;
        if (range == 0 || value < low) return fail(error, "LZARI: the stream is corrupt");
        const std::uint64_t x = ((static_cast<std::uint64_t>(value - low) + 1) * m.sym_cum[0] - 1) / range;
        int i = 1, j = kNChar;
        while (i < j) {
            const int k = (i + j) / 2;
            if (m.sym_cum[k] > x) i = k + 1;
            else j = k;
        }
        const int sym = i;
        high = low + static_cast<std::uint32_t>((range * m.sym_cum[sym - 1]) / m.sym_cum[0]);
        low += static_cast<std::uint32_t>((range * m.sym_cum[sym]) / m.sym_cum[0]);
        renormalise();
        const int ch = m.sym_to_char[sym];
        m.update(sym);
        if (ch < 256) {
            out.push_back(static_cast<std::uint8_t>(ch));
            text[r] = static_cast<std::uint8_t>(ch);
            r = (r + 1) & (kN - 1);
        } else {
            const std::uint64_t prange = high - low;
            if (prange == 0) return fail(error, "LZARI: the stream is corrupt");
            const std::uint64_t px = ((static_cast<std::uint64_t>(value - low) + 1) * m.position_cum[0] - 1) / prange;
            int a = 1, z = kN;
            while (a < z) {
                const int k = (a + z) / 2;
                if (m.position_cum[k] > px) a = k + 1;
                else z = k;
            }
            const int position = a - 1;
            high = low + static_cast<std::uint32_t>((prange * m.position_cum[position]) / m.position_cum[0]);
            low += static_cast<std::uint32_t>((prange * m.position_cum[position + 1]) / m.position_cum[0]);
            renormalise();
            const int from = (r - position - 1) & (kN - 1);
            const int length = ch - 255 + kThreshold;
            for (int k = 0; k < length && out.size() < out_size; ++k) {
                const std::uint8_t c = text[(from + k) & (kN - 1)];
                out.push_back(c);
                text[r] = c;
                r = (r + 1) & (kN - 1);
            }
        }
        if (overrun > 8) return fail(error, "LZARI: the stream ends early");
    }
    return true;
}

std::vector<std::uint8_t> lzari_encode(std::span<const std::uint8_t> in) {
    Bytes out;
    unsigned bitbuf = 0, bitmask = 128;
    const auto put_bit = [&](int b) {
        if (b != 0) bitbuf |= bitmask;
        if ((bitmask >>= 1) == 0) {
            out.push_back(static_cast<std::uint8_t>(bitbuf));
            bitbuf = 0;
            bitmask = 128;
        }
    };
    std::uint32_t low = 0, high = kQ4, shifts = 0;
    const auto output = [&](int b) {
        put_bit(b);
        for (; shifts > 0; --shifts) put_bit(!b);
    };
    const auto renormalise = [&] {
        for (;;) {
            if (high <= kQ2) {
                output(0);
            } else if (low >= kQ2) {
                output(1);
                low -= kQ2, high -= kQ2;
            } else if (low >= kQ1 && high <= kQ3) {
                ++shifts;
                low -= kQ1, high -= kQ1;
            } else {
                break;
            }
            low += low;
            high += high;
        }
    };
    auto model = std::make_unique<LzariModel>();
    LzariModel& m = *model;
    const auto encode_char = [&](int ch) {
        const int sym = m.char_to_sym[ch];
        const std::uint64_t range = high - low;
        high = low + static_cast<std::uint32_t>((range * m.sym_cum[sym - 1]) / m.sym_cum[0]);
        low += static_cast<std::uint32_t>((range * m.sym_cum[sym]) / m.sym_cum[0]);
        renormalise();
        m.update(sym);
    };
    const auto encode_position = [&](int position) {
        const std::uint64_t range = high - low;
        high = low + static_cast<std::uint32_t>((range * m.position_cum[position]) / m.position_cum[0]);
        low += static_cast<std::uint32_t>((range * m.position_cum[position + 1]) / m.position_cum[0]);
        renormalise();
    };
    // Greedy longest match over the last kN - kF bytes of real history (the decoder's initial spaces are never
    // referenced), brute force: the encoder only builds test fixtures.
    std::size_t i = 0;
    while (i < in.size()) {
        std::size_t best_len = 0, best_dist = 0;
        const std::size_t max_dist = std::min<std::size_t>(i, kN - kF);
        for (std::size_t d = 1; d <= max_dist; ++d) {
            std::size_t len = 0;
            while (len < static_cast<std::size_t>(kF) && i + len < in.size() && in[i + len - d] == in[i + len]) ++len;
            if (len > best_len) best_len = len, best_dist = d;
            if (best_len == static_cast<std::size_t>(kF)) break;
        }
        if (best_len > static_cast<std::size_t>(kThreshold)) {
            encode_char(static_cast<int>(255 - kThreshold + best_len));
            encode_position(static_cast<int>(best_dist - 1));
            i += best_len;
        } else {
            encode_char(in[i]);
            ++i;
        }
    }
    ++shifts;
    output(low < kQ1 ? 0 : 1);
    for (int k = 0; k < 7; ++k) put_bit(0);
    return out;
}

const char* to_string(Platform p) {
    switch (p) {
    case Platform::GameCube: return "GameCube";
    case Platform::PlayStation2: return "PlayStation 2";
    case Platform::Xbox: return "Xbox";
    default: return "unknown";
    }
}
const char* to_string(Region r) {
    switch (r) {
    case Region::NtscU: return "NTSC-U";
    case Region::Pal: return "PAL";
    case Region::NtscJ: return "NTSC-J";
    default: return "unknown";
    }
}
const char* to_string(Container c) {
    switch (c) {
    case Container::Gci: return "GCI";
    case Container::GcCardImage: return "GameCube memory-card image";
    case Container::ArMax: return "Action Replay MAX (.max)";
    case Container::CodeBreaker: return "CodeBreaker (.cbs)";
    case Container::SharkPort: return "SharkPort / X-Port (.xps)";
    case Container::Psv: return "PS3 PSV";
    case Container::XboxSaveDat: return "Xbox Save.dat";
    case Container::Zip: return "ZIP";
    default: return "unknown";
    }
}
const char* to_string(RecordState s) {
    switch (s) {
    case RecordState::Decoded: return "decoded";
    case RecordState::Encrypted: return "encrypted (key unknown)";
    default: return "none";
    }
}

namespace {

// ---- The game's identities ------------------------------------------------------------------------------------------

constexpr const char* kGcName = "SHADOWTHEHEDGEHOG";
constexpr std::size_t kPs2DataBytes = 0x2000;   // the game's PS2 data file
constexpr std::size_t kPs2RecordBytes = 0x1C40; // its encrypted part (zero padding follows in every sample)
constexpr std::size_t kXboxSaveBytes = 0x1F74;  // Save.dat: 0x18-byte header + the 0x1F5C record
constexpr std::size_t kXboxHeaderBytes = 0x18;
constexpr const char* kXboxTitle = "53450086";

Region gc_region(char c) {
    switch (c) {
    case 'E': return Region::NtscU;
    case 'P': return Region::Pal;
    case 'J': return Region::NtscJ;
    default: return Region::Unknown;
    }
}

// The game's PS2 products (the directory / data file name is "B" + a region letter + the product).
struct Ps2Product {
    const char* product;
    Region region;
};
constexpr Ps2Product kPs2Products[] = {
    {"SLUS-21261", Region::NtscU},
    {"SLES-53542", Region::Pal},
    {"SLPM-66166", Region::NtscJ},
};
// The product for a PS2 save directory name ("BASLUS-21261"); null when it is not this game.
const Ps2Product* ps2_product(const std::string& dir) {
    if (dir.size() < 12 || dir[0] != 'B') return nullptr;
    for (const Ps2Product& p : kPs2Products)
        if (dir.compare(2, std::strlen(p.product), p.product) == 0) return &p;
    return nullptr;
}

struct Ctx {
    int depth = 0;
};

void identify_into(View bytes, const std::string& name, Identification& id, Ctx ctx);

Save ps2_save(Container c, const Ps2Product& p, const std::string& dir) {
    Save s;
    s.platform = Platform::PlayStation2;
    s.container = c;
    s.region = p.region;
    s.product = p.product;
    s.location = dir;
    return s;
}

// The PS2 data file found in a container: the record is reachable, but encrypted.
void ps2_payload(Save& s, View data, std::string checks) {
    if (data.size() != kPs2DataBytes) {
        s.valid = false;
        s.reason = checks + "; the game's data file is " + std::to_string(data.size()) + " bytes, " +
                   std::to_string(kPs2DataBytes) + " expected";
        return;
    }
    s.payload.assign(data.begin(), data.end());
    s.record_state = RecordState::Encrypted;
    s.valid = true;
    s.reason = checks + "; record encrypted with the PS2 key (unknown), PS2 record layout differs from the GameCube's";
}

// ---- GameCube ---------------------------------------------------------------------------------------------------------

void gc_from_gci(const save_file::Gci& gci, Container c, const std::string& location, Identification& id) {
    Save s;
    s.platform = Platform::GameCube;
    s.container = c;
    s.product = gci.game_code();
    s.region = gc_region(s.product.size() == 4 ? s.product[3] : '?');
    s.location = location;
    s.payload.assign(gci.header.begin(), gci.header.end());
    s.payload.insert(s.payload.end(), gci.data.begin(), gci.data.end());
    if (gci.data.size() < save_file::kRecordOffset + save_file::kRecordBytes) {
        s.reason = "the file is too short for the record";
    } else if (be32(gci.header.data() + 0x2C) == 0xFFFFFFFFu) {
        s.reason = "an interrupted create (iconAddr 0xFFFFFFFF): the game treats it as broken";
    } else {
        const save_file::RecordCheck r = save_file::check_record(gci.data);
        s.record_state = RecordState::Decoded;
        s.valid = r.valid;
        s.reason = r.valid ? "record decrypted, MD5 matches" : "record decrypted, MD5 does not match";
        s.convertible = r.valid && gci.data.size() == 2 * save_file::kBlockBytes && s.region != Region::Unknown;
    }
    id.saves.push_back(std::move(s));
}

bool try_gci(View bytes, const std::string& location, Identification& id) {
    if (bytes.size() < save_file::kGciHeaderBytes + save_file::kBlockBytes) return false;
    const std::size_t blocks = be16(bytes.data() + 0x38);
    if (blocks == 0 || bytes.size() != save_file::kGciHeaderBytes + blocks * save_file::kBlockBytes) return false;
    save_file::Gci gci;
    std::copy_n(bytes.begin(), save_file::kGciHeaderBytes, gci.header.begin());
    const std::string game = gci.game_code(), maker = gci.maker_code();
    if (!printable(game) || game.size() != 4 || maker.size() != 2) return false;
    id.outer = Container::Gci;
    if (game.compare(0, 3, "GUP") != 0 || maker != "8P" || gci.file_name() != kGcName) {
        id.reason = "a GameCube GCI of another game or file (" + game + "/" + maker + " " + gci.file_name() + ")";
        return true;
    }
    gci.data.assign(bytes.begin() + save_file::kGciHeaderBytes, bytes.end());
    gc_from_gci(gci, Container::Gci, location, id);
    return true;
}

bool try_card(View bytes, const std::string& location, Identification& id) {
    if (bytes.size() < 5u * save_file::kBlockBytes || bytes.size() % save_file::kBlockBytes != 0u) return false;
    const Bytes card(bytes.begin(), bytes.end());
    bool is_card = false;
    for (const char* game : {"GUPE", "GUPP", "GUPJ"}) {
        save_file::Gci gci;
        std::string error;
        if (save_file::extract_from_card(card, game, "8P", kGcName, gci, &error)) {
            is_card = true;
            gc_from_gci(gci, Container::GcCardImage, location.empty() ? std::string(game) : location + "/" + game, id);
        } else if (error.rfind("the card holds no", 0) == 0 || error.find("block chain") != std::string::npos) {
            is_card = true;
            if (error.find("block chain") != std::string::npos) {
                Save s;
                s.platform = Platform::GameCube;
                s.container = Container::GcCardImage;
                s.product = game;
                s.region = gc_region(game[3]);
                s.location = location;
                s.reason = "the card lists the game's file but " + error;
                id.saves.push_back(std::move(s));
            }
        }
    }
    if (!is_card) return false;
    id.outer = Container::GcCardImage;
    if (id.saves.empty()) id.reason = "a GameCube memory-card image without the game's file";
    return true;
}

// ---- PlayStation 2 ----------------------------------------------------------------------------------------------------

// Action Replay MAX: "Ps2PowerSave", CRC-32 of the file with its CRC field zeroed at +0x0C, the directory name at
// +0x10, +0x50 the compressed length (from +0x58; some writers store the decompressed length here), +0x54 the file
// count, +0x58 the decompressed length, LZARI from +0x5C. The decompressed stream: per file a u32 length, a 32-byte
// name, the data; the next entry at round_up(end + 8, 16) - 8.
bool try_max(View b, Identification& id) {
    if (b.size() < 0x5C || std::memcmp(b.data(), "Ps2PowerSave", 12) != 0) return false;
    id.outer = Container::ArMax;
    const std::string dir = field(b.data() + 0x10, 32);
    const Ps2Product* p = ps2_product(dir);
    if (p == nullptr) {
        id.reason = "an Action Replay MAX save of another game (" + dir + ")";
        return true;
    }
    Save s = ps2_save(Container::ArMax, *p, dir);
    Bytes zeroed(b.begin(), b.end());
    std::memset(zeroed.data() + 0x0C, 0, 4);
    const bool crc_ok = crc32(zeroed) == le32(b.data() + 0x0C);
    const std::uint32_t files = le32(b.data() + 0x54), size = le32(b.data() + 0x58);
    std::string checks = crc_ok ? "CRC-32 matches" : "CRC-32 does not match";
    if (le32(b.data() + 0x50) != b.size() - 0x58) checks += ", nonstandard length field at +0x50";
    Bytes plain;
    std::string error;
    if (size > kMaxSourceBytes || !lzari_decode(b.subspan(0x5C), size, plain, &error)) {
        s.reason = checks + "; " + (size > kMaxSourceBytes ? "decompressed length implausible" : error);
        id.saves.push_back(std::move(s));
        return true;
    }
    std::size_t off = 0;
    View data;
    bool found = false, table_ok = true;
    for (std::uint32_t f = 0; f < files; ++f) {
        if (off + 0x24 > plain.size()) {
            table_ok = false;
            break;
        }
        const std::uint32_t len = le32(plain.data() + off);
        const std::string name = field(plain.data() + off + 4, 32);
        if (!printable(name) || off + 0x24 + len > plain.size()) {
            table_ok = false;
            break;
        }
        if (name == dir) data = View(plain).subspan(off + 0x24, len), found = true;
        off = ((off + 0x24 + len + 8 + 15) & ~std::size_t{15}) - 8;
    }
    if (!table_ok) checks += ", file list corrupt";
    if (!found) {
        s.reason = checks + "; the game's data file " + dir + " is missing";
        id.saves.push_back(std::move(s));
        return true;
    }
    ps2_payload(s, data, checks);
    s.valid = s.valid && crc_ok && table_ok;
    id.saves.push_back(std::move(s));
    return true;
}

// CodeBreaker: "CFU\0", +0x08 the payload offset, +0x0C the decompressed length, +0x10 the payload length, +0x14 the
// directory name. The payload is zlib, RC4-encrypted with CodeBreaker's fixed 256-byte key - not available offline,
// so the save is recognised from its header only.
bool try_cbs(View b, Identification& id) {
    if (b.size() < 0x128 || std::memcmp(b.data(), "CFU\0", 4) != 0) return false;
    id.outer = Container::CodeBreaker;
    const std::string dir = field(b.data() + 0x14, 32);
    const Ps2Product* p = ps2_product(dir);
    if (p == nullptr) {
        id.reason = "a CodeBreaker save of another game (" + dir + ")";
        return true;
    }
    Save s = ps2_save(Container::CodeBreaker, *p, dir);
    const std::uint32_t offset = le32(b.data() + 8), length = le32(b.data() + 0x10);
    const bool sizes_ok = offset >= 0x128 && static_cast<std::uint64_t>(offset) + length == b.size();
    s.reason = std::string(sizes_ok ? "header consistent" : "header lengths do not match the file") +
               "; payload is RC4-encrypted with CodeBreaker's key (not available offline): not unwrapped";
    id.saves.push_back(std::move(s));
    return true;
}

// SharkPort / X-Port: u32 13 "SharkPortSave", a u32 (0), three u32-length strings (title, subtitle, comment), the
// u32 length of the rest; then the directory header (u16 header length, 64-byte name, u32 entry count incl. . and ..)
// and per file a header (u16 length, 64-byte name, u32 data length) followed by the data. Four trailing bytes (a
// checksum of a form not known) are not checked.
bool try_xps(View b, Identification& id) {
    if (b.size() < 0x15 || le32(b.data()) != 13 || std::memcmp(b.data() + 4, "SharkPortSave", 13) != 0) return false;
    id.outer = Container::SharkPort;
    std::size_t o = 4 + 13 + 4;
    for (int i = 0; i < 3; ++i) {
        if (o + 4 > b.size()) return id.reason = "a SharkPort file cut in its header", true;
        const std::uint32_t n = le32(b.data() + o);
        o += 4 + static_cast<std::size_t>(n);
    }
    if (o + 4 + 0x46 > b.size()) return id.reason = "a SharkPort file cut in its header", true;
    const std::uint32_t total = le32(b.data() + o);
    o += 4;
    const std::size_t end = o + total;
    const std::uint16_t root_len = le16(b.data() + o);
    const std::string dir = field(b.data() + o + 2, 64);
    const std::uint32_t entries = le32(b.data() + o + 0x42);
    const Ps2Product* p = ps2_product(dir);
    if (p == nullptr) {
        id.reason = "a SharkPort save of another game (" + dir + ")";
        return true;
    }
    Save s = ps2_save(Container::SharkPort, *p, dir);
    std::string checks = end <= b.size() ? "lengths consistent" : "the file is shorter than its length field";
    o += root_len;
    View data;
    bool found = false, table_ok = end <= b.size() && root_len >= 0x46 && entries >= 2;
    for (std::uint32_t f = 0; table_ok && f + 2 < entries; ++f) {
        if (o + 0x46 > end) {
            table_ok = false;
            break;
        }
        const std::uint16_t h = le16(b.data() + o);
        const std::string name = field(b.data() + o + 2, 64);
        const std::uint32_t len = le32(b.data() + o + 0x42);
        if (h < 0x46 || !printable(name) || o + h + len > end) {
            table_ok = false;
            break;
        }
        if (name == dir) data = b.subspan(o + h, len), found = true;
        o += h + len;
    }
    if (!table_ok) checks += ", file list corrupt";
    if (!found) {
        s.reason = checks + "; the game's data file " + dir + " is missing";
        id.saves.push_back(std::move(s));
        return true;
    }
    ps2_payload(s, data, checks);
    s.valid = s.valid && table_ok;
    id.saves.push_back(std::move(s));
    return true;
}

// PS3 PSV of a PS2 save: "\0VSP" u32 0, +0x08 seed and +0x1C signature (an HMAC with PS3 keys: not checked), +0x3C
// u32 2 (PS2), +0x64 the file count, +0x68 the directory entry (+0x18 its name), files from +0xA0 at 0x3C each
// (+0x10 length, +0x18 32-byte name, +0x38 offset in the PSV).
bool try_psv(View b, Identification& id) {
    if (b.size() < 0xA0 || std::memcmp(b.data(), "\0VSP", 4) != 0) return false;
    id.outer = Container::Psv;
    if (le32(b.data() + 0x3C) != 2) {
        id.reason = "a PSV that is not a PS2 save (type " + std::to_string(le32(b.data() + 0x3C)) + ")";
        return true;
    }
    const std::string dir = field(b.data() + 0x80, 32);
    const Ps2Product* p = ps2_product(dir);
    if (p == nullptr) {
        id.reason = "a PSV save of another game (" + dir + ")";
        return true;
    }
    Save s = ps2_save(Container::Psv, *p, dir);
    const std::uint32_t files = le32(b.data() + 0x64);
    View data;
    bool found = false;
    std::string broken;
    for (std::uint32_t f = 0; f < files && f < 1024; ++f) {
        const std::size_t e = 0xA0 + static_cast<std::size_t>(f) * 0x3C;
        if (e + 0x3C > b.size()) {
            broken = "the file table is cut";
            break;
        }
        const std::uint32_t len = le32(b.data() + e + 0x10), off = le32(b.data() + e + 0x38);
        const std::string name = field(b.data() + e + 0x18, 32);
        if (!printable(name) || static_cast<std::uint64_t>(off) + len > b.size() ||
            off < 0xA0 + static_cast<std::uint64_t>(files) * 0x3C) {
            broken = "file table entry " + std::to_string(f + 1) + " of " + std::to_string(files) + " is unreadable";
            break;
        }
        if (name == dir) data = b.subspan(off, len), found = true;
    }
    if (!broken.empty()) {
        // Offsets from a broken table cannot be trusted: nothing is unwrapped.
        s.reason = broken + ": the data offsets cannot be trusted, not unwrapped";
        id.saves.push_back(std::move(s));
        return true;
    }
    if (!found) {
        s.reason = "the game's data file " + dir + " is missing";
        id.saves.push_back(std::move(s));
        return true;
    }
    ps2_payload(s, data, "file table consistent, PS3 signature not checked");
    id.saves.push_back(std::move(s));
    return true;
}

// ---- Xbox ------------------------------------------------------------------------------------------------------------

// The path components of a name ("UDATA/53450086/25575D9B5D04/Save.dat", either separator).
std::vector<std::string> components(const std::string& name) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : name) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

// Save.dat: u32 its own length (0x1F74), 20 bytes (a signature of a form not known - not checked), then the record
// with the GameCube record's block layout, encrypted with a key that is not known.
bool try_xbox(View b, const std::string& name, Identification& id) {
    const std::vector<std::string> parts = components(name);
    if (parts.empty() || lower(parts.back()) != "save.dat") return false;
    std::string title;
    if (parts.size() >= 3) title = parts[parts.size() - 3];
    if (!title.empty() && title.size() == 8 && lower(title) != lower(kXboxTitle) &&
        std::all_of(title.begin(), title.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
        id.outer = Container::XboxSaveDat;
        id.reason = "an Xbox save of another title (" + title + ")";
        return true;
    }
    if (b.size() != kXboxSaveBytes || le32(b.data()) != kXboxSaveBytes) return false;
    id.outer = Container::XboxSaveDat;
    Save s;
    s.platform = Platform::Xbox;
    s.container = Container::XboxSaveDat;
    s.region = Region::Unknown;  // nothing in the save or its folder names a region
    s.payload.assign(b.begin(), b.end());
    s.record_state = RecordState::Encrypted;
    if (lower(title) == lower(kXboxTitle)) {
        s.product = kXboxTitle;
        s.valid = true;
        s.reason = "length field matches; signature not checked; record encrypted with the Xbox key (unknown)";
    } else {
        s.reason = "a Save.dat of this game's size outside a UDATA/53450086 folder: the title cannot be confirmed";
    }
    id.saves.push_back(std::move(s));
    return true;
}

// ---- ZIP -------------------------------------------------------------------------------------------------------------

bool try_zip(View b, Identification& id, Ctx ctx) {
    if (b.size() < 22 || std::memcmp(b.data(), "PK\x03\x04", 4) != 0) return false;
    id.outer = Container::Zip;
    // The end-of-central-directory record: the last "PK\5\6" within the final 64 KiB + 22 bytes.
    std::size_t eocd = std::string::npos;
    const std::size_t lowest = b.size() > 0x10000 + 22 ? b.size() - 0x10000 - 22 : 0;
    for (std::size_t i = b.size() - 22 + 1; i-- > lowest;) {
        if (std::memcmp(b.data() + i, "PK\x05\x06", 4) == 0) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return id.reason = "a ZIP without its central directory", true;
    const std::uint16_t count = le16(b.data() + eocd + 10);
    std::size_t cd = le32(b.data() + eocd + 16);
    std::string skipped;
    for (std::uint16_t n = 0; n < count; ++n) {
        if (cd + 46 > b.size() || std::memcmp(b.data() + cd, "PK\x01\x02", 4) != 0) {
            skipped += "; the central directory is corrupt";
            break;
        }
        const std::uint16_t flags = le16(b.data() + cd + 8), method = le16(b.data() + cd + 10);
        const std::uint32_t crc = le32(b.data() + cd + 16), csize = le32(b.data() + cd + 20),
                            usize = le32(b.data() + cd + 24);
        const std::uint16_t nlen = le16(b.data() + cd + 28), xlen = le16(b.data() + cd + 30),
                            clen = le16(b.data() + cd + 32);
        const std::uint32_t local = le32(b.data() + cd + 42);
        if (cd + 46 + nlen > b.size()) {
            skipped += "; the central directory is corrupt";
            break;
        }
        const std::string name(reinterpret_cast<const char*>(b.data() + cd + 46), nlen);
        cd += 46u + nlen + xlen + clen;
        if (name.empty() || name.back() == '/') continue;
        if ((flags & 1u) != 0u) {
            skipped += "; " + name + ": encrypted";
            continue;
        }
        if (usize > kMaxSourceBytes || csize == 0xFFFFFFFFu) {
            skipped += "; " + name + ": too large";
            continue;
        }
        if (local + 30u > b.size() || std::memcmp(b.data() + local, "PK\x03\x04", 4) != 0) {
            skipped += "; " + name + ": its local header is missing";
            continue;
        }
        const std::size_t data = local + 30u + le16(b.data() + local + 26) + le16(b.data() + local + 28);
        if (data + csize > b.size()) {
            skipped += "; " + name + ": cut";
            continue;
        }
        Bytes content;
        std::string error;
        if (method == 0) {
            if (csize != usize) {
                skipped += "; " + name + ": stored sizes differ";
                continue;
            }
            content.assign(b.begin() + static_cast<std::ptrdiff_t>(data),
                           b.begin() + static_cast<std::ptrdiff_t>(data + csize));
        } else if (method == 8) {
            if (!inflate_raw(b.subspan(data, csize), content, usize, &error)) {
                skipped += "; " + name + ": " + error;
                continue;
            }
        } else {
            skipped += "; " + name + ": compression method " + std::to_string(method);
            continue;
        }
        if (crc32(content) != crc) {
            skipped += "; " + name + ": CRC-32 does not match";
            continue;
        }
        if (ctx.depth >= 2) continue;
        Identification inner;
        identify_into(content, name, inner, Ctx{ctx.depth + 1});
        for (Save& s : inner.saves) {
            s.location = s.location.empty() ? name : name + "/" + s.location;
            id.saves.push_back(std::move(s));
        }
    }
    if (id.saves.empty()) id.reason = "a ZIP without a save of this game" + skipped;
    else if (!skipped.empty()) id.reason = "entries skipped" + skipped;
    return true;
}

void identify_into(View bytes, const std::string& name, Identification& id, Ctx ctx) {
    if (try_zip(bytes, id, ctx) || try_max(bytes, id) || try_cbs(bytes, id) || try_xps(bytes, id) ||
        try_psv(bytes, id) || try_gci(bytes, "", id) || try_xbox(bytes, name, id) || try_card(bytes, "", id))
        return;
    id.reason = "not a save container this recognizer knows";
}

void finish(Identification& id, const std::string& source) {
    for (Save& s : id.saves) s.id = source + "|" + s.location;
    id.recognized = !id.saves.empty();
    if (!id.saves.empty()) {
        const Save& f = id.saves.front();
        id.platform = f.platform;
        id.container = f.container;
        id.region = f.region;
        id.valid = f.valid;
        if (id.reason.empty()) id.reason = f.reason;
        else id.reason = f.reason + " (" + id.reason + ")";
    }
}

}  // namespace

Identification identify_bytes(std::span<const std::uint8_t> bytes, const std::string& name) {
    Identification id;
    identify_into(bytes, name, id, Ctx{});
    finish(id, name);
    return id;
}

Identification identify(const std::filesystem::path& path) {
    Identification id;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        id.reason = "a folder: use scan()";
        return id;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        id.reason = "cannot be read";
        return id;
    }
    if (size > kMaxSourceBytes) {
        id.reason = "larger than any save container (" + std::to_string(size) + " bytes)";
        return id;
    }
    std::ifstream in(path, std::ios::binary);
    Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.eof() && in.fail()) {
        id.reason = "cannot be read";
        return id;
    }
    identify_into(bytes, path.generic_string(), id, Ctx{});
    finish(id, path.generic_string());
    return id;
}

std::vector<ScanEntry> scan(const std::filesystem::path& folder, std::size_t max_files) {
    std::vector<ScanEntry> found;
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(
             folder, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        files.push_back(it->path());
        if (files.size() >= max_files) break;
    }
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        Identification id = identify(p);
        if (id.recognized) found.push_back({p, std::move(id)});
    }
    return found;
}

bool extract_record(const Save& save, Record& out, std::string* error) {
    out = Record{};
    switch (save.platform) {
    case Platform::GameCube: {
        if (save.payload.size() < save_file::kGciHeaderBytes + save_file::kRecordOffset + save_file::kRecordBytes)
            return fail(error, "the GameCube file is too short for the record");
        const Bytes data(save.payload.begin() + save_file::kGciHeaderBytes, save.payload.end());
        out.options = save_file::check_record(data);
        out.bytes.assign(data.begin() + save_file::kRecordOffset,
                         data.begin() + save_file::kRecordOffset + save_file::kRecordBytes);
        static constexpr std::uint8_t kKey[8] = {0xEE, 0xB0, 0x6C, 0xAE, 0xA4, 0x78, 0x49, 0x90};
        save_file::des_ecb_decrypt(kKey, out.bytes.data(), 0x1F58);
        out.state = RecordState::Decoded;
        out.md5_ok = out.options.valid;
        return true;
    }
    case Platform::PlayStation2:
        if (save.payload.size() != kPs2DataBytes) return fail(error, "the PS2 data file was not unwrapped");
        out.bytes.assign(save.payload.begin(), save.payload.begin() + kPs2RecordBytes);
        out.state = RecordState::Encrypted;
        return true;
    case Platform::Xbox:
        if (save.payload.size() != kXboxSaveBytes) return fail(error, "the Xbox Save.dat was not read");
        out.bytes.assign(save.payload.begin() + kXboxHeaderBytes, save.payload.end());
        out.state = RecordState::Encrypted;
        return true;
    default:
        return fail(error, "not a save");
    }
}

bool to_pc_gci(const Save& save, save_file::Gci& out, std::string* error) {
    if (save.platform != Platform::GameCube)
        return fail(error, std::string(to_string(save.platform)) +
                               " saves are not converted: their record cannot be decrypted and its equivalence to "
                               "the GameCube record is not proven");
    if (!save.convertible) return fail(error, "not convertible: " + save.reason);
    if (save.payload.size() != save_file::kGciHeaderBytes + 2 * save_file::kBlockBytes)
        return fail(error, "the GameCube file is not 2 blocks");
    out = save_file::Gci{};
    std::copy_n(save.payload.begin(), save_file::kGciHeaderBytes, out.header.begin());
    out.data.assign(save.payload.begin() + save_file::kGciHeaderBytes, save.payload.end());
    return save_file::to_canonical_pc_gci(out, error);
}

}  // namespace gekko::save_recognize
