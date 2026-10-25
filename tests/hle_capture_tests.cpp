// Tests for the Dolphin capture parsers and the comparison report (src/hle/capture_parse, capture_compare). The Dolphin log layout
// is not verified, so the parser is tested against several plausible variants. Every input is synthetic (invented numbers and
// addresses chosen here); expected values are worked out by hand from those inputs, e.g. 33 ms steps against a 16.6833 ms field.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "boot_table.h"
#include "capture_compare.h"
#include "capture_parse.h"
#include "guest.h"

namespace awl::rt {
// awl_hle references the generated function table; these tests execute no recompiled code.
static void dummyFn(Ctx&) {}
const FnEntry kFunctions[] = {{0x80000000, dummyFn}};
const uint32_t kFunctionCount = 1;
}  // namespace awl::rt

using namespace awl::hle;
using namespace awl::hle::capture;
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

bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

#define CHECK_HAS(report, text)                                                                       \
    do {                                                                                              \
        ++g_checks;                                                                                   \
        if (!has((report), (text))) {                                                                 \
            ++g_failures;                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: report lacks \"%s\"\n", __FILE__, __LINE__, text);      \
        }                                                                                             \
    } while (0)

#define CHECK_LACKS(report, text)                                                                     \
    do {                                                                                              \
        ++g_checks;                                                                                   \
        if (has((report), (text))) {                                                                  \
            ++g_failures;                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: report contains \"%s\"\n", __FILE__, __LINE__, text);   \
        }                                                                                             \
    } while (0)

void testBpLineVariants() {
    BpHit h;
    std::string why;
    // Layout assumed from the capture procedure: time stamp, source location, BP address symbol(arguments) LR.
    CHECK(parseBpLine("12:34:567 PowerPC\\Breakpoints.cpp:60 N[MEMMAP]: BP 801b0de0 OSInit(80000000 00000001 00000002 00000003 00000004 00000005 00000006 00000007) LR=80003178", h, why) == 1);
    CHECK(h.addr == 0x801B0DE0 && h.symbol == "OSInit" && h.args.size() == 8 && h.args[0] == 0x80000000 && h.args[1] == 1 && h.args[7] == 7);
    CHECK(h.hasLr && h.lr == 0x80003178);
    CHECK(h.hasTime && h.seconds > 754.566 && h.seconds < 754.568);             // 12 min 34.567 s

    // Bracketed seconds, 0x prefixes, rN= argument labels.
    CHECK(parseBpLine("[12.345] BP 0x801B0DE0 (r3=80000000 r4=0x00000001) LR=0x80003178", h, why) == 1);
    CHECK(h.addr == 0x801B0DE0 && h.args.size() == 2 && h.args[1] == 1 && h.lr == 0x80003178 && h.symbol.empty());
    CHECK(h.hasTime && h.seconds > 12.344 && h.seconds < 12.346);

    // HH:MM:SS.fff, comma-separated arguments, LR with a colon.
    CHECK(parseBpLine("00:00:12.345 BP 801B0DE0 OSInit (80000000, 1, 0) LR: 80003178", h, why) == 1);
    CHECK(h.args.size() == 3 && h.args[0] == 0x80000000 && h.args[2] == 0 && h.hasLr && h.lr == 0x80003178);
    CHECK(h.hasTime && h.seconds > 12.344 && h.seconds < 12.346);

    // No parentheses: loose labelled registers.
    CHECK(parseBpLine("BP 801B0DE0 r3=80000000 r4=00000001 LR=80003178", h, why) == 1);
    CHECK(h.args.size() == 2 && h.args[1] == 1 && h.lr == 0x80003178 && !h.hasTime);

    // HH:MM:SS:mmm.
    CHECK(parseBpLine("01:02:03:004 BP 801B0DE0 (0 1) LR=80003178", h, why) == 1);
    CHECK(h.hasTime && h.seconds > 3723.003 && h.seconds < 3723.005);            // 1 h 2 min 3.004 s

    // Neither time stamp nor LR.
    CHECK(parseBpLine("BP 801B0DE0 (1 2 3)", h, why) == 1);
    CHECK(h.args.size() == 3 && !h.hasLr && !h.hasTime);

    // Spelled-out marker.
    CHECK(parseBpLine("Breakpoint hit 801B0DE0 (80000000 1) LR=80003178", h, why) == 1);
    CHECK(h.addr == 0x801B0DE0 && h.args.size() == 2);

    // Lines that are not breakpoint lines.
    CHECK(parseBpLine("12:00:000 N[MEMMAP]: some other message", h, why) == 0);
    CHECK(parseBpLine("", h, why) == 0);
    CHECK(parseBpLine("BPM 801B0DE0 (1)", h, why) == 0);                         // a longer word, not the marker

    // Breakpoint lines that cannot be read are reported, not accepted.
    CHECK(parseBpLine("BP zzzz (1)", h, why) == -1 && !why.empty());
    CHECK(parseBpLine("BP 801B0DE0 (1 xyz)", h, why) == -1 && has(why, "xyz"));
    CHECK(parseBpLine("BP 801B0DE0 (1 2", h, why) == -1);
    CHECK(parseBpLine("a sentence that mentions a breakpoint without an address", h, why) == -1);   // reported, never silently dropped
    CHECK(parseBpLine("BP 1801B0DE0 (1)", h, why) == -1);                        // nine digits are not an address
}

