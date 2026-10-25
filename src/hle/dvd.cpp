#include "dvd.h"

#include <algorithm>

namespace awl::hle {

Dvd::Dvd(Platform& p) : p_(p) {
    p.onEvent(Ev::DvdDone, [this](Ctx& c, const Event& e) { complete(c, e.a); });
}

Dvd::~Dvd() { closeFile(); }

void Dvd::closeFile() {
    if (openFile_) std::fclose(openFile_);
    openFile_ = nullptr;
    openPath_.clear();
}

// Names become host paths, so anything that could leave the disc directory or name a device is rejected.
static bool validName(const std::string& n) {
    if (n.empty() || n == "." || n == "..") return false;
    for (unsigned char ch : n) if (ch < 0x20 || ch == 0x7F || ch == '/' || ch == '\\' || ch == ':') return false;
    return true;
}

bool Dvd::loadFst(const std::vector<uint8_t>& fst, std::string& err) {
    auto u32 = [&](size_t off) { return uint32_t(fst[off]) << 24 | uint32_t(fst[off + 1]) << 16 | uint32_t(fst[off + 2]) << 8 | fst[off + 3]; };
    if (fst.size() < 12) { err = "FST too small"; return false; }
    const uint32_t count = u32(8);
    if (count == 0 || size_t(count) * 12 > fst.size()) { err = "FST entry count exceeds the table"; return false; }
    const size_t names = size_t(count) * 12;
    files_.clear();
    struct Dir { std::string path; uint32_t end; };
    std::vector<Dir> stack{{"", count}};
    for (uint32_t i = 1; i < count; ++i) {
        while (stack.size() > 1 && i >= stack.back().end) stack.pop_back();
        const size_t e = size_t(i) * 12;
        const bool isDir = fst[e] != 0;
        const uint32_t nameOff = u32(e) & 0x00FFFFFFu;
        if (names + nameOff >= fst.size()) { err = "FST name offset outside the table"; return false; }
        std::string name;
        size_t k = names + nameOff;
        for (; k < fst.size() && fst[k]; ++k) name.push_back(static_cast<char>(fst[k]));
        if (k >= fst.size()) { err = "FST name is not terminated inside the table"; return false; }
        if (!validName(name)) { err = "FST entry " + std::to_string(i) + " has an unsafe name (empty, '.', '..', or containing a slash, backslash, colon or control character)"; return false; }
        const std::string full = stack.back().path.empty() ? name : stack.back().path + "/" + name;
        if (isDir) {
            const uint32_t end = u32(e + 8);
            if (end <= i || end > count || end > stack.back().end) { err = "FST directory " + std::to_string(i) + " has an invalid end index"; return false; }
            stack.push_back({full, end});
        } else {
            const uint32_t off = u32(e + 4), size = u32(e + 8);
            if (uint64_t(off) + size > 0xFFFFFFFFull) { err = "FST file " + std::to_string(i) + " extends past 4 GB"; return false; }
            files_.push_back(DiscFile{off, size, full});
        }
    }
    std::sort(files_.begin(), files_.end(), [](const DiscFile& a, const DiscFile& b) { return a.offset < b.offset; });
    return true;
}

int64_t Dvd::readDisc(uint32_t offset, uint32_t len, uint8_t* dst) {
    std::memset(dst, 0, len);       // gaps between files read as zero
    uint32_t done = 0;
    while (done < len) {
        const uint32_t pos = offset + done;
        auto it = std::upper_bound(files_.begin(), files_.end(), pos, [](uint32_t v, const DiscFile& f) { return v < f.offset; });
        const uint32_t untilNext = it == files_.end() ? len - done : std::min<uint32_t>(len - done, it->offset - pos);
        if (it == files_.begin()) { done += std::max<uint32_t>(untilNext, 1); continue; }          // before the first file
        const DiscFile& f = *(it - 1);
        const uint64_t end = uint64_t(f.offset) + f.size;
        if (pos >= end) { done += std::max<uint32_t>(untilNext, 1); continue; }                    // gap after a file
        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(len - done, end - pos));
        const std::string full = p_.config().discDir + "/files/" + f.path;
        if (full != openPath_) {
            if (openFile_) std::fclose(openFile_);
            openFile_ = std::fopen(full.c_str(), "rb");
            openPath_ = full;
            trace().line("dvd open %s", f.path.c_str());
        }
        if (!openFile_) { lastError_ = "cannot open " + f.path; trace().line("dvd read failed: %s", lastError_.c_str()); return -1; }
        std::fseek(openFile_, static_cast<long>(pos - f.offset), SEEK_SET);
        const size_t got = std::fread(dst + done, 1, chunk, openFile_);
        if (got != chunk) {
            // A file shorter than its FST entry means the extraction is corrupt or incomplete: never pad it with zeros.
            lastError_ = "short read of " + f.path;
            trace().line("dvd read failed: short read of %s (wanted %u at +%u, got %zu)", f.path.c_str(), chunk, pos - f.offset, got);
            return -1;
        }
        done += chunk;
    }
    return len;
}

uint64_t Dvd::latency(uint32_t command, uint32_t len) const {
    // Model: 1 ms command overhead plus 4 ticks per byte (about 10 MB/s). Not the drive's real timing.
    return 40500 + (command == dvdcmd::Read ? uint64_t(len) * 4 : 0);
}

