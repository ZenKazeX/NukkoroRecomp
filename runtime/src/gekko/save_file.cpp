// The game's save file on the host (gekko/save_file.h).

#include "gekko/save_file.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace gekko::save_file {

namespace {

// Whole-file read.
bool read_all(const std::filesystem::path& path, std::vector<std::uint8_t>& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = path.string() + ": cannot be opened";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        if (error != nullptr) *error = path.string() + ": cannot be read";
        return false;
    }
    return true;
}

// Durable replacement: <path>.tmp written, flushed to the disk, then moved over <path> (write-through), so an
// interrupted save leaves the old file or the new one, never a torn one (as gekko/host_files.h does for the card).
bool write_all_atomically(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
                          std::string* error) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const auto failed = [&](const char* what) {
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        if (error != nullptr) *error = path.string() + ": " + what;
        return false;
    };
#if defined(_WIN32)
    HANDLE h = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return failed("the temporary file cannot be created");
    DWORD written = 0;
    const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) return failed("the temporary file cannot be written");
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return failed("the temporary file cannot replace it");
#else
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return failed("the temporary file cannot be created");
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) return failed("the temporary file cannot be written");
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) return failed("the temporary file cannot replace it");
#endif
    return true;
}

std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

std::string text(const std::uint8_t* p, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n && p[i] != 0; ++i) s.push_back(static_cast<char>(p[i]));
    return s;
}

bool fail(std::string* error, std::string why) {
    if (error != nullptr) *error = std::move(why);
    return false;
}

// ---- DES (FIPS 46-3), bit 1 = the most significant bit -------------------------------------------------------------

