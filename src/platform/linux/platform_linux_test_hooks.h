// Internal declarations shared by platform_linux.cpp and the platform's own tests (not installed).
#ifndef NUE_PLATFORM_LINUX_TEST_HOOKS_H
#define NUE_PLATFORM_LINUX_TEST_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* "nue.linux.v1" extension table (nue_ext_linux.h). */
const void* nue_linux_get_extension_v1(void);

/* Tests: make the layer read /proc, /sys, /etc through `fs` (nue::lx::IFileSystem*) and the environment through `env`
 * (nue::lx::IEnv*) instead of the real ones. Call before the platform's init; pass NULL to restore. */
void nue_linux_test_set_environment(const void* fs, const void* env);

/* Tests: run one power/thermal polling cycle synchronously (the poller thread normally does it every 2 s). */
void nue_linux_test_poll_now(void);

#ifdef __cplusplus
}
#endif
#endif
