#include <condition_variable>
#include <algorithm>
#include <vector>
// Kernel objects, handles, threads, waits and synchronization for the Madden NFL Arcade host.
#include "kernel.h"
#include "platform.h"
#include <unordered_map>
#include <map>
#include <thread>
#include <atomic>
#include <chrono>
#include <ctime>

#define IMP(n) PPC_FUNC(__imp__##n)
#define TRACE(n) HostTrace("__imp__" #n, ctx)

std::mutex g_kLock;
std::condition_variable g_kCond;

// ---------------------------------------------------------------- handles
static std::mutex g_hLock;
static std::unordered_map<uint32_t, KObject*> g_handles;
static uint32_t g_nextHandle = 0x100;

uint32_t KNewHandle(KObject* o) {
    std::lock_guard<std::mutex> l(g_hLock);
    uint32_t h = g_nextHandle; g_nextHandle += 4;
    g_handles[h] = o; return h;
}
KObject* KLookup(uint32_t h) {
    std::lock_guard<std::mutex> l(g_hLock);
    auto it = g_handles.find(h); return it == g_handles.end() ? nullptr : it->second;
}
bool KCloseHandle(uint32_t h) {
    KObject* o;
    { std::lock_guard<std::mutex> l(g_hLock);
      auto it = g_handles.find(h); if (it == g_handles.end()) return false;
      o = it->second; g_handles.erase(it); }
    if (--o->refs == 0) {
        if (o->file) { if (o->file->fp) fclose(o->file->fp); delete o->file; }
        delete o;
    }
    return true;
}

// ---------------------------------------------------------------- dispatcher objects in guest memory
// DISPATCHER_HEADER: u8 Type, u8 Absolute, u8 Size, u8 Inserted, i32 SignalState, LIST_ENTRY WaitListHead
static inline uint8_t ObjType(uint32_t o) { return g_base[o]; }
static inline int32_t ObjSignal(uint32_t o) { return (int32_t)ReadU32(o + 4); }
static inline void SetSignal(uint32_t o, int32_t v) { WriteU32(o + 4, (uint32_t)v); }

uint32_t KAllocDispatcher(uint8_t type, int32_t signal, uint32_t size) {
    uint32_t o = GuestAlloc(size);
    g_base[o] = type; g_base[o + 2] = (uint8_t)(size / 4);
    SetSignal(o, signal);
    WriteU32(o + 8, o + 8); WriteU32(o + 12, o + 8);
    return o;
}
void KSignal(uint32_t) { g_kCond.notify_all(); }

static std::unordered_map<uint32_t, uint32_t> g_mutantOwner;   // mutant -> owning thread object

// ---------------------------------------------------------------- threads
struct ThreadExit { uint32_t code; };
struct HostThread {
    uint32_t kthread = 0, pcr = 0, stackTop = 0, id = 0;
    uint32_t xapiStartup = 0, start = 0, param = 0;
    bool suspended = false;
    std::thread th;
};
static std::unordered_map<uint32_t, HostThread*> g_threadsByObj;   // kthread -> host thread
static std::atomic<uint32_t> g_nextThreadId{1};
static thread_local uint32_t t_kthread = 0;
static thread_local PPCContext* t_ctx = nullptr;   // this thread's guest state (crash reports)
PPCContext* CurrentGuestContext() { return t_ctx; }
// every running guest thread's context (for the debug thread dump when the window closes)
static std::mutex g_ctxListLock;
static std::vector<std::pair<uint32_t, PPCContext*>> g_ctxList;
static std::unordered_map<PPCContext*, uint32_t> g_ctxKthread;
static void RegisterCtx(uint32_t id, PPCContext* c) { std::lock_guard<std::mutex> l(g_ctxListLock); g_ctxList.push_back({id, c}); g_ctxKthread[c] = t_kthread; }
uint32_t GuestThreadKthread(PPCContext* c) { std::lock_guard<std::mutex> l(g_ctxListLock); auto it = g_ctxKthread.find(c); return it == g_ctxKthread.end() ? 0 : it->second; }
static void UnregisterCtx(PPCContext* c) {
    std::lock_guard<std::mutex> l(g_ctxListLock);
    g_ctxList.erase(std::remove_if(g_ctxList.begin(), g_ctxList.end(), [c](auto& e) { return e.second == c; }), g_ctxList.end());
}
std::vector<std::pair<uint32_t, PPCContext*>> GuestThreadContexts() { std::lock_guard<std::mutex> l(g_ctxListLock); return g_ctxList; }
static thread_local std::unordered_map<uint32_t, uint32_t> t_tls;

