#pragma once

// Presentation (docs/VULKAN_RENDERER.md 3.2): the counterpart of render/swap_chain.h - a swapchain on a Win32 window
// (desktop testing) or an ANativeWindow (Android), at the surface's own size.
//
// Semaphores: one "image acquired" per frame slot (the frame waits for it before its colour output) and one "ready to
// present" per swapchain IMAGE (a present may still hold the semaphore of an image when the frame slot comes round
// again, so a per-slot one could be signalled twice).
// Pre-rotation (Android): when the surface is turned against the display's native orientation (currentTransform a
// rotation), the swapchain is made in the NATIVE orientation with preTransform = currentTransform, and the renderer's
// final pass draws the picture turned (quarter_turns(), pipelines.h turned_rect, Screen::Present) - the compositor
// then shows the image as it is instead of turning every frame. width() / height() stay the picture's (the display's
// orientation); the images are image(i).width x height. Desktop surfaces are never turned (quarter_turns() == 0).

#include <cstdint>
#include <string>
#include <vector>

#include "render_vk/device.h"
#include "render_vk/resources.h"

namespace shadow::render_vk {

enum class PresentMode {
    VSync,      // FIFO: one refresh per present (always available)
    Immediate,  // IMMEDIATE, else MAILBOX, else FIFO
};

class Swapchain {
public:
    Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    ~Swapchain();

    bool create(Device& device, const NativeWindow& window, std::uint32_t width, std::uint32_t height,
                PresentMode mode, std::uint32_t frames_in_flight, std::string* error);
    // Recreates the swapchain at the new size (or the surface's own size when it dictates one).
    bool resize(std::uint32_t width, std::uint32_t height, std::string* error);
    void destroy();

    // The next image; `*out_of_date` when the swapchain must be resized first (nothing acquired then).
    bool acquire(std::uint32_t frame_slot, std::uint32_t* image, bool* out_of_date, std::string* error);
    VkSemaphore acquired(std::uint32_t frame_slot) const noexcept { return acquired_[frame_slot]; }
    VkSemaphore ready(std::uint32_t image) const noexcept { return ready_[image]; }
    bool present(std::uint32_t image, bool* out_of_date, std::string* error);

    Image& image(std::uint32_t index) noexcept { return images_[index]; }
    VkFormat format() const noexcept { return format_; }
    // The picture's size, in the display's orientation.
    std::uint32_t width() const noexcept { return logical_.width; }
    std::uint32_t height() const noexcept { return logical_.height; }
    VkSurfaceTransformFlagBitsKHR transform() const noexcept { return transform_; }
    // Quarter turns clockwise the final pass draws the picture with (0 unless pre-rotating).
    std::uint32_t quarter_turns() const noexcept { return quarter_turns_; }
    bool valid() const noexcept { return swapchain_ != VK_NULL_HANDLE; }

private:
    bool build(std::uint32_t width, std::uint32_t height, std::string* error);
    void release_images();

    Device* device_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colour_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};   // the images'
    VkExtent2D logical_{};  // the picture's
    VkSurfaceTransformFlagBitsKHR transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::uint32_t quarter_turns_ = 0;
    PresentMode mode_ = PresentMode::VSync;
    std::vector<Image> images_;
    std::vector<VkSemaphore> acquired_;  // per frame slot
    std::vector<VkSemaphore> ready_;     // per image
};

}  // namespace shadow::render_vk
