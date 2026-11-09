#pragma once

// Line-preserving key = value configuration files (display.cfg, options.cfg): one reader and one in-place writer for
// every settings service, so no service parses or rewrites its file its own way.
//
// The format: one `key = value` per line, `#` starts a comment, keys are case-insensitive, blank lines and comments
// are kept. rewrite() replaces only the value range of the keys it is given (a key's spelling, spacing and trailing
// comment stay) and appends the keys the file lacks; every other byte, line endings included, stays as it was. The
// file is replaced whole: a temporary beside it, then renamed over it, so a failed write leaves the old file.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace gekko::config_file {

struct Entry {
    std::string key;    // lower case, trimmed
    std::string value;  // trimmed, comment removed
    int line = 0;       // 1-based
};

// The entries of `text`. Lines without '=' that are not blank or comments are reported in `bad_lines`.
std::vector<Entry> parse(const std::string& text, std::vector<int>* bad_lines = nullptr);

// Reads the whole file; false when it exists and cannot be read. A missing file reads as empty (true).
bool read(const std::filesystem::path& path, std::string& text, std::string* error = nullptr);

// `text` with each (key, value) of `values` written: in place where the key is (any of its `aliases` spellings,
// lower case), appended otherwise under the first spelling.
struct KeyValue {
    std::vector<std::string> spellings;  // lower case; [0] is the one appended
    std::string value;
};
std::string rewrite(const std::string& text, const std::vector<KeyValue>& values);

// rewrite() applied to the file at `path` and written back atomically (nothing is written when nothing changes).
bool update(const std::filesystem::path& path, const std::vector<KeyValue>& values, std::string* error = nullptr);

}  // namespace gekko::config_file
