# Console sample: prints what the machine supports. Included from src/platform/windows/CMakeLists.txt (not from the generic samples loop, which
# looks for main.cpp). Run: <build>/samples_windows_probe/nue_win_probe.exe [--probe-gpu]
add_executable(nue_win_probe probe.cpp)
target_link_libraries(nue_win_probe PRIVATE nue_win_modules)
set_target_properties(nue_win_probe PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
nue_win_request_discrete_gpu(nue_win_probe)
