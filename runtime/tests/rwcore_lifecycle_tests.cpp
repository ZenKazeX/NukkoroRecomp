// Native tests of the RenderWare engine's life (rwcore/src/device.cpp): RwEngineInit -> Open -> Start -> Stop ->
// Close -> Term, repeated, with and without free lists; every allocation failing in turn during start-up (each failure
// must unwind, leave no allocation behind and let a later start-up succeed); device requests failing; a module's open
// callback failing; the order of plugin callbacks; plugin registration only before RwEngineOpen; misuse of the state
// order; errors raised before the error module is open. The GameCube device cannot run here (it is the driver), so
// these tests check the native engine's behaviour, not an original's: the original sequence each function follows is
// in device.cpp. Memory is counted by a recording allocator: every test ends with no live allocation.
//
// Usage: rwcore_lifecycle_tests
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/frame.h"
#include "rw/core/matrix.h"
#include "rw/core/memory.h"
#include "rw/core/plugin.h"
#include "rw/core/stream.h"
#include "rw/core/vector.h"

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

// ---- a counting allocator that can fail a chosen call ----------------------------------------------------------
std::map<void*, std::size_t> g_live;
long g_calls = 0, g_fail_at = -1;
void* countMalloc(std::size_t size, RwUInt32) {
    if (g_calls++ == g_fail_at) return nullptr;
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
    if (g_calls++ == g_fail_at) return nullptr;
    require(g_live.count(p) == 1, "realloc of a foreign pointer");
    void* q = std::realloc(p, size ? size : 1);
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

// ---- a device that can fail a chosen request ---------------------------------------------------------------------
RwDevice* g_host = nullptr;
int g_fail_request = -1;
std::vector<int> g_requests;
RwBool scriptedSystem(RwInt32 request, void* pOut, void* pInOut, RwInt32 numIn) {
    g_requests.push_back(request);
    if (request == g_fail_request) return 0;
    if (request == rwDEVICESYSTEMREGISTER) {
        const RwBool r = g_host->fpSystem(request, pOut, pInOut, numIn);
        static_cast<RwDevice*>(pOut)->fpSystem = scriptedSystem;   // the engine's copy keeps asking us
        return r;
    }
    return g_host->fpSystem(request, pOut, pInOut, numIn);
}
RwDevice g_scripted{};

// ---- an application plugin that records its callbacks ---------------------------------------------------------
std::vector<std::string> g_events;
bool g_plugin_fails = false;
void* appInit(void* instance, RwInt32 offset, RwInt32 size) {
    const auto* bytes = static_cast<const unsigned char*>(instance) + offset;
    bool zero = true;
    for (RwInt32 i = 0; i < size; ++i) zero = zero && bytes[i] == 0;
    g_events.push_back(std::string("init ") + (zero ? "zeroed" : "dirty") + " " + std::to_string(offset % 8));
    // the core modules are open by now (registered first)
    g_events.push_back(std::string("vector ") + (_rwSqrt(16.0f) == 4.0f ? "open" : "closed"));
    return g_plugin_fails ? nullptr : instance;
}
void* appTerm(void* instance, RwInt32, RwInt32) {
    // registered last, closed first: the core modules are still open
    g_events.push_back(std::string("term vector ") + (vectorModule.numInstances > 0 ? "open" : "closed"));
    return instance;
}

/// The engine is fully usable: every ported core module answers.
void exercise(const char* what) {
    const std::string w = what;
    require(rwEngineGetStatus() == rwENGINESTATUSSTARTED, w + ": not started");
    require(_rwSqrt(9.0f) == 3.0f && _rwInvSqrt(4.0f) == 0.5f, w + ": vector tables");
    RwError set{rwID_COREPLUGIN, E_RW_BADPARAM};
    RwErrorSet(&set);
    const RwError got = takeError();
    require(got.pluginID == 0 && got.errorCode == E_RW_BADPARAM && takeError().errorCode == rwERRORNONE, w + ": errors");
    RwMatrix* m = RwMatrixCreate();
    require(m != nullptr && m->right.x == 1.0f && m->up.y == 1.0f && m->at.z == 1.0f, w + ": RwMatrixCreate");
    require(RwMatrixDestroy(m) == 1, w + ": RwMatrixDestroy");
    RwMemory memory{nullptr, 0};
    RwStream* s = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMWRITE, &memory);
    const char text[] = "native";
    require(s != nullptr && RwStreamWrite(s, text, sizeof text) == s && RwStreamClose(s, &memory) == 1 &&
                memory.length == sizeof text && std::memcmp(memory.start, text, sizeof text) == 0,
            w + ": memory stream");
    RwFree(memory.start);
    RwFrame* parent = RwFrameCreate();
    RwFrame* child = RwFrameCreate();
    const RwV3d t{1.0f, 2.0f, 3.0f};
    require(parent != nullptr && child != nullptr && RwFrameAddChild(parent, child) == parent &&
                RwFrameTranslate(parent, &t, rwCOMBINEREPLACE) == parent && RwFrameGetLTM(child)->pos.z == 3.0f &&
                _rwFrameSyncDirty() == 1 && RwFrameDestroyHierarchy(parent) == 1,
            w + ": frames");
    for (RwUInt32 id : {0x40Fu, 0x401u, 0x40Du, 0x402u, 0x403u, 0x404u}) {
        const RwInt32 offset = RwEngineGetPluginOffset(id);
        require(offset >= static_cast<RwInt32>(sizeof(RwGlobals)) && offset % 8 == 0, w + ": a module offset");
    }
}

