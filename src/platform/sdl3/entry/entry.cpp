// nue::entry - portable program entry. Applications define `int nue_main(int, char**)`; this stub supplies the OS entry point
// (main on desktop, SDL_main on Android, UIApplicationMain on iOS through SDL) and calls nue_main on the thread the OS designates.
#include "nue/entry.h"

#include <SDL3/SDL_main.h>

int main(int argc, char** argv) { return nue_main(argc, argv); }