uint32_t KCurrentThreadObject() { return t_kthread; }

// Sets up stack + PCR + KTHREAD for a guest thread. KPCR+0x100 = current KTHREAD, KTHREAD+0x14C = thread id,
// KTHREAD+0x160 = last error (these are the offsets the game reads).
static void SetupThreadContext(PPCContext& ctx, uint32_t stackSize, uint32_t& kthreadOut, uint32_t& idOut) {
    if (stackSize < 0x40000) stackSize = 0x40000;
    stackSize = (stackSize + 0xFFFF) & ~0xFFFFu;
    uint32_t stack = GuestAlloc(stackSize, 0x10000);
    uint32_t pcr = GuestAlloc(0x1000, 0x1000);
    uint32_t kthread = KAllocDispatcher(kThread, 0, 0x400);
    uint32_t id = g_nextThreadId++;
    WriteU32(pcr + 0x100, kthread);
    WriteU32(pcr + 0x70, stack + stackSize);   // stack base
    WriteU32(pcr + 0x74, stack);               // stack limit
    WriteU32(kthread + 0x14C, id);
    WriteU32(kthread + 0x5C, stack + stackSize);
    WriteU32(kthread + 0x60, stack);
    ctx.fpscr.loadFromHost();
    ctx.r1.u64 = stack + stackSize - 0x200;
    ctx.r13.u64 = pcr;
    kthreadOut = kthread; idOut = id;
}

void KInitMainThread(PPCContext& ctx) {
    uint32_t kt, id;
    SetupThreadContext(ctx, 0x400000, kt, id);
    t_kthread = kt; t_ctx = &ctx; RegisterCtx(id, &ctx);
    fprintf(stderr, "[host] main thread kthread=%08X id=%u stack top=%08X\n", kt, id, ctx.r1.u32);
}

void KInitHostGuestThread(PPCContext& ctx, uint32_t stackSize) {
    uint32_t kt, id;
    SetupThreadContext(ctx, stackSize, kt, id);
    t_kthread = kt; t_ctx = &ctx; RegisterCtx(id, &ctx);
}

static void ThreadMain(HostThread* t) {
    {   // wait while suspended
        std::unique_lock<std::mutex> l(g_kLock);
        g_kCond.wait(l, [t] { return !t->suspended; });
    }
    t_kthread = t->kthread;
    PPCContext ctx{};
    t_ctx = &ctx; RegisterCtx(t->id, &ctx);
    uint32_t kt, id;
    // the stack/PCR were allocated by the creator; rebuild the context from them
    ctx.fpscr.loadFromHost();
    ctx.r1.u64 = t->stackTop; ctx.r13.u64 = t->pcr;
    uint32_t exitCode = 0;
    try {
        if (t->xapiStartup) {
            ctx.r3.u64 = t->start; ctx.r4.u64 = t->param;
            (PPC_LOOKUP_FUNC(g_base, t->xapiStartup))(ctx, g_base);
        } else {
            ctx.r3.u64 = t->param;
            (PPC_LOOKUP_FUNC(g_base, t->start))(ctx, g_base);
        }
        exitCode = ctx.r3.u32;
    } catch (const ThreadExit& e) { exitCode = e.code; }
    (void)kt; (void)id;
    UnregisterCtx(&ctx);
    if (g_trace) fprintf(stderr, "[host] thread %u exited (%u)\n", t->id, exitCode);
    std::lock_guard<std::mutex> l(g_kLock);
    WriteU32(t->kthread + 0x14, exitCode);
    SetSignal(t->kthread, 1);
    g_kCond.notify_all();
}

