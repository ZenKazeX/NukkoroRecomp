// The VulkanMemoryAllocator's implementation, compiled once for the Vulkan renderer (the D3D12 side compiles
// D3D12MemAlloc.cpp the same way in shadow_d3d12ma).

#include "render_vk/vk_common.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include "vk_mem_alloc.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
