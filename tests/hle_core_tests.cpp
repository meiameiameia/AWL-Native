// Tests for the platform runtime's pure parts: event queue, clock, interrupt controller, VI, EXI with the RTC/SRAM device,
// the GX command parser and the DVD drive. The expectations are derived by hand from public documentation and from the SDK
// code the models serve (docs/research/hle-platform.md): time-base arithmetic, register bit roles read from the retrace ISR,
// SetInterruptMask and the SRAM checksum loop in UnlockSram, the GX command formats, and the DVD command-block layout.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "devices.h"
#include "dvd.h"
#include "platform.h"

namespace awl::rt {
// The runtime references the generated function table; these tests execute no recompiled code.
static void dummyFn(Ctx&) {}
const FnEntry kFunctions[] = {{0x80000000, dummyFn}};
const uint32_t kFunctionCount = 1;
}  // namespace awl::rt

using namespace awl;
using namespace awl::hle;

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

struct Rig {
    std::vector<uint8_t> mem = std::vector<uint8_t>(rt::kMem1Size, 0);
    std::vector<uint8_t> lc = std::vector<uint8_t>(rt::kLockedCacheSize, 0);
    rt::Machine machine;
    std::unique_ptr<Ctx> ctx;
    Config cfg;
    explicit Rig(Config c = Config()) : cfg(c) {
        machine.mem = mem.data();
        machine.lc = lc.data();
        Platform::get().attach(machine, cfg);
        ctx = std::make_unique<Ctx>(machine);
    }
    Platform& p() { return Platform::get(); }
    Device& dev(Block b) { return p().dev(b); }
    uint64_t rd(uint32_t addr, uint32_t size) { return dev(static_cast<Block>(blockOf(addr))).read(*ctx, addr, size); }
    void wr(uint32_t addr, uint32_t size, uint64_t v) { dev(static_cast<Block>(blockOf(addr))).write(*ctx, addr, size, v); }
    // Advances the clock to the given time-base tick and runs every due event.
    void runTo(uint64_t tick) {
        machine.icount = tick * kInstructionsPerTick;
        p().processEvents(*ctx);
    }
};

template <class F>
bool stops(F&& f, rt::StopKind kind) {
    try {
        f();
    } catch (const rt::Stop& s) {
        return s.kind == kind;
    }
    return false;
}

// ---- time ------------------------------------------------------------------------------------------------
template <class T> T id(T v) { return v; }   // keeps constant checks from tripping the constant-condition warning

void testTimeConstants() {
    // Field rate 60000/1001 Hz at a 40.5 MHz time base.
    CHECK(id(kFieldTicks) == 40500000ull * 1001 / 60000);
    CHECK(id(40500000ull * 1001 % 60000) == 0);
    // 858 pixel clocks of 13.5 MHz per line = 3 ticks per clock; 262.5 lines per field.
    CHECK(id(kLineTicks) == 858 * 3);
    CHECK(id(kLineTicks * 525 / 2) == kFieldTicks);
    CHECK(id(kTickRate) == 162000000 / 4);
}

void testEventQueueAndClock() {
    EventQueue q;
    q.post(10, Ev::DvdDone, 1);
    q.post(5, Ev::PeFinish, 2);
    q.post(10, Ev::AiDmaDone, 3);   // same tick as the first: posted later, runs later
    Event e;
    CHECK(!q.popDue(4, e));
    CHECK(q.popDue(5, e) && e.a == 2);
    CHECK(!q.popDue(9, e));
    CHECK(q.popDue(10, e) && e.a == 1);
    CHECK(q.popDue(10, e) && e.a == 3);
    CHECK(q.empty());
    q.post(7, Ev::DvdDone, 9);
    q.post(8, Ev::DvdDone, 10);
    q.cancel(Ev::DvdDone, 9);
    CHECK(q.size() == 1 && q.nextTick() == 8);

    Clock c;
    CHECK(c.now(400) == 100);
    CHECK(c.retiredFor(100) == 400);
    c.skipTo(400, 500);             // idle: jump to tick 500
    CHECK(c.now(400) == 500);
    CHECK(c.now(800) == 600);       // 400 more instructions = 100 ticks
    CHECK(c.retiredFor(600) == 800);
}

