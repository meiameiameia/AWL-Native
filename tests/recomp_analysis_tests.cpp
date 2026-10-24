// Analysis tests for the static-recompilation spike: function discovery, jump-table resolution and
// platform evidence on a tiny program assembled by hand (no game bytes). Words are built from the
// architecture's instruction formats, as in recomp_decoder_tests.cpp.
#include <cstdio>
#include <cstdint>
#include <vector>

#include "analysis.h"

using namespace awl::recomp;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                             \
    } while (0)

constexpr uint32_t D(uint32_t op, uint32_t rt, uint32_t ra, int32_t imm) {
    return (op << 26) | (rt << 21) | (ra << 16) | (static_cast<uint32_t>(imm) & 0xFFFF);
}
constexpr uint32_t X(uint32_t op, uint32_t f1, uint32_t f2, uint32_t f3, uint32_t xo, uint32_t rc = 0) {
    return (op << 26) | (f1 << 21) | (f2 << 16) | (f3 << 11) | (xo << 1) | rc;
}
constexpr uint32_t M(uint32_t op, uint32_t s, uint32_t a, uint32_t sh, uint32_t mb, uint32_t me) {
    return (op << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1);
}
constexpr uint32_t B(uint32_t from, uint32_t to, bool link) {
    return (18u << 26) | ((to - from) & 0x03FFFFFCu) | (link ? 1u : 0u);
}
constexpr uint32_t kBlr = (19u << 26) | (20u << 21) | (16u << 1);
constexpr uint32_t kBctr = (19u << 26) | (20u << 21) | (528u << 1);

void put(std::vector<uint8_t>& f, uint32_t off, uint32_t v) {
    f[off] = static_cast<uint8_t>(v >> 24);
    f[off + 1] = static_cast<uint8_t>(v >> 16);
    f[off + 2] = static_cast<uint8_t>(v >> 8);
    f[off + 3] = static_cast<uint8_t>(v);
}

struct Fixture {
    DolImage dol;
    Fixture() {
        const uint32_t base = 0x80003100, textSize = 0x108;
        dol.file.assign(textSize + 32, 0);
        auto w = [&](uint32_t addr, uint32_t v) { put(dol.file, addr - base, v); };
        w(0x80003100, 0x7C0802A6);                                   // mflr r0
        w(0x80003104, D(37, 1, 1, -16));                             // stwu r1,-16(r1)
        w(0x80003108, D(10, 0, 3, 3));                               // cmplwi r3,3
        w(0x8000310C, (16u << 26) | (12u << 21) | (1u << 16) | 0x24);   // bgt 0x80003130
        w(0x80003110, D(15, 4, 0, -0x8000));                         // lis r4,0x8000
        w(0x80003114, D(14, 4, 4, 0x4000));                          // addi r4,r4,0x4000  -> table 0x80004000
        w(0x80003118, M(21, 3, 0, 2, 0, 29));                        // slwi r0,r3,2
        w(0x8000311C, X(31, 0, 4, 0, 23));                           // lwzx r0,r4,r0
        w(0x80003120, X(31, 0, 9, 0, 467));                          // mtctr r0
        w(0x80003124, kBctr);
        w(0x80003128, D(14, 3, 0, 1));                               // case 0: li r3,1
        w(0x8000312C, D(14, 3, 0, 2));                               // case 1: li r3,2
        w(0x80003130, D(14, 3, 0, 3));                               // case 2 / default: li r3,3
        w(0x80003134, D(14, 3, 0, 4));                               // case 3: li r3,4
        w(0x80003138, B(0x80003138, 0x80003200, true));              // bl 0x80003200
        w(0x8000313C, D(32, 0, 1, 20));                              // lwz r0,20(r1)
        w(0x80003140, 0x7C0803A6);                                   // mtlr r0
        w(0x80003144, D(14, 1, 1, 16));                              // addi r1,r1,16
        w(0x80003148, kBlr);
        w(0x80003200, kBlr);                                         // second function (callee)
        // jump table in a data section: four case targets, then an extra text pointer that must NOT be
        // included because the compare bounds the table at 4 entries
        const uint32_t d = textSize;
        put(dol.file, d + 0, 0x80003128);
        put(dol.file, d + 4, 0x8000312C);
        put(dol.file, d + 8, 0x80003130);
        put(dol.file, d + 12, 0x80003134);
        put(dol.file, d + 16, 0x80003138);
        DolSection t;
        t.index = 0; t.text = true; t.fileOffset = 0; t.addr = base; t.size = textSize;
        DolSection ds;
        ds.index = 7; ds.text = false; ds.fileOffset = textSize; ds.addr = 0x80004000; ds.size = 32;
        dol.sections = {t, ds};
        dol.entry = 0x80003100;
    }
};