void testBpLog() {
    const std::string text =
        "00:01:000 N[MEMMAP]: BP 801B0DE0 OSInit(80000000) LR=80003178\r\n"
        "garbage that is not a hit\r\n"
        "BP not-an-address (1)\r\n"
        "00:01:100 N[MEMMAP]: BP 801BADF0 DVDInit() LR=80003180\r\n"
        "\r\n"
        "00:01:200 N[MEMMAP]: BP 801BE3BC VIInit(1) LR=80003188";               // no final newline
    BpParse p = parseBpLog(text);
    CHECK(p.hits.size() == 3);
    CHECK(p.malformed.size() == 1 && has(p.malformed[0], "line 3"));
    CHECK(p.ignored == 2);                                                       // the garbage line and the empty line
    CHECK(p.hits.size() == 3 && p.hits[1].addr == 0x801BADF0 && p.hits[1].args.empty() && p.hits[1].lineNo == 4);
    CHECK(p.hits[2].addr == 0x801BE3BC && p.hits[2].args.size() == 1);
    CHECK(parseBpLog("").hits.empty());

    // Minutes:seconds:milliseconds wraps every hour; unwrapTimes makes the sequence monotonic: 59:58.000 then 00:01.000 is 3 s later.
    std::vector<BpHit> w(2);
    w[0].hasTime = w[1].hasTime = true;
    w[0].seconds = 59 * 60 + 58;
    w[1].seconds = 1;
    unwrapTimes(w);
    CHECK(w[1].seconds > 3600.99 && w[1].seconds < 3601.01);
}

void testNotesAndOurTrace() {
    Notes n = parseNotes("r3 = 803A1B20\nTBU: 0\nTBL: 0x0001F4A0\nsome unrelated words\n\n");
    CHECK(n.hasR3 && n.r3 == 0x803A1B20 && n.hasTbu && n.tbu == 0 && n.hasTbl && n.tbl == 0x1F4A0);
    CHECK(n.unreadable.size() == 1);
    n = parseNotes("R3: 0x80123456\r\n");
    CHECK(n.hasR3 && n.r3 == 0x80123456 && !n.hasTbu && n.unreadable.empty());
    n = parseNotes("");
    CHECK(!n.hasR3 && n.unreadable.empty());

    const OurTrace t = parseOurTrace(
        "retrace 1 t=2027025 latches=0 copies=0 consecutive=0\n"
        "dvd inquiry done addr=80317A40 t=1767988\n"
        "dsp mail ->cpu 8071FEED t=100\n"
        "dsp mail ->dsp 80F3A001 t=200\n"
        "call 801B0DE0 site=80003178 depth=1 icount=1234\n"
        "boot.after-osinit icount=4321\n"
        "call broken line\n");
    CHECK(t.loaded && t.retraceTicks.size() == 1 && t.retraceTicks[0] == 2027025);
    CHECK(t.haveDriveInfoAddr && t.driveInfoAddr == 0x80317A40);
    CHECK(t.mails.size() == 2 && !t.mails[0].toDsp && t.mails[0].value == 0x8071FEED && t.mails[1].toDsp && t.mails[1].value == 0x80F3A001);
    CHECK(t.calls.size() == 1 && t.calls[0].callee == 0x801B0DE0 && t.calls[0].site == 0x80003178 && t.calls[0].depth == 1 && t.calls[0].icount == 1234);
    CHECK(t.haveAfterOsInit && t.afterOsInitIcount == 4321);
    CHECK(t.malformed.size() == 1);
}

