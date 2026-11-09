// The RenderWare world: attach, modules, stream reading, sectors, destruction, world lights (see rw/world/world.h; the
// reading of the original is .build/dolrecon/world_read_spec.md).
#include "rw/world/world.h"

#include <bit>
#include <cstring>
#include <vector>

#include "rw/core/binary.h"
#include "rw/core/error.h"
#include "rw/core/memory.h"

RwModuleInfo worldModule{}, meshModule{};

namespace {

constexpr RwUInt32 rwID_WORLDPLUGIN = 0x02;
constexpr RwUInt32 kWorldHint = 0x00030000u | rwID_WORLD;       // rwMEMHINTDUR_EVENT | rwID_WORLD
constexpr RwUInt32 kMeshHint = 0x00030000u | rwID_MESHMODULE;
constexpr RwUInt32 kRegistryHint = 0x00040000u | rwID_WORLDMODULE;
constexpr RwUInt32 kTieHint = 0x00030000u | rwID_WORLDMODULE;
constexpr RwInt32 E_RW_BADVERSION = static_cast<RwInt32>(0x80000004u);
constexpr RwInt32 E_RW_NULLP = static_cast<RwInt32>(0x80000016u);
RwPluginRegistry worldTKList = rwPLUGINREGISTRYINIT(sizeof(RpWorld));          // 0x8056E958
RwPluginRegistry sectorTKList = rwPLUGINREGISTRYINIT(sizeof(RpWorldSector));   // 0x8056E940
RwModuleInfo module50A{}, module50B{};   // the plugin's two counting modules (0x50A: r13 + 0x60A0, 0x50B: r13 + 0x6058)
RwInt32 g_lightExtOffset = -1;
RwFreeList worldRegistrySpace{};

struct RegistryEntry {   // the original's 0x10-byte entry, native pointers and sizes
    RpWorld* world;
    std::size_t size;
    RwLLLink link;
};

rpWorldGlobals& wg() { return rwModuleGlobals<rpWorldGlobals>(worldModule); }
rpMeshGlobals& mg() { return rwModuleGlobals<rpMeshGlobals>(meshModule); }

void error(RwInt32 code) {
    RwError e{rwID_WORLDPLUGIN, _rwerror(code)};
    RwErrorSet(&e);
}
void errorSize(RwInt32 code, std::size_t size) {
    RwError e{rwID_WORLDPLUGIN, _rwerror(code, size)};
    RwErrorSet(&e);
}
bool versionOk(RwUInt32 v) { return v >= rwLIBRARYBASEVERSION && v <= rwLIBRARYCURRENTVERSION; }

void listInit(RwLinkList& l) { l.link.next = l.link.prev = &l.link; }
void linkAtHead(RwLinkList& l, RwLLLink& k) {
    k.next = l.link.next;
    k.prev = &l.link;
    l.link.next->prev = &k;
    l.link.next = &k;
}
void unlink(RwLLLink& k) {
    k.prev->next = k.next;
    k.next->prev = k.prev;
}
template <class T> T* containerOf(RwLLLink* link, std::size_t offset) {
    return reinterpret_cast<T*>(reinterpret_cast<char*>(link) - offset);
}

std::size_t align(std::size_t n, std::size_t a) { return (n + a - 1) & ~(a - 1); }

/// The one block of a world read from a stream, handed out front to back (native: every piece aligned, and the end
/// checked - the original trusts the struct's totals).
struct Block {
    RwUInt8* at;
    RwUInt8* end;
    void* take(std::size_t bytes, std::size_t alignment) {
        RwUInt8* p = reinterpret_cast<RwUInt8*>(align(reinterpret_cast<std::uintptr_t>(at), alignment));
        if (p > end || static_cast<std::size_t>(end - p) < bytes) return nullptr;
        at = p + bytes;
        return p;
    }
};

rpWorldLightExt* ext(RpLight* light) {
    return reinterpret_cast<rpWorldLightExt*>(reinterpret_cast<char*>(light) + g_lightExtOffset);
}

void freeLightTies(RpLight* light) {
    RwLinkList& list = light->WorldSectorsInLight;
    for (RwLLLink* l = list.link.next; l != &list.link;) {
        RwLLLink* next = l->next;
        auto* tie = containerOf<rpLightTie>(l, offsetof(rpLightTie, WorldSectorInLight));
        unlink(tie->WorldSectorInLight);
        unlink(tie->lightInWorldSector);
        rwSrcGlobals().memoryFree(wg().lightTies, tie);
        l = next;
    }
}

// ---- sectors ------------------------------------------------------------------------------------------------------

/// Every atomic sector, left before right, iteratively (native: any depth; the original's walks use 64-entry stacks).
template <class F> void forEachAtomic(RpSector* root, F f) {
    std::vector<RpSector*> stack;
    RpSector* s = root;
    while (s != nullptr) {
        RpSector* next = nullptr;
        if (s->type < 0) {
            if (s->type == rwSECTORATOMIC && !f(reinterpret_cast<RpWorldSector*>(s))) return;
        } else {
            auto* p = reinterpret_cast<RpPlaneSector*>(s);
            if (p->rightSubTree != nullptr) stack.push_back(p->rightSubTree);
            next = p->leftSubTree;
        }
        if (next == nullptr) {
            if (stack.empty()) return;
            next = stack.back();
            stack.pop_back();
        }
        s = next;
    }
}

/// SectorDestroySingleMalloc (0x80460730): ties, plugin data and meshes of every atomic sector of a tree in the block.
void destroySectorsInBlock(RpSector* root) {
    forEachAtomic(root, [](RpWorldSector* ws) {
        for (RwLLLink* l = ws->lightsInWorldSector.link.next; l != &ws->lightsInWorldSector.link;) {
            RwLLLink* next = l->next;
            auto* tie = containerOf<rpLightTie>(l, offsetof(rpLightTie, lightInWorldSector));
            unlink(tie->WorldSectorInLight);
            unlink(tie->lightInWorldSector);
            rwSrcGlobals().memoryFree(wg().lightTies, tie);
            l = next;
        }
        _rwPluginRegistryDeInitObject(&sectorTKList, ws);
        if (ws->mesh != nullptr) {
            _rpMeshDestroy(ws->mesh);
            ws->mesh = nullptr;
        }
        return true;
    });
}

/// AtomicSectorRead (0x80455D34).
RpWorldSector* readAtomicSector(RwStream* stream, Block& block, RpWorld* world, RwUInt32 format) {
    RwUInt32 length = 0, version = 0;
    if (!RwStreamFindChunk(stream, rwID_STRUCT, &length, &version)) return nullptr;
    if (!versionOk(version)) return error(E_RW_BADVERSION), nullptr;
    // {matListWindowBase, numTriangles, numVertices, inf, sup, collSectorPresent, unused}: 0x2C bytes
    RwUInt32 sc[11] = {};
    // the struct chunk holds the header and then the arrays (the original reads on without looking at its length)
    if (length < sizeof sc) return nullptr;   // native: a chunk too short for its header fails
    if (RwStreamRead(stream, sc, sizeof sc) != sizeof sc) return nullptr;
    RwMemNative32(sc, sizeof sc);
    if (sc[1] > 0xFFFFu || sc[2] > 0xFFFFu || sc[0] > 0xFFFFu) return nullptr;   // native: the u16 fields hold them
    auto* sec = static_cast<RpWorldSector*>(block.take(sectorTKList.sizeOfStruct, 16));
    if (sec == nullptr) return errorSize(E_RW_NOMEM, sectorTKList.sizeOfStruct), nullptr;
    sec->type = rwSECTORATOMIC;
    std::memcpy(&sec->tightBoundingBox.inf, &sc[3], 12);
    std::memcpy(&sec->tightBoundingBox.sup, &sc[6], 12);
    sec->matListWindowBase = static_cast<RwUInt16>(sc[0]);
    sec->numTriangles = static_cast<RwUInt16>(sc[1]);
    sec->numVertices = static_cast<RwUInt16>(sc[2]);
    listInit(sec->collAtomicsInWorldSector);
    listInit(sec->lightsInWorldSector);
    bool ok = true;
    if ((world->flags & rpWORLDNATIVE) == 0) {
        const std::size_t nv = sc[2], nt = sc[1];
        if (nv != 0) {
            sec->vertices = static_cast<RwV3d*>(block.take(nv * 12, 8));
            ok = sec->vertices != nullptr &&
                 RwStreamReadReal(stream, &sec->vertices->x, static_cast<RwUInt32>(nv * 12)) != nullptr;
            if (ok && (format & rpWORLDNORMALS) != 0) {
                sec->normals = static_cast<RpVertexNormal*>(block.take(nv * 4, 4));
                ok = sec->normals != nullptr && RwStreamRead(stream, sec->normals, static_cast<RwUInt32>(nv * 4)) == nv * 4;
            }
            if (ok && (format & rpWORLDPRELIT) != 0) {
                sec->preLitLum = static_cast<RwRGBA*>(block.take(nv * 4, 4));
                // native: the whole array (the original accepts any non-empty read)
                ok = sec->preLitLum != nullptr &&
                     RwStreamRead(stream, sec->preLitLum, static_cast<RwUInt32>(nv * 4)) == nv * 4;
            }
            for (RwInt32 i = 0; ok && i < world->numTexCoordSets; ++i) {
                sec->texCoords[i] = static_cast<RwTexCoords*>(block.take(nv * 8, 8));
                ok = sec->texCoords[i] != nullptr &&
                     RwStreamReadReal(stream, &sec->texCoords[i]->u, static_cast<RwUInt32>(nv * 8)) != nullptr;
            }
        }
        if (ok && nt != 0) {
            sec->triangles = static_cast<RpTriangle*>(block.take(nt * 8, 8));
            ok = sec->triangles != nullptr &&
                 RwStreamReadInt16(stream, reinterpret_cast<RwInt16*>(sec->triangles), static_cast<RwUInt32>(nt * 8)) != nullptr;
            if (ok && version < 0x36002u) {   // before 3.6.0.2: {matIndex, v0, v1, v2}
                for (std::size_t i = 0; i < nt; ++i) {
                    RwUInt16* t = &sec->triangles[i].vertIndex[0];
                    const RwUInt16 a = t[0], b = t[1], c = t[2], d = t[3];
                    t[0] = b, t[1] = c, t[2] = d, t[3] = a;
                }
            }
            for (std::size_t i = 0; ok && i < nt; ++i) {   // native: indices inside the sector
                const RpTriangle& t = sec->triangles[i];
                ok = t.vertIndex[0] < nv && t.vertIndex[1] < nv && t.vertIndex[2] < nv;
            }
        }
    }
    if (!ok) return nullptr;   // nothing of the sector's own to undo: its arrays are the block's, no plugin data yet
    {   // native: the arrays must fit the struct chunk, and what the chunk holds past them is skipped
        std::size_t used = sizeof sc;
        if ((world->flags & rpWORLDNATIVE) == 0) {
            const std::size_t nv = sc[2];
            used += nv * 12 + static_cast<std::size_t>(sc[1]) * 8 + static_cast<std::size_t>(world->numTexCoordSets) * nv * 8;
            if (nv != 0 && (format & rpWORLDNORMALS) != 0) used += nv * 4;
            if (nv != 0 && (format & rpWORLDPRELIT) != 0) used += nv * 4;
            if (nv == 0) used = sizeof sc + static_cast<std::size_t>(sc[1]) * 8;
        }
        if (used > length) return nullptr;
        if (used < length && RwStreamSkip(stream, static_cast<RwUInt32>(length - used)) == nullptr) return nullptr;
    }
    if (_rwPluginRegistryInitObject(&sectorTKList, sec) == nullptr) return nullptr;
    if (_rwPluginRegistryReadDataChunks(&sectorTKList, stream, sec) == nullptr) {
        destroySectorsInBlock(reinterpret_cast<RpSector*>(sec));
        return nullptr;
    }
    return sec;
}

/// PlaneSectorRead (0x804562FC), iteratively: a plane's struct, then its left and right subtrees in stream order.
RpSector* readSectorTree(RwStream* stream, Block& block, RpWorld* world, RwUInt32 format, bool rootIsAtomic) {
    const auto readPlane = [&](RpPlaneSector** out, RwUInt32 flags[2]) -> bool {
        RwUInt32 length = 0, version = 0;
        if (!RwStreamFindChunk(stream, rwID_STRUCT, &length, &version)) return false;
        if (!versionOk(version)) return error(E_RW_BADVERSION), false;
        RwUInt32 pc[6] = {};   // {type, value, leftIsWorldSector, rightIsWorldSector, leftValue, rightValue}
        const RwUInt32 take = length < sizeof pc ? length : static_cast<RwUInt32>(sizeof pc);
        if (RwStreamRead(stream, pc, take) != take) return false;
        if (length > take && RwStreamSkip(stream, length - take) == nullptr) return false;   // native: the rest skipped
        RwMemNative32(pc, sizeof pc);
        if (pc[0] != 0 && pc[0] != 4 && pc[0] != 8) return false;   // native: an axis of RwV3d
        auto* p = static_cast<RpPlaneSector*>(block.take(sizeof(RpPlaneSector), 8));
        if (p == nullptr) return errorSize(E_RW_NOMEM, sizeof(RpPlaneSector)), false;
        p->type = static_cast<RwInt32>(pc[0]);
        p->value = std::bit_cast<RwReal>(pc[1]);
        if ((format & rpWORLDSECTORSOVERLAP) != 0) {
            p->leftValue = std::bit_cast<RwReal>(pc[4]);
            p->rightValue = std::bit_cast<RwReal>(pc[5]);
        } else {
            p->leftValue = p->value;
            p->rightValue = p->value;
        }
        flags[0] = pc[2], flags[1] = pc[3];
        *out = p;
        return true;
    };
    // a child: find its chunk (atomic 9 / plane 0xA)
    const auto findChild = [&](bool atomic) -> bool {
        RwUInt32 version = 0;
        if (!RwStreamFindChunk(stream, atomic ? rwID_ATOMICSECT : rwID_PLANESECT, nullptr, &version)) return false;
        if (!versionOk(version)) return error(E_RW_BADVERSION), false;
        return true;
    };
    if (rootIsAtomic) {
        if (!findChild(true)) return nullptr;
        return reinterpret_cast<RpSector*>(readAtomicSector(stream, block, world, format));
    }
    if (!findChild(false)) return nullptr;
    struct Pending {
        RpPlaneSector* plane;
        RwUInt32 isAtomic[2];
        int next;   // 0: left to read, 1: right to read, 2: done
    };
    std::vector<Pending> stack;
    RpPlaneSector* root = nullptr;
    {
        Pending first{};
        if (!readPlane(&first.plane, first.isAtomic)) return nullptr;
        root = first.plane;
        stack.push_back(first);
    }
    bool ok = true;
    while (ok && !stack.empty()) {
        Pending& top = stack.back();
        if (top.next == 2) {
            stack.pop_back();
            continue;
        }
        const int side = top.next++;
        const bool atomic = top.isAtomic[side] != 0;
        RpPlaneSector* parent = top.plane;
        if (!findChild(atomic)) {
            ok = false;
            break;
        }
        RpSector*& slot = side == 0 ? parent->leftSubTree : parent->rightSubTree;
        if (atomic) {
            slot = reinterpret_cast<RpSector*>(readAtomicSector(stream, block, world, format));
            ok = slot != nullptr;
        } else {
            Pending child{};
            ok = readPlane(&child.plane, child.isAtomic);
            if (ok) {
                slot = reinterpret_cast<RpSector*>(child.plane);
                stack.push_back(child);
            }
        }
    }
    if (!ok) {
        // what was read before the failure, in the original's unwinding order (0x80456700 .. 0x80456714): innermost
        // plane first, each destroying its left subtree when the failure came from its right one (a failed child has
        // destroyed itself)
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            if (it->next == 2 && it->plane->leftSubTree != nullptr) {
                destroySectorsInBlock(it->plane->leftSubTree);
                it->plane->leftSubTree = nullptr;
            }
        }
        return nullptr;
    }
    return reinterpret_cast<RpSector*>(root);
}

