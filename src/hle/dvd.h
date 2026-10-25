// Native DVD drive: priority queue of command blocks, file reads through the extracted disc, completion events.
// Replaces the SDK's public command functions (docs/research/hle-platform.md, section 5.6).
#pragma once

#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "platform.h"

namespace awl::hle {

// DVDCommandBlock fields (SDK layout, read from the command functions' stores).
namespace dvdblock {
constexpr uint32_t Command = 0x08, State = 0x0C, Offset = 0x10, Length = 0x14, Addr = 0x18, Transferred = 0x20, Callback = 0x28;
}
namespace dvdstate {
constexpr int32_t End = 0, Busy = 1, Waiting = 2, Canceled = 10;
}
namespace dvdcmd {
constexpr uint32_t Read = 1, Seek = 2, ReadDiskId = 5, InitStream = 6, CancelStream = 7, RequestAudioError = 9, Inquiry = 14;
}

struct DiscFile {
    uint32_t offset = 0, size = 0;
    std::string path;            // relative to <disc>/files
};

class Dvd {
public:
    explicit Dvd(Platform& p);
    ~Dvd();
    // Parses the FST (entry table + names). Returns false with a message if it is malformed.
    bool loadFst(const std::vector<uint8_t>& fst, std::string& err);
    const std::vector<DiscFile>& files() const { return files_; }

    // Command entry points (called by the handlers; the context is the caller's).
    bool submit(Ctx& c, uint32_t block, uint32_t command, uint32_t prio);
    bool cancel(Ctx& c, uint32_t block, uint32_t callback);
    void closeFile();                  // releases the host file handle (so the disc directory can be removed)
    bool busy() const { return active_ != 0 || !queue_.empty(); }
    // Reads len bytes of disc address space at offset into dst; returns bytes produced or -1 on failure.
    int64_t readDisc(uint32_t offset, uint32_t len, uint8_t* dst);

    uint64_t completedReads = 0, completedOther = 0;

private:
    struct Request { uint64_t id; uint32_t block; uint32_t command; uint32_t prio; uint64_t seq; };
    void startNext();
    void complete(Ctx& c, uint64_t id);
    uint64_t latency(uint32_t command, uint32_t len) const;

    Platform& p_;
    std::vector<DiscFile> files_;       // sorted by disc offset
    std::deque<Request> queue_;
    uint64_t active_ = 0;               // request id being executed (0 = idle)
    Request activeReq_{};
    uint64_t nextId_ = 1, nextSeq_ = 0;
    std::string openPath_;
    FILE* openFile_ = nullptr;
};

}  // namespace awl::hle
