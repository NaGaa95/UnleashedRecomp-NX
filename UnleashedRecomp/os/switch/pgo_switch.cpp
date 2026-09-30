// Profile dump for the instrumented PGO build (UNLEASHED_RECOMP_SWITCH_PGO=generate). Empty otherwise.
//
// Ported from nfsmw-nx (app/src/nfsmw_pgo.cpp). GCC embeds in the executable, for every object, the
// path UNLEASHED_RECOMP_PGO_DIR/<object path relative to the build folder, '/' replaced by '#'>.gcda,
// with UNLEASHED_RECOMP_PGO_DIR being a folder on the build PC. The --wrap=fopen below redirects those
// paths to sdmc:/switch/UnleashedRecomp/pgo/ with the same file names, so they can be copied back into
// that folder for the "use" build.
//
// The console does not always exit cleanly (closing from HOME kills the process), so the profile is
// not left for exit: a thread dumps and resets the counters every 3 minutes. libgcov adds to what
// each .gcda already holds, so the dumps accumulate into the total for the session and across
// sessions. Play normally for as long as possible: menus, stages, cutscenes, hub worlds.

#if defined(UNLEASHED_RECOMP_PGO_GENERATE) && defined(__SWITCH__)

#include <switch.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>

extern "C"
{
    FILE* __real_fopen(const char* path, const char* mode);
    void __gcov_dump(void);
    void __gcov_reset(void);

    FILE* __wrap_fopen(const char* path, const char* mode)
    {
        static constexpr char PREFIX[] = UNLEASHED_RECOMP_PGO_DIR;

        if (path != nullptr && strncmp(path, PREFIX, sizeof(PREFIX) - 1) == 0)
        {
            const char* rest = path + sizeof(PREFIX) - 1;
            while (*rest == '\\' || *rest == '/')
                rest++;

            std::string redirected = "sdmc:/switch/UnleashedRecomp/pgo/";
            for (; *rest != '\0'; rest++)
                redirected += (*rest == '\\') ? '/' : *rest;

            return __real_fopen(redirected.c_str(), mode);
        }

        return __real_fopen(path, mode);
    }
}

namespace
{
    Thread g_pgoThread;

    void PgoThread(void*)
    {
        for (int dump = 1;; dump++)
        {
            svcSleepThread(180ll * 1000 * 1000 * 1000);

            mkdir("sdmc:/switch/UnleashedRecomp/pgo", 0777);

            const u64 start = armGetSystemTick();
            __gcov_dump();
            __gcov_reset();
            const double ms = double(armTicksToNs(armGetSystemTick() - start)) / 1e6;

            if (FILE* log = __real_fopen("sdmc:/switch/UnleashedRecomp/pgo/dumps.txt", "a"))
            {
                fprintf(log, "dump %d: %.0f ms\n", dump, ms);
                fclose(log);
            }
        }
    }

    __attribute__((constructor)) void StartPgoThread()
    {
        // Default game priority; it works once every 3 minutes.
        if (R_SUCCEEDED(threadCreate(&g_pgoThread, PgoThread, nullptr, nullptr, 0x10000, 0x3B, -2)))
            threadStart(&g_pgoThread);
    }
}

#endif
