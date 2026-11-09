#pragma once

// GPU buffers and images (VMA placements) and the layout / access state an image carries between passes
// (docs/VULKAN_RENDERER.md 3.3, 3.6). The counterpart of render/resources.h.
//
// Every image remembers the last stage, access and layout it was used with: the frame graph (frame_graph.h) plans each
// frame's barriers from there and leaves the last use behind for the next frame; `use_image` records one barrier from
// there to a next use directly (the one-time upload command buffers, outside any frame graph).

#include <cstdint>
#include <string>

#include "render_vk/device.h"

struct VmaAllocation_T;
typedef VmaAllocation_T* VmaAllocation;

namespace shadow::render_vk {

inline constexpr std::uint32_t kNoDescriptor = 0xFFFFFFFFu;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;  // host-visible buffers stay mapped
    VkDeviceSize size = 0;
    explicit operator bool() const noexcept { return buffer != VK_NULL_HANDLE; }
};

// How an image is used at one point of the frame.
struct ImageUse {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// The uses the renderer has (the frame graph's Access values, render/frame_graph.h).
namespace use {
inline constexpr ImageUse kColourWrite{VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                       VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
inline constexpr ImageUse kDepthWrite{VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL};
inline constexpr ImageUse kDepthRead{VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                                     VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
inline constexpr ImageUse kShaderRead{VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
inline constexpr ImageUse kCopySource{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
inline constexpr ImageUse kCopyDest{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
inline constexpr ImageUse kPresent{VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
}  // namespace use

struct ImageInfo {
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t mip_levels = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    // Tile memory only (VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT + lazily allocated memory where the GPU has it): a
    // multisampled target that is never stored on a tiler (docs/VULKAN_RENDERER.md 5.1).
    bool transient = false;
    const char* name = nullptr;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;  // null: not ours (a swapchain image)
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    std::uint32_t width = 0, height = 0, mip_levels = 1;
    std::uint32_t srv = kNoDescriptor;  // its index in the bindless set (sampled images), when it has one
    ImageUse state;                     // the last use recorded
    bool transient = false;             // attachment-only (TRANSIENT_ATTACHMENT usage)
    bool lazy = false;                  // in lazily allocated memory: backed only where a pass stores it
    explicit operator bool() const noexcept { return image != VK_NULL_HANDLE; }
};

bool is_depth_format(VkFormat format) noexcept;

// Whether `format` can be a sampled colour attachment with `samples` samples (and blended into, with `blend`) here.
bool colour_target_supported(VkPhysicalDevice physical, VkFormat format, VkSampleCountFlagBits samples, bool blend);

// Where a buffer lives: device memory, host memory the CPU writes in order (upload rings), or host memory the CPU
// reads back (captures; cached where the GPU offers it).
enum class HostAccess : std::uint8_t { None, Write, Read };
bool create_buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, HostAccess host, Buffer* out,
                   std::string* error);
// Makes the GPU's writes to a Read buffer visible to the CPU (non-coherent memory).
void invalidate_buffer(Device& device, const Buffer& buffer);
bool create_image(Device& device, const ImageInfo& info, Image* out, std::string* error);
// A view of an image that is not ours (a swapchain image).
bool wrap_image(Device& device, VkImage image, VkFormat format, std::uint32_t width, std::uint32_t height, Image* out,
                std::string* error);
// Now (the GPU must be done with it) or after the GPU has passed everything submitted so far.
void destroy_buffer(Device& device, Buffer& buffer);
void destroy_image(Device& device, Image& image);
void retire_buffer(Device& device, Buffer& buffer);
void retire_image(Device& device, Image& image);

// Records the barrier from the image's last use to `next` (a layout change or a hazard; nothing when both only read
// in the same layout). `discard`: the earlier contents are not needed (the layout starts UNDEFINED).
void use_image(VkCommandBuffer cmd, Image& image, const ImageUse& next, bool discard = false);

}  // namespace shadow::render_vk
