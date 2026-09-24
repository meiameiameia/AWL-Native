cmake_minimum_required(VERSION 3.20)
project(awl-decomp VERSION 0.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    src/main.cpp
    src/memory.cpp
    src/tpl.cpp
    src/gpl.cpp
    src/gpl_render_mesh.cpp
    src/ground_material.cpp
    src/render.cpp
    src/render_validation.cpp
    src/dvd.cpp
    src/platform.cpp
    src/input.cpp
    src/player_input.cpp
    src/collision_asset.cpp
    src/game.cpp
    src/filesystem.cpp
    src/asset_inventory.cpp
)

# Header-only includes
set(HEADERS
    include/awl/types.h
    include/awl/memory.h
    include/awl/tpl.h
    include/awl/gpl.h
    include/awl/gpl_render_mesh.h
    include/awl/ground_material.h
    include/awl/render.h
    include/awl/render_validation.h
    include/awl/dvd.h
    include/awl/player_input.h
    include/awl/collision_asset.h
)

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Platform defines
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE AWL_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${PROJECT_NAME} PRIVATE d3d11 d3dcompiler xinput)
endif()

# Compiler options
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<CONFIG:Release>:-O2>
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
        $<$<CONFIG:Release>:/O2>
    )
endif()

# Tests
enable_testing()
add_executable(gpl_render_mesh_tests
    tests/gpl_render_mesh_tests.cpp
    src/gpl_render_mesh.cpp
)
target_include_directories(gpl_render_mesh_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(gpl_render_mesh_tests PRIVATE
        -Wall -Wextra
    )
elseif(MSVC)
    target_compile_options(gpl_render_mesh_tests PRIVATE
        /W4
    )
endif()
add_test(NAME gpl_render_mesh_tests COMMAND gpl_render_mesh_tests)

add_executable(asset_parser_tests
    tests/asset_parser_tests.cpp
    src/tpl.cpp
    src/gpl.cpp
    src/filesystem.cpp
    src/memory.cpp
    src/platform.cpp
    src/input.cpp
)
target_include_directories(asset_parser_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
if(WIN32)
    target_compile_definitions(asset_parser_tests PRIVATE AWL_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(asset_parser_tests PRIVATE xinput)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(asset_parser_tests PRIVATE
        -Wall -Wextra
    )
elseif(MSVC)
    target_compile_options(asset_parser_tests PRIVATE
        /W4
    )
endif()
add_test(NAME asset_parser_tests COMMAND asset_parser_tests)

add_executable(render_validation_tests
    tests/render_validation_tests.cpp
    src/render_validation.cpp
)
target_include_directories(render_validation_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(render_validation_tests PRIVATE
        -Wall -Wextra
    )
elseif(MSVC)
    target_compile_options(render_validation_tests PRIVATE
        /W4
    )
endif()
add_test(NAME render_validation_tests COMMAND render_validation_tests)

add_executable(ground_material_tests
    tests/ground_material_tests.cpp
    src/ground_material.cpp
)
target_include_directories(ground_material_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ground_material_tests PRIVATE
        -Wall -Wextra
    )
elseif(MSVC)
    target_compile_options(ground_material_tests PRIVATE
        /W4
    )
endif()
add_test(NAME ground_material_tests COMMAND ground_material_tests)

add_executable(native_input_tests
    tests/native_input_tests.cpp
    src/input.cpp
)
target_include_directories(native_input_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(native_input_tests PRIVATE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(native_input_tests PRIVATE /W4)
endif()
add_test(NAME native_input_tests COMMAND native_input_tests)

add_executable(player_input_tests
    tests/player_input_tests.cpp
    src/player_input.cpp
)
target_include_directories(player_input_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(player_input_tests PRIVATE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(player_input_tests PRIVATE /W4)
endif()
add_test(NAME player_input_tests COMMAND player_input_tests)

add_executable(collision_asset_tests
    tests/collision_asset_tests.cpp
    src/collision_asset.cpp
)
target_include_directories(collision_asset_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(collision_asset_tests PRIVATE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(collision_asset_tests PRIVATE /W4)
endif()
add_test(NAME collision_asset_tests COMMAND collision_asset_tests)
