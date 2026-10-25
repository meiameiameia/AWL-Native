// Native replacements of SDK functions (the entries of tools/recomp/hle_manifest.txt), the MMIO routing, the
// preemption hook installation and the main-loop bookkeeping.
#include "devices.h"
#include "dvd.h"
#include "pad_source.h"

namespace awl::hle {

// ---- MMIO routing ---------------------------------------------------------------------------------------
namespace {

uint64_t mmioRead(Ctx& c, uint32_t addr, uint32_t size) {
    Platform& p = Platform::get();
    const int b = blockOf(addr);
    if (b < 0) stopHw(c, addr, "hardware register outside every modelled block");
    return p.dev(static_cast<uint32_t>(b)).read(c, addr, size);
}

void mmioWrite(Ctx& c, uint32_t addr, uint32_t size, uint64_t value) {
    Platform& p = Platform::get();
    const int b = blockOf(addr);
    if (b < 0) stopHw(c, addr, "hardware register outside every modelled block");
    p.dev(static_cast<uint32_t>(b)).write(c, addr, size, value);
}

uint64_t timeBase(Ctx& c) {
    (void)c;
    return Platform::get().now();
}

// ---- handlers ----------------------------------------------------------------------------------------------
void hPPCMtdec(Ctx& c) { Platform::get().setDecrementer(c.r[3]); }
void hNop(Ctx& c) {
    // PPCSync / cache range functions are exact no-ops on flat memory; the others below are logged stubs.
    if (c.pc == 0x801B20F4 || c.pc == 0x801F52F4) trace().line("stub no-op pc=%08X", c.pc);
}
void hOSSaveContext(Ctx& c) { Platform::get().osSaveContext(c); }
void hOSLoadContext(Ctx& c) { Platform::get().osLoadContext(c); }

// DVD command layer. Arguments follow the SDK functions read in the DOL (docs/research/hle-platform.md, section 2.2).
void hDvdReadAbs(Ctx& c) {
    Platform& p = Platform::get();
    GuestMem& m = p.mem();
    m.w32(c.r[3] + dvdblock::Addr, c.r[4]);
    m.w32(c.r[3] + dvdblock::Length, c.r[5]);
    m.w32(c.r[3] + dvdblock::Offset, c.r[6]);
    m.w32(c.r[3] + dvdblock::Callback, c.r[7]);
    c.r[3] = p.dvd().submit(c, c.r[3], dvdcmd::Read, c.r[8]) ? 1 : 0;
}
void hDvdSeekAbs(Ctx& c) {
    Platform& p = Platform::get();
    GuestMem& m = p.mem();
    m.w32(c.r[3] + dvdblock::Offset, c.r[4]);
    m.w32(c.r[3] + dvdblock::Callback, c.r[5]);
    c.r[3] = p.dvd().submit(c, c.r[3], dvdcmd::Seek, c.r[6]) ? 1 : 0;
}
void hDvdPrepareStream(Ctx& c) {
    Platform& p = Platform::get();
    GuestMem& m = p.mem();
    m.w32(c.r[3] + dvdblock::Length, c.r[4]);
    m.w32(c.r[3] + dvdblock::Offset, c.r[5]);
    m.w32(c.r[3] + dvdblock::Callback, c.r[6]);
    c.r[3] = p.dvd().submit(c, c.r[3], dvdcmd::InitStream, 1) ? 1 : 0;
}
void hDvdStreamCmd(Ctx& c, uint32_t cmd) {
    Platform& p = Platform::get();
    p.mem().w32(c.r[3] + dvdblock::Callback, c.r[4]);
    c.r[3] = p.dvd().submit(c, c.r[3], cmd, 1) ? 1 : 0;
}
void hDvdCancelStream(Ctx& c) { hDvdStreamCmd(c, dvdcmd::CancelStream); }
void hDvdErrorStatus(Ctx& c) { hDvdStreamCmd(c, dvdcmd::RequestAudioError); }
void hDvdInquiry(Ctx& c) {
    Platform& p = Platform::get();
    GuestMem& m = p.mem();
    m.w32(c.r[3] + dvdblock::Addr, c.r[4]);
    m.w32(c.r[3] + dvdblock::Length, 0x20);
    m.w32(c.r[3] + dvdblock::Callback, c.r[5]);
    c.r[3] = p.dvd().submit(c, c.r[3], dvdcmd::Inquiry, 2) ? 1 : 0;
}
void hDvdGetDriveStatus(Ctx& c) { c.r[3] = Platform::get().dvd().busy() ? 1u : 0u; }
void hDvdCancelAsync(Ctx& c) {
    const uint32_t block = c.r[3], cb = c.r[4];
    Platform::get().dvd().cancel(c, block, cb);
    c.r[3] = 1;
}

// PAD: the HSD pad reader calls these directly (docs/research/hle-platform.md, section 2.4). Neutral input for P1:
// channel 0 connected with no buttons and centred sticks, channels 1-3 report PAD_ERR_NO_CONTROLLER (-1).
void hPadTrue(Ctx& c) {
    static uint64_t calls = 0;
    if (++calls <= 8) trace().line("stub pad pc=%08X: PAD call reports success (neutral input only)", c.pc);
    c.r[3] = 1;
}
void hPadRead(Ctx& c) {
    Platform& p = Platform::get();
    GuestMem& m = p.mem();
    const PadReading in = readPad(p.config().padXInput ? PadMode::XInput : PadMode::Neutral);
    const uint32_t status = c.r[3];
    for (uint32_t ch = 0; ch < 4; ++ch) {
        const uint32_t e = status + 12 * ch;
        for (uint32_t i = 0; i < 12; ++i) m.w8(e + i, 0);
        if (ch != 0 || !in.connected) { m.w8(e + 10, 0xFF); continue; }       // PAD_ERR_NO_CONTROLLER
        m.w16(e + 0, in.buttons);
        m.w8(e + 2, static_cast<uint8_t>(in.stickX)); m.w8(e + 3, static_cast<uint8_t>(in.stickY));
        m.w8(e + 4, static_cast<uint8_t>(in.substickX)); m.w8(e + 5, static_cast<uint8_t>(in.substickY));
        m.w8(e + 6, in.triggerL); m.w8(e + 7, in.triggerR);
    }
    c.r[3] = 0;
}
void hNop2(Ctx&) {}

// GBA link: no GBA is attached, so every call reports GBA_NOT_READY (stub register: GBA).
void hGbaNotReady(Ctx& c) {
    trace().line("stub gba pc=%08X t=%llu: reports GBA_NOT_READY", c.pc, static_cast<unsigned long long>(Platform::get().now()));
    if (c.r[4]) Platform::get().mem().w8(c.r[4], 0);
    c.r[3] = 1;
}

struct Entry { uint32_t addr; const char* name; rt::HleHandler fn; };
const Entry kEntries[] = {
    {0x801B0A48, "PPCMtdec", hPPCMtdec},
    {0x801B0A50, "PPCSync", hNop},
    {0x801B23C8, "DCFlushRange", hNop},
    {0x801B23F8, "DCStoreRange", hNop},
    {0x801B2D8C, "OSSaveContext", hOSSaveContext},
    {0x801B2E0C, "OSLoadContext", hOSLoadContext},
    {0x801B5054, "RealMode", hNop},
    {0x801B20F4, "__OSInitAudioSystem", hNop},
    {0x801F52F4, "SIInit", hNop},
    {0x801C06D4, "PADInit", hPadTrue},
    {0x801C04B0, "PADReset", hPadTrue},
    {0x801C05C0, "PADRecalibrate", hPadTrue},
    {0x801C0824, "PADRead", hPadRead},
    {0x801C0B24, "PADControlMotor", hNop2},
    {0x801FB9E4, "GBAInit", hNop2},
    {0x801FBA88, "GBAGetStatus", hGbaNotReady},
    {0x801FBB18, "GBAReset", hGbaNotReady},
    {0x801FBC18, "GBARead", hGbaNotReady},
    {0x801FBCDC, "GBAWrite", hGbaNotReady},
    {0x801BC9D8, "DVDReadAbsAsyncPrio", hDvdReadAbs},
    {0x801BCAB4, "DVDSeekAbsAsyncPrio", hDvdSeekAbs},
    {0x801BCD24, "DVDPrepareStreamAbsAsync", hDvdPrepareStream},
    {0x801BCDE8, "DVDCancelStreamAsync", hDvdCancelStream},
    {0x801BCEA4, "DVDGetStreamErrorStatusAsync", hDvdErrorStatus},
    {0x801BCF60, "DVDInquiryAsync", hDvdInquiry},
    {0x801BD0C0, "DVDGetDriveStatus", hDvdGetDriveStatus},
    {0x801BD1CC, "DVDCancelAsync", hDvdCancelAsync},
};

}  // namespace

void Platform::registerHandlers() {
    for (const Entry& e : kEntries) rt::hleRegister(e.addr, e.name, e.fn);
}

void Platform::installHandlers() {
    registerHandlers();
    rt::mmioInstall(mmioRead, mmioWrite);
    rt::timeBaseInstall(timeBase);
    rt::preemptInstall(&Platform::preempt);
    hookInstalled_ = true;
    lowerDeadline(machine_->icount + 1);   // the first hook run arms the real deadline
}

// ---- main-loop criterion ------------------------------------------------------------------------------------
// A retrace "qualifies" when a framebuffer latch happened within the last three retraces and an EFB copy
// followed that latch (the frame the latch shows was produced by a copy). N consecutive qualifying retraces
// mean the game's main loop is running (docs/research/hle-platform.md, section 6).
void Platform::onRetrace() {
    ++counters.retraces;
    ++counters.sinceLatch;
    const bool qualifies = counters.haveLatch && counters.sinceLatch <= 3 && counters.lastLatchHadCopy;
    counters.consecutive = qualifies ? counters.consecutive + 1 : 0;
    trace().line("retrace %llu t=%llu latches=%llu copies=%llu consecutive=%u", static_cast<unsigned long long>(counters.retraces),
                 static_cast<unsigned long long>(now()), static_cast<unsigned long long>(counters.fbLatches),
                 static_cast<unsigned long long>(counters.efbCopies), counters.consecutive);
    if (counters.consecutive >= cfg_.mainLoopRetraces) requestExit("main loop reached");
}

}  // namespace awl::hle
