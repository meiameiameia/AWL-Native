#include "boot_table.h"

#include <ctime>

namespace awl::hle {

const BootValues& bootValues() {
    static const BootValues v = [] {
        BootValues b;
        b.bootMagic = 0x0D15EA5E;       // read by DVDInit at 0x801BAE74: this value selects the retail path
        b.bootVersion = 1;              // not read by the SDK code examined; same value the P0 runner used
        b.memSize = 0x01800000;         // 24 MB; read by __OSInitMemoryProtection and OSInit
        b.consoleType = 0x00000003;     // retail (YAGCD 4.2.1.1.2); the DOL tests only the top nibble / bit 0x10000000, see the field note
        b.arenaLo = 0;                  // OSInit substitutes its own default (end of the DOL image)
        b.tvMode = 0;                   // NTSC; read by VIInit, VIConfigure and VIGetTvFormat
        b.aramSize = 0x01000000;        // unverified; no constant-address reader found
        b.simMemSize = 0x01800000;      // read by __OSInitMemoryProtection to pick the 24 MB configuration
        b.busClock = 162000000;         // 54 functions read it for tick conversions
        b.cpuClock = 486000000;
        b.ramTop = 0x81800000;
        b.bi2Size = 0x2000;
        b.entryMsr = 0x00002032;        // FP and ME set, external interrupts off
        b.diConfig = 0;                 // any value except 0xFF selects the normal path in EXISync
        b.driveRevision = 0;            // drive information as Dolphin's HLE drive reports it (capture A): its choice, not a measured drive
        b.driveDeviceCode = 2;
        b.driveReleaseDate = 0x20060526;
        b.viClock = 1;                  // inferred from capture A: Dolphin's Si.poll after SISetSamplingRate(0) is twice the 27 MHz value, so the register read 1
        b.rtcSeconds = 846221347;       // 2026-10-25 05:29:07, the clock Dolphin held at capture A (its system time at 0x800030D8 is exactly this x 40.5 MHz); fixed so runs are repeatable
        return b;
    }();
    return v;
}

uint32_t hostRtcSeconds() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
    const std::time_t asUtc = _mkgmtime(&local);
#else
    localtime_r(&now, &local);
    const std::time_t asUtc = timegm(&local);
#endif
    const long long s = static_cast<long long>(asUtc) - 946684800LL;      // Unix epoch to 2000-01-01
    return s > 0 ? static_cast<uint32_t>(s) : 0;
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
    auto add = [&](uint32_t addr, uint32_t size, uint64_t value, const char* name, K kind, const char* note, const char* member = "") {
        BootField b;
        b.addr = addr; b.size = size; b.value = value; b.name = name; b.kind = kind; b.note = note; b.member = member;
        f.push_back(b);
    };
    add(0x80000000, 32, 0, "disc header (game id, disc number, version, streaming flags, magic)", K::FromDisc, "copy of sys/boot.bin[0..0x20)");
    add(0x80000020, 4, v.bootMagic, "boot magic", K::Assumed, "tested by DVDInit", "boot_table.cpp: bootMagic");
    add(0x80000024, 4, v.bootVersion, "boot version", K::Assumed, "not read", "boot_table.cpp: bootVersion");
    add(0x80000028, 4, v.memSize, "physical memory size", K::Assumed, "OSInit, __OSInitMemoryProtection", "boot_table.cpp: memSize");
    add(0x8000002C, 4, v.consoleType, "console type", K::Assumed,
        "OSGetConsoleType (0x801B0C54) returns it; every user tests only the top nibble: OSInit (0x801B1008, nibble 0 = retail text), EXISync (0x801F6878, compares with 0x20000000) and 0x801F7F1C (bit 0x10000000 = development)",
        "boot_table.cpp: consoleType");
    f.back().intent = "retail value 0x00000003 (YAGCD 4.2.1.1.2); Dolphin's HLE boot writes a development-kit type";
    add(0x80000030, 4, v.arenaLo, "arena low", K::Assumed, "0 = OSInit default", "boot_table.cpp: arenaLo");
    add(0x80000034, 4, l.fstAddr, "arena high", K::Assumed, "the apploader leaves it equal to the FST location; OSInit then lowers it below the FST", "boot_table.cpp: ramTop / FST rule (computeLayout)");
    add(0x80000038, 4, l.fstAddr, "FST location", K::Assumed, "placement rule: FST at the top of memory", "boot_table.cpp: ramTop / FST rule (computeLayout)");
    add(0x8000003C, 4, l.fstMax, "FST maximum length", K::Assumed, "sys/boot.bin+0x42C", "boot_table.cpp: sys/boot.bin+0x42C");
    add(0x800000CC, 4, v.tvMode, "TV mode", K::Assumed, "NTSC", "boot_table.cpp: tvMode");
    add(0x800000D0, 4, v.aramSize, "ARAM size", K::Assumed, "unverified", "boot_table.cpp: aramSize");
    add(0x800000F0, 4, v.simMemSize, "simulated memory size", K::Assumed, "__OSInitMemoryProtection", "boot_table.cpp: simMemSize");
    add(0x800000F4, 4, l.bi2Addr, "BI2 pointer", K::Assumed, "placement rule: BI2 directly below the FST", "boot_table.cpp: ramTop, bi2Size (computeLayout)");
    add(0x800000F8, 4, v.busClock, "bus clock", K::Assumed, "tick conversions", "boot_table.cpp: busClock");
    add(0x800000FC, 4, v.cpuClock, "CPU clock", K::Assumed, "", "boot_table.cpp: cpuClock");
    // Written by the recompiled OS during OSInit; compared against the platform's own run.
    add(0x80000048, 4, 0, "written by 0x801D3348", K::OsWritten, "");
    // 0x80000060-0x80000080: the debugger-exception trampoline code copied by the OS's debugger-interface setup; 0x801B11B8 reads 0x80000060.
    for (uint32_t a = 0x80000060; a <= 0x80000080; a += 4) add(a, 4, 0, "debugger trampoline word", K::OsWritten, "");
    add(0x800000C0, 4, 0, "written by 0x801B2D24", K::OsWritten, "");
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
    add(0x800030D8, 8, static_cast<uint64_t>(v.rtcSeconds) * 40500000ull, "system time offset", K::Assumed,
        "set by the IPL from the RTC: seconds since 2000-01-01 at 40.5 MHz; the EXI RTC uses the same rtcSeconds", "boot_table.cpp: rtcSeconds x 40,500,000");
    // Provided by the IPL, shown by Dolphin's capture; no constant-address access in the DOL or RELs reads them (awl-hle-scan facts).
    add(0x800000EC, 4, 0, "top of memory (IPL)", K::Unread, "Dolphin holds 0x81800000");
    add(0x800030C0, 4, 0, "EXI-area word", K::Unread, "zero at entry, non-zero in Dolphin after OSInit (origin not found: no constant-address writer besides EXIInit zeroing it, no literal in the code)");
    add(0x800030D4, 4, 0, "IPL word", K::Unread, "");
    add(0x800030EC, 4, 0, "IPL word (OSResetSystem writes it)", K::Unread, "written by 0x801B568C only");
    add(0x800030E2, 1, 0, "reset code", K::Assumed, "OSGetResetCode");
    add(0x800030E3, 1, 0, "PAD flags", K::Assumed, "__PADDisableRecalibration");
    add(0x800030E4, 2, 0, "PAD 3 held buttons at boot", K::Assumed, "__check_pad3");
    add(0x800030E6, 2, 0x8000u | v.driveDeviceCode, "DVD device code word", K::OsWritten, "InquiryCallback: deviceCode | 0x8000");
    add(0x800030E8, 1, 0, "boot flag from BI2 +0x0C", K::OsWritten, "OSInit");
    add(0x800030E9, 1, 0, "boot flag from BI2 +0x24", K::OsWritten, "OSInit");
    return f;
}

}  // namespace awl::hle
