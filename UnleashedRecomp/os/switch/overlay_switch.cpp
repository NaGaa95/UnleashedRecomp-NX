// Publishes FPS and render resolution for the console overlays through SaltyNX's shared memory.
//
// The FPS counter of Status Monitor Overlay (and forks such as Horizon OC Monitor) is not computed
// by the overlay: it reads a block that NX-FPS (a SaltyNX plugin) fills in after hooking the game's
// present call (nvnQueuePresentTexture, eglSwapBuffers or vkQueuePresentKHR). This NRO links its own
// Vulkan driver (NVK talks to nvdrv directly), so there is nothing to hook and the field stays empty.
// Here the game writes that block itself.
//
// Ported from nfsmw-nx (sdk/src/ui/switch_saltynx.cpp), which was debugged on the console against
// masagrator/SaltyNX (saltysd_core), masagrator/Status-Monitor-Overlay (source/Utils.hpp) and
// ReverseNX-RT. The Reverse-NX part of that file is not ported: this game does not switch its
// handheld/docked settings from that overlay. Everything the original learned the hard way is kept:
//
//  - The block is SaltyNX's struct NxFpsSharedBlock (174 packed bytes, magic 0x465053). The overlay
//    finds it by scanning the shared page 4 bytes at a time.
//  - The overlay only believes the game is alive if it answers two handshakes quickly: it clears
//    pluginActive and checks it 100 ms later, and it writes 0xFFFF into renderCalls[0].calls and
//    waits for the game to overwrite it. Hence a heartbeat on every present, not once per second.
//  - The overlay averages FPS from FPSticks[10] (tick frequency / mean of the ticks). With the array
//    at zero it shows "inf", so the time between presents is written there.
//  - SaltySD exposes two ports that accept one session each. "SaltySD" serves commands 6 and 7
//    (allocate in the shared page, get its handle); "InjectServ" connects and then closes. The session
//    is released right away, or the overlay could never connect.
//  - A process started through hbloader/forwarder may have a single port session, already used by
//    libnx for "sm:" (0x10801 on connect). Room is made by raising the session limit if the loader
//    allows svcSetResourceLimitLimitValue, otherwise by releasing "sm:" for a few milliseconds.
//  - SaltySD never resets its allocation counter for us, so a block already in the page is reused
//    instead of allocating one per boot.
//  - The page is shared by several clients: the magic is re-checked on every use.

#include <os/switch_overlay.h>

