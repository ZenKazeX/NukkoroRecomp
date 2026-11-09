// RenderWare frames (modules baframe, basync, batypehf; see rw/core/frame.h).
//
// Traversals: the originals recurse (CodeWarrior unrolls several levels inline); here every walk is iterative in the
// same visiting order - pre-order through the parent pointers for root updates and syncs (the flags inherited down
// the path in a reusable per-depth stack), post-order for destruction, an explicit stack for cloning - so a hierarchy
// of any depth works and the callbacks (plugins, object syncs) run in the original's order.
#include "rw/core/frame.h"

#include <cstring>
#include <vector>

#include "rw/core/error.h"
#include "rw/core/memory.h"

RwModuleInfo frameModule{};

namespace {

// The free list's creation parameters (baframe statics 0x805EEF90 / 0x805EEF94; RwFrameSetFreeListCreateParams is
// not linked) and its in-place header (0x805E3D38). The original aligns frames to 4; 16 here (rw/core/frame.h).
constexpr RwInt32 kFramesPerBlock = 50;
constexpr RwInt32 kPreallocBlocks = 1;
constexpr RwUInt32 rwID_FRAMELIST = 0x0E;
constexpr RwUInt32 kFrameHint = 0x00030000u | rwID_FRAMELIST;   // rwMEMHINTDUR_EVENT, as the original passes it
RwFreeList frameFreeListSpace{};
RwPluginRegistry frameTKList = rwPLUGINREGISTRYINIT(sizeof(RwFrame));   // 0x8056F070's sibling 0x8056F0A8

constexpr RwUInt8 kSyncBits = rwFRAMEPRIVATEHIERARCHYSYNCLTM | rwFRAMEPRIVATEHIERARCHYSYNCOBJ;
constexpr RwUInt8 kSubtreeBits = rwFRAMEPRIVATESUBTREESYNCLTM | rwFRAMEPRIVATESUBTREESYNCOBJ;

RwFreeList*& frameFreeList() { return rwModuleGlobals<rwFrameGlobals>(frameModule).frames; }
RwLinkList& dirtyFrames() { return rwSrcGlobals().dirtyFrameList; }

RwFrame* parentOf(const RwFrame* frame) { return static_cast<RwFrame*>(frame->object.parent); }

void linkAtHead(RwLLLink* head, RwLLLink* link) {
    link->next = head->next;
    link->prev = head;
    head->next->prev = link;
    head->next = link;
}

void unlink(RwLLLink* link) {
    link->prev->next = link->next;
    link->next->prev = link->prev;
}

void storeIdentity(RwMatrix& m) {
    m.right = {1.0f, 0.0f, 0.0f};
    m.up = {0.0f, 1.0f, 0.0f};
    m.at = {0.0f, 0.0f, 1.0f};
    m.pos = {0.0f, 0.0f, 0.0f};
    m.flags = RwUInt32{rwMATRIXTYPEORTHONORMAL} | RwUInt32{rwMATRIXINTERNALIDENTITY};
}

/// The next frame of a pre-order walk of the hierarchy below top (children before siblings), or NULL.
RwFrame* nextInPreOrder(RwFrame* frame, const RwFrame* top) {
    if (frame->child != nullptr) return frame->child;
    while (frame != top) {
        if (frame->next != nullptr) return frame->next;
        frame = parentOf(frame);
    }
    return nullptr;
}

/// rwSetHierarchyRoot (0x804879E0): root into frame and everything below it.
void setHierarchyRoot(RwFrame* frame, RwFrame* root) {
    for (RwFrame* f = frame; f != nullptr; f = nextInPreOrder(f, frame)) f->root = root;
}

/// The inline tail of RwFrameUpdateObjects that most frame functions share: the root into the dirty list once.
void markForUpdate(RwFrame* frame) {
    RwFrame* root = frame->root;
    const RwUInt8 rootFlags = root->object.privateFlags;
    if ((rootFlags & kSyncBits) == 0) linkAtHead(&dirtyFrames().link, &root->inDirtyListLink);
    root->object.privateFlags = static_cast<RwUInt8>(rootFlags | kSyncBits);
    frame->object.privateFlags = static_cast<RwUInt8>(frame->object.privateFlags | kSubtreeBits);
}

void leaveDirtyList(RwFrame* frame) {
    if (frame->object.privateFlags & kSyncBits) unlink(&frame->inDirtyListLink);
}

/// _rwFrameDeInit (0x80487F28) and the free that follows it in the destroy walk (0x80487F84).
/// Native: the objects still attached to a frame being destroyed lose it (the original leaves their parent and links
/// pointing into the freed frame, so detaching them later writes freed memory).
void releaseObjects(RwFrame* frame) {
    RwLLLink* head = &frame->objectList.link;
    for (RwLLLink* link = head->next; link != head; link = link->next) {
        reinterpret_cast<RwObjectHasFrame*>(reinterpret_cast<char*>(link) - offsetof(RwObjectHasFrame, lFrame))
            ->object.parent = nullptr;
    }
    head->next = head->prev = head;
}

void deInitAndFree(RwFrame* frame) {
    _rwPluginRegistryDeInitObject(&frameTKList, frame);
    leaveDirtyList(frame);
    releaseObjects(frame);
    rwSrcGlobals().memoryFree(frameFreeList(), frame);
}

/// rwFrameDestroyRecurse (0x80487FFC): frame and everything below it, post-order (children in sibling order, each
/// subtree before its parent).
void destroyRecurse(RwFrame* top) {
    RwFrame* node = top;
    for (;;) {
        while (node->child != nullptr) node = node->child;
        if (node == top) {
            deInitAndFree(top);
            return;
        }
        RwFrame* parent = parentOf(node);
        RwFrame* next = node->next;
        deInitAndFree(node);
        if (next != nullptr) {
            node = next;
        } else {
            parent->child = nullptr;   // its children are gone: the parent is next
            node = parent;
        }
    }
}

/// Calls every object's sync function on frame's object list (the next link is read after each call, as the
/// original does).
void syncObjects(RwFrame* frame) {
    RwLLLink* head = &frame->objectList.link;
    for (RwLLLink* link = head->next; link != head; link = link->next) {
        auto* object = reinterpret_cast<RwObjectHasFrame*>(reinterpret_cast<char*>(link) - offsetof(RwObjectHasFrame, lFrame));
        object->sync(object);
    }
}

/// The flags inherited down a walk, one per depth: 64 levels in place, deeper ones on the heap. Each walk has its own
/// (an object's sync callback may call RwFrameGetLTM, which walks again while the outer walk is under way - the
/// original recurses, so its walks nest the same way).
class FlagStack {
public:
    void push_back(RwUInt8 v) {
        if (size_ < kInline) {
            inline_[size_] = v;
        } else {
            heap_.push_back(v);
        }
        ++size_;
    }
    void pop_back() {
        --size_;
        if (size_ >= kInline) heap_.pop_back();
    }
    RwUInt8 back() const { return size_ <= kInline ? inline_[size_ - 1] : heap_.back(); }

private:
    static constexpr std::size_t kInline = 64;
    RwUInt8 inline_[kInline];
    std::vector<RwUInt8> heap_;
    std::size_t size_ = 0;
};

/// The frames below root (its children's subtrees) in pre-order, each with its parent's inherited flags; visit(frame,
/// inherited) returns the flags its own children inherit.
template <class Visit> void walkBelow(RwFrame* root, RwUInt8 rootFlags, Visit visit) {
    if (root->child == nullptr) return;
    FlagStack inherited;
    inherited.push_back(rootFlags);   // depth 0: what root's children inherit
    RwFrame* f = root->child;
    for (;;) {
        const RwUInt8 mine = visit(f, inherited.back());
        if (f->child != nullptr) {
            inherited.push_back(mine);
            f = f->child;
            continue;
        }
        while (f->next == nullptr) {
            f = parentOf(f);
            if (f == root) return;
            inherited.pop_back();
        }
        f = f->next;
    }
}

/// The LTM of frame from its parent's (the original's RwMatrixMultiply(&ltm, &modelling, &parent->ltm)).
void updateLTM(RwFrame* frame) { RwMatrixMultiply(&frame->ltm, &frame->modelling, &parentOf(frame)->ltm); }

}  // namespace

