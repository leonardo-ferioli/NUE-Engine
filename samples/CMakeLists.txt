# Samples open real windows/audio devices when run. Build them freely, run them by hand only.
# Add a sample: samples/<name>/main.cpp  ->  nue_sample_<name>
file(GLOB NUE_SAMPLE_DIRS CONFIGURE_DEPENDS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(dir IN LISTS NUE_SAMPLE_DIRS)
    if(IS_DIRECTORY "${dir}" AND EXISTS "${dir}/main.cpp")
        get_filename_component(name "${dir}" NAME)
        add_executable(nue_sample_${name} "${dir}/main.cpp")
        target_link_libraries(nue_sample_${name} PRIVATE nue::nue nue_internal)
    endif()
endforeach()