// ExCreateThread(pHandle, StackSize, pThreadId, XapiThreadStartup, StartAddress, StartContext, CreationFlags)
IMP(ExCreateThread) {
    TRACE(ExCreateThread);
    uint32_t pHandle = ctx.r3.u32, stackSize = ctx.r4.u32, pId = ctx.r5.u32;
    auto* t = new HostThread();
    t->xapiStartup = ctx.r6.u32; t->start = ctx.r7.u32; t->param = ctx.r8.u32;
    t->suspended = (ctx.r9.u32 & 1) != 0;
    PPCContext tmp{};
    SetupThreadContext(tmp, stackSize, t->kthread, t->id);
    t->stackTop = tmp.r1.u32; t->pcr = tmp.r13.u32;
    { std::lock_guard<std::mutex> l(g_kLock); g_threadsByObj[t->kthread] = t; }
    auto* o = new KObject{KObject::Dispatcher}; o->guestPtr = t->kthread;
    uint32_t h = KNewHandle(o);
    if (pHandle) WriteU32(pHandle, h);
    if (pId) WriteU32(pId, t->id);
    fprintf(stderr, "[host] ExCreateThread id=%u start=%08X startup=%08X param=%08X%s\n", t->id, t->start, t->xapiStartup, t->param, t->suspended ? " (suspended)" : "");
    t->th = std::thread(ThreadMain, t);
    t->th.detach();
    ctx.r3.u64 = STATUS_SUCCESS;
}
IMP(ExTerminateThread) { TRACE(ExTerminateThread); throw ThreadExit{ctx.r3.u32}; }
IMP(NtResumeThread) {
    TRACE(NtResumeThread);
    uint32_t obj = KHandleToGuestObject(ctx.r3.u32);
    std::lock_guard<std::mutex> l(g_kLock);
    auto it = g_threadsByObj.find(obj);
    if (ctx.r4.u32) WriteU32(ctx.r4.u32, it != g_threadsByObj.end() && it->second->suspended ? 1 : 0);
    if (it != g_threadsByObj.end()) it->second->suspended = false;
    g_kCond.notify_all();
    ctx.r3.u64 = STATUS_SUCCESS;
}
IMP(KeSetAffinityThread) { TRACE(KeSetAffinityThread); if (ctx.r5.u32) WriteU32(ctx.r5.u32, 0x3F); ctx.r3.u64 = 0; }
IMP(KeSetBasePriorityThread) { TRACE(KeSetBasePriorityThread); ctx.r3.u64 = 0; }
IMP(KeQueryBasePriorityThread) { TRACE(KeQueryBasePriorityThread); ctx.r3.u64 = 0; }
IMP(KeGetCurrentProcessType) { TRACE(KeGetCurrentProcessType); ctx.r3.u64 = 1; }   // title process
IMP(NtYieldExecution) { TRACE(NtYieldExecution); std::this_thread::yield(); ctx.r3.u64 = 0; }
IMP(KeEnableFpuExceptions) { TRACE(KeEnableFpuExceptions); }

uint32_t KHandleToGuestObject(uint32_t h) {
    if (h == 0xFFFFFFFE) return t_kthread;             // NtCurrentThread()
    KObject* o = KLookup(h);
    return o && o->kind == KObject::Dispatcher ? o->guestPtr : 0;
}

