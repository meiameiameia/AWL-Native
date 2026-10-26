// Platform runtime for the recompiled game: clock, event queue, interrupt controller, hardware models,
// threads on fibers and the native replacements of SDK functions. One Platform per process.
// Design and evidence: docs/research/hle-platform.md.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "boot_table.h"
#include "hle_common.h"
#include "hle_abi.h"

namespace awl::hle {

// ---- Events --------------------------------------------------------------------------------------------
enum class Ev : uint32_t {
    Decrementer = 1,   // a = generation
    ViDisplayInt,      // a = display interrupt index 0-3, b = generation
    DvdDone,           // a = request id
    AiDmaDone,         // AI DMA buffer consumed
    AiSampleCounter,
    AramDmaDone,
    DspMail,           // b = mail value (DSP -> CPU)
    PeToken,           // a = token
    PeFinish,
    CpInt,
    SiXfer,
    Custom             // a = index into Platform::customEvents
};

struct Event {
    uint64_t tick = 0;
    uint64_t seq = 0;
    Ev kind = Ev::Custom;
    uint64_t a = 0;
    uint64_t b = 0;
};

// Ordered by (tick, sequence number): two events due at the same tick run in the order they were posted.
class EventQueue {
public:
    void post(uint64_t tick, Ev kind, uint64_t a = 0, uint64_t b = 0);
    bool popDue(uint64_t now, Event& out);
    bool empty() const { return q_.empty(); }
    uint64_t nextTick() const { return q_.empty() ? ~0ull : q_.front().tick; }
    size_t size() const { return q_.size(); }
    // Removes every queued event of this kind with this first argument (used when a request is cancelled).
    void cancel(Ev kind, uint64_t a);
    void clear() { q_.clear(); }
private:
    std::vector<Event> q_;   // sorted by (tick, seq)
    uint64_t seq_ = 0;
};

// ---- Time ----------------------------------------------------------------------------------------------
// Deterministic clock: one tick per kInstructionsPerTick retired instructions plus ticks skipped while idle.
class Clock {
public:
    uint64_t now(uint64_t retired) const { return retired / kInstructionsPerTick + skipped_; }
    // Smallest retired-instruction count at which now() >= tick.
    uint64_t retiredFor(uint64_t tick) const { return tick <= skipped_ ? 0 : (tick - skipped_) * kInstructionsPerTick; }
    void skipTo(uint64_t retired, uint64_t tick) { const uint64_t n = now(retired); if (tick > n) skipped_ += tick - n; }
    uint64_t skipped() const { return skipped_; }
private:
    uint64_t skipped_ = 0;
};

// ---- Interrupt controller (PI) -------------------------------------------------------------------------
// INTSR/INTMR bit positions, read from SetInterruptMask and __OSDispatchInterrupt in the DOL.
namespace pibit {
constexpr uint32_t Error = 1u << 0, ResetSwitch = 1u << 1, Di = 1u << 2, Si = 1u << 3, Exi = 1u << 4, Ai = 1u << 5,
                   Dsp = 1u << 6, Mem = 1u << 7, Vi = 1u << 8, PeToken = 1u << 9, PeFinish = 1u << 10,
                   Cp = 1u << 11, Debug = 1u << 12, Hsp = 1u << 13;
constexpr uint32_t ResetSwitchState = 1u << 16;   // set while the button is NOT pressed (read by OSGetResetButtonState)
}

class Device {
public:
    virtual ~Device() = default;
    virtual uint64_t read(Ctx& c, uint32_t addr, uint32_t size) = 0;
    virtual void write(Ctx& c, uint32_t addr, uint32_t size, uint64_t value) = 0;
};

class Platform;
class Dvd;

// ---- Guest threads -------------------------------------------------------------------------------------
struct Thread {
    Ctx* base = nullptr;               // the thread's own context
    std::vector<Ctx*> irq;             // interrupt activations (index = nesting level - 1)
    void* fiber = nullptr;
    uint32_t osContext = 0;            // guest address of the OSContext last bound to this thread
    bool started = false;
    uint32_t entry = 0;                // guest entry for threads started from an OSContext image
    uint32_t exitFn = 0;               // function run after the entry returns (the image's LR)
    bool exiting = false;              // SelectThread saved its context after OSExitThread: it loads the next context and never returns
    bool dead = false;                 // exited: never resumed, fiber and contexts reaped from another fiber
    int depth = 0;                     // current interrupt nesting level on this fiber
    uint32_t id = 0;
};

struct Config {
    std::string discDir = "disc";                 // contains sys/ and files/
    uint32_t mainLoopRetraces = 120;              // N consecutive qualifying retraces = main loop reached
    bool dvdLatencyModel = true;
    bool padXInput = false;                       // PAD channel 0 reads the first XInput controller (not deterministic); default neutral
    bool idleSkip = true;                         // fast-forward the scheduler's idle spin to the next event (no guest-visible effect)
    uint32_t rtcSeconds = bootValues().rtcSeconds;   // RTC counter (seconds since 2000-01-01): fixed for deterministic runs
};

struct StopReport {
    bool stopped = false;
    rt::Stop stop{};
    Ctx* ctx = nullptr;                // context that was running
    uint32_t threadId = 0;
};

class Platform {
public:
    static Platform& get();
    Platform();
    ~Platform();

