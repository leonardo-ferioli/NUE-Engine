// A client that lets the engine own the program entry point: it defines nue_main (nue/entry.h) and links nue::entry, which supplies main / WinMain /
// SDL_main for the platform. It runs headless on the SDL3 backend (SDL's dummy drivers when the environment selects them) and quits from the first frame.
#include "nue/entry.h"
#include "nuepp/nue.hpp"

#include <cstdio>

namespace {
int g_frames = 0;
nue_app_result on_frame(void*) {
    nue_frame_mark();
    return ++g_frames >= 3 ? NUE_APP_QUIT_OK : NUE_APP_CONTINUE;
}
}  // namespace

int NUE_CALL nue_main(int argc, char** argv) {
    nue_init_desc init = nuepp::init<nue_init_desc>();
    init.app_name = "consume_entry"; init.app_id = "consume-entry"; init.org_name = "NUE";
    init.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER;
    init.log_level = NUE_LOG_WARN;
    nue_app_callbacks cb = nuepp::init<nue_app_callbacks>();
    cb.on_frame = on_frame;
    int rc = nue_app_run(&init, &cb, argc, argv);
    std::printf("consume_entry: %d frames, exit code %d\n", g_frames, rc);
    return g_frames >= 3 ? rc : 1;
}
