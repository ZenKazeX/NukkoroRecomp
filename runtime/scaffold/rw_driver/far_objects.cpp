// SCAFFOLDING (R2): far-object proxies read from the translated layout manager and RenderWare resources - deleted by R6: the scene graph's lift owns the layout and draws far objects itself
// Far-object proxies. Contract: far_objects.h and docs/DRAW_DISTANCE.md "Far-object proxies".

#include "far_objects.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "gekko/cpu.h"
#include "gekko/native_motion_store.h"
#include "motion/skeleton_pose.h"

namespace gekko::scaffold {

namespace render = shadow::render;

namespace {

// The layout manager (0x800C9F6C: a holder in small data, the manager at its +0).
constexpr u32 kSdaManagerHolder = 0x327Cu;
constexpr u32 kManagerFactories = 0x14u;  // the per-type factory table
constexpr u32 kManagerSlots = 0x18u;      // 0x800 slots of 0x20 bytes
constexpr u32 kManagerRestricted = 0x428u;  // bit 0: only entries flagged 0x8000 run
// The event flags (bit 0 set by TPjsEventActionManager's constructor 0x80173EF4 -> 0x800CB8C0, cleared by the layout
// load 0x800CBC54 and 0x800CBC08): while set, the leave test 0x800C9E4C sends every object whose entry lacks 0x8000
// away the frame it spawns - the game never draws it.
constexpr u32 kSdaEventFlags = 0x3274u;
constexpr u32 kSlotCount = 0x800u;
constexpr u32 kSlotStride = 0x20u;
constexpr u32 kSlotFactoryIndex = 0x10u;

// A layout entry (0x2C bytes).
constexpr u32 kEntryRotation = 0x0Cu;  // three floats, degrees
constexpr u32 kEntryFlags = 0x18u;
// A live object (0x800CA0B8, the set object's constructor): the slot's +0x0C is it, its +0x28 the slot. A model
// holder's +0x3C flags have bit 7 while the holder fades its model (in: bit 9, out: bit 8; alpha +0x44; 0x80163670)
// and bit 10 once a fade out is done (the model left at alpha 0 until the object goes).
constexpr u32 kSlotObject = 0x0Cu;
constexpr u32 kObjectSlot = 0x28u;
constexpr u32 kHolderFlags = 0x3Cu;
constexpr u32 kHolderFading = 0x80u;
constexpr u32 kHolderFadedOut = 0x400u;
constexpr u32 kEntryRange = 0x23u;
constexpr u32 kEntryMiscLength = 0x24u;
constexpr u32 kEntryMisc = 0x28u;
constexpr u32 kDormant = 0x00000001u;       // may spawn (cleared when the object is destroyed or collected)
constexpr u32 kLive = 0x00000002u;          // spawned: the game's object exists (the loop counts it)
constexpr u32 kHeld = 0x00020000u;          // 0x800CA8D8 refuses to spawn it
constexpr u32 kRunsRestricted = 0x00008000u;

// A model library (<CLASS>.BIN's SCENE) as the game holds it (0x80208F3C, 0x80208B78, 0x80162DBC, 0x80162E4C,
// 0x80162EB4).
constexpr u32 kLibraryParts = 0x00u;      // the part table, 0x40 bytes each
constexpr u32 kLibraryEntries = 0x08u;    // the model entries, 0x10 bytes each
constexpr u32 kLibraryEntryCount = 0x0Cu;
constexpr u32 kLibraryRecords = 0x28u;    // the gadget records, 0x48 bytes each
constexpr u32 kLibraryCount = 0x2Cu;
constexpr u32 kRecordStride = 0x48u;
constexpr u32 kRecordId = 0x04u;
constexpr u32 kRecordModelName = 0x08u;   // its model entry's name
constexpr u32 kEntryStride = 0x10u;
constexpr u32 kEntryKey = 0x08u;          // the key 0x80162E4C selects by
constexpr u32 kEntryPartList = 0x04u;     // pairs of words: (_, part index)
constexpr u32 kEntryPartCount = 0x0Cu;
constexpr u32 kPartStride = 0x40u;
constexpr u32 kPartModel = 0x04u;         // the DFF's name
constexpr u32 kPartTranslation = 0x18u;   // three floats
constexpr u32 kPartRotation = 0x24u;      // X, Y, Z degrees

// The resource manager and its one-file archives (0x800122D4, 0x80012700, 0x80012D88, 0x800148E0).
constexpr u32 kSdaResourceManager = 0x2B7Cu;
constexpr u32 kMapHeader = 0x04u;
constexpr u32 kMapNodeLeft = 0x00u;
constexpr u32 kMapNodeRight = 0x04u;
constexpr u32 kMapNodeKey = 0x0Cu;
constexpr u32 kMapNodeValue = 0x10u;
constexpr u32 kContainerGet = 0x18u;      // vtable slots: by id, by name
constexpr u32 kContainerFind = 0x1Cu;
constexpr u32 kOneFileGet = 0x8001483Cu;  // ResourceOneFile
constexpr u32 kOneFileFind = 0x800148E0u;
constexpr u32 kOneFileReqGet = 0x8006B114u;   // ResourceOneFileReq
constexpr u32 kOneFileReqFind = 0x8006B090u;
constexpr u32 kOneFileRecords = 0x14u;
constexpr u32 kOneFileCount = 0x18u;
constexpr u32 kOneFileStride = 0x3Cu;
constexpr u32 kOneFileKind = 0x2Cu;

// RenderWare (3.7, GameCube layout).
constexpr u32 kObjectParent = 0x04u;      // an atomic's frame, a clump's root frame, a frame's parent
constexpr u32 kFrameModelling = 0x10u;
constexpr u32 kClumpAtomics = 0x08u;      // RwLinkList head
constexpr u32 kAtomicInClumpLink = 0x40u;
constexpr u32 kAtomicGeometry = 0x18u;
constexpr u32 kAtomicSphere = 0x1Cu;      // local bounding sphere: centre, radius

float read_f32(Cpu& c, u32 address) {
    const u32 bits = c.mem_read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// A guest address the game's data can live at: its memory, or the native motion store's arena (the clips and their
// banks, gekko/native_motion_store.h).
bool in_ram(u32 p) {
    return (p >= 0x80000000u && p < 0x81800000u) ||
           (p >= gekko::NativeMotionStore::kArenaBase &&
            p < gekko::NativeMotionStore::kArenaBase + gekko::NativeMotionStore::kArenaReserve);
}
bool pointer(u32 p) { return in_ram(p) && (p & 3u) == 0u; }

// A RenderWare matrix (right, up, at, pos rows of 16 bytes) as a column-vector 3x4.
render::Float3x4 rw_matrix(Cpu& c, u32 matrix) {
    render::Float3x4 m;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 3; ++row)
            m.m[row][column] = read_f32(c, matrix + 16u * static_cast<u32>(column) + 4u * static_cast<u32>(row));
    return m;
}

// a after b: p' = a * (b * p).
render::Float3x4 compose(const render::Float3x4& a, const render::Float3x4& b) {
    render::Float3x4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            float sum = j == 3 ? a.m[i][3] : 0.0f;
            for (int k = 0; k < 3; ++k)
                sum += a.m[i][k] * b.m[k][j];
            r.m[i][j] = sum;
        }
    return r;
}

// The entry's frame: position, and R = Ry * Rx * Rz (degrees) - or Rx * Rz for a class that never turns about Y.
render::Float3x4 entry_frame(Cpu& c, u32 entry, bool yaw) {
    constexpr float kRadians = 3.14159265358979323846f / 180.0f;
    const float x = read_f32(c, entry + kEntryRotation) * kRadians;
    const float y = yaw ? read_f32(c, entry + kEntryRotation + 4u) * kRadians : 0.0f;
    const float z = read_f32(c, entry + kEntryRotation + 8u) * kRadians;
    const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z),
                sz = std::sin(z);
    // Ry * Rx
    const float a[3][3] = {{cy, sy * sx, sy * cx}, {0.0f, cx, -sx}, {-sy, cy * sx, cy * cx}};
    const float rz[3][3] = {{cz, -sz, 0.0f}, {sz, cz, 0.0f}, {0.0f, 0.0f, 1.0f}};
    render::Float3x4 m;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            m.m[i][j] = a[i][0] * rz[0][j] + a[i][1] * rz[1][j] + a[i][2] * rz[2][j];
        m.m[i][3] = read_f32(c, entry + 4u * static_cast<u32>(i));
    }
    return m;
}

// A loaded binary's symbol (0x8040F50C): entries of {value, name offset} (8 bytes, count +0xC) at +0x1C, names at
// +0x24, the data at +0x14; both words little-endian unless the byte at +1 is set. The symbol is data + value.
u32 swap_unless(bool native, u32 v) { return native ? v : ((v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24)); }

bool guest_string_equals(Cpu& c, u32 guest, const char* host);

u32 binary_symbol(Cpu& c, u32 binary, const char* symbol) {
    if (!(binary >= 0x80000000u && binary < 0x81800000u))
        return 0u;
    const bool native = c.mem_read8(binary + 1u) != 0u;
    const u32 table = c.mem_read32(binary + 0x1Cu), names = c.mem_read32(binary + 0x24u), data = c.mem_read32(binary + 0x14u);
    const u32 count = std::min(c.mem_read32(binary + 0x0Cu), 4096u);
    for (u32 i = 0; i < count; ++i) {
        const u32 e = table + i * 8u;
        if (guest_string_equals(c, names + swap_unless(native, c.mem_read32(e + 4u)), symbol))
            return data + swap_unless(native, c.mem_read32(e));
    }
    return 0u;
}

// A guest string (inline at `guest`) against a host one.
bool guest_string_equals(Cpu& c, u32 guest, const char* host) {
    for (u32 i = 0; i < 256u; ++i) {
        const char g = static_cast<char>(c.mem_read8(guest + i));
        if (g != host[i])
            return false;
        if (g == '\0')
            return true;
    }
    return false;
}

bool guest_strings_equal(Cpu& c, u32 a, u32 b) {
    if (!(a >= 0x80000000u && a < 0x81800000u) || !(b >= 0x80000000u && b < 0x81800000u))
        return false;
    for (u32 i = 0; i < 256u; ++i) {
        const u8 x = c.mem_read8(a + i);
        if (x != c.mem_read8(b + i))
            return false;
        if (x == 0u)
            return true;
    }
    return false;
}

// T * Ry * Rx * Rz (degrees): RwMatrixRotate about Z (replace), X and Y (postconcat), then RwMatrixTranslate.
// RwMatrixRotate Z (replace), X and Y (postconcat), then translate - angles in radians.
render::Float3x4 rotate_zxy(float z, float x, float y, float tx, float ty, float tz) {
    const float cx = std::cos(x), sx = std::sin(x);
    const float cy = std::cos(y), sy = std::sin(y);
    const float cz = std::cos(z), sz = std::sin(z);
    const float a[3][3] = {{cy, sy * sx, sy * cx}, {0.0f, cx, -sx}, {-sy, cy * sx, cy * cx}};  // Ry * Rx
    const float rz[3][3] = {{cz, -sz, 0.0f}, {sz, cz, 0.0f}, {0.0f, 0.0f, 1.0f}};
    render::Float3x4 m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m.m[i][j] = a[i][0] * rz[0][j] + a[i][1] * rz[1][j] + a[i][2] * rz[2][j];
    m.m[0][3] = tx;
    m.m[1][3] = ty;
    m.m[2][3] = tz;
    return m;
}

render::Float3x4 part_matrix(float z_deg, float x_deg, float y_deg, float tx, float ty, float tz) {
    constexpr float kRadians = 3.14159265358979323846f / 180.0f;
    return rotate_zxy(z_deg * kRadians, x_deg * kRadians, y_deg * kRadians, tx, ty, tz);
}

// A camera-facing model's frame at `p` (HINTRING01: 0x80109DDC): the camera's angles 0x80109F68 - camera 0 of the
// list [r13+0x2B34] (0x80009548, 0x8000D0F0), its transform at element +8 (eye +0, target +0xC); 0x8000A610: the
// target - eye direction normalised (0x8040DEC4), 0x804107B0: pitch = atan2(d.y, sqrt(d.x^2 + d.z^2)), yaw =
// atan2(-d.x, -d.z); roll = transform +0x18 - then Z by roll (0x80109EE0, replace), X by pitch (0x800EA04C), Y by
// yaw (0x800EA0D4), translated to `p` (0x80488600): the same order as a part's frame, in radians.
render::Float3x4 camera_facing(Cpu& c, const float p[3]) {
    const u32 manager = c.mem_read32(c.gpr[13] + 0x2B34u);
    const u32 element = in_ram(manager) ? c.mem_read32(manager + 0xCu) : 0u;
    const u32 camera = in_ram(element) ? c.mem_read32(element) : 0u;
    if (!in_ram(camera))
        return rotate_zxy(0.0f, 0.0f, 0.0f, p[0], p[1], p[2]);
    const u32 transform = camera + 8u;  // its own, not the render camera's: the one 0x80109F68 reads
    float d[3];
    for (u32 i = 0; i < 3u; ++i)
        d[i] = read_f32(c, transform + 0xCu + 4u * i) - read_f32(c, transform + 4u * i);
    const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (length > 0.0f)
        for (float& v : d)
            v /= length;
    const float pitch = std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2]));
    const float yaw = std::atan2(-d[0], -d[2]);
    return rotate_zxy(read_f32(c, transform + 0x18u), pitch, yaw, p[0], p[1], p[2]);
}

// ---- the resource manager, read natively ----------------------------------------------------------------------
// 0x80012700(manager, type, name) / 0x8001277C(manager, type, id): the manager (0x800122D4, r13+0x2B7C) is a
// std::map<int, container> (header at +4: root at its +0; nodes: left +0, right +4, key +0xC, value +0x10; lower_bound
// with signed compare, 0x80012D88); the container answers by name through its vtable +0x1C and by id through +0x18.
// Both one-file kinds keep records of 0x3C bytes at +0x14 (count +0x18), the name inline at +0, the kind at +0x2C.

u32 container_of(Cpu& c, s32 type) {
    const u32 manager = c.mem_read32(c.gpr[13] + kSdaResourceManager);
    if (!pointer(manager))
        return 0u;
    const u32 header = manager + kMapHeader;
    u32 node = c.mem_read32(header);
    u32 candidate = header;
    for (int guard = 0; pointer(node) && guard < 64; ++guard) {
        if (!(static_cast<s32>(c.mem_read32(node + kMapNodeKey)) < type)) {
            candidate = node;
            node = c.mem_read32(node + kMapNodeLeft);
        } else {
            node = c.mem_read32(node + kMapNodeRight);
        }
    }
    if (candidate == header || type < static_cast<s32>(c.mem_read32(candidate + kMapNodeKey)))
        return 0u;
    const u32 container = c.mem_read32(candidate + kMapNodeValue);
    return pointer(container) ? container : 0u;
}

// A record's object. ResourceOneFile (0x800148E0 by name): kind 6 -> (+0x28)->+0x14, 8 -> +0x24, else +0x20; by id
// (0x8001483C) only kind 1 -> +0x30. ResourceOneFileReq (0x8006B114, both): 6 -> the loaded binary's "SCENE" symbol
// (0x8040F50C), 7 -> (+0x28)->+0x14, 0xA -> +0x24, else +0x20.
u32 record_object(Cpu& c, bool request, bool by_id, u32 record) {
    const u32 kind = c.mem_read32(record + kOneFileKind);
    if (!request) {
        if (by_id)
            return kind == 1u ? c.mem_read32(record + 0x30u) : 0u;
        if (kind == 6u)
            return c.mem_read32(c.mem_read32(record + 0x28u) + 0x14u);
        return c.mem_read32(record + (kind == 8u ? 0x24u : 0x20u));
    }
    if (kind == 6u)
        return binary_symbol(c, c.mem_read32(record + 0x28u), "SCENE");
    if (kind == 7u)
        return c.mem_read32(c.mem_read32(record + 0x28u) + 0x14u);
    return c.mem_read32(record + (kind == 0xAu ? 0x24u : 0x20u));
}

