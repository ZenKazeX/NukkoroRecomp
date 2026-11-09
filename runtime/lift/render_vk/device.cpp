#include "render_vk/device.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "vk_mem_alloc.h"

namespace shadow::render_vk {

const char* result_name(VkResult result) noexcept {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    default: return "VkResult(other)";
    }
}

bool check(VkResult result, const char* what, std::string* error) {
    if (result == VK_SUCCESS)
        return true;
    if (error != nullptr) {
        char text[32];
        std::snprintf(text, sizeof(text), " (%d)", static_cast<int>(result));
        *error = std::string(what) + ": " + result_name(result) + text;
    }
    return false;
}

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* user) {
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT || data == nullptr || data->pMessage == nullptr)
        return VK_FALSE;
    static_cast<Device*>(user)->note_validation_message(
        std::string(severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error: " : "warning: ") +
        data->pMessage);
    return VK_FALSE;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(),
                       [name](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

}  // namespace

Device::~Device() {
    destroy();
}

void Device::note_validation_message(std::string message) {
    std::lock_guard<std::mutex> lock(messages_mutex_);
    if (messages_.size() < 256)
        messages_.push_back(std::move(message));
}

std::vector<std::string> Device::take_validation_messages() {
    std::lock_guard<std::mutex> lock(messages_mutex_);
    std::vector<std::string> out;
    out.swap(messages_);
    return out;
}

bool Device::create(const DeviceOptions& options, std::string* error) {
    // ---- instance ----
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> instance_extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, instance_extensions.data());
    std::vector<const char*> extensions;
    // Surfaces when the platform has them; a headless renderer (tests, captures) needs none.
    if (has_extension(instance_extensions, VK_KHR_SURFACE_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        if (has_extension(instance_extensions, VK_KHR_WIN32_SURFACE_EXTENSION_NAME))
            extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
        if (has_extension(instance_extensions, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME))
            extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif
    }
    std::vector<const char*> layers;
    bool debug_utils = false;
    if (options.validation) {
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        for (const VkLayerProperties& l : available)
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                layers.push_back("VK_LAYER_KHRONOS_validation");
        if (has_extension(instance_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            debug_utils = true;
        }
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Shadow the Hedgehog";
    app.pEngineName = "shadow";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    if (!check(vkCreateInstance(&instance_info, nullptr, &instance_), "vkCreateInstance", error))
        return false;
    if (debug_utils) {
        auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            m.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            m.pfnUserCallback = debug_message;
            m.pUserData = this;
            create_messenger(instance_, &m, nullptr, &messenger_);
        }
    }

    // ---- adapter ----
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance_, &count, physicals.data());
    int best_score = -1;
    std::string seen;  // every device and its version, for the error when none qualifies
    for (VkPhysicalDevice p : physicals) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(p, &props);
        seen += std::string(seen.empty() ? "" : ", ") + props.deviceName + " (Vulkan " +
                std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." +
                std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + ")";
        if (props.apiVersion < VK_API_VERSION_1_3)
            continue;
        int score = 1;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score = options.prefer_integrated ? 2 : 3;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            score = options.prefer_integrated ? 3 : 2;
        if (!options.adapter.empty() && std::strstr(props.deviceName, options.adapter.c_str()) != nullptr)
            score = 10;
        if (score > best_score) {
            best_score = score;
            physical_ = p;
        }
    }
    if (physical_ == VK_NULL_HANDLE)
        return fail(error, "no Vulkan 1.3 device (found: " + (seen.empty() ? std::string("none") : seen) + ")");

    VkPhysicalDeviceVulkan12Properties props12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &props12;
    vkGetPhysicalDeviceProperties2(physical_, &props2);
    caps_.name = props2.properties.deviceName;
    caps_.api_version = props2.properties.apiVersion;
    caps_.signed_zero_inf_nan_f32 = props12.shaderSignedZeroInfNanPreserveFloat32 == VK_TRUE;
    caps_.max_sampled_images = props12.maxDescriptorSetUpdateAfterBindSampledImages;
    caps_.max_storage_buffers = props12.maxDescriptorSetUpdateAfterBindStorageBuffers;
    caps_.min_uniform_offset_alignment = props2.properties.limits.minUniformBufferOffsetAlignment;
    caps_.max_uniform_range = props2.properties.limits.maxUniformBufferRange;
    caps_.max_anisotropy = props2.properties.limits.maxSamplerAnisotropy;
    caps_.timestamps = props2.properties.limits.timestampComputeAndGraphics == VK_TRUE;

    // ---- the capability floor ----
    VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    have12.pNext = &have13;
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &have12;
    vkGetPhysicalDeviceFeatures2(physical_, &have);
    const struct {
        VkBool32 value;
        const char* name;
    } required[] = {
        {have13.dynamicRendering, "dynamicRendering"},
        {have13.synchronization2, "synchronization2"},
        {have13.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation"},
        {have12.timelineSemaphore, "timelineSemaphore"},
        {have12.descriptorIndexing, "descriptorIndexing"},
        {have12.runtimeDescriptorArray, "runtimeDescriptorArray"},
        {have12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {have12.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind"},
        {have12.descriptorBindingStorageBufferUpdateAfterBind, "descriptorBindingStorageBufferUpdateAfterBind"},
        {have12.descriptorBindingUpdateUnusedWhilePending, "descriptorBindingUpdateUnusedWhilePending"},
        {have.features.shaderSampledImageArrayDynamicIndexing, "shaderSampledImageArrayDynamicIndexing"},
        {have.features.shaderStorageBufferArrayDynamicIndexing, "shaderStorageBufferArrayDynamicIndexing"},
    };
    for (const auto& r : required)
        if (r.value != VK_TRUE)
            return fail(error, std::string("the device lacks ") + r.name + " (" + caps_.name + ")");
    caps_.scalar_block_layout = have12.scalarBlockLayout == VK_TRUE;
    caps_.sampler_anisotropy = have.features.samplerAnisotropy == VK_TRUE;

    // ---- one graphics + compute queue ----
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, families.data());
    bool found = false;
    for (std::uint32_t i = 0; i < count && !found; ++i) {
        const VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((families[i].queueFlags & want) == want) {
            queue_family_ = i;
            found = true;
        }
    }
    if (!found)
        return fail(error, "no graphics + compute queue family");
    caps_.timestamp_bits = families[queue_family_].timestampValidBits;
    caps_.timestamp_period = props2.properties.limits.timestampPeriod;
    caps_.timestamps = caps_.timestamps && caps_.timestamp_bits != 0u && caps_.timestamp_period > 0.0f;

    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> device_extensions(count);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, device_extensions.data());
    std::vector<const char*> enabled;
    if (has_extension(device_extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        enabled.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // Local read (attachment locations): a view's scene and glare sources in one rendering instance, when offered.
    LocalReadFeatures want_local;
    if (options.local_read && has_extension(device_extensions, kLocalReadExtension)) {
        LocalReadFeatures have_local;
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &have_local;
        vkGetPhysicalDeviceFeatures2(physical_, &query);
        if (have_local.dynamicRenderingLocalRead == VK_TRUE) {
            enabled.push_back(kLocalReadExtension);
            want_local.dynamicRenderingLocalRead = VK_TRUE;
            caps_.local_read = true;
        }
    }

    VkPhysicalDeviceVulkan13Features want13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    want13.dynamicRendering = VK_TRUE;
    want13.synchronization2 = VK_TRUE;
    want13.shaderDemoteToHelperInvocation = VK_TRUE;
    if (caps_.local_read)
        want13.pNext = &want_local;
    VkPhysicalDeviceVulkan12Features want12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    want12.pNext = &want13;
    want12.timelineSemaphore = VK_TRUE;
    want12.descriptorIndexing = VK_TRUE;
    want12.runtimeDescriptorArray = VK_TRUE;
    want12.descriptorBindingPartiallyBound = VK_TRUE;
    want12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    want12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    want12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    want12.scalarBlockLayout = have12.scalarBlockLayout;
    VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    want.pNext = &want12;
    want.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    want.features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
    want.features.samplerAnisotropy = have.features.samplerAnisotropy;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &want;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    device_info.ppEnabledExtensionNames = enabled.data();
    if (!check(vkCreateDevice(physical_, &device_info, nullptr, &device_), "vkCreateDevice", error))
        return false;
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    if (caps_.local_read) {
        set_locations_ = reinterpret_cast<PfnSetAttachmentLocations>(
            vkGetDeviceProcAddr(device_, "vkCmdSetRenderingAttachmentLocationsKHR"));
        caps_.local_read = set_locations_ != nullptr;
    }

    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.instance = instance_;
    allocator_info.physicalDevice = physical_;
    allocator_info.device = device_;
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
    if (!check(vmaCreateAllocator(&allocator_info, &allocator_), "vmaCreateAllocator", error))
        return false;

    VkSemaphoreTypeCreateInfo timeline_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_type.initialValue = 0;
    VkSemaphoreCreateInfo timeline_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    timeline_info.pNext = &timeline_type;
    return check(vkCreateSemaphore(device_, &timeline_info, nullptr, &timeline_), "vkCreateSemaphore (timeline)",
                 error);
}

bool Device::submit(VkCommandBuffer cmd, VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal,
                    std::uint64_t* value, std::string* error) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    const std::uint64_t next = submitted_.load(std::memory_order_relaxed) + 1;
    VkSemaphoreSubmitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait_info.semaphore = wait;
    wait_info.stageMask = wait_stage;
    VkSemaphoreSubmitInfo signals[2]{};
    signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[0].semaphore = timeline_;
    signals[0].value = next;
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[1].semaphore = signal;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command.commandBuffer = cmd;
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    info.waitSemaphoreInfoCount = wait != VK_NULL_HANDLE ? 1u : 0u;
    info.pWaitSemaphoreInfos = &wait_info;
    info.commandBufferInfoCount = 1;
    info.pCommandBufferInfos = &command;
    info.signalSemaphoreInfoCount = signal != VK_NULL_HANDLE ? 2u : 1u;
    info.pSignalSemaphoreInfos = signals;
    if (!check(vkQueueSubmit2(queue_, 1, &info, VK_NULL_HANDLE), "vkQueueSubmit2", error))
        return false;
    submitted_.store(next, std::memory_order_release);
    if (value != nullptr)
        *value = next;
    return true;
}

VkResult Device::present(const VkPresentInfoKHR& info) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return vkQueuePresentKHR(queue_, &info);
}

std::uint64_t Device::completed() const {
    std::uint64_t value = 0;
    if (device_ != VK_NULL_HANDLE && timeline_ != VK_NULL_HANDLE)
        vkGetSemaphoreCounterValue(device_, timeline_, &value);
    return value;
}

bool Device::wait(std::uint64_t value, std::uint64_t timeout_ns, std::string* error) const {
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &timeline_;
    info.pValues = &value;
    return check(vkWaitSemaphores(device_, &info, timeout_ns), "vkWaitSemaphores", error);
}

void Device::wait_idle() {
    if (device_ != VK_NULL_HANDLE)
        vkDeviceWaitIdle(device_);
}

void Device::retire(std::function<void()> destroy) {
    std::lock_guard<std::mutex> lock(retire_mutex_);
    retired_.push_back(Retired{submitted(), std::move(destroy)});
}

void Device::collect() {
    const std::uint64_t done = completed();
    std::deque<Retired> ready;
    {
        std::lock_guard<std::mutex> lock(retire_mutex_);
        while (!retired_.empty() && retired_.front().after <= done) {
            ready.push_back(std::move(retired_.front()));
            retired_.pop_front();
        }
    }
    for (Retired& r : ready)
        r.destroy();
}

void Device::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        std::deque<Retired> all;
        {
            std::lock_guard<std::mutex> lock(retire_mutex_);
            all.swap(retired_);
        }
        for (Retired& r : all)
            r.destroy();
        if (timeline_ != VK_NULL_HANDLE)
            vkDestroySemaphore(device_, timeline_, nullptr);
        timeline_ = VK_NULL_HANDLE;
        if (allocator_ != nullptr)
            vmaDestroyAllocator(allocator_);
        allocator_ = nullptr;
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (messenger_ != VK_NULL_HANDLE) {
        auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_messenger != nullptr)
            destroy_messenger(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE)
        vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    submitted_.store(0, std::memory_order_release);
}

}  // namespace shadow::render_vk
