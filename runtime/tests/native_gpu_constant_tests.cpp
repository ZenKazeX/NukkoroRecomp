// Actual native renderer commands on DX12. The opt-in oracle reconstructs all
// 512 constant bytes for every claimed source restoration and compares them.
// Counter witnesses distinguish eligibility from merely unchanged output.
#include "gekko/native_gpu.h"
#include "plume_d3d12.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {
using namespace gekko;
using namespace gekko::native;
int failures = 0;
void check(bool value, const char* scenario, const char* expression, int line) {
    if (!value) {
        ++failures;
        std::printf("FAIL %s:%d: %s\n", scenario, line, expression);
    }
}
#define CHECK(value) check(static_cast<bool>(value), name, #value, __LINE__)

struct ObserverEnvironment {
    std::string previous;
    bool existed = false;
    explicit ObserverEnvironment(bool validate) {
        if (const char* p = std::getenv("GEKKO_NATIVE_CONSTANT_VALIDATE")) {
            previous = p;
            existed = true;
        }
        _putenv_s("GEKKO_NATIVE_CONSTANT_VALIDATE", validate ? "1" : "0");
    }
    ~ObserverEnvironment() {
        _putenv_s("GEKKO_NATIVE_CONSTANT_VALIDATE", existed ? previous.c_str() : "");
    }
};

Projection projection(float translation = 0.0f) {
    return Projection{1u, {1.0f, translation, 1.0f, 0.0f, 1.0f, 0.0f}};
}
float float_bits(u32 bits) {
    float value;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
Fog fog(float start = 1.0f) {
    return Fog{2u, start, 10.0f, 0.1f, 100.0f, {19u, 31u, 47u, 255u}};
}
void setup(FrameSegment& segment, const Projection& p, const Fog& f) {
    segment.append(Op::Cull, Scalar{0u, 0u});
    segment.append(Op::DepthMode, DepthMode{0u, 7u, 0u, 0u});
    segment.append(Op::Blend, Blend{});
    segment.append(Op::AlphaCompare, AlphaCompare{});
    segment.append(Op::Viewport, Viewport{0, 0, 640, 480, 0, 1, -1});
    segment.append(Op::Scissor, Scissor{0, 0, 640, 480});
    segment.append(Op::Projection, p);
    segment.append(Op::Fog, f);
}
void append_draw(FrameSegment& segment, MaterialProgram program, u32 attributes) {
    std::array<shadow::render::RenderVertex, 3> vertices{};
    constexpr float positions[3][3] = {{-0.5f, -0.5f, 0}, {0, 0.5f, 0}, {0.5f, -0.5f, 0}};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        for (std::size_t j = 0; j < 3; ++j) vertices[i].position[j] = positions[i][j];
        vertices[i].normal[2] = 1.0f;
        for (unsigned j = 0; j < 4; ++j) vertices[i].color0[j] = vertices[i].color1[j] = 255u;
        vertices[i].uv[0][0] = static_cast<float>(i) * 0.25f;
        vertices[i].uv[0][1] = static_cast<float>(i) * 0.125f;
    }
    DrawImmediate draw{};
    draw.producer = DrawProducer::Im3D;
    draw.program = program;
    draw.primitive = 2u;
    draw.attributes = attributes;
    draw.vertex_count = static_cast<u32>(vertices.size());
    draw.vertices = segment.add_payload(vertices.data(), sizeof(vertices));
    segment.append(Op::DrawImmediate, draw);
}
constexpr u32 attributes = kImmediatePosition | kImmediateNormal | kImmediateColor0 | kImmediateUv0;
enum class Expect { Rebuild, Restored, Identity };
struct Fixture {
    NativeGpu gpu;
    bool validate;
    MaterialProgram program = MaterialProgram::PassColor;
    u32 vertex_attributes = attributes;
    Projection p = projection();
    Fog f = fog();
    explicit Fixture(bool enabled) : validate(enabled) {}

