// Tests for the HLE manifest reader and for the generator's handling of manifest entries, on a tiny
// program assembled by hand (no game bytes). The accepted and rejected forms follow the format documented
// in the header of tools/recomp/hle_manifest.txt.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "analysis.h"
#include "codegen.h"
#include "manifest.h"

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

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

bool anyContains(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v) if (contains(s, needle)) return true;
    return false;
}

struct Parsed {
    std::vector<HleEntry> entries;
    std::vector<std::string> errors;
    bool ok = false;
};
Parsed parse(const std::string& text) {
    Parsed p;
    p.ok = parseHleManifest(text, "m.txt", p.entries, p.errors);
    return p;
}

void testParserAccepts() {
    // The header's own example shape; comment lines, blank lines, CRLF endings and tabs are all fine.
    Parsed p = parse(
        "# header comment\r\n"
        "\n"
        "dol 0x801B3AC0 OSExampleFunction  # writes PI 0xCC003000, called once from the boot path\r\n"
        "dol\t0x80000100\tOSFirst\t# lowest address, listed after the higher one\n"
        "   # indented comment\n"
        "dol 0X80003000 __OSUpper_1 # uppercase 0X and underscores\n");
    CHECK(p.ok && p.errors.empty());
    CHECK(p.entries.size() == 3);
    // Entries come back sorted by address.
    CHECK(p.entries[0].addr == 0x80000100 && p.entries[0].name == "OSFirst" && p.entries[0].line == 4);
    CHECK(p.entries[1].addr == 0x80003000 && p.entries[1].name == "__OSUpper_1");
    CHECK(p.entries[2].addr == 0x801B3AC0 && p.entries[2].name == "OSExampleFunction" && p.entries[2].line == 3);
    CHECK(p.entries[2].evidence == "writes PI 0xCC003000, called once from the boot path");

    // An empty manifest (nothing but comments and blank lines, or no text at all) is valid.
    CHECK(parse("").ok && parse("").entries.empty());
    CHECK(parse("# only a header\n\n   \n").ok && parse("# only a header\n\n   \n").entries.empty());
    // The last line need not end with a newline.
    CHECK(parse("dol 0x80003000 F # e").ok);
}

void testParserRejects() {
    struct Case {
        const char* text;
        const char* message;   // substring of the expected diagnostic
    };
    const Case cases[] = {
        {"rel 0x80003000 F # e\n", "unsupported module 'rel'"},
        {"main 0x80003000 F # e\n", "unsupported module 'main'"},
        {"dol 0x80003000\n", "expected '<module> <address> <name>"},
        {"dol\n", "expected '<module> <address> <name>"},
        {"dol 80003000 F # e\n", "bad address '80003000'"},
        {"dol 0x F # e\n", "bad address '0x'"},
        {"dol 0x8000300G F # e\n", "bad address '0x8000300G'"},
        {"dol 0x123456789 F # e\n", "bad address '0x123456789'"},
        {"dol 0x80003002 F # e\n", "not 4-byte aligned"},
        {"dol 0x80003000 9Bad # e\n", "bad function name '9Bad'"},
        {"dol 0x80003000 bad-name # e\n", "bad function name 'bad-name'"},
        {"dol 0x80003000 F\n", "evidence must follow a '#'"},
        {"dol 0x80003000 F extra # e\n", "evidence must follow a '#'"},
        {"dol 0x80003000 F #\n", "missing evidence"},
        {"dol 0x80003000 F #    \n", "missing evidence"},
    };
    for (const Case& c : cases) {
        Parsed p = parse(c.text);
        CHECK(!p.ok);
        CHECK(p.errors.size() == 1);
        CHECK(anyContains(p.errors, c.message));
        CHECK(!p.errors.empty() && p.errors[0].rfind("m.txt:1: ", 0) == 0);   // source and line number prefix
        CHECK(p.entries.empty());
    }

    // Duplicate addresses are rejected and name the first line; the duplicate's name does not matter.
    Parsed d = parse("dol 0x80003000 A # e\n\ndol 0x80003000 B # e\n");
    CHECK(!d.ok && d.errors.size() == 1 && contains(d.errors[0], "m.txt:3: ") && contains(d.errors[0], "duplicate address 0x80003000") &&
          contains(d.errors[0], "line 1"));
    // Every problem is reported, not just the first.
    Parsed m = parse("rel 0x80003000 A # e\ndol 0x80003004 B\ndol 0x80003008 C # ok\n");
    CHECK(!m.ok && m.errors.size() == 2 && contains(m.errors[0], "m.txt:1:") && contains(m.errors[1], "m.txt:2:"));
}

