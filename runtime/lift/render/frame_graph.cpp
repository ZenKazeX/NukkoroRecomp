#include "render/frame_graph.h"
#include "render/profile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace shadow::render {

namespace {

std::wstring widen(const char* text) {
    std::wstring out;
    if (text == nullptr)
        return out;
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 1)
        return out;
    out.resize(static_cast<std::size_t>(count - 1));
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), count);
    return out;
}

DXGI_FORMAT depth_resource_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return format;
    }
}

DXGI_FORMAT depth_read_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return format;
    }
}

// Textures this large are placed in shared render-target memory (TexturePool::aliasable); the same threshold above
// which a texture gets its own allocation (resources.cpp kOwnAllocationTexture), so no smaller texture changes place.
constexpr std::uint64_t kPlacedTextureMinimum = 1ull << 20;

bool has_stencil(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

bool is_write(Access access) {
    switch (access) {
    case Access::RenderTarget:
    case Access::DepthWrite:
    case Access::UnorderedAccess:
    case Access::CopyDest:
    case Access::ResolveDest:
        return true;
    default:
        return false;
    }
}

constexpr D3D12_RESOURCE_STATES kReadOnlyStates =
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
    D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
    D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_RESOLVE_SOURCE;

bool read_only(D3D12_RESOURCE_STATES state) {
    return state != D3D12_RESOURCE_STATE_COMMON && (state & ~kReadOnlyStates) == 0;
}

const char* access_name(Access access) {
    switch (access) {
    case Access::RenderTarget: return "render target";
    case Access::DepthWrite: return "depth write";
    case Access::DepthRead: return "depth read";
    case Access::ShaderRead: return "shader read";
    case Access::UnorderedAccess: return "unordered access";
    case Access::CopySource: return "copy source";
    case Access::CopyDest: return "copy destination";
    case Access::ResolveSource: return "resolve source";
    case Access::ResolveDest: return "resolve destination";
    case Access::VertexRead: return "vertex read";
    case Access::IndexRead: return "index read";
    }
    return "?";
}

}  // namespace

