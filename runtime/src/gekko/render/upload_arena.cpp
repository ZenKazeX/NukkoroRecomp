#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gekko/render/upload_arena.h"

#include <algorithm>
#include <utility>
#include <vector>

#if defined(SHADOW_ENABLE_PLUME)
#include "plume_render_interface.h"
#endif

namespace gekko::render {

#if !defined(SHADOW_ENABLE_PLUME)

struct UploadArena::Impl {};
UploadArena::UploadArena() : impl_(std::make_unique<Impl>()) {}
UploadArena::~UploadArena() = default;
UploadArena::UploadArena(UploadArena&&) noexcept = default;
UploadArena& UploadArena::operator=(UploadArena&&) noexcept = default;
bool UploadArena::create(plume::RenderDevice&, const Config&, std::string& error) {
    error = "built without plume";
    return false;
}
void UploadArena::destroy() {}
void UploadArena::reset() noexcept {}
UploadArena::Allocation UploadArena::allocate(u64, u64) { return {}; }
UploadArena::Stats UploadArena::stats() const noexcept { return {}; }
u64 UploadArena::used_bytes() const noexcept { return 0; }
u64 UploadArena::max_bytes() const noexcept { return 0; }

#else

namespace {
u64 align_up(u64 value, u64 alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}
u64 power_of_two_at_least(u64 value) noexcept {
    u64 result = 1u;
    while (result < value && result <= (~u64{0} >> 1u)) result <<= 1u;
    return result;
}
}  // namespace

struct UploadArena::Impl {
    struct Chunk {
        std::unique_ptr<plume::RenderBuffer> buffer;
        u8* mapped = nullptr;
        u64 capacity = 0;
        u64 used = 0;
    };

    plume::RenderDevice* device = nullptr;
    Config config{};
    std::vector<Chunk> chunks;
    std::size_t current = 0;
    Stats stats{};

    bool add_chunk(u64 request) {
        if (stats.retained_bytes >= config.max_bytes) return false;
        const u64 remaining = config.max_bytes - stats.retained_bytes;
        u64 target = std::max(config.minimum_chunk_bytes,
                              stats.retained_bytes == 0u ? config.minimum_chunk_bytes
                                                         : stats.retained_bytes);
        target = std::max(target, power_of_two_at_least(request));
        target = std::min(target, remaining);
        if (target < request) return false;

        plume::RenderBufferFlags flags = plume::RenderBufferFlag::NONE;
        if (config.vertex) flags |= plume::RenderBufferFlag::VERTEX;
        if (config.index) flags |= plume::RenderBufferFlag::INDEX;
        if (config.constant) flags |= plume::RenderBufferFlag::CONSTANT;
        plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(target, flags);
        desc.committed = config.committed;
        auto buffer = device->createBuffer(desc);
        if (buffer == nullptr) {
            ++stats.allocation_failures;
            return false;
        }
        auto* mapped = static_cast<u8*>(buffer->map());
        if (mapped == nullptr) {
            ++stats.allocation_failures;
            return false;
        }
        Chunk chunk;
        chunk.buffer = std::move(buffer);
        chunk.mapped = mapped;
        chunk.capacity = target;
        chunks.push_back(std::move(chunk));
        stats.retained_bytes += target;
        ++stats.growths;
        return true;
    }
};

UploadArena::UploadArena() : impl_(std::make_unique<Impl>()) {}
UploadArena::~UploadArena() { destroy(); }
UploadArena::UploadArena(UploadArena&&) noexcept = default;
UploadArena& UploadArena::operator=(UploadArena&&) noexcept = default;

bool UploadArena::create(plume::RenderDevice& device, const Config& config, std::string& error) {
    destroy();
    if (config.minimum_chunk_bytes == 0u || config.max_bytes == 0u ||
        config.minimum_chunk_bytes > config.max_bytes) {
        error = "invalid upload arena bounds";
        return false;
    }
    impl_->device = &device;
    impl_->config = config;
    return true;
}

void UploadArena::destroy() {
    if (!impl_) return;
    for (auto& chunk : impl_->chunks) {
        if (chunk.buffer != nullptr && chunk.mapped != nullptr) chunk.buffer->unmap();
    }
    impl_->chunks.clear();
    impl_->device = nullptr;
    impl_->current = 0;
    impl_->stats = Stats{};
}

void UploadArena::reset() noexcept {
    Impl& impl = *impl_;
    if (impl.stats.used_bytes != 0u) ++impl.stats.resets;
    for (auto& chunk : impl.chunks) chunk.used = 0;
    impl.current = 0;
    impl.stats.used_bytes = 0;
}

UploadArena::Allocation UploadArena::allocate(u64 bytes, u64 alignment) {
    Impl& impl = *impl_;
    impl.stats.largest_request_bytes = std::max(impl.stats.largest_request_bytes, bytes);
    if (impl.device == nullptr || bytes == 0u || alignment == 0u ||
        (alignment & (alignment - 1u)) != 0u || bytes > impl.config.max_bytes) {
        return {};
    }
    for (;;) {
        for (std::size_t index = impl.current; index < impl.chunks.size(); ++index) {
            auto& chunk = impl.chunks[index];
            const u64 offset = align_up(chunk.used, alignment);
            if (offset <= chunk.capacity && bytes <= chunk.capacity - offset) {
                const u64 consumed = offset + bytes - chunk.used;
                chunk.used = offset + bytes;
                if (index != impl.current) ++impl.stats.chunk_switches;
                impl.current = index;
                impl.stats.used_bytes += consumed;
                impl.stats.peak_used_bytes =
                    std::max(impl.stats.peak_used_bytes, impl.stats.used_bytes);
                return {chunk.buffer.get(), chunk.mapped + offset, offset};
            }
        }
        if (!impl.add_chunk(bytes)) return {};
    }
}

UploadArena::Stats UploadArena::stats() const noexcept { return impl_->stats; }
u64 UploadArena::used_bytes() const noexcept { return impl_->stats.used_bytes; }
u64 UploadArena::max_bytes() const noexcept { return impl_->config.max_bytes; }

#endif

}  // namespace gekko::render
