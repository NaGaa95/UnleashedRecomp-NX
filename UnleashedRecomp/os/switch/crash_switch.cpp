#include <os/switch_crash.h>
#include <os/switch_cpu_profiler.h>
#include <os/switch_stall_watch.h>

#include <switch.h>
#include <switch_build_id.h>
#include <plume_render_interface.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// See os/switch_crash.h. Nothing here takes a lock or allocates once a report has started: the thread that
// crashed may hold the heap's, stdio's or the profiler's. The report is formatted into a static buffer and
// written with the file system service directly (fsdev's FsFileSystem, not a FILE).

namespace
{
    constexpr const char* CRASH_PATH = "/switch/UnleashedRecomp/crash.log"; // On sdmc.
    constexpr size_t STACK_DUMP_BYTES = 0x200;
    constexpr uint32_t BACKTRACE_DEPTH = 24;
    constexpr uint32_t STACK_SCAN_WORDS = 2048; // 16 KB of stack searched for return addresses.
    constexpr uint32_t STACK_SCAN_MAX = 32;

    bool g_alsoStderr = false;

    // One report per process: a second thread that faults meanwhile waits for the first to break.
    std::atomic<bool> g_reporting{ false };

    char g_report[24 * 1024];
    size_t g_reportLength = 0;

    // The driver's latest messages (errors), kept for a GPU report.
    constexpr uint32_t DRIVER_MESSAGES = 8;
    constexpr size_t DRIVER_MESSAGE_BYTES = 480;
    char g_driverMessages[DRIVER_MESSAGES][DRIVER_MESSAGE_BYTES];
    std::atomic<uint32_t> g_driverMessageCount{ 0 };

    __attribute__((format(printf, 1, 2)))
    void Append(const char* format, ...)
    {
        if (g_reportLength >= sizeof(g_report) - 1)
            return;

        va_list args;
        va_start(args, format);
        const int written = vsnprintf(g_report + g_reportLength, sizeof(g_report) - g_reportLength, format, args);
        va_end(args);

        if (written > 0)
            g_reportLength = std::min(sizeof(g_report) - 1, g_reportLength + size_t(written));
    }

    uint64_t ModuleBase()
    {
        return os::switch_cpu_profiler::ModuleBase();
    }

    bool InModule(uint64_t address)
    {
        const uint64_t base = ModuleBase();
        return base != 0 && address >= base && address - base < os::switch_cpu_profiler::ModuleSize();
    }

    // "+0x..." for an address inside the executable (what tools/switch-cpu-profile.py names), the address otherwise.
    void AppendAddress(uint64_t address)
    {
        if (InModule(address))
            Append(" +0x%07llx", (unsigned long long)(address - ModuleBase()));
        else
            Append(" 0x%llx", (unsigned long long)address);
    }

    bool Readable(uint64_t address, size_t size)
    {
        if (address < 0x1000 || address + size < address)
            return false;

        uint64_t at = address;
        const uint64_t end = address + size;
        while (at < end)
        {
            MemoryInfo info{};
            u32 pageInfo = 0;
            if (R_FAILED(svcQueryMemory(&info, &pageInfo, at)) || info.type == MemType_Unmapped || (info.perm & Perm_R) == 0)
                return false;

            const uint64_t blockEnd = info.addr + info.size;
            if (blockEnd <= at)
                return false;
            at = blockEnd;
        }

        return true;
    }

    void AppendHeader(const char* what)
    {
        const uint64_t uptimeMs = armTicksToNs(armGetSystemTick()) / 1'000'000ull;
        u64 now = 0;
        timeGetCurrentTime(TimeType_UserSystemClock, &now);

        char name[48] = "unregistered";
        os::switch_cpu_profiler::TryGetCurrentThreadName(name, sizeof(name));
        u64 threadId = 0;
        svcGetThreadId(&threadId, CUR_THREAD_HANDLE);

        Append("\n[crash] ===== %s =====\n", what);
        Append("[crash] build %s; posix time %llu; %llu.%03llu s after boot of the process clock; %llu frames presented; "
            "thread %s (id 0x%llx); module base 0x%llx\n", UNLEASHED_RECOMP_SWITCH_BUILD_ID, (unsigned long long)now,
            (unsigned long long)(uptimeMs / 1000), (unsigned long long)(uptimeMs % 1000),
            (unsigned long long)os::switch_stall_watch::FrameCount(), name, (unsigned long long)threadId,
            (unsigned long long)ModuleBase());
    }

