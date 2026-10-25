# Samples open real windows/audio devices when run. Build them freely, run them by hand only.
# Add a sample: samples/<name>/main.cpp  ->  nue_sample_<name>
file(GLOB NUE_SAMPLE_DIRS CONFIGURE_DEPENDS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(dir IN LISTS NUE_SAMPLE_DIRS)
    if(IS_DIRECTORY "${dir}" AND EXISTS "${dir}/main.cpp")
        get_filename_component(name "${dir}" NAME)
        if(name STREQUAL "hello_window" AND NOT NUE_HAVE_SDL3)
            continue()   # needs nue::entry, which comes with the SDL3 backend
        endif()
        if(name STREQUAL "hello_gfx")
            continue()   # has its own CMakeLists.txt (gfx session), added below when the graphics module is built
        endif()
        add_executable(nue_sample_${name} "${dir}/main.cpp")
        target_link_libraries(nue_sample_${name} PRIVATE nue::nue nue_internal)
        if(name STREQUAL "hello_window")
            # this one defines nue_main and takes the program entry point from the engine (the others have their own main)
            target_link_libraries(nue_sample_${name} PRIVATE nue::entry)
        endif()
    endif()
endforeach()

if(NUE_WITH_GFX AND NUE_HAVE_SDL3 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hello_gfx/CMakeLists.txt")
    add_subdirectory(hello_gfx)
endif()
