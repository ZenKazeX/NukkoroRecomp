// Installing mods (shadow/mods/install.h).
#include "shadow/mods/install.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// stb_image's zlib inflater, private to this file (STB_IMAGE_STATIC): zip entries use raw deflate.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505 4244 4100 4456 4457)
#endif
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace shadow::mods {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMaxEntries = 200000;
constexpr std::uint64_t kMaxTotal = 16ull << 30;

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint64_t le64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(le32(p)) | static_cast<std::uint64_t>(le32(p + 4)) << 32;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// IBM code page 437, 0x80..0xFF (zip names without the UTF-8 flag).
const char16_t kCp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC,
    0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2,
    0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310,
    0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C,
    0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF,
    0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2,
    0x25A0, 0x00A0};

std::string cp437_to_utf8(std::string_view s) {
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) {
            out.push_back(ch);
            continue;
        }
        const char16_t u = kCp437[c - 0x80];
        if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

// An archive entry name as safe relative components; empty on anything that could leave the destination.
std::vector<std::string> safe_components(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    std::vector<std::string> parts;
    if (!name.empty() && name[0] == '/') return {};
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t slash = name.find('/', start);
        std::string part = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") return {};
        if (part.find(':') != std::string::npos) return {};
        // Windows drops trailing dots and spaces; a name made only of them is refused.
        while (!part.empty() && (part.back() == '.' || part.back() == ' ')) part.pop_back();
        if (!part.empty() && part != ".") parts.push_back(part);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    for (const std::string& p : parts)
        for (const char c : p)
            if (static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' ||
                c == '*')
                return {};
    return parts;
}

fs::path join(const fs::path& base, const std::vector<std::string>& parts) {
    fs::path p = base;
    for (const std::string& s : parts) p /= from_utf8(s);
    return p;
}

