// Host files of the PC's save storage. Contract: include/gekko/host_files.h.

#include "gekko/host_files.h"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "gekko/audio_audit.h"

namespace gekko {

namespace save_trace {
namespace {

#ifdef _WIN32
DWORD owner_thread = 0;
bool on_owner() noexcept { return owner_thread != 0 && GetCurrentThreadId() == owner_thread; }
#else
bool on_owner() noexcept { return false; }
#endif

u64 now_ns() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

}  // namespace

void claim_owner() noexcept {
#ifdef _WIN32
    if (owner_thread == 0) owner_thread = GetCurrentThreadId();
#endif
}

void mark(const char* stage, u64 a, u64 b, u64 c, u64 d) noexcept {
    if (audio_audit::enabled() && on_owner()) {
        audio_audit::event(audio_audit::Lane::Guest, stage, now_ns(), a, b, c, d);
    }
}

Stage::Stage(const char* stage, u64 size, u64 origin) noexcept : name_(stage), bytes_(size), origin_(origin) {
    if (audio_audit::enabled() && on_owner()) start_ = now_ns();
}

Stage::~Stage() {
    if (start_ != 0) {
        audio_audit::event(audio_audit::Lane::Guest, name_, start_, now_ns() - start_, bytes_, origin_);
    }
}

}  // namespace save_trace

std::vector<u8> read_host_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("could not open memory-card file: " + path.string());
    }
    const std::streamoff length = input.tellg();
    if (length < 0) {
        throw std::runtime_error("could not size memory-card file: " + path.string());
    }
    std::vector<u8> bytes(static_cast<std::size_t>(length));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), length);
    }
    if (!input) {
        throw std::runtime_error("could not read memory-card file: " + path.string());
    }
    return bytes;
}

void write_host_file_atomically(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    save_trace::Stage whole("save.file", bytes.size());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output;
        {
            save_trace::Stage stage("save.open");
            output.open(temporary, std::ios::binary | std::ios::trunc);
        }
        if (!output) {
            throw std::runtime_error("could not create memory-card file: " + temporary.string());
        }
        if (!bytes.empty()) {
            save_trace::Stage stage("save.write", bytes.size());
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        {
            save_trace::Stage stage("save.flush");
            output.flush();
        }
        if (!output) {
            throw std::runtime_error("could not write memory-card file: " + temporary.string());
        }
        save_trace::Stage stage("save.close");
        output.close();
    }
#ifdef _WIN32
    {
        save_trace::Stage stage("save.fsync");
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(file);
            CloseHandle(file);
        }
    }
    save_trace::Stage rename_stage("save.rename");
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD error = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("could not commit memory-card file (Win32 error " + std::to_string(error) +
                                 "): " + path.string());
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("could not commit memory-card file: " + path.string() + ": " + error.message());
    }
#endif
}

}  // namespace gekko