/// _rpWorldSetupSectorBoundingBoxes (0x80460BF4): each atomic sector's box from the planes above it.
void setupSectorBoundingBoxes(RpWorld* world) {
    struct Item {
        RpSector* s;
        RwBBox box;
    };
    std::vector<Item> stack{{world->rootSector, world->boundingBox}};
    while (!stack.empty()) {
        Item it = stack.back();
        stack.pop_back();
        if (it.s == nullptr) continue;
        if (it.s->type < 0) {
            reinterpret_cast<RpWorldSector*>(it.s)->boundingBox = it.box;
            continue;
        }
        const auto* p = reinterpret_cast<const RpPlaneSector*>(it.s);
        const int axis = p->type / 4;
        Item right{p->rightSubTree, it.box}, left{p->leftSubTree, it.box};
        (&right.box.inf.x)[axis] = p->rightValue;
        (&left.box.sup.x)[axis] = p->leftValue;
        stack.push_back(right);   // left is visited first, as the original's walk
        stack.push_back(left);
    }
}

void registerWorld(RpWorld* world, std::size_t size) {
    auto* e = static_cast<RegistryEntry*>(rwSrcGlobals().memoryAlloc(wg().registryEntries, kRegistryHint));
    if (e == nullptr) return;   // as the original: unregistered (its sectors' world is then not found)
    e->world = world;
    e->size = size;
    linkAtHead(wg().worlds, e->link);
}

