#include "boot_table.h"

namespace awl::hle {

const BootValues& bootValues() {
    static const BootValues v = [] {
        BootValues b;
        b.bootMagic = 0x0D15EA5E;       // read by DVDInit at 0x801BAE74: this value selects the retail path
        b.bootVersion = 1;              // not read by the SDK code examined; same value the P0 runner used
        b.memSize = 0x01800000;         // 24 MB; read by __OSInitMemoryProtection and OSInit
        b.consoleType = 0x00000001;     // retail hardware revision 1; OSInit prints from the top nibble (0 = retail)
        b.arenaLo = 0;                  // OSInit substitutes its own default (end of the DOL image)
        b.arenaHi = 0;                  // __start sets it from the BI2 argument vector
        b.tvMode = 0;                   // NTSC; read by VIInit, VIConfigure and VIGetTvFormat
        b.aramSize = 0x01000000;        // unverified; no constant-address reader found
        b.simMemSize = 0x01800000;      // read by __OSInitMemoryProtection to pick the 24 MB configuration
        b.busClock = 162000000;         // 54 functions read it for tick conversions
        b.cpuClock = 486000000;
        b.ramTop = 0x81800000;
        b.bi2Size = 0x2000;
        b.entryMsr = 0x00002032;        // FP and ME set, external interrupts off
        b.diConfig = 0;                 // any value except 0xFF selects the normal path in EXISync
        b.driveRevision = 0;            // placeholder drive information
        b.driveDeviceCode = 1;
        b.driveReleaseDate = 0;
        b.rtcSeconds = 820540800;       // 2026-01-01 00:00:00, fixed so runs are repeatable
        return b;
    }();
    return v;
}

BootLayout computeLayout(const BootValues& v, uint32_t fstMaxLength) {
    BootLayout l;
    l.fstMax = fstMaxLength;
    l.fstAddr = v.ramTop - ((fstMaxLength + 31) & ~31u);
    l.bi2Addr = l.fstAddr - v.bi2Size;
    return l;
}

std::vector<BootField> bootFields(const BootValues& v, const BootLayout& l) {
    using K = FieldKind;
    std::vector<BootField> f;
    auto add = [&](uint32_t addr, uint32_t size, uint64_t value, const char* name, K kind, const char* note) {
        BootField b;
        b.addr = addr; b.size = size; b.value = value; b.name = name; b.kind = kind; b.note = note;
        f.push_back(b);
    };
    add(0x80000000, 32, 0, "disc header (game id, disc number, version, streaming flags, magic)", K::FromDisc, "copy of sys/boot.bin[0..0x20)");
    add(0x80000020, 4, v.bootMagic, "boot magic", K::Assumed, "tested by DVDInit");
    add(0x80000024, 4, v.bootVersion, "boot version", K::Assumed, "not read");
    add(0x80000028, 4, v.memSize, "physical memory size", K::Assumed, "OSInit, __OSInitMemoryProtection");
    add(0x8000002C, 4, v.consoleType, "console type", K::Assumed, "OSGetConsoleType");
    add(0x80000030, 4, v.arenaLo, "arena low", K::Assumed, "0 = OSInit default");
    add(0x80000034, 4, v.arenaHi, "arena high", K::Assumed, "0 = set by __start from the BI2 arguments");
    add(0x80000038, 4, l.fstAddr, "FST location", K::Assumed, "placement rule: FST at the top of memory");
    add(0x8000003C, 4, l.fstMax, "FST maximum length", K::Assumed, "sys/boot.bin+0x42C");
    add(0x800000CC, 4, v.tvMode, "TV mode", K::Assumed, "NTSC");
    add(0x800000D0, 4, v.aramSize, "ARAM size", K::Assumed, "unverified");
    add(0x800000F0, 4, v.simMemSize, "simulated memory size", K::Assumed, "__OSInitMemoryProtection");
    add(0x800000F4, 4, l.bi2Addr, "BI2 pointer", K::Assumed, "placement rule: BI2 directly below the FST");
    add(0x800000F8, 4, v.busClock, "bus clock", K::Assumed, "tick conversions");
    add(0x800000FC, 4, v.cpuClock, "CPU clock", K::Assumed, "");
    // Written by the recompiled OS during OSInit; compared against the platform's own run.
    add(0x800000C4, 4, 0, "interrupt mask word 1", K::OsWritten, "__OSMaskInterrupts");
    add(0x800000C8, 4, 0, "interrupt mask word 2", K::OsWritten, "__OSMaskInterrupts");
    add(0x800000D4, 4, 0, "current OSContext", K::OsWritten, "OSSetCurrentContext");
    add(0x800000D8, 4, 0, "FPU-owner OSContext", K::OsWritten, "");
    add(0x800000DC, 4, 0, "first thread", K::OsWritten, "");
    add(0x800000E0, 4, 0, "last thread", K::OsWritten, "");
    add(0x800000E4, 4, 0, "current thread", K::OsWritten, "");
    add(0x800030C8, 4, 0, "module list head", K::OsWritten, "");
    add(0x800030CC, 4, 0, "module list tail", K::OsWritten, "");
    add(0x800030D0, 4, 0, "module string table", K::OsWritten, "");
    add(0x800030D8, 8, 0, "system time offset", K::OsWritten, "__OSGetSystemTime");
    add(0x800030E2, 1, 0, "reset code", K::Assumed, "OSGetResetCode");
    add(0x800030E3, 1, 0, "PAD flags", K::Assumed, "__PADDisableRecalibration");
    add(0x800030E4, 2, 0, "PAD 3 held buttons at boot", K::Assumed, "__check_pad3");
    add(0x800030E6, 2, 0x8000u | v.driveDeviceCode, "DVD device code word", K::OsWritten, "InquiryCallback: deviceCode | 0x8000");
    add(0x800030E8, 1, 0, "boot flag from BI2 +0x0C", K::OsWritten, "OSInit");
    add(0x800030E9, 1, 0, "boot flag from BI2 +0x24", K::OsWritten, "OSInit");
    return f;
}

}  // namespace awl::hle