    void step(const char* name, FrameSegment& segment, Expect expected) {
        const auto before = gpu.stats();
        append_draw(segment, program, vertex_attributes);
        gpu.process(segment);
        const auto after = gpu.stats();
        CHECK(after.draws == before.draws + 1u);
        CHECK(after.last_error.empty());
        CHECK(after.constant_restore_mismatches == 0u);
        if (expected == Expect::Rebuild) {
            CHECK(after.constant_materializations == before.constant_materializations + 1u);
            CHECK(after.constant_restored_reuses == before.constant_restored_reuses);
            CHECK(after.constant_restore_checks == before.constant_restore_checks);
        } else if (expected == Expect::Restored) {
            CHECK(after.constant_materializations == before.constant_materializations + (validate ? 1u : 0u));
            CHECK(after.constant_restore_checks == before.constant_restore_checks + (validate ? 1u : 0u));
            // The validation lane attributes successful reconstructed equality
            // to rebuilt reuse, not the shipping shortcut counter.
            CHECK(after.constant_restored_reuses == before.constant_restored_reuses + (validate ? 0u : 1u));
        } else {
            CHECK(after.constant_materializations == before.constant_materializations);
            CHECK(after.constant_identity_reuses == before.constant_identity_reuses + 1u);
            CHECK(after.constant_restore_checks == before.constant_restore_checks);
        }
    }
    void restore(FrameSegment& s, bool change_projection = true, bool change_fog = false) {
        if (change_projection) {
            auto other = p;
            other.p[0] = 0.75f;
            s.append(Op::Projection, other);
            s.append(Op::Projection, p);
        }
        if (change_fog) {
            auto other = f;
            other.rgba[0] ^= 0x55u;
            s.append(Op::Fog, other);
            s.append(Op::Fog, f);
        }
    }
};
void texture(FrameSegment& s, ResourceKey key) {
    std::array<u8, 4u * 4u * 4u> pixels{};
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<u8>(i * 17u);
    TextureContent content{};
    content.texture = key;
    content.width = content.height = 4u;
    content.gx_format = 6u;
    content.flags = kTextureNativeLinear;
    content.texels = s.add_payload(pixels.data(), pixels.size());
    s.append(Op::TextureContent, content);
}
void copy_target(FrameSegment& s, ResourceKey key, u16 size) {
    CopyTexture copy{};
    copy.target = key;
    copy.target_width = copy.target_height = size;
    copy.source_width = copy.source_height = size;
    s.append(Op::CopyTexture, copy);
}
void present(Fixture& x, u32 ordinal) {
    FrameSegment s;
    Present p{};
    p.frame = ordinal;
    p.xfb_width = 640u;
    p.xfb_height = p.efb_height = 480u;
    s.append(Op::Present, p);
    s.set_ends_frame(true);
    x.gpu.process(s);
}