// The container's kind from its vtable: 1 one-file, 2 request, 0 neither (not read natively). A one-file container
// made for an archive (0x80014C58, vtable 0x8051D7DC: enemies' `enemy/*.one`) has its by-id getter one slot later.
int container_kind(Cpu& c, u32 container) {
    const u32 vtable = c.mem_read32(container);
    const u32 find = c.mem_read32(vtable + kContainerFind), get = c.mem_read32(vtable + kContainerGet);
    if (find == kOneFileFind && (get == kOneFileGet || c.mem_read32(vtable + kContainerFind + 4u) == kOneFileGet))
        return 1;
    if (find == kOneFileReqFind && get == kOneFileReqGet)
        return 2;
    return 0;
}

u32 resource_by_name(Cpu& c, s32 type, const char* name) {
    const u32 container = container_of(c, type);
    const int kind = container != 0u ? container_kind(c, container) : 0;
    if (kind == 0)
        return 0u;
    const u32 records = c.mem_read32(container + kOneFileRecords);
    const s32 count = std::min(static_cast<s32>(c.mem_read32(container + kOneFileCount)), 4096);
    for (s32 n = 0; pointer(records) && n < count; ++n) {
        const s32 i = kind == 1 ? count - 1 - n : n;  // ResourceOneFile searches last to first
        const u32 record = records + static_cast<u32>(i) * kOneFileStride;
        if (guest_string_equals(c, record, name))
            return record_object(c, kind == 2, false, record);
    }
    return 0u;
}

// By id: the record at id - 2 (both getters address records + id * 0x3C - 0x78).
u32 resource_by_id(Cpu& c, s32 type, u32 id) {
    const u32 container = container_of(c, type);
    const int kind = container != 0u ? container_kind(c, container) : 0;
    const u32 records = kind != 0 ? c.mem_read32(container + kOneFileRecords) : 0u;
    if (!pointer(records) || id < 2u || id - 2u >= c.mem_read32(container + kOneFileCount))
        return 0u;
    return record_object(c, kind == 2, true, records + (id - 2u) * kOneFileStride);
}

// ---- the classes ----------------------------------------------------------------------------------------------
// Each from its constructor: the holder 0x8015FBE4(out, type, name) or 0x8015FDBC(out, type, id) sets the holder's
// library (+0x30) to the resource, its type (+0x70) for the parts' models; then 0x80162E4C(holder, key, 0) builds the
// model entry whose +8 is the key, or (ART GADGET) 0x80162DBC builds the one named by the gadget record.
enum class Selection : u8 { GadgetRecord, Key, MiscKey, RingGroup, Enemy };
struct Rule {
    const char* factory;  // the class name the factory's first word points to
    s32 type;             // the resource type
    const char* library;  // the model library by name, or
    u32 library_id;       // ... by id
    Selection selection;
    u32 key;              // Key: the model entry's key; MiscKey: the key is the entry's first misc word
    bool yaw;             // the entry's Y rotation turns it (the frame's Ry)
    float lift;           // added to the entry's Y position
    u32 holder;           // the object's model holder, when the game fades the model at the range's edge (0: never)
    bool verified;        // its pose measured equal to the game's at handovers (docs/DRAW_DISTANCE.md)
    bool scaled = false;  // misc words 1..3 scale it (T * R * S; 1 when the misc data is shorter)
    const char* clip = nullptr;   // Enemy: the clip its stand-in plays (`key` = the enemy table row)
    // A type-0 model the object draws itself, camera-facing at its position, from a task under render level 2
    // (camera_facing; light group 4: CameraRender's default, the draw requests none).
    const char* camera_model = nullptr;
};
constexpr Rule kRules[] = {
    // ARTISTGADGET 0x800E4360 -> 0x800E5934: misc word = the gadget record's id.
    // It fades: in once spawned (0x800E5CD8 -> 0x80160004), out past its range + 50 (0x800E43F0 -> 0x80160078, the
    // object leaves when the holder is done) - its holder at +0x38.
    {"ART GADGET", 1, "ARTISTGADGET.BIN", 0u, Selection::GadgetRecord, 0u, true, 0.0f, 0x38u, true},
    // 0x801210F4: 0x8015FBE4(out, 0, "SPRING.BIN"), 0x80162E4C(holder, 0, 0). Its matrix (0x8012081C) is
    // T * Rx * Rz: the entry's Y rotation is never applied.
    {"SPRING", 0, "SPRING.BIN", 0u, Selection::Key, 0u, false, 0.0f, 0u, true},
    // 0x800FFC90: 0x8015FDBC(out, 0, 0xD0), 0x80162E4C(holder, 0, 0).
    {"DASHPANEL", 0, nullptr, 0xD0u, Selection::Key, 0u, true, 0.0f, 0u, true},
    // 0x8010F13C: 0x8015FDBC(out, 0, 0xD8), 0x80162E4C(holder, 0, 0).
    {"JUMP_BOARD", 0, nullptr, 0xD8u, Selection::Key, 0u, true, 0.0f, 0u, true},
    // 0x800F6A38 -> 0x800F8660: misc word 0 (of 12 bytes) is the variant, 0x800F6FC8: 0x8015FDBC(out, 0, 0xCA),
    // 0x80162E4C(holder, variant, 1); 0x800F7B24 lifts its position by 10 (0x805F4788).
    {"CONTAINER", 0, nullptr, 0xCAu, Selection::MiscKey, 0u, true, 10.0f, 0u, true},
    // The same shape: HRD_CONTAINER 0x800FABE0 / 0x800FA0B0 (0xCB, misc word 0 of 12, lifted by 2 at 0x805F4720);
    // WEAPON_CONTAINER 0x800FDE80 / 0x800FD3F8 (0xCE, misc word 0 of 8, lifted by 10 at 0x805F4884).
    {"HRD_CONTAINER", 0, nullptr, 0xCBu, Selection::MiscKey, 0u, true, 2.0f, 0u, false},
    {"WEAPON_CONTAINER", 0, nullptr, 0xCEu, Selection::MiscKey, 0u, true, 10.0f, 0u, true},
    // RING 0x80112738 -> 0x80114564: a group of rings (ring_group); each ring's model is resource 0x7C of type 0
    // (0x80113964: 0x8001277C(_, 0, 0x7C)), the "library" here.
    {"RING", 0, nullptr, 0x7Cu, Selection::RingGroup, 0u, true, 0.0f, 0u, true},
    // ART WORK 0x800E5E70 -> 0x800E64FC: misc of 16 bytes - the record's id (looked up as ART GADGET's, 0x80208F3C /
    // 0x80208B78, ARTISTWORK.BIN) and a scale; its matrix (0x800E5F00) is T * Ry * Rx * Rz * S (0x8040C770). It does
    // not fade (the base gate 0x8015AC7C).
    {"ART WORK", 1, "ARTISTWORK.BIN", 0u, Selection::GadgetRecord, 0u, true, 0.0f, 0u, true, true},
    // One model, key 0 (each create: the library call, then 0x80162E4C(holder, 0, 0)): HINTRING 0x8010AE60,
    // SAVEPOINT 0x80118E08, LONGSPRING 0x801102F4, BALLOON 0x800E8044, Fruit 0x8010630C, TORNADO 0x800DE3B4,
    // SECRETKEY 0x8011CA94, CityChaosEmerald 0x800E0544.
    // HINTRING's "?" (HINTRING01.DFF, object +0x70, 0x8010B0CC) is no part of its model: its functor 0x80109DDC (PTMF
    // 0x80527494) draws it camera-facing in render level 2.
    {"HINTRING", 0, "HINTRING.BIN", 0u, Selection::Key, 0u, true, 0.0f, 0u, true, false, nullptr, "HINTRING01.DFF"},
    {"SAVEPOINT", 0, nullptr, 0xDCu, Selection::Key, 0u, true, 0.0f, 0u, true},
    {"LONGSPRING", 0, nullptr, 0xD9u, Selection::Key, 0u, true, 0.0f, 0u, false},
    {"BALLOON", 0, nullptr, 0xC2u, Selection::Key, 0u, true, 0.0f, 0u, false},
    {"Fruit", 0, nullptr, 0xD3u, Selection::Key, 0u, true, 0.0f, 0u, false},
    {"TORNADO", 1, "TORNADO.BIN", 0u, Selection::Key, 0u, true, 0.0f, 0u, false},
    {"SECRETKEY", 0, nullptr, 0xDEu, Selection::Key, 0u, true, 0.0f, 0u, false},
    {"CityChaosEmerald", 1, "CITYCHAOSEMERALD.BIN", 0u, Selection::Key, 0u, true, 0.0f, 0u, true},
    // A key from the object (each create: the library call, then 0x80162E4C(holder, <object field>, ...)), taken as
    // misc word 0 until the handovers say otherwise: STONE_POLE 0x800D9C38, ROLL_CIRCLE 0x800D6C80, MOVE_FOOTING
    // 0x8014BB94, STONE_STATUE 0x800DD434, UNBRK_CONTAINER 0x800FC98C, CAGE 0x800F3024 (misc 1..3 its scale, copied
    // to +0x44), DOOR 0x80145248, GOALRING 0x80106F14, POWER_DEVICE_CAGE 0x800D31AC, FallBuilding 0x800E39F0.
    {"STONE_POLE", 1, "STONEPOLE.BIN", 0u, Selection::MiscKey, 0u, true, 0.0f, 0u, true},
    {"ROLL_CIRCLE", 1, "ROLLCIRCLE.BIN", 0u, Selection::MiscKey, 0u, true, 0.0f, 0u, false},
    {"MOVE_FOOTING", 1, "FOOTINGMOVABLE.BIN", 0u, Selection::MiscKey, 0u, true, 0.0f, 0u, false},
    {"STONE_STATUE", 1, "STONESTATUE.BIN", 0u, Selection::MiscKey, 0u, true, 0.0f, 0u, false},
    {"UNBRK_CONTAINER", 0, nullptr, 0xCDu, Selection::MiscKey, 0u, true, 0.0f, 0u, false},
    {"CAGE", 0, "CASE.BIN", 0u, Selection::MiscKey, 0u, true, 0.0f, 0u, true, true},
    {"DOOR", 1, "DOOR.BIN", 0u, Selection::MiscKey, 0u, true, 0.0f, 0u, false},
    {"GOALRING", 0, "GOALRING.BIN", 0u, Selection::MiscKey, 0u, true, 0.0f, 0u, false},
    {"POWER_DEVICE_CAGE", 1, "POWERDEVICECAGE.BIN", 0u, Selection::MiscKey, 0u, true, 0.0f, 0u, false},
    {"FallBuilding", 1, "FALLBUILDING.BIN", 0u, Selection::MiscKey, 0u, true, 0.0f, 0u, true},
    // No holder key: STICK 0x80123044 (STICK.BIN), entry 0 (16 handovers exact). Not CityLaser (0x800E1BE4): an
    // emitter whose beam the game draws only while it fires (16 handovers, 7 equal, models up to 4083 apart).
    {"STICK", 1, "STICK.BIN", 0u, Selection::Key, 0u, true, 0.0f, 0u, true},
    // Enemies: the enemy table row (0x804CCFDC: archive, DFF, BON, MTP), posed by a clip of their own motion bank.
    {"BK_SOLDIER", 0, nullptr, 0u, Selection::Enemy, 5u, true, 0.0f, 0u, false, false, "soldierbk_alerta"},
    {"BK_WINGLARGE", 0, nullptr, 0u, Selection::Enemy, 6u, true, 0.0f, 0u, false, false, "wingbklarge_fly"},
    {"BK_WINGSMALL", 0, nullptr, 0u, Selection::Enemy, 7u, true, 0.0f, 0u, false, false, "wingbksmall_fly"},
};
constexpr std::size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);
static_assert(kRuleCount <= FarObjects::kMaxRules);

// The entry's frame under its rule: entry_frame, lifted, scaled; `reach` multiplies the model's radius.
render::Float3x4 rule_frame(Cpu& c, u32 entry, const Rule& rule, float* reach) {
    render::Float3x4 frame = entry_frame(c, entry, rule.yaw);
    frame.m[1][3] += rule.lift;
    *reach = 1.0f;
    const u32 misc = c.mem_read32(entry + kEntryMisc);
    if (rule.scaled && c.mem_read32(entry + kEntryMiscLength) >= 16u && in_ram(misc)) {
        for (int column = 0; column < 3; ++column) {
            const float s = read_f32(c, misc + 4u + 4u * static_cast<u32>(column));
            for (int row = 0; row < 3; ++row)
                frame.m[row][column] *= s;
            *reach = std::max(*reach, std::fabs(s));
        }
    }
    return frame;
}

u32 library_of(Cpu& c, const Rule& rule) {
    return rule.library != nullptr ? resource_by_name(c, rule.type, rule.library)
                                   : resource_by_id(c, rule.type, rule.library_id);
}

float matrix_error(const render::Float3x4& a, const render::Float3x4& b) {
    float e = 0.0f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            e = std::max(e, std::fabs(a.m[i][j] - b.m[i][j]));
    return e;
}

// A RING group's rings as its object places them, those still there: the misc data (0x80113F18, 0x14 bytes) is
// {pattern, count, length or radius, amplitude, keep}; 0x801130A4 places ring i through M = T * Ry * Rx * Rz of the
// entry: 0 all at the entry, 1 a line M * (0, 0, -length / count * i), 2 a circle M * Ry(2 pi / count * i) *
// (0, 0, radius), 3 a wave M * (amplitude * sin(pi / count * i), 0, length / count * i). Each ring turns about Y only,
// by the entry's Y rotation (+0x1C), when it is created. A group that left its range keeps, in the slot's state object
// (0x80112588, the slot's +0x14), a bit per ring still there (present, 0x4C bit 1, and not bit 2); 0x801124C4 removes
// the others when the group comes back - so a collected ring has no proxy.
constexpr u32 kSlotState = 0x14u;
constexpr u32 kRingStateVtable = 0x80527DB8u;
constexpr u32 kRingMisc = 0x14u;
constexpr u32 kMaxRings = 128u;  // the state's four bit words
struct Ring {
    u32 index = 0;
    render::Float3x4 world;
};
// -1: not a group this rule reads.
int ring_group(Cpu& c, u32 entry, u32 state, std::array<Ring, kMaxRings>& rings) {
    const u32 misc = c.mem_read32(entry + kEntryMisc);
    if (c.mem_read32(entry + kEntryMiscLength) < kRingMisc || !pointer(misc))
        return -1;
    const s32 pattern = static_cast<s32>(c.mem_read32(misc));
    const s32 count = static_cast<s32>(c.mem_read32(misc + 4u));
    if (pattern < 0 || pattern > 3 || count <= 0 || count > static_cast<s32>(kMaxRings))
        return -1;
    if (state != 0u && (!pointer(state) || c.mem_read32(state) != kRingStateVtable))
        return -1;  // a state this rule does not know: no proxy rather than a wrong one
    const float length = read_f32(c, misc + 8u), amplitude = read_f32(c, misc + 12u);
    const render::Float3x4 m = entry_frame(c, entry, true);
    constexpr float kRadians = 3.14159274f / 180.0f;
    const float yaw = read_f32(c, entry + kEntryRotation + 4u) * kRadians;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float n = static_cast<float>(count);
    int placed = 0;
    for (s32 i = 0; i < count; ++i) {
        if (state != 0u &&
            (c.mem_read32(state + 4u + static_cast<u32>(i / 32) * 4u) & (1u << static_cast<u32>(i % 32))) == 0u)
            continue;
        const float k = static_cast<float>(i);
        float p[3] = {0.0f, 0.0f, 0.0f};
        if (pattern == 1) {
            p[2] = -(length / n) * k;
        } else if (pattern == 2) {
            const float a = 360.0f / n * kRadians * k;
            p[0] = length * std::sin(a);
            p[2] = length * std::cos(a);
        } else if (pattern == 3) {
            p[0] = amplitude * std::sin(180.0f / n * kRadians * k);
            p[2] = length / n * k;
        }
        Ring& ring = rings[placed++];
        ring.index = static_cast<u32>(i);
        ring.world = render::Float3x4{};
        for (int row = 0; row < 3; ++row)
            ring.world.m[row][3] = m.m[row][0] * p[0] + m.m[row][1] * p[1] + m.m[row][2] * p[2] + m.m[row][3];
        ring.world.m[0][0] = cy, ring.world.m[0][2] = sy;
        ring.world.m[1][1] = 1.0f;
        ring.world.m[2][0] = -sy, ring.world.m[2][2] = cy;
    }
    return placed;
}

}  // namespace