// ---- report ------------------------------------------------------------------------------------------------------------

void writeFile(const fs::path& p, const void* data, size_t n) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
}
void writeText(const fs::path& p, const std::string& s) { writeFile(p, s.data(), s.size()); }

void put32(std::vector<uint8_t>& m, uint32_t addr, uint32_t v) {
    const size_t o = addr & 0x0FFFFFFF;
    m[o] = uint8_t(v >> 24); m[o + 1] = uint8_t(v >> 16); m[o + 2] = uint8_t(v >> 8); m[o + 3] = uint8_t(v);
}
void put16(std::vector<uint8_t>& m, uint32_t addr, uint16_t v) {
    const size_t o = addr & 0x0FFFFFFF;
    m[o] = uint8_t(v >> 8); m[o + 1] = uint8_t(v);
}

BootValues testValues() {
    BootValues v;
    v.bootMagic = 0x0D15EA5E; v.bootVersion = 1; v.memSize = 0x00100000; v.consoleType = 1; v.tvMode = 0; v.aramSize = 0x01000000;
    v.simMemSize = 0x00100000; v.busClock = 162000000; v.cpuClock = 486000000;
    v.ramTop = 0x80100000;             // a 1 MB machine keeps the synthetic dumps small
    v.bi2Size = 0x2000;
    v.driveRevision = 0x0000; v.driveDeviceCode = 0x0001; v.driveReleaseDate = 0x20020402;
    return v;
}

