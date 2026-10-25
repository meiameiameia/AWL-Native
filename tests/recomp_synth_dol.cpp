// Writes a tiny synthetic DOL and REL (hand-assembled, no game bytes) used by the end-to-end generator tests:
//   recomp_synth_dol <out-dir>      writes <out-dir>/synth.dol and <out-dir>/wlp0.rel
//
// DOL, all text at 0x80003100 (strings in a data section at 0x80004000):
//   0x80003100 entry       calls every function below so discovery reaches them
//   0x80003200 mmio        lis/addi build 0xCC006C00; stw r3,8(r4); lwz r3,12(r4); blr      (MMIO-only evidence)
//   0x80003300 loop        mtctr r3; L: addi r4,r4,1; bdnz L; blr                           (register-only backward loop)
//   0x80003400 hle         li r3,1; blr                                                     (replaced via the manifest)
//   0x80003500 priv        rfi                                                              (privileged evidence)
//   0x80003600 relocLink   references "OSLink: unknown relocation type"        (stand-ins for the SDK's OSLink chain:
//   0x80003620 link        calls relocLink, returns 1                           OSLink -> Link -> relocation routine)
//   0x80003660 osLink      li r5,0; calls link
//   0x80003700 relocUnlink references "OSUnlink: unknown relocation type"
//   0x80003740 osUnlink    calls relocUnlink, returns 1
//   0x80003800 dolDouble   add r3,r3,r3; blr                                                (called from the REL by REL24)
//
// REL (module id 1, version 3; sections 1 text, 2 data, 3 bss), offsets within the text section:
//   0x10 prolog   stores 7 at data+0 through a lis/addi pair relocated against its own data section
//   0x30 epilog   li r3,5; blr
//   0x40 calls    r3 = dolDouble(internal(r3)): an internal bl and a REL24 into the DOL
//   0x80 internal addi r3,r3,10; blr
//   0x90 addr     r3 = address of data+8 (lis/addi, ADDR16_HA/LO against the module)
//   0xA0 vcall    calls through the function pointer in data+0x10 (ADDR32 to 0x80)
//   0xD0 switch   jump table in data+0x18 (ADDR32 to 0xF0 and 0xF8): 0 -> 100, 1 -> 200, else 300
//   data+0x20     ADDR32 pointers to calls, addr, vcall and switch (so discovery reaches them)
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr uint32_t D(uint32_t op, uint32_t rt, uint32_t ra, int32_t imm) {
    return (op << 26) | (rt << 21) | (ra << 16) | (static_cast<uint32_t>(imm) & 0xFFFF);
}
constexpr uint32_t X(uint32_t op, uint32_t f1, uint32_t f2, uint32_t f3, uint32_t xo) {
    return (op << 26) | (f1 << 21) | (f2 << 16) | (f3 << 11) | (xo << 1);
}
constexpr uint32_t M(uint32_t op, uint32_t s, uint32_t a, uint32_t sh, uint32_t mb, uint32_t me) {
    return (op << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1);
}
constexpr uint32_t B(uint32_t from, uint32_t to, bool link) {
    return (18u << 26) | ((to - from) & 0x03FFFFFCu) | (link ? 1u : 0u);
}
constexpr uint32_t kBlr = (19u << 26) | (20u << 21) | (16u << 1);
constexpr uint32_t kRfi = (19u << 26) | (50u << 1);
constexpr uint32_t kBctr = (19u << 26) | (20u << 21) | (528u << 1);
constexpr uint32_t kBctrl = kBctr | 1u;
constexpr uint32_t kMflrR0 = 0x7C0802A6, kMtlrR0 = 0x7C0803A6;
constexpr uint32_t kStwR0 = D(36, 0, 1, 4), kStwuR1 = D(37, 1, 1, -16), kLwzR0 = D(32, 0, 1, 20), kAddiR1 = D(14, 1, 1, 16);

