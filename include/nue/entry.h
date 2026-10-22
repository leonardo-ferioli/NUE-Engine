/*
 * NUE engine - portable program entry.
 *
 * Applications that want the engine-owned loop (or run on mobile) define
 *
 *     int nue_main(int argc, char** argv);
 *
 * and link the CMake target  nue::entry.  The entry stub supplies the OS entry point on every platform
 * (main/WinMain on desktop, SDL's SDL_main on Android, UIApplicationMain on iOS) and calls nue_main on the thread the OS designates
 * as the main/UI thread (iOS requires window and event work there). nue_main normally just fills an nue_init_desc and nue_app_callbacks and
 * returns nue_app_run(...).
 *
 * Applications that own their loop and their entry point (a recompiled game with its own WinMain) do NOT link nue::entry and
 * call nue_init() themselves (nue_init_desc.loop_model = NUE_LOOP_HOST).
 *
 * This header only declares the contract; the stub lives in src/platform/<os> (entry_*.cpp) and in src/platform/sdl3/entry.cpp.
 */
#ifndef NUE_ENTRY_H
#define NUE_ENTRY_H

#include "nue/app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented by the application when it links nue::entry. */
int NUE_CALL nue_main(int argc, char** argv);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_ENTRY_H */
