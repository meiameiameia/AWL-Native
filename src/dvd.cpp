#include "awl/dvd.h"
#include <cstdio>

namespace awl {

DvdFile::DvdFile() : is_open_(false), handle_(nullptr) {}

DvdFile::~DvdFile() {
    Close();
}

DvdFile::DvdFile(void* arg1, const char* filename) : is_open_(false), handle_(nullptr) {
    (void)arg1;
    Open(filename);
}

bool DvdFile::Open(const char* filename) {
    if (is_open_) {
        Close();
    }

    handle_ = fopen(filename, "rb");
    if (handle_) {
        is_open_ = true;
        return true;
    }
    return false;
}

void DvdFile::Close() {
    if (is_open_ && handle_) {
        fclose(static_cast<FILE*>(handle_));
        handle_ = nullptr;
        is_open_ = false;
    }
}

bool DvdFile::Read(void* buffer, uint64_t size) {
    if (!is_open_ || !handle_ || !buffer) {
        return false;
    }

    size_t bytes_read = fread(buffer, 1, static_cast<size_t>(size), static_cast<FILE*>(handle_));
    return bytes_read == static_cast<size_t>(size);
}

} /* namespace awl */
