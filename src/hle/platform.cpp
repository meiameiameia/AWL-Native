#include "platform.h"

#include <algorithm>
#include <windows.h>

#include "devices.h"
#include "dvd.h"

namespace awl::hle {

// ---- EventQueue ----------------------------------------------------------------------------------------

void EventQueue::post(uint64_t tick, Ev kind, uint64_t a, uint64_t b) {
    Event e;
    e.tick = tick;
    e.seq = seq_++;
    e.kind = kind;
    e.a = a;
    e.b = b;
    auto it = std::upper_bound(q_.begin(), q_.end(), e, [](const Event& x, const Event& y) {
        return x.tick != y.tick ? x.tick < y.tick : x.seq < y.seq;
    });
    q_.insert(it, e);
}

bool EventQueue::popDue(uint64_t now, Event& out) {
    if (q_.empty() || q_.front().tick > now) return false;
    out = q_.front();
    q_.erase(q_.begin());
    return true;
}

void EventQueue::cancel(Ev kind, uint64_t a) {
    q_.erase(std::remove_if(q_.begin(), q_.end(), [&](const Event& e) { return e.kind == kind && e.a == a; }), q_.end());
}

// ---- Platform ------------------------------------------------------------------------------------------

void Platform::resetThreads() {
    for (auto& t : threads_) {
        if (t->fiber && t->fiber != GetCurrentFiber()) DeleteFiber(t->fiber);
        for (Ctx* i : t->irq) rt::destroyThreadContext(i);
        if (t->base && t->id != 1) rt::destroyThreadContext(t->base);    // the boot context belongs to the host
    }
    threads_.clear();
    byCtx_.clear();
    byOsCtx_.clear();
    cur_ = nullptr;
    nextThreadId_ = 1;
    entryReturned_ = false;
}

Platform& Platform::get() {
    static Platform p;
    return p;
}

Platform::Platform() = default;
Platform::~Platform() = default;

void Platform::attach(Machine& m, const Config& cfg) {
    machine_ = &m;
    mem_ = GuestMem(m.mem);
    cfg_ = cfg;
    clock_ = Clock();
    events_.clear();
    level_ = latched_ = intMask_ = 0;
    decGeneration_ = 0;
    decPending_ = false;
    counters = Counters();
    resetThreads();
    exitReason_.clear();
    exiting_ = false;
    report_ = StopReport();
    callbacks_.clear();
    hookInstalled_ = false;
    deadline_ = ~0ull;
    createDevices(*this, blocks_);
    dvd_ = std::make_unique<Dvd>(*this);
}

Device& Platform::dev(uint32_t block) { return *blocks_[block]; }

void Platform::setLine(uint32_t piBit, bool level) {
    const uint32_t before = piStatus() & intMask_;
    if (level) level_ |= piBit; else level_ &= ~piBit;
    if ((piStatus() & intMask_) && !before) lowerDeadline(machine_->icount + 1);
}

void Platform::lowerDeadline(uint64_t retired) {
    if (retired <= machine_->icount) retired = machine_->icount + 1;
    if (!hookInstalled_ || retired >= deadline_) return;
    deadline_ = retired;
    rt::preemptDeadlineSet(deadline_);
}

void Platform::post(uint64_t tick, Ev kind, uint64_t a, uint64_t b) {
    events_.post(tick, kind, a, b);
    lowerDeadline(retiredAt(tick));
}

void Platform::onEvent(Ev kind, std::function<void(Ctx&, const Event&)> fn) { handlers_[static_cast<uint32_t>(kind)] = std::move(fn); }

// ---- Time and interrupts -------------------------------------------------------------------------------

void Platform::setDecrementer(uint32_t value) {
    ++decGeneration_;
    // The decrementer interrupts when it counts down from 0 to -1: value + 1 ticks from now.
    post(now() + static_cast<uint64_t>(value) + 1, Ev::Decrementer, decGeneration_);
}

void Platform::handleEvent(Ctx& c, const Event& e) {
    if (e.kind == Ev::Decrementer) {
        if (e.a == decGeneration_) decPending_ = true;
        return;
    }
    const auto& h = handlers_[static_cast<uint32_t>(e.kind)];
    if (!h) stopPlatform(c, static_cast<uint32_t>(e.kind), "event with no handler");
    h(c, e);
}

void Platform::processEvents(Ctx& c) {
    Event e;
    while (events_.popDue(now(), e)) handleEvent(c, e);
}

void Platform::rearm(Ctx& c) {
    const uint64_t icount = machine_->icount;
    uint64_t dl = ~0ull;
    if (!events_.empty()) dl = std::max<uint64_t>(retiredAt(events_.nextTick()), icount + 1);
    const bool pending = externalPending() || decPending_ || !callbacks_.empty();
    if (pending) dl = std::min<uint64_t>(dl, icount + ((c.msr & kMsrEE) ? 16 : 256));
    deadline_ = dl;
    hookInstalled_ = true;
    rt::preemptDeadlineSet(dl);
}

void Platform::preempt(Ctx& c) {
    Platform& p = get();
    p.processEvents(c);
    p.deliver(c);
    p.rearm(c);
}

void Platform::deliver(Ctx& c) {
    for (int guard = 0; guard < 8; ++guard) {
        processEvents(c);
        if (!(c.msr & kMsrEE)) return;
        if (externalPending()) {
            rearm(c);
            deliverException(c, 4, 0, 0, 0);
        } else if (decPending_) {
            decPending_ = false;
            rearm(c);
            deliverException(c, 8, 0, 0, 0);
        } else if (!callbacks_.empty()) {
            const PendingCallback cb = callbacks_.front();
            callbacks_.pop_front();
            rearm(c);
            deliverException(c, 0, cb.fn, cb.r3, cb.r4);
        } else {
            return;
        }
    }
}

void Platform::queueCallback(uint32_t fn, uint32_t r3, uint32_t r4) {
    callbacks_.push_back(PendingCallback{fn, r3, r4});
    lowerDeadline(machine_->icount + 1);
}

// ---- Threads and fibers --------------------------------------------------------------------------------

Thread* Platform::threadOf(Ctx& c) {
    const auto it = byCtx_.find(&c);
    if (it == byCtx_.end()) stopPlatform(c, 0, "execution on a context the platform does not know");
    return it->second;
}

Ctx* Platform::irqCtx(Thread* t, Ctx& from) {
    if (static_cast<size_t>(t->depth) >= t->irq.size()) {
        if (t->irq.size() >= 3) stopPlatform(from, 0, "interrupt nesting deeper than 3");
        Ctx* n = rt::createThreadContext(from);
        t->irq.push_back(n);
        byCtx_[n] = t;
    }
    Ctx* i = t->irq[static_cast<size_t>(t->depth)];
    // The activation sees the interrupted registers, as the handler does on hardware.
    std::memcpy(i->r, from.r, sizeof i->r);
    for (int k = 0; k < 32; ++k) i->f[k] = from.f[k];
    i->cr = from.cr; i->so = from.so; i->ov = from.ov; i->ca = from.ca; i->xerbc = from.xerbc;
    i->ctr = from.ctr; i->fpscr = from.fpscr;
    std::memcpy(i->gqr, from.gqr, sizeof i->gqr);
    std::memcpy(i->spr, from.spr, sizeof i->spr);
    std::memcpy(i->sr, from.sr, sizeof i->sr);
    i->msr = from.msr;
    i->pc = from.pc;
    i->lr = 0;
    i->sp = 0;
    return i;
}

namespace {
constexpr uint32_t kOffState = 0x1A2, kOffSrr0 = 0x198, kOffSrr1 = 0x19C;
constexpr uint32_t kOsCurrentContext = 0x800000D4;
constexpr uint32_t kCurrentThread = 0x800000E4;                    // read by SelectThread
// OSThread.state: OSExitThread stores 8 (moribund, joinable) or 0 (exited, detached); a running, ready or waiting thread has 2, 1 or 4.
constexpr uint32_t kOffThreadState = 0x2C8;
bool isExitedState(uint16_t state) { return state == 0 || state == 8; }
constexpr uint32_t kFnOSDisableScheduler = 0x801B6B98, kFnOSEnableScheduler = 0x801B6BD8, kFnOSReschedule = 0x801B70A4;
}

// Writes the machine state of c into the guest OSContext at osctx the way the exception vector and the
// external-interrupt handler tail do. Used for interrupts; OSSaveContext writes a smaller subset.
void Platform::writeContextImage(Ctx& c, uint32_t osctx, bool exception) {
    GuestMem& m = mem_;
    for (uint32_t i = 0; i < 32; ++i) m.w32(osctx + 4 * i, c.r[i]);
    m.w32(osctx + 0x80, c.cr);
    m.w32(osctx + 0x84, c.lr);
    m.w32(osctx + 0x88, c.ctr);
    m.w32(osctx + 0x8C, rt::xerValue(c));
    for (uint32_t i = 1; i < 8; ++i) m.w32(osctx + 0x1A4 + 4 * i, c.gqr[i]);
    m.w32(osctx + kOffSrr0, c.pc);
    m.w32(osctx + kOffSrr1, c.msr);
    if (exception) m.w16(osctx + kOffState, static_cast<uint16_t>(m.r16(osctx + kOffState) | 2));
}

// Delivers an exception to the thread running on c: external interrupt (4), decrementer (8), or, with
// exception 0, a callback envelope for an API-level completion (fn, r3, r4).
void Platform::deliverException(Ctx& c, uint32_t exception, uint32_t fn, uint32_t r3, uint32_t r4) {
    Thread* t = threadOf(c);
    const uint32_t x = mem_.r32(kOsCurrentContext);
    if (!x) stopPlatform(c, exception, "interrupt delivered while the OS has no current context");
    writeContextImage(c, x, true);
    byOsCtx_[x] = t;
    t->osContext = x;
    Ctx* i = irqCtx(t, c);
    i->msr = c.msr & ~kMsrEE;
    i->r[3] = exception;
    i->r[4] = x;
    ++t->depth;
    trace().line("irq depth=%d exc=%u fn=%08X t=%llu pc=%08X", t->depth, exception, fn, static_cast<unsigned long long>(now()), c.pc);
    if (exception == 0) {
        rt::callGuest(*i, kFnOSDisableScheduler);
        i->r[3] = r3;
        i->r[4] = r4;
        rt::callGuest(*i, fn);
        rt::callGuest(*i, kFnOSEnableScheduler);
        rt::callGuest(*i, kFnOSReschedule);
        // The envelope ends like __OSDispatchInterrupt: OSLoadContext(context) returns to the interrupted code.
        mem_.w16(x + kOffState, static_cast<uint16_t>(mem_.r16(x + kOffState) & ~2u));
    } else {
        const uint32_t table = mem_.r32(i->r[13] - 21468);
        const uint32_t handler = table ? mem_.r32(table + 4 * exception) : 0;
        if (!handler) stopPlatform(c, exception, "exception with no registered handler");
        rt::callGuest(*i, handler);
    }
    --t->depth;
    rt::syncHostFp(c.fpscr);
}

void Platform::switchTo(Thread* t) {
    Thread* me = cur_;
    cur_ = t;
    SwitchToFiber(t->fiber);
    // resumed: whoever switched back set cur_ to this thread
    reapDead();
    Ctx* top = me->depth > 0 ? me->irq[static_cast<size_t>(me->depth - 1)] : me->base;
    rt::syncHostFp(top->fpscr);
}

void Platform::osSaveContext(Ctx& c) {
    const uint32_t x = c.r[3];
    GuestMem& m = mem_;
    m.w32(x + 4, c.r[1]);
    m.w32(x + 8, c.r[2]);
    for (uint32_t i = 13; i < 32; ++i) m.w32(x + 0x34 + 4 * (i - 13), c.r[i]);
    for (uint32_t i = 1; i < 8; ++i) m.w32(x + 0x1A4 + 4 * i, c.gqr[i]);
    m.w32(x + 0x80, c.cr);
    m.w32(x + 0x84, c.lr);
    m.w32(x + kOffSrr0, c.lr);
    m.w32(x + kOffSrr1, c.msr);
    m.w32(x + 0x88, c.ctr);
    m.w32(x + 0x8C, rt::xerValue(c));
    m.w32(x + 0xC, 1);
    Thread* t = threadOf(c);
    if (x == mem_.r32(kCurrentThread) && isExitedState(mem_.r16(x + kOffThreadState))) {
        // SelectThread saves the context of a thread that has just exited (OSExitThread marked it exited or moribund and the scheduler
        // is leaving it for good). The original never loads this image again as a continuation: a later OSCreateThread
        // rewrites it with OSInitContext. Binding it to this fiber would let that new thread resume the dead one.
        t->exiting = true;                 // its fiber is abandoned once it loads the next context (it still runs the idle loop first)
        byOsCtx_.erase(x);
        t->osContext = 0;
        trace().line("thread %u exited ctx=%08X t=%llu", t->id, x, static_cast<unsigned long long>(now()));
        c.r[3] = 0;
        return;
    }
    byOsCtx_[x] = t;
    t->osContext = x;
    c.r[3] = 0;
}

// Frees the fibers and contexts of exited threads. Only called from a fiber other than the dead one.
void Platform::reapDead() {
    for (size_t i = 0; i < threads_.size();) {
        Thread* t = threads_[i].get();
        if (!t->dead || t == cur_) { ++i; continue; }
        for (Ctx* k : t->irq) { byCtx_.erase(k); rt::destroyThreadContext(k); }
        byCtx_.erase(t->base);
        if (t->fiber && t->fiber != GetCurrentFiber()) DeleteFiber(t->fiber);
        if (t->id != 1) rt::destroyThreadContext(t->base);
        threads_.erase(threads_.begin() + static_cast<std::ptrdiff_t>(i));
        ++reaped;
    }
}

void Platform::osLoadContext(Ctx& c) {
    const uint32_t x = c.r[3];
    Thread* me = threadOf(c);
    const uint16_t state = mem_.r16(x + kOffState);
    if (state & 2) mem_.w16(x + kOffState, static_cast<uint16_t>(state & ~2u));
    Thread* target = nullptr;              // no container iterator may live across a fiber switch (debug iterators are registered with the container)
    {
        const auto it = byOsCtx_.find(x);
        if (it != byOsCtx_.end()) target = it->second;
    }
    if (me->exiting && target != me) {
        // The exiting thread leaves for good: nothing may resume this fiber, so drop every context bound to it (the idle context
        // may have been bound to it by an interrupt taken in the idle loop).
        me->dead = true;
        for (auto it = byOsCtx_.begin(); it != byOsCtx_.end();) { if (it->second == me) it = byOsCtx_.erase(it); else ++it; }
    }
    if (target) {
        if (target->dead) stopPlatform(c, x, "OSLoadContext of the context of an exited thread");
        if (target == me) return;          // returning into the activation that owns this context
        trace().line("switch thread %u -> %u ctx=%08X t=%llu", me->id, target->id, x, static_cast<unsigned long long>(now()));
        switchTo(target);
        return;
    }
    startThreadFromImage(c, x);
}

void Platform::startThreadFromImage(Ctx& from, uint32_t x) {
    GuestMem& m = mem_;
    auto th = std::make_unique<Thread>();
    Thread* t = th.get();
    t->id = nextThreadId_++;
    Ctx* n = rt::createThreadContext(from);
    t->base = n;
    for (uint32_t i = 0; i < 32; ++i) n->r[i] = m.r32(x + 4 * i);
    n->cr = m.r32(x + 0x80);
    n->lr = 0;
    n->ctr = m.r32(x + 0x88);
    rt::setXer(*n, m.r32(x + 0x8C));
    for (uint32_t i = 0; i < 8; ++i) n->gqr[i] = m.r32(x + 0x1A4 + 4 * i);
    n->msr = m.r32(x + kOffSrr1);
    n->fpscr = from.fpscr;
    std::memcpy(n->spr, from.spr, sizeof n->spr);
    std::memcpy(n->sr, from.sr, sizeof n->sr);
    t->entry = m.r32(x + kOffSrr0);
    t->exitFn = m.r32(x + 0x84);
    t->started = true;
    t->osContext = x;
    t->fiber = CreateFiberEx(128 * 1024, 4 * 1024 * 1024, 0, &Platform::fiberMain, t);
    if (!t->fiber) stopPlatform(from, x, "CreateFiberEx failed");
    byCtx_[n] = t;
    byOsCtx_[x] = t;
    trace().line("thread %u start ctx=%08X entry=%08X sp=%08X t=%llu", t->id, x, t->entry, n->r[1], static_cast<unsigned long long>(now()));
    threads_.push_back(std::move(th));
    Thread* me = threadOf(from);
    (void)me;
    switchTo(t);
}

void __stdcall Platform::fiberMain(void* param) {
    Thread* t = static_cast<Thread*>(param);
    Platform& p = get();
    try {
        p.reapDead();
        rt::setXer(*t->base, 0);
        rt::callGuest(*t->base, t->entry);
        if (t->exitFn) {
            rt::callGuest(*t->base, t->exitFn);
        }
        // A thread's exit function never returns on hardware.
        p.exitReason_ = t->exitFn ? "thread exit function returned" : "boot entry returned";
        p.entryReturned_ = t->exitFn == 0;
    } catch (const rt::Stop& s) {
        p.report_.stopped = true;
        p.report_.stop = s;
        p.report_.threadId = t->id;
        p.report_.ctx = t->depth > 0 ? t->irq[static_cast<size_t>(t->depth - 1)] : t->base;
    } catch (const std::exception& e) {
        p.report_.stopped = true;
        p.report_.stop = rt::Stop{StopKind::Contract, 0, 0, "host exception in guest execution"};
        p.report_.ctx = t->base;
        p.exitReason_ = e.what();
    }
    SwitchToFiber(p.supervisor_);
}

bool Platform::run(Ctx& boot, uint32_t entry) {
    if (!IsThreadAFiber()) supervisor_ = ConvertThreadToFiber(nullptr); else supervisor_ = GetCurrentFiber();
    auto th = std::make_unique<Thread>();
    Thread* t = th.get();
    t->id = nextThreadId_++;
    t->base = &boot;
    t->entry = entry;
    t->started = true;
    t->fiber = CreateFiberEx(128 * 1024, 4 * 1024 * 1024, 0, &Platform::fiberMain, t);
    byCtx_[&boot] = t;
    threads_.push_back(std::move(th));
    cur_ = t;
    exiting_ = false;
    report_ = StopReport();
    rt::syncHostFp(boot.fpscr);
    SwitchToFiber(t->fiber);
    return !report_.stopped && !exiting_ && entryReturned_;
}

void Platform::requestExit(const char* why) {
    exiting_ = true;
    exitReason_ = why;
    if (!cur_) return;                     // not running guest code (unit tests): only record the request
    SwitchToFiber(supervisor_);
}

}  // namespace awl::hle