#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
    constexpr uint32_t MAGIC_FPS = 0x465053;   // "SPF": NX-FPS block
    constexpr size_t SHARED_SIZE = 0x1000;     // SaltyNX maps one page
    constexpr uint8_t API_VULKAN = 3;          // 0 unknown, 1 NVN, 2 GL, 3 Vulkan

    struct ResolutionCalls
    {
        uint16_t width;
        uint16_t height;
        uint16_t calls;
    } __attribute__((packed));

    // SaltyNX's struct NxFpsSharedBlock.
    struct NxFpsSharedBlock
    {
        uint32_t magic;
        uint8_t fps;
        float fpsAverage;
        bool pluginActive;
        uint8_t fpsLocked;
        uint8_t fpsMode;
        uint8_t zeroSync;
        uint8_t patchApplied;
        uint8_t api;
        uint32_t ticks[10];
        uint8_t buffers;
        uint8_t setBuffers;
        uint8_t activeBuffers;
        uint8_t setActiveBuffers;
        uint8_t displaySync;
        ResolutionCalls renderCalls[8];
        ResolutionCalls viewportCalls[8];
        bool forceOriginalRefreshRate;
        bool dontForce60InDocked;
        bool forceSuspend;
        uint8_t currentRefreshRate;
        float readSpeedPerSecond;
        uint8_t fpsLockedDocked;
        uint64_t frameNumber;
        int8_t expectedSetBuffers;
    } __attribute__((packed));

    static_assert(sizeof(NxFpsSharedBlock) == 174, "Status Monitor expects the 174-byte NX-FPS block.");

    SharedMemory g_sharedMemory{};
    bool g_mapped = false;
    uint8_t* g_base = nullptr;
    size_t g_bytes = 0;

    // Written by the publisher thread, read on every present.
    std::atomic<NxFpsSharedBlock*> g_block{ nullptr };

    // Present thread only.
    uint64_t g_previousTick = 0;
    unsigned g_tickPosition = 0;
    uint64_t g_presentedFrames = 0;
    const NxFpsSharedBlock* g_lastSeenBlock = nullptr;

    // Present thread -> publisher thread.
    std::atomic<uint64_t> g_frameCounter{ 0 };
    std::atomic<uint32_t> g_width{ 0 };
    std::atomic<uint32_t> g_height{ 0 };

    // Publisher thread only: the last values published, used to seed a block that appears late
    // (overlay opened later, SaltyNX slow to hand out memory).
    uint8_t g_lastFps = 0;
    uint16_t g_lastWidth = 0;
    uint16_t g_lastHeight = 0;
    uint64_t g_lastFrames = 0;
    int g_attempts = 0;
    uint64_t g_firstAttemptTick = 0;
    bool g_reportedPublished = false;
    bool g_reportedConnection = false;
    bool g_reportedNoMemory = false;
    bool g_reportedNoRoom = false;
    bool g_reportedExisting = false;

    Thread g_thread;

    // SaltySD speaks plain CMIF, so serviceDispatch works on the raw session.
    Result AllocateShared(Service* service, uint64_t size, uint64_t* offset)
    {
        return serviceDispatchInOut(service, 6, size, *offset, .in_send_pid = true);
    }

    Result GetSharedHandle(Service* service, Handle* out)
    {
        return serviceDispatch(service, 7, .in_send_pid = true,
            .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = out);
    }

    Result CloseSession(Service* service)
    {
        const uint64_t zero = 0;
        return serviceDispatchIn(service, 0, zero, .in_send_pid = true);
    }

    void* FindMagicIn(uint8_t* base, size_t bytes, uint32_t magic)
    {
        if (base == nullptr)
            return nullptr;

        for (size_t offset = 0; offset + sizeof(uint32_t) <= bytes; offset += 4)
        {
            uint32_t value = 0;
            memcpy(&value, base + offset, sizeof(value));
            if (value == magic)
                return base + offset;
        }

        return nullptr;
    }

    void* FindMagic(uint32_t magic)
    {
        return FindMagicIn(g_base, g_bytes, magic);
    }

    bool IsValid(const NxFpsSharedBlock* block)
    {
        return block != nullptr && block->magic == MAGIC_FPS;
    }

    // Everything the overlay needs for its FPS and RES rows, right away.
    void Seed(NxFpsSharedBlock* block)
    {
        block->pluginActive = true;
        block->api = API_VULKAN;
        block->fps = g_lastFps;
        block->fpsAverage = float(g_lastFps);
        block->frameNumber = g_lastFrames;

        if (g_lastWidth != 0 && g_lastHeight != 0)
        {
            // Never 0xFFFF: that is the value the overlay writes to ask whether we know the resolution.
            const uint16_t calls = g_lastFps != 0 ? uint16_t(g_lastFps) : uint16_t(1);
            const ResolutionCalls resolution = { g_lastWidth, g_lastHeight, calls };
            block->renderCalls[0] = resolution;
            block->viewportCalls[0] = resolution;
        }

        // A fresh block has ticks[] at zero and the overlay would show "inf" until two presents
        // arrive. Index access only: the block is packed and ticks[] is misaligned.
        if (g_lastFps != 0 && block->ticks[0] == 0)
        {
            const uint32_t perFrame = uint32_t(armGetSystemTickFreq() / g_lastFps);
            for (unsigned i = 0; i < 10; i++)
                block->ticks[i] = perFrame;
        }
    }

    void ReportPublished(const NxFpsSharedBlock* block)
    {
        if (g_reportedPublished)
            return;

        g_reportedPublished = true;
        const double seconds = double(armTicksToNs(armGetSystemTick() - g_firstAttemptTick)) / 1e9;
        fprintf(stderr, "[overlay] FPS block published %.2f s after start (offset 0x%X)\n",
            seconds, unsigned(reinterpret_cast<const uint8_t*>(block) - g_base));
    }

    // Cheap: 1024 comparisons in a page that is already mapped, no IPC.
    bool Attach()
    {
        auto block = static_cast<NxFpsSharedBlock*>(FindMagic(MAGIC_FPS));
        if (block == nullptr)
            return false;

        g_block.store(block, std::memory_order_release);
        Seed(block);
        ReportPublished(block);
        return true;
    }

    // If SaltyNX was injected into this process, its shared page is already mapped here.
    bool FindOwnSharedMemory()
    {
        uint64_t address = 0;
        for (int regions = 0; regions < 4096; regions++)
        {
            MemoryInfo info{};
            u32 pageInfo = 0;
            if (R_FAILED(svcQueryMemory(&info, &pageInfo, address)) || info.size == 0)
                return false;

            if (info.type == MemType_SharedMem && (info.perm & Perm_R) != 0 && info.size >= SHARED_SIZE)
            {
                auto base = reinterpret_cast<uint8_t*>(uintptr_t(info.addr));
                const size_t bytes = info.size < 0x10000 ? size_t(info.size) : 0x10000;
                if (FindMagicIn(base, bytes, MAGIC_FPS) != nullptr)
                {
                    g_base = base;
                    g_bytes = bytes;
                    return true;
                }
            }

            const uint64_t next = info.addr + info.size;
            if (next <= address)
                return false;

            address = next;
        }

        return false;
    }

    Handle OpenResourceLimit()
    {
        u64 raw = 0;
        if (R_SUCCEEDED(svcGetInfo(&raw, InfoType_ResourceLimit, INVALID_HANDLE, 0)))
            return Handle(raw);
        if (R_SUCCEEDED(svcGetInfo(&raw, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0)))
            return Handle(raw);
        return INVALID_HANDLE;
    }

    bool HasRoomForSession(Handle limit)
    {
        s64 current = 0;
        s64 maximum = 0;
        if (R_FAILED(svcGetResourceLimitCurrentValue(&current, limit, LimitableResource_Sessions)) ||
            R_FAILED(svcGetResourceLimitLimitValue(&maximum, limit, LimitableResource_Sessions)))
        {
            return true; // Unknown: try anyway.
        }

        return current < maximum;
    }

    // Sets *smReleased when "sm:" had to be released; the caller restores it.
    bool MakeRoom(bool* smReleased, bool allowReleasingSm)
    {
        *smReleased = false;

        const Handle limit = OpenResourceLimit();
        if (limit == INVALID_HANDLE)
            return true;

        bool room = HasRoomForSession(limit);

        // Raising our own session limit needs no privilege, but calling a syscall the loader does
        // not allow kills the process, so check the hint first.
        if (!room && envIsSyscallHinted(0x7E))
        {
            s64 maximum = 0;
            svcGetResourceLimitLimitValue(&maximum, limit, LimitableResource_Sessions);
            if (R_SUCCEEDED(svcSetResourceLimitLimitValue(limit, LimitableResource_Sessions, u64(maximum + 4))))
            {
                room = HasRoomForSession(limit);
                fprintf(stderr, "[overlay] Raised the process session limit from %lld to %lld\n",
                    (long long)maximum, (long long)(maximum + 4));
            }
        }

        if (!room && allowReleasingSm)
        {
            smExit(); // Reference counted; if it really closes, there is room.
            if (HasRoomForSession(limit))
            {
                *smReleased = true;
                room = true;
            }
            else
            {
                smInitialize();
            }
        }

        svcCloseHandle(limit);
        return room;
    }

    void Initialize()
    {
        if (g_firstAttemptTick == 0)
            g_firstAttemptTick = armGetSystemTick();

        if (IsValid(g_block.load(std::memory_order_acquire)))
            return;

        g_block.store(nullptr, std::memory_order_release);

        if (g_mapped && Attach())
            return;

        // Releasing "sm:" is not done on the first attempts (startup is still opening services),
        // and later only once a minute: it is a short window without the name service.
        const bool releaseSm = g_attempts >= 3 && (g_attempts < 15 || (g_attempts % 60) == 0);
        bool smReleased = false;
        const bool room = MakeRoom(&smReleased, releaseSm);
        const int passes = smReleased ? 1 : (g_attempts == 0 ? 20 : 2);

        static const char* const PORTS[2] = { "SaltySD", "InjectServ" };

        uint64_t offset = 0;
        bool allocated = false;
        bool connected = false;
        Result portResult = 0;
        Result handleResult = 0;

        for (int pass = 0; pass < passes && !connected && room; pass++)
        {
            for (const char* name : PORTS)
            {
                Handle port = INVALID_HANDLE;
                portResult = svcConnectToNamedPort(&port, name);
                if (R_FAILED(portResult))
                    continue;

                Service service{};
                service.session = port;

                bool havePage = g_mapped;
                if (!havePage)
                {
                    Handle memory = INVALID_HANDLE;
                    handleResult = GetSharedHandle(&service, &memory);
                    if (R_SUCCEEDED(handleResult))
                    {
                        shmemLoadRemote(&g_sharedMemory, memory, SHARED_SIZE, Perm_Rw);
                        if (R_SUCCEEDED(shmemMap(&g_sharedMemory)))
                        {
                            g_base = static_cast<uint8_t*>(shmemGetAddr(&g_sharedMemory));
                            g_bytes = SHARED_SIZE;
                            g_mapped = true;
                            havePage = true;
                        }
                    }
                }

                if (havePage)
                {
                    connected = true;
                    if (FindMagic(MAGIC_FPS) == nullptr)
                        allocated = R_SUCCEEDED(AllocateShared(&service, sizeof(NxFpsSharedBlock), &offset));
                }

                // Close both ends: the port accepts a single session, and the overlay needs it too.
                // The shared memory handle stays valid without the session.
                CloseSession(&service);
                svcCloseHandle(port);

                if (connected)
                {
                    if (!g_reportedConnection)
                    {
                        g_reportedConnection = true;
                        fprintf(stderr, "[overlay] Connected to SaltyNX through %s\n", name);
                    }
                    break;
                }
            }

            if (!connected)
                svcSleepThread(10'000'000);
        }

        if (smReleased)
            smInitialize();

        if (!g_mapped)
        {
            if (R_FAILED(handleResult) && handleResult != 0 && !g_reportedNoMemory)
            {
                g_reportedNoMemory = true;
                fprintf(stderr, "[overlay] SaltyNX connected but did not share its memory (0x%X)\n", unsigned(handleResult));
            }

            if (!FindOwnSharedMemory())
            {
                // Retried for the whole session: the port's single session may be busy (0x10801).
                g_attempts++;
                if (g_attempts == 5)
                    fprintf(stderr, "[overlay] SaltyNX not reachable yet (last error 0x%X); retrying every second\n", unsigned(portResult));
                return;
            }

            g_mapped = true;
        }

        auto existing = static_cast<NxFpsSharedBlock*>(FindMagic(MAGIC_FPS));
        if (existing != nullptr)
        {
            g_block.store(existing, std::memory_order_release);
            if (!g_reportedExisting)
            {
                g_reportedExisting = true;
                fprintf(stderr, "[overlay] Reusing the FPS block already in SaltyNX's page\n");
            }
        }
        else if (allocated)
        {
            auto block = reinterpret_cast<NxFpsSharedBlock*>(g_base + offset);
            memset(block, 0, sizeof(*block));
            block->magic = MAGIC_FPS;
            g_block.store(block, std::memory_order_release);
        }
        else if (!g_reportedNoRoom)
        {
            g_reportedNoRoom = true;
            fprintf(stderr, "[overlay] No room for the FPS block yet; retrying every second\n");
        }

        if (NxFpsSharedBlock* block = g_block.load(std::memory_order_acquire))
        {
            Seed(block);
            ReportPublished(block);
            g_attempts = 0;
        }
        else
        {
            g_attempts++;
        }
    }

    void PublisherThread(void*)
    {
        uint64_t previousFrames = 0;
        uint64_t previousTick = armGetSystemTick();

        Initialize();

        while (true)
        {
            svcSleepThread(1'000'000'000);

            const uint64_t tick = armGetSystemTick();
            const uint64_t frames = g_frameCounter.load(std::memory_order_relaxed);
            const double seconds = double(armTicksToNs(tick - previousTick)) / 1e9;
            const double fps = seconds > 0.0 ? double(frames - previousFrames) / seconds : 0.0;
            previousFrames = frames;
            previousTick = tick;

            g_lastFps = uint8_t(fps < 0.0 ? 0.0 : (fps > 255.0 ? 255.0 : fps + 0.5));
            g_lastFrames = frames;

            const uint32_t width = g_width.load(std::memory_order_relaxed);
            const uint32_t height = g_height.load(std::memory_order_relaxed);
            if (width != 0 && height != 0)
            {
                g_lastWidth = uint16_t(width);
                g_lastHeight = uint16_t(height);
            }

            // Re-attach whenever the magic is gone: this is what lets the overlay be opened at any
            // time without restarting the game.
            NxFpsSharedBlock* block = g_block.load(std::memory_order_acquire);
            if (!IsValid(block))
            {
                Initialize();
                block = g_block.load(std::memory_order_acquire);
            }

            // Also refreshed here, not only on present: during loading screens the game may not
            // present for a while and the overlay would drop its rows.
            if (block != nullptr)
                Seed(block);
        }
    }
}

