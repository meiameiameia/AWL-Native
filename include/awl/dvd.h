#pragma once

#include <cstdint>
#include <string>

namespace awl {

/* DVD file handling — maps GameCube DVDRead to standard file I/O */

class DvdFile {
public:
    DvdFile();
    ~DvdFile();

    DvdFile(void* arg1, const char* filename);

    bool Open(const char* filename);
    void Close();
    bool Read(void* buffer, uint64_t size);

    bool is_open() const { return is_open_; }

private:
    bool is_open_;
    void* handle_;
};

} /* namespace awl */
