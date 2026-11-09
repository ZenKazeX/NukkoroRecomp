#include "render_vk/swapchain.h"

#include <algorithm>

namespace shadow::render_vk {

Swapchain::~Swapchain() {
    destroy();
}

bool Swapchain::create(Device& device, const NativeWindow& window, std::uint32_t width, std::uint32_t height,
                       PresentMode mode, std::uint32_t frames_in_flight, std::string* error) {
    device_ = &device;
    mode_ = mode;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    VkWin32SurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface.hinstance = static_cast<HINSTANCE>(window.instance != nullptr ? window.instance : GetModuleHandleW(nullptr));
    surface.hwnd = static_cast<HWND>(window.handle);
    if (!check(vkCreateWin32SurfaceKHR(device.instance(), &surface, nullptr, &surface_), "vkCreateWin32SurfaceKHR",
               error))
        return false;
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
    VkAndroidSurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    surface.window = static_cast<ANativeWindow*>(window.handle);
    if (!check(vkCreateAndroidSurfaceKHR(device.instance(), &surface, nullptr, &surface_), "vkCreateAndroidSurfaceKHR",
               error))
        return false;
#else
    (void)window;
    return fail(error, "no window system on this platform");
#endif
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device.physical(), device.queue_family(), surface_, &supported);
    if (supported != VK_TRUE)
        return fail(error, "the renderer's queue cannot present to this surface");

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device.physical(), surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device.physical(), surface_, &count, formats.data());
    // UNORM: the picture is written as the D3D12 renderer's back buffer is (R8G8B8A8_UNORM, no sRGB conversion).
    format_ = VK_FORMAT_UNDEFINED;
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32})
        for (const VkSurfaceFormatKHR& f : formats)
            if (format_ == VK_FORMAT_UNDEFINED && f.format == want &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format_ = f.format;
                colour_space_ = f.colorSpace;
            }
    if (format_ == VK_FORMAT_UNDEFINED)
        return fail(error, "the surface offers no 8-bit UNORM format");

    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    acquired_.assign(frames_in_flight, VK_NULL_HANDLE);
    for (VkSemaphore& s : acquired_)
        if (!check(vkCreateSemaphore(device.vk(), &semaphore, nullptr, &s), "vkCreateSemaphore", error))
            return false;
    return build(width, height, error);
}

bool Swapchain::build(std::uint32_t width, std::uint32_t height, std::string* error) {
    VkSurfaceCapabilitiesKHR caps;
    if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->physical(), surface_, &caps),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", error))
        return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0)
        return fail(error, "the surface has no area (minimised)");
    // Pre-rotation: a turned surface gets images in the native orientation (its extent's sides swapped for a quarter
    // or three-quarter turn) and the picture is drawn turned into them. Other transforms (mirrors) are left to the
    // compositor (identity) where it accepts that.
    const VkExtent2D logical = extent;
    switch (caps.currentTransform) {
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: quarter_turns_ = 1; break;
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: quarter_turns_ = 2; break;
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: quarter_turns_ = 3; break;
    default: quarter_turns_ = 0; break;
    }
    transform_ = caps.currentTransform;
    if (quarter_turns_ == 0 && caps.currentTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR &&
        (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0)
        transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    if ((quarter_turns_ & 1u) != 0)
        std::swap(extent.width, extent.height);

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device_->physical(), surface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device_->physical(), surface_, &count, modes.data());
    VkPresentModeKHR present = VK_PRESENT_MODE_FIFO_KHR;
    if (mode_ == PresentMode::Immediate) {
        for (VkPresentModeKHR want : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
            if (present == VK_PRESENT_MODE_FIFO_KHR && std::find(modes.begin(), modes.end(), want) != modes.end())
                present = want;
    }
    std::uint32_t image_count = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount != 0)
        image_count = std::min(image_count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = format_;
    info.imageColorSpace = colour_space_;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = transform_;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = present;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR made = VK_NULL_HANDLE;
    if (!check(vkCreateSwapchainKHR(device_->vk(), &info, nullptr, &made), "vkCreateSwapchainKHR", error))
        return false;
    if (swapchain_ != VK_NULL_HANDLE) {
        device_->wait_idle();
        release_images();
        vkDestroySwapchainKHR(device_->vk(), swapchain_, nullptr);
    }
    swapchain_ = made;
    extent_ = extent;
    logical_ = logical;

    vkGetSwapchainImagesKHR(device_->vk(), swapchain_, &count, nullptr);
    std::vector<VkImage> handles(count);
    vkGetSwapchainImagesKHR(device_->vk(), swapchain_, &count, handles.data());
    images_.resize(count);
    ready_.assign(count, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!wrap_image(*device_, handles[i], format_, extent.width, extent.height, &images_[i], error))
            return false;
        if (!check(vkCreateSemaphore(device_->vk(), &semaphore, nullptr, &ready_[i]), "vkCreateSemaphore", error))
            return false;
    }
    return true;
}

bool Swapchain::resize(std::uint32_t width, std::uint32_t height, std::string* error) {
    if (device_ == nullptr || surface_ == VK_NULL_HANDLE)
        return fail(error, "no swapchain");
    device_->wait_idle();
    return build(width, height, error);
}

bool Swapchain::acquire(std::uint32_t frame_slot, std::uint32_t* image, bool* out_of_date, std::string* error) {
    *out_of_date = false;
    const VkResult r =
        vkAcquireNextImageKHR(device_->vk(), swapchain_, UINT64_MAX, acquired_[frame_slot], VK_NULL_HANDLE, image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        *out_of_date = true;
        return true;
    }
    if (r == VK_SUBOPTIMAL_KHR)
        return true;  // usable; the next present reports it and the caller resizes
    return check(r, "vkAcquireNextImageKHR", error);
}

bool Swapchain::present(std::uint32_t image, bool* out_of_date, std::string* error) {
    *out_of_date = false;
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &ready_[image];
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &image;
    const VkResult r = device_->present(info);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        *out_of_date = true;
        return true;
    }
    return check(r, "vkQueuePresentKHR", error);
}

void Swapchain::release_images() {
    for (Image& i : images_)
        destroy_image(*device_, i);  // wrapped: only the view is ours
    images_.clear();
    for (VkSemaphore s : ready_)
        if (s != VK_NULL_HANDLE)
            vkDestroySemaphore(device_->vk(), s, nullptr);
    ready_.clear();
}

void Swapchain::destroy() {
    if (device_ == nullptr)
        return;
    device_->wait_idle();
    release_images();
    for (VkSemaphore s : acquired_)
        if (s != VK_NULL_HANDLE)
            vkDestroySemaphore(device_->vk(), s, nullptr);
    acquired_.clear();
    if (swapchain_ != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(device_->vk(), swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    if (surface_ != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(device_->instance(), surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    device_ = nullptr;
}

}  // namespace shadow::render_vk
