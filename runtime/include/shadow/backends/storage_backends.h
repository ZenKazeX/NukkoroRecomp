// Storage backends of the runtime contract (shadow/runtime/v1/runtime.h) and their registry.
//
//   host-directory   the extracted disc tree (<game data>/files) on the host file system: every file a mapped view
//                    (gekko::assets::FileProvider). What the PC build has always read.
//   gamecube-disc    the console's own medium: a GameCube disc image (GCM; tools/platform/gcm.py rebuilds it from the
//                    extracted sys/ and files/ at the offsets the original fst.bin gives). The tree is the image's
//                    FST, read from the image (boot.bin 0x424 / 0x428), and each file is the extent its FST entry
//                    names. Disc offsets exist only inside this backend. It models the medium, not the drive: no DI
//                    registers, no seek timing (the DVD SDK's own code is pruned from the build, docs/DISC_SYSTEM_MAP.md).
//
// Selection: create_storage(name, options) at start-up (--storage-backend), negotiated against kStorageRequired.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gekko/asset_namespace.h"
#include "gekko/types.h"
#include "shadow/runtime/v1/runtime.h"

namespace gekko::assets {
class AssetSystem;
}

namespace shadow::backends {

struct StorageOptions {
    std::filesystem::path files_root;  // host-directory: the files/ directory
    std::filesystem::path disc_image;  // gamecube-disc: the .gcm image
    // host-directory: host files served at the tree's root under their own names when the tree lacks those names
    // (the Japanese voice files from the player's Japanese disc, gekko/voice_language.h). Never replaces a file.
    std::vector<std::filesystem::path> extra_files;
};

// The names create_storage accepts, in registry order.
std::vector<std::string_view> storage_backend_names();

// A backend by name, already negotiated against the core's requirements (runtime::v1::kStorageRequired). Null with
// `error` set when the name is unknown, the backend cannot open its medium, or negotiation refuses it.
std::unique_ptr<runtime::v1::Storage> create_storage(std::string_view name, const StorageOptions& options,
                                                     std::string& error);

// The namespace of a host directory tree, enumerated by the host-directory backend (the launcher's check mode,
// tests comparing the tree with the disc's FST). Names outside the contract's rule are refused.
bool namespace_from_host_directory(const std::filesystem::path& root, gekko::assets::AssetNamespace& out,
                                   std::string& error);

// The extracted tree under files_root mounted through the host-directory backend (tests, tools, the default launch).
bool mount_host_directory(gekko::assets::AssetSystem& assets, const std::filesystem::path& files_root,
                          std::vector<gekko::u8> identity, std::string& error);
// The same with the namespace already built: its files must exist under files_root with their sizes.
bool mount_host_directory(gekko::assets::AssetSystem& assets, const std::filesystem::path& files_root,
                          gekko::assets::AssetNamespace names, std::vector<gekko::u8> identity, std::string& error);

}  // namespace shadow::backends