// ---------------------------------------------------------------- waiting
// timeout: guest pointer to LARGE_INTEGER (100ns; negative = relative, positive = absolute FILETIME), 0 = infinite
static bool TimeoutDeadline(uint32_t pTimeout, std::chrono::steady_clock::time_point& dl) {
    if (!pTimeout) return false;
    int64_t v = (int64_t)ReadU64(pTimeout);
    int64_t rel100ns;
    if (v < 0) rel100ns = -v;
    else {
        int64_t now = (int64_t)PlatFileTimeNow();
        rel100ns = v > now ? v - now : 0;
    }
    dl = std::chrono::steady_clock::now() + std::chrono::nanoseconds(rel100ns * 100);
    return true;
}
static bool IsSignaled(uint32_t o) {
    if (ObjType(o) == kMutant) { auto it = g_mutantOwner.find(o); return ObjSignal(o) > 0 || (it != g_mutantOwner.end() && it->second == t_kthread); }
    return ObjSignal(o) > 0;
}
static void Acquire(uint32_t o) {
    switch (ObjType(o)) {
    case kEventSynchronization: case kTimerSynchronization: SetSignal(o, 0); break;
    case kSemaphore: SetSignal(o, ObjSignal(o) - 1); break;
    case kMutant: SetSignal(o, ObjSignal(o) - 1); g_mutantOwner[o] = t_kthread; break;
    default: break;
    }
}
static uint32_t WaitObjects(const std::vector<uint32_t>& objs, bool waitAll, uint32_t pTimeout) {
    std::chrono::steady_clock::time_point dl;
    bool hasTimeout = TimeoutDeadline(pTimeout, dl);
    std::unique_lock<std::mutex> l(g_kLock);
    for (;;) {
        if (waitAll) {
            bool all = true; for (auto o : objs) if (o && !IsSignaled(o)) { all = false; break; }
            if (all) { for (auto o : objs) if (o) Acquire(o); return STATUS_SUCCESS; }
        } else {
            for (size_t i = 0; i < objs.size(); i++) if (objs[i] && IsSignaled(objs[i])) { Acquire(objs[i]); return (uint32_t)i; }
        }
        if (hasTimeout) { if (g_kCond.wait_until(l, dl) == std::cv_status::timeout && std::chrono::steady_clock::now() >= dl) {
            // one last check
            if (!waitAll) { for (size_t i = 0; i < objs.size(); i++) if (objs[i] && IsSignaled(objs[i])) { Acquire(objs[i]); return (uint32_t)i; } }
            return STATUS_TIMEOUT; } }
        else g_kCond.wait_for(l, std::chrono::milliseconds(50));
    }
}
// KeWaitForSingleObject(Object, WaitReason, WaitMode, Alertable, Timeout*)
IMP(KeWaitForSingleObject) { TRACE(KeWaitForSingleObject); ctx.r3.u64 = WaitObjects({ctx.r3.u32}, false, ctx.r7.u32); }
// NtWaitForSingleObjectEx(Handle, WaitMode, Alertable, Timeout*)
IMP(NtWaitForSingleObjectEx) {
    TRACE(NtWaitForSingleObjectEx);
    uint32_t o = KHandleToGuestObject(ctx.r3.u32);
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    ctx.r3.u64 = WaitObjects({o}, false, ctx.r6.u32);
}
// KeWaitForMultipleObjects(Count, Objects*, WaitType, WaitReason, WaitMode, Alertable, Timeout*, WaitBlockArray)
IMP(KeWaitForMultipleObjects) {
    TRACE(KeWaitForMultipleObjects);
    std::vector<uint32_t> objs;
    for (uint32_t i = 0; i < ctx.r3.u32; i++) objs.push_back(ReadU32(ctx.r4.u32 + 4 * i));
    ctx.r3.u64 = WaitObjects(objs, ctx.r5.u32 == 0, ctx.r9.u32);
}
// KeDelayExecutionThread(WaitMode, Alertable, Interval*)
IMP(KeDelayExecutionThread) {
    TRACE(KeDelayExecutionThread);
    int64_t v = ctx.r5.u32 ? (int64_t)ReadU64(ctx.r5.u32) : 0;
    if (v < 0) std::this_thread::sleep_for(std::chrono::nanoseconds(-v * 100)); else std::this_thread::yield();
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------- events
static uint32_t NewDispatcherHandle(uint8_t type, int32_t signal, uint32_t size, uint32_t* pObj = nullptr) {
    uint32_t o = KAllocDispatcher(type, signal, size);
    auto* k = new KObject{KObject::Dispatcher}; k->guestPtr = o;
    if (pObj) *pObj = o;
    return KNewHandle(k);
}
// NtCreateEvent(pHandle, ObjAttr, EventType(0=notification,1=sync), InitialState)
IMP(NtCreateEvent) {
    TRACE(NtCreateEvent);
    uint32_t h = NewDispatcherHandle(ctx.r5.u32 ? kEventSynchronization : kEventNotification, ctx.r6.u32 ? 1 : 0, 0x10);
    WriteU32(ctx.r3.u32, h); ctx.r3.u64 = STATUS_SUCCESS;
}
static uint32_t SetEventObj(uint32_t o, int32_t v) {
    std::lock_guard<std::mutex> l(g_kLock);
    int32_t prev = ObjSignal(o); SetSignal(o, v); g_kCond.notify_all(); return (uint32_t)prev;
}
IMP(NtSetEvent)   { TRACE(NtSetEvent);   uint32_t o = KHandleToGuestObject(ctx.r3.u32); if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; } uint32_t p = SetEventObj(o, 1); if (ctx.r4.u32) WriteU32(ctx.r4.u32, p); ctx.r3.u64 = 0; }
IMP(NtClearEvent) { TRACE(NtClearEvent); uint32_t o = KHandleToGuestObject(ctx.r3.u32); if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; } SetEventObj(o, 0); ctx.r3.u64 = 0; }
IMP(KeSetEvent)   { TRACE(KeSetEvent);   ctx.r3.u64 = SetEventObj(ctx.r3.u32, 1); }
IMP(KeResetEvent) { TRACE(KeResetEvent); ctx.r3.u64 = SetEventObj(ctx.r3.u32, 0); }