    // Wiring ------------------------------------------------------------------------------------------
    void attach(Machine& m, const Config& cfg);        // memory, devices, boot-environment-independent state
    void installHandlers();                            // hleRegister / mmioInstall / timeBaseInstall / preemptInstall
    void registerHandlers();                           // hleRegister only (used by tests)
    void installPreemption();                          // the preemption hook and its first deadline (installHandlers does this too; tests call it alone)
    Machine& machine() { return *machine_; }
    GuestMem& mem() { return mem_; }
    const Config& config() const { return cfg_; }

    // Time ---------------------------------------------------------------------------------------------
    uint64_t now() const { return clock_.now(machine_->icount); }
    uint64_t retiredAt(uint64_t tick) const { return clock_.retiredFor(tick); }
    void post(uint64_t tick, Ev kind, uint64_t a = 0, uint64_t b = 0);
    void postAfter(uint64_t ticks, Ev kind, uint64_t a = 0, uint64_t b = 0) { post(now() + ticks, kind, a, b); }
    void onEvent(Ev kind, std::function<void(Ctx&, const Event&)> fn);
    void lowerDeadline(uint64_t retired);              // make the preemption hook run no later than this instruction count
    // Queues a guest callback to run in an interrupt-style envelope at the next delivery point (API-level completions).
    void queueCallback(uint32_t fn, uint32_t r3, uint32_t r4);
    EventQueue& events() { return events_; }
    Clock& clock() { return clock_; }

    // Interrupt lines ---------------------------------------------------------------------------------
    void setLine(uint32_t piBit, bool level);
    uint32_t piStatus() const { return level_ | latched_; }
    void latch(uint32_t piBit) { latched_ |= piBit; }
    bool externalPending() const { return (piStatus() & intMask_) != 0; }

    // Devices -----------------------------------------------------------------------------------------
    Device& dev(uint32_t block);
    Dvd& dvd() { return *dvd_; }
    uint32_t& piMask() { return intMask_; }
    uint32_t piFifoBase = 0, piFifoEnd = 0, piFifoWp = 0;   // CPU FIFO registers (PI 0x0C, 0x10, 0x14); bit 26 of the write pointer is the wrap flag
    uint32_t& piLatched() { return latched_; }

    // Execution ---------------------------------------------------------------------------------------
    // Runs the guest entry function on a fiber until it returns, stops, or the host requests an exit.
    // Returns true if the entry returned normally.
    bool run(Ctx& boot, uint32_t entry);
    const StopReport& report() const { return report_; }
    void requestExit(const char* why);                 // from inside guest execution; switches to the supervisor
    const std::string& exitReason() const { return exitReason_; }

