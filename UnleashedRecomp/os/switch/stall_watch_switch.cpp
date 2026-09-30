#include <os/switch_stall_watch.h>
#include <os/switch_cpu_profiler.h>

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

// See os/switch_stall_watch.h. Adapted from the watchdog of battd_nx (source/diag.c): threads are only
// paused to read their registers and copy the top of their stack, and resumed before anything is formatted,
// allocated or written, so the watchdog never waits on a lock a paused thread holds; the stack is read from
// the copy made while the thread was paused, never live (a running thread can return out of those frames or
// exit, which unmaps its stack).

namespace
{
    constexpr uint64_t POLL_NS = 250'000'000ull;
    constexpr uint64_t REDUMP_NS = 2'000'000'000ull;
    constexpr uint64_t HITCH_NS = 100'000'000ull;          // a frame slower than this is a hitch
    constexpr uint64_t HITCH_REPORT_NS = 1'000'000'000ull; // hitches are summed up at most once a second
    constexpr int MAX_DUMPS_PER_STALL = 5;
    constexpr size_t MAX_THREADS = 64;
    constexpr size_t STACK_COPY_BYTES = 16384;
    constexpr int MAX_FRAMES = 16;         // frame-pointer chain
    constexpr int MAX_STACK_ADDRESSES = 24; // return addresses found scanning the stack

    std::atomic<uint64_t> g_lastFrameTick{ 0 };
    std::atomic<uint64_t> g_frames{ 0 };
    std::atomic<uint32_t> g_hitches{ 0 };
    std::atomic<uint64_t> g_longestHitchTicks{ 0 };
    uint64_t g_thresholdTicks = 0;
    uint64_t g_hitchTicks = 0;
    void (*g_reporter)(std::string&) = nullptr;
    Thread g_thread;
    bool g_started = false;

    uint64_t g_base = 0;
    uint64_t g_size = 0;

    bool InModule(uint64_t address)
    {
        return address >= g_base && address - g_base < g_size;
    }

    bool IsReturnAddress(uint64_t address)
    {
        if ((address & 3) != 0 || !InModule(address) || address - g_base < 4)
            return false;

        uint32_t previous;
        memcpy(&previous, reinterpret_cast<const void*>(address - 4), sizeof(previous));
        return (previous & 0xFC000000u) == 0x94000000u || (previous & 0xFFFFFC1Fu) == 0xD63F0000u;
    }

    // One thread's state, captured while it was paused.
    struct Capture
    {
        char name[48];
        uint32_t handle;
        bool paused;
        bool contextRead;
        uint64_t ticks;
        ThreadContext context;
        uint64_t stackBase;   // target address of stack[0]
        size_t stackBytes;
    };

    Capture g_captures[MAX_THREADS];
    uint8_t g_stacks[MAX_THREADS][STACK_COPY_BYTES];
    os::switch_cpu_profiler::ThreadInfo g_threads[MAX_THREADS];

    struct TicksSeen
    {
        uint32_t handle;
        uint64_t ticks;
    };
    TicksSeen g_ticksSeen[MAX_THREADS];