// ---------------------------------------------------------------- semaphores / mutants
// NtCreateSemaphore(pHandle, ObjAttr, InitialCount, MaximumCount)
IMP(NtCreateSemaphore) {
    TRACE(NtCreateSemaphore);
    uint32_t o; uint32_t h = NewDispatcherHandle(kSemaphore, (int32_t)ctx.r5.u32, 0x14, &o);
    WriteU32(o + 0x10, ctx.r6.u32);
    WriteU32(ctx.r3.u32, h); ctx.r3.u64 = STATUS_SUCCESS;
}
// NtReleaseSemaphore(Handle, ReleaseCount, PreviousCount*)
IMP(NtReleaseSemaphore) {
    TRACE(NtReleaseSemaphore);
    uint32_t o = KHandleToGuestObject(ctx.r3.u32); if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::lock_guard<std::mutex> l(g_kLock);
    int32_t prev = ObjSignal(o), lim = (int32_t)ReadU32(o + 0x10);
    if (prev + (int32_t)ctx.r4.u32 > lim) { ctx.r3.u64 = STATUS_SEMAPHORE_LIMIT_EXCEEDED; return; }
    SetSignal(o, prev + (int32_t)ctx.r4.u32);
    if (ctx.r5.u32) WriteU32(ctx.r5.u32, (uint32_t)prev);
    g_kCond.notify_all(); ctx.r3.u64 = STATUS_SUCCESS;
}
// NtCreateMutant(pHandle, ObjAttr, InitialOwner)
IMP(NtCreateMutant) {
    TRACE(NtCreateMutant);
    uint32_t o; uint32_t h = NewDispatcherHandle(kMutant, ctx.r5.u32 ? 0 : 1, 0x20, &o);
    if (ctx.r5.u32) { std::lock_guard<std::mutex> l(g_kLock); g_mutantOwner[o] = t_kthread; }
    WriteU32(ctx.r3.u32, h); ctx.r3.u64 = STATUS_SUCCESS;
}
// NtReleaseMutant(Handle, PreviousCount*)
IMP(NtReleaseMutant) {
    TRACE(NtReleaseMutant);
    uint32_t o = KHandleToGuestObject(ctx.r3.u32); if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::lock_guard<std::mutex> l(g_kLock);
    int32_t prev = ObjSignal(o);
    if (ctx.r4.u32) WriteU32(ctx.r4.u32, (uint32_t)prev);
    SetSignal(o, prev + 1);
    if (prev + 1 > 0) g_mutantOwner.erase(o);
    g_kCond.notify_all(); ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------- object manager
IMP(NtClose) { TRACE(NtClose); ctx.r3.u64 = KCloseHandle(ctx.r3.u32) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE; }
// NtDuplicateObject(SourceHandle, pTargetHandle, Options)
IMP(NtDuplicateObject) {
    TRACE(NtDuplicateObject);
    uint32_t src = ctx.r3.u32;
    KObject* o;
    if (src == 0xFFFFFFFE) { o = new KObject{KObject::Dispatcher}; o->guestPtr = t_kthread; }
    else { o = KLookup(src); if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; } o->refs++; }
    if (ctx.r4.u32) WriteU32(ctx.r4.u32, KNewHandle(o));
    if (ctx.r5.u32 & 1) KCloseHandle(src);   // DUPLICATE_CLOSE_SOURCE
    ctx.r3.u64 = STATUS_SUCCESS;
}
// ObReferenceObjectByHandle(Handle, ObjectType, pObject)
IMP(ObReferenceObjectByHandle) {
    TRACE(ObReferenceObjectByHandle);
    uint32_t o = KHandleToGuestObject(ctx.r3.u32);
    if (!o) { KObject* k = KLookup(ctx.r3.u32); if (k) { if (!k->guestPtr) k->guestPtr = KAllocDispatcher(0x11, 0, 0x20); o = k->guestPtr; } }
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (ctx.r5.u32) WriteU32(ctx.r5.u32, o);
    ctx.r3.u64 = STATUS_SUCCESS;
}
IMP(ObDereferenceObject) { TRACE(ObDereferenceObject); ctx.r3.u64 = 0; }
IMP(ObReferenceObject) { TRACE(ObReferenceObject); ctx.r3.u64 = 0; }