// ---- PI ----------------------------------------------------------------------------------------------------
void testPi() {
    Rig r;
    Platform& p = r.p();
    // Nothing pending; bit 16 reads 1 while the reset button is not pressed (OSGetResetButtonState tests it).
    CHECK(r.rd(0xCC003000, 4) == 0x10000);
    r.wr(0xCC003004, 4, 0x100);                                  // unmask VI (SetInterruptMask: VI is mask bit 0x100)
    CHECK(r.rd(0xCC003004, 4) == 0x100);
    CHECK(!p.externalPending());
    p.setLine(pibit::Vi, true);
    CHECK(r.rd(0xCC003000, 4) == (0x10000u | 0x100u));
    CHECK(p.externalPending());
    p.setLine(pibit::Vi, false);
    CHECK(!p.externalPending());
    p.setLine(pibit::Dsp, true);                                 // unmasked bits do not interrupt
    CHECK(!p.externalPending());
    p.setLine(pibit::Dsp, false);
    p.latch(pibit::ResetSwitch);                                 // latched causes clear by writing 1
    CHECK((r.rd(0xCC003000, 4) & 2) == 2);
    r.wr(0xCC003000, 4, 2);
    CHECK((r.rd(0xCC003000, 4) & 2) == 0);
    CHECK(stops([&] { r.rd(0xCC003000, 2); }, rt::StopKind::Mmio));       // 16-bit access to a 32-bit register
    CHECK(stops([&] { r.rd(0xCC003040, 4); }, rt::StopKind::Mmio));       // unmodelled register
    CHECK(stops([&] { r.wr(0xCC003024, 4, 3); }, rt::StopKind::Mmio));    // reset request
}

// ---- VI ----------------------------------------------------------------------------------------------------
void testVi() {
    Rig r;
    Platform& p = r.p();
    // Beam position: vertical count from 1, horizontal count from 1 in 3-tick steps.
    r.runTo(0);
    CHECK(r.rd(0xCC00202C, 2) == 1 && r.rd(0xCC00202E, 2) == 1);
    r.runTo(kLineTicks * 5 + 30);
    CHECK(r.rd(0xCC00202C, 2) == 6 && r.rd(0xCC00202E, 2) == 11);
    r.runTo(kFieldTicks + 2 * kLineTicks);                       // the next field starts again at line 1
    CHECK(r.rd(0xCC00202C, 2) == 3);
    CHECK(stops([&] { r.wr(0xCC00202C, 2, 1); }, rt::StopKind::Mmio));

    // Display interrupt 0 at vertical position 1, horizontal position 1: enable bit 12, status bit 15 of the high half.
    r.wr(0xCC002030, 2, 0x1001);
    r.wr(0xCC002032, 2, 0x0001);
    p.piMask() = pibit::Vi;
    CHECK((r.rd(0xCC002030, 2) & 0x8000) == 0);
    const uint64_t next = (r.p().now() / kFieldTicks + 1) * kFieldTicks;
    r.runTo(next - 1);
    CHECK((r.rd(0xCC002030, 2) & 0x8000) == 0 && !p.externalPending());
    r.runTo(next);
    CHECK((r.rd(0xCC002030, 2) & 0x8000) != 0);
    CHECK(p.externalPending());
    CHECK(p.counters.retraces == 1);
    // The ISR acknowledges by writing the half back with bit 15 cleared.
    r.wr(0xCC002030, 2, r.rd(0xCC002030, 2) & 0x7FFF);
    CHECK((r.rd(0xCC002030, 2) & 0x8000) == 0 && !p.externalPending());
    r.runTo(next + kFieldTicks);                                  // re-arms every field
    CHECK(p.counters.retraces == 2 && p.externalPending());
    // A framebuffer register write that changes the value counts as a latch; rewriting the same value does not.
    r.wr(0xCC00201C, 2, 0x1234);
    r.wr(0xCC00201C, 2, 0x1234);
    CHECK(p.counters.fbLatches == 1);
}

