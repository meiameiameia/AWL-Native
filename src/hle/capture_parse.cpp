#include "capture_parse.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace awl::hle::capture {

const WatchedFunction kWatched[] = {
    {0x801B0DE0, "OSInit", true},
    {0x801BADF0, "DVDInit", true},
    {0x801BE3BC, "VIInit", true},
    {0x801CB378, "GXInit", true},
    {0x801C19D8, "AIInit", true},
    {0x801C3D48, "DSPInit", true},
    {0x801C2018, "ARInit", true},
    {0x801B70D4, "OSCreateThread", true},
    {0x801BA66C, "DVDOpen", true},
    {0x801BC9D8, "DVDReadAbsAsyncPrio", true},
    {0x801BF68C, "VISetNextFrameBuffer", true},
    {0x801CED04, "GXCopyDisp", true},
    {0x801BED34, "VIConfigure", false},
    {0x8017A7F8, "game: end of frame", false},
};
const uint32_t kWatchedCount = sizeof(kWatched) / sizeof(kWatched[0]);

bool isCaptureC(uint32_t addr) {
    for (uint32_t i = 0; i < kWatchedCount; ++i)
        if (kWatched[i].addr == addr) return kWatched[i].captureC;
    return false;
}

const char* watchedName(uint32_t addr) {
    for (uint32_t i = 0; i < kWatchedCount; ++i)
        if (kWatched[i].addr == addr) return kWatched[i].name;
    return nullptr;
}

namespace {

bool isHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
bool isWord(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string clip(const std::string& s) { return s.size() > 120 ? s.substr(0, 120) + "..." : s; }

// 1 to 8 hex digits at s[i], optionally after 0x, ended by a non-word character. Advances i.
bool takeHex(const std::string& s, size_t& i, uint32_t& v) {
    size_t p = i;
    if (p + 1 < s.size() && s[p] == '0' && (s[p + 1] == 'x' || s[p + 1] == 'X')) p += 2;
    size_t n = 0;
    uint64_t acc = 0;
    while (p + n < s.size() && isHex(s[p + n]) && n < 9) {
        const char c = s[p + n];
        acc = acc * 16 + static_cast<uint64_t>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::tolower(static_cast<unsigned char>(c)) - 'a' + 10));
        ++n;
    }
    if (n == 0 || n > 8) return false;
    if (p + n < s.size() && isWord(s[p + n])) return false;
    v = static_cast<uint32_t>(acc);
    i = p + n;
    return true;
}

// Reads the leading time stamp of a line (the text before the breakpoint token). Returns false when there is none.
bool parseTimestamp(const std::string& prefix, double& seconds) {
    size_t i = 0;
    while (i < prefix.size() && (std::isspace(static_cast<unsigned char>(prefix[i])) || prefix[i] == '[')) ++i;
    size_t j = i;
    while (j < prefix.size() && (std::isdigit(static_cast<unsigned char>(prefix[j])) || prefix[j] == ':' || prefix[j] == '.')) ++j;
    if (j == i) return false;
    std::string tok = prefix.substr(i, j - i);
    while (!tok.empty() && (tok.back() == ':' || tok.back() == '.')) tok.pop_back();
    std::vector<std::string> f;
    size_t s = 0;
    for (size_t k = 0; k <= tok.size(); ++k)
        if (k == tok.size() || tok[k] == ':') { f.push_back(tok.substr(s, k - s)); s = k + 1; }
    for (const std::string& x : f) if (x.empty()) return false;
    auto num = [](const std::string& x) { return std::atof(x.c_str()); };
    const bool lastFrac = f.back().find('.') != std::string::npos;
    for (size_t k = 0; k + 1 < f.size(); ++k) if (f[k].find('.') != std::string::npos) return false;
    switch (f.size()) {
    case 1:
        if (!lastFrac) return false;          // a bare integer has no unit
        seconds = num(f[0]);
        return true;
    case 2: seconds = num(f[0]) * 60 + num(f[1]); return true;                       // MM:SS or MM:SS.fff
    case 3:
        if (lastFrac || f[2].size() != 3) seconds = num(f[0]) * 3600 + num(f[1]) * 60 + num(f[2]);   // HH:MM:SS(.fff)
        else seconds = num(f[0]) * 60 + num(f[1]) + num(f[2]) / 1000.0;              // MM:SS:mmm (Dolphin's own layout)
        return true;
    case 4: seconds = num(f[0]) * 3600 + num(f[1]) * 60 + num(f[2]) + num(f[3]) / 1000.0; return true;   // HH:MM:SS:mmm
    default: return false;
    }
}

