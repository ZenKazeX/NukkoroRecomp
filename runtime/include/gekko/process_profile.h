#pragma once

// PROCESS-ONLY RESOURCE PLOTS FOR THE PROFILER.
//
// Every figure published here describes THIS process and nothing else: its
// own CPU time, its own memory, its own I/O, its own handles. Machine-wide
// readings (total RAM free, total CPU busy, other processes' activity) are
// deliberately absent - a profile of the game that moves because a browser
// woke up is a profile of the browser.
//
// Plots, all under host/process:
//   cpu cores      CPU time consumed per wall second (2.5 = two and a half
//                  logical processors' worth)
//   working set    physical memory currently mapped for the process
//   private bytes  memory committed by the process (what it has asked for)
//   page faults/s  soft and hard faults the process took
//   io read/s      bytes the process read, from any file or device
//   io write/s     bytes the process wrote
//   handles        kernel handles the process holds
//
// Compiles to nothing without SHADOW_TRACY, so the shipped binary contains no
// call and no code.

namespace gekko {

#if defined(SHADOW_TRACY)
// Call regularly from ONE thread; it limits itself to four samples a second
// and does nothing while no profiler is attached.
void publish_process_resources() noexcept;
#else
inline void publish_process_resources() noexcept {}
#endif

}  // namespace gekko