constexpr std::uint8_t kIP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                                  62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                                  57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                                  61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
constexpr std::uint8_t kFP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                                  38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                                  36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                                  34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
constexpr std::uint8_t kE[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                                 12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                                 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
constexpr std::uint8_t kP[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                                 2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
constexpr std::uint8_t kPC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18,
                                   10, 2,  59, 51, 43, 35, 27, 19, 11, 3,  60, 52, 44, 36,
                                   63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22,
                                   14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
constexpr std::uint8_t kPC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                                   26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                                   51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
constexpr std::uint8_t kShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
constexpr std::uint8_t kS[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7,  0, 15, 7,  4,  14, 2,  13, 1,  10, 6, 12, 11,
     9,  5,  3,  8, 4,  1,  14, 8,  13, 6,  2,  11, 15, 12, 9, 7,  3, 10, 5,  0,  15, 12, 8,  2,  4,  9, 1,  7,
     5,  11, 3,  14, 10, 0, 6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7,  2,  13, 12, 0, 5,  10, 3,  13, 4,  7, 15, 2,  8,  14, 12, 0, 1,  10,
     6,  9,  11, 5,  0,  14, 7,  11, 10, 4,  13, 1,  5,  8, 12, 6,  9,  3,  2,  15, 13, 8, 10, 1,  3,  15, 4, 2,
     11, 6,  7,  12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6,  3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8, 13, 7, 0,  9, 3,  4,  6,  10, 2,  8,  5,  14,
     12, 11, 15, 1,  13, 6,  4,  9,  8,  15, 3,  0,  11, 1,  2,  12, 5, 10, 14, 7, 1,  10, 13, 0,  6,  9,  8,  7,
     4,  15, 14, 3,  11, 5,  2,  12},
    {7,  13, 14, 3,  0,  6,  9,  10, 1,  2, 8,  5,  11, 12, 4,  15, 13, 8,  11, 5,  6,  15, 0,  3,  4,  7,  2,  12,
     1,  10, 14, 9,  10, 6,  9,  0,  12, 11, 7, 13, 15, 1,  3,  14, 5,  2,  8,  4,  3,  15, 0,  6,  10, 1,  13, 8,
     9,  4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0, 14, 9, 14, 11, 2,  12, 4,  7,  13, 1,  5,  0,  15, 10,
     3,  9,  8,  6,  4,  2,  1,  11, 10, 13, 7,  8,  15, 9, 12, 5, 6,  3,  0,  14, 11, 8,  12, 7,  1,  14, 2,  13,
     6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9, 2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12, 9,  5,  6,  1,  13, 14,
     0,  11, 3,  8,  9, 14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10, 1,  13, 11, 6,  4,  3,  2,  12, 9,  5,  15, 10,
     11, 14, 1,  7,  6, 0,  8,  13},
    {4,  11, 2,  14, 15, 0,  8,  13, 3,  12, 9,  7,  5,  10, 6,  1, 13, 0, 11, 7,  4,  9,  1,  10, 14, 3,  5,  12,
     2,  15, 8,  6,  1,  4,  11, 13, 12, 3,  7,  14, 10, 15, 6,  8, 0, 5,  9,  2,  6,  11, 13, 8,  1,  4,  10, 7,
     9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2,  8,  4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7, 1,  15, 13, 8,  10, 3,  7,  4,  12, 5,  6,  11,
     0,  14, 9,  2,  7,  11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13, 15, 3, 5,  8,  2,  1,  14, 7,  4,  10, 8,  13,
     15, 12, 9,  0,  3,  5,  6,  11}};

std::uint64_t permute(std::uint64_t in, const std::uint8_t* table, int in_bits, int out_bits) {
    std::uint64_t out = 0;
    for (int i = 0; i < out_bits; ++i) out = out << 1 | ((in >> (in_bits - table[i])) & 1u);
    return out;
}

void key_schedule(const std::uint8_t key[8], std::uint64_t sub[16]) {
    std::uint64_t k = 0;
    for (int i = 0; i < 8; ++i) k = k << 8 | key[i];
    const std::uint64_t cd = permute(k, kPC1, 64, 56);
    std::uint32_t c = static_cast<std::uint32_t>(cd >> 28) & 0x0FFFFFFFu, d = static_cast<std::uint32_t>(cd) & 0x0FFFFFFFu;
    for (int r = 0; r < 16; ++r) {
        for (int s = 0; s < kShifts[r]; ++s) {
            c = ((c << 1) | (c >> 27)) & 0x0FFFFFFFu;
            d = ((d << 1) | (d >> 27)) & 0x0FFFFFFFu;
        }
        sub[r] = permute(static_cast<std::uint64_t>(c) << 28 | d, kPC2, 56, 48);
    }
}

std::uint32_t feistel(std::uint32_t r, std::uint64_t k) {
    const std::uint64_t e = permute(r, kE, 32, 48) ^ k;
    std::uint32_t out = 0;
    for (int j = 0; j < 8; ++j) {
        const unsigned six = static_cast<unsigned>(e >> (42 - 6 * j)) & 0x3Fu;
        const unsigned row = (six & 0x20u) >> 4 | (six & 1u), col = (six >> 1) & 0xFu;
        out = out << 4 | kS[j][row * 16u + col];
    }
    return static_cast<std::uint32_t>(permute(out, kP, 32, 32));
}

void des_blocks(const std::uint8_t key[8], std::uint8_t* data, std::size_t size, bool decrypt) {
    std::uint64_t sub[16];
    key_schedule(key, sub);
    for (std::size_t off = 0; off + 8 <= size; off += 8) {
        std::uint64_t b = 0;
        for (int i = 0; i < 8; ++i) b = b << 8 | data[off + i];
        const std::uint64_t ip = permute(b, kIP, 64, 64);
        std::uint32_t l = static_cast<std::uint32_t>(ip >> 32), r = static_cast<std::uint32_t>(ip);
        for (int round = 0; round < 16; ++round) {
            const std::uint32_t next = l ^ feistel(r, sub[decrypt ? 15 - round : round]);
            l = r;
            r = next;
        }
        const std::uint64_t out = permute(static_cast<std::uint64_t>(r) << 32 | l, kFP, 64, 64);
        for (int i = 0; i < 8; ++i) data[off + i] = static_cast<std::uint8_t>(out >> (56 - 8 * i));
    }
}

// ---- MD5 (RFC 1321) ---------------------------------------------------------------------------------------------

constexpr std::uint32_t kK[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr std::uint8_t kR[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

std::uint32_t rotl(std::uint32_t x, unsigned c) { return x << c | x >> (32u - c); }

}  // namespace

std::array<std::uint8_t, 16> md5(const std::uint8_t* data, std::size_t size, std::uint8_t pad_byte) {
    std::vector<std::uint8_t> m(data, data + size);
    m.push_back(pad_byte);
    while (m.size() % 64u != 56u) m.push_back(0);
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8u;
    for (int i = 0; i < 8; ++i) m.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    std::uint32_t a0 = 0x67452301u, b0 = 0xefcdab89u, c0 = 0x98badcfeu, d0 = 0x10325476u;
    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<std::uint32_t>(m[off + 4 * i]) | static_cast<std::uint32_t>(m[off + 4 * i + 1]) << 8 |
                   static_cast<std::uint32_t>(m[off + 4 * i + 2]) << 16 |
                   static_cast<std::uint32_t>(m[off + 4 * i + 3]) << 24;
        std::uint32_t a = a0, b = b0, c = c0, d = d0;
        for (unsigned j = 0; j < 64; ++j) {
            std::uint32_t f;
            unsigned g;
            if (j < 16) f = (b & c) | (~b & d), g = j;
            else if (j < 32) f = (d & b) | (~d & c), g = (5u * j + 1u) % 16u;
            else if (j < 48) f = b ^ c ^ d, g = (3u * j + 5u) % 16u;
            else f = c ^ (b | ~d), g = (7u * j) % 16u;
            f = f + a + kK[j] + w[g];
            a = d;
            d = c;
            c = b;
            b = b + rotl(f, kR[j]);
        }
        a0 += a, b0 += b, c0 += c, d0 += d;
    }
    std::array<std::uint8_t, 16> out{};
    const std::uint32_t words[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 16; ++i) out[i] = static_cast<std::uint8_t>(words[i / 4] >> (8 * (i % 4)));
    return out;
}

void des_ecb_decrypt(const std::uint8_t key[8], std::uint8_t* data, std::size_t size) {
    des_blocks(key, data, size, true);
}
void des_ecb_encrypt(const std::uint8_t key[8], std::uint8_t* data, std::size_t size) {
    des_blocks(key, data, size, false);
}

std::string Gci::game_code() const { return text(header.data(), 4); }
std::string Gci::maker_code() const { return text(header.data() + 4, 2); }
std::string Gci::file_name() const { return text(header.data() + 8, 32); }
std::uint16_t Gci::block_count() const { return be16(header.data() + 0x38); }

bool read_gci(const std::filesystem::path& path, Gci& out, std::string* error) {
    std::vector<std::uint8_t> bytes;
    if (!read_all(path, bytes, error)) return false;
    if (bytes.size() < kGciHeaderBytes) return fail(error, "shorter than a GCI header");
    std::copy_n(bytes.begin(), kGciHeaderBytes, out.header.begin());
    const std::size_t blocks = out.block_count();
    if (blocks == 0u || bytes.size() != kGciHeaderBytes + blocks * kBlockBytes)
        return fail(error, "its size does not match its " + std::to_string(blocks) + " blocks");
    out.data.assign(bytes.begin() + kGciHeaderBytes, bytes.end());
    return true;
}

bool write_gci(const std::filesystem::path& path, const Gci& gci, std::string* error) {
    if (gci.data.empty() || gci.data.size() % kBlockBytes != 0u) return fail(error, "not whole 8 KiB blocks");
    std::vector<std::uint8_t> bytes(gci.header.begin(), gci.header.end());
    bytes.insert(bytes.end(), gci.data.begin(), gci.data.end());
    return write_all_atomically(path, bytes, error);
}

bool to_canonical_pc_gci(Gci& gci, std::string* error) {
    if (gci.data.size() != 2u * kBlockBytes) return fail(error, "the GameCube file is not 2 blocks");
    if (!check_record(gci.data).valid) return fail(error, "the record's MD5 does not match");
    const auto put16 = [&](std::size_t at, std::uint16_t v) {
        gci.header[at] = static_cast<std::uint8_t>(v >> 8);
        gci.header[at + 1] = static_cast<std::uint8_t>(v);
    };
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        put16(at, static_cast<std::uint16_t>(v >> 16));
        put16(at + 2, static_cast<std::uint16_t>(v));
    };
    const std::uint32_t modified = be32(gci.header.data() + 0x28);
    std::fill(gci.header.begin(), gci.header.end(), std::uint8_t{0});
    static constexpr char kId[] = "GUPP8P";
    static constexpr char kName[] = "SHADOWTHEHEDGEHOG";
    std::copy_n(kId, 6, gci.header.begin());
    gci.header[6] = 0xFF;
    gci.header[7] = 0x02;
    std::copy_n(kName, sizeof(kName) - 1, gci.header.begin() + 8);
    put32(0x28, modified);
    put32(0x2C, 0x40u);
    put16(0x30, 0x0002u);
    put16(0x32, 0x0002u);
    gci.header[0x34] = 0x04;
    gci.header[0x35] = 0x00;
    put16(0x36, 5u);
    put16(0x38, 2u);
    put16(0x3A, 0xFFFFu);
    put32(0x3C, 0u);
    return true;
}

