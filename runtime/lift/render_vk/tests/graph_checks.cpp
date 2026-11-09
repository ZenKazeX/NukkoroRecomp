#include "render_vk/tests/graph_checks.h"

#include <cstdio>
#include <vector>

#include "render/frame.h"
#include "render_vk/gpu_timer.h"

namespace shadow::render_vk {

namespace {

template <class... Args>
std::string line(const char* format, Args... args) {
    char text[512];
    std::snprintf(text, sizeof(text), format, args...);
    return text;
}

std::uint32_t rgba8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    return r | (g << 8) | (b << 16) | (static_cast<std::uint32_t>(a) << 24);
}

VkClearValue colour(float r, float g, float b) {
    VkClearValue v{};
    v.color = {{r, g, b, 1.0f}};
    return v;
}

void canvas_quad(render::FrameBuilder& builder, float x0, float y0, float x1, float y1, std::uint32_t c,
                 bool depth_test = true) {
    const render::ImmediateVertex v[4] = {{{x0, y0, 0.0f}, c, {0, 0}}, {{x1, y0, 0.0f}, c, {1, 0}},
                                          {{x1, y1, 0.0f}, c, {1, 1}}, {{x0, y1, 0.0f}, c, {0, 1}}};
    const std::uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    render::ImmediateDraw draw;
    draw.space = render::Space::Canvas;
    draw.anchor = render::Anchor::Fill;
    render::Recipe recipe;
    recipe.cull = render::Cull::None;
    recipe.depth_write = false;
    recipe.depth_test = depth_test;
    builder.immediate(draw, v, 4, indices, 6, render::Float3x4{}, recipe);
}

void copy_image(VkCommandBuffer cmd, const Image& from, const Image& to, std::int32_t x, std::uint32_t width) {
    VkImageCopy c{};
    c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.dstSubresource = c.srcSubresource;
    c.srcOffset = {x, 0, 0};
    c.dstOffset = c.srcOffset;
    c.extent = {width, from.height, 1};
    vkCmdCopyImage(cmd, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &c);
}

// The graph on its own: declarations, compile, execute on the GPU.
int graph_alone(Device& device, const std::function<void(const std::string&)>& log) {
    int failures = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        log(std::string(ok ? "  ok   " : "  FAIL ") + what);
        failures += ok ? 0 : 1;
    };
    std::string error;
    constexpr std::uint32_t kSide = 8;
    TargetPool pool;
    pool.attach(device);
    Image out, out2;
    Buffer readback;
    ImageInfo info;
    info.width = kSide;
    info.height = kSide;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.name = "graph check output";
    VkCommandPool commands = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device.queue_family();
    if (!create_image(device, info, &out, &error) || !create_image(device, info, &out2, &error) ||
        !create_buffer(device, VkDeviceSize{kSide} * kSide * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HostAccess::Read,
                       &readback, &error) ||
        vkCreateCommandPool(device.vk(), &pool_info, nullptr, &commands) != VK_SUCCESS) {
        log("graph checks: setup: " + error);
        destroy_image(device, out);
        destroy_image(device, out2);
        destroy_buffer(device, readback);
        return 1;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commands;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    vkAllocateCommandBuffers(device.vk(), &allocate, &cmd);

    // The frame: x cleared red (with a depth nobody reads after), y cleared and never read, x copied whole into out
    // and read again into out2 (a read after a read), z cleared green after x's last use (it takes over x's image),
    // z's right half copied over out, out read back.
    const TextureDesc colour_desc{kSide, kSide, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, false};
    const TextureDesc depth_desc{kSide, kSide, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, false};
    FrameGraph graph;
    ResourceId x = kNoResource, y = kNoResource, z = kNoResource, depth = kNoResource;
    VkRenderingAttachmentInfo seen_x{}, seen_depth{};
    const auto clear_pass = [](PassContext& ctx, ResourceId target, ResourceId depth_target,
                               VkRenderingAttachmentInfo* seen, VkRenderingAttachmentInfo* seen_depth_info) {
        const VkRenderingAttachmentInfo a = ctx.attachment(target);
        VkRenderingAttachmentInfo d{};
        if (depth_target != kNoResource)
            d = ctx.attachment(depth_target);
        if (seen != nullptr)
            *seen = a;
        if (seen_depth_info != nullptr)
            *seen_depth_info = d;
        VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
        r.renderArea = {{0, 0}, {kSide, kSide}};
        r.layerCount = 1;
        r.colorAttachmentCount = 1;
        r.pColorAttachments = &a;
        r.pDepthAttachment = depth_target != kNoResource ? &d : nullptr;
        vkCmdBeginRendering(ctx.cmd(), &r);
        vkCmdEndRendering(ctx.cmd());
        return true;
    };
    std::uint32_t pass_c2 = 0;
    const auto build = [&](std::uint64_t frame) {
        graph.reset(frame);
        x = graph.create_texture("x", colour_desc);
        y = graph.create_texture("y", colour_desc);
        z = graph.create_texture("z", colour_desc);
        depth = graph.create_texture("depth", depth_desc);
        const ResourceId o = graph.import_image("out", out);
        const ResourceId o2 = graph.import_image("out2", out2);
        graph.add_pass(
            "clear x",
            [&](PassBuilder& b) {
                b.write(x, Access::RenderTarget, Load::Clear, colour(1, 0, 0));
                VkClearValue far_plane{};
                far_plane.depthStencil = {1.0f, 0};
                b.write(depth, Access::DepthWrite, Load::Clear, far_plane);
            },
            [&](PassContext& ctx, std::string*) { return clear_pass(ctx, x, depth, &seen_x, &seen_depth); });
        graph.add_pass(
            "clear y", [&](PassBuilder& b) { b.write(y, Access::RenderTarget, Load::Clear, colour(0, 0, 1)); },
            [&](PassContext& ctx, std::string*) { return clear_pass(ctx, y, kNoResource, nullptr, nullptr); });
        graph.add_pass(
            "x to out",
            [&](PassBuilder& b) {
                b.read(x, Access::CopySource);
                b.write(o, Access::CopyDest, Load::Discard);
            },
            [&, o](PassContext& ctx, std::string*) {
                copy_image(ctx.cmd(), ctx.image(x), ctx.image(o), 0, kSide);
                return true;
            });
        pass_c2 = static_cast<std::uint32_t>(graph.pass_count());
        graph.add_pass(
            "x to out2",
            [&](PassBuilder& b) {
                b.read(x, Access::CopySource);
                b.write(o2, Access::CopyDest, Load::Discard);
            },
            [&, o2](PassContext& ctx, std::string*) {
                copy_image(ctx.cmd(), ctx.image(x), ctx.image(o2), 0, kSide);
                return true;
            });
        graph.add_pass(
            "clear z", [&](PassBuilder& b) { b.write(z, Access::RenderTarget, Load::Clear, colour(0, 1, 0)); },
            [&](PassContext& ctx, std::string*) { return clear_pass(ctx, z, kNoResource, nullptr, nullptr); });
        graph.add_pass(
            "z right half to out",
            [&](PassBuilder& b) {
                b.read(z, Access::CopySource);
                b.write(o, Access::CopyDest, Load::Keep);
            },
            [&, o](PassContext& ctx, std::string*) {
                copy_image(ctx.cmd(), ctx.image(z), ctx.image(o), kSide / 2, kSide / 2);
                return true;
            });
        graph.add_pass(
            "readback",
            [&](PassBuilder& b) {
                b.read(o, Access::CopySource);
                b.side_effect();
            },
            [&, o](PassContext& ctx, std::string*) {
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {kSide, kSide, 1};
                vkCmdCopyImageToBuffer(ctx.cmd(), ctx.image(o).image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       readback.buffer, 1, &region);
                VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                to_host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &to_host;
                vkCmdPipelineBarrier2(ctx.cmd(), &dependency);
                return true;
            });
    };
    const auto run = [&](std::uint64_t frame, std::vector<std::uint8_t>* pixels) {
        build(frame);
        if (!graph.compile(pool, &error))
            return false;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        const bool recorded = graph.execute(cmd, &error);
        vkEndCommandBuffer(cmd);
        pool.end_frame(frame);
        std::uint64_t value = 0;
        if (!recorded || !device.submit(cmd, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, &value, &error) ||
            !device.wait(value, UINT64_MAX, &error))
            return false;
        invalidate_buffer(device, readback);
        const auto* bytes = static_cast<const std::uint8_t*>(readback.mapped);
        pixels->assign(bytes, bytes + std::size_t{kSide} * kSide * 4u);
        return true;
    };

    std::vector<std::uint8_t> pixels;
    if (!run(1, &pixels)) {
        expect(false, "frame 1 compiled and ran: " + error);
    } else {
        const auto kept = graph.kept_passes();
        const auto culled = graph.culled_passes();
        expect(kept.size() == 6 && culled.size() == 1 && culled[0] == "clear y",
               line("culling: %zu kept, %zu culled (%s): 'clear y' nothing reads", kept.size(), culled.size(),
                    culled.empty() ? "-" : culled[0].c_str()));
        const FrameGraph::Stats& s = graph.stats();
        expect(graph.image_of(z) == graph.image_of(x) && graph.image_of(z) != nullptr && s.aliased == 1 &&
                   s.images == 2 && s.textures == 3,
               line("aliasing: z takes over x's image (%s); %u textures in %u images, %u aliased",
                    graph.image_of(z) == graph.image_of(x) ? "same image" : "different images", s.textures, s.images,
                    s.aliased));
        // The barriers of x's image: cleared from UNDEFINED, then to a copy source; none for the second read; z's
        // clear discards it again.
        int x_barriers = 0, x_discards = 0, second_read = 0;
        bool layouts_ok = true;
        for (const FrameGraph::Barrier& b : graph.barriers()) {
            if (b.resource == x) {
                ++x_barriers;
                x_discards += b.discard ? 1 : 0;
                if (b.pass == 0)
                    layouts_ok = layouts_ok && b.discard && b.after.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                else
                    layouts_ok = layouts_ok && !b.discard &&
                                 b.before.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                                 b.after.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                second_read += b.pass == pass_c2 ? 1 : 0;
            }
            if (b.resource == z && b.after.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                layouts_ok = layouts_ok && b.discard && b.before.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        expect(x_barriers == 2 && x_discards == 1 && second_read == 0 && layouts_ok,
               line("barriers of x: %d (%d discarding), %d before its second read, layouts %s", x_barriers, x_discards,
                    second_read, layouts_ok ? "as planned" : "WRONG"));
        expect(seen_x.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && seen_x.storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
                   seen_depth.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
                   seen_depth.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE,
               line("attachment ops: x load %d store %d (clear, store), depth load %d store %d (clear, don't care)",
                    static_cast<int>(seen_x.loadOp), static_cast<int>(seen_x.storeOp),
                    static_cast<int>(seen_depth.loadOp), static_cast<int>(seen_depth.storeOp)));
        const auto at = [&](std::uint32_t px, std::uint32_t py) {
            const std::uint8_t* p = &pixels[(std::size_t{py} * kSide + px) * 4u];
            return rgba8(p[0], p[1], p[2], p[3]);
        };
        expect(at(1, 1) == rgba8(255, 0, 0) && at(1, 6) == rgba8(255, 0, 0) && at(6, 1) == rgba8(0, 255, 0) &&
                   at(6, 6) == rgba8(0, 255, 0),
               line("executed: left 0x%08x (red), right 0x%08x (green)", at(1, 1), at(6, 1)));
        const std::uint64_t made = pool.created();
        std::vector<std::uint8_t> again;
        const bool second = run(2, &again);
        expect(second && pool.created() == made && again == pixels,
               line("pooling: frame 2 made %llu new images (0), same pixels: %s",
                    static_cast<unsigned long long>(pool.created() - made), again == pixels ? "yes" : "no"));
    }

    // Invalid graphs, refused at compile with the reason.
    const auto refused = [&](const char* what, const char* reason, const std::function<void()>& declare) {
        graph.reset(3);
        declare();
        std::string why;
        const bool compiled = graph.compile(pool, &why);
        expect(!compiled && why.find(reason) != std::string::npos,
               line("refused: %s (%s)", what, compiled ? "COMPILED" : why.c_str()));
    };
    const auto none = [](PassContext&, std::string*) { return true; };
    refused("a read before any write", "before any pass wrote it", [&] {
        const ResourceId t = graph.create_texture("t", colour_desc);
        const ResourceId o = graph.import_image("out", out);
        graph.add_pass(
            "read t",
            [&](PassBuilder& b) {
                b.read(t, Access::CopySource);
                b.write(o, Access::CopyDest, Load::Discard);
            },
            none);
    });
    refused("an add before any write", "adds to", [&] {
        const ResourceId t = graph.create_texture("t", colour_desc);
        graph.add_pass(
            "add to t",
            [&](PassBuilder& b) {
                b.write(t, Access::RenderTarget, Load::Keep);
                b.side_effect();
            },
            none);
    });
    refused("one pass reading and writing a texture", "both as", [&] {
        const ResourceId o = graph.import_image("out", out);
        graph.add_pass(
            "out onto itself",
            [&](PassBuilder& b) {
                b.read(o, Access::CopySource);
                b.write(o, Access::CopyDest, Load::Keep);
            },
            none);
    });
    refused("a depth access of a colour texture", "DepthWrite of the colour texture", [&] {
        const ResourceId o = graph.import_image("out", out);
        graph.add_pass("depth into out", [&](PassBuilder& b) { b.write(o, Access::DepthWrite, Load::Clear); }, none);
    });
    refused("a clear of a copy destination", "only render-target and depth writes can clear", [&] {
        const ResourceId o = graph.import_image("out", out);
        graph.add_pass("clear by copy", [&](PassBuilder& b) { b.write(o, Access::CopyDest, Load::Clear); }, none);
    });

    device.wait_idle();
    pool.destroy();
    vkDestroyCommandPool(device.vk(), commands, nullptr);
    destroy_image(device, out);
    destroy_image(device, out2);
    destroy_buffer(device, readback);
    return failures;
}

// V6 on the graph alone: two mergeable passes on one colour and depth become one rendering instance (the second's
// barrier dropped, the instance's store ops those of its last pass), an attachment-only texture gets a transient image,
// a layout change or a clear ends the instance, and the GPU timer measures one span per rendering group.
int graph_v6(Device& device, const std::function<void(const std::string&)>& log) {
    int failures = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        log(std::string(ok ? "  ok   " : "  FAIL ") + what);
        failures += ok ? 0 : 1;
    };
    std::string error;
    constexpr std::uint32_t kSide = 8;
    TargetPool pool;
    pool.attach(device);
    GpuTimer timer;
    Image out;
    Buffer readback;
    ImageInfo info;
    info.width = kSide;
    info.height = kSide;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.name = "graph check output";
    VkCommandPool commands = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device.queue_family();
    if (!create_image(device, info, &out, &error) ||
        !create_buffer(device, VkDeviceSize{kSide} * kSide * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HostAccess::Read,
                       &readback, &error) ||
        !timer.create(device, 1, 16, &error) ||
        vkCreateCommandPool(device.vk(), &pool_info, nullptr, &commands) != VK_SUCCESS) {
        log("graph V6 checks: setup: " + error);
        destroy_image(device, out);
        destroy_buffer(device, readback);
        return 1;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commands;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    vkAllocateCommandBuffers(device.vk(), &allocate, &cmd);

    // The frame: "draw a" clears c red and d to the far plane; "draw b" keeps both and clears c's right half green
    // inside the instance (vkCmdClearAttachments: a draw's stand-in); `third` is a third pass on c and d - "draw c"
    // (keeps both, merges too), "test d" (d read-only: a layout change) or "clear c" (a clear) - then c is copied out
    // and read back.
    enum class Third { Keep, DepthRead, Clear };
    const TextureDesc colour_desc{kSide, kSide, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, false};
    const TextureDesc depth_desc{kSide, kSide, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, false};
    FrameGraph graph;
    ResourceId c = kNoResource, d = kNoResource;
    VkRenderingAttachmentInfo seen_c{}, seen_d{};
    const auto draw = [&](PassContext& ctx, std::string* e, bool record, bool green_half) {
        const VkRenderingAttachmentInfo a = ctx.attachment(c);
        const VkRenderingAttachmentInfo z = ctx.attachment(d);
        if (record) {
            seen_c = a;
            seen_d = z;
        }
        VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
        r.renderArea = {{0, 0}, {kSide, kSide}};
        r.layerCount = 1;
        r.colorAttachmentCount = 1;
        r.pColorAttachments = &a;
        r.pDepthAttachment = &z;
        if (!ctx.begin_rendering(r, e))
            return false;
        if (green_half) {
            VkClearAttachment clear{};
            clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clear.colorAttachment = 0;
            clear.clearValue = colour(0, 1, 0);
            const VkClearRect rect{{{kSide / 2, 0}, {kSide / 2, kSide}}, 0, 1};
            vkCmdClearAttachments(ctx.cmd(), 1, &clear, 1, &rect);
        }
        ctx.end_rendering();
        return true;
    };
    std::uint32_t pass_b = 0;
    const auto build = [&](std::uint64_t frame, Third third) {
        graph.reset(frame);
        c = graph.create_texture("c", colour_desc);
        d = graph.create_texture("d", depth_desc);
        const ResourceId o = graph.import_image("out", out);
        VkClearValue far_plane{};
        far_plane.depthStencil = {1.0f, 0};
        graph.add_pass(
            "draw a",
            [&](PassBuilder& b) {
                b.mergeable();
                b.write(c, Access::RenderTarget, Load::Clear, colour(1, 0, 0));
                b.write(d, Access::DepthWrite, Load::Clear, far_plane);
            },
            [&](PassContext& ctx, std::string* e) { return draw(ctx, e, true, false); });
        pass_b = static_cast<std::uint32_t>(graph.pass_count());
        graph.add_pass(
            "draw b",
            [&](PassBuilder& b) {
                b.mergeable();
                b.write(c, Access::RenderTarget, Load::Keep);
                b.write(d, Access::DepthWrite, Load::Keep);
            },
            [&](PassContext& ctx, std::string* e) { return draw(ctx, e, false, true); });
        graph.add_pass(
            third == Third::Keep ? "draw c" : third == Third::DepthRead ? "test d" : "clear c",
            [&, third](PassBuilder& b) {
                b.mergeable();
                b.write(c, Access::RenderTarget, third == Third::Clear ? Load::Clear : Load::Keep, colour(1, 0, 0));
                if (third == Third::DepthRead)
                    b.read(d, Access::DepthRead);
                else
                    b.write(d, Access::DepthWrite, Load::Keep);
            },
            [&, third](PassContext& ctx, std::string* e) {
                // "clear c" redraws b's green half so that every variant shows the same picture.
                return draw(ctx, e, false, third == Third::Clear);
            });
        graph.add_pass(
            "c to out",
            [&](PassBuilder& b) {
                b.read(c, Access::CopySource);
                b.write(o, Access::CopyDest, Load::Discard);
            },
            [&, o](PassContext& ctx, std::string*) {
                copy_image(ctx.cmd(), ctx.image(c), ctx.image(o), 0, kSide);
                return true;
            });
        graph.add_pass(
            "readback",
            [&](PassBuilder& b) {
                b.read(o, Access::CopySource);
                b.side_effect();
            },
            [&, o](PassContext& ctx, std::string*) {
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {kSide, kSide, 1};
                vkCmdCopyImageToBuffer(ctx.cmd(), ctx.image(o).image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       readback.buffer, 1, &region);
                VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                to_host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &to_host;
                vkCmdPipelineBarrier2(ctx.cmd(), &dependency);
                return true;
            });
    };
    std::uint64_t frame_number = 1;
    const auto run = [&](Third third, bool merging, std::vector<std::uint8_t>* pixels) {
        const std::uint64_t frame = frame_number++;
        build(frame, third);
        graph.set_merging(merging);
        if (!graph.compile(pool, &error))
            return false;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        timer.begin(cmd, 0);
        const bool recorded = graph.execute(cmd, &error, timer.enabled() ? &timer : nullptr);
        vkEndCommandBuffer(cmd);
        pool.end_frame(frame);
        std::uint64_t value = 0;
        if (!recorded || !device.submit(cmd, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, &value, &error) ||
            !device.wait(value, UINT64_MAX, &error))
            return false;
        timer.collect(0);
        invalidate_buffer(device, readback);
        const auto* bytes = static_cast<const std::uint8_t*>(readback.mapped);
        pixels->assign(bytes, bytes + std::size_t{kSide} * kSide * 4u);
        return true;
    };
    const auto join = [](const std::vector<std::string>& names) {
        std::string text;
        for (const std::string& n : names)
            text += (text.empty() ? "" : " | ") + n;
        return text;
    };

    std::vector<std::uint8_t> merged, plain, depth_read, cleared;
    if (!run(Third::Keep, true, &merged)) {
        expect(false, "V6 frame compiled and ran: " + error);
    } else {
        const FrameGraph::Stats s = graph.stats();
        const std::vector<std::string> groups = graph.groups();
        int b_barriers = 0;
        for (const FrameGraph::Barrier& b : graph.barriers())
            b_barriers += b.pass == pass_b ? 1 : 0;
        expect(s.merged == 2 && groups.size() == 3 && groups[0] == "draw a + draw b + draw c" && b_barriers == 0,
               line("merged: %u passes into the one before (2), groups: %s; barriers before 'draw b': %d (0)", s.merged,
                    join(groups).c_str(), b_barriers));
        expect(seen_c.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && seen_c.storeOp == VK_ATTACHMENT_STORE_OP_STORE &&
                   seen_d.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR &&
                   seen_d.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE,
               line("instance ops: c load %d store %d (clear, store: copied later), d load %d store %d (clear, don't "
                    "care: no pass after the instance)",
                    static_cast<int>(seen_c.loadOp), static_cast<int>(seen_c.storeOp),
                    static_cast<int>(seen_d.loadOp), static_cast<int>(seen_d.storeOp)));
        const Image* ci = graph.image_of(c);
        const Image* di = graph.image_of(d);
        expect(s.transient == 1 && di != nullptr && di->transient && ci != nullptr && !ci->transient,
               line("transient: %u texture (d, attachment-only: %s; c, copied: %s), lazily allocated memory: %s",
                    s.transient, di != nullptr && di->transient ? "transient" : "NOT transient",
                    ci != nullptr && ci->transient ? "TRANSIENT" : "not transient",
                    di != nullptr && di->lazy ? "yes" : "no (none on this GPU)"));
        const auto at = [&](std::uint32_t px, std::uint32_t py) {
            const std::uint8_t* p = &merged[(std::size_t{py} * kSide + px) * 4u];
            return rgba8(p[0], p[1], p[2], p[3]);
        };
        expect(at(1, 1) == rgba8(255, 0, 0) && at(6, 6) == rgba8(0, 255, 0),
               line("merged instance executed: left 0x%08x (red), right 0x%08x (green)", at(1, 1), at(6, 6)));
        if (timer.enabled()) {
            const auto& spans = timer.spans();
            bool names = spans.size() == groups.size();
            double sum = 0.0;
            for (std::size_t i = 0; names && i < spans.size(); ++i) {
                names = spans[i].name == groups[i] && spans[i].ms >= 0.0;
                sum += spans[i].ms;
            }
            expect(names && timer.frame_ms() > 0.0 && timer.frame_ms() < 1000.0 && sum == timer.frame_ms(),
                   line("timestamps: %zu spans, one per group (%zu), frame %.4f ms", spans.size(), groups.size(),
                        timer.frame_ms()));
        } else {
            log("  skip timestamps: the device's queue writes none");
        }

        const bool off = run(Third::Keep, false, &plain);
        expect(off && graph.stats().merged == 0 && graph.groups().size() == 5 && plain == merged,
               line("merging off: %u merged, %zu groups (5), picture %s", graph.stats().merged, graph.groups().size(),
                    plain == merged ? "identical" : "DIFFERENT"));
        const bool layout = run(Third::DepthRead, true, &depth_read);
        expect(layout && graph.stats().merged == 1 && graph.groups().size() == 4 && depth_read == merged,
               line("a layout change ends the instance: 'test d' (d read-only) not merged, %u merged, groups: %s, "
                    "picture %s",
                    graph.stats().merged, join(graph.groups()).c_str(), depth_read == merged ? "identical" : "DIFFERENT"));
        const bool clear = run(Third::Clear, true, &cleared);
        expect(clear && graph.stats().merged == 1 && graph.groups().size() == 4 && cleared == merged,
               line("a clear ends the instance: 'clear c' not merged, %u merged, groups: %s, picture %s",
                    graph.stats().merged, join(graph.groups()).c_str(), cleared == merged ? "identical" : "DIFFERENT"));
    }

    device.wait_idle();
    timer.destroy();
    pool.destroy();
    vkDestroyCommandPool(device.vk(), commands, nullptr);
    destroy_image(device, out);
    destroy_buffer(device, readback);
    return failures;
}

// The graph in the renderer.
int graph_in_renderer(Renderer& renderer, const std::function<void(const std::string&)>& log) {
    int failures = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        log(std::string(ok ? "  ok   " : "  FAIL ") + what);
        failures += ok ? 0 : 1;
    };
    constexpr std::uint32_t W = 160, H = 120;
    render::Camera camera;
    render::FrameBuilder builder;
    // P1: a red quad. P2: the same, then a snapshot nobody samples and glare sources no glare pass reads.
    const auto packet = [&](std::uint64_t tick, bool dead_passes) {
        builder.begin_frame(tick, render::Float4{0.1f, 0.2f, 0.3f, 1});
        const render::ViewId view = builder.view(camera, {0, 0, 1, 1});
        builder.begin_pass(view, render::PassKind::Scene, render::Clear{});
        canvas_quad(builder, 20, 20, 60, 60, rgba8(255, 0, 0));
        builder.end_pass();
        if (dead_passes) {
            builder.snapshot(view);
            builder.begin_pass(view, render::PassKind::GlareSources, render::Clear{});
            canvas_quad(builder, 80, 20, 120, 60, rgba8(255, 255, 255));
            builder.end_pass();
        }
        return builder.finish();
    };
    std::string error;
    std::vector<std::uint8_t> plain, dead, again;
    const render::FramePacket p1 = packet(1, false);
    const render::FramePacket p2 = packet(2, true);
    if (!renderer.render_to_image(p1, W, H, &plain, &error)) {
        expect(false, "render P1: " + error);
        return failures;
    }
    const Renderer::Stats s1 = renderer.stats();
    if (!renderer.render_to_image(p2, W, H, &dead, &error)) {
        expect(false, "render P2: " + error);
        return failures;
    }
    const Renderer::Stats s2 = renderer.stats();
    expect(s1.graph.culled == 0 && s2.graph.culled == 2 && s2.graph.kept == s1.graph.kept && dead == plain,
           line("culled in the renderer: %u passes (2: the unread snapshot, the glare sources without glare), kept %u "
                "as without them (%u), picture %s",
                s2.graph.culled, s2.graph.kept, s1.graph.kept, dead == plain ? "identical" : "DIFFERENT"));
    if (!renderer.render_to_image(p1, W, H, &again, &error)) {
        expect(false, "render P1 again: " + error);
        return failures;
    }
    const Renderer::Stats s3 = renderer.stats();
    expect(s3.pool_created == s1.pool_created && again == plain,
           line("pooled across frames: %llu targets made for frames seen before (0), picture %s",
                static_cast<unsigned long long>(s3.pool_created - s1.pool_created),
                again == plain ? "identical" : "DIFFERENT"));

    // Split screen: two views of one size; the second view's scene colour and depth take over the first's.
    builder.begin_frame(3, render::Float4{0, 0, 0, 1});
    for (int v = 0; v < 2; ++v) {
        const render::ViewId view = builder.view(camera, {0.5f * static_cast<float>(v), 0, 0.5f, 1});
        builder.begin_pass(view, render::PassKind::Scene, render::Clear{});
        canvas_quad(builder, 10, 10, 30, 30, rgba8(0, 255, 0));
        builder.end_pass();
    }
    const render::FramePacket split = builder.finish();
    std::vector<std::uint8_t> picture;
    if (!renderer.render_to_image(split, W, H, &picture, &error)) {
        expect(false, "render split screen: " + error);
        return failures;
    }
    const Renderer::Stats s4 = renderer.stats();
    const auto at = [&](std::uint32_t px, std::uint32_t py) {
        const std::uint8_t* p = &picture[(std::size_t{py} * W + px) * 4u];
        return rgba8(p[0], p[1], p[2], p[3]);
    };
    expect(s4.graph.aliased == 2 && s4.graph.textures == 6 && s4.graph.images == 4 &&
               at(20, 20) == rgba8(0, 255, 0) && at(100, 20) == rgba8(0, 255, 0) && at(60, 60) == rgba8(0, 0, 0),
           line("split screen: %u textures in %u images (%u aliased: the second view's colour and depth), quads "
                "0x%08x 0x%08x",
                s4.graph.textures, s4.graph.images, s4.graph.aliased, at(20, 20), at(100, 20)));

    // V6 in the renderer: two Scene passes in a row share one rendering instance, the view's depth (attachment-only)
    // is transient, and the GPU times come back one per rendering group.
    const RendererOptions& options = renderer.options();
    if (!options.merge_passes || !options.transient_targets) {
        log("  skip V6 in the renderer: transient targets / pass merging turned off");
        return failures;
    }
    builder.begin_frame(4, render::Float4{0, 0, 0, 1});
    {
        const render::ViewId view = builder.view(camera, {0, 0, 1, 1});
        builder.begin_pass(view, render::PassKind::Scene, render::Clear{});
        canvas_quad(builder, 10, 10, 30, 30, rgba8(255, 0, 0));
        builder.end_pass();
        builder.begin_pass(view, render::PassKind::Scene, render::Clear{});
        canvas_quad(builder, 50, 10, 70, 30, rgba8(0, 0, 255));
        builder.end_pass();
    }
    const render::FramePacket two = builder.finish();
    if (!renderer.render_to_image(two, W, H, &picture, &error)) {
        expect(false, "render two scene passes: " + error);
        return failures;
    }
    const Renderer::Stats s5 = renderer.stats();
    expect(s5.graph.merged == 1 && s5.graph.transient >= 1 && at(20, 20) == rgba8(255, 0, 0) &&
               at(60, 20) == rgba8(0, 0, 255),
           line("two scene passes: %u merged (1), %u transient textures (the depth), quads 0x%08x 0x%08x",
                s5.graph.merged, s5.graph.transient, at(20, 20), at(60, 20)));
    if (renderer.device().caps().timestamps) {
        const std::size_t groups = s5.graph.kept - s5.graph.merged;
        bool named = s5.gpu_passes.size() == groups && !s5.gpu_passes.empty() &&
                     s5.gpu_passes[0].name.find(" + ") != std::string::npos;
        expect(named && s5.gpu_ms > 0.0 && s5.gpu_frames > s4.gpu_frames,
               line("GPU times of this frame: %zu groups (%zu), first '%s', frame %.4f ms", s5.gpu_passes.size(),
                    groups, s5.gpu_passes.empty() ? "-" : s5.gpu_passes[0].name.c_str(), s5.gpu_ms));
    } else {
        log("  skip GPU times: the device's queue writes no timestamps");
    }

    // Keep depth on tile: a scene pass, glare sources, their glare and a canvas pass that does not test depth. With
    // local read the scene and the sources are one rendering instance whose depth is never stored (its last use), and
    // the canvas pass after the glare takes no depth; without it the depth is stored once, for the sources' test.
    if (!options.keep_depth_on_tile) {
        log("  skip keep depth on tile: turned off");
        return failures;
    }
    builder.begin_frame(5, render::Float4{0, 0, 0, 1});
    {
        const render::ViewId view = builder.view(camera, {0, 0, 1, 1});
        builder.begin_pass(view, render::PassKind::Scene, render::Clear{});
        canvas_quad(builder, 10, 10, 30, 30, rgba8(255, 0, 0));
        builder.end_pass();
        builder.begin_pass(view, render::PassKind::GlareSources, render::Clear{});
        canvas_quad(builder, 80, 60, 100, 80, rgba8(255, 255, 255));
        builder.end_pass();
        builder.glare(view, render::GlareParams{});
        builder.begin_pass(view, render::PassKind::Scene, render::Clear{});
        canvas_quad(builder, 50, 10, 70, 30, rgba8(0, 0, 255), false);
        builder.end_pass();
    }
    const render::FramePacket glare = builder.finish();
    if (!renderer.render_to_image(glare, W, H, &picture, &error)) {
        expect(false, "render scene + glare sources: " + error);
        return failures;
    }
    const Renderer::Stats s6 = renderer.stats();
    const bool pairing = renderer.device().caps().local_read;
    bool paired_group = false;
    for (const auto& span : s6.gpu_passes)
        paired_group = paired_group || span.name == "scene + glare sources";
    const bool drawn = at(20, 20) == rgba8(255, 0, 0) && at(60, 20) == rgba8(0, 0, 255) &&
                       at(90, 70) == rgba8(255, 255, 255);
    if (pairing)
        expect(s6.pairs == 1 && s6.graph.depth_stores == 0 && (paired_group || !renderer.device().caps().timestamps) &&
                   drawn,
               line("scene + glare sources in one instance: %llu pair (1), depth stored %u times (0: never leaves the "
                    "tile), %u attachment stores, picture %s",
                    static_cast<unsigned long long>(s6.pairs), s6.graph.depth_stores, s6.graph.stores,
                    drawn ? "as drawn" : "WRONG"));
    else
        expect(s6.pairs == 0 && s6.graph.depth_stores == 1 && drawn,
               line("no local read on this device: %llu pairs (0), depth stored %u time (1: for the sources' test), "
                    "picture %s",
                    static_cast<unsigned long long>(s6.pairs), s6.graph.depth_stores, drawn ? "as drawn" : "WRONG"));
    return failures;
}

}  // namespace

int run_graph_checks(Renderer& renderer, const std::function<void(const std::string&)>& log) {
    log("frame graph checks:");
    return graph_alone(renderer.device(), log) + graph_v6(renderer.device(), log) + graph_in_renderer(renderer, log);
}

}  // namespace shadow::render_vk
