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
