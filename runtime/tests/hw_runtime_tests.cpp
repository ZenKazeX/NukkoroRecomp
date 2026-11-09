// Hardware-type tests: generational handles and the audio host contract.
// The COPY-queue and resource-state tests went with hw::GpuRuntime: the COPY
// queue is render::TransferQueue now (render_rhi_tests), and resource
// state is tracked per object by the RHI backend.

#include "gekko/hw/audio.h"
#include "gekko/hw/types.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using gekko::u32;
using gekko::u64;
using gekko::u8;
using gekko::hw::Handle;
using gekko::hw::TextureTag;

int g_failures = 0;
int g_skip = 0;

void check(bool condition, int line, const char* text) {
    if (!condition) {
        ++g_failures;
        std::printf("    CHECK failed (line %d): %s\n", line, text);
    }
}
#define CHECK(condition) check(static_cast<bool>(condition), __LINE__, #condition)

void test_handles() {
    std::printf("handles\n");
    gekko::hw::TextureHandle a{};
    CHECK(!a.valid());
    a.index = 3;
    a.generation = 1;
    CHECK(a.valid());
    const auto b = gekko::hw::TextureHandle::from_pack(a.pack());
    CHECK(b == a);
}

void test_audio_contract() {
    std::printf("audio contract\n");
    CHECK(gekko::hw::kAudioRingFrames == 16384u);
    gekko::hw::AudioHost host;
    gekko::AudioRuntime::Config config;
    config.mode = gekko::AudioRuntime::Mode::Manual;
    config.ring_frames = gekko::hw::kAudioRingFrames;
    CHECK(host.open(config));
    u8 block[32]{};
    host.publish_block(block, sizeof(block));
    CHECK(host.source_fill_frames() == 8u);
    host.close();
}

}  // namespace

int main() {
    test_handles();
    test_audio_contract();
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    if (g_skip != 0) {
        std::printf("passed with skip %d\n", g_skip);
        return g_skip;
    }
    std::printf("ok\n");
    return 0;
}