// ---------------------------------------------------------------- critical sections & spinlocks
// RTL_CRITICAL_SECTION: DISPATCHER_HEADER(0x10), LockCount(+0x10), RecursionCount(+0x14), OwningThread(+0x18)
static std::mutex g_csMapLock;
static std::unordered_map<uint32_t, std::recursive_mutex*> g_cs;
static std::recursive_mutex* CS(uint32_t a) {
    std::lock_guard<std::mutex> l(g_csMapLock);
    auto& m = g_cs[a]; if (!m) m = new std::recursive_mutex(); return m;
}
static void CsInit(uint32_t a) {
    g_base[a] = 1; WriteU32(a + 0x10, 0xFFFFFFFF); WriteU32(a + 0x14, 0); WriteU32(a + 0x18, 0); CS(a);
}
IMP(RtlInitializeCriticalSection) { TRACE(RtlInitializeCriticalSection); CsInit(ctx.r3.u32); ctx.r3.u64 = 0; }
IMP(RtlInitializeCriticalSectionAndSpinCount) { TRACE(RtlInitializeCriticalSectionAndSpinCount); CsInit(ctx.r3.u32); ctx.r3.u64 = 0; }
IMP(RtlEnterCriticalSection) {
    TRACE(RtlEnterCriticalSection);
    uint32_t a = ctx.r3.u32; CS(a)->lock();
    WriteU32(a + 0x14, ReadU32(a + 0x14) + 1); WriteU32(a + 0x18, t_kthread);
}
IMP(RtlTryEnterCriticalSection) {
    TRACE(RtlTryEnterCriticalSection);
    uint32_t a = ctx.r3.u32;
    if (CS(a)->try_lock()) { WriteU32(a + 0x14, ReadU32(a + 0x14) + 1); WriteU32(a + 0x18, t_kthread); ctx.r3.u64 = 1; }
    else ctx.r3.u64 = 0;
}
IMP(RtlLeaveCriticalSection) {
    TRACE(RtlLeaveCriticalSection);
    uint32_t a = ctx.r3.u32; uint32_t rc = ReadU32(a + 0x14);
    if (rc) { WriteU32(a + 0x14, rc - 1); if (rc == 1) WriteU32(a + 0x18, 0); }
    CS(a)->unlock();
}
IMP(KeEnterCriticalRegion) { TRACE(KeEnterCriticalRegion); }
IMP(KeLeaveCriticalRegion) { TRACE(KeLeaveCriticalRegion); }
static std::recursive_mutex* Spin(uint32_t a) { return CS(a | 0x1); }   // separate key space from critical sections
IMP(KfAcquireSpinLock) { TRACE(KfAcquireSpinLock); Spin(ctx.r3.u32)->lock(); ctx.r3.u64 = 0; }
IMP(KfReleaseSpinLock) { TRACE(KfReleaseSpinLock); Spin(ctx.r3.u32)->unlock(); }
IMP(KeAcquireSpinLockAtRaisedIrql) { TRACE(KeAcquireSpinLockAtRaisedIrql); Spin(ctx.r3.u32)->lock(); }
IMP(KeReleaseSpinLockFromRaisedIrql) { TRACE(KeReleaseSpinLockFromRaisedIrql); Spin(ctx.r3.u32)->unlock(); }
IMP(KeLockL2) { TRACE(KeLockL2); ctx.r3.u64 = 0; }
IMP(KeUnlockL2) { TRACE(KeUnlockL2); }

