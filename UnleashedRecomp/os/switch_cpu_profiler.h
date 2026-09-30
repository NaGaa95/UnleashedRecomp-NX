#pragma once

#if defined(__SWITCH__)

#include <cstddef>
#include <cstdint>
#include <string>

// [Switch] Per-thread CPU accounting and a sampling CPU profiler.
//
// Threads register themselves with a name. The kernel counts the CPU time of every thread; the GPU
// pass profiler report prints each registered thread's share of the last report window
// (AppendThreadCpuUsage). With [Switch] SwitchCpuProfiler = true, a sampler thread also pauses each
// registered thread every 2 ms, reads where it is (program counter) and resumes it; every 30 seconds
// it writes the hottest code addresses of each thread to stderr.log, as offsets into the executable
// that tools/switch-cpu-profile.py turns into function names (recompiled game functions included).
namespace os::switch_cpu_profiler
{
    // The name is copied (up to 47 characters).
    void RegisterCurrentThread(const char* name);
    void RegisterCurrentThreadWithAddress(const char* prefix, uint32_t guestAddress);
    void UnregisterCurrentThread();

    // Starts the per-thread accounting, and the sampler when asked ([Switch] SwitchCpuProfiler).
    void Start(bool sampler);

    // CPU time of every registered thread since the previous call, as "name 12.3%" entries. Never waits
    // for the sampler: while it holds the thread list, the entries are left out of this report.
    void AppendThreadCpuUsage(std::string& out);

    // The registered threads (kernel handle and name), for the stall watchdog. Returns how many were copied.
    struct ThreadInfo
    {
        uint32_t handle;
        char name[48];
    };
    size_t CopyThreads(ThreadInfo* out, size_t capacity);

    // The calling thread's registered name, for a crash report: false (and `out` untouched) when it has none or the
    // thread list is locked at the moment (the crash handler cannot wait for it).
    bool TryGetCurrentThreadName(char* out, size_t size);

    // Start and size of the executable's code: the offsets every report prints are relative to it.
    uint64_t ModuleBase();
    uint64_t ModuleSize();

    // Appends text to stderr.log in one write. stderr is line-buffered (so that a crash loses nothing), which
    // made every fwrite of a report one SD card write per line: seconds for a CPU profile, during which the
    // writer held stderr's lock (and the profiler its thread list).
    void WriteLog(const char* text, size_t size);
    inline void WriteLog(const std::string& text) { WriteLog(text.data(), text.size()); }
}

#endif