// ---- A tiny program: 0x80003100 calls 0x80003200 (HLE candidate) which calls 0x80003300 ----------------------
constexpr uint32_t kBlr = (19u << 26) | (20u << 21) | (16u << 1);
constexpr uint32_t B(uint32_t from, uint32_t to, bool link) { return (18u << 26) | ((to - from) & 0x03FFFFFCu) | (link ? 1u : 0u); }
constexpr uint32_t LI(uint32_t rt, int32_t imm) { return (14u << 26) | (rt << 21) | (static_cast<uint32_t>(imm) & 0xFFFF); }

void put(std::vector<uint8_t>& f, uint32_t off, uint32_t v) {
    f[off] = static_cast<uint8_t>(v >> 24);
    f[off + 1] = static_cast<uint8_t>(v >> 16);
    f[off + 2] = static_cast<uint8_t>(v >> 8);
    f[off + 3] = static_cast<uint8_t>(v);
}

struct Tiny {
    DolImage dol;
    Tiny() {
        const uint32_t base = 0x80003100, size = 0x320;
        dol.file.assign(size, 0);
        auto w = [&](uint32_t addr, uint32_t v) { put(dol.file, addr - base, v); };
        w(0x80003100, B(0x80003100, 0x80003200, true));
        w(0x80003104, B(0x80003104, 0x80003400, true));
        w(0x80003108, kBlr);
        w(0x80003200, B(0x80003200, 0x80003300, true));
        w(0x80003204, LI(3, 1));
        w(0x80003208, kBlr);
        w(0x80003300, LI(3, 2));
        w(0x80003304, kBlr);
        // 0x80003400: a counted loop with a backward conditional branch: li r3,5; addi r3,r3,-1; cmpwi r3,0; bne -8; blr
        w(0x80003400, LI(3, 5));
        w(0x80003404, (14u << 26) | (3u << 21) | (3u << 16) | 0xFFFF);
        w(0x80003408, (11u << 26) | (3u << 16));
        w(0x8000340C, (16u << 26) | (4u << 21) | (2u << 16) | 0xFFF8);
        w(0x80003410, kBlr);
        DolSection t;
        t.index = 0; t.text = true; t.fileOffset = 0; t.addr = base; t.size = size;
        dol.sections = {t};
        dol.entry = 0x80003100;
    }
};

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string readAllGenerated(const std::filesystem::path& dir) {
    std::string all;
    std::vector<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator(dir)) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    for (const auto& n : names) if (n.rfind("gen_", 0) == 0) all += readFile(dir / n);
    return all;
}

void testValidation() {
    Tiny t;
    const Program prog = buildProgram(t.dol);
    const Discovery d = discoverFunctions(prog, DiscoveryInput{});
    CHECK(d.funcs.count(0x80003200) == 1 && d.funcs.count(0x80003300) == 1);

    std::vector<std::string> errors;
    CHECK(validateHleManifest(parse("dol 0x80003200 Callee # test\n").entries, d, "m.txt", errors) && errors.empty());
    CHECK(validateHleManifest({}, d, "m.txt", errors) && errors.empty());   // an empty manifest is allowed

    // An address inside a function, one in padding, and one outside the text are not function starts.
    for (const char* line : {"dol 0x80003204 Mid # e\n", "dol 0x80003240 Padding # e\n", "dol 0x80000000 Outside # e\n"}) {
        errors.clear();
        const Parsed p = parse(line);
        CHECK(p.ok);
        CHECK(!validateHleManifest(p.entries, d, "m.txt", errors));
        CHECK(errors.size() == 1 && contains(errors[0], "m.txt:1: ") && contains(errors[0], "is not a discovered DOL function start"));
    }
}

// Two functions in different 32 KB address windows: 0x80003100 calls 0x8000B100 (each `li r3,N; blr`).
struct TwoWindows {
    DolImage dol;
    TwoWindows() {
        dol.file.assign(0x40, 0);
        put(dol.file, 0x00, B(0x80003100, 0x8000B100, true));
        put(dol.file, 0x04, kBlr);
        put(dol.file, 0x20, LI(3, 2));
        put(dol.file, 0x24, kBlr);
        DolSection a, b;
        a.index = 0; a.text = true; a.fileOffset = 0; a.addr = 0x80003100; a.size = 0x20;
        b.index = 1; b.text = true; b.fileOffset = 0x20; b.addr = 0x8000B100; b.size = 0x20;
        dol.sections = {a, b};
        dol.entry = 0x80003100;
    }
};