bool extract_zip(const fs::path& archive, const fs::path& dest, std::string& error, bool& unsupported_method) {
    unsupported_method = false;
    std::ifstream in(archive, std::ios::binary);
    if (!in) {
        error = "cannot open " + utf8(archive);
        return false;
    }
    std::error_code ec;
    const std::uint64_t size = fs::file_size(archive, ec);
    if (size < 22) {
        error = "not a zip archive";
        return false;
    }
    const std::uint64_t tail_size = std::min<std::uint64_t>(size, 65536 + 22);
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(tail_size));
    in.seekg(static_cast<std::streamoff>(size - tail_size));
    in.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    std::int64_t eocd = -1;
    for (std::int64_t i = static_cast<std::int64_t>(tail.size()) - 22; i >= 0; --i)
        if (le32(tail.data() + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    if (eocd < 0) {
        error = "not a zip archive (no end of central directory)";
        return false;
    }
    std::uint64_t count = le16(tail.data() + eocd + 10);
    std::uint64_t cd_size = le32(tail.data() + eocd + 12);
    std::uint64_t cd_offset = le32(tail.data() + eocd + 16);
    if (eocd >= 20 && le32(tail.data() + eocd - 20) == 0x07064b50u) {  // zip64 locator
        const std::uint64_t z64 = le64(tail.data() + eocd - 20 + 8);
        std::uint8_t rec[56];
        in.clear();
        in.seekg(static_cast<std::streamoff>(z64));
        if (in.read(reinterpret_cast<char*>(rec), 56) && le32(rec) == 0x06064b50u) {
            count = le64(rec + 32);
            cd_size = le64(rec + 40);
            cd_offset = le64(rec + 48);
        }
    }
    if (count > kMaxEntries) {
        error = "the archive has more than 200,000 entries";
        return false;
    }
    if (cd_offset + cd_size > size || cd_size > (256u << 20)) {
        error = "damaged zip archive (central directory outside the file)";
        return false;
    }
    std::vector<std::uint8_t> cd(static_cast<std::size_t>(cd_size));
    in.clear();
    in.seekg(static_cast<std::streamoff>(cd_offset));
    if (!in.read(reinterpret_cast<char*>(cd.data()), static_cast<std::streamsize>(cd.size()))) {
        error = "damaged zip archive (truncated central directory)";
        return false;
    }
    std::uint64_t total = 0;
    std::size_t at = 0;
    for (std::uint64_t n = 0; n < count; ++n) {
        if (at + 46 > cd.size() || le32(cd.data() + at) != 0x02014b50u) {
            error = "damaged zip archive (bad central directory entry)";
            return false;
        }
        const std::uint8_t* h = cd.data() + at;
        const std::uint16_t flags = le16(h + 8);
        const std::uint16_t method = le16(h + 10);
        const std::uint32_t crc = le32(h + 16);
        std::uint64_t csize = le32(h + 20);
        std::uint64_t usize = le32(h + 24);
        const std::uint16_t name_len = le16(h + 28), extra_len = le16(h + 30), comment_len = le16(h + 32);
        const std::uint32_t external = le32(h + 38);
        std::uint64_t local = le32(h + 42);
        if (at + 46u + name_len + extra_len + comment_len > cd.size()) {
            error = "damaged zip archive (entry runs past the directory)";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(h + 46), name_len);
        // zip64 extra field: the 0xFFFFFFFF fields, in order usize, csize, local.
        for (std::size_t e = 0; e + 4 <= extra_len;) {
            const std::uint8_t* x = h + 46 + name_len + e;
            const std::uint16_t id = le16(x), len = le16(x + 2);
            if (id == 0x0001) {
                std::size_t k = 4;
                if (usize == 0xFFFFFFFFu && k + 8 <= 4u + len) { usize = le64(x + k); k += 8; }
                if (csize == 0xFFFFFFFFu && k + 8 <= 4u + len) { csize = le64(x + k); k += 8; }
                if (local == 0xFFFFFFFFu && k + 8 <= 4u + len) { local = le64(x + k); k += 8; }
            }
            e += 4u + len;
        }
        at += 46u + name_len + extra_len + comment_len;
        if (!(flags & 0x0800)) name = cp437_to_utf8(name);
        const bool directory = !name.empty() && (name.back() == '/' || name.back() == '\\');
        const bool symlink = ((external >> 16) & 0170000u) == 0120000u;
        if (symlink) {
            error = "the archive contains a symbolic link (" + name + "); refused";
            return false;
        }
        const std::vector<std::string> parts = safe_components(name);
        if (parts.empty()) {
            if (directory) continue;
            error = "the archive has an unsafe entry name: " + name;
            return false;
        }
        const fs::path target = join(dest, parts);
        if (directory) {
            fs::create_directories(target, ec);
            continue;
        }
        if (flags & 0x0001) {
            error = "the archive is encrypted (" + name + ")";
            return false;
        }
        if (method != 0 && method != 8) {
            unsupported_method = true;
            error = "zip compression method " + std::to_string(method) + " (" + name + ")";
            return false;
        }
        total += usize;
        if (total > kMaxTotal || usize > 0x7FFFFFFFull || csize > 0x7FFFFFFFull) {
            error = "the archive unpacks to more than the installer accepts (" + name + ")";
            return false;
        }
        std::uint8_t lh[30];
        in.clear();
        in.seekg(static_cast<std::streamoff>(local));
        if (!in.read(reinterpret_cast<char*>(lh), 30) || le32(lh) != 0x04034b50u) {
            error = "damaged zip archive (bad local header for " + name + ")";
            return false;
        }
        const std::uint64_t data_at = local + 30u + le16(lh + 26) + le16(lh + 28);
        if (data_at + csize > size) {
            error = "damaged zip archive (" + name + " runs past the end)";
            return false;
        }
        std::vector<std::uint8_t> packed(static_cast<std::size_t>(csize));
        in.seekg(static_cast<std::streamoff>(data_at));
        if (csize && !in.read(reinterpret_cast<char*>(packed.data()), static_cast<std::streamsize>(csize))) {
            error = "cannot read " + name;
            return false;
        }
        std::vector<std::uint8_t> data;
        if (method == 0) {
            data = std::move(packed);
            if (data.size() != usize) {
                error = "damaged zip archive (" + name + " has the wrong size)";
                return false;
            }
        } else {
            data.resize(static_cast<std::size_t>(usize));
            const int got = usize == 0 ? 0
                                       : stbi_zlib_decode_noheader_buffer(
                                             reinterpret_cast<char*>(data.data()), static_cast<int>(usize),
                                             reinterpret_cast<const char*>(packed.data()), static_cast<int>(csize));
            if (got < 0 || static_cast<std::uint64_t>(got) != usize) {
                error = "damaged zip archive (" + name + " does not inflate)";
                return false;
            }
        }
        if (crc32(data.data(), data.size()) != crc) {
            error = "damaged zip archive (" + name + " fails its checksum)";
            return false;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            error = "cannot write " + utf8(target);
            return false;
        }
    }
    return true;
}

#if defined(_WIN32)
// Windows' own tar.exe (libarchive): 7z, rar, tar.* and zip variants the native reader does not take.
bool extract_with_system_tar(const fs::path& archive, const fs::path& dest, std::string& error) {
    wchar_t system[MAX_PATH];
    const UINT n = GetSystemDirectoryW(system, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        error = "cannot find the Windows system directory";
        return false;
    }
    const fs::path tar = fs::path(system) / L"tar.exe";
    std::error_code ec;
    if (!fs::is_regular_file(tar, ec)) {
        error = "this archive type needs Windows' tar.exe (Windows 10 1803 or later); extract it and install the folder";
        return false;
    }
    fs::create_directories(dest, ec);
    // tar.exe takes plain paths (it handles long names inside the archive itself).
    const auto plain = [](const fs::path& p) { return from_utf8(utf8(p)).make_preferred().wstring(); };
    std::wstring cmd = L"\"" + tar.wstring() + L"\" -x -f \"" + plain(archive) + L"\" -C \"" + plain(dest) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    if (!CreateProcessW(tar.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        error = "cannot run tar.exe";
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        error = "tar.exe could not extract " + utf8(archive.filename()) + " (damaged or unsupported archive)";
        return false;
    }
    return true;
}
#endif

// Walks `root`: refuses links and junctions (an extractor might have made one) and counts.
bool check_tree(const fs::path& root, std::string& error) {
    std::error_code ec;
    std::uint64_t count = 0;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::file_status s = it->symlink_status(ec);
        if (s.type() != fs::file_type::regular && s.type() != fs::file_type::directory) {
            error = "the download contains a link or special file (" + utf8(it->path()) + "); refused";
            return false;
        }
        if (++count > kMaxEntries) {
            error = "the download has more than 200,000 files";
            return false;
        }
    }
    if (ec) {
        error = "cannot list " + utf8(root) + ": " + ec.message();
        return false;
    }
    return true;
}

std::string random_tag() {
    std::random_device rd;
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%08x%08x", rd(), rd());
    return buf;
}

struct Staged {
    std::vector<std::string> parts;  // relative components
    fs::path path;
};

void list_tree(const fs::path& root, std::vector<Staged>& out) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        Staged s;
        s.path = it->path();
        for (const fs::path& p : fs::relative(it->path(), root, ec)) s.parts.push_back(utf8(p));
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const Staged& a, const Staged& b) { return a.parts < b.parts; });
}

