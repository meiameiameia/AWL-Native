// Parsers for the Dolphin capture files (docs/research/hle-platform.md, section 10) and for the host's boot trace.
// The Dolphin breakpoint-log line format is not verified yet, so the parser accepts several plausible layouts and reports
// every line it cannot read instead of failing. No game data is interpreted here.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace awl::hle::capture {

// Functions whose calls are logged in capture C (and B) and recorded by the host: address, name used in the report.
struct WatchedFunction {
    uint32_t addr;
    const char* name;
    bool captureC;      // logged in capture C (the call-order comparison); the others belong to capture B
};
extern const WatchedFunction kWatched[];
extern const uint32_t kWatchedCount;
const char* watchedName(uint32_t addr);   // nullptr when the address is not listed
bool isCaptureC(uint32_t addr);

// One logged breakpoint hit.
struct BpHit {
    uint32_t addr = 0;
    std::string symbol;             // text between the address and the argument list, if any
    std::vector<uint32_t> args;     // r3, r4, ... in the order printed
    bool hasLr = false;
    uint32_t lr = 0;
    bool hasTime = false;
    double seconds = 0;             // time stamp as printed (wraps are undone by unwrapTimes)
    size_t lineNo = 0;              // 1-based
};

struct BpParse {
    std::vector<BpHit> hits;
    std::vector<std::string> malformed;   // "line N: reason: text" for lines that mention a breakpoint but could not be read
    size_t lines = 0;
    size_t ignored = 0;                   // lines that are not breakpoint lines at all
};

// Returns 1 for a hit, 0 for a line that is not a breakpoint line, -1 for a breakpoint line that cannot be read (why is set).
int parseBpLine(const std::string& line, BpHit& out, std::string& why);
BpParse parseBpLog(const std::string& text);
// Makes time stamps monotonic when they wrap (minutes:seconds:milliseconds wraps every hour).
void unwrapTimes(std::vector<BpHit>& hits);

// Values the owner writes into the *-notes.txt files.
struct Notes {
    bool hasR3 = false;
    uint32_t r3 = 0;
    bool hasTbu = false, hasTbl = false;
    uint32_t tbu = 0, tbl = 0;
    std::vector<std::string> unreadable;  // non-empty lines that carried none of the keys
};
Notes parseNotes(const std::string& text);

// The parts of the host's boot trace the comparison uses.
struct OurCall {
    uint32_t callee = 0, site = 0, depth = 0;
    uint64_t icount = 0;
    bool hasArgs = false;
    uint32_t args[4] = {};          // r3-r6 at the call, when the trace carries them (call lines with r3= .. r6=)
};
struct OurMail {
    bool toDsp = false;
    uint32_t value = 0;
    uint64_t tick = 0;
};
struct OurTrace {
    bool loaded = false;
    std::vector<OurCall> calls;
    std::vector<OurMail> mails;
    std::vector<uint64_t> retraceTicks;
    bool haveDriveInfoAddr = false;
    uint32_t driveInfoAddr = 0;
    bool haveAfterOsInit = false;
    uint64_t afterOsInitIcount = 0;       // "boot.after-osinit icount=N": instruction count when OSInit has returned
    std::vector<std::string> malformed;
};
OurTrace parseOurTrace(const std::string& text);

}  // namespace awl::hle::capture
