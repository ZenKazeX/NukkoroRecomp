#include "render_vk/report.h"

#include <cstdio>

#include "render_vk/resources.h"

namespace shadow::render_vk {

namespace {

const char* yes(bool v) {
    return v ? "yes" : "no";
}

bool format_feature(VkPhysicalDevice physical, VkFormat format, VkFormatFeatureFlags want) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(physical, format, &props);
    return (props.optimalTilingFeatures & want) == want;
}

template <class... Args>
std::string line(const char* format, Args... args) {
    char text[512];
    std::snprintf(text, sizeof(text), format, args...);
    return text;
}

}  // namespace

std::vector<std::string> capability_report(const Device& device) {
    std::vector<std::string> out;
    const Capabilities& caps = device.caps();
    VkPhysicalDevice physical = device.physical();
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    out.push_back(line("device: %s, Vulkan %u.%u.%u, driver 0x%x, vendor 0x%x", caps.name.c_str(),
                       VK_API_VERSION_MAJOR(caps.api_version), VK_API_VERSION_MINOR(caps.api_version),
                       VK_API_VERSION_PATCH(caps.api_version), props.driverVersion, props.vendorID));
    out.push_back(line("caps: scalarBlockLayout %s, signedZeroInfNanPreserve f32 %s, anisotropy %s (max %.0f), "
                       "timestamps %s (%u bits, %.3f ns), dynamic rendering local read %s",
                       yes(caps.scalar_block_layout), yes(caps.signed_zero_inf_nan_f32), yes(caps.sampler_anisotropy),
                       static_cast<double>(caps.max_anisotropy), yes(caps.timestamps), caps.timestamp_bits,
                       static_cast<double>(caps.timestamp_period), yes(caps.local_read)));
    out.push_back(line("caps: update-after-bind sampled images %u, storage buffers %u, min UBO offset %llu, max UBO "
                       "range %llu",
                       caps.max_sampled_images, caps.max_storage_buffers,
                       static_cast<unsigned long long>(caps.min_uniform_offset_alignment),
                       static_cast<unsigned long long>(caps.max_uniform_range)));
    const struct {
        VkFormat format;
        const char* name;
    } scene_formats[] = {{VK_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM"},
                         {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT"},
                         {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM"},
                         {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"}};
    for (const auto& f : scene_formats)
        out.push_back(line("format %s: blended target 1x %s, 4x %s", f.name,
                           yes(colour_target_supported(physical, f.format, VK_SAMPLE_COUNT_1_BIT, true)),
                           yes(colour_target_supported(physical, f.format, VK_SAMPLE_COUNT_4_BIT, true))));
    const VkFormatFeatureFlags depth = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    out.push_back(line("depth: D32_SFLOAT %s, X8_D24 %s, D24S8 %s",
                       yes(format_feature(physical, VK_FORMAT_D32_SFLOAT, depth)),
                       yes(format_feature(physical, VK_FORMAT_X8_D24_UNORM_PACK32, depth)),
                       yes(format_feature(physical, VK_FORMAT_D24_UNORM_S8_UINT, depth))));
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    out.push_back(line("textures: ASTC LDR %s, ETC2 %s, BC %s", yes(features.textureCompressionASTC_LDR == VK_TRUE),
                       yes(features.textureCompressionETC2 == VK_TRUE), yes(features.textureCompressionBC == VK_TRUE)));
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    bool lazy = false;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        lazy |= (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0;
    out.push_back(line("memory: %u types, %u heaps, lazily allocated (tile memory) %s", memory.memoryTypeCount,
                       memory.memoryHeapCount, yes(lazy)));
    return out;
}

}  // namespace shadow::render_vk
