# Static-recompilation generator track: decoder, analysis, code generator, guest runtime and its tests.
# Owned by the P1 generator track (see docs/roadmap.md). Included from the root CMakeLists.txt.

add_library(awl_recomp_core STATIC
    tools/recomp/ppc.cpp tools/recomp/image.cpp tools/recomp/analysis.cpp
    tools/recomp/codegen.cpp tools/recomp/manifest.cpp tools/recomp/report.cpp)
target_include_directories(awl_recomp_core PUBLIC ${CMAKE_SOURCE_DIR}/tools/recomp)
add_executable(awl-recomp tools/recomp/awl_recomp.cpp)
target_compile_definitions(awl-recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(awl-recomp PRIVATE awl_recomp_core)

# Guest runtime shared by the runner and the semantics tests (generated code needs /fp:strict).
add_library(awl_recomp_runtime STATIC tools/recomp/runtime/guest.cpp)
target_include_directories(awl_recomp_runtime PUBLIC ${CMAKE_SOURCE_DIR}/tools/recomp/runtime)
if(MSVC)
    foreach(t awl_recomp_core awl-recomp awl_recomp_runtime)
        target_compile_options(${t} PRIVATE /W4)
    endforeach()
    target_compile_options(awl_recomp_runtime PRIVATE /fp:strict)
else()
    foreach(t awl_recomp_core awl-recomp awl_recomp_runtime)
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endforeach()
endif()

# The generated code is compiled once, into the static library awl_recomp_gen; the runner and the game host
# (src/hle) link it instead of each compiling the whole output.
set(AWL_RECOMP_GEN_DIR "${CMAKE_SOURCE_DIR}/build/recomp-gen" CACHE PATH "Directory written by awl-recomp gen")
if(EXISTS "${AWL_RECOMP_GEN_DIR}/sources.cmake")
    include("${AWL_RECOMP_GEN_DIR}/sources.cmake")
    add_library(awl_recomp_gen STATIC ${AWL_RECOMP_GEN_SOURCES})
    target_include_directories(awl_recomp_gen PUBLIC ${AWL_RECOMP_GEN_DIR})
    target_compile_definitions(awl_recomp_gen PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(awl_recomp_gen PUBLIC awl_recomp_runtime)
    add_executable(awl-recomp-spike tools/recomp/runtime/spike_main.cpp)
    target_compile_definitions(awl-recomp-spike PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(awl-recomp-spike PRIVATE awl_recomp_core awl_recomp_gen)
    if(MSVC)
        target_compile_options(awl_recomp_gen PRIVATE /W4 /fp:strict /MP)   # /MP: the generated chunks compile in parallel
        target_compile_options(awl-recomp-spike PRIVATE /W4 /fp:strict)
        set_source_files_properties(${AWL_RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/wd4102;/wd4702;/wd4100;/wd4189")
    else()
        target_compile_options(awl_recomp_gen PRIVATE -Wall -Wextra)
        target_compile_options(awl-recomp-spike PRIVATE -Wall -Wextra)
    endif()
endif()

add_executable(recomp_decoder_tests tests/recomp_decoder_tests.cpp)
target_link_libraries(recomp_decoder_tests PRIVATE awl_recomp_core)
add_test(NAME recomp_decoder_tests COMMAND recomp_decoder_tests)
add_executable(recomp_semantics_tests tests/recomp_semantics_tests.cpp)
target_link_libraries(recomp_semantics_tests PRIVATE awl_recomp_runtime)
add_test(NAME recomp_semantics_tests COMMAND recomp_semantics_tests)
if(MSVC)
    target_compile_options(recomp_decoder_tests PRIVATE /W4)
    target_compile_options(recomp_semantics_tests PRIVATE /W4 /fp:strict)
else()
    target_compile_options(recomp_decoder_tests PRIVATE -Wall -Wextra)
    target_compile_options(recomp_semantics_tests PRIVATE -Wall -Wextra)
endif()
add_executable(recomp_analysis_tests tests/recomp_analysis_tests.cpp)
target_link_libraries(recomp_analysis_tests PRIVATE awl_recomp_core)
add_test(NAME recomp_analysis_tests COMMAND recomp_analysis_tests)
if(MSVC)
    target_compile_options(recomp_analysis_tests PRIVATE /W4)
else()
    target_compile_options(recomp_analysis_tests PRIVATE -Wall -Wextra)
endif()

# Runtime contract tests (hle_abi.h) and HLE manifest tests.
add_executable(recomp_contract_tests tests/recomp_contract_tests.cpp)
target_link_libraries(recomp_contract_tests PRIVATE awl_recomp_runtime)
add_test(NAME recomp_contract_tests COMMAND recomp_contract_tests)
add_executable(recomp_manifest_tests tests/recomp_manifest_tests.cpp)
target_link_libraries(recomp_manifest_tests PRIVATE awl_recomp_core)
add_test(NAME recomp_manifest_tests COMMAND recomp_manifest_tests)
if(MSVC)
    target_compile_options(recomp_contract_tests PRIVATE /W4 /fp:strict)
    target_compile_options(recomp_manifest_tests PRIVATE /W4)
else()
    target_compile_options(recomp_contract_tests PRIVATE -Wall -Wextra)
    target_compile_options(recomp_manifest_tests PRIVATE -Wall -Wextra)
endif()

# End-to-end generator test: a hand-assembled DOL goes through `awl-recomp gen` at build time and the generated
# code is compiled into the test (no game bytes involved).
set(AWL_SYNTH_DIR "${CMAKE_BINARY_DIR}/recomp-synth")
add_executable(recomp_synth_dol tests/recomp_synth_dol.cpp)
set(AWL_SYNTH_OUTPUTS
    "${AWL_SYNTH_DIR}/gen_80000000.cpp" "${AWL_SYNTH_DIR}/gen_dispatch.cpp" "${AWL_SYNTH_DIR}/gen_decls.h")
add_custom_command(
    OUTPUT ${AWL_SYNTH_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AWL_SYNTH_DIR}"
    COMMAND recomp_synth_dol "${AWL_SYNTH_DIR}/synth.dol"
    COMMAND awl-recomp gen --dol "${AWL_SYNTH_DIR}/synth.dol" --out "${AWL_SYNTH_DIR}"
            --hle-manifest "${CMAKE_SOURCE_DIR}/tests/recomp_synth_manifest.txt"
    DEPENDS recomp_synth_dol awl-recomp "${CMAKE_SOURCE_DIR}/tests/recomp_synth_manifest.txt"
    COMMENT "Generating the synthetic end-to-end fixture")
add_executable(recomp_generated_tests tests/recomp_generated_tests.cpp "${AWL_SYNTH_DIR}/gen_80000000.cpp" "${AWL_SYNTH_DIR}/gen_dispatch.cpp")
target_include_directories(recomp_generated_tests PRIVATE "${AWL_SYNTH_DIR}")
target_compile_definitions(recomp_generated_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(recomp_generated_tests PRIVATE awl_recomp_runtime)
add_test(NAME recomp_generated_tests COMMAND recomp_generated_tests)
if(MSVC)
    target_compile_options(recomp_synth_dol PRIVATE /W4)
    target_compile_definitions(recomp_synth_dol PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(recomp_generated_tests PRIVATE /W4 /fp:strict)
    set_source_files_properties("${AWL_SYNTH_DIR}/gen_80000000.cpp" "${AWL_SYNTH_DIR}/gen_dispatch.cpp"
        PROPERTIES COMPILE_OPTIONS "/wd4102;/wd4702;/wd4100;/wd4189")
else()
    target_compile_options(recomp_synth_dol PRIVATE -Wall -Wextra)
    target_compile_options(recomp_generated_tests PRIVATE -Wall -Wextra)
endif()
