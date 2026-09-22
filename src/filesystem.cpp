#include "awl/filesystem.h"
#include "awl/platform.h"
#include "awl/memory.h"
#include <string>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <windows.h>

namespace awl {

static bool g_mounted = false;
static std::string g_logical_root;
static std::string g_native_root;

void filesystem_init() {
    AWL_LOG_INFO("File system init.");
    g_mounted = false;
    g_logical_root = "";
    g_native_root = "";
}

void filesystem_shutdown() {
    AWL_LOG_INFO("File system shutdown.");
    g_mounted = false;
    g_logical_root = "";
    g_native_root = "";
}

// Checks if a path contains a strict ".." traversal segment
static bool contains_traversal_segment(const std::string& path) {
    if (path == ".." || path.find("../") == 0 || path.find("/../") != std::string::npos) {
        return true;
    }
    // Also check if it ends with "/.."
    if (path.length() >= 3 && path.substr(path.length() - 3) == "/..") {
        return true;
    }
    return false;
}

static std::string sanitize_path(const std::string& path) {
    std::string result;
    bool last_was_slash = false;

    for (char c : path) {
        char normalized = (c == '\\') ? '/' : c;
        if (normalized == '/') {
            if (!last_was_slash) {
                result += '/';
                last_was_slash = true;
            }
        } else {
            result += normalized;
            last_was_slash = false;
        }
    }
    return result;
}

bool filesystem_mount(const char* logical_root, const char* native_root) {
    if (g_mounted) {
        AWL_LOG_ERROR("Filesystem already mounted. Duplicate mounts not supported.");
        return false;
    }

    if (!logical_root || logical_root[0] == '\0') {
        AWL_LOG_ERROR("Mount failed: Invalid logical root.");
        return false;
    }

    if (!native_root || native_root[0] == '\0') {
        AWL_LOG_ERROR("Mount failed: Invalid native root.");
        return false;
    }

    std::string lroot = sanitize_path(logical_root);
    if (lroot[0] != '/') {
        AWL_LOG_ERROR("Mount failed: Logical root must start with '/'.");
        return false;
    }

    std::string nroot = sanitize_path(native_root);
    if (contains_traversal_segment(nroot)) {
        AWL_LOG_ERROR("Mount failed: Native root cannot contain traversal segments.");
        return false;
    }

    g_logical_root = lroot;
    g_native_root = nroot;

    // Ensure native root ends with a slash for easy appending
    if (g_native_root.back() != '/') {
        g_native_root += '/';
    }

    // Ensure logical root ends with a slash for prefix matching (unless it's just "/")
    if (g_logical_root.length() > 1 && g_logical_root.back() != '/') {
        g_logical_root += '/';
    }

    g_mounted = true;
    AWL_LOG_INFO("Mounted logical '%s' to native '%s'",
                 g_logical_root.c_str(), g_native_root.c_str());
    return true;
}

bool filesystem_resolve_path(const char* logical_path, char* out_path, size_t out_size) {
    if (!g_mounted) {
        AWL_LOG_ERROR("Cannot resolve path: filesystem not mounted.");
        return false;
    }

    if (!logical_path || !out_path || out_size == 0) {
        return false;
    }

    std::string path = logical_path;
    if (path.empty()) {
        return false; // Reject empty paths
    }

    path = sanitize_path(path);

    if (contains_traversal_segment(path)) {
        AWL_LOG_ERROR("Rejected path traversal attempt: %s", path.c_str());
        return false;
    }

    if (path[0] != '/') {
        AWL_LOG_ERROR("Rejected relative logical path: %s", path.c_str());
        return false;
    }

    // Check if the requested path is actually under the mounted logical root
    if (g_logical_root != "/" && path.find(g_logical_root) != 0) {
        AWL_LOG_ERROR("Rejected path escaping mount root: %s", path.c_str());
        return false;
    }

    std::string relative_path;
    if (g_logical_root == "/") {
        relative_path = path.substr(1); // strip leading slash
    } else {
        relative_path = path.substr(g_logical_root.length());
    }

    std::string resolved = g_native_root + relative_path;

    if (resolved.length() >= out_size) {
        AWL_LOG_ERROR("Resolved path exceeds output buffer size");
        return false;
    }

    snprintf(out_path, out_size, "%s", resolved.c_str());
    return true;
}

bool filesystem_exists(const char* logical_path) {
    if (!g_mounted) return false;

    char native_path[MAX_PATH];
    if (!filesystem_resolve_path(logical_path, native_path, sizeof(native_path))) {
        return false;
    }

    DWORD attribs = GetFileAttributesA(native_path);
    return (attribs != INVALID_FILE_ATTRIBUTES && !(attribs & FILE_ATTRIBUTE_DIRECTORY));
}

bool filesystem_read_entire_file(const char* logical_path, void** out_data, size_t* out_size) {
    if (!out_data || !out_size) return false;
    *out_data = nullptr;
    *out_size = 0;

    if (!g_mounted) {
        return false;
    }

    char native_path[MAX_PATH];
    if (!filesystem_resolve_path(logical_path, native_path, sizeof(native_path))) {
        return false;
    }

    FILE* file = fopen(native_path, "rb");
    if (!file) {
        AWL_LOG_ERROR("Filesystem: Resolved '%s' -> '%s' (exists: false)", logical_path, native_path);
        return false;
    }
    AWL_LOG_INFO("Filesystem: Resolved '%s' -> '%s' (exists: true)", logical_path, native_path);

    if (_fseeki64(file, 0, SEEK_END) != 0) {
        AWL_LOG_ERROR("Failed to seek to end of file %s", logical_path);
        fclose(file);
        return false;
    }
    const int64_t file_size_value = _ftelli64(file);
    if (file_size_value <= 0 ||
        static_cast<uint64_t>(file_size_value) >
            (std::numeric_limits<uint32_t>::max)()) {
        AWL_LOG_ERROR("File size is invalid or unsupported for %s", logical_path);
        fclose(file);
        return false;
    }
    if (_fseeki64(file, 0, SEEK_SET) != 0) {
        AWL_LOG_ERROR("Failed to rewind file %s", logical_path);
        fclose(file);
        return false;
    }

    const size_t file_size = static_cast<size_t>(file_size_value);

    void* buffer = awl_malloc(static_cast<u32>(file_size));
    if (!buffer) {
        AWL_LOG_ERROR("Failed to allocate %zu bytes for file %s", file_size, logical_path);
        fclose(file);
        return false;
    }

    size_t read_bytes = fread(buffer, 1, file_size, file);
    if (read_bytes != file_size) {
        AWL_LOG_ERROR("Failed to read entire file %s", logical_path);
        awl_free(buffer);
        fclose(file);
        return false;
    }

    fclose(file);
    *out_data = buffer;
    *out_size = read_bytes;
    return true;
}

void filesystem_free_file_data(void* data) {
    if (data) {
        awl_free(data);
    }
}

} // namespace awl