void testIncrementalOutput() {
    namespace fs = std::filesystem;
    TwoWindows t;
    const Program prog = buildProgram(t.dol);
    const Discovery d = discoverFunctions(prog, DiscoveryInput{});
    const fs::path out = fs::temp_directory_path() / "awl_recomp_incremental_test";
    fs::remove_all(out);
    CHECK(generateCode(prog, d, {}, out.string(), true) == 0);
    // Chunks are named by address window, not by order.
    CHECK(fs::exists(out / "gen_80000000.cpp") && fs::exists(out / "gen_80008000.cpp") && fs::exists(out / "sources.cmake"));
    std::ofstream(out / "gen_stale.cpp") << "// left over from an earlier run\n";
    std::ofstream(out / "notes.txt") << "not generated output\n";
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(1);
    const char* all[] = {"gen_80000000.cpp", "gen_80008000.cpp", "gen_dispatch.cpp", "gen_decls.h", "sources.cmake"};
    for (const char* n : all) fs::last_write_time(out / n, old);

    // Regenerating with nothing changed rewrites nothing, so file timestamps survive; the stale chunk goes away.
    CHECK(generateCode(prog, d, {}, out.string(), true) == 0);
    for (const char* n : all) CHECK(fs::last_write_time(out / n) == old);
    CHECK(!fs::exists(out / "gen_stale.cpp") && fs::exists(out / "notes.txt"));

    // Replacing one function by an HLE call changes only its own window's chunk and the dispatch table (names).
    const Parsed p = parse("dol 0x8000B100 SecondFn  # synthetic fixture\n");
    CHECK(p.ok);
    CHECK(generateCode(prog, d, p.entries, out.string(), true) == 0);
    CHECK(fs::last_write_time(out / "gen_80008000.cpp") != old);
    CHECK(fs::last_write_time(out / "gen_dispatch.cpp") != old);
    CHECK(fs::last_write_time(out / "gen_80000000.cpp") == old);
    CHECK(fs::last_write_time(out / "gen_decls.h") == old);
    CHECK(fs::last_write_time(out / "sources.cmake") == old);
    fs::remove_all(out);
}

void testEmission() {
    namespace fs = std::filesystem;
    Tiny t;
    const Program prog = buildProgram(t.dol);
    const Discovery d = discoverFunctions(prog, DiscoveryInput{});
    const fs::path root = fs::temp_directory_path() / "awl_recomp_manifest_test";
    fs::remove_all(root);

    // No manifest entries: every function is recompiled and no HLE name table is emitted.
    {
        const fs::path out = root / "empty";
        CHECK(generateCode(prog, d, {}, out.string(), true) == 0);
        const std::string all = readAllGenerated(out);
        CHECK(!contains(all, "hleCall") && !contains(all, "declareHleNames"));
        CHECK(contains(all, "void f_80003200(Ctx& c) {\n    uint32_t ea = 0;"));
        // Preemption checkpoints: the backward conditional branch and the return both record the pc and compare
        // against the deadline; the forward-only entry function has no backward branch and only its return one.
        CHECK(contains(all, "if (!rt::crBit(c, 2)) { rt::at(c, 0x8000340C, 0); goto L_80003404; }"));
        CHECK(contains(all, "rt::at(c, 0x80003410, 0);\n    rt::ret(c, 0x80003410);"));
        CHECK(contains(all, "rt::at(c, 0x80003108, 0);\n    rt::ret(c, 0x80003108);"));
        CHECK(!contains(all, "goto L_80003108"));
    }
    // Whole-DOL mode: the entry's body becomes one hleCall; the callee it used to call is still emitted.
    const Parsed p = parse("dol 0x80003200 TestCallee  # synthetic fixture\n");
    CHECK(p.ok);
    {
        const fs::path out = root / "full";
        CHECK(generateCode(prog, d, p.entries, out.string(), true) == 0);
        const std::string all = readAllGenerated(out);
        CHECK(contains(all, "void f_80003200(Ctx& c) { rt::hleCall(c, 0x80003200); }"));
        CHECK(!contains(all, "void f_80003200(Ctx& c) {\n"));               // not also recompiled
        CHECK(contains(all, "void f_80003300(Ctx& c) {"));                 // unrelated functions stay recompiled
        CHECK(contains(all, "{0x80003200, \"TestCallee\"}") && contains(all, "declareHleNames(kHleNames, 1)"));
        CHECK(contains(all, "#include \"hle_abi.h\""));
    }
    // Boot-path mode: an HLE function is a leaf, so its callees are no longer reached from the entry.
    {
        const fs::path out = root / "boot-hle";
        CHECK(generateCode(prog, d, p.entries, out.string(), false) == 0);
        const std::string all = readAllGenerated(out);
        CHECK(contains(all, "rt::hleCall(c, 0x80003200)"));
        CHECK(!contains(all, "f_80003300"));
    }
    {
        const fs::path out = root / "boot-plain";
        CHECK(generateCode(prog, d, {}, out.string(), false) == 0);
        CHECK(contains(readAllGenerated(out), "void f_80003300(Ctx& c) {"));
    }
    // Output is deterministic: generating twice yields identical files.
    {
        const fs::path again = root / "full2";
        CHECK(generateCode(prog, d, p.entries, again.string(), true) == 0);
        CHECK(readAllGenerated(again) == readAllGenerated(root / "full"));
    }
    fs::remove_all(root);
}

}  // namespace

int main() {
    testParserAccepts();
    testParserRejects();
    testValidation();
    testEmission();
    testIncrementalOutput();
    std::printf("recomp manifest tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