// Position of the breakpoint marker: the word "BP" or "breakpoint" (any case for the long form).
bool findMarker(const std::string& line, size_t& start, size_t& end) {
    const std::string low = lower(line);
    for (size_t i = 0; i < line.size(); ++i) {
        if (i > 0 && isWord(line[i - 1])) continue;
        if (line.compare(i, 2, "BP") == 0 && (i + 2 == line.size() || !isWord(line[i + 2]))) { start = i; end = i + 2; return true; }
        if (low.compare(i, 10, "breakpoint") == 0 && (i + 10 == line.size() || !isWord(line[i + 10]))) { start = i; end = i + 10; return true; }
    }
    return false;
}

}  // namespace

int parseBpLine(const std::string& line, BpHit& out, std::string& why) {
    size_t ms = 0, me = 0;
    if (!findMarker(line, ms, me)) return 0;
    BpHit h;
    h.hasTime = parseTimestamp(line.substr(0, ms), h.seconds);
    size_t i = me;
    while (i < line.size() && (std::isspace(static_cast<unsigned char>(line[i])) || line[i] == ':' || line[i] == '#')) ++i;
    if (i < line.size() && line.compare(i, 4, "hit ") == 0) i += 4;       // "Breakpoint hit 801B0DE0"
    if (i < line.size() && line.compare(i, 3, "at ") == 0) i += 3;
    if (!takeHex(line, i, h.addr)) { why = "no address after the breakpoint marker"; return -1; }
    // Rest: optional symbol, then "( ... )" or loose rN=value tokens, then LR.
    const std::string rest = line.substr(i);
    const size_t open = rest.find('(');
    std::string argText;
    size_t afterArgs = 0;
    if (open != std::string::npos) {
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t k = open; k < rest.size(); ++k) {
            if (rest[k] == '(') ++depth;
            else if (rest[k] == ')' && --depth == 0) { close = k; break; }
        }
        if (close == std::string::npos) { why = "unterminated argument list"; return -1; }
        h.symbol = trim(rest.substr(0, open));
        argText = rest.substr(open + 1, close - open - 1);
        afterArgs = close + 1;
    } else {
        const std::string lowRest = lower(rest);
        size_t lr = std::string::npos;
        for (size_t k = 0; k + 1 < lowRest.size() && lr == std::string::npos; ++k)
            if (lowRest[k] == 'l' && lowRest[k + 1] == 'r' && (k == 0 || !isWord(lowRest[k - 1])) && (k + 2 == lowRest.size() || !isWord(lowRest[k + 2]))) lr = k;
        argText = rest.substr(0, lr == std::string::npos ? rest.size() : lr);
        afterArgs = 0;
    }
    // Arguments: split on whitespace and commas; accept hex with optional 0x and an optional "rN=" / "rN:" prefix.
    size_t p = 0;
    while (p < argText.size()) {
        while (p < argText.size() && (std::isspace(static_cast<unsigned char>(argText[p])) || argText[p] == ',')) ++p;
        if (p >= argText.size()) break;
        size_t q = p;
        while (q < argText.size() && !std::isspace(static_cast<unsigned char>(argText[q])) && argText[q] != ',') ++q;
        std::string tok = argText.substr(p, q - p);
        p = q;
        if ((tok[0] == 'r' || tok[0] == 'R') && tok.size() > 2 && std::isdigit(static_cast<unsigned char>(tok[1]))) {
            const size_t eq = tok.find_first_of("=:");
            if (eq != std::string::npos) tok = tok.substr(eq + 1);
        }
        if (tok.empty()) continue;
        if (open == std::string::npos && !(argText.find('=') != std::string::npos)) continue;   // loose text without values
        size_t t = 0;
        uint32_t v = 0;
        if (!takeHex(tok, t, v) || t != tok.size()) {
            if (open == std::string::npos) continue;        // free text between the address and LR is the symbol name
            why = "argument is not a hexadecimal number: " + clip(tok);
            return -1;
        }
        h.args.push_back(v);
    }
    // Link register.
    const std::string tail = lower(rest.substr(afterArgs));
    for (size_t k = 0; k + 1 < tail.size(); ++k) {
        if (tail[k] == 'l' && tail[k + 1] == 'r' && (k == 0 || !isWord(tail[k - 1]))) {
            size_t m = k + 2;
            while (m < tail.size() && (tail[m] == '=' || tail[m] == ':' || tail[m] == ' ')) ++m;
            uint32_t v = 0;
            std::string t2 = rest.substr(afterArgs);
            if (takeHex(t2, m, v)) { h.hasLr = true; h.lr = v; }
            break;
        }
    }
    out = h;
    return 1;
}