void testReportWithCapture() {
    const fs::path root = fs::temp_directory_path() / "awl_hle_capture_test";
    fs::remove_all(root);
    const fs::path cap = root / "capture", disc = root / "disc", dumps = root / "dumps";
    const BootValues v = testValues();

    // Disc: boot.bin with the FST maximum length 0x1000 at +0x42C, a 16-byte FST and a BI2 of 0x2000 bytes.
    std::vector<uint8_t> boot(0x440, 0);
    boot[0x42C] = 0; boot[0x42D] = 0; boot[0x42E] = 0x10; boot[0x42F] = 0;
    boot[0] = 'T'; boot[1] = 'S'; boot[2] = 'T'; boot[3] = '1';
    writeFile(disc / "sys" / "boot.bin", boot.data(), boot.size());
    std::vector<uint8_t> fst(16), bi2(0x2000);
    for (size_t i = 0; i < fst.size(); ++i) fst[i] = uint8_t(0x30 + i);
    for (size_t i = 0; i < bi2.size(); ++i) bi2[i] = uint8_t(i * 7);
    writeFile(disc / "sys" / "fst.bin", fst.data(), fst.size());
    writeFile(disc / "sys" / "bi2.bin", bi2.data(), bi2.size());
    // Layout rule: FST (0x1000 rounded to 32) ends at ramTop 0x80100000 -> FST 0x800FF000, BI2 0x800FD000.

    // Capture A at entry: everything as assumed except the bus clock; one unmodelled non-zero word; a marker that must not be quoted.
    std::vector<uint8_t> a(0x100000, 0);
    std::memcpy(&a[0], boot.data(), 4);
    put32(a, 0x80000020, v.bootMagic); put32(a, 0x80000024, v.bootVersion); put32(a, 0x80000028, v.memSize); put32(a, 0x8000002C, v.consoleType);
    put32(a, 0x80000038, 0x800FF000); put32(a, 0x8000003C, 0x1000);
    put32(a, 0x800000CC, v.tvMode); put32(a, 0x800000D0, v.aramSize); put32(a, 0x800000F0, v.simMemSize);
    put32(a, 0x800000F4, 0x800FD000); put32(a, 0x800000F8, 162000001); put32(a, 0x800000FC, v.cpuClock);
    put32(a, 0x800000E8, 0x00C0FFEE);                                            // not in the table
    put32(a, 0x80001000, 0xDEADBEEF);                                            // outside every compared field
    std::memcpy(&a[0xFF000], fst.data(), fst.size());                            // FST identical to sys/fst.bin
    std::memcpy(&a[0xFD000], bi2.data(), bi2.size());
    a[0xFD000 + 100] ^= 0xFF;                                                    // one BI2 byte differs
    writeFile(cap / "A-at-entry-mem1.raw", a.data(), a.size());
    writeText(cap / "A-notes.txt", "TBU: 0\nTBL: 1F4A0\n");

    // Capture B: render mode object at 80050000 (NTSC interlaced 640x480, double field), drive info at 80317A40... kept inside 1 MB at 80060000.
    std::vector<uint8_t> b(0x100000, 0);
    put32(b, 0x80050000, 0);                       // viTVmode: NTSC (0) << 2 | interlaced (0)
    put16(b, 0x80050004, 640); put16(b, 0x80050006, 480); put16(b, 0x80050008, 480);
    put16(b, 0x8005000A, 0); put16(b, 0x8005000C, 0); put16(b, 0x8005000E, 640); put16(b, 0x80050010, 480);
    put32(b, 0x80050014, 1); b[0x50018] = 1; b[0x50019] = 0;
    put16(b, 0x80060000, 0x0000); put16(b, 0x80060002, 0x0002); put32(b, 0x80060004, 0x20020402);   // drive info: device code differs (2, table 1)
    writeFile(cap / "B-viconfigure-mem1.raw", b.data(), b.size());
    writeText(cap / "B-notes.txt", "r3: 80050000\n");
    std::string frames = "00:00:500 N[MEMMAP]: some other line\n";
    char buf[160];
    for (int i = 0; i < 20; ++i) {                                               // 33 ms steps, starting at 1.000 s
        const int ms = 1000 + 33 * i;
        std::snprintf(buf, sizeof buf, "00:%02d:%03d N[MEMMAP]: BP 8017a7f8 (80000000 00000000) LR=80003000\n", ms / 1000, ms % 1000);
        frames += buf;
    }
    writeText(cap / "B-frames.log", frames);

    // Capture C: Dolphin has VIInit before GXInit, the platform the other way round; the other three calls agree.
    writeText(cap / "C-calls.log",
              "00:02:000 N[MEMMAP]: BP 801b0de0 OSInit(80000000 00000001) LR=80003178\n"
              "00:02:010 N[MEMMAP]: BP 801badf0 DVDInit(00000000) LR=80003180\n"
              "00:02:020 N[MEMMAP]: BP 801be3bc VIInit(00000000) LR=80003188\n"
              "00:02:030 N[MEMMAP]: BP 801cb378 GXInit(80100000 00040000) LR=80003190\n"
              "BP broken line\n");
    // Capture D: the handshake from the procedure, then one audio frame.
    writeText(cap / "D-dsp.log",
              "00:03:000 BP 801c3d30 (8071feed) LR=801c3d00\n"
              "00:03:001 BP 801c3d34 (80f3a001) LR=801c3d00\n00:03:002 BP 801c3d34 (00000001) LR=801c3d00\n"
              "00:03:003 BP 801c3d34 (80f3c002) LR=801c3d00\n00:03:004 BP 801c3d34 (00000002) LR=801c3d00\n"
              "00:03:005 BP 801c3d34 (80f3a002) LR=801c3d00\n00:03:006 BP 801c3d34 (00000003) LR=801c3d00\n"
              "00:03:007 BP 801c3d34 (80f3b002) LR=801c3d00\n00:03:008 BP 801c3d34 (00000004) LR=801c3d00\n"
              "00:03:009 BP 801c3d34 (80f3d001) LR=801c3d00\n00:03:010 BP 801c3d34 (00000005) LR=801c3d00\n"
              "00:03:011 BP 801c3d30 (dcd10000) LR=801c3d00\n"
              "00:03:012 BP 801c3d34 (babe1234) LR=801c3d00\n00:03:013 BP 801c3d34 (80200000) LR=801c3d00\n"
              "00:03:014 BP 801c3d30 (dcd10001) LR=801c3d00\n");

    // Platform trace: two end-of-frame calls 2 fields apart (2 * 675675 ticks * 4 instructions), matching call order except GX/VI swapped.
    std::string tr = "dvd inquiry done addr=80060000 t=1767988\n";
    tr += "call 801B0DE0 site=80003178 depth=1 icount=1000\ncall 801BADF0 site=80003180 depth=1 icount=2000\n";
    tr += "call 801CB378 site=80003190 depth=1 icount=3000\ncall 801BE3BC site=80003188 depth=1 icount=4000\n";
    tr += "call 8017A7F8 site=80003200 depth=2 icount=100000000\ncall 8017A7F8 site=80003200 depth=2 icount=105405400\n";
    tr += "dsp mail ->cpu 8071FEED t=1\ndsp mail ->dsp 80F3A001 t=2\ndsp mail ->dsp 00000001 t=3\ndsp mail ->cpu DCD10000 t=4\n";
    writeText(root / "boot-trace.log", tr);

    CompareInputs in;
    in.captureDir = cap.string();
    in.tracePath = (root / "boot-trace.log").string();
    in.discDir = disc.string();
    in.hostDumpDir = dumps.string();            // does not exist
    in.values = v;
    const std::string r = buildReport(in);

    // Q1/Q5
    CHECK_HAS(r, "## Q1 and Q5");
    CHECK_HAS(r, "| 80000020 | boot magic | assumed | 0D15EA5E | 0D15EA5E |");   // captured equals assumed
    CHECK_HAS(r, "DIFFERS: set 09A7EC81");                                       // bus clock 162000001 against 162000000 (0x09A7EC80)
    CHECK_HAS(r, "bus clock: 09A7EC80 -> 09A7EC81 (boot_table.cpp: busClock)");   // names the table member to edit
    CHECK_HAS(r, "800000E8=00C0FFEE");                                           // unmodelled word listed
    CHECK_HAS(r, "FST address: Dolphin 800FF000, platform rule 800FF000");
    CHECK_HAS(r, "BI2 address: Dolphin 800FD000, platform rule 800FD000");
    CHECK_HAS(r, "BI2 directly below the FST");
    CHECK_HAS(r, "FST contents at the Dolphin address: identical");
    CHECK_HAS(r, "BI2 contents at the Dolphin address: DIFFER from sys/bi2.bin in 1 of 8192 bytes (first at +100)");
    CHECK_HAS(r, "| 80000000 | disc header");                                    // disc header compared by equality
    CHECK_HAS(r, "| equal |");
    CHECK_LACKS(r, "DEADBEEF");                                                  // raw memory outside the compared fields is never quoted
    CHECK_LACKS(r, "54535431");                                                  // nor the disc header bytes
    // Drive info: the buffer address comes from the platform trace; Dolphin's device code 2 differs from the table's 1.
    CHECK_HAS(r, "Buffer address from the platform trace: 80060000");
    CHECK_HAS(r, "device code 0002, release date 20020402 -> DIFFERS");
    // Q2/Q3
    CHECK_HAS(r, "end-of-frame events (8017A7F8) found in B-frames.log: 20");
    CHECK_HAS(r, "Dolphin, all intervals: 19 intervals, median 33.000 ms");
    CHECK_HAS(r, "1.98 field periods");                                          // 33 / 16.6833
    CHECK_HAS(r, "platform (instruction-count time base): 1 intervals, median 33.367 ms");   // 5405400 / 4 / 40.5e6 * 1000
    CHECK_HAS(r, "TBU 00000000, TBL 0001F4A0");
    CHECK_HAS(r, "Decoded as a VIRenderModeObj at 80050000");
    CHECK_HAS(r, "TV format NTSC (0), scan interlaced (0); EFB 640x480, XFB height 480");
    CHECK_HAS(r, "XFB mode double field");
    CHECK_HAS(r, "TV mode word at 800000CC = 0 (NTSC) -> match");
    // Q6: four Dolphin events and four platform events; VIInit/GXInit swapped, so three are common.
    CHECK_HAS(r, "Result: 3 in common, 1 only in Dolphin, 1 only in the platform");
    CHECK_HAS(r, "| OSInit (801B0DE0) | 80000000 00000001 |");
    CHECK_HAS(r, "only in Dolphin: ");
    // DSP
    CHECK_HAS(r, "first message from the DSP is 8071FEED: yes");
    CHECK_HAS(r, "five command words at the start of the sends: 5 of 5 in place");
    CHECK_HAS(r, "DCD10000 as the second read: yes");
    CHECK_HAS(r, "BABE messages sent: 1; DCD10001 messages read: 1");
    // Parse notes
    CHECK_HAS(r, "C-calls.log: 5 lines, 4 breakpoint hits, 0 other lines ignored, 1 breakpoint lines unreadable");
    CHECK_HAS(r, "line 5:");
    // Sections for files that were not captured.
    CHECK_HAS(r, "| capture A, memory after OSInit (A-after-osinit-mem1.raw) | not captured |");
    CHECK_HAS(r, "not present (run awl-host with --dump-boot)");
    fs::remove_all(root);
}