std::string joined(const std::vector<std::string>& parts, std::size_t from, std::size_t to) {
    std::string s;
    for (std::size_t i = from; i < to && i < parts.size(); ++i) s += (s.empty() ? "" : "/") + parts[i];
    return s;
}

bool is_doc(const std::string& ext) {
    static const char* const kDocs[] = {".txt", ".md", ".pdf", ".png", ".jpg", ".jpeg", ".gif", ".bmp",
                                        ".webp", ".rtf", ".htm", ".html", ".url", ".json", ".nfo", ".doc", ".docx"};
    return std::find(std::begin(kDocs), std::end(kDocs), ext) != std::end(kDocs);
}

bool is_image(const std::string& ext) { return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp"; }

// Programs (a randomizer, an editor, a launcher, their libraries and scripts): never placed anywhere, never run.
bool is_program(const std::string& ext) {
    static const char* const kPrograms[] = {".exe", ".dll", ".com", ".msi", ".bat", ".cmd", ".ps1",
                                            ".vbs", ".js",  ".jar",  ".sh",  ".so",  ".dylib", ".appimage",
                                            ".pdb", ".sys", ".cpl"};
    return std::find(std::begin(kPrograms), std::end(kPrograms), ext) != std::end(kPrograms);
}

// A GameCube save: a GCI (the card's 64-byte directory entry, then the file's 8 KiB blocks), the GameShark / Action
// Replay (.gcs) and MaxDrive (.sav) wrappings of one, or a raw memory-card image. Empty when `file` is none of
// these; otherwise a description ("game GUPR, maker 8P, file \"SHADOWRELOADED\"") and, for a GCI, whether it is this
// game's own save file ("SHADOWTHEHEDGEHOG" of a GUP* disc, the only file the port's save store reads).
struct SaveKind {
    std::string what;
    bool games_own = false;
};
SaveKind save_kind(const fs::path& file, const std::string& ext) {
    SaveKind k;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(long_path(file), ec);
    if (ec) return k;
    if (ext == ".raw") {
        // 59 to 2043 usable blocks: 512 KiB .. 16 MiB, a whole number of 256 KiB.
        if (size >= 0x80000 && size <= 0x1000000 && size % 0x40000 == 0) k.what = "a GameCube memory-card image";
        return k;
    }
    if (ext != ".gci" && ext != ".gcs" && ext != ".sav") return k;
    k.what = "a GameCube save";
    if (ext != ".gci" || size < 0x40) return k;
    std::array<std::uint8_t, 0x40> h{};
    std::ifstream f(long_path(file), std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(h.data()), static_cast<std::streamsize>(h.size()))) return k;
    const auto text = [&](std::size_t at, std::size_t n) {
        std::string s;
        for (std::size_t i = at; i < at + n && h[i] != 0; ++i) s += std::isprint(h[i]) ? static_cast<char>(h[i]) : '?';
        return s;
    };
    const std::string game = text(0, 4), maker = text(4, 2), name = text(8, 32);
    const std::uintmax_t blocks = static_cast<std::uintmax_t>(h[0x38]) << 8 | h[0x39];
    if (size != 0x40 + blocks * 0x2000) return k;  // not a GCI after all: keep the generic words
    k.what = "a GameCube save (game " + game + ", maker " + maker + ", file \"" + name + "\")";
    k.games_own = game.rfind("GUP", 0) == 0 && name == "SHADOWTHEHEDGEHOG";
    return k;
}

bool copy_file(const fs::path& from, const fs::path& to_any, std::string& error) {
    std::error_code ec;
    const fs::path to = long_path(to_any);  // '/' from a disc path becomes a backslash, which the long form needs
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(long_path(from), to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "cannot copy " + utf8(from) + ": " + ec.message();
        return false;
    }
    return true;
}

