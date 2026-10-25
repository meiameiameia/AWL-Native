// Every assumed boot value in one place. The boot environment builder, the DI/EXI/DVD models and the host read these;
// awl-capture-compare reads the same table to list assumed against captured values. To adopt a Dolphin capture, change the
// numbers in boot_table.cpp (and nothing else). Evidence for each value: docs/research/hle-platform.md, section 3.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace awl::hle {

struct BootValues {
    // OS low memory (written by the IPL/apploader before the DOL entry)
    uint32_t bootMagic = 0;        // 0x80000020: retail disc boot, tested by DVDInit
    uint32_t bootVersion = 0;      // 0x80000024
    uint32_t memSize = 0;          // 0x80000028 physical memory
    uint32_t consoleType = 0;      // 0x8000002C
    uint32_t arenaLo = 0;          // 0x80000030 (0: OSInit uses its own default)
    uint32_t arenaHi = 0;          // 0x80000034 (0: __start derives it from the BI2 argument vector)
    uint32_t tvMode = 0;           // 0x800000CC
    uint32_t aramSize = 0;         // 0x800000D0
    uint32_t simMemSize = 0;       // 0x800000F0
    uint32_t busClock = 0;         // 0x800000F8
    uint32_t cpuClock = 0;         // 0x800000FC
    // Placement of the boot data at the top of memory: the FST (aligned to 32 bytes) ends at ramTop, BI2 sits directly below it.
    uint32_t ramTop = 0;
    uint32_t bi2Size = 0;
    // CPU state at the DOL entry
    uint32_t entryMsr = 0;
    // Devices the SDK reads during boot
    uint32_t diConfig = 0;         // DI_CONFIG (0xCC006024), read by EXISync through __OSGetDIConfig
    uint32_t driveRevision = 0, driveDeviceCode = 0, driveReleaseDate = 0;   // DVDDriveInfo returned by the inquiry command
    uint32_t rtcSeconds = 0;       // RTC counter, seconds since 2000-01-01
};

// The values currently used.
const BootValues& bootValues();

enum class FieldKind {
    Assumed,       // chosen by the platform; a capture should confirm or replace it
    FromDisc,      // copied from the disc extraction (sys/boot.bin, sys/bi2.bin, sys/fst.bin): compare by equality only
    OsWritten      // written by the recompiled OS code itself; compared with the platform's own run, not assumed
};

struct BootField {
    uint32_t addr = 0;             // guest address
    uint32_t size = 4;             // bytes (1, 2, 4)
    uint64_t value = 0;            // assumed value (meaningless for FromDisc / OsWritten)
    const char* name = "";
    FieldKind kind = FieldKind::Assumed;
    const char* note = "";
    const char* member = "";       // BootValues member (or layout rule) that holds the value, for the capture report
};

struct BootLayout {
    uint32_t fstAddr = 0, fstMax = 0, bi2Addr = 0;
};

// Layout rule: FST at the top of memory, BI2 directly below it.
BootLayout computeLayout(const BootValues& v, uint32_t fstMaxLength);

// All low-memory fields the platform assumes or relies on, in address order.
std::vector<BootField> bootFields(const BootValues& v, const BootLayout& layout);

}  // namespace awl::hle
