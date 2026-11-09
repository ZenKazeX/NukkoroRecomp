// RenderWare Graphics 3.7 core: frames (modules baframe, basync, batypehf), native x64.
//
// A frame is a node of a transform hierarchy: its modelling matrix is relative to its parent, its LTM (local-to-world
// matrix) is the modelling matrix times the parent's LTM. Objects (atomics, lights, cameras) hang off frames through
// RwObjectHasFrame. Changing a frame puts its hierarchy's root on the engine's dirty-frame list once; _rwFrameSyncDirty
// (or RwFrameGetLTM, for one hierarchy) brings the LTMs up to date and calls every attached object's sync function.
//
// Written from PAL main.dol: baframe 0x804878D8..0x80488960, basync 0x8048BE30..0x8048C6F8, batypehf 0x8048E810 /
// 0x8048E884. Proof: runtime/tests/rwcore_frame_tests.cpp (oracle) and rwcore_frame_native_tests.cpp. Deliberate
// differences (rwcore/README.md "Native contracts"): hierarchies of any depth (no recursion), the original's
// dangling-pointer and partial-construction defects fixed, invalid hierarchies (cycles) refused.
// Not linked by the game, so not provided: RwFrameCloneHierarchy, RwFrameCount, RwFrameOrthoNormalize,
// RwFrameForAllObjects, RwFrameGetPluginOffset, RwFrameSetFreeListCreateParams, _rwFrameSetStaticPluginsSize.
#pragma once

#include "rw/core/engine.h"
#include "rw/core/matrix.h"
#include "rw/core/plugin.h"
#include "rw/core/types.h"

/// The header of every RenderWare object (rwplcore.h RwObject).
struct RwObject {
    RwUInt8 type;
    RwUInt8 subType;
    RwUInt8 flags;
    RwUInt8 privateFlags;
    void* parent;   ///< for objects with a frame: the frame; for frames: the parent frame
};

inline constexpr RwUInt8 rwFRAME = 0;   ///< RwObject::type of a frame

/// RwFrame::object.privateFlags (rwcore.h rwFRAMEPRIVATE*): bits 0-1 on a root mean "in the dirty list"; 2-3 mark
/// the frames below a change whose LTM / objects need updating.
inline constexpr RwUInt8 rwFRAMEPRIVATEHIERARCHYSYNCLTM = 0x01;
inline constexpr RwUInt8 rwFRAMEPRIVATEHIERARCHYSYNCOBJ = 0x02;
inline constexpr RwUInt8 rwFRAMEPRIVATESUBTREESYNCLTM = 0x04;
inline constexpr RwUInt8 rwFRAMEPRIVATESUBTREESYNCOBJ = 0x08;
inline constexpr RwUInt8 rwFRAMEPRIVATESTATIC = 0x10;

/// A frame (rwcore.h RwFrame). 16-byte aligned so both matrices are (the GameCube's frames are 4-aligned).
struct alignas(16) RwFrame {
    RwObject object;
    RwLLLink inDirtyListLink;   ///< in RwGlobals::dirtyFrameList while a root's sync bits are set
    RwMatrix modelling;
    RwMatrix ltm;
    RwLinkList objectList;      ///< the RwObjectHasFrame::lFrame links of the attached objects
    RwFrame* child;
    RwFrame* next;
    RwFrame* root;
    RwUInt8 pad[8];             ///< to the 16-byte alignment (explicit, so the size is visible)
};
static_assert(sizeof(RwFrame) == 208 && alignof(RwFrame) == 16, "RwFrame's native layout");

struct RwObjectHasFrame;
using RwObjectHasFrameSyncFunction = RwObjectHasFrame* (*)(RwObjectHasFrame* object);

/// An object attached to a frame (rwcore.h RwObjectHasFrame).
struct RwObjectHasFrame {
    RwObject object;
    RwLLLink lFrame;
    RwObjectHasFrameSyncFunction sync;
};

using RwFrameCallBack = RwFrame* (*)(RwFrame* frame, void* data);

/// The frame module's globals: the free list of frames.
struct rwFrameGlobals {
    RwFreeList* frames;
};

extern RwModuleInfo frameModule;

/// The frames' plugin registry (frameTKList).
RwPluginRegistry& rwFrameRegistry();

/// Opens the module: the frame free list (entries of the registry's size, 50 to a block, one preallocated, a static
/// header, hint rwMEMHINTDUR_GLOBAL | rwID_FRAMELIST) and an empty dirty-frame list. NULL when the free list cannot
/// be made. Original: 0x804878D8.
void* _rwFrameOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Destroys the frame free list. Returns instance. Original: 0x8048797C.
void* _rwFrameClose(void* instance, RwInt32 offset, RwInt32 size);

/// A frame: identity modelling and LTM, no parent, no children, its own root, the plugins' constructors run. NULL
/// when the allocation fails, (native) a plugin constructor fails or no frame module is open (E_RW_BADENGINESTATE).
/// Original: 0x80487D50.
RwFrame* RwFrameCreate();

