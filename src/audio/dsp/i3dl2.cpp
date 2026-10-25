// I3DL2 (DirectSound DSFXI3DL2Reverb) and EAX property conversions to the EFX style model, and the I3DL2 environment presets.
//
// The preset values are the numbers of the IA-SIG "Interactive 3D Audio Rendering Guidelines Level 2.0" environment table, as published in the
// DirectSound headers (DSFX_I3DL2_ENVIRONMENT_PRESET_*). They are data, typed here by hand and cross-checked against the Wine / mingw-w64 dsound.h
// table (LGPL-2.1, not copied: see docs/THIRD_PARTY_AUDIO.md). Field order: room, room_hf, rolloff, decay_time, decay_hf_ratio, reflections,
// reflections_delay, reverb, reverb_delay, diffusion, density, hf_reference.
#include "nue/audio_dsp.h"
#include "dsp/common.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

struct Preset { const char* name; nue_i3dl2_reverb v; };

const Preset kPresets[NUE_I3DL2_PRESET_COUNT] = {
    {"Default",         {-1000,   -100, 0.0f,  1.49f, 0.83f,  -2602, 0.007f,   200, 0.011f, 100.0f, 100.0f, 5000.0f}},
    {"Generic",         {-1000,   -100, 0.0f,  1.49f, 0.83f,  -2602, 0.007f,   200, 0.011f, 100.0f, 100.0f, 5000.0f}},
    {"PaddedCell",      {-1000,  -6000, 0.0f,  0.17f, 0.10f,  -1204, 0.001f,   207, 0.002f, 100.0f, 100.0f, 5000.0f}},
    {"Room",            {-1000,   -454, 0.0f,  0.40f, 0.83f,  -1646, 0.002f,    53, 0.003f, 100.0f, 100.0f, 5000.0f}},
    {"Bathroom",        {-1000,  -1200, 0.0f,  1.49f, 0.54f,   -370, 0.007f,  1030, 0.011f, 100.0f,  60.0f, 5000.0f}},
    {"LivingRoom",      {-1000,  -6000, 0.0f,  0.50f, 0.10f,  -1376, 0.003f, -1104, 0.004f, 100.0f, 100.0f, 5000.0f}},
    {"StoneRoom",       {-1000,   -300, 0.0f,  2.31f, 0.64f,   -711, 0.012f,    83, 0.017f, 100.0f, 100.0f, 5000.0f}},
    {"Auditorium",      {-1000,   -476, 0.0f,  4.32f, 0.59f,   -789, 0.020f,  -289, 0.030f, 100.0f, 100.0f, 5000.0f}},
    {"ConcertHall",     {-1000,   -500, 0.0f,  3.92f, 0.70f,  -1230, 0.020f,    -2, 0.029f, 100.0f, 100.0f, 5000.0f}},
    {"Cave",            {-1000,      0, 0.0f,  2.91f, 1.30f,   -602, 0.015f,  -302, 0.022f, 100.0f, 100.0f, 5000.0f}},
    {"Arena",           {-1000,   -698, 0.0f,  7.24f, 0.33f,  -1166, 0.020f,    16, 0.030f, 100.0f, 100.0f, 5000.0f}},
    {"Hangar",          {-1000,  -1000, 0.0f, 10.05f, 0.23f,   -602, 0.020f,   198, 0.030f, 100.0f, 100.0f, 5000.0f}},
    {"CarpetedHallway", {-1000,  -4000, 0.0f,  0.30f, 0.10f,  -1831, 0.002f, -1630, 0.030f, 100.0f, 100.0f, 5000.0f}},
    {"Hallway",         {-1000,   -300, 0.0f,  1.49f, 0.59f,  -1219, 0.007f,   441, 0.011f, 100.0f, 100.0f, 5000.0f}},
    {"StoneCorridor",   {-1000,   -237, 0.0f,  2.70f, 0.79f,  -1214, 0.013f,   395, 0.020f, 100.0f, 100.0f, 5000.0f}},
    {"Alley",           {-1000,   -270, 0.0f,  1.49f, 0.86f,  -1204, 0.007f,    -4, 0.011f, 100.0f, 100.0f, 5000.0f}},
    {"Forest",          {-1000,  -3300, 0.0f,  1.49f, 0.54f,  -2560, 0.162f,  -613, 0.088f,  79.0f, 100.0f, 5000.0f}},
    {"City",            {-1000,   -800, 0.0f,  1.49f, 0.67f,  -2273, 0.007f,  2217, 0.011f,  50.0f, 100.0f, 5000.0f}},
    {"Mountains",       {-1000,  -2500, 0.0f,  1.49f, 0.21f,  -2780, 0.300f, -2014, 0.100f,  27.0f, 100.0f, 5000.0f}},
    {"Quarry",          {-1000,  -1000, 0.0f,  1.49f, 0.83f, -10000, 0.061f,   500, 0.025f, 100.0f, 100.0f, 5000.0f}},
    {"Plain",           {-1000,  -2000, 0.0f,  1.49f, 0.50f,  -2466, 0.179f, -2514, 0.100f,  21.0f, 100.0f, 5000.0f}},
    {"ParkingLot",      {-1000,      0, 0.0f,  1.65f, 1.50f,  -1363, 0.008f, -1153, 0.012f, 100.0f, 100.0f, 5000.0f}},
    {"SewerPipe",       {-1000,  -1000, 0.0f,  2.81f, 0.14f,    429, 0.014f,   648, 0.011f,  80.0f,  60.0f, 5000.0f}},
    {"Underwater",      {-1000,  -4000, 0.0f,  1.49f, 0.10f,   -449, 0.007f,  1700, 0.011f, 100.0f, 100.0f, 5000.0f}},
    {"SmallRoom",       {-1000,   -600, 0.0f,  1.10f, 0.83f,   -400, 0.005f,   500, 0.010f, 100.0f, 100.0f, 5000.0f}},
    {"MediumRoom",      {-1000,   -600, 0.0f,  1.30f, 0.83f,  -1000, 0.010f,  -200, 0.020f, 100.0f, 100.0f, 5000.0f}},
    {"LargeRoom",       {-1000,   -600, 0.0f,  1.50f, 0.83f,  -1600, 0.020f, -1000, 0.040f, 100.0f, 100.0f, 5000.0f}},
    {"MediumHall",      {-1000,   -600, 0.0f,  1.80f, 0.70f,  -1300, 0.015f,  -800, 0.030f, 100.0f, 100.0f, 5000.0f}},
    {"LargeHall",       {-1000,   -600, 0.0f,  1.80f, 0.70f,  -2000, 0.030f, -1400, 0.060f, 100.0f, 100.0f, 5000.0f}},
    {"Plate",           {-1000,   -200, 0.0f,  1.30f, 0.90f,      0, 0.002f,     0, 0.010f, 100.0f,  75.0f, 5000.0f}},
};