    // Hooks used by handlers and models ---------------------------------------------------------------
    static void preempt(Ctx& c);                       // preemption hook (checkpoint reached the deadline)
    void rearm(Ctx& c);                                // recompute the preemption deadline
    void processEvents(Ctx& c);                        // run every due event
    void deliver(Ctx& c);                              // deliver pending interrupts if the context allows it
    void setDecrementer(uint32_t value);
    uint32_t& mainLoopCount() { return mainLoopCount_; }

    // Context primitives (OSSaveContext / OSLoadContext) --------------------------------------------
    void osSaveContext(Ctx& c);
    void osLoadContext(Ctx& c);
    Thread* threadOf(Ctx& c);

    // Counters for the main-loop criterion and bring-up diagnostics.
    struct Counters {
        uint64_t retraces = 0, fbLatches = 0, efbCopies = 0, tokens = 0, finishes = 0;
        uint32_t consecutive = 0;          // consecutive qualifying retraces
        uint32_t sinceLatch = 0;           // retraces since the last framebuffer latch
        bool copySinceLatch = false;       // an EFB copy happened since the last latch
        bool lastLatchHadCopy = false;     // the last latch was preceded by an EFB copy
        bool haveLatch = false;
        uint64_t lastLatchTick = 0;
    } counters;
    struct IdleStats { uint64_t probes = 0, skips = 0, instructionsSkipped = 0; } idle;
    std::unordered_map<uint32_t, uint64_t> pcProfile;   // AWL_PC_PROFILE: guest pc at each idle probe (a sampling profile of the run)
    uint64_t reaped = 0;                   // exited threads whose fibers were freed
    void onRetrace();                      // called by the VI model at every retrace

private:
    friend class Scheduler;
    void handleEvent(Ctx& c, const Event& e);
    void armHook();                                    // program the runtime's deadline: the semantic deadline, or an earlier idle probe
    void probeIdle(Ctx& c);
    void deliverException(Ctx& c, uint32_t exception, uint32_t fn, uint32_t r3, uint32_t r4);
    void writeContextImage(Ctx& c, uint32_t osctx, bool exception);
    void startThreadFromImage(Ctx& from, uint32_t osctx);
    void switchTo(Thread* t);
    void resetThreads();
    void reapDead();
    Ctx* irqCtx(Thread* t, Ctx& from);
    static void __stdcall fiberMain(void* param);

    Machine* machine_ = nullptr;
    GuestMem mem_;
    Config cfg_;
    Clock clock_;
    EventQueue events_;
    uint32_t level_ = 0, latched_ = 0, intMask_ = 0;
    std::unique_ptr<Device> blocks_[16];
    std::unique_ptr<Dvd> dvd_;
    struct PendingCallback { uint32_t fn, r3, r4; };
    std::deque<PendingCallback> callbacks_;
    std::function<void(Ctx&, const Event&)> handlers_[16];
    uint64_t deadline_ = ~0ull;
    bool hookInstalled_ = false;
    bool profile_ = std::getenv("AWL_PC_PROFILE") != nullptr;
    bool entryReturned_ = false;
    uint64_t decGeneration_ = 0;
    bool decPending_ = false;

    // threads
    std::vector<std::unique_ptr<Thread>> threads_;
    std::unordered_map<Ctx*, Thread*> byCtx_;
    std::unordered_map<uint32_t, Thread*> byOsCtx_;
    Thread* cur_ = nullptr;
    void* supervisor_ = nullptr;
    StopReport report_;
    std::string exitReason_;
    bool exiting_ = false;
    uint32_t mainLoopCount_ = 0;
    uint32_t nextThreadId_ = 1;
};

}  // namespace awl::hle
