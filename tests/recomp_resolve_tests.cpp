// Tests for the M2 discovery resolution on programs assembled by hand (no game bytes): the overlap rules, the
// verified jump-table bounds (range exit by bgtlr, counted-loop induction, and the cases that must stay unproven),
// the never-returns analysis and the SHA-256 used for the regression digest. Expected results are derived from the
// instruction semantics and from the SHA-256 test vectors in FIPS 180-4, not from the implementation.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "analysis.h"
#include "codegen.h"
#include "resolve.h"
#include "sha256.h"

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
constexpr uint32_t X(uint32_t op, uint32_t f1, uint32_t f2, uint32_t f3, uint32_t xo) {
    return (op << 26) | (f1 << 21) | (f2 << 16) | (f3 << 11) | (xo << 1);
}
constexpr uint32_t M(uint32_t op, uint32_t s, uint32_t a, uint32_t sh, uint32_t mb, uint32_t me) {
    return (op << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1);
}
constexpr uint32_t B(uint32_t from, uint32_t to, bool link) { return (18u << 26) | ((to - from) & 0x03FFFFFCu) | (link ? 1u : 0u); }
constexpr uint32_t BC(uint32_t bo, uint32_t bi, uint32_t from, uint32_t to) {
    return (16u << 26) | (bo << 21) | (bi << 16) | ((to - from) & 0xFFFCu);
}
constexpr uint32_t kBlr = (19u << 26) | (20u << 21) | (16u << 1);
constexpr uint32_t kBgtlr = (19u << 26) | (12u << 21) | (1u << 16) | (16u << 1);   // bclr 12,1: return when GT
constexpr uint32_t kBctr = (19u << 26) | (20u << 21) | (528u << 1);
constexpr uint32_t kMtctrR0 = X(31, 0, 9, 0, 467);

// A DOL with one text section at 0x80003100 and an optional data section at 0x80004000.
struct Image {
    DolImage dol;
    std::vector<uint32_t> text, data;
    void build() {
        dol.file.assign(text.size() * 4 + data.size() * 4, 0);
        auto put = [&](size_t off, uint32_t v) {
            dol.file[off] = static_cast<uint8_t>(v >> 24); dol.file[off + 1] = static_cast<uint8_t>(v >> 16);
            dol.file[off + 2] = static_cast<uint8_t>(v >> 8); dol.file[off + 3] = static_cast<uint8_t>(v);
        };
        for (size_t i = 0; i < text.size(); ++i) put(i * 4, text[i]);
        for (size_t i = 0; i < data.size(); ++i) put(text.size() * 4 + i * 4, data[i]);
        DolSection t;
        t.index = 0; t.text = true; t.fileOffset = 0; t.addr = 0x80003100; t.size = static_cast<uint32_t>(text.size() * 4);
        dol.sections = {t};
        if (!data.empty()) {
            DolSection ds;
            ds.index = 7; ds.text = false; ds.fileOffset = t.size; ds.addr = 0x80004000; ds.size = static_cast<uint32_t>(data.size() * 4);
            dol.sections.push_back(ds);
        }
        dol.entry = 0x80003100;
    }
    void at(uint32_t addr, uint32_t word) {
        const size_t i = (addr - 0x80003100) / 4;
        if (text.size() <= i) text.resize(i + 1, 0);
        text[i] = word;
    }
};