void fullCycle(RwUInt32 flags, const char* what) {
    const std::string w = what;
    RwEngineOpenParams params{nullptr};
    require(RwEngineInit(&kCounting, flags, 0) == 1, w + ": RwEngineInit");
    require(rwEngineGetStatus() == rwENGINESTATUSINITED, w + ": inited");
    require(RwEngineOpen(&params) == 1 && rwEngineGetStatus() == rwENGINESTATUSOPENED, w + ": RwEngineOpen");
    require(RwEngineStart() == 1, w + ": RwEngineStart");
    exercise(what);
    require(RwEngineStop() == 1 && rwEngineGetStatus() == rwENGINESTATUSOPENED, w + ": RwEngineStop");
    require(errorModule.numInstances == 0 && vectorModule.numInstances == 0 && matrixModule.numInstances == 0 &&
                frameModule.numInstances == 0 && streamModule.numInstances == 0,
            w + ": a module stays open after RwEngineStop");
    require(RwEngineClose() == 1 && rwEngineGetStatus() == rwENGINESTATUSINITED, w + ": RwEngineClose");
    require(RwEngineTerm() == 1 && rwEngineGetStatus() == rwENGINESTATUSIDLE, w + ": RwEngineTerm");
    require(_rwFreeListMaster() == nullptr, w + ": RwEngineTerm leaves memory open");
    require(_rwFreeListLiveIndexes() == 0, w + ": a free list's block index outlives RwEngineTerm");
    require(g_live.empty(), w + ": " + std::to_string(g_live.size()) + " allocations outlive RwEngineTerm");
}

}  // namespace