// ---- main-loop criterion ---------------------------------------------------------------------------------------
void testMainLoopCriterion() {
    Config cfg;
    cfg.mainLoopRetraces = 3;
    Rig r(cfg);
    Platform& p = r.p();
    auto latch = [&](uint16_t value) {          // a VIFlush: four framebuffer words written together
        r.runTo(p.now() + 100000);
        for (uint32_t off : {0x201Cu, 0x201Eu, 0x2024u, 0x2026u}) r.wr(0xCC000000u + off, 2, value + off);
    };
    // Retraces with no framebuffer activity never qualify.
    p.onRetrace(); p.onRetrace();
    CHECK(p.counters.consecutive == 0 && p.exitReason().empty());
    // A latch without a preceding EFB copy does not qualify either.
    latch(1);
    CHECK(p.counters.fbLatches == 1);              // four register writes, one latch event
    p.onRetrace(); p.onRetrace();
    CHECK(p.counters.consecutive == 0);
    // A copy, then a latch: the next retraces qualify; the third consecutive one satisfies the criterion.
    ++p.counters.efbCopies; p.counters.copySinceLatch = true;
    latch(2);
    CHECK(p.counters.fbLatches == 2);
    p.onRetrace(); CHECK(p.counters.consecutive == 1);
    p.onRetrace(); CHECK(p.counters.consecutive == 2 && p.exitReason().empty());
    p.onRetrace();
    CHECK(p.exitReason() == "main loop reached");
    // More than three retraces without a new latch break the run (a frozen picture is not a running loop).
    Config longRun;
    longRun.mainLoopRetraces = 10;
    Rig r2(longRun);
    ++r2.p().counters.efbCopies; r2.p().counters.copySinceLatch = true;
    r2.runTo(r2.p().now() + 100000);
    r2.wr(0xCC00201C, 2, 7);
    for (int i = 0; i < 3; ++i) r2.p().onRetrace();
    CHECK(r2.p().counters.consecutive == 3);
    r2.p().onRetrace();
    CHECK(r2.p().counters.consecutive == 0 && r2.p().exitReason().empty());
}

// ---- EXI / RTC / SRAM -------------------------------------------------------------------------------------------
void testExi() {
    Rig r;
    const uint32_t csr0 = 0xCC006800, mar0 = 0xCC006804, len0 = 0xCC006808, cr0 = 0xCC00680C, data0 = 0xCC006810;
    CHECK((r.rd(csr0, 4) & 0x1000) == 0);                         // EXT clear: no card inserted
    // Select chip select 1 (bit 8), write the SRAM read command 0x20000100 (4 bytes), then DMA 64 bytes.
    r.wr(csr0, 4, 0x100);
    r.wr(data0, 4, 0x20000100);
    r.wr(cr0, 4, 0x35);                                           // transfer start, write, 4 bytes
    CHECK((r.rd(cr0, 4) & 1) == 0);                               // immediate transfers complete at once
    CHECK((r.rd(csr0, 4) & 8) != 0);                              // transfer-complete status
    r.wr(mar0, 4, 0x00200000);
    r.wr(len0, 4, 64);
    r.wr(cr0, 4, 0x03);                                           // transfer start, DMA, read
    const uint8_t* s = r.mem.data() + 0x200000;
    // UnlockSram's loop: checksum 1 is the sum of the halfwords at bytes 12-19, checksum 2 the sum of their complements.
    uint16_t sum1 = 0, sum2 = 0;
    for (int i = 12; i < 20; i += 2) {
        const uint16_t h = static_cast<uint16_t>(s[i] << 8 | s[i + 1]);
        sum1 = static_cast<uint16_t>(sum1 + h);
        sum2 = static_cast<uint16_t>(sum2 + static_cast<uint16_t>(~h));
    }
    CHECK((s[0] << 8 | s[1]) == sum1 && (s[2] << 8 | s[3]) == sum2);
    // Writing a byte back through the same command with the write bit set changes the block.
    r.wr(csr0, 4, 0);
    r.wr(csr0, 4, 0x100);
    r.wr(data0, 4, 0xA0000100u + (19u << 6));                     // write SRAM from offset 19
    r.wr(cr0, 4, 0x35);
    r.wr(data0, 4, 0x80000000);                                   // one byte: 0x80 (left-aligned)
    r.wr(cr0, 4, 0x05 | 0x01);                                    // start, write, 1 byte
    r.wr(csr0, 4, 0);
    r.wr(csr0, 4, 0x100);
    r.wr(data0, 4, 0x20000100u + (19u << 6));
    r.wr(cr0, 4, 0x35);
    r.wr(cr0, 4, 0x01);                                           // start, read, 1 byte
    CHECK((r.rd(data0, 4) >> 24) == 0x80);
    // RTC counter: command 0x20000000, then a 4-byte read returns the configured seconds.
    r.wr(csr0, 4, 0);
    r.wr(csr0, 4, 0x100);
    r.wr(data0, 4, 0x20000000);
    r.wr(cr0, 4, 0x35);
    r.wr(cr0, 4, 0x31);
    CHECK(r.rd(data0, 4) == Config().rtcSeconds);
    // An absent device (chip select 0, the memory card slot) reads zero and does not stop.
    r.wr(csr0, 4, 0);
    r.wr(csr0, 4, 0x80);
    r.wr(data0, 4, 0xFFFFFFFF);
    r.wr(cr0, 4, 0x31);
    CHECK(r.rd(data0, 4) == 0);
    CHECK(stops([&] { r.wr(cr0, 4, 0x0B); }, rt::StopKind::Mmio));          // read-write DMA is not modelled
    CHECK(stops([&] { r.rd(0xCC006800 + 3 * 0x14, 4); }, rt::StopKind::Mmio));  // there are three channels
}

