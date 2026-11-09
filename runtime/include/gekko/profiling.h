#pragma once

// HOST PROFILING VOCABULARY.
//
// Every macro here compiles to nothing unless SHADOW_TRACY is defined, which
// only the build-tracy tree does. That is the whole contract: instrumentation
// must be free in the shipped binary, because the shipped binary is the one the
// pixel gate hashes and the one a paired A/B measures. A Tracy build is NEVER
// the binary that produces a gate hash or a benchmark number - use it to find
// WHERE the time goes, then confirm the size of the win on build-native.
//
// NAMING. Zone names are "subsystem/what", lower case, stable across builds:
// gpu/, res/, cpu/, aud/, rw/, sched/, mem/, present/. Tracy groups and diffs
// by name, so a name that changes between builds cannot be compared.
//
// EVENT RATE IS THE DESIGN CONSTRAINT. Tracy records every zone; a site that
// fires per guest instruction, per GX command or per vertex produces tens of
// millions of events a second, and the profiler then measures its own overhead
// instead of the program. Instrument per BATCH, per DRAW, per FRAME or per
// RESOURCE - never per element. If a site can fire more than ~100k times a
// second it belongs in a plot or a counter, not a zone.

#if defined(SHADOW_TRACY)
#include <cstring>

#include <tracy/Tracy.hpp>

// ---- CPU zones -----------------------------------------------------------
#define GEKKO_PROFILE_ZONE(name) ZoneScopedN(name)
#define GEKKO_PROFILE_ZONE_COLOR(name, color) ZoneScopedNC(name, color)
#define GEKKO_PROFILE_ZONE_DYNAMIC(name) \
    ZoneScoped;                          \
    ZoneName((name), std::strlen(name))
// Attaches a number or a string to the zone that is currently open, so a slow
// zone says WHICH resource or HOW MANY items it was working on. This is the
// difference between "res/decode took 8 ms" and "res/decode took 8 ms for a
// 1024x1024 CMPR map".
#define GEKKO_PROFILE_VALUE(value) ZoneValue(static_cast<uint64_t>(value))
#define GEKKO_PROFILE_TEXT(text, length) ZoneText(text, length)

// ---- threads, frames, messages -------------------------------------------
#define GEKKO_PROFILE_THREAD(name) tracy::SetThreadName(name)
#define GEKKO_PROFILE_FRAME(name) FrameMarkNamed(name)
#define GEKKO_PROFILE_FRAME_START(name) FrameMarkStart(name)
#define GEKKO_PROFILE_FRAME_END(name) FrameMarkEnd(name)
#define GEKKO_PROFILE_MESSAGE(text, length) TracyMessage(text, length)
#define GEKKO_PROFILE_MESSAGE_LITERAL(text) TracyMessageL(text)
#define GEKKO_PROFILE_APP_INFO(text, length) TracyAppInfo(text, length)
// True only while a profiler is attached. Guards work that exists solely to
// produce profiling data - formatting a name, walking a pool to plot its size -
// so an unattached Tracy build still runs at close to full speed.
#define GEKKO_PROFILE_CONNECTED() TracyIsConnected
// Code that exists ONLY to feed the profiler and must not be in the shipped
// binary at all. GEKKO_PROFILE_CONNECTED() is the wrong tool for anything that
// has to run before a profiler attaches - on-demand means nothing is connected
// at startup, so a CONNECTED() guard there would silence it permanently.
// Variadic so a block containing commas still passes through in one piece.
#define GEKKO_PROFILE_ONLY(...) __VA_ARGS__

// ---- plots ---------------------------------------------------------------
// A plot is the right shape for a QUANTITY over time: pool occupancy, bytes
// resident, queue depth, refusals. Plots are cheap and do not nest, so they
// carry the resource picture that zones cannot.
#define GEKKO_PROFILE_PLOT(name, value) TracyPlot(name, value)
// Declares how a plot is drawn once, at startup. Format: 0 Number, 1 Memory
// (bytes, shown as KiB/MiB), 2 Percentage. step = draw as a staircase rather
// than interpolating, which is what a resource count actually does.
#define GEKKO_PROFILE_PLOT_CONFIG(name, format, step, fill, color) \
    TracyPlotConfig(name, static_cast<tracy::PlotFormatType>(format), step, fill, color)

// ---- memory --------------------------------------------------------------
// Named pools. Tracy then reports, per pool, live bytes over time, the call
// stack of every outstanding allocation and any double free - which is the
// resource question that aggregate counters have never been able to answer.
#define GEKKO_PROFILE_ALLOC(ptr, size, name) TracyAllocN(ptr, size, name)
#define GEKKO_PROFILE_FREE(ptr, name) TracyFreeN(ptr, name)