RwPluginRegistry& rwFrameRegistry() { return frameTKList; }

void* _rwFrameOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    frameModule.globalsOffset = offset;
    frameFreeList() = RwFreeListCreateAndPreallocateSpace(frameTKList.sizeOfStruct, kFramesPerBlock, alignof(RwFrame),
                                                          kPreallocBlocks, &frameFreeListSpace,
                                                          rwMEMHINTDUR_GLOBAL | rwID_FRAMELIST);
    if (frameFreeList() == nullptr) return nullptr;
    dirtyFrames().link.next = dirtyFrames().link.prev = &dirtyFrames().link;
    ++frameModule.numInstances;
    return instance;
}

void* _rwFrameClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    if (frameFreeList() != nullptr) {
        RwFreeListDestroy(frameFreeList());
        frameFreeList() = nullptr;
    }
    --frameModule.numInstances;
    return instance;
}

RwFrame* RwFrameCreate() {
    if (frameModule.numInstances <= 0) {   // native: no frame module open (the original uses whatever the slot holds)
        RwError error{rwID_COREPLUGIN, _rwerror(static_cast<RwInt32>(0x80000001u))};   // E_RW_BADENGINESTATE
        RwErrorSet(&error);
        return nullptr;
    }
    auto* frame = static_cast<RwFrame*>(rwSrcGlobals().memoryAlloc(frameFreeList(), kFrameHint));
    if (frame == nullptr) return nullptr;
    frame->object = {rwFRAME, 0, 0, 0, nullptr};
    frame->objectList.link.next = frame->objectList.link.prev = &frame->objectList.link;
    frame->inDirtyListLink = {nullptr, nullptr};
    std::memset(&frame->modelling, 0, sizeof frame->modelling);   // pads: 0 (the original leaves them as they were)
    std::memset(&frame->ltm, 0, sizeof frame->ltm);
    storeIdentity(frame->modelling);
    storeIdentity(frame->ltm);
    frame->child = nullptr;
    frame->next = nullptr;
    frame->root = frame;
    if (_rwPluginRegistryInitObject(&frameTKList, frame) == nullptr) {
        // native: a failed plugin constructor fails the creation (the original returns the half-built frame); the
        // constructors that ran were undone by the registry
        rwSrcGlobals().memoryFree(frameFreeList(), frame);
        return nullptr;
    }
    return frame;
}

