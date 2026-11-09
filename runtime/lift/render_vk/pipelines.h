#pragma once

// Pipelines (docs/VULKAN_RENDERER.md 3.7): the D3D12 renderer's own HLSL (runtime/lift/render/shaders), compiled to
// SPIR-V by DXC at build time (CMakeLists.txt) and embedded; pipelines are made on first use and cached by their key,
// as the D3D12 renderer caches its PSOs. Dynamic rendering (no render-pass objects): a pipeline names its attachment
// formats and sample count. Viewport and scissor are dynamic state.
//
// Clip space: the shaders write D3D's (y up, z in [0, 1]); every viewport is set with a negative height
// (VK_KHR_maintenance1, core since 1.1) so the same clip space lands on the same pixels - no shader is changed.

#include <cstdint>
#include <map>
#include <string>
#include <tuple>

#include "render/frame.h"
#include "render_vk/bindless.h"
#include "render_vk/device.h"

namespace shadow::render_vk {

// The full-screen passes (render/renderer.h Screen; GlareComposite adds into the scene). Present: the compose into a
// swapchain image turned by the surface's pre-rotation (render_vk/shaders/present.hlsl + PSTexture).
enum class Screen : std::uint8_t {
    GlareCapture, GlareBlur, GlareBox, GlareMerge, GlareComposite, Resolve, Under, Scale, ResolveGlare, Present
};

// The draw programs (render/renderer.h Program): a mesh's base pass, its environment reflection, its second texture; a
// particle batch; immediate geometry.
enum class Program : std::uint8_t { Base, Environment, Dual, Particles, Immediate };

// The rendering instance a draw pipeline draws in: one colour attachment, or the pair of a view's scene colour
// (attachment 0) and its glare sources (attachment 1) - the draw writing one of them through the attachment locations
// of VK_KHR_dynamic_rendering_local_read (its output location 0 mapped to that attachment, the other unused).
enum class Pair : std::uint8_t { None, Scene, Sources };

// What a draw pipeline is made of (render/renderer.h PipelineKey): the recipe's blend, cull and depth, the target.
// Primitive topology is dynamic state (core Vulkan 1.3) within the pipeline's class: lines or triangles.
struct DrawPipelineKey {
    Program program = Program::Base;
    render::Blend src = render::Blend::One;
    render::Blend dst = render::Blend::Zero;
    render::Cull cull = render::Cull::None;
    bool depth_test = true;
    bool depth_write = true;
    VkFormat colour = VK_FORMAT_UNDEFINED;  // the attachment the draw writes
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool lines = false;
    bool depth_attachment = true;  // false: the instance has no depth (the draw neither tests nor writes it)
    Pair pair = Pair::None;
    VkFormat other = VK_FORMAT_UNDEFINED;  // in a pair: the other attachment's format
    bool operator<(const DrawPipelineKey& o) const {
        return std::tie(program, src, dst, cull, depth_test, depth_write, colour, samples, lines, depth_attachment,
                        pair, other) < std::tie(o.program, o.src, o.dst, o.cull, o.depth_test, o.depth_write,
                                                o.colour, o.samples, o.lines, o.depth_attachment, o.pair, o.other);
    }
};
// The attachment locations of a pair draw (attachment 0 scene, 1 sources).
inline constexpr std::uint32_t kSceneLocations[2] = {0u, VK_ATTACHMENT_UNUSED};
inline constexpr std::uint32_t kSourcesLocations[2] = {VK_ATTACHMENT_UNUSED, 0u};
inline constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

class Pipelines {
public:
    Pipelines() = default;
    Pipelines(const Pipelines&) = delete;
    Pipelines& operator=(const Pipelines&) = delete;
    ~Pipelines();

    bool create(Device& device, const Bindless& bindless, std::string* error);
    void destroy();

    VkPipeline screen(Screen screen, VkFormat colour, VkSampleCountFlagBits samples, std::string* error);
    // A draw pipeline: colour (or the scene / sources pair) + D32 depth attachments (or none).
    VkPipeline draw(const DrawPipelineKey& key, std::string* error);
    // The deformation pass (shaders/skinning.hlsl CSSkin, 64 vertices per group).
    VkPipeline skin(std::string* error);
    std::size_t count() const noexcept { return screens_.size() + draws_.size(); }

private:
    VkShaderModule module(const unsigned char* bytes, std::size_t size, std::string* error);

    Device* device_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::map<const unsigned char*, VkShaderModule> modules_;
    std::map<std::tuple<Screen, VkFormat, VkSampleCountFlagBits>, VkPipeline> screens_;
    std::map<DrawPipelineKey, VkPipeline> draws_;
    VkPipeline skin_ = VK_NULL_HANDLE;
};

// A rectangle in pixels, y down.
struct PixelRect {
    float x = 0, y = 0, width = 0, height = 0;
};
// Where a rectangle of the picture (in the display's orientation, `logical_width` x `logical_height`) lies in a
// swapchain image whose content is turned `quarter_turns` x 90 degrees clockwise (the surface's pre-rotation).
PixelRect turned_rect(const PixelRect& r, std::uint32_t quarter_turns, float logical_width, float logical_height);

// The viewport of the rectangle (x, y, width, height) in pixels, y down, with D3D's clip space (negative height).
void set_viewport(VkCommandBuffer cmd, float x, float y, float width, float height);
void set_scissor(VkCommandBuffer cmd, std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height);

}  // namespace shadow::render_vk
