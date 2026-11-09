#pragma once

// A stage's land as the new renderer draws it, loaded straight from the disc (tools/stage_preview and
// tools/stage_viewer). Every ONE archive of the stage folder is opened; texture dictionaries are collected by
// texture name; each archive's land worlds are chosen with the game's own class rule (per class token in table
// order, the first member whose name contains it); meshes and textures go to the renderer in the Stage scope; the
// draws are listed in the game's pass order with their land-class recipes.

#include <filesystem>
#include <string>
#include <vector>

#include "render/frame.h"
#include "render/renderer.h"

namespace stage {

struct Draw {
    shadow::render::MeshHandle mesh;
    std::uint32_t submesh = 0;
    shadow::render::Material material;
    shadow::render::Recipe recipe;
    bool far_scenery = false;  // the N / D classes: sky domes and skylines
};

struct Scene {
    std::vector<Draw> draws;   // in the game's pass order
    float lo[3] = {0, 0, 0};   // bounds of the land itself (far scenery excluded)
    float hi[3] = {0, 0, 0};
    std::size_t worlds = 0, textures = 0, triangles = 0, missing_textures = 0;
};

struct LoadOptions {
    bool cull = true;  // back-face culling where the land class culls
};

bool load(shadow::render::Renderer& renderer, const std::filesystem::path& files, const std::string& stage,
          const LoadOptions& options, Scene* out, std::string* error);

// Adds the scene's draws to the builder's open pass.
void submit(const Scene& scene, shadow::render::FrameBuilder& builder, bool include_far_scenery);

}  // namespace stage
