#pragma once

// A recorded stream of what the game hands a renderer (docs/VULKAN_RENDERER.md, "game packets"): every resource it
// asks for and releases, every packet boundary it reports, and every FramePacket it has rendered - in the order the
// renderer saw them. Recorded from the D3D12 renderer (GEKKO_PACKET_RECORD=<file>, non-shipping builds), replayed by
// any renderer with the same contract (render_vk's replay tool), so two backends can draw the same game frames.
//
// The format is this build's own: the packet's records are written as their bytes (every one is trivially copyable),
// and the header carries their sizes - a reader built with other layouts refuses the stream. Plain values, no
// graphics API.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "assets/geometry_import.h"
#include "assets/texture_import.h"
#include "render/frame.h"

namespace shadow::render {

enum class StreamEvent : std::uint32_t {
    TextureSource = 1,  // a texture from a stored record (request_texture): the record, its texels, the options
    TextureImage = 2,   // a texture from decoded texels (create_texture)
    Mesh = 3,           // a mesh (request_mesh / create_mesh)
    Release = 4,        // a texture or mesh released (named through a packet tick)
    FinishPackets = 5,  // every packet up to a tick is recorded or discarded
    ReleaseScope = 6,
    InvalidateHistories = 7,
    Frame = 8,          // a packet rendered: to the window, or to an image (read back or not)
};

// One event as read back.
struct StreamRecord {
    StreamEvent event = StreamEvent::Frame;
    Scope scope = Scope::Session;
    std::uint32_t handle = 0;       // the handle the recording renderer issued (resources), or released
    bool texture = false;           // Release: a texture (else a mesh)
    std::uint64_t tick = 0;         // Release: named through; FinishPackets: the tick
    assets::TextureRecord record;   // TextureSource (record.texels points into `texels`)
    std::vector<std::uint8_t> texels;
    assets::ImportOptions options;
    assets::TextureImage image;     // TextureImage
    assets::MeshData mesh;          // Mesh
    FramePacket packet;             // Frame
    std::uint32_t width = 0, height = 0;  // Frame: the image's size (0: the window)
    bool read_back = false;         // Frame: the image was read back (a capture)
    bool commit_history = true;     // Frame
};

class PacketRecorder {
public:
    // A recorder writing to GEKKO_PACKET_RECORD, or null when the variable is not set (or the file cannot be made).
    static std::unique_ptr<PacketRecorder> from_environment();
    explicit PacketRecorder(std::FILE* file) : file_(file) {}
    PacketRecorder(const PacketRecorder&) = delete;
    PacketRecorder& operator=(const PacketRecorder&) = delete;
    ~PacketRecorder();

    void texture_source(Scope scope, std::uint32_t handle, const assets::TextureRecord& record,
                        const std::vector<std::uint8_t>& texels, const assets::ImportOptions& options);
    void texture_image(Scope scope, std::uint32_t handle, const assets::TextureImage& image);
    void mesh(Scope scope, std::uint32_t handle, const assets::MeshData& mesh);
    void release(bool texture, std::uint32_t handle, std::uint64_t named_through);
    void finish_packets(std::uint64_t tick);
    void release_scope(Scope scope);
    void invalidate_histories();
    // `width` x `height` 0 x 0: rendered to the window.
    void frame(const FramePacket& packet, std::uint32_t width, std::uint32_t height, bool read_back,
               bool commit_history);
    std::uint64_t events() const noexcept { return events_; }

private:
    void write(StreamEvent event, const std::vector<std::uint8_t>& payload);
    std::mutex mutex_;  // events come from the game thread (resources) and the rendering thread (frames)
    std::FILE* file_ = nullptr;
    std::uint64_t events_ = 0;
};

class PacketReader {
public:
    PacketReader() = default;
    PacketReader(const PacketReader&) = delete;
    PacketReader& operator=(const PacketReader&) = delete;
    ~PacketReader();
    bool open(const std::string& path, std::string* error);
    // The next event; false at the end (`error` empty) or on a damaged stream (`error` says why).
    bool next(StreamRecord* out, std::string* error);

private:
    std::FILE* file_ = nullptr;
};

}  // namespace shadow::render