void unregisterWorld(RpWorld* world) {
    RwLinkList& list = wg().worlds;
    for (RwLLLink* l = list.link.next; l != &list.link; l = l->next) {
        auto* e = containerOf<RegistryEntry>(l, offsetof(RegistryEntry, link));
        if (e->world == world) {
            unlink(e->link);
            rwSrcGlobals().memoryFree(wg().registryEntries, e);
            return;
        }
    }
}

/// RpWorldDestroy's body; pluginsInitialised false on a failed read (native: the original runs the world's plugin
/// destructors on data never constructed).
void destroyWorld(RpWorld* world, bool pluginsInitialised) {
    for (RwLinkList* lights : {&world->lightList, &world->directionalLightList}) {   // native: no dangling lights
        while (lights->link.next != nullptr && lights->link.next != &lights->link) {
            RpLight* l = containerOf<RpLight>(lights->link.next, offsetof(RpLight, inWorld));
            RpWorldRemoveLight(world, l);
        }
    }
    unregisterWorld(world);
    // the original's order (0x80460FF0): every sector's mesh (0x804610A0), the material list (0x804610D8), the sectors
    // (0x804610F4: their plugin destructors see no mesh), the world's plugin data (0x80461104)
    if (world->rootSector != nullptr) {
        forEachAtomic(world->rootSector, [](RpWorldSector* s) {
            if (s->mesh != nullptr) _rpMeshDestroy(s->mesh), s->mesh = nullptr;
            return true;
        });
    }
    _rpMaterialListDeinitialize(&world->matList);
    if (world->rootSector != nullptr) destroySectorsInBlock(world->rootSector);
    if (pluginsInitialised) _rwPluginRegistryDeInitObject(&worldTKList, world);
    RwFree(world);
}