bool is_depth_format(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
           format == DXGI_FORMAT_D16_UNORM || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

D3D12_RESOURCE_STATES state_for(Access access) noexcept {
    switch (access) {
    case Access::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case Access::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case Access::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
    case Access::ShaderRead: return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    case Access::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case Access::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case Access::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
    case Access::ResolveSource: return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    case Access::ResolveDest: return D3D12_RESOURCE_STATE_RESOLVE_DEST;
    case Access::VertexRead: return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    case Access::IndexRead: return D3D12_RESOURCE_STATE_INDEX_BUFFER;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

// ---- TexturePool --------------------------------------------------------------------------------------------

TexturePool::~TexturePool() {
    destroy();
}

bool TexturePool::create(Device& device, BindlessHeap& heap, std::uint32_t rtv_capacity, std::uint32_t dsv_capacity,
                         std::string* error) {
    destroy();
    device_ = &device;
    heap_ = &heap;
    return rtvs_.create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtv_capacity, error) &&
           dsvs_.create(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsv_capacity, error);
}

void TexturePool::destroy() {
    clear();
    device_ = nullptr;
    heap_ = nullptr;
}

void TexturePool::release(Entry& entry) {
    rtvs_.free(entry.rtv);
    dsvs_.free(entry.dsv);
    dsvs_.free(entry.dsv_read_only);
    entry.rtv = {};
    entry.dsv = {};
    entry.dsv_read_only = {};
    if (entry.uav != kNoDescriptor)
        heap_->free(entry.uav, device_->mark());
    entry.uav = kNoDescriptor;
    retire(*device_, entry.texture, heap_);
}

bool TexturePool::Memory::conflicts(std::uint64_t at_frame, std::uint32_t first, std::uint32_t last,
                                    const void* holder) const {
    if (frame != at_frame)
        return false;
    for (const Busy& b : busy) {
        if (b.holder != holder && b.first <= last && first <= b.last)
            return true;
    }
    return false;
}

void TexturePool::Memory::hold(std::uint64_t at_frame, std::uint32_t first, std::uint32_t last, const void* holder) {
    if (frame != at_frame) {
        busy.clear();
        frame = at_frame;
    }
    busy.push_back(Busy{first, last, holder});
}

std::uint64_t TexturePool::texture_bytes(const TextureDesc& desc) const {
    if (device_ == nullptr || desc.width == 0 || desc.height == 0 || desc.format == DXGI_FORMAT_UNKNOWN)
        return 0;
    TextureInfo info;
    info.width = desc.width;
    info.height = desc.height;
    info.samples = desc.samples;
    const bool depth = is_depth_format(desc.format);
    info.format = depth ? depth_resource_format(desc.format) : desc.format;
    info.flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (desc.unordered_access)
        info.flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_RESOURCE_DESC d = texture_resource_desc(info);
    return device_->d3d()->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
}

namespace {
// SHADOW_R2_TARGET_ALIASING (same-binary A/B only): 0 every texture committed in its own memory, as before aliasing;
// 2 placed textures, each in memory of its own (placement without sharing); 3 aliasing of every class, including the
// ones that stay committed by default (below); unset or 1 aliasing.
int target_aliasing_mode() {
    static const int mode = [] {
        const char* v = std::getenv("SHADOW_R2_TARGET_ALIASING");
        return v == nullptr || v[0] == '\0' ? 1 : v[0] - '0';
    }();
    return mode;
}
}  // namespace

bool TexturePool::aliasable(const TextureDesc& desc) const {
    const int mode = target_aliasing_mode();
    if (mode == 0 || desc.unordered_access || texture_bytes(desc) < kPlacedTextureMinimum)
        return false;
    if (mode == 3)
        return true;
    // Only the classes proven to render bit for bit as committed textures are placed (docs/RENDER_TARGET_ALIASING_5K.md,
    // 2026-10-30): multisampled colour and single-sampled R16G16B16A16_UNORM (the view's resolve and snapshots) - 4x,
    // 5120x2880, 3 + 3 runs, no pixel consistent with the arm. Placing a 4x depth or an 8-bit single-sampled target
    // instead changed 4x output reproducibly (one step on ~1.6 pixels a frame; at 1x nothing changed), so they and every
    // class not tested keep committed memory of their own. Neither of them can share memory in the game's frames: the
    // depth lives through the scene, the output from the glare resolve to the end of the frame.
    return !is_depth_format(desc.format) && (desc.samples > 1 || desc.format == DXGI_FORMAT_R16G16B16A16_UNORM);
}

std::uint64_t TexturePool::memory_bytes() const noexcept {
    std::uint64_t total = 0;
    for (const std::shared_ptr<Memory>& m : memories_)
        total += m->size;
    return total;
}

std::uint64_t TexturePool::placed_bytes() const noexcept {
    std::uint64_t total = 0;
    for (const std::unique_ptr<Entry>& entry : entries_) {
        // Textures left unused (waiting out kKeepFrames in memory another texture now uses) take no memory of their own.
        if (entry->memory && entry->last_frame >= last_frame_)
            total += entry->texture.resource ? device_->d3d()->GetResourceAllocationInfo(0, 1, &entry->texture.desc).SizeInBytes
                                             : 0;
    }
    return total;
}

std::string TexturePool::memory_map() const {
    std::string out;
    char line[160];
    for (const std::shared_ptr<Memory>& m : memories_) {
        std::snprintf(line, sizeof(line), "%.1f MiB:", static_cast<double>(m->size) / 1048576.0);
        out += line;
        const char* separator = " ";
        for (const std::unique_ptr<Entry>& entry : entries_) {
            if (entry->memory != m)
                continue;
            const std::uint64_t bytes = device_->d3d()->GetResourceAllocationInfo(0, 1, &entry->texture.desc).SizeInBytes;
            const bool used = entry->last_frame >= last_frame_;
            const int n = std::snprintf(line, sizeof(line), "%s%s %ux%u s%u %.1f MiB", separator,
                                        entry->name.c_str(), entry->desc.width, entry->desc.height,
                                        entry->desc.samples, static_cast<double>(bytes) / 1048576.0);
            if (!used && n > 0 && static_cast<std::size_t>(n) < sizeof(line))
                std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n), " (unused since frame %llu)",
                              static_cast<unsigned long long>(entry->last_frame));
            out += line;
            separator = ", ";
        }
        out += "\n";
    }
    return out;
}

TexturePool::Entry* TexturePool::acquire(const TextureDesc& desc, std::uint64_t frame, const char* name,
                                         std::string* error) {
    return acquire(desc, frame, name, 0u, ~0u, error);
}