bool extract_from_card(const std::vector<std::uint8_t>& card, const std::string& game, const std::string& maker,
                       const std::string& name, Gci& out, std::string* error) {
    if (card.size() < 5u * kBlockBytes || card.size() % kBlockBytes != 0u) return fail(error, "not a card image");
    const std::size_t total = card.size() / kBlockBytes;
    // The CARD library's checksum pair over 16-bit big-endian words (0xFFFF becomes 0).
    const auto sums = [&](std::size_t off, std::size_t n, std::uint16_t* a, std::uint16_t* b) {
        std::uint32_t t = 0, c = 0;
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            const std::uint16_t w = be16(card.data() + off + i);
            t = (t + w) & 0xFFFFu;
            c = (c + (w ^ 0xFFFFu)) & 0xFFFFu;
        }
        *a = static_cast<std::uint16_t>(t == 0xFFFFu ? 0 : t);
        *b = static_cast<std::uint16_t>(c == 0xFFFFu ? 0 : c);
    };
    // The current copy: valid checksums, the higher update counter (compared as the library does, s16 wrap).
    int dir = -1, bat = -1;
    {
        std::int16_t best = 0;
        for (int i = 1; i <= 2; ++i) {
            const std::size_t off = static_cast<std::size_t>(i) * kBlockBytes;
            std::uint16_t a = 0, b = 0;
            sums(off, 0x1FFC, &a, &b);
            if (a != be16(card.data() + off + 0x1FFC) || b != be16(card.data() + off + 0x1FFE)) continue;
            const auto counter = static_cast<std::int16_t>(be16(card.data() + off + 0x1FFA));
            if (dir < 0 || static_cast<std::int16_t>(counter - best) > 0) dir = i, best = counter;
        }
    }
    {
        std::int16_t best = 0;
        for (int i = 3; i <= 4; ++i) {
            const std::size_t off = static_cast<std::size_t>(i) * kBlockBytes;
            std::uint16_t a = 0, b = 0;
            sums(off + 4, kBlockBytes - 4, &a, &b);
            if (a != be16(card.data() + off) || b != be16(card.data() + off + 2)) continue;
            const auto counter = static_cast<std::int16_t>(be16(card.data() + off + 4));
            if (bat < 0 || static_cast<std::int16_t>(counter - best) > 0) bat = i, best = counter;
        }
    }
    if (dir < 0 || bat < 0) return fail(error, "no valid directory or allocation table");
    const std::uint8_t* d = card.data() + static_cast<std::size_t>(dir) * kBlockBytes;
    const std::uint8_t* t = card.data() + static_cast<std::size_t>(bat) * kBlockBytes;
    for (int e = 0; e < 127; ++e) {
        const std::uint8_t* entry = d + e * 0x40;
        if (be32(entry) == 0xFFFFFFFFu) continue;
        if (text(entry, 4) != game || text(entry + 4, 2) != maker || text(entry + 8, 32) != name) continue;
        const std::uint16_t length = be16(entry + 0x38);
        std::uint16_t block = be16(entry + 0x36);
        std::vector<std::uint8_t> data;
        for (std::uint16_t i = 0; i < length; ++i) {
            if (block < 5u || block >= total) return fail(error, "its block chain leaves the card");
            data.insert(data.end(), card.begin() + static_cast<std::ptrdiff_t>(block * kBlockBytes),
                        card.begin() + static_cast<std::ptrdiff_t>((block + 1u) * kBlockBytes));
            const std::uint16_t next = be16(t + 0x0A + (block - 5u) * 2u);
            if (i + 1u < length && (next == 0xFFFFu || next == 0u)) return fail(error, "its block chain is short");
            block = next;
        }
        std::copy_n(entry, kGciHeaderBytes, out.header.begin());
        out.data = std::move(data);
        return true;
    }
    return fail(error, "the card holds no " + game + " " + name);
}

RecordCheck check_record(const std::vector<std::uint8_t>& file_data) {
    RecordCheck r;
    if (file_data.size() < kRecordOffset + kRecordBytes) return r;
    std::vector<std::uint8_t> rec(file_data.begin() + kRecordOffset, file_data.begin() + kRecordOffset + kRecordBytes);
    static constexpr std::uint8_t kKey[8] = {0xEE, 0xB0, 0x6C, 0xAE, 0xA4, 0x78, 0x49, 0x90};
    des_ecb_decrypt(kKey, rec.data(), 0x1F58);
    const std::array<std::uint8_t, 16> sum = md5(rec.data() + 0x10, kRecordBytes - 0x10, 0x00);
    r.valid = std::equal(sum.begin(), sum.end(), rec.begin());
    r.text_language = be32(rec.data() + 0x18);
    r.voice_language = be32(rec.data() + 0x1C);
    r.speaker_mode = be32(rec.data() + 0x20);
    r.vibration = rec[0x24];
    r.stamp = static_cast<std::uint64_t>(be32(rec.data() + 0x1774)) << 32 | be32(rec.data() + 0x1778);
    return r;
}

}  // namespace gekko::save_file
