// Native tests of the RenderWare materials, material lists, texture stream reads and string chunks for what the oracle
// cannot cover - the deliberate differences (rwcore/README.md "Native contracts" -> Materials, Rasters and textures):
// a material struct longer than 28 bytes and a texture struct longer than 4 read into their size and the rest
// skipped (the stream stays in step), names longer than 127 characters cut and skipped, a failing material plugin
// read destroying the material and releasing its texture, a failing texture plugin read dropping the reference, a
// failed string read freeing the buffer it allocated, Unicode string chunks narrowed as RenderWare does on a PC, a
// failing material plugin constructor, no material module; plus material lists growing in steps of 20 and releasing
// their materials. Memory is counted: every part ends with no live allocation.
//
// Usage: rpworld_material_native_tests
#include "rw/core/binary.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/memory.h"
#include "rw/core/stream.h"
#include "rw/core/texture.h"
#include "rw/world/material.h"
#include "rw/world/world.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::map<void*, std::size_t> g_live;
void* countMalloc(std::size_t size, RwUInt32) {
    void* p = std::malloc(size ? size : 1);
    g_live[p] = size;
    return p;
}
void countFree(void* p) {
    if (p == nullptr) return;
    require(g_live.erase(p) == 1, "free of a pointer the allocator did not hand out");
    std::free(p);
}
void* countRealloc(void* p, std::size_t size, RwUInt32 hint) {
    if (p == nullptr) return countMalloc(size, hint);
    void* q = std::realloc(p, size);
    if (q != nullptr) g_live.erase(p), g_live[q] = size;
    return q;
}
void* countCalloc(std::size_t n, std::size_t size, RwUInt32 hint) {
    void* p = countMalloc(n * size, hint);
    if (p != nullptr) std::memset(p, 0, n * size);
    return p;
}
const RwMemoryFunctions kCounting{countMalloc, countFree, countRealloc, countCalloc};
RwError takeError() {
    RwError e{};
    RwErrorGet(&e);
    return e;
}

bool g_fail_material_read = false, g_fail_texture_read = false;
int g_ctor = 0, g_fail_ctor = -1, g_dtor = 0;
void* ctor(void* o, RwInt32, RwInt32) { return g_ctor++ == g_fail_ctor ? nullptr : o; }
void* dtor(void* o, RwInt32, RwInt32) { ++g_dtor; return o; }
RwStream* failingRead(RwStream* stream, RwInt32 length, void*, RwInt32, RwInt32) {
    if (g_fail_material_read) return nullptr;
    return RwStreamSkip(stream, static_cast<RwUInt32>(length));
}
RwStream* failingTextureRead(RwStream* stream, RwInt32 length, void*, RwInt32, RwInt32) {
    if (g_fail_texture_read) return nullptr;
    return RwStreamSkip(stream, static_cast<RwUInt32>(length));
}

void startEngine() {
    RwEngineOpenParams params{nullptr};
    require(RwEngineInit(&kCounting, 0, 0) == 1 && RpWorldPluginAttach() == 1, "init");
    require(RpMaterialRegisterPlugin(4, 0x7401, ctor, dtor, nullptr) >= 0 &&
                RpMaterialRegisterPluginStream(0x7401, failingRead, nullptr, nullptr) >= 0 &&
                RwTextureRegisterPlugin(4, 0x7402, nullptr, nullptr, nullptr) >= 0 &&
                RwTextureRegisterPluginStream(0x7402, failingTextureRead, nullptr, nullptr) >= 0,
            "plugins");
    require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
}
void stopEngine(const char* what) {
    require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1, std::string(what) + ": shutdown");
    require(g_live.empty(), std::string(what) + ": " + std::to_string(g_live.size()) + " allocations leaked");
}
unsigned g_checks = 0;
void check(bool c, const std::string& what) {
    ++g_checks;
    require(c, what);
}

