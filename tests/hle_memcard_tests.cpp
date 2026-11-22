// Tests of the memory card in slot A: the card's EXI command protocol (memcard.h), the image file handling and the card
// as seen through the EXI registers (dev_exi.cpp). No game files are needed. Expectations come from the public card
// protocol (command bytes, the 7+8+2+7 bit address encoding, 128-byte pages, 8 KiB sectors, ID size codes, erased flash
// reads 0xFF, flash programming only clears bits) and from the SDK code the model serves (docs/research/memory-card.md):
// the status bits it tests, the dummy bytes after a read command, the flash-ID check.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "devices.h"
#include "memcard.h"
#include "platform.h"

namespace awl::rt {
// The runtime references the generated function table; these tests execute no recompiled code.
static void dummyFn(Ctx&) {}
const FnEntry kFunctions[] = {{0x80000000, dummyFn}};
const uint32_t kFunctionCount = 1;
}  // namespace awl::rt

using namespace awl;
using namespace awl::hle;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

template <class T> T id(T v) { return v; }   // keeps constant checks from tripping the constant-condition warning

template <class F>
bool faults(F&& f) {
    try {
        f();
    } catch (const MemcardFault&) {
        return true;
    }
    return false;
}

// ---- helpers: one chip-select cycle on the pure card ------------------------------------------------------------------
void send(Memcard& m, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) m.write(b);
}
std::vector<uint8_t> readN(Memcard& m, uint32_t n) {
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = m.read();
    return v;
}
std::vector<uint8_t> patternImage(uint32_t sizeCode) {
    std::vector<uint8_t> img(static_cast<size_t>(sizeCode) << 17);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>((i * 7 + (i >> 8)) & 0xFF);
    return img;
}
// A fresh scratch folder per test run.
fs::path scratch(const char* name) {
    const fs::path p = fs::temp_directory_path() / "awl-memcard-tests" / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

// ---- sizes and ID ---------------------------------------------------------------------------------------------------
void testSizesAndId() {
    CHECK(Memcard::validSize(512u << 10) && Memcard::validSize(1u << 20) && Memcard::validSize(2u << 20));
    CHECK(Memcard::validSize(4u << 20) && Memcard::validSize(8u << 20) && Memcard::validSize(16u << 20));
    CHECK(!Memcard::validSize(0) && !Memcard::validSize((2u << 20) + 1) && !Memcard::validSize(256u << 10) && !Memcard::validSize(32u << 20));
    CHECK(faults([] { Memcard m(std::vector<uint8_t>(1000)); }));
    // The size code is the card size in Mbit (4 = 59 blocks ... 128 = 2043 blocks); a 2 MiB card is 16 Mbit = 251 user blocks.
    const struct { uint32_t bytes, code; } sizes[] = {{512u << 10, 4}, {1u << 20, 8}, {2u << 20, 0x10}, {4u << 20, 0x20}, {8u << 20, 0x40}, {16u << 20, 0x80}};
    for (const auto& s : sizes) {
        Memcard m(Memcard::blankImage(s.code));
        CHECK(m.image().size() == s.bytes && m.sizeCode() == s.code);
        m.select();
        send(m, {0x00, 0x00});                                   // get ID: command 0x0000, then four bytes
        const auto id = readN(m, 4);
        CHECK(id[0] == 0 && id[1] == 0 && id[2] == 0 && id[3] == s.code);
        CHECK(faults([&] { m.read(); }));                        // a fifth byte is not modelled
        m.deselect();
    }
    CHECK(id(kCardSectorBytes) == 0x2000 && id(kCardPageBytes) == 0x80 && id(kCardReadBytes) == 0x200);
    Memcard blank(Memcard::blankImage(kCardDefaultSizeCode));
    CHECK(blank.image().size() == 2u * 1024 * 1024 && blank.image().size() / kCardSectorBytes == 256);   // 251 user blocks + 5 system blocks
    bool allFF = true;
    for (uint8_t b : blank.image()) allFF = allFF && b == 0xFF;
    CHECK(allFF);
}

// ---- status, interrupt enable --------------------------------------------------------------------------------------
void testStatus() {
    Memcard m(Memcard::blankImage(0x10));
    m.select();
    send(m, {0x83, 0x00});
    const uint8_t st = m.read();
    CHECK(st & Memcard::kStatusUnlocked);                         // the SDK skips the unlock exchange when bit 6 is set
    CHECK((st & (Memcard::kStatusEraseError | Memcard::kStatusProgramError)) == 0);   // the SDK fails with -5 when 0x18 is set
    CHECK(faults([&] { m.read(); }));
    m.deselect();
    m.select(); send(m, {0x89}); m.deselect();                    // clear status: one byte, no data
    CHECK(!m.interruptEnabled());
    m.select(); send(m, {0x81, 0x01}); m.deselect();
    CHECK(m.interruptEnabled());
    m.select(); send(m, {0x81, 0x00}); m.deselect();
    CHECK(!m.interruptEnabled());
    CHECK(faults([&] { m.select(); send(m, {0x81, 0x02}); }));
    // Anything not in the public command set stops: 0x20 (read sector), 0xF4 (not used by the SDK), a stray byte.
    m.deselect();
    CHECK(faults([&] { m.select(); m.write(0x20); }));
    m.deselect();
    CHECK(faults([&] { m.select(); m.write(0xF4); }));
    m.deselect();
    CHECK(faults([&] { m.select(); send(m, {0x89, 0x00}); }));
    m.deselect();
    CHECK(faults([&] { m.write(0x00); }));                        // not selected
    CHECK(faults([&] { m.select(); m.read(); }));                 // read before a command
    m.deselect();
    CHECK(faults([&] { m.select(); send(m, {0x00}); m.deselect(); }));   // command ended early
}

// ---- read array ------------------------------------------------------------------------------------------------------
void testRead() {
    Memcard m(patternImage(0x10));
    const auto& img = m.image();
    // Address 0x1A00 encodes as 00 0D 00 00 (bits 23-17, 16-9, 8-7, 6-0), then 4 dummy bytes, then data.
    m.select();
    send(m, {0x52, 0x00, 0x0D, 0x00, 0x00, 0, 0, 0, 0});
    auto d = readN(m, 512);
    CHECK(std::memcmp(d.data(), img.data() + 0x1A00, 512) == 0);
    CHECK(faults([&] { m.read(); }));                             // beyond one 512-byte page
    m.deselect();
    // Address 0x1F0200 encodes as 0F 81 00 00.
    m.select();
    send(m, {0x52, 0x0F, 0x81, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD});
    d = readN(m, 512);
    CHECK(std::memcmp(d.data(), img.data() + 0x1F0200, 512) == 0);
    m.deselect();
    // The last page of the card.
    m.select();
    send(m, {0x52, 0x0F, 0xFF, 0x00, 0x00, 0, 0, 0, 0});
    d = readN(m, 512);
    CHECK(std::memcmp(d.data(), img.data() + 0x1FFE00, 512) == 0);
    m.deselect();
    // Bits 8-7 of the address live in the third byte: address 0x180 + 0x200 is not 512 aligned and stops, 0x400 is fine.
    m.select();
    send(m, {0x52, 0x00, 0x02, 0x00, 0x00, 0, 0, 0, 0});
    d = readN(m, 512);
    CHECK(std::memcmp(d.data(), img.data() + 0x400, 512) == 0);
    m.deselect();
    CHECK(faults([&] { m.select(); send(m, {0x52, 0x00, 0x02, 0x01, 0x00, 0, 0, 0, 0}); m.read(); }));   // 0x480
    m.deselect();
    CHECK(faults([&] { m.select(); send(m, {0x52, 0x00, 0x02, 0x00, 0x00, 0, 0, 0}); m.read(); }));       // 3 dummy bytes
    m.deselect();
    CHECK(faults([&] { m.select(); send(m, {0x52, 0x00, 0x02, 0x00, 0x00, 0, 0, 0, 0, 0}); m.read(); })); // 5 dummy bytes
    m.deselect();
    CHECK(faults([&] { m.select(); send(m, {0x52, 0x10, 0x00, 0x00, 0x00, 0, 0, 0, 0}); m.read(); }));    // 0x200000: past the end
    m.deselect();
}

// ---- erase and program -----------------------------------------------------------------------------------------------
void testEraseProgram() {
    Memcard m(patternImage(0x10));
    auto& img = m.image();
    // Sector erase of the sector at 0x6000: F1, bits 23-17 = 00, bits 16-9 = 0x30. The sector reads 0xFF afterwards, the rest is untouched.
    const std::vector<uint8_t> before = img;
    m.select();
    send(m, {0xF1, 0x00, 0x30});
    CHECK(!m.busy());                                             // nothing happens until chip select drops
    m.deselect();
    CHECK(m.busy() && m.dirty() && m.erases() == 1);
    CHECK((m.status() & Memcard::kStatusNotBusy) == 0);
    CHECK(m.takeOperationTicks() == kCardEraseTicks && m.takeOperationTicks() == 0);
    bool erased = true, same = true;
    for (size_t i = 0; i < img.size(); ++i) {
        if (i >= 0x6000 && i < 0x8000) erased = erased && img[i] == 0xFF;
        else same = same && img[i] == before[i];
    }
    CHECK(erased && same);
    CHECK(faults([&] { m.select(); send(m, {0xF1, 0x00, 0x30}); }));   // a new erase while busy
    m.deselect();
    CHECK(!m.finishOperation());                                  // interrupts were never enabled: no EXI interrupt
    CHECK(!m.busy() && (m.status() & Memcard::kStatusNotBusy));
    m.select(); send(m, {0x81, 0x01}); m.deselect();
    // Page program at 0x6080 (a 128-byte page): F2, 00 30 01 00 (bit 7 set in the third byte), 128 data bytes.
    m.select();
    send(m, {0xF2, 0x00, 0x30, 0x01, 0x00});
    for (int i = 0; i < 128; ++i) m.write(static_cast<uint8_t>(i * 3 + 1));
    m.deselect();
    CHECK(m.programs() == 1 && m.busy());
    CHECK(m.takeOperationTicks() == kCardProgramTicks);
    bool ok = true;
    for (int i = 0; i < 128; ++i) ok = ok && img[0x6080 + i] == static_cast<uint8_t>(i * 3 + 1);
    CHECK(ok && img[0x607F] == 0xFF && img[0x6100] == 0xFF);
    CHECK(m.finishOperation());                                   // interrupts enabled: the card raises its EXI interrupt
    // Programming only clears bits: a second program of the same page ANDs with what is there.
    m.select();
    send(m, {0xF2, 0x00, 0x30, 0x01, 0x00});
    for (int i = 0; i < 128; ++i) m.write(0xF0);
    m.deselect();
    m.takeOperationTicks(); m.finishOperation();
    ok = true;
    for (int i = 0; i < 128; ++i) ok = ok && img[0x6080 + i] == (static_cast<uint8_t>(i * 3 + 1) & 0xF0);
    CHECK(ok);
    // Things that stop: unaligned erase, wrong data length, unaligned page, outside the card, read while busy.
    CHECK(faults([&] { m.select(); send(m, {0xF1, 0x00, 0x31}); m.deselect(); }));      // 0x6200
    m.deselect();
    CHECK(faults([&] { m.select(); send(m, {0xF1, 0x10, 0x00}); m.deselect(); }));      // 0x200000
    m.deselect();
    CHECK(faults([&] { m.select(); send(m, {0xF2, 0x00, 0x30, 0x00, 0x00}); for (int i = 0; i < 127; ++i) m.write(0); m.deselect(); }));
    m.deselect();
    CHECK(faults([&] { m.select(); send(m, {0xF2, 0x00, 0x30, 0x00, 0x00}); for (int i = 0; i < 129; ++i) m.write(0); }));
    m.deselect();
    CHECK(faults([&] { m.select(); send(m, {0xF2, 0x00, 0x30, 0x00, 0x01}); for (int i = 0; i < 128; ++i) m.write(0); m.deselect(); }));
    m.deselect();
    m.select(); send(m, {0xF1, 0x00, 0x20}); m.deselect();
    CHECK(faults([&] { m.select(); send(m, {0x52, 0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0}); }));   // read while busy
    m.deselect();
    m.select(); send(m, {0x83, 0x00}); CHECK((m.read() & Memcard::kStatusNotBusy) == 0); m.deselect();   // status is allowed
}

// ---- flash ID ---------------------------------------------------------------------------------------------------------
void testFlashId() {
    // Format time 0: the pseudo-random stream is all zero (0x3039 >> 16 = 0), so the flash ID is the serial itself.
    uint8_t header[0x14] = {};
    for (int i = 0; i < 12; ++i) header[i] = static_cast<uint8_t>(0x10 + i);
    uint8_t id[12];
    memcardFlashId(header, id);
    CHECK(std::memcmp(id, header, 12) == 0);
    // Format time 1: first step (1 * 0x41C64E6D + 0x3039) >> 16 = 0x41C6, so the first stream byte is 0xC6; the others were derived by
    // hand with 64-bit arithmetic: C6 C9 CB 69 14 20 D9 2E 54 C7 C4 E2.
    static const uint8_t r1[12] = {0xC6, 0xC9, 0xCB, 0x69, 0x14, 0x20, 0xD9, 0x2E, 0x54, 0xC7, 0xC4, 0xE2};
    std::memset(header, 0, sizeof header);
    header[0x13] = 0x01;                                          // 64-bit format time = 1 at header + 0x0C, big endian (low byte last)
    for (int i = 0; i < 12; ++i) header[i] = static_cast<uint8_t>(0x80 + r1[i]);   // serial = flash ID (0x80) + stream
    memcardFlashId(header, id);
    bool ok = true;
    for (int i = 0; i < 12; ++i) ok = ok && id[i] == 0x80;
    CHECK(ok);
    // A full 64-bit format time (high word used by the first multiplication).
    static const uint8_t r2[12] = {0xED, 0x09, 0x38, 0xE3, 0xB3, 0x41, 0x53, 0xE7, 0x88, 0xEA, 0x8F, 0x53};
    const uint8_t ft[8] = {0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x90};
    std::memcpy(header + 0x0C, ft, 8);
    for (int i = 0; i < 12; ++i) header[i] = static_cast<uint8_t>(0x55 + r2[i]);
    memcardFlashId(header, id);
    ok = true;
    for (int i = 0; i < 12; ++i) ok = ok && id[i] == 0x55;
    CHECK(ok);
}

// ---- files -----------------------------------------------------------------------------------------------------------
void testFiles() {
    const fs::path dir = scratch("files");
    const std::string path = (dir / "sub" / "card.raw").string();
    std::string err;
    CHECK(memcardPrepare(path, err));                              // creates the folder and a blank 2 MiB card
    CHECK(fs::exists(path) && fs::file_size(path) == 2u * 1024 * 1024);
    std::vector<uint8_t> img;
    CHECK(memcardLoad(path, img, err) && img == Memcard::blankImage(0x10));
    // Round trip: erase one sector and program a page through the card, save, reload, read the page back.
    {
        Memcard m(img);
        m.select(); send(m, {0xF1, 0x00, 0x10}); m.deselect(); m.takeOperationTicks(); m.finishOperation();
        m.select(); send(m, {0xF2, 0x00, 0x10, 0x00, 0x00}); for (int i = 0; i < 128; ++i) m.write(static_cast<uint8_t>(0xA0 ^ i)); m.deselect();
        CHECK(m.dirty());
        CHECK(memcardSave(path, m.image(), err));
        CHECK(!fs::exists(path + ".tmp"));                         // the temporary file was renamed over the target
    }
    std::vector<uint8_t> back;
    CHECK(memcardLoad(path, back, err));
    Memcard m2(back);
    m2.select();
    send(m2, {0x52, 0x00, 0x10, 0x00, 0x00, 0, 0, 0, 0});
    const auto d = readN(m2, 512);
    bool ok = true;
    for (int i = 0; i < 128; ++i) ok = ok && d[i] == static_cast<uint8_t>(0xA0 ^ i);
    for (int i = 128; i < 512; ++i) ok = ok && d[i] == 0xFF;
    CHECK(ok);
    // prepare() never modifies an existing image.
    const auto stamp = fs::last_write_time(path);
    const auto sizeBefore = fs::file_size(path);
    CHECK(memcardPrepare(path, err));
    CHECK(fs::last_write_time(path) == stamp && fs::file_size(path) == sizeBefore);
    // A file that is not a card size is rejected with a message and left alone.
    const std::string bad = (dir / "bad.raw").string();
    { std::ofstream f(bad, std::ios::binary); f << "not a card"; }
    CHECK(!memcardPrepare(bad, err) && err.find("not a raw memory card image") != std::string::npos);
    CHECK(fs::file_size(bad) == 10);
    CHECK(!memcardPrepare(dir.string(), err));                     // a folder is not a card
    // 4 MiB and 512 KiB images load.
    const std::string small = (dir / "small.raw").string();
    CHECK(memcardSave(small, Memcard::blankImage(4), err) && memcardLoad(small, img, err) && img.size() == (512u << 10));
    fs::remove_all(dir);
}

// ---- the card through the EXI registers ------------------------------------------------------------------------------
struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(rt::kMem1Size, 0);
    std::vector<uint8_t> lc = std::vector<uint8_t>(rt::kLockedCacheSize, 0);
    rt::Machine machine;
    std::unique_ptr<Ctx> ctx;
    explicit Rig(const Config& cfg) {
        machine.mem = mem.data();
        machine.lc = lc.data();
        Platform::get().attach(machine, cfg);
        ctx = std::make_unique<Ctx>(machine);
    }
    Platform& p() { return Platform::get(); }
    uint64_t rd(uint32_t addr) { return p().dev(BlkExi).read(*ctx, addr, 4); }
    void wr(uint32_t addr, uint64_t v) { p().dev(BlkExi).write(*ctx, addr, 4, v); }
    void runTo(uint64_t tick) {
        machine.icount = tick * kInstructionsPerTick;
        p().processEvents(*ctx);
    }
};
constexpr uint32_t kCsr0 = 0xCC006800, kMar0 = 0xCC006804, kLen0 = 0xCC006808, kCr0 = 0xCC00680C, kData0 = 0xCC006810;