// ---- GX ----------------------------------------------------------------------------------------------------------
void writeBytes(Rig& r, const std::vector<uint8_t>& bytes) {
    // Whole 32-byte chunks, written as 4-byte stores like the game's stfs/stw to the write-gather port.
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        const uint32_t v = uint32_t(bytes[i]) << 24 | uint32_t(bytes[i + 1]) << 16 | uint32_t(bytes[i + 2]) << 8 | bytes[i + 3];
        r.wr(0xCC008000, 4, v);
    }
}
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s)); }

void testGx() {
    Rig r;
    Platform& p = r.p();
    p.piFifoBase = 0x00400000;
    p.piFifoEnd = 0x004FFFE0;
    p.piFifoWp = 0x00400000;
    // Vertex descriptor: position direct (bits 9-10 = 1) and colour 0 direct (bits 13-14 = 1).
    // Attribute table 0: position XYZ (bit 0) float (format 4 at bits 1-3), colour RGBA (bit 13) 8888 (format 5 at bits 14-16).
    // Vertex size = 3 floats (12 bytes) + 4 colour bytes = 16.
    std::vector<uint8_t> s;
    s.push_back(0x08); s.push_back(0x50); put32(s, (1u << 9) | (1u << 13));
    s.push_back(0x08); s.push_back(0x70); put32(s, 1u | (4u << 1) | (1u << 13) | (5u << 14));
    s.push_back(0x80); s.push_back(0x00); s.push_back(0x02);     // quads, VAT 0, 2 vertices
    for (int i = 0; i < 32; ++i) s.push_back(static_cast<uint8_t>(0xA0 + i));
    s.push_back(0x61); put32(s, 0x45000002);                       // BP: pixel-engine finish
    s.push_back(0x61); put32(s, 0x48001234);                       // BP: token with interrupt
    s.push_back(0x61); put32(s, 0x47001234);                       // BP: token
    s.push_back(0x61); put32(s, 0x52000000);                       // BP: EFB copy
    while (s.size() % 32) s.push_back(0x00);                       // NOP padding to a whole chunk, as GXFlush does
    writeBytes(r, s);
    // The ring in memory received the first chunk and the write pointer advanced by one chunk per 32 bytes.
    CHECK(std::memcmp(r.mem.data() + 0x400000, s.data(), 32) == 0);
    CHECK(p.piFifoWp == 0x00400000u + static_cast<uint32_t>(s.size()));
    CHECK(r.rd(0xCC000034, 2) == ((0x00400000u + s.size()) & 0xFFFF));
    CHECK(p.counters.efbCopies == 1);
    // Finish and token events complete after a small latency.
    CHECK(r.rd(0xCC00100A, 2) == 0);
    r.runTo(p.now() + 5000);
    CHECK(r.rd(0xCC00100A, 2) == 0xC);                             // token status (bit 2) and finish status (bit 3)
    CHECK(r.rd(0xCC00100E, 2) == 0x1234);
    p.piMask() = pibit::PeFinish | pibit::PeToken;
    CHECK(p.externalPending());
    r.wr(0xCC00100A, 2, 0x8);                                      // the finish handler clears bit 3 by writing 1
    CHECK(r.rd(0xCC00100A, 2) == 0x4);
    r.wr(0xCC00100A, 2, 0x4);
    CHECK(r.rd(0xCC00100A, 2) == 0 && !p.externalPending());
    CHECK(p.counters.finishes == 1 && p.counters.tokens == 1);
    // CP status: read-idle and command-idle, nothing in flight.
    CHECK(r.rd(0xCC000000, 2) == 0x000C);
    CHECK(stops([&] { r.rd(0xCC008000, 4); }, rt::StopKind::Mmio));                          // write-only port
    // An unknown opcode means the stream is out of sync: stop.
    std::vector<uint8_t> bad(32, 0x00);
    bad[0] = 0x07;
    CHECK(stops([&] { writeBytes(r, bad); }, rt::StopKind::Mmio));
}