void testSha256() {
    // FIPS 180-4 / NIST example vectors.
    CHECK(Sha256::of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // One million 'a' fed in odd-sized pieces exercises the block buffering.
    Sha256 h;
    const std::string chunk(997, 'a');
    size_t fed = 0;
    while (fed + chunk.size() <= 1000000) { h.update(chunk); fed += chunk.size(); }
    h.update(std::string(1000000 - fed, 'a'));
    CHECK(h.finish() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// ---- Overlap resolution on a hand-built discovery --------------------------------------------------------------
void testOverlapRules() {
    // Text: A (entry, bl-called style) at 0x80003100 jumps over padding into a tail; B is a gap-seeded fragment that
    // runs into the same tail; C is a function whose straight-line start runs into an undecodable word and is only
    // named by a code constant; E is a bl-seeded function that also hits an undecodable word.
    Image img;
    img.at(0x80003100, D(14, 3, 0, 1));            // A: li r3,1
    img.at(0x80003104, B(0x80003104, 0x80003130, false));   // b tail
    img.at(0x80003110, D(14, 3, 0, 2));            // B: li r3,2   (gap fragment after padding)
    img.at(0x80003114, B(0x80003114, 0x80003130, false));
    img.at(0x80003130, D(14, 3, 3, 5));            // tail: addi r3,r3,5
    img.at(0x80003134, kBlr);
    img.at(0x80003140, D(14, 3, 0, 7));            // C: li r3,7 ; undecodable word
    img.at(0x80003144, 0x00000118);
    img.at(0x80003150, D(14, 3, 0, 8));            // E: li r3,8 ; undecodable word
    img.at(0x80003154, 0x00000194);
    img.build();
    const Program prog = buildProgram(img.dol);
    CHECK(prog.at(0x80003144)->op == Op::invalid);

    Discovery d;
    auto fn = [&](uint32_t start, uint32_t seeds, std::vector<uint32_t> body, uint32_t bad = 0) {
        Function f;
        f.start = start; f.seeds = seeds; f.body = std::move(body); f.badDecodes = bad;
        d.funcs[start] = std::move(f);
    };
    fn(0x80003100, SeedEntry, {0x80003100, 0x80003104, 0x80003130, 0x80003134});
    fn(0x80003110, SeedGap, {0x80003110, 0x80003114, 0x80003130, 0x80003134});
    fn(0x80003140, SeedConst, {0x80003140, 0x80003144}, 1);
    fn(0x80003150, SeedBl, {0x80003150, 0x80003154}, 1);
    const Resolution r = resolveFunctions(prog, d);
    CHECK(r.dropped.size() == 2);
    CHECK(r.isDropped(0x80003110) && r.dropped.at(0x80003110) == DropReason::UnreferencedFragment);
    CHECK(r.isDropped(0x80003140) && r.dropped.at(0x80003140) == DropReason::DataInText);   // straight line to an invalid word
    CHECK(!r.isDropped(0x80003150));                    // a bl-referenced function is never dropped: its stop stays loud
    CHECK(!r.isDropped(0x80003100));
    CHECK(r.overlapFunctionsBefore == 2 && r.overlapWordsBefore == 4);   // A and B share the tail words 0x80003130/34 (2 each)
    CHECK(r.overlapFunctionsAfter == 0 && r.overlapWordsAfter == 0);
    CHECK(r.exclusiveWordsDropped == 4);                               // B: its 2 own words, C: its 2 words; B's tail words stay in A
    // A fragment something refers to stays: give B an inbound tail-call edge and a bl seed.
    d.funcs[0x80003100].tailCalls = {0x80003110};
    CHECK(!resolveFunctions(prog, d).isDropped(0x80003110));
    d.funcs[0x80003100].tailCalls.clear();
    d.funcs[0x80003110].seeds = SeedGap | SeedBl;
    CHECK(!resolveFunctions(prog, d).isDropped(0x80003110));
}

void testJumpTableLabelRule() {
    // S (strong) holds a switch with two case labels L0/L1 listed in a data table; the labels were also seeded from
    // those data words. Both are dropped. L2 is referenced by a data word outside every table, so it stays.
    Image img;
    for (uint32_t a = 0x80003100; a < 0x80003140; a += 4) img.at(a, D(14, 3, 0, 1));
    img.at(0x80003140, kBlr);
    img.data = {0x80003120, 0x80003124, 0x80003128, 0x80003128};   // table (2 entries) then a stray pointer to 0x80003128
    img.build();
    const Program prog = buildProgram(img.dol);
    Discovery d;
    auto fn = [&](uint32_t start, uint32_t seeds, std::vector<uint32_t> body) {
        Function f;
        f.start = start; f.seeds = seeds; f.body = std::move(body);
        d.funcs[start] = std::move(f);
    };
    fn(0x80003100, SeedEntry, {0x80003100, 0x80003120, 0x80003124, 0x80003128, 0x8000312C});
    fn(0x80003120, SeedData, {0x80003120});
    fn(0x80003124, SeedData, {0x80003124});
    fn(0x80003128, SeedData, {0x80003128});
    IndirectJump j;
    j.addr = 0x80003110; j.funcStart = 0x80003100; j.kind = IndirectKind::JumpTableBounded;
    j.table = 0x80004000; j.count = 2; j.targets = {0x80003120, 0x80003124};
    d.indirect.push_back(j);
    // 0x80003128 is not in the table's targets, so it is not even a candidate.
    Resolution r = resolveFunctions(prog, d);
    CHECK(r.isDropped(0x80003120) && r.dropped.at(0x80003120) == DropReason::JumpTableLabel);
    CHECK(r.isDropped(0x80003124) && r.dropped.at(0x80003124) == DropReason::JumpTableLabel);
    CHECK(!r.isDropped(0x80003128));
    // A label that is also named by a data word outside the table (a function pointer elsewhere) must stay.
    d.indirect[0].targets = {0x80003120, 0x80003128};
    d.indirect[0].count = 2;
    r = resolveFunctions(prog, d);
    CHECK(!r.isDropped(0x80003128));   // data words 0x80004008 and 0x8000400C name it; the table covers 0x80004000-0x80004007
    CHECK(r.isDropped(0x80003120));
}

// ---- Verified jump-table bounds ---------------------------------------------------------------------------------
// Builds: [prologue-free] cmplwi/bounds, table dispatch, 3 case labels, and a data table with `extra` more valid text
// pointers after the bounded entries (the next table, as the compiler lays them out).
struct TableCase {
    Image img;
    uint32_t bctr = 0;
};

TableCase scaledTable(uint32_t cmpReg, bool exitIsReturn) {
    TableCase t;
    Image& m = t.img;
    // r3 = index source. r0 = scaled offset.
    uint32_t a = 0x80003100;
    m.at(a, D(10, 0, cmpReg, 2)); a += 4;                              // cmplwi cr0,cmpReg,2 : indices 0..2
    if (exitIsReturn) { m.at(a, kBgtlr); a += 4; }                     // bgtlr
    else { m.at(a, BC(12, 1, a, 0x80003160)); a += 4; }                // bgt default
    m.at(a, D(15, 4, 0, -0x8000)); a += 4;                             // lis r4,0x8000
    m.at(a, D(14, 4, 4, 0x4000)); a += 4;                              // addi r4,r4,0x4000 -> table 0x80004000
    m.at(a, M(21, 3, 0, 2, 0, 29)); a += 4;                            // slwi r0,r3,2
    m.at(a, X(31, 0, 4, 0, 23)); a += 4;                               // lwzx r0,r4,r0
    m.at(a, kMtctrR0); a += 4;
    t.bctr = a;
    m.at(a, kBctr); a += 4;
    m.at(0x80003130, D(14, 3, 0, 10)); m.at(0x80003134, kBlr);         // case 0
    m.at(0x80003138, D(14, 3, 0, 20)); m.at(0x8000313C, kBlr);         // case 1
    m.at(0x80003140, D(14, 3, 0, 30)); m.at(0x80003144, kBlr);         // case 2
    m.at(0x80003150, D(14, 3, 0, 40)); m.at(0x80003154, kBlr);         // the next table's first label
    m.at(0x80003160, kBlr);
    m.data = {0x80003130, 0x80003138, 0x80003140, 0x80003150, 0x80003150};   // 3 entries, then the next table
    m.build();
    return t;
}

void testBoundedTables() {
    // The compare is on r3, which the rlwinm scales: bgt-to-label and bgtlr exits both bound the table at 3 entries
    // even though more valid pointers follow it in the data section.
    for (const bool ret : {false, true}) {
        TableCase t = scaledTable(3, ret);
        const Program prog = buildProgram(t.img.dol);
        const IndirectJump j = classifyBctr(prog, 0x80003100, t.bctr);
        CHECK(j.kind == IndirectKind::JumpTableBounded);
        CHECK(j.count == 3 && j.targets.size() == 3 && j.table == 0x80004000);
    }
    // A compare on a different register is not a bound for this index: the table stays scan-length.
    {
        TableCase t = scaledTable(5, true);
        const Program prog = buildProgram(t.img.dol);
        const IndirectJump j = classifyBctr(prog, 0x80003100, t.bctr);
        CHECK(j.kind == IndirectKind::JumpTableScanned);
        CHECK(j.targets.size() == 5);
    }
}

void testInductionTable() {
    // for (i = 0; i < 3; ++i) switch (i): r5 counts 0..2 (compare/exit at the top, bound r5 <= 2), r7 is the byte offset
    // stepped by 4 with it. The same table with the offset stepped by 8 must not be accepted.
    for (const int step : {4, 8}) {
        Image m;
        m.at(0x80003100, D(14, 5, 0, 0));                  // li r5,0
        m.at(0x80003104, D(14, 7, 0, 0));                  // li r7,0
        m.at(0x80003108, D(10, 0, 5, 2));                  // L: cmplwi r5,2
        m.at(0x8000310C, BC(12, 1, 0x8000310C, 0x80003170));   // bgt done
        m.at(0x80003110, D(15, 8, 0, -0x8000));            // lis r8,0x8000
        m.at(0x80003114, D(14, 8, 8, 0x4000));             // addi r8,r8,0x4000
        m.at(0x80003118, X(31, 0, 8, 7, 23));              // lwzx r0,r8,r7
        m.at(0x8000311C, kMtctrR0);
        m.at(0x80003120, kBctr);
        m.at(0x80003130, D(14, 9, 0, 1)); m.at(0x80003134, B(0x80003134, 0x80003160, false));
        m.at(0x80003138, D(14, 9, 0, 2)); m.at(0x8000313C, B(0x8000313C, 0x80003160, false));
        m.at(0x80003140, D(14, 9, 0, 3)); m.at(0x80003144, B(0x80003144, 0x80003160, false));
        m.at(0x80003150, D(14, 9, 0, 4)); m.at(0x80003154, kBlr);
        m.at(0x80003160, D(14, 5, 5, 1));                  // ++i
        m.at(0x80003164, D(14, 7, 7, step));               // offset += step
        m.at(0x80003168, B(0x80003168, 0x80003108, false));  // back to the loop head
        m.at(0x8000316C, kBlr);
        m.at(0x80003170, kBlr);
        m.data = {0x80003130, 0x80003138, 0x80003140, 0x80003150};
        m.build();
        const Program prog = buildProgram(m.dol);
        const IndirectJump j = classifyBctr(prog, 0x80003100, 0x80003120);
        if (step == 4) {
            CHECK(j.kind == IndirectKind::JumpTableBounded && j.count == 3);
        } else {
            CHECK(j.kind == IndirectKind::JumpTableScanned);   // offset != 4 * counter: unproven
        }
    }
}

// ---- Never-returning functions -----------------------------------------------------------------------------------
void testNoReturn() {
    Image m;
    m.at(0x80003100, B(0x80003100, 0x80003120, true));    // main: bl spin ; blr (returns)
    m.at(0x80003104, kBlr);
    m.at(0x80003120, B(0x80003120, 0x80003120, false));   // spin: b spin (never returns)
    m.at(0x80003130, B(0x80003130, 0x80003120, false));   // forwarder: b spin (tail call to a function that never returns)
    m.at(0x80003140, D(14, 3, 0, 1));                     // plain: li r3,1 ; blr
    m.at(0x80003144, kBlr);
    m.at(0x80003150, B(0x80003150, 0x80003130, true));    // caller of forwarder
    m.at(0x80003154, kBlr);
    m.build();
    const Program prog = buildProgram(m.dol);
    DiscoveryInput in;
    in.extraEntries = {0x80003130, 0x80003140, 0x80003150};
    const Discovery d = discoverFunctions(prog, in);
    const std::set<uint32_t> nr = findNoReturn(prog, d);
    CHECK(nr.count(0x80003120) == 1);   // no return instruction at all
    CHECK(nr.count(0x80003130) == 1);   // its only exit is a tail call into a function that never returns
    CHECK(nr.count(0x80003100) == 0 && nr.count(0x80003140) == 0 && nr.count(0x80003150) == 0);   // each has a blr
    // Generation: a bl to a function that never returns is followed by a loud stop instead of running on.
    namespace fs = std::filesystem;
    const fs::path out = fs::temp_directory_path() / "awl_recomp_noreturn_test";
    fs::remove_all(out);
    CHECK(generateCode(prog, d, {}, out.string(), true) == 0);
    std::string text;
    for (const auto& e : fs::directory_iterator(out)) {
        if (e.path().extension() != ".cpp") continue;
        std::ifstream file(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << file.rdbuf();
        text += ss.str();
    }
    CHECK(text.find("rt::returnedFromNoReturn(c, 0x80003100, 0x80003120);") != std::string::npos);
    CHECK(text.find("rt::returnedFromNoReturn(c, 0x80003150, 0x80003130);") != std::string::npos);   // the forwarder never returns either
    fs::remove_all(out);
}

}  // namespace

int main() {
    testSha256();
    testOverlapRules();
    testJumpTableLabelRule();
    testBoundedTables();
    testInductionTable();
    testNoReturn();
    std::printf("recomp resolve tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
