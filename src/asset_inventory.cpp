#include "awl/asset_inventory.h"
#include "awl/filesystem.h"
#include "awl/platform.h"
#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>

namespace awl {

namespace fs = std::filesystem;

struct AssetStats {
    uint32_t count = 0;
    uint64_t total_size = 0;
};

static std::string get_extension(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return "";
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return ext;
}

static void log_preview(const std::string& logical_path, void* data, size_t size) {
    AWL_LOG_INFO("  Asset Preview: %s (%zu bytes)", logical_path.c_str(), size);
    
    // Hex dump first 16 bytes
    size_t preview_size = (size < 16) ? size : 16;
    std::string hex_str = "";
    std::string ascii_str = "";
    
    uint8_t* bytes = static_cast<uint8_t*>(data);
    for (size_t i = 0; i < preview_size; ++i) {
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X ", bytes[i]);
        hex_str += hex;
        
        if (bytes[i] >= 32 && bytes[i] <= 126) {
            ascii_str += (char)bytes[i];
        } else {
            ascii_str += '.';
        }
    }
    
    AWL_LOG_INFO("    Hex:   %s", hex_str.c_str());
    AWL_LOG_INFO("    ASCII: %s", ascii_str.c_str());
}

void asset_inventory_run(const char* logical_root) {
    AWL_LOG_INFO("--- ASSET INVENTORY START: %s ---", logical_root);

    char native_root[260];
    if (!filesystem_resolve_path(logical_root, native_root, sizeof(native_root))) {
        AWL_LOG_ERROR("Failed to resolve logical root for inventory: %s", logical_root);
        return;
    }

    if (!fs::exists(native_root) || !fs::is_directory(native_root)) {
        AWL_LOG_ERROR("Native root does not exist or is not a directory: %s", native_root);
        return;
    }

    uint32_t total_files = 0;
    uint64_t total_bytes = 0;
    std::map<std::string, AssetStats> ext_stats;
    std::vector<std::pair<std::string, uint64_t>> largest_files;

    int previews_done = 0;
    const int MAX_PREVIEWS = 5;

    for (const auto& entry : fs::recursive_directory_iterator(native_root)) {
        if (entry.is_regular_file()) {
            total_files++;
            uint64_t size = entry.file_size();
            total_bytes += size;

            std::string native_path = entry.path().string();
            // Convert to logical path by stripping native root prefix
            std::string rel_path = native_path.substr(std::string(native_root).length());
            std::replace(rel_path.begin(), rel_path.end(), '\\', '/');
            std::string logical_path = std::string(logical_root);
            if (logical_path.back() != '/' && rel_path.front() != '/') {
                logical_path += "/";
            }
            if (logical_path == "/" && rel_path.front() == '/') {
                rel_path = rel_path.substr(1);
            }
            logical_path += rel_path;

            std::string ext = get_extension(logical_path);
            if (ext.empty()) ext = "<none>";
            
            ext_stats[ext].count++;
            ext_stats[ext].total_size += size;

            largest_files.push_back({logical_path, size});
            std::sort(largest_files.begin(), largest_files.end(), 
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            if (largest_files.size() > 5) largest_files.pop_back();

            // Preview a few interesting known assets safely via the logical API
            if (previews_done < MAX_PREVIEWS && (ext == ".tpl" || ext == ".gpl" || ext == ".arc")) {
                void* data = nullptr;
                size_t read_size = 0;
                if (filesystem_read_entire_file(logical_path.c_str(), &data, &read_size)) {
                    log_preview(logical_path, data, read_size);
                    filesystem_free_file_data(data);
                    previews_done++;
                }
            }
        }
    }

    AWL_LOG_INFO("Total files scanned: %u", total_files);
    AWL_LOG_INFO("Total bytes scanned: %llu bytes", (unsigned long long)total_bytes);

    AWL_LOG_INFO("Counts by extension:");
    for (const auto& kv : ext_stats) {
        AWL_LOG_INFO("  %s: %u files (%llu bytes)", kv.first.c_str(), kv.second.count, (unsigned long long)kv.second.total_size);
    }

    AWL_LOG_INFO("Largest files:");
    for (const auto& kv : largest_files) {
        AWL_LOG_INFO("  %s (%llu bytes)", kv.first.c_str(), (unsigned long long)kv.second);
    }

    AWL_LOG_INFO("--- ASSET INVENTORY COMPLETE ---");
}

} // namespace awl