struct Bytes {
    std::vector<unsigned char> b;
    void put(RwUInt32 w) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>(w >> (8 * i))); }
};
constexpr RwUInt32 libraryID(RwUInt32 v) { return (((v - 0x30000u) << 14) & 0xFFC00000u) | ((v & 0x3Fu) << 16) | 0x37u; }
constexpr RwUInt32 kV = 0x36003u;
void string(Bytes& s, const std::string& text) {
    const RwUInt32 length = (static_cast<RwUInt32>(text.size()) + 4u) & ~3u;
    s.put(rwID_STRING), s.put(length), s.put(libraryID(kV));
    for (RwUInt32 i = 0; i < length; ++i) s.b.push_back(i < text.size() ? static_cast<unsigned char>(text[i]) : 0);
}
void texture(Bytes& s, RwUInt32 structLength, const std::string& name, RwUInt32 pluginBytes) {
    s.put(rwID_TEXTURE), s.put(0), s.put(libraryID(kV));
    s.put(rwID_STRUCT), s.put(structLength), s.put(libraryID(kV));
    for (RwUInt32 i = 0; i < structLength / 4; ++i) s.put(i == 0 ? 0x00001102u : 0xEEEEEEEEu);
    string(s, name), string(s, "");
    s.put(rwID_EXTENSION), s.put(pluginBytes ? 12 + pluginBytes : 0), s.put(libraryID(kV));
    if (pluginBytes) {
        s.put(0x7402), s.put(pluginBytes), s.put(libraryID(kV));
        for (RwUInt32 i = 0; i < pluginBytes; ++i) s.b.push_back(0);
    }
}
void material(Bytes& s, RwUInt32 structLength, bool textured, const std::string& name, RwUInt32 pluginBytes) {
    s.put(rwID_STRUCT), s.put(structLength), s.put(libraryID(kV));
    const RwUInt32 words[7] = {0, 0x80402010u, 0, textured ? 1u : 0u, std::bit_cast<RwUInt32>(0.5f), std::bit_cast<RwUInt32>(0.25f),
                               std::bit_cast<RwUInt32>(0.75f)};
    for (RwUInt32 i = 0; i < structLength / 4; ++i) s.put(i < 7 ? words[i] : 0xEEEEEEEEu);
    if (textured) texture(s, 4, name, 0);
    s.put(rwID_EXTENSION), s.put(pluginBytes ? 12 + pluginBytes : 0), s.put(libraryID(kV));
    if (pluginBytes) {
        s.put(0x7401), s.put(pluginBytes), s.put(libraryID(kV));
        for (RwUInt32 i = 0; i < pluginBytes; ++i) s.b.push_back(0);
    }
}
template <class F> auto withStream(Bytes& s, F f) {
    s.put(0x4D41524Bu);   // a marker the reader must stop before
    RwMemory memory{s.b.data(), s.b.size()};
    RwStream* st = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
    auto r = f(st);
    RwUInt32 marker = 0;
    const bool inStep = RwStreamRead(st, &marker, 4) == 4 && marker == 0x4D41524Bu;
    RwStreamClose(st, nullptr);
    return std::make_pair(r, inStep);
}

}  // namespace