BpParse parseBpLog(const std::string& text) {
    BpParse r;
    size_t pos = 0, no = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = nl + 1;
        ++no;
        if (nl == text.size() && line.empty()) break;
        ++r.lines;
        BpHit h;
        std::string why;
        const int rc = parseBpLine(line, h, why);
        if (rc == 1) { h.lineNo = no; r.hits.push_back(h); }
        else if (rc == 0) ++r.ignored;
        else r.malformed.push_back("line " + std::to_string(no) + ": " + why + ": " + clip(line));
    }
    return r;
}

void unwrapTimes(std::vector<BpHit>& hits) {
    double offset = 0, prev = -1e300;
    for (BpHit& h : hits) {
        if (!h.hasTime) continue;
        while (h.seconds + offset < prev - 1800.0) offset += 3600.0;
        h.seconds += offset;
        prev = h.seconds;
    }
}

Notes parseNotes(const std::string& text) {
    Notes n;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) continue;
        const std::string low = lower(line);
        bool any = false;
        auto key = [&](const char* k, bool& has, uint32_t& val) {
            const size_t len = std::strlen(k);
            for (size_t i = 0; i + len <= low.size(); ++i) {
                if (low.compare(i, len, k) != 0) continue;
                if (i > 0 && isWord(low[i - 1])) continue;
                if (i + len < low.size() && isWord(low[i + len])) continue;
                size_t m = i + len;
                while (m < line.size() && (line[m] == ' ' || line[m] == '\t' || line[m] == ':' || line[m] == '=')) ++m;
                uint32_t v = 0;
                if (takeHex(line, m, v)) { has = true; val = v; any = true; return; }
            }
        };
        key("r3", n.hasR3, n.r3);
        key("tbu", n.hasTbu, n.tbu);
        key("tbl", n.hasTbl, n.tbl);
        if (!any) n.unreadable.push_back(clip(line));
    }
    return n;
}

OurTrace parseOurTrace(const std::string& text) {
    OurTrace t;
    t.loaded = true;
    size_t pos = 0, no = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = nl + 1;
        ++no;
        const char* s = line.c_str();
        unsigned a = 0, b = 0, d = 0;
        unsigned long long ull = 0;
        auto bad = [&](const char* what) { t.malformed.push_back("line " + std::to_string(no) + ": " + what + ": " + clip(line)); };
        if (std::strncmp(s, "call ", 5) == 0) {
            unsigned r3 = 0, r4 = 0, r5 = 0, r6 = 0;
            const int n = std::sscanf(s, "call %x site=%x depth=%u icount=%llu r3=%x r4=%x r5=%x r6=%x", &a, &b, &d, &ull, &r3, &r4, &r5, &r6);
            if (n == 4 || n == 8) {
                OurCall c;
                c.callee = a; c.site = b; c.depth = d; c.icount = ull;
                if (n == 8) { c.hasArgs = true; c.args[0] = r3; c.args[1] = r4; c.args[2] = r5; c.args[3] = r6; }
                t.calls.push_back(c);
            } else bad("unreadable call line");
        } else if (std::strncmp(s, "dsp mail ->", 11) == 0) {
            char dir[8] = {};
            if (std::sscanf(s, "dsp mail ->%7s %x t=%llu", dir, &a, &ull) == 3) t.mails.push_back(OurMail{std::strcmp(dir, "dsp") == 0, a, ull});
            else bad("unreadable mail line");
        } else if (std::strncmp(s, "dvd inquiry done", 16) == 0) {
            if (std::sscanf(s, "dvd inquiry done addr=%x", &a) == 1) { t.haveDriveInfoAddr = true; t.driveInfoAddr = a; }
            else bad("unreadable inquiry line");
        } else if (std::strncmp(s, "retrace ", 8) == 0) {
            if (std::sscanf(s, "retrace %u t=%llu", &a, &ull) == 2) t.retraceTicks.push_back(ull);
            else bad("unreadable retrace line");
        } else if (std::strncmp(s, "boot.after-osinit", 17) == 0) {
            if (std::sscanf(s, "boot.after-osinit icount=%llu", &ull) == 1) { t.haveAfterOsInit = true; t.afterOsInitIcount = ull; }
            else bad("unreadable after-osinit line");
        }
    }
    return t;
}

}  // namespace awl::hle::capture