RwBool RwFrameDestroy(RwFrame* frame) {
    _rwPluginRegistryDeInitObject(&frameTKList, frame);
    if (frame->object.parent != nullptr) RwFrameRemoveChild(frame);
    leaveDirtyList(frame);
    // native: each child becomes a root of its own, marked for an update (the original only clears its parent, so
    // its root keeps pointing at the freed frame and the next RwFrameGetLTM reads freed memory)
    for (RwFrame* child = frame->child; child != nullptr;) {
        RwFrame* next = child->next;
        child->object.parent = nullptr;
        child->next = nullptr;
        setHierarchyRoot(child, child);
        markForUpdate(child);
        child = next;
    }
    releaseObjects(frame);
    rwSrcGlobals().memoryFree(frameFreeList(), frame);
    return 1;
}

RwBool RwFrameDestroyHierarchy(RwFrame* frame) {
    if (frame == nullptr) return 1;
    // native: detached first (the original leaves its parent's child list pointing at the freed frames)
    if (frame->object.parent != nullptr) RwFrameRemoveChild(frame);
    destroyRecurse(frame);
    return 1;
}

RwFrame* RwFrameUpdateObjects(RwFrame* frame) {
    markForUpdate(frame);
    return frame;
}

RwBool RwFrameDirty(const RwFrame* frame) { return frame->root->object.privateFlags & kSyncBits; }

RwMatrix* RwFrameGetLTM(RwFrame* frame) {
    if (frame->root->object.privateFlags & rwFRAMEPRIVATEHIERARCHYSYNCLTM) _rwFrameSyncHierarchyLTM(frame->root);
    return &frame->ltm;
}

RwFrame* RwFrameGetRoot(const RwFrame* frame) { return frame->root; }

RwFrame* RwFrameAddChildNoUpdate(RwFrame* parent, RwFrame* child) {
    // native: child must not be parent or one of its ancestors (the original builds a cycle). A frame without
    // children is nobody's ancestor, so only then is the walk up from parent needed.
    for (const RwFrame* p = parent; p != nullptr; p = child->child != nullptr ? parentOf(p) : nullptr) {
        if (p == child) {
            RwError error{rwID_COREPLUGIN, _rwerror(E_RW_BADPARAM, "a frame cannot become its own descendant")};
            RwErrorSet(&error);
            return nullptr;
        }
    }
    if (child->object.parent != nullptr) RwFrameRemoveChild(child);
    child->next = parent->child;
    parent->child = child;
    child->object.parent = parent;
    setHierarchyRoot(child, parent->root);
    return parent;
}

RwFrame* RwFrameAddChild(RwFrame* parent, RwFrame* child) {
    if (RwFrameAddChildNoUpdate(parent, child) == nullptr) return nullptr;
    if (child->object.privateFlags & kSyncBits) {   // a former root leaves the dirty list
        unlink(&child->inDirtyListLink);
        child->object.privateFlags = static_cast<RwUInt8>(child->object.privateFlags & ~kSyncBits);
    }
    markForUpdate(child);
    return parent;
}