int FarObjects::rule_of(Cpu& c, u32 factory) {
    const auto it = factory_rules_.find(factory);
    if (it != factory_rules_.end())
        return it->second;
    const u32 text = c.mem_read32(factory);
    int found = -1;
    for (std::size_t r = 0; in_ram(text) && r < kRuleCount && found < 0; ++r)
        if (guest_string_equals(c, text, kRules[r].factory))
            found = static_cast<int>(r);
    factory_rules_.emplace(factory, found);
    return found;
}

const FarObjects::Model* FarObjects::clump_model(Cpu& c, u32 clump, s32 layer, s32 light_group) {
    const auto it = clumps_.find(clump);
    if (it != clumps_.end())
        return it->second.atomics.empty() ? nullptr : &it->second;
    Model model;
    const u32 root = c.mem_read32(clump + kObjectParent);
    const u32 head = clump + kClumpAtomics;
    u32 link = c.mem_read32(head);
    for (int guard = 0; pointer(link) && link != head && guard < 64; ++guard) {
        const u32 atomic = link - kAtomicInClumpLink;
        // The atomic's pose under the clump root: its frame's modelling matrices up to (not including) the root.
        render::Float3x4 pose;
        u32 frame = c.mem_read32(atomic + kObjectParent);
        for (int depth = 0; pointer(frame) && frame != root && depth < 32; ++depth) {
            pose = compose(rw_matrix(c, frame + kFrameModelling), pose);
            frame = c.mem_read32(frame + kObjectParent);
        }
        const float centre[3] = {read_f32(c, atomic + kAtomicSphere), read_f32(c, atomic + kAtomicSphere + 4u),
                                 read_f32(c, atomic + kAtomicSphere + 8u)};
        const float r = read_f32(c, atomic + kAtomicSphere + 12u);
        float placed[3];
        for (int i = 0; i < 3; ++i)
            placed[i] = pose.m[i][0] * centre[0] + pose.m[i][1] * centre[1] + pose.m[i][2] * centre[2] + pose.m[i][3];
        const float extent = std::sqrt(placed[0] * placed[0] + placed[1] * placed[1] + placed[2] * placed[2]) + r;
        if (std::isfinite(extent) && r >= 0.0f)
            model.radius = std::max(model.radius, extent);
        model.atomics.push_back(
            Atomic{atomic, c.mem_read32(atomic + kAtomicGeometry), pose, 0u, layer, light_group});
        link = c.mem_read32(link);
    }
    const auto [placed, inserted] = clumps_.emplace(clump, std::move(model));
    (void)inserted;
    return placed->second.atomics.empty() ? nullptr : &placed->second;
}

// A model entry (0x80162EB4): its parts (+4: pairs of words, the second the part's index; count +0xC) in the part
// table (+0, 0x40 bytes each). A part is its DFF (+4, a resource of the holder's type) placed by a matrix the game
// builds with RwMatrixRotate and RwMatrixTranslate: Z by +0x2C, then X by +0x24, then Y by +0x28 (degrees), then the
// translation at +0x18.
// Display layers (docs/DRAW_DISTANCE.md "Layers"): the stage draws its worlds in layers, 15 LandDispManager entries
// at 0x8051E328 (0x18 bytes: name, task name, flags, ..., the layer's RpWorld at +0x10), each in its own pass
// (0x800471BC). A model holder puts each part's instance in one layer's world (0x80163C14): its record's +0x38 bit 1
// set - the layer the part's geometry asks for, the first int user-data array named ATTR_WORLDPARM (0x80169748 over
// the clump's atomics, value 0 when none) through 0x8052C7E0; clear - 0x804C39BC[record +0x30]. Its draw applies the
// light group 0x804C397C[record +0x34] (0x8016454C stores it; the render callback 0x801645D0 applies it, 0x80048308).
constexpr u32 kLayerTable = 0x8051E328u;
constexpr u32 kLayerStride = 0x18u;
constexpr u32 kLayerFlags = 0x08u;  // bit 0: lit (0x80046D80 attaches the light manager's lights to its world)
constexpr u32 kLayerCount = 15u;
constexpr u32 kLayerByAttribute = 0x8052C7E0u;
constexpr u32 kLayerByRecord = 0x804C39BCu;
constexpr u32 kLightGroupByRecord = 0x804C397Cu;
constexpr u32 kRecordTables = 12u;  // the three tables' entries
constexpr u32 kRecordLayer = 0x30u;
constexpr u32 kRecordLightGroup = 0x34u;
constexpr u32 kRecordFlags = 0x38u;  // bit 1: the layer from the geometry's ATTR_WORLDPARM
constexpr u32 kSdaUserDataOffset = 0x6000u;  // RpGeometry user-data plugin offset {count, arrays}
constexpr u32 kLightGroups = 17u;           // 0x800483B8 ignores any other

// The layer a clump's geometry asks for: its first ATTR_WORLDPARM (0x80169748) through 0x8052C7E0 (0x80169734).
s32 worldparm_layer(Cpu& c, u32 clump) {
    u32 index = 0u;
    const u32 offset = c.mem_read32(c.gpr[13] + kSdaUserDataOffset);
    const u32 head = clump + kClumpAtomics;
    bool found = false;
    u32 link = c.mem_read32(head);
    for (int guard = 0; pointer(link) && link != head && guard < 64 && !found; ++guard) {
        const u32 geometry = c.mem_read32(link - kAtomicInClumpLink + kAtomicGeometry);
        const u32 plugin = pointer(geometry) ? geometry + offset : 0u;
        // 0x80453BE0 counts the arrays that have data; 0x80453C28 then takes the first `count` arrays.
        u32 count = 0u;
        const u32 total = pointer(plugin) ? c.mem_read32(plugin) : 0u;
        const u32 arrays = pointer(plugin) ? c.mem_read32(plugin + 4u) : 0u;
        for (u32 i = 0; i < total && i < 64u && pointer(arrays); ++i)
            count += c.mem_read32(arrays + 0x10u * i + 0xCu) != 0u ? 1u : 0u;
        for (u32 i = 0; i < count && !found; ++i) {
            const u32 array = arrays + 0x10u * i;
            if (c.mem_read32(array + 4u) == 1u &&
                guest_string_equals(c, c.mem_read32(array), "ATTR_WORLDPARM.ATTR_WORLDPARM")) {
                index = c.mem_read32(c.mem_read32(array + 0xCu));
                found = true;
            }
        }
        link = c.mem_read32(link);
    }
    return index < kRecordTables ? static_cast<s32>(c.mem_read32(kLayerByAttribute + 4u * index)) : -1;
}

s32 layer_of(Cpu& c, u32 record, u32 clump) {
    if ((c.mem_read32(record + kRecordFlags) & 2u) != 0u)
        return worldparm_layer(c, clump);
    const u32 index = c.mem_read32(record + kRecordLayer);
    return index < kRecordTables ? static_cast<s32>(c.mem_read32(kLayerByRecord + 4u * index)) : -1;
}

// An enemy's model (0x801A4654, from its kind's setup 0x801A40A0): the clone joins the layer its geometry asks for,
// layer 9 taken as 10; its light record (+0x18) gets group 8 (+0x3C, `li r0, 8` at 0x801A4724) and 0x800381C0
// installs the render callbacks that apply it before every draw (0x80038304 and its kin: [record] +0x24 ->
// 0x800483B8 / 0x80048308).
constexpr s32 kEnemyLayerAsked = 9, kEnemyLayerTaken = 10;
constexpr s32 kEnemyLightGroup = 8;

// A Black Arms soldier whose layout AppearType (misc word 9, kept at object +0x274 by 0x80198D90) is OFFSETPOS (4) or
// WARP (5) stays hidden after it spawns until the nearest player character enters its search cylinder (SearchWidth
// word 3 across, SearchHeight word 4 up, around its layout position): the appear check 0x8018E478 (`cmpwi 4 / 5` at
// 0x8018E490), the cylinder test 0x801A6C7C; WARP waits 1.0 s inside it more (0x8018E948). Any other type is shown
// at once (0x8018E5B4). The game never shows such a soldier from afar, so it has no stand-in.
constexpr u32 kSoldierAppearWord = 9u;
constexpr u32 kAppearOffsetPos = 4u, kAppearWarp = 5u;

// The switch table (docs/distant-rendering/hide_rules.md): [r13+0x32A0] (made by 0x800CBAB8 at stage load, all 0)
// points at 256 records of 8 bytes; 0x80169430(table, link) is the record's word +0 for links 0..0xFF, else 0. Laser
// strikes, cleared enemy groups and switches write it; a savepoint commits it, a restart clears it.
u32 switch_value(Cpu& c, u32 link) {
    const u32 holder = c.mem_read32(c.gpr[13] + 0x32A0u);
    const u32 table = pointer(holder) ? c.mem_read32(holder) : 0u;
    return pointer(table) && link < 0x100u ? c.mem_read32(table + 8u * link) : 0u;
}

// Classes that show or hide their model by the switch of their layout link (entry +0x22, 0x800CA238): the stand-in
// shows what the object would show if it existed now.
//   ART GADGET 0x800E52C4: link > 0 (object +0x80 bit 2, 0x800E4BFC) and switch == 1 -> hide-all 0x80162B74.
//   HINTRING 0x8010A660: link > 0 and switch == 1 -> hidden.
//   RING 0x80114068: link > 0 -> the group waits (its draw skips it) until switch == 1.
//   FallBuilding 0x800E3074: switch[link] == 1 -> it falls (no link test; the link copied to object +0x2C at
//     0x800E3AA0). Its saved state (slot +0x14) is {standing key, fallen key}: the respawn 0x800E3ECC builds the
//     standing model while word +4 is -1; the fall stores 1 there (0x800E3370), and it comes back fallen.
constexpr u32 kEntryLink = 0x22u;
bool hidden_by_switch(Cpu& c, const char* factory, u32 entry, u32 slot) {
    const u32 link = c.mem_read8(entry + kEntryLink);
    if (std::strcmp(factory, "ART GADGET") == 0 || std::strcmp(factory, "HINTRING") == 0)
        return link > 0u && switch_value(c, link) == 1u;
    if (std::strcmp(factory, "RING") == 0)
        return link > 0u && switch_value(c, link) != 1u;
    if (std::strcmp(factory, "FallBuilding") == 0) {
        const u32 state = c.mem_read32(slot + 0x14u);
        return switch_value(c, link) == 1u || (pointer(state) && c.mem_read32(state + 4u) != 0xFFFFFFFFu);
    }
    return false;
}

bool hidden_until_near(Cpu& c, const char* factory, u32 entry) {
    if (std::strcmp(factory, "BK_SOLDIER") != 0)
        return false;
    const u32 misc = c.mem_read32(entry + kEntryMisc);
    if (!in_ram(misc) || c.mem_read32(entry + kEntryMiscLength) < 4u * (kSoldierAppearWord + 1u))
        return false;
    const u32 appear = c.mem_read32(misc + 4u * kSoldierAppearWord);
    return appear == kAppearOffsetPos || appear == kAppearWarp;
}

s32 light_group_of(Cpu& c, u32 record) {
    const u32 index = c.mem_read32(record + kRecordLightGroup);
    const u32 group = index < kRecordTables ? c.mem_read32(kLightGroupByRecord + 4u * index) : kLightGroups;
    return group < kLightGroups ? static_cast<s32>(group) : -1;
}

const FarObjects::Model* FarObjects::entry_model(Cpu& c, s32 type, u32 library, u32 model_entry) {
    const u64 key = (u64{library} << 32) | model_entry;
    const auto cached = entries_.find(key);
    if (cached != entries_.end())
        return cached->second.atomics.empty() ? nullptr : &cached->second;
    Model model;
    const u32 parts = c.mem_read32(library + kLibraryParts);
    const u32 list = c.mem_read32(model_entry + kEntryPartList);
    const s32 part_count = static_cast<s32>(c.mem_read32(model_entry + kEntryPartCount));
    for (s32 k = 0; pointer(list) && pointer(parts) && k < part_count && k < 64; ++k) {
        const u32 part = parts + c.mem_read32(list + static_cast<u32>(k) * 8u + 4u) * kPartStride;
        const u32 dff = c.mem_read32(part + kPartModel);
        char file[64]{};
        for (u32 i = 0; in_ram(dff) && i + 1u < sizeof(file); ++i) {
            file[i] = static_cast<char>(c.mem_read8(dff + i));
            if (file[i] == '\0')
                break;
        }
        const u32 clump = file[0] != '\0' ? resource_by_name(c, type, file) : 0u;
        const Model* part_model = pointer(clump) ? clump_model(c, clump) : nullptr;
        if (part_model == nullptr)
            continue;
        const render::Float3x4 placement =
            part_matrix(read_f32(c, part + kPartRotation + 8u), read_f32(c, part + kPartRotation),
                        read_f32(c, part + kPartRotation + 4u), read_f32(c, part + kPartTranslation),
                        read_f32(c, part + kPartTranslation + 4u), read_f32(c, part + kPartTranslation + 8u));
        const float offset = std::sqrt(placement.m[0][3] * placement.m[0][3] + placement.m[1][3] * placement.m[1][3] +
                                       placement.m[2][3] * placement.m[2][3]);
        model.radius = std::max(model.radius, part_model->radius + offset);
        const s32 layer = layer_of(c, part, clump), light_group = light_group_of(c, part);
        for (const Atomic& a : part_model->atomics)
            model.atomics.push_back(
                Atomic{a.atomic, a.geometry, compose(placement, a.pose), part, layer < static_cast<s32>(kLayerCount) ? layer : -1,
                       light_group, c.mem_read32(part + 8u) != 0u});
    }
    const auto [placed, inserted] = entries_.emplace(key, std::move(model));
    (void)inserted;
    return placed->second.atomics.empty() ? nullptr : &placed->second;
}

// The layout entry's model under its class's rule: the model entry its constructor would build.
const FarObjects::Model* FarObjects::model_for(Cpu& c, std::size_t rule_index, u32 library, u32 entry) {
    const Rule& rule = kRules[rule_index];
    const u32 entries = c.mem_read32(library + kLibraryEntries);
    const s32 entry_count = std::min(static_cast<s32>(c.mem_read32(library + kLibraryEntryCount)), 4096);
    if (!pointer(entries))
        return nullptr;
    u32 name = 0u;
    if (rule.selection == Selection::GadgetRecord) {
        // 0x800CA1A4(slot, &id, 4): the misc data is exactly one word, the gadget record's id.
        if (c.mem_read32(entry + kEntryMiscLength) < 4u || !in_ram(c.mem_read32(entry + kEntryMisc)))
            return nullptr;
        const u32 id = c.mem_read32(c.mem_read32(entry + kEntryMisc));
        const u32 records = c.mem_read32(library + kLibraryRecords);
        const s32 record_count = std::min(static_cast<s32>(c.mem_read32(library + kLibraryCount)), 4096);
        // The last record with the id, as 0x80208B78 finds it.
        for (s32 r = 0; pointer(records) && r < record_count; ++r) {
            const u32 record = records + static_cast<u32>(r) * kRecordStride;
            if (c.mem_read32(record + kRecordId) == id)
                name = c.mem_read32(record + kRecordModelName);
        }
        if (name == 0u)
            return nullptr;
    }
    u32 key = rule.key;
    if (rule.selection == Selection::MiscKey) {
        if (c.mem_read32(entry + kEntryMiscLength) < 4u || !in_ram(c.mem_read32(entry + kEntryMisc)))
            return nullptr;
        key = c.mem_read32(c.mem_read32(entry + kEntryMisc));
    }
    for (s32 e = 0; e < entry_count; ++e) {
        const u32 candidate = entries + static_cast<u32>(e) * kEntryStride;
        const bool chosen = rule.selection == Selection::GadgetRecord
                                ? guest_strings_equal(c, c.mem_read32(candidate), name)
                                : c.mem_read32(candidate + kEntryKey) == key;
        if (chosen)
            return entry_model(c, rule.type, library, candidate);
    }
    return nullptr;
}