// EXI helpers as the SDK's EXIImm / EXIDma do them (CR: bit 0 start, bit 1 DMA, bits 2-3 type 0 read / 1 write, bits 4-5 length - 1).
void select0(Rig& r) { r.wr(kCsr0, (r.rd(kCsr0) & 0x405) | 0x80 | (4u << 4)); }   // chip select 0, 16 MHz, interrupt masks kept (EXISelect)
void deselect0(Rig& r) { r.wr(kCsr0, r.rd(kCsr0) & 0x405); }                       // EXIDeselect keeps only the masks
template <class F>
bool stopsIn(Rig&, F&& f) {
    try {
        f();
    } catch (const rt::Stop& s) {
        return s.kind == rt::StopKind::Mmio;
    }
    return false;
}
void immWrite(Rig& r, uint32_t data, uint32_t len) { r.wr(kData0, data); r.wr(kCr0, 1 | (1u << 2) | ((len - 1) << 4)); }
uint32_t immRead(Rig& r, uint32_t len) { r.wr(kCr0, 1 | ((len - 1) << 4)); return static_cast<uint32_t>(r.rd(kData0)); }
void dmaRead(Rig& r, uint32_t addr, uint32_t len) { r.wr(kMar0, addr); r.wr(kLen0, len); r.wr(kCr0, 1 | 2); }
void dmaWrite(Rig& r, uint32_t addr, uint32_t len) { r.wr(kMar0, addr); r.wr(kLen0, len); r.wr(kCr0, 1 | 2 | (1u << 2)); }

