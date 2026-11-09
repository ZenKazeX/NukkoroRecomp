// RenderWare errors (see rw/core/error.h).
#include "rw/core/error.h"

RwModuleInfo errorModule{};

namespace {

// Where errors go while no engine has the error module open (RwEngineOpen reports before RwEngineStart opens it).
// The original writes them at offset 0 of the engine block, over RwGlobals::curCamera.
RwError g_unopenedError{0, rwERRORNONE};

RwError& heldError() { return errorModule.numInstances > 0 ? rwModuleGlobals<RwError>(errorModule) : g_unopenedError; }

}  // namespace

void* _rwErrorOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    errorModule.globalsOffset = offset;
    ++errorModule.numInstances;
    // an error raised before the module opened is carried over (the original starts empty: it had written such an
    // error over RwGlobals::curCamera)
    rwModuleGlobals<RwError>(errorModule) = g_unopenedError;
    g_unopenedError = {0, rwERRORNONE};
    return instance;
}

void* _rwErrorClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    --errorModule.numInstances;
    return instance;
}

RwError* RwErrorSet(RwError* code) {
    RwError& held = heldError();
    if (held.pluginID != 0 || held.errorCode != rwERRORNONE) return code;  // the first error stays
    held.pluginID = (static_cast<RwUInt32>(code->errorCode) & 0x80000000u) ? 0 : code->pluginID;
    held.errorCode = code->errorCode;
    return code;
}

RwError* RwErrorGet(RwError* code) {
    RwError& held = heldError();
    *code = held;
    held = {0, rwERRORNONE};
    return code;
}

RwInt32 _rwerror(RwInt32 code, ...) { return code; }