// Measuring (SHADOW_DISTANCE_TRACE=1; docs/DRAW_DISTANCE.md "Landscape blocks"). TBlockManage ([0x8051DFA8]): the
// stage's BDT at +0x34 (count +0x40, 0x20-byte records {block, min, max, pad} from +0x44), the test point [+0x3C],
// the mask buffer index +0x44, the masks at +0x48 + 0x150 i (s16 per block), the resident list at +0x828 (s16, -1
// ends). The land manager 0x80572560: 32 slots of 0xC0 {state +0, block +0xBC}.
void FarObjects::distance_trace(Cpu& c, const render::Camera& camera) {
    ++distance_frames_;
    const u32 blocks = c.mem_read32(0x8051DFA8u);
    if (!pointer(blocks))
        return;
    const u32 table = c.mem_read32(blocks + 0x34u);
    const u32 count = pointer(table) ? c.mem_read32(table + 0x40u) : 0u;
    float list_eye[3] = {0, 0, 0};
    const u32 list = c.mem_read32(c.gpr[13] + 0x2B34u);
    const u32 data = pointer(list) ? c.mem_read32(list + 0xCu) : 0u;
    const u32 object = pointer(data) ? c.mem_read32(data) : 0u;
    for (int i = 0; i < 3 && pointer(object); ++i)
        list_eye[i] = read_f32(c, object + 8u + 4u * static_cast<u32>(i));
    // The in-stage RwCamera's own frame (its LTM position), not the render camera: 0x8056FF1C +0 the camera, +4 its
    // frame, LTM at +0x50 (position +0x30).
    float eye[3] = {camera.frame.m[0][3], camera.frame.m[1][3], camera.frame.m[2][3]};
    const u32 rw_camera = c.mem_read32(0x8056FF1Cu);
    const u32 rw_frame = pointer(rw_camera) ? c.mem_read32(rw_camera + 4u) : 0u;
    if (pointer(rw_frame))
        for (int i = 0; i < 3; ++i)
            eye[i] = read_f32(c, rw_frame + 0x80u + 4u * static_cast<u32>(i));
    const u32 point = c.mem_read32(blocks + 0x3Cu);
    const auto predict = [&](const float* p, std::array<bool, 168>& in) {
        for (u32 r = 0; r < count && r < 512u; ++r) {
            const u32 record = table + 0x44u + 0x20u * r;
            const u32 block = c.mem_read32(record);
            bool inside = block < 168u;
            for (u32 i = 0; i < 3u && inside; ++i)
                inside = read_f32(c, record + 4u + 4u * i) <= p[i] && p[i] < read_f32(c, record + 0x10u + 4u * i);
            if (inside)
                in[block] = true;
        }
    };
    std::array<bool, 168> by_list{}, by_camera{};
    predict(list_eye, by_list);
    predict(eye, by_camera);
    const u32 buffer = c.mem_read32(blocks + 0x44u);
    bool same_list = true, same_camera = true;
    for (u32 b = 0; b < 168u; ++b) {
        const bool game = static_cast<s16>(c.mem_read16(blocks + 0x48u + 0x150u * buffer + 2u * b)) != 0;
        same_list = same_list && game == by_list[b];
        same_camera = same_camera && game == by_camera[b];
    }
    if (count != 0u && !pointer(point)) {  // without a set point the game tests the list's eye
        ++mask_frames_;
        mask_by_list_ += same_list ? 1u : 0u;
        mask_by_camera_ += same_camera ? 1u : 0u;
    }
    std::array<u16, 32> resident{};
    for (u32 i = 0; i < 32u; ++i) {
        const s16 b = static_cast<s16>(c.mem_read16(blocks + 0x828u + 2u * i));
        resident[i] = b < 0 ? 0xFFFFu : static_cast<u16>(b);
        if (b < 0)
            break;
    }
    // Load latency and hysteresis: frames from a block entering the current mask to a land slot holding it in state 5
    // (loaded), and from leaving the mask to no slot holding it (unloaded).
    std::array<bool, 168> held{}, loaded{};
    for (u32 i = 0; i < 32u; ++i) {
        const u32 slot = 0x80572560u + 0xC0u * i;
        const u32 state = c.mem_read32(slot), block = c.mem_read32(slot + 0xBCu);
        if (state != 0u && block < 168u) {
            held[block] = true;
            loaded[block] = loaded[block] || state == 5u;
        }
    }
    const u32 other = buffer == 0u ? 1u : 0u;
    for (u32 b = 0; b < 168u; ++b) {
        const bool in = static_cast<s16>(c.mem_read16(blocks + 0x48u + 0x150u * buffer + 2u * b)) != 0;
        const bool in_other = static_cast<s16>(c.mem_read16(blocks + 0x48u + 0x150u * other + 2u * b)) != 0;
        if (in && block_in_mask_[b] == 0u) {
            block_in_mask_[b] = distance_frames_ + 1u;
            block_out_of_mask_[b] = 0u;
        }
        if (!in && block_in_mask_[b] != 0u) {
            block_in_mask_[b] = 0u;
            block_out_of_mask_[b] = distance_frames_ + 1u;
        }
        if (loaded[b] && !block_resident_[b]) {
            block_resident_[b] = true;
            std::fprintf(stderr, "[distance] %llu block %u loaded: %lld frames after it entered the mask\n",
                         static_cast<unsigned long long>(distance_frames_), b,
                         block_in_mask_[b] != 0u ? static_cast<long long>(distance_frames_ + 1u - block_in_mask_[b]) : -1ll);
        }
        if (!held[b] && block_resident_[b]) {
            block_resident_[b] = false;
            std::fprintf(stderr, "[distance] %llu block %u unloaded: %lld frames after it left the mask (in the other "
                         "buffer now: %d)\n", static_cast<unsigned long long>(distance_frames_), b,
                         block_out_of_mask_[b] != 0u ? static_cast<long long>(distance_frames_ + 1u - block_out_of_mask_[b]) : -1ll,
                         in_other ? 1 : 0);
        }
    }
    if (resident != last_resident_) {
        last_resident_ = resident;
        std::fprintf(stderr, "[distance] %llu drawn blocks:", static_cast<unsigned long long>(distance_frames_));
        for (u16 b : resident) {
            if (b == 0xFFFFu)
                break;
            std::fprintf(stderr, " %u", b);
        }
        std::fprintf(stderr, "; slots (state:block):");
        for (u32 i = 0; i < 32u; ++i) {
            const u32 slot = 0x80572560u + 0xC0u * i;
            if (c.mem_read32(slot) != 0u)
                std::fprintf(stderr, " %u:%u", c.mem_read32(slot), c.mem_read32(slot + 0xBCu));
        }
        std::fprintf(stderr, "; list eye %.0f %.0f %.0f camera %.0f %.0f %.0f point %s; mask = list eye's %d camera's %d\n",
                     static_cast<double>(list_eye[0]), static_cast<double>(list_eye[1]), static_cast<double>(list_eye[2]),
                     static_cast<double>(eye[0]), static_cast<double>(eye[1]), static_cast<double>(eye[2]),
                     pointer(point) ? "set" : "none", same_list ? 1 : 0, same_camera ? 1 : 0);
    }
    u32 far_bits;
    std::memcpy(&far_bits, &camera.far_plane, 4);
    if (far_bits != last_far_bits_ && (distance_frames_ % 10u == 0u || distance_frames_ < 4u)) {
        last_far_bits_ = far_bits;
        std::fprintf(stderr, "[distance] %llu camera near %.3f far %.3f\n", static_cast<unsigned long long>(distance_frames_),
                     static_cast<double>(camera.near_plane), static_cast<double>(camera.far_plane));
    }
    if (distance_frames_ % 600u == 1u)
        std::fprintf(stderr, "[distance] %llu masks matched by the camera position: %llu of %llu frames (the list at "
                     "r13+0x2B34 is the camera list 0x8056FED4; its element +8 is the camera eye)\n",
                     static_cast<unsigned long long>(distance_frames_), static_cast<unsigned long long>(mask_by_camera_),
                     static_cast<unsigned long long>(mask_frames_));
}