int main() {
    try {
        startEngine();
        RwTexture* wall = RwTextureCreate(nullptr);
        RwTextureSetName(wall, "wall");
        RwTexDictionaryAddTexture(RwTexDictionaryGetCurrent(), wall);
        {   // a long material struct and a long texture struct: read into their size, the rest skipped
            Bytes s;
            s.put(rwID_STRUCT), s.put(36), s.put(libraryID(kV));
            const RwUInt32 words[9] = {0, 0x80402010u, 0, 1, std::bit_cast<RwUInt32>(0.5f), std::bit_cast<RwUInt32>(0.25f),
                                       std::bit_cast<RwUInt32>(0.75f), 0xEEEEEEEEu, 0xEEEEEEEEu};
            for (RwUInt32 w : words) s.put(w);
            texture(s, 12, "WALL", 0);
            s.put(rwID_EXTENSION), s.put(0), s.put(libraryID(kV));
            auto [m, inStep] = withStream(s, RpMaterialStreamRead);
            check(m != nullptr && inStep && m->texture == wall && m->surfaceProps.ambient == 0.5f && m->surfaceProps.diffuse == 0.75f &&
                      m->color.red == 0x10 && m->color.alpha == 0x80 && wall->refCount == 2,
                  "long structs are read into their size and skipped; the stream stays in step");
            RpMaterialDestroy(m);
            check(wall->refCount == 1, "the material's destroy released its texture");
        }
        {   // a name longer than 127 characters: cut and skipped
            Bytes s;
            texture(s, 4, std::string(200, 'w'), 0);
            s.b.erase(s.b.begin(), s.b.begin() + 12);
            takeError();
            auto [t, inStep] = withStream(s, RwTextureStreamRead);
            check(t == nullptr && inStep && takeError().errorCode == 0x16, "a 200-character name is cut, the stream in step");
        }
        {   // a failing material plugin read: the material is destroyed and its texture released
            Bytes s;
            material(s, 28, true, "wall", 4);
            g_fail_material_read = true;
            const int d = g_dtor;
            auto [m, inStep] = withStream(s, RpMaterialStreamRead);
            g_fail_material_read = false;
            (void)inStep;
            check(m == nullptr && g_dtor == d + 1 && wall->refCount == 1, "a failed plugin read destroys the material");
        }
        {   // a found texture whose plugin data cannot be skipped (an extension longer than the stream): the reference
            // RwTextureRead took is dropped
            Bytes s;
            texture(s, 4, "wall", 0);
            s.b.erase(s.b.begin(), s.b.begin() + 12);
            const std::size_t extensionLength = s.b.size() - 8;   // the extension header's length word
            s.b[extensionLength] = 0xE8, s.b[extensionLength + 1] = 0x03;   // 1000 bytes
            RwMemory memory{s.b.data(), s.b.size()};
            RwStream* st = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
            RwTexture* t = RwTextureStreamRead(st);
            RwStreamClose(st, nullptr);
            check(t == nullptr && wall->refCount == 1, "a failed plugin skip drops the reference RwTextureRead took");
        }
        {   // strings: Unicode narrowed as on a PC; a failed read frees the buffer it allocated
            Bytes s;
            const char16_t text[] = u"Westopolis";
            s.put(rwID_UNICODESTRING), s.put(sizeof text), s.put(libraryID(kV));
            for (char16_t c : text) s.b.push_back(static_cast<unsigned char>(c)), s.b.push_back(static_cast<unsigned char>(c >> 8));
            auto [str, inStep] = withStream(s, [](RwStream* st) { return _rwStringStreamFindAndRead(nullptr, st); });
            check(str != nullptr && inStep && std::strcmp(str, "Westopolis") == 0, "a Unicode string reads as its characters");
            RwFree(str);
            Bytes t;
            t.put(rwID_STRING), t.put(64), t.put(libraryID(kV));
            for (int i = 0; i < 10; ++i) t.b.push_back('x');   // 64 promised, 10 present
            RwMemory memory{t.b.data(), t.b.size()};
            RwStream* st = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
            check(_rwStringStreamFindAndRead(nullptr, st) == nullptr, "a truncated string fails");
            RwStreamClose(st, nullptr);
            RwChar small[8];
            Bytes u;
            string(u, "a long texture name");
            RwMemory memory2{u.b.data(), u.b.size()};
            st = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory2);
            check(_rwStringStreamFindAndReadCapped(small, sizeof small, st) == small && std::strcmp(small, "a long ") == 0,
                  "the capped reader keeps 7 characters and a NUL");
            RwStreamClose(st, nullptr);
        }
        {   // material lists: growth in steps of 20, references, release
            RpMaterialList list;
            _rpMaterialListInitialize(&list);
            RpMaterial* m = RpMaterialCreate();
            for (int i = 0; i < 45; ++i) check(_rpMaterialListAppendMaterial(&list, m) == i, "append index");
            check(list.space == 60 && list.numMaterials == 45 && m->refCount == 46, "space grows by 20; each entry a reference");
            check(_rpMaterialListFindMaterialIndex(&list, m) == 44, "the last index");
            _rpMaterialListDeinitialize(&list);
            check(list.materials == nullptr && list.numMaterials == 0 && list.space == 0 && m->refCount == 1, "deinitialise releases");
            RpMaterialDestroy(m);
        }
        {   // a failing constructor
            g_fail_ctor = g_ctor;
            check(RpMaterialCreate() == nullptr, "a failing plugin constructor fails RpMaterialCreate");
            g_fail_ctor = -1;
        }
        RwTextureDestroy(wall);
        stopEngine("materials");
        {
            require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
            check(RpMaterialCreate() == nullptr && takeError().errorCode == static_cast<RwInt32>(0x80000001u), "no material module");
            require(RwEngineTerm() == 1 && g_live.empty(), "term");
        }
        std::printf("rpworld_material_native_tests: %u checks passed (no allocation outlives a shutdown)\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rpworld_material_native_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