void testReportWithoutCapture() {
    const fs::path root = fs::temp_directory_path() / "awl_hle_capture_empty";
    fs::remove_all(root);
    fs::create_directories(root);
    CompareInputs in;
    in.captureDir = (root / "none").string();
    in.tracePath = (root / "none.log").string();
    in.discDir = (root / "nodisc").string();
    in.hostDumpDir = (root / "nodumps").string();
    const std::string r = buildReport(in);
    CHECK_HAS(r, "Capture A not captured");
    CHECK_HAS(r, "Capture B frame log not captured");
    CHECK_HAS(r, "Capture C not captured");
    CHECK_HAS(r, "Capture D not captured");
    CHECK_HAS(r, "not found (run awl-host)");
    CHECK_HAS(r, "boot.bin` is missing or short");
    CHECK_HAS(r, "## DSP and AX handshake");
    // The assumed values are still listed so the owner can see what is under test.
    CHECK_HAS(r, "| 80000020 | boot magic | assumed |");
    fs::remove_all(root);

    // Garbage input files must be survived: binary junk in the logs and a truncated dump.
    fs::create_directories(root / "cap");
    std::vector<uint8_t> junk(300);
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = uint8_t(i * 37 + 11);
    writeFile(root / "cap" / "C-calls.log", junk.data(), junk.size());
    writeFile(root / "cap" / "D-dsp.log", junk.data(), junk.size());
    writeFile(root / "cap" / "B-frames.log", junk.data(), junk.size());
    writeFile(root / "cap" / "A-at-entry-mem1.raw", junk.data(), junk.size());           // far too short
    writeFile(root / "cap" / "B-notes.txt", junk.data(), junk.size());
    in.captureDir = (root / "cap").string();
    const std::string r2 = buildReport(in);
    CHECK_HAS(r2, "## Q6");
    CHECK_HAS(r2, "unreadable");                                                  // the short dump is reported as unreadable
    fs::remove_all(root);
}

