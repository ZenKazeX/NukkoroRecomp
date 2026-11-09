#pragma once

// The Vulkan renderer's common ground (docs/VULKAN_RENDERER.md): the platform's Vulkan header and the small helpers
// every file of render_vk uses. The renderer talks to Vulkan directly - no portability layer - exactly as the D3D12
// renderer talks to D3D12.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#elif defined(__ANDROID__)
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#endif

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace shadow::render_vk {

const char* result_name(VkResult result) noexcept;

// False (and `error` = "<what>: <result>") unless `result` is VK_SUCCESS.
bool check(VkResult result, const char* what, std::string* error);

inline bool fail(std::string* error, const std::string& what) {
    if (error != nullptr)
        *error = what;
    return false;
}

// The window a swapchain presents to: Win32 (`handle` the HWND, `instance` the HINSTANCE) or Android (`handle` the
// ANativeWindow*). Plain pointers so this header stays free of both platforms' window headers.
// VK_KHR_dynamic_rendering_local_read (core in Vulkan 1.4): the attachment-location remap the renderer uses to draw a
// view's scene and its glare sources in one rendering instance (docs/VULKAN_RENDERER.md 3.8). Declared here with the
// registry's values because the NDK's headers (VK_HEADER_VERSION 275) predate it; layouts as in vulkan_core.h.
inline constexpr const char* kLocalReadExtension = "VK_KHR_dynamic_rendering_local_read";
inline constexpr auto kLocalReadFeaturesType = static_cast<VkStructureType>(1000232000);
inline constexpr auto kAttachmentLocationInfoType = static_cast<VkStructureType>(1000232001);
struct LocalReadFeatures {  // VkPhysicalDeviceDynamicRenderingLocalReadFeatures
    VkStructureType sType = kLocalReadFeaturesType;
    void* pNext = nullptr;
    VkBool32 dynamicRenderingLocalRead = VK_FALSE;
};
struct AttachmentLocationInfo {  // VkRenderingAttachmentLocationInfo
    VkStructureType sType = kAttachmentLocationInfoType;
    const void* pNext = nullptr;
    std::uint32_t colorAttachmentCount = 0;
    const std::uint32_t* pColorAttachmentLocations = nullptr;
};
using PfnSetAttachmentLocations = void(VKAPI_PTR*)(VkCommandBuffer, const AttachmentLocationInfo*);
inline constexpr std::uint32_t kUnusedLocation = VK_ATTACHMENT_UNUSED;

struct NativeWindow {
    void* handle = nullptr;
    void* instance = nullptr;
};

}  // namespace shadow::render_vk