// Text of a readme for the manifest's description: trimmed, at most ~800 characters, valid UTF-8 only (a file in
// another encoding contributes its ASCII).
std::string readme_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.erase(0, 3);
    // Markdown readmes: HTML tags and heading marks are noise in a plain description; runs of blank lines collapse.
    {
        std::string plain;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '<') {
                const std::size_t close = text.find('>', i);
                if (close != std::string::npos && text.find('\n', i) > close) {
                    i = close;
                    continue;
                }
            }
            if (text[i] == '#' && (plain.empty() || plain.back() == '\n')) {
                while (i < text.size() && text[i] == '#') ++i;
                while (i < text.size() && text[i] == ' ') ++i;
                --i;
                continue;
            }
            if (text[i] == '\r') continue;
            if (text[i] == '\n' && plain.size() >= 2 && plain[plain.size() - 1] == '\n' && plain[plain.size() - 2] == '\n')
                continue;
            if (text[i] == '\n') {
                while (!plain.empty() && plain.back() == ' ') plain.pop_back();
            }
            plain.push_back(text[i]);
        }
        text = std::move(plain);
    }
    std::string out;
    for (std::size_t i = 0; i < text.size() && out.size() < 800;) {
        const auto c = static_cast<unsigned char>(text[i]);
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && i + static_cast<std::size_t>(len) <= text.size();
        for (int k = 1; ok && k < len; ++k) ok = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
        if (!ok) {
            ++i;
            continue;
        }
        if (c == '\r') {
            ++i;
            continue;
        }
        if (c < 0x20 && c != '\n' && c != '\t') {
            ++i;
            continue;
        }
        out.append(text, i, static_cast<std::size_t>(len));
        i += static_cast<std::size_t>(len);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    std::size_t start = 0;
    while (start < out.size() && (out[start] == '\n' || out[start] == ' ')) ++start;
    return out.substr(start);
}

std::string pretty_name(std::string stem) {
    // GameBanana appends "_<5 hex>" to uploads ("maria_fixed_eyes_218d1"); drop it.
    if (stem.size() > 6 && stem[stem.size() - 6] == '_' &&
        std::all_of(stem.end() - 5, stem.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }))
        stem.resize(stem.size() - 6);
    for (char& c : stem)
        if (c == '_') c = ' ';
    bool start = true;
    for (char& c : stem) {
        if (start && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        start = c == ' ' || c == '-';
    }
    return stem;
}

}  // namespace

bool is_archive(const fs::path& file) {
    const std::string ext = lower(file.extension().string());
    static const char* const kArchives[] = {".zip", ".7z", ".rar", ".tar", ".gz", ".tgz", ".xz", ".bz2", ".zst"};
    return std::find(std::begin(kArchives), std::end(kArchives), ext) != std::end(kArchives);
}

bool extract_archive(const fs::path& archive, const fs::path& dest, std::string& error) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    bool ok = false;
    bool unsupported = true;
    if (lower(archive.extension().string()) == ".zip") ok = extract_zip(archive, dest, error, unsupported);
#if defined(_WIN32)
    if (!ok && unsupported) {
        fs::remove_all(dest, ec);
        fs::create_directories(dest, ec);
        ok = extract_with_system_tar(archive, dest, error);
    }
#else
    if (!ok && unsupported && error.empty()) error = "only .zip archives can be installed on this platform";
#endif
    return ok && check_tree(dest, error);
}

bool remove_installed(const fs::path& mods_dir, const fs::path& folder, std::string& error) {
    std::error_code ec;
    const fs::path canon_dir = fs::weakly_canonical(mods_dir, ec);
    const fs::path canon = fs::weakly_canonical(folder, ec);
    if (canon.parent_path() != canon_dir || canon.filename().empty() || utf8(canon.filename())[0] == '.') {
        error = utf8(folder) + " is not a mod folder of " + utf8(mods_dir);
        return false;
    }
    fs::remove_all(canon, ec);
    if (ec) {
        error = "cannot remove " + utf8(canon) + ": " + ec.message();
        return false;
    }
    return true;
}

