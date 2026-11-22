// GameCube memory card in slot A: the raw flash image, its file, and the card's EXI command protocol. The protocol is
// the public one (command bytes, address encoding, page and sector sizes); every encoding below was also checked against
// what the recompiled CARD library sends (docs/research/memory-card.md). Pure data and byte streams: no Platform, so the
// tests need no game files.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace awl::hle {

// ---- Geometry (public card format) ----------------------------------------------------------------------------------
constexpr uint32_t kCardSectorBytes = 0x2000;      // erase unit, also called a block
constexpr uint32_t kCardPageBytes = 0x80;          // program unit (one 0xF2 command)
constexpr uint32_t kCardReadBytes = 0x200;         // read-array unit (the SDK reads 512 bytes per command)
constexpr uint32_t kCardDefaultSizeCode = 0x10;    // 16 Mbit = 2 MiB = 251 user blocks (+ 5 system blocks): the card created when none exists
constexpr uint32_t kCardLatencyBytes = 4;          // dummy bytes between a read command and its data: the SDK sends LatencyTable[(id >> 8) & 7] = 4 for these IDs

// Model parameters (not measured on hardware). The SDK enforces its own timeouts: 2 s per 8 KiB erase and 100 ms per
// program (alarm set in the SDK's command start, 0x801C4E3C), so anything below them is accepted; these values are of the
// order a serial flash needs and make a three-block save last about 0.2 s of game time.
constexpr uint64_t kCardEraseTicks = 405000;       // 10 ms at the 40.5 MHz time base, per sector erase
constexpr uint64_t kCardProgramTicks = 40500;      // 1 ms, per 128-byte page program
constexpr uint64_t kCardFlushDelayTicks = 40500000;   // the image file is rewritten 1 s of game time after the last erase or program (and at exit)

// Flash ID the card reports for a blank (never formatted) image. The real card has a factory flash ID that the SDK reads
// through its DSP unlock exchange; the emulated card is formatted by the game with whatever the SRAM holds, so any fixed
// value works. The value is arbitrary.
constexpr uint8_t kCardBlankFlashId[12] = {0x41, 0x57, 0x4C, 0x4E, 0x61, 0x74, 0x69, 0x76, 0x65, 0x43, 0x61, 0x72};

// A fault of the guest against what this card model implements; the EXI device turns it into a loud stop.
struct MemcardFault {
    std::string what;
};

class Memcard {
public:
    // Status register bits (command 0x83). The SDK tests only 0x40 (unlocked: when clear it starts the DSP unlock
    // exchange) and 0x18 (erase and program errors); 0x80 and 0x01 follow the public protocol notes and carry no weight.
    static constexpr uint8_t kStatusNotBusy = 0x80, kStatusUnlocked = 0x40, kStatusEraseError = 0x10, kStatusProgramError = 0x08, kStatusReady = 0x01;

    static bool validSize(uint64_t bytes);                               // 512 KiB .. 16 MiB in the public sizes
    static std::vector<uint8_t> blankImage(uint32_t sizeCode);           // all 0xFF: an erased, unformatted card
    explicit Memcard(std::vector<uint8_t> image);                        // throws MemcardFault if the size is not a card size

    uint32_t sizeCode() const { return static_cast<uint32_t>(image_.size() / (1u << 17)); }   // Mbit: the SDK reads size = id & 0xFC
    uint32_t idWord() const { return sizeCode(); }                       // command 0x0000: four bytes, the size code in the low byte
    const std::vector<uint8_t>& image() const { return image_; }
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    // One chip-select cycle: select(), bytes in command order (write() / read()), deselect().
    void select();
    void deselect();                     // an erase or a program takes effect here; throws if the command is incomplete
    void write(uint8_t b);               // byte from the host
    uint8_t read();                      // byte to the host

    // Asynchronous completion of erase and program. takeOperationTicks() returns the delay (ticks) of the operation the last
    // deselect() started, once (0 = none); finishOperation() ends it and says whether the card raises its interrupt.
    uint64_t takeOperationTicks();
    bool finishOperation();
    bool busy() const { return busy_; }
    bool interruptEnabled() const { return intEnabled_; }
    uint8_t status() const { return static_cast<uint8_t>(status_ | (busy_ ? 0 : kStatusNotBusy)); }
    uint64_t erases() const { return erases_; }
    uint64_t programs() const { return programs_; }

private:
    enum : uint8_t { CmdId = 0x00, CmdRead = 0x52, CmdIntEnable = 0x81, CmdStatus = 0x83, CmdClearStatus = 0x89, CmdEraseSector = 0xF1, CmdProgram = 0xF2 };
    uint32_t addressOf() const;          // the byte address in the five-byte read and program commands
    [[noreturn]] void fault(const std::string& what);     // poisons the current chip-select cycle: nothing it did takes effect

    std::vector<uint8_t> image_;
    bool dirty_ = false;
    bool selected_ = false;
    bool faulted_ = false;
    uint8_t cmd_ = 0;
    uint8_t args_[8] = {};               // command bytes after the opcode
    uint32_t nargs_ = 0, need_ = 0;      // received and required argument bytes
    uint32_t dummy_ = 0;                 // bytes written after a read command, before the data
    uint32_t reads_ = 0;                 // bytes read since the command was complete
    uint32_t dataIn_ = 0;                // program data bytes received
    uint8_t page_[kCardPageBytes] = {};
    bool anyCmd_ = false;
    uint8_t status_ = kStatusUnlocked | kStatusReady;   // powers up unlocked: see docs/research/memory-card.md
    bool busy_ = false;
    bool intEnabled_ = false;
    uint64_t pendingTicks_ = 0;
    uint64_t erases_ = 0, programs_ = 0;
};

// The flash ID the SDK will check against the card's header: header serial minus the pseudo-random stream the SDK derives
// from the format time (VerifyID, 0x801C77xx-0x801C79xx). header points at the first 0x14 bytes of the image.
void memcardFlashId(const uint8_t* header, uint8_t out[12]);

// Files. prepare(): a missing file is created blank (2 MiB); an existing one is only validated, never modified. Returns false with a
// message for a path that cannot be used, a size that is not a card size or a read error.
bool memcardPrepare(const std::string& path, std::string& err);
bool memcardLoad(const std::string& path, std::vector<uint8_t>& image, std::string& err);
// Safe write: temporary file next to the target, then rename over it, so a crash never leaves a half-written card.
bool memcardSave(const std::string& path, const std::vector<uint8_t>& image, std::string& err);

}  // namespace awl::hle
