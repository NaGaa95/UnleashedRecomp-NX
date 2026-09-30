// Handheld GPU profile through apm. Based on nfsmw-nx's sdk/src/ui/switch_apm.cpp, which
// documents why each step is there:
//  - apmSetPerformanceConfiguration returns before pcv applies the clocks, so a read on the next
//    line shows the old values; the result is checked by polling for 1.5 s.
//  - 0x92220007 is "GPU 460.8 + memory 1600"; 0x92220008 is "GPU 460.8 + memory 1331.2", the
//    pair commercial games run at. Only the latter is requested, and it is reverted if the
//    memory clock moves anyway (pcv reimposes the memory clock of the active configuration,
//    so lowering it by hand with clkrst does not stick).
//  - The apm session is kept open for the life of the process.

#include <os/switch_perf.h>

#include <switch.h>

#include <cstdio>

namespace
{
    constexpr u32 HANDHELD_GPU_460_MEMORY_1331 = 0x92220008;
    constexpr u32 CLOCK_MARGIN_HZ = 20000000;
    constexpr u64 POLL_STEP_NS = 150000000;
    constexpr int POLL_STEPS = 10;

    Thread g_apmThread;

    bool ReadClocks(u32& gpuHz, u32& memoryHz)
    {
        gpuHz = 0;
        memoryHz = 0;

        if (R_FAILED(clkrstInitialize()))
            return false;

        bool ok = true;
        ClkrstSession session{};

        if (R_SUCCEEDED(clkrstOpenSession(&session, PcvModuleId_GPU, 3)))
        {
            ok &= R_SUCCEEDED(clkrstGetClockRate(&session, &gpuHz));
            clkrstCloseSession(&session);
        }
        else
        {
            ok = false;
        }

        if (R_SUCCEEDED(clkrstOpenSession(&session, PcvModuleId_EMC, 3)))
        {
            ok &= R_SUCCEEDED(clkrstGetClockRate(&session, &memoryHz));
            clkrstCloseSession(&session);
        }
        else
        {
            ok = false;
        }

        clkrstExit();
        return ok;
    }

    void ApmThreadMain(void*)
    {
        if (R_FAILED(apmInitialize()))
        {
            fprintf(stderr, "[apm] apm is not available; clocks unchanged.\n");
            return;
        }

        ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
        if (R_FAILED(apmGetPerformanceMode(&mode)) || mode != ApmPerformanceMode_Normal)
        {
            fprintf(stderr, "[apm] Console is not in handheld mode; clocks unchanged.\n");
            return;
        }

        u32 originalConfiguration = 0;
        const bool hasOriginal = R_SUCCEEDED(apmGetPerformanceConfiguration(mode, &originalConfiguration));

        u32 gpuBefore = 0;
        u32 memoryBefore = 0;
        if (!ReadClocks(gpuBefore, memoryBefore))
        {
            // Without reading the clocks there is no way to guarantee the memory clock stays put.
            fprintf(stderr, "[apm] Unable to read clocks through clkrst; clocks unchanged.\n");
            return;
        }

        if (R_FAILED(apmSetPerformanceConfiguration(mode, HANDHELD_GPU_460_MEMORY_1331)))
        {
            fprintf(stderr, "[apm] Configuration 0x%08X is not available on this firmware.\n", HANDHELD_GPU_460_MEMORY_1331);
            return;
        }

        u32 gpuAfter = 0;
        u32 memoryAfter = 0;
        bool memoryStable = true;

        for (int i = 0; i < POLL_STEPS; i++)
        {
            svcSleepThread(POLL_STEP_NS);

            if (!ReadClocks(gpuAfter, memoryAfter))
            {
                memoryStable = false;
                break;
            }

            const u32 delta = memoryAfter > memoryBefore ? memoryAfter - memoryBefore : memoryBefore - memoryAfter;
            if (delta > CLOCK_MARGIN_HZ)
            {
                memoryStable = false;
                break;
            }
        }

        if (!memoryStable)
        {
            if (hasOriginal)
                apmSetPerformanceConfiguration(mode, originalConfiguration);

            fprintf(stderr, "[apm] 0x%08X changed the memory clock (%.1f -> %.1f MHz); reverted to 0x%08X.\n",
                HANDHELD_GPU_460_MEMORY_1331, memoryBefore / 1e6, memoryAfter / 1e6, originalConfiguration);
            return;
        }

        fprintf(stderr, "[apm] GPU %.1f -> %.1f MHz, memory stays at %.1f MHz (configuration 0x%08X).\n",
            gpuBefore / 1e6, gpuAfter / 1e6, memoryAfter / 1e6, HANDHELD_GPU_460_MEMORY_1331);
    }
}

void os::switch_perf::StartHandheldGpuBoost()
{
    // A libnx thread rather than a detached std::thread: detach() can throw on Horizon.
    if (R_SUCCEEDED(threadCreate(&g_apmThread, ApmThreadMain, nullptr, nullptr, 0x10000, 0x3B, -2)))
        threadStart(&g_apmThread);
}
