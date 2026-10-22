// ABI-level checks that must hold on 32-bit and 64-bit builds, plus version / result / last-error behaviour.
#include "nue/nue.h"
#include "nue_test.h"

#include <cstddef>

TEST_CASE(abi_layout) {
    CHECK_EQ(sizeof(nue_event), 64u);
    CHECK_EQ(offsetof(nue_event, payload), 16u);
    CHECK(sizeof(nue_event_user) <= 48u);
    CHECK_EQ(sizeof(nue_guid), 16u);
    CHECK_EQ(sizeof(nue_native_window), 32u);   // uint32 + enum + 3 x uint64, same on ILP32 and LP64
    CHECK_EQ(sizeof(nue_bool), 1u);
    CHECK_EQ(sizeof(nue_result), 4u);
    CHECK_EQ(sizeof(nue_file), 4u);             // file ids are 32-bit: a recompiled game can hand them out as HANDLE values
    CHECK_EQ(sizeof(nue_voice_id), 4u);
}

TEST_CASE(struct_init) {
    nue_init_desc d;
    NUE_STRUCT_INIT(&d);
    CHECK_EQ(d.struct_size, sizeof(nue_init_desc));
    CHECK_EQ(d.log_level, 0u);
    CHECK(NUE_STRUCT_HAS(&d, nue_init_desc, log_file));
    d.struct_size = offsetof(nue_init_desc, log_file);
    CHECK(!NUE_STRUCT_HAS(&d, nue_init_desc, log_file));
    CHECK(NUE_STRUCT_HAS(&d, nue_init_desc, config_file));
}

TEST_CASE(version) {
    CHECK_EQ(nue_get_version(), NUE_API_VERSION);
    CHECK_EQ(NUE_VERSION_GET_MAJOR(NUE_MAKE_VERSION(3, 14, 159)), 3u);
    CHECK_EQ(NUE_VERSION_GET_MINOR(NUE_MAKE_VERSION(3, 14, 159)), 14u);
    CHECK_EQ(NUE_VERSION_GET_PATCH(NUE_MAKE_VERSION(3, 14, 159)), 159u);
    CHECK(std::strlen(nue_get_version_string()) > 4);
}

TEST_CASE(result_names) {
    CHECK_STR(nue_result_name(NUE_OK), "NUE_OK");
    CHECK_STR(nue_result_name(NUE_ERR_NOT_FOUND), "NUE_ERR_NOT_FOUND");
    CHECK_STR(nue_result_name(-9999), "NUE_RESULT_UNKNOWN");
    CHECK(NUE_SUCCEEDED(NUE_OK_PARTIAL));
    CHECK(NUE_FAILED(NUE_ERR_IO));
}

TEST_CASE(last_error_is_per_thread_text) {
    nue_clear_last_error();
    CHECK_STR(nue_get_last_error_message(), "");
    nue_set_last_error("device %d failed: %s", 3, "boom");
    CHECK_STR(nue_get_last_error_message(), "device 3 failed: boom");
    nue_clear_last_error();
    CHECK_STR(nue_get_last_error_message(), "");
}

TEST_MAIN_BEGIN
    RUN(abi_layout);
    RUN(struct_init);
    RUN(version);
    RUN(result_names);
    RUN(last_error_is_per_thread_text);
TEST_MAIN_END
