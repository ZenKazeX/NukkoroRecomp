#include "render_vk/resources.h"

#include "vk_mem_alloc.h"

namespace shadow::render_vk {

bool is_depth_format(VkFormat format) noexcept {
    return format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D16_UNORM;
}

bool colour_target_supported(VkPhysicalDevice physical, VkFormat format, VkSampleCountFlagBits samples, bool blend) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(physical, format, &props);
    const VkFormatFeatureFlags want = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      (blend ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0u);
    if ((props.optimalTilingFeatures & want) != want)
        return false;
    VkImageFormatProperties image;
    if (vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                                                 &image) != VK_SUCCESS)
        return false;
    return (image.sampleCounts & samples) != 0;
}

bool create_buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, HostAccess host, Buffer* out,
                   std::string* error) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (host == HostAccess::Write)
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    else if (host == HostAccess::Read)
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo result{};
    Buffer b;
    if (!check(vmaCreateBuffer(device.allocator(), &info, &alloc, &b.buffer, &b.allocation, &result),
               "vmaCreateBuffer", error))
        return false;
    b.mapped = result.pMappedData;
    b.size = size;
    *out = b;
    return true;
}

void invalidate_buffer(Device& device, const Buffer& buffer) {
    if (buffer.allocation != nullptr)
        vmaInvalidateAllocation(device.allocator(), buffer.allocation, 0, VK_WHOLE_SIZE);
}

namespace {

bool make_view(Device& device, Image& image, std::string* error) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange = {image.aspect, 0, image.mip_levels, 0, 1};
    return check(vkCreateImageView(device.vk(), &view, nullptr, &image.view), "vkCreateImageView", error);
}

}  // namespace

bool create_image(Device& device, const ImageInfo& info, Image* out, std::string* error) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = info.format;
    ci.extent = {info.width, info.height, 1};
    ci.mipLevels = info.mip_levels;
    ci.arrayLayers = 1;
    ci.samples = info.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = info.usage | (info.transient ? VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT : 0u);
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = info.transient ? VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    Image image;
    VkResult r = vmaCreateImage(device.allocator(), &ci, &alloc, &image.image, &image.allocation, nullptr);
    image.lazy = info.transient && r == VK_SUCCESS;
    if (r != VK_SUCCESS && info.transient) {
        // No lazily allocated memory (desktop GPUs): ordinary device memory.
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        r = vmaCreateImage(device.allocator(), &ci, &alloc, &image.image, &image.allocation, nullptr);
    }
    if (!check(r, "vmaCreateImage", error))
        return false;
    if (info.name != nullptr)
        vmaSetAllocationName(device.allocator(), image.allocation, info.name);
    image.format = info.format;
    image.samples = info.samples;
    image.aspect = is_depth_format(info.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    image.width = info.width;
    image.height = info.height;
    image.mip_levels = info.mip_levels;
    image.transient = info.transient;
    if (!make_view(device, image, error)) {
        vmaDestroyImage(device.allocator(), image.image, image.allocation);
        return false;
    }
    *out = image;
    return true;
}

bool wrap_image(Device& device, VkImage handle, VkFormat format, std::uint32_t width, std::uint32_t height,
                Image* out, std::string* error) {
    Image image;
    image.image = handle;
    image.format = format;
    image.width = width;
    image.height = height;
    if (!make_view(device, image, error))
        return false;
    *out = image;
    return true;
}

void destroy_buffer(Device& device, Buffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(device.allocator(), buffer.buffer, buffer.allocation);
    buffer = Buffer{};
}

void destroy_image(Device& device, Image& image) {
    if (image.view != VK_NULL_HANDLE)
        vkDestroyImageView(device.vk(), image.view, nullptr);
    if (image.allocation != nullptr)
        vmaDestroyImage(device.allocator(), image.image, image.allocation);
    image = Image{};
}

void retire_buffer(Device& device, Buffer& buffer) {
    if (!buffer)
        return;
    Buffer b = buffer;
    buffer = Buffer{};
    device.retire([&device, b]() mutable { destroy_buffer(device, b); });
}

void retire_image(Device& device, Image& image) {
    if (!image)
        return;
    Image i = image;
    image = Image{};
    device.retire([&device, i]() mutable { destroy_image(device, i); });
}

void use_image(VkCommandBuffer cmd, Image& image, const ImageUse& next, bool discard) {
    const ImageUse& last = image.state;
    const bool last_wrote =
        (last.access & (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) != 0;
    const VkImageLayout from = discard ? VK_IMAGE_LAYOUT_UNDEFINED : last.layout;
    if (!discard && !last_wrote && from == next.layout && (next.access & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT) != 0 &&
        (last.access & ~VK_ACCESS_2_SHADER_SAMPLED_READ_BIT) == 0) {
        // Read after read in the same layout: no barrier, the stages accumulate.
        image.state.stage |= next.stage;
        return;
    }
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = last.stage == VK_PIPELINE_STAGE_2_NONE ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : last.stage;
    barrier.srcAccessMask = last.access;
    barrier.dstStageMask = next.stage == VK_PIPELINE_STAGE_2_NONE ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : next.stage;
    barrier.dstAccessMask = next.access;
    barrier.oldLayout = from;
    barrier.newLayout = next.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {image.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
    image.state = next;
}

}  // namespace shadow::render_vk
