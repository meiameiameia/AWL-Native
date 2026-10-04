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
    src/world_map_contact.cpp
    src/world_map_movement.cpp
    src/world_map_trigger_asset.cpp
    src/world_map_event_conditions.cpp
    src/world_map_action_asset.cpp
    src/world_map_request_transition.cpp
    src/world_map_selection.cpp
    src/world_map_message_asset.cpp
    src/world_map_message_stream.cpp
    src/world_map_message_staging.cpp
    src/world_map_selection_rows.cpp
    src/world_map_presentation_data.cpp
    src/world_map_presentation.cpp
    src/world_map_feedback.cpp
    src/world_map_presentation_actor.cpp
    src/world_map_actor_animation.cpp
    src/world_map_animation_channel.cpp
    src/world_map_animation_channel_owner.cpp
    src/world_map_animation_initializer.cpp
    src/world_map_model_links.cpp
    src/world_map_secondary_model.cpp
    src/world_map_model_initialization.cpp
    src/world_map_model_feature.cpp
    src/world_map_held_item_assets.cpp
    src/world_map_gpl_metadata.cpp
    src/world_map_skin_metadata.cpp
    src/world_map_player_model_auxiliary.cpp
    src/world_map_player_model_setup.cpp
    src/world_map_player_animation_assets.cpp
    src/world_map_player_model_assets.cpp
    src/world_map_camera.cpp
    src/world_map_scene_index.cpp
    src/world_map_collision_registry.cpp
    src/world_map_actor_step.cpp
    src/world_map_actor_target.cpp
    src/world_map_actor_action.cpp
    src/world_map_actor_heading.cpp
    src/world_map_collision_assets.cpp
    src/development_wall_route.cpp
    src/world_map_collision_records.cpp
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
    include/awl/world_map_contact.h
    include/awl/world_map_movement.h
    include/awl/world_map_trigger_asset.h
    include/awl/world_map_event_conditions.h
    include/awl/world_map_action_asset.h
    include/awl/world_map_request_transition.h
    include/awl/world_map_selection.h
    include/awl/world_map_message_asset.h
    include/awl/world_map_message_stream.h
    include/awl/world_map_message_staging.h
    include/awl/world_map_selection_rows.h
    include/awl/world_map_presentation_data.h
    include/awl/world_map_presentation.h
    include/awl/world_map_feedback.h
    include/awl/world_map_presentation_actor.h
    include/awl/world_map_actor_animation.h
    include/awl/world_map_animation_channel.h
    include/awl/world_map_animation_channel_owner.h
    include/awl/world_map_animation_initializer.h
    include/awl/world_map_model_links.h
    include/awl/world_map_secondary_model.h
    include/awl/world_map_model_initialization.h
    include/awl/world_map_model_feature.h
    include/awl/world_map_held_item_assets.h
    include/awl/world_map_gpl_metadata.h
    include/awl/world_map_skin_metadata.h
    include/awl/world_map_player_model_auxiliary.h
    include/awl/world_map_player_model_setup.h
    include/awl/world_map_player_animation_assets.h
    include/awl/world_map_player_model_assets.h
    src/world_map_flat_archive.h
    src/world_map_transition_clock.h
    include/awl/world_map_camera.h
    include/awl/world_map_scene_index.h
    include/awl/world_map_collision_registry.h
    include/awl/world_map_actor_step.h
    include/awl/world_map_actor_target.h
    include/awl/world_map_actor_action.h
    include/awl/world_map_actor_heading.h
    include/awl/world_map_collision_assets.h
    include/awl/development_wall_route.h
    include/awl/world_map_collision_records.h
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
    src/world_map_contact.cpp
    src/world_map_movement.cpp
    src/world_map_trigger_asset.cpp
    src/world_map_event_conditions.cpp
    src/world_map_action_asset.cpp
    src/world_map_request_transition.cpp
    src/world_map_selection.cpp
    src/world_map_message_asset.cpp
    src/world_map_message_stream.cpp
    src/world_map_message_staging.cpp
    src/world_map_selection_rows.cpp
    src/world_map_presentation_data.cpp
    src/world_map_presentation.cpp
    src/world_map_feedback.cpp
    src/world_map_presentation_actor.cpp
    src/world_map_actor_animation.cpp
    src/world_map_animation_channel.cpp
    src/world_map_animation_initializer.cpp
    src/world_map_model_links.cpp
    src/world_map_secondary_model.cpp
    src/world_map_model_initialization.cpp
    src/world_map_camera.cpp
    src/world_map_scene_index.cpp
    src/world_map_collision_registry.cpp
    src/world_map_actor_step.cpp
    src/world_map_actor_target.cpp
    src/world_map_actor_action.cpp
    src/world_map_actor_heading.cpp
    src/world_map_collision_assets.cpp
    src/development_wall_route.cpp
    src/world_map_collision_records.cpp
    src/filesystem.cpp
    src/memory.cpp
    src/platform.cpp
    src/input.cpp
    src/player_input.cpp
)
target_include_directories(collision_asset_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
if(WIN32)
    target_compile_definitions(collision_asset_tests PRIVATE AWL_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(collision_asset_tests PRIVATE xinput)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(collision_asset_tests PRIVATE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(collision_asset_tests PRIVATE /W4)
endif()
add_test(NAME collision_asset_tests COMMAND collision_asset_tests)

add_executable(world_map_selection_tests
    tests/world_map_selection_tests.cpp
    src/world_map_selection.cpp
    src/world_map_request_transition.cpp
)
target_include_directories(world_map_selection_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(world_map_selection_tests PRIVATE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(world_map_selection_tests PRIVATE /W4)
endif()
add_test(NAME world_map_selection_tests COMMAND world_map_selection_tests)

add_executable(world_map_message_asset_tests
    tests/world_map_message_asset_tests.cpp
    src/world_map_message_asset.cpp
    src/world_map_message_stream.cpp
    src/world_map_message_staging.cpp
    src/world_map_selection_rows.cpp
    src/world_map_presentation_data.cpp
    src/world_map_presentation.cpp
    src/filesystem.cpp
    src/memory.cpp
    src/platform.cpp
    src/input.cpp
    src/player_input.cpp
)
target_include_directories(world_map_message_asset_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(WIN32)
    target_compile_definitions(world_map_message_asset_tests PRIVATE AWL_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(world_map_message_asset_tests PRIVATE xinput)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(world_map_message_asset_tests PRIVATE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(world_map_message_asset_tests PRIVATE /W4)
endif()
add_test(NAME world_map_message_asset_tests COMMAND world_map_message_asset_tests)

add_executable(world_map_feedback_tests
    tests/world_map_feedback_tests.cpp
    src/world_map_feedback.cpp
    src/world_map_presentation.cpp
    src/world_map_presentation_data.cpp
    src/world_map_message_stream.cpp
)
target_include_directories(world_map_feedback_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(world_map_feedback_tests PRIVATE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(world_map_feedback_tests PRIVATE /W4)
endif()
add_test(NAME world_map_feedback_tests COMMAND world_map_feedback_tests)

add_executable(world_map_presentation_actor_tests
    tests/world_map_presentation_actor_tests.cpp
    src/world_map_presentation_actor.cpp
    src/world_map_presentation.cpp
    src/world_map_presentation_data.cpp
    src/world_map_message_stream.cpp
)
target_include_directories(world_map_presentation_actor_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(world_map_presentation_actor_tests PRIVATE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(world_map_presentation_actor_tests PRIVATE /W4)
endif()
add_test(NAME world_map_presentation_actor_tests COMMAND world_map_presentation_actor_tests)

add_executable(world_map_actor_animation_tests
    tests/world_map_actor_animation_tests.cpp
    src/world_map_actor_animation.cpp
    src/world_map_presentation_actor.cpp
    src/world_map_presentation.cpp
    src/world_map_presentation_data.cpp
    src/world_map_message_stream.cpp
)
target_include_directories(world_map_actor_animation_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(world_map_actor_animation_tests PRIVATE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(world_map_actor_animation_tests PRIVATE /W4)
endif()
add_test(NAME world_map_actor_animation_tests COMMAND world_map_actor_animation_tests)

add_executable(world_map_animation_channel_tests
    tests/world_map_animation_channel_tests.cpp
    src/world_map_animation_channel.cpp
    src/world_map_actor_animation.cpp
)
target_include_directories(world_map_animation_channel_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(world_map_animation_channel_tests PRIVATE /W4)
else()
    target_compile_options(world_map_animation_channel_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME world_map_animation_channel_tests COMMAND world_map_animation_channel_tests)

add_executable(world_map_animation_initializer_tests
    tests/world_map_animation_initializer_tests.cpp
    src/world_map_animation_initializer.cpp
    src/world_map_model_links.cpp
    src/world_map_secondary_model.cpp
    src/world_map_animation_channel.cpp
    src/world_map_actor_animation.cpp
)
target_include_directories(world_map_animation_initializer_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(world_map_animation_initializer_tests PRIVATE /W4)
else()
    target_compile_options(world_map_animation_initializer_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME world_map_animation_initializer_tests COMMAND world_map_animation_initializer_tests)

add_executable(world_map_model_links_tests
    tests/world_map_model_links_tests.cpp
    src/world_map_model_links.cpp
)
target_include_directories(world_map_model_links_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(world_map_model_links_tests PRIVATE /W4)
else()
    target_compile_options(world_map_model_links_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME world_map_model_links_tests COMMAND world_map_model_links_tests)

add_executable(world_map_secondary_model_tests
    tests/world_map_secondary_model_tests.cpp
    src/world_map_secondary_model.cpp
)
target_include_directories(world_map_secondary_model_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(world_map_secondary_model_tests PRIVATE /W4)
else()
    target_compile_options(world_map_secondary_model_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME world_map_secondary_model_tests COMMAND world_map_secondary_model_tests)

add_executable(world_map_model_resource_tests
    tests/world_map_model_resource_tests.cpp
    src/world_map_secondary_model.cpp
)
target_include_directories(world_map_model_resource_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(world_map_model_resource_tests PRIVATE /W4)
else()
    target_compile_options(world_map_model_resource_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME world_map_model_resource_tests COMMAND world_map_model_resource_tests)

add_executable(world_map_model_initialization_tests
    tests/world_map_model_initialization_tests.cpp
    src/world_map_model_initialization.cpp
    src/world_map_model_feature.cpp
    src/world_map_secondary_model.cpp
    src/world_map_animation_channel.cpp
    src/world_map_animation_initializer.cpp
    src/world_map_actor_animation.cpp
    src/world_map_model_links.cpp
)
target_include_directories(world_map_model_initialization_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(world_map_model_initialization_tests PRIVATE /W4)
else()
    target_compile_options(world_map_model_initialization_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME world_map_model_initialization_tests COMMAND world_map_model_initialization_tests)

add_executable(world_map_model_feature_tests
    tests/world_map_model_feature_tests.cpp
    src/world_map_model_feature.cpp
)
target_include_directories(world_map_model_feature_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(world_map_model_feature_tests PRIVATE /W4)
else()
    target_compile_options(world_map_model_feature_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME world_map_model_feature_tests COMMAND world_map_model_feature_tests)

add_executable(world_map_held_item_assets_tests
    tests/world_map_held_item_assets_tests.cpp
    src/world_map_held_item_assets.cpp
    src/world_map_gpl_metadata.cpp
    src/world_map_model_feature.cpp
    src/tpl.cpp
    src/filesystem.cpp
    src/memory.cpp
    src/platform.cpp
    src/input.cpp
)
target_include_directories(world_map_held_item_assets_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(WIN32)
    target_compile_definitions(world_map_held_item_assets_tests PRIVATE AWL_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(world_map_held_item_assets_tests PRIVATE xinput)
endif()
if(MSVC)
    target_compile_options(world_map_held_item_assets_tests PRIVATE /W4)
else()
    target_compile_options(world_map_held_item_assets_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME world_map_held_item_assets_tests COMMAND world_map_held_item_assets_tests)

add_executable(world_map_player_animation_assets_tests
    tests/world_map_player_animation_assets_tests.cpp
    src/world_map_player_animation_assets.cpp
    src/world_map_model_initialization.cpp
    src/world_map_model_feature.cpp
    src/world_map_secondary_model.cpp
    src/world_map_animation_channel.cpp
    src/world_map_animation_channel_owner.cpp
    src/world_map_animation_initializer.cpp
    src/world_map_actor_animation.cpp
    src/world_map_model_links.cpp
    src/filesystem.cpp
    src/memory.cpp
    src/platform.cpp
    src/input.cpp
)
target_include_directories(world_map_player_animation_assets_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(WIN32)
    target_compile_definitions(world_map_player_animation_assets_tests PRIVATE AWL_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(world_map_player_animation_assets_tests PRIVATE xinput)
endif()
if(MSVC)
    target_compile_options(world_map_player_animation_assets_tests PRIVATE /W4)
else()
    target_compile_options(world_map_player_animation_assets_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME world_map_player_animation_assets_tests COMMAND world_map_player_animation_assets_tests)

add_executable(world_map_player_model_assets_tests
    tests/world_map_player_model_assets_tests.cpp
    src/world_map_player_model_assets.cpp
    src/world_map_player_model_auxiliary.cpp
    src/world_map_player_model_setup.cpp
    src/world_map_model_initialization.cpp
    src/world_map_animation_initializer.cpp
    src/world_map_animation_channel.cpp
    src/world_map_actor_animation.cpp
    src/world_map_model_links.cpp
    src/world_map_gpl_metadata.cpp
    src/world_map_skin_metadata.cpp
    src/world_map_secondary_model.cpp
    src/tpl.cpp
    src/filesystem.cpp
    src/memory.cpp
    src/platform.cpp
    src/input.cpp
)
target_include_directories(world_map_player_model_assets_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(WIN32)
    target_compile_definitions(world_map_player_model_assets_tests PRIVATE AWL_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(world_map_player_model_assets_tests PRIVATE xinput)
endif()
if(MSVC)
    target_compile_options(world_map_player_model_assets_tests PRIVATE /W4)
else()
    target_compile_options(world_map_player_model_assets_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME world_map_player_model_assets_tests COMMAND world_map_player_model_assets_tests)
