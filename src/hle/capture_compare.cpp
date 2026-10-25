#include "capture_compare.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "capture_parse.h"
#include "hle_common.h"

namespace awl::hle::capture {

namespace {

std::string sf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

std::string joinPath(const std::string& dir, const char* name) {
    if (dir.empty()) return name;
    const char c = dir.back();
    return dir + ((c == '/' || c == '\\') ? "" : "/") + name;
}

struct Blob {
    std::string path;
    bool present = false;
    std::vector<uint8_t> data;
};

bool loadBlob(const std::string& path, Blob& b) {
    b.path = path;
    b.present = false;
    b.data.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.data.insert(b.data.end(), buf, buf + n);
    std::fclose(f);
    b.present = true;
    return true;
}

std::string asText(const Blob& b) { return std::string(b.data.begin(), b.data.end()); }

bool memRead(const Blob& b, uint32_t addr, uint32_t size, uint64_t& v) {
    const uint32_t seg = addr >> 28;
    if (!b.present || (seg != 0 && seg != 8 && seg != 0xC)) return false;
    const uint64_t off = addr & 0x0FFFFFFFu;
    if (off + size > b.data.size()) return false;
    v = 0;
    for (uint32_t i = 0; i < size; ++i) v = v << 8 | b.data[static_cast<size_t>(off) + i];
    return true;
}

std::string hx(uint64_t v, uint32_t size) { return sf("%0*llX", static_cast<int>(size * 2), static_cast<unsigned long long>(v)); }

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string sizeText(const Blob& b) { return b.present ? sf("present, %llu bytes", static_cast<unsigned long long>(b.data.size())) : "not captured"; }

// Number of bytes of `a` at guest address `addr` in the dump that differ from `ref`; first difference offset in `first`.
// Returns false if the range is not inside the dump.
bool compareRange(const Blob& dump, uint32_t addr, const std::vector<uint8_t>& ref, size_t& differing, size_t& first) {
    const uint32_t seg = addr >> 28;
    if (!dump.present || (seg != 8 && seg != 0xC)) return false;
    const uint64_t off = addr & 0x0FFFFFFFu;
    if (off + ref.size() > dump.data.size()) return false;
    differing = 0;
    first = 0;
    for (size_t i = 0; i < ref.size(); ++i)
        if (dump.data[static_cast<size_t>(off) + i] != ref[i]) { if (!differing) first = i; ++differing; }
    return true;
}

// Low-memory ranges of the report.
bool inReportRange(uint32_t a) { return (a >= 0x80000000u && a <= 0x800000FFu) || (a >= 0x800030C0u && a <= 0x800030FFu); }

struct Stats {
    size_t n = 0;
    double mean = 0, median = 0, mn = 0, mx = 0;
};
Stats statsOf(std::vector<double> v) {
    Stats s;
    s.n = v.size();
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    s.median = v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
    s.mn = v.front();
    s.mx = v.back();
    return s;
}

std::string periodLine(const char* who, const Stats& s, double fieldMs) {
    if (s.n == 0) return sf("- %s: not enough end-of-frame events (need at least two)\n", who);
    return sf("- %s: %zu intervals, median %.3f ms (%.2f Hz), mean %.3f ms, min %.3f, max %.3f; median is %.2f field periods of %.4f ms\n", who, s.n, s.median,
              1000.0 / s.median, s.mean, s.mn, s.mx, s.median / fieldMs, fieldMs);
}

const char* tvFormatName(uint32_t f) {
    switch (f) {
    case 0: return "NTSC";
    case 1: return "PAL";
    case 2: return "MPAL";
    case 3: return "DEBUG";
    case 4: return "DEBUG_PAL";
    case 5: return "EURGB60";
    default: return "unknown";
    }
}
const char* tvScanName(uint32_t m) {
    switch (m) {
    case 0: return "interlaced";
    case 1: return "double-strike";
    case 2: return "progressive";
    default: return "unknown";
    }
}

std::string fmtMail(uint32_t v) { return sf("%08X", v); }

struct DiffOp {
    char kind;       // '=', '<' (only in Dolphin), '>' (only in ours)
    size_t a, b;     // indices into the sequences
};

// Longest-common-subsequence alignment of two address sequences.
std::vector<DiffOp> alignSequences(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<std::vector<uint16_t>> dp(n + 1, std::vector<uint16_t>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;) dp[i][j] = a[i] == b[j] ? static_cast<uint16_t>(dp[i + 1][j + 1] + 1) : std::max(dp[i + 1][j], dp[i][j + 1]);
    std::vector<DiffOp> ops;
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a[i] == b[j]) { ops.push_back({'=', i, j}); ++i; ++j; }
        else if (i < n && (j >= m || dp[i + 1][j] >= dp[i][j + 1])) { ops.push_back({'<', i, j}); ++i; }
        else { ops.push_back({'>', i, j}); ++j; }
    }
    return ops;
}