int main() {
    try {
        unsigned checks = 0;
        g_host = rwDeviceHandle();
        g_scripted = *g_host;
        g_scripted.fpSystem = scriptedSystem;

        // ---- repeated full cycles, both memory modes ------------------------------------------------------------
        for (int i = 0; i < 200; ++i) fullCycle(i % 2 ? rwENGINEINITNOFREELISTS : rwENGINEINITFREELISTS, "cycle"), ++checks;

        // ---- every allocation of start-up failing in turn --------------------------------------------------------
        for (RwUInt32 flags : {rwENGINEINITFREELISTS, rwENGINEINITNOFREELISTS}) {
            bool completed = false;
            for (long k = 0; !completed && k < 10000; ++k) {
                g_calls = 0, g_fail_at = k;
                RwEngineOpenParams params{nullptr};
                int stage = 0;
                if (RwEngineInit(&kCounting, flags, 0)) {
                    stage = 1;
                    if (RwEngineOpen(&params)) {
                        stage = 2;
                        if (RwEngineStart()) stage = 3;
                    }
                }
                completed = g_calls <= k;   // the failure point lies past start-up: everything succeeded
                require(completed == (stage == 3), "a start-up failed without a failed allocation (k=" + std::to_string(k) + ")");
                g_fail_at = -1;
                if (stage == 3) require(RwEngineStop() == 1, "stop");
                if (stage >= 2) require(RwEngineClose() == 1, "close");
                if (stage >= 1) require(RwEngineTerm() == 1, "term");
                require(rwEngineGetStatus() == rwENGINESTATUSIDLE, "not idle after unwinding (k=" + std::to_string(k) + ")");
                require(g_live.empty(), "allocation " + std::to_string(k) + " failing leaks " + std::to_string(g_live.size()));
                takeError();
                fullCycle(flags, "after a failed start-up");
                ++checks;
            }
            require(completed, "start-up never completed");
        }

        // ---- misuse of the order --------------------------------------------------------------------------------
        {
            RwEngineOpenParams params{nullptr};
            require(RwEngineStart() == 0 && RwEngineStop() == 0 && RwEngineClose() == 0 && RwEngineTerm() == 0,
                    "steps before RwEngineInit must fail");
            require(RwEngineInit(&kCounting, 0, 0) == 1 && RwEngineInit(&kCounting, 0, 0) == 0, "RwEngineInit twice");
            require(RwEngineStart() == 0 && takeError().errorCode == static_cast<RwInt32>(0x80000001u),
                    "RwEngineStart before RwEngineOpen: E_RW_BADENGINESTATE");
            require(RwEngineOpen(nullptr) == 0 && takeError().errorCode == static_cast<RwInt32>(0x80000016u),
                    "RwEngineOpen(NULL): E_RW_NULLP, held before the error module is open");
            require(RwEngineOpen(&params) == 1 && RwEngineOpen(&params) == 0 && RwEngineTerm() == 0,
                    "RwEngineOpen twice / RwEngineTerm while open");
            require(takeError().errorCode == static_cast<RwInt32>(0x80000001u), "the second open's E_RW_BADENGINESTATE");
            const RwInt32 late = RwEngineRegisterPlugin(16, 0x7777, appInit, appTerm);
            require(late == -1 && takeError().errorCode == E_RW_PLUGININIT, "a plugin registered after RwEngineOpen");
            require(RwEngineStart() == 1 && RwEngineClose() == 0 && RwEngineInit(&kCounting, 0, 0) == 0,
                    "RwEngineClose / RwEngineInit while started");
            require(takeError().errorCode == static_cast<RwInt32>(0x80000001u), "close while started: E_RW_BADENGINESTATE");
            exercise("after misuse");
            require(RwEngineStop() == 1 && RwEngineStop() == 0 && RwEngineClose() == 1 && RwEngineTerm() == 1,
                    "shutdown after misuse");
            require(g_live.empty(), "misuse leaks");
            takeError();
            checks += 12;
        }

        // ---- an application plugin: callbacks after the core modules, before them on stop; a failing one --------
        {
            RwEngineOpenParams params{nullptr};
            require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
            const RwInt32 offset = RwEngineRegisterPlugin(12, 0x7777, appInit, appTerm);
            require(offset >= static_cast<RwInt32>(sizeof(RwGlobals)) && offset % 8 == 0 &&
                        RwEngineGetPluginOffset(0x7777) == offset,
                    "the plugin's offset");
            require(RwEngineOpen(&params) == 1, "open");
            g_plugin_fails = true;
            require(RwEngineStart() == 0, "a failing plugin must fail RwEngineStart");
            require(errorModule.numInstances == 0 && vectorModule.numInstances == 0 && matrixModule.numInstances == 0 &&
                        streamModule.numInstances == 0 && rwEngineGetStatus() == rwENGINESTATUSOPENED,
                    "a failed start must close the opened modules again");
            g_plugin_fails = false;
            g_events.clear();
            require(RwEngineStart() == 1, "start after a failed start");
            require(RwEngineStop() == 1, "stop");
            require((g_events == std::vector<std::string>{"init zeroed 0", "vector open", "term vector open"}), "the plugin's callbacks");
            require(RwEngineClose() == 1 && RwEngineTerm() == 1 && g_live.empty(), "shutdown with a plugin");
            require(RwEngineGetPluginOffset(0x7777) == -1, "RwEngineTerm forgets the plugins");
            fullCycle(0, "after a plugin");
            checks += 6;
        }

        // ---- device requests failing ------------------------------------------------------------------------------
        for (int request : {rwDEVICESYSTEMOPEN, rwDEVICESYSTEMSTART, rwDEVICESYSTEMSTOP, rwDEVICESYSTEMCLOSE,
                            rwDEVICESYSTEMREGISTER, rwDEVICESYSTEMSTANDARDS}) {
            RwEngineOpenParams params{nullptr};
            rwDeviceHandle() = &g_scripted;
            g_fail_request = request;
            require(RwEngineInit(&kCounting, 0, 0) == 1, "init");
            const RwBool opened = RwEngineOpen(&params);
            require(opened == (request != rwDEVICESYSTEMOPEN && request != rwDEVICESYSTEMREGISTER),
                    "RwEngineOpen with a failing device request");
            if (!opened) {
                require(rwEngineGetStatus() == rwENGINESTATUSINITED && takeError().errorCode == 0x18,
                        "a failed open leaves the engine initialised (E_RW_SYSTEMREQUEST)");
                g_fail_request = -1;
                require(RwEngineOpen(&params) == 1, "open after a failed open");
            }
            takeError();
            const RwBool started = RwEngineStart();
            require(started == (request != rwDEVICESYSTEMSTART), "RwEngineStart with a failing device request");
            if (!started) {
                require(takeError().errorCode == 0x18, "a refused start: E_RW_SYSTEMREQUEST");
                g_fail_request = -1;
                require(RwEngineStart() == 1, "start after a failed start");
            }
            exercise("with a scripted device");
            const RwBool stopped = RwEngineStop();
            require(stopped == (request != rwDEVICESYSTEMSTOP), "RwEngineStop with a failing device request");
            if (!stopped) {
                require(rwEngineGetStatus() == rwENGINESTATUSOPENED && errorModule.numInstances == 0,
                        "a refused stop still closes the modules and leaves the engine opened");
                g_fail_request = -1;
            }
            const RwBool closed = RwEngineClose();
            require(closed == (request != rwDEVICESYSTEMCLOSE), "RwEngineClose with a failing device request");
            if (!closed) {
                require(rwEngineGetStatus() == rwENGINESTATUSOPENED, "a refused close keeps the instance");
                g_fail_request = -1;
                require(RwEngineClose() == 1, "close after a failed close");
            }
            g_fail_request = -1;
            require(RwEngineTerm() == 1 && g_live.empty(), "shutdown after device failures");
            rwDeviceHandle() = g_host;
            takeError();
            checks += 4;
        }

        // ---- an error raised before the error module opens is still there after RwEngineStart ------------------
        {
            RwEngineOpenParams params{nullptr};
            require(RwEngineInit(&kCounting, 0, 0) == 1 && RwEngineOpen(nullptr) == 0, "a NULL open");
            require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
            require(takeError().errorCode == static_cast<RwInt32>(0x80000016u), "the pending E_RW_NULLP after start");
            require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1 && g_live.empty(), "shutdown");
            ++checks;
        }

        // ---- the device's defaults --------------------------------------------------------------------------------
        {
            RwEngineOpenParams params{nullptr};
            require(RwEngineInit(&kCounting, 0, 0) == 1 && RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "start");
            RwInt32 n = -1, current = -1;
            RwSubSystemInfo info{};
            require(_rwDeviceSystemRequest(g_host, rwDEVICESYSTEMGETNUMSUBSYSTEMS, &n, nullptr, 0) == 1 && n == 1 &&
                        _rwDeviceSystemRequest(g_host, rwDEVICESYSTEMGETSUBSYSTEMINFO, &info, nullptr, 0) == 1 &&
                        std::strcmp(info.name, "Only rendering sub system") == 0 &&
                        _rwDeviceSystemRequest(g_host, rwDEVICESYSTEMGETCURRENTSUBSYSTEM, &current, nullptr, 0) == 1 &&
                        current == 0 && _rwDeviceSystemRequest(g_host, rwDEVICESYSTEMSETSUBSYSTEM, nullptr, nullptr, 0) == 1 &&
                        _rwDeviceSystemRequest(g_host, rwDEVICESYSTEMSETSUBSYSTEM, nullptr, nullptr, 1) == 0 &&
                        takeError().errorCode == 0x18 &&
                        _rwDeviceSystemRequest(g_host, rwDEVICESYSTEMGETSUBSYSTEMINFO, &info, nullptr, 1) == 0,
                    "the device request defaults");
            require(RwEngineGetVersion() == 0x37002u, "RwEngineGetVersion");
            takeError();
            require(RwEngineStop() == 1 && RwEngineClose() == 1 && RwEngineTerm() == 1 && g_live.empty(), "shutdown");
            checks += 7;
        }

        std::printf("rwcore_lifecycle_tests: %u checks passed (no allocation outlives a shutdown)\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_lifecycle_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