void testBootTable() {
    // The single table: every assumed field has an editable member name, addresses are unique and the layout rule holds.
    const BootValues v = bootValues();
    const BootLayout l = computeLayout(v, 0xB836);
    CHECK(l.fstAddr == v.ramTop - 0xB840);                                        // 0xB836 rounded up to a multiple of 32
    CHECK(l.bi2Addr == l.fstAddr - v.bi2Size);
    const std::vector<BootField> f = bootFields(v, l);
    size_t unnamed = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i].kind == FieldKind::Assumed && f[i].addr < 0x80000100 && !f[i].member[0]) ++unnamed;
        for (size_t j = i + 1; j < f.size(); ++j) CHECK(f[i].addr != f[j].addr);
    }
    CHECK(unnamed == 0);
}

}  // namespace

int main() {
    const struct { const char* name; void (*fn)(); } tests[] = {
        {"BP line variants", testBpLineVariants}, {"BP log", testBpLog}, {"notes and platform trace", testNotesAndOurTrace},
        {"report with capture", testReportWithCapture}, {"report without capture", testReportWithoutCapture}, {"boot table", testBootTable},
    };
    for (const auto& t : tests) {
        std::printf("[%s]\n", t.name);
        std::fflush(stdout);
        t.fn();
    }
    std::printf("hle capture tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
