#pragma once

// Geometry becomes native vertex and index data once, at load (docs/lift-20261006/R_DESIGN.md 2.4, R2_DRIVER.md 2.3).
//
// Two sources on the disc (R2_MATERIAL_CENSUS.md sections 2-4):
//   * models: RenderWare geometry streams - per-vertex arrays shared by every attribute, triangle strips per
//     material (the mesh plugin), morph targets, prelit colours, normals, up to two UV sets, skin weights;
//   * worlds: sectors whose vertex data was prepared for the console's draw lists - one array per attribute
//     (positions, colours, UV sets) and per material a list of triangle strips in which every vertex names an
//     index into each array. The importer turns each distinct index combination into one vertex.
// Either way the result is the same native shape: float streams, triangle lists (degenerate strip triangles
// dropped, winding kept), one submesh per material.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/bytes.h"
#include "assets/chunk_stream.h"
#include "assets/material_import.h"

namespace shadow::assets {

struct Sphere {
    float x = 0, y = 0, z = 0, radius = 0;
};

struct MeshData {
    std::uint32_t vertex_count = 0;
    std::vector<float> positions;          // 3 per vertex
    std::vector<float> normals;            // 3 per vertex, or empty
    std::vector<std::uint8_t> colours;     // RGBA8 per vertex, or empty
    std::vector<float> uv0, uv1;           // 2 per vertex, or empty
    std::vector<std::uint8_t> skin_indices;  // 4 per vertex, or empty
    std::vector<float> skin_weights;         // 4 per vertex, or empty
    struct MorphTarget {
        Sphere bounds;
        std::vector<float> positions, normals;
    };
    std::vector<MorphTarget> morph_targets;  // targets after the first (the first is `positions` / `normals`)
    // Delta-morph targets (facial animation, the slingshot plant's band, flags, drapes): offsets the game blends onto
    // `positions` / `normals` with one weight per target at every draw - the drawn shape is base + sum w_t delta_t,
    // computed by the renderer from the draw's weights; the mesh itself never changes. One offset per vertex (zero where
    // the target leaves the vertex alone); a target's array is empty when it does not move that attribute.
    struct DeltaTarget {
        std::vector<float> positions;  // 3 per vertex, or empty
        std::vector<float> normals;    // 3 per vertex, or empty
    };
    std::vector<DeltaTarget> delta_targets;
    std::vector<std::uint32_t> indices;      // triangle list
    struct Submesh {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material = 0;          // index into the owner's material list
    };
    std::vector<Submesh> submeshes;
    Sphere bounds;
};

struct SkinData {
    std::uint32_t bone_count = 0;
    std::uint32_t max_weights = 0;
    std::vector<std::uint8_t> used_bones;
    std::vector<float> inverse_bind;          // 16 per bone, row-major as stored
    std::uint32_t bone_limit = 0;
    std::uint32_t split_meshes = 0;
};

// A delta-morph target: per-vertex offsets the game blends onto the base mesh with a weight (facial animation and
// similar). Stored on the disc run-length encoded over the vertices; expanded here to one offset per vertex.
struct DeltaMorphTarget {
    std::string name;
    std::uint32_t flags = 0;                  // what the target moves (geometry-flag bits: 0x02 positions, 0x10 normals)
    std::vector<float> positions;             // 3 per vertex (zero where the target leaves the vertex alone), or empty
    std::vector<float> normals;               // 3 per vertex, or empty
    Sphere bounds;
};

struct ModelGeometry {
    std::uint32_t format = 0;                 // geometry flags (bits 0-7) and UV-set count (bits 16-23)
    std::uint32_t material_count = 0;
    std::vector<MaterialRecord> materials;
    MeshData mesh;
    bool skinned = false;
    SkinData skin;
    bool delta_morph = false;                 // carries delta-morph targets
    std::vector<DeltaMorphTarget> delta_targets;
    std::uint32_t source_strips = 0;          // strips in the mesh plugin
};

// Parses one geometry chunk (type 0x0F) of `bytes`.
bool import_geometry(ByteView bytes, const Chunk& geometry, ModelGeometry* out, std::string* error);

// A model as stored: its frame hierarchy, its geometries and the atomics that place a geometry on a frame.
struct Clump {
    struct Frame {
        float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // right, up, at (rows, as stored)
        float position[3] = {0, 0, 0};
        std::int32_t parent = -1;
    };
    struct Atomic {
        std::uint32_t frame = 0;
        std::uint32_t geometry = 0;
        std::uint32_t flags = 0;
    };
    std::vector<Frame> frames;
    std::vector<ModelGeometry> geometries;
    std::vector<Atomic> atomics;
};

// Parses one clump chunk (type 0x10): frame list, geometry list and atomics.
bool import_clump(ByteView bytes, const Chunk& clump, Clump* out, std::string* error);

struct WorldSector {
    MeshData mesh;                            // empty when the sector carries no vertex data
    std::uint32_t attributes = 0;             // bit mask: 1 position, 2 colour, 4 UV0, 8 UV1
    std::uint32_t strips = 0;
    std::uint32_t strip_vertices = 0;
};

struct World {
    std::uint32_t format = 0;                 // world flags (bits 0-7), UV sets (16-23), native (bit 24)
    std::uint32_t material_count = 0;
    std::vector<MaterialRecord> materials;
    std::uint32_t plane_sectors = 0;
    std::vector<WorldSector> sectors;         // atomic sectors, in stream order
};

// Parses one world chunk (type 0x0B): sectors with prepared vertex data (the RG1 worlds) or platform-independent
// sectors (the RP2 worlds; collision worlds parse too, their sectors carry no draw meshes).
bool import_world(ByteView bytes, const Chunk& world, World* out, std::string* error);

// The prepared vertex data of one sector, as described by its big-endian header: one array per attribute and one
// draw list (triangle strips whose vertices name an index into every array) per mesh, all at offsets into `data`.
// The disc stores the offsets relative to the data block; the game relocates them when it reads the block into
// memory - the caller makes them relative again.
struct PreparedArrays {
    struct Attribute {
        std::uint32_t offset = 0;
        std::uint8_t kind = 0;         // 9 position (f32 x3), 11 colour (RGBA8), 13 UV0 / 14 UV1 (f32 x2)
        std::uint8_t stride = 0;
        std::uint8_t index_bytes = 0;  // 1 or 2 per vertex in the draw lists
    };
    struct List {
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
    };
    std::vector<Attribute> attributes;
    std::vector<List> lists;  // one per mesh, in mesh order
    ByteView data;
};

// Builds the native mesh of prepared arrays: one vertex per distinct index combination, strips to triangle lists
// (degenerate triangles dropped, winding kept), submesh j from draw list j with material `materials[j]`.
bool import_prepared_mesh(const PreparedArrays& arrays, const std::vector<std::uint32_t>& materials,
                          WorldSector* out, std::string* error);

// The triangles of a triangle strip, appended to a triangle list: winding kept (every other triangle swaps its first
// two corners), triangles with two equal corners dropped. The one convention every importer path uses.
void append_strip_triangles(const std::uint32_t* strip, std::size_t count, std::vector<std::uint32_t>& out);
// The bounding sphere of positions (3 floats per vertex): the box centre and the farthest vertex.
Sphere bounding_sphere(const std::vector<float>& positions);

}  // namespace shadow::assets
