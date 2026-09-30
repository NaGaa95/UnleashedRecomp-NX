#include "stdafx.h"
#include <kernel/function.h>
#include <kernel/xdm.h>
#if defined(__SWITCH__)
#include <os/switch_cpu_profiler.h>
#include <os/switch_lzx.h>
#endif

uint32_t QueryPerformanceCounterImpl(LARGE_INTEGER* lpPerformanceCount)
{
    lpPerformanceCount->QuadPart = ByteSwap(std::chrono::steady_clock::now().time_since_epoch().count());
    return TRUE;
}

uint32_t QueryPerformanceFrequencyImpl(LARGE_INTEGER* lpFrequency)
{
    constexpr auto Frequency = std::chrono::steady_clock::period::den / std::chrono::steady_clock::period::num;
    lpFrequency->QuadPart = ByteSwap(Frequency);
    return TRUE;
}

uint32_t GetTickCountImpl()
{
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void GlobalMemoryStatusImpl(XLPMEMORYSTATUS lpMemoryStatus)
{
    lpMemoryStatus->dwLength = sizeof(XMEMORYSTATUS);
    lpMemoryStatus->dwMemoryLoad = 0;
    lpMemoryStatus->dwTotalPhys = 0x20000000;
    lpMemoryStatus->dwAvailPhys = 0x20000000;
    lpMemoryStatus->dwTotalPageFile = 0x20000000;
    lpMemoryStatus->dwAvailPageFile = 0x20000000;
    lpMemoryStatus->dwTotalVirtual = 0x20000000;
    lpMemoryStatus->dwAvailVirtual = 0x20000000;
}

GUEST_FUNCTION_HOOK(sub_831B0ED0, memcpy);
GUEST_FUNCTION_HOOK(sub_831CCB98, memcpy);
GUEST_FUNCTION_HOOK(sub_831CEAE0, memcpy);
GUEST_FUNCTION_HOOK(sub_831CEE04, memcpy);
GUEST_FUNCTION_HOOK(sub_831CF2D0, memcpy);
GUEST_FUNCTION_HOOK(sub_831CF660, memcpy);
GUEST_FUNCTION_HOOK(sub_831B1358, memcpy);
GUEST_FUNCTION_HOOK(sub_831B5E00, memmove);
GUEST_FUNCTION_HOOK(sub_831B0BA0, memset);
GUEST_FUNCTION_HOOK(sub_831CCAA0, memset);

#ifdef _WIN32
GUEST_FUNCTION_HOOK(sub_82BD4CA8, OutputDebugStringA);
#else
GUEST_FUNCTION_STUB(sub_82BD4CA8);
#endif

GUEST_FUNCTION_HOOK(sub_82BD4AC8, QueryPerformanceCounterImpl);
GUEST_FUNCTION_HOOK(sub_831CD040, QueryPerformanceFrequencyImpl);
GUEST_FUNCTION_HOOK(sub_831CDAD0, GetTickCountImpl);

GUEST_FUNCTION_HOOK(sub_82BD4BC0, GlobalMemoryStatusImpl);

// sprintf
PPC_FUNC(sub_82BD4AE8)
{
    sub_831B1630(ctx, base);
}

#if defined(__SWITCH__)
// [Switch] SwitchNativeRtti, SwitchNativeShaderConstants (set in main() before any guest code runs). Small guest
// functions the CPU profile of the game's main thread found hot, as native code that leaves guest memory and
// the return value exactly as the recompiled code does (it does not repeat stores below the stack pointer,
// dead once the function returns). Off, the recompiled code runs.
bool g_nativeRtti = false;
bool g_nativeShaderConstants = false;

// type_info::operator== (MSVC CRT): the decorated names, from offset 9 of each type_info (after the '.'),
// compared byte by byte up to the end of the second; 1 when they are the same string.
// Round 8: the type_infos of the executable's image (all of the game's: MSVC places them with the vftables)
// never change, so the results for pairs of those are remembered: the message handlers compare the same few
// pairs over and over. The same object is the same name.
// Round 9: one table for every thread, without the thread-local lookup (a call per access with -mtp=soft) and
// four times larger. Each entry is one 64-bit word, written and read whole: the pair (the second type_info's
// address, 4-byte aligned, carries the result in bit 0), so a reader sees a whole entry or none.
static std::atomic<uint64_t> g_typeInfoComparisons[1024];

PPC_FUNC_IMPL(__imp__sub_831B0AB8);
PPC_FUNC(sub_831B0AB8)
{
    if (!g_nativeRtti)
    {
        __imp__sub_831B0AB8(ctx, base);
        return;
    }

    const uint32_t a = ctx.r3.u32;
    const uint32_t b = ctx.r4.u32;
    if (a == b)
    {
        ctx.r3.u64 = 1;
        return;
    }

    auto inImage = [](uint32_t address)
        {
            return address >= PPC_IMAGE_BASE && address - PPC_IMAGE_BASE < PPC_IMAGE_SIZE - 256;
        };

    const bool cacheable = inImage(a) && inImage(b) && ((a | b) & 3) == 0;
    const uint64_t key = (uint64_t(a) << 32) | b;
    std::atomic<uint64_t>& slot = g_typeInfoComparisons[((a * 0x9E3779B1u) ^ (b * 0x85EBCA77u)) >> 22];
    if (cacheable)
    {
        const uint64_t entry = slot.load(std::memory_order_relaxed);
        if ((entry & ~uint64_t(1)) == key)
        {
            ctx.r3.u64 = entry & 1;
            return;
        }
    }

    const char* first = reinterpret_cast<const char*>(base + uint32_t(a + 9));
    const char* second = reinterpret_cast<const char*>(base + uint32_t(b + 9));
    const uint32_t equal = strcmp(first, second) == 0 ? 1 : 0;
    if (cacheable)
        slot.store(key | equal, std::memory_order_relaxed);
    ctx.r3.u64 = equal;
}

// __RTtypeid (MSVC CRT): the type descriptor of the complete object r3 points to, from its vftable's complete
// object locator (offset 12). A null object or a locator without one throws: the recompiled code does that.
PPC_FUNC_IMPL(__imp__sub_831B2438);
PPC_FUNC(sub_831B2438)
{
    if (g_nativeRtti && ctx.r3.u32 != 0)
    {
        const uint32_t locator = PPC_LOAD_U32(PPC_LOAD_U32(ctx.r3.u32) - 4);
        const uint32_t typeDescriptor = PPC_LOAD_U32(locator + 12);
        if (typeDescriptor != 0)
        {
            ctx.r3.u64 = typeDescriptor;
            return;
        }
    }

    __imp__sub_831B2438(ctx, base);
}

// The D3D device's shader constant setters: copy r6 float4 registers from r5 (any alignment) into the stage's
// array at register r4 (16-byte stores at the aligned address, as stvx), then OR r7 into the stage's 64-bit
// dirty word. Overlapping ranges, which the recompiled code copies 64 bytes at a time, go to it.
static bool UploadShaderConstants(PPCContext& ctx, uint8_t* base, uint32_t firstRegister, uint32_t dirtyOffset)
{
    const uint32_t destination = (ctx.r3.u32 + ((ctx.r4.u32 + firstRegister) << 4)) & ~0xFu;
    const uint32_t source = ctx.r5.u32;
    const uint64_t size = uint64_t(ctx.r6.u32) * 16;
    if (uint64_t(source) < uint64_t(destination) + size && uint64_t(destination) < uint64_t(source) + size)
        return false;

    memcpy(base + destination, base + source, size);

    const uint32_t dirty = ctx.r3.u32 + dirtyOffset;
    PPC_STORE_U64(dirty, PPC_LOAD_U64(dirty) | ctx.r7.u64);
    return true;
}

// Vertex shader constants: registers from 120 (1920 bytes into the device), dirty word at 0.
PPC_FUNC_IMPL(__imp__sub_82BDFAA0);
PPC_FUNC(sub_82BDFAA0)
{
    if (!g_nativeShaderConstants || !UploadShaderConstants(ctx, base, 120, 0))
        __imp__sub_82BDFAA0(ctx, base);
}

// Pixel shader constants: registers from 376, dirty word at 8.
PPC_FUNC_IMPL(__imp__sub_82BDFB80);
PPC_FUNC(sub_82BDFB80)
{
    if (!g_nativeShaderConstants || !UploadShaderConstants(ctx, base, 376, 8))
        __imp__sub_82BDFB80(ctx, base);
}

// [Switch] SwitchNativeDecompress, SwitchVerifyNativeDecompress (set in main()). XMemDecompress(context,
// destination, pDestinationSize, source, sourceSize), the XDK's LZX decoder the game's loader threads spend
// most of their time in while a stage loads or streams: native code (os/switch_lzx.h) for LZX contexts
// without the streaming flag (magic, codec 1 and flags at 0, 4 and 8, window size at 24), which the guest
// resets on every call. The guest's result for such a call is the decoded frames at the destination, their
// total size at pDestinationSize and S_OK; the native decoder writes the same or gives up, and the guest
// decoder then runs instead. Verifying, the guest decoder always runs and each native result is compared
// with it ("[lzx]" lines in stderr.log).
bool g_nativeDecompress = false;
bool g_verifyNativeDecompress = false;

namespace
{
    std::atomic<uint32_t> g_decompressCalls;
    std::atomic<uint32_t> g_decompressNative;
    std::atomic<uint32_t> g_decompressMismatches;
    std::atomic<uint64_t> g_decompressBytes;
    std::atomic<uint64_t> g_decompressNanoseconds;
}

PPC_FUNC_IMPL(__imp__sub_831CE0D0);
PPC_FUNC(sub_831CE0D0)
{
    const uint32_t context = ctx.r3.u32;
    const uint32_t destination = ctx.r4.u32;
    const uint32_t destinationSize = ctx.r5.u32;
    const uint32_t source = ctx.r6.u32;
    const uint32_t sourceSize = ctx.r7.u32;
    const auto start = std::chrono::steady_clock::now();

    // Decoded straight into the destination, where the guest writes; when the native decoder gives up, the
    // guest's decoder writes it again. Verifying, the hash of the native output is compared with the guest's
    // output decoded over it.
    bool native = false;
    uint32_t nativeSize = 0;
    if ((g_nativeDecompress || g_verifyNativeDecompress) && context != 0 && PPC_LOAD_U32(context) == 0x76C3F251 &&
        PPC_LOAD_U32(context + 4) == 1 && (PPC_LOAD_U32(context + 8) & 1) == 0)
    {
        native = os::switch_lzx::Decompress(base + source, sourceSize, PPC_LOAD_U32(context + 24), base + destination, nativeSize);
    }

    uint32_t decoded;
    if (native && !g_verifyNativeDecompress)
    {
        PPC_STORE_U32(destinationSize, nativeSize);
        ctx.r3.u64 = 0; // S_OK
        decoded = nativeSize;
    }
    else
    {
        const uint64_t nativeHash = native ? XXH3_64bits(base + destination, nativeSize) : 0;
        __imp__sub_831CE0D0(ctx, base);
        decoded = PPC_LOAD_U32(destinationSize);

        if (native && (decoded != nativeSize || ctx.r3.u32 != 0 || XXH3_64bits(base + destination, decoded) != nativeHash))
        {
            const uint32_t mismatches = ++g_decompressMismatches;
            if (mismatches <= 20)
            {
                char line[256];
                const int length = snprintf(line, sizeof(line), "[lzx] MISMATCH #%u: %u compressed bytes, game %u bytes (result 0x%08X), "
                    "native %u bytes\n", mismatches, sourceSize, decoded, ctx.r3.u32, nativeSize);
                os::switch_cpu_profiler::WriteLog(line, size_t(std::max(length, 0)));
            }
        }
    }

    const uint64_t nanoseconds = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    const uint32_t calls = ++g_decompressCalls;
    const uint32_t nativeCalls = native ? ++g_decompressNative : g_decompressNative.load();
    const uint64_t bytes = (g_decompressBytes += decoded);
    const uint64_t totalNanoseconds = (g_decompressNanoseconds += nanoseconds);

    // A line now and then, from the loader thread (the writes are rare, and WriteLog is one write).
    if ((calls % 64) == 0)
    {
        char line[256];
        const int length = snprintf(line, sizeof(line), "[lzx] %u XMemDecompress calls, %u decoded natively%s, %.1f MB out in %.0f ms "
            "(%.1f MB/s)%s\n", calls, nativeCalls, g_verifyNativeDecompress ? " and compared with the game's decoder" : "",
            double(bytes) / 1e6, double(totalNanoseconds) / 1e6, totalNanoseconds != 0 ? double(bytes) / 1e6 / (double(totalNanoseconds) / 1e9) : 0.0,
            g_verifyNativeDecompress ? (g_decompressMismatches.load() == 0 ? ", all identical" : ", MISMATCHES") : "");
        os::switch_cpu_profiler::WriteLog(line, size_t(std::max(length, 0)));
    }
}
#endif