void testExiCard() {
    const fs::path dir = scratch("exi");
    const std::string path = (dir / "card.raw").string();
    std::string err;
    // A formatted-looking image: header with serial = flash ID + stream for format time 1, so the flash ID is 0x80 x 12.
    std::vector<uint8_t> img = patternImage(0x10);
    {
        static const uint8_t r1[12] = {0xC6, 0xC9, 0xCB, 0x69, 0x14, 0x20, 0xD9, 0x2E, 0x54, 0xC7, 0xC4, 0xE2};
        std::memset(img.data(), 0, 0x14);
        for (int i = 0; i < 12; ++i) img[i] = static_cast<uint8_t>(0x80 + r1[i]);
        img[0x13] = 1;
    }
    CHECK(memcardSave(path, img, err));
    Config cfg;
    cfg.memcardPath = path;
    {
        Rig r(cfg);
        CHECK((r.rd(kCsr0) & 0x1000) != 0);                        // EXT: a card is in slot A
        CHECK((r.rd(0xCC006814) & 0x1000) == 0 && (r.rd(0xCC006828) & 0x1000) == 0);   // slot B and the serial port have none
        // SRAM: the flash ID of slot A and its checksum (complement of the byte sum) as DoMount re-checks them.
        r.wr(kCsr0, 0x100);                                        // chip select 1: RTC and SRAM
        r.wr(kData0, 0x20000100); r.wr(kCr0, 0x35);
        r.wr(kMar0, 0x00200000); r.wr(kLen0, 64); r.wr(kCr0, 0x03);
        const uint8_t* s = r.mem.data() + 0x200000;
        bool ok = true;
        for (int i = 0; i < 12; ++i) ok = ok && s[20 + i] == 0x80;
        CHECK(ok && s[58] == static_cast<uint8_t>(~(12 * 0x80)));
        // Card: ID and status through immediate transfers, as EXIGetID and __CARDReadStatus do.
        select0(r);
        immWrite(r, 0x00000000, 2);
        CHECK(immRead(r, 4) == 0x10);
        deselect0(r);
        select0(r);
        immWrite(r, 0x83000000, 2);
        CHECK((immRead(r, 1) >> 24) == 0xC1);
        deselect0(r);
        // Read array with the 4 dummy bytes and a 512-byte DMA read into guest memory.
        select0(r);
        immWrite(r, 0x52000D00, 4); immWrite(r, 0x00000000, 1);   // 52 00 0D 00 | 00
        immWrite(r, 0, 4);
        dmaRead(r, 0x00300000, 512);
        CHECK(std::memcmp(r.mem.data() + 0x300000, img.data() + 0x1A00, 512) == 0);
        CHECK((r.rd(kCsr0) & 8) != 0);                             // transfer-complete status
        deselect0(r);
        // Erase: interrupts enabled first (0x81 0x01), then F1 00 30; the card is busy for the model's erase time, then raises EXIINT.
        select0(r); immWrite(r, 0x81010000, 2); deselect0(r);
        r.wr(kCsr0, r.rd(kCsr0) | 1);                              // EXIINT mask
        select0(r); immWrite(r, 0xF1003000, 3); deselect0(r);
        CHECK((r.rd(kCsr0) & 2) == 0);
        const uint64_t t0 = r.p().now();
        r.runTo(t0 + kCardEraseTicks - 1);
        CHECK((r.rd(kCsr0) & 2) == 0 && (r.p().piStatus() & pibit::Exi) == 0);
        r.runTo(t0 + kCardEraseTicks);
        CHECK((r.rd(kCsr0) & 2) != 0);                             // EXIINT set by the card
        CHECK((r.p().piStatus() & pibit::Exi) != 0);               // and the interrupt reaches the PI
        r.wr(kCsr0, r.rd(kCsr0) | 2);                              // acknowledge (write 1 clears)
        CHECK((r.rd(kCsr0) & 2) == 0 && (r.p().piStatus() & pibit::Exi) == 0);
        // Program a page from guest memory by DMA: F2 00 30 01 00 then 128 bytes.
        for (int i = 0; i < 128; ++i) r.mem[0x310000 + i] = static_cast<uint8_t>(i ^ 0x5A);
        select0(r); immWrite(r, 0xF2003001, 4); immWrite(r, 0x00000000, 1);
        dmaWrite(r, 0x00310000, 128);
        deselect0(r);
        const uint64_t t1 = r.p().now();
        r.runTo(t1 + kCardProgramTicks);
        CHECK((r.rd(kCsr0) & 2) != 0);
        r.wr(kCsr0, r.rd(kCsr0) | 2);
        // Interrupts disabled (0x81 0x00): an erase finishes without EXIINT.
        select0(r); immWrite(r, 0x81000000, 2); deselect0(r);
        select0(r); immWrite(r, 0xF1002000, 3); deselect0(r);
        r.runTo(r.p().now() + kCardEraseTicks);
        CHECK((r.rd(kCsr0) & 2) == 0);
        // Unmodelled: an unknown command, a read-write transfer and a read before a command stop loudly with the guest pc.
        select0(r);
        CHECK(stopsIn(r, [&] { immWrite(r, 0x20000000, 1); }));
        deselect0(r);
        select0(r);
        CHECK(stopsIn(r, [&] { r.wr(kCr0, 1 | (2u << 2)); }));
        deselect0(r);
        // The image file is written 1 s of game time after the last change, not before.
        std::vector<uint8_t> onDisk;
        CHECK(memcardLoad(path, onDisk, err) && onDisk == img);
        r.runTo(r.p().now() + kCardFlushDelayTicks);
        CHECK(memcardLoad(path, onDisk, err) && onDisk != img);
        bool pageOk = true;
        for (int i = 0; i < 128; ++i) pageOk = pageOk && onDisk[0x6080 + i] == static_cast<uint8_t>(i ^ 0x5A);
        CHECK(pageOk);
        CHECK(onDisk[0x4000] == 0xFF && onDisk[0x5FFF] == 0xFF);   // the sector erased last (0x4000-0x5FFF)
        CHECK(onDisk[0x8000] == img[0x8000]);
    }
    fs::remove_all(dir);
}

