#include "image.h"

#include <algorithm>
#include <fstream>

namespace awl::recomp {

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    const std::streamsize n = f.tellg();
    if (n <= 0) {
        error = "empty file " + path;
        return false;
    }
    out.resize(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), n);
    if (!f) {
        error = "short read " + path;
        return false;
    }
    return true;
}

bool inFile(const std::vector<uint8_t>& f, uint64_t off, uint64_t len) {
    return off <= f.size() && len <= f.size() - off;
}

}  // namespace

const DolSection* DolImage::findSection(uint32_t addr) const {
    for (const auto& s : sections) {
        if (addr >= s.addr && addr - s.addr < s.size) return &s;
    }
    return nullptr;
}

bool DolImage::isText(uint32_t addr) const {
    const DolSection* s = findSection(addr);
    return s && s->text;
}

bool DolImage::readWord(uint32_t addr, uint32_t& out) const {
    const DolSection* s = findSection(addr);
    if (!s || addr - s->addr + 4 > s->size) return false;
    out = be32(&file[s->fileOffset + (addr - s->addr)]);
    return true;
}

bool loadDol(const std::string& path, DolImage& out, std::string& error) {
    if (!readFile(path, out.file, error)) return false;
    if (out.file.size() < 0x100) {
        error = "DOL smaller than its 0x100-byte header";
        return false;
    }
    const uint8_t* h = out.file.data();
    out.sections.clear();
    for (int i = 0; i < 18; ++i) {
        DolSection s;
        s.index = i;
        s.text = i < 7;
        s.fileOffset = be32(h + 4 * i);
        s.addr = be32(h + 0x48 + 4 * i);
        s.size = be32(h + 0x90 + 4 * i);
        if (s.size == 0) continue;
        if (!inFile(out.file, s.fileOffset, s.size)) {
            error = "DOL section " + std::to_string(i) + " extends past end of file";
            return false;
        }
        if ((s.addr & 3) != 0 || (s.size & 3) != 0) {
            error = "DOL section " + std::to_string(i) + " is not word aligned";
            return false;
        }
        for (const auto& o : out.sections) {
            if (s.addr < o.addr + o.size && o.addr < s.addr + s.size) {
                error = "DOL sections " + std::to_string(o.index) + " and " + std::to_string(i) + " overlap";
                return false;
            }
        }
        out.sections.push_back(s);
    }
    out.bssAddr = be32(h + 0xd8);
    out.bssSize = be32(h + 0xdc);
    out.entry = be32(h + 0xe0);
    if (!out.isText(out.entry)) {
        error = "DOL entry point is outside every text section";
        return false;
    }
    return true;
}

bool RelImage::readWord(uint32_t section, uint32_t offset, uint32_t& out) const {
    if (section >= sections.size()) return false;
    const RelSection& s = sections[section];
    if (s.offset == 0 || offset > s.length || s.length - offset < 4) return false;
    if (!inFile(file, static_cast<uint64_t>(s.offset) + offset, 4)) return false;
    out = be32(&file[s.offset + offset]);
    return true;
}

const char* relocTypeName(uint8_t t) {
    switch (t) {
        case 0: return "R_PPC_NONE";
        case 1: return "R_PPC_ADDR32";
        case 2: return "R_PPC_ADDR24";
        case 3: return "R_PPC_ADDR16";
        case 4: return "R_PPC_ADDR16_LO";
        case 5: return "R_PPC_ADDR16_HI";
        case 6: return "R_PPC_ADDR16_HA";
        case 7: return "R_PPC_ADDR14";
        case 8: return "R_PPC_ADDR14_BRTAKEN";
        case 9: return "R_PPC_ADDR14_BRNTAKEN";
        case 10: return "R_PPC_REL24";
        case 11: return "R_PPC_REL14";
        case 201: return "R_DOLPHIN_NOP";
        case 202: return "R_DOLPHIN_SECTION";
        case 203: return "R_DOLPHIN_END";
        case 204: return "R_DOLPHIN_MRKREF";
        default: return "unknown";
    }
}

