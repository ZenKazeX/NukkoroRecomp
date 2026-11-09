// RenderWare Graphics 3.7 core: errors (module baerr), native x64.
//
// The engine keeps one error, the first one set since the last RwErrorGet. Rebuilt from the game's functions and
// proven by runtime/tests/rwcore_vector_tests.cpp (the error module is what _rwVectorOpen reports through).
#pragma once

#include "rw/core/engine.h"
#include "rw/core/types.h"

/// An error: the plugin that raised it and its code (rwplcore.h RwError). Codes with the top bit set are the
/// core's own (E_RW_*), whose plugin is stored as 0.
struct RwError {
    RwInt32 pluginID;
    RwInt32 errorCode;
};

inline constexpr RwInt32 rwERRORNONE = static_cast<RwInt32>(0x80000000u);  ///< errorCode when no error is held
inline constexpr RwInt32 E_RW_NOMEM = static_cast<RwInt32>(0x80000013u);   ///< out of memory (with the size asked)
inline constexpr RwInt32 E_RW_BADPARAM = static_cast<RwInt32>(0x80000003u); ///< invalid parameter (with a message)
inline constexpr RwInt32 E_RW_PLUGININIT = static_cast<RwInt32>(0x80000017u); ///< a plugin registered late or twice
inline constexpr RwInt32 E_RW_ZEROLENGTH = 0x19;                             ///< a zero-length vector (core plugin code)
inline constexpr RwInt32 E_RW_INSUFFICIENTRESOURCES = 0x0C;                  ///< the resources arena is full (core plugin code)
inline constexpr RwInt32 rwID_COREPLUGIN = 0x01;

/// The error module's registration (its globals hold one RwError).
extern RwModuleInfo errorModule;

/// Opens the error module: no error held ({0, rwERRORNONE}), or the one raised while no error module was open
/// (native: such errors are held aside, not written over RwGlobals). Returns instance. Original: 0x8047EB88.
void* _rwErrorOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Closes the error module. Returns instance. Original: 0x8047EBC0.
void* _rwErrorClose(void* instance, RwInt32 offset, RwInt32 size);

/// Records code unless an error is already held; a core code (top bit set) is stored with plugin 0. Returns code.
/// Original: 0x8047EBD4.
RwError* RwErrorSet(RwError* code);

/// Copies the held error into code and clears it. Returns code. Original: 0x8047EC34.
RwError* RwErrorGet(RwError* code);

/// An error code with its arguments (formatted only in debug builds): returns code. Original: 0x8047EC78.
RwInt32 _rwerror(RwInt32 code, ...);