void os::switch_overlay::Start()
{
    // Default game priority (time-sliced); it wakes up once per second.
    if (R_SUCCEEDED(threadCreate(&g_thread, PublisherThread, nullptr, nullptr, 0x10000, 0x3B, -2)))
        threadStart(&g_thread);
}

void os::switch_overlay::OnPresent(uint32_t width, uint32_t height)
{
    g_frameCounter.fetch_add(1, std::memory_order_relaxed);
    g_width.store(width, std::memory_order_relaxed);
    g_height.store(height, std::memory_order_relaxed);

    NxFpsSharedBlock* block = g_block.load(std::memory_order_acquire);
    if (!IsValid(block))
    {
        // The publisher thread re-attaches within a second; no IPC or searching on this thread.
        if (block != nullptr)
            g_block.store(nullptr, std::memory_order_release);
        return;
    }

    // Heartbeat: the overlay clears this and expects it back within 100 ms.
    block->pluginActive = true;
    block->api = API_VULKAN;

    // A block attached just now: the previous present was long ago, do not report that interval.
    if (g_lastSeenBlock != block)
    {
        g_lastSeenBlock = block;
        g_previousTick = 0;
    }

    const uint64_t now = armGetSystemTick();
    if (g_previousTick != 0)
    {
        const uint64_t delta = now - g_previousTick;
        block->ticks[g_tickPosition] = uint32_t(delta > 0xFFFFFFFFull ? 0xFFFFFFFFull : delta);
        g_tickPosition = (g_tickPosition + 1) % 10;
        block->frameNumber = ++g_presentedFrames;
    }
    g_previousTick = now;

    if (width != 0 && height != 0)
    {
        // Answer the resolution handshake (the overlay writes 0xFFFF into calls).
        const uint16_t calls = block->fps != 0 ? block->fps : uint16_t(1);
        const ResolutionCalls resolution = { uint16_t(width), uint16_t(height), calls };
        block->renderCalls[0] = resolution;
        block->viewportCalls[0] = resolution;
    }
}