// ---- the plugin's stream callbacks and extensions ---------------------------------------------------------------

RwStream* readSectorMesh(RwStream* stream, RwInt32, void* object, RwInt32, RwInt32) {
    auto* sector = static_cast<RpWorldSector*>(object);
    RpWorld* world = RpWorldSectorGetWorld(sector);
    if (world == nullptr) return nullptr;   // native: the original dereferences NULL when the world was not registered
    if (sector->mesh != nullptr) _rpMeshDestroy(sector->mesh);   // native: the original overwrites it
    sector->mesh = _rpMeshRead(stream, world, &world->matList);
    return sector->mesh != nullptr ? stream : nullptr;
}

RwObjectHasFrame* WorldLightSync(RwObjectHasFrame* object);

void* WorldInitLightExt(void* object, RwInt32, RwInt32) {
    auto* light = static_cast<RpLight*>(object);
    ext(light)->world = nullptr;
    ext(light)->oldSync = light->object.sync;
    light->object.sync = WorldLightSync;
    return object;
}

void* WorldDeInitLightExt(void* object, RwInt32, RwInt32) {
    auto* light = static_cast<RpLight*>(object);
    if (ext(light)->world != nullptr) {
        RpWorldRemoveLight(ext(light)->world, light);   // native: the original leaves it linked into the world
    } else {
        freeLightTies(light);
    }
    return object;
}

void* WorldCopyLightExt(void* dst, const void* src, RwInt32, RwInt32) {
    auto* from = static_cast<RpLight*>(const_cast<void*>(src));
    if (ext(from)->world != nullptr) RpWorldAddLight(ext(from)->world, static_cast<RpLight*>(dst));
    return dst;
}

