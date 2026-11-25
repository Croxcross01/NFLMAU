// Shared kernel-object layer for the Madden NFL Arcade host.
#pragma once
#include "host.h"
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <cstdio>

// NTSTATUS values
constexpr uint32_t STATUS_SUCCESS = 0, STATUS_TIMEOUT = 0x102, STATUS_PENDING = 0x103,
    STATUS_NO_MORE_FILES = 0x80000006, STATUS_UNSUCCESSFUL = 0xC0000001, STATUS_NOT_IMPLEMENTED = 0xC0000002,
    STATUS_INVALID_HANDLE = 0xC0000008, STATUS_INVALID_PARAMETER = 0xC000000D, STATUS_END_OF_FILE = 0xC0000011,
    STATUS_ACCESS_DENIED = 0xC0000022, STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034, STATUS_OBJECT_NAME_COLLISION = 0xC0000035,
    STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A, STATUS_MUTANT_NOT_OWNED = 0xC0000046, STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047,
    STATUS_NO_SUCH_FILE = 0xC000000F;

// Dispatcher object types (DISPATCHER_HEADER.Type)
enum : uint8_t { kEventNotification = 0, kEventSynchronization = 1, kMutant = 2, kSemaphore = 5, kThread = 6,
                 kTimerNotification = 8, kTimerSynchronization = 9 };

// Every waitable object lives in guest memory as a DISPATCHER_HEADER (so game code that touches it directly works)
// and all signalling goes through one global lock + condition variable.
extern std::mutex g_kLock;
extern std::condition_variable g_kCond;

struct HostFile {
    std::string hostPath; FILE* fp = nullptr; bool isDir = false; uint64_t pos = 0;
    struct ArchiveOverlay* overlay = nullptr;   // archive with replaced entries (archive_overlay.h)
    std::vector<std::string> dirEntries; size_t dirPos = 0; bool dirListed = false;
};
struct KObject {
    enum Kind { Dispatcher, File } kind;
    uint32_t guestPtr = 0;   // dispatcher header in guest memory
    HostFile* file = nullptr;
    int refs = 1;
};

uint32_t KAllocDispatcher(uint8_t type, int32_t signal, uint32_t size = 0x20);
uint32_t KNewHandle(KObject* o);
KObject* KLookup(uint32_t handle);
bool KCloseHandle(uint32_t handle);
uint32_t KHandleToGuestObject(uint32_t handle);   // dispatcher pointer for handle (incl. pseudo-handles)
uint32_t KCurrentThreadObject();
void KSignal(uint32_t obj);                         // wake waiters after state change
std::string GuestAnsiString(uint32_t pAnsi);        // reads ANSI_STRING
std::string GuestObjectName(uint32_t pObjAttr);     // reads OBJECT_ATTRIBUTES.ObjectName
uint32_t GuestArgStack(PPCContext& ctx, int index); // argument index (0 = r3) beyond r10 from the stack
