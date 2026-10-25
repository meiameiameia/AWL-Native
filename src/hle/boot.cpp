#include "boot.h"
#include "dvd.h"

#include <fstream>
#include <iterator>
#include <vector>

namespace awl::hle {
namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

uint32_t be32(const std::vector<uint8_t>& v, size_t off) {
    return uint32_t(v[off]) << 24 | uint32_t(v[off + 1]) << 16 | uint32_t(v[off + 2]) << 8 | v[off + 3];
}

}  // namespace

bool buildBootEnvironment(Machine& m, const recomp::DolImage& dol, const std::string& discDir, BootLayout& layout, std::string& err) {
    std::vector<uint8_t> boot, bi2, fst;
    if (!readFile(discDir + "/sys/boot.bin", boot, err) || !readFile(discDir + "/sys/bi2.bin", bi2, err) || !readFile(discDir + "/sys/fst.bin", fst, err)) return false;
    if (boot.size() < 0x440 || bi2.size() != 0x2000) { err = "boot.bin or bi2.bin has an unexpected size"; return false; }

    std::memset(m.mem, 0, rt::kMem1Size);
    std::memset(m.lc, 0, rt::kLockedCacheSize);
    for (const auto& s : dol.sections) {
        if ((s.addr >> 28) != 8 || (s.addr & 0x0FFFFFFFu) + s.size > rt::kMem1Size) {
            err = "DOL section does not fit MEM1";
            return false;
        }
        std::memcpy(m.mem + (s.addr & 0x0FFFFFFFu), dol.file.data() + s.fileOffset, s.size);
    }

    GuestMem mem(m.mem);
    const BootValues& v = bootValues();
    const uint32_t fstSize = be32(boot, 0x428);
    layout = computeLayout(v, be32(boot, 0x42C));
    if (fst.size() < fstSize || fstSize == 0) { err = "fst.bin is smaller than the size recorded in boot.bin"; return false; }
    if (v.ramTop != 0x80000000u + rt::kMem1Size) { err = "boot table: ramTop does not match the 24 MB memory"; return false; }
    {
        std::vector<uint8_t> table(fst.begin(), fst.begin() + fstSize);
        if (!Platform::get().dvd().loadFst(table, err)) return false;
    }
    std::memcpy(m.mem + (layout.fstAddr & 0x0FFFFFFFu), fst.data(), fstSize);
    std::memcpy(m.mem + (layout.bi2Addr & 0x0FFFFFFFu), bi2.data(), bi2.size());

    // Low memory from the single table of assumed values (boot_table.cpp); the disc header comes from the extraction.
    for (const BootField& f : bootFields(v, layout)) {
        if (f.kind == FieldKind::FromDisc) { std::memcpy(m.mem + (f.addr & 0x0FFFFFFFu), boot.data(), f.size); continue; }
        if (f.kind != FieldKind::Assumed || f.value == 0) continue;
        if (f.size == 4) mem.w32(f.addr, static_cast<uint32_t>(f.value));
        else if (f.size == 2) mem.w16(f.addr, static_cast<uint16_t>(f.value));
        else if (f.size == 1) mem.w8(f.addr, static_cast<uint8_t>(f.value));
    }

    TraceLog& t = trace();
    t.line("boot.environment (assumed values from boot_table.cpp; diff against a Dolphin capture of memory at the DOL entry)");
    t.line("boot.mem1 size=%08X", rt::kMem1Size);
    t.line("boot.header %02X%02X%02X%02X%02X%02X disc=%u version=%u streaming=%u streambuf=%u", boot[0], boot[1], boot[2], boot[3], boot[4], boot[5], boot[6], boot[7], boot[8], boot[9]);
    for (const BootField& f : bootFields(v, layout)) {
        if (f.kind != FieldKind::Assumed) continue;
        t.line("boot.low %08X size=%u value=%llX %s", f.addr, f.size, static_cast<unsigned long long>(f.value), f.name);
    }
    t.line("boot.layout bi2=%08X..%08X fst=%08X..%08X fstSize=%08X", layout.bi2Addr, layout.bi2Addr + v.bi2Size, layout.fstAddr, v.ramTop, fstSize);
    t.line("boot.cpu msr=%08X fpscr=0 entry=%08X", v.entryMsr, dol.entry);
    t.line("boot.devices diConfig=%02X driveRevision=%04X driveDeviceCode=%04X driveDate=%08X", v.diConfig, v.driveRevision, v.driveDeviceCode, v.driveReleaseDate);
    t.line("boot.time tick=%llu Hz instructionsPerTick=%llu fieldTicks=%llu lineTicks=%llu rtc=%u", static_cast<unsigned long long>(kTickRate),
           static_cast<unsigned long long>(kInstructionsPerTick), static_cast<unsigned long long>(kFieldTicks), static_cast<unsigned long long>(kLineTicks),
           Platform::get().config().rtcSeconds);
    return true;
}

}  // namespace awl::hle