/// WorldLightSync (0x804622F8): after the light's own sync, a positional light is tied to every atomic sector its
/// radius box reaches (strictly past the planes' left / right values).
RwObjectHasFrame* WorldLightSync(RwObjectHasFrame* object) {
    auto* light = reinterpret_cast<RpLight*>(object);
    if (ext(light)->oldSync(object) == nullptr) {
        error(E_RW_NULLP);
        return object;
    }
    if (light->object.object.subType < rpLIGHTPOINT) return object;
    RpWorld* world = ext(light)->world;
    RwFrame* frame = static_cast<RwFrame*>(light->object.object.parent);
    if (world == nullptr || frame == nullptr) return object;
    const RwReal r = light->radius;
    freeLightTies(light);
    const RwV3d p = RwFrameGetLTM(frame)->pos;
    RwBBox bb;
    bb.sup = {p.x + r, p.y + r, p.z + r};
    bb.inf = {p.x - r, p.y - r, p.z - r};
    std::vector<RpSector*> stack;
    RpSector* s = world->rootSector;
    while (s != nullptr) {
        RpSector* next = nullptr;
        if (s->type < 0) {
            auto* sec = reinterpret_cast<RpWorldSector*>(s);
            auto* tie = static_cast<rpLightTie*>(rwSrcGlobals().memoryAlloc(wg().lightTies, kTieHint));
            if (tie == nullptr) {   // native: the original does not check
                errorSize(E_RW_NOMEM, sizeof(rpLightTie));
                return object;
            }
            tie->sect = sec;
            tie->light = light;
            linkAtHead(sec->lightsInWorldSector, tie->lightInWorldSector);
            linkAtHead(light->WorldSectorsInLight, tie->WorldSectorInLight);
        } else {
            const auto* plane = reinterpret_cast<const RpPlaneSector*>(s);
            const int axis = plane->type / 4;
            if ((&bb.inf.x)[axis] < plane->leftValue) {
                if (plane->rightValue < (&bb.sup.x)[axis]) stack.push_back(plane->rightSubTree);
                next = plane->leftSubTree;
            } else if (plane->rightValue < (&bb.sup.x)[axis]) {
                next = plane->rightSubTree;
            }
        }
        if (next == nullptr) {
            if (stack.empty()) break;
            next = stack.back();
            stack.pop_back();
        }
        s = next;
    }
    return object;
}

// The two counting modules (0x50A, 0x50B): their globals' offsets are not kept, as in the original.
void* count50AOpen(void* i, RwInt32, RwInt32) { return ++module50A.numInstances, i; }
void* count50AClose(void* i, RwInt32, RwInt32) { return --module50A.numInstances, i; }
void* count50BOpen(void* i, RwInt32, RwInt32) { return ++module50B.numInstances, i; }
void* count50BClose(void* i, RwInt32, RwInt32) { return --module50B.numInstances, i; }

}  // namespace

RwPluginRegistry& rpWorldRegistry() { return worldTKList; }
RwPluginRegistry& rpWorldSectorRegistry() { return sectorTKList; }

RwBool RpWorldPluginAttach() {
    RwInt32 offsets = 0;
    offsets |= RwEngineRegisterPlugin(sizeof(rpMaterialGlobals), rwID_MATERIALMODULE, _rpMaterialOpen, _rpMaterialClose);
    offsets |= RwEngineRegisterPlugin(sizeof(rpMeshGlobals), rwID_MESHMODULE, _rpMeshOpen, _rpMeshClose);
    // geometry 0x503, clump 0x504: not yet ported
    offsets |= RwEngineRegisterPlugin(sizeof(rpLightGlobals), rwID_LIGHTMODULE, _rpLightOpen, _rpLightClose);
    offsets |= RwEngineRegisterPlugin(0, 0x50Au, count50AOpen, count50AClose);
    offsets |= RwEngineRegisterPlugin(sizeof(rpWorldGlobals), rwID_WORLDMODULE, WorldOpen, WorldClose);
    offsets |= RwEngineRegisterPlugin(0, 0x50Bu, count50BOpen, count50BClose);
    if (offsets < 0) return 0;
    // the world-object extensions this port uses (_rpWorldObjRegisterExtensions 0x80462E70 registers more: atomic,
    // clump and camera extensions, the native sector chunk 0x510, the rights-to-render readers)
    g_lightExtOffset = RpLightRegisterPlugin(sizeof(rpWorldLightExt), rwID_WORLDLIGHTEXT, WorldInitLightExt,
                                             WorldDeInitLightExt, WorldCopyLightExt);
    if (g_lightExtOffset < 0) return 0;
    if (RpWorldSectorRegisterPlugin(0, rwID_BINMESHPLUGIN, nullptr, nullptr, nullptr) < 0 ||
        RpWorldSectorRegisterPluginStream(rwID_BINMESHPLUGIN, readSectorMesh, nullptr, nullptr) < 0) {
        return 0;
    }
    return 1;
}

void* _rpMeshOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    meshModule.globalsOffset = offset;
    mg().nextSerialNum = 1;
    ++meshModule.numInstances;
    return instance;   // native: no build-mesh free list (the mesh generators are not ported)
}

void* _rpMeshClose(void* instance, RwInt32, RwInt32) {
    --meshModule.numInstances;
    return instance;
}

void* WorldOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    worldModule.globalsOffset = offset;
    rpWorldGlobals& g = wg();
    // native: no pipelines (the original opens the GameCube's here); the light ties' free list joins the registry's
    g.registryEntries = RwFreeListCreateAndPreallocateSpace(sizeof(RegistryEntry), 8, alignof(RegistryEntry), 1,
                                                            &worldRegistrySpace, 0x00040000u | rwID_WORLD);
    if (g.registryEntries == nullptr) return nullptr;
    g.lightTies = RwFreeListCreate(sizeof(rpLightTie), 64, alignof(rpLightTie), kRegistryHint);
    if (g.lightTies == nullptr) {
        RwFreeListDestroy(g.registryEntries);
        g.registryEntries = nullptr;
        return nullptr;
    }
    listInit(g.worlds);
    ++worldModule.numInstances;
    return instance;
}

void* WorldClose(void* instance, RwInt32, RwInt32) {
    rpWorldGlobals& g = wg();
    if (g.lightTies != nullptr) RwFreeListDestroy(g.lightTies), g.lightTies = nullptr;
    if (g.registryEntries != nullptr) RwFreeListDestroy(g.registryEntries), g.registryEntries = nullptr;
    --worldModule.numInstances;
    return instance;
}

RpMaterialList* _rpMaterialListStreamRead(RwStream* stream, RpMaterialList* matList) {
    RwUInt32 length = 0, version = 0;
    if (!RwStreamFindChunk(stream, rwID_STRUCT, &length, &version)) return nullptr;
    if (!versionOk(version)) return error(E_RW_BADVERSION), nullptr;
    RwInt32 count = 0;
    if (RwStreamReadInt32(stream, &count, 4) == nullptr) return nullptr;
    _rpMaterialListInitialize(matList);
    if (count == 0) return matList;
    if (count < 0) return nullptr;   // native: a count the list cannot hold
    std::vector<RwInt32> indices(static_cast<std::size_t>(count));
    const auto fail = [&]() -> RpMaterialList* {
        _rpMaterialListDeinitialize(matList);
        return nullptr;
    };
    {   // the whole array at once, as the original (0x8045CA20 .. 0x8045CAB0: space = count, no growth while appending)
        const std::size_t bytes = static_cast<std::size_t>(count) * sizeof(RpMaterial*);
        auto* materials = static_cast<RpMaterial**>(RwMalloc(bytes, 0x01030000u | rwID_MATLIST));
        if (materials == nullptr) return errorSize(E_RW_NOMEM, bytes), fail();
        matList->materials = materials;
        matList->space = count;
    }
    if (RwStreamReadInt32(stream, indices.data(), static_cast<RwUInt32>(count) * 4u) == nullptr) return fail();
    for (RwInt32 i = 0; i < count; ++i) {
        RpMaterial* m = nullptr;
        const RwInt32 index = indices[static_cast<std::size_t>(i)];
        if (index < 0) {
            RwUInt32 materialVersion = 0;
            if (!RwStreamFindChunk(stream, rwID_MATERIAL, nullptr, &materialVersion)) return fail();
            if (!versionOk(materialVersion)) return error(E_RW_BADVERSION), fail();
            m = RpMaterialStreamRead(stream);
            if (m == nullptr) return fail();
        } else {
            // native: only an earlier entry (the original reads any slot, also uninitialised ones)
            if (index >= matList->numMaterials) return fail();
            m = matList->materials[index];
            ++m->refCount;
        }
        if (_rpMaterialListAppendMaterial(matList, m) < 0) {
            RpMaterialDestroy(m);
            return fail();   // native: the original carries on without it (later indices shift)
        }
        RpMaterialDestroy(m);   // the list's reference is the one that stays
    }
    return matList;
}

RpMeshHeader* _rpMeshRead(RwStream* stream, const void* object, const RpMaterialList* matList) {
    const auto* o = static_cast<const RwObject*>(object);
    bool hasIndices = true;
    if (o->type == rpWORLD) hasIndices = (static_cast<const RpWorld*>(object)->flags & rpWORLDNATIVE) == 0;
    RwUInt32 hdr[3] = {};
    if (RwStreamReadInt32(stream, reinterpret_cast<RwInt32*>(hdr), 12) == nullptr) return nullptr;
    if (hdr[1] > 0xFFFFu) return nullptr;   // native: the u16 count holds it
    const std::size_t meshesAt = sizeof(RpMeshHeader), indicesAt = meshesAt + hdr[1] * sizeof(RpMesh);
    const std::size_t size = indicesAt + (hasIndices ? static_cast<std::size_t>(hdr[2]) * 2 : 0);
    auto* h = static_cast<RpMeshHeader*>(RwMalloc(size, kMeshHint));
    if (h == nullptr) return nullptr;
    h->flags = hdr[0];
    h->numMeshes = static_cast<RwUInt16>(hdr[1]);
    h->serialNum = mg().nextSerialNum++;
    h->totalIndicesInMesh = hdr[2];
    h->firstMeshOffset = 0;
    RpMesh* mesh = RpMeshHeaderGetMeshes(h);
    auto* ix = reinterpret_cast<RwUInt16*>(reinterpret_cast<RwUInt8*>(h) + indicesAt);
    std::size_t used = 0;
    RwInt32 buffer[256];
    for (RwUInt32 n = 0; n < h->numMeshes; ++n, ++mesh) {
        RwInt32 m[2] = {};
        bool ok = RwStreamReadInt32(stream, m, 8) != nullptr;
        // native: the material index and the indices' total checked (the original reads past both)
        ok = ok && m[1] >= 0 && m[1] < matList->numMaterials &&
             (!hasIndices || static_cast<std::size_t>(static_cast<RwUInt32>(m[0])) <= hdr[2] - used);
        if (!ok) {
            RwFree(h);   // native: the original leaks it
            return nullptr;
        }
        mesh->numIndices = static_cast<RwUInt32>(m[0]);
        mesh->material = matList->materials[m[1]];
        mesh->indices = ix;
        if (hasIndices) {
            for (RwUInt32 left = mesh->numIndices; left != 0;) {
                const RwUInt32 k = left < 256 ? left : 256;
                if (RwStreamReadInt32(stream, buffer, k * 4) == nullptr) {
                    RwFree(h);
                    return nullptr;
                }
                left -= k;
                for (RwUInt32 j = 0; j < k; ++j) *ix++ = static_cast<RwUInt16>(buffer[j]);
            }
            used += mesh->numIndices;
        }
    }
    return h;
}

