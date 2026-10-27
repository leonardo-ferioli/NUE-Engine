/*
 * NUE engine - umbrella header. Include this, or just the module headers you use.
 *
 *   base.h      platform macros, version, result codes, handle/struct conventions, allocator
 *   core.h      init/shutdown, log, config, hashing, time, threads, extensions
 *   app.h       events, app lifecycle, host loop / engine loop
 *   window.h    displays, windows, cursor
 *   input.h     keyboard, mouse, gamepad, touch, sensors, text input, last-used device
 *   audio.h     devices and output streams
 *   mixer.h     software mixer (voices, buses, 3D)
 *   storage.h   virtual file system, async read, integrity, game data import
 *   power.h     power/thermal/game mode, thread priority and affinity, performance hints
 *   system.h    system info, platform class (SteamOS, gamescope...) and capabilities
 *   memory.h    memory info, pressure events, configurable budgets with eviction hooks
 *   gfx.h       graphics context (bgfx) and the Direct3D 9 layer seam
 *   diag.h      crash reports, assertions, counters, frame stats, trace
 *   vm.h        virtual address space: reserve, commit, protect, decommit (arenas, sparse tables)
 *   null_backend.h  test control of the headless backend (not included here)
 *   entry.h     portable entry point (nue::entry target; not included here)
 */
#ifndef NUE_NUE_H
#define NUE_NUE_H

#include "nue/base.h"
#include "nue/core.h"
#include "nue/app.h"
#include "nue/window.h"
#include "nue/input.h"
#include "nue/audio.h"
#include "nue/mixer.h"
#include "nue/storage.h"
#include "nue/power.h"
#include "nue/system.h"
#include "nue/memory.h"
#include "nue/gfx.h"
#include "nue/diag.h"
#include "nue/vm.h"

/* Optional modules, present when their headers are installed (owned by their own sessions). */
#if defined(__has_include)
#  if __has_include("nue/texture.h")
#    include "nue/texture.h"
#  endif
#endif

#endif /* NUE_NUE_H */
