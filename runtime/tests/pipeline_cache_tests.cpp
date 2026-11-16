// The pipeline cache's file and records (render/pipeline_cache.h, docs/PIPELINE_CACHE.md). No device: it runs on any
// host.

#include "render/pipeline_cache.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using shadow::render::PipelineCache;
using shadow::render::PipelineRecord;

namespace {

void require(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

PipelineRecord mesh(std::uint8_t program, std::uint8_t src, std::uint8_t dst, std::uint8_t flags,
                    std::uint32_t format = 10u, std::uint32_t samples = 4u) {
    PipelineRecord r;
    r.kind = PipelineRecord::Kind::Mesh;
    r.program = program;
    r.src = src;
    r.dst = dst;
    r.cull = 1u;
    r.flags = flags;
    r.format = format;
    r.samples = samples;
    return r;
}

std::filesystem::path scratch(const char* name) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "shadow-pipeline-cache-tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

constexpr std::uint8_t kOne = 1u, kZero = 0u, kImmediate = 4u;

void test_round_trip() {
    const auto path = scratch("round_trip.cache");
    std::filesystem::remove(path);
    PipelineCache cache(0x1234u);
    require(!cache.load(path), "no file: nothing loaded");
    require(cache.note(mesh(0, 4, 5, PipelineRecord::kDepthTest | PipelineRecord::kDepthWrite)), "a new record");
    require(!cache.note(mesh(0, 4, 5, PipelineRecord::kDepthTest | PipelineRecord::kDepthWrite)), "a repeat is not new");
    PipelineRecord screen;
    screen.kind = PipelineRecord::Kind::Screen;
    screen.program = 5u;
    screen.format = 28u;
    screen.samples = 1u;
    PipelineRecord compute;
    compute.kind = PipelineRecord::Kind::Compute;
    compute.program = 2u;
    require(cache.note(screen) && cache.note(compute) && cache.dirty(), "three records, unsaved");
    std::string error;
    require(cache.save(path, &error) && !cache.dirty(), "saved");
    require(!std::filesystem::exists(path.string() + ".tmp"), "no temporary left behind");

    PipelineCache again(0x1234u);
    std::string why;
    require(again.load(path, &why), "the same build loads it");
    require(again.records().size() == 3u && again.records()[0] == cache.records()[0] &&
                again.records()[1] == screen && again.records()[2] == compute && !again.dirty(),
            "every record comes back, in order, nothing unsaved");
    require(!again.note(screen), "a loaded record is known");
}

void test_refusals() {
    const auto path = scratch("refusals.cache");
    PipelineCache cache(7u);
    cache.note(mesh(1, 4, 5, 0));
    require(cache.save(path), "saved");
    std::string why;
    PipelineCache other(8u);
    require(!other.load(path, &why) && why == "written by another build" && other.records().empty(),
            "another build's file is ignored");

    std::vector<std::uint8_t> bytes = PipelineCache::encode(7u, cache.records());
    std::vector<PipelineRecord> out;
    require(PipelineCache::decode(bytes, 7u, &out, &why) && out.size() == 1u, "the encoded bytes decode");
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        std::vector<std::uint8_t> damaged = bytes;
        damaged[i] ^= 0x40u;
        require(!PipelineCache::decode(damaged, 7u, &out, &why), "any flipped byte is refused");
    }
    std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 1);
    require(!PipelineCache::decode(truncated, 7u, &out, &why), "a truncated file is refused");
    require(!PipelineCache::decode({}, 7u, &out, &why) && why == "too short", "an empty file is refused");
    // A file of nonsense of the right length.
    std::ofstream(path, std::ios::binary | std::ios::trunc) << std::string(bytes.size(), 'x');
    PipelineCache junk(7u);
    require(!junk.load(path, &why) && junk.records().empty(), "junk is ignored");
}

void test_canonical_mesh() {
    using R = PipelineRecord;
    // plain: the blend factors are unused.
    const R a = shadow::render::canonical_mesh(mesh(0, 4, 5, R::kPlain | R::kDepthTest), kOne, kZero, kImmediate);
    const R b = shadow::render::canonical_mesh(mesh(0, 1, 0, R::kPlain | R::kDepthTest), kOne, kZero, kImmediate);
    require(a == b && a.src == kOne && a.dst == kZero, "plain keys share one pipeline");
    // no depth target: the depth flags are unused.
    const R c = shadow::render::canonical_mesh(mesh(4, 4, 5, R::kNoDepth | R::kDepthTest | R::kDepthWrite), kOne,
                                               kZero, kImmediate);
    require((c.flags & (R::kDepthTest | R::kDepthWrite)) == 0u && (c.flags & R::kNoDepth) != 0u,
            "without a depth target the depth flags are cleared");
    // lines: immediate only.
    const R d = shadow::render::canonical_mesh(mesh(0, 4, 5, R::kLines), kOne, kZero, kImmediate);
    const R e = shadow::render::canonical_mesh(mesh(kImmediate, 4, 5, R::kLines), kOne, kZero, kImmediate);
    require((d.flags & R::kLines) == 0u && (e.flags & R::kLines) != 0u, "only the immediate program keeps lines");
    // Everything that does change a pipeline stays.
    const R f = shadow::render::canonical_mesh(mesh(2, 4, 5, R::kDepthTest | R::kTiles, 24u, 8u), kOne, kZero,
                                               kImmediate);
    require(f == mesh(2, 4, 5, R::kDepthTest | R::kTiles, 24u, 8u), "a blended, depth-tested key is unchanged");
    PipelineRecord screen;
    screen.kind = PipelineRecord::Kind::Screen;
    screen.flags = R::kPlain;
    screen.src = 9u;
    require(shadow::render::canonical_mesh(screen, kOne, kZero, kImmediate) == screen, "only mesh keys change");
}

void test_cap() {
    PipelineCache cache(1u);
    for (std::uint32_t i = 0; i < PipelineCache::kMaxRecords + 10u; ++i)
        cache.note(mesh(0, 0, 0, 0, i, 1u));
    require(cache.records().size() == PipelineCache::kMaxRecords, "the cache stops at its cap");
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        void (*run)();
    } tests[] = {
        {"round trip", test_round_trip},
        {"refusals", test_refusals},
        {"canonical mesh keys", test_canonical_mesh},
        {"cap", test_cap},
    };
    int failures = 0;
    for (const Test& t : tests) {
        try {
            t.run();
            std::cout << "PASS " << t.name << "\n";
        } catch (const std::exception& e) {
            std::cout << "FAIL " << t.name << ": " << e.what() << "\n";
            ++failures;
        }
    }
    std::cout << (failures == 0 ? "all pipeline cache tests passed\n" : "pipeline cache tests failed\n");
    return failures == 0 ? 0 : 1;
}