// ---- no card: nothing changes ---------------------------------------------------------------------------------------
void testNoCard() {
    Rig r{Config()};
    CHECK((r.rd(kCsr0) & 0x1000) == 0);
    select0(r);
    immWrite(r, 0x00000000, 2);
    CHECK(immRead(r, 4) == 0);                                     // absent device reads zero
    deselect0(r);
    // The SRAM flash ID stays as the unmodified default (zeros), so a run without a card is unchanged.
    r.wr(kCsr0, 0x100);
    r.wr(kData0, 0x20000100); r.wr(kCr0, 0x35);
    r.wr(kMar0, 0x00200000); r.wr(kLen0, 64); r.wr(kCr0, 0x03);
    bool zero = true;
    for (int i = 20; i < 64; ++i) zero = zero && r.mem[0x200000 + i] == 0;
    CHECK(zero);
    // A blank image gets the fixed blank flash ID.
    const fs::path dir = scratch("blank");
    Config cfg;
    cfg.memcardPath = (dir / "new.raw").string();
    Rig r2(cfg);
    CHECK(fs::exists(cfg.memcardPath) && fs::file_size(cfg.memcardPath) == 2u * 1024 * 1024);   // created blank by the device too
    r2.wr(kCsr0, 0x100);
    r2.wr(kData0, 0x20000100); r2.wr(kCr0, 0x35);
    r2.wr(kMar0, 0x00200000); r2.wr(kLen0, 64); r2.wr(kCr0, 0x03);
    bool ok = true;
    uint32_t sum = 0;
    for (int i = 0; i < 12; ++i) { ok = ok && r2.mem[0x200000 + 20 + i] == kCardBlankFlashId[i]; sum += kCardBlankFlashId[i]; }
    CHECK(ok && r2.mem[0x200000 + 58] == static_cast<uint8_t>(~sum));
    Rig r3{Config()};   // detach the card device before the folder goes away
    fs::remove_all(dir);
}

}  // namespace

int main() try {
    const struct { const char* name; void (*fn)(); } tests[] = {
        {"sizes and ID", testSizesAndId}, {"status and interrupt enable", testStatus}, {"read array", testRead}, {"erase and program", testEraseProgram},
        {"flash ID", testFlashId}, {"files", testFiles}, {"card through EXI", testExiCard}, {"no card", testNoCard},
    };
    for (const auto& t : tests) {
        std::printf("[%s]\n", t.name);
        std::fflush(stdout);
        t.fn();
    }
    std::printf("memory card tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
catch (const rt::Stop& s) {
    std::fprintf(stderr, "uncaught Stop: kind %u pc 0x%08X addr 0x%08X: %s\n", static_cast<unsigned>(s.kind), s.pc, s.addr, s.what);
    return 2;
}
catch (const MemcardFault& f) {
    std::fprintf(stderr, "uncaught MemcardFault: %s\n", f.what.c_str());
    return 2;
}
