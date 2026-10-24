#include "manifest.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

namespace awl::recomp {

namespace {

std::string where(const std::string& source, int line) { return source + ":" + std::to_string(line) + ": "; }

bool isSpace(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }

// Splits off the next whitespace-delimited token starting at `pos`; advances pos past it.
bool nextToken(const std::string& s, size_t& pos, std::string& tok) {
    while (pos < s.size() && isSpace(s[pos])) ++pos;
    if (pos >= s.size() || s[pos] == '#') return false;
    const size_t begin = pos;
    while (pos < s.size() && !isSpace(s[pos]) && s[pos] != '#') ++pos;
    tok = s.substr(begin, pos - begin);
    return true;
}

bool parseAddress(const std::string& tok, uint32_t& out) {
    if (tok.size() < 3 || tok.size() > 10 || tok[0] != '0' || (tok[1] != 'x' && tok[1] != 'X')) return false;
    uint64_t v = 0;
    for (size_t i = 2; i < tok.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(tok[i]);
        if (!std::isxdigit(ch)) return false;
        v = v * 16 + (std::isdigit(ch) ? ch - '0' : (std::tolower(ch) - 'a' + 10));
    }
    out = static_cast<uint32_t>(v);
    return true;
}

bool validName(const std::string& n) {
    if (n.empty() || !(std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_')) return false;
    return std::all_of(n.begin(), n.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; });
}

}  // namespace

bool parseHleManifest(const std::string& text, const std::string& source, std::vector<HleEntry>& out,
                      std::vector<std::string>& errors) {
    out.clear();
    const size_t errorsBefore = errors.size();
    std::map<uint32_t, int> seen;   // address -> line of first entry
    size_t start = 0;
    int lineNo = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        ++lineNo;

        size_t pos = 0;
        std::string moduleTok, addrTok, nameTok;
        if (!nextToken(line, pos, moduleTok)) continue;   // blank or comment-only line
        if (!nextToken(line, pos, addrTok) || !nextToken(line, pos, nameTok)) {
            errors.push_back(where(source, lineNo) + "expected '<module> <address> <name>  # <evidence>'");
            continue;
        }
        if (moduleTok != "dol") {
            errors.push_back(where(source, lineNo) + "unsupported module '" + moduleTok + "' (only 'dol' is supported; REL entries are not)");
            continue;
        }
        HleEntry e;
        e.line = lineNo;
        if (!parseAddress(addrTok, e.addr)) {
            errors.push_back(where(source, lineNo) + "bad address '" + addrTok + "' (expected 0x followed by 1-8 hexadecimal digits)");
            continue;
        }
        if ((e.addr & 3) != 0) {
            errors.push_back(where(source, lineNo) + "address " + addrTok + " is not 4-byte aligned");
            continue;
        }
        if (!validName(nameTok)) {
            errors.push_back(where(source, lineNo) + "bad function name '" + nameTok + "' (letters, digits and '_', not starting with a digit)");
            continue;
        }
        e.name = nameTok;
        // After the name only whitespace and the '#' evidence comment may follow.
        while (pos < line.size() && isSpace(line[pos])) ++pos;
        if (pos >= line.size() || line[pos] != '#') {
            errors.push_back(where(source, lineNo) + "unexpected text after the name; the evidence must follow a '#'");
            continue;
        }
        ++pos;
        while (pos < line.size() && isSpace(line[pos])) ++pos;
        e.evidence = line.substr(pos);
        while (!e.evidence.empty() && isSpace(e.evidence.back())) e.evidence.pop_back();
        if (e.evidence.empty()) {
            errors.push_back(where(source, lineNo) + "missing evidence after '#' (how was " + e.name + " identified?)");
            continue;
        }
        const auto ins = seen.emplace(e.addr, lineNo);
        if (!ins.second) {
            errors.push_back(where(source, lineNo) + "duplicate address " + addrTok + " (first listed on line " + std::to_string(ins.first->second) + ")");
            continue;
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const HleEntry& a, const HleEntry& b) { return a.addr < b.addr; });
    return errors.size() == errorsBefore;
}

bool validateHleManifest(const std::vector<HleEntry>& entries, const Discovery& d, const std::string& source,
                         std::vector<std::string>& errors) {
    const size_t errorsBefore = errors.size();
    for (const HleEntry& e : entries) {
        if (d.funcs.count(e.addr)) continue;
        char addr[16];
        std::snprintf(addr, sizeof addr, "0x%08X", e.addr);
        errors.push_back(where(source, e.line) + std::string(addr) + " (" + e.name + ") is not a discovered DOL function start");
    }
    return errors.size() == errorsBefore;
}

}  // namespace awl::recomp