void FarObjects::collect(Cpu& c, const render::Camera& camera, float focal, std::vector<Proxy>& out) {
    out.clear();
    palette_.clear();
    // Tracing a class at draw distance 1 (the original): everything runs for the trace, nothing is drawn.
    if (distance_trace_)
        distance_trace(c, camera);
    if (!enabled_ && trace_class_ == nullptr)
        return;
    struct DrawNothing {
        std::vector<Proxy>& out;
        bool enabled;
        ~DrawNothing() {
            if (!enabled)
                out.clear();
        }
    } draw_nothing{out, enabled_};
    ++stats_.frames;
    const u32 manager = c.mem_read32(c.gpr[13] + kSdaManagerHolder);
    if (!pointer(manager))
        return;
    const u32 factories = c.mem_read32(manager + kManagerFactories);
    const u32 slots = c.mem_read32(manager + kManagerSlots);
    if (!pointer(factories) || !pointer(slots))
        return;
    // Each class's library, found as the game finds it; a changed one (a new stage) drops every resolved model.
    std::array<u32, kMaxRules> libraries{};
    for (std::size_t r = 0; r < kRuleCount; ++r)
        libraries[r] = library_of(c, kRules[r]);
    if (libraries != libraries_) {
        clumps_.clear();
        entries_.clear();
        factory_rules_.clear();
        shown_.clear();
        handover_.clear();
        late_.clear();
        enemies_.clear();
        libraries_ = libraries;
        ++stage_epoch_;
    }
    stats_.class_count = kRuleCount;
    for (std::size_t r = 0; r < kRuleCount; ++r) {
        stats_.classes[r].name = kRules[r].factory;
        stats_.classes[r].verified = kRules[r].verified;
    }
    const bool restricted = (c.mem_read32(manager + kManagerRestricted) & 1u) != 0u ||
                            (c.mem_read32(c.gpr[13] + kSdaEventFlags) & 1u) != 0u;

    if (census_frame_ != 0u && stats_.frames >= census_frame_ && stats_.frames <= census_last_ &&
        (stats_.frames - census_frame_) % census_step_ == 0u)
        census(c, factories, slots, restricted, camera);
    const bool tracing = trace_class_ != nullptr && stats_.frames >= trace_first_ && stats_.frames <= trace_last_;
    if (tracing)
        for (const Real& real : real_this_frame_) {
            bool traced_geometry = false;
            for (const auto& [key, shown] : shown_)
                for (const Atomic& a : shown.model->atomics)
                    traced_geometry = traced_geometry || (a.geometry == real.geometry &&
                                                          std::strcmp(kRules[shown.rule].factory, trace_class_) == 0);
            if (traced_geometry)
                std::fprintf(stderr, "[far-trace] %llu the game draws geometry 0x%08X at %.1f %.1f %.1f\n",
                             static_cast<unsigned long long>(stats_.frames), real.geometry,
                             static_cast<double>(real.world.m[0][3]), static_cast<double>(real.world.m[1][3]),
                             static_cast<double>(real.world.m[2][3]));
        }
    // Measuring: SHADOW_FAR_WATCH=<atomic>,<atomic>... - each traced frame, the sector renderer's gates for those
    // atomics (0x80460418: tied to a sector, render flag 0x04, renderFrame != RwGlobals' (r13+0x615C +8), the world
    // bounding sphere 0x80457720 in the frustum 0x80486718).
    static const char* watch = std::getenv("SHADOW_FAR_WATCH");
    if (tracing) {  // the light manager's current group and its directional light's frame
        const u32 lights = c.mem_read32(c.gpr[13] + 0x2C30u);
        const u32 wrapper = pointer(lights) ? c.mem_read32(lights + 4u) : 0u;
        const u32 light = pointer(wrapper) ? c.mem_read32(wrapper + 8u) : 0u;
        const u32 frame = pointer(light) ? c.mem_read32(light + 4u) : 0u;
        if (pointer(frame)) {
            const u32 parent = c.mem_read32(frame + 4u);
            std::fprintf(stderr,
                         "[far-light] %llu current group %d requested %d light 1 colour %.3f %.3f %.3f frame 0x%08X "
                         "parent 0x%08X root 0x%08X modelling at %.4f %.4f %.4f ltm at %.4f %.4f %.4f\n",
                         static_cast<unsigned long long>(stats_.frames),
                         static_cast<int>(static_cast<s8>(c.mem_read8(lights + 0x454u))),
                         static_cast<int>(static_cast<s8>(c.mem_read8(lights + 0x456u))),
                         static_cast<double>(read_f32(c, light + 0x18u)), static_cast<double>(read_f32(c, light + 0x1Cu)),
                         static_cast<double>(read_f32(c, light + 0x20u)), frame, parent, c.mem_read32(frame + 0xA0u),
                         static_cast<double>(read_f32(c, frame + 0x30u)), static_cast<double>(read_f32(c, frame + 0x34u)),
                         static_cast<double>(read_f32(c, frame + 0x38u)), static_cast<double>(read_f32(c, frame + 0x70u)),
                         static_cast<double>(read_f32(c, frame + 0x74u)), static_cast<double>(read_f32(c, frame + 0x78u)));
        }
    }
    // Measuring: SHADOW_FAR_MATERIAL=<material> - its RpUVAnim plugin data (material + [0x805F2544]; raw words, the
    // layout not reversed) and the uv matrix its MatFX data (material + [0x805F2750], word +0, stored by ApplyUpdate
    // 0x8045567C -> 0x804A3C30) points at, each traced frame (docs/DISTANT_RENDERING.md 6b).
    static const char* material_text = std::getenv("SHADOW_FAR_MATERIAL");
    if (tracing && material_text != nullptr) {
        const u32 material = static_cast<u32>(std::strtoul(material_text, nullptr, 16));
        const u32 uv = c.mem_read32(material + c.mem_read32(0x805F2544u));
        const u32 matfx = c.mem_read32(material + c.mem_read32(0x805F2750u));
        std::fprintf(stderr, "[far-uv] %llu plugin:", static_cast<unsigned long long>(stats_.frames));
        for (u32 o = 0; o < 0x20u && pointer(uv); o += 4u)
            std::fprintf(stderr, " %08X", c.mem_read32(uv + o));
        const u32 matrix = pointer(matfx) ? c.mem_read32(matfx) : 0u;
        std::fprintf(stderr, "; matrix at 0x%08X:", matrix);
        for (u32 o = 0; o < 0x40u && pointer(matrix); o += 4u)
            std::fprintf(stderr, " %.3f", static_cast<double>(read_f32(c, matrix + o)));
        std::fprintf(stderr, "\n");
    }
    if (tracing && watch != nullptr) {
        const u32 globals = c.mem_read32(c.gpr[13] + 0x615Cu);
        for (const char* p = watch; *p != '\0';) {
            char* end = nullptr;
            const u32 atomic = static_cast<u32>(std::strtoul(p, &end, 16));
            if (end == p)
                break;
            p = *end == ',' ? end + 1 : end;
            const u32 frame = c.mem_read32(atomic + 4u);
            const u32 ties = c.mem_read32(atomic + 0x64u);
            u32 tie_count = 0u;
            for (u32 link = ties; pointer(link) && link != atomic + 0x64u && tie_count < 32u; link = c.mem_read32(link))
                ++tie_count;
            std::fprintf(stderr,
                         "[far-watch] %llu atomic 0x%08X flags %02X private %02X renderFrame %04X global %04X ties %u "
                         "sphere %.1f %.1f %.1f r %.1f interp %08X frame 0x%08X frame flags %02X/%02X ltm %.1f %.1f %.1f\n",
                         static_cast<unsigned long long>(stats_.frames), atomic, c.mem_read8(atomic + 2u),
                         c.mem_read8(atomic + 3u), c.mem_read16(atomic + 0x60u),
                         pointer(globals) ? c.mem_read16(globals + 8u) : 0u, tie_count,
                         static_cast<double>(read_f32(c, atomic + 0x2Cu)), static_cast<double>(read_f32(c, atomic + 0x30u)),
                         static_cast<double>(read_f32(c, atomic + 0x34u)), static_cast<double>(read_f32(c, atomic + 0x38u)),
                         c.mem_read32(atomic + 0x4Cu), frame, pointer(frame) ? c.mem_read8(frame + 2u) : 0u,
                         pointer(frame) ? c.mem_read8(frame + 3u) : 0u,
                         pointer(frame) ? static_cast<double>(read_f32(c, frame + 0x50u + 0x30u)) : 0.0,
                         pointer(frame) ? static_cast<double>(read_f32(c, frame + 0x50u + 0x34u)) : 0.0,
                         pointer(frame) ? static_cast<double>(read_f32(c, frame + 0x50u + 0x38u)) : 0.0);
        }
    }
    if (tracing)
        for (const Real& real : transparent_this_frame_)
            for (const auto& [key, shown] : shown_)
                for (const Atomic& a : shown.model->atomics)
                    if (a.geometry == real.geometry && std::strcmp(kRules[shown.rule].factory, trace_class_) == 0)
                        std::fprintf(stderr, "[far-trace] %llu the game draws geometry 0x%08X at %.1f %.1f %.1f with every material at alpha 0\n",
                                     static_cast<unsigned long long>(stats_.frames), real.geometry,
                                     static_cast<double>(real.world.m[0][3]), static_cast<double>(real.world.m[1][3]),
                                     static_cast<double>(real.world.m[2][3]));
    transparent_this_frame_.clear();
    // The game's draws this frame by geometry, for drawn_by_game (sorted in place: no allocation).
    std::sort(real_this_frame_.begin(), real_this_frame_.end(),
              [](const Real& a, const Real& b) { return a.geometry < b.geometry; });

    float eye[3], right[3], up[3], forward[3];
    for (int i = 0; i < 3; ++i) {
        right[i] = camera.frame.m[i][0];
        up[i] = camera.frame.m[i][1];
        forward[i] = camera.frame.m[i][2];
        eye[i] = camera.frame.m[i][3];
    }
    const float wx = camera.view_window_x, wy = camera.view_window_y;
    const float slack_x = std::sqrt(1.0f + wx * wx), slack_y = std::sqrt(1.0f + wy * wy);
    std::array<Ring, kMaxRings> rings;

    for (u32 s = 0; s < kSlotCount; ++s) {
        const u32 slot = slots + s * kSlotStride;
        const u32 entry = c.mem_read32(slot);
        if (!pointer(entry))
            continue;
        const s32 type = static_cast<s32>(c.mem_read32(slot + kSlotFactoryIndex));
        const u32 flags = c.mem_read32(entry + kEntryFlags);
        const bool candidate = type >= 0 && (flags & kDormant) != 0u && (flags & (kLive | kHeld)) == 0u &&
                               (!restricted || (flags & kRunsRestricted) != 0u);
        // Tracing (SHADOW_FAR_TRACE): every entry of the traced class near the eye, candidate or not, each frame.
        const bool traced = tracing && type >= 0 && trace_entry(c, factories, type, entry, eye);
        if (traced && !candidate) {
            // Whether the game drew this entry's model this frame (while not a candidate, it is the game's).
            const u32 factory = c.mem_read32(factories + static_cast<u32>(type) * 4u);
            const int rule = pointer(factory) ? rule_of(c, factory) : -1;
            const u32 library = rule >= 0 ? libraries[static_cast<std::size_t>(rule)] : 0u;
            const Model* model = rule >= 0 && pointer(library) && kRules[rule].selection != Selection::RingGroup
                                     ? model_for(c, static_cast<std::size_t>(rule), library, entry)
                                     : nullptr;
            char part_text[96];
            const char* drawn = "model unknown";
            float sx = 0.0f, sy = 0.0f;  // where it is on screen: -1..1 across the view, up positive
            if (model != nullptr) {
                float reach = 1.0f;
                const render::Float3x4 frame = rule_frame(c, entry, kRules[rule], &reach);
                // Once: what each part is (its atomic, geometry and textures).
                static bool described = false;
                if (!described) {
                    described = true;
                    // The light manager ([r13+0x2C30]): its lights (wrapper +8 the RpLight) and group records
                    // (+0x20 + 0x34 g), and each display layer's world (0x8051E328 + 0x18 i, +0x10) with its lights.
                    const u32 lights = c.mem_read32(c.gpr[13] + 0x2C30u);
                    for (u32 k = 0; pointer(lights) && k < 8u; ++k) {
                        const u32 wrapper = c.mem_read32(lights + 4u * k);
                        const u32 light = pointer(wrapper) ? c.mem_read32(wrapper + 8u) : 0u;
                        if (pointer(light))
                            std::fprintf(stderr, "[far-light] manager light %u: wrapper 0x%08X light 0x%08X subtype %u flags %02X "
                                         "frame 0x%08X kind %d\n", k, wrapper, light, c.mem_read8(light + 1u),
                                         c.mem_read8(light + 2u), c.mem_read32(light + 4u),
                                         static_cast<int>(static_cast<s8>(c.mem_read8(wrapper))));
                    }
                    for (u32 g = 0; pointer(lights) && g < 17u; ++g) {
                        std::fprintf(stderr, "[far-light] group %u:", g);
                        for (u32 o = 0; o < 0x34u; o += 4u)
                            std::fprintf(stderr, " %08X", c.mem_read32(lights + 0x20u + 0x34u * g + o));
                        std::fprintf(stderr, "\n");
                    }
                    for (u32 i = 0; i < 15u; ++i) {
                        const u32 world = c.mem_read32(0x8051E328u + 0x18u * i + 0x10u);
                        std::fprintf(stderr, "[far-light] layer %u world 0x%08X lights:", i, world);
                        const u32 head = world + 0x3Cu;
                        u32 link = pointer(world) ? c.mem_read32(head) : 0u;
                        for (int guard = 0; pointer(link) && link != head && guard < 16; ++guard) {
                            std::fprintf(stderr, " 0x%08X", link - 0x34u);
                            link = c.mem_read32(link);
                        }
                        std::fprintf(stderr, "\n");
                    }
                    u32 index = 0u;
                    for (const Atomic& a : model->atomics) {
                        traced_geometries_.push_back(a.geometry);
                        std::fprintf(stderr, "[far-trace] %s part %u: atomic 0x%08X geometry 0x%08X (flags %08X) frame 0x%08X "
                                     "record 0x%08X (+30 %u +34 %u +38 %08X) textures:",
                                     trace_class_, index++, a.atomic, a.geometry, c.mem_read32(a.geometry + 8u),
                                     c.mem_read32(a.atomic + 4u), a.part, pointer(a.part) ? c.mem_read32(a.part + 0x30u) : 0u,
                                     pointer(a.part) ? c.mem_read32(a.part + 0x34u) : 0u,
                                     pointer(a.part) ? c.mem_read32(a.part + 0x38u) : 0u);
                        const u32 mesh = c.mem_read32(a.geometry + 0x54u);
                        const u32 meshes = pointer(mesh) ? c.mem_read16(mesh + 4u) : 0u;
                        for (u32 j = 0; j < meshes && j < 16u; ++j) {
                            const u32 material = c.mem_read32(mesh + 0x10u + 12u * j + 8u);
                            const u32 texture = pointer(material) ? c.mem_read32(material) : 0u;
                            char name[33] = "-";
                            for (u32 i = 0; pointer(texture) && i < 32u; ++i)
                                if ((name[i] = static_cast<char>(c.mem_read8(texture + 0x10u + i))) == '\0')
                                    break;
                            std::fprintf(stderr, " %s (colour %08X)", name,
                                         pointer(material) ? c.mem_read32(material + 4u) : 0u);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
                // Part by part: which of the model's atomics the game drew this frame (1) or not (0).
                std::snprintf(part_text, sizeof(part_text), "the game draws parts");
                for (const Atomic& a : model->atomics) {
                    const std::size_t used = std::strlen(part_text);
                    std::snprintf(part_text + used, sizeof(part_text) - used, " %c",
                                  atomic_drawn(compose(frame, a.pose), a.geometry, kSamePlace) ? '1' : '0');
                }
                drawn = part_text;
                const float d[3] = {frame.m[0][3] - eye[0], frame.m[1][3] - eye[1], frame.m[2][3] - eye[2]};
                const float z = d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2];
                if (z > 0.0f) {
                    sx = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / (z * wx);
                    sy = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / (z * wy);
                }
            }
            const float ex = read_f32(c, entry) - eye[0], ey = read_f32(c, entry + 4u) - eye[1],
                        ez = read_f32(c, entry + 8u) - eye[2];
            std::fprintf(stderr,
                         "[far-trace] %llu entry 0x%08X flags 0x%08X not a candidate, %s, screen %.3f %.3f, distance "
                         "%.1f range %u tick %llu\n",
                         static_cast<unsigned long long>(stats_.frames), entry, flags, drawn, static_cast<double>(sx),
                         static_cast<double>(sy), static_cast<double>(std::sqrt(ex * ex + ey * ey + ez * ez)),
                         100u * c.mem_read8(entry + kEntryRange),
                         static_cast<unsigned long long>(c.game_clock().now()));
        }
        if (!candidate && (type < 0 || (flags & kLive) == 0u))
            continue;
        const u32 factory = c.mem_read32(factories + static_cast<u32>(type) * 4u);
        const int rule = pointer(factory) ? rule_of(c, factory) : -1;
        // An object whose holder is fading its model - in after it spawned, out at the range's edge, or faded out and
        // about to go - is drawn by its proxy at full opacity meanwhile: the far view neither thins out nor blinks
        // where the game fades (the fade's first frame draws nothing, then 0.15 s of fading, then the model at alpha 0
        // until the object leaves; docs/DRAW_DISTANCE.md "The game's fade"). Its own (faded) draw does not hide it.
        bool fading = false;
        if (rule >= 0 && kRules[rule].holder != 0u) {
            const u32 object = c.mem_read32(slot + kSlotObject);
            const u32 holder = pointer(object) && c.mem_read32(object + kObjectSlot) == slot
                                   ? c.mem_read32(object + kRules[rule].holder)
                                   : 0u;
            fading = pointer(holder) && (c.mem_read32(holder + kHolderFlags) & (kHolderFading | kHolderFadedOut)) != 0u;
        }
        if (!candidate && !fading)
            continue;
        if (candidate)
            ++stats_.candidates;
        if (rule < 0)
            continue;
        ClassStats& cls = stats_.classes[static_cast<std::size_t>(rule)];
        if (candidate)
            ++cls.candidates;
        else
            ++cls.faded;
        if (!kRules[rule].verified && !validate_)
            continue;
        if (hidden_by_switch(c, kRules[rule].factory, entry, slot)) {  // the object would hide its model now
            ++cls.hidden;
            continue;
        }
        const u32 library = libraries[static_cast<std::size_t>(rule)];
        // Inside the view (sphere against the view's slopes), at least kMinPixels across, and within the slider's cap.
        const auto visible = [&](const render::Float3x4& frame, float r) {
            const float d[3] = {frame.m[0][3] - eye[0], frame.m[1][3] - eye[1], frame.m[2][3] - eye[2]};
            const float z = d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2];
            const float x = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
            const float y = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
            if (z + r <= camera.near_plane || z - r >= camera.far_plane)
                return false;
            if (std::fabs(x) - r * slack_x > z * wx || std::fabs(y) - r * slack_y > z * wy)
                return false;
            if (z > r && r * focal < kMinPixels * z)
                return false;
            if (range_scale_ > 0.0f) {
                const float limit = range_scale_ * 100.0f * static_cast<float>(c.mem_read8(entry + kEntryRange));
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > limit * limit)
                    return false;
            }
            return true;
        };
        const auto show = [&](u64 key, const render::Float3x4& frame, const Model* model) {
            if (!fading && all_drawn_by_game(frame, *model, kSamePlace)) {
                // The game still draws it this frame (an object outlives its entry's 0x2 by a frame): no double.
                if (++cls.doubled <= 4u && validate_)
                    std::fprintf(stderr, "[far-objects] %s drawn by the game: frame %llu entry 0x%08X flags 0x%08X\n",
                                 cls.name, static_cast<unsigned long long>(stats_.frames), entry,
                                 c.mem_read32(entry + kEntryFlags));
                return false;
            }
            const u32 state = c.mem_read32(slot + kSlotState);
            shown_now_[key] = Shown{frame, model, static_cast<std::size_t>(rule), 0u, key, state,
                                    pointer(state) ? c.mem_read32(state + 4u) : 0u};
            // Part by part: a part the game already draws here is the game's (a dash panel's UV-animated arrow draws
            // a frame before its base).
            for (const Atomic& a : model->atomics) {
                const render::Float3x4 world = compose(frame, a.pose);
                if (!fading && atomic_drawn(world, a.geometry, kSamePlace))
                    continue;
                out.push_back(Proxy{a.atomic, world, 0u, 0u, a.layer, a.light_group, a.uva});
                ++stats_.drawn_atomics;
            }
            // The model the object draws itself, camera-facing at its position.
            if (kRules[rule].camera_model != nullptr) {
                constexpr s32 kCameraRenderGroup = 4;  // 0x800477A8's default (+0x455), set by CameraRender
                const u32 clump = resource_by_name(c, 0, kRules[rule].camera_model);
                const Model* facing = pointer(clump) ? clump_model(c, clump, kOpaqueLevel, kCameraRenderGroup) : nullptr;
                const float p[3] = {frame.m[0][3], frame.m[1][3], frame.m[2][3]};
                const render::Float3x4 at = camera_facing(c, p);
                for (std::size_t i = 0; facing != nullptr && i < facing->atomics.size(); ++i) {
                    const Atomic& a = facing->atomics[i];
                    const render::Float3x4 world = compose(at, a.pose);
                    if (!fading && atomic_drawn(world, a.geometry, kSamePlace))
                        continue;
                    out.push_back(Proxy{a.atomic, world, 0u, 0u, a.layer, a.light_group, a.uva});
                    ++stats_.drawn_atomics;
                }
            }
            return true;
        };
        if (kRules[rule].selection == Selection::RingGroup) {
            // The rings draw from their SetRing task under render level 2 (0x80112C70), light group 5 (`li r4, 5` at
            // 0x80112CA4; 0x800477A8 restores the default group 4 after).
            constexpr s32 kRingLightGroup = 5;
            const Model* model = pointer(library) ? clump_model(c, library, kOpaqueLevel, kRingLightGroup) : nullptr;
            if (model == nullptr)
                continue;
            const int count = ring_group(c, entry, c.mem_read32(slot + kSlotState), rings);
            if (count < 0)
                continue;
            ++stats_.resolved;
            ++cls.resolved;
            bool drawn = false;
            for (int i = 0; i < count; ++i)
                if (visible(rings[i].world, model->radius) &&
                    show((u64{rings[i].index + 1u} << 32) | entry, rings[i].world, model))
                    drawn = true;
            if (drawn) {
                ++stats_.drawn_entries;
                ++cls.drawn_entries;
            }
            continue;
        }
        if (kRules[rule].selection == Selection::Enemy) {
            if (hidden_until_near(c, kRules[rule].factory, entry)) {
                ++cls.hidden;
                continue;
            }
            Enemy* enemy = enemy_for(c, static_cast<std::size_t>(rule));
            if (enemy == nullptr)
                continue;
            ++stats_.resolved;
            ++cls.resolved;
            float reach = 1.0f;
            const render::Float3x4 frame = rule_frame(c, entry, kRules[rule], &reach);
            if (!visible(frame, enemy->model.radius))
                continue;
            if (!fading && drawn_by_game(frame, enemy->model)) {
                ++cls.doubled;
                continue;
            }
            shown_now_[entry] = Shown{frame, &enemy->model, static_cast<std::size_t>(rule), 0u, entry, 0u, 0u};
            push_enemy(c, *enemy, frame, out);
            ++stats_.drawn_entries;
            ++cls.drawn_entries;
            continue;
        }
        const Model* model = pointer(library) ? model_for(c, static_cast<std::size_t>(rule), library, entry) : nullptr;
        if (model == nullptr)
            continue;
        ++stats_.resolved;
        ++cls.resolved;
        float reach = 1.0f;
        const render::Float3x4 frame = rule_frame(c, entry, kRules[rule], &reach);
        const bool seen = visible(frame, model->radius * reach);
        const bool drawn = seen && show(entry, frame, model);
        if (traced) {
            const float dx = frame.m[0][3] - eye[0], dy = frame.m[1][3] - eye[1], dz = frame.m[2][3] - eye[2];
            std::fprintf(stderr, "[far-trace] %llu entry 0x%08X flags 0x%08X distance %.1f proxy %s\n",
                         static_cast<unsigned long long>(stats_.frames), entry, flags,
                         static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz)),
                         drawn ? "drawn" : seen ? "not drawn (the game draws it)" : "not drawn (outside the view)");
        }
        if (!drawn)
            continue;
        ++stats_.drawn_entries;
        ++cls.drawn_entries;
    }
    // Entries shown last frame that are no candidates any more: spawned (0x2 set: their proxy stays until the game
    // draws its model, a few frames at most) or destroyed (0x1 cleared: gone with the game's object).
    for (const auto& [key, shown] : shown_) {
        if (shown_now_.count(key) != 0u || handover_.count(key) != 0u)
            continue;
        const u32 flags = c.mem_read32(static_cast<u32>(key) + kEntryFlags);
        const bool enemy = kRules[shown.rule].selection == Selection::Enemy;
        const bool complete = enemy ? match_real(shown)
                                    : all_drawn_by_game(shown.frame, *shown.model, kHandoverDistance) && match_real(shown);
        if ((flags & kDormant) != 0u && (flags & kLive) != 0u && !complete) {
            const u32 window = kRules[shown.rule].selection == Selection::Enemy ? kHandoverFrames : kHandoverHold;
            handover_[key] = Shown{shown.frame, shown.model, shown.rule, window, key, shown.state, shown.bits};
            ++stats_.handovers;
            if (validate_) {
                // Where it is on screen (-1..1 across the view, up positive; z the depth), and the game tick.
                const float d[3] = {shown.frame.m[0][3] - eye[0], shown.frame.m[1][3] - eye[1],
                                    shown.frame.m[2][3] - eye[2]};
                const float z = d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2];
                const float sx = z > 0.0f ? (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / (z * wx) : 9.0f;
                const float sy = z > 0.0f ? (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / (z * wy) : 9.0f;
                u32 parts = 0u, drawn_parts = 0u;
                for (const Atomic& a : shown.model->atomics) {
                    ++parts;
                    drawn_parts += atomic_drawn(compose(shown.frame, a.pose), a.geometry, kHandoverDistance) ? 1u : 0u;
                }
                std::fprintf(stderr,
                             "[far-objects] frame %llu: %s handover begins: entry 0x%08X flags 0x%08X, the game draws "
                             "%u of %u parts, screen %.2f %.2f depth %.0f tick %llu\n",
                             static_cast<unsigned long long>(stats_.frames), kRules[shown.rule].factory,
                             static_cast<u32>(key), flags, drawn_parts, parts, static_cast<double>(sx),
                             static_cast<double>(sy), static_cast<double>(z),
                             static_cast<unsigned long long>(c.game_clock().now()));
            }
        }
    }
    // The game's nearest draw of the shown model's geometry, at any distance (-1: none this frame).
    const auto nearest_real = [&](const Shown& shown) {
        float nearest = -1.0f;
        for (const Atomic& a : shown.model->atomics)
            for (const Real& real : real_this_frame_) {
                if (real.geometry != a.geometry)
                    continue;
                const render::Float3x4 p = compose(shown.frame, a.pose);
                const float dx = p.m[0][3] - real.world.m[0][3], dy = p.m[1][3] - real.world.m[1][3],
                            dz = p.m[2][3] - real.world.m[2][3];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (nearest < 0.0f || d < nearest)
                    nearest = d;
            }
        return nearest;
    };
    // Measuring: how long after a timed-out handover the game first draws the model at its entry.
    for (auto it = late_.begin(); it != late_.end();) {
        const float d = nearest_real(it->second);
        ++it->second.frames_left;
        if ((d >= 0.0f && d <= kHandoverDistance) || it->second.frames_left >= 600u) {
            const float ex = it->second.frame.m[0][3] - eye[0], ey = it->second.frame.m[1][3] - eye[1],
                        ez = it->second.frame.m[2][3] - eye[2];
            std::fprintf(stderr, "[far-objects] %s entry 0x%08X: the game first drew it %u frames after the timeout%s, "
                         "%.1f from the eye\n",
                         kRules[it->second.rule].factory, static_cast<u32>(it->first), it->second.frames_left,
                         it->second.frames_left >= 600u ? " (not within 600: never)" : "",
                         static_cast<double>(std::sqrt(ex * ex + ey * ey + ez * ez)));
            const u32 entry = static_cast<u32>(it->first);
            std::fprintf(stderr, "  entry words:");
            for (u32 w = 0; w < 8u; ++w)
                std::fprintf(stderr, " %08X", c.mem_read32(entry + 4u * w));
            const u32 misc = c.mem_read32(entry + kEntryMisc), length = c.mem_read32(entry + kEntryMiscLength);
            std::fprintf(stderr, "; misc (%u bytes):", length);
            for (u32 w = 0; in_ram(misc) && w < std::min<u32>(length / 4u, 32u); ++w)
                std::fprintf(stderr, " %08X", c.mem_read32(misc + 4u * w));
            const u32 slot_object = c.mem_read32(entry + kEntryFlags + 8u);
            std::fprintf(stderr, "; +0x20 %08X\n", slot_object);
            it = late_.erase(it);
            continue;
        }
        ++it;
    }
    for (auto it = handover_.begin(); it != handover_.end();) {
        // The game drew it this frame: done. A prop's parts may live in different RpWorlds, each drawn once a frame by
        // the in-stage camera in its own pass (a dash ramp's base, ob_hnal0, in world 0x807A25A0 early in the frame;
        // its UV-animated arrows, ob_hnuv0, in world 0x807A2260 in the frame's last pass). The layout manager spawns
        // between those passes, so on the spawn frame only the parts of the later worlds are drawn; the earlier ones
        // first draw the next frame (docs/DRAW_DISTANCE.md "Parts in two worlds"). The proxy keeps the parts the game
        // has not drawn yet. This runs after the frame's last pass, so by the first full frame after the spawn every
        // part the game draws has been drawn: a part still missing then is one the game never draws (a cage's, a fall
        // building's, a stick's) and the proxy lets it go.
        const bool is_enemy = kRules[it->second.rule].selection == Selection::Enemy;
        u32 parts = 0u, drawn_parts = 0u;
        if (!is_enemy)
            for (const Atomic& a : it->second.model->atomics) {
                ++parts;
                if (atomic_drawn(compose(it->second.frame, a.pose), a.geometry, kHandoverDistance))
                    ++drawn_parts;
            }
        if (drawn_parts != 0u && drawn_parts != parts)
            ++it->second.partial;
        const bool complete = is_enemy || drawn_parts == parts || (drawn_parts != 0u && it->second.partial > 1u);
        if (complete && match_real(it->second)) {
            if (validate_ && drawn_parts != parts) {
                std::fprintf(stderr, "[far-objects] frame %llu: %s entry 0x%08X: the game draws %u of %u parts; not:",
                             static_cast<unsigned long long>(stats_.frames), kRules[it->second.rule].factory,
                             static_cast<u32>(it->first), drawn_parts, parts);
                u32 index = 0u;
                for (const Atomic& a : it->second.model->atomics) {
                    std::fprintf(stderr, " %u%s (geometry 0x%08X atomic 0x%08X flags %02X)", index,
                                 atomic_drawn(compose(it->second.frame, a.pose), a.geometry, kHandoverDistance) ? "" : "*",
                                 a.geometry, a.atomic, c.mem_read8(a.atomic + 2u));
                    ++index;
                }
                std::fprintf(stderr, "\n");
            }
            it = handover_.erase(it);
            continue;
        }
        // An object gone again before the game drew it (left or destroyed): nothing to stand in for.
        const bool gone = (c.mem_read32(static_cast<u32>(it->first) + kEntryFlags) & kLive) == 0u;
        if (it->second.frames_left == 0u || gone) {
            ++stats_.handover_timeouts;
            if (validate_) {  // what it missed: the game's nearest draw of the same geometry, at any distance
                const u32 entry = static_cast<u32>(it->first);
                std::fprintf(stderr, "[far-objects] frame %llu: %s handover timed out: entry 0x%08X flags 0x%08X, the game's nearest "
                             "draw of its geometry %g away (-1: none)\n", static_cast<unsigned long long>(stats_.frames),
                             kRules[it->second.rule].factory, entry,
                             c.mem_read32(entry + kEntryFlags), static_cast<double>(nearest_real(it->second)));
                Shown late = it->second;
                late.frames_left = 0u;
                late_[it->first] = late;
            }
            it = handover_.erase(it);
            continue;
        }
        --it->second.frames_left;
        ++stats_.handover_frames;
        if (kRules[it->second.rule].selection == Selection::Enemy) {
            if (Enemy* enemy = enemy_for(c, it->second.rule))
                push_enemy(c, *enemy, it->second.frame, out, true);
        } else {
            for (const Atomic& a : it->second.model->atomics) {
                const render::Float3x4 world = compose(it->second.frame, a.pose);
                if (!atomic_drawn(world, a.geometry, kHandoverDistance))  // the parts the game draws are its own
                    out.push_back(Proxy{a.atomic, world, 0u, 0u, a.layer, a.light_group, a.uva});
            }
        }
        ++it;
    }
    // Measuring (a class trace): the proxies drawn this frame with the traced class's geometries, and the pass.
    if (tracing && enabled_) {
        const u32 globals = c.mem_read32(c.gpr[13] + 0x615Cu);
        for (const Proxy& proxy : out) {
            const u32 geometry = c.mem_read32(proxy.atomic + kAtomicGeometry);
            if (std::find(traced_geometries_.begin(), traced_geometries_.end(), geometry) != traced_geometries_.end())
                std::fprintf(stderr, "[far-pass] %llu proxy geometry 0x%08X at %.1f %.1f %.1f renderFrame %04X\n",
                             static_cast<unsigned long long>(stats_.frames), geometry,
                             static_cast<double>(proxy.world.m[0][3]), static_cast<double>(proxy.world.m[1][3]),
                             static_cast<double>(proxy.world.m[2][3]),
                             pointer(globals) ? c.mem_read16(globals + 8u) : 0u);
        }
    }
    if (validate_)
        find_gaps(c, factories, slots, camera, out);
    shown_.swap(shown_now_);
    shown_now_.clear();
    real_this_frame_.clear();
}