RwFrame* RwFrameRemoveChild(RwFrame* child) {
    RwFrame* parent = parentOf(child);
    if (parent == nullptr) return child;   // native: nothing to remove (the original dereferences NULL)
    if (parent->child == child) {
        parent->child = child->next;
    } else {
        RwFrame* before = parent->child;
        while (before->next != child) before = before->next;
        before->next = child->next;
    }
    child->object.parent = nullptr;
    child->next = nullptr;
    setHierarchyRoot(child, child);
    markForUpdate(child);
    return child;
}

RwFrame* RwFrameForAllChildren(RwFrame* frame, RwFrameCallBack callBack, void* data) {
    for (RwFrame* child = frame->child; child != nullptr;) {
        RwFrame* next = child->next;
        if (callBack(child, data) == nullptr) break;
        child = next;
    }
    return frame;
}

RwFrame* RwFrameTranslate(RwFrame* frame, const RwV3d* v, RwOpCombineType combine) {
    RwMatrixTranslate(&frame->modelling, v, combine);
    markForUpdate(frame);
    return frame;
}

RwFrame* RwFrameScale(RwFrame* frame, const RwV3d* v, RwOpCombineType combine) {
    RwMatrixScale(&frame->modelling, v, combine);
    markForUpdate(frame);
    return frame;
}

RwFrame* RwFrameTransform(RwFrame* frame, const RwMatrix* m, RwOpCombineType combine) {
    RwMatrixTransform(&frame->modelling, m, combine);
    markForUpdate(frame);
    return frame;
}

RwFrame* RwFrameRotate(RwFrame* frame, const RwV3d* axis, RwReal angle, RwOpCombineType combine) {
    RwMatrixRotate(&frame->modelling, axis, angle, combine);
    markForUpdate(frame);
    return frame;
}

RwFrame* RwFrameSetIdentity(RwFrame* frame) {
    const RwUInt32 flags = frame->modelling.flags;
    storeIdentity(frame->modelling);
    frame->modelling.flags = flags | RwUInt32{rwMATRIXINTERNALIDENTITY} | RwUInt32{rwMATRIXTYPEORTHONORMAL};
    markForUpdate(frame);
    return frame;
}

RwInt32 RwFrameRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                              RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB) {
    return _rwPluginRegistryAddPlugin(&frameTKList, size, pluginID, constructCB, destructCB, copyCB);
}

RwFrame* _rwFrameCloneAndLinkClones(RwFrame* root) {
    // rwFrameCloneRecurse (0x80487AF4), iteratively: each copy is allocated before its source's children are copied
    // (pre-order), attached to its parent copy at the head once its own children are done, and gets its plugins'
    // constructors and copy callbacks then (post-order) - the original's order.
    struct Step {
        RwFrame* source;
        RwFrame* copy;
        RwFrame* nextChild;   // the source child to copy next
    };
    std::vector<Step> stack;
    RwFrame* cloneRoot = nullptr;
    const auto start = [&](RwFrame* source) -> bool {
        auto* copy = static_cast<RwFrame*>(rwSrcGlobals().memoryAlloc(frameFreeList(), kFrameHint));
        if (copy == nullptr) return false;
        if (cloneRoot == nullptr) cloneRoot = copy;
        copy->object = source->object;
        copy->object.parent = nullptr;
        // native: a copy is in no dirty list, so it does not keep a source's "in the list" bits (the original copies
        // them; only the root's are cleared, and a later destroy of such a copy unlinks garbage)
        copy->object.privateFlags = static_cast<RwUInt8>(copy->object.privateFlags & ~kSyncBits);
        copy->objectList.link.next = copy->objectList.link.prev = &copy->objectList.link;
        copy->inDirtyListLink = {nullptr, nullptr};
        copy->modelling = source->modelling;
        std::memset(&copy->ltm, 0, sizeof copy->ltm);   // the original leaves it (the update below recomputes it)
        copy->child = nullptr;
        copy->next = nullptr;
        copy->root = cloneRoot;
        source->root = copy;                           // the link the caller maps objects through
        stack.push_back({source, copy, source->child});
        return true;
    };
    if (!start(root)) return nullptr;
    while (!stack.empty()) {
        Step& top = stack.back();
        if (top.nextChild != nullptr) {
            RwFrame* source = top.nextChild;
            top.nextChild = source->next;
            if (!start(source)) {
                // native: every copy on the stack is still unconstructed - its finished children are destroyed (they
                // were constructed), the copy itself freed without destructors (the original destroys it, running
                // destructors on plugin data never constructed); the sources' roots are restored (the original
                // leaves them pointing at the freed copies)
                for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                    for (RwFrame* c = it->copy->child; c != nullptr;) {
                        RwFrame* next = c->next;
                        destroyRecurse(c);
                        c = next;
                    }
                    rwSrcGlobals().memoryFree(frameFreeList(), it->copy);
                }
                _rwFramePurgeClone(root);
                return nullptr;
            }
            continue;
        }
        const Step done = top;
        stack.pop_back();
        _rwPluginRegistryInitObject(&frameTKList, done.copy);
        _rwPluginRegistryCopyObject(&frameTKList, done.copy, done.source);
        if (!stack.empty()) {
            RwFrame* parent = stack.back().copy;
            done.copy->next = parent->child;
            parent->child = done.copy;
            done.copy->object.parent = parent;
        }
    }
    cloneRoot->object.privateFlags = static_cast<RwUInt8>(cloneRoot->object.privateFlags & ~kSyncBits);
    RwFrameUpdateObjects(cloneRoot);
    return cloneRoot;
}

