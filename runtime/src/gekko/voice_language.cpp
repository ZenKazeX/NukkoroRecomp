// Japanese voices for the PAL game. Contract: include/gekko/voice_language.h.

#include "gekko/voice_language.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>

namespace gekko {

namespace {

std::atomic<bool> g_available{false};

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// The Japanese twins the PAL tree's English voice files call for: sng_X_E.adx -> sng_X_J.adx, plus PRS_VOICE_J.afs.
std::vector<std::string> wanted_names(const std::filesystem::path& files) {
    std::vector<std::string> out{"PRS_VOICE_J.afs"};
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(files, ec)) {
        const std::string name = item.path().filename().string();
        if (name.rfind("sng_", 0) == 0 && ends_with(name, "_E.adx"))
            out.push_back(name.substr(0, name.size() - 6) + "_J.adx");  // cutscene voices (sng_EV*_E.adx)
        else if (name.rfind("sng_", 0) == 0 && ends_with(name, "_e.adx"))
            out.push_back(name.substr(0, name.size() - 6) + "_j.adx");  // power-move calls (sng_jin_cblast_e.adx ...)
    }
    return out;
}

std::vector<JapaneseBank> g_banks;
VoiceArchivePaths g_archives;

constexpr const char* kBankNames[] = {"B3_SHADOW_J.mlt", "B4_SUBHERO_J.mlt", "B5_SUBDARK_J.mlt"};

std::uint32_t be32(const std::vector<unsigned char>& d, size_t at) {
    return (std::uint32_t(d[at]) << 24) | (std::uint32_t(d[at + 1]) << 16) | (std::uint32_t(d[at + 2]) << 8) |
           std::uint32_t(d[at + 3]);
}

// The program unit ("gcaxMPBP" payload) of a bank: {offset, size}, or size 0 when the file is not a bank.
std::pair<size_t, size_t> program_unit(const std::vector<unsigned char>& d) {
    static const char kWave[] = "gcaxMPBW", kProgram[] = "gcaxMPBP";
    auto find = [&d](const char* tag, size_t from) -> size_t {
        for (size_t i = from; i + 16 <= d.size(); ++i)
            if (std::memcmp(d.data() + i, tag, 8) == 0) return i;
        return size_t(-1);
    };
    if (d.size() < 0x60 || std::memcmp(d.data(), "gcaxMLT ", 8) != 0) return {0, 0};
    const size_t w = find(kWave, 0);
    if (w == size_t(-1)) return {0, 0};
    const size_t p = find(kProgram, w + 16);
    if (p == size_t(-1) || p + 16 > d.size()) return {0, 0};
    const size_t size = be32(d, p + 12);  // tag, u32 0, u32 size
    if (p + 16 + size > d.size()) return {0, 0};
    return {p + 16, size};
}

}  // namespace

bool japanese_bank_compatible(const std::vector<unsigned char>& japanese, const std::vector<unsigned char>& pal_english) {
    const auto [jo, js] = program_unit(japanese);
    const auto [eo, es] = program_unit(pal_english);
    // Same program layout as the PAL English bank the game's AudioData rows were authored against: same program
    // unit size and identical first 64 bytes (header, program table); only wave offsets/lengths differ.
    return js != 0 && js == es && js >= 64 && std::memcmp(japanese.data() + jo, pal_english.data() + eo, 64) == 0;
}

const std::vector<JapaneseBank>& japanese_banks() noexcept { return g_banks; }

std::vector<std::filesystem::path> find_japanese_voice_files(const std::filesystem::path& game_root) {
    g_banks.clear();
    g_archives = {};
    const std::filesystem::path files = game_root / "files";
    const std::vector<std::string> wanted = wanted_names(files);
    const std::filesystem::path candidates[] = {game_root / "voice_jp",
                                                game_root.parent_path() / "EXTRACTED_DISKS" / "Japan" / "files"};
    for (const std::filesystem::path& dir : candidates) {
        std::vector<std::filesystem::path> found;
        std::error_code ec;
        for (const std::string& name : wanted) {
            const std::filesystem::path p = dir / name;
            if (!std::filesystem::is_regular_file(p, ec)) {
                found.clear();
                break;
            }
            found.push_back(p);
        }
        if (found.empty()) continue;
        // The Japanese disc's own character voice banks (see the header); optional, all three or none.
        std::vector<JapaneseBank> banks;
        for (const char* name : kBankNames) {
            const std::filesystem::path p = dir / "sound" / name;
            if (std::filesystem::is_regular_file(p, ec)) banks.push_back({std::string("sound/") + name, p});
        }
        if (banks.size() == std::size(kBankNames)) g_banks = std::move(banks);
        g_archives.english = files / "PRS_VOICE_E.afs";
        g_archives.japanese = dir / "PRS_VOICE_J.afs";
        return found;
    }
    return {};
}

void set_japanese_voices_available(bool available) noexcept { g_available.store(available, std::memory_order_relaxed); }
bool japanese_voices_available() noexcept { return g_available.load(std::memory_order_relaxed); }

const VoiceArchivePaths& voice_archive_paths() noexcept { return g_archives; }