TexturePool::Entry* TexturePool::acquire(const TextureDesc& desc, std::uint64_t frame, const char* name,
                                         std::uint32_t first, std::uint32_t last, std::string* error) {
    // DIAGNOSTIC (attribution of placed-texture output differences): SHADOW_R2_PLACED_FILTER places only the
    // multisampled ("ms"), single-sampled ("ss"), multisampled depth ("msdepth"), multisampled colour ("mscolour"),
    // single-sampled RGBA16 - the resolved view and snapshots - ("view"), or both of the last two ("viewglare")
    // textures; the others keep committed memory of their own.
    static const std::string filter = [] {
        const char* v = std::getenv("SHADOW_R2_PLACED_FILTER");
        return std::string(v != nullptr ? v : "");
    }();
    const bool depth = is_depth_format(desc.format);
    const bool filtered = !filter.empty() &&
                          !((filter == "ms" && desc.samples > 1) || (filter == "ss" && desc.samples == 1) ||
                            (filter == "msdepth" && desc.samples > 1 && depth) ||
                            (filter == "mscolour" && desc.samples > 1 && !depth) ||
                            (filter == "viewglare" && !depth &&
                             (desc.samples > 1 || desc.format == DXGI_FORMAT_R16G16B16A16_UNORM)) ||
                            (filter == "glareview" && !depth &&
                             ((desc.samples > 1 && desc.format == DXGI_FORMAT_R8G8B8A8_UNORM) ||
                              (desc.samples == 1 && desc.format == DXGI_FORMAT_R16G16B16A16_UNORM))) ||
                            (filter == "view" && !depth && desc.samples == 1 &&
                             desc.format == DXGI_FORMAT_R16G16B16A16_UNORM));
    if (device_ == nullptr || filtered || !aliasable(desc)) {
        for (const std::unique_ptr<Entry>& entry : entries_) {
            if (!entry->in_use && !entry->memory && pool_compatible(entry->desc, desc)) {
                entry->in_use = true;
                entry->last_frame = frame;
                return entry.get();
            }
        }
        return create_entry(desc, frame, name, nullptr, error);
    }
    // The largest memory free during [first, last]: a pooled texture of this description already placed there, or
    // memory to place a new one in (a tie keeps the texture that exists).
    const std::uint64_t need = texture_bytes(desc);
    Entry* best_entry = nullptr;
    std::shared_ptr<Memory> best_memory;
    std::uint64_t best_size = 0;
    for (const std::unique_ptr<Entry>& entry : entries_) {
        if (entry->in_use || !entry->memory || !pool_compatible(entry->desc, desc) ||
            entry->memory->conflicts(frame, first, last, entry.get()))
            continue;
        if (entry->memory->size > best_size) {
            best_entry = entry.get();
            best_size = entry->memory->size;
        }
    }
    for (const std::shared_ptr<Memory>& m : memories_) {
        if (target_aliasing_mode() == 2)
            break;
        if (m->size >= need && m->size > best_size && !m->conflicts(frame, first, last, nullptr)) {
            best_entry = nullptr;
            best_memory = m;
            best_size = m->size;
        }
    }
    if (best_entry != nullptr) {
        best_entry->in_use = true;
        best_entry->last_frame = frame;
        best_entry->memory->hold(frame, first, last, best_entry);
        return best_entry;
    }
    if (!best_memory) {
        auto memory = std::make_shared<Memory>();
        if (!allocate_target_memory(*device_, need, L"shadow.render_target_memory", &memory->allocation, error))
            return nullptr;
        memory->size = memory->allocation->GetSize();
        memories_.push_back(memory);
        best_memory = std::move(memory);
    }
    Entry* entry = create_entry(desc, frame, name, best_memory, error);
    if (entry != nullptr)
        best_memory->hold(frame, first, last, entry);
    return entry;
}

