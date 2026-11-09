// Line-preserving configuration files. Contract: include/gekko/config_file.h.

#include "gekko/config_file.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace gekko::config_file {

namespace {

std::string trimmed(const std::string& s, bool lower) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    std::string out = s.substr(b, e - b);
    if (lower)
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

std::vector<Entry> parse(const std::string& text, std::vector<int>* bad_lines) {
    std::vector<Entry> out;
    std::size_t begin = 0;
    int line = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        const std::size_t next = end == std::string::npos ? text.size() : end + 1;
        if (end == std::string::npos) end = text.size();
        ++line;
        std::string body = text.substr(begin, end - begin);
        if (line == 1 && body.compare(0, 3, "\xEF\xBB\xBF") == 0) body.erase(0, 3);  // an editor's UTF-8 BOM
        if (!body.empty() && body.back() == '\r') body.pop_back();
        const std::size_t hash = body.find('#');
        if (hash != std::string::npos) body.resize(hash);
        const std::size_t eq = body.find('=');
        if (eq == std::string::npos) {
            if (bad_lines != nullptr && !trimmed(body, false).empty()) bad_lines->push_back(line);
        } else {
            out.push_back({trimmed(body.substr(0, eq), true), trimmed(body.substr(eq + 1), false), line});
        }
        begin = next;
    }
    return out;
}

bool read(const std::filesystem::path& path, std::string& text, std::string* error) {
    text.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = path.string() + ": cannot be read";
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        if (error != nullptr) *error = path.string() + ": cannot be read";
        return false;
    }
    return true;
}

std::string rewrite(const std::string& text, const std::vector<KeyValue>& values) {
    std::vector<bool> written(values.size(), false);
    const auto find = [&](const std::string& key) -> int {
        for (std::size_t i = 0; i < values.size(); ++i)
            for (const std::string& s : values[i].spellings)
                if (s == key) return static_cast<int>(i);
        return -1;
    };
    std::string out;
    out.reserve(text.size() + 256u);
    bool crlf = false;
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        const std::size_t next = end == std::string::npos ? text.size() : end + 1;
        if (end == std::string::npos) end = text.size();
        std::size_t body_end = end;
        if (body_end > begin && text[body_end - 1] == '\r') {
            --body_end;
            crlf = true;
        }
        std::string body = text.substr(begin, body_end - begin);
        // An editor's UTF-8 BOM on the first line is no part of the first key (the line is copied back untouched when
        // it is not rewritten; when it is, the BOM stays: only the value range changes).
        std::size_t bom = 0;
        if (begin == 0 && body.compare(0, 3, "\xEF\xBB\xBF") == 0) bom = 3;
        if (bom != 0) body.erase(0, 3);
        const std::size_t comment = body.find('#');
        const std::size_t content_end = comment == std::string::npos ? body.size() : comment;
        const std::size_t split = body.find('=');
        int index = -1;
        if (split != std::string::npos && split < content_end) index = find(trimmed(body.substr(0, split), true));
        if (index >= 0) {
            std::size_t value_begin = split + 1;
            while (value_begin < content_end && (body[value_begin] == ' ' || body[value_begin] == '\t')) ++value_begin;
            std::size_t value_end = content_end;
            while (value_end > value_begin && std::isspace(static_cast<unsigned char>(body[value_end - 1]))) --value_end;
            out.append(text, begin, bom);
            out.append(body, 0, value_begin);
            out += values[static_cast<std::size_t>(index)].value;
            out.append(body, value_end, std::string::npos);
            out.append(text, body_end, next - body_end);
            written[static_cast<std::size_t>(index)] = true;
        } else {
            out.append(text, begin, next - begin);
        }
        begin = next;
    }
    const char* newline = crlf ? "\r\n" : "\n";
    bool any_missing = false;
    for (bool w : written) any_missing |= !w;
    if (any_missing && !out.empty() && out.back() != '\n') out += newline;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (written[i] || values[i].spellings.empty()) continue;
        out += values[i].spellings[0] + "=" + values[i].value + newline;
    }
    return out;
}

bool update(const std::filesystem::path& path, const std::vector<KeyValue>& values, std::string* error) {
    std::string text;
    if (!read(path, text, error)) return false;
    const std::string out = rewrite(text, values);
    if (out == text) return true;
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = path.string() + ": " + message;
        return false;
    };
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::error_code ec;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return fail("cannot write " + temporary.string());
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
        file.close();
        if (!file) {
            std::filesystem::remove(temporary, ec);
            return fail("cannot write " + temporary.string());
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        const std::string reason = ec.message();
        std::filesystem::remove(temporary, ec);
        return fail("cannot replace the file (" + reason + ")");
    }
    return true;
}

}  // namespace gekko::config_file