// ---- locks ---------------------------------------------------------------
// A lock declared this way shows contention: who held it, who waited, for how
// long. The renderer's ledger and the segment queue are the two places where a
// guest thread and a render thread meet, and neither could be seen before.
#define GEKKO_PROFILE_LOCKABLE(type, name) TracyLockable(type, name)
#define GEKKO_PROFILE_LOCKABLE_N(type, name, description) TracyLockableN(type, name, description)
#define GEKKO_PROFILE_LOCKABLE_BASE(type) LockableBase(type)

// ---- GPU (Direct3D 12) ---------------------------------------------------
// GPU zones are timestamps the GPU itself writes, so they show what the device
// did rather than what the CPU asked for. A context is per command QUEUE and
// must be collected once a frame on the thread that owns it.
//
// TracyD3D12.hpp includes windows.h, and windows.h defines min and max as
// MACROS. That turns every later std::numeric_limits<T>::max() into a syntax
// error - which is a defect this header would have INTRODUCED into files that
// never included windows.h before, and only in the profiling build, where it
// is least likely to be noticed. NOMINMAX prevents it; the undefs afterwards
// cover the case where something earlier in the translation unit already
// pulled windows.h in without it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <tracy/TracyD3D12.hpp>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#define GEKKO_PROFILE_GPU_CONTEXT(device, queue) TracyD3D12Context(device, queue)
#define GEKKO_PROFILE_GPU_CONTEXT_NAME(ctx, name, size) TracyD3D12ContextName(ctx, name, size)
#define GEKKO_PROFILE_GPU_DESTROY(ctx) TracyD3D12Destroy(ctx)
#define GEKKO_PROFILE_GPU_ZONE(ctx, list, name) TracyD3D12Zone(ctx, list, name)
#define GEKKO_PROFILE_GPU_ZONE_TRANSIENT(ctx, var, list, name, active) \
    TracyD3D12ZoneTransient(ctx, var, list, name, active)
#define GEKKO_PROFILE_GPU_NEW_FRAME(ctx) TracyD3D12NewFrame(ctx)
#define GEKKO_PROFILE_GPU_COLLECT(ctx) TracyD3D12Collect(ctx)
#define GEKKO_PROFILE_GPU_CONTEXT_TYPE TracyD3D12Ctx

#else

#define GEKKO_PROFILE_ZONE(name) ((void)0)
#define GEKKO_PROFILE_ZONE_COLOR(name, color) ((void)0)
#define GEKKO_PROFILE_ZONE_DYNAMIC(name) ((void)0)
#define GEKKO_PROFILE_VALUE(value) ((void)0)
#define GEKKO_PROFILE_TEXT(text, length) ((void)0)

#define GEKKO_PROFILE_THREAD(name) ((void)0)
#define GEKKO_PROFILE_FRAME(name) ((void)0)
#define GEKKO_PROFILE_FRAME_START(name) ((void)0)
#define GEKKO_PROFILE_FRAME_END(name) ((void)0)
#define GEKKO_PROFILE_MESSAGE(text, length) ((void)0)
#define GEKKO_PROFILE_MESSAGE_LITERAL(text) ((void)0)
#define GEKKO_PROFILE_APP_INFO(text, length) ((void)0)
#define GEKKO_PROFILE_CONNECTED() false
#define GEKKO_PROFILE_ONLY(...)

#define GEKKO_PROFILE_PLOT(name, value) ((void)0)
#define GEKKO_PROFILE_PLOT_CONFIG(name, format, step, fill, color) ((void)0)

#define GEKKO_PROFILE_ALLOC(ptr, size, name) ((void)0)
#define GEKKO_PROFILE_FREE(ptr, name) ((void)0)

// Without Tracy these must still declare a REAL mutex of the requested type:
// the code that uses them locks and unlocks through the same names.
#define GEKKO_PROFILE_LOCKABLE(type, name) type name
#define GEKKO_PROFILE_LOCKABLE_N(type, name, description) type name
#define GEKKO_PROFILE_LOCKABLE_BASE(type) type

#define GEKKO_PROFILE_GPU_CONTEXT(device, queue) nullptr
#define GEKKO_PROFILE_GPU_CONTEXT_NAME(ctx, name, size) ((void)0)
#define GEKKO_PROFILE_GPU_DESTROY(ctx) ((void)0)
#define GEKKO_PROFILE_GPU_ZONE(ctx, list, name) ((void)0)
#define GEKKO_PROFILE_GPU_ZONE_TRANSIENT(ctx, var, list, name, active) ((void)0)
#define GEKKO_PROFILE_GPU_NEW_FRAME(ctx) ((void)0)
#define GEKKO_PROFILE_GPU_COLLECT(ctx) ((void)0)
#define GEKKO_PROFILE_GPU_CONTEXT_TYPE void*

#endif

// ---- plot format constants, usable from either build ---------------------
#define GEKKO_PLOT_NUMBER 0
#define GEKKO_PLOT_MEMORY 1
#define GEKKO_PLOT_PERCENTAGE 2