void testJumpTable() {
    Fixture f;
    const Program prog = buildProgram(f.dol);
    const IndirectJump j = classifyBctr(prog, 0x80003100, 0x80003124);
    CHECK(j.kind == IndirectKind::JumpTableBounded);
    CHECK(j.table == 0x80004000);
    CHECK(j.count == 4);   // cmplwi 3 + bgt: indices 0..3
    CHECK(j.targets.size() == 4 && j.targets[0] == 0x80003128 && j.targets[3] == 0x80003134);
}

void testDiscovery() {
    Fixture f;
    const Program prog = buildProgram(f.dol);
    const Discovery d = discoverFunctions(prog, DiscoveryInput{});
    CHECK(d.funcs.count(0x80003100) == 1);
    CHECK(d.funcs.count(0x80003200) == 1);
    CHECK(d.stats.functions == 2);
    const Function& fn = d.funcs.at(0x80003100);
    CHECK(fn.body.size() == 19);                       // 0x80003100 .. 0x80003148 inclusive
    CHECK(fn.calls.size() == 1 && fn.calls[0] == 0x80003200);
    CHECK(fn.seeds & SeedEntry);
    CHECK(d.funcs.at(0x80003200).seeds & SeedBl);
    CHECK(d.stats.assignedBytes == 19 * 4 + 4);
    CHECK(d.stats.unassignedBytes == 0);               // everything else in the section is zero padding
    CHECK(d.bctrUnique.size() == 1 && d.bctrUnique[0].kind == IndirectKind::JumpTableBounded);
    // The case labels are inside the body, so the table's text pointers do not create new functions.
    CHECK(d.stats.dataInsideBody >= 4);
    CHECK(d.stats.overlapWords == 0);
}

void testPlatformEvidence() {
    // A function that loads 0xCC006C00 (audio interface block) and reads a register: MMIO evidence.
    std::vector<uint8_t> file(0x40, 0);
    DolImage dol;
    const uint32_t base = 0x80003100;
    put(file, 0, D(15, 3, 0, -0x3400));      // lis r3,0xCC00
    put(file, 4, D(14, 3, 3, 0x6C00));       // addi r3,r3,0x6C00
    put(file, 8, D(32, 4, 3, 8));            // lwz r4,8(r3)  -> 0xCC006C08
    put(file, 12, X(31, 5, 0, 0, 146));      // mtmsr r5 (modelled supervisor state)
    put(file, 16, kBlr);
    DolSection t;
    t.index = 0; t.text = true; t.fileOffset = 0; t.addr = base; t.size = 0x40;
    dol.file = file;
    dol.sections = {t};
    dol.entry = base;
    const Program prog = buildProgram(dol);
    const Discovery d = discoverFunctions(prog, DiscoveryInput{});
    const Function& fn = d.funcs.at(base);
    CHECK(fn.platform.mmio.count("AI") == 1 && fn.platform.mmio.at("AI") == 1);
    CHECK(fn.platform.supervisorState == 1 && fn.platform.privileged == 0);
    CHECK(fn.platform.any());
    CHECK(std::string(mmioBlockName(0xCC006C08)) == "AI");
    CHECK(std::string(mmioBlockName(0xCC008000)) == "GXFIFO");
    CHECK(std::string(mmioBlockName(0xCC002000)) == "VI");
    CHECK(mmioBlockName(0x80000000) == nullptr);
}

}  // namespace

int main() {
    testJumpTable();
    testDiscovery();
    testPlatformEvidence();
    std::printf("recomp analysis tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
