#pragma once

#include <kernel/xdm.h>
#include <condition_variable>

// Use pthreads directly on macOS to be able to increase default stack size.
#if defined(__APPLE__) || defined(__SWITCH__)

#define USE_PTHREAD 1
#endif

#ifdef USE_PTHREAD
#include <pthread.h>
#endif

#define CURRENT_THREAD_HANDLE uint32_t(-2)

struct GuestThreadContext
{
    PPCContext ppcContext{};
    uint8_t* thread = nullptr;

    GuestThreadContext(uint32_t cpuNumber);
    ~GuestThreadContext();
};

struct GuestThreadParams
{
    uint32_t function;
    uint32_t value;
    uint32_t flags;
};

struct GuestThreadHandle : KernelObject
{
    GuestThreadParams params;
    std::atomic<bool> suspended;
    std::mutex suspendMutex;
    std::condition_variable suspendCv;
#ifdef USE_PTHREAD
    pthread_t thread;
    bool threadCreated = false;
    std::atomic<bool> joined = false;
#else
    std::thread thread;
#endif
#if defined(__SWITCH__)
    // Kernel handle of the thread (captured at thread entry) and the Horizon
    // priority requested before the handle became available. Lets
    // KeSetBasePriorityThread work cross-thread and pre-start.
    std::atomic<uint32_t> kernelHandle = 0;
    std::atomic<int32_t> pendingHorizonPriority = -1;
    // Core named by SetThreadIdealProcessor before the handle existed ([Switch] SwitchThreadIdealCores).
    std::atomic<int32_t> pendingIdealCore = -1;
#endif

    GuestThreadHandle(const GuestThreadParams& params);
    ~GuestThreadHandle() override;

    uint32_t GetThreadId() const;
    void Suspend();
    void Resume();
    void WaitUntilResumed();

    uint32_t Wait(uint32_t timeout) override;
};

struct GuestThread
{
    static uint32_t Start(const GuestThreadParams& params);
    static GuestThreadHandle* Start(const GuestThreadParams& params, uint32_t* threadId);

    static uint32_t GetCurrentThreadId();
    static void SetLastError(uint32_t error);

#ifdef _WIN32
    static void SetThreadName(uint32_t threadId, const char* name);
#endif
};
