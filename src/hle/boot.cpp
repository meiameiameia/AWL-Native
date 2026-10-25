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
    const uint32_t top = 0x80000000u + rt::kMem1Size;
    layout.fstMax = be32(boot, 0x42C);
    const uint32_t fstSize = be32(boot, 0x428);
    if (fst.size() < fstSize || fstSize == 0) { err = "fst.bin is smaller than the size recorded in boot.bin"; return false; }
    layout.fstAddr = top - ((layout.fstMax + 31) & ~31u);
    layout.bi2Addr = layout.fstAddr - 0x2000;

    {
        std::vector<uint8_t> table(fst.begin(), fst.begin() + fstSize);
        if (!Platform::get().dvd().loadFst(table, err)) return false;
    }
    std::memcpy(m.mem + (layout.fstAddr & 0x0FFFFFFFu), fst.data(), fstSize);
    std::memcpy(m.mem + (layout.bi2Addr & 0x0FFFFFFFu), bi2.data(), bi2.size());

    // Low memory. See docs/research/hle-platform.md section 3.1 for the reader of each value.
    std::memcpy(m.mem, boot.data(), 0x20);                 // 32-byte disc header (game id, disc number, version, streaming flags, magic)
    mem.w32(0x80000020, 0x0D15EA5E);                       // retail disc boot (tested by DVDInit)
    mem.w32(0x80000024, 1);                                // boot version
    mem.w32(0x80000028, rt::kMem1Size);                    // physical memory size
    mem.w32(0x8000002C, 0x00000001);                       // console type: retail hardware revision 1
    mem.w32(0x80000030, 0);                                // arena low: OSInit uses its own default
    mem.w32(0x80000034, 0);                                // arena high: set by __start from the BI2 argument vector
    mem.w32(0x80000038, layout.fstAddr);                   // FST location
    mem.w32(0x8000003C, layout.fstMax);                    // FST maximum length
    mem.w32(0x800000CC, 0);                                // video mode: NTSC
    mem.w32(0x800000D0, 0x01000000);                       // ARAM size (unverified)
    mem.w32(0x800000F0, rt::kMem1Size);                    // simulated memory size
    mem.w32(0x800000F4, layout.bi2Addr);                   // BI2 pointer
    mem.w32(0x800000F8, 162000000);                        // bus clock
    mem.w32(0x800000FC, 486000000);                        // CPU clock

    TraceLog& t = trace();
    t.line("boot.environment (assumed values; diff against a Dolphin capture of memory at the DOL entry)");
    t.line("boot.mem1 size=%08X", rt::kMem1Size);
    t.line("boot.header %02X%02X%02X%02X%02X%02X disc=%u version=%u streaming=%u streambuf=%u", boot[0], boot[1], boot[2], boot[3], boot[4], boot[5], boot[6], boot[7], boot[8], boot[9]);
    t.line("boot.low 80000020=0D15EA5E 80000024=00000001 80000028=%08X 8000002C=00000001 80000030=00000000 80000034=00000000", rt::kMem1Size);
    t.line("boot.low 80000038=%08X 8000003C=%08X 800000CC=00000000 800000D0=01000000 800000F0=%08X 800000F4=%08X", layout.fstAddr, layout.fstMax, rt::kMem1Size, layout.bi2Addr);
    t.line("boot.low 800000F8=%08X 800000FC=%08X", 162000000, 486000000);
    t.line("boot.layout bi2=%08X..%08X fst=%08X..%08X fstSize=%08X", layout.bi2Addr, layout.bi2Addr + 0x2000, layout.fstAddr, top, fstSize);
    t.line("boot.cpu msr=00002032 fpscr=0 entry=%08X", dol.entry);
    t.line("boot.time tick=%llu Hz instructionsPerTick=%llu fieldTicks=%llu lineTicks=%llu rtc=%u", static_cast<unsigned long long>(kTickRate),
           static_cast<unsigned long long>(kInstructionsPerTick), static_cast<unsigned long long>(kFieldTicks), static_cast<unsigned long long>(kLineTicks),
           Platform::get().config().rtcSeconds);
    return true;
}

}  // namespace awl::hle