std::string callName(uint32_t addr) {
    const char* n = watchedName(addr);
    return n ? sf("%s (%08X)", n, addr) : sf("%08X", addr);
}

}  // namespace

std::string buildReport(const CompareInputs& in) {
    std::string r;
    const BootValues& v = in.values;

    // ---- inputs -----------------------------------------------------------------------------------------------------------
    Blob aEntry, aOs, aNotesB, bDump, bNotesB, bFrames, cCalls, dDsp, trace, hostEntry, hostOs, bootBin, bi2Bin, fstBin;
    loadBlob(joinPath(in.captureDir, "A-at-entry-mem1.raw"), aEntry);
    loadBlob(joinPath(in.captureDir, "A-after-osinit-mem1.raw"), aOs);
    loadBlob(joinPath(in.captureDir, "A-notes.txt"), aNotesB);
    loadBlob(joinPath(in.captureDir, "B-viconfigure-mem1.raw"), bDump);
    loadBlob(joinPath(in.captureDir, "B-notes.txt"), bNotesB);
    loadBlob(joinPath(in.captureDir, "B-frames.log"), bFrames);
    loadBlob(joinPath(in.captureDir, "C-calls.log"), cCalls);
    loadBlob(joinPath(in.captureDir, "D-dsp.log"), dDsp);
    loadBlob(in.tracePath, trace);
    loadBlob(joinPath(in.hostDumpDir, "at-entry-mem1.raw"), hostEntry);
    loadBlob(joinPath(in.hostDumpDir, "after-osinit-mem1.raw"), hostOs);
    loadBlob(joinPath(in.discDir, "sys/boot.bin"), bootBin);
    loadBlob(joinPath(in.discDir, "sys/bi2.bin"), bi2Bin);
    loadBlob(joinPath(in.discDir, "sys/fst.bin"), fstBin);

    const Notes notesA = parseNotes(asText(aNotesB));
    const Notes notesB = parseNotes(asText(bNotesB));
    const OurTrace ours = trace.present ? parseOurTrace(asText(trace)) : OurTrace{};
    std::vector<std::string> parseNotesOut;   // malformed-line summary, written at the end

    r += "# Dolphin capture comparison\n\n";
    r += "Generated by `awl-capture-compare` from the files below. Dolphin is a black-box reference: its dumps are read as data. ";
    r += "Only the compared fields are printed here; no memory contents beyond them are quoted.\n\n";
    r += "| Input | Status |\n|---|---|\n";
    r += sf("| %s | %s |\n", "capture A, memory at the game program entry (A-at-entry-mem1.raw)", sizeText(aEntry).c_str());
    r += sf("| %s | %s |\n", "capture A, memory after OSInit (A-after-osinit-mem1.raw)", sizeText(aOs).c_str());
    r += sf("| %s | %s |\n", "capture A notes (A-notes.txt)", sizeText(aNotesB).c_str());
    r += sf("| %s | %s |\n", "capture B, memory at VIConfigure (B-viconfigure-mem1.raw)", sizeText(bDump).c_str());
    r += sf("| %s | %s |\n", "capture B notes (B-notes.txt)", sizeText(bNotesB).c_str());
    r += sf("| %s | %s |\n", "capture B, frame log (B-frames.log)", sizeText(bFrames).c_str());
    r += sf("| %s | %s |\n", "capture C, call log (C-calls.log)", sizeText(cCalls).c_str());
    r += sf("| %s | %s |\n", "capture D, DSP log (D-dsp.log)", sizeText(dDsp).c_str());
    r += sf("| %s | %s |\n", "platform boot trace", trace.present ? sf("present, %zu call, %zu mail, %zu retrace lines", ours.calls.size(), ours.mails.size(), ours.retraceTicks.size()).c_str() : "not found (run awl-host)");
    r += sf("| %s | %s |\n", "platform memory at entry (host-dumps/at-entry-mem1.raw)", hostEntry.present ? "present" : "not present (run awl-host with --dump-boot)");
    r += sf("| %s | %s |\n", "platform memory after OSInit (host-dumps/after-osinit-mem1.raw)", hostOs.present ? "present" : "not present (see dump_host_memory.ps1)");
    r += sf("| %s | %s |\n\n", "disc sys files (boot.bin, bi2.bin, fst.bin)", bootBin.present && bi2Bin.present && fstBin.present ? "present" : "not all present");

    // ---- layout -------------------------------------------------------------------------------------------------------------
    BootLayout layout;
    bool layoutOk = false;
    if (bootBin.present && bootBin.data.size() >= 0x430) {
        layout = computeLayout(v, be32(&bootBin.data[0x42C]));
        layoutOk = true;
    }

    // ---- Q1 / Q5 ------------------------------------------------------------------------------------------------------------
    r += "## Q1 and Q5: boot memory\n\n";
    if (!aEntry.present && !aOs.present) {
        r += "Capture A not captured: no memory dumps to compare. The assumed values are listed so the owner can see what is being tested.\n\n";
    }
    if (!layoutOk) r += "`disc\\sys\\boot.bin` is missing or short: the FST and BI2 placement values cannot be derived and are shown as `?`.\n\n";
    r += "Fields of the low-memory areas 0x80000000-0x800000FF and 0x800030C0-0x800030FF that the platform assumes or relies on. ";
    r += "*Dolphin at entry* is the memory when the game program starts (breakpoint 80003154), *after OSInit* the memory at 80003268. ";
    r += "A `*` after the second value means OSInit changed the field. *Platform* is awl-host's own memory after OSInit, for the fields the OS code writes itself.\n\n";
    r += "| Address | Field | Kind | Assumed | Dolphin at entry | Dolphin after OSInit | Platform after OSInit | Verdict |\n|---|---|---|---|---|---|---|---|\n";
    struct Change { std::string member; std::string from, to; std::string field; };
    std::vector<Change> changes;
    size_t nMatch = 0, nDiffer = 0, nUnknown = 0;
    std::vector<BootField> fields = bootFields(v, layout);
    std::sort(fields.begin(), fields.end(), [](const BootField& a, const BootField& b) { return a.addr < b.addr; });
    for (const BootField& f : fields) {
        if (!inReportRange(f.addr)) continue;
        std::string assumed = "-", cap = "-", capOs = "-", plat = "-", verdict;
        const bool derivedFromLayout = f.addr == 0x80000038 || f.addr == 0x8000003C || f.addr == 0x800000F4;
        if (f.kind == FieldKind::FromDisc) {
            assumed = "disc header";
            std::vector<uint8_t> ref;
            if (bootBin.present && bootBin.data.size() >= f.size) ref.assign(bootBin.data.begin(), bootBin.data.begin() + f.size);
            size_t d = 0, first = 0;
            if (!ref.empty() && aEntry.present && compareRange(aEntry, f.addr, ref, d, first)) {
                cap = d ? sf("%zu of %u bytes differ", d, f.size) : "equal";
                verdict = d ? "DIFFERS from sys/boot.bin" : "match";
                (d ? nDiffer : nMatch)++;
            } else { cap = aEntry.present ? "unreadable" : "not captured"; verdict = "not compared"; ++nUnknown; }
            r += sf("| %08X | %s | disc | %s | %s | - | - | %s |\n", f.addr, f.name, assumed.c_str(), cap.c_str(), verdict.c_str());
            continue;
        }
        if (f.kind == FieldKind::Assumed) assumed = (derivedFromLayout && !layoutOk) ? "?" : hx(f.value, f.size);
        uint64_t x = 0, y = 0, z = 0;
        const bool haveEntry = memRead(aEntry, f.addr, f.size, x);
        const bool haveOs = memRead(aOs, f.addr, f.size, y);
        const bool havePlat = memRead(hostOs, f.addr, f.size, z);
        if (haveEntry) cap = hx(x, f.size);
        else if (aEntry.present) cap = "unreadable";
        else cap = "not captured";
        if (haveOs) capOs = hx(y, f.size) + ((haveEntry && x != y) ? "*" : "");
        else if (aOs.present) capOs = "unreadable";
        else capOs = "not captured";
        if (havePlat) plat = hx(z, f.size);
        else plat = hostOs.present ? "unreadable" : "not present";
        if (f.kind == FieldKind::Assumed) {
            if (derivedFromLayout && !layoutOk) { verdict = "not compared (no disc)"; ++nUnknown; }
            else if (haveEntry) {
                if (x == f.value) { verdict = "match"; ++nMatch; }
                else {
                    verdict = "DIFFERS: set " + hx(x, f.size);
                    ++nDiffer;
                    changes.push_back({f.member, hx(f.value, f.size), hx(x, f.size), f.name});
                }
            } else { verdict = "not compared"; ++nUnknown; }
        } else {   // OsWritten
            assumed = "(OS writes it)";
            if (haveOs && havePlat) {
                if (y == z) { verdict = "match"; ++nMatch; }
                else { verdict = "DIFFERS: platform " + hx(z, f.size) + ", Dolphin " + hx(y, f.size); ++nDiffer; }
            } else { verdict = haveOs ? "no platform dump" : "not compared"; ++nUnknown; }
        }
        r += sf("| %08X | %s | %s | %s | %s | %s | %s | %s |\n", f.addr, f.name, f.kind == FieldKind::Assumed ? "assumed" : "OS-written", assumed.c_str(), cap.c_str(),
                capOs.c_str(), plat.c_str(), verdict.c_str());
    }
    r += sf("\nSummary: %zu match, %zu differ, %zu not compared.\n\n", nMatch, nDiffer, nUnknown);
    if (!changes.empty()) {
        r += "Table changes this capture asks for (edit `src/hle/boot_table.cpp`, nothing else):\n\n";
        for (const Change& c : changes) r += sf("- %s: %s -> %s (%s)\n", c.field.c_str(), c.from.c_str(), c.to.c_str(), c.member.c_str());
        r += "\n";
    }

    // Words that are non-zero in the capture but not covered by any table field.
    for (int which = 0; which < 2; ++which) {
        const Blob& b = which == 0 ? aEntry : aOs;
        if (!b.present) continue;
        std::vector<std::string> extra;
        for (uint32_t a = 0x80000020; a < 0x80003100; a += 4) {
            if (!inReportRange(a)) { if (a == 0x80000100) a = 0x800030BC; continue; }
            bool covered = false;
            for (const BootField& f : fields) if (f.kind != FieldKind::FromDisc && a + 4 > f.addr && a < f.addr + f.size) covered = true;
            if (covered) continue;
            uint64_t w = 0;
            if (memRead(b, a, 4, w) && w != 0) extra.push_back(sf("%08X=%08llX", a, static_cast<unsigned long long>(w)));
        }
        r += sf("Non-zero words %s that the table does not model (%zu): ", which == 0 ? "at entry" : "after OSInit", extra.size());
        if (extra.empty()) r += "none";
        for (size_t i = 0; i < extra.size() && i < 48; ++i) r += (i ? ", " : "") + extra[i];
        if (extra.size() > 48) r += ", ...";
        r += "\n\n";
    }

    // BI2 and FST placement.
    r += "### BI2 and FST placement\n\n";
    if (!aEntry.present) r += "Capture A not captured.\n\n";
    else {
        uint64_t fstPtr = 0, bi2Ptr = 0, fstLen = 0;
        const bool okF = memRead(aEntry, 0x80000038, 4, fstPtr), okB = memRead(aEntry, 0x800000F4, 4, bi2Ptr), okL = memRead(aEntry, 0x8000003C, 4, fstLen);
        r += sf("- FST address: Dolphin %s, platform rule %s\n", okF ? hx(fstPtr, 4).c_str() : "unreadable", layoutOk ? hx(layout.fstAddr, 4).c_str() : "?");
        r += sf("- BI2 address: Dolphin %s, platform rule %s\n", okB ? hx(bi2Ptr, 4).c_str() : "unreadable", layoutOk ? hx(layout.bi2Addr, 4).c_str() : "?");
        if (okF && okB && okL) {
            const char* order = bi2Ptr + v.bi2Size == fstPtr ? "BI2 directly below the FST" : (bi2Ptr > fstPtr ? "BI2 above the FST" : "BI2 below the FST with a gap");
            r += sf("- Dolphin layout: %s; FST end %08llX (platform assumes the FST ends at RAM top %08X)\n", order, static_cast<unsigned long long>(fstPtr + ((fstLen + 31) & ~31ull)), v.ramTop);
        }
        if (okF && fstBin.present) {
            size_t d = 0, first = 0;
            if (compareRange(aEntry, static_cast<uint32_t>(fstPtr), fstBin.data, d, first))
                r += d ? sf("- FST contents at the Dolphin address: DIFFER from sys/fst.bin in %zu of %zu bytes (first at +%zu)\n", d, fstBin.data.size(), first)
                       : sf("- FST contents at the Dolphin address: identical to sys/fst.bin (%zu bytes)\n", fstBin.data.size());
            else r += "- FST contents: the Dolphin address is outside the dump\n";
        }
        if (okB && bi2Bin.present) {
            size_t d = 0, first = 0;
            if (compareRange(aEntry, static_cast<uint32_t>(bi2Ptr), bi2Bin.data, d, first))
                r += d ? sf("- BI2 contents at the Dolphin address: DIFFER from sys/bi2.bin in %zu of %zu bytes (first at +%zu)\n", d, bi2Bin.data.size(), first)
                       : sf("- BI2 contents at the Dolphin address: identical to sys/bi2.bin (%zu bytes)\n", bi2Bin.data.size());
            else r += "- BI2 contents: the Dolphin address is outside the dump\n";
        }
        r += "\n";
    }

    // Arena words after OSInit (the boot words the SDK reads back) and the drive-information buffer.
    r += "### Arena words after OSInit\n\n";
    if (!aOs.present) r += "Capture A (after OSInit) not captured.\n\n";
    else {
        uint64_t lo = 0, hi = 0;
        if (memRead(aOs, 0x80000030, 4, lo) && memRead(aOs, 0x80000034, 4, hi))
            r += sf("- arena low word 80000030: %08llX; arena high word 80000034: %08llX (platform assumes %s and %s before OSInit; the SDK keeps its own arena globals, whose addresses are not modelled here)\n\n",
                    static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi), hx(v.arenaLo, 4).c_str(), hx(v.arenaHi, 4).c_str());
        else r += "unreadable\n\n";
    }
    r += "### Drive-information buffer\n\n";
    if (!ours.haveDriveInfoAddr) r += "The platform trace has no `dvd inquiry done addr=` line, so the buffer address is unknown.\n\n";
    else {
        r += sf("Buffer address from the platform trace: %08X (DVDDriveInfo: revision, device code, release date).\n\n", ours.driveInfoAddr);
        r += sf("- platform table: revision %04X, device code %04X, release date %08X\n", v.driveRevision & 0xFFFF, v.driveDeviceCode & 0xFFFF, v.driveReleaseDate);
        const struct { const Blob* b; const char* what; } srcs[] = {{&bDump, "Dolphin at VIConfigure (capture B)"}, {&aOs, "Dolphin after OSInit (capture A)"}};
        for (const auto& s : srcs) {
            uint64_t rev = 0, code = 0, date = 0;
            if (memRead(*s.b, ours.driveInfoAddr, 2, rev) && memRead(*s.b, ours.driveInfoAddr + 2, 2, code) && memRead(*s.b, ours.driveInfoAddr + 4, 4, date))
                r += sf("- %s: revision %04llX, device code %04llX, release date %08llX -> %s\n", s.what, static_cast<unsigned long long>(rev), static_cast<unsigned long long>(code),
                        static_cast<unsigned long long>(date),
                        (rev == (v.driveRevision & 0xFFFF) && code == (v.driveDeviceCode & 0xFFFF) && date == v.driveReleaseDate) ? "match" : "DIFFERS (update driveRevision / driveDeviceCode / driveReleaseDate)");
            else r += sf("- %s: %s\n", s.what, s.b->present ? "address outside the dump" : "not captured");
        }
        r += "\n";
    }

    // ---- Q2 / Q3 ------------------------------------------------------------------------------------------------------------
    r += "## Q2 and Q3: frame period and render mode\n\n";
    const double fieldMs = 1000.0 * static_cast<double>(kFieldTicks) / static_cast<double>(kTickRate);
    const BpParse framesLog = bFrames.present ? parseBpLog(asText(bFrames)) : BpParse{};
    std::vector<BpHit> frameHits;
    for (const BpHit& h : framesLog.hits) if (h.addr == 0x8017A7F8) frameHits.push_back(h);
    unwrapTimes(frameHits);

    r += "### Frame period\n\n";
    if (!bFrames.present) r += "Capture B frame log not captured.\n\n";
    else {
        std::vector<double> deltas;
        const BpHit* prev = nullptr;
        for (const BpHit& h : frameHits) {
            if (h.hasTime && prev) deltas.push_back((h.seconds - prev->seconds) * 1000.0);
            prev = h.hasTime ? &h : prev;
        }
        r += sf("- end-of-frame events (8017A7F8) found in B-frames.log: %zu", frameHits.size());
        if (frameHits.empty()) r += " (no usable lines; check the parse notes at the end)";
        r += "\n";
        r += periodLine("Dolphin, all intervals", statsOf(deltas), fieldMs);
        if (deltas.size() >= 10) r += periodLine("Dolphin, without the first 3 intervals", statsOf(std::vector<double>(deltas.begin() + 3, deltas.end())), fieldMs);
        else if (!frameHits.empty() && deltas.empty()) r += "- the lines carry no readable time stamp, so no period can be derived\n";
    }
    {
        std::vector<double> od;
        uint64_t prevIc = 0;
        bool have = false;
        for (const OurCall& c : ours.calls) {
            if (c.callee != 0x8017A7F8) continue;
            if (have) od.push_back(static_cast<double>(c.icount - prevIc) / static_cast<double>(kInstructionsPerTick) * 1000.0 / static_cast<double>(kTickRate));
            prevIc = c.icount;
            have = true;
        }
        r += periodLine("platform (instruction-count time base)", statsOf(od), fieldMs);
        if (ours.retraceTicks.size() >= 2) {
            std::vector<double> rd;
            for (size_t i = 1; i < ours.retraceTicks.size(); ++i) rd.push_back(static_cast<double>(ours.retraceTicks[i] - ours.retraceTicks[i - 1]) * 1000.0 / static_cast<double>(kTickRate));
            r += periodLine("platform vertical retraces", statsOf(rd), fieldMs);
        }
        if (!trace.present) r += "- platform trace not found\n";
        else if (od.empty()) r += "- the platform trace holds no end-of-frame call (run awl-host with --calls)\n";
    }
    r += "\nDolphin paces its output in real time, so only the ratio to the field period is meaningful. A median near one field period means the game finishes a frame per retrace; near two, one frame per two retraces.\n\n";

    r += "### Time base at the program entry\n\n";
    if (notesA.hasTbu && notesA.hasTbl) r += sf("- Dolphin (A-notes.txt): TBU %08X, TBL %08X. The platform time base starts at 0 when the entry runs; the SDK only uses differences, so a non-zero start matters only if a value is hashed or stored.\n\n", notesA.tbu, notesA.tbl);
    else r += "- not captured (A-notes.txt has no TBU/TBL)\n\n";

    r += "### Render mode\n\n";
    if (!notesB.hasR3) r += "B-notes.txt has no r3 value (or the file is missing): not captured.\n\n";
    else if (!bDump.present) r += sf("r3 = %08X, but the memory dump B-viconfigure-mem1.raw is not captured.\n\n", notesB.r3);
    else {
        uint64_t tv = 0, fbw = 0, efbh = 0, xfbh = 0, xo = 0, yo = 0, viw = 0, vih = 0, xfbMode = 0, field = 0, aa = 0;
        const uint32_t o = notesB.r3;
        const bool ok = memRead(bDump, o, 4, tv) && memRead(bDump, o + 4, 2, fbw) && memRead(bDump, o + 6, 2, efbh) && memRead(bDump, o + 8, 2, xfbh) && memRead(bDump, o + 10, 2, xo) &&
                        memRead(bDump, o + 12, 2, yo) && memRead(bDump, o + 14, 2, viw) && memRead(bDump, o + 16, 2, vih) && memRead(bDump, o + 20, 4, xfbMode) &&
                        memRead(bDump, o + 24, 1, field) && memRead(bDump, o + 25, 1, aa);
        if (!ok) r += sf("r3 = %08X is outside the dump.\n\n", notesB.r3);
        else {
            const uint32_t fmt = static_cast<uint32_t>(tv >> 2), scan = static_cast<uint32_t>(tv & 3);
            r += sf("Decoded as a VIRenderModeObj at %08X (r3 of VIConfigure):\n\n", notesB.r3);
            r += sf("- TV format %s (%u), scan %s (%u); EFB %llux%llu, XFB height %llu, VI origin (%llu,%llu), VI size %llux%llu, XFB mode %s, field rendering %llu, anti-aliasing %llu\n",
                    tvFormatName(fmt), fmt, tvScanName(scan), scan, static_cast<unsigned long long>(fbw), static_cast<unsigned long long>(efbh), static_cast<unsigned long long>(xfbh),
                    static_cast<unsigned long long>(xo), static_cast<unsigned long long>(yo), static_cast<unsigned long long>(viw), static_cast<unsigned long long>(vih),
                    xfbMode == 0 ? "single field" : (xfbMode == 1 ? "double field" : "unknown"), static_cast<unsigned long long>(field), static_cast<unsigned long long>(aa));
            r += sf("- platform assumption: TV mode word at 800000CC = %u (%s) -> %s\n", v.tvMode, tvFormatName(v.tvMode), fmt == v.tvMode ? "match" : "DIFFERS (set tvMode)");
            r += sf("- platform VI model: NTSC, field period %.4f ms (%llu ticks) -> %s\n", fieldMs, static_cast<unsigned long long>(kFieldTicks),
                    fmt == 0 || fmt == 5 ? "consistent with a 59.94 Hz field rate" : "the model has no timing for this format; extend the VI model before using it");
            if (fbw == 0 || fbw > 1024 || efbh == 0 || efbh > 1024 || fmt > 5) r += "- warning: the values look implausible for a render-mode object; r3 may not point at one\n";
            r += "\n";
        }
    }

    // ---- Q6 -----------------------------------------------------------------------------------------------------------------
    r += "## Q6: order and arguments of the first system calls\n\n";
    BpParse cParse;
    if (!cCalls.present) r += "Capture C not captured.\n\n";
    else {
        cParse = parseBpLog(asText(cCalls));
        unwrapTimes(cParse.hits);
        std::vector<const BpHit*> dh;
        size_t unlisted = 0;
        for (const BpHit& h : cParse.hits) { if (isCaptureC(h.addr)) dh.push_back(&h); else ++unlisted; }
        std::vector<const OurCall*> oh;
        for (const OurCall& c : ours.calls) if (isCaptureC(c.callee)) oh.push_back(&c);
        r += sf("Dolphin: %zu logged hits of the listed functions (%zu lines at other addresses ignored). Platform: %zu recorded calls of the same functions", dh.size(), unlisted, oh.size());
        r += trace.present ? ". The platform trace records calls up to the depth given to `awl-host --calls`; deeper calls are missing.\n\n" : " (trace not found).\n\n";
        r += "| Function | Dolphin hits | Platform calls |\n|---|---|---|\n";
        for (uint32_t i = 0; i < kWatchedCount; ++i) {
            size_t a = 0, b = 0;
            for (const BpHit* h : dh) a += h->addr == kWatched[i].addr;
            for (const OurCall* c : oh) b += c->callee == kWatched[i].addr;
            if ((a || b) && kWatched[i].captureC) r += sf("| %s | %zu | %zu |\n", callName(kWatched[i].addr).c_str(), a, b);
        }
        // Sequence alignment over the first K events of each side.
        const size_t K = 150;
        std::vector<uint32_t> sa, sb;
        for (size_t i = 0; i < dh.size() && i < K; ++i) sa.push_back(dh[i]->addr);
        for (size_t i = 0; i < oh.size() && i < K; ++i) sb.push_back(oh[i]->callee);
        r += sf("\nAlignment of the first %zu Dolphin events and the first %zu platform events (longest common subsequence; both sides run for different lengths, so a tail difference alone is not a mismatch):\n\n", sa.size(), sb.size());
        if (sa.empty() || sb.empty()) r += "Nothing to align.\n\n";
        else {
            const std::vector<DiffOp> ops = alignSequences(sa, sb);
            size_t same = 0, shown = 0;
            size_t run = 0;
            auto flush = [&]() { if (run) { r += sf("- %zu calls in the same order\n", run); run = 0; } };
            for (const DiffOp& op : ops) {
                if (op.kind == '=') { ++same; ++run; continue; }
                flush();
                if (shown++ >= 80) continue;
                if (op.kind == '<') { r += sf("- only in Dolphin: %s (hit %zu, line %zu)\n", callName(sa[op.a]).c_str(), op.a + 1, dh[op.a]->lineNo); }
                else { r += sf("- only in the platform: %s (call %zu, instruction %llu)\n", callName(sb[op.b]).c_str(), op.b + 1, static_cast<unsigned long long>(oh[op.b]->icount)); }
            }
            flush();
            size_t totalD = 0, totalO = 0;
            for (const DiffOp& op : ops) { totalD += op.kind == '<'; totalO += op.kind == '>'; }
            r += sf("\nResult: %zu in common, %zu only in Dolphin, %zu only in the platform%s.\n\n", same, totalD, totalO, shown > 80 ? " (list truncated at 80 differences)" : "");
        }
        r += "First hit of each function (Dolphin arguments r3-r6 and caller; the platform trace records no registers, so its arguments are not compared yet):\n\n";
        r += "| Function | Dolphin r3 r4 r5 r6 | Dolphin LR | Dolphin t (s from the first hit) | Platform first call: instruction / call site |\n|---|---|---|---|---|\n";
        double t0 = 0;
        bool haveT0 = false;
        for (const BpHit* h : dh) if (h->hasTime) { t0 = h->seconds; haveT0 = true; break; }
        for (uint32_t i = 0; i < kWatchedCount; ++i) {
            const BpHit* d = nullptr;
            for (const BpHit* h : dh) if (h->addr == kWatched[i].addr) { d = h; break; }
            const OurCall* o = nullptr;
            for (const OurCall* c : oh) if (c->callee == kWatched[i].addr) { o = c; break; }
            if (!d && !o) continue;
            std::string args = "-";
            if (d) {
                args.clear();
                for (size_t k = 0; k < 4 && k < d->args.size(); ++k) args += (k ? " " : "") + fmtMail(d->args[k]);
                if (args.empty()) args = "(none printed)";
            }
            r += sf("| %s | %s | %s | %s | %s |\n", callName(kWatched[i].addr).c_str(), args.c_str(), d && d->hasLr ? fmtMail(d->lr).c_str() : "-",
                    d && d->hasTime && haveT0 ? sf("%.3f", d->seconds - t0).c_str() : "-",
                    o ? sf("%llu / %08X", static_cast<unsigned long long>(o->icount), o->site).c_str() : "-");
        }
        r += "\n";
    }

    // ---- DSP / AX -----------------------------------------------------------------------------------------------------------
    r += "## DSP and AX handshake\n\n";
    BpParse dParse;
    if (!dDsp.present) r += "Capture D not captured.\n\n";
    else {
        dParse = parseBpLog(asText(dDsp));
        std::vector<uint32_t> sends, recvs;
        for (const BpHit& h : dParse.hits) {
            if (h.args.empty()) continue;
            if (h.addr == 0x801C3D34) sends.push_back(h.args[0]);
            else if (h.addr == 0x801C3D30) recvs.push_back(h.args[0]);
        }
        std::vector<uint32_t> oSends, oRecvs;
        for (const OurMail& m : ours.mails) (m.toDsp ? oSends : oRecvs).push_back(m.value);
        size_t babe = 0, resume = 0;
        for (uint32_t x : sends) babe += (x >> 16) == 0xBABE;
        for (uint32_t x : recvs) resume += x == 0xDCD10001;
        r += sf("Dolphin: %zu messages sent to the DSP (801C3D34), %zu read from it (801C3D30). Platform: %zu sent, %zu delivered.\n\n", sends.size(), recvs.size(), oSends.size(), oRecvs.size());
        static const uint32_t kCmds[5] = {0x80F3A001, 0x80F3C002, 0x80F3A002, 0x80F3B002, 0x80F3D001};
        bool first = !recvs.empty() && recvs[0] == 0x8071FEED;
        bool init = recvs.size() > 1 && recvs[1] == 0xDCD10000;
        size_t cmdOk = 0;
        for (size_t i = 0; i < 5; ++i) if (2 * i < sends.size() && sends[2 * i] == kCmds[i]) ++cmdOk;
        r += "Expected exchange (docs/research/hle-platform.md, section 5.4): first message from the DSP 8071FEED; five command/data pairs starting 80F3A001, 80F3C002, 80F3A002, 80F3B002, 80F3D001; the answer DCD10000; then one BABE message plus a list address per audio frame, each answered DCD10001.\n\n";
        r += sf("- first message from the DSP is 8071FEED: %s\n", recvs.empty() ? "no messages read" : (first ? "yes" : "NO"));
        r += sf("- five command words at the start of the sends: %zu of 5 in place\n", cmdOk);
        r += sf("- DCD10000 as the second read: %s\n", recvs.size() < 2 ? "not enough messages" : (init ? "yes" : "NO"));
        r += sf("- BABE messages sent: %zu; DCD10001 messages read: %zu%s\n\n", babe, resume, babe && resume + 1 < babe ? " (fewer answers than frames: check the log)" : "");
        auto table = [&](const char* title, const std::vector<uint32_t>& d, const std::vector<uint32_t>& o) {
            r += sf("%s (first %zu):\n\n| # | Dolphin | Platform | |\n|---|---|---|---|\n", title, std::min<size_t>(std::max(d.size(), o.size()), 24));
            size_t diff = 0;
            for (size_t i = 0; i < 24 && (i < d.size() || i < o.size()); ++i) {
                const bool both = i < d.size() && i < o.size();
                const bool eq = both && d[i] == o[i];
                if (both && !eq) ++diff;
                r += sf("| %zu | %s | %s | %s |\n", i, i < d.size() ? fmtMail(d[i]).c_str() : "-", i < o.size() ? fmtMail(o[i]).c_str() : "-", both ? (eq ? "=" : "differs") : "");
            }
            r += sf("\n%zu of the shown rows differ.\n\n", diff);
        };
        table("Messages the game sends", sends, oSends);
        table("Messages the game reads", recvs, oRecvs);
    }

    // ---- parse notes --------------------------------------------------------------------------------------------------------
    r += "## Parse notes\n\n";
    auto logNote = [&](const char* name, const Blob& b, const BpParse& p) {
        if (!b.present) return;
        r += sf("- %s: %zu lines, %zu breakpoint hits, %zu other lines ignored, %zu breakpoint lines unreadable\n", name, p.lines, p.hits.size(), p.ignored, p.malformed.size());
        for (size_t i = 0; i < p.malformed.size() && i < 10; ++i) r += "  - " + p.malformed[i] + "\n";
    };
    logNote("B-frames.log", bFrames, framesLog);
    logNote("C-calls.log", cCalls, cParse);
    logNote("D-dsp.log", dDsp, dParse);
    const struct { const char* name; const Blob* b; const Notes* n; } noteFiles[] = {{"A-notes.txt", &aNotesB, &notesA}, {"B-notes.txt", &bNotesB, &notesB}};
    for (const auto& nf : noteFiles) {
        if (!nf.b->present) continue;
        r += sf("- %s: %zu lines without a readable value\n", nf.name, nf.n->unreadable.size());
        for (size_t i = 0; i < nf.n->unreadable.size() && i < 10; ++i) r += "  - " + nf.n->unreadable[i] + "\n";
    }
    if (trace.present) {
        r += sf("- boot-trace.log: %zu unreadable call/mail/retrace lines\n", ours.malformed.size());
        for (size_t i = 0; i < ours.malformed.size() && i < 10; ++i) r += "  - " + ours.malformed[i] + "\n";
    }
    r += "\nThe time stamps, breakpoint layout and register order of the Dolphin log are assumed from the capture procedure; if a line is listed as unreadable, send it and the parser will be extended.\n";
    return r;
}

}  // namespace awl::hle::capture