// Measuring: every prop entry whose model stands well inside the view (its centre within 90 % of the view window, in
// front of the near plane, closer than the far plane) is drawn this frame by a proxy or by the game; one drawn last
// frame and by neither now opens a gap, logged with its length when it closes. Rings and enemies are left out (a
// ring group's rings and an enemy's body do not stand at the entry).
void FarObjects::find_gaps(Cpu& c, u32 factories, u32 slots, const render::Camera& camera,
                           const std::vector<Proxy>& out) {
    float eye[3], right[3], up[3], forward[3];
    for (int i = 0; i < 3; ++i) {
        right[i] = camera.frame.m[i][0];
        up[i] = camera.frame.m[i][1];
        forward[i] = camera.frame.m[i][2];
        eye[i] = camera.frame.m[i][3];
    }
    const auto proxy_at = [&](const render::Float3x4& world, u32 geometry) {
        for (const Proxy& p : out)
            if (c.mem_read32(p.atomic + kAtomicGeometry) == geometry && std::fabs(p.world.m[0][3] - world.m[0][3]) < 1.0f &&
                std::fabs(p.world.m[1][3] - world.m[1][3]) < 1.0f && std::fabs(p.world.m[2][3] - world.m[2][3]) < 1.0f)
                return true;
        return false;
    };
    for (u32 s = 0; s < kSlotCount; ++s) {
        const u32 slot = slots + s * kSlotStride;
        const u32 entry = c.mem_read32(slot);
        if (!pointer(entry))
            continue;
        const s32 type = static_cast<s32>(c.mem_read32(slot + kSlotFactoryIndex));
        const u32 factory = type >= 0 ? c.mem_read32(factories + static_cast<u32>(type) * 4u) : 0u;
        const int rule = pointer(factory) ? rule_of(c, factory) : -1;
        if (rule < 0 || kRules[rule].selection == Selection::RingGroup || kRules[rule].selection == Selection::Enemy)
            continue;
        const u32 library = library_of(c, kRules[rule]);
        const Model* model = pointer(library) ? model_for(c, static_cast<std::size_t>(rule), library, entry) : nullptr;
        if (model == nullptr)
            continue;
        float reach = 1.0f;
        const render::Float3x4 frame = rule_frame(c, entry, kRules[rule], &reach);
        const float d[3] = {frame.m[0][3] - eye[0], frame.m[1][3] - eye[1], frame.m[2][3] - eye[2]};
        const float z = d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2];
        const float x = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
        const float y = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        // An entry the game hides by its switch is not expected on screen (the game's own rule, stand-in or not).
        const bool inside = z > camera.near_plane + model->radius && z < camera.far_plane &&
                            std::fabs(x) < 0.9f * z * camera.view_window_x &&
                            std::fabs(y) < 0.9f * z * camera.view_window_y &&
                            !hidden_by_switch(c, kRules[rule].factory, entry, slot);
        Gap& g = gaps_[entry];
        bool drawn = false;
        for (const Atomic& a : model->atomics) {
            const render::Float3x4 world = compose(frame, a.pose);
            if (atomic_drawn(world, a.geometry, kSamePlace) || proxy_at(world, a.geometry)) {
                drawn = true;
                break;
            }
        }
        const u32 flags = c.mem_read32(entry + kEntryFlags);
        if (drawn) {
            if (g.open != 0u) {
                ++gap_count_;
                gap_frames_ += stats_.frames - g.open;
                std::fprintf(stderr,
                             "[far-gap] %s entry 0x%08X: on screen and drawn by neither for %llu frames (%llu-%llu), "
                             "flags 0x%08X then 0x%08X now, %.1f from the eye, range %u\n",
                             kRules[rule].factory, entry, static_cast<unsigned long long>(stats_.frames - g.open),
                             static_cast<unsigned long long>(g.open), static_cast<unsigned long long>(stats_.frames - 1u),
                             g.flags, flags, static_cast<double>(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])),
                             100u * c.mem_read8(entry + kEntryRange));
                g.open = 0u;
            }
            g.drawn = stats_.frames;
        } else if (inside && g.open == 0u && g.drawn + 1u == stats_.frames) {
            g.open = stats_.frames;
            g.flags = flags;
            // What the gadget leave gate (0x800C9D54 -> 0x80169FC8) measures: camera 0 of the list [r13+0x2B34]
            // (0x80010244: element +8, the eye) against (100 * range + [r13+0x3270])^2.
            const u32 manager = c.mem_read32(c.gpr[13] + 0x2B34u);
            const u32 element = pointer(manager) ? c.mem_read32(manager + 0xCu) : 0u;
            const u32 list_camera = pointer(element) ? c.mem_read32(element) : 0u;
            float le[3] = {0.0f, 0.0f, 0.0f};
            for (u32 i = 0; i < 3u && pointer(list_camera); ++i)
                le[i] = read_f32(c, list_camera + 8u + 4u * i);
            const float ld[3] = {read_f32(c, entry) - le[0], read_f32(c, entry + 4u) - le[1],
                                 read_f32(c, entry + 8u) - le[2]};
            // The object's model holder (ART GADGET: object +0x38; object_distance.md 2.4: +0x3C bits - 2 hidden,
            // 4/5 blink, 7 fading, 8 out, 9 in, 10 faded out; +0x44 the fade alpha).
            const u32 object = c.mem_read32(slot + kSlotObject);
            const u32 holder = pointer(object) && kRules[rule].holder != 0u ? c.mem_read32(object + kRules[rule].holder) : 0u;
            const u32 holder_bits = pointer(holder) ? c.mem_read32(holder + 0x3Cu) : 0u;
            const float holder_alpha = pointer(holder) ? read_f32(c, holder + 0x44u) : -1.0f;
            const u32 manager_now = c.mem_read32(c.gpr[13] + kSdaManagerHolder);
            const u32 restricted_now = pointer(manager_now) ? (c.mem_read32(manager_now + 0x428u) & 1u) : 9u;
            std::fprintf(stderr,
                         "[far-gap] %llu %s entry 0x%08X opens: render eye %.1f %.1f %.1f, list camera 0x%08X eye %.1f "
                         "%.1f %.1f (%.1f from the entry), margin [r13+0x3270] %.1f, range %u, object 0x%08X holder "
                         "bits 0x%08X alpha %.3f, event flag [r13+0x3274] 0x%08X, restricted %u\n",
                         static_cast<unsigned long long>(stats_.frames), kRules[rule].factory, entry,
                         static_cast<double>(eye[0]), static_cast<double>(eye[1]), static_cast<double>(eye[2]),
                         list_camera, static_cast<double>(le[0]), static_cast<double>(le[1]), static_cast<double>(le[2]),
                         static_cast<double>(std::sqrt(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2])),
                         static_cast<double>(read_f32(c, c.gpr[13] + 0x3270u)), 100u * c.mem_read8(entry + kEntryRange),
                         c.mem_read32(slot + kSlotObject), holder_bits, holder_alpha,
                         c.mem_read32(c.gpr[13] + 0x3274u), restricted_now);
            // Its instances (holder +0x4C list, node +8 the clump, object_distance.md 2.3) and each atomic's sector
            // renderer gates (0x80460418: render flag 0x04, renderFrame +0x60 against RwGlobals' +8, world-sector ties
            // +0x64), and the clump's world (+0x3C? printed raw: clump words +0x30..+0x40).
            // The list (0x8015FF30 / 0x8015FEF4 / 0x801639A8): its sentinel node at holder +0x50, next at node +4.
            const u32 head = pointer(holder) ? holder + 0x50u : 0u;
            u32 node = pointer(head) ? c.mem_read32(head + 4u) : 0u;
            const u32 globals = c.mem_read32(c.gpr[13] + 0x615Cu);
            for (int guard = 0; pointer(node) && node != head && guard < 16; ++guard, node = c.mem_read32(node + 4u)) {
                const u32 clump = c.mem_read32(node + 8u);
                if (!pointer(clump))
                    continue;
                std::fprintf(stderr, "[far-gap]   clump 0x%08X words +0x30..:", clump);
                for (u32 o = 0x30u; o < 0x44u; o += 4u)
                    std::fprintf(stderr, " %08X", c.mem_read32(clump + o));
                std::fprintf(stderr, "\n");
                const u32 atoms = clump + kClumpAtomics;
                u32 link = c.mem_read32(atoms);
                for (int g2 = 0; pointer(link) && link != atoms && g2 < 16; ++g2, link = c.mem_read32(link)) {
                    const u32 atomic = link - kAtomicInClumpLink;
                    const u32 ties = atomic + 0x64u;
                    u32 tie_count = 0u;
                    for (u32 t = c.mem_read32(ties); pointer(t) && t != ties && tie_count < 64u; t = c.mem_read32(t))
                        ++tie_count;
                    std::fprintf(stderr,
                                 "[far-gap]     atomic 0x%08X flags %02X renderFrame %04X global %04X ties %u sphere "
                                 "%.1f %.1f %.1f r %.1f\n",
                                 atomic, c.mem_read8(atomic + 2u), c.mem_read16(atomic + 0x60u),
                                 pointer(globals) ? c.mem_read16(globals + 8u) : 0u, tie_count,
                                 static_cast<double>(read_f32(c, atomic + 0x2Cu)),
                                 static_cast<double>(read_f32(c, atomic + 0x30u)),
                                 static_cast<double>(read_f32(c, atomic + 0x34u)),
                                 static_cast<double>(read_f32(c, atomic + 0x38u)));
                    // Its materials' colours (RGBA, +4) and the geometry's flags: a material at alpha 0 draws nothing.
                    const u32 geometry = c.mem_read32(atomic + kAtomicGeometry);
                    const u32 mesh = pointer(geometry) ? c.mem_read32(geometry + 0x54u) : 0u;
                    const u32 meshes = pointer(mesh) ? c.mem_read16(mesh + 4u) : 0u;
                    std::fprintf(stderr, "[far-gap]       geometry 0x%08X flags %08X materials:", geometry,
                                 pointer(geometry) ? c.mem_read32(geometry + 8u) : 0u);
                    for (u32 j = 0; j < meshes && j < 8u; ++j) {
                        const u32 material = c.mem_read32(mesh + 0x10u + 12u * j + 8u);
                        std::fprintf(stderr, " %08X", pointer(material) ? c.mem_read32(material + 4u) : 0u);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
        } else if (!inside) {
            g.open = 0u;  // it left the view: not a blink
        }
    }
    if (stats_.frames % 600u == 0u)
        std::fprintf(stderr, "[far-gap] frame %llu: %llu blinks, %llu frames in all\n",
                     static_cast<unsigned long long>(stats_.frames), static_cast<unsigned long long>(gap_count_),
                     static_cast<unsigned long long>(gap_frames_));
}

