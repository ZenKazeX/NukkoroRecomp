#include "embedded_game_data.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace shadow {
namespace {

#ifdef _WIN32

constexpr int kBootResource = 101;
constexpr int kBi2Resource = 102;
constexpr int kDolResource = 105;

std::vector<std::uint8_t> load_resource(int identifier) {
    const HMODULE executable = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(
        executable, MAKEINTRESOURCEW(identifier), MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (resource == nullptr) {
        return {};
    }
    const DWORD size = SizeofResource(executable, resource);
    const HGLOBAL loaded = LoadResource(executable, resource);
    const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (bytes == nullptr || size == 0u) {
        return {};
    }
    const auto* begin = static_cast<const std::uint8_t*>(bytes);
    return std::vector<std::uint8_t>(begin, begin + size);
}

#endif

}  // namespace

EmbeddedGameData load_embedded_game_data() {
    EmbeddedGameData data;
#ifdef _WIN32
    data.boot = load_resource(kBootResource);
    data.bi2 = load_resource(kBi2Resource);
    data.dol = load_resource(kDolResource);
#endif
    return data;
}

}  // namespace shadow