InstallResult install(const InstallRequest& req) {
    InstallResult r;
    std::error_code ec;
    if (req.game == nullptr || req.game->empty()) {
        r.issues.push_back({Severity::Error, {}, "the game's files were not found, so the download cannot be placed"});
        return r;
    }
    const fs::path staging_root = req.mods_dir / ".staging";
    const fs::path staging = long_path(staging_root / random_tag());  // every path below it may be long
    const fs::path in = staging / "in";
    const fs::path out = staging / "out";
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(p, e);
            fs::remove(p.parent_path(), e);  // .staging, when empty
        }
    } cleanup{staging};
    fs::create_directories(in, ec);
    fs::create_directories(out, ec);
    std::string error;
    // 1. The download, unpacked.
    if (fs::is_directory(req.source, ec)) {
        fs::copy(req.source, in, fs::copy_options::recursive, ec);
        if (ec) {
            r.issues.push_back({Severity::Error, {}, "cannot copy " + utf8(req.source) + ": " + ec.message()});
            return r;
        }
        if (!check_tree(in, error)) {
            r.issues.push_back({Severity::Error, {}, error});
            return r;
        }
    } else if (fs::is_regular_file(req.source, ec) && is_archive(req.source)) {
        if (!extract_archive(req.source, in, error)) {
            r.issues.push_back({Severity::Error, {}, utf8(req.source.filename()) + ": " + error});
            return r;
        }
    } else {
        r.issues.push_back({Severity::Error, {}, utf8(req.source) + " is neither a folder nor an archive"});
        return r;
    }
    // Archives inside the download (Dolphin resource packs, "pick one" bundles): unpacked in place, one level.
    {
        std::vector<Staged> files;
        list_tree(in, files);
        for (const Staged& s : files) {
            if (!is_archive(s.path)) continue;
            fs::path inner = s.path;
            inner.replace_extension();
            inner += ".unpacked";
            std::string e;
            if (extract_archive(s.path, inner, e)) {
                fs::remove(s.path, ec);
            } else {
                fs::remove_all(inner, ec);
                r.left_out.push_back(joined(s.parts, 0, s.parts.size()) + ": nested archive not unpacked (" + e + ")");
                fs::remove(s.path, ec);
            }
        }
    }
    // 2. Already a mod?
    fs::path native_root;
    {
        fs::path probe = in;
        for (int depth = 0; depth < 3 && native_root.empty(); ++depth) {
            if (fs::is_regular_file(probe / std::string(kManifestName), ec)) {
                native_root = probe;
                break;
            }
            std::vector<fs::path> dirs;
            std::size_t files = 0;
            for (fs::directory_iterator it(probe, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_directory(ec)) dirs.push_back(it->path());
                else ++files;
            }
            if (dirs.size() != 1 || files != 0) break;
            probe = dirs.front();
        }
    }
    Manifest manifest;
    // Saves and programs of the download: never placed, never run; named when the download has nothing else.
    std::vector<std::pair<std::string, SaveKind>> saves;
    std::vector<std::pair<std::string, std::uintmax_t>> programs;
    if (!native_root.empty()) {
        std::vector<Issue> mi;
        if (!read_manifest(native_root / std::string(kManifestName), manifest, mi)) {
            r.issues.insert(r.issues.end(), mi.begin(), mi.end());
            r.issues.push_back({Severity::Error, {}, "the download's mod.json is not usable"});
            return r;
        }
        r.issues.insert(r.issues.end(), mi.begin(), mi.end());
        fs::remove_all(out, ec);
        fs::rename(native_root, out, ec);
        if (ec) {
            fs::copy(native_root, out, fs::copy_options::recursive, ec);
            if (ec) {
                r.issues.push_back({Severity::Error, {}, "cannot stage the mod: " + ec.message()});
                return r;
            }
        }
    } else {
        // 3. Place every file.
        const GameIndex& game = *req.game;
        std::vector<Staged> files;
        list_tree(in, files);
        struct Placed {
            std::string game_path;  // disc spelling (or the mod's own for an added file)
            fs::path from;
            bool by_name = false;
        };
        std::map<std::string, std::vector<Placed>> by_root;  // root prefix -> files
        std::vector<std::pair<std::string, fs::path>> textures, afs_entries, docs, codes;
        std::string game_id;
        fs::path dolphin_manifest;
        std::vector<fs::path> readmes;
        for (const Staged& s : files) {
            const std::vector<std::string>& p = s.parts;
            const std::string rel = joined(p, 0, p.size());
            const std::string name = p.back();
            const std::string lname = lower(name);
            const std::string ext = lower(fs::path(from_utf8(name)).extension().string());
            for (const std::string& part : p) {
                const std::string l = lower(part);
                if (l.size() == 6 && l.rfind("gup", 0) == 0) game_id = part;
            }
            bool code_dir = false, sys_dir = false;
            for (std::size_t i = 0; i + 1 < p.size(); ++i) {
                const std::string l = lower(p[i]);
                code_dir |= l == "code" || l == "codes" || l == "gecko" || l == "cheats";
                sys_dir |= l == "sys";
            }
            // 2. textures
            if (lname.rfind("tex1_", 0) == 0 && (ext == ".png" || ext == ".dds")) {
                textures.emplace_back(rel, s.path);
                continue;
            }
            // Programs and saves: kept out of the mod entirely (the manager neither runs programs nor writes saves).
            if (is_program(ext)) {
                programs.emplace_back(rel, fs::file_size(long_path(s.path), ec));
                r.left_out.push_back(rel + ": a program; the manager never installs or runs programs");
                continue;
            }
            if (SaveKind save = save_kind(s.path, ext); !save.what.empty()) {
                r.left_out.push_back(rel + ": " + save.what + "; the manager never installs or writes saves");
                saves.emplace_back(rel, std::move(save));
                continue;
            }
            // 3. code
            if (code_dir || ext == ".gct" || lname == "main.dol" || ext == ".dol" || ext == ".xdelta" ||
                ext == ".vcdiff" || ext == ".ups" || ext == ".bps" || ext == ".ips" || ext == ".ppf") {
                codes.emplace_back(rel, s.path);
                continue;
            }
            if (ext == ".ini") {
                std::ifstream f(s.path, std::ios::binary);
                std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                if (t.find("[Gecko]") != std::string::npos || t.find("[ActionReplay]") != std::string::npos ||
                    t.find("[OnFrame]") != std::string::npos) {
                    codes.emplace_back(rel, s.path);
                    continue;
                }
            }
            // 4. disc header files
            if (sys_dir) {
                r.left_out.push_back(rel + ": disc header file (the port reads no disc header)");
                continue;
            }
            // 5. AFS entries
            if (p.size() >= 2 && game.find(p[p.size() - 2] + ".afs") != nullptr && !is_doc(ext)) {
                afs_entries.emplace_back(p[p.size() - 2] + "/" + name, s.path);
                continue;
            }
            // 6. a disc path's tail
            bool placed = false;
            for (std::size_t k = 0; k < p.size(); ++k) {
                if (const std::string* disc = game.find(joined(p, k, p.size()))) {
                    by_root[joined(p, 0, k)].push_back({*disc, s.path, false});
                    placed = true;
                    break;
                }
            }
            if (placed) continue;
            // 7. below files/ or root/
            std::size_t marker = p.size();
            for (std::size_t i = 0; i + 1 < p.size(); ++i) {
                const std::string l = lower(p[i]);
                if (l == "files" || l == "root") marker = i;
            }
            if (marker < p.size() && !is_doc(ext)) {
                by_root[joined(p, 0, marker + 1)].push_back({joined(p, marker + 1, p.size()), s.path, false});
                continue;
            }
            // 8. a unique file name
            if (!is_doc(ext)) {
                auto it = game.by_name.find(lname);
                if (it != game.by_name.end() && it->second.size() == 1) {
                    by_root[joined(p, 0, p.size() - 1)].push_back({it->second.front(), s.path, true});
                    continue;
                }
                if (it != game.by_name.end()) {
                    std::string list;
                    for (const std::string& c : it->second) list += (list.empty() ? "" : ", ") + c;
                    r.left_out.push_back(rel + ": more than one disc file has this name (" + list +
                                         "); put it in files/<path> to choose");
                    continue;
                }
            }
            // 9. documents
            if (is_doc(ext)) {
                if (lname == "manifest.json") dolphin_manifest = s.path;
                // A readme that carries Gecko / Action Replay code lines ("Code: 044978f0 60000000") also goes to
                // codes/, where the manager offers it as code to choose.
                if (ext == ".txt") {
                    std::ifstream f(s.path, std::ios::binary);
                    std::string line;
                    bool has_code = false;
                    while (!has_code && std::getline(f, line)) {
                        std::size_t run = 0, groups = 0;
                        for (const char c : line) {
                            if (std::isxdigit(static_cast<unsigned char>(c))) {
                                if (++run == 8) ++groups;
                            } else {
                                if (run > 8) groups = 0;
                                run = 0;
                            }
                        }
                        has_code = groups >= 2 && line.size() < 64;
                    }
                    if (has_code) codes.emplace_back(rel, s.path);
                }
                if (ext == ".txt" || ext == ".md") readmes.push_back(s.path);
                docs.emplace_back(rel, s.path);
                continue;
            }
            r.left_out.push_back(rel + ": not a file of this game's disc");
        }
        // Base set and alternative sets.
        std::vector<std::pair<std::string, std::vector<Placed>*>> roots;
        for (auto& [root, list] : by_root) roots.emplace_back(root, &list);
        std::stable_sort(roots.begin(), roots.end(),
                         [](const auto& a, const auto& b) { return a.second->size() > b.second->size(); });
        std::set<std::string> base;
        std::vector<std::pair<std::string, std::vector<Placed>*>> variants;
        for (auto& [root, list] : roots) {
            bool repeats = false;
            for (const Placed& f : *list) repeats |= base.count(lower(f.game_path)) != 0;
            if (repeats && !base.empty()) {
                variants.emplace_back(root, list);
                continue;
            }
            for (const Placed& f : *list) {
                if (!base.insert(lower(f.game_path)).second) continue;  // a repeat inside one set: first wins
                if (!copy_file(f.from, out / "files" / from_utf8(f.game_path), error)) {
                    r.issues.push_back({Severity::Error, {}, error});
                    return r;
                }
                if (f.by_name)
                    r.issues.push_back({Severity::Info, {}, utf8(f.from.filename()) + " placed at " + f.game_path +
                                                                " (the only disc file of that name)"});
            }
        }
        for (const auto& [rel, from] : textures)
            if (!copy_file(from, out / "textures" / from_utf8(rel), error)) {
                r.issues.push_back({Severity::Error, {}, error});
                return r;
            }
        for (const auto& [rel, from] : afs_entries)
            if (!copy_file(from, out / "afs" / from_utf8(rel), error)) {
                r.issues.push_back({Severity::Error, {}, error});
                return r;
            }
        for (const auto& [rel, from] : docs) copy_file(from, out / "docs" / from_utf8(rel), error);
        for (const auto& [rel, from] : codes) {
            copy_file(from, out / "codes" / from_utf8(rel), error);
            const std::string ext = lower(fs::path(from_utf8(rel)).extension().string());
            const bool image = ext == ".xdelta" || ext == ".vcdiff" || ext == ".ups" || ext == ".bps" || ext == ".ips" ||
                               ext == ".ppf";
            r.issues.push_back(
                {image ? Severity::Warning : Severity::Info, {},
                 rel + (image ? ": a patch of a whole disc image; not applied (install the files it contains instead)"
                              : ": code; kept in codes/ and applied only when chosen under Code mods")});
        }
        // Options for the alternative sets.
        std::set<std::string> option_ids;
        for (const auto& [root, list] : variants) {
            // Named after the set's own folder (with its parent when that alone is ambiguous: ".../16x9").
            std::string label = root.empty() ? "loose files" : root;
            if (const std::size_t last = label.rfind('/'); last != std::string::npos) {
                const std::size_t before = label.rfind('/', last - 1);
                const std::string tail = label.substr(last + 1);
                label = tail.size() <= 6 && before != std::string::npos ? label.substr(before + 1) : tail;
            }
            std::string oid = make_mod_id(label);
            while (!option_ids.insert(oid).second) oid += "-2";
            ModOption opt;
            opt.id = oid;
            opt.name = label;
            opt.description = "An alternative set from the download (" + std::to_string(list->size()) +
                              " files) that replaces some of the base files.";
            opt.choices.push_back({"off", "Off", ""});
            opt.choices.push_back({"on", "On", "options/" + oid + "/on"});
            opt.default_choice = "off";
            std::set<std::string> seen;
            for (const Placed& f : *list) {
                if (!seen.insert(lower(f.game_path)).second) continue;
                if (!copy_file(f.from, out / "options" / oid / "on" / "files" / from_utf8(f.game_path), error)) {
                    r.issues.push_back({Severity::Error, {}, error});
                    return r;
                }
            }
            manifest.options.push_back(std::move(opt));
            r.issues.push_back({Severity::Info, {},
                                "'" + label + "' repeats files of the main set: installed as an option (off)"});
        }
        // The manifest.
        std::string name = req.name_hint;
        if (name.empty()) name = pretty_name(utf8(req.source.stem()));
        manifest.name = name;
        manifest.version = "1.0";
        manifest.game_id = game_id;
        if (!dolphin_manifest.empty()) {  // Dolphin resource pack: name, id, authors, description, version
            std::ifstream f(dolphin_manifest, std::ios::binary);
            std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            json::Value v;
            if (json::parse(t, v, error) && v.is_object()) {
                if (const json::Value* n = v.find("name"); n && n->is_string() && !n->as_string().empty())
                    manifest.name = n->as_string();
                if (const json::Value* d = v.find("description"); d && d->is_string())
                    manifest.description = d->as_string();
                if (const json::Value* ver = v.find("version"); ver && ver->is_string() && Version::parse(ver->as_string()))
                    manifest.version = ver->as_string();
                if (const json::Value* a = v.find("authors"); a && a->is_array()) {
                    for (const json::Value& x : a->items())
                        if (x.is_string()) manifest.author += (manifest.author.empty() ? "" : ", ") + x.as_string();
                } else if (a && a->is_string()) {
                    manifest.author = a->as_string();
                }
            }
        }
        if (manifest.description.empty() && !readmes.empty()) {
            std::stable_sort(readmes.begin(), readmes.end(), [](const fs::path& a, const fs::path& b) {
                // A readme before other text, the shallowest first (a download's own README, not a sub-folder's).
                const auto rank = [](const fs::path& p) {
                    const std::string t = utf8(p);
                    const auto depth = static_cast<int>(std::count(t.begin(), t.end(), '/'));
                    return std::pair{lower(utf8(p.filename())).find("read") == 0 ? 0 : 1, depth};
                };
                return rank(a) < rank(b);
            });
            manifest.description = readme_text(readmes.front());
        }
        // A preview image: one named like a preview, else the largest picture among the documents.
        fs::path preview;
        std::uintmax_t best = 0;
        for (const auto& [rel, from] : docs) {
            const std::string ext = lower(from.extension().string());
            if (!is_image(ext)) continue;
            const std::string l = lower(rel);
            const std::uintmax_t size = fs::file_size(from, ec);
            const std::uintmax_t score =
                (l.find("preview") != std::string::npos || l.find("screen") != std::string::npos) ? size + (1ull << 40)
                                                                                                   : size;
            if (score > best) {
                best = score;
                preview = fs::path("docs") / from_utf8(rel);
            }
        }
        if (!preview.empty()) manifest.preview = utf8(preview);
    }
    // 4. Damaged downloads: a container the game would crash on is refused here, with the file named.
    {
        std::vector<Staged> files;
        list_tree(out, files);
        std::vector<std::string> damaged;
        for (const Staged& s : files) {
            // A download that is a mod already (its own mod.json) is copied as it is, except programs.
            if (is_program(lower(s.path.extension().string()))) {
                const std::string rel = joined(s.parts, 0, s.parts.size());
                programs.emplace_back(rel, fs::file_size(s.path, ec));
                r.left_out.push_back(rel + ": a program; the manager never installs or runs programs");
                fs::remove(s.path, ec);
                continue;
            }
            if (s.parts.empty() || (s.parts[0] != "files" && s.parts[0] != "afs" && s.parts[0] != "options")) continue;
            std::string why;
            // Whole but made for another version of the game (an Xbox movie): left out, the rest installs.
            if (!check_file_compatible(s.path, s.parts.back(), why)) {
                r.left_out.push_back(joined(s.parts, 0, s.parts.size()) + ": " + why);
                r.issues.push_back({Severity::Warning, {}, joined(s.parts, 0, s.parts.size()) + " " + why + "; left out"});
                fs::remove(s.path, ec);
                continue;
            }
            if (!check_file_format(s.path, s.parts.back(), why)) damaged.push_back(joined(s.parts, 0, s.parts.size()) + " " + why);
        }
        if (!damaged.empty()) {
            for (const std::string& d : damaged) r.issues.push_back({Severity::Error, {}, d});
            r.issues.push_back({Severity::Error, {}, "the download is damaged (" + std::to_string(damaged.size()) +
                                                         " file(s)); download it again"});
            return r;
        }
    }
    // 5. Identity and the final folder.
    if (manifest.id.empty()) {
        // The download's own name is remembered in the manifest ("installed_from"), so installing the same download
        // again is recognised as the same mod (an update) and not installed twice as "<id>-2".
        const std::string source_name = utf8(req.source.filename());
        const auto installed_from = [&](const fs::path& folder) {
            Manifest existing;
            std::vector<Issue> ignored;
            if (!read_manifest(folder / std::string(kManifestName), existing, ignored)) return std::string();
            const json::Value* from = existing.extra.find("installed_from");
            return from != nullptr && from->is_string() ? from->as_string() : std::string();
        };
        std::string id = make_mod_id(manifest.name);
        std::string candidate = id;
        for (int n = 2; fs::exists(req.mods_dir / from_utf8(candidate), ec); ++n) {
            if (lower(installed_from(req.mods_dir / from_utf8(candidate))) == lower(source_name)) break;  // same mod
            candidate = id + "-" + std::to_string(n);
        }
        manifest.id = candidate;
        manifest.extra.set("installed_from", json::Value::string(source_name));
    }
    r.content = summarize_content(out);
    std::size_t option_files = 0;
    for (const ModOption& o : manifest.options)
        for (const OptionChoice& c : o.choices)
            if (!c.folder.empty()) {
                const ContentSummary s = summarize_content(out / from_utf8(c.folder));
                option_files += s.files + s.afs_entries + s.textures;
            }
    if (r.content.files + r.content.afs_entries + r.content.textures + option_files == 0) {
        // Say what the download is when it is a save or a program, not only that nothing in it loads.
        if (!programs.empty()) {
            std::string main = programs.front().first;
            std::uintmax_t largest = 0;
            for (const auto& [rel, size] : programs)
                if (lower(fs::path(from_utf8(rel)).extension().string()) == ".exe" && size >= largest) {
                    largest = size;
                    main = rel;
                }
            r.issues.push_back({Severity::Error, {},
                                "this download is a program (" + main + "), not a mod: the manager never installs or "
                                "runs programs, and nothing was installed. If it is a tool that makes modified game "
                                "files (a randomizer, an editor), run it yourself on a copy of the game's files and "
                                "install the folder it writes"});
            return r;
        }
        if (!saves.empty()) {
            const auto& [rel, save] = saves.front();
            r.issues.push_back({Severity::Error, {},
                                "this download is a save, not a mod: " + rel + " is " + save.what +
                                    ". The manager never writes into your saves, and nothing was installed"});
            r.issues.push_back(
                {Severity::Error, {},
                 save.games_own
                     ? "to play from it, keep a copy outside the game's folder and start the game with "
                       "--save-file <that copy>: the game then reads and writes the copy, and your own save stays as "
                       "it is"
                     : "it is not this game's own save file (\"SHADOWTHEHEDGEHOG\"), which is the only save this build "
                       "reads, so it cannot be used here (a save of a modded game such as ShdTH-Reloaded belongs to "
                       "that game's own code)"});
            return r;
        }
        r.issues.push_back({Severity::Error, {},
                            "nothing in this download is content the game can load (no disc files, AFS entries or "
                            "Dolphin textures were recognised)"});
        return r;
    }
    // An update of a mod whose details the user edited keeps those edits (Edit info / set-info mark the manifest
    // "user_edited"); the content and the options come from the new download.
    if (native_root.empty() && req.replace_existing) {
        Manifest old;
        std::vector<Issue> ignored;
        const fs::path old_file = req.mods_dir / from_utf8(manifest.id) / std::string(kManifestName);
        if (fs::is_regular_file(old_file, ec) && read_manifest(old_file, old, ignored)) {
            const json::Value* edited = old.extra.find("user_edited");
            if (edited != nullptr && edited->as_bool()) {
                manifest.name = old.name;
                manifest.version = old.version;
                manifest.author = old.author;
                manifest.website = old.website;
                manifest.description = old.description;
                manifest.extra.set("user_edited", json::Value::boolean(true));
                r.issues.push_back({Severity::Info, manifest.id, "kept the name, version, author, website and "
                                                                 "description you edited"});
            }
        }
    }
    if (native_root.empty() && !write_manifest(out / std::string(kManifestName), manifest, error)) {
        r.issues.push_back({Severity::Error, {}, error});
        return r;
    }
    const fs::path final_dir = req.mods_dir / from_utf8(manifest.id);
    if (fs::exists(final_dir, ec)) {
        if (!req.replace_existing) {
            r.issues.push_back({Severity::Error, manifest.id, "a mod with the id '" + manifest.id + "' is installed"});
            return r;
        }
        const fs::path old = staging / "old";
        fs::rename(final_dir, old, ec);
        if (ec) {
            r.issues.push_back({Severity::Error, manifest.id, "cannot replace the installed copy: " + ec.message()});
            return r;
        }
    }
    fs::rename(out, final_dir, ec);
    if (ec) {
        r.issues.push_back({Severity::Error, manifest.id, "cannot move the mod into place: " + ec.message()});
        if (fs::exists(staging / "old")) fs::rename(staging / "old", final_dir, ec);
        return r;
    }
    for (Issue& i : r.issues)
        if (i.mod_id.empty()) i.mod_id = manifest.id;
    r.ok = true;
    r.id = manifest.id;
    r.folder = final_dir;
    r.manifest = manifest;
    return r;
}

}  // namespace shadow::mods
