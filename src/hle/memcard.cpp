#include "memcard.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace awl::hle {

bool Memcard::validSize(uint64_t bytes) {
    for (uint32_t code = 4; code <= 0x80; code <<= 1)
        if (bytes == static_cast<uint64_t>(code) << 17) return true;
    return false;
}

std::vector<uint8_t> Memcard::blankImage(uint32_t sizeCode) { return std::vector<uint8_t>(static_cast<size_t>(sizeCode) << 17, 0xFF); }

Memcard::Memcard(std::vector<uint8_t> image) : image_(std::move(image)) {
    if (!validSize(image_.size())) fault("card image size is not a memory card size");
}

void Memcard::fault(const std::string& what) {
    faulted_ = true;
    throw MemcardFault{what};
}

void Memcard::select() {
    selected_ = true;
    faulted_ = false;
    anyCmd_ = false;
    cmd_ = 0;
    nargs_ = need_ = dummy_ = reads_ = dataIn_ = 0;
}

uint32_t Memcard::addressOf() const {
    // Five-byte commands carry the byte address as 7 + 8 + 2 + 7 bits: bits 23-17, 16-9, 8-7 and 6-0 (the SDK builds them
    // with rotates at 0x801C5358 and 0x801C54A0).
    return (static_cast<uint32_t>(args_[0] & 0x7F) << 17) | (static_cast<uint32_t>(args_[1]) << 9) | (static_cast<uint32_t>(args_[2] & 3) << 7) | (args_[3] & 0x7Fu);
}

void Memcard::write(uint8_t b) {
    if (!selected_) fault("card byte written while not selected");
    if (faulted_) fault("card byte written after a fault");
    if (!anyCmd_) {
        anyCmd_ = true;
        cmd_ = b;
        switch (b) {
            case CmdId: case CmdIntEnable: case CmdStatus: need_ = 1; break;
            case CmdClearStatus: need_ = 0; break;
            case CmdRead: case CmdProgram: need_ = 4; break;
            case CmdEraseSector: need_ = 2; break;
            default: fault("memory card command " + std::to_string(b) + " is not modelled");
        }
        if ((b == CmdRead || b == CmdProgram || b == CmdEraseSector) && busy_) fault("memory card command sent while an erase or program is in progress");
        if (b == CmdClearStatus) status_ = static_cast<uint8_t>(status_ & ~(kStatusEraseError | kStatusProgramError));
        return;
    }
    if (nargs_ < need_) {
        args_[nargs_++] = b;
        if (nargs_ == need_) {
            if (cmd_ == CmdId && args_[0] != 0) fault("memory card ID command has a non-zero second byte");
            if (cmd_ == CmdStatus && args_[0] != 0) fault("memory card status command has a non-zero second byte");
            if (cmd_ == CmdIntEnable) {
                if (args_[0] > 1) fault("memory card interrupt-enable value is not 0 or 1");
                intEnabled_ = args_[0] != 0;
            }
        }
        return;
    }
    // Bytes after the command.
    switch (cmd_) {
        case CmdRead:
            if (reads_ != 0) fault("write to the card after read data started");
            ++dummy_;
            return;
        case CmdProgram:
            if (dataIn_ >= kCardPageBytes) fault("program data beyond the 128-byte page buffer");
            page_[dataIn_++] = b;
            return;
        default: fault("extra byte after a memory card command");
    }
}

uint8_t Memcard::read() {
    if (faulted_) fault("card read after a fault");
    if (!selected_ || !anyCmd_ || nargs_ < need_) fault("card read before a complete command");
    switch (cmd_) {
        case CmdId:
            if (reads_ >= 4) fault("memory card ID read beyond 4 bytes");
            return static_cast<uint8_t>(idWord() >> (24 - 8 * reads_++));
        case CmdStatus:
            if (reads_ >= 1) fault("memory card status read beyond 1 byte");
            ++reads_;
            return status();
        case CmdRead: {
            if (reads_ == 0 && dummy_ != kCardLatencyBytes) fault("read command followed by " + std::to_string(dummy_) + " dummy bytes, expected 4");
            const uint32_t base = addressOf();
            if (base & (kCardReadBytes - 1)) fault("read address is not 512-byte aligned");
            if (reads_ >= kCardReadBytes) fault("read beyond one 512-byte page");
            if (static_cast<uint64_t>(base) + kCardReadBytes > image_.size()) fault("read address outside the card");
            if (busy_) fault("read while an erase or program is in progress");
            return image_[base + reads_++];
        }
        default: fault("this memory card command has no read data");
    }
}

