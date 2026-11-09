#include "render/frame_loop.h"

namespace shadow::render {

FrameLoop::~FrameLoop() {
    destroy();
}

bool FrameLoop::create(Device& device, BindlessHeap& heap, std::uint32_t frames_in_flight,
                       std::uint64_t ring_segment_bytes, std::string* error) {
    destroy();
    device_ = &device;
    heap_ = &heap;
    slots_.resize(frames_in_flight == 0 ? 1 : frames_in_flight);
    for (Slot& slot : slots_) {
        const HRESULT hr =
            device.d3d()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
        if (FAILED(hr)) {
            if (error != nullptr)
                *error = "CreateCommandAllocator: " + hresult_text(hr);
            return false;
        }
    }
    const HRESULT hr = device.d3d()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots_[0].allocator.Get(),
                                                       nullptr, IID_PPV_ARGS(&list_));
    if (FAILED(hr)) {
        if (error != nullptr)
            *error = "CreateCommandList: " + hresult_text(hr);
        return false;
    }
    list_->SetName(L"shadow.frame");
    list_->Close();
    number_ = 0;
    open_ = false;
    return ring_.create(device, ring_segment_bytes, static_cast<std::uint32_t>(slots_.size()), error);
}

void FrameLoop::destroy() {
    if (device_ == nullptr)
        return;
    device_->wait_idle();
    ring_.destroy(*device_);
    list_.Reset();
    slots_.clear();
    device_->collect();
    device_ = nullptr;
    heap_ = nullptr;
}

FrameLoop::Frame FrameLoop::begin() {
    ++number_;
    slot_ = static_cast<std::uint32_t>(number_ % slots_.size());
    Slot& slot = slots_[slot_];
    device_->direct().wait(slot.fence);
    device_->collect();
    heap_->reclaim(*device_);
    heap_->begin_frame(slot_);
    ring_.begin(slot_);
    slot.allocator->Reset();
    list_->Reset(slot.allocator.Get(), nullptr);
    open_ = true;
    return Frame{list_.Get(), slot_, number_};
}

std::uint64_t FrameLoop::submit() {
    if (!open_)
        return slots_[slot_].fence;
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    const std::uint64_t value = device_->direct().submit(lists, 1);
    slots_[slot_].fence = value;
    open_ = false;
    return value;
}

}  // namespace shadow::render