TexturePool::Entry* TexturePool::create_entry(const TextureDesc& desc, std::uint64_t frame, const char* name,
                                              std::shared_ptr<Memory> memory, std::string* error) {
    if (device_ == nullptr) {
        if (error != nullptr)
            *error = "texture pool not created";
        return nullptr;
    }
    if (desc.width == 0 || desc.height == 0 || desc.format == DXGI_FORMAT_UNKNOWN) {
        if (error != nullptr)
            *error = std::string("texture '") + name + "' has no size or format";
        return nullptr;
    }
    const bool depth = is_depth_format(desc.format);
    if (desc.samples > 1 && desc.unordered_access) {
        if (error != nullptr)
            *error = std::string("texture '") + name + "': multisampled textures cannot be unordered-access";
        return nullptr;
    }
    auto entry = std::make_unique<Entry>();
    entry->desc = desc;
    entry->name = name != nullptr ? name : "";
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.format;
    TextureInfo info;
    info.width = desc.width;
    info.height = desc.height;
    info.samples = desc.samples;
    const std::wstring wide_name = widen(name);
    info.name = wide_name.c_str();
    // One optimized clear value per kind, whatever this first requester clears to (a clear to another value is the
    // same clear, without the fast-clear optimisation).
    constexpr float kPoolClearColour[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    constexpr float kPoolClearDepth = 0.0f;  // reversed depth: the far end (frame_math.h kFarDepth)
    constexpr std::uint8_t kPoolClearStencil = 0;
    if (depth) {
        clear.DepthStencil.Depth = kPoolClearDepth;
        clear.DepthStencil.Stencil = kPoolClearStencil;
        info.format = depth_resource_format(desc.format);
        info.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        info.initial_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        std::copy(std::begin(kPoolClearColour), std::end(kPoolClearColour), clear.Color);
        info.format = desc.format;
        info.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (desc.unordered_access)
            info.flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        info.initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    info.clear = &clear;
    if (memory) {
        if (!create_placed_texture_2d(*device_, info, memory->allocation.Get(), &entry->texture, error))
            return nullptr;
        // Initialised by its activation at the first pass of each resource it backs (FrameGraph::Activation).
        entry->initialized = true;
        entry->memory = std::move(memory);
    } else if (!create_texture_2d(*device_, info, &entry->texture, error)) {
        return nullptr;
    }
    if (!create_texture_srv(*device_, *heap_, entry->texture, depth ? depth_read_format(desc.format) : desc.format,
                            error)) {
        retire(*device_, entry->texture, heap_);
        return nullptr;
    }
    const bool multisampled = desc.samples > 1;
    if (depth) {
        entry->dsv = dsvs_.allocate();
        entry->dsv_read_only = dsvs_.allocate();
        if (entry->dsv.ptr == 0 || entry->dsv_read_only.ptr == 0) {
            if (error != nullptr)
                *error = "depth-stencil view pool full";
            dsvs_.free(entry->dsv);
            dsvs_.free(entry->dsv_read_only);
            entry->dsv = {};
            entry->dsv_read_only = {};
            retire(*device_, entry->texture, heap_);
            return nullptr;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = desc.format;
        view.ViewDimension = multisampled ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->d3d()->CreateDepthStencilView(entry->texture.resource.Get(), &view, entry->dsv);
        view.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        if (has_stencil(desc.format))
            view.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        device_->d3d()->CreateDepthStencilView(entry->texture.resource.Get(), &view, entry->dsv_read_only);
    } else {
        entry->rtv = rtvs_.allocate();
        if (entry->rtv.ptr == 0) {
            if (error != nullptr)
                *error = "render-target view pool full";
            retire(*device_, entry->texture, heap_);
            return nullptr;
        }
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = desc.format;
        view.ViewDimension = multisampled ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->d3d()->CreateRenderTargetView(entry->texture.resource.Get(), &view, entry->rtv);
        if (desc.unordered_access) {
            entry->uav = heap_->allocate();
            if (entry->uav == kNoDescriptor) {
                if (error != nullptr)
                    *error = "bindless heap full";
                release(*entry);
                return nullptr;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = desc.format;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device_->d3d()->CreateUnorderedAccessView(entry->texture.resource.Get(), nullptr, &uav,
                                                      heap_->cpu(entry->uav));
        }
    }
    entry->state = info.initial_state;
    entry->in_use = true;
    entry->last_frame = frame;
    entries_.push_back(std::move(entry));
    ++created_;
    return entries_.back().get();
}

void TexturePool::end_frame(std::uint64_t frame, std::uint64_t keep_frames) {
    last_frame_ = frame;
    // A placed texture alone in its memory, unused this frame while a texture of its description served from LARGER
    // memory, is superseded: acquire always takes the largest free memory, so it would be chosen again only in a frame
    // where that memory is busy - and then a new one is made. Keeping it for kKeepFrames held a second copy: at
    // 5120x2880 the view's first texture (made before the glare existed) stayed 60 s beside the one in the glare
    // sources' memory, 115 MiB of VRAM and private commit.
    auto superseded = [&](const Entry& e) {
        if (!e.memory || e.last_frame >= frame || e.memory.use_count() != 2)  // 2: the entry and memories_
            return false;
        for (const std::unique_ptr<Entry>& other : entries_) {
            if (other.get() != &e && other->memory && other->last_frame == frame &&
                pool_compatible(other->desc, e.desc) && other->memory->size > e.memory->size)
                return true;
        }
        return false;
    };
    for (auto it = entries_.begin(); it != entries_.end();) {
        Entry& entry = **it;
        entry.in_use = false;
        if (frame > entry.last_frame + keep_frames || superseded(entry)) {
            release(entry);
            it = entries_.erase(it);
            ++released_;
        } else {
            ++it;
        }
    }
    // Memory no texture is placed in any more goes with the last of them (after it, in the GPU's order).
    for (auto it = memories_.begin(); it != memories_.end();) {
        if (it->use_count() == 1) {
            device_->retire(Com<IUnknown>((*it)->allocation.Get()));
            it = memories_.erase(it);
        } else {
            ++it;
        }
    }
}

void TexturePool::clear() {
    for (const std::unique_ptr<Entry>& entry : entries_)
        release(*entry);
    entries_.clear();
    for (const std::shared_ptr<Memory>& m : memories_) {
        if (device_ != nullptr)
            device_->retire(Com<IUnknown>(m->allocation.Get()));
    }
    memories_.clear();
}

// ---- PassBuilder / PassContext ------------------------------------------------------------------------------

void PassBuilder::read(ResourceId resource, Access access) {
    graph_.add_use(pass_, resource, access, Load::Keep, false);
}

void PassBuilder::write(ResourceId resource, Access access, Load load) {
    graph_.add_use(pass_, resource, access, load, true);
}

void PassBuilder::side_effect() {
    graph_.passes_[pass_].side_effect = true;
}

ID3D12Resource* PassContext::resource(ResourceId id) const {
    return graph_.resources_[id].d3d;
}

D3D12_CPU_DESCRIPTOR_HANDLE PassContext::rtv(ResourceId id) const {
    return graph_.resources_[id].rtv;
}

D3D12_CPU_DESCRIPTOR_HANDLE PassContext::dsv(ResourceId id) const {
    return graph_.resources_[id].dsv;
}

D3D12_CPU_DESCRIPTOR_HANDLE PassContext::dsv_read_only(ResourceId id) const {
    return graph_.resources_[id].dsv_read_only;
}

std::uint32_t PassContext::srv(ResourceId id) const {
    return graph_.resources_[id].srv;
}

std::uint32_t PassContext::uav(ResourceId id) const {
    return graph_.resources_[id].uav;
}

const TextureDesc& PassContext::desc(ResourceId id) const {
    return graph_.resources_[id].desc;
}

void PassContext::full_viewport(ResourceId id) const {
    const TextureDesc& d = desc(id);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
    set_viewport(viewport, scissor);
}

void PassContext::set_viewport(const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor) const {
    list_->RSSetViewports(1, &viewport);
    list_->RSSetScissorRects(1, &scissor);
    if (graph_.trace_ != nullptr) {
        char line[192];
        std::snprintf(line, sizeof(line), "  viewport %.1f,%.1f %.1fx%.1f scissor %ld,%ld-%ld,%ld", viewport.TopLeftX,
                      viewport.TopLeftY, viewport.Width, viewport.Height, static_cast<long>(scissor.left),
                      static_cast<long>(scissor.top), static_cast<long>(scissor.right),
                      static_cast<long>(scissor.bottom));
        graph_.trace_->push_back(line);
    }
}

// ---- FrameGraph ---------------------------------------------------------------------------------------------

void FrameGraph::reset(std::uint64_t frame) {
    frame_ = frame;
    resources_.clear();
    passes_.clear();
    transitions_.clear();
    compiled_ = false;
    build_error_.clear();
}

ResourceId FrameGraph::create_texture(const char* name, const TextureDesc& desc) {
    Resource resource;
    resource.name = name;
    resource.kind = Kind::Created;
    resource.desc = desc;
    resources_.push_back(std::move(resource));
    return static_cast<ResourceId>(resources_.size() - 1);
}

ResourceId FrameGraph::import_texture(const char* name, const ImportedTexture& texture) {
    Resource resource;
    resource.name = name;
    resource.kind = Kind::ImportedTexture;
    resource.desc = texture.desc;
    resource.d3d = texture.resource;
    resource.state = texture.state;
    resource.final_state = texture.final_state;
    resource.rtv = texture.rtv;
    resource.dsv = texture.dsv;
    resource.srv = texture.srv;
    resource.discard_first = texture.discard_first;
    resources_.push_back(std::move(resource));
    return static_cast<ResourceId>(resources_.size() - 1);
}

ResourceId FrameGraph::import_buffer(const char* name, const ImportedBuffer& buffer) {
    Resource resource;
    resource.name = name;
    resource.kind = Kind::ImportedBuffer;
    resource.d3d = buffer.resource;
    resource.state = buffer.state;
    resource.final_state = buffer.final_state;
    resources_.push_back(std::move(resource));
    return static_cast<ResourceId>(resources_.size() - 1);
}

void FrameGraph::add_use(std::uint32_t pass, ResourceId resource, Access access, Load load, bool write) {
    Pass& p = passes_[pass];
    if (resource >= resources_.size()) {
        if (build_error_.empty())
            build_error_ = "pass '" + p.name + "' uses an unknown resource";
        return;
    }
    if (is_write(access) != write) {
        if (build_error_.empty())
            build_error_ = "pass '" + p.name + "' declares " + access_name(access) + " of '" +
                           resources_[resource].name + "' as a " + (write ? "write" : "read");
        return;
    }
    if (load == Load::Clear && access != Access::RenderTarget && access != Access::DepthWrite) {
        if (build_error_.empty())
            build_error_ = "pass '" + p.name + "': only render-target and depth writes can clear";
        return;
    }
    p.uses.push_back(Use{resource, access, load, write});
}

bool FrameGraph::fail(std::string* error, const std::string& message) {
    if (error != nullptr)
        *error = message;
    compiled_ = false;
    return false;
}

bool FrameGraph::compile(TexturePool& pool, std::string* error) {
    transitions_.clear();
    compiled_ = false;
    if (!build_error_.empty())
        return fail(error, build_error_);

    // A pass may write a resource or read it, not both (except the read implied by Load::Keep).
    for (const Pass& pass : passes_) {
        for (std::size_t i = 0; i < pass.uses.size(); ++i) {
            for (std::size_t j = i + 1; j < pass.uses.size(); ++j) {
                const Use& a = pass.uses[i];
                const Use& b = pass.uses[j];
                if (a.resource == b.resource && (a.write || b.write))
                    return fail(error, "pass '" + pass.name + "' uses '" + resources_[a.resource].name +
                                           "' both as " + access_name(a.access) + " and as " + access_name(b.access));
            }
        }
    }

    // Cull, walking backwards: a pass is kept when it has an outside effect, writes an imported resource, or
    // writes something a kept later pass reads.
    std::vector<bool> needed(resources_.size(), false);
    for (std::size_t n = passes_.size(); n-- > 0;) {
        Pass& pass = passes_[n];
        bool keep = pass.side_effect;
        for (const Use& use : pass.uses) {
            if (!use.write)
                continue;
            if (resources_[use.resource].kind != Kind::Created || needed[use.resource])
                keep = true;
        }
        pass.kept = keep;
        if (!keep)
            continue;
        for (const Use& use : pass.uses) {
            if (use.write && use.load != Load::Keep)
                needed[use.resource] = false;  // overwritten: earlier contents are not observed
        }
        for (const Use& use : pass.uses) {
            if (!use.write || use.load == Load::Keep)
                needed[use.resource] = true;
        }
    }

    // A graph-owned texture must be written (cleared or fully overwritten) before anything reads or adds to it.
    std::vector<bool> written(resources_.size(), false);
    for (Pass& pass : passes_) {
        if (!pass.kept)
            continue;
        for (const Use& use : pass.uses) {
            Resource& resource = resources_[use.resource];
            resource.used = true;
            if (resource.kind != Kind::Created || written[use.resource])
                continue;
            if (!use.write)
                return fail(error, "pass '" + pass.name + "' reads '" + resource.name + "' before any pass wrote it");
            if (use.load == Load::Keep)
                return fail(error, "pass '" + pass.name + "' adds to '" + resource.name +
                                       "' before any pass wrote it (clear or overwrite it first)");
        }
        for (const Use& use : pass.uses) {
            if (use.write)
                written[use.resource] = true;
        }
    }

    // Pooled textures for the graph-owned resources the kept passes use. A resource whose last kept pass comes
    // before another's first takes over its pooled texture: the next one's first use writes it whole (checked
    // above), so nothing it reads was left by the one before. A mid-frame snapshot then shares the view's resolve
    // target, and the glare sources' resolve the output, instead of each holding its own output-sized texture.
    std::vector<std::uint32_t> first_use(resources_.size(), ~0u), last_use(resources_.size(), 0);
    for (std::size_t n = 0; n < passes_.size(); ++n) {
        if (!passes_[n].kept)
            continue;
        for (const Use& use : passes_[n].uses) {
            first_use[use.resource] = std::min(first_use[use.resource], static_cast<std::uint32_t>(n));
            last_use[use.resource] = std::max(last_use[use.resource], static_cast<std::uint32_t>(n));
        }
    }
    std::vector<std::size_t> order;
    for (std::size_t r = 0; r < resources_.size(); ++r) {
        if (resources_[r].kind == Kind::Created && resources_[r].used)
            order.push_back(r);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return first_use[a] < first_use[b]; });
    // The resource whose state a resource starts from: itself, or the one that held its texture before it.
    std::vector<std::size_t> slot(resources_.size());
    for (std::size_t r = 0; r < resources_.size(); ++r)
        slot[r] = r;
    struct Held {
        TexturePool::Entry* entry;
        std::uint32_t until;  // the holder's last kept pass
        std::size_t holder;
    };
    std::vector<Held> held;
    for (const std::size_t r : order) {
        Resource& resource = resources_[r];
        TexturePool::Entry* entry = nullptr;
        for (Held& h : held) {
            // A texture in shared memory hands on only when nothing else holds that memory in the new lifetime.
            if (h.until < first_use[r] && TexturePool::pool_compatible(h.entry->desc, resource.desc) &&
                !(h.entry->memory && h.entry->memory->conflicts(frame_, first_use[r], last_use[r], h.entry))) {
                entry = h.entry;
                slot[r] = slot[h.holder];
                h.until = last_use[r];
                h.holder = r;
                if (entry->memory)
                    entry->memory->hold(frame_, first_use[r], last_use[r], entry);
                break;
            }
        }
        if (entry == nullptr) {
            std::string reason;
            entry = pool.acquire(resource.desc, frame_, resource.name.c_str(), first_use[r], last_use[r], &reason);
            if (entry == nullptr)
                return fail(error, "texture '" + resource.name + "': " + reason);
            held.push_back(Held{entry, last_use[r], r});
        }
        resource.pooled = entry;
        resource.d3d = entry->texture.resource.Get();
        resource.state = entry->state;
        resource.rtv = entry->rtv;
        resource.dsv = entry->dsv;
        resource.dsv_read_only = entry->dsv_read_only;
        resource.srv = entry->texture.srv;
        resource.uav = entry->uav;
    }

    // Transitions, pass by pass, in the authored order. States are tracked per texture (the slot): a resource that
    // takes over a texture starts in the state the one before left it in. A resource in shared memory is activated
    // at its first pass, in the state DiscardResource needs, before that pass's own transitions start from there.
    activations_.clear();
    // DIAGNOSTIC SHADOW_R2_ACTIVATE=changed: activate only when another texture used the memory since this one.
    static const bool activate_changed_only = [] {
        const char* v = std::getenv("SHADOW_R2_ACTIVATE");
        return v != nullptr && std::string(v) == "changed";
    }();
    std::vector<std::pair<TexturePool::Memory*, const void*>>& last_user = memory_users_;  // this frame, pass order
    last_user.clear();
    auto memory_last_user = [&](const TexturePool::Memory* m) {
        for (auto it = last_user.rbegin(); it != last_user.rend(); ++it)
            if (it->first == m)
                return it->second;
        return m->gpu_last_user;
    };
    std::vector<D3D12_RESOURCE_STATES> state(resources_.size());
    std::vector<bool> last_uav(resources_.size(), false);
    for (std::size_t r = 0; r < resources_.size(); ++r)
        state[r] = resources_[r].state;
    for (std::size_t n = 0; n < passes_.size(); ++n) {
        const Pass& pass = passes_[n];
        if (!pass.kept)
            continue;
        for (const Use& use : pass.uses) {
            const ResourceId r = use.resource;
            const Resource& resource = resources_[r];
            if (resource.pooled == nullptr || !resource.pooled->memory || first_use[r] != n)
                continue;
            if (std::any_of(activations_.begin(), activations_.end(),
                            [&](const Activation& a) { return a.pass == n && a.resource == r; }))
                continue;
            TexturePool::Memory* memory = resource.pooled->memory.get();
            const bool same_user = memory_last_user(memory) == resource.pooled;
            last_user.emplace_back(memory, resource.pooled);
            if (activate_changed_only && same_user)
                continue;
            const D3D12_RESOURCE_STATES discard_state = is_depth_format(resource.desc.format)
                                                            ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                            : D3D12_RESOURCE_STATE_RENDER_TARGET;
            activations_.push_back(Activation{static_cast<std::uint32_t>(n), r, state[slot[r]], discard_state});
            state[slot[r]] = discard_state;
            last_uav[slot[r]] = false;
        }
        // Merge the pass's uses per resource, first use first.
        std::vector<std::pair<ResourceId, D3D12_RESOURCE_STATES>> required;
        std::vector<bool> required_write;
        for (const Use& use : pass.uses) {
            auto it = std::find_if(required.begin(), required.end(),
                                   [&](const auto& entry) { return entry.first == use.resource; });
            if (it == required.end()) {
                required.emplace_back(use.resource, state_for(use.access));
                required_write.push_back(use.write);
            } else {
                it->second |= state_for(use.access);
            }
        }
        for (std::size_t i = 0; i < required.size(); ++i) {
            const ResourceId r = required[i].first;
            const std::size_t s = slot[r];
            const D3D12_RESOURCE_STATES want = required[i].second;
            const D3D12_RESOURCE_STATES have = state[s];
            const auto pass_index = static_cast<std::uint32_t>(n);
            if (have == want) {
                if (want == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && last_uav[s])
                    transitions_.push_back(Transition{pass_index, r, have, want, true});
            } else if (!required_write[i] && read_only(have) && (have & want) == want) {
                // Already in a read state that includes what this pass reads.
            } else {
                transitions_.push_back(Transition{pass_index, r, have, want, false});
                state[s] = want;
            }
            last_uav[s] = want == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
    }
    const auto closing = static_cast<std::uint32_t>(passes_.size());
    for (std::size_t r = 0; r < resources_.size(); ++r) {
        Resource& resource = resources_[r];
        if (resource.kind != Kind::Created && state[r] != resource.final_state) {
            transitions_.push_back(Transition{closing, static_cast<ResourceId>(r), state[r], resource.final_state, false});
            state[r] = resource.final_state;
        }
    }
    for (std::size_t r = 0; r < resources_.size(); ++r)
        resources_[r].end_state = state[slot[r]];  // every holder of a texture leaves it in its last state
    compiled_ = true;
    return true;
}

void FrameGraph::execute(ID3D12GraphicsCommandList* list, const BindlessHeap& heap, const SamplerHeap& samplers,
                         ID3D12RootSignature* root_signature, PassMarks* marks) {
    if (!compiled_)
        return;
    GEKKO_PROFILE_ZONE("gpu/execute");
    ID3D12DescriptorHeap* heaps[] = {heap.heap(), samplers.heap()};
    list->SetDescriptorHeaps(2, heaps);
    if (root_signature != nullptr) {
        list->SetGraphicsRootSignature(root_signature);
        list->SetComputeRootSignature(root_signature);
    }
    // A texture the pool has just created is still in its creation state (render target or depth write), the
    // state DiscardResource needs; discarding is what initialises its memory.
    for (Resource& resource : resources_) {
        if (resource.pooled != nullptr && !resource.pooled->initialized) {
            list->DiscardResource(resource.d3d, nullptr);
            resource.pooled->initialized = true;
        }
    }
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    std::size_t next = 0, next_activation = 0;
    auto flush = [&](std::uint32_t pass) {
        // Activations first: the aliasing barriers (the memory's earlier resources are done), each texture into
        // the state its discard needs, then the discards that initialise them.
        barriers.clear();
        const std::size_t activation_begin = next_activation;
        while (next_activation < activations_.size() && activations_[next_activation].pass == pass) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            barrier.Aliasing.pResourceBefore = nullptr;
            barrier.Aliasing.pResourceAfter = resources_[activations_[next_activation].resource].d3d;
            barriers.push_back(barrier);
            ++next_activation;
        }
        for (std::size_t a = activation_begin; a < next_activation; ++a) {
            const Activation& act = activations_[a];
            if (act.before == act.discard_state)
                continue;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resources_[act.resource].d3d;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = act.before;
            barrier.Transition.StateAfter = act.discard_state;
            barriers.push_back(barrier);
        }
        if (!barriers.empty())
            list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        for (std::size_t a = activation_begin; a < next_activation; ++a)
            list->DiscardResource(resources_[activations_[a].resource].d3d, nullptr);
        barriers.clear();
        while (next < transitions_.size() && transitions_[next].pass == pass) {
            const Transition& t = transitions_[next++];
            D3D12_RESOURCE_BARRIER barrier{};
            if (t.uav_barrier) {
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.UAV.pResource = resources_[t.resource].d3d;
            } else {
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = resources_[t.resource].d3d;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = t.before;
                barrier.Transition.StateAfter = t.after;
            }
            barriers.push_back(barrier);
        }
        if (!barriers.empty())
            list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    };
    for (std::size_t n = 0; n < passes_.size(); ++n) {
        Pass& pass = passes_[n];
        if (!pass.kept)
            continue;
        list->BeginEvent(1, pass.name.c_str(), static_cast<UINT>(pass.name.size() + 1));
        if (marks != nullptr)
            marks->pass_begin(list, pass.name);
        flush(static_cast<std::uint32_t>(n));
        if (trace_ != nullptr) {
            trace_->push_back("pass " + pass.name);
            for (const Use& use : pass.uses) {
                const bool target = use.access == Access::RenderTarget || use.access == Access::DepthWrite ||
                                    use.access == Access::DepthRead || use.access == Access::ResolveDest ||
                                    use.access == Access::CopyDest;
                const Resource& resource = resources_[use.resource];
                if (!target || resource.d3d == nullptr)
                    continue;
                const D3D12_RESOURCE_DESC d = resource.d3d->GetDesc();  // what the GPU resource really is
                char line[256];
                std::snprintf(line, sizeof(line), "  %s '%s' %llux%u samples %u format %d",
                              use.access == Access::RenderTarget  ? "render-target"
                              : use.access == Access::DepthWrite  ? "depth-write"
                              : use.access == Access::DepthRead   ? "depth-read"
                              : use.access == Access::ResolveDest ? "resolve-dest"
                                                                  : "copy-dest",
                              resource.name.c_str(), static_cast<unsigned long long>(d.Width), d.Height,
                              d.SampleDesc.Count, static_cast<int>(d.Format));
                trace_->push_back(line);
            }
        }
        for (const Use& use : pass.uses) {
            // An imported texture's first write ever (ImportedTexture::discard_first), now in its render-target state:
            // DiscardResource initialises it, as D3D12 requires before any other use.
            if (use.write && use.load == Load::Discard && use.access == Access::RenderTarget &&
                resources_[use.resource].discard_first) {
                list->DiscardResource(resources_[use.resource].d3d, nullptr);
                resources_[use.resource].discard_first = false;
                continue;
            }
            if (!use.write || use.load != Load::Clear)
                continue;
            const Resource& resource = resources_[use.resource];
            if (use.access == Access::RenderTarget) {
                list->ClearRenderTargetView(resource.rtv, resource.desc.clear_color.data(), 0, nullptr);
            } else if (use.access == Access::DepthWrite) {
                D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
                if (has_stencil(resource.desc.format))
                    flags |= D3D12_CLEAR_FLAG_STENCIL;
                list->ClearDepthStencilView(resource.dsv, flags, resource.desc.clear_depth,
                                            resource.desc.clear_stencil, 0, nullptr);
            }
        }
        PassContext context(*this, list);
        if (pass.execute)
            pass.execute(context);
        if (marks != nullptr)
            marks->pass_end(list);
        list->EndEvent();
    }
    flush(static_cast<std::uint32_t>(passes_.size()));
    // The memories' last users, now that this frame's uses are recorded in the GPU's order.
    for (const auto& use : memory_users_)
        use.first->gpu_last_user = use.second;
    for (Resource& resource : resources_) {
        if (resource.pooled != nullptr)
            resource.pooled->state = resource.end_state;
    }
}

D3D12_RESOURCE_STATES FrameGraph::state_after(ResourceId id) const {
    return id < resources_.size() ? resources_[id].end_state : D3D12_RESOURCE_STATE_COMMON;
}

std::vector<std::string> FrameGraph::kept_passes() const {
    std::vector<std::string> out;
    for (const Pass& pass : passes_) {
        if (pass.kept)
            out.push_back(pass.name);
    }
    return out;
}

}  // namespace shadow::render