void Memcard::deselect() {
    if (!selected_) return;
    selected_ = false;
    if (!anyCmd_ || faulted_) return;
    if (nargs_ < need_) fault("memory card command ended before all its bytes were sent");
    if (cmd_ == CmdEraseSector) {
        const uint32_t addr = (static_cast<uint32_t>(args_[0] & 0x7F) << 17) | (static_cast<uint32_t>(args_[1]) << 9);
        if (addr % kCardSectorBytes) fault("sector erase address is not sector aligned");
        if (static_cast<uint64_t>(addr) + kCardSectorBytes > image_.size()) fault("sector erase address outside the card");
        std::memset(image_.data() + addr, 0xFF, kCardSectorBytes);
        dirty_ = true;
        ++erases_;
        busy_ = true;
        pendingTicks_ = kCardEraseTicks;
    } else if (cmd_ == CmdProgram) {
        const uint32_t addr = addressOf();
        if (dataIn_ != kCardPageBytes) fault("page program with " + std::to_string(dataIn_) + " data bytes, expected 128");
        if (addr % kCardPageBytes) fault("page program address is not 128-byte aligned");
        if (static_cast<uint64_t>(addr) + kCardPageBytes > image_.size()) fault("page program address outside the card");
        // Flash programming can only clear bits: the page ends up as old AND new.
        for (uint32_t i = 0; i < kCardPageBytes; ++i) image_[addr + i] = static_cast<uint8_t>(image_[addr + i] & page_[i]);
        dirty_ = true;
        ++programs_;
        busy_ = true;
        pendingTicks_ = kCardProgramTicks;
    }
}

uint64_t Memcard::takeOperationTicks() {
    const uint64_t t = pendingTicks_;
    pendingTicks_ = 0;
    return t;
}

bool Memcard::finishOperation() {
    busy_ = false;
    return intEnabled_;
}

// ---- Flash ID --------------------------------------------------------------------------------------------------------
// The SDK's check (VerifyID): for i in 0..11, with the 64-bit state starting as the format time stored at header + 0x0C:
//   t = (state * 0x41C64E6D + 0x3039) >> 16;  expect serial[i] == (uint8)(flashId[i] + (uint8)t);
//   state = ((t * 0x41C64E6D + 0x3039) >> 16) & 0x7FFF.
// The flash ID that satisfies it is therefore serial[i] - (uint8)t.
void memcardFlashId(const uint8_t* header, uint8_t out[12]) {
    uint64_t state = 0;
    for (int i = 0; i < 8; ++i) state = state << 8 | header[0x0C + i];
    constexpr uint64_t a = 0x41C64E6Dull, c = 0x3039ull;
    for (int i = 0; i < 12; ++i) {
        const uint64_t t = (state * a + c) >> 16;
        out[i] = static_cast<uint8_t>(header[i] - static_cast<uint8_t>(t));
        state = (((t * a) + c) >> 16) & 0x7FFF;
    }
}

// ---- Files -----------------------------------------------------------------------------------------------------------
bool memcardLoad(const std::string& path, std::vector<uint8_t>& image, std::string& err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { err = "cannot open " + path; return false; }
    const std::streamoff n = f.tellg();
    if (n < 0 || !Memcard::validSize(static_cast<uint64_t>(n))) {
        err = path + " is not a raw memory card image (its size, " + std::to_string(static_cast<long long>(n)) + " bytes, is not 512 KiB, 1, 2, 4, 8 or 16 MiB)";
        return false;
    }
    image.resize(static_cast<size_t>(n));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(image.data()), n)) { err = "cannot read " + path; return false; }
    return true;
}

bool memcardSave(const std::string& path, const std::vector<uint8_t>& image, std::string& err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot write " + tmp; return false; }
        f.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        f.flush();
        if (!f) { err = "short write to " + tmp; return false; }
    }
    fs::rename(tmp, target, ec);
    if (ec) { err = "cannot replace " + path + ": " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

bool memcardPrepare(const std::string& path, std::string& err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(path, ec)) {
        if (!fs::is_regular_file(path, ec)) { err = path + " is not a file"; return false; }
        std::vector<uint8_t> probe;
        return memcardLoad(path, probe, err);     // validated, never modified
    }
    return memcardSave(path, Memcard::blankImage(kCardDefaultSizeCode), err);
}

}  // namespace awl::hle