RwBool _rpMeshDestroy(RpMeshHeader* mesh) {
    // an all-zero header is the original's static empty one (0x8056E928) and is not freed
    if (mesh->flags != 0 || mesh->numMeshes != 0 || mesh->serialNum != 0 || mesh->totalIndicesInMesh != 0 ||
        mesh->firstMeshOffset != 0) {
        RwFree(mesh);
    }
    return 1;
}

RpWorld* RpWorldStreamRead(RwStream* stream) {
    RwUInt32 length = 0, version = 0;
    if (!RwStreamFindChunk(stream, rwID_STRUCT, &length, &version)) return nullptr;
    if (!versionOk(version)) return error(E_RW_BADVERSION), nullptr;
    RwUInt32 wc[16] = {};   // {rootIsWorldSector, invWorldOrigin, numTriangles, numVertices, numPlaneSectors,
                            //  numWorldSectors, colSectorSize, format, boundingBox (sup, inf)}
    const RwUInt32 take = length < sizeof wc ? length : static_cast<RwUInt32>(sizeof wc);
    if (RwStreamRead(stream, wc, take) != take) return nullptr;
    if (length > take && RwStreamSkip(stream, length - take) == nullptr) return nullptr;   // native: the rest skipped
    // the original tests colSectorSize before converting the words: the stream's bytes as a big-endian word
    const RwUInt8* raw = reinterpret_cast<const RwUInt8*>(&wc[8]);
    const auto rawBigEndian = static_cast<RwInt32>((RwUInt32{raw[0]} << 24) | (RwUInt32{raw[1]} << 16) |
                                                   (RwUInt32{raw[2]} << 8) | raw[3]);
    if (rawBigEndian > 0) return error(E_RW_BADVERSION), nullptr;
    RwMemNative32(wc, sizeof wc);
    const RwUInt32 format = wc[9];
    if ((format & rpWORLDNATIVE) != 0) {   // GameCube display lists: the converter writes platform-independent sectors
        RwError e{rwID_WORLDPLUGIN, _rwerror(E_RW_BADPARAM, "a GameCube-native world (convert it offline)")};
        RwErrorSet(&e);
        return nullptr;
    }
    const RwInt32 numTexCoordSets = (format & 0x00FF0000u) != 0 ? static_cast<RwInt32>((format & 0x00FF0000u) >> 16)
                                    : (format & rpWORLDTEXTURED2) != 0 ? 2
                                                                        : static_cast<RwInt32>((format >> 2) & 1u);
    if (numTexCoordSets > 8) return nullptr;
    // the one block: the world, then the tree (native: each piece aligned; the arrays' totals come from the struct)
    const std::size_t nv = wc[5], nt = wc[4], planes = wc[6], sectors = wc[7];
    std::size_t size = align(worldTKList.sizeOfStruct, 16) + planes * (sizeof(RpPlaneSector) + 8) +
                       sectors * (align(sectorTKList.sizeOfStruct, 16) + 16 + 6 * 8) + nv * 12 + nt * 8;
    if ((format & rpWORLDNORMALS) != 0) size += nv * 4;
    if ((format & rpWORLDPRELIT) != 0) size += nv * 4;
    size += static_cast<std::size_t>(numTexCoordSets) * (nv * 8 + sectors * 8);
    auto* world = static_cast<RpWorld*>(RwMalloc(size, kWorldHint));
    if (world == nullptr) return errorSize(E_RW_NOMEM, size), nullptr;
    std::memset(world, 0, size);
    Block block{reinterpret_cast<RwUInt8*>(world) + worldTKList.sizeOfStruct, reinterpret_cast<RwUInt8*>(world) + size};
    world->object = {rpWORLD, 0, 0, 1, nullptr};   // privateFlags 1: one block
    registerWorld(world, size);
    world->flags = format;
    world->renderOrder = 2;
    const RwReal kMinusOne = -1.0f;
    world->worldOrigin = {kMinusOne * std::bit_cast<RwReal>(wc[1]), kMinusOne * std::bit_cast<RwReal>(wc[2]),
                          kMinusOne * std::bit_cast<RwReal>(wc[3])};
    world->numTexCoordSets = numTexCoordSets;
    listInit(world->clumpList);
    listInit(world->lightList);
    listInit(world->directionalLightList);
    RwUInt32 listVersion = 0;
    if (!RwStreamFindChunk(stream, rwID_MATLIST, nullptr, &listVersion)) return destroyWorld(world, false), nullptr;
    if (!versionOk(listVersion)) return error(E_RW_BADVERSION), destroyWorld(world, false), nullptr;
    if (_rpMaterialListStreamRead(stream, &world->matList) == nullptr) return destroyWorld(world, false), nullptr;
    world->rootSector = readSectorTree(stream, block, world, format, wc[0] != 0);
    if (world->rootSector == nullptr) return destroyWorld(world, false), nullptr;
    world->numClumpsInWorld = 0;
    world->currentClumpLink = &world->clumpList.link;
    std::memcpy(&world->boundingBox, &wc[10], sizeof world->boundingBox);
    setupSectorBoundingBoxes(world);
    world->renderCallBack = nullptr;   // native: the renderer draws the sectors
    for (RwInt32 i = 0; i < world->matList.numMaterials; ++i) {   // as the original (no net effect on references)
        RpMaterial* m = world->matList.materials[i];
        if (m->texture != nullptr) RpMaterialSetTexture(m, m->texture);
    }
    if (_rwPluginRegistryInitObject(&worldTKList, world) == nullptr) return destroyWorld(world, false), nullptr;
    if (_rwPluginRegistryReadDataChunks(&worldTKList, stream, world) == nullptr) return destroyWorld(world, true), nullptr;
    bool meshes = true;   // the original builds a missing mesh here (0x80460E00); native: not ported, fail closed
    forEachAtomic(world->rootSector, [&](RpWorldSector* s) { return meshes = s->mesh != nullptr; });
    if (!meshes) {
        RwError e{rwID_WORLDPLUGIN, _rwerror(E_RW_BADPARAM, "a world sector without its mesh chunk")};
        RwErrorSet(&e);
        return destroyWorld(world, true), nullptr;
    }
    return world;
}