    uint64_t ThreadTicks(uint32_t handle)
    {
        uint64_t ticks = 0;
        if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, UINT64_MAX)))
            return 0;
        return ticks;
    }

    uint64_t PreviousTicks(uint32_t handle, uint64_t ticks)
    {
        for (TicksSeen& seen : g_ticksSeen)
        {
            if (seen.handle == handle)
            {
                const uint64_t previous = seen.ticks;
                seen.ticks = ticks;
                return previous;
            }
        }
        for (TicksSeen& seen : g_ticksSeen)
        {
            if (seen.handle == 0)
            {
                seen.handle = handle;
                seen.ticks = ticks;
                break;
            }
        }
        return ticks;
    }

    bool StackRead(const Capture& capture, const uint8_t* stack, uint64_t address, uint64_t& value)
    {
        if ((address & 7) != 0 || address < capture.stackBase || address + 8 > capture.stackBase + capture.stackBytes)
            return false;
        memcpy(&value, stack + (address - capture.stackBase), sizeof(value));
        return true;
    }

    void CaptureThreads(size_t count)
    {
        const uint32_t self = threadGetCurHandle();
        for (size_t i = 0; i < count; i++)
        {
            Capture& capture = g_captures[i];
            capture = {};
            memcpy(capture.name, g_threads[i].name, sizeof(capture.name));
            capture.handle = g_threads[i].handle;
            capture.ticks = ThreadTicks(capture.handle);
            if (capture.handle == 0 || capture.handle == self)
                continue;

            // Nothing between pause and resume may allocate, lock or log.
            if (R_FAILED(svcSetThreadActivity(capture.handle, ThreadActivity_Paused)))
                continue;

            capture.paused = true;
            if (R_SUCCEEDED(svcGetThreadContext3(&capture.context, capture.handle)))
            {
                capture.contextRead = true;

                MemoryInfo info{};
                u32 pageInfo = 0;
                const uint64_t sp = capture.context.sp & ~uint64_t(7);
                if (R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, sp)) && info.type != MemType_Unmapped &&
                    info.type != MemType_Io && (info.perm & Perm_R) != 0)
                {
                    const uint64_t end = std::min<uint64_t>(info.addr + info.size, sp + STACK_COPY_BYTES);
                    if (end > sp)
                    {
                        memcpy(g_stacks[i], reinterpret_cast<const void*>(sp), size_t(end - sp));
                        capture.stackBase = sp;
                        capture.stackBytes = size_t(end - sp);
                    }
                }
            }

            svcSetThreadActivity(capture.handle, ThreadActivity_Runnable);
        }
    }

    void AppendOffset(std::string& out, uint64_t address)
    {
        char text[32];
        if (InModule(address))
            snprintf(text, sizeof(text), " +0x%07llx", (unsigned long long)(address - g_base));
        else
            snprintf(text, sizeof(text), " 0x%llx", (unsigned long long)address);
        out += text;
    }

    void Dump(int episode, int dump, uint64_t now, uint64_t lastFrame)
    {
        const size_t count = os::switch_cpu_profiler::CopyThreads(g_threads, MAX_THREADS);
        CaptureThreads(count);

        // Everything is running again from here on.
        std::string out;
        out.reserve(16384);
        char line[256];
        snprintf(line, sizeof(line), "[stall] #%d dump %d: no frame for %.1f s (%llu frames so far); module base 0x%llx; "
            "run tools/switch-cpu-profile.py on this log to name the offsets\n",
            episode, dump, double(armTicksToNs(now - lastFrame)) / 1e9, (unsigned long long)g_frames.load(std::memory_order_relaxed),
            (unsigned long long)g_base);
        out += line;

        if (g_reporter != nullptr)
        {
            out += "[stall]   renderer: ";
            g_reporter(out);
            out += '\n';
        }

        for (size_t i = 0; i < count; i++)
        {
            const Capture& capture = g_captures[i];
            const uint64_t previous = PreviousTicks(capture.handle, capture.ticks);
            const double ranMs = double(armTicksToNs(capture.ticks - previous)) / 1e6;

            snprintf(line, sizeof(line), "[stall]   %s: %s, %.1f ms of CPU since the last look", capture.name,
                !capture.paused ? "not paused" : !capture.contextRead ? "no context" : "paused", ranMs);
            out += line;

            if (capture.contextRead)
            {
                // At an SVC instruction (Horizon reports a thread blocked in a system call there; round 9: only
                // right after it was checked) or right after one: in that system call (0x0B sleep, 0x18 wait
                // synchronization, 0x1A arbitrate lock, 0x1C condition variable, 0x21 IPC, 0x34 wait for address).
                const uint64_t pc = capture.context.pc.x;
                if (InModule(pc) && (pc & 3) == 0)
                {
                    uint32_t at;
                    uint32_t previous = 0;
                    memcpy(&at, reinterpret_cast<const void*>(pc), sizeof(at));
                    if (pc - g_base >= 4)
                        memcpy(&previous, reinterpret_cast<const void*>(pc - 4), sizeof(previous));

                    const uint32_t svc = (at & 0xFFE0001Fu) == 0xD4000001u ? at : previous;
                    if ((svc & 0xFFE0001Fu) == 0xD4000001u)
                    {
                        snprintf(line, sizeof(line), "; in svc 0x%02x", (svc >> 5) & 0xFFFF);
                        out += line;
                    }
                }

                out += "; pc";
                AppendOffset(out, capture.context.pc.x);
                out += " lr";
                AppendOffset(out, capture.context.lr);

                // Frame-pointer chain (the port's code keeps frame pointers; recompiled game code does not).
                out += "; frames";
                uint64_t fp = capture.context.fp;
                for (int depth = 0; depth < MAX_FRAMES; depth++)
                {
                    uint64_t nextFp, returnAddress;
                    if (!StackRead(capture, g_stacks[i], fp, nextFp) || !StackRead(capture, g_stacks[i], fp + 8, returnAddress) ||
                        returnAddress == 0)
                    {
                        break;
                    }
                    AppendOffset(out, returnAddress);
                    if (nextFp <= fp)
                        break;
                    fp = nextFp;
                }

                // Every return address on the stack, which also covers the recompiled code.
                out += "; stack";
                int found = 0;
                for (size_t offset = 0; offset + 8 <= capture.stackBytes && found < MAX_STACK_ADDRESSES; offset += 8)
                {
                    uint64_t value;
                    memcpy(&value, g_stacks[i] + offset, sizeof(value));
                    if (IsReturnAddress(value))
                    {
                        AppendOffset(out, value);
                        found++;
                    }
                }
            }
            out += '\n';
        }

        os::switch_cpu_profiler::WriteLog(out);
    }

    void WatchThread(void*)
    {
        int episode = 0;
        bool stalled = false;
        int dumps = 0;
        uint64_t lastDump = 0;
        uint64_t stallStart = 0;
        uint64_t lastHitchReport = 0;

        while (true)
        {
            svcSleepThread(POLL_NS);

            const uint64_t lastFrame = g_lastFrameTick.load(std::memory_order_acquire);
            if (lastFrame == 0)
                continue; // No frame yet (boot).

            const uint64_t now = armGetSystemTick();

            // Frames that took over 100 ms but not long enough for a stall: the stutters. (A stall counts too,
            // once it ends.)
            if (now - lastHitchReport >= armNsToTicks(HITCH_REPORT_NS))
            {
                const uint32_t hitches = g_hitches.exchange(0, std::memory_order_relaxed);
                if (hitches != 0)
                {
                    const uint64_t longest = g_longestHitchTicks.exchange(0, std::memory_order_relaxed);
                    char line[192];
                    snprintf(line, sizeof(line), "[hitch] %u frame%s over %llu ms since the last report, the longest %.0f ms "
                        "(%llu frames so far)\n", hitches, hitches == 1 ? "" : "s", (unsigned long long)(HITCH_NS / 1'000'000ull),
                        double(armTicksToNs(longest)) / 1e6, (unsigned long long)g_frames.load(std::memory_order_relaxed));
                    os::switch_cpu_profiler::WriteLog(line, strlen(line));
                }
                lastHitchReport = now;
            }
            if (now - lastFrame >= g_thresholdTicks)
            {
                if (!stalled)
                {
                    stalled = true;
                    episode++;
                    dumps = 0;
                    stallStart = lastFrame;
                }

                if (dumps < MAX_DUMPS_PER_STALL && (dumps == 0 || now - lastDump >= armNsToTicks(REDUMP_NS)))
                {
                    Dump(episode, ++dumps, now, lastFrame);
                    lastDump = armGetSystemTick();
                }
            }
            else if (stalled)
            {
                stalled = false;
                char line[160];
                snprintf(line, sizeof(line), "[stall] #%d ended: frames resumed after %.1f s without one\n", episode,
                    double(armTicksToNs(lastFrame - stallStart)) / 1e9);
                os::switch_cpu_profiler::WriteLog(line, strlen(line));
            }
        }
    }
}