// ---- DVD ---------------------------------------------------------------------------------------------------------
std::vector<uint8_t> makeFst() {
    // Entries are 12 bytes: flag/name offset, offset or parent, size or next. Names follow the entry table.
    // 0 root (5 entries), 1 dir "d" (its children end before entry 4), 2 "d/a.bin" @0x1000 size 8, 3 "d/b.bin" @0x2000 size 4,
    // 4 "c.bin" in the root @0x3000 size 4.
    const char names[] = "d\0a.bin\0b.bin\0c.bin";
    std::vector<uint8_t> f;
    auto entry = [&](uint8_t flag, uint32_t nameOff, uint32_t a, uint32_t b) {
        f.push_back(flag); f.push_back(uint8_t(nameOff >> 16)); f.push_back(uint8_t(nameOff >> 8)); f.push_back(uint8_t(nameOff));
        put32(f, a); put32(f, b);
    };
    entry(1, 0, 0, 5);
    entry(1, 0, 0, 4);
    entry(0, 2, 0x1000, 8);
    entry(0, 8, 0x2000, 4);
    entry(0, 14, 0x3000, 4);
    f.insert(f.end(), names, names + sizeof names);
    return f;
}

void testDvd() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "awl-hle-test-disc";
    fs::remove_all(root);
    fs::create_directories(root / "files" / "d");
    auto write = [&](const char* name, const std::string& data) { std::ofstream(root / "files" / "d" / name, std::ios::binary) << data; };
    write("a.bin", "ABCDEFGH");
    write("b.bin", "wxyz");
    std::ofstream(root / "files" / "c.bin", std::ios::binary) << "1234";

    Config cfg;
    cfg.discDir = root.string();
    Rig r(cfg);
    Platform& p = r.p();
    Dvd& dvd = p.dvd();
    std::string err;
    CHECK(dvd.loadFst(makeFst(), err));
    CHECK(dvd.files().size() == 3);
    CHECK(dvd.files()[0].path == "d/a.bin" && dvd.files()[0].offset == 0x1000 && dvd.files()[0].size == 8);
    CHECK(dvd.files()[2].path == "c.bin");

    // Reads: inside a file, across a gap (zero filled), across two files.
    uint8_t buf[0x2010] = {};
    CHECK(dvd.readDisc(0x1002, 4, buf) == 4 && std::memcmp(buf, "CDEF", 4) == 0);
    CHECK(dvd.readDisc(0x1006, 0x10, buf) == 0x10 && std::memcmp(buf, "GH", 2) == 0 && buf[2] == 0 && buf[0xF] == 0);
    CHECK(dvd.readDisc(0x2000, 0x1004, buf) == 0x1004 && std::memcmp(buf, "wxyz", 4) == 0 && buf[4] == 0 && std::memcmp(buf + 0x1000, "1234", 4) == 0);

    // Queue: a read (prio 2) is running when a more urgent request (prio 0) and a later one (prio 2) arrive; the urgent one
    // runs next, then the older of the equal priorities.
    GuestMem& m = p.mem();
    const uint32_t b1 = 0x80100000, b2 = 0x80100040, b3 = 0x80100080;
    auto fill = [&](uint32_t b, uint32_t addr, uint32_t len, uint32_t off) {
        m.w32(b + dvdblock::Addr, addr); m.w32(b + dvdblock::Length, len); m.w32(b + dvdblock::Offset, off); m.w32(b + dvdblock::Callback, 0);
    };
    fill(b1, 0x80200000, 4, 0x1000);
    fill(b2, 0x80200010, 4, 0x2000);
    fill(b3, 0x80200020, 4, 0x3000);
    dvd.submit(*r.ctx, b1, dvdcmd::Read, 2);
    dvd.submit(*r.ctx, b3, dvdcmd::Read, 2);
    dvd.submit(*r.ctx, b2, dvdcmd::Read, 0);
    CHECK(int32_t(m.r32(b1 + dvdblock::State)) == dvdstate::Busy);
    CHECK(int32_t(m.r32(b2 + dvdblock::State)) == dvdstate::Waiting && int32_t(m.r32(b3 + dvdblock::State)) == dvdstate::Waiting);
    CHECK(dvd.busy());
    // Each request takes 40,500 ticks plus 4 per byte.
    r.runTo(p.now() + 40500 + 16);
    CHECK(int32_t(m.r32(b1 + dvdblock::State)) == dvdstate::End && m.r32(b1 + dvdblock::Transferred) == 4);
    CHECK(std::memcmp(r.mem.data() + 0x200000, "ABCD", 4) == 0);
    CHECK(int32_t(m.r32(b2 + dvdblock::State)) == dvdstate::Busy);       // the urgent request starts before the older one
    r.runTo(p.now() + 40500 + 16);
    CHECK(int32_t(m.r32(b2 + dvdblock::State)) == dvdstate::End && std::memcmp(r.mem.data() + 0x200010, "wxyz", 4) == 0);
    CHECK(int32_t(m.r32(b3 + dvdblock::State)) == dvdstate::Busy);
    // Cancelling a request that is running ends it as cancelled and starts nothing else.
    dvd.cancel(*r.ctx, b3, 0);
    CHECK(int32_t(m.r32(b3 + dvdblock::State)) == dvdstate::Canceled);
    r.runTo(p.now() + 100000);
    CHECK(!dvd.busy() && std::memcmp(r.mem.data() + 0x200020, "\0\0\0\0", 4) == 0);

    CHECK(!dvd.loadFst(std::vector<uint8_t>(8, 0), err));
    dvd.closeFile();
    fs::remove_all(root);
}

