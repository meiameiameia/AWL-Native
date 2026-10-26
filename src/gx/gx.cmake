# P2 GX core track (docs/roadmap.md): command-stream capture format, survey tool, and (milestone 2) the command processor,
# XF/TEV decode, texture decoder and IR backend. Included from the root CMakeLists.txt before platform.cmake, which links
# awl_gx into awl_hle when this target exists. Sources live in src/gx/.

add_library(awl_gx STATIC
    src/gx/gx_dump.cpp
    src/gx/gx_core.cpp
    src/gx/gx_draw.cpp
    src/gx/gx_vertex.cpp
    src/gx/gx_pipeline.cpp
    src/gx/gx_texture.cpp
    src/gx/gx_record.cpp
)
target_include_directories(awl_gx PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(awl_gx PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(awl_gx PRIVATE /W4 /fp:strict)
else()
    target_compile_options(awl_gx PRIVATE -Wall -Wextra)
endif()

# Survey of the game's GX usage over a captured command stream (docs/research/gx-core.md).
add_executable(awl-gx-survey src/gx/gx_survey.cpp)
target_compile_definitions(awl-gx-survey PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(awl-gx-survey PRIVATE awl_gx)
if(MSVC)
    target_compile_options(awl-gx-survey PRIVATE /W4 /fp:strict)
else()
    target_compile_options(awl-gx-survey PRIVATE -Wall -Wextra)
endif()

# Compile and layout checks of the IR header.
add_executable(gx_ir_tests tests/gx_ir_tests.cpp)
target_link_libraries(gx_ir_tests PRIVATE awl_gx)
add_test(NAME gx_ir_tests COMMAND gx_ir_tests)
if(MSVC)
    target_compile_options(gx_ir_tests PRIVATE /W4 /fp:strict)
else()
    target_compile_options(gx_ir_tests PRIVATE -Wall -Wextra)
endif()

# Command processor, XF, TEV decode, texture decoding and EFB copies on hand-assembled streams.
add_executable(gx_core_tests tests/gx_core_tests.cpp)
target_link_libraries(gx_core_tests PRIVATE awl_gx)
add_test(NAME gx_core_tests COMMAND gx_core_tests)
if(MSVC)
    target_compile_options(gx_core_tests PRIVATE /W4 /fp:strict)
else()
    target_compile_options(gx_core_tests PRIVATE -Wall -Wextra)
endif()

# Replay benchmark of the vertex path with a checksumming backend (bit-identity check for optimisations).
add_executable(awl-gx-bench src/gx/gx_bench.cpp)
target_compile_definitions(awl-gx-bench PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(awl-gx-bench PRIVATE awl_gx)
if(MSVC)
    target_compile_options(awl-gx-bench PRIVATE /W4 /fp:strict)
else()
    target_compile_options(awl-gx-bench PRIVATE -Wall -Wextra)
endif()
