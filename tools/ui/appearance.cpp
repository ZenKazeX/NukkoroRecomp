#include "appearance.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace ui {

namespace {

std::string trim(const std::string& s) {
    std::size_t begin = 0, end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The key and value of a "key = value  # comment" line; false for blank lines, comments and other lines.
bool split(const std::string& line, std::string& key, std::string& value) {
    std::string body = line;
    if (const std::size_t hash = body.find('#'); hash != std::string::npos) body.erase(hash);
    const std::size_t equals = body.find('=');
    if (equals == std::string::npos) return false;
    key = lower(trim(body.substr(0, equals)));
    value = lower(trim(body.substr(equals + 1)));
    return !key.empty();
}

const char* theme_name(Theme t) {
    switch (t) {
        case Theme::Light: return "light";
        case Theme::Dark: return "dark";
        default: return "system";
    }
}

bool read_text(const std::filesystem::path& file, std::string& text) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

Appearance parse_appearance(const std::string& text) {
    Appearance a;
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        std::string key, value;
        if (split(text.substr(begin, end - begin), key, value)) {
            if (key == "theme") {
                if (value == "system") a.theme = Theme::System;
                else if (value == "light") a.theme = Theme::Light;
                else if (value == "dark") a.theme = Theme::Dark;
            } else if (key == "text_size") {
                std::size_t used = 0;
                int size = 0;
                try {
                    size = std::stoi(value, &used);
                } catch (...) {
                    used = 0;
                }
                if (used != 0 && used == value.size()) a.text_size = std::clamp(size, kSmallestText, kLargestText);
            }
        }
        begin = end + 1;
    }
    return a;
}

std::string update_appearance(const std::string& text, const Appearance& appearance) {
    const std::string theme_line = std::string("theme = ") + theme_name(appearance.theme);
    const std::string size_line = "text_size = " + std::to_string(step_text_size(appearance.text_size, 0));
    const bool crlf = text.find("\r\n") != std::string::npos;
    const std::string newline = crlf ? "\r\n" : "\n";
    std::string out;
    bool theme_done = false, size_done = false;
    if (text.empty())
        out = "# How the Settings program and the Mod Manager look. The game does not read this file." + newline;
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        const bool last = end == std::string::npos;
        if (last) end = text.size();
        std::string line = text.substr(begin, end - begin);
        std::string ending = last ? "" : "\n";
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
            ending = "\r\n";
        }
        std::string key, value;
        if (split(line, key, value) && key == "theme") {
            line = theme_line;
            theme_done = true;
        } else if (split(line, key, value) && key == "text_size") {
            line = size_line;
            size_done = true;
        }
        out += line + ending;
        begin = end + 1;
    }
    if (!theme_done || !size_done) {
        if (!out.empty() && out.back() != '\n') out += newline;
        if (!theme_done) out += theme_line + newline;
        if (!size_done) out += size_line + newline;
    }
    return out;
}

int step_text_size(int now, int steps) {
    const int snapped = (std::clamp(now, kSmallestText, kLargestText) - kSmallestText + kTextStep / 2) / kTextStep;
    return std::clamp(kSmallestText + (snapped + steps) * kTextStep, kSmallestText, kLargestText);
}

Appearance load_appearance(const std::filesystem::path& folder) {
    std::string text;
    if (!read_text(folder / kAppearanceFile, text)) return {};
    return parse_appearance(text);
}

bool save_appearance(const std::filesystem::path& folder, const Appearance& appearance) {
    const std::filesystem::path file = folder / kAppearanceFile;
    std::string before;
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec);
    if (exists && !read_text(file, before)) return false;
    const std::string after = update_appearance(before, appearance);
    if (exists && after == before) return true;
    std::filesystem::path temporary = file;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << after;
        if (!out.flush()) return false;
    }
    std::filesystem::rename(temporary, file, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

}  // namespace ui
