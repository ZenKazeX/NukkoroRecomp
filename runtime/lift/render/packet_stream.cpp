#include "render/packet_stream.h"

#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace shadow::render {

namespace {

constexpr char kMagic[8] = {'S', 'H', 'P', 'K', 'T', 'S', '0', '1'};

// The layouts the stream's raw records depend on, in a fixed order: the header carries them, the reader compares.
std::vector<std::uint32_t> layout() {
    return {sizeof(ViewRecord),    sizeof(LightSet),       sizeof(PassRecord),         sizeof(DrawRecord),
            sizeof(Float3x4),      sizeof(Particle),       sizeof(ParticleBatch),      sizeof(ImmediateVertex),
            sizeof(ImmediateDraw), sizeof(CanvasRegion),   sizeof(Float4),             sizeof(assets::Sphere),
            sizeof(assets::MeshData::Submesh), sizeof(float)};
}

static_assert(std::is_trivially_copyable_v<ViewRecord> && std::is_trivially_copyable_v<LightSet> &&
                  std::is_trivially_copyable_v<PassRecord> && std::is_trivially_copyable_v<DrawRecord> &&
                  std::is_trivially_copyable_v<Particle> && std::is_trivially_copyable_v<ParticleBatch> &&
                  std::is_trivially_copyable_v<ImmediateVertex> && std::is_trivially_copyable_v<ImmediateDraw> &&
                  std::is_trivially_copyable_v<CanvasRegion> &&
                  std::is_trivially_copyable_v<assets::MeshData::Submesh>,
              "the stream writes these records as their bytes");

class Out {
public:
    template <class T>
    void value(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    template <class T>
    void vector(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        value(static_cast<std::uint64_t>(v.size()));
        const auto* p = reinterpret_cast<const std::uint8_t*>(v.data());
        bytes.insert(bytes.end(), p, p + v.size() * sizeof(T));
    }
    void string(const std::string& s) {
        value(static_cast<std::uint64_t>(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
    std::vector<std::uint8_t> bytes;
};

class In {
public:
    In(const std::uint8_t* data, std::size_t size) : p_(data), end_(data + size) {}
    template <class T>
    bool value(T* v) {
        if (static_cast<std::size_t>(end_ - p_) < sizeof(T))
            return false;
        std::memcpy(v, p_, sizeof(T));
        p_ += sizeof(T);
        return true;
    }
    template <class T>
    bool vector(std::vector<T>* v) {
        std::uint64_t n = 0;
        if (!value(&n) || n > static_cast<std::uint64_t>(end_ - p_) / sizeof(T))
            return false;
        v->resize(static_cast<std::size_t>(n));
        std::memcpy(v->data(), p_, static_cast<std::size_t>(n) * sizeof(T));
        p_ += static_cast<std::size_t>(n) * sizeof(T);
        return true;
    }
    bool string(std::string* s) {
        std::uint64_t n = 0;
        if (!value(&n) || n > static_cast<std::uint64_t>(end_ - p_))
            return false;
        s->assign(reinterpret_cast<const char*>(p_), static_cast<std::size_t>(n));
        p_ += static_cast<std::size_t>(n);
        return true;
    }

private:
    const std::uint8_t* p_;
    const std::uint8_t* end_;
};

void write_mesh(Out& o, const assets::MeshData& mesh) {
    o.value(mesh.vertex_count);
    o.vector(mesh.positions);
    o.vector(mesh.normals);
    o.vector(mesh.colours);
    o.vector(mesh.uv0);
    o.vector(mesh.uv1);
    o.vector(mesh.skin_indices);
    o.vector(mesh.skin_weights);
    o.value(static_cast<std::uint64_t>(mesh.delta_targets.size()));
    for (const assets::MeshData::DeltaTarget& t : mesh.delta_targets) {
        o.vector(t.positions);
        o.vector(t.normals);
    }
    o.vector(mesh.indices);
    o.vector(mesh.submeshes);
    o.value(mesh.bounds);
    // morph_targets (the keyframe morphs the game animates itself) are not read by a renderer.
}

bool read_mesh(In& in, assets::MeshData* mesh) {
    std::uint64_t targets = 0;
    if (!in.value(&mesh->vertex_count) || !in.vector(&mesh->positions) || !in.vector(&mesh->normals) ||
        !in.vector(&mesh->colours) || !in.vector(&mesh->uv0) || !in.vector(&mesh->uv1) ||
        !in.vector(&mesh->skin_indices) || !in.vector(&mesh->skin_weights) || !in.value(&targets) || targets > 64)
        return false;
    mesh->delta_targets.resize(static_cast<std::size_t>(targets));
    for (assets::MeshData::DeltaTarget& t : mesh->delta_targets)
        if (!in.vector(&t.positions) || !in.vector(&t.normals))
            return false;
    return in.vector(&mesh->indices) && in.vector(&mesh->submeshes) && in.value(&mesh->bounds);
}

}  // namespace

// ---- recording ---------------------------------------------------------------------------------------------------

std::unique_ptr<PacketRecorder> PacketRecorder::from_environment() {
    const char* path = std::getenv("GEKKO_PACKET_RECORD");
    if (path == nullptr || path[0] == '\0')
        return nullptr;
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "[packet-stream] cannot write %s\n", path);
        return nullptr;
    }
    std::fwrite(kMagic, 1, sizeof(kMagic), file);
    const std::vector<std::uint32_t> sizes = layout();
    const auto count = static_cast<std::uint32_t>(sizes.size());
    std::fwrite(&count, sizeof(count), 1, file);
    std::fwrite(sizes.data(), sizeof(std::uint32_t), sizes.size(), file);
    std::fprintf(stderr, "[packet-stream] recording to %s\n", path);
    return std::make_unique<PacketRecorder>(file);
}

PacketRecorder::~PacketRecorder() {
    if (file_ != nullptr)
        std::fclose(file_);
}

void PacketRecorder::write(StreamEvent event, const std::vector<std::uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto kind = static_cast<std::uint32_t>(event);
    const auto bytes = static_cast<std::uint64_t>(payload.size());
    std::fwrite(&kind, sizeof(kind), 1, file_);
    std::fwrite(&bytes, sizeof(bytes), 1, file_);
    std::fwrite(payload.data(), 1, payload.size(), file_);
    ++events_;
    if (event == StreamEvent::Frame)
        std::fflush(file_);  // a run that ends abruptly keeps every frame it rendered
}

void PacketRecorder::texture_source(Scope scope, std::uint32_t handle, const assets::TextureRecord& record,
                                    const std::vector<std::uint8_t>& texels, const assets::ImportOptions& options) {
    Out o;
    o.value(scope);
    o.value(handle);
    o.string(record.name);
    o.string(record.mask_name);
    o.value(record.platform);
    o.value(record.filter_address);
    o.value(record.raster_flags);
    o.value(record.width);
    o.value(record.height);
    o.value(record.depth);
    o.value(record.levels);
    o.value(record.encoding);
    o.value(record.palette_encoding);
    o.value(record.has_alpha);
    o.vector(record.palette);
    o.vector(texels);
    o.value(options.generate_mips);
    write(StreamEvent::TextureSource, o.bytes);
}

void PacketRecorder::texture_image(Scope scope, std::uint32_t handle, const assets::TextureImage& image) {
    Out o;
    o.value(scope);
    o.value(handle);
    o.value(image.width);
    o.value(image.height);
    o.value(image.mip_levels);
    o.value(image.texel_alpha);
    o.vector(image.rgba);
    o.vector(image.level_offsets);
    write(StreamEvent::TextureImage, o.bytes);
}

void PacketRecorder::mesh(Scope scope, std::uint32_t handle, const assets::MeshData& mesh) {
    Out o;
    o.value(scope);
    o.value(handle);
    write_mesh(o, mesh);
    write(StreamEvent::Mesh, o.bytes);
}

void PacketRecorder::release(bool texture, std::uint32_t handle, std::uint64_t named_through) {
    Out o;
    o.value(texture);
    o.value(handle);
    o.value(named_through);
    write(StreamEvent::Release, o.bytes);
}

void PacketRecorder::finish_packets(std::uint64_t tick) {
    Out o;
    o.value(tick);
    write(StreamEvent::FinishPackets, o.bytes);
}

void PacketRecorder::release_scope(Scope scope) {
    Out o;
    o.value(scope);
    write(StreamEvent::ReleaseScope, o.bytes);
}

void PacketRecorder::invalidate_histories() {
    write(StreamEvent::InvalidateHistories, {});
}

void PacketRecorder::frame(const FramePacket& packet, std::uint32_t width, std::uint32_t height, bool read_back,
                           bool commit_history) {
    Out o;
    o.value(width);
    o.value(height);
    o.value(read_back);
    o.value(commit_history);
    o.value(packet.tick);
    o.value(packet.game_ticks);
    o.value(packet.background);
    o.value(packet.content_aspect);
    o.vector(packet.views);
    o.vector(packet.light_sets);
    o.vector(packet.passes);
    o.vector(packet.draws);
    o.vector(packet.bones);
    o.vector(packet.morph_weights);
    o.vector(packet.particles);
    o.vector(packet.particle_batches);
    o.vector(packet.immediate_vertices);
    o.vector(packet.immediate_indices);
    o.vector(packet.immediates);
    o.vector(packet.canvas_regions);
    write(StreamEvent::Frame, o.bytes);
}

// ---- reading -----------------------------------------------------------------------------------------------------

PacketReader::~PacketReader() {
    if (file_ != nullptr)
        std::fclose(file_);
}

bool PacketReader::open(const std::string& path, std::string* error) {
    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) {
        *error = "cannot open " + path;
        return false;
    }
    char magic[8];
    std::uint32_t count = 0;
    if (std::fread(magic, 1, sizeof(magic), file_) != sizeof(magic) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 ||
        std::fread(&count, sizeof(count), 1, file_) != 1 || count > 64) {
        *error = path + " is not a packet stream";
        return false;
    }
    std::vector<std::uint32_t> sizes(count);
    if (std::fread(sizes.data(), sizeof(std::uint32_t), count, file_) != count || sizes != layout()) {
        *error = path + " was recorded with other record layouts (another build)";
        return false;
    }
    return true;
}

bool PacketReader::next(StreamRecord* out, std::string* error) {
    error->clear();
    std::uint32_t kind = 0;
    std::uint64_t bytes = 0;
    if (std::fread(&kind, sizeof(kind), 1, file_) != 1)
        return false;  // the end
    if (std::fread(&bytes, sizeof(bytes), 1, file_) != 1 || bytes > (std::uint64_t{1} << 34)) {
        *error = "damaged stream (event header)";
        return false;
    }
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(bytes));
    if (std::fread(payload.data(), 1, payload.size(), file_) != payload.size()) {
        *error = "damaged stream (truncated event)";
        return false;
    }
    In in(payload.data(), payload.size());
    StreamRecord& r = *out;
    r = StreamRecord{};
    r.event = static_cast<StreamEvent>(kind);
    bool ok = false;
    switch (r.event) {
    case StreamEvent::TextureSource: {
        assets::TextureRecord& t = r.record;
        ok = in.value(&r.scope) && in.value(&r.handle) && in.string(&t.name) && in.string(&t.mask_name) &&
             in.value(&t.platform) && in.value(&t.filter_address) && in.value(&t.raster_flags) &&
             in.value(&t.width) && in.value(&t.height) && in.value(&t.depth) && in.value(&t.levels) &&
             in.value(&t.encoding) && in.value(&t.palette_encoding) && in.value(&t.has_alpha) &&
             in.vector(&t.palette) && in.vector(&r.texels) && in.value(&r.options.generate_mips);
        t.texels = assets::ByteView{r.texels.data(), r.texels.size()};
        break;
    }
    case StreamEvent::TextureImage:
        ok = in.value(&r.scope) && in.value(&r.handle) && in.value(&r.image.width) && in.value(&r.image.height) &&
             in.value(&r.image.mip_levels) && in.value(&r.image.texel_alpha) && in.vector(&r.image.rgba) &&
             in.vector(&r.image.level_offsets);
        break;
    case StreamEvent::Mesh:
        ok = in.value(&r.scope) && in.value(&r.handle) && read_mesh(in, &r.mesh);
        break;
    case StreamEvent::Release:
        ok = in.value(&r.texture) && in.value(&r.handle) && in.value(&r.tick);
        break;
    case StreamEvent::FinishPackets:
        ok = in.value(&r.tick);
        break;
    case StreamEvent::ReleaseScope:
        ok = in.value(&r.scope);
        break;
    case StreamEvent::InvalidateHistories:
        ok = true;
        break;
    case StreamEvent::Frame: {
        FramePacket& p = r.packet;
        ok = in.value(&r.width) && in.value(&r.height) && in.value(&r.read_back) && in.value(&r.commit_history) &&
             in.value(&p.tick) && in.value(&p.game_ticks) && in.value(&p.background) && in.value(&p.content_aspect) &&
             in.vector(&p.views) && in.vector(&p.light_sets) && in.vector(&p.passes) && in.vector(&p.draws) &&
             in.vector(&p.bones) && in.vector(&p.morph_weights) && in.vector(&p.particles) &&
             in.vector(&p.particle_batches) && in.vector(&p.immediate_vertices) && in.vector(&p.immediate_indices) &&
             in.vector(&p.immediates) && in.vector(&p.canvas_regions);
        break;
    }
    }
    if (!ok) {
        *error = "damaged stream (event " + std::to_string(kind) + ")";
        return false;
    }
    return true;
}

}  // namespace shadow::render
