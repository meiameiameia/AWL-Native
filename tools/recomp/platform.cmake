# Static-recompilation platform-runtime track: high-level replacements (HLE) for the SDK's OS, DVD,
# PAD/SI, VI, EXI, AI/DSP and GX services, the hardware-window models, and the game host executable.
# Owned by the P1 platform track (see docs/roadmap.md). Included from the root CMakeLists.txt after
# generator.cmake, so it may link awl_recomp_core and awl_recomp_runtime. Sources live in src/hle/.

# Evidence tool for docs/research/hle-platform.md: per-function hardware, low-memory and call facts.
add_executable(awl-hle-scan src/hle/hle_scan.cpp)
target_compile_definitions(awl-hle-scan PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(awl-hle-scan PRIVATE awl_recomp_core)
if(MSVC)
    target_compile_options(awl-hle-scan PRIVATE /W4)
else()
    target_compile_options(awl-hle-scan PRIVATE -Wall -Wextra)
endif()

# Platform runtime library: clock, events, interrupts, fibers, hardware models, native SDK replacements.
file(GLOB AWL_HLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/hle/*.cpp)
list(FILTER AWL_HLE_SOURCES EXCLUDE REGEX "(hle_scan|host_main)\\.cpp$")
add_library(awl_hle STATIC ${AWL_HLE_SOURCES})
target_include_directories(awl_hle PUBLIC ${CMAKE_SOURCE_DIR}/src/hle)
target_compile_definitions(awl_hle PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(awl_hle PUBLIC awl_recomp_core awl_recomp_runtime)
if(MSVC)
    target_compile_options(awl_hle PRIVATE /W4 /fp:strict)
else()
    target_compile_options(awl_hle PRIVATE -Wall -Wextra)
endif()

# Tests of the platform runtime (no game files needed; they supply their own function table).
add_executable(hle_core_tests tests/hle_core_tests.cpp)
target_link_libraries(hle_core_tests PRIVATE awl_hle)
add_test(NAME hle_core_tests COMMAND hle_core_tests)
if(MSVC)
    target_compile_options(hle_core_tests PRIVATE /W4 /fp:strict)
else()
    target_compile_options(hle_core_tests PRIVATE -Wall -Wextra)
endif()

add_executable(hle_sched_tests tests/hle_sched_tests.cpp)
target_link_libraries(hle_sched_tests PRIVATE awl_hle)
add_test(NAME hle_sched_tests COMMAND hle_sched_tests)
if(MSVC)
    target_compile_options(hle_sched_tests PRIVATE /W4 /fp:strict)
else()
    target_compile_options(hle_sched_tests PRIVATE -Wall -Wextra)
endif()

# Game host: links the generated whole-DOL output (build/recomp-gen, written by `awl-recomp gen --full`).
# Only configured when that output exists, like the P0 runner.
if(EXISTS "${AWL_RECOMP_GEN_DIR}/sources.cmake")
    include("${AWL_RECOMP_GEN_DIR}/sources.cmake")
    add_executable(awl-host src/hle/host_main.cpp ${AWL_RECOMP_GEN_SOURCES})
    target_include_directories(awl-host PRIVATE ${AWL_RECOMP_GEN_DIR})
    target_compile_definitions(awl-host PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(awl-host PRIVATE awl_hle)
    if(MSVC)
        target_compile_options(awl-host PRIVATE /W4 /fp:strict)
        set_source_files_properties(${AWL_RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/wd4102;/wd4702;/wd4100;/wd4189")
    endif()
endif()