std::string key(const char* s) {
    std::string k;
    for (; *s; ++s) if (*s != ' ' && *s != '_' && *s != '-') k.push_back(char(std::tolower(static_cast<unsigned char>(*s))));
    return k;
}

}  // namespace

extern "C" {

NUE_API float NUE_CALL nue_millibel_to_gain(int32_t mb) { return std::pow(10.0f, float(mb) / 2000.0f); }

NUE_API const char* NUE_CALL nue_i3dl2_preset_name(uint32_t index) { return index < NUE_I3DL2_PRESET_COUNT ? kPresets[index].name : nullptr; }

NUE_API nue_result NUE_CALL nue_i3dl2_preset(uint32_t index, nue_i3dl2_reverb* out) {
    if (!out || index >= NUE_I3DL2_PRESET_COUNT) return NUE_ERR_INVALID_ARGUMENT;
    *out = kPresets[index].v;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_i3dl2_preset_by_name(const char* name, nue_i3dl2_reverb* out) {
    if (!name || !out) return NUE_ERR_INVALID_ARGUMENT;
    const std::string k = key(name);
    for (const Preset& p : kPresets) if (key(p.name) == k) { *out = p.v; return NUE_OK; }
    return NUE_ERR_NOT_FOUND;
}

NUE_API void NUE_CALL nue_reverb_params_from_i3dl2(nue_reverb_params* o, const nue_i3dl2_reverb* in) {
    nue_reverb_params_default(o);
    o->gain = nuea::clampf(nue_millibel_to_gain(in->room), 0.0f, 1.0f);
    o->gain_hf = nuea::clampf(nue_millibel_to_gain(in->room_hf), 0.0f, 1.0f);
    o->room_rolloff_factor = in->room_rolloff_factor;
    o->decay_time = nuea::clampf(in->decay_time, 0.1f, 20.0f);
    o->decay_hf_ratio = nuea::clampf(in->decay_hf_ratio, 0.1f, 2.0f);
    o->reflections_gain = nuea::clampf(nue_millibel_to_gain(in->reflections), 0.0f, 3.16f);
    o->reflections_delay = nuea::clampf(in->reflections_delay, 0.0f, 0.3f);
    o->late_gain = nuea::clampf(nue_millibel_to_gain(in->reverb), 0.0f, 10.0f);
    o->late_delay = nuea::clampf(in->reverb_delay, 0.0f, 0.1f);
    o->diffusion = nuea::clampf(in->diffusion / 100.0f, 0.0f, 1.0f);
    o->density = nuea::clampf(in->density / 100.0f, 0.0f, 1.0f);
    o->hf_reference = nuea::clampf(in->hf_reference, 1000.0f, 20000.0f);
}

NUE_API void NUE_CALL nue_reverb_params_from_eax(nue_reverb_params* o, const nue_eax_reverb* in) {
    nue_reverb_params_default(o);
    o->gain = nuea::clampf(nue_millibel_to_gain(in->room), 0.0f, 1.0f);
    o->gain_hf = nuea::clampf(nue_millibel_to_gain(in->room_hf), 0.0f, 1.0f);
    o->gain_lf = nuea::clampf(nue_millibel_to_gain(in->room_lf), 0.0f, 1.0f);
    o->decay_time = nuea::clampf(in->decay_time, 0.1f, 20.0f);
    o->decay_hf_ratio = nuea::clampf(in->decay_hf_ratio, 0.1f, 2.0f);
    o->decay_lf_ratio = nuea::clampf(in->decay_lf_ratio, 0.1f, 2.0f);
    o->reflections_gain = nuea::clampf(nue_millibel_to_gain(in->reflections), 0.0f, 3.16f);
    o->reflections_delay = nuea::clampf(in->reflections_delay, 0.0f, 0.3f);
    for (int i = 0; i < 3; ++i) { o->reflections_pan[i] = in->reflections_pan[i]; o->late_pan[i] = in->reverb_pan[i]; }
    o->late_gain = nuea::clampf(nue_millibel_to_gain(in->reverb), 0.0f, 10.0f);
    o->late_delay = nuea::clampf(in->reverb_delay, 0.0f, 0.1f);
    o->echo_time = nuea::clampf(in->echo_time, 0.075f, 0.25f);
    o->echo_depth = nuea::clampf(in->echo_depth, 0.0f, 1.0f);
    o->modulation_time = nuea::clampf(in->modulation_time, 0.04f, 4.0f);
    o->modulation_depth = nuea::clampf(in->modulation_depth, 0.0f, 1.0f);
    o->air_absorption_gain_hf = nuea::clampf(nue_millibel_to_gain(int32_t(std::lround(in->air_absorption_hf))), 0.892f, 1.0f);
    o->hf_reference = nuea::clampf(in->hf_reference, 1000.0f, 20000.0f);
    o->lf_reference = nuea::clampf(in->lf_reference, 20.0f, 1000.0f);
    o->room_rolloff_factor = nuea::clampf(in->room_rolloff_factor, 0.0f, 10.0f);
    o->diffusion = nuea::clampf(in->diffusion / 100.0f, 0.0f, 1.0f);
    o->density = nuea::clampf(in->density / 100.0f, 0.0f, 1.0f);
    o->decay_hf_limit = (in->flags & 0x20u) ? NUE_TRUE : NUE_FALSE;
}

}  // extern "C"