RwBool RpWorldDestroy(RpWorld* world) {
    destroyWorld(world, true);
    return 1;
}

RpWorld* RpWorldSectorGetWorld(const RpWorldSector* sector) {
    RwLinkList& list = wg().worlds;
    for (RwLLLink* l = list.link.next; l != &list.link; l = l->next) {
        auto* e = containerOf<RegistryEntry>(l, offsetof(RegistryEntry, link));
        RpWorld* w = e->world;
        if ((w->object.privateFlags & 1) != 0) {
            const auto* at = reinterpret_cast<const RwUInt8*>(sector);
            const auto* base = reinterpret_cast<const RwUInt8*>(w);
            if (at >= base && at < base + e->size) return w;
        } else {
            bool found = false;
            forEachAtomic(w->rootSector, [&](RpWorldSector* s) { return !(found = s == sector); });
            if (found) return w;
        }
    }
    return nullptr;
}

RpWorld* RpWorldForAllWorldSectors(RpWorld* world, RpWorldSectorCallBack callBack, void* data) {
    forEachAtomic(world->rootSector, [&](RpWorldSector* s) { return callBack(s, data) != nullptr; });
    return world;
}

RwInt32 RpWorldRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                              RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB) {
    return _rwPluginRegistryAddPlugin(&worldTKList, size, pluginID, constructCB, destructCB, copyCB);
}
RwInt32 RpWorldSectorRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                                    RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB) {
    return _rwPluginRegistryAddPlugin(&sectorTKList, size, pluginID, constructCB, destructCB, copyCB);
}
RwInt32 RpWorldRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                    RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB) {
    return _rwPluginRegistryAddPluginStream(&worldTKList, pluginID, readCB, writeCB, getSizeCB);
}
RwInt32 RpWorldSectorRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                          RwPluginDataChunkWriteCallBack writeCB,
                                          RwPluginDataChunkGetSizeCallBack getSizeCB) {
    return _rwPluginRegistryAddPluginStream(&sectorTKList, pluginID, readCB, writeCB, getSizeCB);
}

RpWorld* RpWorldAddLight(RpWorld* world, RpLight* light) {
    if (ext(light)->world != nullptr) RpWorldRemoveLight(ext(light)->world, light);   // native: moved, not doubled
    ext(light)->world = world;
    if (light->object.object.subType < rpLIGHTPOINT) {
        linkAtHead(world->directionalLightList, light->inWorld);
    } else {
        if (RwFrame* frame = static_cast<RwFrame*>(light->object.object.parent)) RwFrameUpdateObjects(frame);
        linkAtHead(world->lightList, light->inWorld);
    }
    return world;
}

RpWorld* RpWorldRemoveLight(RpWorld* world, RpLight* light) {
    ext(light)->world = nullptr;
    freeLightTies(light);
    if (light->inWorld.next != nullptr) {
        unlink(light->inWorld);
        light->inWorld = {nullptr, nullptr};   // native: the original leaves the link pointing into the list
    }
    return world;
}

RpWorld* RpLightGetWorld(const RpLight* light) { return ext(const_cast<RpLight*>(light))->world; }

rpWorldLightExt* rpWorldLightExtension(RpLight* light) { return ext(light); }