/// Runs the plugin destructors, detaches the frame from its parent and the dirty list, and frees it. Its children
/// become roots of their own (native: the original leaves them pointing at the freed frame). TRUE. Original: 0x80487E7C.
RwBool RwFrameDestroy(RwFrame* frame);

/// Destroys frame and everything below it, children before parents, siblings in order (native: detached from its
/// parent first; the original leaves the parent pointing at freed memory). TRUE. Original: 0x80488258.
RwBool RwFrameDestroyHierarchy(RwFrame* frame);

/// The frame's hierarchy needs an update: its root joins the dirty list (once) with both sync bits, the frame gets
/// both subtree bits. Returns frame. Original: 0x8048827C.
RwFrame* RwFrameUpdateObjects(RwFrame* frame);

/// TRUE-ish (the root's sync bits) when the frame's hierarchy has a pending update. Original: 0x80487D40.
RwBool RwFrameDirty(const RwFrame* frame);

/// The frame's LTM, its hierarchy's LTMs brought up to date first when needed. Original: 0x804882E8.
RwMatrix* RwFrameGetLTM(RwFrame* frame);

/// The root of the frame's hierarchy. Original: 0x80488328.
RwFrame* RwFrameGetRoot(const RwFrame* frame);

/// child becomes parent's first child (removed from a former parent first; a former root leaves the dirty list) and
/// the hierarchy is marked for an update. NULL (native) when child is parent or one of its ancestors: the original
/// makes a cycle. Returns parent. Original: 0x80488398.
RwFrame* RwFrameAddChild(RwFrame* parent, RwFrame* child);

/// As RwFrameAddChild without marking anything (the caller updates). Original: 0x80488330.
RwFrame* RwFrameAddChildNoUpdate(RwFrame* parent, RwFrame* child);

/// child leaves its parent and becomes a root, marked for an update. Returns child (native: also when it has no
/// parent, where the original dereferences NULL). Original: 0x80488498.
RwFrame* RwFrameRemoveChild(RwFrame* child);

/// callBack(child, data) for each child in order until one returns NULL. Returns frame. Original: 0x8048857C.
RwFrame* RwFrameForAllChildren(RwFrame* frame, RwFrameCallBack callBack, void* data);

/// The modelling matrix transformed (RwMatrixTranslate / Scale / Transform / Rotate with combineOp) and the frame
/// marked for an update. Return frame. Originals: 0x80488600, 0x8048869C, 0x80488738, 0x804887D4.
RwFrame* RwFrameTranslate(RwFrame* frame, const RwV3d* v, RwOpCombineType combine);
RwFrame* RwFrameScale(RwFrame* frame, const RwV3d* v, RwOpCombineType combine);
RwFrame* RwFrameTransform(RwFrame* frame, const RwMatrix* m, RwOpCombineType combine);
RwFrame* RwFrameRotate(RwFrame* frame, const RwV3d* axis, RwReal angle, RwOpCombineType combine);

/// An identity modelling matrix, the frame marked for an update. Returns frame. Original: 0x80488870.
RwFrame* RwFrameSetIdentity(RwFrame* frame);

/// A frame plugin (before the frame module opens). Its offset, or -1. Original: 0x80488924.
RwInt32 RwFrameRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                              RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);

/// A copy of the hierarchy below root (modelling matrices, object flags and private flags, the plugins' copy
/// callbacks; no objects; each copy's children in reverse order, as the original builds them). Each source frame's
/// root field is left pointing at its copy (the caller maps objects through it, then calls _rwFramePurgeClone). NULL
/// when an allocation fails (everything copied so far is destroyed). Original: 0x80487CAC.
RwFrame* _rwFrameCloneAndLinkClones(RwFrame* root);

/// Restores the root fields of the hierarchy below frame after _rwFrameCloneAndLinkClones. Returns frame.
/// Original: 0x80487CF4.
RwFrame* _rwFramePurgeClone(RwFrame* frame);

/// Brings every dirty hierarchy up to date (LTMs where marked, then every attached object's sync, frame by frame)
/// and empties the dirty-frame list. TRUE. Original: 0x8048C3A4.
RwBool _rwFrameSyncDirty();

/// Brings one hierarchy's LTMs up to date without syncing objects (RwFrameGetLTM). Original: 0x8048C628.
void _rwFrameSyncHierarchyLTM(RwFrame* root);

/// Attaches object to frame (detaching it from its former frame), at the head of the frame's object list, and marks
/// the frame for an update; frame NULL only detaches. Original: 0x8048E810.
void _rwObjectHasFrameSetFrame(void* object, RwFrame* frame);

/// Detaches object from its frame's object list (object->parent is kept, as in the original). Original: 0x8048E884.
void _rwObjectHasFrameReleaseFrame(void* object);