RwFrame* _rwFramePurgeClone(RwFrame* frame) {
    setHierarchyRoot(frame, frame->object.parent != nullptr ? parentOf(frame)->root : frame);
    return frame;
}

void _rwFrameSyncHierarchyLTM(RwFrame* root) {
    const RwUInt8 flags = root->object.privateFlags;
    if (flags & rwFRAMEPRIVATESUBTREESYNCLTM) root->ltm = root->modelling;
    // FrameSyncHierarchyLTMRecurse (0x8048C410)
    walkBelow(root, flags, [](RwFrame* f, RwUInt8 inherited) {
        const RwUInt8 combined = static_cast<RwUInt8>(inherited | f->object.privateFlags);
        if (combined & rwFRAMEPRIVATESUBTREESYNCLTM) {
            updateLTM(f);
            f->object.privateFlags = static_cast<RwUInt8>(f->object.privateFlags & ~rwFRAMEPRIVATESUBTREESYNCLTM);
        }
        return combined;
    });
    root->object.privateFlags =
        static_cast<RwUInt8>(flags & ~(rwFRAMEPRIVATEHIERARCHYSYNCLTM | rwFRAMEPRIVATESUBTREESYNCLTM));
}

RwBool _rwFrameSyncDirty() {
    RwLLLink* head = &dirtyFrames().link;
    for (RwLLLink* link = head->next; link != head; link = link->next) {
        auto* root = reinterpret_cast<RwFrame*>(reinterpret_cast<char*>(link) - offsetof(RwFrame, inDirtyListLink));
        const RwUInt8 flags = root->object.privateFlags;
        if (flags & rwFRAMEPRIVATEHIERARCHYSYNCLTM) {
            const RwUInt8 rootLTM = flags & rwFRAMEPRIVATESUBTREESYNCLTM;
            if (rootLTM) root->ltm = root->modelling;
            syncObjects(root);
            // FrameSyncHierarchyRecurse (0x8048BE30): the root's children inherit only its LTM bit
            walkBelow(root, rootLTM, [](RwFrame* f, RwUInt8 inherited) {
                const RwUInt8 combined = static_cast<RwUInt8>(inherited | f->object.privateFlags);
                if (combined & rwFRAMEPRIVATESUBTREESYNCLTM) updateLTM(f);
                syncObjects(f);
                f->object.privateFlags = static_cast<RwUInt8>(f->object.privateFlags & ~kSubtreeBits);
                return combined;
            });
        } else {
            syncObjects(root);
            // FrameSyncHierarchyRecurseNoLTM (0x8048C040)
            walkBelow(root, 0, [](RwFrame* f, RwUInt8) {
                syncObjects(f);
                f->object.privateFlags = static_cast<RwUInt8>(f->object.privateFlags & ~rwFRAMEPRIVATESUBTREESYNCOBJ);
                return RwUInt8{0};
            });
        }
        root->object.privateFlags = static_cast<RwUInt8>(flags & 0xF0);
    }
    head->next = head->prev = head;
    return 1;
}

void _rwObjectHasFrameSetFrame(void* object, RwFrame* frame) {
    auto* o = static_cast<RwObjectHasFrame*>(object);
    if (o->object.parent != nullptr) unlink(&o->lFrame);
    o->object.parent = frame;
    if (frame != nullptr) {
        linkAtHead(&frame->objectList.link, &o->lFrame);
        RwFrameUpdateObjects(frame);
    }
}

void _rwObjectHasFrameReleaseFrame(void* object) {
    auto* o = static_cast<RwObjectHasFrame*>(object);
    if (o->object.parent != nullptr) unlink(&o->lFrame);
}
