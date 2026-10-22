// Compile this file into the MAIN EXECUTABLE (not into a static library, whose exports the linker drops):
// nue_win_request_discrete_gpu(<target>) in CMake does it. The NVIDIA Optimus and AMD PowerXpress / Switchable Graphics drivers look for
// these two exported variables in the executable's export table and then run the process on the discrete GPU of a hybrid laptop.
// They are a request, not a guarantee: the Windows "Graphics settings" per-app preference (see gpu.hpp) takes precedence.
#include <windows.h>

extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
