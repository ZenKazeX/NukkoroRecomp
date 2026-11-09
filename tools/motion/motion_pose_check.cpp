// Plays one clip of a motion bank (.MTP) on its skeleton (.BON) and prints every bone's matrix per frame as raw
// binary32 bits, for comparison with the animation research's reference player (motion_player.py, MotionPlayer).
// usage: motion_pose_check <file.BON> <file.MTP> <clip name> <frames>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "motion/skeleton_pose.h"

namespace {

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) | (std::uint32_t{b[at + 2]} << 8) | b[at + 3];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: motion_pose_check <file.BON> <file.MTP> <clip name> <frames>\n");
        return 2;
    }
    const std::vector<std::uint8_t> bon = read_file(argv[1]), mtp = read_file(argv[2]);
    shadow::motion::Skeleton skeleton;
    if (!shadow::motion::parse_skeleton(bon.data(), bon.size(), 0u, &skeleton)) {
        std::fprintf(stderr, "bad skeleton\n");
        return 1;
    }
    // Bank: u16 count at +2, the table offset at +0x10; entries {name, clip, events}, file-relative.
    const std::size_t count = (std::size_t{mtp[2]} << 8) | mtp[3];
    const std::size_t table = be32(mtp, 0x10);
    shadow::motion::Clip clip;
    bool found = false;
    for (std::size_t i = 0; i < count && !found; ++i) {
        const std::size_t name = be32(mtp, table + 12 * i), at = be32(mtp, table + 12 * i + 4);
        if (std::strcmp(reinterpret_cast<const char*>(mtp.data() + name), argv[3]) != 0)
            continue;
        const std::size_t size = be32(mtp, at + 8);
        found = at + size <= mtp.size() && shadow::motion::parse_clip(mtp.data() + at, size, &clip);
    }
    if (!found) {
        std::fprintf(stderr, "no clip %s\n", argv[3]);
        return 1;
    }
    shadow::motion::SkeletonPose pose;
    if (!pose.bind(skeleton, clip)) {
        std::fprintf(stderr, "bind failed\n");
        return 1;
    }
    const int frames = std::atoi(argv[4]);
    std::vector<shadow::motion::Mtx34> world;
    for (int f = 0; f < frames; ++f) {
        if (f > 0)
            pose.advance();
        pose.evaluate(&world);
        for (std::size_t b = 0; b < world.size(); ++b) {
            std::printf("%d %08X", f, skeleton.bones[b].id);
            for (float v : world[b]) {
                std::uint32_t u;
                std::memcpy(&u, &v, 4);
                std::printf(" %08X", u);
            }
            std::printf("\n");
        }
    }
    return 0;
}