// ---------------------------------------------------------------- TLS (per host thread)
static std::atomic<uint32_t> g_nextTls{0};
IMP(KeTlsAlloc)    { TRACE(KeTlsAlloc); ctx.r3.u32 = g_nextTls++; }
IMP(KeTlsFree)     { TRACE(KeTlsFree); ctx.r3.u32 = 1; }
IMP(KeTlsGetValue) { TRACE(KeTlsGetValue); auto it = t_tls.find(ctx.r3.u32); ctx.r3.u64 = it == t_tls.end() ? 0 : it->second; }
IMP(KeTlsSetValue) { TRACE(KeTlsSetValue); t_tls[ctx.r3.u32] = ctx.r4.u32; ctx.r3.u32 = 1; }

// ---------------------------------------------------------------- APCs/DPCs/timers: minimal
IMP(KeInitializeApc) { TRACE(KeInitializeApc); }
IMP(KeInitializeDpc) { TRACE(KeInitializeDpc); }
IMP(KeInsertQueueApc) { TRACE(KeInsertQueueApc); ctx.r3.u64 = 0; }
IMP(NtQueueApcThread) { TRACE(NtQueueApcThread); ctx.r3.u64 = STATUS_SUCCESS; }
// NtCreateTimer(pHandle, ObjAttr, TimerType)
IMP(NtCreateTimer) {
    TRACE(NtCreateTimer);
    uint32_t h = NewDispatcherHandle(ctx.r5.u32 ? kTimerSynchronization : kTimerNotification, 0, 0x28);
    WriteU32(ctx.r3.u32, h); ctx.r3.u64 = STATUS_SUCCESS;
}
// NtSetTimerEx(Handle, DueTime*, ApcRoutine, ApcMode, ApcContext, Resume, Period, PreviousState*)
IMP(NtSetTimerEx) {
    TRACE(NtSetTimerEx);
    uint32_t o = KHandleToGuestObject(ctx.r3.u32); if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int64_t due = (int64_t)ReadU64(ctx.r4.u32); int32_t period = (int32_t)ctx.r9.u32;
    SetEventObj(o, 0);
    std::thread([o, due, period] {
        auto ns = std::chrono::nanoseconds((due < 0 ? -due : 0) * 100);
        do { std::this_thread::sleep_for(ns); SetEventObj(o, 1); ns = std::chrono::milliseconds(period); } while (period > 0 && ObjType(o) != 0xFF);
    }).detach();
    ctx.r3.u64 = STATUS_SUCCESS;
}
IMP(NtCancelTimer) { TRACE(NtCancelTimer); ctx.r3.u64 = STATUS_SUCCESS; }

// Wait on handles with a millisecond timeout (0xFFFFFFFF = infinite), for host-side helpers such as WSA events.
uint32_t KWaitHandlesMs(const std::vector<uint32_t>& handles, bool waitAll, uint32_t ms) {
    std::vector<uint32_t> objs; for (auto h : handles) objs.push_back(KHandleToGuestObject(h));
    if (ms == 0xFFFFFFFF) return WaitObjects(objs, waitAll, 0);
    uint32_t t = GuestAlloc(8); WriteU64(t, (uint64_t)(-(int64_t)ms * 10000));
    return WaitObjects(objs, waitAll, t);
}
uint32_t KCreateEventHandle(bool manualReset, bool initial) {
    return NewDispatcherHandle(manualReset ? kEventNotification : kEventSynchronization, initial ? 1 : 0, 0x10);
}
bool KSetEventHandle(uint32_t h, bool state) { uint32_t o = KHandleToGuestObject(h); if (!o) return false; SetEventObj(o, state ? 1 : 0); return true; }