// The game's own atomic nearest the proxy's of the same geometry, within kHandoverDistance: its handover, and its
// class's measured pose error. The nearest, since a class's objects may stand side by side.
// Enemies (docs/DRAW_DISTANCE.md "Enemies and characters"): the enemy table {archive, DFF, BON, MTP} per kind, the
// BON banks ([r13+0x5F30], link +0x14, first record +0x10) and the motion banks ([r13+0x5F58], link +0xC).
constexpr u32 kEnemyTable = 0x804CCFDCu;
constexpr u32 kEnemyKinds = 16u;
constexpr u32 kSdaBoneBanks = 0x5F30u;
constexpr u32 kSdaMotionBanks = 0x5F58u;
constexpr u32 kSdaHAnimFrameOffset = 0x5FE8u;      // the HAnim frame extension's offset {id, hierarchy}
constexpr u32 kSkinGeometryOffsetHolder = 0x8056E7D0u;  // RpSkin* at geometry + [this]
constexpr u32 kFrameChild = 0x98u;
constexpr u32 kFrameSibling = 0x9Cu;

// Measuring (SHADOW_FAR_VALIDATE with SHADOW_FAR_CENSUS): what the game has loaded for each enemy kind.
void enemy_probe(Cpu& c) {
    char name[4][40];
    for (u32 k = 0; k < kEnemyKinds; ++k) {
        for (u32 f = 0; f < 4u; ++f) {
            const u32 text = c.mem_read32(kEnemyTable + 16u * k + 4u * f);
            u32 i = 0;
            for (; in_ram(text) && i + 1u < sizeof(name[f]); ++i) {
                name[f][i] = static_cast<char>(c.mem_read8(text + i));
                if (name[f][i] == '\0')
                    break;
            }
            name[f][i] = '\0';
        }
        // The DFF's clump under whichever resource type holds it.
        s32 dff_type = -1;
        u32 clump = 0u;
        for (s32 t = 0; t <= 0x20 && clump == 0u; ++t)
            if ((clump = resource_by_name(c, t, name[1])) != 0u)
                dff_type = t;
        // The skeleton: a BON record named as the BON file without its extension, case-insensitively.
        u32 record = 0u, bones = 0u;
        for (u32 bank = c.mem_read32(c.gpr[13] + kSdaBoneBanks); pointer(bank) && record == 0u;
             bank = c.mem_read32(bank + 0x14u)) {
            u32 r = c.mem_read32(bank + 0x10u);
            for (u32 n = 0; pointer(r) && n < c.mem_read32(bank) && n < 64u; ++n) {
                char model[33] = {};
                for (u32 i = 0; i < 32u; ++i)
                    model[i] = static_cast<char>(c.mem_read8(r + 0x10u + i));
                if (_strnicmp(model, name[2], std::strlen(model)) == 0 && name[2][std::strlen(model)] == '.') {
                    record = r;
                    bones = c.mem_read16(r + 0xAu);
                    break;
                }
                r += 0x30u + 0x70u * c.mem_read16(r + 0xAu);
            }
        }
        // The clump's atomics: skin bones, HAnim hierarchy and its node ids.
        if (pointer(clump)) {
            const u32 head = clump + kClumpAtomics;
            u32 link = c.mem_read32(head);
            for (int guard = 0; pointer(link) && link != head && guard < 8; ++guard, link = c.mem_read32(link)) {
                const u32 atomic = link - kAtomicInClumpLink;
                const u32 geometry = c.mem_read32(atomic + kAtomicGeometry);
                const u32 skin = pointer(geometry) ? c.mem_read32(geometry + c.mem_read32(0x8056E7D0u)) : 0u;
                const u32 hierarchy = c.mem_read32(atomic + c.mem_read32(0x8056E7CCu));
                const u32 nodes = pointer(hierarchy) ? c.mem_read32(hierarchy + 4u) : 0u;
                const u32 info = pointer(hierarchy) ? c.mem_read32(hierarchy + 0x10u) : 0u;
                std::fprintf(stderr, "[far-enemy]   atomic 0x%08X geometry 0x%08X skin 0x%08X (%u bones) hierarchy "
                             "0x%08X (%u nodes):", atomic, geometry, skin, pointer(skin) ? c.mem_read32(skin) : 0u,
                             hierarchy, nodes);
                for (u32 n = 0; pointer(info) && n < nodes && n < 24u; ++n)
                    std::fprintf(stderr, " %X", c.mem_read32(info + 16u * n));
                std::fprintf(stderr, "\n");
                // The hierarchy's node ids against the skeleton's bones (enemy_for's mapping).
                if (pointer(skin) && pointer(record)) {
                    const u32 extension = c.mem_read32(c.gpr[13] + kSdaHAnimFrameOffset);
                    u32 tree = 0u;
                    std::vector<u32> frames{c.mem_read32(clump + kObjectParent)};
                    for (int g = 0; !frames.empty() && tree == 0u && g < 256; ++g) {
                        const u32 frame = frames.back();
                        frames.pop_back();
                        if (!pointer(frame))
                            continue;
                        tree = c.mem_read32(frame + extension + 4u);
                        frames.push_back(c.mem_read32(frame + kFrameSibling));
                        frames.push_back(c.mem_read32(frame + kFrameChild));
                    }
                    const u32 count = pointer(tree) ? c.mem_read32(tree + 4u) : 0u;
                    const u32 ids = pointer(tree) ? c.mem_read32(tree + 0x10u) : 0u;
                    std::fprintf(stderr, "[far-enemy]     hierarchy 0x%08X (%u nodes):", tree, count);
                    for (u32 n = 0; pointer(ids) && n < count && n < 32u; ++n)
                        std::fprintf(stderr, " %X", c.mem_read32(ids + 16u * n));
                    std::fprintf(stderr, "\n");
                }
            }
        }
        u32 clips = 0u, banks = 0u;
        for (u32 bank = c.mem_read32(c.gpr[13] + kSdaMotionBanks); pointer(bank) && banks < 256u;
             bank = c.mem_read32(bank + 0xCu), ++banks)
            clips += c.mem_read16(bank + 2u);
        std::fprintf(stderr,
                     "[far-enemy] kind %u %s %s %s %s: clump 0x%08X (type %d); skeleton 0x%08X (%u bones); "
                     "%u motion banks, %u clips\n",
                     k, name[0], name[1], name[2], name[3], clump, dff_type, record, bones, banks, clips);
    }
}

// Measuring (SHADOW_FAR_CENSUS): every class the layout holds - its factory's name - with how many entries, how many
// the spawn loop would spawn if in range, how many are alive, the nearest candidate and whether a rule covers it.
void FarObjects::census(Cpu& c, u32 factories, u32 slots, bool restricted, const render::Camera& camera) {
    struct Count {
        u32 entries = 0, candidates = 0, live = 0, in_view = 0;
        float nearest = 1e30f;
    };
    std::unordered_map<u32, Count> counts;  // factory -> its entries
    for (u32 s = 0; s < kSlotCount; ++s) {
        const u32 slot = slots + s * kSlotStride;
        const u32 entry = c.mem_read32(slot);
        const s32 type = static_cast<s32>(c.mem_read32(slot + kSlotFactoryIndex));
        if (!pointer(entry) || type < 0)
            continue;
        const u32 factory = c.mem_read32(factories + static_cast<u32>(type) * 4u);
        const u32 flags = c.mem_read32(entry + kEntryFlags);
        Count& n = counts[factory];
        ++n.entries;
        if ((flags & kLive) != 0u)
            ++n.live;
        if ((flags & kDormant) != 0u && (flags & (kLive | kHeld)) == 0u &&
            (!restricted || (flags & kRunsRestricted) != 0u)) {
            ++n.candidates;
            const float dx = read_f32(c, entry) - camera.frame.m[0][3];
            const float dy = read_f32(c, entry + 4u) - camera.frame.m[1][3];
            const float dz = read_f32(c, entry + 8u) - camera.frame.m[2][3];
            n.nearest = std::min(n.nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
            // Its position inside the view (the frustum through the camera's window, near to far).
            float v[3];
            for (int k = 0; k < 3; ++k)
                v[k] = dx * camera.frame.m[0][k] + dy * camera.frame.m[1][k] + dz * camera.frame.m[2][k];
            if (v[2] > camera.near_plane && v[2] < camera.far_plane && std::fabs(v[0]) <= v[2] * camera.view_window_x &&
                std::fabs(v[1]) <= v[2] * camera.view_window_y)
                ++n.in_view;
        }
    }
    if (validate_)
        enemy_probe(c);
    std::fprintf(stderr, "[far-census] frame %llu: %zu classes; camera near %.1f far %.1f at %.0f %.0f %.0f\n",
                 static_cast<unsigned long long>(stats_.frames), counts.size(), static_cast<double>(camera.near_plane),
                 static_cast<double>(camera.far_plane), static_cast<double>(camera.frame.m[0][3]),
                 static_cast<double>(camera.frame.m[1][3]), static_cast<double>(camera.frame.m[2][3]));
    for (const auto& [factory, n] : counts) {
        char name[48] = "?";
        const u32 text = pointer(factory) ? c.mem_read32(factory) : 0u;
        for (u32 i = 0; in_ram(text) && i + 1u < sizeof(name); ++i) {
            name[i] = static_cast<char>(c.mem_read8(text + i));
            if (name[i] == '\0')
                break;
        }
        const int rule = pointer(factory) ? rule_of(c, factory) : -1;
        std::fprintf(stderr,
                     "[far-census]   %-24s entries %4u candidates %4u (in view %3u) live %3u nearest %8.0f  %s\n",
                     name, n.entries, n.candidates, n.in_view, n.live,
                     static_cast<double>(n.candidates ? n.nearest : 0.0f),
                     rule < 0 ? "no proxy" : kRules[rule].verified ? "proxy" : "proxy (unverified)");
    }
}

bool FarObjects::trace_entry(Cpu& c, u32 factories, s32 type, u32 entry, const float eye[3]) {
    const u32 factory = c.mem_read32(factories + static_cast<u32>(type) * 4u);
    const int rule = pointer(factory) ? rule_of(c, factory) : -1;
    if (rule < 0 || std::strcmp(kRules[rule].factory, trace_class_) != 0)
        return false;
    const float dx = read_f32(c, entry) - eye[0], dy = read_f32(c, entry + 4u) - eye[1],
                dz = read_f32(c, entry + 8u) - eye[2];
    return dx * dx + dy * dy + dz * dz <= kTraceRadius * kTraceRadius;
}

bool FarObjects::atomic_drawn(const render::Float3x4& world, u32 geometry, float reach) const {
    auto it = std::lower_bound(real_this_frame_.begin(), real_this_frame_.end(), geometry,
                               [](const Real& real, u32 g) { return real.geometry < g; });
    for (; it != real_this_frame_.end() && it->geometry == geometry; ++it) {
        const float dx = world.m[0][3] - it->world.m[0][3], dy = world.m[1][3] - it->world.m[1][3],
                    dz = world.m[2][3] - it->world.m[2][3];
        if (dx * dx + dy * dy + dz * dz <= reach * reach)
            return true;
    }
    return false;
}

bool FarObjects::drawn_by_game(const render::Float3x4& frame, const Model& model) const {
    for (const Atomic& a : model.atomics)
        if (atomic_drawn(compose(frame, a.pose), a.geometry, kSamePlace))
            return true;
    return false;
}

bool FarObjects::all_drawn_by_game(const render::Float3x4& frame, const Model& model, float reach) const {
    for (const Atomic& a : model.atomics)
        if (!atomic_drawn(compose(frame, a.pose), a.geometry, reach))
            return false;
    return true;
}

bool FarObjects::match_real(const Shown& shown) {
    const Real* best = nullptr;
    render::Float3x4 predicted_at_best;
    float best_distance = kHandoverDistance * kHandoverDistance, best_error = 0.0f;
    for (const Atomic& a : shown.model->atomics) {
        const render::Float3x4 predicted = compose(shown.frame, a.pose);
        for (const Real& real : real_this_frame_) {
            if (real.geometry != a.geometry)
                continue;
            const float dx = predicted.m[0][3] - real.world.m[0][3], dy = predicted.m[1][3] - real.world.m[1][3],
                        dz = predicted.m[2][3] - real.world.m[2][3];
            const float distance = dx * dx + dy * dy + dz * dz;
            // Coincident copies (two entries placing one model at one spot, turned apart): the one it matches.
            const float error = matrix_error(predicted, real.world);
            if (best == nullptr || distance < best_distance - 1e-4f ||
                (distance <= best_distance + 1e-4f && error < best_error)) {
                if (distance > kHandoverDistance * kHandoverDistance)
                    continue;
                best_distance = distance;
                best_error = error;
                best = &real;
                predicted_at_best = predicted;
            }
        }
    }
    if (best == nullptr)
        return false;
    ClassStats& cls = stats_.classes[shown.rule];
    ++cls.compared;
    const float error = matrix_error(predicted_at_best, best->world);
    cls.max_error = std::max(cls.max_error, error);
    if (error <= 1e-3f)
        ++cls.exact;
    if (validate_ && error > 1e-3f && cls.reported < 8u) {  // what the measurement saw: the proxy's and the game's
        ++cls.reported;
        std::fprintf(stderr, "[far-objects] %s differs by %g: entry 0x%08X ring %u, slot state 0x%08X bits 0x%08X\n",
                     cls.name, static_cast<double>(error), static_cast<u32>(shown.key),
                     static_cast<u32>(shown.key >> 32), shown.state, shown.bits);
        const render::Float3x4* const pair[2] = {&predicted_at_best, &best->world};
        for (const render::Float3x4* m : pair)
            std::fprintf(stderr, "  %s [%.5f %.5f %.5f %.3f] [%.5f %.5f %.5f %.3f] [%.5f %.5f %.5f %.3f]\n",
                         m == &predicted_at_best ? "proxy" : "game ", m->m[0][0], m->m[0][1], m->m[0][2],
                         m->m[0][3], m->m[1][0], m->m[1][1], m->m[1][2], m->m[1][3], m->m[2][0], m->m[2][1],
                         m->m[2][2], m->m[2][3]);
        // Coincident copies (two entries placing one model at one spot): every game draw of the geometry there.
        for (const Real& real : real_this_frame_) {
            const float dx = real.world.m[0][3] - best->world.m[0][3], dy = real.world.m[1][3] - best->world.m[1][3],
                        dz = real.world.m[2][3] - best->world.m[2][3];
            if (real.geometry == best->geometry && dx * dx + dy * dy + dz * dz <= kSamePlace * kSamePlace)
                std::fprintf(stderr, "  game draw here: x axis [%.5f %.5f %.5f]\n", real.world.m[0][0],
                             real.world.m[1][0], real.world.m[2][0]);
        }
    }
    return true;
}

void FarObjects::trace_atomic(Cpu& c, u32 atomic, u32 geometry) {
    if (trace_class_ == nullptr ||
        std::find(traced_geometries_.begin(), traced_geometries_.end(), geometry) == traced_geometries_.end())
        return;
    // Every draw inside the trace's frames: the pass (RwGlobals' renderFrame, the camera) and the chain's callers.
    if (stats_.frames + 1u >= trace_first_ && stats_.frames + 1u <= trace_last_) {
        const u32 globals = c.mem_read32(c.gpr[13] + 0x615Cu);
        std::fprintf(stderr, "[far-pass] %llu atomic 0x%08X geometry 0x%08X renderFrame %04X camera 0x%08X world 0x%08X chain",
                     static_cast<unsigned long long>(stats_.frames + 1u), atomic, geometry,
                     pointer(globals) ? c.mem_read16(globals + 8u) : 0u,
                     pointer(globals) ? c.mem_read32(globals) : 0u, pointer(globals) ? c.mem_read32(globals + 4u) : 0u);
        u32 sp = c.gpr[1];
        for (int depth = 0; depth < 16 && pointer(sp); ++depth) {
            const u32 caller = c.mem_read32(sp);
            if (!pointer(caller))
                break;
            std::fprintf(stderr, " %08X", c.mem_read32(caller + 4u));
            sp = caller;
        }
        std::fprintf(stderr, "\n");
    }
    if (std::find(traced_atomics_.begin(), traced_atomics_.end(), atomic) != traced_atomics_.end() ||
        std::find(traced_atomics_.begin(), traced_atomics_.end(), atomic) != traced_atomics_.end() ||
        traced_atomics_.size() >= 64u)
        return;
    traced_atomics_.push_back(atomic);
    std::fprintf(stderr, "[far-trace] %llu first draw of game atomic 0x%08X (geometry 0x%08X):",
                 static_cast<unsigned long long>(stats_.frames), atomic, geometry);
    for (u32 o = 0; o < 0x80u; o += 4u)
        std::fprintf(stderr, " %08X", c.mem_read32(atomic + o));
    // The guest's call chain: lr, then each frame's saved LR along the back chain.
    std::fprintf(stderr, "\n[far-trace]   call chain: lr %08X", c.lr);
    u32 sp = c.gpr[1];
    for (int depth = 0; depth < 16 && pointer(sp); ++depth) {
        const u32 caller = c.mem_read32(sp);
        if (!pointer(caller))
            break;
        std::fprintf(stderr, " %08X", c.mem_read32(caller + 4u));
        sp = caller;
    }
    std::fprintf(stderr, "\n");
}

void FarObjects::real_drawn(u32 geometry, const render::Float3x4& world) noexcept {
    if ((enabled_ || trace_class_ != nullptr) && real_this_frame_.size() < 8192u)
        real_this_frame_.push_back(Real{geometry, world});
}

namespace {

std::string guest_text(Cpu& c, u32 text) {
    std::string s;
    for (u32 i = 0; in_ram(text) && i < 64u; ++i) {
        const char ch = static_cast<char>(c.mem_read8(text + i));
        if (ch == '\0')
            break;
        s.push_back(ch);
    }
    return s;
}

bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

render::Float3x4 float3x4(const shadow::motion::Mtx34& m) {
    render::Float3x4 f;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k)
            f.m[r][k] = m[static_cast<std::size_t>(4 * r + k)];
    return f;
}

}  // namespace