bool run_suite(bool validate) {
    const char* name = validate ? "canonical validation suite" : "shipping suite";
    ObserverEnvironment observer(validate);
    Fixture x(validate);
    NativeGpuSettings settings{};
    settings.internal_scale = 1u;
    std::string error;
    if (!x.gpu.create(settings, error)) {
        std::printf("FAIL native GPU create after successful DX12 probe: %s\n", error.c_str());
        ++failures;
        return false;
    }
    {
        FrameSegment s;
        setup(s, x.p, x.f);
        x.step("initial materialization", s, Expect::Rebuild);
    }
    for (unsigned mask = 1u; mask <= 3u; ++mask) {
        FrameSegment s;
        x.restore(s, (mask & 1u) != 0u, (mask & 2u) != 0u);
        x.step(mask == 1u ? "projection A-B-A" : mask == 2u ? "fog A-B-A" : "both sources A-B-A", s, Expect::Restored);
    }
    {
        FrameSegment s;
        x.p.p[1] = -0.0f;
        s.append(Op::Projection, x.p);
        x.step("signed zero changed endpoint", s, Expect::Rebuild);
    }
    {
        FrameSegment s;
        auto positive_zero = x.p;
        positive_zero.p[1] = 0.0f;
        s.append(Op::Projection, positive_zero);
        s.append(Op::Projection, x.p);
        x.step("signed zero exact restoration", s, Expect::Restored);
    }
    {
        FrameSegment s;
        x.p.p[1] = float_bits(0x7fc12345u);
        s.append(Op::Projection, x.p);
        x.step("NaN payload first materialization", s, Expect::Rebuild);
    }
    {
        FrameSegment s;
        auto other_nan = x.p;
        other_nan.p[1] = float_bits(0x7fc12346u);
        s.append(Op::Projection, other_nan);
        s.append(Op::Projection, x.p);
        x.step("NaN payload exact restoration", s, Expect::Restored);
        s.clear();
        x.p = projection();
        s.append(Op::Projection, x.p);
        x.f = fog(2.0f);
        s.append(Op::Fog, x.f);
        x.step("changed final projection and fog", s, Expect::Rebuild);
    }
    {
        FrameSegment s;
        x.restore(s);
        s.append(Op::MaterialColor, ChannelColor{4u, {17u, 31u, 47u, 255u}});
        x.step("material mutation vetoes restoration", s, Expect::Rebuild);
        s.clear();
        x.restore(s);
        MatrixLoad matrix{};
        matrix.m = {1, 0, 0, 0.125f, 0, 1, 0, 0, 0, 0, 1, 0};
        s.append(Op::PositionMatrix, matrix);
        x.step("position mutation vetoes restoration", s, Expect::Rebuild);
        s.clear();
        x.restore(s);
        matrix.m[3] = 0.25f;
        s.append(Op::NormalMatrix, matrix);
        x.step("normal mutation vetoes restoration", s, Expect::Rebuild);
        s.clear();
        x.restore(s);
        s.append(Op::TextureMatrix, matrix);
        x.step("texture matrix mutation vetoes restoration", s, Expect::Rebuild);
        s.clear();
        x.restore(s);
        s.append(Op::CurrentMatrix, Scalar{3u, 0u});
        x.step("current matrix mutation vetoes restoration", s, Expect::Rebuild);
        s.clear();
        x.restore(s);
        s.append(Op::ChannelCount, Scalar{1u, 0u});
        s.append(Op::ChannelControl, ChannelControl{0u, 1u, 0u, 1u, 1u, 1u, 0u, 0u});
        x.step("channel mutation vetoes restoration", s, Expect::Rebuild);
        s.clear();
        x.restore(s);
        s.append(Op::AmbientColor, ChannelColor{4u, {11u, 23u, 37u, 255u}});
        x.step("ambient mutation vetoes restoration", s, Expect::Rebuild);
        s.clear();
        x.restore(s);
        s.append(Op::TevRegisterColor, TevColor{0u, {13, 29, 41, 255}});
        x.step("TEV mutation vetoes restoration", s, Expect::Rebuild);
        s.clear();
        x.restore(s);
        s.append(Op::TexGenCount, Scalar{1u, 0u});
        s.append(Op::TexGen, TexGen{0u, 1u, 4u, 0u, 30u, 125u});
        x.step("texgen mutation vetoes restoration", s, Expect::Rebuild);
        s.clear();
        x.restore(s);
        s.append(Op::AlphaCompare, AlphaCompare{6u, 32u, 0u, 7u, 0u, {0u, 0u, 0u}});
        x.step("alpha mutation vetoes restoration", s, Expect::Rebuild);
        s.clear();
        x.restore(s);
        x.program = MaterialProgram::Modulate;
        x.step("material program identity veto", s, Expect::Rebuild);
        s.clear();
        x.restore(s);
        x.vertex_attributes ^= kImmediateNormal;
        x.step("vertex flags identity veto", s, Expect::Rebuild);
    }
    BindTexture bind{};
    bind.texture = {0x1000u, 1u};
    bind.width = bind.height = 4u;
    {
        FrameSegment s;
        s.append(Op::BindTexture, bind);
        x.step("pending texture magenta", s, Expect::Rebuild);
        const auto missing = x.gpu.stats().missing_textures;
        s.clear();
        texture(s, bind.texture);
        x.restore(s);
        x.step("late texture arrival without rebind", s, Expect::Rebuild);
        CHECK(x.gpu.stats().missing_textures == missing);
        s.clear();
        texture(s, ResourceKey{bind.texture.address, 2u});
        x.restore(s);
        x.step("replacement generation without rebind", s, Expect::Rebuild);
        CHECK(x.gpu.stats().missing_textures == missing + 1u);
        s.clear();
        bind.texture.generation = 2u;
        s.append(Op::BindTexture, bind);
        x.step("binding replacement generation", s, Expect::Rebuild);
        s.clear();
        s.append(Op::ReleaseTexture, ResourceKey{bind.texture.address, 1u});
        x.restore(s);
        x.step("older release preserves current owner", s, Expect::Restored);
        s.clear();
        texture(s, bind.texture);
        x.restore(s);
        x.step("duplicate resident generation preserves owner", s, Expect::Restored);
        s.clear();
        s.append(Op::ReleaseTexture, bind.texture);
        x.restore(s);
        x.step("release bound generation without rebind", s, Expect::Rebuild);
        s.clear();
        texture(s, bind.texture);
        x.restore(s);
        x.step("same generation re-created after release", s, Expect::Rebuild);
    }
    {
        FrameSegment s;
        bind.texture = {0x2000u, 1u};
        bind.flags = kTextureCameraTarget;
        // Give release an actual texture owner as well as a camera target.
        texture(s, bind.texture);
        s.append(Op::BindTexture, bind);
        x.step("camera missing target white", s, Expect::Rebuild);
        s.clear();
        copy_target(s, bind.texture, 4u);
        x.restore(s);
        x.step("camera first copy without rebind", s, Expect::Rebuild);
        s.clear();
        copy_target(s, bind.texture, 8u);
        x.restore(s);
        x.step("camera dimensions changed at scale one", s, Expect::Rebuild);
        s.clear();
        s.append(Op::ReleaseTexture, bind.texture);
        x.restore(s);
        x.step("camera release to white fallback", s, Expect::Rebuild);
    }
    constexpr std::size_t lights = static_cast<std::size_t>(NativeGpu::RingUploadOwner::LightConstants);
    Light light{};
    light.mask = 1u;
    light.rgba = {255u, 127u, 63u, 255u};
    light.direction[2] = -1.0f;
    {
        FrameSegment s;
        auto previous = x.gpu.stats().ring_owner_allocations[lights];
        s.append(Op::Light, light);
        x.step("light owner changes while draw constants reuse", s, Expect::Identity);
        CHECK(x.gpu.stats().ring_owner_allocations[lights] == previous + 1u);
        s.clear();
        previous = x.gpu.stats().ring_owner_allocations[lights];
        const auto resets = x.gpu.stats().state_resets;
        s.append_empty(Op::ResetState);
        setup(s, x.p, x.f);
        s.append(Op::Light, light);
        x.program = MaterialProgram::PassColor;
        x.vertex_attributes = attributes;
        x.step("reset invalidates constant and light owners", s, Expect::Rebuild);
        CHECK(x.gpu.stats().state_resets == resets + 1u);
        CHECK(x.gpu.stats().ring_owner_allocations[lights] == previous + 1u);
    }
    for (u32 ordinal = 1u; ordinal <= 4u; ++ordinal) {
        present(x, ordinal);
        FrameSegment s;
        x.restore(s, true, true);
        x.step("new command list cannot reuse retired upload", s, Expect::Rebuild);
    }
    x.gpu.finish();
    const auto stats = x.gpu.stats();
    CHECK(stats.last_error.empty());
    CHECK(stats.constant_restore_mismatches == 0u);
    CHECK(stats.skipped_draws == 0u);
    CHECK(stats.unsupported_program_draws == 0u);
    CHECK(stats.texture_decode_failures == 0u);
    CHECK(stats.target_allocation_failures == 0u);
    CHECK(stats.ring_allocation_failures == 0u);
    CHECK(validate ? stats.constant_restore_checks >= 5u : stats.constant_restored_reuses >= 5u);
    std::printf("%s: draws=%llu materializations=%llu restored=%llu checks=%llu mismatches=%llu\n", name,
                static_cast<unsigned long long>(stats.draws),
                static_cast<unsigned long long>(stats.constant_materializations),
                static_cast<unsigned long long>(stats.constant_restored_reuses),
                static_cast<unsigned long long>(stats.constant_restore_checks),
                static_cast<unsigned long long>(stats.constant_restore_mismatches));
    return true;
}
} // namespace

int main() {
    if (!NativeGpu::available()) {
        std::printf("SKIP: native DX12 backend absent\n");
        return 77;
    }
    {
        plume::D3D12Interface api;
        if (!api.isValid() || api.createDevice("") == nullptr) {
            std::printf("SKIP: no actual DX12 device\n");
            return 77;
        }
    }
    run_suite(false);
    run_suite(true);
    std::printf("native GPU constant tests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