struct Buf {
    std::vector<uint8_t> b;
    explicit Buf(size_t n) : b(n, 0) {}
    void put32(uint32_t off, uint32_t v) {
        b[off] = static_cast<uint8_t>(v >> 24); b[off + 1] = static_cast<uint8_t>(v >> 16);
        b[off + 2] = static_cast<uint8_t>(v >> 8); b[off + 3] = static_cast<uint8_t>(v);
    }
    void put16(uint32_t off, uint32_t v) { b[off] = static_cast<uint8_t>(v >> 8); b[off + 1] = static_cast<uint8_t>(v); }
};

bool save(const std::string& path, const std::vector<uint8_t>& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

bool writeDol(const std::string& path) {
    constexpr uint32_t base = 0x80003100, textSize = 0x740 + 0x100;
    constexpr uint32_t strBase = 0x80004000, strSize = 0x80;
    Buf f(0x100 + textSize + strSize);
    auto w = [&](uint32_t addr, uint32_t v) { f.put32(0x100 + (addr - base), v); };
    // entry: call everything (never executed by the tests)
    uint32_t a = 0x80003100;
    for (uint32_t target : {0x80003200u, 0x80003300u, 0x80003400u, 0x80003500u, 0x80003660u, 0x80003740u, 0x80003800u}) {
        w(a, B(a, target, true));
        a += 4;
    }
    w(a, kBlr);
    // MMIO-only function
    w(0x80003200, D(15, 4, 0, -0x3400));   // lis r4,0xCC00
    w(0x80003204, D(14, 4, 4, 0x6C00));    // addi r4,r4,0x6C00
    w(0x80003208, D(36, 3, 4, 8));         // stw r3,8(r4)    -> 0xCC006C08
    w(0x8000320C, D(32, 3, 4, 12));        // lwz r3,12(r4)   -> 0xCC006C0C
    w(0x80003210, kBlr);
    // register-only loop
    w(0x80003300, X(31, 3, 9, 0, 467));    // mtctr r3
    w(0x80003304, D(14, 4, 4, 1));         // addi r4,r4,1
    w(0x80003308, (16u << 26) | (16u << 21) | 0xFFFC);   // bdnz 0x80003304
    w(0x8000330C, kBlr);
    // HLE function and privileged function
    w(0x80003400, D(14, 3, 0, 1));         // li r3,1
    w(0x80003404, kBlr);
    w(0x80003500, kRfi);
    // OSLink / OSUnlink stand-ins. The strings are referenced through lis/addi.
    w(0x80003600, D(15, 3, 0, -0x8000));   // lis r3,0x8000
    w(0x80003604, D(14, 3, 3, 0x4000));    // addi r3,r3,0x4000 -> "OSLink: ..."
    w(0x80003608, kBlr);
    Buf* fp = &f;
    auto pro = [&](uint32_t at) { fp->put32(0x100 + (at - base), kMflrR0); fp->put32(0x100 + (at - base) + 4, kStwR0); fp->put32(0x100 + (at - base) + 8, kStwuR1); };
    auto epi = [&](uint32_t at) { w(at, kLwzR0); w(at + 4, kAddiR1); w(at + 8, kMtlrR0); w(at + 12, kBlr); };
    pro(0x80003620);                                   // link: bl relocLink; li r3,1
    w(0x8000362C, B(0x8000362C, 0x80003600, true));
    w(0x80003630, D(14, 3, 0, 1));
    epi(0x80003634);
    pro(0x80003660);                                   // osLink: li r5,0; bl link
    w(0x8000366C, D(14, 5, 0, 0));
    w(0x80003670, B(0x80003670, 0x80003620, true));
    epi(0x80003674);
    w(0x80003700, D(15, 3, 0, -0x8000));   // lis r3,0x8000
    w(0x80003704, D(14, 3, 3, 0x4040));    // addi r3,r3,0x4040 -> "OSUnlink: ..."
    w(0x80003708, kBlr);
    pro(0x80003740);                                   // osUnlink: bl relocUnlink; li r3,1
    w(0x8000374C, B(0x8000374C, 0x80003700, true));
    w(0x80003750, D(14, 3, 0, 1));
    epi(0x80003754);
    w(0x80003800, X(31, 3, 3, 3, 266));    // add r3,r3,r3
    w(0x80003804, kBlr);
    const char* s1 = "OSLink: unknown relocation type %3d";
    const char* s2 = "OSUnlink: unknown relocation type %3d";
    for (uint32_t i = 0; s1[i]; ++i) f.b[0x100 + textSize + i] = static_cast<uint8_t>(s1[i]);
    for (uint32_t i = 0; s2[i]; ++i) f.b[0x100 + textSize + 0x40 + i] = static_cast<uint8_t>(s2[i]);
    f.put32(0x00, 0x100);                  // text section 0
    f.put32(0x48, base);
    f.put32(0x90, textSize);
    f.put32(0x1C, 0x100 + textSize);       // data section 7: the strings
    f.put32(0x64, strBase);
    f.put32(0xAC, strSize);
    f.put32(0xE0, 0x80003100);             // entry
    return save(path, f.b);
}

bool writeRel(const std::string& path) {
    constexpr uint32_t kText = 0x80, kTextLen = 0x120, kData = 0x1A0, kDataLen = 0x40, kImports = 0x1E0, kStreamDol = 0x1F0;
    Buf f(0x300);
    auto t = [&](uint32_t off, uint32_t v) { f.put32(kText + off, v); };
    // text
    t(0x10, D(15, 4, 0, 0));       // lis r4,0          HA(data+0)
    t(0x14, D(14, 4, 4, 0));       // addi r4,r4,0      LO(data+0)
    t(0x18, D(14, 5, 0, 7));       // li r5,7
    t(0x1C, D(36, 5, 4, 0));       // stw r5,0(r4)
    t(0x20, kBlr);
    t(0x30, D(14, 3, 0, 5));       // epilog: li r3,5
    t(0x34, kBlr);
    t(0x40, kMflrR0); t(0x44, kStwR0); t(0x48, kStwuR1);
    t(0x4C, B(0x4C, 0x80, true));  // bl internal (PC-relative inside the module)
    t(0x50, B(0, 0, true));        // bl dolDouble: REL24 against the DOL patches this word
    t(0x54, kLwzR0); t(0x58, kAddiR1); t(0x5C, kMtlrR0); t(0x60, kBlr);
    t(0x80, D(14, 3, 3, 10));      // internal: addi r3,r3,10
    t(0x84, kBlr);
    t(0x90, D(15, 3, 0, 0));       // addr: lis r3,0    HA(data+8)
    t(0x94, D(14, 3, 3, 0));       //       addi r3,r3,0  LO(data+8)
    t(0x98, kBlr);
    t(0xA0, kMflrR0); t(0xA4, kStwR0); t(0xA8, kStwuR1);
    t(0xAC, D(15, 4, 0, 0));       // lis r4,0          HA(data+0x10)
    t(0xB0, D(32, 12, 4, 0));      // lwz r12,0(r4)     LO(data+0x10)
    t(0xB4, X(31, 12, 9, 0, 467)); // mtctr r12
    t(0xB8, kBctrl);
    t(0xBC, kLwzR0); t(0xC0, kAddiR1); t(0xC4, kMtlrR0); t(0xC8, kBlr);
    t(0xD0, D(10, 0, 3, 1));       // switch: cmplwi r3,1
    t(0xD4, (16u << 26) | (12u << 21) | (1u << 16) | 0x2C);   // bgt 0x100
    t(0xD8, D(15, 4, 0, 0));       // lis r4,0          HA(data+0x18)
    t(0xDC, D(14, 4, 4, 0));       // addi r4,r4,0      LO(data+0x18)
    t(0xE0, M(21, 3, 0, 2, 0, 29));        // slwi r0,r3,2
    t(0xE4, X(31, 0, 4, 0, 23));           // lwzx r0,r4,r0
    t(0xE8, X(31, 0, 9, 0, 467));          // mtctr r0
    t(0xEC, kBctr);
    t(0xF0, D(14, 3, 0, 100)); t(0xF4, kBlr);
    t(0xF8, D(14, 3, 0, 200)); t(0xFC, kBlr);
    t(0x100, D(14, 3, 0, 300)); t(0x104, kBlr);
    // header
    f.put32(0x00, 1);              // module id
    f.put32(0x0C, 4);              // sections
    f.put32(0x10, 0x4C);           // section table
    f.put32(0x1C, 3);              // version
    f.put32(0x20, 0x20);           // bss size
    f.put32(0x24, kStreamDol);     // relocation data
    f.put32(0x28, kImports);
    f.put32(0x2C, 16);             // two imports
    f.b[0x30] = 1; f.b[0x31] = 1; f.b[0x32] = 0;   // prolog/epilog sections; no unresolved handler
    f.put32(0x34, 0x10);           // prolog
    f.put32(0x38, 0x30);           // epilog
    f.put32(0x3C, 0);
    f.put32(0x40, 32);             // align
    f.put32(0x44, 32);             // bss align
    f.put32(0x48, 0);              // fix size
    // section table: 0 unused, 1 text, 2 data, 3 bss
    f.put32(0x4C + 8, kText | 1); f.put32(0x4C + 12, kTextLen);
    f.put32(0x4C + 16, kData);    f.put32(0x4C + 20, kDataLen);
    f.put32(0x4C + 24, 0);        f.put32(0x4C + 28, 0x20);
    // imports: module 0 (the DOL) and module 1 (itself)
    constexpr uint32_t kStreamSelf = kStreamDol + 24;
    f.put32(kImports, 0);  f.put32(kImports + 4, kStreamDol);
    f.put32(kImports + 8, 1); f.put32(kImports + 12, kStreamSelf);
    auto rec = [&](uint32_t off, uint32_t delta, uint32_t type, uint32_t section, uint32_t addend) {
        f.put16(off, delta); f.b[off + 2] = static_cast<uint8_t>(type); f.b[off + 3] = static_cast<uint8_t>(section); f.put32(off + 4, addend);
    };
    // against the DOL: section 1, REL24 at text+0x50 to dolDouble
    rec(kStreamDol, 0, 202, 1, 0);
    rec(kStreamDol + 8, 0x50, 10, 0, 0x80003800);
    rec(kStreamDol + 16, 0, 203, 0, 0);
    // against itself: address halves in text (section 1), then ADDR32 words in data (section 2)
    uint32_t p = kStreamSelf;
    rec(p, 0, 202, 1, 0); p += 8;
    uint32_t prev = 0;
    auto half = [&](uint32_t at, uint32_t type, uint32_t addend) { rec(p, at - prev, type, 2, addend); prev = at; p += 8; };
    half(0x12, 6, 0x00); half(0x16, 4, 0x00);
    half(0x92, 6, 0x08); half(0x96, 4, 0x08);
    half(0xAE, 6, 0x10); half(0xB2, 4, 0x10);
    half(0xDA, 6, 0x18); half(0xDE, 4, 0x18);
    rec(p, 0, 202, 2, 0); p += 8;
    prev = 0;
    auto word = [&](uint32_t at, uint32_t textOffset) { rec(p, at - prev, 1, 1, textOffset); prev = at; p += 8; };
    word(0x10, 0x80); word(0x18, 0xF0); word(0x1C, 0xF8);
    word(0x20, 0x40); word(0x24, 0x90); word(0x28, 0xA0); word(0x2C, 0xD0);   // function table: makes the entry points reachable
    rec(p, 0, 203, 0, 0);
    return save(path, f.b);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: recomp_synth_dol <out-dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    if (!writeDol(dir + "/synth.dol") || !writeRel(dir + "/wlp0.rel")) {
        std::fprintf(stderr, "cannot write into %s\n", dir.c_str());
        return 1;
    }
    return 0;
}
