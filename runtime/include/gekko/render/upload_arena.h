#pragma once

// Fence-owned, persistently mapped GPU upload memory. Each arena belongs to
// exactly one command-list slot/batch and may be reset only after that owner's
// fence retires. Chunks are added from observed allocation requests, retained
// for reuse, and bounded by max_bytes.

#include <memory>
#include <string>

#include "gekko/types.h"

namespace plume {
struct RenderBuffer;
struct RenderDevice;
}  // namespace plume

namespace gekko::render {

class UploadArena {
public:
    struct Config {
        u64 minimum_chunk_bytes = 64ull << 10;
        u64 max_bytes = 8ull << 20;
        bool vertex = false;
        bool index = false;
        bool constant = false;
        bool committed = false;
    };
    struct Allocation {
        plume::RenderBuffer* buffer = nullptr;
        u8* data = nullptr;
        u64 offset = 0;
        explicit operator bool() const noexcept { return buffer != nullptr && data != nullptr; }
    };
    struct Stats {
        u64 retained_bytes = 0;
        u64 used_bytes = 0;
        u64 peak_used_bytes = 0;
        u64 largest_request_bytes = 0;
        u64 growths = 0;
        u64 allocation_failures = 0;
        u64 resets = 0;
        u64 chunk_switches = 0;
    };

    UploadArena();
    ~UploadArena();
    UploadArena(UploadArena&&) noexcept;
    UploadArena& operator=(UploadArena&&) noexcept;
    UploadArena(const UploadArena&) = delete;
    UploadArena& operator=(const UploadArena&) = delete;

    bool create(plume::RenderDevice& device, const Config& config, std::string& error);
    void destroy();
    // The owner fence must have retired. Retains chunks and rewinds them.
    void reset() noexcept;
    Allocation allocate(u64 bytes, u64 alignment);
    Stats stats() const noexcept;
    u64 used_bytes() const noexcept;
    u64 max_bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gekko::render