bool loadRel(const std::string& path, RelImage& out, std::string& error) {
    if (!readFile(path, out.file, error)) return false;
    const auto& f = out.file;
    if (f.size() < 0x40) {
        error = "REL smaller than the version-1 header";
        return false;
    }
    const uint8_t* h = f.data();
    out.id = be32(h + 0x00);
    out.numSections = be32(h + 0x0c);
    const uint32_t sectionInfo = be32(h + 0x10);
    out.nameOffset = be32(h + 0x14);
    out.nameSize = be32(h + 0x18);
    out.version = be32(h + 0x1c);
    out.bssSize = be32(h + 0x20);
    const uint32_t relOffset = be32(h + 0x24);
    const uint32_t impOffset = be32(h + 0x28);
    const uint32_t impSize = be32(h + 0x2c);
    out.prologSection = h[0x30];
    out.epilogSection = h[0x31];
    out.unresolvedSection = h[0x32];
    out.prolog = be32(h + 0x34);
    out.epilog = be32(h + 0x38);
    out.unresolved = be32(h + 0x3c);
    if (out.version < 1 || out.version > 3) {
        error = "unsupported REL version " + std::to_string(out.version);
        return false;
    }
    if (out.version >= 2) {
        if (f.size() < 0x48) { error = "truncated REL v2 header"; return false; }
        out.align = be32(h + 0x40);
        out.bssAlign = be32(h + 0x44);
    }
    if (out.version >= 3) {
        if (f.size() < 0x4c) { error = "truncated REL v3 header"; return false; }
        out.fixSize = be32(h + 0x48);
    }
    if (out.numSections == 0 || out.numSections > 64 ||
        !inFile(f, sectionInfo, static_cast<uint64_t>(out.numSections) * 8)) {
        error = "REL section table out of range";
        return false;
    }
    out.sections.clear();
    for (uint32_t i = 0; i < out.numSections; ++i) {
        RelSection s;
        const uint32_t rawOff = be32(h + sectionInfo + i * 8);
        s.length = be32(h + sectionInfo + i * 8 + 4);
        s.exec = (rawOff & 1) != 0;
        s.offset = rawOff & ~3u;
        if (s.offset != 0 && !inFile(f, s.offset, s.length)) {
            error = "REL section " + std::to_string(i) + " extends past end of file";
            return false;
        }
        out.sections.push_back(s);
    }
    if (out.nameOffset != 0 && inFile(f, out.nameOffset, out.nameSize)) {
        out.name.assign(reinterpret_cast<const char*>(h + out.nameOffset), out.nameSize);
        while (!out.name.empty() && out.name.back() == '\0') out.name.pop_back();
    }
    if (impSize % 8 != 0 || !inFile(f, impOffset, impSize)) {
        error = "REL import table out of range";
        return false;
    }
    out.imports.clear();
    for (uint32_t i = 0; i < impSize / 8; ++i) {
        RelImport imp;
        imp.moduleId = be32(h + impOffset + i * 8);
        imp.relOffset = be32(h + impOffset + i * 8 + 4);
        out.imports.push_back(std::move(imp));
    }
    // Each import's relocation stream runs until its R_DOLPHIN_END record; the delta stream is relative to
    // the current section (set by R_DOLPHIN_SECTION) and the previous record's position.
    (void)relOffset;
    for (auto& imp : out.imports) {
        uint64_t p = imp.relOffset;
        uint32_t curSection = 0;
        uint32_t curOffset = 0;
        bool ended = false;
        while (inFile(f, p, 8)) {
            RelReloc r;
            r.delta = be16(h + p);
            r.type = h[p + 2];
            r.section = h[p + 3];
            r.addend = be32(h + p + 4);
            p += 8;
            curOffset += r.delta;
            if (r.type == 203) { ++out.endRecords; ended = true; break; }
            if (r.type == 204) ++out.markRecords;
            if (r.type == 202) {
                ++out.sectionRecords;
                curSection = r.section;
                curOffset = 0;
                continue;
            }
            r.fromSection = curSection;
            r.fromOffset = curOffset;
            if (r.type == 201) { ++out.nopRecords; continue; }  // R_DOLPHIN_NOP only advances the offset
            imp.relocs.push_back(r);
        }
        if (!ended) {
            error = "REL relocation stream for module " + std::to_string(imp.moduleId) + " has no END record";
            return false;
        }
    }
    return true;
}

RelLayout layoutRel(const RelImage& rel, uint32_t base) {
    RelLayout l;
    l.id = rel.id;
    l.base = base;
    l.sectionAddr.assign(rel.sections.size(), 0);
    uint32_t cur = base;
    for (size_t i = 0; i < rel.sections.size(); ++i) {
        const RelSection& s = rel.sections[i];
        if (s.length == 0) continue;
        const uint32_t align = s.bss() ? (rel.bssAlign ? rel.bssAlign : 8) : (rel.align ? rel.align : 8);
        cur = (cur + align - 1) & ~(align - 1);
        l.sectionAddr[i] = cur;
        cur += s.length;
    }
    l.end = cur;
    return l;
}

namespace {

const RelLayout* findLayout(const std::vector<RelLayout>& layouts, uint32_t id) {
    for (const auto& l : layouts) if (l.id == id) return &l;
    return nullptr;
}

void put32(std::vector<uint8_t>& f, size_t off, uint32_t v) {
    f[off] = static_cast<uint8_t>(v >> 24); f[off + 1] = static_cast<uint8_t>(v >> 16);
    f[off + 2] = static_cast<uint8_t>(v >> 8); f[off + 3] = static_cast<uint8_t>(v);
}
void put16(std::vector<uint8_t>& f, size_t off, uint32_t v) {
    f[off] = static_cast<uint8_t>(v >> 8); f[off + 1] = static_cast<uint8_t>(v);
}

}  // namespace

