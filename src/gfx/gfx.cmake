# DirectX 11 renderer track (docs/roadmap.md, P2): device layer, game window, presentation, pacing, input, TEV shader
# generation. Owned by the DX11 renderer track; design and status in docs/research/gfx-dx11.md.
# Included optionally from the root CMakeLists.txt; tools/recomp/platform.cmake links awl_gfx into awl-host when it exists.

# The PadAdapter / NativeInputAccumulator implementation is shared by the PAD replacement (awl_hle) and the window input.
add_library(awl_pad_input STATIC ${CMAKE_SOURCE_DIR}/src/input.cpp)
target_include_directories(awl_pad_input PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(awl_pad_input PUBLIC xinput)
if(MSVC)
    target_compile_options(awl_pad_input PRIVATE /W4 /fp:strict)
else()
    target_compile_options(awl_pad_input PRIVATE -Wall -Wextra)
endif()

set(AWL_GFX_SOURCES
    src/gfx/gfx_image.cpp
    src/gfx/gfx_device.cpp
    src/gfx/gfx_blit.cpp
    src/gfx/gfx_window.cpp
    src/gfx/gfx_pacing.cpp
    src/gfx/gfx_input.cpp
    src/gfx/gfx_output.cpp
    src/gfx/gfx_tev.cpp
    src/gfx/gfx_shader_cache.cpp
    src/gfx/gfx_renderer.cpp)
list(TRANSFORM AWL_GFX_SOURCES PREPEND ${CMAKE_SOURCE_DIR}/)
add_library(awl_gfx STATIC ${AWL_GFX_SOURCES})
target_include_directories(awl_gfx PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(awl_gfx PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(awl_gfx PUBLIC awl_pad_input d3d11 dxgi d3dcompiler winmm)
if(TARGET awl_gx)   # the renderer is a gx::Backend and reads the IR (src/gx/gx_ir.h)
    target_link_libraries(awl_gfx PUBLIC awl_gx)
endif()
if(MSVC)
    target_compile_options(awl_gfx PRIVATE /W4)
else()
    target_compile_options(awl_gfx PRIVATE -Wall -Wextra)
endif()

# Tests: all headless (offscreen device, no window). tests/gfx_*.cpp, one executable each.
foreach(t gfx_device_tests gfx_pacing_tests gfx_input_tests gfx_tev_tests gfx_renderer_tests)
    add_executable(${t} ${CMAKE_SOURCE_DIR}/tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE awl_gfx)
    target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME ${t} COMMAND ${t})
endforeach()
