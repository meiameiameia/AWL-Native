#pragma once

#include <cstddef>
#include <stdbool.h>

namespace awl {

// Native replacement for the GameCube DVD/file access systems.
// Maps logical GameCube paths to native Windows project paths.

void filesystem_init();
void filesystem_shutdown();

// Mounts a logical root (e.g. "/") to a native directory (e.g. "disc/")
// Only one mount is supported currently.
bool filesystem_mount(const char* logical_root, const char* native_root);

// Resolves a logical path into a native path based on the active mount.
// Returns true if successful, false if the path is invalid (e.g. directory traversal ".." attempted)
bool filesystem_resolve_path(const char* logical_path, char* out_path, size_t out_size);

// Checks if a logical path exists on the native filesystem
bool filesystem_exists(const char* logical_path);

// Reads an entire file into a newly allocated buffer (via awl_malloc).
// Returns true on success, filling out_data with the pointer and out_size with the byte count.
// Returns false if the file is missing, empty, or cannot be read.
bool filesystem_read_entire_file(const char* logical_path, void** out_data, size_t* out_size);

// Frees the buffer returned by filesystem_read_entire_file.
void filesystem_free_file_data(void* data);

} // namespace awl
