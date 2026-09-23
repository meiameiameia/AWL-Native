#include "awl/platform.h"
#include "awl/game.h"
#include "awl/memory.h"
#include "awl/render.h"
#include "awl/filesystem.h"
#include "awl/asset_inventory.h"
#include "awl/tpl.h"
#include "awl/gpl.h"
#include "awl/gpl_render_mesh.h"
#include "awl/ground_material.h"
#include <algorithm>
#include <limits>
#include <vector>
#include <string>
#include <cstdlib>
#include <utility>

namespace {

bool environment_flag_enabled(const char* name) {
    const char* value = getenv(name);
    return value != nullptr && strcmp(value, "1") == 0;
}

struct SelectedDecodedTexture {
    uint32_t image_index = 0;
    uint32_t format = 0;
    awl::TplDecodedTexture texture;
};

struct SelectedGroundChunk {
    std::string logical_path;
    awl::GplDrawSequenceAnalysis sequence;
    awl::GplTargetMaterial material;
};

} // namespace

int main(int argc, char** argv)
{
    constexpr const char* kTargetGroundGpl = "/files/jimen-L-0-0-3.gpl";
    constexpr const char* kSceneGroundGplA = "/files/jimen-L-1-0-2.gpl";
    constexpr const char* kSceneGroundGplB = "/files/jimen-L-2-0-2.gpl";
    int exit_code = 0;
    bool target_smoke = false;
    bool preview_smoke = false;
    bool scene_smoke = false;
    for (int argument_index = 1; argument_index < argc; ++argument_index) {
        if (strcmp(argv[argument_index], "--target-smoke") == 0) {
            target_smoke = true;
        } else if (strcmp(argv[argument_index], "--preview-smoke") == 0) {
            preview_smoke = true;
        } else if (strcmp(argv[argument_index], "--scene-smoke") == 0) {
            scene_smoke = true;
        }
    }
    const char* preview_gpl = getenv("AWL_PREVIEW_GPL");
    const bool has_preview_gpl =
        preview_gpl != nullptr && preview_gpl[0] != '\0';
    const int smoke_mode_count = static_cast<int>(target_smoke) +
                                 static_cast<int>(preview_smoke) +
                                 static_cast<int>(scene_smoke);
    if (smoke_mode_count > 1) {
        return 2;
    }
    if (preview_smoke && !has_preview_gpl) {
        return 2;
    }
    const bool validation_smoke =
        target_smoke || preview_smoke || scene_smoke;
    const char* selected_ground_gpl =
        scene_smoke ? kSceneGroundGplA
                    : (has_preview_gpl && !target_smoke
                           ? preview_gpl
                           : kTargetGroundGpl);
    std::vector<std::string> selected_ground_gpls;
    selected_ground_gpls.emplace_back(selected_ground_gpl);
    if (scene_smoke) {
        selected_ground_gpls.emplace_back(kSceneGroundGplB);
    }
    
    bool hold_window_only = (getenv("AWL_HOLD_WINDOW_ONLY") != nullptr && strcmp(getenv("AWL_HOLD_WINDOW_ONLY"), "1") == 0);
    bool hold_on_error = (getenv("AWL_HOLD_ON_ERROR") != nullptr && strcmp(getenv("AWL_HOLD_ON_ERROR"), "1") == 0);

    
    // Subsystem init states
    bool memory_initialized = false;
    bool window_initialized = false;
    bool filesystem_initialized = false;
    bool input_initialized = false;
    bool render_initialized = false;
    bool audio_initialized = false;
    bool game_initialized = false;
    awl::RenderContext render_ctx;
    std::vector<SelectedGroundChunk> selected_ground_chunks;
    bool visual_gate_mesh_uploaded = false;
    bool target_texture_uploaded = false;
    bool target_material_ready = false;

    std::vector<SelectedDecodedTexture> decoded_textures;
    std::string decoded_texture_path;

    // 1. Core OS and Logging
    awl::logging_init();
    awl::platform_init();
    
    char cwd[1024];
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    AWL_LOG_INFO("Current Working Directory: %s", cwd);
    
    AWL_LOG_INFO("Active Debug Flags:");
    AWL_LOG_INFO("  AWL_NORMALIZED_PLACEMENT: %s", getenv("AWL_NORMALIZED_PLACEMENT") ? getenv("AWL_NORMALIZED_PLACEMENT") : "unset");
    AWL_LOG_INFO("  AWL_FORCE_SOLID_MESH: %s", getenv("AWL_FORCE_SOLID_MESH") ? getenv("AWL_FORCE_SOLID_MESH") : "unset");
    AWL_LOG_INFO("  AWL_MESH_ONLY: %s", getenv("AWL_MESH_ONLY") ? getenv("AWL_MESH_ONLY") : "unset");
    AWL_LOG_INFO("  AWL_DRAW_SANITY_TRIANGLE: %s", getenv("AWL_DRAW_SANITY_TRIANGLE") ? getenv("AWL_DRAW_SANITY_TRIANGLE") : "unset");
    AWL_LOG_INFO("  AWL_WIREFRAME: %s", getenv("AWL_WIREFRAME") ? getenv("AWL_WIREFRAME") : "unset");
    AWL_LOG_INFO("  AWL_DEBUG_UV_COLOR: %s", getenv("AWL_DEBUG_UV_COLOR") ? getenv("AWL_DEBUG_UV_COLOR") : "unset");
    AWL_LOG_INFO("  AWL_TEXTURE_FORCE_OPAQUE: %s", getenv("AWL_TEXTURE_FORCE_OPAQUE") ? getenv("AWL_TEXTURE_FORCE_OPAQUE") : "unset");
    AWL_LOG_INFO("  AWL_DEBUG_TEXTURE_ALPHA: %s", getenv("AWL_DEBUG_TEXTURE_ALPHA") ? getenv("AWL_DEBUG_TEXTURE_ALPHA") : "unset");
    AWL_LOG_INFO("  AWL_DEBUG_TEXTURE_TRANSPARENCY: %s", getenv("AWL_DEBUG_TEXTURE_TRANSPARENCY") ? getenv("AWL_DEBUG_TEXTURE_TRANSPARENCY") : "unset");
    AWL_LOG_INFO("  AWL_USE_CHECKER_TEXTURE: %s", getenv("AWL_USE_CHECKER_TEXTURE") ? getenv("AWL_USE_CHECKER_TEXTURE") : "unset");
    AWL_LOG_INFO("  AWL_USE_UV_GRID_TEXTURE: %s", getenv("AWL_USE_UV_GRID_TEXTURE") ? getenv("AWL_USE_UV_GRID_TEXTURE") : "unset");
    AWL_LOG_INFO("  AWL_POINT_SAMPLER: %s", getenv("AWL_POINT_SAMPLER") ? getenv("AWL_POINT_SAMPLER") : "unset");
    AWL_LOG_INFO("  AWL_FRAME_LOGS: %s", getenv("AWL_FRAME_LOGS") ? getenv("AWL_FRAME_LOGS") : "unset");
    AWL_LOG_INFO("  AWL_CAPTURE_FRAME: %s", getenv("AWL_CAPTURE_FRAME") ? getenv("AWL_CAPTURE_FRAME") : "unset");
    AWL_LOG_INFO("  AWL_PREVIEW_GPL: %s", has_preview_gpl ? preview_gpl : "unset");
    AWL_LOG_INFO("  --target-smoke: %s", target_smoke ? "enabled" : "disabled");
    AWL_LOG_INFO("  --preview-smoke: %s", preview_smoke ? "enabled" : "disabled");
    AWL_LOG_INFO("  --scene-smoke: %s", scene_smoke ? "enabled" : "disabled");
    
    // 2. Memory Arena
    awl_memory_init();
    memory_initialized = true;
    AWL_LOG_INFO("Memory arena initialized: %llu bytes", static_cast<unsigned long long>(awl_memory_free()));
    
    // 3. Platform Window
    awl::window_init();
    window_initialized = true;
    
    // 4. File system
    awl::filesystem_init();
    filesystem_initialized = true;

    // Mount native project disc directory
    if (!awl::filesystem_mount("/", "disc/")) {
        AWL_LOG_ERROR("Failed to mount filesystem. Shutting down.");
        exit_code = 1;
        goto shutdown;
    }

    // --- Asset Inventory (Dev-only) ---
    // awl::asset_inventory_run("/");
    
    // Verified container and target-GPL image binding with development-only
    // game-state selection.
    {
        awl::GroundTextureBinding ground_binding;
        if (!awl::build_ground_texture_binding(0, 0, &ground_binding)) {
            AWL_LOG_ERROR("Unable to construct verified ground texture binding.");
            exit_code = 1;
            goto shutdown;
        }
        AWL_LOG_INFO(
            "Ground alias binding from DOL 0x8001BF3C: %s -> %s",
            ground_binding.alias.c_str(), ground_binding.logical_path.c_str());
        AWL_LOG_INFO(
            "Development state selection is period=0 step=0; game-state axis semantics remain unverified.");

        std::vector<uint32_t> required_image_indices;
        for (const std::string& logical_path : selected_ground_gpls) {
            awl::GplFile selected_gpl;
            if (!awl::gpl_load_from_file(
                    logical_path.c_str(), &selected_gpl)) {
                AWL_LOG_ERROR("Unable to load selected GPL: %s",
                              logical_path.c_str());
                exit_code = 1;
                goto shutdown;
            }

            std::vector<awl::GplTextureCommand> texture_commands;
            if (!awl::gpl_parse_texture_commands_for_analysis(
                    selected_gpl, &texture_commands) ||
                texture_commands.empty()) {
                AWL_LOG_ERROR(
                    "Selected GPL did not contain verified type-1 texture commands: %s",
                    logical_path.c_str());
                exit_code = 1;
                goto shutdown;
            }
            for (const awl::GplTextureCommand& texture_command :
                 texture_commands) {
                AWL_LOG_INFO(
                    "GPL texture binding for %s from DOL 0x801A50E8/0x801A5F6C: "
                    "command=%u image=%u unit=%u state=0x%02X",
                    logical_path.c_str(), texture_command.command_index,
                    texture_command.image_index, texture_command.texture_unit,
                    texture_command.state_selector);
            }

            SelectedGroundChunk chunk;
            chunk.logical_path = logical_path;
            const char* selected_base = strrchr(logical_path.c_str(), '/');
            selected_base = selected_base ? selected_base + 1
                                          : logical_path.c_str();
            if (!awl::gpl_parse_draw_sequence_for_analysis(
                    selected_gpl, selected_base, &chunk.sequence)) {
                AWL_LOG_ERROR(
                    "Selected GPL ordered material/draw sequence is unsupported or malformed: %s",
                    logical_path.c_str());
                exit_code = 1;
                goto shutdown;
            }
            if (!awl::gpl_parse_target_material_for_analysis(
                    selected_gpl, &chunk.material)) {
                AWL_LOG_ERROR(
                    "Selected GPL material color/TEV state is unsupported or malformed: %s",
                    logical_path.c_str());
                exit_code = 1;
                goto shutdown;
            }
            AWL_LOG_INFO(
                "GPL target material for %s from DOL 0x801A59C0/0x801A714C: "
                "command=%u TEV=0x%08X source=%s RGBA=(%u,%u,%u,%u)",
                logical_path.c_str(), chunk.material.command_index,
                chunk.material.tev_mode_word,
                chunk.material.uses_vertex_color ? "COLOR0" : "register",
                chunk.material.red, chunk.material.green, chunk.material.blue,
                chunk.material.alpha);

            for (const awl::GplDrawBatchAnalysis& batch :
                 chunk.sequence.batches) {
                if (std::find(required_image_indices.begin(),
                              required_image_indices.end(),
                              batch.texture_image_index) ==
                    required_image_indices.end()) {
                    required_image_indices.push_back(
                        batch.texture_image_index);
                }
            }
            selected_ground_chunks.push_back(std::move(chunk));
        }
        target_material_ready =
            selected_ground_chunks.size() == selected_ground_gpls.size();

        awl::TplFile test_tpl;
        AWL_LOG_INFO("Loading verified ground TPL container: %s",
                     ground_binding.logical_path.c_str());
        if (awl::tpl_load_from_file(ground_binding.logical_path.c_str(), &test_tpl)) {
            awl::tpl_dump_metadata(test_tpl);

            for (uint32_t image_index : required_image_indices) {
                if (image_index >= test_tpl.textures.size()) {
                    AWL_LOG_ERROR(
                        "GPL texture image index %u exceeds TPL image count %zu.",
                        image_index, test_tpl.textures.size());
                    exit_code = 1;
                    goto shutdown;
                }
                const auto& selected_texture = test_tpl.textures[image_index];
                AWL_LOG_INFO(
                    "Decoding ordered GPL TPL image %u (Format %u).",
                    image_index, selected_texture.header.format);
                SelectedDecodedTexture decoded;
                decoded.image_index = image_index;
                decoded.format = selected_texture.header.format;
                if (!awl::tpl_decode_mip_chain_to_rgba8(
                        selected_texture, &decoded.texture) ||
                    decoded.texture.mip_levels.empty()) {
                    AWL_LOG_ERROR(
                        "Ordered GPL texture decode failed or returned no data.");
                    exit_code = 1;
                    goto shutdown;
                }
                AWL_LOG_INFO("  Decoded width: %u", selected_texture.header.width);
                AWL_LOG_INFO("  Decoded height: %u", selected_texture.header.height);
                AWL_LOG_INFO("  Decoded mip levels: %zu",
                             decoded.texture.mip_levels.size());
                decoded_textures.push_back(std::move(decoded));
            }
            decoded_texture_path = ground_binding.logical_path;
            awl::tpl_free(&test_tpl);
        } else {
            AWL_LOG_ERROR("Unable to load verified ground TPL container.");
            exit_code = 1;
            goto shutdown;
        }
    }
    // Extract target mesh for visual gate

    // --- GPL Parser Diagnostic Loop ---
    if (!hold_window_only && !scene_smoke) {
        const char* test_files[] = {
            "/files/2dground.gpl",
            selected_ground_gpl,
            "/files/debugarrow.gpl",
            "/files/jimen-bottom.gpl"
        };

    for (const char* filename : test_files) {
        if ((target_smoke || preview_smoke) &&
            strcmp(filename, selected_ground_gpl) != 0) {
            continue;
        }
        awl::GplFile test_gpl;
        AWL_LOG_INFO("=========================================================");
        AWL_LOG_INFO("Loading GPL: %s", filename);
        if (awl::gpl_load_from_file(filename, &test_gpl)) {
            awl::gpl_dump_metadata(test_gpl);
            if (strcmp(filename, selected_ground_gpl) == 0) {
                AWL_LOG_INFO(
                    "Selected GPL already validated as %zu ordered draw batch(es).",
                    selected_ground_chunks[0].sequence.batches.size());
                awl::gpl_free(&test_gpl);
                continue;
            }
            awl::gpl_analyze_payload(test_gpl);
            
            // Extract basename for debug files
            const char* base = strrchr(filename, '/');
            base = base ? base + 1 : filename;
            
            awl::GplMeshAnalysis extracted_mesh;
            const bool display_list_parsed =
                awl::gpl_parse_display_list_for_analysis(test_gpl, base, &extracted_mesh);
            (void)display_list_parsed;
            
            awl::gpl_free(&test_gpl);
        } else {
            AWL_LOG_ERROR("Failed to load %s", filename);
            if (hold_on_error) {
                AWL_LOG_INFO("Holding window open due to AWL_HOLD_ON_ERROR");
            } else {
                exit_code = 1;
                goto shutdown;
            }
        }
    }
    }
    if (!hold_window_only) {
        bool sequence_ready = !selected_ground_chunks.empty();
        for (const SelectedGroundChunk& chunk : selected_ground_chunks) {
            if (chunk.sequence.batches.empty()) {
                sequence_ready = false;
                break;
            }
            for (const awl::GplDrawBatchAnalysis& batch :
                 chunk.sequence.batches) {
                if (batch.mesh.positions.empty() ||
                    batch.mesh.raw_refs.empty()) {
                    sequence_ready = false;
                    break;
                }
            }
            if (!sequence_ready) {
                break;
            }
        }
        if (!sequence_ready) {
            AWL_LOG_ERROR(
                "Required selected GPL mesh is unavailable; fallback rendering is not a successful validation.");
            exit_code = 1;
            if (!hold_on_error || validation_smoke) {
                goto shutdown;
            }
        }
    }
    // ------------------------

    // 5. Input
    awl::input_init();
    input_initialized = true;
    
    // 6. Rendering
    if (!render_ctx.init(static_cast<HWND>(awl::platform_get_window_handle()))) {
        AWL_LOG_ERROR("Failed to initialize render context. Shutting down.");
        exit_code = 1;
        goto shutdown;
    }
    render_initialized = true;
    const awl::GplTargetMaterial& primary_material =
        selected_ground_chunks.front().material;
    render_ctx.set_debug_material_color_rgba8(
        primary_material.uses_vertex_color ? 0xFF : primary_material.red,
        primary_material.uses_vertex_color ? 0xFF : primary_material.green,
        primary_material.uses_vertex_color ? 0xFF : primary_material.blue,
        primary_material.uses_vertex_color ? 0xFF : primary_material.alpha);

    const bool use_checker = environment_flag_enabled("AWL_USE_CHECKER_TEXTURE");
    const bool use_uv_grid = environment_flag_enabled("AWL_USE_UV_GRID_TEXTURE");

    if (!hold_window_only && !use_checker && !use_uv_grid) {
        target_texture_uploaded = !decoded_textures.empty();
        for (size_t texture_slot = 0;
             texture_slot < decoded_textures.size(); ++texture_slot) {
            const SelectedDecodedTexture& decoded = decoded_textures[texture_slot];
            AWL_LOG_INFO(
                "Uploading ordered ground texture %s image %u to slot %zu",
                decoded_texture_path.c_str(), decoded.image_index, texture_slot);
            if (!render_ctx.create_debug_texture_from_decoded_tpl(
                    static_cast<uint32_t>(texture_slot), decoded.texture)) {
                AWL_LOG_ERROR("Failed to upload ordered GPL texture slot.");
                target_texture_uploaded = false;
                exit_code = 1;
                goto shutdown;
            }
        }
        if (validation_smoke && decoded_textures.size() > 1) {
            AWL_LOG_INFO(
                "Validation smoke replacing texture slot 0 after higher slots to verify resource preservation.");
            if (!render_ctx.create_debug_texture_from_decoded_tpl(
                    0, decoded_textures[0].texture)) {
                AWL_LOG_ERROR(
                    "Failed to replace ordered GPL texture slot during validation.");
                target_texture_uploaded = false;
                exit_code = 1;
                goto shutdown;
            }
        }
    }
    
    // Upload the ordered GPL draw sequence as one shared vertex/index buffer
    // plus exact contiguous DX11 draw batches.
    if (!hold_window_only && !selected_ground_chunks.empty()) {
        AWL_LOG_INFO("Uploading %zu selected ordered GPL chunk(s) to DX11.",
                     selected_ground_chunks.size());
        std::vector<awl::RenderContext::DebugTexturedVertex> render_verts;
        std::vector<uint32_t> render_indices;
        std::vector<awl::DebugDrawBatch> render_batches;
        bool sequence_built = true;

        for (const SelectedGroundChunk& chunk : selected_ground_chunks) {
            const uint8_t material_red =
                chunk.material.uses_vertex_color ? 0xFF : chunk.material.red;
            const uint8_t material_green =
                chunk.material.uses_vertex_color ? 0xFF : chunk.material.green;
            const uint8_t material_blue =
                chunk.material.uses_vertex_color ? 0xFF : chunk.material.blue;
            const uint8_t material_alpha =
                chunk.material.uses_vertex_color ? 0xFF : chunk.material.alpha;
            for (const awl::GplDrawBatchAnalysis& source_batch :
                 chunk.sequence.batches) {
            awl::GplRenderMesh cpu_mesh;
            const size_t max_render_elements =
                (std::numeric_limits<uint32_t>::max)();
            if (!awl::build_gpl_render_mesh(source_batch.mesh, cpu_mesh) ||
                cpu_mesh.vertices.size() > max_render_elements ||
                cpu_mesh.indices.size() > max_render_elements ||
                render_verts.size() >
                    max_render_elements - cpu_mesh.vertices.size() ||
                render_indices.size() >
                    max_render_elements - cpu_mesh.indices.size()) {
                sequence_built = false;
                break;
            }

            const uint32_t vertex_base =
                static_cast<uint32_t>(render_verts.size());
            const uint32_t first_index =
                static_cast<uint32_t>(render_indices.size());
            for (const awl::GplRenderVertex& vertex : cpu_mesh.vertices) {
                awl::RenderContext::DebugTexturedVertex render_vertex;
                render_vertex.x = vertex.x;
                render_vertex.y = vertex.y;
                render_vertex.z = vertex.z;
                render_vertex.u = vertex.u;
                render_vertex.v = vertex.v;
                render_vertex.r = vertex.r;
                render_vertex.g = vertex.g;
                render_vertex.b = vertex.b;
                render_vertex.a = vertex.a;
                render_verts.push_back(render_vertex);
            }
            for (uint32_t index : cpu_mesh.indices) {
                render_indices.push_back(vertex_base + index);
            }

            const auto texture = std::find_if(
                decoded_textures.begin(), decoded_textures.end(),
                [&](const SelectedDecodedTexture& candidate) {
                    return candidate.image_index ==
                           source_batch.texture_image_index;
                });
            if (texture == decoded_textures.end()) {
                sequence_built = false;
                break;
            }
            awl::DebugDrawBatch render_batch;
            render_batch.first_index = first_index;
            render_batch.index_count =
                static_cast<uint32_t>(cpu_mesh.indices.size());
            render_batch.texture_slot = static_cast<uint32_t>(
                std::distance(decoded_textures.begin(), texture));
            render_batch.material_red = material_red;
            render_batch.material_green = material_green;
            render_batch.material_blue = material_blue;
            render_batch.material_alpha = material_alpha;
            render_batches.push_back(render_batch);
            AWL_LOG_INFO(
                "  Chunk=%s batch command=%u texture_image=%u vertices=%zu indices=%zu",
                chunk.logical_path.c_str(), source_batch.command_index,
                source_batch.texture_image_index, cpu_mesh.vertices.size(),
                cpu_mesh.indices.size());
            }
            if (!sequence_built) {
                break;
            }
        }

        if (!sequence_built || render_verts.empty() ||
            render_indices.empty() || render_batches.empty()) {
            AWL_LOG_ERROR("GPL ordered render-mesh construction failed.");
            exit_code = 1;
        } else {
            AWL_LOG_INFO("Ordered GPL total vertex count: %zu",
                         render_verts.size());
            AWL_LOG_INFO("Ordered GPL total index count: %zu",
                         render_indices.size());
            const bool uploaded = use_checker || use_uv_grid
                ? render_ctx.create_debug_mesh(
                      render_verts.data(),
                      static_cast<uint32_t>(render_verts.size()),
                      render_indices.data(),
                      static_cast<uint32_t>(render_indices.size()))
                : render_ctx.create_debug_mesh_batches(
                      render_verts.data(),
                      static_cast<uint32_t>(render_verts.size()),
                      render_indices.data(),
                      static_cast<uint32_t>(render_indices.size()),
                      render_batches.data(),
                      static_cast<uint32_t>(render_batches.size()));
            if (uploaded) {
                AWL_LOG_INFO("  Ordered debug mesh resources created.");
                visual_gate_mesh_uploaded = true;
            } else {
                AWL_LOG_ERROR("Failed to create required ordered debug mesh.");
                exit_code = 1;
            }
        }
    }

    if (!hold_window_only && !visual_gate_mesh_uploaded &&
        (!hold_on_error || validation_smoke)) {
        goto shutdown;
    }

    const bool force_solid_mesh = environment_flag_enabled("AWL_FORCE_SOLID_MESH");
    const bool debug_uv_color = environment_flag_enabled("AWL_DEBUG_UV_COLOR");
    const bool target_pipeline_override =
        environment_flag_enabled("AWL_NORMALIZED_PLACEMENT") ||
        environment_flag_enabled("AWL_DRAW_SANITY_TRIANGLE") ||
        environment_flag_enabled("AWL_WIREFRAME") ||
        environment_flag_enabled("AWL_TEXTURE_FORCE_OPAQUE") ||
        environment_flag_enabled("AWL_DEBUG_TEXTURE_ALPHA") ||
         environment_flag_enabled("AWL_DEBUG_TEXTURE_TRANSPARENCY") ||
         environment_flag_enabled("AWL_POINT_SAMPLER");
    if (validation_smoke &&
        (hold_window_only || use_checker || use_uv_grid || force_solid_mesh ||
         debug_uv_color || target_pipeline_override ||
         ((target_smoke || scene_smoke) && has_preview_gpl))) {
        AWL_LOG_ERROR(
            "Validation smoke cannot use overrides that bypass its selected mesh, texture, sampler, material shader, or placement.");
        exit_code = 1;
        goto shutdown;
    }
    
    if (use_uv_grid) {
        AWL_LOG_INFO(
            "UV grid is a debug-only override of the verified material texture.");
        AWL_LOG_INFO("Using generated opaque UV grid texture for real-UV debug.");
        
        uint32_t grid_w = 256;
        uint32_t grid_h = 256;
        AWL_LOG_INFO("  width: %u, height: %u, alpha mode: fully opaque", grid_w, grid_h);
        
        std::vector<uint8_t> grid_pixels(grid_w * grid_h * 4);
        for (uint32_t y = 0; y < grid_h; ++y) {
            for (uint32_t x = 0; x < grid_w; ++x) {
                // Background gradient
                uint8_t r = static_cast<uint8_t>((x * 255) / grid_w);
                uint8_t g = static_cast<uint8_t>((y * 255) / grid_h);
                uint8_t b = 128; // fixed blue base
                
                // Add quadrant color variation
                if (x >= grid_w / 2) r = min(r + 50, 255);
                if (y >= grid_h / 2) g = min(g + 50, 255);
                
                // High-frequency grid lines every 16 pixels
                bool is_grid_line = (x % 16 == 0) || (y % 16 == 0);
                if (is_grid_line) {
                    r = 0; g = 0; b = 0; // Black grid lines
                }
                
                uint32_t idx = (y * grid_w + x) * 4;
                grid_pixels[idx + 0] = r;
                grid_pixels[idx + 1] = g;
                grid_pixels[idx + 2] = b;
                grid_pixels[idx + 3] = 255; // Fully opaque
            }
        }
        if (render_ctx.create_texture_from_rgba8(
                grid_pixels.data(), grid_pixels.size(), grid_w, grid_h)) {
            AWL_LOG_INFO("  UV grid texture SRV bound.");
        }
    } else if (use_checker) {
        AWL_LOG_INFO(
            "Checker is a debug-only override of the verified material texture.");
        AWL_LOG_INFO("Using generated opaque checker texture for real-UV debug.");
        
        uint32_t check_w = 64;
        uint32_t check_h = 64;
        AWL_LOG_INFO("  width: %u, height: %u, alpha mode: fully opaque", check_w, check_h);
        
        std::vector<uint8_t> checker_pixels(check_w * check_h * 4);
        for (uint32_t y = 0; y < check_h; ++y) {
            for (uint32_t x = 0; x < check_w; ++x) {
                bool is_white = ((x / 8) + (y / 8)) % 2 == 0;
                uint8_t color = is_white ? 255 : 32; // Stronger contrast
                uint32_t idx = (y * check_w + x) * 4;
                checker_pixels[idx + 0] = color;
                checker_pixels[idx + 1] = color;
                checker_pixels[idx + 2] = color;
                checker_pixels[idx + 3] = 255; // Fully opaque
            }
        }
        if (render_ctx.create_texture_from_rgba8(
                checker_pixels.data(), checker_pixels.size(), check_w, check_h)) {
            AWL_LOG_INFO("  Checker texture SRV bound.");
        }
    } else if (!hold_window_only && target_texture_uploaded) {
        AWL_LOG_INFO(
            "Using %zu ordered GPL texture resource(s) from %s",
            decoded_textures.size(), decoded_texture_path.c_str());
    } else {
        AWL_LOG_INFO("No texture available, rendering fallback triangle.");
    }

    if (validation_smoke &&
        (!visual_gate_mesh_uploaded || !target_texture_uploaded ||
         !target_material_ready)) {
        AWL_LOG_ERROR("Validation smoke prerequisites were not satisfied.");
        exit_code = 1;
        goto shutdown;
    }
    
    AWL_LOG_INFO("Render context initialized.");

    // 7. Audio
    awl::audio_init();
    audio_initialized = true;

    // 8. Game Systems
    awl::game_init();
    game_initialized = true;

    // 9. Main Loop
    AWL_LOG_INFO("Entering main loop...");
    int frame_count = 0;
    bool validation_smoke_completed = false;
    while (awl::platform_pump_messages()) {
        awl::time_begin_frame();
        awl::input_begin_frame();
        awl::game_update(awl::time_get_delta());
        if (frame_count < 10) AWL_LOG_INFO("Frame %d: before render", frame_count);

        const uint64_t presented_before = render_ctx.presented_frame_count();
        if (!render_ctx.render()) {
            AWL_LOG_ERROR("Rendering failed. Shutting down.");
            exit_code = 1;
            break;
        }

        if (validation_smoke &&
            render_ctx.presented_frame_count() == presented_before) {
            AWL_LOG_ERROR(
                "Validation smoke did not present a visible frame.");
            exit_code = 1;
            break;
        }

        if (frame_count < 10) AWL_LOG_INFO("Frame %d: after render loop iteration", frame_count);
        frame_count++;
        if (validation_smoke && render_ctx.presented_frame_count() >= 10) {
            validation_smoke_completed = true;
            if (target_smoke) {
                AWL_LOG_INFO(
                    "Target smoke passed: required mesh and texture rendered for %llu presented frames.",
                    static_cast<unsigned long long>(render_ctx.presented_frame_count()));
            } else if (scene_smoke) {
                AWL_LOG_INFO(
                    "Scene smoke passed: %zu ground chunks rendered together in world coordinates for %llu presented frames.",
                    selected_ground_chunks.size(),
                    static_cast<unsigned long long>(
                        render_ctx.presented_frame_count()));
            } else {
                AWL_LOG_INFO(
                    "Preview smoke passed for %s: selected mesh, COLOR0/material state, and texture rendered for %llu presented frames.",
                    selected_ground_gpl,
                    static_cast<unsigned long long>(render_ctx.presented_frame_count()));
            }
            break;
        }
    }

    if (validation_smoke && !validation_smoke_completed && exit_code == 0) {
        AWL_LOG_ERROR(
            "Validation smoke exited before completing 10 presented frames.");
        exit_code = 1;
    }

    AWL_LOG_INFO("Main loop exited. Reason: %d", (int)awl::platform_get_exit_reason());

    goto shutdown;

shutdown:
    // 10. Shutdown (Strict Reverse Order, only if initialized)
    if (game_initialized) awl::game_shutdown();
    if (audio_initialized) awl::audio_shutdown();
    if (render_initialized) render_ctx.shutdown();
    if (input_initialized) awl::input_shutdown();
    if (filesystem_initialized) awl::filesystem_shutdown();
    if (window_initialized) awl::window_shutdown();
    if (memory_initialized) awl_memory_shutdown();
    
    awl::platform_shutdown();

    return exit_code;
}