void os::switch_stall_watch::Start(double seconds)
{
    if (g_started || seconds <= 0.0)
        return;

    g_base = os::switch_cpu_profiler::ModuleBase();
    g_size = os::switch_cpu_profiler::ModuleSize();
    g_thresholdTicks = armNsToTicks(uint64_t(seconds * 1e9));
    g_hitchTicks = armNsToTicks(HITCH_NS);

    // Above the game's threads (0x1C-0x3B), like the CPU sampler, so a busy thread cannot hide the stall; it
    // sleeps between looks.
    if (R_SUCCEEDED(threadCreate(&g_thread, WatchThread, nullptr, nullptr, 0x10000, 0x1E, -2)))
    {
        svcSetThreadCoreMask(g_thread.handle, -1, 0x7);
        if (R_SUCCEEDED(threadStart(&g_thread)))
        {
            g_started = true;
            fprintf(stderr, "Stall watchdog: thread dumps after %.1f s without a frame, frames over %llu ms counted\n",
                seconds, (unsigned long long)(HITCH_NS / 1'000'000ull));
        }
    }
}

void os::switch_stall_watch::OnFrame()
{
    if (!g_started)
    {
        g_frames.store(g_frames.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return;
    }

    const uint64_t now = armGetSystemTick();
    const uint64_t last = g_lastFrameTick.load(std::memory_order_relaxed);
    if (last != 0 && now - last >= g_hitchTicks)
    {
        const uint64_t gap = now - last;
        g_hitches.fetch_add(1, std::memory_order_relaxed);
        uint64_t longest = g_longestHitchTicks.load(std::memory_order_relaxed);
        while (gap > longest && !g_longestHitchTicks.compare_exchange_weak(longest, gap, std::memory_order_relaxed))
            ;
    }

    // Only this thread writes these two.
    g_frames.store(g_frames.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    g_lastFrameTick.store(now, std::memory_order_release);
}

uint64_t os::switch_stall_watch::FrameCount()
{
    return g_frames.load(std::memory_order_relaxed);
}

void os::switch_stall_watch::SetStateReporter(void (*reporter)(std::string& out))
{
    g_reporter = reporter;
}