bool linkRel(const RelImage& rel, const std::vector<RelLayout>& layouts, const DolImage& dol, DolImage& out,
             RelLinkStats& stats, std::string& error) {
    const RelLayout* self = findLayout(layouts, rel.id);
    if (!self) { error = "no layout for module"; return false; }
    // Image buffer: every section back to back in layout order (offsets relative to the module base).
    out = DolImage{};
    out.file.assign(self->end - self->base, 0);
    for (size_t i = 0; i < rel.sections.size(); ++i) {
        const RelSection& s = rel.sections[i];
        if (s.length == 0 || self->sectionAddr[i] == 0) continue;
        const uint32_t off = self->sectionAddr[i] - self->base;
        if (!s.bss()) std::copy_n(rel.file.begin() + s.offset, s.length, out.file.begin() + off);
        DolSection ds;
        ds.index = static_cast<int>(i);
        ds.text = s.exec;
        ds.fileOffset = off;
        ds.addr = self->sectionAddr[i];
        ds.size = s.length;
        out.sections.push_back(ds);
    }
    for (const auto& imp : rel.imports) {
        const RelLayout* target = imp.moduleId == 0 ? nullptr : findLayout(layouts, imp.moduleId);
        for (const RelReloc& r : imp.relocs) {
            if (r.fromSection >= rel.sections.size() || self->sectionAddr[r.fromSection] == 0) { ++stats.unresolved; continue; }
            const uint32_t P = self->sectionAddr[r.fromSection] + r.fromOffset;
            const uint32_t need = (r.type >= 3 && r.type <= 6) ? 2u : 4u;   // ADDR16 family patches a halfword
            if (static_cast<uint64_t>(r.fromOffset) + need > rel.sections[r.fromSection].length) { ++stats.unresolved; continue; }
            uint32_t S;
            if (imp.moduleId == 0) {
                S = r.addend;
                if (dol.isText(S)) ++stats.toDolText;
                else if (dol.findSection(S)) ++stats.toDolData;
                else ++stats.toDolOther;
            } else if (target) {
                if (r.section >= target->sectionAddr.size() || target->sectionAddr[r.section] == 0) { ++stats.unresolved; continue; }
                S = target->sectionAddr[r.section] + r.addend;
                if (imp.moduleId == rel.id) ++stats.toSelf; else ++stats.toOtherRel;
            } else {
                ++stats.unresolved;
                continue;
            }
            const size_t at = P - self->base;
            uint32_t insn = 0;
            if (at + 4 <= out.file.size()) insn = be32(&out.file[at]);
            auto signedFits = [](int64_t v, int bits) { return v >= -(int64_t{1} << (bits - 1)) && v < (int64_t{1} << (bits - 1)); };
            switch (r.type) {
                case 1: put32(out.file, at, S); break;                                           // ADDR32
                case 2: put32(out.file, at, (insn & ~0x03FFFFFCu) | (S & 0x03FFFFFCu)); break;   // ADDR24
                case 3: put16(out.file, at, S); break;                                           // ADDR16
                case 4: put16(out.file, at, S & 0xFFFF); break;                                  // ADDR16_LO
                case 5: put16(out.file, at, S >> 16); break;                                     // ADDR16_HI
                case 6: put16(out.file, at, (S + 0x8000) >> 16); break;                          // ADDR16_HA
                case 7: case 8: case 9: put32(out.file, at, (insn & ~0xFFFCu) | (S & 0xFFFCu)); break;   // ADDR14*
                case 10: {                                                                       // REL24
                    const int64_t d = static_cast<int64_t>(S) - P;
                    if (!signedFits(d, 26)) ++stats.rangeErrors;
                    put32(out.file, at, (insn & ~0x03FFFFFCu) | (static_cast<uint32_t>(d) & 0x03FFFFFCu));
                    break;
                }
                case 11: {                                                                       // REL14
                    const int64_t d = static_cast<int64_t>(S) - P;
                    if (!signedFits(d, 16)) ++stats.rangeErrors;
                    put32(out.file, at, (insn & ~0xFFFCu) | (static_cast<uint32_t>(d) & 0xFFFCu));
                    break;
                }
                default: ++stats.unknownTypes; continue;
            }
            ++stats.applied;
            ++stats.byType[r.type];
        }
    }
    if (rel.prolog) out.entry = self->sectionAddr[rel.prologSection] + rel.prolog;
    else out.entry = 0;
    return true;
}

}  // namespace awl::recomp