namespace {

using u64 = unsigned long long;
constexpr u64 kAlign = 2048u;  // both discs' archives align every entry to 2048
u64 align_up(u64 v) { return (v + kAlign - 1u) / kAlign * kAlign; }
std::uint32_t le32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
void put_le32(unsigned char* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
bool read_at(std::FILE* f, u64 at, unsigned char* dest, u64 length) {
#if defined(_WIN32)
    if (_fseeki64(f, static_cast<long long>(at), SEEK_SET) != 0) return false;
#else
    if (fseeko(f, static_cast<off_t>(at), SEEK_SET) != 0) return false;
#endif
    return std::fread(dest, 1, static_cast<std::size_t>(length), f) == length;
}
std::FILE* open_read(const std::filesystem::path& p) {
#if defined(_WIN32)
    return _wfopen(p.c_str(), L"rb");
#else
    return std::fopen(p.c_str(), "rb");
#endif
}

}  // namespace

bool VoiceArchive::open(const VoiceArchivePaths& paths, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const std::filesystem::path* p[2] = {&paths.english, &paths.japanese};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> toc[2];
    std::uint32_t name_table[2][2] = {};
    for (int i = 0; i < 2; ++i) {
        std::FILE* f = open_read(*p[i]);
        if (f == nullptr) return fail("cannot open " + p[i]->string());
        files_[i] = f;
        unsigned char head[8];
        if (!read_at(f, 0, head, 8) || std::memcmp(head, "AFS", 4) != 0) return fail(p[i]->string() + " is not an AFS");
        const std::uint32_t count = le32(head + 4);
        std::vector<unsigned char> table(8u * count + 8u);
        if (count == 0u || !read_at(f, 8, table.data(), table.size())) return fail(p[i]->string() + ": bad table");
        for (std::uint32_t e = 0; e < count; ++e) toc[i].emplace_back(le32(&table[8u * e]), le32(&table[8u * e + 4u]));
        name_table[i][0] = le32(&table[8u * count]);
        name_table[i][1] = le32(&table[8u * count + 4u]);
    }
    if (toc[0].size() != toc[1].size()) return fail("the two voice archives differ in entry count");
    const u64 count = toc[0].size();
    // The composed layout: the head where the English archive has it, then one slot per entry.
    const u64 first = std::min<u64>(toc[0].front().first, toc[1].front().first);
    if (first < 8u + 8u * count + 8u) return fail("the voice archive's table overlaps its first entry");
    u64 at = std::max<u64>(toc[0].front().first, toc[1].front().first);
    entries_.resize(static_cast<std::size_t>(count));
    for (u64 e = 0; e < count; ++e) {
        Entry& entry = entries_[static_cast<std::size_t>(e)];
        for (int i = 0; i < 2; ++i) {
            entry.source[i] = toc[i][static_cast<std::size_t>(e)].first;
            entry.source_size[i] = toc[i][static_cast<std::size_t>(e)].second;
        }
        entry.slot = at;
        entry.slot_size = std::max(entry.source_size[0], entry.source_size[1]);
        at += align_up(std::max<u64>(entry.slot_size, 1u));
    }
    // The English name table follows the last slot (the game looks entries up by index).
    names_.resize(name_table[0][1]);
    if (!names_.empty() && !read_at(static_cast<std::FILE*>(files_[0]), name_table[0][0], names_.data(), names_.size()))
        return fail("cannot read the English name table");
    names_at_ = at;
    size_ = align_up(names_at_ + names_.size());
    if (size_ > 0xFFFFFFFFull) return fail("the composed voice archive would pass 4 GiB");
    head_.assign(static_cast<std::size_t>(entries_.front().slot), 0u);
    std::memcpy(head_.data(), "AFS", 4);
    put_le32(&head_[4], static_cast<std::uint32_t>(count));
    for (u64 e = 0; e < count; ++e) {
        put_le32(&head_[8u + 8u * e], static_cast<std::uint32_t>(entries_[static_cast<std::size_t>(e)].slot));
        put_le32(&head_[12u + 8u * e], static_cast<std::uint32_t>(entries_[static_cast<std::size_t>(e)].slot_size));
    }
    put_le32(&head_[8u + 8u * count], static_cast<std::uint32_t>(names_at_));
    put_le32(&head_[12u + 8u * count], static_cast<std::uint32_t>(names_.size()));
    lock_ = new std::mutex();
    return true;
}

VoiceArchive::~VoiceArchive() {
    for (void*& f : files_)
        if (f != nullptr) std::fclose(static_cast<std::FILE*>(f));
    delete static_cast<std::mutex*>(lock_);
}

bool VoiceArchive::read(u64 offset, u64 length, unsigned char* dest, u64& got, bool japanese) {
    got = 0;
    if (dest == nullptr || lock_ == nullptr) return false;
    std::memset(dest, 0, static_cast<std::size_t>(length));
    const u64 end = offset + length;
    // The head.
    if (offset < head_.size()) {
        const u64 n = std::min<u64>(end, head_.size()) - offset;
        std::memcpy(dest, head_.data() + offset, static_cast<std::size_t>(n));
    }
    // The name table.
    if (end > names_at_ && offset < names_at_ + names_.size()) {
        const u64 from = std::max(offset, names_at_), to = std::min<u64>(end, names_at_ + names_.size());
        std::memcpy(dest + (from - offset), names_.data() + (from - names_at_), static_cast<std::size_t>(to - from));
    }
    // The slots the range touches: the first whose slot ends after `offset`.
    auto it = std::upper_bound(entries_.begin(), entries_.end(), offset,
                               [](u64 o, const Entry& e) { return o < e.slot; });
    if (it != entries_.begin()) --it;
    const int which = japanese ? 1 : 0;
    std::lock_guard<std::mutex> guard(*static_cast<std::mutex*>(lock_));
    for (; it != entries_.end() && it->slot < end; ++it) {
        const u64 clip_end = it->slot + it->source_size[which];  // zeros from here to the slot's end
        const u64 from = std::max(offset, it->slot), to = std::min(end, clip_end);
        if (from >= to) continue;
        if (!read_at(static_cast<std::FILE*>(files_[which]), it->source[which] + (from - it->slot), dest + (from - offset),
                     to - from))
            return false;
    }
    got = length;
    return true;
}

}  // namespace gekko
