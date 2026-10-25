// Shared between the Objective-C++ translation units of the iOS backend. Never compiled yet (no macOS here): see PLATFORM_IOS.md.
#pragma once
#include <string>

#include "nue_ios.h"

namespace nue::ios::detail {

// Delivers an event to the host callback. Main thread only (callers on other threads must hop with dispatch_async(dispatch_get_main_queue())).
void emit(const NueIosEvent& e);
void emit(NueIosEventKind kind, int64_t a = 0, int64_t b = 0, float f0 = 0, float f1 = 0, float f2 = 0, float f3 = 0);

const NueIosConfig& config();
std::string game_dir_name();
bool app_is_active();   // last state from the lifecycle machine

// Per-module start/stop, called from nue_ios_init / nue_ios_shutdown in this order.
void lifecycle_start();  void lifecycle_stop();
void power_start();      void power_stop();
void audio_start();      void audio_stop();
void input_start();      void input_stop();
void display_start();    void display_stop();

// Lifecycle consumers that need to react to actions (audio pause/resume etc.).
void audio_on_lifecycle_actions(unsigned actions);
void input_on_lifecycle_actions(unsigned actions);
void display_on_lifecycle_actions(unsigned actions);

}  // namespace nue::ios::detail
