#include "render_vk/gpu_timer.h"

namespace shadow::render_vk {

GpuTimer::~GpuTimer() {
    destroy();
}

bool GpuTimer::create(Device& device, std::uint32_t slots, std::uint32_t capacity, std::string* error) {
    destroy();
    device_ = &device;
    const Capabilities& caps = device.caps();
    if (!caps.timestamps || capacity < 2)
        return true;  // off: nothing to measure with
    mask_ = caps.timestamp_bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << caps.timestamp_bits) - 1u;
    ns_per_tick_ = static_cast<double>(caps.timestamp_period);
    capacity_ = capacity;
    slots_.resize(slots);
    for (Slot& s : slots_) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = capacity;
        if (!check(vkCreateQueryPool(device.vk(), &info, nullptr, &s.pool), "vkCreateQueryPool", error)) {
            destroy();
            return false;
        }
    }
    return true;
}

void GpuTimer::destroy() {
    if (device_ != nullptr)
        for (Slot& s : slots_)
            if (s.pool != VK_NULL_HANDLE)
                vkDestroyQueryPool(device_->vk(), s.pool, nullptr);
    slots_.clear();
    spans_.clear();
    frame_ms_ = 0.0;
}

void GpuTimer::begin(VkCommandBuffer cmd, std::uint32_t slot) {
    if (slot >= slots_.size())
        return;
    collect(slot);
    Slot& s = slots_[slot];
    s.pending = false;  // a frame the GPU never finished writing is not read
    s.names.clear();
    recording_ = slot;
    vkCmdResetQueryPool(cmd, s.pool, 0, capacity_);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.pool, 0);
    s.pending = true;
}

void GpuTimer::mark(VkCommandBuffer cmd, std::string name) {
    if (recording_ >= slots_.size())
        return;
    Slot& s = slots_[recording_];
    if (!s.pending)
        return;
    const auto query = static_cast<std::uint32_t>(s.names.size() + 1);
    if (query >= capacity_) {
        ++dropped_;
        return;
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.pool, query);
    s.names.push_back(std::move(name));
}

bool GpuTimer::collect(std::uint32_t slot) {
    if (slot >= slots_.size() || !slots_[slot].pending || slots_[slot].names.empty())
        return false;
    Slot& s = slots_[slot];
    const auto count = static_cast<std::uint32_t>(s.names.size() + 1);
    std::vector<std::uint64_t> ticks(count);
    if (vkGetQueryPoolResults(device_->vk(), s.pool, 0, count, count * sizeof(std::uint64_t), ticks.data(),
                              sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return false;  // not all written yet
    s.pending = false;
    spans_.clear();
    frame_ms_ = 0.0;
    for (std::uint32_t q = 1; q < count; ++q) {
        const std::uint64_t elapsed = ((ticks[q] & mask_) - (ticks[q - 1] & mask_)) & mask_;
        const double ms = static_cast<double>(elapsed) * ns_per_tick_ * 1e-6;
        spans_.push_back(Span{s.names[q - 1], ms});
        frame_ms_ += ms;
    }
    ++frames_read_;
    return true;
}

}  // namespace shadow::render_vk