// An enemy kind's stand-in, from what the game loaded for it: the enemy table row's DFF (its archive's clump, under
// the resource type its preload registered), the BON record named like its .BON file (the BON banks), the rule's clip
// (the motion banks), the clump's skinned atomics with each skin bone's skeleton bone (the HAnim hierarchy on the
// clump's frames, [frame + [r13+0x5FE8] + 4]: node ids = BON bone ids; the bone links 80015570 / 80015740 set a node
// frame's matrix to its bone's pose). A clip of another model (cross-model binding) is not taken.
FarObjects::Enemy* FarObjects::enemy_for(Cpu& c, std::size_t rule) {
    const u32 kind = kRules[rule].key;
    Enemy& e = enemies_[kind];
    if (e.ok)
        return &e;
    if (stats_.frames < e.retry)
        return nullptr;
    e.retry = stats_.frames + 60u;
    const auto fail = [&](const char* why) -> Enemy* {
        if (e.why != why)
            std::fprintf(stderr, "[far] enemy kind %u (%s): %s\n", kind, kRules[rule].factory, why);
        e.why = why;
        e.skins.clear();
        e.model.atomics.clear();
        return nullptr;
    };
    const u32 row = kEnemyTable + 16u * kind;
    const std::string dff = guest_text(c, c.mem_read32(row + 4u)), bon = guest_text(c, c.mem_read32(row + 8u));
    u32 clump = 0u;
    for (s32 type = 0; type <= 0x40 && clump == 0u; ++type)
        clump = resource_by_name(c, type, dff.c_str());
    if (!pointer(clump))
        return fail("no clump");
    // The skeleton.
    const std::string model = bon.substr(0, bon.find('.'));
    u32 record = 0u;
    for (u32 bank = c.mem_read32(c.gpr[13] + kSdaBoneBanks); pointer(bank) && record == 0u;
         bank = c.mem_read32(bank + 0x14u)) {
        u32 r = c.mem_read32(bank + 0x10u);
        for (u32 n = 0; pointer(r) && n < c.mem_read32(bank) && n < 64u; ++n) {
            if (same_name(guest_text(c, r + 0x10u), model)) {
                record = r;
                break;
            }
            r += 0x30u + 0x70u * c.mem_read16(r + 0xAu);
        }
    }
    if (record == 0u)
        return fail("no skeleton record");
    std::vector<std::uint8_t> bytes(0x30u + 0x70u * c.mem_read16(record + 0xAu));
    for (u32 i = 0; i < bytes.size(); ++i)
        bytes[i] = c.mem_read8(record + i);
    if (!shadow::motion::parse_skeleton(bytes.data(), bytes.size(), record, &e.skeleton))
        return fail("skeleton parse");
    // The clip: a bank flagged at +6 keeps its clips in ARAM, one {name, name size, clip, clip size, events, events
    // size} record of ARAM addresses a clip from +0x1C (the loader 80414370 copies a clip in on demand); any other
    // holds them in memory, the file's table of {name, clip, events} pointers at +0x10.
    bytes.clear();
    for (u32 bank = c.mem_read32(c.gpr[13] + kSdaMotionBanks), n = 0; pointer(bank) && bytes.empty() && n < 256u;
         bank = c.mem_read32(bank + 0xCu), ++n) {
        const u32 count = std::min<u32>(c.mem_read16(bank + 2u), 512u);
        if (c.mem_read8(bank + 6u) != 0u) {
            // The records' addresses are the pool's, in the native motion store's arena (a state made before ARAM was
            // retired keeps them at ARAM addresses, which the store serves from the arena: pool_address).
            const auto aux = [&](u32 at, u32 size) {
                std::string out;
                for (u32 i = 0; i < size; ++i) {
                    const u32 address = gekko::NativeMotionStore::pool_address(at + i);
                    if (in_ram(address))
                        out.push_back(static_cast<char>(c.mem_read8(address)));
                }
                return out;
            };
            for (u32 i = 0; i < count; ++i) {
                const u32 clip_record = bank + 0x1Cu + 0x18u * i;
                const u32 name_size = std::min<u32>(c.mem_read32(clip_record + 4u), 64u);
                const std::string name = aux(c.mem_read32(clip_record), name_size);
                if (std::string(name.c_str()) != kRules[rule].clip)
                    continue;
                const u32 size = c.mem_read32(clip_record + 12u);
                if (size == 0u || size > 0x80000u)
                    break;
                const std::string clip = aux(c.mem_read32(clip_record + 8u), size);
                bytes.assign(clip.begin(), clip.end());
                break;
            }
            continue;
        }
        const u32 table = c.mem_read32(bank + 0x10u);
        for (u32 i = 0; pointer(table) && i < count; ++i)
            if (guest_text(c, c.mem_read32(table + 12u * i)) == kRules[rule].clip) {
                const u32 clip = c.mem_read32(table + 12u * i + 4u);
                if (!pointer(clip) || c.mem_read32(clip + 8u) > 0x80000u)
                    break;
                bytes.assign(c.mem_read32(clip + 8u), 0u);
                for (u32 k = 0; k < bytes.size(); ++k)
                    bytes[k] = c.mem_read8(clip + k);
                break;
            }
    }
    if (bytes.empty())
        return fail("no clip");
    if (!shadow::motion::parse_clip(bytes.data(), bytes.size(), &e.clip) ||
        !same_name(e.clip.model, e.skeleton.model) || !e.pose.bind(e.skeleton, e.clip))
        return fail("clip parse, model or bind");
    // The hierarchy's node ids.
    const u32 extension = c.mem_read32(c.gpr[13] + kSdaHAnimFrameOffset);
    u32 tree = 0u;
    std::vector<u32> frames{c.mem_read32(clump + kObjectParent)};
    for (int guard = 0; !frames.empty() && tree == 0u && guard < 256; ++guard) {
        const u32 frame = frames.back();
        frames.pop_back();
        if (!pointer(frame))
            continue;
        tree = c.mem_read32(frame + extension + 4u);
        frames.push_back(c.mem_read32(frame + kFrameSibling));
        frames.push_back(c.mem_read32(frame + kFrameChild));
    }
    if (!pointer(tree))
        return fail("no hierarchy");
    const u32 nodes = c.mem_read32(tree + 4u), records = c.mem_read32(tree + 0x10u);
    const std::vector<std::int32_t> order = e.skeleton.preorder();
    // The clump's skinned atomics.
    const u32 head = clump + kClumpAtomics;
    u32 link = c.mem_read32(head);
    for (int guard = 0; pointer(link) && link != head && guard < 16; ++guard, link = c.mem_read32(link)) {
        const u32 atomic = link - kAtomicInClumpLink;
        const u32 geometry = c.mem_read32(atomic + kAtomicGeometry);
        const u32 skin = pointer(geometry) ? c.mem_read32(geometry + c.mem_read32(kSkinGeometryOffsetHolder)) : 0u;
        if (!pointer(skin) || !pointer(records))
            continue;
        Skin s;
        s.atomic = atomic;
        s.bones = c.mem_read32(skin);
        s.inverse_bind = c.mem_read32(skin + 0x0Cu);
        if (s.bones == 0u || s.bones > nodes || s.bones > 64u || !pointer(s.inverse_bind))
            continue;
        const auto bone_of = [&](u32 id) {
            for (std::int32_t i : order)  // the first in pre-order with the id (80416EC0)
                if (e.skeleton.bones[static_cast<std::size_t>(i)].id == id)
                    return static_cast<int>(i);
            return -1;
        };
        for (u32 b = 0; b < s.bones; ++b)
            s.node_bone.push_back(bone_of(c.mem_read32(records + 16u * b)));
        // The atomic's own frame carries an HAnim id too (its extension's first word): that bone places the game's
        // draw of it.
        const u32 own = c.mem_read32(atomic + kObjectParent);
        if (pointer(own))
            s.frame_bone = bone_of(c.mem_read32(own + extension));
        if (validate_)
            std::fprintf(stderr, "[far-objects] %s: skinned atomic 0x%08X on HAnim id %d (bone %d)\n",
                         kRules[rule].factory, atomic, pointer(own) ? static_cast<int>(c.mem_read32(own + extension)) : -1,
                         s.frame_bone);
        e.skins.push_back(std::move(s));
        e.model.atomics.push_back(Atomic{atomic, geometry, render::Float3x4{}});
        const float cx = read_f32(c, atomic + kAtomicSphere), cy = read_f32(c, atomic + kAtomicSphere + 4u),
                    cz = read_f32(c, atomic + kAtomicSphere + 8u);
        const float centre = std::sqrt(cx * cx + cy * cy + cz * cz);
        e.model.radius = std::max(e.model.radius, 2.0f * (centre + read_f32(c, atomic + kAtomicSphere + 12u)));
    }
    // The clip's first frame (the pose has not advanced yet). The game's skinned atomic hangs on one of the
    // hierarchy's nodes: placement * that node's bone at the spawn is where its first draw stands (handovers and
    // doubles compare against it).
    e.pose.evaluate(&e.spawn);
    for (std::size_t i = 0; i < e.skins.size() && i < e.model.atomics.size(); ++i) {
        const int bone = e.skins[i].frame_bone;
        e.model.atomics[i].pose = bone >= 0 ? float3x4(e.spawn[static_cast<std::size_t>(bone)]) : render::Float3x4{};
    }
    e.layer = worldparm_layer(c, clump);
    if (e.layer == kEnemyLayerAsked)
        e.layer = kEnemyLayerTaken;
    e.light_group = kEnemyLightGroup;
    if (validate_)
        std::fprintf(stderr, "[far-objects] %s: layer %d, light group %d\n", kRules[rule].factory, e.layer, e.light_group);
    e.ok = !e.skins.empty();
    return e.ok ? &e : fail("no skinned atomic");
}

// The enemy's pose for this frame (one evaluation a frame, shared by its stand-ins; the clip advances a frame each
// time), then each skinned atomic at `frame`: palette[b] = frame * bone pose * inverse bind[b], a node without a bone
// at the frame itself.
void FarObjects::push_enemy(Cpu& c, Enemy& enemy, const render::Float3x4& frame, std::vector<Proxy>& out,
                            bool spawned) {
    if (enemy.posed_frame != stats_.frames) {
        if (enemy.posed_frame != ~u64{0})
            enemy.pose.advance();
        enemy.pose.evaluate(&enemy.world);
        enemy.posed_frame = stats_.frames;
    }
    const std::vector<shadow::motion::Mtx34>& world = spawned ? enemy.spawn : enemy.world;
    for (const Skin& s : enemy.skins) {
        const auto first = static_cast<u32>(palette_.size());
        for (u32 b = 0; b < s.bones; ++b) {
            const int bone = s.node_bone[b];
            const render::Float3x4 pose =
                bone >= 0 ? compose(frame, float3x4(world[static_cast<std::size_t>(bone)])) : frame;
            palette_.push_back(compose(pose, rw_matrix(c, s.inverse_bind + 64u * b)));
        }
        out.push_back(Proxy{s.atomic, render::Float3x4{}, first, s.bones, enemy.layer, enemy.light_group});
        ++stats_.drawn_atomics;
    }
}

}  // namespace gekko::scaffold