struct FstEntry { uint8_t flag; std::string name; uint32_t a, b; };
// Builds an FST table from entries (entry 0 is the root: its b is the entry count).
std::vector<uint8_t> fstOf(const std::vector<FstEntry>& es) {
    std::vector<uint8_t> f, names;
    for (const auto& e : es) {
        const uint32_t off = static_cast<uint32_t>(names.size());
        names.insert(names.end(), e.name.begin(), e.name.end());
        names.push_back(0);
        f.push_back(e.flag); f.push_back(uint8_t(off >> 16)); f.push_back(uint8_t(off >> 8)); f.push_back(uint8_t(off));
        put32(f, e.a); put32(f, e.b);
    }
    f.insert(f.end(), names.begin(), names.end());
    return f;
}

void testDvdFstValidation() {
    Rig r;
    Dvd& dvd = r.p().dvd();
    std::string err;
    const FstEntry root{1, "", 0, 3};
    // A well-formed table passes; each defect below must be rejected.
    CHECK(dvd.loadFst(fstOf({root, {1, "d", 0, 3}, {0, "f.bin", 0x1000, 4}}), err));
    CHECK(!dvd.loadFst(fstOf({root, {0, "..", 0x1000, 4}, {0, "g", 0x2000, 4}}), err));              // parent reference
    CHECK(!dvd.loadFst(fstOf({root, {0, ".", 0x1000, 4}, {0, "g", 0x2000, 4}}), err));
    CHECK(!dvd.loadFst(fstOf({root, {0, "", 0x1000, 4}, {0, "g", 0x2000, 4}}), err));                // empty name
    CHECK(!dvd.loadFst(fstOf({root, {0, "a/b", 0x1000, 4}, {0, "g", 0x2000, 4}}), err));              // separator
    CHECK(!dvd.loadFst(fstOf({root, {0, std::string("a") + char(92) + "b", 0x1000, 4}, {0, "g", 0x2000, 4}}), err));
    CHECK(!dvd.loadFst(fstOf({root, {0, "C:x", 0x1000, 4}, {0, "g", 0x2000, 4}}), err));              // drive / stream syntax
    CHECK(!dvd.loadFst(fstOf({root, {0, std::string("a") + char(7), 0x1000, 4}, {0, "g", 0x2000, 4}}), err));   // control character
    CHECK(!dvd.loadFst(fstOf({root, {1, "d", 0, 1}, {0, "g", 0x2000, 4}}), err));                    // directory end must exceed its own index
    CHECK(dvd.loadFst(fstOf({root, {1, "d", 0, 2}, {0, "g", 0x2000, 4}}), err));          // end == next entry is a valid empty directory
    CHECK(!dvd.loadFst(fstOf({root, {1, "d", 0, 9}, {0, "g", 0x2000, 4}}), err));                    // end beyond the entry count
    CHECK(!dvd.loadFst(fstOf({{1, "", 0, 4}, {1, "d", 0, 3}, {1, "e", 0, 4}, {0, "g", 0x2000, 4}}), err));   // child directory outlives its parent
    CHECK(!dvd.loadFst(fstOf({root, {0, "f", 0xFFFFFFF0u, 0x20}, {0, "g", 0x2000, 4}}), err));       // file past 4 GB
    std::vector<uint8_t> unterminated = fstOf({root, {0, "abc", 0x1000, 4}, {0, "g", 0x2000, 4}});
    unterminated.pop_back();                                                                             // drops the last name's NUL
    CHECK(!dvd.loadFst(unterminated, err));
    CHECK(!err.empty());
}