    // Appends the report to crash.log (and to stderr.log with SwitchLog when `stderrToo`).
    void WriteReport(bool stderrToo)
    {
        if (FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc"))
        {
            fsFsCreateDirectory(fs, "/switch");
            fsFsCreateDirectory(fs, "/switch/UnleashedRecomp");
            fsFsCreateFile(fs, CRASH_PATH, 0, 0); // Fails when it exists, which is fine.

            FsFile file;
            if (R_SUCCEEDED(fsFsOpenFile(fs, CRASH_PATH, FsOpenMode_Write | FsOpenMode_Append, &file)))
            {
                s64 size = 0;
                fsFileGetSize(&file, &size);
                fsFileWrite(&file, size, g_report, g_reportLength, FsWriteOption_Flush);
                fsFileClose(&file);
            }
        }

        if (stderrToo && g_alsoStderr)
        {
            fwrite(g_report, 1, g_reportLength, stderr);
            fflush(stderr);
        }
    }

    [[noreturn]] void Break()
    {
        svcBreak(BreakReason_Panic, 0, 0);
        for (;;)
            svcSleepThread(1'000'000'000ull);
    }

    const char* ExceptionName(u32 desc)
    {
        switch (desc)
        {
        case ThreadExceptionDesc_InstructionAbort: return "instruction abort";
        case ThreadExceptionDesc_MisalignedPC: return "misaligned pc";
        case ThreadExceptionDesc_MisalignedSP: return "misaligned sp";
        case ThreadExceptionDesc_SError: return "SError";
        case ThreadExceptionDesc_BadSVC: return "bad svc";
        case ThreadExceptionDesc_Trap: return "trap (undefined instruction, illegal state)";
        case ThreadExceptionDesc_Other: return "data abort or other (see esr)";
        default: return "unknown";
        }
    }

    void OnDriverMessage(const char* message)
    {
        const uint32_t index = g_driverMessageCount.fetch_add(1, std::memory_order_relaxed) % DRIVER_MESSAGES;
        snprintf(g_driverMessages[index], DRIVER_MESSAGE_BYTES, "%s", message);

        if (g_alsoStderr)
            fprintf(stderr, "[vulkan] %s\n", message);
    }

    void OnDeviceLost(const char* call)
    {
        if (g_reporting.exchange(true))
        {
            for (;;)
                svcSleepThread(1'000'000'000ull);
        }

        AppendHeader("GPU LOST");
        Append("[crash] %s returned VK_ERROR_DEVICE_LOST: the GPU channel is gone and nothing more can be drawn\n", call);

        const uint32_t count = g_driverMessageCount.load(std::memory_order_relaxed);
        const uint32_t first = count > DRIVER_MESSAGES ? count - DRIVER_MESSAGES : 0;
        if (count == 0)
            Append("[crash] the driver reported no error message before it\n");
        for (uint32_t i = first; i < count; i++)
            Append("[crash] driver: %s\n", g_driverMessages[i % DRIVER_MESSAGES]);

        Append("[crash] ============== END ==============\n");
        WriteReport(true);
        Break();
    }
}

// libnx turns user-mode exception handling on when these are defined. The handler runs on the thread that faulted,
// on this stack.
extern "C"
{
    alignas(16) __attribute__((used)) u8 __nx_exception_stack[0x8000];
    __attribute__((used)) u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

    __attribute__((used)) void __libnx_exception_handler(ThreadExceptionDump* context)
    {
        if (g_reporting.exchange(true))
        {
            for (;;)
                svcSleepThread(1'000'000'000ull);
        }

        // The fault first: if anything below fails, this line is still worth reading.
        AppendHeader("CPU EXCEPTION");
        Append("[crash] %s (0x%x); pc", ExceptionName(context->error_desc), context->error_desc);
        AppendAddress(context->pc.x);
        Append("; lr");
        AppendAddress(context->lr.x);
        Append("; far 0x%llx; esr 0x%08x; pstate 0x%08x\n", (unsigned long long)context->far.x, context->esr, context->pstate);

        for (uint32_t i = 0; i < 28; i += 4)
        {
            Append("[crash] x%-2u %016llx  x%-2u %016llx  x%-2u %016llx  x%-2u %016llx\n",
                i, (unsigned long long)context->cpu_gprs[i].x, i + 1, (unsigned long long)context->cpu_gprs[i + 1].x,
                i + 2, (unsigned long long)context->cpu_gprs[i + 2].x, i + 3, (unsigned long long)context->cpu_gprs[i + 3].x);
        }
        Append("[crash] x28 %016llx  fp  %016llx  sp  %016llx\n", (unsigned long long)context->cpu_gprs[28].x,
            (unsigned long long)context->fp.x, (unsigned long long)context->sp.x);

        // Frame-pointer chain: [fp] is the caller's fp, [fp + 8] the return address.
        Append("[crash] frames");
        uint64_t fp = context->fp.x;
        for (uint32_t depth = 0; depth < BACKTRACE_DEPTH && fp != 0 && Readable(fp, 16); depth++)
        {
            const uint64_t next = reinterpret_cast<const uint64_t*>(fp)[0];
            const uint64_t ret = reinterpret_cast<const uint64_t*>(fp)[1];
            if (ret == 0)
                break;
            AppendAddress(ret);
            if (next <= fp)
                break;
            fp = next;
        }
        Append("\n");

        // Return addresses on the stack (the frame-pointer chain misses leaf and frame-less code, and recompiled
        // functions often keep no frame pointer at all).
        const uint64_t sp = context->sp.x;
        Append("[crash] stack");
        uint32_t found = 0;
        for (uint32_t i = 0; i < STACK_SCAN_WORDS && found < STACK_SCAN_MAX; i++)
        {
            const uint64_t at = sp + uint64_t(i) * 8;
            if ((at & 0xFFF) == 0 || i == 0)
            {
                if (!Readable(at, 8))
                    break;
            }
            const uint64_t word = *reinterpret_cast<const uint64_t*>(at);
            if (InModule(word) && (word & 3) == 0)
            {
                AppendAddress(word);
                found++;
            }
        }
        Append("\n");

        if (Readable(sp, STACK_DUMP_BYTES))
        {
            for (size_t offset = 0; offset < STACK_DUMP_BYTES; offset += 0x20)
            {
                const uint64_t* q = reinterpret_cast<const uint64_t*>(sp + offset);
                Append("[crash]   sp+%03zx: %016llx %016llx %016llx %016llx\n", offset, (unsigned long long)q[0],
                    (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3]);
            }
        }
        else
        {
            Append("[crash]   stack unreadable\n");
        }

        Append("[crash] ============== END ==============\n");

        // Not stderr: the thread that faulted may hold its lock.
        WriteReport(false);
        Break();
    }
}

void os::switch_crash::Init(bool alsoStderr)
{
    g_alsoStderr = alsoStderr;
    plume::SetSwitchDriverMessageCallback(OnDriverMessage);
    plume::SetSwitchDeviceLostCallback(OnDeviceLost);
}
