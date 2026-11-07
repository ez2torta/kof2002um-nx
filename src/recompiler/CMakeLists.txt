# kof2002um_nx - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(kof2002um_nx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use the SDK sources shipped with this port. Host builds get the code generator;
# the devkitA64 toolchain selects the Horizon runtime and omits host tools.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/rexglue-sdk" CACHE PATH
    "Path to the ReXGlue SDK source tree")
if(REXGLUE_PLATFORM_SWITCH)
    add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
endif()

include(generated/rexglue.cmake)

# Sources
set(KOF2002UMNX_SOURCES
    src/main.cpp
    src/bench_seed.cpp
    src/texture_write_hook.cpp
)

if(WIN32)
    add_executable(kof2002um_nx WIN32 ${KOF2002UMNX_SOURCES})
else()
    add_executable(kof2002um_nx ${KOF2002UMNX_SOURCES})
endif()

if(REXGLUE_PLATFORM_SWITCH)
    set_target_properties(kof2002um_nx PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_include_directories(kof2002um_nx PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${REXGLUE_ENTRYPOINT_INCLUDE_DIR})
    if(REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
        add_library(kof2002um_nx_recomp OBJECT
            ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES})
        target_include_directories(kof2002um_nx_recomp PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${REXGLUE_ENTRYPOINT_INCLUDE_DIR})
        target_link_libraries(kof2002um_nx_recomp PRIVATE rex::runtime)
        rexglue_apply_target_settings(kof2002um_nx_recomp)
        rexglue_apply_recomp_settings(kof2002um_nx_recomp
            "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}/kof2002um_nx_pch.h")
        # This 60,000-entry address registration function is startup-only;
        # optimizing it at -O3 makes GCC spend minutes on one translation unit.
        set_source_files_properties(
            "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}/kof2002um_nx_register.cpp"
            PROPERTIES COMPILE_OPTIONS "-O0")
        target_link_libraries(kof2002um_nx PRIVATE kof2002um_nx_recomp)
    endif()
    rexglue_switch_configure_target(kof2002um_nx GPU_PLUGINS xenos)
    rexglue_switch_add_nro(kof2002um_nx
        NAME "KOF 2002 UM NX"
        AUTHOR "Codex"
        VERSION "0.1.0")
    add_custom_command(TARGET kof2002um_nx POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE_DIR:kof2002um_nx>/kof2002um_nx.nro"
            "$<TARGET_FILE_DIR:kof2002um_nx>/kof2002um-nx.nro"
        COMMENT "Writing the requested kof2002um-nx.nro filename"
        VERBATIM)
else()
    rexglue_setup_target(kof2002um_nx)
endif()