void testDvdCorruptExtraction() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "awl-hle-test-disc2";
    fs::remove_all(root);
    fs::create_directories(root / "files");
    std::ofstream(root / "files" / "short.bin", std::ios::binary) << "AB";      // the FST says 8 bytes
    std::ofstream(root / "files" / "ok.bin", std::ios::binary) << "WXYZ";
    Config cfg;
    cfg.discDir = root.string();
    Rig r(cfg);
    Dvd& dvd = r.p().dvd();
    std::string err;
    CHECK(dvd.loadFst(fstOf({{1, "", 0, 4}, {0, "short.bin", 0x1000, 8}, {0, "missing.bin", 0x2000, 4}, {0, "ok.bin", 0x3000, 4}}), err));
    uint8_t buf[16] = {};
    CHECK(dvd.readDisc(0x3000, 4, buf) == 4 && std::memcmp(buf, "WXYZ", 4) == 0);
    CHECK(dvd.readDisc(0x1000, 8, buf) == -1 && dvd.lastError().find("short read") != std::string::npos);   // never zero-padded
    CHECK(dvd.readDisc(0x2000, 4, buf) == -1 && dvd.lastError().find("cannot open") != std::string::npos);
    // A queued read of the short file stops the run loudly when it completes.
    GuestMem& m = r.p().mem();
    const uint32_t b = 0x80100000;
    m.w32(b + dvdblock::Addr, 0x80200000); m.w32(b + dvdblock::Length, 8); m.w32(b + dvdblock::Offset, 0x1000); m.w32(b + dvdblock::Callback, 0);
    dvd.submit(*r.ctx, b, dvdcmd::Read, 2);
    CHECK(stops([&] { r.runTo(r.p().now() + 100000); }, rt::StopKind::Platform));
    dvd.closeFile();
    fs::remove_all(root);
}

}  // namespace

int main() try {
    const struct { const char* name; void (*fn)(); } tests[] = {
        {"time constants", testTimeConstants}, {"events and clock", testEventQueueAndClock}, {"PI", testPi}, {"VI", testVi},
        {"main-loop criterion", testMainLoopCriterion}, {"EXI", testExi}, {"GX", testGx}, {"DVD", testDvd}, {"DVD FST validation", testDvdFstValidation},
        {"DVD corrupt extraction", testDvdCorruptExtraction},
    };
    for (const auto& t : tests) {
        std::printf("[%s]\n", t.name);
        std::fflush(stdout);
        t.fn();
    }
    std::printf("hle core tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
catch (const rt::Stop& s) {
    std::fprintf(stderr, "uncaught Stop: kind %u pc 0x%08X addr 0x%08X: %s\n", static_cast<unsigned>(s.kind), s.pc, s.addr, s.what);
    return 2;
}
