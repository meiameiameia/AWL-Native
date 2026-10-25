// Writes a tiny synthetic DOL (hand-assembled, no game bytes) used by the end-to-end generator test:
//   recomp_synth_dol <out.dol>
// Layout, all in one text section at 0x80003100:
//   0x80003100 entry     bl the four functions below, blr
//   0x80003200 mmio      lis/addi build 0xCC006C00; stw r3,8(r4); lwz r3,12(r4); blr   (MMIO-only evidence)
//   0x80003300 loop      mtctr r3; L: addi r4,r4,1; bdnz L; blr                        (register-only backward loop)
//   0x80003400 hle       li r3,1; blr                                                  (replaced via the manifest)
//   0x80003500 priv      rfi                                                           (privileged evidence)
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr uint32_t D(uint32_t op, uint32_t rt, uint32_t ra, int32_t imm) {
    return (op << 26) | (rt << 21) | (ra << 16) | (static_cast<uint32_t>(imm) & 0xFFFF);
}
constexpr uint32_t X(uint32_t op, uint32_t f1, uint32_t f2, uint32_t f3, uint32_t xo) {
    return (op << 26) | (f1 << 21) | (f2 << 16) | (f3 << 11) | (xo << 1);
}
constexpr uint32_t B(uint32_t from, uint32_t to, bool link) {
    return (18u << 26) | ((to - from) & 0x03FFFFFCu) | (link ? 1u : 0u);
}
constexpr uint32_t kBlr = (19u << 26) | (20u << 21) | (16u << 1);
constexpr uint32_t kRfi = (19u << 26) | (50u << 1);

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: recomp_synth_dol <out.dol>\n");
        return 2;
    }
    constexpr uint32_t base = 0x80003100, textSize = 0x408;
    std::vector<uint8_t> file(0x100 + textSize, 0);
    auto put32 = [&](uint32_t off, uint32_t v) {
        file[off] = static_cast<uint8_t>(v >> 24);
        file[off + 1] = static_cast<uint8_t>(v >> 16);
        file[off + 2] = static_cast<uint8_t>(v >> 8);
        file[off + 3] = static_cast<uint8_t>(v);
    };
    auto w = [&](uint32_t addr, uint32_t v) { put32(0x100 + (addr - base), v); };
    w(0x80003100, B(0x80003100, 0x80003200, true));
    w(0x80003104, B(0x80003104, 0x80003300, true));
    w(0x80003108, B(0x80003108, 0x80003400, true));
    w(0x8000310C, B(0x8000310C, 0x80003500, true));
    w(0x80003110, kBlr);
    w(0x80003200, D(15, 4, 0, -0x3400));   // lis r4,0xCC00
    w(0x80003204, D(14, 4, 4, 0x6C00));    // addi r4,r4,0x6C00
    w(0x80003208, D(36, 3, 4, 8));         // stw r3,8(r4)    -> 0xCC006C08
    w(0x8000320C, D(32, 3, 4, 12));        // lwz r3,12(r4)   -> 0xCC006C0C
    w(0x80003210, kBlr);
    w(0x80003300, X(31, 3, 9, 0, 467));    // mtctr r3
    w(0x80003304, D(14, 4, 4, 1));         // addi r4,r4,1
    w(0x80003308, (16u << 26) | (16u << 21) | 0xFFFC);   // bdnz 0x80003304
    w(0x8000330C, kBlr);
    w(0x80003400, D(14, 3, 0, 1));         // li r3,1
    w(0x80003404, kBlr);
    w(0x80003500, kRfi);
    put32(0x00, 0x100);                    // text section 0: file offset, address, size
    put32(0x48, base);
    put32(0x90, textSize);
    put32(0xE0, 0x80003100);               // entry
    std::FILE* f = std::fopen(argv[1], "wb");
    if (!f || std::fwrite(file.data(), 1, file.size(), f) != file.size()) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    std::fclose(f);
    return 0;
}
