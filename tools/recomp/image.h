// DOL and REL (Nintendo SDK relocatable module) loaders for the static-recompilation spike.
// Parsers validate every offset against the file size and reject malformed input with an error string.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace awl::recomp {

struct DolSection {
    int index = 0;           // 0-6 text, 7-17 data
    bool text = false;
    uint32_t fileOffset = 0;
    uint32_t addr = 0;
    uint32_t size = 0;
};

struct DolImage {
    std::vector<uint8_t> file;
    std::vector<DolSection> sections;  // only non-empty sections
    uint32_t bssAddr = 0;
    uint32_t bssSize = 0;
    uint32_t entry = 0;

    const DolSection* findSection(uint32_t addr) const;   // section holding addr, or nullptr
    bool isText(uint32_t addr) const;
    bool readWord(uint32_t addr, uint32_t& out) const;    // big-endian word from a file-backed section
};

bool loadDol(const std::string& path, DolImage& out, std::string& error);

struct RelSection {
    uint32_t offset = 0;      // file offset (low bit stripped)
    uint32_t length = 0;
    bool exec = false;
    bool bss() const { return offset == 0 && length != 0; }
};

struct RelReloc {
    uint16_t delta = 0;
    uint8_t type = 0;
    uint8_t section = 0;
    uint32_t addend = 0;
    // Resolved positions (filled by the parser from the delta stream).
    uint32_t fromSection = 0;
    uint32_t fromOffset = 0;
};

struct RelImport {
    uint32_t moduleId = 0;
    uint32_t relOffset = 0;
    std::vector<RelReloc> relocs;
};

struct RelImage {
    std::string name;
    std::vector<uint8_t> file;
    uint32_t id = 0;
    uint32_t numSections = 0;
    uint32_t version = 0;
    uint32_t bssSize = 0;
    uint32_t nameOffset = 0;
    uint32_t nameSize = 0;
    uint8_t prologSection = 0, epilogSection = 0, unresolvedSection = 0;
    uint32_t prolog = 0, epilog = 0, unresolved = 0;
    uint32_t align = 0, bssAlign = 0, fixSize = 0;
    std::vector<RelSection> sections;
    std::vector<RelImport> imports;
    size_t nopRecords = 0, sectionRecords = 0, markRecords = 0, endRecords = 0;   // control records seen while parsing

    bool readWord(uint32_t section, uint32_t offset, uint32_t& out) const;
};

bool loadRel(const std::string& path, RelImage& out, std::string& error);

const char* relocTypeName(uint8_t type);

// Result of laying out and relocating a set of RELs at fixed bases (what a native OSLink would produce).
struct RelLinkStats {
    size_t applied = 0;
    size_t byType[256] = {};
    size_t toDolText = 0, toDolData = 0, toDolOther = 0;   // module-0 targets by kind
    size_t toSelf = 0, toOtherRel = 0, unresolved = 0;
    size_t rangeErrors = 0;                                 // branch displacement does not fit (REL24/REL14)
    size_t unknownTypes = 0;
};

struct RelLayout {
    uint32_t id = 0;
    uint32_t base = 0;
    std::vector<uint32_t> sectionAddr;   // 0 for empty sections
    uint32_t end = 0;
};

// Lays out `rel` at `base` (sections 32-byte aligned, bss zero-filled in the image), then applies every
// relocation: module 0 addends are absolute DOL addresses, other modules resolve through `layouts`.
RelLayout layoutRel(const RelImage& rel, uint32_t base);
bool linkRel(const RelImage& rel, const std::vector<RelLayout>& layouts, const DolImage& dol, DolImage& out,
             RelLinkStats& stats, std::string& error);

uint32_t be32(const uint8_t* p);
uint16_t be16(const uint8_t* p);

}  // namespace awl::recomp