bool Dvd::submit(Ctx&, uint32_t block, uint32_t command, uint32_t prio) {
    GuestMem& m = p_.mem();
    m.w32(block + dvdblock::Command, command);
    m.w32(block + dvdblock::Transferred, 0);
    m.w32(block + dvdblock::State, static_cast<uint32_t>(dvdstate::Waiting));
    queue_.push_back(Request{nextId_++, block, command, prio, nextSeq_++});
    std::stable_sort(queue_.begin(), queue_.end(), [](const Request& a, const Request& b) { return a.prio != b.prio ? a.prio < b.prio : a.seq < b.seq; });
    trace().line("dvd submit cmd=%u block=%08X prio=%u t=%llu", command, block, prio, static_cast<unsigned long long>(p_.now()));
    if (!active_) startNext();
    return true;
}

void Dvd::startNext() {
    if (queue_.empty()) return;
    activeReq_ = queue_.front();
    queue_.pop_front();
    active_ = activeReq_.id;
    GuestMem& m = p_.mem();
    m.w32(activeReq_.block + dvdblock::State, static_cast<uint32_t>(dvdstate::Busy));
    const uint32_t len = m.r32(activeReq_.block + dvdblock::Length);
    p_.postAfter(latency(activeReq_.command, len), Ev::DvdDone, activeReq_.id);
}

void Dvd::complete(Ctx& c, uint64_t id) {
    if (id != active_) return;      // cancelled
    GuestMem& m = p_.mem();
    const Request r = activeReq_;
    const uint32_t cb = m.r32(r.block + dvdblock::Callback);
    int32_t result = 0;
    switch (r.command) {
        case dvdcmd::Read: {
            const uint32_t addr = m.r32(r.block + dvdblock::Addr), len = m.r32(r.block + dvdblock::Length), off = m.r32(r.block + dvdblock::Offset);
            uint8_t* dst = m.ptr(addr, len);
            if (!dst) stopPlatform(c, addr, "DVD read into memory outside MEM1");
            const int64_t got = readDisc(off, len, dst);
            if (got < 0) {
                // The SDK would report a drive error; here it means the extracted disc does not match its FST.
                stopPlatform(c, off, "DVD read failed: a file is missing or shorter than the FST says (corrupt or incomplete disc extraction; see the boot trace)");
            } else {
                result = static_cast<int32_t>(len);
                m.w32(r.block + dvdblock::Transferred, len);
            }
            ++completedReads;
            trace().line("dvd read done off=%08X len=%u addr=%08X result=%d t=%llu", off, len, addr, result, static_cast<unsigned long long>(p_.now()));
            break;
        }
        case dvdcmd::Seek: case dvdcmd::InitStream: case dvdcmd::CancelStream: case dvdcmd::RequestAudioError:
            ++completedOther;
            trace().line("dvd cmd %u done (no data) t=%llu", r.command, static_cast<unsigned long long>(p_.now()));
            break;
        case dvdcmd::Inquiry: {
            // DVDDriveInfo: revision level, device code, release date, padding (32 bytes). Placeholder values
            // (Dolphin question 5 in docs/research/hle-platform.md).
            const uint32_t addr = m.r32(r.block + dvdblock::Addr);
            uint8_t* dst = m.ptr(addr, 32);
            if (!dst) stopPlatform(c, addr, "DVD inquiry buffer outside MEM1");
            std::memset(dst, 0, 32);
            const BootValues& bv = bootValues();
            dst[0] = uint8_t(bv.driveRevision >> 8); dst[1] = uint8_t(bv.driveRevision);
            dst[2] = uint8_t(bv.driveDeviceCode >> 8); dst[3] = uint8_t(bv.driveDeviceCode);
            for (int k = 0; k < 4; ++k) dst[4 + k] = uint8_t(bv.driveReleaseDate >> (24 - 8 * k));
            ++completedOther;
            trace().line("dvd inquiry done addr=%08X t=%llu", addr, static_cast<unsigned long long>(p_.now()));
            break;
        }
        default:
            stopPlatform(c, r.command, "DVD command not modelled");
    }
    m.w32(r.block + dvdblock::State, static_cast<uint32_t>(dvdstate::End));
    active_ = 0;
    if (cb) p_.queueCallback(cb, static_cast<uint32_t>(result), r.block);
    startNext();
}

bool Dvd::cancel(Ctx& c, uint32_t block, uint32_t callback) {
    GuestMem& m = p_.mem();
    const int32_t state = static_cast<int32_t>(m.r32(block + dvdblock::State));
    const uint32_t blockCb = m.r32(block + dvdblock::Callback);
    auto call = [&](uint32_t fn, int32_t r3) {
        if (!fn) return;
        c.r[3] = static_cast<uint32_t>(r3);
        c.r[4] = block;
        rt::callGuest(c, fn);
    };
    if (state == dvdstate::Waiting || state == dvdstate::Busy) {
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Request& r) { return r.block == block; }), queue_.end());
        if (active_ && activeReq_.block == block) {
            p_.events().cancel(Ev::DvdDone, active_);
            active_ = 0;
            startNext();
        }
        m.w32(block + dvdblock::State, static_cast<uint32_t>(dvdstate::Canceled));
        trace().line("dvd cancel block=%08X state=%d", block, state);
        call(blockCb, -3);
        call(callback, 0);
    } else if (state == dvdstate::End || state == dvdstate::Canceled || state == -1) {
        call(callback, 0);
    }
    return true;
}

}  // namespace awl::hle
