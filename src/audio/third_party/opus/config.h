/* NUE: build configuration for the vendored Opus (floating point, plain C, no SIMD run-time detection). */
#ifndef NUE_OPUS_CONFIG_H
#define NUE_OPUS_CONFIG_H
#define PACKAGE_VERSION "1.6.1"
#define OPUS_BUILD 1
#define USE_ALLOCA 1
#define CUSTOM_MODES 1
#define HAVE_LRINTF 1
#define HAVE_LRINT 1
#ifdef _MSC_VER
#  undef HAVE_LRINTF
#  undef HAVE_LRINT
#endif
#endif
