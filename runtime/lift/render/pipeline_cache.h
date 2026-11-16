#pragma once
// The pipelines a renderer has made, kept across runs (docs/PIPELINE_CACHE.md).
//
// A pipeline state the renderer has not made yet is compiled by the driver the first time a draw needs it, in the
// middle of a frame: a hitch, the first time each blend, cull, depth or format combination is seen. The renderer
// notes every pipeline it makes here, and the list is saved next to the settings; at the next start the renderer
// makes them all before the first frame. The driver's own shader cache then makes those creations cheap.
//
// The file holds plain values only (no driver blobs), so it does not depend on the GPU or the driver: a file written
// by another build (another shader set) is ignored, a damaged one is ignored, and either is rewritten as pipelines are
// made again. Nothing here touches Direct3D: the renderer turns a record back into its pipeline.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace shadow::render {

// One pipeline, as the values the renderer makes it from.
struct PipelineRecord {
    enum class Kind : std::uint8_t { Mesh = 1, Screen = 2, Compute = 3 };
    // Mesh flags.
    static constexpr std::uint8_t kDepthTest = 1u << 0;
    static constexpr std::uint8_t kDepthWrite = 1u << 1;
    static constexpr std::uint8_t kLines = 1u << 2;
    static constexpr std::uint8_t kTiles = 1u << 3;
    static constexpr std::uint8_t kPlain = 1u << 4;
    static constexpr std::uint8_t kNoDepth = 1u << 5;
    Kind kind = Kind::Mesh;
    std::uint8_t program = 0;  // Mesh: the program; Screen: the screen pass; Compute: the compute step
    std::uint8_t src = 0, dst = 0, cull = 0;  // Mesh: the blend factors and cull mode
    std::uint8_t flags = 0;                   // Mesh: the k* flags
    std::uint32_t format = 0;                 // the colour format (DXGI_FORMAT); 0 for Compute
    std::uint32_t samples = 0;                // 0 for Compute

    bool operator<(const PipelineRecord& o) const {
        return std::tie(kind, program, src, dst, cull, flags, format, samples) <
               std::tie(o.kind, o.program, o.src, o.dst, o.cull, o.flags, o.format, o.samples);
    }
    bool operator==(const PipelineRecord& o) const { return !(*this < o) && !(o < *this); }
};

// A mesh pipeline's key with the fields that cannot change the pipeline cleared, so equivalent keys are one pipeline:
// - plain: the blend unit is off, so the blend factors are unused (they read as One / Zero);
// - no depth target: depth test and write are unused;
// - lines: only the immediate program draws lines.
// `blend_one` / `blend_zero`: the renderer's Blend values; `immediate`: its immediate program.
PipelineRecord canonical_mesh(PipelineRecord record, std::uint8_t blend_one, std::uint8_t blend_zero,
                              std::uint8_t immediate) noexcept;

// 64-bit FNV-1a, for the build's shader identity (seed: the previous hash, to chain several blobs).
std::uint64_t pipeline_cache_hash(const void* data, std::size_t size,
                                  std::uint64_t seed = 0xCBF29CE484222325ull) noexcept;

class PipelineCache {
public:
    static constexpr std::uint32_t kMagic = 0x434C5053u;  // "SPLC"
    static constexpr std::uint32_t kVersion = 1u;
    static constexpr std::size_t kMaxRecords = 4096u;  // more is a runaway: the renderer makes a few dozen

    // `build`: identifies the shader set; a file written for another one is not loaded.
    explicit PipelineCache(std::uint64_t build = 0) noexcept : build_(build) {}

    // Reads the records saved at `path`. False (and `why`) when there is none to use: no file, another build's,
    // damaged. The records already noted are kept either way.
    bool load(const std::filesystem::path& path, std::string* why = nullptr);
    // Writes every record to `path` (through a temporary file renamed over it, so a crash never leaves half a file).
    bool save(const std::filesystem::path& path, std::string* error = nullptr);

    // Notes a pipeline made; true when it is new (the cache then has unsaved records).
    bool note(const PipelineRecord& record);
    bool dirty() const noexcept { return dirty_; }

    // The records, in the order they were first noted or loaded.
    const std::vector<PipelineRecord>& records() const noexcept { return order_; }
    std::uint64_t build() const noexcept { return build_; }

    // The file's bytes for `records` (tests, tools).
    static std::vector<std::uint8_t> encode(std::uint64_t build, const std::vector<PipelineRecord>& records);
    // The records in `bytes`, or false and `why`.
    static bool decode(const std::vector<std::uint8_t>& bytes, std::uint64_t build, std::vector<PipelineRecord>* out,
                       std::string* why);

private:
    std::uint64_t build_;
    std::set<PipelineRecord> known_;
    std::vector<PipelineRecord> order_;
    bool dirty_ = false;
};

}  // namespace shadow::render
