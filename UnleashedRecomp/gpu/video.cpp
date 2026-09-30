#include "video.h"

#include "imgui/imgui_common.h"
#include "imgui/imgui_snapshot.h"
#include "imgui/imgui_font_builder.h"

#include <app.h>
#include <bc_diff.h>
#include <cpu/guest_thread.h>
#include <decompressor.h>
#include <kernel/function.h>
#include <kernel/heap.h>
#include <hid/hid.h>
#include <kernel/memory.h>
#include <kernel/xdbf.h>
#include <res/bc_diff/button_bc_diff.bin.h>
#include <res/font/im_font_atlas.dds.h>
#include <shader/shader_cache.h>
#include <SWA.h>
#include <ui/achievement_menu.h>
#include <ui/achievement_overlay.h>
#include <ui/button_guide.h>
#include <ui/fader.h>
#include <ui/imgui_utils.h>
#include <ui/installer_wizard.h>
#include <ui/message_window.h>
#include <ui/options_menu.h>
#include <ui/game_window.h>
#include <ui/black_bar.h>
#include <patches/aspect_ratio_patches.h>
#include <user/config.h>
#include <user/paths.h>
#include <sdl_listener.h>
#include <xxHashMap.h>
#include <os/process.h>
#include <version.h>

#if defined(ASYNC_PSO_DEBUG) || defined(PSO_CACHING)
#include <magic_enum/magic_enum.hpp>
#endif

#if defined(__SWITCH__)
#include <condition_variable>
#include <cstdarg>
#include <os/switch_overlay.h>
#include <os/switch_cpu_profiler.h>
#include <os/switch_stall_watch.h>
#include <switch_build_id.h>

// Driver-side fast paths of nfsmw-nx's Mesa patch (mesa/mesa-switch-nfsmw.patch), requested through
// weak symbols the driver defines. With any other driver these resolve to null and nothing happens.
// The field order and types are a versioned contract (version 1).
struct NvkSwitchSet4Header
{
    int32_t version;     // 1
    int32_t requested;   // app: 1 yes, 0 no, -1 unspecified (= yes)
    int32_t environment; // NVK_SWITCH_DYN_UBO_DELTA: -1 unread, 0 off, 1 on
    int32_t disabled;    // 1: the driver's self-check saw a difference and turned it off
};

struct NvkSwitchDrawPart
{
    uint64_t count;
    uint64_t ticks;
};

struct NvkSwitchDrawImprovement
{
    int32_t requested;   // app: 1 yes, 0 no, -1 unspecified (= yes)
    int32_t disabled;    // 1: its self-check saw a difference
    uint64_t uses;
    uint64_t validated;
    uint64_t differences;
    uint64_t unchecked;
};

struct NvkSwitchDraw
{
    int32_t version;     // 1
    int32_t measure;
    int32_t measureMisses;
    int32_t environment;
    uint64_t ticksPerSecond;
    NvkSwitchDrawPart parts[16];
    uint64_t counts[13];
    NvkSwitchDrawImprovement improvements[5]; // emission, cbufs, dynamic state, fast set 4, prefetch
};

static_assert(sizeof(NvkSwitchDraw) == 24 + 16 * 16 + 13 * 8 + 5 * 40, "Must match struct nvk_switch_dibujo.");

extern "C"
{
    extern NvkSwitchSet4Header nvk_switch_set4 __attribute__((weak));
    extern NvkSwitchDraw nvk_switch_dibujo __attribute__((weak));
    // Mesa's BLAKE3 (util/mesa-blake3.h), linked in with the driver: the GPU draw profiler names
    // SPIR-V with it the way the driver's NVK_SHADER_STATS lines do.
    void _mesa_blake3_compute(const void* data, size_t size, unsigned char result[32]) __attribute__((weak));
}

// Minimal libnx declarations (avoids pulling <switch.h> macros into this TU).
extern "C"
{
    void svcSleepThread(int64_t nano);
    uint32_t svcSetThreadPriority(uint32_t handle, uint32_t priority);
    uint32_t svcSetThreadCoreMask(uint32_t handle, int32_t preferredCore, uint32_t affinityMask);
    uint32_t svcGetInfo(uint64_t* out, uint32_t id0, uint32_t handle, uint64_t id1);
    uint32_t threadGetCurHandle(void);
}

// [Switch] SwitchHostThreadCores: the port's busy threads start on a core of their own (the render thread on
// core 1, pipeline compilation on core 2) instead of the process's default core 0, where the game's main
// thread runs; they may still run on every core of the process. Set once the configuration is loaded
// (CreateHostDevice): the render and pipeline task threads start during static initialisation.
static std::atomic<bool> g_hostThreadCoresReady{ false };

static void SetHostThreadCore(int32_t core)
{
    if (!Config::SwitchHostThreadCores)
        return;

    uint64_t processCores = 0;
    if (svcGetInfo(&processCores, 0 /* InfoType_CoreMask */, 0xFFFF8001 /* CUR_PROCESS_HANDLE */, 0) != 0 || processCores == 0)
        processCores = 0x7;

    if ((processCores & (uint64_t(1) << core)) != 0)
        svcSetThreadCoreMask(threadGetCurHandle(), core, uint32_t(processCores));
}
#endif

#define UNLEASHED_RECOMP
#include "../../tools/XenosRecomp/XenosRecomp/shader_common.h"

#ifdef UNLEASHED_RECOMP_D3D12
#include "shader/blend_color_alpha_ps.hlsl.dxil.h"
#include "shader/copy_vs.hlsl.dxil.h"
#include "shader/copy_color_ps.hlsl.dxil.h"
#include "shader/copy_depth_ps.hlsl.dxil.h"
#include "shader/csd_filter_ps.hlsl.dxil.h"
#include "shader/csd_no_tex_vs.hlsl.dxil.h"
#include "shader/csd_vs.hlsl.dxil.h"
#include "shader/enhanced_motion_blur_ps.hlsl.dxil.h"
#include "shader/gamma_correction_ps.hlsl.dxil.h"
#include "shader/gaussian_blur_3x3.hlsl.dxil.h"
#include "shader/gaussian_blur_5x5.hlsl.dxil.h"
#include "shader/gaussian_blur_7x7.hlsl.dxil.h"
#include "shader/gaussian_blur_9x9.hlsl.dxil.h"
#include "shader/imgui_ps.hlsl.dxil.h"
#include "shader/imgui_vs.hlsl.dxil.h"
#include "shader/movie_ps.hlsl.dxil.h"
#include "shader/movie_vs.hlsl.dxil.h"
#include "shader/resolve_msaa_color_2x.hlsl.dxil.h"
#include "shader/resolve_msaa_color_4x.hlsl.dxil.h"
#include "shader/resolve_msaa_color_8x.hlsl.dxil.h"
#include "shader/resolve_msaa_depth_2x.hlsl.dxil.h"
#include "shader/resolve_msaa_depth_4x.hlsl.dxil.h"
#include "shader/resolve_msaa_depth_8x.hlsl.dxil.h"
#endif

#include "shader/blend_color_alpha_ps.hlsl.spirv.h"
#include "shader/copy_vs.hlsl.spirv.h"
#include "shader/copy_color_ps.hlsl.spirv.h"
#include "shader/copy_depth_ps.hlsl.spirv.h"
#include "shader/csd_filter_ps.hlsl.spirv.h"
#include "shader/csd_no_tex_vs.hlsl.spirv.h"
#include "shader/csd_vs.hlsl.spirv.h"
#include "shader/enhanced_motion_blur_ps.hlsl.spirv.h"
#include "shader/gamma_correction_ps.hlsl.spirv.h"
#include "shader/gaussian_blur_3x3.hlsl.spirv.h"
#include "shader/gaussian_blur_5x5.hlsl.spirv.h"
#include "shader/gaussian_blur_7x7.hlsl.spirv.h"
#include "shader/gaussian_blur_9x9.hlsl.spirv.h"
#include "shader/imgui_ps.hlsl.spirv.h"
#include "shader/imgui_vs.hlsl.spirv.h"
#include "shader/movie_ps.hlsl.spirv.h"
#include "shader/movie_vs.hlsl.spirv.h"
#include "shader/resolve_msaa_color_2x.hlsl.spirv.h"
#include "shader/resolve_msaa_color_4x.hlsl.spirv.h"
#include "shader/resolve_msaa_color_8x.hlsl.spirv.h"
#include "shader/resolve_msaa_depth_2x.hlsl.spirv.h"
#include "shader/resolve_msaa_depth_4x.hlsl.spirv.h"
#include "shader/resolve_msaa_depth_8x.hlsl.spirv.h"

#ifdef _WIN32
extern "C"
{
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace plume
{
#ifdef UNLEASHED_RECOMP_D3D12
    extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
#ifdef SDL_VULKAN_ENABLED
    extern std::unique_ptr<RenderInterface> CreateVulkanInterface(RenderWindow sdlWindow);
#else
    extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#endif

    static std::unique_ptr<RenderInterface> CreateVulkanInterfaceWrapper() {
#if defined(__SWITCH__)
        const auto userPath = GetUserPath();
        SwitchFrameGenerationConfig frameGeneration;
        frameGeneration.enabled = Config::FrameGeneration;
        frameGeneration.shaderPath = (userPath / "lsfg" / "Lossless.dll").string();
        frameGeneration.pipelineCachePath = (userPath / "cache" / "lsfg-vk-pipeline-cache.bin").string();
        frameGeneration.flowScale = Config::FrameGenerationFlowScale;
        frameGeneration.performanceMode = Config::FrameGenerationPerformanceMode;
        ConfigureSwitchFrameGeneration(frameGeneration);

        if (Config::SwitchPipelineCache)
        {
            // Persistent VkPipelineCache (plume-switch-perf.patch). The tag invalidates the file
            // whenever the NRO changes: the Mesa driver is linked into it and on Horizon its
            // pipeline cache UUID does not necessarily change between driver builds.
            std::error_code ec;
            const auto cacheDirectory = userPath / "cache";
            std::filesystem::create_directories(cacheDirectory, ec);

            const auto cachePath = cacheDirectory / "pipelines.bin";
            std::string buildTag;
            if (UNLEASHED_RECOMP_SWITCH_DRIVER_ID[0] != '\0')
            {
                // Round 9: keyed by the driver linked in (the SHA-256 of its library), so the cache survives
                // rebuilds of the rest of the NRO; the driver keys its own entries by everything a pipeline is made
                // of, so entries of changed shaders or states are simply not found.
                buildTag = fmt::format("driver|{}", UNLEASHED_RECOMP_SWITCH_DRIVER_ID);

                // The round-8 test builds wrote theirs (the pipelines.bin handed back from that round) under a key
                // made from their NRO, with this same driver.
                if (strcmp(UNLEASHED_RECOMP_SWITCH_DRIVER_ID, "98c8b89562db4b10de1f345245b1386e602b32ff8fda8b833e46a673846212cf") == 0)
                    ConfigureVulkanPipelineCacheLegacyKey(0x4901F4390434165AULL);
            }
            else
            {
                buildTag = fmt::format("{}|{}|{}", g_versionString, g_commitHash, __DATE__ " " __TIME__);
                const auto nroSize = std::filesystem::file_size(os::process::GetExecutablePath(), ec);
                if (!ec)
                    buildTag += fmt::format("|{}", nroSize);
            }

            // Mesa's disk cache (sdmc:/.mesa) only duplicates this one and writes every new shader to
            // the SD card again from its own threads. Its key is the driver's build id, i.e. the Mesa
            // commit, which a rebuilt driver with local changes keeps: it could hand back shaders
            // compiled by an older driver. Must be set before the instance is created.
            setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);

            ConfigureVulkanPipelineCache(cachePath.string(), buildTag);
        }

        SetVulkanPreciseBarriers(Config::SwitchPreciseBarriers);

        // Driver switches that have their own [Switch] option. SwitchMesaEnvironment, applied next,
        // overrides any of them.
        if (Config::SwitchZcullGreater)
            setenv("NVK_ZCULL", "greater", 1);
        setenv("NVK_LINK_VARYINGS", Config::SwitchLinkVaryings ? "1" : "0", 1);

        // [Switch] SwitchMesaEnvironment = "NAME=value;NAME=value": driver switches such as the
        // nfsmw-nx ones (NVK_SWITCH_DIBUJO, NVK_SWITCH_DYN_UBO_DELTA, NVK_COPY_ENGINE,
        // NVK_SUBTILING_KNOB...), read by Mesa when the instance and device are created.
        {
            const std::string environment = Config::SwitchMesaEnvironment;
            size_t start = 0;
            while (start < environment.size())
            {
                size_t end = environment.find(';', start);
                if (end == std::string::npos)
                    end = environment.size();

                const std::string entry = environment.substr(start, end - start);
                const size_t equals = entry.find('=');
                if (equals != std::string::npos && equals > 0)
                {
                    const std::string name = entry.substr(0, equals);
                    const std::string value = entry.substr(equals + 1);
                    setenv(name.c_str(), value.c_str(), 1);
                    fprintf(stderr, "Mesa environment: %s=%s\n", name.c_str(), value.c_str());
                }

                start = end + 1;
            }
        }
#endif
#ifdef SDL_VULKAN_ENABLED
        return CreateVulkanInterface(GameWindow::s_renderWindow);
#else
        return CreateVulkanInterface();
#endif
    }
}

#if defined(__SWITCH__) && defined(SPEC_CONSTANT_CONSTANTS_UBO)
// Shader constants read from dynamic uniform buffers (set 4) instead of through 64-bit
// pointers. NVK promotes dynamic UBOs to hardware constant banks, also on Maxwell; the pointer
// path compiles to global memory loads. Output is bit-identical: the same upload-buffer bytes are
// read either way. Needs patches/XenosRecomp-switch-perf.patch and a regenerated shader cache;
// with an old shader cache the bit is simply ignored by the shaders. See docs/SWITCH-PERFORMANCE.md.
#define UNLEASHED_RECOMP_CONSTANTS_UBO
static constexpr uint32_t CONSTANTS_UBO_SET_INDEX = 4;
static bool g_constantsUbo = false;        // Decided once in CreateHostDevice, before the pipeline layout.
static uint32_t g_constantsUboSpecBit = 0; // SPEC_CONSTANT_CONSTANTS_UBO when enabled.
static RenderDescriptorSetBuilder g_constantsUboSetBuilder;
#endif

#if defined(__SWITCH__) && defined(SPEC_CONSTANT_TEXTURE_SIZE)
// SPEC_CONSTANT_TEXTURE_SIZE and/or SPEC_CONSTANT_TEXTURE_SIZE_VERIFY, ORed into every pipeline.
static uint32_t g_textureSizeSpecBits = 0;
#endif

#if defined(__SWITCH__) && defined(SPEC_CONSTANT_SHADOW_GATHER)
// Translator features of XenosRecomp-switch-perf.patch that the renderer switches per pipeline. All
// are decided once in CreateHostDevice. See docs/SWITCH-PERFORMANCE.md.
#define UNLEASHED_RECOMP_SHADER_SPECIALIZATION
// SPEC_CONSTANT_SHADOW_GATHER and/or SPEC_CONSTANT_SHADOW_GATHER_VERIFY, ORed into every pipeline.
static uint32_t g_shadowGatherSpecBits = 0;
// SPEC_CONSTANT_RELATIVE_FROM_MEMORY for the vertex shaders that have a0-indexed constant arrays.
static bool g_relativeFromMemory = false;
// Pipelines of vertex shaders that branch on mrgHasBone get a variant for the value of the draw.
static bool g_bonesSpecialization = false;
// Unused oC0 components (SPEC_CONSTANT_UNUSED_COLOR_SHIFT) and vertex outputs (constants 1-3).
static bool g_trimPixelOutputs = false;
static bool g_trimVertexOutputs = false;
// SwitchPrecompileBonesVariants: loading screens also build the mrgHasBone variant a model will use.
static bool g_precompileBonesVariants = false;
#endif

#if defined(__SWITCH__)
// [Switch] SwitchStreamingBuffers, decided once in CreateHostDevice (see StreamBuffer).
static bool g_streamingBuffers = false;
// [Switch] SwitchLazyResolves, SwitchResolveHandOver, SwitchKeepResolvesPending (see TryResolveHandOver)
// and SwitchSkipUnusedPixelConstants, decided once in CreateHostDevice.
static bool g_lazyResolves = false;
static bool g_resolveHandOver = false;
static bool g_keepResolvesPending = false;
static bool g_skipUnusedPixelConstants = false;
// Between ProcBeginCommandList and the end of ProcExecuteCommandList (render thread).
static bool g_commandListOpen = false;
// [Switch] SwitchCoverageHandOver (TryCoverageHandOver), SwitchTrimConstantUploads (ConstantBytesToUpload)
// and SwitchAlphaTestEarlyOut (the SPEC_CONSTANT_ALPHA_TEST_EARLY_OUT bit ORed into every pipeline).
static bool g_coverageHandOver = false;
static bool g_trimConstantUploads = false;
static uint32_t g_alphaTestEarlyOutBit = 0;
// Bytes of each constant block uploaded last in the frame (ConstantBytesToUpload).
static uint32_t g_vertexConstantBytesUploaded = 0;
static uint32_t g_pixelConstantBytesUploaded = 0;
// [Switch] SwitchSparseConstantCopies (EnqueueShaderConstants), SwitchShadowGatherSpecialization
// (SpecializationBits) and SwitchEagerSampleTransitions (FlushRenderStateForRenderThread).
static bool g_sparseConstantCopies = false;
static bool g_shadowGatherSpecialization = false;
static bool g_eagerSampleTransitions = false;
// [Switch] SwitchBatchSeveralDraws (LocalRenderCommandQueue::submit, game thread).
static bool g_batchSeveralDraws = false;
// The colour surface of the previous draw (render thread; reset every frame).
static struct GuestSurface* g_lastColorSurface = nullptr;
// [Switch] Round 7, decided once in CreateHostDevice (docs/SWITCH-PERFORMANCE.md):
//  SwitchSingleCopyTriangle: the port's full-screen passes (resolve copies, coverage fix-ups, the gamma
//    pass) draw the one triangle copy_vs builds, instead of that triangle and a second one over half of it.
//  SwitchExactCoverage: a hand-over at a draw that provably covers the whole target (IsExactFullScreenDraw)
//    needs neither the coverage stencil marks nor the fix-up.
//  SwitchSkipOverwrittenClears: a colour clear such a draw overwrites entirely is not made (DeferredClear).
//  SwitchSkipNoOpDraws: a draw that writes no colour channel and no depth is not sent.
//  SwitchAlphaTestSink, SwitchSkipTransparentPixels: SPEC_CONSTANT_ALPHA_TEST_SINK and _BLEND_SKIP_*.
//  SwitchSkipRedundantRenderStates, SwitchSkipRedundantSamplerStates: the game thread does not send a render
//    or sampler state again when the render thread already has that value (StateFilter).
//  SwitchPipelineLookupCache, SwitchSamplerCache: render thread lookups kept for states seen before.
static bool g_singleCopyTriangle = false;
static bool g_exactCoverage = false;
static bool g_skipOverwrittenClears = false;
static bool g_skipNoOpDraws = false;
static bool g_skipRestoreDraws = false;
static uint32_t g_alphaTestSinkBit = 0;
static uint32_t g_alphaTestQuadSinkBit = 0;
static bool g_skipTransparentPixels = false;
static bool g_skipRedundantRenderStates = false;
static bool g_skipRedundantSamplerStates = false;
static bool g_pipelineLookupCache = false;
static bool g_samplerCache = false;
// [Switch] Round 9 GPU (docs/SWITCH-PERFORMANCE.md):
//  SwitchCarryClears: a deferred colour clear (DeferredClear) also waits through target changes, depth clears and
//    draws into other targets, which do not touch its surface, so it is made in the pass that next draws into it.
//  SwitchSkipOverwrittenDepthClears: a depth clear waits the same way (DeferredDepthClear); when the next draw
//    into that depth buffer writes every one of its pixels regardless of the old values, the clear is not made.
//  SwitchEagerDepthTransitions: SwitchEagerSampleTransitions for depth buffers left with pending resolves.
static bool g_carryClears = false;
static bool g_deferDepthClears = false;
static bool g_eagerDepthTransitions = false;
// The depth buffer of the previous draw (render thread; reset every frame).
static struct GuestSurface* g_lastDepthSurface = nullptr;
#endif

#pragma pack(push, 1)
struct PipelineState
{
    GuestShader* vertexShader = nullptr;
    GuestShader* pixelShader = nullptr;
    GuestVertexDeclaration* vertexDeclaration = nullptr;
    bool instancing = false;
    bool zEnable = true;
    bool zWriteEnable = true;
    RenderBlend srcBlend = RenderBlend::ONE;
    RenderBlend destBlend = RenderBlend::ZERO;
    RenderCullMode cullMode = RenderCullMode::NONE;
    RenderComparisonFunction zFunc = RenderComparisonFunction::LESS;
    bool alphaBlendEnable = false;
    RenderBlendOperation blendOp = RenderBlendOperation::ADD;
    float slopeScaledDepthBias = 0.0f;
    int32_t depthBias = 0;
    RenderBlend srcBlendAlpha = RenderBlend::ONE;
    RenderBlend destBlendAlpha = RenderBlend::ZERO;
    RenderBlendOperation blendOpAlpha = RenderBlendOperation::ADD;
    uint32_t colorWriteEnable = uint32_t(RenderColorWriteEnable::ALL);
    RenderPrimitiveTopology primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    uint8_t vertexStrides[16]{};
    RenderFormat renderTargetFormat{};
    RenderFormat depthStencilFormat{};
    RenderSampleCounts sampleCount = RenderSampleCount::COUNT_1;
    bool enableAlphaToCoverage = false;
    uint32_t specConstants = 0;
    // [Switch] SwitchCoverageHandOver: the variant that also writes 1 to a stencil buffer of its own.
    uint8_t coverageStencil = 0;
};
#pragma pack(pop)

struct UploadAllocation
{
    const RenderBuffer* buffer;
    uint64_t offset;
    uint8_t* memory;
    uint64_t deviceAddress;
    RenderDescriptorSet* constantsSet = nullptr; // Set 4 addressing the upload buffer of this allocation.
};

struct SharedConstants
{
    uint32_t texture2DIndices[16]{};
    uint32_t texture3DIndices[16]{};
    uint32_t textureCubeIndices[16]{};
    uint32_t samplerIndices[16]{};
    uint32_t booleans{};
    uint32_t swappedTexcoords{};
    float halfPixelOffsetX{};
    float halfPixelOffsetY{};
    float alphaThreshold{};
    // Bit s: the texture and sampler of 2D slot s make a gather return exactly the texels of point
    // fetches (XenosRecomp-switch-perf.patch, SPEC_CONSTANT_SHADOW_GATHER).
    uint32_t gatherableSlots{};
    uint32_t padding[2]{}; // 288 bytes (18 float4), the original fields padded to a float4 boundary.
    float textureSizes[16][2]{}; // Size of the 2D texture bound to each slot (XenosRecomp-switch-perf.patch).
};

static_assert(sizeof(SharedConstants) == 416, "shader_common.h declares the shared block as float4 v[26].");
static_assert(offsetof(SharedConstants, gatherableSlots) == 276, "XenosRecomp reads g_GatherableSlots at byte 276.");
static_assert(offsetof(SharedConstants, textureSizes) == 288, "XenosRecomp reads the texture sizes at byte 288 + slot * 8.");

// Depth bias values here are only used when the render device has 
// dynamic depth bias capability enabled. Otherwise, they get unused
// and the values get assigned in the pipeline state instead.

static GuestSurface* g_renderTarget;
static GuestSurface* g_depthStencil;
static RenderFramebuffer* g_framebuffer;
static RenderViewport g_viewport(0.0f, 0.0f, 1280.0f, 720.0f);
static PipelineState g_pipelineState;
static int32_t g_depthBias;
static float g_slopeScaledDepthBias;
// Kept in host byte order (swapped when set, only the range that changed), so each upload of the
// 4 KB / 3.5 KB block is a plain copy instead of a byte swap of the whole block. Same bytes uploaded.
static uint32_t g_vertexShaderConstants[0x400];
static uint32_t g_pixelShaderConstants[0x380];
static SharedConstants g_sharedConstants;
static GuestTexture* g_textures[16];
static RenderSamplerDesc g_samplerDescs[16];
static bool g_scissorTestEnable = false;
static RenderRect g_scissorRect;
static RenderVertexBufferView g_vertexBufferViews[16];
static RenderInputSlot g_inputSlots[16];
static RenderIndexBufferView g_indexBufferView({}, 0, RenderFormat::R16_UINT);

struct DirtyStates
{
    bool renderTargetAndDepthStencil;
    bool viewport;
    bool pipelineState;
    bool depthBias;
    bool sharedConstants;
    bool scissorRect;
    bool vertexShaderConstants;
    uint8_t vertexStreamFirst;
    uint8_t vertexStreamLast;
    bool indices;
    bool pixelShaderConstants;

    DirtyStates(bool value)
        : renderTargetAndDepthStencil(value)
        , viewport(value)
        , pipelineState(value)
        , depthBias(value)
        , sharedConstants(value)
        , scissorRect(value)
        , vertexShaderConstants(value)
        , vertexStreamFirst(value ? 0 : 255)
        , vertexStreamLast(value ? 15 : 0)
        , indices(value)
        , pixelShaderConstants(value)
    {
    }
};

static DirtyStates g_dirtyStates(true);

template<typename T>
static void SetDirtyValue(bool& dirtyState, T& dest, const T& src)
{
    if (dest != src)
    {
        dest = src;
        dirtyState = true;
    }
}

static constexpr size_t PROFILER_VALUE_COUNT = 256;
static size_t g_profilerValueIndex;

struct Profiler
{
    std::atomic<double> value;
    double values[PROFILER_VALUE_COUNT];
    std::chrono::steady_clock::time_point start;

    void Begin()
    {
        start = std::chrono::steady_clock::now();
    }

    void End()
    {
        value = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void Set(double v)
    {
        value = v;
    }

    void Reset()
    {
        End();
        Begin();
    }

    double UpdateAndReturnAverage()
    {
        values[g_profilerValueIndex] = value;
        return std::accumulate(values, values + PROFILER_VALUE_COUNT, 0.0) / PROFILER_VALUE_COUNT;
    }
};

static double g_applicationValues[PROFILER_VALUE_COUNT];
static Profiler g_gpuFrameProfiler;
static Profiler g_presentProfiler;
static Profiler g_updateDirectorProfiler;
static Profiler g_renderDirectorProfiler;
static Profiler g_frameFenceProfiler;
static Profiler g_presentWaitProfiler;
static Profiler g_swapChainAcquireProfiler;

static bool g_profilerVisible;
static bool g_profilerWasToggled;

#ifdef UNLEASHED_RECOMP_D3D12
static bool g_vulkan = false;
#else
static constexpr bool g_vulkan = true;
#endif

static bool g_triangleStripWorkaround = false;

static bool g_hardwareResolve = true;
static bool g_hardwareDepthResolve = true;

static std::unique_ptr<RenderInterface> g_interface;
static std::unique_ptr<RenderDevice> g_device;

static RenderDeviceCapabilities g_capabilities;

static constexpr size_t NUM_FRAMES = 2;
static constexpr size_t NUM_QUERIES = 2;

static uint32_t g_frame = 0;
static uint32_t g_nextFrame = 1;

static std::unique_ptr<RenderCommandQueue> g_queue;
static std::unique_ptr<RenderCommandList> g_commandLists[NUM_FRAMES];
static std::unique_ptr<RenderCommandFence> g_commandFences[NUM_FRAMES];
static std::unique_ptr<RenderQueryPool> g_queryPools[NUM_FRAMES];

#if defined(__SWITCH__)
// ------------------------------------------------------------------ GPU pass profiler
// [Switch] SwitchGpuPassProfiler = true: a GPU timestamp at the start of the frame and at every
// framebuffer change, so the time between two timestamps is the GPU time of one render pass
// (including the barrier waits in front of it). Passes are grouped by what they render to and by
// their order among passes with that target, averaged over ~300 frames and printed to stderr.
// Needed to aim further GPU work: the whole-frame GPU time does not say which pass costs what.
static constexpr uint32_t PASS_PROFILER_MAX_PASSES = 160;

// [Switch] SwitchGpuDrawProfiler = true (turns the pass profiler on as well): one more timestamp after
// every draw, so each draw gets the GPU time from the previous timestamp of its pass (the pass start
// or the draw before it) to its own. Draws are grouped by pass, as in the pass table, by shader pair
// and by the state that changes their cost; the most expensive groups of the most expensive passes
// are printed after each pass table. The timestamps are written at the end of the pipeline without
// draining it, so draws that overlap share their time; the flush in front of every timestamp costs
// some GPU time, so the totals read higher than with the pass profiler alone. Shaders are named by
// the hash of their Xbox 360 code and by the start of the BLAKE3 of their SPIR-V, which the driver's
// NVK_SHADER_STATS lines print as well. Timestamps go to pools of 128 so that only the unused tail of
// the last pool has to be written at the end of the frame (plume reads whole pools).
static constexpr uint32_t DRAW_PROFILER_POOL_SIZE = 128;
static constexpr uint32_t DRAW_PROFILER_MAX_POOLS = 32; // Up to 4096 draws per frame.

enum : uint32_t
{
    DRAW_PROFILER_BLEND = 1u << 0,
    DRAW_PROFILER_Z_WRITE = 1u << 1,
    DRAW_PROFILER_ALPHA_TEST = 1u << 2,
    DRAW_PROFILER_ALPHA_TO_COVERAGE = 1u << 3,
    DRAW_PROFILER_REVERSE_Z = 1u << 4,
    DRAW_PROFILER_NO_PIXEL_SHADER = 1u << 5,
    DRAW_PROFILER_INSTANCED = 1u << 6,
    DRAW_PROFILER_Z_FUNC_SHIFT = 8, // RenderComparisonFunction, UNKNOWN when depth testing is off.
};

// PassProfilerPass::kind. Resolve copies (surface to texture) get passes of their own, by what made
// them due; their "draws" are the copies.
enum : uint32_t
{
    PASS_PROFILER_PASS = 0,
    PASS_PROFILER_COPIES_BEFORE_DRAW = 1, // the surface was about to be drawn into (or sampled while bound)
    PASS_PROFILER_COPIES_AT_CLEAR = 2,    // the surface was about to be cleared
    PASS_PROFILER_COPIES_AT_PRESENT = 3,  // end of the frame
    PASS_PROFILER_COPIES_OTHER = 4,       // the surface went away, or the texture got a CPU update
    PASS_PROFILER_KINDS = 5
};

struct PassProfilerPass
{
    uint32_t width, height;
    RenderFormat colorFormat, depthFormat;
    uint32_t samples;
    uint32_t draws;
    uint32_t kind;
};

struct DrawProfilerDraw
{
    uint32_t pass;
    uint32_t state;
    uint64_t vertexShader;
    uint64_t pixelShader;
    uint32_t vertexSpirv;
    uint32_t pixelSpirv;
    uint32_t count; // Vertices or indices, times instances.
};

struct PassProfilerFrame
{
    std::unique_ptr<RenderQueryPool> queries;
    std::vector<PassProfilerPass> passes;
    std::unique_ptr<RenderQueryPool> drawQueries[DRAW_PROFILER_MAX_POOLS];
    std::vector<DrawProfilerDraw> draws;
    bool recorded = false;
};

struct PassProfilerTotal
{
    PassProfilerPass pass{};
    uint32_t ordinal = 0;
    double gpuMs = 0.0;
    uint64_t draws = 0;
};

struct DrawProfilerKey
{
    uint32_t width, height;
    RenderFormat colorFormat, depthFormat;
    uint32_t samples;
    uint32_t ordinal;
    uint64_t vertexShader, pixelShader;
    uint32_t vertexSpirv, pixelSpirv;
    uint32_t state;

    bool operator==(const DrawProfilerKey&) const = default;
};

struct DrawProfilerKeyHash
{
    using is_avalanching = void;

    uint64_t operator()(const DrawProfilerKey& key) const
    {
        const uint64_t words[] =
        {
            (uint64_t(key.width) << 32) | key.height,
            (uint64_t(key.colorFormat) << 32) | uint32_t(key.depthFormat),
            (uint64_t(key.samples) << 32) | key.ordinal,
            key.vertexShader,
            key.pixelShader,
            (uint64_t(key.vertexSpirv) << 32) | key.pixelSpirv,
            key.state
        };
        return XXH3_64bits(words, sizeof(words));
    }
};

struct DrawProfilerTotal
{
    double gpuMs = 0.0;
    uint64_t draws = 0;
    uint64_t count = 0;
};

static bool g_passProfilerEnabled;
static bool g_drawProfilerEnabled;
static PassProfilerFrame g_passProfilerFrames[NUM_FRAMES];
static std::vector<PassProfilerTotal> g_passProfilerTotals;
static ankerl::unordered_dense::map<DrawProfilerKey, DrawProfilerTotal, DrawProfilerKeyHash> g_drawProfilerTotals;
static uint32_t g_passProfilerFrameCount;
static double g_passProfilerFrameMs;

// Render-thread counters over one report: vertex/index buffer unlocks from the game's D3D thread
// (streamed or copied into place between draws) and texture updates between draws.
static uint64_t g_profilerStreamedBuffers;
static uint64_t g_profilerCopiedBuffers;
static uint64_t g_profilerTextureUpdates;
// Resolves: copies by trigger (PASS_PROFILER_COPIES_*) and their pixels, render targets handed over
// to their texture at a clear, copies kept pending over a Present, depth copies dropped at a Present
// (as always: depth resolves are transient). Barrier batches and render target changes.
static uint64_t g_profilerResolveCopies[PASS_PROFILER_KINDS];
static uint64_t g_profilerResolvePixels;
static uint64_t g_profilerHandOvers;
static uint64_t g_profilerCopiesKept;
static uint64_t g_profilerDepthDropped;
static uint64_t g_profilerBarrierBatches;
static uint64_t g_profilerFramebufferChanges;
// Round 5: resolve copies replaced by a hand-over at the draw that needed them, and why the others
// could not be (blending or partial write mask, a depth buffer bound, several textures, images that differ,
// pipeline variant still compiling); constant bytes uploaded and not uploaded.
static uint64_t g_profilerCoverageHandOvers;
static uint64_t g_profilerCoverageMissBlend;
static uint64_t g_profilerCoverageMissDepth;
static uint64_t g_profilerCoverageMissTextures;
static uint64_t g_profilerCoverageMissImage;
static uint64_t g_profilerCoverageMissVariant;
static uint64_t g_profilerConstantBytes;
static uint64_t g_profilerConstantBytesSaved;
// Round 6: barrier batches flushed between two draws of the same pass (each one is a GPU wait for idle),
// and the shader constant bytes the game's thread copied for the render thread (span of the dirty
// registers as before, and what was really copied).
static uint64_t g_profilerMidPassBarrierBatches;
static std::atomic<uint64_t> g_profilerMainConstantSpanBytes;
static std::atomic<uint64_t> g_profilerMainConstantCopiedBytes;
// Round 7: hand-overs without marks and fix-up, clears not made, draws not sent, pipeline and sampler
// lookups answered by their caches (render thread); render and sampler states not sent (game thread).
static uint64_t g_profilerExactHandOvers;
static uint64_t g_profilerExactCoverageMisses[4]; // By ExactCoverageMiss.
static uint64_t g_profilerClearsDeferred;
static uint64_t g_profilerClearsSkipped;
static uint64_t g_profilerClearsCarried;      // Round 9: colour clears made in a later pass (SwitchCarryClears)
static uint64_t g_profilerClearsReplaced;     // Round 9: waiting clears a later clear of the same surface replaced
static uint64_t g_profilerDepthClearsDeferred; // Round 9: SwitchSkipOverwrittenDepthClears
static uint64_t g_profilerDepthClearsSkipped;
static uint64_t g_profilerDeadCopiesSkipped;  // Round 9: SwitchSkipDeadCopies (DropDeadPendingResolves)
static uint64_t g_profilerDeadCopiesRead;
static uint64_t g_profilerDeadCopiesUnknown;
static uint64_t g_profilerDeadCopyWaitUs;
static uint64_t g_profilerNoOpDraws;
static uint64_t g_profilerPipelineCacheHits;
static uint64_t g_profilerSamplerCacheHits;
static std::atomic<uint64_t> g_profilerStatesSkipped;

// Round 8: the statistics the game's thread counts (it is their only writer) without an atomic read-modify-write
// (a load/store-exclusive loop on the Cortex-A57) per render state and draw; the report on the render thread
// reads and resets them, so an increment next to a reset may count in either window.
static void AddGameThreadCount(std::atomic<uint64_t>& counter, uint64_t value)
{
    counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}
// Round 8: draws that copy a surface's pending resolve back into it, skipped (colour, depth), and the
// candidates that were not (by RestoreMiss).
static uint64_t g_profilerRestoresSkipped[2];
static uint64_t g_profilerRestoreMisses[6];
// Where the game's thread spends a frame (Present, game thread only): its own work between two Presents, then
// in Present the wait for the render thread to finish the frame's commands, the present call, the wait for
// the GPU to finish the frame before last, getting the next swap chain image and the frame limiter. At the
// 60 fps cap the frame rate hides CPU savings; the working time shows them.
enum FrameTime
{
    FRAME_TIME_WORK,
    FRAME_TIME_RENDER_THREAD,
    FRAME_TIME_PRESENT,
    FRAME_TIME_GPU,
    FRAME_TIME_ACQUIRE,
    FRAME_TIME_LIMITER,
    FRAME_TIME_COUNT
};
static double g_profilerFrameTimes[FRAME_TIME_COUNT];
static double g_profilerLongestWork;
static uint32_t g_profilerFrameTimeFrames;
// SwitchPresentOnRenderThread: the render thread adds its present, GPU and acquire times, and reports.
static std::mutex g_frameTimesMutex;
static bool g_presentOnRenderThread = false; // See PresentOnRenderThread.
// What the resolve copies about to be recorded are due to (set by the callers).
static uint32_t g_resolveCopyTrigger = PASS_PROFILER_COPIES_BEFORE_DRAW;
// Render commands the render thread has taken off its queue (only it writes this), for the stall
// watchdog's dumps (ReportRendererState).
static std::atomic<uint64_t> g_renderCommandsTaken;
static void ReportRendererState(std::string& out);

static void AppendFormat(std::string& out, const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    if (length > 0)
        out.append(line, std::min<size_t>(size_t(length), sizeof(line) - 1));
}

// The reports go to stderr.log on the SD card. The render thread only formats them; this thread writes
// each one with a single call. Writing them from the render thread, line-buffered, i.e. one SD card
// write per line, stalled it for up to half a second at every report. (An fwrite to the line-buffered
// stderr is still one write per line, holding stderr's lock throughout: WriteLog is one write.)
static void WriteReportAsync(std::string report)
{
    struct Writer
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<std::string> reports;
        std::thread* thread = nullptr;
    };

    // Never destroyed, and the thread is never detached: pthread_detach fails on Horizon, and
    // std::thread::detach() then throws (std::terminate). A std::thread that is never destroyed
    // needs neither; the thread may still be waiting when the process exits.
    static Writer* writer = []
        {
            auto* newWriter = new Writer();
            newWriter->thread = new std::thread([newWriter]
                {
                    while (true)
                    {
                        std::vector<std::string> reports;
                        {
                            std::unique_lock lock(newWriter->mutex);
                            newWriter->condition.wait(lock, [newWriter] { return !newWriter->reports.empty(); });
                            reports.swap(newWriter->reports);
                        }

                        for (const auto& text : reports)
                        {
#if defined(__SWITCH__)
                            os::switch_cpu_profiler::WriteLog(text);
#else
                            fwrite(text.data(), 1, text.size(), stderr);
#endif
                        }

                        fflush(stderr);
                    }
                });
            return newWriter;
        }();

    {
        std::lock_guard lock(writer->mutex);
        writer->reports.push_back(std::move(report));
    }
    writer->condition.notify_one();
}

static void PassProfilerOpen(const PassProfilerPass& pass)
{
    auto& frame = g_passProfilerFrames[g_frame];
    if (!g_passProfilerEnabled || frame.passes.size() >= PASS_PROFILER_MAX_PASSES)
        return;

    // Timestamp k opens pass k (and closes pass k - 1).
    g_commandLists[g_frame]->writeTimestamp(frame.queries.get(), uint32_t(frame.passes.size()));
    frame.passes.push_back(pass);
}

static void PassProfilerBeginFrame()
{
    auto& frame = g_passProfilerFrames[g_frame];
    frame.passes.clear();
    frame.draws.clear();
    frame.recorded = false;

    if (!g_passProfilerEnabled)
        return;

    g_commandLists[g_frame]->resetQueryPool(frame.queries.get(), 0, PASS_PROFILER_MAX_PASSES + 1);

    // Queries may only be reset outside render passes, so every pool is reset here.
    if (g_drawProfilerEnabled)
    {
        for (auto& pool : frame.drawQueries)
            g_commandLists[g_frame]->resetQueryPool(pool.get(), 0, DRAW_PROFILER_POOL_SIZE);
    }

    PassProfilerOpen(PassProfilerPass{});
}

static void PassProfilerFramebuffer(GuestSurface* renderTarget, GuestSurface* depthStencil)
{
    if (!g_passProfilerEnabled)
        return;

    PassProfilerPass pass{};
    GuestSurface* size = renderTarget != nullptr ? renderTarget : depthStencil;
    if (size != nullptr)
    {
        pass.width = size->width;
        pass.height = size->height;
        pass.samples = uint32_t(size->sampleCount);
    }

    pass.colorFormat = renderTarget != nullptr ? renderTarget->format : RenderFormat::UNKNOWN;
    pass.depthFormat = depthStencil != nullptr ? depthStencil->format : RenderFormat::UNKNOWN;
    PassProfilerOpen(pass);
}

// Before every resolve copy.
static void FrameLogCopy(const GuestSurface* surface, const GuestTexture* texture, uint32_t trigger);

static void PassProfilerCopy(const GuestSurface* surface, const GuestTexture* texture)
{
    FrameLogCopy(surface, texture, g_resolveCopyTrigger);
    g_profilerResolveCopies[g_resolveCopyTrigger]++;
    g_profilerResolvePixels += uint64_t(texture->width) * texture->height;

    auto& frame = g_passProfilerFrames[g_frame];
    if (!g_passProfilerEnabled)
        return;

    if (frame.passes.empty() || frame.passes.back().kind != g_resolveCopyTrigger)
    {
        const bool depth = surface->format == RenderFormat::D32_FLOAT;
        PassProfilerPass pass{};
        pass.width = surface->width;
        pass.height = surface->height;
        pass.samples = uint32_t(surface->sampleCount);
        pass.colorFormat = depth ? RenderFormat::UNKNOWN : surface->format;
        pass.depthFormat = depth ? surface->format : RenderFormat::UNKNOWN;
        pass.kind = g_resolveCopyTrigger;
        PassProfilerOpen(pass);
    }

    if (!frame.passes.empty() && frame.passes.back().kind == g_resolveCopyTrigger)
        frame.passes.back().draws++;
}

// [Switch] SwitchFrameLog: the render thread's work of one frame, once a minute (at most five times), as
// "[frame]" lines in stderr.log: each framebuffer bound, clear, resolve, resolve copy, barrier batch and draw
// (shaders, state, bound textures; skipped draws too), in order. To find what a frame does that it does not
// need to, for the next optimisations. Surfaces and textures are named by their address.
struct FrameLog
{
    bool active = false;
    uint32_t logged = 0;
    uint32_t draws = 0;
    std::chrono::steady_clock::time_point next{};
    std::string text;
};

static FrameLog g_frameLog; // Render thread only.
static bool g_frameLogEnabled = false;

static const char* PassProfilerFormatName(RenderFormat format);

static uint32_t FrameLogId(const void* object)
{
    return uint32_t(reinterpret_cast<uintptr_t>(object) & 0xFFFFFF);
}

static void FrameLogBegin()
{
    if (!g_frameLogEnabled || g_frameLog.logged >= 5)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (g_frameLog.next.time_since_epoch().count() == 0)
        g_frameLog.next = now + std::chrono::seconds(60);

    if (now < g_frameLog.next)
        return;

    g_frameLog.next = now + std::chrono::seconds(60);
    g_frameLog.active = true;
    g_frameLog.draws = 0;
    g_frameLog.text.clear();
    g_frameLog.text.reserve(1 << 20);
    AppendFormat(g_frameLog.text, "[frame] log %u: the render thread's commands of one frame (P framebuffer, C clear, R resolve, "
        "X resolve copy, B barriers, D draw)\n", g_frameLog.logged + 1);
}

static void FrameLogEnd()
{
    if (!g_frameLog.active)
        return;

    g_frameLog.active = false;
    g_frameLog.logged++;
    AppendFormat(g_frameLog.text, "[frame] end of log %u: %u draws\n", g_frameLog.logged, g_frameLog.draws);
    WriteReportAsync(std::move(g_frameLog.text));
    g_frameLog.text = std::string();
}

static void FrameLogSurface(std::string& out, const char* label, const GuestBaseTexture* surface)
{
    if (surface == nullptr)
        AppendFormat(out, " %s -", label);
    else
        AppendFormat(out, " %s %06X %ux%u %s", label, FrameLogId(surface), surface->width, surface->height, PassProfilerFormatName(surface->format));
}

static void FrameLogPass(const GuestSurface* renderTarget, const GuestSurface* depthStencil)
{
    if (!g_frameLog.active)
        return;

    g_frameLog.text += "[frame] P";
    FrameLogSurface(g_frameLog.text, "rt", renderTarget);
    FrameLogSurface(g_frameLog.text, "ds", depthStencil);
    g_frameLog.text += '\n';
}

static void FrameLogClear(uint32_t flags, const float* color, float z)
{
    if (!g_frameLog.active)
        return;

    AppendFormat(g_frameLog.text, "[frame] C flags %X colour %g %g %g %g z %g", flags, color[0], color[1], color[2], color[3], z);
    FrameLogSurface(g_frameLog.text, "rt", g_renderTarget);
    FrameLogSurface(g_frameLog.text, "ds", g_depthStencil);
    g_frameLog.text += '\n';
}

static void FrameLogResolve(const GuestSurface* surface, const GuestTexture* texture)
{
    if (!g_frameLog.active)
        return;

    g_frameLog.text += "[frame] R";
    FrameLogSurface(g_frameLog.text, "from", surface);
    FrameLogSurface(g_frameLog.text, "to", texture);
    g_frameLog.text += '\n';
}

static void FrameLogCopy(const GuestSurface* surface, const GuestTexture* texture, uint32_t trigger)
{
    if (!g_frameLog.active)
        return;

    static constexpr const char* TRIGGERS[] = { "", "before-draw", "at-clear", "at-present", "other" };
    AppendFormat(g_frameLog.text, "[frame] X %s", trigger < std::size(TRIGGERS) ? TRIGGERS[trigger] : "?");
    FrameLogSurface(g_frameLog.text, "from", surface);
    FrameLogSurface(g_frameLog.text, "to", texture);
    g_frameLog.text += '\n';
}

// SwitchSkipDeadCopies: a pending resolve dropped instead of copied (its texture is rewritten before any read).
static void FrameLogDeadCopy(const GuestSurface* surface, const GuestTexture* texture)
{
    if (!g_frameLog.active)
        return;

    g_frameLog.text += "[frame] X dead, not made:";
    FrameLogSurface(g_frameLog.text, "from", surface);
    FrameLogSurface(g_frameLog.text, "to", texture);
    g_frameLog.text += '\n';
}

static void FrameLogBarriers(size_t count)
{
    if (g_frameLog.active)
        AppendFormat(g_frameLog.text, "[frame] B %zu\n", count);
}

static uint64_t DrawProfilerShaderId(const GuestShader* shader);

// `skipped`: why the draw was not recorded, or nullptr.
static void FrameLogDraw(const char* kind, uint32_t count, const char* skipped)
{
    if (!g_frameLog.active)
        return;

    g_frameLog.draws++;
    const auto& state = g_pipelineState;
    AppendFormat(g_frameLog.text, "[frame] D %s %u vs %016llX ps %016llX mask %X", kind, count,
        (unsigned long long)DrawProfilerShaderId(state.vertexShader), (unsigned long long)DrawProfilerShaderId(state.pixelShader),
        state.colorWriteEnable);
    if (state.alphaBlendEnable)
        AppendFormat(g_frameLog.text, " blend %u/%u/%u", uint32_t(state.srcBlend), uint32_t(state.destBlend), uint32_t(state.blendOp));
    if (state.zEnable)
        AppendFormat(g_frameLog.text, " z %u%s", uint32_t(state.zFunc), state.zWriteEnable ? " write" : "");
    if ((state.specConstants & SPEC_CONSTANT_ALPHA_TEST) != 0)
        g_frameLog.text += " alphatest";
    if (state.enableAlphaToCoverage)
        g_frameLog.text += " a2c";

    for (uint32_t slot = 0; slot < std::size(g_textures); slot++)
    {
        const GuestTexture* texture = g_textures[slot];
        if (texture == nullptr)
            continue;

        AppendFormat(g_frameLog.text, " t%u %06X %ux%u", slot, FrameLogId(texture), texture->width, texture->height);
        if (texture->sourceSurface != nullptr)
            AppendFormat(g_frameLog.text, "<%06X", FrameLogId(texture->sourceSurface));
    }

    if (skipped != nullptr)
        AppendFormat(g_frameLog.text, " SKIPPED %s", skipped);
    g_frameLog.text += '\n';
}

static void PassProfilerCountDraw()
{
    auto& frame = g_passProfilerFrames[g_frame];
    if (g_passProfilerEnabled && !frame.passes.empty())
        frame.passes.back().draws++;
}

static uint64_t DrawProfilerShaderId(const GuestShader* shader)
{
    if (shader == nullptr)
        return 0;

    if (shader->shaderCacheEntry != nullptr)
        return shader->shaderCacheEntry->hash;

    // A shader the port wrote itself (blur, motion blur, CSD...): no cache hash to show.
    return uint64_t(reinterpret_cast<uintptr_t>(shader)) | (1ull << 63);
}

// After every draw of the render thread (with PassProfilerCountDraw).
static void DrawProfilerAfterDraw(uint32_t count, bool instanced)
{
    auto& frame = g_passProfilerFrames[g_frame];
    if (!g_drawProfilerEnabled || frame.passes.empty() || frame.draws.size() >= DRAW_PROFILER_POOL_SIZE * DRAW_PROFILER_MAX_POOLS)
        return;

    const auto& state = g_pipelineState;
    const bool colorWrites = state.colorWriteEnable != 0 && state.renderTargetFormat != RenderFormat::UNKNOWN;
    const bool depthTest = state.zEnable && state.depthStencilFormat != RenderFormat::UNKNOWN;
    const bool alphaTest = (state.specConstants & SPEC_CONSTANT_ALPHA_TEST) != 0;

    DrawProfilerDraw draw{};
    draw.pass = uint32_t(frame.passes.size() - 1);
    draw.count = count;
    draw.vertexShader = DrawProfilerShaderId(state.vertexShader);
    draw.vertexSpirv = state.vertexShader != nullptr ? state.vertexShader->spirvBlake3 : 0;

    // The same test as CreateGraphicsPipeline's depth-only pipelines without a fragment stage.
    if (state.pixelShader == nullptr || (!colorWrites && depthTest && !state.enableAlphaToCoverage && !alphaTest &&
        state.pixelShader->removableInDepthOnlyPass.load(std::memory_order_relaxed)))
    {
        draw.state |= DRAW_PROFILER_NO_PIXEL_SHADER;
    }
    else
    {
        draw.pixelShader = DrawProfilerShaderId(state.pixelShader);
        draw.pixelSpirv = state.pixelShader->spirvBlake3;
    }

    if (colorWrites && state.alphaBlendEnable)
        draw.state |= DRAW_PROFILER_BLEND;
    if (depthTest && state.zWriteEnable)
        draw.state |= DRAW_PROFILER_Z_WRITE;
    if (alphaTest)
        draw.state |= DRAW_PROFILER_ALPHA_TEST;
    if (state.enableAlphaToCoverage)
        draw.state |= DRAW_PROFILER_ALPHA_TO_COVERAGE;
    if ((state.specConstants & SPEC_CONSTANT_REVERSE_Z) != 0)
        draw.state |= DRAW_PROFILER_REVERSE_Z;
    if (instanced)
        draw.state |= DRAW_PROFILER_INSTANCED;
    draw.state |= uint32_t(depthTest ? state.zFunc : RenderComparisonFunction::UNKNOWN) << DRAW_PROFILER_Z_FUNC_SHIFT;

    const uint32_t index = uint32_t(frame.draws.size());
    g_commandLists[g_frame]->writeTimestamp(frame.drawQueries[index / DRAW_PROFILER_POOL_SIZE].get(), index % DRAW_PROFILER_POOL_SIZE);
    frame.draws.push_back(draw);
}

static void PassProfilerEndFrame()
{
    auto& frame = g_passProfilerFrames[g_frame];
    if (!g_passProfilerEnabled || frame.passes.empty())
        return;

    // Closing timestamp of the last pass. The unused slots get one too: plume reads the whole pool,
    // and an unwritten query would make the read fail (VK_NOT_READY). They are written back to back
    // after the frame's work, so they cost next to nothing.
    for (uint32_t i = uint32_t(frame.passes.size()); i <= PASS_PROFILER_MAX_PASSES; i++)
        g_commandLists[g_frame]->writeTimestamp(frame.queries.get(), i);

    // The same for the tail of the last draw pool in use; later pools are not read.
    if (g_drawProfilerEnabled && !frame.draws.empty())
    {
        const uint32_t used = uint32_t(frame.draws.size());
        auto* pool = frame.drawQueries[(used - 1) / DRAW_PROFILER_POOL_SIZE].get();
        for (uint32_t i = used % DRAW_PROFILER_POOL_SIZE; i != 0 && i < DRAW_PROFILER_POOL_SIZE; i++)
            g_commandLists[g_frame]->writeTimestamp(pool, i);
    }

    frame.recorded = true;
}

static const char* PassProfilerFormatName(RenderFormat format)
{
    switch (format)
    {
    case RenderFormat::UNKNOWN: return "-";
    case RenderFormat::R16G16B16A16_FLOAT: return "RGBA16F";
    case RenderFormat::R8G8B8A8_UNORM: return "RGBA8";
    case RenderFormat::B8G8R8A8_UNORM: return "BGRA8";
    case RenderFormat::R16G16_FLOAT: return "RG16F";
    case RenderFormat::R32_FLOAT: return "R32F";
    case RenderFormat::R8_UNORM: return "R8";
    case RenderFormat::D32_FLOAT: return "D32F";
    default: return "fmt";
    }
}

static std::string DrawProfilerShaderName(uint64_t id, uint32_t spirv)
{
    // Hand-written shader, named by its address (game shaders have a SPIR-V hash, and half of their
    // cache hashes have the top bit set too).
    if ((id >> 63) != 0 && spirv == 0)
        return fmt::format("port:{:04X}", uint32_t(id & 0xFFFF));

    return fmt::format("{:016X}/{:08x}", id, spirv);
}

// The draw groups of the most expensive passes, after the pass table (which is sorted by then).
static void DrawProfilerPrint(double frames, std::string& report)
{
    static constexpr const char* Z_FUNC_NAMES[] = { "noZ", "never", "<", "==", "<=", ">", "!=", ">=", "always" };
    static constexpr size_t MAX_PASSES = 16;
    static constexpr size_t MAX_GROUPS = 16;

    std::vector<std::pair<const DrawProfilerKey*, const DrawProfilerTotal*>> groups;
    groups.reserve(g_drawProfilerTotals.size());
    for (const auto& [key, total] : g_drawProfilerTotals)
        groups.emplace_back(&key, &total);

    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.second->gpuMs > b.second->gpuMs; });

    size_t passesShown = 0;
    for (const auto& passTotal : g_passProfilerTotals)
    {
        const auto& p = passTotal.pass;
        const double passMs = passTotal.gpuMs / frames;
        if (passesShown == MAX_PASSES || passMs < 0.1)
            break;
        if (p.width == 0)
            continue;

        passesShown++;
        AppendFormat(report, "[gpu draws] %ux%u %s x%u depth %s #%u: %.2f ms, %.0f draws per frame; groups by shaders and state:\n",
            p.width, p.height, PassProfilerFormatName(p.colorFormat), p.samples, PassProfilerFormatName(p.depthFormat),
            passTotal.ordinal, passMs, double(passTotal.draws) / frames);

        size_t shownGroups = 0;
        size_t otherGroups = 0;
        double otherMs = 0.0;
        for (const auto& [key, total] : groups)
        {
            if (key->width != p.width || key->height != p.height || key->colorFormat != p.colorFormat ||
                key->depthFormat != p.depthFormat || key->samples != p.samples || key->ordinal != passTotal.ordinal)
            {
                continue;
            }

            const double ms = total->gpuMs / frames;
            if (shownGroups == MAX_GROUPS)
            {
                otherGroups++;
                otherMs += ms;
                continue;
            }

            shownGroups++;

            const uint32_t zFunc = std::min<uint32_t>(key->state >> DRAW_PROFILER_Z_FUNC_SHIFT, std::size(Z_FUNC_NAMES) - 1);
            const std::string pixelShader = (key->state & DRAW_PROFILER_NO_PIXEL_SHADER) != 0 ?
                std::string("none") : DrawProfilerShaderName(key->pixelShader, key->pixelSpirv);

            AppendFormat(report, "  %6.3f ms %6.1f draws %8.0f verts  ps %-25s vs %-25s z%s%s%s%s%s%s%s\n",
                ms, double(total->draws) / frames, double(total->count) / frames,
                pixelShader.c_str(), DrawProfilerShaderName(key->vertexShader, key->vertexSpirv).c_str(),
                Z_FUNC_NAMES[zFunc],
                (key->state & DRAW_PROFILER_Z_WRITE) != 0 ? " zwrite" : "",
                (key->state & DRAW_PROFILER_BLEND) != 0 ? " blend" : "",
                (key->state & DRAW_PROFILER_ALPHA_TEST) != 0 ? " alphatest" : "",
                (key->state & DRAW_PROFILER_ALPHA_TO_COVERAGE) != 0 ? " a2c" : "",
                (key->state & DRAW_PROFILER_REVERSE_Z) != 0 ? " reverseZ" : "",
                (key->state & DRAW_PROFILER_INSTANCED) != 0 ? " instanced" : "");
        }

        if (otherGroups != 0)
            AppendFormat(report, "  %6.3f ms in %zu more groups\n", otherMs, otherGroups);
    }
}

// Called once the frame's fence has been waited on, so its timestamps are final.
static void PassProfilerCollect(double gpuFrameMs)
{
    auto& frame = g_passProfilerFrames[g_frame];
    if (!g_passProfilerEnabled || !frame.recorded)
        return;

    frame.queries->queryResults();
    const uint64_t* timestamps = frame.queries->getResults();

    // Group by target description and by the order among passes with the same description.
    std::vector<uint32_t> ordinals(frame.passes.size());
    for (size_t i = 0; i < frame.passes.size(); i++)
    {
        const auto& pass = frame.passes[i];
        const double ms = double(timestamps[i + 1] - timestamps[i]) / 1000000.0 * 1.627; // see the GPU timer note

        uint32_t ordinal = 0;
        for (size_t j = 0; j < i; j++)
        {
            const auto& other = frame.passes[j];
            if (other.width == pass.width && other.height == pass.height && other.colorFormat == pass.colorFormat &&
                other.depthFormat == pass.depthFormat && other.samples == pass.samples && other.kind == pass.kind)
            {
                ordinal++;
            }
        }
        ordinals[i] = ordinal;

        PassProfilerTotal* total = nullptr;
        for (auto& candidate : g_passProfilerTotals)
        {
            const auto& p = candidate.pass;
            if (candidate.ordinal == ordinal && p.width == pass.width && p.height == pass.height &&
                p.colorFormat == pass.colorFormat && p.depthFormat == pass.depthFormat && p.samples == pass.samples &&
                p.kind == pass.kind)
            {
                total = &candidate;
                break;
            }
        }

        if (total == nullptr)
        {
            g_passProfilerTotals.push_back(PassProfilerTotal{ pass, ordinal, 0.0, 0 });
            total = &g_passProfilerTotals.back();
        }

        total->gpuMs += ms;
        total->draws += pass.draws;
    }

    if (g_drawProfilerEnabled && !frame.draws.empty())
    {
        const uint32_t drawCount = uint32_t(frame.draws.size());
        const uint64_t* poolResults[DRAW_PROFILER_MAX_POOLS]{};
        for (uint32_t pool = 0; pool * DRAW_PROFILER_POOL_SIZE < drawCount; pool++)
        {
            frame.drawQueries[pool]->queryResults();
            poolResults[pool] = frame.drawQueries[pool]->getResults();
        }

        auto drawTimestamp = [&](uint32_t index)
            {
                return poolResults[index / DRAW_PROFILER_POOL_SIZE][index % DRAW_PROFILER_POOL_SIZE];
            };

        for (uint32_t i = 0; i < drawCount; i++)
        {
            const auto& draw = frame.draws[i];
            const uint64_t previous = (i > 0 && frame.draws[i - 1].pass == draw.pass) ? drawTimestamp(i - 1) : timestamps[draw.pass];
            const int64_t ticks = int64_t(drawTimestamp(i) - previous);
            const double ms = double(std::max<int64_t>(ticks, 0)) / 1000000.0 * 1.627;

            const auto& pass = frame.passes[draw.pass];
            DrawProfilerKey key{};
            key.width = pass.width;
            key.height = pass.height;
            key.colorFormat = pass.colorFormat;
            key.depthFormat = pass.depthFormat;
            key.samples = pass.samples;
            key.ordinal = ordinals[draw.pass];
            key.vertexShader = draw.vertexShader;
            key.pixelShader = draw.pixelShader;
            key.vertexSpirv = draw.vertexSpirv;
            key.pixelSpirv = draw.pixelSpirv;
            key.state = draw.state;

            auto& total = g_drawProfilerTotals[key];
            total.gpuMs += ms;
            total.draws++;
            total.count += draw.count;
        }
    }

    g_passProfilerFrameMs += gpuFrameMs;

    if (++g_passProfilerFrameCount < 300)
        return;

    std::sort(g_passProfilerTotals.begin(), g_passProfilerTotals.end(),
        [](const PassProfilerTotal& a, const PassProfilerTotal& b) { return a.gpuMs > b.gpuMs; });

    const double frames = double(g_passProfilerFrameCount);
    std::string report;
    report.reserve(8192);
    AppendFormat(report, "[gpu passes] average over %u frames, GPU frame %.2f ms:\n", g_passProfilerFrameCount, g_passProfilerFrameMs / frames);
    AppendFormat(report, "  per frame: %.1f vertex/index buffer unlocks streamed, %.1f copied into place, %.1f texture updates\n",
        double(g_profilerStreamedBuffers) / frames, double(g_profilerCopiedBuffers) / frames, double(g_profilerTextureUpdates) / frames);

    uint64_t resolveCopies = 0;
    for (uint64_t count : g_profilerResolveCopies)
        resolveCopies += count;

    AppendFormat(report, "  per frame: %.1f resolve copies (%.1f before draws, %.1f at clears, %.1f at present, %.1f other), "
        "%.2f Mpixels; %.1f handed over at clears, %.1f kept pending over present, %.1f depth dropped; "
        "%.1f barrier batches, %.1f render target changes\n",
        double(resolveCopies) / frames, double(g_profilerResolveCopies[PASS_PROFILER_COPIES_BEFORE_DRAW]) / frames,
        double(g_profilerResolveCopies[PASS_PROFILER_COPIES_AT_CLEAR]) / frames,
        double(g_profilerResolveCopies[PASS_PROFILER_COPIES_AT_PRESENT]) / frames,
        double(g_profilerResolveCopies[PASS_PROFILER_COPIES_OTHER]) / frames, double(g_profilerResolvePixels) / 1000000.0 / frames,
        double(g_profilerHandOvers) / frames, double(g_profilerCopiesKept) / frames, double(g_profilerDepthDropped) / frames,
        double(g_profilerBarrierBatches) / frames, double(g_profilerFramebufferChanges) / frames);

    AppendFormat(report, "  per frame: %.1f copies replaced by a hand-over at the draw (not: %.1f blend or partial mask, "
        "%.1f depth bound, %.1f several textures, %.1f images differ, %.1f variant compiling); "
        "%.0f KB of shader constants uploaded, %.0f KB skipped\n",
        double(g_profilerCoverageHandOvers) / frames, double(g_profilerCoverageMissBlend) / frames,
        double(g_profilerCoverageMissDepth) / frames, double(g_profilerCoverageMissTextures) / frames,
        double(g_profilerCoverageMissImage) / frames, double(g_profilerCoverageMissVariant) / frames,
        double(g_profilerConstantBytes) / 1024.0 / frames, double(g_profilerConstantBytesSaved) / 1024.0 / frames);

    AppendFormat(report, "  per frame: %.1f barrier batches inside a pass; game thread copied %.0f KB of shader constants "
        "(%.0f KB spanned by the changed registers)\n",
        double(g_profilerMidPassBarrierBatches) / frames,
        double(g_profilerMainConstantCopiedBytes.load(std::memory_order_relaxed)) / 1024.0 / frames,
        double(g_profilerMainConstantSpanBytes.load(std::memory_order_relaxed)) / 1024.0 / frames);

    AppendFormat(report, "  per frame: %.1f hand-overs without marks or fix-up (full coverage not proven: %.1f not a quad, "
        "%.1f shaders, %.1f shape, %.1f edges); %.1f of %.1f colour clears skipped, %.1f no-op draws skipped\n",
        double(g_profilerExactHandOvers) / frames, double(g_profilerExactCoverageMisses[0]) / frames,
        double(g_profilerExactCoverageMisses[1]) / frames, double(g_profilerExactCoverageMisses[2]) / frames,
        double(g_profilerExactCoverageMisses[3]) / frames, double(g_profilerClearsSkipped) / frames,
        double(g_profilerClearsDeferred) / frames, double(g_profilerNoOpDraws) / frames);

    AppendFormat(report, "  per frame: %.0f render and sampler states not sent by the game thread, %.0f pipeline and %.0f sampler "
        "lookups answered from their caches\n",
        double(g_profilerStatesSkipped.load(std::memory_order_relaxed)) / frames, double(g_profilerPipelineCacheHits) / frames,
        double(g_profilerSamplerCacheHits) / frames);

    // Taken (and reset) under the lock: with SwitchPresentOnRenderThread both threads add to them.
    double frameTimes[FRAME_TIME_COUNT];
    double longestWork;
    uint32_t frameTimeFrames;
    {
        std::lock_guard lock(g_frameTimesMutex);
        memcpy(frameTimes, g_profilerFrameTimes, sizeof(frameTimes));
        longestWork = g_profilerLongestWork;
        frameTimeFrames = g_profilerFrameTimeFrames;
        memset(g_profilerFrameTimes, 0, sizeof(g_profilerFrameTimes));
        g_profilerLongestWork = 0.0;
        g_profilerFrameTimeFrames = 0;
    }

    if (frameTimeFrames != 0)
    {
        const double timedFrames = double(frameTimeFrames);
        AppendFormat(report, "  game thread per frame: %.2f ms working (longest %.1f), then in Present %.2f ms waiting for the "
            "render thread, %.2f presenting, %.2f waiting for the GPU, %.2f for the next image, %.2f in the frame limiter%s\n",
            frameTimes[FRAME_TIME_WORK] / timedFrames, longestWork,
            frameTimes[FRAME_TIME_RENDER_THREAD] / timedFrames, frameTimes[FRAME_TIME_PRESENT] / timedFrames,
            frameTimes[FRAME_TIME_GPU] / timedFrames, frameTimes[FRAME_TIME_ACQUIRE] / timedFrames,
            frameTimes[FRAME_TIME_LIMITER] / timedFrames,
            g_presentOnRenderThread ? " (presenting, the GPU wait and the next image on the render thread)" : "");
    }

    AppendFormat(report, "  per frame: %.1f colour and %.1f depth restores skipped (copy draws not skipped: %.1f not a draw of vertices "
        "from memory, %.1f shaders, %.1f blend or other writes, %.1f texture not the target's pending resolve, %.1f filtering, "
        "%.1f texture coordinates)\n",
        double(g_profilerRestoresSkipped[0]) / frames, double(g_profilerRestoresSkipped[1]) / frames,
        double(g_profilerRestoreMisses[0]) / frames, double(g_profilerRestoreMisses[1]) / frames,
        double(g_profilerRestoreMisses[2]) / frames, double(g_profilerRestoreMisses[3]) / frames,
        double(g_profilerRestoreMisses[4]) / frames, double(g_profilerRestoreMisses[5]) / frames);

    AppendFormat(report, "  per frame (round 9): %.1f dead resolve copies not made (%.1f read first, %.1f undecided; %.3f ms waited "
        "for commands); %.1f colour clears made in a later pass, %.1f waiting clears replaced, %.1f of %.1f depth clears not made\n",
        double(g_profilerDeadCopiesSkipped) / frames, double(g_profilerDeadCopiesRead) / frames,
        double(g_profilerDeadCopiesUnknown) / frames, double(g_profilerDeadCopyWaitUs) / 1000.0 / frames,
        double(g_profilerClearsCarried) / frames, double(g_profilerClearsReplaced) / frames,
        double(g_profilerDepthClearsSkipped) / frames, double(g_profilerDepthClearsDeferred) / frames);

    {
        extern std::atomic<uint32_t> g_switchAudioUnderruns;
        std::string threads;
        os::switch_cpu_profiler::AppendThreadCpuUsage(threads);
        AppendFormat(report, "  cpu: %s; audio gaps since start %u\n", threads.empty() ? "-" : threads.c_str(),
            g_switchAudioUnderruns.load(std::memory_order_relaxed));
    }

    double shown = 0.0;
    size_t lines = 0;
    for (const auto& total : g_passProfilerTotals)
    {
        if (lines++ == 30)
            break;

        const auto& p = total.pass;
        const double ms = total.gpuMs / frames;
        shown += ms;
        if (p.kind != PASS_PROFILER_PASS)
        {
            static constexpr const char* TRIGGERS[] = { "", "before-draw", "at-clear", "at-present", "other" };
            AppendFormat(report, "  %6.2f ms  %4ux%-4u %-7s x%u copies %-11s #%u  draws %5.1f\n", ms, p.width, p.height,
                PassProfilerFormatName(p.colorFormat != RenderFormat::UNKNOWN ? p.colorFormat : p.depthFormat), p.samples,
                TRIGGERS[p.kind < PASS_PROFILER_KINDS ? p.kind : 0], total.ordinal, double(total.draws) / frames);
        }
        else if (p.width == 0)
            AppendFormat(report, "  %6.2f ms  frame start (no target)  draws %5.0f\n", ms, double(total.draws) / frames);
        else
            AppendFormat(report, "  %6.2f ms  %4ux%-4u %-7s x%u depth %-5s #%u  draws %5.0f\n", ms, p.width, p.height,
                PassProfilerFormatName(p.colorFormat), p.samples, PassProfilerFormatName(p.depthFormat), total.ordinal,
                double(total.draws) / frames);
    }

    AppendFormat(report, "  (%.2f ms in the passes listed)\n", shown);

    if (g_drawProfilerEnabled && !g_drawProfilerTotals.empty())
        DrawProfilerPrint(frames, report);

    WriteReportAsync(std::move(report));
    g_profilerStreamedBuffers = 0;
    g_profilerCopiedBuffers = 0;
    g_profilerTextureUpdates = 0;
    memset(g_profilerResolveCopies, 0, sizeof(g_profilerResolveCopies));
    g_profilerResolvePixels = 0;
    g_profilerHandOvers = 0;
    g_profilerCopiesKept = 0;
    g_profilerDepthDropped = 0;
    g_profilerBarrierBatches = 0;
    g_profilerFramebufferChanges = 0;
    g_profilerCoverageHandOvers = 0;
    g_profilerCoverageMissBlend = 0;
    g_profilerCoverageMissDepth = 0;
    g_profilerCoverageMissTextures = 0;
    g_profilerCoverageMissImage = 0;
    g_profilerCoverageMissVariant = 0;
    g_profilerConstantBytes = 0;
    g_profilerConstantBytesSaved = 0;
    g_profilerMidPassBarrierBatches = 0;
    g_profilerMainConstantSpanBytes.store(0, std::memory_order_relaxed);
    g_profilerMainConstantCopiedBytes.store(0, std::memory_order_relaxed);
    g_profilerExactHandOvers = 0;
    std::fill(std::begin(g_profilerExactCoverageMisses), std::end(g_profilerExactCoverageMisses), 0);
    g_profilerClearsDeferred = 0;
    g_profilerClearsSkipped = 0;
    g_profilerClearsCarried = 0;
    g_profilerClearsReplaced = 0;
    g_profilerDepthClearsDeferred = 0;
    g_profilerDepthClearsSkipped = 0;
    g_profilerDeadCopiesSkipped = 0;
    g_profilerDeadCopiesRead = 0;
    g_profilerDeadCopiesUnknown = 0;
    g_profilerDeadCopyWaitUs = 0;
    g_profilerNoOpDraws = 0;
    g_profilerPipelineCacheHits = 0;
    g_profilerSamplerCacheHits = 0;
    g_profilerStatesSkipped.store(0, std::memory_order_relaxed);
    memset(g_profilerRestoresSkipped, 0, sizeof(g_profilerRestoresSkipped));
    memset(g_profilerRestoreMisses, 0, sizeof(g_profilerRestoreMisses));

    g_passProfilerTotals.clear();
    g_drawProfilerTotals.clear();
    g_passProfilerFrameCount = 0;
    g_passProfilerFrameMs = 0.0;
}
#endif
static bool g_commandListStates[NUM_FRAMES];

static RecompMutex g_copyMutex;
static std::unique_ptr<RenderCommandQueue> g_copyQueue;
static std::unique_ptr<RenderCommandList> g_copyCommandList;
static std::unique_ptr<RenderCommandFence> g_copyCommandFence;

static std::unique_ptr<RenderSwapChain> g_swapChain;
static bool g_swapChainValid;

#if defined(__SWITCH__)
static constexpr RenderFormat BACKBUFFER_FORMAT = RenderFormat::R8G8B8A8_UNORM;
#else
static constexpr RenderFormat BACKBUFFER_FORMAT = RenderFormat::B8G8R8A8_UNORM;
#endif

static std::unique_ptr<RenderCommandSemaphore> g_acquireSemaphores[NUM_FRAMES];
static std::unique_ptr<RenderCommandSemaphore> g_renderSemaphores[NUM_FRAMES];
static uint32_t g_backBufferIndex;
static std::unique_ptr<GuestSurface> g_backBufferHolder;
static GuestSurface* g_backBuffer;

static std::unique_ptr<RenderTexture> g_intermediaryBackBufferTexture;
static uint32_t g_intermediaryBackBufferTextureWidth;
static uint32_t g_intermediaryBackBufferTextureHeight;
static uint32_t g_intermediaryBackBufferTextureDescriptorIndex;

static std::unique_ptr<RenderPipeline> g_gammaCorrectionPipeline;

struct std::unique_ptr<RenderDescriptorSet> g_textureDescriptorSet;
struct std::unique_ptr<RenderDescriptorSet> g_samplerDescriptorSet;

enum
{
    TEXTURE_DESCRIPTOR_NULL_TEXTURE_2D,
    TEXTURE_DESCRIPTOR_NULL_TEXTURE_3D,
    TEXTURE_DESCRIPTOR_NULL_TEXTURE_CUBE,
    TEXTURE_DESCRIPTOR_NULL_COUNT
};

struct TextureDescriptorAllocator
{
    RecompMutex mutex;
    uint32_t capacity = TEXTURE_DESCRIPTOR_NULL_COUNT;
    std::vector<uint32_t> freed;
    // Heap size when it is smaller than the arrays (the Switch's compact heap); no limit otherwise.
    uint32_t limit = UINT32_MAX;
    bool exhausted = false;

    uint32_t allocate()
    {
        std::lock_guard lock(mutex);

        uint32_t value;
        if (!freed.empty())
        {
            value = freed.back();
            freed.pop_back();
        }
        else if (capacity >= limit)
        {
            // Out of descriptors: hand out the null 2D texture, whose descriptor is never overwritten.
            if (!exhausted)
            {
                fprintf(stderr, "Texture descriptors: the heap of %u is full; further textures get the null texture.\n", limit);
                exhausted = true;
            }

            value = TEXTURE_DESCRIPTOR_NULL_TEXTURE_2D;
        }
        else
        {
            value = capacity;
            ++capacity;

#if defined(__SWITCH__)
            // High-water mark of the heap, so its size can be checked against real play sessions.
            if ((capacity % 1024) == 0)
                fprintf(stderr, "Texture descriptors: %u in use at most so far.\n", capacity);
#endif
        }

        return value;
    }

    void free(uint32_t value)
    {
        assert(value != NULL || exhausted);
        if (value < TEXTURE_DESCRIPTOR_NULL_COUNT)
            return; // A null descriptor handed out by a full heap: not ours to recycle.

        std::lock_guard lock(mutex);
        freed.push_back(value);
    }
};

static std::unique_ptr<RenderTexture> g_blankTextures[TEXTURE_DESCRIPTOR_NULL_COUNT];
static std::unique_ptr<RenderTextureView> g_blankTextureViews[TEXTURE_DESCRIPTOR_NULL_COUNT];

static TextureDescriptorAllocator g_textureDescriptorAllocator;

static std::unique_ptr<RenderPipelineLayout> g_pipelineLayout;
static xxHashMap<std::unique_ptr<RenderPipeline>> g_pipelines;

#ifdef ASYNC_PSO_DEBUG
static std::atomic<uint32_t> g_pipelinesCreatedInRenderThread;
static std::atomic<uint32_t> g_pipelinesCreatedAsynchronously;
static std::atomic<uint32_t> g_pipelinesDropped;
static std::atomic<uint32_t> g_pipelinesCurrentlyCompiling;
static std::string g_pipelineDebugText;
static RecompMutex g_debugMutex;
#endif

#ifdef PSO_CACHING
static xxHashMap<PipelineState> g_pipelineStatesToCache;
static RecompMutex g_pipelineCacheMutex;
#endif

static std::atomic<uint32_t> g_compilingPipelineTaskCount;
static std::atomic<uint32_t> g_pendingPipelineTaskCount;

enum class PipelineTaskType
{
    Null,
    DatabaseData,
    PrecompilePipelines,
    RecompilePipelines
};

struct PipelineTask
{
    PipelineTaskType type{};
    boost::shared_ptr<Hedgehog::Database::CDatabaseData> databaseData;
};

static RecompMutex g_pipelineTaskMutex;
static std::vector<PipelineTask> g_pipelineTaskQueue;

static void EnqueuePipelineTask(PipelineTaskType type, const boost::shared_ptr<Hedgehog::Database::CDatabaseData>& databaseData)
{
    // Precompiled pipelines deliberately do not increment 
    // this counter to overlap the compilation with intro logos.
    if (type != PipelineTaskType::PrecompilePipelines)
        ++g_compilingPipelineTaskCount;

    {
        std::lock_guard lock(g_pipelineTaskMutex);
        g_pipelineTaskQueue.emplace_back(type, databaseData);
    }

    if ((++g_pendingPipelineTaskCount) == 1)
        g_pendingPipelineTaskCount.notify_one();
}

static const PipelineState g_pipelineStateCache[] =
{
#include "cache/pipeline_state_cache.h"
};

#include "cache/vertex_element_cache.h"

static uint8_t* const g_vertexDeclarationCache[] =
{
#include "cache/vertex_declaration_cache.h"
};

static xxHashMap<std::pair<uint32_t, std::unique_ptr<RenderSampler>>> g_samplerStates;

static RecompMutex g_vertexDeclarationMutex;
static xxHashMap<GuestVertexDeclaration*> g_vertexDeclarations;

struct UploadBuffer
{
    static constexpr size_t SIZE = 16 * 1024 * 1024;

    std::unique_ptr<RenderBuffer> buffer;
    uint8_t* memory = nullptr;
    uint64_t deviceAddress = 0;
#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
    std::unique_ptr<RenderDescriptorSet> constantsSet;
#endif
};

struct UploadAllocator
{
    std::vector<UploadBuffer> buffers;
    uint32_t index = 0;
    uint32_t offset = 0;

    // reserve: bytes that must fit in the buffer from the returned offset (a uniform buffer binding covers
    // its whole block even when fewer bytes are written).
    UploadAllocation allocate(uint32_t size, uint32_t alignment, uint32_t reserve = 0)
    {
        assert(size <= UploadBuffer::SIZE);

        offset = (offset + alignment - 1) & ~(alignment - 1);

        if (offset + std::max(size, reserve) > UploadBuffer::SIZE)
        {
            ++index;
            offset = 0;
        }

        if (buffers.size() <= index)
            buffers.resize(index + 1);

        auto& buffer = buffers[index];
        if (buffer.buffer == nullptr)
        {
            buffer.buffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(UploadBuffer::SIZE, RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
            buffer.memory = reinterpret_cast<uint8_t*>(buffer.buffer->map());
            buffer.deviceAddress = buffer.buffer->getDeviceAddress();

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
            if (g_constantsUbo)
            {
                // Bindings 0/1/2 = vertex, pixel and shared constants. The dynamic offsets
                // select the block; the ranges match the sizes declared in shader_common.h.
                buffer.constantsSet = g_constantsUboSetBuilder.create(g_device.get());
                buffer.constantsSet->setBuffer(0, buffer.buffer.get(), sizeof(g_vertexShaderConstants));
                buffer.constantsSet->setBuffer(1, buffer.buffer.get(), sizeof(g_pixelShaderConstants));
                buffer.constantsSet->setBuffer(2, buffer.buffer.get(), sizeof(SharedConstants));
            }
#endif
        }
        
        auto ref = buffer.buffer->at(offset);
        offset += size;

        UploadAllocation result{ ref.ref, ref.offset, buffer.memory + ref.offset, buffer.deviceAddress + ref.offset };
#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
        result.constantsSet = buffer.constantsSet.get();
#endif
        return result;
    }

    // [Switch] SwitchTrimConstantUploads: the first copySize bytes of a constant block of blockSize bytes.
    UploadAllocation allocateConstants(const void* memory, uint32_t copySize, uint32_t blockSize, uint32_t alignment)
    {
        auto result = allocate(copySize, alignment, blockSize);
        memcpy(result.memory, memory, copySize);
        return result;
    }

    template<bool TByteSwap, typename T>
    UploadAllocation allocate(const T* memory, uint32_t size, uint32_t alignment)
    {
        auto result = allocate(size, alignment);

        if constexpr (TByteSwap)
        {
            auto destination = reinterpret_cast<T*>(result.memory);

            for (size_t i = 0; i < size; i += sizeof(T))
            {
                *destination = ByteSwap(*memory);
                ++destination;
                ++memory;
            }
        }
        else
        {
            memcpy(result.memory, memory, size);
        }

        return result;
    }

    void reset()
    {
        index = 0;
        offset = 0;
    }
};

static UploadAllocator g_uploadAllocators[NUM_FRAMES];

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
// With the constant-buffer path, the translated shaders never read the push-constant pointers. The
// pointers are only pushed before draws whose shaders still read them (the hand-written shaders, or
// shaders from a cache without the path), which saves up to three vkCmdPushConstants per draw and
// the root-table update NVK does for each of them. 0 = nothing known to be pushed.
static uint64_t g_pendingRootAddresses[3];
static uint64_t g_pushedRootAddresses[3];

static void InvalidatePushedRootAddresses()
{
    memset(g_pushedRootAddresses, 0, sizeof(g_pushedRootAddresses));
}

static bool ShaderReadsConstantsThroughUbo(const GuestShader* shader)
{
    return shader == nullptr || shader->constantsThroughUbo.load(std::memory_order_acquire);
}

static void PushRootAddressesIfNeeded()
{
    bool needed = !ShaderReadsConstantsThroughUbo(g_pipelineState.vertexShader) || !ShaderReadsConstantsThroughUbo(g_pipelineState.pixelShader);

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
    // With SPEC_CONSTANT_RELATIVE_FROM_MEMORY, the a0-indexed arrays are read through the pointer.
    const GuestShader* vertexShader = g_pipelineState.vertexShader;
    if (g_relativeFromMemory && vertexShader != nullptr && vertexShader->shaderCacheEntry != nullptr &&
        (vertexShader->shaderCacheEntry->specConstantsMask & SPEC_CONSTANT_RELATIVE_FROM_MEMORY) != 0)
    {
        needed = true;
    }
#endif

    if (!needed)
        return;

    auto& commandList = g_commandLists[g_frame];
    for (size_t i = 0; i < 3; i++)
    {
        if (g_pendingRootAddresses[i] != g_pushedRootAddresses[i])
        {
            commandList->setGraphicsPushConstants(0, &g_pendingRootAddresses[i], 8 * i, 8);
            g_pushedRootAddresses[i] = g_pendingRootAddresses[i];
        }
    }
}
#endif

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
// Where the vertex/pixel/shared constant blocks currently live, and what set 4 was last bound to.
struct ConstantsUboBinding
{
    RenderDescriptorSet* sets[3]{};
    uint32_t offsets[3]{};
    RenderDescriptorSet* boundSet = nullptr;
    uint32_t boundOffsets[3]{};

    void Record(size_t index, const UploadAllocation& allocation)
    {
        sets[index] = allocation.constantsSet;
        offsets[index] = uint32_t(allocation.offset);
    }

    // One descriptor set can only address its own upload buffer.
    bool InOneBuffer() const
    {
        return sets[0] == sets[1] && sets[1] == sets[2];
    }

    void Bind(RenderCommandList* commandList)
    {
        if (sets[0] == nullptr || !InOneBuffer())
            return;

        if (boundSet == sets[0] && memcmp(boundOffsets, offsets, sizeof(offsets)) == 0)
            return;

        commandList->setGraphicsDescriptorSetDynamic(sets[0], CONSTANTS_UBO_SET_INDEX, offsets, 3);
        boundSet = sets[0];
        memcpy(boundOffsets, offsets, sizeof(offsets));
    }
};

static ConstantsUboBinding g_constantsUboBinding;
#endif

struct IntermediaryUploadAllocator
{
    static constexpr size_t SIZE = 16 * 1024 * 1024;

    std::vector<std::unique_ptr<uint8_t[]>> buffers;
    uint32_t index = 0;
    uint32_t offset = 0;

    uint8_t* allocate(uint32_t size)
    {
        assert(size <= SIZE);

        if (offset + size > SIZE)
        {
            ++index;
            offset = 0;
        }

        if (buffers.size() <= index)
            buffers.resize(index + 1);

        auto& buffer = buffers[index];
        if (buffer == nullptr)
            buffer = std::make_unique_for_overwrite<uint8_t[]>(SIZE);

        auto result = buffer.get() + offset;
        offset += ((size + 0xF) & ~0xF);

        return result;
    }

    uint8_t* allocate(const void* memory, uint32_t size)
    {
        auto result = allocate(size);
        memcpy(result, memory, size);
        return result;
    }

    void reset()
    {
        index = 0;
        offset = 0;
    }
};

static IntermediaryUploadAllocator g_intermediaryUploadAllocator;

static std::vector<GuestResource*> g_tempResources[NUM_FRAMES];
static std::vector<std::unique_ptr<RenderBuffer>> g_tempBuffers[NUM_FRAMES];

template<GuestPrimitiveType PrimitiveType>
struct PrimitiveIndexData
{
    std::vector<uint16_t> indexData;
    RenderBufferReference indexBuffer;
    uint32_t currentIndexCount = 0;

    uint32_t prepare(uint32_t guestPrimCount)
    {
        uint32_t primCount;
        uint32_t indexCountPerPrimitive;

        switch (PrimitiveType)
        {
        case D3DPT_TRIANGLEFAN:
            primCount = guestPrimCount - 2;
            indexCountPerPrimitive = 3; 
            break;
        case D3DPT_QUADLIST:
            primCount = guestPrimCount / 4;
            indexCountPerPrimitive = 6;
            break;
        default:
            assert(false && "Unknown primitive type.");
            break;
        }

        uint32_t indexCount = primCount * indexCountPerPrimitive;

        if (indexData.size() < indexCount)
        {
            const size_t oldPrimCount = indexData.size() / indexCountPerPrimitive;
            indexData.resize(indexCount);

            for (size_t i = oldPrimCount; i < primCount; i++)
            {
                switch (PrimitiveType)
                {
                case D3DPT_TRIANGLEFAN:
                {
                    indexData[i * 3 + 0] = 0;
                    indexData[i * 3 + 1] = static_cast<uint16_t>(i + 1);
                    indexData[i * 3 + 2] = static_cast<uint16_t>(i + 2);
                    break;
                }
                case D3DPT_QUADLIST:
                {
                    indexData[i * 6 + 0] = static_cast<uint16_t>(i * 4 + 0);
                    indexData[i * 6 + 1] = static_cast<uint16_t>(i * 4 + 1);
                    indexData[i * 6 + 2] = static_cast<uint16_t>(i * 4 + 2);

                    indexData[i * 6 + 3] = static_cast<uint16_t>(i * 4 + 0);
                    indexData[i * 6 + 4] = static_cast<uint16_t>(i * 4 + 2);
                    indexData[i * 6 + 5] = static_cast<uint16_t>(i * 4 + 3);
                    break;
                }
                default:
                    assert(false && "Unknown primitive type.");
                    break;
                }
            }
        }

        if (indexBuffer == NULL || currentIndexCount < indexCount)
        {
            auto allocation = g_uploadAllocators[g_frame].allocate<false>(indexData.data(), indexCount * 2, 2);
            indexBuffer = allocation.buffer->at(allocation.offset);
            currentIndexCount = indexCount;
        }

        SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.buffer, indexBuffer);
        SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.size, indexCount * 2);
        SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.format, RenderFormat::R16_UINT);

        return indexCount;
    }

    void reset()
    {
        indexBuffer = {};
        currentIndexCount = 0;
    }
};

static PrimitiveIndexData<D3DPT_TRIANGLEFAN> g_triangleFanIndexData;
static PrimitiveIndexData<D3DPT_QUADLIST> g_quadIndexData;

#if defined(__SWITCH__)
static void ForgetDestroyedBuffer(const GuestBuffer* buffer);
#endif

#if defined(__SWITCH__)
// [Switch] SwitchResolveHandOver: views, descriptors and framebuffers that commands already recorded
// in the frame may still use after their texture or surface got another image. Released when the
// frame slot comes around again (its fence has been waited on by then).
static std::vector<std::unique_ptr<RenderTextureView>> g_handOverViews[NUM_FRAMES];
static std::vector<uint32_t> g_handOverDescriptors[NUM_FRAMES];
static std::vector<std::unique_ptr<RenderFramebuffer>> g_handOverFramebuffers[NUM_FRAMES];

// Every render target and depth surface alive (not the back buffer). Their framebuffer caches are keyed
// by colour image, so a destroyed image is dropped from all of them: a new image at the same address
// must not find a framebuffer of the old one. Images move between textures and surfaces with hand-overs.
static ankerl::unordered_dense::set<GuestSurface*> g_liveSurfaces;

static void ForgetFramebuffersOf(const RenderTexture* image)
{
    for (GuestSurface* surface : g_liveSurfaces)
    {
        auto findResult = surface->framebuffers.find(image);
        if (findResult != surface->framebuffers.end())
        {
            if (g_framebuffer == findResult->second.get())
                g_framebuffer = nullptr;

            surface->framebuffers.erase(findResult);
        }
    }
}
#endif

#if defined(__SWITCH__)
static void ForgetCoverageFramebuffersOf(const RenderTexture* image);
#endif

static void DestructTempResources()
{
#if defined(__SWITCH__)
    for (uint32_t descriptorIndex : g_handOverDescriptors[g_frame])
        g_textureDescriptorAllocator.free(descriptorIndex);

    g_handOverDescriptors[g_frame].clear();
    g_handOverViews[g_frame].clear();
    g_handOverFramebuffers[g_frame].clear();
#endif

    for (auto resource : g_tempResources[g_frame])
    {
        switch (resource->type)
        {
        case ResourceType::Texture:
        case ResourceType::VolumeTexture:
        {
            const auto texture = reinterpret_cast<GuestTexture*>(resource);

            if (texture->mappedMemory != nullptr)
                g_userHeap.Free(texture->mappedMemory);

            g_textureDescriptorAllocator.free(texture->descriptorIndex);

            if (texture->patchedTexture != nullptr)
                g_textureDescriptorAllocator.free(texture->patchedTexture->descriptorIndex); 
            
            if (texture->recreatedCubeMapTexture != nullptr)
                g_textureDescriptorAllocator.free(texture->recreatedCubeMapTexture->descriptorIndex);

#if defined(__SWITCH__)
            if (texture->hadSurfaceImage)
            {
                ForgetFramebuffersOf(texture->texture);
                ForgetCoverageFramebuffersOf(texture->texture);
            }
#endif

            texture->~GuestTexture();
            break;
        }

        case ResourceType::VertexBuffer:
        case ResourceType::IndexBuffer:
        {
            const auto buffer = reinterpret_cast<GuestBuffer*>(resource);

#if defined(__SWITCH__)
            ForgetDestroyedBuffer(buffer);
#endif

            if (buffer->mappedMemory != nullptr)
                g_userHeap.Free(buffer->mappedMemory);

            buffer->~GuestBuffer();
            break;
        }

        case ResourceType::RenderTarget:
        case ResourceType::DepthStencil:
        {
            const auto surface = reinterpret_cast<GuestSurface*>(resource);

            if (surface->descriptorIndex != NULL)
                g_textureDescriptorAllocator.free(surface->descriptorIndex);

#if defined(__SWITCH__)
            g_liveSurfaces.erase(surface);
            ForgetFramebuffersOf(surface->texture);
            ForgetCoverageFramebuffersOf(surface->texture);
#endif

            surface->~GuestSurface();
            break;
        }

        case ResourceType::VertexDeclaration:
            reinterpret_cast<GuestVertexDeclaration*>(resource)->~GuestVertexDeclaration();
            break;

        case ResourceType::VertexShader:
        case ResourceType::PixelShader:
        {
            reinterpret_cast<GuestShader*>(resource)->~GuestShader();
            break;
        }
        }

        g_userHeap.Free(resource);
    }

    g_tempResources[g_frame].clear();
    g_tempBuffers[g_frame].clear();
}

static std::thread::id g_presentThreadId = std::this_thread::get_id();
#if defined(__SWITCH__)
// [Switch] Round 8: whether this thread is the D3D thread, remembered per thread until g_presentThreadId changes
// (g_presentThreadEpoch), instead of asking for the thread's id (pthread_self through newlib) on every render
// command and state the game sends.
// Round 9: by the thread's TLS region instead (TPIDRRO_EL0, one register read: every thread has its own), noted
// wherever g_presentThreadId is set, on that thread; the thread-local variables cost a call per access (-mtp=soft).
static std::atomic<uint32_t> g_presentThreadEpoch{ 0 };

static uintptr_t CurrentThreadTlsRegion()
{
    uintptr_t region;
    __asm__ ("mrs %x[data], tpidrro_el0" : [data] "=r" (region));
    return region;
}

static std::atomic<uintptr_t> g_presentThreadTlsRegion{ CurrentThreadTlsRegion() };

static bool IsPresentThread()
{
    return CurrentThreadTlsRegion() == g_presentThreadTlsRegion.load(std::memory_order_acquire);
}
#else
static bool IsPresentThread()
{
    return std::this_thread::get_id() == g_presentThreadId;
}
#endif
static std::atomic<bool> g_readyForCommands;
static void FlushDeferredRenderCommands();
#if defined(__SWITCH__)
// StateFilter: changed by any thread that sends a state other than the D3D thread, and by a new D3D thread.
static std::atomic<uint32_t> g_stateFilterEpoch{ 0 };

// [Switch] Round 9, SwitchPresentOnRenderThread. Present waits only until the render thread has recorded the
// frame; the render thread then submits it, presents, waits for the GPU to finish the frame slot it reuses and
// acquires the next image by itself (PresentOnRenderThread), while the game thread starts its next frame. Only
// once the game renders (its D3D thread took over; the installer's frames stay as they were).
static std::atomic<bool> g_gamePresenting{ false };
static std::atomic<bool> g_recordedCommandList{ false };
// Frames whose present the render thread has still to finish: the D3D thread waits for it before it locks a
// buffer or texture again (their unlocks are read from guest memory by the render thread; see WaitForPresentTail).
static std::atomic<uint32_t> g_presentTailsSent{ 0 };
static std::atomic<uint32_t> g_presentTailsDone{ 0 };
#endif


PPC_FUNC_IMPL(__imp__sub_824ECA00);
PPC_FUNC(sub_824ECA00)
{
    // Guard against thread ownership changes when between command lists.
    g_readyForCommands.wait(false);
    FlushDeferredRenderCommands(); // Normally empty here: Present flushes.
    g_presentThreadId = std::this_thread::get_id();
#if defined(__SWITCH__)
    g_presentThreadTlsRegion.store(CurrentThreadTlsRegion(), std::memory_order_release);
    g_presentThreadEpoch.fetch_add(1, std::memory_order_acq_rel);
    g_stateFilterEpoch.fetch_add(1, std::memory_order_acq_rel); // StateFilter: a new D3D thread starts afresh.
    g_gamePresenting.store(true, std::memory_order_release); // SwitchPresentOnRenderThread: the game renders now.
#endif
    __imp__sub_824ECA00(ctx, base);
}

static ankerl::unordered_dense::map<RenderTexture*, RenderTextureLayout> g_barrierMap;

static void AddBarrier(GuestBaseTexture* texture, RenderTextureLayout layout)
{
    if (texture != nullptr && texture->layout != layout)
    {
        g_barrierMap[texture->texture] = layout;
        texture->layout = layout;
    }
}

static std::vector<RenderTextureBarrier> g_barriers;

static void FlushBarriers()
{
    if (!g_barrierMap.empty())
    {
        for (auto& [texture, layout] : g_barrierMap)
            g_barriers.emplace_back(texture, layout);

        g_commandLists[g_frame]->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY, g_barriers);
#if defined(__SWITCH__)
        g_profilerBarrierBatches++;
        FrameLogBarriers(g_barriers.size());
#endif

        g_barrierMap.clear();
        g_barriers.clear();
    }
}

static std::unique_ptr<uint8_t[]> g_shaderCache;
static std::unique_ptr<uint8_t[]> g_buttonBcDiff;

static void LoadEmbeddedResources()
{
    if (g_vulkan)
    {
        g_shaderCache = std::make_unique<uint8_t[]>(g_spirvCacheDecompressedSize);
        ZSTD_decompress(g_shaderCache.get(), g_spirvCacheDecompressedSize, g_compressedSpirvCache, g_spirvCacheCompressedSize);
    }
#ifdef UNLEASHED_RECOMP_D3D12
    else
    {
        g_shaderCache = std::make_unique<uint8_t[]>(g_dxilCacheDecompressedSize);
        ZSTD_decompress(g_shaderCache.get(), g_dxilCacheDecompressedSize, g_compressedDxilCache, g_dxilCacheCompressedSize);
    }
#endif

    g_buttonBcDiff = decompressZstd(g_button_bc_diff, g_button_bc_diff_uncompressed_size);
}

enum class CsdFilterState
{
    Unknown,
    On,
    Off
};

static CsdFilterState g_csdFilterState;

static ankerl::unordered_dense::set<GuestSurface*> g_pendingSurfaceCopies;
static ankerl::unordered_dense::set<GuestSurface*> g_pendingMsaaResolves;

enum class RenderCommandType
{
    SetRenderState,
    DestructResource,
    UnlockTextureRect,
    UnlockBuffer16,
    UnlockBuffer32,
    DrawImGui,
    ExecuteCommandList,
    BeginCommandList,
    StretchRect,
    SetRenderTarget,
    SetDepthStencilSurface,
    ExecutePendingStretchRectCommands,
    Clear,
    SetViewport,
    SetTexture,
    SetScissorRect,
    SetSamplerState,
    SetBooleans,
    SetVertexShaderConstants,
    SetPixelShaderConstants,
    AddPipeline,
    DrawPrimitive,
    DrawIndexedPrimitive,
    DrawPrimitiveUP,
    SetVertexDeclaration,
    SetVertexShader,
    SetStreamSource,
    SetIndices,
    SetPixelShader,
#if defined(__SWITCH__)
    ExecuteCommandBatch,
#endif
};

#if defined(__SWITCH__)
struct RenderCommandBatch;
#endif

struct RenderCommand
{
    RenderCommandType type;
    union
    {
#if defined(__SWITCH__)
        struct
        {
            RenderCommandBatch* batch;
            uint32_t count;
        } executeCommandBatch;

        struct
        {
            // SwitchPresentOnRenderThread: the render thread presents and starts the next frame itself.
            bool pipelined;
        } executeCommandList;
#endif

        struct
        {
            GuestRenderState type;
            uint32_t value;
        } setRenderState;

        struct 
        {
            GuestResource* resource;
        } destructResource;

        struct
        {
            GuestTexture* texture;
        } unlockTextureRect;

        struct
        {
            GuestBuffer* buffer;
        } unlockBuffer;

        struct 
        {
            GuestDevice* device;
            uint32_t flags;
            GuestTexture* texture;
        } stretchRect;

        struct 
        {
            GuestSurface* renderTarget;
        } setRenderTarget;

        struct 
        {
            GuestSurface* depthStencil;
        } setDepthStencilSurface;

        struct 
        {
            uint32_t flags;
            float color[4];
            float z;
        } clear;

        struct 
        {
            float x;
            float y;
            float width;
            float height;
            float minDepth;
            float maxDepth;
        } setViewport;

        struct 
        {
            uint32_t index;
            GuestTexture* texture;
        } setTexture;

        struct 
        {
            int32_t left;
            int32_t top;
            int32_t right;
            int32_t bottom;
        } setScissorRect;

        struct
        {
            uint32_t index;
            uint32_t data0;
            uint32_t data3;
            uint32_t data5;
        } setSamplerState;

        struct
        {
            uint32_t booleans;
        } setBooleans;

        struct
        {
            uint8_t* memory;
            uint32_t index;
            uint32_t size;
        } setVertexShaderConstants;  
        
        struct
        {
            uint8_t* memory;
            uint32_t index;
            uint32_t size;
        } setPixelShaderConstants;

        struct
        {
            XXH64_hash_t hash;
            RenderPipeline* pipeline;
        } addPipeline;

        struct 
        {
            uint32_t primitiveType; 
            uint32_t startVertex; 
            uint32_t primitiveCount;
        } drawPrimitive;

        struct 
        {
            uint32_t primitiveType;
            int32_t baseVertexIndex; 
            uint32_t startIndex;
            uint32_t primCount;
        } drawIndexedPrimitive;

        struct 
        {
            uint32_t primitiveType;
            uint32_t primitiveCount; 
            uint8_t* vertexStreamZeroData;
            uint32_t vertexStreamZeroSize;
            uint32_t vertexStreamZeroStride;
            CsdFilterState csdFilterState;
        } drawPrimitiveUP;

        struct 
        {
            GuestVertexDeclaration* vertexDeclaration;
        } setVertexDeclaration;

        struct 
        {
            GuestShader* shader;
        } setVertexShader;

        struct 
        {
            uint32_t index;
            GuestBuffer* buffer;
            uint32_t offset;
            uint32_t stride;
        } setStreamSource;

        struct 
        {
            GuestBuffer* buffer;
        } setIndices;

        struct 
        {
            GuestShader* shader;
        } setPixelShader;
    };
};

#if defined(__SWITCH__)
// moodycamel spins 10,000 times (tens of microseconds) before sleeping whenever the render thread
// catches up with the game thread. At priority 0x2D that spin is not time-sliced, so it would keep
// guest worker threads off that core; spin ~1,000 times (a few microseconds) instead.
struct RenderQueueTraits : moodycamel::ConcurrentQueueDefaultTraits
{
    static const int MAX_SEMA_SPINS = 1000;
};

static moodycamel::BlockingConcurrentQueue<RenderCommand, RenderQueueTraits> g_renderQueue;
#else
static moodycamel::BlockingConcurrentQueue<RenderCommand> g_renderQueue;
#endif

// Commands from the D3D thread (the present thread) are gathered and handed to the render thread
// in one bulk enqueue per draw, per flush point or when the batch is full, instead of one enqueue
// per state change. Every enqueue can wake the render thread (a kernel signal on the game thread
// plus a context switch) when it went to sleep waiting for work, which happens between most draws.
// Order is unchanged: everything this thread sends goes through the same batch. Other threads
// (pipeline compilation, resource destruction on loaders) enqueue directly, as before; their order
// relative to the D3D thread was never defined.
#if defined(__SWITCH__)
constexpr uint32_t RENDER_COMMAND_BATCH_SIZE = 512;

// [Switch] SwitchZeroCopyBatches (round 8). A batch is handed to the render thread as one command naming its
// buffer (ExecuteCommandBatch), which the render thread runs in place and then gives back, instead of being
// copied command by command into the queue and out of it again. The D3D thread then fills another buffer of
// the pool; when none is free (the render thread far behind), it copies the batch into the queue as before.
// Only the D3D thread takes buffers and only the render thread gives them back: a single-producer,
// single-consumer ring of free buffers.
struct RenderCommandBatch
{
    RenderCommand commands[RENDER_COMMAND_BATCH_SIZE];
};

constexpr uint32_t RENDER_COMMAND_BATCH_POOL = 32;

struct RenderCommandBatchPool
{
    RenderCommandBatch batches[RENDER_COMMAND_BATCH_POOL];
    RenderCommandBatch* free[RENDER_COMMAND_BATCH_POOL + 1];
    std::atomic<uint32_t> head{ 0 }; // Next to take (D3D thread).
    std::atomic<uint32_t> tail{ 0 }; // Next to give back (render thread).

    RenderCommandBatchPool()
    {
        for (uint32_t i = 0; i < RENDER_COMMAND_BATCH_POOL; i++)
            free[i] = &batches[i];
        tail.store(RENDER_COMMAND_BATCH_POOL, std::memory_order_release);
    }

    RenderCommandBatch* Take()
    {
        const uint32_t index = head.load(std::memory_order_relaxed);
        if (index == tail.load(std::memory_order_acquire))
            return nullptr;

        RenderCommandBatch* batch = free[index % std::size(free)];
        head.store(index + 1, std::memory_order_release);
        return batch;
    }

    void GiveBack(RenderCommandBatch* batch)
    {
        const uint32_t index = tail.load(std::memory_order_relaxed);
        free[index % std::size(free)] = batch;
        tail.store(index + 1, std::memory_order_release);
    }
};

static RenderCommandBatchPool g_renderCommandBatchPool;
static bool g_zeroCopyBatches = false;

// [Switch] Round 9, SwitchSkipDeadCopies. What the render thread has taken from the queue but not processed
// yet, in order: the rest of the batch it is in (the command being processed is batch[batchNext - 1]), then
// window[head, tail) (batches in it still to be expanded). The render loop processes from here; the
// look-ahead below reads it, and may take more commands from the queue into it, but never processes any.
struct RenderLookahead
{
    static constexpr size_t CAPACITY = 512;
    RenderCommand window[CAPACITY];
    size_t head = 0;
    size_t tail = 0;
    const RenderCommand* batch = nullptr;
    uint32_t batchNext = 0;
    uint32_t batchCount = 0;
};

static RenderLookahead g_lookahead;

// Takes the commands already in the queue into the window without waiting, or waits up to `timeoutUs` for some.
static bool TakeMoreRenderCommands(int64_t timeoutUs)
{
    auto& lookahead = g_lookahead;
    if (lookahead.tail == RenderLookahead::CAPACITY)
    {
        if (lookahead.head == 0)
            return false;

        std::memmove(lookahead.window, lookahead.window + lookahead.head, (lookahead.tail - lookahead.head) * sizeof(RenderCommand));
        lookahead.tail -= lookahead.head;
        lookahead.head = 0;
    }

    const size_t room = RenderLookahead::CAPACITY - lookahead.tail;
    const size_t count = timeoutUs > 0 ?
        g_renderQueue.wait_dequeue_bulk_timed(lookahead.window + lookahead.tail, room, timeoutUs) :
        g_renderQueue.try_dequeue_bulk(lookahead.window + lookahead.tail, room);

    lookahead.tail += count;
    g_renderCommandsTaken.store(g_renderCommandsTaken.load(std::memory_order_relaxed) + count, std::memory_order_relaxed);
    return count != 0;
}

// After which the D3D thread may wait for the render thread (Present) or relies on it being processed soon
// (an unlocked buffer or texture it may write again): the look-ahead never waits for more commands past one.
static bool RenderThreadIsAwaited(RenderCommandType type)
{
    switch (type)
    {
    case RenderCommandType::UnlockTextureRect:
    case RenderCommandType::UnlockBuffer16:
    case RenderCommandType::UnlockBuffer32:
    case RenderCommandType::DrawImGui:
    case RenderCommandType::ExecuteCommandList:
    case RenderCommandType::BeginCommandList:
    case RenderCommandType::ExecutePendingStretchRectCommands:
        return true;
    default:
        return false;
    }
}

enum class TextureFuture
{
    READ,    // something may read its contents first
    DEAD,    // resolved into again (every texel rewritten) or destroyed before anything can read it
    UNKNOWN, // neither, as far as the commands taken so far go
};

static bool g_skipDeadCopies = false;
static uint32_t g_deadCopyWaitBudgetUs = 0; // left for this frame

// Waiting for more commands only for copies this large (a 1920x1080 RGBA8 copy is ~8 MB and ~0.45 ms of GPU
// time; at low resolutions, where the CPU limits the frame, the render thread never waits), for at most
// this long per copy and per frame.
static constexpr uint64_t DEAD_COPY_WAIT_MIN_BYTES = 4ull << 20;
static constexpr uint32_t DEAD_COPY_WAIT_MAX_US = 1000;
static constexpr uint32_t DEAD_COPY_WAIT_FRAME_US = 2000;
static constexpr uint32_t DEAD_COPY_SCAN_MAX_COMMANDS = 4096;

// What happens first to `texture`'s contents, from the draw being flushed on, in the commands the render thread
// has taken: a draw while it is in a texture slot (the draw being flushed included) or an update of it from the
// CPU (READ); a resolve into it, which rewrites every texel, or its destruction (DEAD). When the commands run
// out before either, more are taken from the queue; the render thread waits for them (up to `waitUs`, which is
// reduced by the time waited) only until it has taken a command the D3D thread may wait for.
static TextureFuture ScanTextureFuture(const GuestTexture* texture, uint32_t& waitUs)
{
    uint32_t bound = 0;
    for (uint32_t i = 0; i < std::size(g_textures); i++)
    {
        if (g_textures[i] == texture)
            bound |= 1u << i;
    }

    if (bound != 0)
        return TextureFuture::READ;

    bool mayWait = true;
    uint32_t visited = 0;
    auto visit = [&](const RenderCommand& cmd, TextureFuture& future)
        {
            // A bounded search: past this many commands the answer is UNKNOWN (the copy is made).
            if (++visited > DEAD_COPY_SCAN_MAX_COMMANDS)
            {
                future = TextureFuture::UNKNOWN;
                return true;
            }

            switch (cmd.type)
            {
            case RenderCommandType::SetTexture:
                if (cmd.setTexture.index < std::size(g_textures))
                {
                    if (cmd.setTexture.texture == texture)
                        bound |= 1u << cmd.setTexture.index;
                    else
                        bound &= ~(1u << cmd.setTexture.index);
                }
                break;

            case RenderCommandType::DrawPrimitive:
            case RenderCommandType::DrawIndexedPrimitive:
            case RenderCommandType::DrawPrimitiveUP:
                if (bound != 0)
                {
                    future = TextureFuture::READ;
                    return true;
                }
                break;

            case RenderCommandType::StretchRect:
                if (cmd.stretchRect.texture == texture)
                {
                    future = TextureFuture::DEAD;
                    return true;
                }
                break;

            case RenderCommandType::DestructResource:
                if (cmd.destructResource.resource == static_cast<const GuestResource*>(texture))
                {
                    future = TextureFuture::DEAD;
                    return true;
                }
                break;

            case RenderCommandType::UnlockTextureRect:
                if (cmd.unlockTextureRect.texture == texture)
                {
                    future = TextureFuture::READ;
                    return true;
                }
                break;

            default:
                break;
            }

            if (RenderThreadIsAwaited(cmd.type))
                mayWait = false;

            return false;
        };

    TextureFuture future = TextureFuture::UNKNOWN;
    auto& lookahead = g_lookahead;
    if (lookahead.batch != nullptr)
    {
        for (uint32_t k = lookahead.batchNext; k < lookahead.batchCount; k++)
        {
            if (visit(lookahead.batch[k], future))
                return future;
        }
    }

    size_t offset = 0; // From head (which a compaction of the window moves).
    while (true)
    {
        if (lookahead.head + offset == lookahead.tail)
        {
            const uint32_t timeoutUs = mayWait ? waitUs : 0;
            const auto start = std::chrono::steady_clock::now();
            const bool more = TakeMoreRenderCommands(timeoutUs);
            if (timeoutUs != 0)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
                const uint32_t waited = uint32_t(std::clamp<int64_t>(elapsed, 0, timeoutUs));
                waitUs -= waited;
                g_profilerDeadCopyWaitUs += waited;
            }

            if (!more)
                return TextureFuture::UNKNOWN;

            continue;
        }

        const RenderCommand& cmd = lookahead.window[lookahead.head + offset];
        offset++;

        if (cmd.type == RenderCommandType::ExecuteCommandBatch)
        {
            const auto& args = cmd.executeCommandBatch;
            for (uint32_t k = 0; k < args.count; k++)
            {
                if (visit(args.batch->commands[k], future))
                    return future;
            }
            continue;
        }

        if (visit(cmd, future))
            return future;
    }
}

// Before `surface` is written (a draw into it or a clear): its pending resolves whose textures are dead
// (ScanTextureFuture) are dropped instead of copied. Every texel of such a texture is written again by a resolve
// into it, or the texture is destroyed, before anything can read it, so no read ever sees what the copy would
// have written; until then the texture keeps its old image, which nothing reads.
static void DropDeadPendingResolves(GuestSurface* surface)
{
    if (!g_skipDeadCopies || surface == nullptr || surface->destinationTextures.empty())
        return;

    GuestTexture* dead[8];
    uint32_t deadCount = 0;
    for (GuestTexture* texture : surface->destinationTextures)
    {
        if (deadCount == std::size(dead))
            break;

        const uint64_t bytes = uint64_t(texture->width) * texture->height * RenderFormatSize(texture->format);
        uint32_t waitUs = bytes >= DEAD_COPY_WAIT_MIN_BYTES ? std::min(g_deadCopyWaitBudgetUs, DEAD_COPY_WAIT_MAX_US) : 0;
        const uint32_t budget = waitUs;
        const TextureFuture future = ScanTextureFuture(texture, waitUs);
        g_deadCopyWaitBudgetUs -= std::min(g_deadCopyWaitBudgetUs, budget - waitUs);

        if (future == TextureFuture::DEAD)
            dead[deadCount++] = texture;
        else if (future == TextureFuture::READ)
            g_profilerDeadCopiesRead++;
        else
            g_profilerDeadCopiesUnknown++;
    }

    for (uint32_t i = 0; i < deadCount; i++)
    {
        GuestTexture* texture = dead[i];
        FrameLogDeadCopy(surface, texture);
        g_profilerDeadCopiesSkipped++;
        texture->sourceSurface = nullptr;
        texture->pendingCarried = false;
        surface->destinationTextures.erase(texture);
    }
}
#endif

struct DeferredRenderCommands
{
#if defined(__SWITCH__)
    // [Switch] SwitchLargerCommandBatches uses all of it; without it only the first 128 (RenderCommandBatchCapacity).
    // With SwitchZeroCopyBatches, `commands` is a buffer of the pool (`batch`) instead of `storage`.
    RenderCommand storage[RENDER_COMMAND_BATCH_SIZE];
    RenderCommand* commands = storage;
    RenderCommandBatch* batch = nullptr;
#else
    RenderCommand commands[128];
#endif
    uint32_t count = 0;
};

static DeferredRenderCommands g_deferredRenderCommands;

#if defined(__SWITCH__)
// [Switch] SwitchRenderQueueToken: the D3D thread's batches go through a producer of their own instead of
// the queue's per-thread implicit producers, which moodycamel finds by a hash lookup of the thread on every
// enqueue. Only this thread's batches use it (FlushDeferredRenderCommands; a change of D3D thread flushes
// first), so they stay in order, also across that change.
static moodycamel::ProducerToken g_renderQueueToken(g_renderQueue);
static bool g_renderQueueTokenEnabled = false;

// [Switch] SwitchLargerCommandBatches: a batch holds up to 256 commands and goes out once it has 128 at a
// draw, instead of 128 and 64 (LocalRenderCommandQueue::submit): half the hand-overs to the render thread.
static uint32_t g_renderCommandBatchCapacity = 128;
static uint32_t g_renderCommandBatchThreshold = 64;

// [Switch] Round 9, SwitchIdleRenderThreadBatches: a batch holds up to 512 commands. While the render thread waits
// for work (g_renderThreadWaiting, set around its wait), every hand-over wakes it with a system call made on the
// D3D thread; the batch then goes out only when nearly full. While it works, at the usual 128 commands. The order
// and content of the commands are the same; only when they reach the render thread changes.
static bool g_idleRenderThreadBatches = false;
static std::atomic<bool> g_renderThreadWaiting{ false };

static uint32_t RenderCommandBatchCapacity()
{
    return g_renderCommandBatchCapacity;
}

// Commands a batch collects before it goes out at a draw (SwitchBatchSeveralDraws).
static uint32_t RenderCommandBatchThreshold()
{
    if (g_idleRenderThreadBatches && g_renderThreadWaiting.load(std::memory_order_relaxed))
        return g_renderCommandBatchCapacity - 64;

    return g_renderCommandBatchThreshold;
}
#else
static constexpr uint32_t RenderCommandBatchCapacity()
{
    return uint32_t(std::size(DeferredRenderCommands::commands));
}
#endif

static void FlushDeferredRenderCommands()
{
    auto& deferred = g_deferredRenderCommands;
    if (deferred.count != 0)
    {
#if defined(__SWITCH__)
        // SwitchZeroCopyBatches: the buffer itself goes to the render thread, the D3D thread takes the next one.
        if (deferred.batch != nullptr)
        {
            if (RenderCommandBatch* next = g_renderCommandBatchPool.Take())
            {
                RenderCommand cmd;
                cmd.type = RenderCommandType::ExecuteCommandBatch;
                cmd.executeCommandBatch.batch = deferred.batch;
                cmd.executeCommandBatch.count = deferred.count;
                if (g_renderQueueTokenEnabled)
                    g_renderQueue.enqueue(g_renderQueueToken, cmd);
                else
                    g_renderQueue.enqueue(cmd);

                deferred.batch = next;
                deferred.commands = next->commands;
                deferred.count = 0;
                return;
            }
        }

        if (g_renderQueueTokenEnabled)
            g_renderQueue.enqueue_bulk(g_renderQueueToken, deferred.commands, deferred.count);
        else
#endif
        g_renderQueue.enqueue_bulk(deferred.commands, deferred.count);
        deferred.count = 0;
    }

#if defined(__SWITCH__)
    // The first buffer (or one again after the pool ran out while in the storage).
    if (g_zeroCopyBatches && deferred.batch == nullptr)
    {
        if (RenderCommandBatch* batch = g_renderCommandBatchPool.Take())
        {
            deferred.batch = batch;
            deferred.commands = batch->commands;
        }
    }
#endif
}

static bool ShouldBatchRenderCommands()
{
#if defined(__SWITCH__)
    return Config::SwitchBatchRenderCommands && IsPresentThread();
#else
    return false;
#endif
}

#if defined(__SWITCH__)
// [Switch] SwitchSkipRedundantRenderStates, SwitchSkipRedundantSamplerStates. The game sets most render and
// sampler states before every draw, mostly to the values they already have. The render thread applies each
// one as an assignment (ProcSetRenderState, ProcSetSamplerState; the same value again changes nothing) and
// nothing else changes what they set, so the D3D thread remembers the last value it sent of each and does
// not send it again. Not D3DRS_ALPHATESTENABLE, whose effect depends on the render target at the time, and
// sampler states only while the anisotropic filtering setting they were converted with stays the same. A
// state sent by another thread (the queue does not order it against this thread's) or a change of D3D
// thread makes it forget everything, so the next value of each state is sent again.
struct StateFilter
{
    uint32_t epoch = ~0u;
    uint32_t renderStates[256];
    uint64_t renderStatesKnown[4]{};
    uint32_t samplerStates[16][3];
    uint32_t samplerStatesKnown = 0;
    uint32_t anisotropicFiltering = 0;
};

static StateFilter g_stateFilter; // D3D thread only.

// Whether the filter may be used on this thread; brings it up to date. Other threads make it forget.
static bool EnterStateFilter()
{
    if (!IsPresentThread())
    {
        g_stateFilterEpoch.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }

    auto& filter = g_stateFilter;
    const uint32_t epoch = g_stateFilterEpoch.load(std::memory_order_acquire);
    const uint32_t anisotropicFiltering = Config::AnisotropicFiltering;
    if (filter.epoch != epoch)
    {
        filter.epoch = epoch;
        std::fill(std::begin(filter.renderStatesKnown), std::end(filter.renderStatesKnown), 0);
        filter.samplerStatesKnown = 0;
    }

    if (filter.anisotropicFiltering != anisotropicFiltering)
    {
        filter.anisotropicFiltering = anisotropicFiltering;
        filter.samplerStatesKnown = 0;
    }

    return true;
}

// True when the render thread already has `value` for render state `type` (the command is not needed).
static bool RenderStateUnchanged(uint32_t type, uint32_t value)
{
    if (!EnterStateFilter() || type >= std::size(g_stateFilter.renderStates))
        return false;

    auto& filter = g_stateFilter;
    uint64_t& known = filter.renderStatesKnown[type / 64];
    const uint64_t bit = uint64_t(1) << (type % 64);
    if ((known & bit) != 0 && filter.renderStates[type] == value)
    {
        AddGameThreadCount(g_profilerStatesSkipped, 1);
        return true;
    }

    known |= bit;
    filter.renderStates[type] = value;
    return false;
}
#endif

// flush: the command must reach the render thread now (the caller waits for it, or it reads memory
// the game may change right after this call).
static void EnqueueRenderCommand(const RenderCommand& cmd, bool flush = false)
{
    if (ShouldBatchRenderCommands())
    {
        auto& batch = g_deferredRenderCommands;
        if (batch.count == RenderCommandBatchCapacity())
            FlushDeferredRenderCommands();

        batch.commands[batch.count++] = cmd;

        if (flush)
            FlushDeferredRenderCommands();
    }
    else
    {
        g_renderQueue.enqueue(cmd);
    }
}

template<GuestRenderState TType>
static void SetRenderState(GuestDevice* device, uint32_t value)
{
#if defined(__SWITCH__)
    if (TType != D3DRS_ALPHATESTENABLE && g_skipRedundantRenderStates && RenderStateUnchanged(TType, value))
        return;
#endif

    RenderCommand cmd;
    cmd.type = RenderCommandType::SetRenderState;
    cmd.setRenderState.type = TType;
    cmd.setRenderState.value = value;
    EnqueueRenderCommand(cmd);
}

static void SetRenderStateUnimplemented(GuestDevice* device, uint32_t value)
{
}

static void SetAlphaTestMode(bool enable)
{
    uint32_t specConstants = 0;
    bool enableAlphaToCoverage = false;

    if (enable)
    {
        enableAlphaToCoverage = Config::TransparencyAntiAliasing && g_renderTarget != nullptr && g_renderTarget->sampleCount != RenderSampleCount::COUNT_1;

        if (enableAlphaToCoverage)
            specConstants = SPEC_CONSTANT_ALPHA_TO_COVERAGE;
        else
            specConstants = SPEC_CONSTANT_ALPHA_TEST;
    }

    specConstants |= (g_pipelineState.specConstants & ~(SPEC_CONSTANT_ALPHA_TEST | SPEC_CONSTANT_ALPHA_TO_COVERAGE));

    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.enableAlphaToCoverage, enableAlphaToCoverage);
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.specConstants, specConstants);
}

static RenderBlend ConvertBlendMode(uint32_t blendMode)
{
    switch (blendMode)
    {
    case D3DBLEND_ZERO:
        return RenderBlend::ZERO;
    case D3DBLEND_ONE:
        return RenderBlend::ONE;
    case D3DBLEND_SRCCOLOR:
        return RenderBlend::SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:
        return RenderBlend::INV_SRC_COLOR;
    case D3DBLEND_SRCALPHA:
        return RenderBlend::SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:
        return RenderBlend::INV_SRC_ALPHA;
    case D3DBLEND_DESTCOLOR:
        return RenderBlend::DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR:
        return RenderBlend::INV_DEST_COLOR;
    case D3DBLEND_DESTALPHA:
        return RenderBlend::DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA:
        return RenderBlend::INV_DEST_ALPHA;
    default:
        assert(false && "Invalid blend mode");
        return RenderBlend::ZERO;
    }
}

static RenderBlendOperation ConvertBlendOp(uint32_t blendOp)
{
    switch (blendOp)
    {
    case D3DBLENDOP_ADD:
        return RenderBlendOperation::ADD;
    case D3DBLENDOP_SUBTRACT:
        return RenderBlendOperation::SUBTRACT;
    case D3DBLENDOP_REVSUBTRACT:
        return RenderBlendOperation::REV_SUBTRACT;
    case D3DBLENDOP_MIN:
        return RenderBlendOperation::MIN;
    case D3DBLENDOP_MAX:
        return RenderBlendOperation::MAX;
    default:
        assert(false && "Unknown blend operation");
        return RenderBlendOperation::ADD;
    }
}

static void ProcSetRenderState(const RenderCommand& cmd)
{
    uint32_t value = cmd.setRenderState.value;

    switch (cmd.setRenderState.type)
    {
    case D3DRS_ZENABLE:
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zEnable, value != 0);
        g_dirtyStates.renderTargetAndDepthStencil |= g_dirtyStates.pipelineState;
        break;
    }
    case D3DRS_ZWRITEENABLE:
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zWriteEnable, value != 0);
        break;
    }
    case D3DRS_ALPHATESTENABLE:
    {
        SetAlphaTestMode(value != 0);
        break;
    }
    case D3DRS_SRCBLEND:
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.srcBlend, ConvertBlendMode(value));
        break;
    }
    case D3DRS_DESTBLEND:
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.destBlend, ConvertBlendMode(value));
        break;
    }
    case D3DRS_CULLMODE:
    {
        RenderCullMode cullMode;

        switch (value) {
        case D3DCULL_NONE:
        case D3DCULL_NONE_2:
            cullMode = RenderCullMode::NONE;
            break;
        case D3DCULL_CW:
            cullMode = RenderCullMode::FRONT;
            break;
        case D3DCULL_CCW:
            cullMode = RenderCullMode::BACK;
            break;
        default:
            assert(false && "Invalid cull mode");
            cullMode = RenderCullMode::NONE;
            break;
        }

        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.cullMode, cullMode);
        break;
    }
    case D3DRS_ZFUNC:
    {
        RenderComparisonFunction comparisonFunc;

        switch (value)
        {
        case D3DCMP_NEVER:
            comparisonFunc = RenderComparisonFunction::NEVER;
            break;
        case D3DCMP_LESS:
            comparisonFunc = RenderComparisonFunction::LESS;
            break;
        case D3DCMP_EQUAL:
            comparisonFunc = RenderComparisonFunction::EQUAL;
            break;
        case D3DCMP_LESSEQUAL:
            comparisonFunc = RenderComparisonFunction::LESS_EQUAL;
            break;
        case D3DCMP_GREATER:
            comparisonFunc = RenderComparisonFunction::GREATER;
            break;
        case D3DCMP_NOTEQUAL:
            comparisonFunc = RenderComparisonFunction::NOT_EQUAL;
            break;
        case D3DCMP_GREATEREQUAL:
            comparisonFunc = RenderComparisonFunction::GREATER_EQUAL;
            break;
        case D3DCMP_ALWAYS:
            comparisonFunc = RenderComparisonFunction::ALWAYS;
            break;
        default:
            assert(false && "Unknown comparison function");
            comparisonFunc = RenderComparisonFunction::NEVER;
            break;
        }

        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.zFunc, comparisonFunc);
        break;
    }
    case D3DRS_ALPHAREF:
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_sharedConstants.alphaThreshold, float(value) / 256.0f);
        break;
    }
    case D3DRS_ALPHABLENDENABLE:
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.alphaBlendEnable, value != 0);
        break;
    }
    case D3DRS_BLENDOP:
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.blendOp, ConvertBlendOp(value));
        break;
    }
    case D3DRS_SCISSORTESTENABLE:
    {
        SetDirtyValue(g_dirtyStates.scissorRect, g_scissorTestEnable, value != 0);
        break;
    }
    case D3DRS_SLOPESCALEDEPTHBIAS:
    {
        if (g_capabilities.dynamicDepthBias)
            SetDirtyValue(g_dirtyStates.depthBias, g_slopeScaledDepthBias, *reinterpret_cast<float*>(&value));
        else 
            SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.slopeScaledDepthBias, *reinterpret_cast<float*>(&value));

        break;
    }
    case D3DRS_DEPTHBIAS:
    {
        if (g_capabilities.dynamicDepthBias)
            SetDirtyValue(g_dirtyStates.depthBias, g_depthBias, int32_t(*reinterpret_cast<float*>(&value) * (1 << 24)));
        else
            SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.depthBias, int32_t(*reinterpret_cast<float*>(&value)* (1 << 24)));

        break;
    }
    case D3DRS_SRCBLENDALPHA:
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.srcBlendAlpha, ConvertBlendMode(value));
        break;
    }
    case D3DRS_DESTBLENDALPHA:
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.destBlendAlpha, ConvertBlendMode(value));
        break;
    }
    case D3DRS_BLENDOPALPHA:
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.blendOpAlpha, ConvertBlendOp(value));
        break;
    }
    case D3DRS_COLORWRITEENABLE:
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.colorWriteEnable, value);
        g_dirtyStates.renderTargetAndDepthStencil |= g_dirtyStates.pipelineState;
        break;
    }
    }
}

static const std::pair<GuestRenderState, PPCFunc*> g_setRenderStateFunctions[] =
{
    { D3DRS_ZENABLE, HostToGuestFunction<SetRenderState<D3DRS_ZENABLE>> },
    { D3DRS_ZWRITEENABLE, HostToGuestFunction<SetRenderState<D3DRS_ZWRITEENABLE>> },
    { D3DRS_ALPHATESTENABLE, HostToGuestFunction<SetRenderState<D3DRS_ALPHATESTENABLE>> },
    { D3DRS_SRCBLEND, HostToGuestFunction<SetRenderState<D3DRS_SRCBLEND>> },
    { D3DRS_DESTBLEND, HostToGuestFunction<SetRenderState<D3DRS_DESTBLEND>> },
    { D3DRS_CULLMODE, HostToGuestFunction<SetRenderState<D3DRS_CULLMODE>> },
    { D3DRS_ZFUNC, HostToGuestFunction<SetRenderState<D3DRS_ZFUNC>> },
    { D3DRS_ALPHAREF, HostToGuestFunction<SetRenderState<D3DRS_ALPHAREF>> },
    { D3DRS_ALPHABLENDENABLE, HostToGuestFunction<SetRenderState<D3DRS_ALPHABLENDENABLE>> },
    { D3DRS_BLENDOP, HostToGuestFunction<SetRenderState<D3DRS_BLENDOP>> },
    { D3DRS_SCISSORTESTENABLE, HostToGuestFunction<SetRenderState<D3DRS_SCISSORTESTENABLE>> },
    { D3DRS_SLOPESCALEDEPTHBIAS, HostToGuestFunction<SetRenderState<D3DRS_SLOPESCALEDEPTHBIAS>> },
    { D3DRS_DEPTHBIAS, HostToGuestFunction<SetRenderState<D3DRS_DEPTHBIAS>> },
    { D3DRS_SRCBLENDALPHA, HostToGuestFunction<SetRenderState<D3DRS_SRCBLENDALPHA>> },
    { D3DRS_DESTBLENDALPHA, HostToGuestFunction<SetRenderState<D3DRS_DESTBLENDALPHA>> },
    { D3DRS_BLENDOPALPHA, HostToGuestFunction<SetRenderState<D3DRS_BLENDOPALPHA>> },
    { D3DRS_COLORWRITEENABLE, HostToGuestFunction<SetRenderState<D3DRS_COLORWRITEENABLE>> }
};

static std::unique_ptr<RenderShader> g_copyShader;

static std::unique_ptr<RenderShader> g_copyColorShader;
static ankerl::unordered_dense::map<RenderFormat, std::unique_ptr<RenderPipeline>> g_copyColorPipelines;
static std::unique_ptr<RenderPipeline> g_copyDepthPipeline;

static std::unique_ptr<RenderShader> g_resolveMsaaColorShaders[3];
static ankerl::unordered_dense::map<RenderFormat, std::array<std::unique_ptr<RenderPipeline>, 3>> g_resolveMsaaColorPipelines;
static std::unique_ptr<RenderPipeline> g_resolveMsaaDepthPipelines[3];

enum
{
    GAUSSIAN_BLUR_3X3,
    GAUSSIAN_BLUR_5X5,
    GAUSSIAN_BLUR_7X7,
    GAUSSIAN_BLUR_9X9,
    GAUSSIAN_BLUR_COUNT
};

static std::unique_ptr<GuestShader> g_gaussianBlurShaders[GAUSSIAN_BLUR_COUNT];

static std::unique_ptr<GuestShader> g_csdFilterShader;
static GuestShader* g_csdShader;

static std::unique_ptr<GuestShader> g_enhancedMotionBlurShader;

#ifdef UNLEASHED_RECOMP_D3D12

#define CREATE_SHADER(NAME) \
    g_device->createShader( \
        g_vulkan ? g_##NAME##_spirv : g_##NAME##_dxil, \
        g_vulkan ? sizeof(g_##NAME##_spirv) : sizeof(g_##NAME##_dxil), \
        "main", \
        g_vulkan ? RenderShaderFormat::SPIRV : RenderShaderFormat::DXIL)

#else

#define CREATE_SHADER(NAME) \
    g_device->createShader(g_##NAME##_spirv, sizeof(g_##NAME##_spirv), "main", RenderShaderFormat::SPIRV)

#endif

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
static bool SpirvReadsConstantsThroughUbo(const uint8_t* data, size_t size);

// Hand-written shaders that replace game shaders: draws with them skip the push-constant pointers when
// the SPIR-V reads its constants through set 4, like the translated shaders.
#define MARK_CONSTANTS_THROUGH_UBO(GUEST_SHADER, NAME) \
    (GUEST_SHADER)->constantsThroughUbo.store(SpirvReadsConstantsThroughUbo(g_##NAME##_spirv, sizeof(g_##NAME##_spirv)), std::memory_order_release)
#else
#define MARK_CONSTANTS_THROUGH_UBO(GUEST_SHADER, NAME)
#endif

#ifdef _WIN32
static bool DetectWine()
{
    HMODULE dllHandle = GetModuleHandle("ntdll.dll");
    return dllHandle != nullptr && GetProcAddress(dllHandle, "wine_get_version") != nullptr;
}
#endif

static constexpr size_t TEXTURE_DESCRIPTOR_SIZE = 65536;

// The heap size the descriptor set layouts are created with.
static uint32_t g_textureDescriptorCount = TEXTURE_DESCRIPTOR_SIZE;

#if defined(__SWITCH__)
// [Switch] SwitchCompactTextureHeap: 16,384 entries, 64 KB with NVK's 4-byte descriptors, which is the
// most NVK reads from a hardware constant bank (NVK_MAX_CBUF_SIZE). With 65,536 entries every texture
// fetch first loads its descriptor from memory. Measured at the hub: GPU frame 22.65 -> 21.30 ms; play
// sessions used fewer than 2,048 descriptors. A texture that finds the heap full gets the null 2D
// descriptor (logged once), so it would render black.
static constexpr uint32_t TEXTURE_DESCRIPTOR_COMPACT_SIZE = 16384;

// Set once the null descriptors exist; later writes to their indices (a texture that found the heap
// full) are dropped instead of replacing them.
static bool g_nullTextureDescriptorsWritten;
#endif

// Size of the texture written to each texture descriptor. Every view the renderer creates starts at
// mip 0, so this is exactly what GetDimensions() returns for that descriptor in a shader. All
// descriptor writes go through SetTextureDescriptor so the table cannot go stale; entries are
// written before the descriptor index is handed to the render thread.
static float g_textureDescriptorSizes[TEXTURE_DESCRIPTOR_SIZE][2];

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
// Whether the 2D view of each texture descriptor has a single mip level and power-of-two sides: point
// fetches half a texel (or one texel) apart then land on exactly the texels one gather returns, since
// the offsets divided by the size are exact. 0 mip levels = not known, never gathered.
static bool g_textureDescriptorGatherable[TEXTURE_DESCRIPTOR_SIZE];
#endif

template<typename TDescriptorSet>
static void SetTextureDescriptor(TDescriptorSet& descriptorSet, uint32_t descriptorIndex, const RenderTexture* texture,
    uint32_t width, uint32_t height, RenderTextureLayout layout, const RenderTextureView* textureView = nullptr, uint32_t mipLevels = 0)
{
#if defined(__SWITCH__)
    if (descriptorIndex < TEXTURE_DESCRIPTOR_NULL_COUNT && g_nullTextureDescriptorsWritten)
        return;
#endif

    if (descriptorIndex < TEXTURE_DESCRIPTOR_SIZE)
    {
        g_textureDescriptorSizes[descriptorIndex][0] = float(width);
        g_textureDescriptorSizes[descriptorIndex][1] = float(height);
#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
        g_textureDescriptorGatherable[descriptorIndex] = mipLevels == 1 && width != 0 && height != 0 &&
            (width & (width - 1)) == 0 && (height & (height - 1)) == 0;
#endif
    }

    descriptorSet->setTexture(descriptorIndex, texture, layout, textureView);
}
static constexpr size_t SAMPLER_DESCRIPTOR_SIZE = 1024;

static std::unique_ptr<GuestTexture> g_imFontTexture;
static std::unique_ptr<RenderPipelineLayout> g_imPipelineLayout;
static std::unique_ptr<RenderPipeline> g_imPipeline;
static std::unique_ptr<RenderPipeline> g_imAdditivePipeline;

template<typename T>
static void ExecuteCopyCommandList(const T& function)
{
    std::lock_guard lock(g_copyMutex);

    g_copyCommandList->begin();
    function();
    g_copyCommandList->end();
    g_copyQueue->executeCommandLists(g_copyCommandList.get(), g_copyCommandFence.get());
    g_copyQueue->waitForCommandFence(g_copyCommandFence.get());
}

static constexpr uint32_t PITCH_ALIGNMENT = 0x100;
static constexpr uint32_t PLACEMENT_ALIGNMENT = 0x200;

struct ImGuiPushConstants
{
    ImVec2 boundsMin{};
    ImVec2 boundsMax{};
    ImU32 gradientTopLeft{};
    ImU32 gradientTopRight{};
    ImU32 gradientBottomRight{};
    ImU32 gradientBottomLeft{};
    uint32_t shaderModifier{};
    uint32_t texture2DDescriptorIndex{};
    ImVec2 displaySize{};
    ImVec2 inverseDisplaySize{};
    ImVec2 origin{ 0.0f, 0.0f };
    ImVec2 scale{ 1.0f, 1.0f };
    ImVec2 proceduralOrigin{ 0.0f, 0.0f };
    float outline{};
};

extern ImFontBuilderIO g_fontBuilderIO;

static void CreateImGuiBackend()
{
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

#ifdef ENABLE_IM_FONT_ATLAS_SNAPSHOT
    IM_DELETE(io.Fonts);
    io.Fonts = ImFontAtlasSnapshot::Load();
#else
    io.Fonts->AddFontDefault();
    ImFontAtlasSnapshot::GenerateGlyphRanges();
#endif

    InitImGuiUtils();
    AchievementMenu::Init();
    AchievementOverlay::Init();
    ButtonGuide::Init();
    MessageWindow::Init();
    OptionsMenu::Init();
    InstallerWizard::Init();

    ImGui_ImplSDL2_InitForOther(GameWindow::s_pWindow);

#ifdef ENABLE_IM_FONT_ATLAS_SNAPSHOT
    g_imFontTexture = LoadTexture(
        decompressZstd(g_im_font_atlas_texture, g_im_font_atlas_texture_uncompressed_size).get(), g_im_font_atlas_texture_uncompressed_size);
#else
    io.Fonts->FontBuilderIO = &g_fontBuilderIO;
    io.Fonts->Build();

    g_imFontTexture = std::make_unique<GuestTexture>(ResourceType::Texture);

    uint8_t* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    RenderTextureDesc textureDesc;
    textureDesc.dimension = RenderTextureDimension::TEXTURE_2D;
    textureDesc.width = width;
    textureDesc.height = height;
    textureDesc.depth = 1;
    textureDesc.mipLevels = 1;
    textureDesc.arraySize = 1;
    textureDesc.format = RenderFormat::R8G8B8A8_UNORM;

    g_imFontTexture->textureHolder = g_device->createTexture(textureDesc);
    g_imFontTexture->texture = g_imFontTexture->textureHolder.get();

    uint32_t rowPitch = (width * 4 + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
    uint32_t slicePitch = (rowPitch * height + PLACEMENT_ALIGNMENT - 1) & ~(PLACEMENT_ALIGNMENT - 1);
    auto uploadBuffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(slicePitch));
    uint8_t* mappedMemory = reinterpret_cast<uint8_t*>(uploadBuffer->map());

    if (rowPitch == (width * 4))
    {
        memcpy(mappedMemory, pixels, slicePitch);
    }
    else
    {
        for (size_t i = 0; i < height; i++)
        {
            memcpy(mappedMemory, pixels, width * 4);
            pixels += width * 4;
            mappedMemory += rowPitch;
        }
    }

    uploadBuffer->unmap();

    ExecuteCopyCommandList([&]
        {
            g_copyCommandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(g_imFontTexture->texture, RenderTextureLayout::COPY_DEST));

            g_copyCommandList->copyTextureRegion(
                RenderTextureCopyLocation::Subresource(g_imFontTexture->texture, 0),
                RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), RenderFormat::R8G8B8A8_UNORM, width, height, 1, rowPitch / 4, 0));
        });

    g_imFontTexture->layout = RenderTextureLayout::COPY_DEST;

    RenderTextureViewDesc textureViewDesc;
    textureViewDesc.format = textureDesc.format;
    textureViewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
    textureViewDesc.mipLevels = 1;
    g_imFontTexture->textureView = g_imFontTexture->texture->createTextureView(textureViewDesc);

    g_imFontTexture->descriptorIndex = g_textureDescriptorAllocator.allocate();
    SetTextureDescriptor(g_textureDescriptorSet, g_imFontTexture->descriptorIndex, g_imFontTexture->texture, width, height, RenderTextureLayout::SHADER_READ, g_imFontTexture->textureView.get());
#endif

    io.Fonts->SetTexID(g_imFontTexture.get());

    RenderPipelineLayoutBuilder pipelineLayoutBuilder;
    pipelineLayoutBuilder.begin(false, true);

    RenderDescriptorSetBuilder descriptorSetBuilder;
    descriptorSetBuilder.begin();
    descriptorSetBuilder.addTexture(0, g_textureDescriptorCount);
    descriptorSetBuilder.end(true, g_textureDescriptorCount);
    pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);

    descriptorSetBuilder.begin();
    descriptorSetBuilder.addSampler(0, SAMPLER_DESCRIPTOR_SIZE);
    descriptorSetBuilder.end(true, SAMPLER_DESCRIPTOR_SIZE);
    pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);

    pipelineLayoutBuilder.addPushConstant(0, 2, sizeof(ImGuiPushConstants), RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);

    pipelineLayoutBuilder.end();
    g_imPipelineLayout = pipelineLayoutBuilder.create(g_device.get());

    auto vertexShader = CREATE_SHADER(imgui_vs);
    auto pixelShader = CREATE_SHADER(imgui_ps);

    RenderInputElement inputElements[3];
    inputElements[0] = RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, offsetof(ImDrawVert, pos));
    inputElements[1] = RenderInputElement("TEXCOORD", 0, 1, RenderFormat::R32G32_FLOAT, 0, offsetof(ImDrawVert, uv));
    inputElements[2] = RenderInputElement("COLOR", 0, 2, RenderFormat::R8G8B8A8_UNORM, 0, offsetof(ImDrawVert, col));

    RenderInputSlot inputSlot(0, sizeof(ImDrawVert));

    RenderGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = g_imPipelineLayout.get();
    pipelineDesc.vertexShader = vertexShader.get();
    pipelineDesc.pixelShader = pixelShader.get();
    pipelineDesc.renderTargetFormat[0] = BACKBUFFER_FORMAT;
    pipelineDesc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
    pipelineDesc.renderTargetCount = 1;
    pipelineDesc.inputElements = inputElements;
    pipelineDesc.inputElementsCount = std::size(inputElements);
    pipelineDesc.inputSlots = &inputSlot;
    pipelineDesc.inputSlotsCount = 1;
    g_imPipeline = g_device->createGraphicsPipeline(pipelineDesc);

    pipelineDesc.renderTargetBlend[0].dstBlend = RenderBlend::ONE;
    g_imAdditivePipeline = g_device->createGraphicsPipeline(pipelineDesc);

#ifndef ENABLE_IM_FONT_ATLAS_SNAPSHOT
    ImFontAtlasSnapshot snapshot;
    snapshot.Snap();

    FILE* file = fopen("im_font_atlas.bin", "wb");
    if (file)
    {
        fwrite(snapshot.data.data(), 1, snapshot.data.size(), file);
        fclose(file);
    }

    ddspp::Header header;
    ddspp::HeaderDXT10 headerDX10;
    ddspp::encode_header(ddspp::R8G8B8A8_UNORM, width, height, 1, ddspp::Texture2D, 1, 1, header, headerDX10);

    file = fopen("im_font_atlas.dds", "wb");
    if (file)
    {
        fwrite(&ddspp::DDS_MAGIC, 4, 1, file);
        fwrite(&header, sizeof(header), 1, file);
        fwrite(&headerDX10, sizeof(headerDX10), 1, file);
        fwrite(pixels, 4, width * height, file);
        fclose(file);
    }
#endif
}

static void CheckSwapChain()
{
    g_swapChain->setVsyncEnabled(Config::VSync);
    g_swapChainValid &= !g_swapChain->needsResize();

    if (!g_swapChainValid)
    {
        Video::WaitForGPU();
        g_backBuffer->framebuffers.clear();
        g_swapChainValid = g_swapChain->resize();
        g_needsResize = g_swapChainValid;
    }

    if (g_swapChainValid)
    {
        g_swapChainAcquireProfiler.Begin();
        g_swapChainValid = g_swapChain->acquireTexture(g_acquireSemaphores[g_frame].get(), &g_backBufferIndex);
        g_swapChainAcquireProfiler.End();
    }

    if (g_needsResize)
        Video::ComputeViewportDimensions();

    g_backBuffer->width = Video::s_viewportWidth;
    g_backBuffer->height = Video::s_viewportHeight;
}

static void BeginCommandList()
{
    g_renderTarget = g_backBuffer;
    g_depthStencil = nullptr;
    g_framebuffer = nullptr;

    g_pipelineState.renderTargetFormat = BACKBUFFER_FORMAT;
    g_pipelineState.depthStencilFormat = RenderFormat::UNKNOWN;

    if (g_swapChainValid)
    {
        uint32_t width = Video::s_viewportWidth;
        uint32_t height = Video::s_viewportHeight;

        bool usingIntermediaryTexture = (width != g_swapChain->getWidth()) || (height != g_swapChain->getHeight()) ||
            Config::XboxColorCorrection || (abs(Config::Brightness - 0.5f) > 0.001f);

        if (usingIntermediaryTexture)
        {
            if (g_intermediaryBackBufferTextureWidth != width ||
                g_intermediaryBackBufferTextureHeight != height)
            {
                if (g_intermediaryBackBufferTextureDescriptorIndex == NULL)
                    g_intermediaryBackBufferTextureDescriptorIndex = g_textureDescriptorAllocator.allocate();

                Video::WaitForGPU(); // Fine to wait for GPU, this'll only happen during resize.

                g_intermediaryBackBufferTexture = g_device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, BACKBUFFER_FORMAT, RenderTextureFlag::RENDER_TARGET));
                SetTextureDescriptor(g_textureDescriptorSet, g_intermediaryBackBufferTextureDescriptorIndex, g_intermediaryBackBufferTexture.get(), width, height, RenderTextureLayout::SHADER_READ);

                g_intermediaryBackBufferTextureWidth = width;
                g_intermediaryBackBufferTextureHeight = height;

                g_backBuffer->framebuffers.clear();
            }

            g_backBuffer->texture = g_intermediaryBackBufferTexture.get();
        }
        else
        {
            g_backBuffer->texture = g_swapChain->getTexture(g_backBufferIndex);
        }
    }
    else
    {
        g_backBuffer->texture = g_backBuffer->textureHolder.get();
    }

    g_backBuffer->layout = RenderTextureLayout::UNKNOWN;

    for (size_t i = 0; i < 16; i++)
    {
        g_sharedConstants.texture2DIndices[i] = TEXTURE_DESCRIPTOR_NULL_TEXTURE_2D;
        g_sharedConstants.textureSizes[i][0] = 1.0f;
        g_sharedConstants.textureSizes[i][1] = 1.0f;
        g_sharedConstants.texture3DIndices[i] = TEXTURE_DESCRIPTOR_NULL_TEXTURE_3D;
        g_sharedConstants.textureCubeIndices[i] = TEXTURE_DESCRIPTOR_NULL_TEXTURE_CUBE;
    }
    g_sharedConstants.gatherableSlots = 0;

    memset(g_textures, 0, sizeof(g_textures));

    if (Config::GITextureFiltering == EGITextureFiltering::Bicubic)
        g_pipelineState.specConstants |= SPEC_CONSTANT_BICUBIC_GI_FILTER;
    else
        g_pipelineState.specConstants &= ~SPEC_CONSTANT_BICUBIC_GI_FILTER;

    auto& commandList = g_commandLists[g_frame];

    commandList->begin();
    commandList->resetQueryPool(g_queryPools[g_frame].get(), 0, NUM_QUERIES);
    commandList->writeTimestamp(g_queryPools[g_frame].get(), 0);
#if defined(__SWITCH__)
    PassProfilerBeginFrame();
#endif
    commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
    commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
    commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
    commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
    commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);

    g_readyForCommands = true;
    g_readyForCommands.notify_one();
}

template<typename T>
static void ApplyLowEndDefault(ConfigDef<T> &configDef, T newDefault, bool &changed)
{
    if (configDef.IsDefaultValue() && !configDef.IsLoadedFromConfig)
    {
        configDef = newDefault;
        changed = true;
    }
    
    configDef.DefaultValue = newDefault;
}

static void ApplyLowEndDefaults()
{
    bool changed = false;

    ApplyLowEndDefault(Config::AntiAliasing, EAntiAliasing::MSAA2x, changed);
    ApplyLowEndDefault(Config::ShadowResolution, EShadowResolution::Original, changed);
    ApplyLowEndDefault(Config::TransparencyAntiAliasing, false, changed);
    ApplyLowEndDefault(Config::GITextureFiltering, EGITextureFiltering::Bilinear, changed);

    if (changed) 
    {
        Config::Save();
    }
}

bool Video::CreateHostDevice(const char *sdlVideoDriver, bool graphicsApiRetry)
{
#if defined(__SWITCH__)
    // Before the first render command: the batches' producer must not change while commands are queued.
    g_renderQueueTokenEnabled = Config::SwitchRenderQueueToken;
    g_idleRenderThreadBatches = Config::SwitchLargerCommandBatches && Config::SwitchIdleRenderThreadBatches;
    g_renderCommandBatchCapacity = g_idleRenderThreadBatches ? RENDER_COMMAND_BATCH_SIZE : Config::SwitchLargerCommandBatches ? 256 : 128;
    g_renderCommandBatchThreshold = Config::SwitchLargerCommandBatches ? 128 : 64;
    g_zeroCopyBatches = Config::SwitchZeroCopyBatches;
#endif

    for (uint32_t i = 0; i < 16; i++)
        g_inputSlots[i].index = i;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    GameWindow::Init(sdlVideoDriver);

#ifdef UNLEASHED_RECOMP_D3D12
    g_vulkan = DetectWine() || Config::GraphicsAPI == EGraphicsAPI::Vulkan;
#endif

    // Attempt to create the possible backends using a vector of function pointers. Whichever succeeds first will be the chosen API.
    using RenderInterfaceFunction = std::unique_ptr<RenderInterface>(void);
    std::vector<RenderInterfaceFunction *> interfaceFunctions;

#ifdef UNLEASHED_RECOMP_D3D12
    bool allowVulkanRedirection = true;

    if (graphicsApiRetry)
    {
        // If we are attempting to create again after a reboot due to a crash, swap the order.
        g_vulkan = !g_vulkan;

        // Don't allow redirection to Vulkan if we are retrying after a crash, 
        // so the user can at least boot the game with D3D12 if Vulkan fails to work.
        allowVulkanRedirection = false;
    }

    interfaceFunctions.push_back(g_vulkan ? CreateVulkanInterfaceWrapper : CreateD3D12Interface);
    interfaceFunctions.push_back(g_vulkan ? CreateD3D12Interface : CreateVulkanInterfaceWrapper);
#else
    interfaceFunctions.push_back(CreateVulkanInterfaceWrapper);
#endif

    for (size_t i = 0; i < interfaceFunctions.size(); i++)
    {
        RenderInterfaceFunction* interfaceFunction = interfaceFunctions[i];

#ifdef UNLEASHED_RECOMP_D3D12
        // Wrap the device creation in __try/__except to survive from driver crashes.
        __try
#endif
        {
            g_interface = interfaceFunction();
            if (g_interface == nullptr)
            {
                continue;
            }

            g_device = g_interface->createDevice(Config::GraphicsDevice);
            if (g_device != nullptr)
            {
                const RenderDeviceDescription &deviceDescription = g_device->getDescription();
                
#ifdef UNLEASHED_RECOMP_D3D12
                if (interfaceFunction == CreateD3D12Interface)
                {
                    if (allowVulkanRedirection)
                    {
                        bool redirectToVulkan = false;

                        if (deviceDescription.vendor == RenderDeviceVendor::AMD)
                        {
                            // AMD Drivers before this version have a known issue where MSAA resolve targets will fail to work correctly.
                            // If no specific graphics API was selected, we silently destroy this one and move to the next option as it'll
                            // just work incorrectly otherwise and result in visual glitches and 3D rendering not working in general.
                            constexpr uint64_t MinimumAMDDriverVersion = 0x1F00005DC2005CULL; // 31.0.24002.92
                            if ((Config::GraphicsAPI == EGraphicsAPI::Auto) && (deviceDescription.driverVersion < MinimumAMDDriverVersion))
                                redirectToVulkan = true;
                        }
                        else if (deviceDescription.vendor == RenderDeviceVendor::INTEL)
                        {
                            // Intel drivers on D3D12 are extremely buggy, introducing various graphical glitches.
                            // We will redirect users to Vulkan until a workaround can be found.
                            if (Config::GraphicsAPI == EGraphicsAPI::Auto)
                                redirectToVulkan = true;
                        }

                        if (redirectToVulkan)
                        {
                            g_device.reset();
                            g_interface.reset();

                            // In case Vulkan fails to initialize, we will try D3D12 again afterwards, 
                            // just to get the game to boot. This only really happens in very old Intel GPU drivers.
                            if (!g_vulkan)
                            {
                                interfaceFunctions.push_back(CreateD3D12Interface);
                                allowVulkanRedirection = false;
                            }

                            continue;
                        }
                    }

                    // Hardware resolve seems to be completely bugged on Intel D3D12 drivers.
                    g_hardwareResolve = (deviceDescription.vendor != RenderDeviceVendor::INTEL);
                    g_hardwareDepthResolve = (deviceDescription.vendor != RenderDeviceVendor::INTEL);
                }

                g_vulkan = (interfaceFunction == CreateVulkanInterfaceWrapper);
#endif
                // Enable triangle strip workaround if we are on AMD, as there is a bug where
                // restart indices cause triangles to be culled incorrectly. Converting them to degenerate triangles fixes it.
                g_triangleStripWorkaround = (deviceDescription.vendor == RenderDeviceVendor::AMD);

                break;
            }
        }
#ifdef UNLEASHED_RECOMP_D3D12
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (graphicsApiRetry)
            {
                // If we were retrying, and this also failed, then we'll show the user neither of the graphics APIs succeeded.
                return false;
            }
            else
            {
                // If this is the first crash we ran into, reboot and try the other graphics API.
                os::process::StartProcess(os::process::GetExecutablePath(), { "--graphics-api-retry" });
                std::_Exit(0);
            }
        }
#endif
    }

    if (g_device == nullptr)
    {
        return false;
    }

#ifdef UNLEASHED_RECOMP_D3D12
    if (graphicsApiRetry)
    {
        // If we managed to create a device after retrying it in a reboot, remember the one we picked.
        Config::GraphicsAPI = g_vulkan ? EGraphicsAPI::Vulkan : EGraphicsAPI::D3D12;
    }
#endif

    g_capabilities = g_device->getCapabilities();

    LoadEmbeddedResources();

    constexpr uint64_t LowEndMemoryLimit = 2048ULL * 1024ULL * 1024ULL;
    RenderDeviceDescription deviceDescription = g_device->getDescription();
    bool lowEndType = deviceDescription.type != RenderDeviceType::UNKNOWN && deviceDescription.type != RenderDeviceType::DISCRETE;
    bool lowEndMemory = deviceDescription.dedicatedVideoMemory < LowEndMemoryLimit;
    bool lowEndUMA = deviceDescription.type == RenderDeviceType::UNKNOWN && g_capabilities.uma;
    if (lowEndType || lowEndMemory || lowEndUMA)
    {
        // Switch to low end defaults if a non-discrete GPU was detected or a low amount of VRAM was detected.
        // Checking for UMA on D3D12 seems to be a reliable way to detect integrated GPUs.
        ApplyLowEndDefaults();
    }

    const RenderSampleCounts colourSampleCount = g_device->getSampleCountsSupported(RenderFormat::R16G16B16A16_FLOAT);
    const RenderSampleCounts depthSampleCount  = g_device->getSampleCountsSupported(RenderFormat::D32_FLOAT);
    const RenderSampleCounts commonSampleCount = colourSampleCount & depthSampleCount;

    // Disable specific MSAA levels if they are not supported.
    if ((commonSampleCount & RenderSampleCount::COUNT_2) == 0)
        Config::AntiAliasing.InaccessibleValues.emplace(EAntiAliasing::MSAA2x);
    if ((commonSampleCount & RenderSampleCount::COUNT_4) == 0)
        Config::AntiAliasing.InaccessibleValues.emplace(EAntiAliasing::MSAA4x);
    if ((commonSampleCount & RenderSampleCount::COUNT_8) == 0)
        Config::AntiAliasing.InaccessibleValues.emplace(EAntiAliasing::MSAA8x);

    // Set Anti-Aliasing to nearest supported level.
    Config::AntiAliasing.SnapToNearestAccessibleValue(false);

    g_queue = g_device->createCommandQueue(RenderCommandListType::DIRECT);

    for (auto& commandList : g_commandLists)
        commandList = g_queue->createCommandList();

    for (auto& commandFence : g_commandFences)
        commandFence = g_device->createCommandFence();

    for (auto& queryPool : g_queryPools)
        queryPool = g_device->createQueryPool(NUM_QUERIES);

#if defined(__SWITCH__)
    os::switch_stall_watch::SetStateReporter(ReportRendererState);

    g_drawProfilerEnabled = Config::SwitchGpuDrawProfiler;
    g_passProfilerEnabled = Config::SwitchGpuPassProfiler || g_drawProfilerEnabled;
    if (g_passProfilerEnabled)
    {
        for (auto& frame : g_passProfilerFrames)
        {
            frame.queries = g_device->createQueryPool(PASS_PROFILER_MAX_PASSES + 1);

            if (g_drawProfilerEnabled)
            {
                for (auto& pool : frame.drawQueries)
                    pool = g_device->createQueryPool(DRAW_PROFILER_POOL_SIZE);
            }
        }

        if (g_drawProfilerEnabled)
            fprintf(stderr, "GPU pass and draw profiler on: per-pass and per-draw GPU times every 300 frames.\n");
        else
            fprintf(stderr, "GPU pass profiler on: per-pass GPU times every 300 frames.\n");
    }
#endif

    g_copyQueue = g_device->createCommandQueue(RenderCommandListType::COPY);
    g_copyCommandList = g_copyQueue->createCommandList();
    g_copyCommandFence = g_device->createCommandFence();

    uint32_t bufferCount = 2;

    switch (Config::TripleBuffering)
    {
    case ETripleBuffering::Auto:
        if (g_vulkan)
        {
            // Defaulting to 3 is fine if presentWait as supported, as the maximum frame latency allowed is only 1.
            bufferCount = g_device->getCapabilities().presentWait ? 3 : 2;
        }
        else
        {
            // Defaulting to 3 is fine on D3D12 thanks to flip discard model.
            bufferCount = 3;
        }

        break;
    case ETripleBuffering::On:
        bufferCount = 3;
        break;
    case ETripleBuffering::Off:
        bufferCount = 2;
        break;
    }

#if defined(__SWITCH__)
    // presentWait is compiled out on the VI/NVK path, so Auto resolves to
    // double buffering — under Mesa 26.1.1's WSI the acquire then blocks an
    // extra vblank and the game tick halves (the frame-synced sound engine
    // audibly stutters at exactly half rate). Triple buffer unless the user
    // explicitly opted out.
    if (Config::TripleBuffering == ETripleBuffering::Auto)
        bufferCount = 3;
#endif

    g_swapChain = g_queue->createSwapChain(GameWindow::s_renderWindow, bufferCount, BACKBUFFER_FORMAT, Config::MaxFrameLatency);
    g_swapChain->setVsyncEnabled(Config::VSync);
    g_swapChainValid = !g_swapChain->needsResize();

    for (auto& acquireSemaphore : g_acquireSemaphores)
        acquireSemaphore = g_device->createCommandSemaphore();
    
    for (auto& renderSemaphore : g_renderSemaphores)
        renderSemaphore = g_device->createCommandSemaphore();

    RenderPipelineLayoutBuilder pipelineLayoutBuilder;
    pipelineLayoutBuilder.begin(false, true);
    
#if defined(__SWITCH__)
    if (Config::SwitchCompactTextureHeap)
    {
        g_textureDescriptorCount = TEXTURE_DESCRIPTOR_COMPACT_SIZE;
        g_textureDescriptorAllocator.limit = TEXTURE_DESCRIPTOR_COMPACT_SIZE;
    }
#endif

    RenderDescriptorSetBuilder descriptorSetBuilder;
    descriptorSetBuilder.begin();
    descriptorSetBuilder.addTexture(0, g_textureDescriptorCount);
    descriptorSetBuilder.end(true, g_textureDescriptorCount);

    g_textureDescriptorSet = descriptorSetBuilder.create(g_device.get());
    
    for (size_t i = 0; i < TEXTURE_DESCRIPTOR_NULL_COUNT; i++)
    {
        auto& texture = g_blankTextures[i];
        auto& textureView = g_blankTextureViews[i];

        RenderTextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.depth = 1;
        desc.mipLevels = 1;
        desc.format = RenderFormat::R8_UNORM;

        RenderTextureViewDesc viewDesc;
        viewDesc.format = desc.format;
        viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::ZERO, RenderSwizzle::ZERO, RenderSwizzle::ZERO, RenderSwizzle::ZERO);
        viewDesc.mipLevels = 1;

        switch (i)
        {
        case TEXTURE_DESCRIPTOR_NULL_TEXTURE_2D:
            desc.dimension = RenderTextureDimension::TEXTURE_2D;
            desc.arraySize = 1;
            viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
            break;

        case TEXTURE_DESCRIPTOR_NULL_TEXTURE_3D:
            desc.dimension = RenderTextureDimension::TEXTURE_3D;
            desc.arraySize = 1;
            viewDesc.dimension = RenderTextureViewDimension::TEXTURE_3D;
            break;

        case TEXTURE_DESCRIPTOR_NULL_TEXTURE_CUBE:
            desc.dimension = RenderTextureDimension::TEXTURE_2D;
            desc.arraySize = 6;
            desc.flags = RenderTextureFlag::CUBE;
            viewDesc.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
            break;

        default:
            assert(false && "Unknown null descriptor dimension");
            break;
        }

        texture = g_device->createTexture(desc);
        textureView = texture->createTextureView(viewDesc);

        SetTextureDescriptor(g_textureDescriptorSet, uint32_t(i), texture.get(), 1, 1, RenderTextureLayout::SHADER_READ, textureView.get());
    }

#if defined(__SWITCH__)
    g_nullTextureDescriptorsWritten = true;
#endif

    pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);
    pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);
    pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);
    
    descriptorSetBuilder.begin();
    descriptorSetBuilder.addSampler(0, SAMPLER_DESCRIPTOR_SIZE);
    descriptorSetBuilder.end(true, SAMPLER_DESCRIPTOR_SIZE);
    
    g_samplerDescriptorSet = descriptorSetBuilder.create(g_device.get());
    auto& [descriptorIndex, sampler] = g_samplerStates[XXH3_64bits(&g_samplerDescs[0], sizeof(RenderSamplerDesc))];
    descriptorIndex = 1;
    sampler = g_device->createSampler(g_samplerDescs[0]);
    g_samplerDescriptorSet->setSampler(0, sampler.get());

    pipelineLayoutBuilder.addDescriptorSet(descriptorSetBuilder);

#if defined(__SWITCH__) && defined(SPEC_CONSTANT_TEXTURE_SIZE)
    g_textureSizeSpecBits = 0;
    if (g_vulkan && Config::SwitchTextureSizeConstants)
        g_textureSizeSpecBits |= SPEC_CONSTANT_TEXTURE_SIZE;
    if (g_vulkan && Config::SwitchVerifyTextureSizes)
        g_textureSizeSpecBits |= SPEC_CONSTANT_TEXTURE_SIZE_VERIFY;
#endif

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
    g_shadowGatherSpecBits = 0;
    if (g_vulkan && (Config::SwitchShadowGather || Config::SwitchVerifyShadowGather))
        g_shadowGatherSpecBits |= SPEC_CONSTANT_SHADOW_GATHER;
    if (g_vulkan && Config::SwitchVerifyShadowGather)
        g_shadowGatherSpecBits |= SPEC_CONSTANT_SHADOW_GATHER_VERIFY;

    g_relativeFromMemory = g_vulkan && Config::SwitchIndexedConstantsFromMemory;
    g_bonesSpecialization = g_vulkan && Config::SwitchBonesSpecialization;
    g_trimPixelOutputs = g_vulkan && Config::SwitchTrimPixelOutputs;
    g_trimVertexOutputs = g_vulkan && Config::SwitchTrimVertexOutputs;
#endif
#if defined(__SWITCH__)
    g_streamingBuffers = Config::SwitchStreamingBuffers;
    g_lazyResolves = Config::SwitchLazyResolves;
    g_resolveHandOver = Config::SwitchResolveHandOver;
    g_keepResolvesPending = Config::SwitchKeepResolvesPending;
    g_skipUnusedPixelConstants = Config::SwitchSkipUnusedPixelConstants;
    g_coverageHandOver = g_vulkan && Config::SwitchCoverageHandOver;
    g_trimConstantUploads = Config::SwitchTrimConstantUploads;
    g_alphaTestEarlyOutBit = (g_vulkan && Config::SwitchAlphaTestEarlyOut) ? SPEC_CONSTANT_ALPHA_TEST_EARLY_OUT : 0;
    g_sparseConstantCopies = Config::SwitchSparseConstantCopies;
    g_batchSeveralDraws = Config::SwitchBatchSeveralDraws;
    g_shadowGatherSpecialization = g_vulkan && Config::SwitchShadowGatherSpecialization;
    g_eagerSampleTransitions = Config::SwitchEagerSampleTransitions;
    g_singleCopyTriangle = Config::SwitchSingleCopyTriangle;
    g_exactCoverage = g_coverageHandOver && Config::SwitchExactCoverage;
    g_skipOverwrittenClears = Config::SwitchSkipOverwrittenClears;
    g_skipNoOpDraws = Config::SwitchSkipNoOpDraws;
    g_skipRestoreDraws = Config::SwitchSkipRestoreDraws;
    g_frameLogEnabled = Config::SwitchFrameLog;
    // Never with the gather verification: its point fetches must not move into the early-out branch.
    g_alphaTestSinkBit = (g_vulkan && Config::SwitchAlphaTestSink && !Config::SwitchVerifyShadowGather) ? SPEC_CONSTANT_ALPHA_TEST_SINK : 0;
    g_alphaTestQuadSinkBit = (g_alphaTestSinkBit != 0 && Config::SwitchAlphaTestQuadSink) ? SPEC_CONSTANT_ALPHA_TEST_QUAD_SINK : 0;
    g_skipTransparentPixels = g_vulkan && Config::SwitchSkipTransparentPixels;
    g_skipRedundantRenderStates = Config::SwitchSkipRedundantRenderStates;
    g_skipRedundantSamplerStates = Config::SwitchSkipRedundantSamplerStates;
    g_pipelineLookupCache = Config::SwitchPipelineLookupCache;
    g_samplerCache = Config::SwitchSamplerCache;
    g_skipDeadCopies = Config::SwitchSkipDeadCopies;
    g_presentOnRenderThread = Config::SwitchPresentOnRenderThread;
    g_carryClears = g_skipOverwrittenClears && Config::SwitchCarryClears;
    g_deferDepthClears = Config::SwitchSkipOverwrittenDepthClears;
    g_eagerDepthTransitions = Config::SwitchEagerDepthTransitions;
    g_hostThreadCoresReady.store(true, std::memory_order_release);
#endif
#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
    g_precompileBonesVariants = g_bonesSpecialization && Config::SwitchPrecompileBonesVariants;
#endif

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
    g_constantsUbo = g_vulkan && Config::SwitchConstantsUBO;
    g_constantsUboSpecBit = g_constantsUbo ? SPEC_CONSTANT_CONSTANTS_UBO : 0;

#if defined(__SWITCH__)
    // One line per run saying which renderer changes were active, for comparing runs.
    fprintf(stderr, "Switch renderer: constants in uniform buffers %s, texture sizes from constants %s%s, "
        "depth-only draws without pixel shader %s, precise barriers %s, pipeline cache %s, batched render commands %s, "
        "unread vertex outputs trimmed %s\n",
        g_constantsUbo ? "on" : "off",
#if defined(SPEC_CONSTANT_TEXTURE_SIZE)
        (g_textureSizeSpecBits & SPEC_CONSTANT_TEXTURE_SIZE) ? "on" : "off",
        (g_textureSizeSpecBits & SPEC_CONSTANT_TEXTURE_SIZE_VERIFY) ? " (verify mode)" : "",
#else
        "n/a", "",
#endif
        Config::SwitchDepthOnlyWithoutPixelShader ? "on" : "off",
        Config::SwitchPreciseBarriers ? "on" : "off",
        Config::SwitchPipelineCache ? "on" : "off",
        Config::SwitchBatchRenderCommands ? "on" : "off",
#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
        g_trimVertexOutputs ? "on" : "off");

    fprintf(stderr, "Switch shaders: shadow filters as gathers %s%s, skinning specialized per pipeline %s, "
        "indexed constants from memory %s, unused colour channels trimmed %s, streaming vertex/index buffers %s\n",
        (g_shadowGatherSpecBits & SPEC_CONSTANT_SHADOW_GATHER) ? "on" : "off",
        (g_shadowGatherSpecBits & SPEC_CONSTANT_SHADOW_GATHER_VERIFY) ? " (verify mode)" : "",
        g_bonesSpecialization ? "on" : "off",
        g_relativeFromMemory ? "on" : "off",
        g_trimPixelOutputs ? "on" : "off",
        g_streamingBuffers ? "on" : "off");

    fprintf(stderr, "Switch resolves: lazy %s, hand-over at clears %s, kept pending over present %s, hand-over at draws %s; "
        "unused pixel constants skipped %s, skinning variants built while loading %s, pipeline cache saved during play %s\n",
        g_lazyResolves ? "on" : "off", g_resolveHandOver ? "on" : "off", g_keepResolvesPending ? "on" : "off",
        g_coverageHandOver ? "on" : "off",
        g_skipUnusedPixelConstants ? "on" : "off", g_precompileBonesVariants ? "on" : "off",
        Config::SwitchPipelineCacheSaveDuringPlay ? "on" : "off");

    fprintf(stderr, "Switch round 5: constant uploads trimmed to the registers shaders read %s, "
        "alpha-tested pixels skip the math they would discard %s, CPU sampler %s\n",
        g_trimConstantUploads ? "on" : "off", g_alphaTestEarlyOutBit != 0 ? "on" : "off",
        Config::SwitchCpuProfiler ? "on" : "off");

    fprintf(stderr, "Switch round 6: sparse constant copies %s, render commands handed over every few draws %s, "
        "known-gatherable shadow pipelines %s, early texture transitions %s, relaxed guest atomics %s, "
        "thread cores from the game's hints %s\n",
        g_sparseConstantCopies ? "on" : "off", g_batchSeveralDraws ? "on" : "off",
        g_shadowGatherSpecialization ? "on" : "off", g_eagerSampleTransitions ? "on" : "off",
        Config::SwitchRelaxedAtomics ? "on" : "off", Config::SwitchThreadIdealCores ? "on" : "off");

    fprintf(stderr, "Switch round 7 GPU: one copy triangle %s, exact full-screen hand-overs %s, overwritten clears skipped %s, "
        "no-op draws skipped %s, alpha-test sinking %s (also early-outs for late alpha), transparent pixels skipped %s\n",
        g_singleCopyTriangle ? "on" : "off", g_exactCoverage ? "on" : "off", g_skipOverwrittenClears ? "on" : "off",
        g_skipNoOpDraws ? "on" : "off", g_alphaTestSinkBit != 0 ? "on" : "off", g_skipTransparentPixels ? "on" : "off");
    fprintf(stderr, "Switch round 7 CPU: guest code %s; redundant render states skipped %s, redundant sampler states skipped %s, "
        "larger command batches %s, render queue token %s, native RTTI %s, native shader constants %s, UI modifier cache %s, "
        "host thread cores %s, pipeline lookup cache %s, sampler cache %s\n",
#ifdef UNLEASHED_RECOMP_CLASSIC_CODEGEN
        "classic (volatile memory, all callee saves, FPSCR per block, no hot section)",
#else
        "round 7 (loop barriers, dead callee saves dropped, FPSCR across labels, hot functions grouped)",
#endif
        g_skipRedundantRenderStates ? "on" : "off", g_skipRedundantSamplerStates ? "on" : "off",
        Config::SwitchLargerCommandBatches ? "on" : "off", Config::SwitchRenderQueueToken ? "on" : "off",
        Config::SwitchNativeRtti ? "on" : "off", Config::SwitchNativeShaderConstants ? "on" : "off",
        Config::SwitchModifierCache ? "on" : "off", Config::SwitchHostThreadCores ? "on" : "off",
        g_pipelineLookupCache ? "on" : "off", g_samplerCache ? "on" : "off");
    fprintf(stderr, "Switch round 8: alpha-test quad sinking %s, restore draws skipped %s, native LZX decompression %s%s, "
        "fast critical sections %s, fast events %s, zero-copy command batches %s, frame log %s, stall watchdog %.1f s\n",
        g_alphaTestQuadSinkBit != 0 ? "on" : "off", g_skipRestoreDraws ? "on" : "off",
        Config::SwitchNativeDecompress ? "on" : "off", Config::SwitchVerifyNativeDecompress ? " (verified against the game's)" : "",
        Config::SwitchFastCriticalSections ? "on" : "off", Config::SwitchFastEvents ? "on" : "off",
        g_zeroCopyBatches ? "on" : "off", g_frameLogEnabled ? "on" : "off", double(Config::SwitchStallWatchSeconds));
    fprintf(stderr, "Switch round 9 GPU: dead resolve copies skipped %s, clears carried to their pass %s, overwritten depth clears "
        "skipped %s, early depth transitions %s\n",
        g_skipDeadCopies ? "on" : "off", g_carryClears ? "on" : "off", g_deferDepthClears ? "on" : "off",
        g_eagerDepthTransitions ? "on" : "off");
    fprintf(stderr, "Switch round 9 CPU: present on the render thread %s, larger batches while the render thread waits %s, "
        "render thread priority 0x%X, guest spin locks spin before sleeping %s\n",
        g_presentOnRenderThread ? "on" : "off", g_idleRenderThreadBatches ? "on" : "off",
        uint32_t(Config::SwitchRenderThreadPriority), Config::SwitchGuestSpinBeforeSleep ? "on" : "off");
#else
        "n/a");
#endif
#endif

    if (g_constantsUbo)
    {
        g_constantsUboSetBuilder.begin();
        g_constantsUboSetBuilder.addConstantBufferDynamic(0);
        g_constantsUboSetBuilder.addConstantBufferDynamic(1);
        g_constantsUboSetBuilder.addConstantBufferDynamic(2);
        g_constantsUboSetBuilder.end();

        // Set 4, after the three texture sets and the sampler set.
        pipelineLayoutBuilder.addDescriptorSet(g_constantsUboSetBuilder);
    }
#endif

    if (g_vulkan)
    {
        pipelineLayoutBuilder.addPushConstant(0, 4, 24, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
    }
    else
    {
        pipelineLayoutBuilder.addRootDescriptor(0, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
        pipelineLayoutBuilder.addRootDescriptor(1, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
        pipelineLayoutBuilder.addRootDescriptor(2, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
        pipelineLayoutBuilder.addPushConstant(3, 4, 4, RenderShaderStageFlag::PIXEL); // For copy/resolve shaders.
    }
    pipelineLayoutBuilder.end();
    
    g_pipelineLayout = pipelineLayoutBuilder.create(g_device.get());

#if defined(__SWITCH__)
    {
        // Same configuration nfsmw-nx ships: set 4 by differences, whole-command emission, cbufs
        // without repeated work, dynamic state by dirty groups and pipeline prefetch; the fast set 4
        // path (improvement 3) is not used there, so it is explicitly left off here too. Each path
        // compares its result with the original one on sampled draws and turns itself off on a
        // difference. They take effect from the next command buffer.
        const int32_t enable = Config::SwitchNvkFastPaths ? 1 : 0;
        NvkSwitchSet4Header* set4 = &nvk_switch_set4;
        NvkSwitchDraw* draw = &nvk_switch_dibujo;

        if (set4 != nullptr && set4->version == 1)
            __atomic_store_n(&set4->requested, enable, __ATOMIC_RELAXED);

        if (draw != nullptr && draw->version == 1)
        {
            const int32_t requested[5] = { enable, enable, enable, 0, enable };
            for (size_t i = 0; i < 5; i++)
                __atomic_store_n(&draw->improvements[i].requested, requested[i], __ATOMIC_RELAXED);
        }

        fprintf(stderr, "NVK fast paths: %s (set 4 contract %s, draw contract %s).\n",
            enable ? "requested" : "off",
            (set4 != nullptr && set4->version == 1) ? "found" : "absent",
            (draw != nullptr && draw->version == 1) ? "found" : "absent");
    }
#endif

    g_copyShader = CREATE_SHADER(copy_vs);
    g_copyColorShader = CREATE_SHADER(copy_color_ps);
    auto copyDepthShader = CREATE_SHADER(copy_depth_ps);

    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_pipelineLayout.get();
    desc.vertexShader = g_copyShader.get();
    desc.pixelShader = copyDepthShader.get();
    desc.depthFunction = RenderComparisonFunction::ALWAYS;
    desc.depthEnabled = true;
    desc.depthWriteEnabled = true;
    desc.depthTargetFormat = RenderFormat::D32_FLOAT;
    g_copyDepthPipeline = g_device->createGraphicsPipeline(desc);

    g_resolveMsaaColorShaders[0] = CREATE_SHADER(resolve_msaa_color_2x);
    g_resolveMsaaColorShaders[1] = CREATE_SHADER(resolve_msaa_color_4x);
    g_resolveMsaaColorShaders[2] = CREATE_SHADER(resolve_msaa_color_8x);

    for (size_t i = 0; i < std::size(g_resolveMsaaDepthPipelines); i++)
    {
        std::unique_ptr<RenderShader> pixelShader;
        switch (i)
        {
        case 0:
            pixelShader = CREATE_SHADER(resolve_msaa_depth_2x);
            break;
        case 1:
            pixelShader = CREATE_SHADER(resolve_msaa_depth_4x);
            break;
        case 2:
            pixelShader = CREATE_SHADER(resolve_msaa_depth_8x);
            break;
        }

        desc = {};
        desc.pipelineLayout = g_pipelineLayout.get();
        desc.vertexShader = g_copyShader.get();
        desc.pixelShader = pixelShader.get();
        desc.depthFunction = RenderComparisonFunction::ALWAYS;
        desc.depthEnabled = true;
        desc.depthWriteEnabled = true;
        desc.depthTargetFormat = RenderFormat::D32_FLOAT;
        g_resolveMsaaDepthPipelines[i] = g_device->createGraphicsPipeline(desc);
    }

    for (auto& shader : g_gaussianBlurShaders)
        shader = std::make_unique<GuestShader>(ResourceType::PixelShader);

    g_gaussianBlurShaders[GAUSSIAN_BLUR_3X3]->shader = CREATE_SHADER(gaussian_blur_3x3);
    g_gaussianBlurShaders[GAUSSIAN_BLUR_5X5]->shader = CREATE_SHADER(gaussian_blur_5x5);
    g_gaussianBlurShaders[GAUSSIAN_BLUR_7X7]->shader = CREATE_SHADER(gaussian_blur_7x7);
    g_gaussianBlurShaders[GAUSSIAN_BLUR_9X9]->shader = CREATE_SHADER(gaussian_blur_9x9);
    MARK_CONSTANTS_THROUGH_UBO(g_gaussianBlurShaders[GAUSSIAN_BLUR_3X3], gaussian_blur_3x3);
    MARK_CONSTANTS_THROUGH_UBO(g_gaussianBlurShaders[GAUSSIAN_BLUR_5X5], gaussian_blur_5x5);
    MARK_CONSTANTS_THROUGH_UBO(g_gaussianBlurShaders[GAUSSIAN_BLUR_7X7], gaussian_blur_7x7);
    MARK_CONSTANTS_THROUGH_UBO(g_gaussianBlurShaders[GAUSSIAN_BLUR_9X9], gaussian_blur_9x9);
    for (auto& shader : g_gaussianBlurShaders)
        shader->neverDiscards = true;

    g_csdFilterShader = std::make_unique<GuestShader>(ResourceType::PixelShader);
    g_csdFilterShader->shader = CREATE_SHADER(csd_filter_ps);
    MARK_CONSTANTS_THROUGH_UBO(g_csdFilterShader, csd_filter_ps);

    g_enhancedMotionBlurShader = std::make_unique<GuestShader>(ResourceType::PixelShader);
    g_enhancedMotionBlurShader->shader = CREATE_SHADER(enhanced_motion_blur_ps);
    MARK_CONSTANTS_THROUGH_UBO(g_enhancedMotionBlurShader, enhanced_motion_blur_ps);
    g_enhancedMotionBlurShader->neverDiscards = true;

    CreateImGuiBackend();

    auto gammaCorrectionShader = CREATE_SHADER(gamma_correction_ps);

    desc = {};
    desc.pipelineLayout = g_pipelineLayout.get();
    desc.vertexShader = g_copyShader.get();
    desc.pixelShader = gammaCorrectionShader.get();
    desc.renderTargetFormat[0] = BACKBUFFER_FORMAT;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    g_gammaCorrectionPipeline = g_device->createGraphicsPipeline(desc);

    // NOTE: We initially allocate this on host memory to make the installer work, even if the 4 GB memory allocation fails.
    g_backBufferHolder = std::make_unique<GuestSurface>(ResourceType::RenderTarget);

    g_backBuffer = g_backBufferHolder.get();
    g_backBuffer->width = 1280;
    g_backBuffer->height = 720;
    g_backBuffer->format = BACKBUFFER_FORMAT;
    g_backBuffer->textureHolder = g_device->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, BACKBUFFER_FORMAT, RenderTextureFlag::RENDER_TARGET));

    Video::ComputeViewportDimensions();
    CheckSwapChain();
    BeginCommandList();

    RenderTextureBarrier blankTextureBarriers[TEXTURE_DESCRIPTOR_NULL_COUNT];
    for (size_t i = 0; i < TEXTURE_DESCRIPTOR_NULL_COUNT; i++)
        blankTextureBarriers[i] = RenderTextureBarrier(g_blankTextures[i].get(), RenderTextureLayout::SHADER_READ);

    g_commandLists[g_frame]->barriers(RenderBarrierStage::NONE, blankTextureBarriers, std::size(blankTextureBarriers));

    return true;
}

static uint32_t g_waitForGPUCount = 0;

void Video::WaitForGPU()
{
    g_waitForGPUCount++;

    // Wait for all queued frames to finish.
    for (size_t i = 0; i < NUM_FRAMES; i++)
    {
        if (g_commandListStates[i])
        {
            g_queue->waitForCommandFence(g_commandFences[i].get());
            g_commandListStates[i] = false;
        }
    }

    // Execute an empty command list and wait for it to end to guarantee that any remaining presentation has finished.
    g_commandLists[0]->begin();
    g_commandLists[0]->end();
    g_queue->executeCommandLists(g_commandLists[0].get(), g_commandFences[0].get());
    g_queue->waitForCommandFence(g_commandFences[0].get());
}

static uint32_t CreateDevice(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, be<uint32_t>* a6)
{
    g_xdbfTextureCache = std::unordered_map<uint16_t, GuestTexture *>();

    for (auto &achievement : g_xdbfWrapper.GetAchievements(XDBF_LANGUAGE_ENGLISH))
    {
        // huh?
        if (!achievement.pImageBuffer || !achievement.ImageBufferSize)
            continue;

        g_xdbfTextureCache[achievement.ID] =
            LoadTexture((uint8_t *)achievement.pImageBuffer, achievement.ImageBufferSize).release();
    }

    // Move backbuffer to guest memory.
    assert(!g_memory.IsInMemoryRange(g_backBuffer) && g_backBufferHolder != nullptr);
    g_backBuffer = g_userHeap.AllocPhysical<GuestSurface>(std::move(*g_backBufferHolder));

    // Check for stale reference. BeginCommandList() gets called before CreateDevice() which is where the assignment happens.
    if (g_renderTarget == g_backBufferHolder.get()) g_renderTarget = g_backBuffer;
    if (g_depthStencil == g_backBufferHolder.get()) g_depthStencil = g_backBuffer;

    // Free the host backbuffer.
    g_backBufferHolder = nullptr;

    auto device = g_userHeap.AllocPhysical<GuestDevice>();
    memset(device, 0, sizeof(*device));

    // Append render state functions to the end of guest function table.
    uint32_t functionOffset = PPC_CODE_BASE + PPC_CODE_SIZE;
    g_memory.InsertFunction(functionOffset, HostToGuestFunction<SetRenderStateUnimplemented>);

    for (size_t i = 0; i < std::size(device->setRenderStateFunctions); i++)
        device->setRenderStateFunctions[i] = functionOffset;

    for (auto& [state, function] : g_setRenderStateFunctions)
    {
        functionOffset += 4;
        g_memory.InsertFunction(functionOffset, function);
        device->setRenderStateFunctions[state / 4] = functionOffset;
    }

    for (size_t i = 0; i < std::size(device->setSamplerStateFunctions); i++)
        device->setSamplerStateFunctions[i] = *reinterpret_cast<uint32_t*>(g_memory.Translate(0x8330F3DC + i * 0xC));

    device->viewport.width = 1280.0f;
    device->viewport.height = 720.0f;
    device->viewport.maxZ = 1.0f;

    *a6 = g_memory.MapVirtual(device);

    return 0;
}

#if defined(__SWITCH__)
static void ExecutePendingResolvesOf(GuestSurface* surface, uint32_t trigger);
static void ForgetPendingResolves(GuestResource* resource);
#endif

static void DestructResource(GuestResource* resource) 
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::DestructResource;
    cmd.destructResource.resource = resource;
    EnqueueRenderCommand(cmd);
}

static void ProcDestructResource(const RenderCommand& cmd)
{
    const auto& args = cmd.destructResource;
#if defined(__SWITCH__)
    ForgetPendingResolves(args.resource);
#endif
    g_tempResources[g_frame].push_back(args.resource);
}

static uint32_t ComputeTexturePitch(GuestTexture* texture)
{
    return (texture->width * RenderFormatSize(texture->format) + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
}

#if defined(__SWITCH__)
// SwitchPresentOnRenderThread: before the D3D thread writes a buffer's or texture's memory again (a lock), the
// render thread has finished presenting the previous frame and processes this frame's commands as they come,
// as without it: an earlier unlock of the same memory is copied from it before the new contents can land.
static void WaitForPresentTail()
{
    const uint32_t sent = g_presentTailsSent.load(std::memory_order_acquire);
    uint32_t done = g_presentTailsDone.load(std::memory_order_acquire);
    if (done == sent || !IsPresentThread())
        return;

    while (done != sent)
    {
        g_presentTailsDone.wait(done, std::memory_order_acquire);
        done = g_presentTailsDone.load(std::memory_order_acquire);
    }
}
#endif

static void LockTextureRect(GuestTexture* texture, uint32_t, GuestLockedRect* lockedRect)
{
#if defined(__SWITCH__)
    WaitForPresentTail();
#endif
    uint32_t pitch = ComputeTexturePitch(texture);
    uint32_t slicePitch = pitch * texture->height;

    if (texture->mappedMemory == nullptr)
        texture->mappedMemory = g_userHeap.AllocPhysical(slicePitch, 0x10);

    lockedRect->pitch = pitch;
    lockedRect->bits = g_memory.MapVirtual(texture->mappedMemory);
}

static void UnlockTextureRect(GuestTexture* texture) 
{
    assert(std::this_thread::get_id() == g_presentThreadId);

    RenderCommand cmd;
    cmd.type = RenderCommandType::UnlockTextureRect;
    cmd.unlockTextureRect.texture = texture;
    EnqueueRenderCommand(cmd, true);
}

static void ProcUnlockTextureRect(const RenderCommand& cmd)
{
    const auto& args = cmd.unlockTextureRect;

#if defined(__SWITCH__)
    g_profilerTextureUpdates++;

    // A copy kept pending over a Present (SwitchKeepResolvesPending) was made before this update.
    if (args.texture->sourceSurface != nullptr && args.texture->pendingCarried)
        ExecutePendingResolvesOf(args.texture->sourceSurface, PASS_PROFILER_COPIES_OTHER);
#endif

    AddBarrier(args.texture, RenderTextureLayout::COPY_DEST);
    FlushBarriers();

    uint32_t pitch = ComputeTexturePitch(args.texture);
    uint32_t slicePitch = pitch * args.texture->height;

    auto allocation = g_uploadAllocators[g_frame].allocate(slicePitch, PLACEMENT_ALIGNMENT);
    memcpy(allocation.memory, args.texture->mappedMemory, slicePitch);

    g_commandLists[g_frame]->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(args.texture->texture, 0),
        RenderTextureCopyLocation::PlacedFootprint(allocation.buffer, args.texture->format, args.texture->width, args.texture->height, 1, pitch / RenderFormatSize(args.texture->format), allocation.offset));
}

static void* LockBuffer(GuestBuffer* buffer, uint32_t flags)
{
#if defined(__SWITCH__)
    WaitForPresentTail();
#endif
    buffer->lockedReadOnly = (flags & 0x10) != 0;

    if (buffer->mappedMemory == nullptr)
        buffer->mappedMemory = g_userHeap.AllocPhysical(buffer->dataSize, 0x10);

    return buffer->mappedMemory;
}

static void* LockVertexBuffer(GuestBuffer* buffer, uint32_t, uint32_t, uint32_t flags)
{
    return LockBuffer(buffer, flags);
}

static std::atomic<uint32_t> g_bufferUploadCount = 0;

#if defined(__SWITCH__)
// [Switch] SwitchStreamingBuffers. The game rewrites some vertex and index buffers between the draws of
// a frame, from its D3D thread. Copying every version into place took a barrier on each side of the
// copy, i.e. a GPU pipeline drain per unlock. Instead, the draws after an unlock bind the unlock's copy
// in the upload ring (which stays untouched until this frame slot comes around again), and the last
// version of each buffer is copied into place once, at the end of the frame, behind a single barrier.
// Every draw reads exactly the bytes it read before. Render thread only.
static uint64_t g_streamingFrame = 1;
static std::vector<GuestBuffer*> g_streamingBuffersInFrame;
static std::vector<RenderBufferBarrier> g_streamingBufferBarriers;

// What the vertex streams and the index buffer are bound to, so that a binding follows its buffer to
// the new copy. Cleared when the buffer is destroyed.
static GuestBuffer* g_vertexStreamBuffers[16];
static uint32_t g_vertexStreamOffsets[16];
static GuestBuffer* g_indexStreamBuffer;

static bool IsStreaming(const GuestBuffer* buffer)
{
    return buffer != nullptr && buffer->streamingFrame == g_streamingFrame;
}

static RenderBufferReference GetBufferReference(const GuestBuffer* buffer, uint32_t offset)
{
    if (IsStreaming(buffer))
        return buffer->streamingBuffer->at(buffer->streamingOffset + offset);

    return buffer->buffer->at(offset);
}

static void MarkVertexStreamDirty(uint32_t index)
{
    g_dirtyStates.vertexStreamFirst = std::min<uint8_t>(g_dirtyStates.vertexStreamFirst, index);
    g_dirtyStates.vertexStreamLast = std::max<uint8_t>(g_dirtyStates.vertexStreamLast, index);
}

// Rebinds whatever uses the buffer to its current contents.
static void RebindBuffer(const GuestBuffer* buffer)
{
    for (uint32_t i = 0; i < std::size(g_vertexStreamBuffers); i++)
    {
        if (g_vertexStreamBuffers[i] == buffer)
        {
            bool dirty = false;
            SetDirtyValue(dirty, g_vertexBufferViews[i].buffer, GetBufferReference(buffer, g_vertexStreamOffsets[i]));
            if (dirty)
                MarkVertexStreamDirty(i);
        }
    }

    if (g_indexStreamBuffer == buffer)
        SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.buffer, GetBufferReference(buffer, 0));
}

static void StreamBuffer(GuestBuffer* buffer, const UploadAllocation& allocation)
{
    g_profilerStreamedBuffers++;

    if (buffer->streamingFrame != g_streamingFrame)
        g_streamingBuffersInFrame.push_back(buffer);

    buffer->streamingBuffer = allocation.buffer;
    buffer->streamingOffset = allocation.offset;
    buffer->streamingFrame = g_streamingFrame;

    RebindBuffer(buffer);
}

// End of the frame's commands: the buffers take their last contents of the frame, and whatever is
// bound to them binds the buffers themselves again (their ring copies go away with this frame slot).
static void FlushStreamingBuffers(RenderCommandList* commandList)
{
    if (!g_streamingBuffersInFrame.empty())
    {
        g_streamingBufferBarriers.clear();
        for (auto buffer : g_streamingBuffersInFrame)
            g_streamingBufferBarriers.emplace_back(buffer->buffer.get(), RenderBufferAccess::WRITE);

        commandList->barriers(RenderBarrierStage::COPY, g_streamingBufferBarriers);

        for (auto buffer : g_streamingBuffersInFrame)
            commandList->copyBufferRegion(buffer->buffer->at(0), buffer->streamingBuffer->at(buffer->streamingOffset), buffer->dataSize);

        for (auto& barrier : g_streamingBufferBarriers)
            barrier.accessBits = RenderBufferAccess::READ;

        commandList->barriers(RenderBarrierStage::GRAPHICS, g_streamingBufferBarriers);

        for (auto buffer : g_streamingBuffersInFrame)
        {
            buffer->streamingFrame = 0;
            RebindBuffer(buffer);
        }

        g_streamingBuffersInFrame.clear();
    }

    g_streamingFrame++;
}

static void ForgetDestroyedBuffer(const GuestBuffer* buffer)
{
    for (auto& streamBuffer : g_vertexStreamBuffers)
    {
        if (streamBuffer == buffer)
            streamBuffer = nullptr;
    }

    if (g_indexStreamBuffer == buffer)
        g_indexStreamBuffer = nullptr;
}
#endif

template<typename T>
static void UnlockBuffer(GuestBuffer* buffer, bool useCopyQueue)
{
    auto copyBuffer = [&](T* dest)
        {
            auto src = reinterpret_cast<const T*>(buffer->mappedMemory);

            for (size_t i = 0; i < buffer->dataSize; i += sizeof(T))
            {
                *dest = ByteSwap(*src);
                ++dest;
                ++src;
            }
        };

    if (useCopyQueue && g_capabilities.gpuUploadHeap)
    {
        copyBuffer(reinterpret_cast<T*>(buffer->buffer->map()));
        buffer->buffer->unmap();
    }
#if defined(__SWITCH__)
    else if (!useCopyQueue && buffer->dataSize <= UploadBuffer::SIZE)
    {
        // Staging through the per-frame upload ring instead of creating (and destroying at the
        // end of the frame) a VkBuffer plus a VMA allocation for every unlock.
        auto allocation = g_uploadAllocators[g_frame].allocate(buffer->dataSize, 0x10);
        copyBuffer(reinterpret_cast<T*>(allocation.memory));

        if (g_streamingBuffers)
        {
            StreamBuffer(buffer, allocation);
        }
        else
        {
            g_profilerCopiedBuffers++;

            auto& commandList = g_commandLists[g_frame];

            commandList->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(buffer->buffer.get(), RenderBufferAccess::WRITE));
            commandList->copyBufferRegion(buffer->buffer->at(0), allocation.buffer->at(allocation.offset), buffer->dataSize);
            commandList->barriers(RenderBarrierStage::GRAPHICS, RenderBufferBarrier(buffer->buffer.get(), RenderBufferAccess::READ));
        }
    }
#endif
    else
    {
        auto uploadBuffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(buffer->dataSize));
        copyBuffer(reinterpret_cast<T*>(uploadBuffer->map()));
        uploadBuffer->unmap();

        if (useCopyQueue)
        {
            ExecuteCopyCommandList([&]
                {
                    g_copyCommandList->copyBufferRegion(buffer->buffer->at(0), uploadBuffer->at(0), buffer->dataSize);
                });
        }
        else
        {
            auto& commandList = g_commandLists[g_frame];

            commandList->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(buffer->buffer.get(), RenderBufferAccess::WRITE));
            commandList->copyBufferRegion(buffer->buffer->at(0), uploadBuffer->at(0), buffer->dataSize);
            commandList->barriers(RenderBarrierStage::GRAPHICS, RenderBufferBarrier(buffer->buffer.get(), RenderBufferAccess::READ));

            g_tempBuffers[g_frame].emplace_back(std::move(uploadBuffer));
        }
    }

    g_bufferUploadCount++;
}

template<typename T>
static void UnlockBuffer(GuestBuffer* buffer)
{
    if (!buffer->lockedReadOnly)
    {
        if (IsPresentThread())
        {
            RenderCommand cmd;
            cmd.type = (sizeof(T) == 2) ? RenderCommandType::UnlockBuffer16 : RenderCommandType::UnlockBuffer32;
            cmd.unlockBuffer.buffer = buffer;
            EnqueueRenderCommand(cmd, true);
        }
        else
        {
            UnlockBuffer<T>(buffer, true);
        }
    }
}

static void ProcUnlockBuffer16(const RenderCommand& cmd)
{
    UnlockBuffer<uint16_t>(cmd.unlockBuffer.buffer, false);
}

static void ProcUnlockBuffer32(const RenderCommand& cmd)
{
    UnlockBuffer<uint32_t>(cmd.unlockBuffer.buffer, false);
}

static void UnlockVertexBuffer(GuestBuffer* buffer)
{
    UnlockBuffer<uint32_t>(buffer);
}

static void GetVertexBufferDesc(GuestBuffer* buffer, GuestBufferDesc* desc) 
{
    desc->size = buffer->dataSize;
}

static void* LockIndexBuffer(GuestBuffer* buffer, uint32_t, uint32_t, uint32_t flags) 
{
    return LockBuffer(buffer, flags);
}

static void UnlockIndexBuffer(GuestBuffer* buffer) 
{
    if (buffer->guestFormat == D3DFMT_INDEX32)
        UnlockBuffer<uint32_t>(buffer);
    else
        UnlockBuffer<uint16_t>(buffer);
}

static void GetIndexBufferDesc(GuestBuffer* buffer, GuestBufferDesc* desc)
{
    desc->format = buffer->guestFormat;
    desc->size = buffer->dataSize;
}

static void GetSurfaceDesc(GuestSurface* surface, GuestSurfaceDesc* desc) 
{
    desc->width = surface->width;
    desc->height = surface->height;
}

static void GetVertexDeclaration(GuestVertexDeclaration* vertexDeclaration, GuestVertexElement* vertexElements, be<uint32_t>* count) 
{
    memcpy(vertexElements, vertexDeclaration->vertexElements.get(), vertexDeclaration->vertexElementCount * sizeof(GuestVertexElement));
    *count = vertexDeclaration->vertexElementCount;
}

static uint32_t HashVertexDeclaration(uint32_t vertexDeclaration) 
{
    // Vertex declarations are cached on host side, so the pointer itself can be used.
    return vertexDeclaration;
}

static const char *DeviceTypeName(RenderDeviceType type)
{
    switch (type) 
    {
    case RenderDeviceType::INTEGRATED:
        return "Integrated";
    case RenderDeviceType::DISCRETE:
        return "Discrete";
    case RenderDeviceType::VIRTUAL:
        return "Virtual";
    case RenderDeviceType::CPU:
        return "CPU";
    default:
        return "Unknown";
    }
}

static void DrawProfiler()
{
    bool toggleProfiler = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F1] != 0;

#if defined(__SWITCH__)
    // No F1 key on the console: the profiler follows [Switch] SwitchShowProfiler instead.
    toggleProfiler = false;
    g_profilerVisible = Config::SwitchShowProfiler;
#endif

    if (!g_profilerWasToggled && toggleProfiler)
    {
        g_profilerVisible = !g_profilerVisible;

        GameWindow::SetFullscreenCursorVisibility(App::s_isInit ? g_profilerVisible : true);
    }

    g_profilerWasToggled = toggleProfiler;

    if (!g_profilerVisible)
        return;

    ImFont* font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-M.otf");
    float defaultScale = font->Scale;
    font->Scale = ImGui::GetDefaultFont()->FontSize / font->FontSize;
    ImGui::PushFont(font);

    if (ImGui::Begin("Profiler", &g_profilerVisible))
    {
        g_applicationValues[g_profilerValueIndex] = App::s_deltaTime * 1000.0;

        const double applicationAvg = std::accumulate(g_applicationValues, g_applicationValues + PROFILER_VALUE_COUNT, 0.0) / PROFILER_VALUE_COUNT;
        double gpuFrameAvg = g_gpuFrameProfiler.UpdateAndReturnAverage();
        double presentAvg = g_presentProfiler.UpdateAndReturnAverage();
        double updateDirectorAvg = g_updateDirectorProfiler.UpdateAndReturnAverage();
        double renderDirectorAvg = g_renderDirectorProfiler.UpdateAndReturnAverage();
        double frameFenceAvg = g_frameFenceProfiler.UpdateAndReturnAverage();
        double presentWaitAvg = g_presentWaitProfiler.UpdateAndReturnAverage();
        double swapChainAcquireAvg = g_swapChainAcquireProfiler.UpdateAndReturnAverage();

        if (ImPlot::BeginPlot("Frame Time"))
        {
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 20.0);
            ImPlot::SetupAxis(ImAxis_Y1, "ms", ImPlotAxisFlags_None);
            ImPlot::PlotLine<double>("Application", g_applicationValues, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
            ImPlot::PlotLine<double>("GPU Frame", g_gpuFrameProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
            ImPlot::PlotLine<double>("Present", g_presentProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
            ImPlot::PlotLine<double>("Update Director", g_updateDirectorProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
            ImPlot::PlotLine<double>("Render Director", g_renderDirectorProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
            ImPlot::PlotLine<double>("Frame Fence", g_frameFenceProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
            ImPlot::PlotLine<double>("Present Wait", g_presentWaitProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
            ImPlot::PlotLine<double>("Swap Chain Acquire", g_swapChainAcquireProfiler.values, PROFILER_VALUE_COUNT, 1.0, 0.0, ImPlotLineFlags_None, g_profilerValueIndex);
            ImPlot::EndPlot();
        }

        g_profilerValueIndex = (g_profilerValueIndex + 1) % PROFILER_VALUE_COUNT;

        ImGui::Text("Current Application: %g ms (%g FPS)", App::s_deltaTime * 1000.0, 1.0 / App::s_deltaTime);
        ImGui::Text("Current GPU Frame: %g ms (%g FPS)", g_gpuFrameProfiler.value.load(), 1000.0 / g_gpuFrameProfiler.value.load());
        ImGui::Text("Current Present: %g ms (%g FPS)", g_presentProfiler.value.load(), 1000.0 / g_presentProfiler.value.load());
        ImGui::Text("Current Update Director: %g ms (%g FPS)", g_updateDirectorProfiler.value.load(), 1000.0 / g_updateDirectorProfiler.value.load());
        ImGui::Text("Current Render Director: %g ms (%g FPS)", g_renderDirectorProfiler.value.load(), 1000.0 / g_renderDirectorProfiler.value.load());
        ImGui::Text("Current Frame Fence: %g ms", g_frameFenceProfiler.value.load());
        ImGui::Text("Current Present Wait: %g ms", g_presentWaitProfiler.value.load());
        ImGui::Text("Current Swap Chain Acquire: %g ms", g_swapChainAcquireProfiler.value.load());

        ImGui::NewLine();

        ImGui::Text("Average Application: %g ms (%g FPS)", applicationAvg, 1000.0 / applicationAvg);
        ImGui::Text("Average GPU Frame: %g ms (%g FPS)", gpuFrameAvg, 1000.0 / gpuFrameAvg);
        ImGui::Text("Average Present: %g ms (%g FPS)", presentAvg, 1000.0 / presentAvg);
        ImGui::Text("Average Update Director: %g ms (%g FPS)", updateDirectorAvg, 1000.0 / updateDirectorAvg);
        ImGui::Text("Average Render Director: %g ms (%g FPS)", renderDirectorAvg, 1000.0 / renderDirectorAvg);
        ImGui::Text("Average Frame Fence: %g ms", frameFenceAvg);
        ImGui::Text("Average Present Wait: %g ms", presentWaitAvg);
        ImGui::Text("Average Swap Chain Acquire: %g ms", swapChainAcquireAvg);

        ImGui::NewLine();

        if (g_userHeap.heap != nullptr && g_userHeap.physicalHeap != nullptr)
        {
            O1HeapDiagnostics diagnostics, physicalDiagnostics;
            {
                std::lock_guard lock(g_userHeap.mutex);
                diagnostics = o1heapGetDiagnostics(g_userHeap.heap);
            }
            {
                std::lock_guard lock(g_userHeap.physicalMutex);
                physicalDiagnostics = o1heapGetDiagnostics(g_userHeap.physicalHeap);
            }

            ImGui::Text("Heap Allocated: %d MB", int32_t(diagnostics.allocated / (1024 * 1024)));
            ImGui::Text("Physical Heap Allocated: %d MB", int32_t(physicalDiagnostics.allocated / (1024 * 1024)));
        }

        ImGui::Text("GPU Waits: %d", int32_t(g_waitForGPUCount));
        ImGui::Text("Buffer Uploads: %d", int32_t(g_bufferUploadCount));
        ImGui::NewLine();

        ImGui::Text("Present Wait: %s", g_capabilities.presentWait ? "Supported" : "Unsupported");
        ImGui::Text("Triangle Fan: %s", g_capabilities.triangleFan ? "Supported" : "Unsupported");
        ImGui::Text("Dynamic Depth Bias: %s", g_capabilities.dynamicDepthBias ? "Supported" : "Unsupported");
        ImGui::Text("Triangle Strip Workaround: %s", g_triangleStripWorkaround ? "Enabled" : "Disabled");
        ImGui::Text("Hardware Resolve: %s", g_hardwareResolve ? "Enabled" : "Disabled");
        ImGui::Text("Hardware Depth Resolve: %s", g_hardwareDepthResolve ? "Enabled" : "Disabled");
        ImGui::NewLine();

        ImGui::Text("API: %s", g_vulkan ? "Vulkan" : "D3D12");
        ImGui::Text("Device: %s", g_device->getDescription().name.c_str());
        ImGui::Text("Device Type: %s", DeviceTypeName(g_device->getDescription().type));
        ImGui::Text("VRAM: %.2f MiB", (double)(g_device->getDescription().dedicatedVideoMemory) / (1024.0 * 1024.0));
        ImGui::Text("UMA: %s", g_capabilities.uma ? "Supported" : "Unsupported");
        ImGui::Text("GPU Upload Heap: %s", g_capabilities.gpuUploadHeap ? "Supported" : "Unsupported");

        const char* sdlVideoDriver = SDL_GetCurrentVideoDriver();
        if (sdlVideoDriver != nullptr)
            ImGui::Text("SDL Video Driver: %s", sdlVideoDriver);

        ImGui::NewLine();
        ImGui::Checkbox("Show FPS", &Config::ShowFPS.Value);
        ImGui::NewLine();

        if (ImGui::TreeNode("Device Names"))
        {
            ImGui::Indent();

            uint32_t deviceIndex = 0;
            for (const std::string &deviceName : g_interface->getDeviceNames())
            {
                ImGui::Text("Option #%d: %s", deviceIndex++, deviceName.c_str());
            }

            ImGui::Unindent();
            ImGui::TreePop();
        }
    }
    ImGui::End();

    ImGui::PopFont();
    font->Scale = defaultScale;
}

static void DrawFPS()
{
    if (!Config::ShowFPS)
        return;

    double time = ImGui::GetTime();
    static double updateTime = time;
    static double fps = 0;
    static double totalDeltaTime = 0.0;
    static uint32_t totalDeltaCount = 0;

    totalDeltaTime += g_presentProfiler.value.load();
    totalDeltaCount++;

    if (time - updateTime >= 1.0f)
    {
        fps = 1000.0 / std::max(totalDeltaTime / double(totalDeltaCount), 1.0);
        updateTime = time;
        totalDeltaTime = 0.0;
        totalDeltaCount = 0;
    }

    auto drawList = ImGui::GetBackgroundDrawList();

    auto fmt = fmt::format("FPS: {:.2f}", fps);
    auto font = ImFontAtlasSnapshot::GetFont("FOT-SeuratPro-M.otf");
    auto fontSize = Scale(10);
    auto textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0, fmt.c_str());

    ImVec2 min = { Scale(40), Scale(30) };
    ImVec2 max = { min.x + std::max(Scale(75), textSize.x + Scale(10)), min.y + Scale(15) };
    ImVec2 textPos = { min.x + Scale(2), CENTRE_TEXT_VERT(min, max, textSize) + Scale(0.2f) };

    drawList->AddRectFilled(min, max, IM_COL32(0, 0, 0, 200));
    drawList->AddText(font, fontSize, textPos, IM_COL32_WHITE, fmt.c_str());
}

static void DrawImGui()
{
    ImGui_ImplSDL2_NewFrame();

    auto& io = ImGui::GetIO();
    io.DisplaySize = { float(Video::s_viewportWidth), float(Video::s_viewportHeight) };

    // ImGui doesn't know that we center the screen for specific aspect ratio
    // settings, which causes mouse events to not work correctly. To fix this, 
    // we can adjust the mouse events before ImGui processes them.
    uint32_t width = g_swapChain->getWidth();
    uint32_t height = g_swapChain->getHeight();
    float mousePosScaleX = float(width) / float(GameWindow::s_width);
    float mousePosScaleY = float(height) / float(GameWindow::s_height);
    float mousePosOffsetX = (width - Video::s_viewportWidth) / 2.0f;
    float mousePosOffsetY = (height - Video::s_viewportHeight) / 2.0f;
    for (int i = 0; i < io.Ctx->InputEventsQueue.Size; i++)
    {
        auto& e = io.Ctx->InputEventsQueue[i];
        if (e.Type == ImGuiInputEventType_MousePos)
        {
            if (e.MousePos.PosX != -FLT_MAX)
            {
                e.MousePos.PosX *= mousePosScaleX;
                e.MousePos.PosX -= mousePosOffsetX;
            }

            if (e.MousePos.PosY != -FLT_MAX)
            {
                e.MousePos.PosY *= mousePosScaleY;
                e.MousePos.PosY -= mousePosOffsetY;
            }
        }
    }

    ImGui::NewFrame();

    ResetImGuiCallbacks();

#ifdef ASYNC_PSO_DEBUG
    if (ImGui::Begin("Async PSO Stats"))
    {
        ImGui::Text("Pipelines Created In Render Thread: %d", g_pipelinesCreatedInRenderThread.load());
        ImGui::Text("Pipelines Created Asynchronously: %d", g_pipelinesCreatedAsynchronously.load());
        ImGui::Text("Pipelines Dropped: %d", g_pipelinesDropped.load());
        ImGui::Text("Pipelines Currently Compiling: %d", g_pipelinesCurrentlyCompiling.load());
        ImGui::Text("Compiling Pipeline Task Count: %d", g_compilingPipelineTaskCount.load());
        ImGui::Text("Pending Pipeline Task Count: %d", g_pendingPipelineTaskCount.load());

        std::lock_guard lock(g_debugMutex);
        ImGui::TextUnformatted(g_pipelineDebugText.c_str());
    }
    ImGui::End();
#endif

    AchievementMenu::Draw();
    OptionsMenu::Draw();
    AchievementOverlay::Draw();
    InstallerWizard::Draw();
    MessageWindow::Draw();
    ButtonGuide::Draw();
    Fader::Draw();
    BlackBar::Draw();

    assert(ImGui::GetBackgroundDrawList()->_ClipRectStack.Size == 1 && "Some clip rects were not removed from the stack!");

    DrawFPS();
    DrawProfiler();
    ImGui::Render();

    auto drawData = ImGui::GetDrawData();
    if (drawData->CmdListsCount != 0)
    {
        RenderCommand cmd;
        cmd.type = RenderCommandType::DrawImGui;
        EnqueueRenderCommand(cmd, true);
    }
}

static void SetFramebuffer(GuestSurface *renderTarget, GuestSurface *depthStencil, bool settingForClear);

static void ProcDrawImGui(const RenderCommand& cmd)
{
    // Make sure the backbuffer is the current target.
    AddBarrier(g_backBuffer, RenderTextureLayout::COLOR_WRITE);
    FlushBarriers();
    SetFramebuffer(g_backBuffer, nullptr, false);

    auto& commandList = g_commandLists[g_frame];
    auto pipeline = g_imPipeline.get();

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
    g_constantsUboBinding.boundSet = nullptr;
    InvalidatePushedRootAddresses();
#endif

    commandList->setGraphicsPipelineLayout(g_imPipelineLayout.get());
    commandList->setPipeline(pipeline);
    commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
    commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 1);

    auto& drawData = *ImGui::GetDrawData();
    commandList->setViewports(RenderViewport(drawData.DisplayPos.x, drawData.DisplayPos.y, drawData.DisplaySize.x, drawData.DisplaySize.y));

    ImGuiPushConstants pushConstants{};
    pushConstants.displaySize = drawData.DisplaySize;
    pushConstants.inverseDisplaySize = { 1.0f / drawData.DisplaySize.x, 1.0f / drawData.DisplaySize.y };
    commandList->setGraphicsPushConstants(0, &pushConstants);

    size_t pushConstantRangeMin = ~0;
    size_t pushConstantRangeMax = 0;

    auto setPushConstants = [&](void* destination, const void* source, size_t size)
        {
            bool dirty = memcmp(destination, source, size) != 0;

            memcpy(destination, source, size);

            if (dirty)
            {
                size_t offset = reinterpret_cast<size_t>(destination) - reinterpret_cast<size_t>(&pushConstants);
                pushConstantRangeMin = std::min(pushConstantRangeMin, offset);
                pushConstantRangeMax = std::max(pushConstantRangeMax, offset + size);
            }
        };

    ImRect clipRect{};

    for (int i = 0; i < drawData.CmdListsCount; i++)
    {
        auto& drawList = drawData.CmdLists[i];

        auto vertexBufferAllocation = g_uploadAllocators[g_frame].allocate<false>(drawList->VtxBuffer.Data, drawList->VtxBuffer.Size * sizeof(ImDrawVert), alignof(ImDrawVert));
        auto indexBufferAllocation = g_uploadAllocators[g_frame].allocate<false>(drawList->IdxBuffer.Data, drawList->IdxBuffer.Size * sizeof(uint16_t), alignof(uint16_t));

        const RenderVertexBufferView vertexBufferView(vertexBufferAllocation.buffer->at(vertexBufferAllocation.offset), drawList->VtxBuffer.Size * sizeof(ImDrawVert));
        const RenderInputSlot inputSlot(0, sizeof(ImDrawVert));
        commandList->setVertexBuffers(0, &vertexBufferView, 1, &inputSlot);

        const RenderIndexBufferView indexBufferView(indexBufferAllocation.buffer->at(indexBufferAllocation.offset), drawList->IdxBuffer.Size * sizeof(uint16_t), RenderFormat::R16_UINT);
        commandList->setIndexBuffer(&indexBufferView);

        for (int j = 0; j < drawList->CmdBuffer.Size; j++)
        {
            auto& drawCmd = drawList->CmdBuffer[j];
            if (drawCmd.UserCallback != nullptr)
            {
                auto callbackData = reinterpret_cast<const ImGuiCallbackData*>(drawCmd.UserCallbackData);

                switch (static_cast<ImGuiCallback>(reinterpret_cast<size_t>(drawCmd.UserCallback)))
                {
                case ImGuiCallback::SetGradient:
                    setPushConstants(&pushConstants.boundsMin, &callbackData->setGradient, sizeof(callbackData->setGradient));
                    break;       
                case ImGuiCallback::SetShaderModifier:
                    setPushConstants(&pushConstants.shaderModifier, &callbackData->setShaderModifier, sizeof(callbackData->setShaderModifier));
                    break;
                case ImGuiCallback::SetOrigin:
                    setPushConstants(&pushConstants.origin, &callbackData->setOrigin, sizeof(callbackData->setOrigin));
                    break;
                case ImGuiCallback::SetScale:
                    setPushConstants(&pushConstants.scale, &callbackData->setScale, sizeof(callbackData->setScale));
                    break;       
                case ImGuiCallback::SetMarqueeFade:
                    setPushConstants(&pushConstants.boundsMin, &callbackData->setMarqueeFade, sizeof(callbackData->setMarqueeFade));
                    break;
                case ImGuiCallback::SetOutline:
                    setPushConstants(&pushConstants.outline, &callbackData->setOutline, sizeof(callbackData->setOutline));
                    break;
                case ImGuiCallback::SetProceduralOrigin:
                    setPushConstants(&pushConstants.proceduralOrigin, &callbackData->setProceduralOrigin, sizeof(callbackData->setProceduralOrigin));
                    break;
                case ImGuiCallback::SetAdditive:
                {
                    auto pipelineToSet = callbackData->setAdditive.enabled ? g_imAdditivePipeline.get() : g_imPipeline.get();
                    if (pipeline != pipelineToSet)
                    {
                        commandList->setPipeline(pipelineToSet);
                        pipeline = pipelineToSet;
                    }
                    break;
                }
                default:
                    assert(false && "Unknown ImGui callback type.");
                    break;
                }
            }
            else
            {
                if (drawCmd.ClipRect.z <= drawCmd.ClipRect.x || drawCmd.ClipRect.w <= drawCmd.ClipRect.y)
                    continue;

                auto texture = reinterpret_cast<GuestTexture*>(drawCmd.TextureId);
                uint32_t descriptorIndex = TEXTURE_DESCRIPTOR_NULL_TEXTURE_2D;
                if (texture != nullptr)
                {
                    if (texture->layout != RenderTextureLayout::SHADER_READ)
                    {
                        commandList->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY,
                            RenderTextureBarrier(texture->texture, RenderTextureLayout::SHADER_READ));

                        texture->layout = RenderTextureLayout::SHADER_READ;
                    }

                    descriptorIndex = texture->descriptorIndex;

                    if (texture == g_imFontTexture.get())
                        descriptorIndex |= 0x80000000;

                    setPushConstants(&pushConstants.texture2DDescriptorIndex, &descriptorIndex, sizeof(descriptorIndex));
                }

                if (pushConstantRangeMin < pushConstantRangeMax)
                {
                    commandList->setGraphicsPushConstants(0, reinterpret_cast<const uint8_t*>(&pushConstants) + pushConstantRangeMin, pushConstantRangeMin, pushConstantRangeMax - pushConstantRangeMin);
                    pushConstantRangeMin = ~0;
                    pushConstantRangeMax = 0;
                }

                if (memcmp(&clipRect, &drawCmd.ClipRect, sizeof(clipRect)) != 0)
                {
                    commandList->setScissors(RenderRect(int32_t(drawCmd.ClipRect.x), int32_t(drawCmd.ClipRect.y), int32_t(drawCmd.ClipRect.z), int32_t(drawCmd.ClipRect.w)));
                    clipRect = drawCmd.ClipRect;
                }

                commandList->drawIndexedInstanced(drawCmd.ElemCount, 1, drawCmd.IdxOffset, drawCmd.VtxOffset, 0);
            }
        }
    }
}

// We have to check for this to properly handle the following situation:
// 1. Wait on swap chain.
// 2. Create loading thread.
// 3. Loading thread also waits on swap chain.
// 4. Loading thread presents and quits.
// 5. After the loading thread quits, application also presents.
static bool g_pendingWaitOnSwapChain = true;

#if defined(__SWITCH__)
// Writes the persistent pipeline cache off the main thread. The save itself returns early when
// no pipeline was added since the last one. Requested when a loading screen ends and at most
// once a minute; saving at exit is not an option (closing from HOME kills the process).
static std::mutex g_pipelineCacheSaveMutex;
static std::condition_variable g_pipelineCacheSaveCondition;
static bool g_pipelineCacheSaveRequested = false;
static std::thread* g_pipelineCacheSaveThread = nullptr; // Never destroyed on purpose.

static void RequestPipelineCacheSave()
{
    if (!Config::SwitchPipelineCache || g_device == nullptr)
        return;

    if (g_pipelineCacheSaveThread == nullptr)
    {
        g_pipelineCacheSaveThread = new std::thread([]
            {
                // Stays at the libnx default (0x3B, time-sliced). Not lower: vkGetPipelineCacheData
                // holds the cache lock that pipeline creation on the render thread also takes.

                while (true)
                {
                    {
                        std::unique_lock lock(g_pipelineCacheSaveMutex);
                        g_pipelineCacheSaveCondition.wait(lock, [] { return g_pipelineCacheSaveRequested; });
                        g_pipelineCacheSaveRequested = false;
                    }

                    if (g_device->savePipelineCache())
                        fprintf(stderr, "Pipeline cache saved.\n");
                }
            });
    }

    {
        std::lock_guard lock(g_pipelineCacheSaveMutex);
        g_pipelineCacheSaveRequested = true;
    }

    g_pipelineCacheSaveCondition.notify_one();
}

static void UpdatePipelineCacheSaving()
{
    using namespace std::chrono_literals;

    static bool s_wasLoading = false;
    static auto s_lastRequest = std::chrono::steady_clock::now();

    const bool loading = *SWA::SGlobals::ms_IsLoading;
    const auto now = std::chrono::steady_clock::now();

    // During play, a save holds the driver's pipeline cache lock while pipelines may be created on the
    // render thread; by default it only happens when a loading screen ends (SwitchPipelineCacheSaveDuringPlay).
    const bool periodic = Config::SwitchPipelineCacheSaveDuringPlay && (now - s_lastRequest) >= 60s;
    if ((s_wasLoading && !loading) || periodic)
    {
        RequestPipelineCacheSave();
        s_lastRequest = now;
    }

    s_wasLoading = loading;
}
#endif

void Video::WaitOnSwapChain()
{
#if defined(__SWITCH__)
    // SwitchPresentOnRenderThread: the swap chain belongs to the render thread (and NVK has no present wait).
    if (g_presentOnRenderThread && g_gamePresenting.load(std::memory_order_acquire))
        return;
#endif

    if (g_pendingWaitOnSwapChain)
    {
        if (g_swapChainValid)
        {
            g_presentWaitProfiler.Begin();
            g_swapChain->wait();
            g_presentWaitProfiler.End();
        }

        g_pendingWaitOnSwapChain = false;
    }
}

static bool g_shouldPrecompilePipelines;
static std::atomic<bool> g_executedCommandList;

#if defined(__SWITCH__)
// FrameTime: milliseconds since `since`, which moves on to now.
static double FrameTimeLap(std::chrono::steady_clock::time_point& since)
{
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - since).count();
    since = now;
    return ms;
}

static std::chrono::steady_clock::time_point g_frameTimeLastPresentEnd;
#endif

void Video::Present()
{
#if defined(__SWITCH__)
    auto frameTimeLap = std::chrono::steady_clock::now();
    double frameTimes[FRAME_TIME_COUNT]{};
    if (g_frameTimeLastPresentEnd.time_since_epoch().count() != 0)
        frameTimes[FRAME_TIME_WORK] = std::chrono::duration<double, std::milli>(frameTimeLap - g_frameTimeLastPresentEnd).count();
#endif

    g_readyForCommands = false;

#if defined(__SWITCH__)
    // SwitchPresentOnRenderThread: decided per frame and sent with the command, so both threads agree.
    const bool pipelined = g_presentOnRenderThread && g_gamePresenting.load(std::memory_order_acquire);
#endif

    RenderCommand cmd;
    cmd.type = RenderCommandType::ExecutePendingStretchRectCommands;
    EnqueueRenderCommand(cmd, true);

    DrawImGui();

    cmd.type = RenderCommandType::ExecuteCommandList;
#if defined(__SWITCH__)
    cmd.executeCommandList.pipelined = pipelined;
    if (pipelined)
        g_presentTailsSent.fetch_add(1, std::memory_order_acq_rel);
#endif
    EnqueueRenderCommand(cmd, true);

    // All the shaders are available at this point. We can precompile embedded PSOs then.
    if (g_shouldPrecompilePipelines)
    {
        EnqueuePipelineTask(PipelineTaskType::PrecompilePipelines, {});
        g_shouldPrecompilePipelines = false;
    }

#if defined(__SWITCH__)
    if (pipelined)
    {
        // The render thread has read everything this frame sent (this thread's copies of shader constants and
        // vertices included); it submits, presents and gets the next frame ready (PresentOnRenderThread).
        g_recordedCommandList.wait(false);
        g_recordedCommandList = false;
        frameTimes[FRAME_TIME_RENDER_THREAD] = FrameTimeLap(frameTimeLap);

        os::switch_overlay::OnPresent(Video::s_viewportWidth, Video::s_viewportHeight);
        UpdatePipelineCacheSaving();
        g_intermediaryUploadAllocator.reset();
    }
    else
    {
#endif
    g_executedCommandList.wait(false);
    g_executedCommandList = false;
#if defined(__SWITCH__)
    frameTimes[FRAME_TIME_RENDER_THREAD] = FrameTimeLap(frameTimeLap);
#endif

    if (g_swapChainValid)
    {
        if (g_pendingWaitOnSwapChain)
        {
            g_presentWaitProfiler.Begin();
            g_swapChain->wait(); // Never gonna happen outside loading threads as explained above.
            g_presentWaitProfiler.End();
        }

        RenderCommandSemaphore* signalSemaphores[] = { g_renderSemaphores[g_frame].get() };
        g_swapChainValid = g_swapChain->present(g_backBufferIndex, signalSemaphores, std::size(signalSemaphores));
    }

#if defined(__SWITCH__)
    // FPS, frame times and render resolution for Status Monitor / SaltyNX overlays.
    os::switch_overlay::OnPresent(Video::s_viewportWidth, Video::s_viewportHeight);
#endif

    g_pendingWaitOnSwapChain = true;

#if defined(__SWITCH__)
    UpdatePipelineCacheSaving();
    frameTimes[FRAME_TIME_PRESENT] = FrameTimeLap(frameTimeLap);
#endif

    g_frame = g_nextFrame;
    g_nextFrame = (g_frame + 1) % NUM_FRAMES;

    if (g_commandListStates[g_frame])
    {
        g_frameFenceProfiler.Begin();
        g_queue->waitForCommandFence(g_commandFences[g_frame].get());
        g_frameFenceProfiler.End();
        g_commandListStates[g_frame] = false;

        // Update the GPU profiler with the results from the timestamps of the frame.
        g_queryPools[g_frame]->queryResults();
        const uint64_t *frameTimestamps = g_queryPools[g_frame]->getResults();
        double gpuFrameTime = double(frameTimestamps[1] - frameTimestamps[0]) / 1000000.0;
#if defined(__SWITCH__)
        // NVK on Horizon reports timestampPeriod = 1 ns, but one tick is ~1.627 ns (measured
        // against wall-clock time by nfsmw-nx, docs/measuring.md). Without this the profiler
        // shows ~60 % of the real GPU time.
        gpuFrameTime *= 1.627;
        PassProfilerCollect(gpuFrameTime);
#endif
        g_gpuFrameProfiler.Set(gpuFrameTime);
    }

    g_dirtyStates = DirtyStates(true);
    g_uploadAllocators[g_frame].reset();
    g_intermediaryUploadAllocator.reset();
    g_triangleFanIndexData.reset();
    g_quadIndexData.reset();

#if defined(__SWITCH__)
    frameTimes[FRAME_TIME_GPU] = FrameTimeLap(frameTimeLap);
#endif
    CheckSwapChain();
#if defined(__SWITCH__)
    frameTimes[FRAME_TIME_ACQUIRE] = FrameTimeLap(frameTimeLap);
    }
#endif

    cmd.type = RenderCommandType::BeginCommandList;
    EnqueueRenderCommand(cmd, true);

    if (Config::FPS >= FPS_MIN && Config::FPS < FPS_MAX)
    {
        using namespace std::chrono_literals;

        static std::chrono::steady_clock::time_point s_next;

        auto now = std::chrono::steady_clock::now();

        if (now < s_next)
        {
#if defined(__SWITCH__)
            // svcSleepThread is precise to microseconds; the 2ms yield spin
            // below both burns a shared core and loses to the scheduler.
            std::this_thread::sleep_until(s_next);
            now = std::chrono::steady_clock::now();
#else
            std::this_thread::sleep_for(std::chrono::floor<std::chrono::milliseconds>(s_next - now - 2ms));

            while ((now = std::chrono::steady_clock::now()) < s_next)
                std::this_thread::yield();
#endif
        }
        else
        {
            s_next = now;
        }

        s_next += 1000000000ns / Config::FPS;
    }

#if defined(__SWITCH__)
    os::switch_stall_watch::OnFrame();

    frameTimes[FRAME_TIME_LIMITER] = FrameTimeLap(frameTimeLap);
    g_frameTimeLastPresentEnd = frameTimeLap;
    if (frameTimes[FRAME_TIME_WORK] != 0.0)
    {
        std::lock_guard lock(g_frameTimesMutex);
        for (uint32_t i = 0; i < FRAME_TIME_COUNT; i++)
            g_profilerFrameTimes[i] += frameTimes[i];
        g_profilerLongestWork = std::max(g_profilerLongestWork, frameTimes[FRAME_TIME_WORK]);
        g_profilerFrameTimeFrames++;
    }
#endif

    g_presentProfiler.Reset();
}

void Video::StartPipelinePrecompilation()
{
    g_shouldPrecompilePipelines = true;
}

static void SetRootDescriptor(const UploadAllocation& allocation, size_t index)
{
    auto& commandList = g_commandLists[g_frame];

    if (g_vulkan)
        commandList->setGraphicsPushConstants(0, &allocation.deviceAddress, 8 * index, 8);
    else
        commandList->setGraphicsRootDescriptor(allocation.buffer->at(allocation.offset), index);
}

static void SetRootDescriptorForDraw(const UploadAllocation& allocation, size_t index)
{
#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
    if (g_constantsUbo)
    {
        g_pendingRootAddresses[index] = allocation.deviceAddress;
        return;
    }
#endif

    SetRootDescriptor(allocation, index);
}

#if defined(__SWITCH__)
// SwitchPresentOnRenderThread: what Present does after the frame is submitted, on the render thread right after
// the submission (the D3D thread has gone on to its next frame): present the image, move to the next frame slot
// and wait for the GPU to finish the frame that used it last, reset that slot's allocators and the render state,
// acquire the next image. The next frame's commands (BeginCommandList first) wait in the queue meanwhile.
static void PresentOnRenderThread()
{
    auto lap = std::chrono::steady_clock::now();
    double times[FRAME_TIME_COUNT]{};

    if (g_swapChainValid)
    {
        RenderCommandSemaphore* signalSemaphores[] = { g_renderSemaphores[g_frame].get() };
        g_swapChainValid = g_swapChain->present(g_backBufferIndex, signalSemaphores, std::size(signalSemaphores));
    }
    times[FRAME_TIME_PRESENT] = FrameTimeLap(lap);

    g_frame = g_nextFrame;
    g_nextFrame = (g_frame + 1) % NUM_FRAMES;

    if (g_commandListStates[g_frame])
    {
        g_frameFenceProfiler.Begin();
        g_queue->waitForCommandFence(g_commandFences[g_frame].get());
        g_frameFenceProfiler.End();
        g_commandListStates[g_frame] = false;

        // Update the GPU profiler with the results from the timestamps of the frame (see Present).
        g_queryPools[g_frame]->queryResults();
        const uint64_t* frameTimestamps = g_queryPools[g_frame]->getResults();
        const double gpuFrameTime = double(frameTimestamps[1] - frameTimestamps[0]) / 1000000.0 * 1.627;
        PassProfilerCollect(gpuFrameTime);
        g_gpuFrameProfiler.Set(gpuFrameTime);
    }

    g_dirtyStates = DirtyStates(true);
    g_uploadAllocators[g_frame].reset();
    g_triangleFanIndexData.reset();
    g_quadIndexData.reset();
    times[FRAME_TIME_GPU] = FrameTimeLap(lap);

    CheckSwapChain();
    times[FRAME_TIME_ACQUIRE] = FrameTimeLap(lap);

    {
        std::lock_guard lock(g_frameTimesMutex);
        for (uint32_t i = 0; i < FRAME_TIME_COUNT; i++)
            g_profilerFrameTimes[i] += times[i];
    }

    g_presentTailsDone.fetch_add(1, std::memory_order_acq_rel);
    g_presentTailsDone.notify_all();
}
#endif

static void ProcExecuteCommandList(const RenderCommand& cmd)
{    
    if (g_swapChainValid)
    {
        auto swapChainTexture = g_swapChain->getTexture(g_backBufferIndex);
        if (g_backBuffer->texture == g_intermediaryBackBufferTexture.get())
        {
            struct
            {
                float gammaR;
                float gammaG;
                float gammaB;
                uint32_t textureDescriptorIndex;

                int32_t viewportOffsetX;
                int32_t viewportOffsetY;
                int32_t viewportWidth;
                int32_t viewportHeight;
            } constants;

            if (Config::XboxColorCorrection)
            {
                constants.gammaR = 1.2f;
                constants.gammaG = 1.17f;
                constants.gammaB = 0.98f;
            }
            else
            {
                constants.gammaR = 1.0f;
                constants.gammaG = 1.0f;
                constants.gammaB = 1.0f;
            }

            float offset = (Config::Brightness - 0.5f) * 1.2f;

            constants.gammaR = 1.0f / std::clamp(constants.gammaR + offset, 0.1f, 4.0f);
            constants.gammaG = 1.0f / std::clamp(constants.gammaG + offset, 0.1f, 4.0f);
            constants.gammaB = 1.0f / std::clamp(constants.gammaB + offset, 0.1f, 4.0f);
            constants.textureDescriptorIndex = g_intermediaryBackBufferTextureDescriptorIndex;

            constants.viewportOffsetX = (int32_t(g_swapChain->getWidth()) - int32_t(Video::s_viewportWidth)) / 2;
            constants.viewportOffsetY = (int32_t(g_swapChain->getHeight()) - int32_t(Video::s_viewportHeight)) / 2;
            constants.viewportWidth = Video::s_viewportWidth;
            constants.viewportHeight = Video::s_viewportHeight;

            auto &framebuffer = g_backBuffer->framebuffers[swapChainTexture];
            if (!framebuffer)
            {
                RenderFramebufferDesc desc;
                desc.colorAttachments = const_cast<const RenderTexture **>(&swapChainTexture);
                desc.colorAttachmentsCount = 1;
                framebuffer = g_device->createFramebuffer(desc);
            }

            RenderTextureBarrier srcBarriers[] =
            {
                RenderTextureBarrier(g_intermediaryBackBufferTexture.get(), RenderTextureLayout::SHADER_READ),
                RenderTextureBarrier(swapChainTexture, RenderTextureLayout::COLOR_WRITE)
            };

            auto &commandList = g_commandLists[g_frame];
            commandList->barriers(RenderBarrierStage::GRAPHICS, srcBarriers, std::size(srcBarriers));
            commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
            commandList->setPipeline(g_gammaCorrectionPipeline.get());
            commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
            SetRootDescriptor(g_uploadAllocators[g_frame].allocate<false>(&constants, sizeof(constants), 0x100), 2);
            commandList->setFramebuffer(framebuffer.get());
            commandList->setViewports(RenderViewport(0.0f, 0.0f, g_swapChain->getWidth(), g_swapChain->getHeight()));
            commandList->setScissors(RenderRect(0, 0, g_swapChain->getWidth(), g_swapChain->getHeight()));
#if defined(__SWITCH__)
            // [Switch] SwitchSingleCopyTriangle: copy_vs builds one triangle over the whole target from
            // vertices 0-2; vertices 3-5 made a second one that covered half of it again.
            commandList->drawInstanced(g_singleCopyTriangle ? 3 : 6, 1, 0, 0);
#else
            commandList->drawInstanced(6, 1, 0, 0);
#endif
            commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(swapChainTexture, RenderTextureLayout::PRESENT));
        }
        else
        {
            AddBarrier(g_backBuffer, RenderTextureLayout::PRESENT);
            FlushBarriers();
        }
    }

    auto &commandList = g_commandLists[g_frame];
#if defined(__SWITCH__)
    FlushStreamingBuffers(commandList.get());
    PassProfilerEndFrame();
    FrameLogEnd();
#endif
    commandList->writeTimestamp(g_queryPools[g_frame].get(), 1);
    commandList->end();
#if defined(__SWITCH__)
    g_commandListOpen = false;

    // SwitchPresentOnRenderThread: every command of the frame is recorded; the D3D thread may go on.
    const bool pipelined = cmd.executeCommandList.pipelined;
    if (pipelined)
    {
        g_recordedCommandList = true;
        g_recordedCommandList.notify_one();
    }
#endif

    if (g_swapChainValid)
    {
        const RenderCommandList *commandLists[] = { commandList.get() };
        RenderCommandSemaphore *waitSemaphores[] = { g_acquireSemaphores[g_frame].get() };
        RenderCommandSemaphore *signalSemaphores[] = { g_renderSemaphores[g_frame].get() };

        g_queue->executeCommandLists(
            commandLists, std::size(commandLists),
            waitSemaphores, std::size(waitSemaphores),
            signalSemaphores, std::size(signalSemaphores),
            g_commandFences[g_frame].get());
    }
    else
    {
        g_queue->executeCommandLists(commandList.get(), g_commandFences[g_frame].get());
    }

    g_commandListStates[g_frame] = true;

#if defined(__SWITCH__)
    if (pipelined)
    {
        PresentOnRenderThread();
        return;
    }
#endif

    g_executedCommandList = true;
    g_executedCommandList.notify_one();
}

static void ProcBeginCommandList(const RenderCommand& cmd)
{
    DestructTempResources();
    BeginCommandList();
#if defined(__SWITCH__)
    FrameLogBegin();
    g_commandListOpen = true;
    g_vertexConstantBytesUploaded = 0;
    g_pixelConstantBytesUploaded = 0;
    g_lastColorSurface = nullptr;
    g_lastDepthSurface = nullptr;
    g_deadCopyWaitBudgetUs = DEAD_COPY_WAIT_FRAME_US;
#endif

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
    // New command buffer: nothing is bound, and every block is re-uploaded (all states are dirty).
    g_constantsUboBinding = {};
    InvalidatePushedRootAddresses();
#endif

#if defined(__SWITCH__)
    static bool s_reportedNvkDifference = false;
    if (!s_reportedNvkDifference)
    {
        const NvkSwitchSet4Header* set4 = &nvk_switch_set4;
        const NvkSwitchDraw* draw = &nvk_switch_dibujo;
        bool disabled = set4 != nullptr && set4->version == 1 && __atomic_load_n(&set4->disabled, __ATOMIC_RELAXED) != 0;
        for (size_t i = 0; draw != nullptr && draw->version == 1 && i < 5; i++)
            disabled |= __atomic_load_n(&draw->improvements[i].disabled, __ATOMIC_RELAXED) != 0;

        if (disabled)
        {
            s_reportedNvkDifference = true;
            fprintf(stderr, "NVK fast paths: a driver self-check saw a difference and turned its path off for this session.\n");
        }
    }
#endif
}

static GuestSurface* GetBackBuffer() 
{
    g_backBuffer->AddRef();
    return g_backBuffer;
}

void Video::ComputeViewportDimensions()
{
    uint32_t width = g_swapChain->getWidth();
    uint32_t height = g_swapChain->getHeight();
    float aspectRatio = float(width) / float(height);

    switch (Config::AspectRatio)
    {
    case EAspectRatio::Wide:
    {
        if (aspectRatio > WIDE_ASPECT_RATIO)
        {
            s_viewportWidth = height * 16 / 9;
            s_viewportHeight = height;
        }
        else
        {
            s_viewportWidth = width;
            s_viewportHeight = width * 9 / 16;
        }

        break;
    }

    case EAspectRatio::Narrow:
    case EAspectRatio::OriginalNarrow:
    {
        if (aspectRatio > NARROW_ASPECT_RATIO)
        {
            s_viewportWidth = height * 4 / 3;
            s_viewportHeight = height;
        }
        else
        {
            s_viewportWidth = width;
            s_viewportHeight = width * 3 / 4;
        }

        break;
    }

    default:
        s_viewportWidth = width;
        s_viewportHeight = height;
        break;
    }

    AspectRatioPatches::ComputeOffsets();
}

static RenderFormat ConvertFormat(uint32_t format)
{
    switch (format)
    {
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_A16B16G16R16F_2:
        return RenderFormat::R16G16B16A16_FLOAT;
    case D3DFMT_A8B8G8R8:
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        return RenderFormat::R8G8B8A8_UNORM;
    case D3DFMT_D24FS8:
    case D3DFMT_D24S8:
        return RenderFormat::D32_FLOAT;
    case D3DFMT_G16R16F:
    case D3DFMT_G16R16F_2:
        return RenderFormat::R16G16_FLOAT;
    case D3DFMT_INDEX16:
        return RenderFormat::R16_UINT;
    case D3DFMT_INDEX32:
        return RenderFormat::R32_UINT;
    case D3DFMT_L8:
    case D3DFMT_L8_2:
        return RenderFormat::R8_UNORM;
    default:
        assert(false && "Unknown format");
        return RenderFormat::R16G16B16A16_FLOAT;
    }
}

static GuestTexture* CreateTexture(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool, uint32_t type) 
{
    const auto texture = g_userHeap.AllocPhysical<GuestTexture>(type == 17 ? ResourceType::VolumeTexture : ResourceType::Texture);

    RenderTextureDesc desc;
    desc.dimension = texture->type == ResourceType::VolumeTexture ? RenderTextureDimension::TEXTURE_3D : RenderTextureDimension::TEXTURE_2D;
    desc.width = width;
    desc.height = height;
    desc.depth = depth;
    desc.mipLevels = levels;
    desc.arraySize = 1;
    desc.format = ConvertFormat(format);

    if (desc.format == RenderFormat::D32_FLOAT)
        desc.flags = RenderTextureFlag::DEPTH_TARGET;
    else if (usage != 0)
        desc.flags = RenderTextureFlag::RENDER_TARGET;
    else
        desc.flags = RenderTextureFlag::NONE;

    texture->textureHolder = g_device->createTexture(desc);
    texture->texture = texture->textureHolder.get();

    RenderTextureViewDesc viewDesc;
    viewDesc.format = desc.format;
    viewDesc.dimension = texture->type == ResourceType::VolumeTexture ? RenderTextureViewDimension::TEXTURE_3D : RenderTextureViewDimension::TEXTURE_2D;
    viewDesc.mipLevels = levels;

    switch (format)
    {
    case D3DFMT_D24FS8:
    case D3DFMT_D24S8:
    case D3DFMT_L8:
    case D3DFMT_L8_2:
        viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::R, RenderSwizzle::R, RenderSwizzle::R, RenderSwizzle::ONE);
        break;

    case D3DFMT_X8R8G8B8:
        viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::G, RenderSwizzle::B, RenderSwizzle::A, RenderSwizzle::ONE);
        break;
    }

    texture->textureView = texture->texture->createTextureView(viewDesc);

    texture->width = width;
    texture->height = height;
    texture->depth = depth;
    texture->format = desc.format;
    texture->viewDimension = viewDesc.dimension;
    texture->descriptorIndex = g_textureDescriptorAllocator.allocate();

    SetTextureDescriptor(g_textureDescriptorSet, texture->descriptorIndex, texture->texture, width, height, RenderTextureLayout::SHADER_READ, texture->textureView.get(),
        texture->type == ResourceType::VolumeTexture ? 0 : levels);

#if defined(__SWITCH__)
    // A single-level 2D render target texture is created exactly like a surface of its format and size
    // (same flags, hence the same image), so a surface's image can take its place (TryResolveHandOver).
    texture->viewDesc = viewDesc;
    texture->handOverCapable = texture->type == ResourceType::Texture && levels == 1 && depth <= 1 &&
        desc.flags != RenderTextureFlag::NONE;
#endif
   
#ifdef _DEBUG 
    texture->texture->setName(fmt::format("Texture {:X}", g_memory.MapVirtual(texture)));
#endif

    return texture;
}

static RenderHeapType GetBufferHeapType()
{
    return g_capabilities.gpuUploadHeap ? RenderHeapType::GPU_UPLOAD : RenderHeapType::DEFAULT;
}

static GuestBuffer* CreateVertexBuffer(uint32_t length) 
{
    auto buffer = g_userHeap.AllocPhysical<GuestBuffer>(ResourceType::VertexBuffer);
    buffer->buffer = g_device->createBuffer(RenderBufferDesc::VertexBuffer(length, GetBufferHeapType(), RenderBufferFlag::INDEX));
    buffer->dataSize = length;
#ifdef _DEBUG 
    buffer->buffer->setName(fmt::format("Vertex Buffer {:X}", g_memory.MapVirtual(buffer)));
#endif
    return buffer;
}

static GuestBuffer* CreateIndexBuffer(uint32_t length, uint32_t, uint32_t format)
{
    auto buffer = g_userHeap.AllocPhysical<GuestBuffer>(ResourceType::IndexBuffer);
    buffer->buffer = g_device->createBuffer(RenderBufferDesc::IndexBuffer(length, GetBufferHeapType()));
    buffer->dataSize = length;
    buffer->format = ConvertFormat(format);
    buffer->guestFormat = format;
#ifdef _DEBUG 
    buffer->buffer->setName(fmt::format("Index Buffer {:X}", g_memory.MapVirtual(buffer)));
#endif
    return buffer;
}

static GuestSurface* CreateSurface(uint32_t width, uint32_t height, uint32_t format, uint32_t multiSample) 
{
    RenderTextureDesc desc;
    desc.dimension = RenderTextureDimension::TEXTURE_2D;
    desc.width = width;
    desc.height = height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.multisampling.sampleCount = multiSample != 0 && Config::AntiAliasing != EAntiAliasing::None ? int32_t(Config::AntiAliasing.Value) : RenderSampleCount::COUNT_1;
    desc.format = ConvertFormat(format);
    desc.flags = desc.format == RenderFormat::D32_FLOAT ? RenderTextureFlag::DEPTH_TARGET : RenderTextureFlag::RENDER_TARGET;

    auto surface = g_userHeap.AllocPhysical<GuestSurface>(desc.format == RenderFormat::D32_FLOAT ? 
        ResourceType::DepthStencil : ResourceType::RenderTarget);

    surface->textureHolder = g_device->createTexture(desc);
    surface->texture = surface->textureHolder.get();
    surface->width = width;
    surface->height = height;
    surface->format = desc.format;
    surface->guestFormat = format;
    surface->sampleCount = desc.multisampling.sampleCount;

    RenderTextureViewDesc viewDesc;
    viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
    viewDesc.format = desc.format;
    viewDesc.mipLevels = 1;
    surface->textureView = surface->textureHolder->createTextureView(viewDesc);
    surface->descriptorIndex = g_textureDescriptorAllocator.allocate();
    SetTextureDescriptor(g_textureDescriptorSet, surface->descriptorIndex, surface->textureHolder.get(), width, height, RenderTextureLayout::SHADER_READ, surface->textureView.get(),
        desc.multisampling.sampleCount == RenderSampleCount::COUNT_1 ? 1 : 0);

#if defined(__SWITCH__)
    g_liveSurfaces.emplace(surface);
#endif

#ifdef _DEBUG 
    surface->texture->setName(fmt::format("{} {:X}", desc.flags & RenderTextureFlag::RENDER_TARGET ? "Render Target" : "Depth Stencil", g_memory.MapVirtual(surface)));
#endif

    return surface;
}

static void FlushViewport()
{
    auto& commandList = g_commandLists[g_frame];

    if (g_dirtyStates.viewport)
    {
        auto viewport = g_viewport;

        if (viewport.minDepth > viewport.maxDepth)
            std::swap(viewport.minDepth, viewport.maxDepth);

        commandList->setViewports(viewport);

        g_dirtyStates.viewport = false;
    }

    if (g_dirtyStates.scissorRect)
    {
        auto scissorRect = g_scissorTestEnable ? g_scissorRect : RenderRect(
            g_viewport.x,
            g_viewport.y,
            g_viewport.x + g_viewport.width,
            g_viewport.y + g_viewport.height);

        commandList->setScissors(scissorRect);

        g_dirtyStates.scissorRect = false;
    }
}

static void StretchRect(GuestDevice* device, uint32_t flags, uint32_t, GuestTexture* texture)
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::StretchRect;
    cmd.stretchRect.flags = flags;
    cmd.stretchRect.texture = texture;
    EnqueueRenderCommand(cmd);
}

static void SetTextureInRenderThread(uint32_t index, GuestTexture* texture);
static void SetSurface(uint32_t index, GuestSurface* surface);

static void ProcStretchRect(const RenderCommand& cmd)
{
    const auto& args = cmd.stretchRect;

    const bool isDepthStencil = (args.flags & 0x4) != 0;
    const auto surface = isDepthStencil ? g_depthStencil : g_renderTarget;
#if defined(__SWITCH__)
    FrameLogResolve(surface, args.texture);
#endif

    // Erase previous pending command so it doesn't cause the texture to be overriden.
    if (args.texture->sourceSurface != nullptr)
        args.texture->sourceSurface->destinationTextures.erase(args.texture);

    args.texture->sourceSurface = surface;
    surface->destinationTextures.emplace(args.texture);
#if defined(__SWITCH__)
    args.texture->pendingCarried = false;
#endif

    // If the texture is assigned to any slots, set it again. This'll also push the barrier.
    for (uint32_t i = 0; i < std::size(g_textures); i++)
    {
        if (g_textures[i] == args.texture)
        {
            // Set the original texture for MSAA textures as they always get resolved.
            if (surface->sampleCount != RenderSampleCount::COUNT_1)
            {
                SetTextureInRenderThread(i, args.texture);
                g_pendingMsaaResolves.emplace(surface);
            }
            else
            {
                SetSurface(i, surface);
            }
        }
    }

    // Remember to clear later.
    g_pendingSurfaceCopies.emplace(surface);
}

static void SetDefaultViewport(GuestDevice* device, GuestSurface* surface)
{
    if (surface != nullptr)
    {
        RenderCommand cmd;
        cmd.type = RenderCommandType::SetViewport;
        cmd.setViewport.x = 0.0f;
        cmd.setViewport.y = 0.0f;
        cmd.setViewport.width = float(surface->width);
        cmd.setViewport.height = float(surface->height);
        cmd.setViewport.minDepth = 0.0f;
        cmd.setViewport.maxDepth = 1.0f;
        EnqueueRenderCommand(cmd);

        device->viewport.x = 0.0f;
        device->viewport.y = 0.0f;
        device->viewport.width = float(surface->width);
        device->viewport.height = float(surface->height);
        device->viewport.minZ = 0.0f;
        device->viewport.maxZ = 1.0f;
    }
}

static void SetRenderTarget(GuestDevice* device, uint32_t index, GuestSurface* renderTarget) 
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::SetRenderTarget;
    cmd.setRenderTarget.renderTarget = renderTarget;
    EnqueueRenderCommand(cmd);

    SetDefaultViewport(device, renderTarget);
}

static void ProcSetRenderTarget(const RenderCommand& cmd)
{
    const auto& args = cmd.setRenderTarget;

    SetDirtyValue(g_dirtyStates.renderTargetAndDepthStencil, g_renderTarget, args.renderTarget);
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.renderTargetFormat, args.renderTarget != nullptr ? args.renderTarget->format : RenderFormat::UNKNOWN);
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.sampleCount, args.renderTarget != nullptr ? args.renderTarget->sampleCount : RenderSampleCount::COUNT_1);

    // When alpha to coverage is enabled, update the alpha test mode as it's dependent on sample count.
    SetAlphaTestMode((g_pipelineState.specConstants & (SPEC_CONSTANT_ALPHA_TEST | SPEC_CONSTANT_ALPHA_TO_COVERAGE)) != 0);
}

static void SetDepthStencilSurface(GuestDevice* device, GuestSurface* depthStencil) 
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::SetDepthStencilSurface;
    cmd.setDepthStencilSurface.depthStencil = depthStencil;
    EnqueueRenderCommand(cmd);

    SetDefaultViewport(device, depthStencil);
}

static void ProcSetDepthStencilSurface(const RenderCommand& cmd)
{
    const auto& args = cmd.setDepthStencilSurface;

    SetDirtyValue(g_dirtyStates.renderTargetAndDepthStencil, g_depthStencil, args.depthStencil);
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.depthStencilFormat, args.depthStencil != nullptr ? args.depthStencil->format : RenderFormat::UNKNOWN);
}

static bool PopulateBarriersForStretchRect(GuestSurface* renderTarget, GuestSurface* depthStencil)
{
    bool addedAny = false;

    for (const auto surface : { renderTarget, depthStencil })
    {
        if (surface != nullptr && !surface->destinationTextures.empty())
        {
            const bool multiSampling = surface->sampleCount != RenderSampleCount::COUNT_1;

            RenderTextureLayout srcLayout;
            RenderTextureLayout dstLayout;
            bool shaderResolve = true;

            if (multiSampling && g_hardwareResolve)
            {
                // Hardware depth resolve is only supported on D3D12 when programmable sample positions are available.
                bool hardwareDepthResolveAvailable = g_hardwareDepthResolve && !g_vulkan && g_capabilities.sampleLocations;

                if (surface->format != RenderFormat::D32_FLOAT || hardwareDepthResolveAvailable)
                {
                    srcLayout = RenderTextureLayout::RESOLVE_SOURCE;
                    dstLayout = RenderTextureLayout::RESOLVE_DEST;
                    shaderResolve = false;
                }
            }
            
            if (shaderResolve)
            {
                srcLayout = RenderTextureLayout::SHADER_READ;
                dstLayout = (surface->format == RenderFormat::D32_FLOAT ? RenderTextureLayout::DEPTH_WRITE : RenderTextureLayout::COLOR_WRITE);
            }

            AddBarrier(surface, srcLayout);

            for (const auto texture : surface->destinationTextures)
                AddBarrier(texture, dstLayout);

            addedAny = true;
        }
    }

    return addedAny;
}

static void ExecutePendingStretchRectCommands(GuestSurface* renderTarget, GuestSurface* depthStencil)
{
    auto& commandList = g_commandLists[g_frame];

    for (const auto surface : { renderTarget, depthStencil })
    {
        if (surface != nullptr && !surface->destinationTextures.empty())
        {
            const bool multiSampling = surface->sampleCount != RenderSampleCount::COUNT_1;

            for (const auto texture : surface->destinationTextures)
            {
#if defined(__SWITCH__)
                PassProfilerCopy(surface, texture);
                texture->pendingCarried = false;
#endif
                bool shaderResolve = true;

                if (multiSampling && g_hardwareResolve)
                {
                    bool hardwareDepthResolveAvailable = g_hardwareDepthResolve && !g_vulkan && g_capabilities.sampleLocations;

                    if (surface->format != RenderFormat::D32_FLOAT || hardwareDepthResolveAvailable)
                    {
                        if (surface->format == RenderFormat::D32_FLOAT)
                            commandList->resolveTextureRegion(texture->texture, 0, 0, surface->texture, nullptr, RenderResolveMode::MIN);
                        else
                            commandList->resolveTexture(texture->texture, surface->texture);

                        shaderResolve = false;
                    }
                }

                if (shaderResolve)
                {
                    RenderPipeline* pipeline = nullptr;

                    if (multiSampling)
                    {
                        uint32_t pipelineIndex = 0;

                        switch (surface->sampleCount)
                        {
                        case RenderSampleCount::COUNT_2:
                            pipelineIndex = 0;
                            break;
                        case RenderSampleCount::COUNT_4:
                            pipelineIndex = 1;
                            break;
                        case RenderSampleCount::COUNT_8:
                            pipelineIndex = 2;
                            break;
                        default:
                            assert(false && "Unsupported MSAA sample count");
                            break;
                        }

                        if (texture->format == RenderFormat::D32_FLOAT)
                        {
                            pipeline = g_resolveMsaaDepthPipelines[pipelineIndex].get();
                        }
                        else
                        {
                            auto& resolveMsaaColorPipeline = g_resolveMsaaColorPipelines[surface->format][pipelineIndex];
                            if (resolveMsaaColorPipeline == nullptr)
                            {
                                RenderGraphicsPipelineDesc desc;
                                desc.pipelineLayout = g_pipelineLayout.get();
                                desc.vertexShader = g_copyShader.get();
                                desc.pixelShader = g_resolveMsaaColorShaders[pipelineIndex].get();
                                desc.renderTargetFormat[0] = texture->format;
                                desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
                                desc.renderTargetCount = 1;
                                resolveMsaaColorPipeline = g_device->createGraphicsPipeline(desc);
                            }

                            pipeline = resolveMsaaColorPipeline.get();
                        }
                    }
                    else
                    {
                        if (texture->format == RenderFormat::D32_FLOAT)
                        {
                            pipeline = g_copyDepthPipeline.get();
                        }
                        else
                        {
                            auto& copyColorPipeline = g_copyColorPipelines[surface->format];
                            if (copyColorPipeline == nullptr)
                            {
                                RenderGraphicsPipelineDesc desc;
                                desc.pipelineLayout = g_pipelineLayout.get();
                                desc.vertexShader = g_copyShader.get();
                                desc.pixelShader = g_copyColorShader.get();
                                desc.renderTargetFormat[0] = texture->format;
                                desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
                                desc.renderTargetCount = 1;
                                copyColorPipeline = g_device->createGraphicsPipeline(desc);
                            }

                            pipeline = copyColorPipeline.get();
                        }
                    }

                    if (texture->framebuffer == nullptr)
                    {
                        if (texture->format == RenderFormat::D32_FLOAT)
                        {
                            RenderFramebufferDesc desc;
                            desc.depthAttachment = texture->texture;
                            texture->framebuffer = g_device->createFramebuffer(desc);
                        }
                        else
                        {
                            RenderFramebufferDesc desc;
                            desc.colorAttachments = const_cast<const RenderTexture**>(&texture->texture);
                            desc.colorAttachmentsCount = 1;
                            texture->framebuffer = g_device->createFramebuffer(desc);
                        }
                    }

                    if (g_framebuffer != texture->framebuffer.get())
                    {
                        commandList->setFramebuffer(texture->framebuffer.get());
                        g_framebuffer = texture->framebuffer.get();
#if defined(__SWITCH__)
                        g_profilerFramebufferChanges++;
#endif
                    }

                    commandList->setPipeline(pipeline);
                    commandList->setViewports(RenderViewport(0.0f, 0.0f, float(texture->width), float(texture->height), 0.0f, 1.0f));
                    commandList->setScissors(RenderRect(0, 0, texture->width, texture->height));
                    commandList->setGraphicsPushConstants(0, &surface->descriptorIndex, 0, sizeof(uint32_t));
#if defined(__SWITCH__)
                    commandList->drawInstanced(g_singleCopyTriangle ? 3 : 6, 1, 0, 0);
#else
                    commandList->drawInstanced(6, 1, 0, 0);
#endif
#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
                    InvalidatePushedRootAddresses();
#endif

                    g_dirtyStates.renderTargetAndDepthStencil = true;
                    g_dirtyStates.viewport = true;
                    g_dirtyStates.pipelineState = true;
                    g_dirtyStates.scissorRect = true;

                    if (g_vulkan)
                    {
                        g_dirtyStates.vertexShaderConstants = true; // The push constant call invalidates vertex shader constants.
                        g_dirtyStates.depthBias = true; // Static depth bias in copy pipeline invalidates dynamic depth bias.
                    }
                }

                texture->sourceSurface = nullptr;

                // Check if any texture slots had this texture assigned, and make it point back at the original texture.
                for (uint32_t i = 0; i < std::size(g_textures); i++)
                {
                    if (g_textures[i] == texture)
                        SetTextureInRenderThread(i, texture);
                }
            }

            surface->destinationTextures.clear();
        }
    }
}

#if defined(__SWITCH__)
// [Switch] SwitchKeepResolvesPending: a colour resolve still pending at the end of the frame stays
// pending instead of being copied then. The texture keeps sampling the surface directly, as it did all
// frame, and the copy is made when the surface is about to change (a draw into it, a clear, which may
// hand the image over instead, or its destruction) or the texture gets a CPU update. Every read sees
// the same texels as with the copy at the end of the frame. Not for the back buffer (its image changes
// every frame) nor multisampled surfaces.
static bool CanKeepResolvePending(const GuestSurface* surface)
{
    return surface != g_backBuffer && surface->textureHolder != nullptr && surface->sampleCount == RenderSampleCount::COUNT_1;
}

static void ExecutePendingResolvesOf(GuestSurface* surface, uint32_t trigger)
{
    if (surface->destinationTextures.empty())
        return;

    const bool depth = surface->format == RenderFormat::D32_FLOAT;
    g_resolveCopyTrigger = trigger;
    if (PopulateBarriersForStretchRect(depth ? nullptr : surface, depth ? surface : nullptr))
    {
        FlushBarriers();
        ExecutePendingStretchRectCommands(depth ? nullptr : surface, depth ? surface : nullptr);
    }
}

// Render thread, when the game releases a resource: no texture stays linked to a destroyed surface.
static void ForgetPendingResolves(GuestResource* resource)
{
    switch (resource->type)
    {
    case ResourceType::Texture:
    case ResourceType::VolumeTexture:
    {
        auto texture = reinterpret_cast<GuestTexture*>(resource);
        if (texture->sourceSurface != nullptr)
        {
            texture->sourceSurface->destinationTextures.erase(texture);
            texture->sourceSurface = nullptr;
        }
        break;
    }

    case ResourceType::RenderTarget:
    case ResourceType::DepthStencil:
    {
        auto surface = reinterpret_cast<GuestSurface*>(resource);
        if (!surface->destinationTextures.empty())
        {
            // The textures still read this surface: they get their copy before it goes.
            if (g_commandListOpen)
            {
                ExecutePendingResolvesOf(surface, PASS_PROFILER_COPIES_OTHER);
            }
            else
            {
                for (const auto texture : surface->destinationTextures)
                    texture->sourceSurface = nullptr;

                surface->destinationTextures.clear();
            }
        }

        g_pendingSurfaceCopies.erase(surface);
        g_pendingMsaaResolves.erase(surface);
        break;
    }

    default:
        break;
    }
}
#endif

static void ProcExecutePendingStretchRectCommands(const RenderCommand& cmd)
{
#if defined(__SWITCH__)
    g_resolveCopyTrigger = PASS_PROFILER_COPIES_AT_PRESENT;

    if (g_keepResolvesPending)
    {
        // The same test as below decides whether depth resolves are dropped (they are transient).
        bool foundAny = false;
        bool copyAny = false;
        for (const auto surface : g_pendingSurfaceCopies)
        {
            if (surface->format != RenderFormat::D32_FLOAT && !surface->destinationTextures.empty())
            {
                foundAny = true;
                if (!CanKeepResolvePending(surface))
                    copyAny |= PopulateBarriersForStretchRect(surface, nullptr);
            }
        }

        if (copyAny)
        {
            FlushBarriers();

            for (const auto surface : g_pendingSurfaceCopies)
            {
                if (surface->format != RenderFormat::D32_FLOAT && !CanKeepResolvePending(surface))
                    ExecutePendingStretchRectCommands(surface, nullptr);
            }
        }

        for (const auto surface : g_pendingSurfaceCopies)
        {
            if (surface->format == RenderFormat::D32_FLOAT)
            {
                if (foundAny)
                {
                    g_profilerDepthDropped += surface->destinationTextures.size();

                    for (const auto texture : surface->destinationTextures)
                        texture->sourceSurface = nullptr;

                    surface->destinationTextures.clear();
                }
            }
            else
            {
                g_profilerCopiesKept += surface->destinationTextures.size();

                for (const auto texture : surface->destinationTextures)
                    texture->pendingCarried = true;
            }
        }

        g_pendingSurfaceCopies.clear();
        g_pendingMsaaResolves.clear();
        return;
    }
#endif

    bool foundAny = false;

    for (const auto surface : g_pendingSurfaceCopies)
    {
        // Depth stencil textures in this game are guaranteed to be transient.
        if (surface->format != RenderFormat::D32_FLOAT)
            foundAny |= PopulateBarriersForStretchRect(surface, nullptr);
    }

    if (foundAny)
    {
        FlushBarriers();

        for (const auto surface : g_pendingSurfaceCopies)
        {
            if (surface->format != RenderFormat::D32_FLOAT)
                ExecutePendingStretchRectCommands(surface, nullptr);
#if defined(__SWITCH__)
            else
                g_profilerDepthDropped += surface->destinationTextures.size();
#endif

            for (const auto texture : surface->destinationTextures)
                texture->sourceSurface = nullptr;

            surface->destinationTextures.clear();
        }
    }

    g_pendingSurfaceCopies.clear();
    g_pendingMsaaResolves.clear();
}

static void SetFramebuffer(GuestSurface* renderTarget, GuestSurface* depthStencil, bool settingForClear)
{
    if (settingForClear || g_dirtyStates.renderTargetAndDepthStencil)
    {
        GuestSurface* framebufferContainer = nullptr;
        RenderTexture* framebufferKey = nullptr;

        if (renderTarget != nullptr && depthStencil != nullptr)
        {
            framebufferContainer = depthStencil; // Backbuffer texture changes per frame so we can't use the depth stencil as the key.
            framebufferKey = renderTarget->texture;
        }
        else if (renderTarget != nullptr && depthStencil == nullptr)
        {
            framebufferContainer = renderTarget;
            framebufferKey = renderTarget->texture; // Backbuffer texture changes per frame so we can't assume nullptr for it.
        }
        else if (renderTarget == nullptr && depthStencil != nullptr)
        {
            framebufferContainer = depthStencil;
            framebufferKey = nullptr;
        }

        auto& commandList = g_commandLists[g_frame];

        if (framebufferContainer != nullptr)
        {
            auto& framebuffer = framebufferContainer->framebuffers[framebufferKey];

            if (framebuffer == nullptr)
            {
                RenderFramebufferDesc desc;

                if (renderTarget != nullptr)
                {
                    desc.colorAttachments = const_cast<const RenderTexture**>(&renderTarget->texture);
                    desc.colorAttachmentsCount = 1;
                }

                if (depthStencil != nullptr)
                    desc.depthAttachment = depthStencil->texture;

                framebuffer = g_device->createFramebuffer(desc);
            }

            if (g_framebuffer != framebuffer.get())
            {
#if defined(__SWITCH__)
                PassProfilerFramebuffer(renderTarget, depthStencil);
                FrameLogPass(renderTarget, depthStencil);
                g_profilerFramebufferChanges++;
#endif
                commandList->setFramebuffer(framebuffer.get());
                g_framebuffer = framebuffer.get();
            }
        }
        else if (g_framebuffer != nullptr)
        {
            commandList->setFramebuffer(nullptr);
            g_framebuffer = nullptr;
        }

        if (g_framebuffer != nullptr)
        {
            SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.halfPixelOffsetX, 1.0f / float(g_framebuffer->getWidth()));
            SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.halfPixelOffsetY, -1.0f / float(g_framebuffer->getHeight()));
        }

        g_dirtyStates.renderTargetAndDepthStencil = settingForClear;
    }
}

static void Clear(GuestDevice* device, uint32_t flags, uint32_t, be<float>* color, double z) 
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::Clear;
    cmd.clear.flags = flags;
    cmd.clear.color[0] = color[0];
    cmd.clear.color[1] = color[1];
    cmd.clear.color[2] = color[2];
    cmd.clear.color[3] = color[3];
    cmd.clear.z = float(z);
    EnqueueRenderCommand(cmd);
}

#if defined(__SWITCH__)
// [Switch] SwitchResolveHandOver. A surface resolved into exactly one texture is about to be cleared
// completely: instead of copying the surface into the texture and clearing the surface, the texture
// takes the surface's image (which holds exactly what the copy would have written) and the surface
// takes the texture's old image, which the clear then overwrites entirely. Only when both images were
// created alike (same format, size, one level, same kind of target, one sample), so every later read
// and write sees the same texels as with the copy. The texture is sampled through a view made from its
// own view description (component mapping included); both get new descriptors, and the old views,
// descriptors and framebuffers stay alive until the frame's commands, which may use them, are done.
static bool CanHandOver(const GuestSurface* surface, const GuestTexture* texture)
{
    return CanKeepResolvePending(surface) && texture->handOverCapable && texture->textureHolder != nullptr &&
        texture->patchedTexture == nullptr && texture->recreatedCubeMapTexture == nullptr && texture->width == surface->width &&
        texture->height == surface->height && texture->format == surface->format;
}

// The swap itself. Returns the surface's previous descriptor: a plain view of the image the texture now
// holds, valid until the frame's commands are done.
static uint32_t HandOverSurfaceImage(GuestSurface* surface, GuestTexture* texture)
{
    // Barriers still pending are keyed by image and stay right across the swap (each object's layout
    // moves with its image), so they go out with the caller's batch instead of one of their own.
    const uint32_t oldSurfaceDescriptor = surface->descriptorIndex;

    const uint32_t frame = g_frame;
    g_handOverViews[frame].push_back(std::move(surface->textureView));
    g_handOverViews[frame].push_back(std::move(texture->textureView));
    g_handOverDescriptors[frame].push_back(surface->descriptorIndex);
    g_handOverDescriptors[frame].push_back(texture->descriptorIndex);

    if (texture->framebuffer != nullptr)
    {
        if (g_framebuffer == texture->framebuffer.get())
            g_framebuffer = nullptr;

        g_handOverFramebuffers[frame].push_back(std::move(texture->framebuffer));
    }

    // A depth surface's cached framebuffers all name its image as their depth attachment. (A colour
    // surface's are keyed by its image, so they are simply not found for the new one.)
    if (surface->format == RenderFormat::D32_FLOAT)
    {
        for (auto& [key, framebuffer] : surface->framebuffers)
        {
            if (g_framebuffer == framebuffer.get())
                g_framebuffer = nullptr;

            g_handOverFramebuffers[frame].push_back(std::move(framebuffer));
        }

        surface->framebuffers.clear();
    }

    std::swap(surface->textureHolder, texture->textureHolder);
    std::swap(surface->texture, texture->texture);
    std::swap(surface->layout, texture->layout);
    texture->hadSurfaceImage = true;

    texture->textureView = texture->texture->createTextureView(texture->viewDesc);
    texture->descriptorIndex = g_textureDescriptorAllocator.allocate();
    SetTextureDescriptor(g_textureDescriptorSet, texture->descriptorIndex, texture->texture, texture->width, texture->height,
        RenderTextureLayout::SHADER_READ, texture->textureView.get(), 1);

    RenderTextureViewDesc surfaceViewDesc;
    surfaceViewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
    surfaceViewDesc.format = surface->format;
    surfaceViewDesc.mipLevels = 1;
    surface->textureView = surface->texture->createTextureView(surfaceViewDesc);
    surface->descriptorIndex = g_textureDescriptorAllocator.allocate();
    SetTextureDescriptor(g_textureDescriptorSet, surface->descriptorIndex, surface->texture, surface->width, surface->height,
        RenderTextureLayout::SHADER_READ, surface->textureView.get(), 1);

    texture->sourceSurface = nullptr;
    texture->pendingCarried = false;
    surface->destinationTextures.erase(texture);

    // Slots that read the texture through the surface now read the texture itself.
    for (uint32_t i = 0; i < std::size(g_textures); i++)
    {
        if (g_textures[i] == texture)
            SetTextureInRenderThread(i, texture);
    }

    g_dirtyStates.renderTargetAndDepthStencil = true;
    return oldSurfaceDescriptor;
}

static void TryResolveHandOver(GuestSurface* surface)
{
    if (surface->destinationTextures.size() != 1)
        return;

    GuestTexture* texture = *surface->destinationTextures.begin();
    if (!CanHandOver(surface, texture))
        return;

    HandOverSurfaceImage(surface, texture);
    g_profilerHandOvers++;
}

// [Switch] SwitchCoverageHandOver. A surface resolved into one texture is about to be drawn into, so today
// its pending copy is made first. When the draw neither blends nor leaves a channel of the target
// unwritten, only the pixels the draw does not write need the old contents: the texture takes the surface's
// image (the copy's exact result, as with a hand-over at a clear), the surface takes the texture's old
// image, and the draw also marks every pixel it writes with stencil 1 in a stencil buffer of the surface's
// size (cleared first; the pass has no depth buffer, so the colour output is untouched). Right after the
// draw, the pixels still at 0 (not covered, or discarded by the pixel shader) get the old contents with
// the same copy shader, from the image the texture now holds. Every pixel then holds exactly what the copy
// followed by the draw gave. A full-screen pass, the usual case, copies nothing: the fix-up is rejected by
// the stencil test before shading. The stencil variant of the draw's pipeline is compiled in the
// background; until it exists the copy is made as before.
struct CoverageStencil
{
    std::unique_ptr<RenderTexture> texture;
    RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    // The reference the next hand-over writes (1-255). Each value is used by one hand-over between two
    // clears of the buffer, so the pixels holding the current one are exactly the ones its draw wrote:
    // the buffer is cleared only when it is new and when the values run out, not before every draw.
    uint32_t nextReference = 0;
};

struct CoverageFramebuffer
{
    const RenderTexture* colorImage;
    const CoverageStencil* stencil;
    std::unique_ptr<RenderFramebuffer> framebuffer;
};

struct CoverageFixup
{
    bool pending = false;
    bool clearStencil = false;
    uint32_t reference = 0;
    GuestSurface* surface = nullptr;
    CoverageStencil* stencil = nullptr;
    uint32_t sourceDescriptor = 0;
    RenderPipeline* drawPipeline = nullptr;
};

static std::vector<std::unique_ptr<CoverageStencil>> g_coverageStencils;
static std::vector<CoverageFramebuffer> g_coverageFramebuffers;
static ankerl::unordered_dense::map<RenderFormat, std::unique_ptr<RenderPipeline>> g_coverageFixupPipelines;
static ankerl::unordered_dense::set<XXH64_hash_t> g_coverageVariantsRequested;
static CoverageFixup g_coverageFixup;

static void EnqueueSwitchPipelineVariant(const PipelineState& pipelineState, XXH64_hash_t hash);
static void SanitizePipelineState(PipelineState& pipelineState);

static void ForgetCoverageFramebuffersOf(const RenderTexture* image)
{
    for (size_t i = 0; i < g_coverageFramebuffers.size();)
    {
        if (g_coverageFramebuffers[i].colorImage == image)
        {
            if (g_framebuffer == g_coverageFramebuffers[i].framebuffer.get())
                g_framebuffer = nullptr;

            g_coverageFramebuffers[i] = std::move(g_coverageFramebuffers.back());
            g_coverageFramebuffers.pop_back();
        }
        else
        {
            i++;
        }
    }
}

static CoverageStencil* GetCoverageStencil(uint32_t width, uint32_t height)
{
    for (auto& stencil : g_coverageStencils)
    {
        if (stencil->width == width && stencil->height == height)
            return stencil.get();
    }

    RenderTextureDesc desc;
    desc.dimension = RenderTextureDimension::TEXTURE_2D;
    desc.width = width;
    desc.height = height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = RenderFormat::S8_UINT;
    desc.flags = RenderTextureFlag::DEPTH_TARGET;

    auto stencil = std::make_unique<CoverageStencil>();
    stencil->texture = g_device->createTexture(desc);
    if (stencil->texture == nullptr)
        return nullptr;

    stencil->width = width;
    stencil->height = height;
    g_coverageStencils.push_back(std::move(stencil));
    fprintf(stderr, "Switch: %ux%u coverage stencil created (hand-overs at draws)\n", width, height);
    return g_coverageStencils.back().get();
}

static RenderFramebuffer* GetCoverageFramebuffer(const RenderTexture* colorImage, const CoverageStencil* stencil)
{
    for (auto& entry : g_coverageFramebuffers)
    {
        if (entry.colorImage == colorImage && entry.stencil == stencil)
            return entry.framebuffer.get();
    }

    RenderFramebufferDesc desc;
    desc.colorAttachments = const_cast<const RenderTexture**>(&colorImage);
    desc.colorAttachmentsCount = 1;
    desc.depthAttachment = stencil->texture.get();

    CoverageFramebuffer entry{ colorImage, stencil, g_device->createFramebuffer(desc) };
    g_coverageFramebuffers.push_back(std::move(entry));
    return g_coverageFramebuffers.back().framebuffer.get();
}

static RenderPipeline* GetCoverageFixupPipeline(RenderFormat format)
{
    auto& pipeline = g_coverageFixupPipelines[format];
    if (pipeline == nullptr)
    {
        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = g_pipelineLayout.get();
        desc.vertexShader = g_copyShader.get();
        desc.pixelShader = g_copyColorShader.get();
        desc.renderTargetFormat[0] = format;
        desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        desc.renderTargetCount = 1;
        desc.depthTargetFormat = RenderFormat::S8_UINT;
        desc.depthEnabled = false;
        desc.depthWriteEnabled = false;
        desc.stencilEnabled = true;
        desc.stencilReadMask = 0xFF;
        desc.stencilWriteMask = 0;
        desc.dynamicStencilReferenceEnabled = true; // The hand-over's reference: pixels not holding it.
        desc.stencilFrontFace.compareFunction = RenderComparisonFunction::NOT_EQUAL;
        desc.stencilBackFace = desc.stencilFrontFace;
        pipeline = g_device->createGraphicsPipeline(desc);
    }

    return pipeline.get();
}

// The colour channels a surface of this format stores (colour write mask bits).
static uint32_t FormatChannelMask(RenderFormat format)
{
    switch (format)
    {
    case RenderFormat::R8_UNORM:
    case RenderFormat::R32_FLOAT:
    case RenderFormat::R16_FLOAT:
        return 0x1;
    case RenderFormat::R16G16_FLOAT:
    case RenderFormat::R16G16_UNORM:
    case RenderFormat::R32G32_FLOAT:
        return 0x3;
    case RenderFormat::R8G8B8A8_UNORM:
    case RenderFormat::B8G8R8A8_UNORM:
    case RenderFormat::R16G16B16A16_FLOAT:
    case RenderFormat::R16G16B16A16_UNORM:
    case RenderFormat::R32G32B32A32_FLOAT:
        return 0xF;
    default:
        return 0; // Unknown: not handed over.
    }
}

// Blending whose result does not depend on the target: both destination factors are zero and nothing
// else reads it (MIN/MAX ignore the factors, and the source factors must not name the destination).
// Only for UNORM targets, whose texels are finite: a zero factor times an infinity or NaN would not be 0.
static bool BlendIgnoresDestination(const PipelineState& state, RenderFormat format)
{
    switch (format)
    {
    case RenderFormat::R8_UNORM:
    case RenderFormat::R16G16_UNORM:
    case RenderFormat::R8G8B8A8_UNORM:
    case RenderFormat::B8G8R8A8_UNORM:
    case RenderFormat::R16G16B16A16_UNORM:
        break;
    default:
        return false;
    }

    auto readsDestination = [](RenderBlend blend)
        {
            return blend == RenderBlend::DEST_COLOR || blend == RenderBlend::INV_DEST_COLOR ||
                blend == RenderBlend::DEST_ALPHA || blend == RenderBlend::INV_DEST_ALPHA || blend == RenderBlend::SRC_ALPHA_SAT;
        };

    auto combinesLinearly = [](RenderBlendOperation operation)
        {
            return operation == RenderBlendOperation::ADD || operation == RenderBlendOperation::SUBTRACT ||
                operation == RenderBlendOperation::REV_SUBTRACT;
        };

    return state.destBlend == RenderBlend::ZERO && state.destBlendAlpha == RenderBlend::ZERO &&
        combinesLinearly(state.blendOp) && combinesLinearly(state.blendOpAlpha) &&
        !readsDestination(state.srcBlend) && !readsDestination(state.srcBlendAlpha);
}

// The vertices of the DrawPrimitiveUP being flushed (guest data, big-endian; ProcDrawPrimitiveUP), for
// GetFullScreenCoverage. Other draws leave data null.
struct CurrentDrawVertices
{
    const uint8_t* data = nullptr;
    uint32_t count = 0;
    uint32_t stride = 0;
    uint32_t primitiveType = 0;
};

static CurrentDrawVertices g_currentDrawVertices;

// Pixels along the edges of a target (up to four rectangles, one per side).
struct EdgePixels
{
    RenderRect rects[4];
    uint32_t count = 0;
};

// Why a draw was not proven to cover its whole target (profiler).
enum ExactCoverageMiss : uint32_t
{
    EXACT_COVERAGE_MISS_NOT_QUAD,   // not a DrawPrimitiveUP of four vertices as a strip, fan or quad
    EXACT_COVERAGE_MISS_SHADERS,    // position not passed through, or the pixel shader can discard
    EXACT_COVERAGE_MISS_SHAPE,      // not a rectangle split on its diagonal, out of the depth range, or culled
    EXACT_COVERAGE_MISS_EDGES,      // more than a thin frame of the target left uncovered
    EXACT_COVERAGE_MISS_COUNT
};

static_assert(std::size(g_profilerExactCoverageMisses) == EXACT_COVERAGE_MISS_COUNT);

// [Switch] SwitchExactCoverage. Whether the draw being flushed covers every pixel of `surface` except at most
// a frame EDGE_PIXELS wide, returned in `edges`. It must be a DrawPrimitiveUP of one axis-aligned rectangle
// made of two triangles that share its diagonal (a strip, fan or quad of four vertices), through a vertex
// shader whose position is the POSITION input with w = 1 plus the half-pixel offset
// (SHADER_FLAG_POSITION_PASS_THROUGH), inside the depth range and not culled. The GPU then covers each pixel
// whose centre lies inside the rectangle, the viewport and the scissor; a centre on the diagonal belongs to
// exactly one of the two triangles (Vulkan's rule for a shared edge). Centres on or within EDGE_MARGIN of an
// outer edge depend on the rasterizer's tie rule and sub-pixel rounding: those pixels go into `edges`. The
// caller checks that every covered pixel is written in full.
static bool GetFullScreenCoverage(const GuestSurface* surface, EdgePixels& edges)
{
    static constexpr double EDGE_MARGIN = 1.0 / 64.0; // Pixels; vertices are snapped to 1/256.
    static constexpr int64_t EDGE_PIXELS = 2;

    const auto& draw = g_currentDrawVertices;

    // The two triangles in Vulkan's vertex order (facing) and the vertices of the edge they share.
    uint32_t triangles[2][3];
    uint32_t shared0, shared1;
    switch (draw.primitiveType)
    {
    case D3DPT_TRIANGLESTRIP:
        // (0, 1, 2) and (1, 3, 2).
        triangles[0][0] = 0; triangles[0][1] = 1; triangles[0][2] = 2;
        triangles[1][0] = 1; triangles[1][1] = 3; triangles[1][2] = 2;
        shared0 = 1;
        shared1 = 2;
        break;
    case D3DPT_TRIANGLEFAN:
    case D3DPT_QUADLIST:
        // (0, 1, 2) and (0, 2, 3): the quad list's indices, and a fan either as those indices or natively
        // ((1, 2, 0) and (2, 3, 0), the same windings).
        triangles[0][0] = 0; triangles[0][1] = 1; triangles[0][2] = 2;
        triangles[1][0] = 0; triangles[1][1] = 2; triangles[1][2] = 3;
        shared0 = 0;
        shared1 = 2;
        break;
    default:
        g_profilerExactCoverageMisses[EXACT_COVERAGE_MISS_NOT_QUAD]++;
        return false;
    }

    if (draw.data == nullptr || draw.count != 4)
    {
        g_profilerExactCoverageMisses[EXACT_COVERAGE_MISS_NOT_QUAD]++;
        return false;
    }

    // The position input: location 0 (POSITION0), 32-bit floats in stream 0.
    const GuestVertexDeclaration* declaration = g_pipelineState.vertexDeclaration;
    const RenderInputElement* position = nullptr;
    for (uint32_t i = 0; i < declaration->inputElementCount; i++)
    {
        if (declaration->inputElements[i].location == 0)
        {
            position = &declaration->inputElements[i];
            break;
        }
    }

    uint32_t components = 0;
    if (position != nullptr && position->slotIndex == 0)
    {
        switch (position->format)
        {
        case RenderFormat::R32G32_FLOAT:
            components = 2;
            break;
        case RenderFormat::R32G32B32_FLOAT:
            components = 3;
            break;
        case RenderFormat::R32G32B32A32_FLOAT:
            components = 4;
            break;
        default:
            break;
        }
    }

    if (components == 0 || position->alignedByteOffset + components * sizeof(float) > draw.stride)
    {
        g_profilerExactCoverageMisses[EXACT_COVERAGE_MISS_SHAPE]++;
        return false;
    }

    float x[4], y[4];
    for (uint32_t i = 0; i < 4; i++)
    {
        const uint8_t* vertex = draw.data + i * draw.stride + position->alignedByteOffset;
        auto load = [vertex](uint32_t component)
            {
                uint32_t value;
                memcpy(&value, vertex + component * sizeof(uint32_t), sizeof(value));
                return std::bit_cast<float>(ByteSwap(value));
            };

        x[i] = load(0);
        y[i] = load(1);
        // Clipped against 0 <= z <= w (w is 1); a missing z reads as 0.
        const float z = components >= 3 ? load(2) : 0.0f;
        if (!std::isfinite(x[i]) || !std::isfinite(y[i]) || !(z >= 0.0f && z <= 1.0f))
        {
            g_profilerExactCoverageMisses[EXACT_COVERAGE_MISS_SHAPE]++;
            return false;
        }
    }

    // Two x and two y values, each corner once, the shared edge a diagonal.
    auto twoValues = [](const float* values, float& low, float& high)
        {
            low = std::min({ values[0], values[1], values[2], values[3] });
            high = std::max({ values[0], values[1], values[2], values[3] });
            for (uint32_t i = 0; i < 4; i++)
            {
                if (values[i] != low && values[i] != high)
                    return false;
            }
            return low < high;
        };

    float xLow, xHigh, yLow, yHigh;
    uint32_t corners = 0;
    if (twoValues(x, xLow, xHigh) && twoValues(y, yLow, yHigh))
    {
        for (uint32_t i = 0; i < 4; i++)
            corners |= 1u << ((x[i] == xHigh ? 1 : 0) | (y[i] == yHigh ? 2 : 0));
    }

    if (corners != 0xF || x[shared0] == x[shared1] || y[shared0] == y[shared1])
    {
        g_profilerExactCoverageMisses[EXACT_COVERAGE_MISS_SHAPE]++;
        return false;
    }

    // Framebuffer coordinates. The vertex shader adds the half-pixel offset of the bound framebuffer (the
    // surface's size, SetFramebuffer); DXC then negates y (-fvk-invert-y), so y grows downwards as in D3D.
    const double offsetX = 1.0f / float(surface->width);
    const double offsetY = -1.0f / float(surface->height);
    double fx[4], fy[4];
    for (uint32_t i = 0; i < 4; i++)
    {
        fx[i] = double(g_viewport.x) + (double(x[i]) + offsetX + 1.0) * double(g_viewport.width) * 0.5;
        fy[i] = double(g_viewport.y) + (1.0 - (double(y[i]) + offsetY)) * double(g_viewport.height) * 0.5;
    }

    // Facing: twice the signed area with y down is positive for a triangle that turns clockwise on screen,
    // which plume's pipelines make the front face (VK_FRONT_FACE_CLOCKWISE; D3DCULL_CW culls FRONT).
    if (g_pipelineState.cullMode != RenderCullMode::NONE)
    {
        for (const auto& triangle : triangles)
        {
            double area = 0.0;
            for (uint32_t k = 0; k < 3; k++)
            {
                const uint32_t a = triangle[k];
                const uint32_t b = triangle[(k + 1) % 3];
                area += fx[a] * fy[b] - fx[b] * fy[a];
            }

            const bool front = area > 0.0;
            if ((g_pipelineState.cullMode == RenderCullMode::FRONT) == front)
            {
                g_profilerExactCoverageMisses[EXACT_COVERAGE_MISS_SHAPE]++;
                return false;
            }
        }
    }

    // What is rasterized: the rectangle within the viewport (the clip volume), then the scissor
    // (FlushViewport's rectangle), within the target.
    const double left = std::max(std::min({ fx[0], fx[1], fx[2], fx[3] }), double(g_viewport.x));
    const double right = std::min(std::max({ fx[0], fx[1], fx[2], fx[3] }), double(g_viewport.x) + double(g_viewport.width));
    const double top = std::max(std::min({ fy[0], fy[1], fy[2], fy[3] }), double(g_viewport.y));
    const double bottom = std::min(std::max({ fy[0], fy[1], fy[2], fy[3] }), double(g_viewport.y) + double(g_viewport.height));

    // Within the viewport's extent from here on.
    if (!(left < right) || !(top < bottom))
    {
        g_profilerExactCoverageMisses[EXACT_COVERAGE_MISS_EDGES]++;
        return false;
    }

    const RenderRect scissor = g_scissorTestEnable ? g_scissorRect : RenderRect(
        g_viewport.x,
        g_viewport.y,
        g_viewport.x + g_viewport.width,
        g_viewport.y + g_viewport.height);

    const int64_t width = surface->width;
    const int64_t height = surface->height;
    const int64_t firstColumn = std::max<int64_t>({ int64_t(std::ceil(left + EDGE_MARGIN - 0.5)), scissor.left, 0 });
    const int64_t lastColumn = std::min<int64_t>({ int64_t(std::floor(right - EDGE_MARGIN - 0.5)), int64_t(scissor.right) - 1, width - 1 });
    const int64_t firstRow = std::max<int64_t>({ int64_t(std::ceil(top + EDGE_MARGIN - 0.5)), scissor.top, 0 });
    const int64_t lastRow = std::min<int64_t>({ int64_t(std::floor(bottom - EDGE_MARGIN - 0.5)), int64_t(scissor.bottom) - 1, height - 1 });

    if (firstColumn > EDGE_PIXELS || lastColumn < width - 1 - EDGE_PIXELS || firstRow > EDGE_PIXELS || lastRow < height - 1 - EDGE_PIXELS)
    {
        g_profilerExactCoverageMisses[EXACT_COVERAGE_MISS_EDGES]++;
        return false;
    }

    edges.count = 0;
    auto add = [&](int64_t l, int64_t t, int64_t r, int64_t b)
        {
            if (l < r && t < b)
                edges.rects[edges.count++] = RenderRect(int32_t(l), int32_t(t), int32_t(r), int32_t(b));
        };

    add(0, 0, firstColumn, height);
    add(lastColumn + 1, 0, width, height);
    add(firstColumn, 0, lastColumn + 1, firstRow);
    add(firstColumn, lastRow + 1, lastColumn + 1, height);
    return true;
}

// Whether the draw being flushed, whose state `sanitized` is, writes every pixel of `surface` it covers in
// full and covers all of them but `edges` (GetFullScreenCoverage). The caller has checked the colour mask,
// the blend and the missing depth buffer. Translated pixel shaders discard only through a kill instruction
// (SHADER_FLAG_PIXEL_KILL), the alpha test, alpha to coverage and the blend skip; of the port's own, the
// blur replacements never do (neverDiscards).
static bool IsExactFullScreenDraw(const PipelineState& sanitized, const GuestSurface* surface, EdgePixels& edges)
{
    const GuestShader* vertexShader = sanitized.vertexShader;
    const GuestShader* pixelShader = sanitized.pixelShader;
    const bool pixelShaderMayDiscard = pixelShader == nullptr || (pixelShader->shaderCacheEntry != nullptr ?
        (pixelShader->shaderCacheEntry->flags & SHADER_FLAG_PIXEL_KILL) != 0 : !pixelShader->neverDiscards);

    if (vertexShader->shaderCacheEntry == nullptr || (vertexShader->shaderCacheEntry->flags & SHADER_FLAG_POSITION_PASS_THROUGH) == 0 ||
        pixelShaderMayDiscard || sanitized.enableAlphaToCoverage ||
        (sanitized.specConstants & (SPEC_CONSTANT_ALPHA_TEST | SPEC_CONSTANT_ALPHA_TO_COVERAGE | SPEC_CONSTANT_BLEND_SKIP_ALPHA | SPEC_CONSTANT_BLEND_SKIP_ZERO)) != 0)
    {
        g_profilerExactCoverageMisses[EXACT_COVERAGE_MISS_SHADERS]++;
        return false;
    }

    return GetFullScreenCoverage(surface, edges);
}

// Work on the edge pixels of the draw being flushed, done once its framebuffer is bound
// (FlushRenderStateForRenderThread): the target's old contents copied in (an exact hand-over, before the
// draw overwrites what it covers of them) or a skipped clear's colour (DeferredClear).
struct PendingEdgePixels
{
    enum class Mode
    {
        NONE,
        COPY,
        CLEAR
    };

    Mode mode = Mode::NONE;
    EdgePixels edges;
    uint32_t sourceDescriptor = 0;
    RenderFormat format = RenderFormat::UNKNOWN;
    RenderColor color;
};

static PendingEdgePixels g_pendingEdgePixels;
static ankerl::unordered_dense::map<RenderFormat, std::unique_ptr<RenderPipeline>> g_edgeCopyPipelines;

static void FinishEdgePixels(const GuestSurface* surface)
{
    auto& pending = g_pendingEdgePixels;
    if (pending.mode == PendingEdgePixels::Mode::NONE)
        return;

    auto& commandList = g_commandLists[g_frame];
    if (pending.mode == PendingEdgePixels::Mode::CLEAR)
    {
        commandList->clearColor(0, pending.color, pending.edges.rects, pending.edges.count);
    }
    else
    {
        auto& pipeline = g_edgeCopyPipelines[pending.format];
        if (pipeline == nullptr)
        {
            RenderGraphicsPipelineDesc desc;
            desc.pipelineLayout = g_pipelineLayout.get();
            desc.vertexShader = g_copyShader.get();
            desc.pixelShader = g_copyColorShader.get();
            desc.renderTargetFormat[0] = pending.format;
            desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
            desc.renderTargetCount = 1;
            pipeline = g_device->createGraphicsPipeline(desc);
        }

        commandList->setPipeline(pipeline.get());
        commandList->setViewports(RenderViewport(0.0f, 0.0f, float(surface->width), float(surface->height), 0.0f, 1.0f));
        commandList->setGraphicsPushConstants(0, &pending.sourceDescriptor, 0, sizeof(uint32_t));
        for (uint32_t i = 0; i < pending.edges.count; i++)
        {
            commandList->setScissors(pending.edges.rects[i]);
            commandList->drawInstanced(g_singleCopyTriangle ? 3 : 6, 1, 0, 0);
        }
#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
        InvalidatePushedRootAddresses();
#endif

        // The draw sets its own state again (as after FinishCoverageFixup).
        g_dirtyStates.viewport = true;
        g_dirtyStates.pipelineState = true;
        g_dirtyStates.scissorRect = true;

        if (g_vulkan)
        {
            g_dirtyStates.vertexShaderConstants = true; // The push constant call invalidates vertex shader constants.
            g_dirtyStates.depthBias = true;
        }
    }

    pending.mode = PendingEdgePixels::Mode::NONE;
}

// [Switch] SwitchSkipOverwrittenClears. A colour clear waits for the next command. When that is a draw that
// replaces every pixel of the cleared target (DrawReplacesTarget), the clear is not made: only the edge
// pixels the draw may leave get its colour (FinishEdgePixels). Any other command makes it first, exactly as
// it would have been made (IssueDeferredClear); only commands that neither read nor write images, bind
// targets or record GPU work (KeepsDeferredClear) are processed before it.
struct DeferredClear
{
    bool pending = false;
    GuestSurface* surface = nullptr;
    RenderColor color;
};

static DeferredClear g_deferredClear;

// Round 9: the same for a clear of a depth buffer alone (SwitchSkipOverwrittenDepthClears, DeferredDepthClear).
struct DeferredDepthClear
{
    bool pending = false;
    GuestSurface* surface = nullptr;
    float z = 0.0f;
};

static DeferredDepthClear g_deferredDepthClear;

// Commands that neither read nor write images, bind targets nor record GPU work; draws decide themselves.
static bool KeepsDeferredClears(RenderCommandType type)
{
    switch (type)
    {
    case RenderCommandType::SetRenderState:
    case RenderCommandType::SetSamplerState:
    case RenderCommandType::SetTexture:
    case RenderCommandType::SetViewport:
    case RenderCommandType::SetScissorRect:
    case RenderCommandType::SetBooleans:
    case RenderCommandType::SetVertexShaderConstants:
    case RenderCommandType::SetPixelShaderConstants:
    case RenderCommandType::SetVertexDeclaration:
    case RenderCommandType::SetVertexShader:
    case RenderCommandType::SetPixelShader:
    case RenderCommandType::SetStreamSource:
    case RenderCommandType::SetIndices:
    case RenderCommandType::AddPipeline:
    case RenderCommandType::DrawPrimitive:          // They decide in FlushRenderStateForRenderThread.
    case RenderCommandType::DrawIndexedPrimitive:
    case RenderCommandType::DrawPrimitiveUP:
        return true;
    default:
        return false;
    }
}

// Round 9: also commands that cannot touch `surface` (SwitchCarryClears for colour, always for depth): a change
// of target, a clear (ProcClear decides: one of the same surface replaces the waiting clear), a buffer unlock, a
// resolve from another surface. A resolve from the cleared surface itself needs its contents, so it makes the
// clear first; nothing else reads the surface (its pending resolves were made or handed over by the clear, and
// a new one comes only through such a resolve).
static bool CannotTouchClearedSurface(const RenderCommand& cmd, const GuestSurface* surface)
{
    switch (cmd.type)
    {
    case RenderCommandType::SetRenderTarget:
    case RenderCommandType::SetDepthStencilSurface:
    case RenderCommandType::Clear:
    case RenderCommandType::UnlockBuffer16:
    case RenderCommandType::UnlockBuffer32:
        return true;
    case RenderCommandType::StretchRect:
        return ((cmd.stretchRect.flags & 0x4) != 0 ? g_depthStencil : g_renderTarget) != surface;
    default:
        return false;
    }
}

static bool KeepsDeferredClear(const RenderCommand& cmd)
{
    return KeepsDeferredClears(cmd.type) || (g_carryClears && CannotTouchClearedSurface(cmd, g_deferredClear.surface));
}

static bool KeepsDeferredDepthClear(const RenderCommand& cmd)
{
    return KeepsDeferredClears(cmd.type) || CannotTouchClearedSurface(cmd, g_deferredDepthClear.surface);
}

static void IssueDeferredClear()
{
    auto& clear = g_deferredClear;
    clear.pending = false;

    // Without SwitchCarryClears the render target is still the cleared surface: changing it is not a command that
    // keeps the clear. The depth buffer is bound with it, as ProcClear would have, while it is still a depth attachment.
    GuestSurface* surface = clear.surface;
    if (g_renderTarget != surface)
        g_profilerClearsCarried++;
    GuestSurface* depthStencil = g_depthStencil;
    if (depthStencil != nullptr && (depthStencil->layout != RenderTextureLayout::DEPTH_WRITE ||
        depthStencil->width != surface->width || depthStencil->height != surface->height))
    {
        depthStencil = nullptr;
    }

    AddBarrier(surface, RenderTextureLayout::COLOR_WRITE);
    FlushBarriers();
    SetFramebuffer(surface, depthStencil, true);
    g_commandLists[g_frame]->clearColor(0, clear.color);
}

// Whether the draw being flushed replaces every pixel of `surface` (its render target) but `edges` with
// values that do not depend on the old ones.
static bool DrawReplacesTarget(GuestSurface* surface, GuestSurface* depthStencil, EdgePixels& edges)
{
    const auto& state = g_pipelineState;
    const uint32_t channels = FormatChannelMask(surface->format);
    if (depthStencil != nullptr || channels == 0 || (state.colorWriteEnable & channels) != channels ||
        (state.alphaBlendEnable && !BlendIgnoresDestination(state, surface->format)) ||
        surface->sampleCount != RenderSampleCount::COUNT_1 || state.sampleCount != RenderSampleCount::COUNT_1)
    {
        return false;
    }

    PipelineState sanitized = state;
    SanitizePipelineState(sanitized);
    return IsExactFullScreenDraw(sanitized, surface, edges);
}

// SwitchSkipOverwrittenDepthClears: the waiting depth clear, made on its own before a command that may need it.
static void IssueDeferredDepthClear()
{
    auto& clear = g_deferredDepthClear;
    clear.pending = false;

    AddBarrier(clear.surface, RenderTextureLayout::DEPTH_WRITE);
    FlushBarriers();
    SetFramebuffer(nullptr, clear.surface, true);
    g_commandLists[g_frame]->clearDepth(true, clear.z);
}

// Whether the draw being flushed writes every pixel of `surface` (its depth buffer) but `edges` with depth values
// that do not depend on the old ones: depth test ALWAYS with writes (D32F: no stencil), a vertex shader that
// passes the position through and no way to discard a pixel, on a proven full-screen quad (GetFullScreenCoverage).
// The value written is the fragment's depth or what the pixel shader writes, whatever the old value was.
static bool DrawReplacesDepth(GuestSurface* surface, EdgePixels& edges)
{
    const auto& state = g_pipelineState;
    if (!state.zEnable || !state.zWriteEnable || state.zFunc != RenderComparisonFunction::ALWAYS ||
        surface->format != RenderFormat::D32_FLOAT || surface->sampleCount != RenderSampleCount::COUNT_1 ||
        state.sampleCount != RenderSampleCount::COUNT_1 || state.enableAlphaToCoverage)
    {
        return false;
    }

    PipelineState sanitized = state;
    SanitizePipelineState(sanitized);

    const GuestShader* vertexShader = sanitized.vertexShader;
    const GuestShader* pixelShader = sanitized.pixelShader;
    if (vertexShader == nullptr || vertexShader->shaderCacheEntry == nullptr ||
        (vertexShader->shaderCacheEntry->flags & SHADER_FLAG_POSITION_PASS_THROUGH) == 0)
    {
        return false;
    }

    // No pixel shader (a depth-only pipeline) discards nothing; a translated one only through a kill instruction.
    if (pixelShader != nullptr && (pixelShader->shaderCacheEntry != nullptr ?
        (pixelShader->shaderCacheEntry->flags & SHADER_FLAG_PIXEL_KILL) != 0 : !pixelShader->neverDiscards))
    {
        return false;
    }

    if ((sanitized.specConstants & (SPEC_CONSTANT_ALPHA_TEST | SPEC_CONSTANT_ALPHA_TO_COVERAGE | SPEC_CONSTANT_BLEND_SKIP_ALPHA |
        SPEC_CONSTANT_BLEND_SKIP_ZERO)) != 0)
    {
        return false;
    }

    return GetFullScreenCoverage(surface, edges);
}

// At the start of FlushRenderStateForRenderThread.
static void ResolveDeferredClear(GuestSurface* renderTarget, GuestSurface* depthStencil)
{
    // SwitchCarryClears: a draw that does not write the cleared surface leaves it waiting (see KeepsDeferredClear).
    if (g_carryClears && renderTarget != g_deferredClear.surface)
        return;

    EdgePixels edges;
    if (renderTarget == g_deferredClear.surface && DrawReplacesTarget(renderTarget, depthStencil, edges))
    {
        g_deferredClear.pending = false;
        if (edges.count != 0)
        {
            g_pendingEdgePixels.mode = PendingEdgePixels::Mode::CLEAR;
            g_pendingEdgePixels.edges = edges;
            g_pendingEdgePixels.color = g_deferredClear.color;
        }

        g_profilerClearsSkipped++;
    }
    else
    {
        IssueDeferredClear();
    }
}

// [Switch] SwitchSkipNoOpDraws: a draw that writes no colour channel (no render target or an empty mask) and
// no depth (no depth buffer, depth off or depth writes off). The port has no stencil, occlusion queries or
// shader stores, so it leaves every image as it was. It is not sent; the state it set stays dirty for the
// next draw, and the resolves and barriers the draw would have flushed stay pending until something needs
// them, as they do between draws.
static bool IsNoOpDraw()
{
    const bool writesColor = g_renderTarget != nullptr && g_pipelineState.colorWriteEnable != 0;
    const bool writesDepth = g_depthStencil != nullptr && g_pipelineState.zEnable && g_pipelineState.zWriteEnable;
    return !writesColor && !writesDepth;
}

enum RestoreMiss
{
    RESTORE_MISS_NOT_UP,      // a copy shader in a draw of vertex buffers (only DrawPrimitiveUP data is checked)
    RESTORE_MISS_SHADERS,     // the vertex shader does not pass position and texture coordinates through
    RESTORE_MISS_WRITES,      // blending, a depth write for a colour copy, a colour write for a depth copy...
    RESTORE_MISS_NOT_PENDING, // the texture is not a resolve of the target still pending (the target changed)
    RESTORE_MISS_FILTER,      // not point filtered
    RESTORE_MISS_MAPPING,     // the texture coordinates do not name each pixel's own texel
};

// [Switch] SwitchSkipRestoreDraws. On the Xbox 360, a render target lives in EDRAM, which other targets reuse:
// to draw into it again after a resolve, the game first copies the resolved texture back into it. Here a
// surface keeps its own image, and while a resolve is pending (lazy resolves) the texture reads that image
// itself. A draw that copies such a texture back into its own surface (SHADER_FLAG_PIXEL_COPY: the pixel
// shader outputs the fetched texel; _DEPTH_COPY: its x as the depth) writes every pixel it covers with the
// value that pixel already holds, so it is skipped, and with it the copy of the pending resolve that drawing
// into the surface would have forced first. Exact when:
// - each pixel reads its own texel: point filtering (the fetch takes the texel the sample point lies in) and
//   texture coordinates at every vertex equal to its framebuffer position divided by the surface's size, so
//   that at each pixel centre the interpolated coordinates lie in that pixel's texel (the vertex shader
//   passes both through, w = 1, so the interpolation is linear; the tolerance is 1/64 of a texel against the
//   half texel that would be needed to reach another one);
// - the value survives the round trip: the texture reads the surface's own image in its own format, and a
//   UNORM or float value converts to a float and back to itself; a depth value is in [0, 1], where saturate
//   and the viewport's [0, 1] depth range leave it unchanged;
// - nothing else is written: no blending, no depth write for a colour copy, no colour write for a depth copy
//   (discarded pixels, from the alpha test or any depth test, keep their value anyway).
static bool IsIdentityRestore(const uint8_t* vertices, uint32_t vertexCount, uint32_t stride)
{
    const GuestShader* vertexShader = g_pipelineState.vertexShader;
    const GuestShader* pixelShader = g_pipelineState.pixelShader;
    if (pixelShader == nullptr || pixelShader->shaderCacheEntry == nullptr)
        return false;

    const uint32_t pixelFlags = pixelShader->shaderCacheEntry->flags;
    const bool colourCopy = (pixelFlags & SHADER_FLAG_PIXEL_COPY) != 0;
    const bool depthCopy = (pixelFlags & SHADER_FLAG_DEPTH_COPY) != 0;
    if (!colourCopy && !depthCopy)
        return false;

    if (vertices == nullptr)
    {
        g_profilerRestoreMisses[RESTORE_MISS_NOT_UP]++;
        return false;
    }

    constexpr uint32_t PASS_THROUGH = SHADER_FLAG_POSITION_PASS_THROUGH | SHADER_FLAG_TEXCOORD_PASS_THROUGH;
    if (vertexShader == nullptr || vertexShader->shaderCacheEntry == nullptr ||
        (vertexShader->shaderCacheEntry->flags & PASS_THROUGH) != PASS_THROUGH)
    {
        g_profilerRestoreMisses[RESTORE_MISS_SHADERS]++;
        return false;
    }

    const auto& state = g_pipelineState;
    GuestSurface* renderTarget = state.colorWriteEnable != 0 ? g_renderTarget : nullptr;
    GuestSurface* depthStencil = state.zEnable ? g_depthStencil : nullptr;
    GuestSurface* target = colourCopy ? renderTarget : depthStencil;
    const bool writesDepth = depthStencil != nullptr && state.zWriteEnable;

    bool writesOnlyTarget;
    if (colourCopy)
        writesOnlyTarget = target != nullptr && !state.alphaBlendEnable && !writesDepth;
    else
        writesOnlyTarget = target != nullptr && writesDepth && renderTarget == nullptr &&
            std::min(g_viewport.minDepth, g_viewport.maxDepth) == 0.0f && std::max(g_viewport.minDepth, g_viewport.maxDepth) == 1.0f;

    if (!writesOnlyTarget || target->sampleCount != RenderSampleCount::COUNT_1 || state.sampleCount != RenderSampleCount::COUNT_1)
    {
        g_profilerRestoreMisses[RESTORE_MISS_WRITES]++;
        return false;
    }

    const uint32_t slot = (pixelFlags >> SHADER_FLAG_COPY_SLOT_SHIFT) & SHADER_FLAG_COPY_SLOT_MASK;
    const GuestTexture* texture = slot < std::size(g_textures) ? g_textures[slot] : nullptr;
    if (texture == nullptr || texture->sourceSurface != target || g_sharedConstants.texture2DIndices[slot] != target->descriptorIndex)
    {
        g_profilerRestoreMisses[RESTORE_MISS_NOT_PENDING]++;
        return false;
    }

    const RenderSamplerDesc& sampler = g_samplerDescs[slot];
    if (sampler.minFilter != RenderFilter::NEAREST || sampler.magFilter != RenderFilter::NEAREST || sampler.anisotropyEnabled)
    {
        g_profilerRestoreMisses[RESTORE_MISS_FILTER]++;
        return false;
    }

    // POSITION (location 0) and TEXCOORD0 (location 4), 32-bit floats in stream 0.
    const GuestVertexDeclaration* declaration = state.vertexDeclaration;
    const RenderInputElement* position = nullptr;
    const RenderInputElement* texcoord = nullptr;
    for (uint32_t i = 0; i < declaration->inputElementCount; i++)
    {
        const RenderInputElement& element = declaration->inputElements[i];
        if (element.location == 0)
            position = &element;
        else if (element.location == 4)
            texcoord = &element;
    }

    auto floatComponents = [](const RenderInputElement* element)
        {
            if (element == nullptr || element->slotIndex != 0)
                return 0u;

            switch (element->format)
            {
            case RenderFormat::R32G32_FLOAT:
                return 2u;
            case RenderFormat::R32G32B32_FLOAT:
                return 3u;
            case RenderFormat::R32G32B32A32_FLOAT:
                return 4u;
            default:
                return 0u;
            }
        };

    const uint32_t positionComponents = floatComponents(position);
    const uint32_t texcoordComponents = floatComponents(texcoord);
    if (positionComponents == 0 || texcoordComponents == 0 || position->alignedByteOffset + positionComponents * 4 > stride ||
        texcoord->alignedByteOffset + 8 > stride || vertexCount == 0 || vertexCount > 64 ||
        (declaration->swappedTexcoords & 1) != 0) // Only 16-bit texture coordinates are swapped.
    {
        g_profilerRestoreMisses[RESTORE_MISS_MAPPING]++;
        return false;
    }

    // Framebuffer coordinates as in GetFullScreenCoverage: the half-pixel offset of the framebuffer the draw
    // would bind (its colour attachment's size, else its depth attachment's), DXC's y negation, the viewport.
    const GuestSurface* framebufferSurface = renderTarget != nullptr ? renderTarget : depthStencil;
    const double offsetX = 1.0f / float(framebufferSurface->width);
    const double offsetY = -1.0f / float(framebufferSurface->height);
    const double width = double(target->width);
    const double height = double(target->height);
    constexpr double TOLERANCE = 1.0 / 64.0; // texels

    for (uint32_t i = 0; i < vertexCount; i++)
    {
        const uint8_t* vertex = vertices + size_t(i) * stride;
        auto load = [](const uint8_t* data)
            {
                uint32_t value;
                memcpy(&value, data, sizeof(value));
                return double(std::bit_cast<float>(ByteSwap(value)));
            };

        const double x = load(vertex + position->alignedByteOffset);
        const double y = load(vertex + position->alignedByteOffset + 4);
        const double z = positionComponents >= 3 ? load(vertex + position->alignedByteOffset + 8) : 0.0;
        const double u = load(vertex + texcoord->alignedByteOffset);
        const double v = load(vertex + texcoord->alignedByteOffset + 4);

        const double fx = double(g_viewport.x) + (x + offsetX + 1.0) * double(g_viewport.width) * 0.5;
        const double fy = double(g_viewport.y) + (1.0 - (y + offsetY)) * double(g_viewport.height) * 0.5;

        if (!std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(u) || !std::isfinite(v) || !(z >= 0.0 && z <= 1.0) ||
            std::abs(u * width - fx) > TOLERANCE || std::abs(v * height - fy) > TOLERANCE)
        {
            g_profilerRestoreMisses[RESTORE_MISS_MAPPING]++;
            return false;
        }
    }

    g_profilerRestoresSkipped[colourCopy ? 0 : 1]++;
    return true;
}

// Called for the render target of a draw, before its pending copies are made.
static bool TryCoverageHandOver(GuestSurface* surface, GuestSurface* depthStencil)
{
    if (!g_coverageHandOver || surface == nullptr || surface->destinationTextures.empty())
        return false;

    const auto& state = g_pipelineState;
    const uint32_t channels = FormatChannelMask(surface->format);
    if ((state.alphaBlendEnable && !BlendIgnoresDestination(state, surface->format)) || channels == 0 ||
        (state.colorWriteEnable & channels) != channels)
    {
        g_profilerCoverageMissBlend++;
        return false;
    }

    if (depthStencil != nullptr)
    {
        g_profilerCoverageMissDepth++;
        return false;
    }

    if (surface->destinationTextures.size() != 1)
    {
        g_profilerCoverageMissTextures++;
        return false;
    }

    GuestTexture* texture = *surface->destinationTextures.begin();
    if (!CanHandOver(surface, texture) || state.sampleCount != RenderSampleCount::COUNT_1)
    {
        g_profilerCoverageMissImage++;
        return false;
    }

    PipelineState variant = state;
    SanitizePipelineState(variant);

    // [Switch] SwitchExactCoverage: a draw that writes every pixel of the target needs neither the marks nor
    // the fix-up. The few edge pixels whose coverage the rasterizer decides get the old contents first
    // (FinishEdgePixels); the draw then overwrites those it covers, as it would after the copy.
    EdgePixels edges;
    if (g_exactCoverage && IsExactFullScreenDraw(variant, surface, edges))
    {
        const uint32_t sourceDescriptor = HandOverSurfaceImage(surface, texture);
        AddBarrier(texture, RenderTextureLayout::SHADER_READ);

        if (edges.count != 0)
        {
            g_pendingEdgePixels.mode = PendingEdgePixels::Mode::COPY;
            g_pendingEdgePixels.edges = edges;
            g_pendingEdgePixels.sourceDescriptor = sourceDescriptor;
            g_pendingEdgePixels.format = surface->format;
        }

        g_profilerExactHandOvers++;
        g_profilerCoverageHandOvers++;
        return true;
    }

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
    // The render thread may pick a skinning variant for such a vertex shader; not worth mirroring here.
    if (variant.vertexShader->shaderCacheEntry != nullptr &&
        (variant.vertexShader->shaderCacheEntry->specConstantsMask & SPEC_CONSTANT_BONES_SPECIALIZED) != 0)
    {
        g_profilerCoverageMissVariant++;
        return false;
    }
#endif

    variant.coverageStencil = 1;
    const XXH64_hash_t hash = XXH3_64bits(&variant, sizeof(variant));
    auto findResult = g_pipelines.find(hash);
    if (findResult == g_pipelines.end() || findResult->second == nullptr)
    {
        if (g_coverageVariantsRequested.emplace(hash).second)
            EnqueueSwitchPipelineVariant(variant, hash);

        g_profilerCoverageMissVariant++;
        return false;
    }

    CoverageStencil* stencil = GetCoverageStencil(surface->width, surface->height);
    if (stencil == nullptr)
    {
        g_profilerCoverageMissImage++;
        return false;
    }

    const uint32_t sourceDescriptor = HandOverSurfaceImage(surface, texture);

    // Flushed with the draw's own barriers: the texture's image is read by the fix-up (and maybe the draw).
    AddBarrier(texture, RenderTextureLayout::SHADER_READ);
    if (stencil->layout != RenderTextureLayout::DEPTH_WRITE)
    {
        g_barrierMap[stencil->texture.get()] = RenderTextureLayout::DEPTH_WRITE;
        stencil->layout = RenderTextureLayout::DEPTH_WRITE;
    }

    // A value no pixel of the buffer holds since its last clear.
    g_coverageFixup.clearStencil = stencil->nextReference == 0 || stencil->nextReference > 0xFF;
    if (g_coverageFixup.clearStencil)
        stencil->nextReference = 1;
    g_coverageFixup.reference = stencil->nextReference++;

    g_coverageFixup.pending = true;
    g_coverageFixup.surface = surface;
    g_coverageFixup.stencil = stencil;
    g_coverageFixup.sourceDescriptor = sourceDescriptor;
    g_coverageFixup.drawPipeline = findResult->second.get();
    g_profilerCoverageHandOvers++;
    return true;
}

// In place of SetFramebuffer for the draw of a coverage hand-over, once every other pending copy is made.
static void BindCoverageFramebuffer()
{
    auto& commandList = g_commandLists[g_frame];
    GuestSurface* surface = g_coverageFixup.surface;
    RenderFramebuffer* framebuffer = GetCoverageFramebuffer(surface->texture, g_coverageFixup.stencil);
    if (g_framebuffer != framebuffer)
    {
        PassProfilerFramebuffer(surface, nullptr);
        g_profilerFramebufferChanges++;
        commandList->setFramebuffer(framebuffer);
        g_framebuffer = framebuffer;
    }

    // What SetFramebuffer does for a new target.
    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.halfPixelOffsetX, 1.0f / float(framebuffer->getWidth()));
    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.halfPixelOffsetY, -1.0f / float(framebuffer->getHeight()));
    g_dirtyStates.renderTargetAndDepthStencil = false;

    if (g_coverageFixup.clearStencil)
        commandList->clearDepthStencil(false, true, 0.0f, 0);
}

// After every draw: the fix-up of a coverage hand-over (see TryCoverageHandOver).
static void FinishCoverageFixup()
{
    if (!g_coverageFixup.pending)
        return;

    g_coverageFixup.pending = false;

    auto& commandList = g_commandLists[g_frame];
    GuestSurface* surface = g_coverageFixup.surface;
    commandList->setPipeline(GetCoverageFixupPipeline(surface->format));
    commandList->setStencilReference(g_coverageFixup.reference);
    commandList->setViewports(RenderViewport(0.0f, 0.0f, float(surface->width), float(surface->height), 0.0f, 1.0f));
    commandList->setScissors(RenderRect(0, 0, surface->width, surface->height));
    commandList->setGraphicsPushConstants(0, &g_coverageFixup.sourceDescriptor, 0, sizeof(uint32_t));
    commandList->drawInstanced(g_singleCopyTriangle ? 3 : 6, 1, 0, 0);
#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
    InvalidatePushedRootAddresses();
#endif

    // The next draw goes back to the target's own framebuffer and state.
    g_dirtyStates.renderTargetAndDepthStencil = true;
    g_dirtyStates.viewport = true;
    g_dirtyStates.pipelineState = true;
    g_dirtyStates.scissorRect = true;

    if (g_vulkan)
    {
        g_dirtyStates.vertexShaderConstants = true; // The push constant call invalidates vertex shader constants.
        g_dirtyStates.depthBias = true;
    }
}
#endif

static void ProcClear(const RenderCommand& cmd)
{
    const auto& args = cmd.clear;
#if defined(__SWITCH__)
    FrameLogClear(args.flags, args.color, args.z);

    // Round 9 (SwitchCarryClears, SwitchSkipOverwrittenDepthClears): a waiting clear of a surface this clear
    // clears again is overwritten entirely by it and dropped; one of another surface goes out first, as only one
    // clear of each kind waits.
    if (g_deferredClear.pending && (args.flags & D3DCLEAR_TARGET) != 0 && g_renderTarget != nullptr)
    {
        if (g_renderTarget == g_deferredClear.surface)
        {
            g_deferredClear.pending = false;
            g_profilerClearsReplaced++;
        }
        else
        {
            IssueDeferredClear();
        }
    }

    if (g_deferredDepthClear.pending && (args.flags & D3DCLEAR_ZBUFFER) != 0 && g_depthStencil != nullptr)
    {
        if (g_depthStencil == g_deferredDepthClear.surface)
        {
            g_deferredDepthClear.pending = false;
            g_profilerClearsReplaced++;
        }
        else
        {
            IssueDeferredDepthClear();
        }
    }
#endif

    GuestSurface* resolveTarget = g_renderTarget;
    GuestSurface* resolveDepth = g_depthStencil;

#if defined(__SWITCH__)
    // [Switch] SwitchLazyResolves: only the surfaces this clear overwrites need their pending copies now.
    if (g_lazyResolves)
    {
        if ((args.flags & D3DCLEAR_TARGET) == 0)
            resolveTarget = nullptr;
        if ((args.flags & D3DCLEAR_ZBUFFER) == 0)
            resolveDepth = nullptr;
    }

    // SwitchSkipDeadCopies: pending resolves nothing will read are dropped, not copied or handed over.
    g_resolveCopyTrigger = PASS_PROFILER_COPIES_AT_CLEAR;
    DropDeadPendingResolves(resolveTarget);
    DropDeadPendingResolves(resolveDepth);

    if (g_resolveHandOver)
    {
        if (g_renderTarget != nullptr && (args.flags & D3DCLEAR_TARGET) != 0)
            TryResolveHandOver(g_renderTarget);
        if (g_depthStencil != nullptr && (args.flags & D3DCLEAR_ZBUFFER) != 0)
            TryResolveHandOver(g_depthStencil);
    }

    g_resolveCopyTrigger = PASS_PROFILER_COPIES_AT_CLEAR;
#endif

    if (PopulateBarriersForStretchRect(resolveTarget, resolveDepth))
    {
        FlushBarriers();
        ExecutePendingStretchRectCommands(resolveTarget, resolveDepth);
    }

#if defined(__SWITCH__)
    // [Switch] SwitchSkipOverwrittenClears (DeferredClear): a colour-only clear (or one whose depth part has
    // no depth buffer) of a single-sampled target waits for the next command. Its transition goes out with
    // the next batch.
    if (g_skipOverwrittenClears && g_renderTarget != nullptr && (args.flags & D3DCLEAR_TARGET) != 0 &&
        ((args.flags & D3DCLEAR_ZBUFFER) == 0 || g_depthStencil == nullptr) &&
        g_renderTarget->sampleCount == RenderSampleCount::COUNT_1)
    {
        AddBarrier(g_renderTarget, RenderTextureLayout::COLOR_WRITE);
        g_deferredClear.pending = true;
        g_deferredClear.surface = g_renderTarget;
        g_deferredClear.color = RenderColor(args.color[0], args.color[1], args.color[2], args.color[3]);
        g_profilerClearsDeferred++;
        return;
    }

    // Round 9, SwitchSkipOverwrittenDepthClears (DeferredDepthClear): a clear of the depth buffer alone (D32F, no
    // stencil to clear; single-sampled) waits the same way. Its transition goes out with the next batch.
    if (g_deferDepthClears && g_depthStencil != nullptr && (args.flags & D3DCLEAR_ZBUFFER) != 0 &&
        ((args.flags & D3DCLEAR_TARGET) == 0 || g_renderTarget == nullptr) &&
        g_depthStencil->format == RenderFormat::D32_FLOAT && g_depthStencil->sampleCount == RenderSampleCount::COUNT_1)
    {
        AddBarrier(g_depthStencil, RenderTextureLayout::DEPTH_WRITE);
        g_deferredDepthClear.pending = true;
        g_deferredDepthClear.surface = g_depthStencil;
        g_deferredDepthClear.z = args.z;
        g_profilerDepthClearsDeferred++;
        return;
    }
#endif

    AddBarrier(g_renderTarget, RenderTextureLayout::COLOR_WRITE);
    AddBarrier(g_depthStencil, RenderTextureLayout::DEPTH_WRITE);
    FlushBarriers();

    bool canClearInOnePass = (g_renderTarget == nullptr) || (g_depthStencil == nullptr) ||
        (g_renderTarget->width == g_depthStencil->width && g_renderTarget->height == g_depthStencil->height);

    if (canClearInOnePass)
        SetFramebuffer(g_renderTarget, g_depthStencil, true);

    auto& commandList = g_commandLists[g_frame];

    if (g_renderTarget != nullptr && (args.flags & D3DCLEAR_TARGET) != 0)
    {
        if (!canClearInOnePass)
            SetFramebuffer(g_renderTarget, nullptr, true);

        commandList->clearColor(0, RenderColor(args.color[0], args.color[1], args.color[2], args.color[3]));
    }

    if (g_depthStencil != nullptr && (args.flags & D3DCLEAR_ZBUFFER) != 0)
    {
        if (!canClearInOnePass)
            SetFramebuffer(nullptr, g_depthStencil, true);

        commandList->clearDepth(true, args.z);
    }
}

static void SetViewport(GuestDevice* device, GuestViewport* viewport)
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::SetViewport;
    cmd.setViewport.x = viewport->x;
    cmd.setViewport.y = viewport->y;
    cmd.setViewport.width = viewport->width;
    cmd.setViewport.height = viewport->height;
    cmd.setViewport.minDepth = viewport->minZ;
    cmd.setViewport.maxDepth = viewport->maxZ;
    EnqueueRenderCommand(cmd);

    device->viewport.x = float(viewport->x);
    device->viewport.y = float(viewport->y);
    device->viewport.width = float(viewport->width);
    device->viewport.height = float(viewport->height);
    device->viewport.minZ = viewport->minZ;
    device->viewport.maxZ = viewport->maxZ;
}

static void ProcSetViewport(const RenderCommand& cmd)
{
    const auto& args = cmd.setViewport;

    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.x, args.x);
    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.y, args.y);
    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.width, args.width);
    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.height, args.height);
    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.minDepth, args.minDepth);
    SetDirtyValue<float>(g_dirtyStates.viewport, g_viewport.maxDepth, args.maxDepth);
    
    uint32_t specConstants = g_pipelineState.specConstants;
    if (args.minDepth > args.maxDepth)
        specConstants |= SPEC_CONSTANT_REVERSE_Z;
    else 
        specConstants &= ~SPEC_CONSTANT_REVERSE_Z;

    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.specConstants, specConstants);

    g_dirtyStates.scissorRect |= g_dirtyStates.viewport;
}

static void SetTexture(GuestDevice* device, uint32_t index, GuestTexture* texture) 
{
    auto isPlayStation = Config::ControllerIcons == EControllerIcons::PlayStation;

    if (Config::ControllerIcons == EControllerIcons::Auto)
        isPlayStation = hid::g_inputDeviceController == hid::EInputDevice::PlayStation;

    if (isPlayStation && texture != nullptr && texture->patchedTexture != nullptr)
        texture = texture->patchedTexture.get();

    RenderCommand cmd;
    cmd.type = RenderCommandType::SetTexture;
    cmd.setTexture.index = index;
    cmd.setTexture.texture = texture;
    EnqueueRenderCommand(cmd);
}

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
// g_GatherableSlots bit of a slot, from its 2D texture and its sampler: point filtered both ways and
// without anisotropy (which the sampler state turns on only together with linear filtering).
static void UpdateGatherableSlot(uint32_t index)
{
    if (g_shadowGatherSpecBits == 0 || index >= 16)
        return;

    const uint32_t descriptorIndex = g_sharedConstants.texture2DIndices[index];
    const RenderSamplerDesc& samplerDesc = g_samplerDescs[index];
    const bool gatherable = descriptorIndex < TEXTURE_DESCRIPTOR_SIZE && g_textureDescriptorGatherable[descriptorIndex] &&
        samplerDesc.minFilter == RenderFilter::NEAREST && samplerDesc.magFilter == RenderFilter::NEAREST && !samplerDesc.anisotropyEnabled;

    uint32_t slots = g_sharedConstants.gatherableSlots;
    if (gatherable)
        slots |= 1u << index;
    else
        slots &= ~(1u << index);

#if defined(__SWITCH__)
    // A pipeline built for known-gatherable slots must not outlive the change (SpecializationBits).
    if (g_shadowGatherSpecialization && slots != g_sharedConstants.gatherableSlots)
        g_dirtyStates.pipelineState = true;
#endif

    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.gatherableSlots, slots);
}
#endif

static void UpdateTextureSize(uint32_t index)
{
    const uint32_t descriptorIndex = g_sharedConstants.texture2DIndices[index];
    const float width = descriptorIndex < TEXTURE_DESCRIPTOR_SIZE ? g_textureDescriptorSizes[descriptorIndex][0] : 1.0f;
    const float height = descriptorIndex < TEXTURE_DESCRIPTOR_SIZE ? g_textureDescriptorSizes[descriptorIndex][1] : 1.0f;
    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.textureSizes[index][0], width);
    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.textureSizes[index][1], height);

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
    UpdateGatherableSlot(index);
#endif
}

static void SetTextureInRenderThread(uint32_t index, GuestTexture* texture)
{
    AddBarrier(texture, RenderTextureLayout::SHADER_READ);

    auto viewDimension = texture != nullptr ? texture->viewDimension : RenderTextureViewDimension::UNKNOWN;

    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texture2DIndices[index],
        viewDimension == RenderTextureViewDimension::TEXTURE_2D ? texture->descriptorIndex : TEXTURE_DESCRIPTOR_NULL_TEXTURE_2D);
    UpdateTextureSize(index);

    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texture3DIndices[index], texture != nullptr &&
        viewDimension == RenderTextureViewDimension::TEXTURE_3D ? texture->descriptorIndex : TEXTURE_DESCRIPTOR_NULL_TEXTURE_3D);

    // Check if there's a cubemap texture we recreated and assign it if it's valid. The shader will pick whichever is correct.
    if (viewDimension == RenderTextureViewDimension::TEXTURE_2D && texture->recreatedCubeMapTexture != nullptr)
    {
        texture = texture->recreatedCubeMapTexture.get();
        AddBarrier(texture, RenderTextureLayout::SHADER_READ);
        viewDimension = texture->viewDimension;
    }

    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.textureCubeIndices[index], texture != nullptr &&
        viewDimension == RenderTextureViewDimension::TEXTURE_CUBE ? texture->descriptorIndex : TEXTURE_DESCRIPTOR_NULL_TEXTURE_CUBE);
}

static void SetSurface(uint32_t index, GuestSurface* surface)
{
    AddBarrier(surface, RenderTextureLayout::SHADER_READ);

    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texture2DIndices[index], surface->descriptorIndex);
    UpdateTextureSize(index);
    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.texture3DIndices[index], uint32_t(TEXTURE_DESCRIPTOR_NULL_TEXTURE_3D));
    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.textureCubeIndices[index], uint32_t(TEXTURE_DESCRIPTOR_NULL_TEXTURE_CUBE));
}

static void ProcSetTexture(const RenderCommand& cmd)
{
    const auto& args = cmd.setTexture;

    // If a pending copy operation is detected, set the source surface. The indices will be fixed later if flushing is necessary.
    bool shouldSetTexture = true;
    if (args.texture != nullptr && args.texture->sourceSurface != nullptr)
    {
        // MSAA surfaces need to be resolved and cannot be used directly.
        if (args.texture->sourceSurface->sampleCount != RenderSampleCount::COUNT_1)
        {
            g_pendingMsaaResolves.emplace(args.texture->sourceSurface);
        }
        else
        {
            SetSurface(args.index, args.texture->sourceSurface);
            shouldSetTexture = false;
        }
    }
    
    if (shouldSetTexture)
        SetTextureInRenderThread(args.index, args.texture);
    
    g_textures[args.index] = args.texture;
}

static void SetScissorRect(GuestDevice* device, GuestRect* rect)
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::SetScissorRect;
    cmd.setScissorRect.top = rect->top;
    cmd.setScissorRect.left = rect->left;
    cmd.setScissorRect.bottom = rect->bottom;
    cmd.setScissorRect.right = rect->right;
    EnqueueRenderCommand(cmd);
}

static void ProcSetScissorRect(const RenderCommand& cmd)
{
    const auto& args = cmd.setScissorRect;

    SetDirtyValue<int32_t>(g_dirtyStates.scissorRect, g_scissorRect.top, args.top);
    SetDirtyValue<int32_t>(g_dirtyStates.scissorRect, g_scissorRect.left, args.left);
    SetDirtyValue<int32_t>(g_dirtyStates.scissorRect, g_scissorRect.bottom, args.bottom);
    SetDirtyValue<int32_t>(g_dirtyStates.scissorRect, g_scissorRect.right, args.right);
}

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
// A shader generated with the constant-buffer path declares set 4 whenever it reads constants; a
// shader from an older cache reads them only through PhysicalStorageBuffer pointers. Capabilities
// and decorations come before the first function, so the scan stops there.
static bool SpirvReadsConstantsThroughUbo(const uint8_t* data, size_t size)
{
    const size_t count = size / sizeof(uint32_t);
    if (count < 5)
        return false;

    auto word = [data](size_t index)
        {
            uint32_t value;
            memcpy(&value, data + index * sizeof(uint32_t), sizeof(value));
            return value;
        };

    if (word(0) != 0x07230203)
        return false;

    constexpr uint32_t OP_CAPABILITY = 17;
    constexpr uint32_t OP_DECORATE = 71;
    constexpr uint32_t OP_FUNCTION = 54;
    constexpr uint32_t CAPABILITY_PHYSICAL_STORAGE_BUFFER_ADDRESSES = 5347;
    constexpr uint32_t DECORATION_DESCRIPTOR_SET = 34;

    bool pointers = false;
    bool set4 = false;

    for (size_t i = 5; i < count;)
    {
        const uint32_t instruction = word(i);
        const uint32_t wordCount = instruction >> 16;
        const uint32_t opcode = instruction & 0xFFFF;

        if (wordCount == 0 || i + wordCount > count)
            return false;

        if (opcode == OP_FUNCTION)
            break;

        if (opcode == OP_CAPABILITY && wordCount >= 2 && word(i + 1) == CAPABILITY_PHYSICAL_STORAGE_BUFFER_ADDRESSES)
            pointers = true;
        else if (opcode == OP_DECORATE && wordCount >= 4 && word(i + 2) == DECORATION_DESCRIPTOR_SET && word(i + 3) == CONSTANTS_UBO_SET_INDEX)
            set4 = true;

        i += wordCount;
    }

    return set4 || !pointers;
}
#endif

#if defined(__SWITCH__)
// Whether a translated pixel shader can be left out of a depth-only pipeline. Without colour
// targets its only possible effects are depth writes, sample-mask writes and kills. XenosRecomp
// emits the alpha test as exactly one clip() in shaders that carry SPEC_CONSTANT_ALPHA_TEST, and the
// blend skip as two discards that only a pipeline with a colour target enables (SPEC_CONSTANT_BLEND_SKIP_*);
// allowedKills counts those. Any other kill (a Xenos kill instruction, or several exits) makes the shader
// non-removable.
static bool SpirvRemovableInDepthOnlyPass(const uint8_t* data, size_t size, uint32_t allowedKills)
{
    const size_t count = size / sizeof(uint32_t);
    if (count < 5)
        return false;

    auto word = [data](size_t index)
        {
            uint32_t value;
            memcpy(&value, data + index * sizeof(uint32_t), sizeof(value));
            return value;
        };

    if (word(0) != 0x07230203)
        return false;

    constexpr uint32_t OP_DECORATE = 71;
    constexpr uint32_t OP_KILL = 252;
    constexpr uint32_t OP_TERMINATE_INVOCATION = 4416;
    constexpr uint32_t OP_DEMOTE_TO_HELPER_INVOCATION = 5380;
    constexpr uint32_t OP_IMAGE_WRITE = 99;
    constexpr uint32_t OP_ATOMIC_FIRST = 227; // OpAtomicLoad..OpAtomicXor
    constexpr uint32_t OP_ATOMIC_LAST = 242;
    constexpr uint32_t DECORATION_BUILTIN = 11;
    constexpr uint32_t BUILTIN_SAMPLE_MASK = 20;
    constexpr uint32_t BUILTIN_FRAG_DEPTH = 22;

    uint32_t kills = 0;

    for (size_t i = 5; i < count;)
    {
        const uint32_t instruction = word(i);
        const uint32_t wordCount = instruction >> 16;
        const uint32_t opcode = instruction & 0xFFFF;

        if (wordCount == 0 || i + wordCount > count)
            return false;

        if (opcode == OP_DECORATE && wordCount >= 4 && word(i + 2) == DECORATION_BUILTIN &&
            (word(i + 3) == BUILTIN_FRAG_DEPTH || word(i + 3) == BUILTIN_SAMPLE_MASK))
        {
            return false;
        }

        if (opcode == OP_IMAGE_WRITE || (opcode >= OP_ATOMIC_FIRST && opcode <= OP_ATOMIC_LAST))
            return false;

        if (opcode == OP_KILL || opcode == OP_TERMINATE_INVOCATION || opcode == OP_DEMOTE_TO_HELPER_INVOCATION)
            kills++;

        i += wordCount;
    }

    return kills <= allowedKills;
}

// The locations of the stage inputs a shader reads (bit N for location N < 32). SPIR-V can only
// read an Input variable through OpLoad, OpCopyMemory(Sized), an access chain, OpCopyObject of the
// pointer, a function call argument or an InterpolateAt* extended instruction; the pointer is
// looked for in exactly those operands. Returns ~0u (everything read) for anything unexpected.
static uint32_t SpirvInputLocationsRead(const uint8_t* data, size_t size)
{
    const size_t count = size / sizeof(uint32_t);
    if (count < 5)
        return ~0u;

    auto word = [data](size_t index)
        {
            uint32_t value;
            memcpy(&value, data + index * sizeof(uint32_t), sizeof(value));
            return value;
        };

    if (word(0) != 0x07230203)
        return ~0u;

    constexpr uint32_t OP_EXT_INST = 12;
    constexpr uint32_t OP_CAPABILITY = 17;
    constexpr uint32_t OP_FUNCTION_CALL = 57;
    constexpr uint32_t OP_VARIABLE = 59;
    constexpr uint32_t OP_LOAD = 61;
    constexpr uint32_t OP_COPY_MEMORY = 63;
    constexpr uint32_t OP_COPY_MEMORY_SIZED = 64;
    constexpr uint32_t OP_ACCESS_CHAIN = 65;
    constexpr uint32_t OP_IN_BOUNDS_ACCESS_CHAIN = 66;
    constexpr uint32_t OP_PTR_ACCESS_CHAIN = 67;
    constexpr uint32_t OP_DECORATE = 71;
    constexpr uint32_t OP_COPY_OBJECT = 83;
    constexpr uint32_t OP_IN_BOUNDS_PTR_ACCESS_CHAIN = 70;
    constexpr uint32_t CAPABILITY_VARIABLE_POINTERS_STORAGE_BUFFER = 4441;
    constexpr uint32_t CAPABILITY_VARIABLE_POINTERS = 4442;
    constexpr uint32_t DECORATION_LOCATION = 30;
    constexpr uint32_t STORAGE_CLASS_INPUT = 1;

    ankerl::unordered_dense::map<uint32_t, uint32_t> locations; // variable id -> location
    ankerl::unordered_dense::set<uint32_t> inputs;
    uint32_t read = 0;

    // Declarations come first; with variable pointers an input pointer could flow anywhere.
    for (size_t i = 5; i < count;)
    {
        const uint32_t instruction = word(i);
        const uint32_t wordCount = instruction >> 16;
        const uint32_t opcode = instruction & 0xFFFF;

        if (wordCount == 0 || i + wordCount > count)
            return ~0u;

        if (opcode == OP_CAPABILITY && wordCount >= 2 &&
            (word(i + 1) == CAPABILITY_VARIABLE_POINTERS || word(i + 1) == CAPABILITY_VARIABLE_POINTERS_STORAGE_BUFFER))
        {
            return ~0u;
        }
        else if (opcode == OP_DECORATE && wordCount >= 4 && word(i + 2) == DECORATION_LOCATION)
        {
            locations[word(i + 1)] = word(i + 3);
        }
        else if (opcode == OP_VARIABLE && wordCount >= 4 && word(i + 3) == STORAGE_CLASS_INPUT)
        {
            inputs.insert(word(i + 2));
        }

        i += wordCount;
    }

    auto markRead = [&](uint32_t id)
        {
            if (!inputs.contains(id))
                return;

            auto findResult = locations.find(id);
            if (findResult == locations.end())
                return; // A built-in (position, front face...), not an interpolator.

            read |= findResult->second < 32 ? (1u << findResult->second) : 0u;
        };

    for (size_t i = 5; i < count;)
    {
        const uint32_t instruction = word(i);
        const uint32_t wordCount = instruction >> 16;
        const uint32_t opcode = instruction & 0xFFFF;

        switch (opcode)
        {
        case OP_LOAD:
        case OP_ACCESS_CHAIN:
        case OP_IN_BOUNDS_ACCESS_CHAIN:
        case OP_PTR_ACCESS_CHAIN:
        case OP_IN_BOUNDS_PTR_ACCESS_CHAIN:
        case OP_COPY_OBJECT:
            if (wordCount >= 4)
                markRead(word(i + 3));
            break;

        case OP_COPY_MEMORY:
        case OP_COPY_MEMORY_SIZED:
            if (wordCount >= 3)
                markRead(word(i + 2));
            break;

        case OP_FUNCTION_CALL:
            for (size_t j = 4; j < wordCount; j++)
                markRead(word(i + j));
            break;

        case OP_EXT_INST:
            for (size_t j = 5; j < wordCount; j++)
                markRead(word(i + j));
            break;
        }

        i += wordCount;
    }

    return read;
}
#endif

static RenderShader* GetOrLinkShader(GuestShader* guestShader, uint32_t specConstants)
{
    if (g_vulkan ||
        guestShader->shaderCacheEntry == nullptr || 
        guestShader->shaderCacheEntry->specConstantsMask == 0)
    {
        std::lock_guard lock(guestShader->mutex);

        if (guestShader->shader == nullptr)
        {
            assert(guestShader->shaderCacheEntry != nullptr);

            if (g_vulkan)
            {
                auto compressedSpirvData = g_shaderCache.get() + guestShader->shaderCacheEntry->spirvOffset;

                std::vector<uint8_t> decoded(smolv::GetDecodedBufferSize(compressedSpirvData, guestShader->shaderCacheEntry->spirvSize));
                bool result = smolv::Decode(compressedSpirvData, guestShader->shaderCacheEntry->spirvSize, decoded.data(), decoded.size());
                assert(result);

                guestShader->shader = g_device->createShader(decoded.data(), decoded.size(), "main", RenderShaderFormat::SPIRV);

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
                guestShader->constantsThroughUbo.store(SpirvReadsConstantsThroughUbo(decoded.data(), decoded.size()), std::memory_order_release);
#endif
#if defined(__SWITCH__)
                if (guestShader->type == ResourceType::PixelShader)
                {
                    const uint32_t specConstantsMask = guestShader->shaderCacheEntry->specConstantsMask;
                    const uint32_t allowedKills = ((specConstantsMask & SPEC_CONSTANT_ALPHA_TEST) != 0 ? 1 : 0) +
                        ((specConstantsMask & SPEC_CONSTANT_BLEND_SKIP_ALPHA) != 0 ? 2 : 0);
                    // A Xenos kill instruction is never removable, whatever DXC made of the discards.
                    const bool hasKill = (guestShader->shaderCacheEntry->flags & SHADER_FLAG_PIXEL_KILL) != 0;
                    guestShader->removableInDepthOnlyPass.store(
                        Config::SwitchDepthOnlyWithoutPixelShader && !hasKill && SpirvRemovableInDepthOnlyPass(decoded.data(), decoded.size(), allowedKills),
                        std::memory_order_release);
                    guestShader->inputLocationsRead.store(SpirvInputLocationsRead(decoded.data(), decoded.size()), std::memory_order_release);
                }

                if (g_drawProfilerEnabled && _mesa_blake3_compute != nullptr)
                {
                    uint8_t blake3[32];
                    _mesa_blake3_compute(decoded.data(), decoded.size(), blake3);
                    guestShader->spirvBlake3 = (uint32_t(blake3[0]) << 24) | (uint32_t(blake3[1]) << 16) | (uint32_t(blake3[2]) << 8) | blake3[3];
                }
#endif
            }
            else
            {
                guestShader->shader = g_device->createShader(g_shaderCache.get() + guestShader->shaderCacheEntry->dxilOffset, 
                    guestShader->shaderCacheEntry->dxilSize, "main", RenderShaderFormat::DXIL);
            }
        }

        return guestShader->shader.get();
    }

    specConstants &= guestShader->shaderCacheEntry->specConstantsMask;

    RenderShader* shader;
    {
        std::lock_guard lock(guestShader->mutex);
        shader = guestShader->linkedShaders[specConstants].get();
    }

#ifdef UNLEASHED_RECOMP_D3D12
    if (shader == nullptr)
    {
        static RecompMutex g_compiledSpecConstantLibraryBlobMutex;
        static ankerl::unordered_dense::map<uint32_t, ComPtr<IDxcBlob>> g_compiledSpecConstantLibraryBlobs;

        thread_local ComPtr<IDxcCompiler3> s_dxcCompiler;
        thread_local ComPtr<IDxcLinker> s_dxcLinker;
        thread_local ComPtr<IDxcUtils> s_dxcUtils;

        wchar_t specConstantsLibName[0x100];
        swprintf_s(specConstantsLibName, L"SpecConstants_%d", specConstants);

        ComPtr<IDxcBlob> specConstantLibraryBlob;
        {
            std::lock_guard lock(g_compiledSpecConstantLibraryBlobMutex);
            specConstantLibraryBlob = g_compiledSpecConstantLibraryBlobs[specConstants];
        }

        if (specConstantLibraryBlob == nullptr)
        {
            if (s_dxcCompiler == nullptr)
            {
                HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(s_dxcCompiler.GetAddressOf()));
                assert(SUCCEEDED(hr) && s_dxcCompiler != nullptr);
            }

            char libraryHlsl[0x100];
            sprintf_s(libraryHlsl, "export uint g_SpecConstants() { return %d; }", specConstants);

            DxcBuffer buffer{};
            buffer.Ptr = libraryHlsl;
            buffer.Size = strlen(libraryHlsl);

            const wchar_t* args[1];
            args[0] = L"-T lib_6_3";

            ComPtr<IDxcResult> result;
            HRESULT hr = s_dxcCompiler->Compile(&buffer, args, std::size(args), nullptr, IID_PPV_ARGS(result.GetAddressOf()));
            assert(SUCCEEDED(hr) && result != nullptr);

            hr = result->GetResult(specConstantLibraryBlob.GetAddressOf());
            assert(SUCCEEDED(hr) && specConstantLibraryBlob != nullptr);

            std::lock_guard lock(g_compiledSpecConstantLibraryBlobMutex);
            g_compiledSpecConstantLibraryBlobs.emplace(specConstants, specConstantLibraryBlob);
        }

        if (s_dxcLinker == nullptr)
        {
            HRESULT hr = DxcCreateInstance(CLSID_DxcLinker, IID_PPV_ARGS(s_dxcLinker.GetAddressOf()));
            assert(SUCCEEDED(hr) && s_dxcLinker != nullptr);
        }

        s_dxcLinker->RegisterLibrary(specConstantsLibName, specConstantLibraryBlob.Get());

        wchar_t shaderLibName[0x100];
        swprintf_s(shaderLibName, L"Shader_%d", guestShader->shaderCacheEntry->dxilOffset);

        ComPtr<IDxcBlobEncoding> shaderLibraryBlob;
        {
            std::lock_guard lock(guestShader->mutex);
            shaderLibraryBlob = guestShader->libraryBlob;
        }

        if (shaderLibraryBlob == nullptr)
        {
            if (s_dxcUtils == nullptr)
            {
                HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(s_dxcUtils.GetAddressOf()));
                assert(SUCCEEDED(hr) && s_dxcUtils != nullptr);
            }

            HRESULT hr = s_dxcUtils->CreateBlobFromPinned(
                g_shaderCache.get() + guestShader->shaderCacheEntry->dxilOffset,
                guestShader->shaderCacheEntry->dxilSize,
                DXC_CP_ACP,
                shaderLibraryBlob.GetAddressOf());

            assert(SUCCEEDED(hr) && shaderLibraryBlob != nullptr);

            std::lock_guard lock(guestShader->mutex);
            guestShader->libraryBlob = shaderLibraryBlob;
        }

        s_dxcLinker->RegisterLibrary(shaderLibName, shaderLibraryBlob.Get());

        const wchar_t* libraryNames[] = { specConstantsLibName, shaderLibName };

        ComPtr<IDxcOperationResult> result;
        HRESULT hr = s_dxcLinker->Link(L"main", guestShader->type == ResourceType::VertexShader ? L"vs_6_0" : L"ps_6_0",
            libraryNames, std::size(libraryNames), nullptr, 0, result.GetAddressOf());

        assert(SUCCEEDED(hr) && result != nullptr);

        ComPtr<IDxcBlob> blob;
        hr = result->GetResult(blob.GetAddressOf());
        assert(SUCCEEDED(hr) && blob != nullptr);

        {
            std::lock_guard lock(guestShader->mutex);

            auto& linkedShader = guestShader->linkedShaders[specConstants];
            if (linkedShader == nullptr)
            {
                linkedShader = g_device->createShader(blob->GetBufferPointer(), blob->GetBufferSize(), "main", RenderShaderFormat::DXIL);
                guestShader->shaderBlobs.push_back(std::move(blob));
            }

            shader = linkedShader.get();
        }        
    }
#endif

    return shader;
}

#if defined(__SWITCH__)
// [Switch] SwitchSkipTransparentPixels: the SPEC_CONSTANT_BLEND_SKIP_* bit of a sanitized state whose blend
// leaves a pixel of the target exactly as it was when the shader's output is transparent (shader_common.h).
// UNORM targets only: the blend clamps their source values to [0, 1] first, so an output of 0 or less
// contributes exactly 0, and their texels are finite, so a destination factor of 1 keeps them. The discarded
// pixel must have no other effect: no depth write and no alpha to coverage (the port has no stencil or
// occlusion queries). With several samples a discard keeps all of them, as a blend that changes nothing does.
static uint32_t BlendSkipBits(const PipelineState& state)
{
    if (!g_skipTransparentPixels || !state.alphaBlendEnable || state.enableAlphaToCoverage ||
        (state.specConstants & SPEC_CONSTANT_ALPHA_TO_COVERAGE) != 0 ||
        (state.zWriteEnable && state.depthStencilFormat != RenderFormat::UNKNOWN))
    {
        return 0;
    }

    switch (state.renderTargetFormat)
    {
    case RenderFormat::R8_UNORM:
    case RenderFormat::R16G16_UNORM:
    case RenderFormat::R8G8B8A8_UNORM:
    case RenderFormat::B8G8R8A8_UNORM:
    case RenderFormat::R16G16B16A16_UNORM:
        break;
    default:
        return 0;
    }

    auto keepsDestination = [](RenderBlendOperation operation)
        {
            return operation == RenderBlendOperation::ADD || operation == RenderBlendOperation::REV_SUBTRACT;
        };

    // A destination factor of exactly 1 once the source is 0 (colour) or its alpha is 0 (ALPHA).
    auto factorOne = [](RenderBlend factor, bool sourceZero)
        {
            return factor == RenderBlend::ONE || factor == RenderBlend::INV_SRC_ALPHA ||
                (sourceZero && factor == RenderBlend::INV_SRC_COLOR);
        };

    const bool colorWritten = (state.colorWriteEnable & 0x7) != 0;
    const bool alphaWritten = (state.colorWriteEnable & 0x8) != 0;

    // _ALPHA: a source alpha of 0 or less. The colour source term is 0 through a SRC_ALPHA or ZERO factor; the
    // alpha source term is 0 whatever its factor (all are finite).
    const bool alphaSkip =
        (!colorWritten || ((state.srcBlend == RenderBlend::SRC_ALPHA || state.srcBlend == RenderBlend::ZERO) &&
            factorOne(state.destBlend, false) && keepsDestination(state.blendOp))) &&
        (!alphaWritten || (factorOne(state.destBlendAlpha, true) && keepsDestination(state.blendOpAlpha)));

    if (alphaSkip)
        return SPEC_CONSTANT_BLEND_SKIP_ALPHA;

    // _ZERO: all four outputs 0 or less, so every source term is 0 whatever its factor.
    const bool zeroSkip =
        (!colorWritten || (factorOne(state.destBlend, true) && keepsDestination(state.blendOp))) &&
        (!alphaWritten || (factorOne(state.destBlendAlpha, true) && keepsDestination(state.blendOpAlpha)));

    return zeroSkip ? SPEC_CONSTANT_BLEND_SKIP_ZERO : 0;
}
#endif

static void SanitizePipelineState(PipelineState& pipelineState)
{
    if (!pipelineState.zEnable)
    {
        pipelineState.zWriteEnable = false;
        pipelineState.zFunc = RenderComparisonFunction::LESS;
        pipelineState.slopeScaledDepthBias = 0.0f;
        pipelineState.depthBias = 0;
        pipelineState.depthStencilFormat = RenderFormat::UNKNOWN;
    }

    if (pipelineState.slopeScaledDepthBias == 0.0f)
        pipelineState.slopeScaledDepthBias = 0.0f; // Remove sign.

    if (!pipelineState.colorWriteEnable)
    {
        pipelineState.alphaBlendEnable = false;
        pipelineState.renderTargetFormat = RenderFormat::UNKNOWN;
    }

    if (!pipelineState.alphaBlendEnable)
    {
        pipelineState.srcBlend = RenderBlend::ONE;
        pipelineState.destBlend = RenderBlend::ZERO;
        pipelineState.blendOp = RenderBlendOperation::ADD;
        pipelineState.srcBlendAlpha = RenderBlend::ONE;
        pipelineState.destBlendAlpha = RenderBlend::ZERO;
        pipelineState.blendOpAlpha = RenderBlendOperation::ADD;
    }

    for (size_t i = 0; i < 16; i++)
    {
        if (!pipelineState.vertexDeclaration->vertexStreams[i])
            pipelineState.vertexStrides[i] = 0;
    }

    uint32_t specConstantsMask = 0;
    if (pipelineState.vertexShader->shaderCacheEntry != nullptr)
        specConstantsMask |= pipelineState.vertexShader->shaderCacheEntry->specConstantsMask;

    if (pipelineState.pixelShader != nullptr && pipelineState.pixelShader->shaderCacheEntry != nullptr)
        specConstantsMask |= pipelineState.pixelShader->shaderCacheEntry->specConstantsMask;

#if defined(__SWITCH__)
    {
        // The sinking variants only for pipelines whose early-out can skip pixels (alpha test, or transparent
        // pixels of a blend); the others run their code in its place, where the quad variant's two quad
        // shuffles would be for nothing.
        const uint32_t blendSkipBits = BlendSkipBits(pipelineState);
        const bool earlyOutSkips = (pipelineState.specConstants & SPEC_CONSTANT_ALPHA_TEST) != 0 ||
            (blendSkipBits & SPEC_CONSTANT_BLEND_SKIP_ALPHA) != 0;
        pipelineState.specConstants |= g_alphaTestEarlyOutBit | blendSkipBits |
            (earlyOutSkips ? g_alphaTestSinkBit | g_alphaTestQuadSinkBit : 0);
    }
#endif
    pipelineState.specConstants &= specConstantsMask;

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
    // Applied after masking on purpose: the bit is not in any shader's mask (that would change
    // D3D12/DXIL linking), and every pipeline path (render thread, async, precompiled list)
    // goes through here, so their hashes stay consistent.
    pipelineState.specConstants |= g_constantsUboSpecBit;
#endif
#if defined(__SWITCH__) && defined(SPEC_CONSTANT_TEXTURE_SIZE)
    pipelineState.specConstants |= g_textureSizeSpecBits;
#endif
#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
    // Same reasoning. The bones bits (SPEC_CONSTANT_BONES_SPECIALIZED/HAS_BONES) are in the masks of
    // the shaders that use them but never in the state before masking: the render thread adds them to
    // a copy of the sanitized state (CreateGraphicsPipelineInRenderThread).
    pipelineState.specConstants |= g_shadowGatherSpecBits;
    if (g_relativeFromMemory && (specConstantsMask & SPEC_CONSTANT_RELATIVE_FROM_MEMORY) != 0)
        pipelineState.specConstants |= SPEC_CONSTANT_RELATIVE_FROM_MEMORY;
#endif
}

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
// The render thread's pipeline specialized for the draw: for a vertex shader that branches on mrgHasBone,
// the variant for the draw's value (vertex boolean b0); for a pixel shader whose shadow gathers read only
// slots that are gatherable now, the variant without the check and the point-fetch fallback. Once a
// compiler thread has built it. Until then the skinning-only variant if there is one, else nullptr: the
// generic pipeline draws, exactly as before. All of them produce the same image.
static RenderPipeline* GetSpecializedPipeline(const PipelineState& pipelineState);
#endif

static std::unique_ptr<RenderPipeline> CreateGraphicsPipeline(const PipelineState& pipelineState)
{
#ifdef ASYNC_PSO_DEBUG
    ++g_pipelinesCurrentlyCompiling;
#endif

    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_pipelineLayout.get();
    desc.vertexShader = GetOrLinkShader(pipelineState.vertexShader, pipelineState.specConstants);
    desc.pixelShader = pipelineState.pixelShader != nullptr ? GetOrLinkShader(pipelineState.pixelShader, pipelineState.specConstants) : nullptr;
    desc.depthFunction = pipelineState.zFunc;
    desc.depthEnabled = pipelineState.zEnable;
    desc.depthWriteEnabled = pipelineState.zWriteEnable;
    desc.depthBias = pipelineState.depthBias;
    desc.slopeScaledDepthBias = pipelineState.slopeScaledDepthBias;
    desc.dynamicDepthBiasEnabled = g_capabilities.dynamicDepthBias;
    desc.depthClipEnabled = true;
    desc.primitiveTopology = pipelineState.primitiveTopology;
    desc.cullMode = pipelineState.cullMode;
    desc.renderTargetFormat[0] = pipelineState.renderTargetFormat;
    desc.renderTargetBlend[0].blendEnabled = pipelineState.alphaBlendEnable;
    desc.renderTargetBlend[0].srcBlend = pipelineState.srcBlend;
    desc.renderTargetBlend[0].dstBlend = pipelineState.destBlend;
    desc.renderTargetBlend[0].blendOp = pipelineState.blendOp;
    desc.renderTargetBlend[0].srcBlendAlpha = pipelineState.srcBlendAlpha;
    desc.renderTargetBlend[0].dstBlendAlpha = pipelineState.destBlendAlpha;
    desc.renderTargetBlend[0].blendOpAlpha = pipelineState.blendOpAlpha;
    desc.renderTargetBlend[0].renderTargetWriteMask = pipelineState.colorWriteEnable;
    desc.renderTargetCount = pipelineState.renderTargetFormat != RenderFormat::UNKNOWN ? 1 : 0;
    desc.depthTargetFormat = pipelineState.depthStencilFormat;

#if defined(__SWITCH__)
    // [Switch] SwitchCoverageHandOver: the same draw, marking every pixel it writes with the hand-over's
    // reference in an 8-bit stencil buffer of its own (the pass has no depth buffer; the colour output is
    // untouched).
    if (pipelineState.coverageStencil != 0)
    {
        desc.depthTargetFormat = RenderFormat::S8_UINT;
        desc.depthEnabled = false;
        desc.depthWriteEnabled = false;
        desc.stencilEnabled = true;
        desc.stencilReadMask = 0xFF;
        desc.stencilWriteMask = 0xFF;
        desc.stencilReference = 1;
        desc.dynamicStencilReferenceEnabled = true;
        desc.stencilFrontFace.passOp = RenderStencilOp::REPLACE;
        desc.stencilFrontFace.failOp = RenderStencilOp::KEEP;
        desc.stencilFrontFace.depthFailOp = RenderStencilOp::KEEP;
        desc.stencilFrontFace.compareFunction = RenderComparisonFunction::ALWAYS;
        desc.stencilBackFace = desc.stencilFrontFace;
    }

    // Depth-only draw (shadow maps, depth passes): with no colour target, no alpha test and no
    // alpha to coverage, a pixel shader that cannot write depth, the sample mask or kill has no
    // effect at all, so the pipeline is built without a fragment stage and the GPU skips fragment
    // shading for these draws. Depth, and therefore the image, is identical.
    if (desc.pixelShader != nullptr && desc.renderTargetCount == 0 &&
        pipelineState.depthStencilFormat != RenderFormat::UNKNOWN &&
        !pipelineState.enableAlphaToCoverage &&
        (pipelineState.specConstants & SPEC_CONSTANT_ALPHA_TEST) == 0 &&
        pipelineState.pixelShader->removableInDepthOnlyPass.load(std::memory_order_acquire))
    {
        desc.pixelShader = nullptr;
    }
#endif
    desc.multisampling.sampleCount = pipelineState.sampleCount;
    desc.alphaToCoverageEnabled = pipelineState.enableAlphaToCoverage;
    desc.inputElements = pipelineState.vertexDeclaration->inputElements.get();
    desc.inputElementsCount = pipelineState.vertexDeclaration->inputElementCount;
    
    // Constant 0: g_SpecConstants. 1-3: g_SpecUnusedOutputs0-2 (XenosRecomp-switch-perf.patch).
    RenderSpecConstant specConstants[4];
    for (uint32_t i = 0; i < std::size(specConstants); i++)
        specConstants[i].index = i;

    specConstants[0].value = pipelineState.specConstants;
    uint32_t specConstantCount = 1;

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
    // Vertex outputs that nothing reads: all but the position without a pixel shader, otherwise every
    // component of the locations the pixel shader never loads. The translated vertex shader writes 0 to
    // them instead of computing them (shader_common.h); the driver also drops single components the
    // pixel shader leaves unread (NVK_LINK_VARYINGS). Derived from the shaders and the pipeline state
    // alone, which the pipeline key covers. The hand-written shaders have no cache entry and keep all.
    if (g_trimVertexOutputs && pipelineState.vertexShader->shaderCacheEntry != nullptr)
    {
        uint32_t read = ~0u;
        if (desc.pixelShader == nullptr)
            read = 0;
        else if (pipelineState.pixelShader->shaderCacheEntry != nullptr)
            read = pipelineState.pixelShader->inputLocationsRead.load(std::memory_order_acquire);

        for (uint32_t location = 0; location < SPEC_CONSTANT_UNUSED_OUTPUT_COMPONENT_COUNT / 4; location++)
        {
            if ((read & (1u << location)) == 0)
                specConstants[1 + location / 8].value |= 0xFu << ((location % 8) * 4);
        }

        specConstantCount = 4;
    }

    // oC0 components nothing uses: not written (colour write mask, no colour target) and not read by
    // blending through a source-alpha factor, the alpha test or alpha to coverage. The translated pixel
    // shader writes 0 to them, so their math is dropped.
    if (g_trimPixelOutputs && desc.pixelShader != nullptr && pipelineState.pixelShader->shaderCacheEntry != nullptr)
    {
        auto readsSourceAlpha = [](RenderBlend blend)
            {
                return blend == RenderBlend::SRC_ALPHA || blend == RenderBlend::INV_SRC_ALPHA || blend == RenderBlend::SRC_ALPHA_SAT;
            };

        const uint32_t written = desc.renderTargetCount != 0 ? (pipelineState.colorWriteEnable & 0xF) : 0;
        uint32_t used = written;

        if (pipelineState.alphaBlendEnable && (written & 0x7) != 0 &&
            (readsSourceAlpha(pipelineState.srcBlend) || readsSourceAlpha(pipelineState.destBlend)))
        {
            used |= 0x8;
        }

        if ((pipelineState.specConstants & (SPEC_CONSTANT_ALPHA_TEST | SPEC_CONSTANT_ALPHA_TO_COVERAGE)) != 0 ||
            pipelineState.enableAlphaToCoverage)
        {
            used |= 0x8;
        }

        specConstants[0].value |= (~used & 0xFu) << SPEC_CONSTANT_UNUSED_COLOR_SHIFT;
    }
#endif

    if (specConstants[0].value != 0 || specConstantCount > 1)
    {
        desc.specConstants = specConstants;
        desc.specConstantsCount = specConstantCount;
    }
    
    RenderInputSlot inputSlots[16]{};
    uint32_t inputSlotIndices[16]{};
    uint32_t inputSlotCount = 0;
    
    for (size_t i = 0; i < pipelineState.vertexDeclaration->inputElementCount; i++)
    {
        auto& inputElement = pipelineState.vertexDeclaration->inputElements[i];
        auto& inputSlotIndex = inputSlotIndices[inputElement.slotIndex];
    
        if (inputSlotIndex == NULL)
            inputSlotIndex = ++inputSlotCount;
    
        auto& inputSlot = inputSlots[inputSlotIndex - 1];
        inputSlot.index = inputElement.slotIndex;
        inputSlot.stride = pipelineState.vertexStrides[inputElement.slotIndex];
    
        if (pipelineState.instancing && inputElement.slotIndex != 0 && inputElement.slotIndex != 15)
            inputSlot.classification = RenderInputSlotClassification::PER_INSTANCE_DATA;
        else
            inputSlot.classification = RenderInputSlotClassification::PER_VERTEX_DATA;
    }
    
    desc.inputSlots = inputSlots;
    desc.inputSlotsCount = inputSlotCount;
    
    auto pipeline = g_device->createGraphicsPipeline(desc);

#ifdef ASYNC_PSO_DEBUG
    --g_pipelinesCurrentlyCompiling;
#endif

    return pipeline;
}

#if defined(__SWITCH__)
// [Switch] SwitchPipelineLookupCache. The render thread looks up the pipeline of every state it binds: it
// sanitizes a copy, hashes it, looks it up (and its specialized variants). Draws come back to the same few
// states, so the last results are kept, keyed by the raw state and the rest of what the lookup reads (the
// skinning boolean and the gatherable slots, SpecializationBits). Storing any pipeline in g_pipelines
// (ProcAddPipeline, a creation here) starts a new generation: results found before are not used again.
// Shaders and vertex declarations are kept for good once created (CreateShader), as g_pipelines' keys need.
struct PipelineLookupEntry
{
    PipelineState state;
    uint32_t booleans = 0;
    uint32_t gatherableSlots = 0;
    uint32_t generation = 0;
    RenderPipeline* pipeline = nullptr;
};

static PipelineLookupEntry g_pipelineLookups[64];
static uint32_t g_pipelineGeneration = 1;

static PipelineLookupEntry& PipelineLookupSlot(const PipelineState& state)
{
    uint64_t key = reinterpret_cast<uintptr_t>(state.vertexShader) ^ (reinterpret_cast<uintptr_t>(state.pixelShader) >> 3) ^
        (uint64_t(state.specConstants) << 32) ^ (uint64_t(state.colorWriteEnable) << 24) ^ (uint64_t(state.srcBlend) << 40) ^
        (uint64_t(state.destBlend) << 44) ^ (uint64_t(state.alphaBlendEnable) << 48) ^ (uint64_t(state.zEnable) << 49) ^
        (uint64_t(state.zWriteEnable) << 50) ^ (uint64_t(state.cullMode) << 52) ^ (uint64_t(state.renderTargetFormat) << 56);
    key *= 0x9E3779B97F4A7C15ull;
    return g_pipelineLookups[key >> (64 - 6)];
}
#endif

static RenderPipeline* FindOrCreateGraphicsPipeline(PipelineState pipelineState);

static RenderPipeline* CreateGraphicsPipelineInRenderThread(const PipelineState& pipelineState)
{
#if defined(__SWITCH__)
    if (g_pipelineLookupCache)
    {
        auto& entry = PipelineLookupSlot(pipelineState);
        const uint32_t booleans = g_sharedConstants.booleans & 0x1;
        if (entry.generation == g_pipelineGeneration && entry.booleans == booleans &&
            entry.gatherableSlots == g_sharedConstants.gatherableSlots && memcmp(&entry.state, &pipelineState, sizeof(PipelineState)) == 0)
        {
            g_profilerPipelineCacheHits++;
            return entry.pipeline;
        }

        RenderPipeline* pipeline = FindOrCreateGraphicsPipeline(pipelineState);
        entry.state = pipelineState;
        entry.booleans = booleans;
        entry.gatherableSlots = g_sharedConstants.gatherableSlots;
        entry.generation = g_pipelineGeneration;
        entry.pipeline = pipeline;
        return pipeline;
    }
#endif

    return FindOrCreateGraphicsPipeline(pipelineState);
}

static RenderPipeline* FindOrCreateGraphicsPipeline(PipelineState pipelineState)
{
    SanitizePipelineState(pipelineState);

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
    if (RenderPipeline* specialized = GetSpecializedPipeline(pipelineState))
        return specialized;
#endif

    XXH64_hash_t hash = XXH3_64bits(&pipelineState, sizeof(pipelineState));
    auto& pipeline = g_pipelines[hash];
    if (pipeline == nullptr)
    {
        pipeline = CreateGraphicsPipeline(pipelineState);
#if defined(__SWITCH__)
        g_pipelineGeneration++;
#endif

#ifdef ASYNC_PSO_DEBUG
        bool loading = *SWA::SGlobals::ms_IsLoading;

        if (loading)
            ++g_pipelinesCreatedAsynchronously;
        else
            ++g_pipelinesCreatedInRenderThread;

        pipeline->setName(fmt::format("{} {} {} {:X}", loading ? "ASYNC" : "",
            pipelineState.vertexShader->name, pipelineState.pixelShader != nullptr ? pipelineState.pixelShader->name : "<none>", hash));
        
        if (!loading)
        {
            std::lock_guard lock(g_debugMutex);
            g_pipelineDebugText = fmt::format(
                "PipelineState {:X}:\n"
                "  vertexShader: {}\n"
                "  pixelShader: {}\n"
                "  vertexDeclaration: {:X}\n"
                "  instancing: {}\n"
                "  zEnable: {}\n"
                "  zWriteEnable: {}\n"
                "  srcBlend: {}\n"
                "  destBlend: {}\n"
                "  cullMode: {}\n"
                "  zFunc: {}\n"
                "  alphaBlendEnable: {}\n"
                "  blendOp: {}\n"
                "  slopeScaledDepthBias: {}\n"
                "  depthBias: {}\n"
                "  srcBlendAlpha: {}\n"
                "  destBlendAlpha: {}\n"
                "  blendOpAlpha: {}\n"
                "  colorWriteEnable: {:X}\n"
                "  primitiveTopology: {}\n"
                "  vertexStrides[0]: {}\n"
                "  vertexStrides[1]: {}\n"
                "  vertexStrides[2]: {}\n"
                "  vertexStrides[3]: {}\n"
                "  renderTargetFormat: {}\n"
                "  depthStencilFormat: {}\n"
                "  sampleCount: {}\n"
                "  enableAlphaToCoverage: {}\n"
                "  specConstants: {:X}\n",
                hash,
                pipelineState.vertexShader->name,
                pipelineState.pixelShader != nullptr ? pipelineState.pixelShader->name : "<none>",
                reinterpret_cast<size_t>(pipelineState.vertexDeclaration),
                pipelineState.instancing,
                pipelineState.zEnable,
                pipelineState.zWriteEnable,
                magic_enum::enum_name(pipelineState.srcBlend),
                magic_enum::enum_name(pipelineState.destBlend),
                magic_enum::enum_name(pipelineState.cullMode),
                magic_enum::enum_name(pipelineState.zFunc),
                pipelineState.alphaBlendEnable,
                magic_enum::enum_name(pipelineState.blendOp),
                pipelineState.slopeScaledDepthBias,
                pipelineState.depthBias,
                magic_enum::enum_name(pipelineState.srcBlendAlpha),
                magic_enum::enum_name(pipelineState.destBlendAlpha),
                magic_enum::enum_name(pipelineState.blendOpAlpha),
                pipelineState.colorWriteEnable,
                magic_enum::enum_name(pipelineState.primitiveTopology),
                pipelineState.vertexStrides[0],
                pipelineState.vertexStrides[1],
                pipelineState.vertexStrides[2],
                pipelineState.vertexStrides[3],
                magic_enum::enum_name(pipelineState.renderTargetFormat),
                magic_enum::enum_name(pipelineState.depthStencilFormat),
                pipelineState.sampleCount,
                pipelineState.enableAlphaToCoverage,
                pipelineState.specConstants)
                + g_pipelineDebugText;
        }
#endif

#ifdef PSO_CACHING
        std::lock_guard lock(g_pipelineCacheMutex);
        g_pipelineStatesToCache.emplace(hash, pipelineState);
#endif
    }
    
    return pipeline.get();
}

static RenderTextureAddressMode ConvertTextureAddressMode(size_t value)
{
    switch (value)
    {
    case D3DTADDRESS_WRAP:
        return RenderTextureAddressMode::WRAP;
    case D3DTADDRESS_MIRROR:
        return RenderTextureAddressMode::MIRROR;
    case D3DTADDRESS_CLAMP:
        return RenderTextureAddressMode::CLAMP;
    case D3DTADDRESS_MIRRORONCE:
        return RenderTextureAddressMode::MIRROR_ONCE;
    case D3DTADDRESS_BORDER:
        return RenderTextureAddressMode::BORDER;
    default:
        assert(false && "Unknown texture address mode");
        return RenderTextureAddressMode::UNKNOWN;
    }
}

static RenderFilter ConvertTextureFilter(uint32_t value)
{
    switch (value)
    {
    case D3DTEXF_POINT:
    case D3DTEXF_NONE:
        return RenderFilter::NEAREST;
    case D3DTEXF_LINEAR:
        return RenderFilter::LINEAR;
    default:
        assert(false && "Unknown texture filter");
        return RenderFilter::UNKNOWN;
    }
}

static RenderBorderColor ConvertBorderColor(uint32_t value)
{
    switch (value)
    {
    case 0:
        return RenderBorderColor::TRANSPARENT_BLACK;
    case 1:
        return RenderBorderColor::OPAQUE_WHITE;
    default:
        assert(false && "Unknown border color");
        return RenderBorderColor::UNKNOWN;
    }
}

struct LocalRenderCommandQueue
{
    // Booleans, 16 samplers, up to 4 + 4 constant runs and the draw.
    RenderCommand commands[32];
    uint32_t count = 0;

    RenderCommand& enqueue()
    {
        assert(count < std::size(commands));
        return commands[count++];
    }

    void submit()
    {
        if (ShouldBatchRenderCommands())
        {
            // The draw goes out together with the state changes that preceded it.
            auto& batch = g_deferredRenderCommands;
            if (batch.count + count > RenderCommandBatchCapacity())
                FlushDeferredRenderCommands();

            for (uint32_t i = 0; i < count; i++)
                batch.commands[batch.count++] = commands[i];

#if defined(__SWITCH__)
            // [Switch] SwitchBatchSeveralDraws. Each hand-over of the batch can wake the render thread,
            // which has usually gone to sleep between two draws: a kernel signal paid on this thread,
            // the bottleneck when the CPU limits the frame rate. The batch now goes out once it holds a
            // few draws' worth of commands (half its size), at the flush points (Present, unlocks, ImGui,
            // everything a caller waits for) or when the next draw would not fit. The order and the
            // content of the commands are unchanged: every one either carries its data or is flushed.
            if (g_batchSeveralDraws && batch.count < RenderCommandBatchThreshold())
                return;
#endif
            FlushDeferredRenderCommands();
        }
        else
        {
            g_renderQueue.enqueue_bulk(commands, count);
        }
    }
};

#if defined(__SWITCH__)
// The changed float constants of one stage, from the device's dirty flags: bit 63 - g means registers
// 4g..4g+3 (64 bytes) changed. Before, the whole span from the first changed group to the last one was
// copied, unchanged registers in between included. [Switch] SwitchSparseConstantCopies copies each run
// of consecutive changed groups instead (at most four; more runs copy the span as before). The render
// thread applies each copy to its own array, which already holds the unchanged registers, so it ends up
// with the same values either way.
static void EnqueueShaderConstants(LocalRenderCommandQueue& queue, RenderCommandType type, const uint32_t* constants,
    uint64_t dirty, uint32_t groupCount)
{
    static constexpr uint32_t MAX_RUNS = 4;

    auto enqueue = [&](uint32_t group, uint32_t groups)
        {
            const uint32_t index = group * 16;
            const uint32_t size = groups * 64;
            auto& cmd = queue.enqueue();
            cmd.type = type;
            if (type == RenderCommandType::SetVertexShaderConstants)
            {
                cmd.setVertexShaderConstants.memory = g_intermediaryUploadAllocator.allocate(&constants[index], size);
                cmd.setVertexShaderConstants.index = index;
                cmd.setVertexShaderConstants.size = size;
            }
            else
            {
                cmd.setPixelShaderConstants.memory = g_intermediaryUploadAllocator.allocate(&constants[index], size);
                cmd.setPixelShaderConstants.index = index;
                cmd.setPixelShaderConstants.size = size;
            }
            return size;
        };

    // Groups past the stage's registers are not constants.
    if (groupCount < 64)
        dirty &= ~((uint64_t(1) << (64 - groupCount)) - 1);
    if (dirty == 0)
        return;

    const uint32_t first = std::countl_zero(dirty);
    const uint32_t end = 64 - std::countr_zero(dirty);
    const uint32_t spanBytes = (end - first) * 64;
    uint32_t copied = 0;

    bool sparse = false;
    if (g_sparseConstantCopies)
    {
        uint32_t runs = 0;
        for (uint64_t bits = dirty; bits != 0 && runs <= MAX_RUNS; runs++)
        {
            const uint32_t start = std::countl_zero(bits);
            const uint32_t length = std::countl_one(bits << start);
            bits = (start + length >= 64) ? 0 : (bits & (~uint64_t(0) >> (start + length)));
        }

        if (runs <= MAX_RUNS)
        {
            sparse = true;
            for (uint64_t bits = dirty; bits != 0;)
            {
                const uint32_t start = std::countl_zero(bits);
                const uint32_t length = std::countl_one(bits << start);
                copied += enqueue(start, length);
                bits = (start + length >= 64) ? 0 : (bits & (~uint64_t(0) >> (start + length)));
            }
        }
    }

    if (!sparse)
        copied = enqueue(first, end - first);

    AddGameThreadCount(g_profilerMainConstantSpanBytes, spanBytes);
    AddGameThreadCount(g_profilerMainConstantCopiedBytes, copied);
}
#endif

static void FlushRenderStateForMainThread(GuestDevice* device, LocalRenderCommandQueue& queue)
{
    constexpr size_t BOOL_MASK = 0x100000000000000ull;
    if ((device->dirtyFlags[4].get() & BOOL_MASK) != 0)
    {
        auto& cmd = queue.enqueue();
        cmd.type = RenderCommandType::SetBooleans;
        cmd.setBooleans.booleans = (device->vertexShaderBoolConstants[0].get() & 0xFF) | ((device->pixelShaderBoolConstants[0].get() & 0xFF) << 16);

        device->dirtyFlags[4] = device->dirtyFlags[4].get() & ~BOOL_MASK;
    }

#if defined(__SWITCH__)
    // The dirty samplers are bits 31-16 of the word (sampler i is bit 31 - i), read and cleared once.
    const uint64_t samplerDirtyFlags = device->dirtyFlags[3].get();
    if ((samplerDirtyFlags & 0xFFFF0000ull) != 0)
    {
        const bool filter = g_skipRedundantSamplerStates && EnterStateFilter();
        for (uint32_t dirty = uint32_t(samplerDirtyFlags >> 16) & 0xFFFF; dirty != 0;)
        {
            const uint32_t i = std::countl_zero(dirty) - 16;
            dirty &= ~(0x8000u >> i);

            const uint32_t data0 = device->samplerStates[i].data[0];
            const uint32_t data3 = device->samplerStates[i].data[3];
            const uint32_t data5 = device->samplerStates[i].data[5];

            // SwitchSkipRedundantSamplerStates (StateFilter).
            if (filter)
            {
                auto& filterState = g_stateFilter;
                uint32_t* known = filterState.samplerStates[i];
                if ((filterState.samplerStatesKnown & (1u << i)) != 0 && known[0] == data0 && known[1] == data3 && known[2] == data5)
                {
                    AddGameThreadCount(g_profilerStatesSkipped, 1);
                    continue;
                }

                filterState.samplerStatesKnown |= 1u << i;
                known[0] = data0;
                known[1] = data3;
                known[2] = data5;
            }

            auto& cmd = queue.enqueue();
            cmd.type = RenderCommandType::SetSamplerState;
            cmd.setSamplerState.index = i;
            cmd.setSamplerState.data0 = data0;
            cmd.setSamplerState.data3 = data3;
            cmd.setSamplerState.data5 = data5;
        }

        device->dirtyFlags[3] = samplerDirtyFlags & ~0xFFFF0000ull;
    }
#else
    for (uint32_t i = 0; i < 16; i++)
    {
        const size_t mask = 0x8000000000000000ull >> (i + 32);
        if (device->dirtyFlags[3].get() & mask)
        {
            auto& cmd = queue.enqueue();
            cmd.type = RenderCommandType::SetSamplerState;
            cmd.setSamplerState.index = i;
            cmd.setSamplerState.data0 = device->samplerStates[i].data[0];
            cmd.setSamplerState.data3 = device->samplerStates[i].data[3];
            cmd.setSamplerState.data5 = device->samplerStates[i].data[5];

            device->dirtyFlags[3] = device->dirtyFlags[3].get() & ~mask;
        }
    }
#endif

#if defined(__SWITCH__)
    uint64_t dirtyFlags = device->dirtyFlags[0].get();
    if (dirtyFlags != 0)
    {
        EnqueueShaderConstants(queue, RenderCommandType::SetVertexShaderConstants, device->vertexShaderFloatConstants, dirtyFlags, 64);
        device->dirtyFlags[0] = 0;
    }

    dirtyFlags = device->dirtyFlags[1].get();
    if (dirtyFlags != 0)
    {
        EnqueueShaderConstants(queue, RenderCommandType::SetPixelShaderConstants, device->pixelShaderFloatConstants, dirtyFlags, 56);
        device->dirtyFlags[1] = 0;
    }
#else
    uint64_t dirtyFlags = device->dirtyFlags[0].get();
    if (dirtyFlags != 0)
    {
        int startRegister = std::countl_zero(dirtyFlags);
        int endRegister = 64 - std::countr_zero(dirtyFlags);

        uint32_t index = startRegister * 16;
        uint32_t size = (endRegister - startRegister) * 64;

        auto& cmd = queue.enqueue();
        cmd.type = RenderCommandType::SetVertexShaderConstants;
        cmd.setVertexShaderConstants.memory = g_intermediaryUploadAllocator.allocate(&device->vertexShaderFloatConstants[index], size);
        cmd.setVertexShaderConstants.index = index;
        cmd.setVertexShaderConstants.size = size;

        device->dirtyFlags[0] = 0;
    }

    dirtyFlags = device->dirtyFlags[1].get();
    if (dirtyFlags != 0)
    {
        int startRegister = std::countl_zero(dirtyFlags);
        int endRegister = std::min(56, 64 - std::countr_zero(dirtyFlags));

        uint32_t index = startRegister * 16;
        uint32_t size = (endRegister - startRegister) * 64;

        auto& cmd = queue.enqueue();
        cmd.type = RenderCommandType::SetPixelShaderConstants;
        cmd.setPixelShaderConstants.memory = g_intermediaryUploadAllocator.allocate(&device->pixelShaderFloatConstants[index], size);
        cmd.setPixelShaderConstants.index = index;
        cmd.setPixelShaderConstants.size = size;

        device->dirtyFlags[1] = 0;
    }
#endif
}

static void ProcSetBooleans(const RenderCommand& cmd)
{
#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
    // The bound pipeline may be the variant for the other value of mrgHasBone (vertex boolean b0):
    // choose the pipeline again before the next draw.
    if (g_bonesSpecialization && ((g_sharedConstants.booleans ^ cmd.setBooleans.booleans) & 0x1) != 0)
        g_dirtyStates.pipelineState = true;
#endif

    SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.booleans, cmd.setBooleans.booleans);
}

#if defined(__SWITCH__)
// [Switch] SwitchSamplerCache. The sampler state words' bits ProcSetSamplerState reads (address modes, filters,
// border colour) and the anisotropic filtering setting give its description and descriptor, which are kept
// for the words seen last: a repeated state skips the conversions, the hash and the lookup. Every sampler
// description starts from the same default, and a description's descriptor never changes.
struct SamplerLookupEntry
{
    uint32_t key = ~0u;
    uint32_t anisotropicFiltering = 0;
    RenderSamplerDesc desc;
    uint32_t descriptorIndex = 0;
};

static SamplerLookupEntry g_samplerLookups[64];
#endif

static void ProcSetSamplerState(const RenderCommand& cmd)
{
    const auto& args = cmd.setSamplerState;

#if defined(__SWITCH__)
    SamplerLookupEntry* lookup = nullptr;
    if (g_samplerCache)
    {
        const uint32_t key = ((args.data0 >> 10) & 0x1FF) | (((args.data3 >> 19) & 0x3F) << 9) | ((args.data5 & 0x3) << 15);
        const uint32_t anisotropicFiltering = Config::AnisotropicFiltering;
        lookup = &g_samplerLookups[(key * 0x9E3779B1u) >> (32 - 6)];
        if (lookup->key == key && lookup->anisotropicFiltering == anisotropicFiltering)
        {
            g_profilerSamplerCacheHits++;

            auto& samplerDesc = g_samplerDescs[args.index];
            if (memcmp(&samplerDesc, &lookup->desc, sizeof(RenderSamplerDesc)) != 0)
            {
                samplerDesc = lookup->desc;
                SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.samplerIndices[args.index], lookup->descriptorIndex);

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
                UpdateGatherableSlot(args.index);
#endif
            }

            return;
        }

        lookup->key = key;
        lookup->anisotropicFiltering = anisotropicFiltering;
    }
#endif

    const auto addressU = ConvertTextureAddressMode((args.data0 >> 10) & 0x7);
    const auto addressV = ConvertTextureAddressMode((args.data0 >> 13) & 0x7);
    const auto addressW = ConvertTextureAddressMode((args.data0 >> 16) & 0x7);
    auto magFilter = ConvertTextureFilter((args.data3 >> 19) & 0x3);
    auto minFilter = ConvertTextureFilter((args.data3 >> 21) & 0x3);
    auto mipFilter = ConvertTextureFilter((args.data3 >> 23) & 0x3);
    const auto borderColor = ConvertBorderColor(args.data5 & 0x3);

#if defined(__SWITCH__)
    // Read once, so that a cached description matches the setting it is keyed by.
    const uint32_t anisotropicFiltering = lookup != nullptr ? lookup->anisotropicFiltering : uint32_t(Config::AnisotropicFiltering);
    bool anisotropyEnabled = anisotropicFiltering > 0 && mipFilter == RenderFilter::LINEAR;
#else
    bool anisotropyEnabled = Config::AnisotropicFiltering > 0 && mipFilter == RenderFilter::LINEAR;
#endif
    if (anisotropyEnabled)
    {
        magFilter = RenderFilter::LINEAR;
        minFilter = RenderFilter::LINEAR;
    }

    auto& samplerDesc = g_samplerDescs[args.index];

    bool dirty = false;

    SetDirtyValue(dirty, samplerDesc.addressU, addressU);
    SetDirtyValue(dirty, samplerDesc.addressV, addressV);
    SetDirtyValue(dirty, samplerDesc.addressW, addressW);
    SetDirtyValue(dirty, samplerDesc.minFilter, minFilter);
    SetDirtyValue(dirty, samplerDesc.magFilter, magFilter);
    SetDirtyValue(dirty, samplerDesc.mipmapMode, RenderMipmapMode(mipFilter));
#if defined(__SWITCH__)
    SetDirtyValue(dirty, samplerDesc.maxAnisotropy, anisotropyEnabled ? anisotropicFiltering : 16u);
#else
    SetDirtyValue(dirty, samplerDesc.maxAnisotropy, anisotropyEnabled ? Config::AnisotropicFiltering : 16u);
#endif
    SetDirtyValue(dirty, samplerDesc.anisotropyEnabled, anisotropyEnabled);
    SetDirtyValue(dirty, samplerDesc.borderColor, borderColor);

    if (dirty)
    {
        auto& [descriptorIndex, sampler] = g_samplerStates[XXH3_64bits(&samplerDesc, sizeof(RenderSamplerDesc))];
        if (descriptorIndex == NULL)
        {
            descriptorIndex = g_samplerStates.size();
            sampler = g_device->createSampler(samplerDesc);

            g_samplerDescriptorSet->setSampler(descriptorIndex - 1, sampler.get());
        }

        SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.samplerIndices[args.index], descriptorIndex - 1);

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
        UpdateGatherableSlot(args.index);
#endif
    }

#if defined(__SWITCH__)
    // The slot's description is now this state's, and its descriptor index the description's.
    if (lookup != nullptr)
    {
        lookup->desc = samplerDesc;
        lookup->descriptorIndex = g_sharedConstants.samplerIndices[args.index];
    }
#endif
}

// Stores the byte-swapped words and says whether any of them changed, in one pass over them (before:
// swap into a copy, compare, copy). The stored values are the same.
static bool StoreSwappedConstants(uint32_t* destination, const uint32_t* source, uint32_t count)
{
    uint32_t difference = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t value = ByteSwap(source[i]);
        difference |= destination[i] ^ value;
        destination[i] = value;
    }

    return difference != 0;
}

static void ProcSetVertexShaderConstants(const RenderCommand& cmd)
{
    auto& args = cmd.setVertexShaderConstants;
    assert((args.index * sizeof(uint32_t) + args.size) <= sizeof(g_vertexShaderConstants));

    // The game often sets constants to the values they already have. Only a real change needs
    // the 4 KB block to be uploaded again (and set 4 to be rebound).
    if (StoreSwappedConstants(&g_vertexShaderConstants[args.index], reinterpret_cast<const uint32_t*>(args.memory),
        args.size / sizeof(uint32_t)))
    {
        g_dirtyStates.vertexShaderConstants = true;
    }
}

static void ProcSetPixelShaderConstants(const RenderCommand& cmd)
{
    auto& args = cmd.setPixelShaderConstants;
    assert((args.index * sizeof(uint32_t) + args.size) <= sizeof(g_pixelShaderConstants));

    if (StoreSwappedConstants(&g_pixelShaderConstants[args.index], reinterpret_cast<const uint32_t*>(args.memory),
        args.size / sizeof(uint32_t)))
    {
        g_dirtyStates.pixelShaderConstants = true;
    }
}

static void ProcAddPipeline(const RenderCommand& cmd)
{
    auto& args = cmd.addPipeline;
    auto& pipeline = g_pipelines[args.hash];

    if (pipeline == nullptr)
    {
        pipeline = std::unique_ptr<RenderPipeline>(args.pipeline);
#if defined(__SWITCH__)
        g_pipelineGeneration++; // SwitchPipelineLookupCache: a lookup may now find it.
#endif
#ifdef ASYNC_PSO_DEBUG
        ++g_pipelinesCreatedAsynchronously;
#endif
    }
    else
    {
#ifdef ASYNC_PSO_DEBUG
        ++g_pipelinesDropped;
#endif
        delete args.pipeline;
    }
}

static constexpr int32_t COMMON_DEPTH_BIAS_VALUE = int32_t((1 << 24) * 0.002f);
static constexpr float COMMON_SLOPE_SCALED_DEPTH_BIAS_VALUE = 1.0f;

#if defined(__SWITCH__)
// [Switch] SwitchLazyResolves. A depth surface with pending copies, bound for a draw that does not write
// depth (depth test only), keeps them pending: its contents do not change, so the copies made later
// (when a draw writes it or samples its copy while it is bound, a clear, its destruction) or dropped at
// the end of the frame as before hold the same texels. Only while it is still a depth attachment and no
// texture slot holds one of its textures: then nothing changes layout and nothing needs the copy.
static bool CanLeaveDepthResolvesPending(const GuestSurface* surface)
{
    // Round 9: a depth buffer SwitchEagerDepthTransitions made ready for sampling goes back to being an
    // attachment (the draw's barrier); while no slot holds one of its textures that changes nothing they read.
    const bool attachment = surface->layout == RenderTextureLayout::DEPTH_WRITE ||
        (g_eagerDepthTransitions && surface->layout == RenderTextureLayout::SHADER_READ);
    if (surface->sampleCount != RenderSampleCount::COUNT_1 || !attachment)
        return false;

    for (const auto texture : surface->destinationTextures)
    {
        for (uint32_t i = 0; i < std::size(g_textures); i++)
        {
            if (g_textures[i] == texture)
                return false;
        }
    }

    return true;
}

// [Switch] SwitchTrimConstantUploads. The bytes of a constant block a shader can read: its registers from
// c0 up to the end of the last constant it declares (the whole block if it indexes an array by a0 or aL,
// see XenosRecomp), rounded up to 256 bytes. Nothing past them is read, so nothing past them is copied.
// Hand-written shaders (no cache entry) get the whole block.
static uint32_t ConstantBytesToUpload(const GuestShader* shader, uint32_t blockSize)
{
    if (!g_trimConstantUploads || shader == nullptr || shader->shaderCacheEntry == nullptr)
        return blockSize;

    const uint32_t bytes = (shader->shaderCacheEntry->float4ConstantRegisters * 16 + 0xFF) & ~0xFFu;
    return std::min(bytes, blockSize);
}

// The pipeline of this state is built without a fragment stage (CreateGraphicsPipeline), so nothing
// reads the pixel shader constants.
static bool PipelineHasNoPixelStage(const PipelineState& state)
{
    if (state.pixelShader == nullptr)
        return true;

    const bool noColorTarget = !state.colorWriteEnable || state.renderTargetFormat == RenderFormat::UNKNOWN;
    const bool depthTarget = state.zEnable && state.depthStencilFormat != RenderFormat::UNKNOWN;
    return noColorTarget && depthTarget && !state.enableAlphaToCoverage && (state.specConstants & SPEC_CONSTANT_ALPHA_TEST) == 0 &&
        state.pixelShader->removableInDepthOnlyPass.load(std::memory_order_acquire);
}
#endif

static void FlushRenderStateForRenderThread()
{
    auto renderTarget = g_pipelineState.colorWriteEnable ? g_renderTarget : nullptr;
    auto depthStencil = g_pipelineState.zEnable ? g_depthStencil : nullptr;

#if defined(__SWITCH__)
    if (g_deferredClear.pending)
        ResolveDeferredClear(renderTarget, depthStencil);

    // Round 9, SwitchSkipOverwrittenDepthClears: the waiting clear of this draw's depth buffer is made inside the
    // draw's own pass, once its framebuffer is bound, or not at all (but for the edge pixels) when the draw writes
    // every depth pixel regardless of the old values. A draw that does not use that depth buffer leaves it waiting.
    bool depthClearNow = false;
    EdgePixels depthClearEdges;
    depthClearEdges.count = 0;
    float depthClearValue = 0.0f;
    if (g_deferredDepthClear.pending && depthStencil != nullptr && depthStencil == g_deferredDepthClear.surface)
    {
        g_deferredDepthClear.pending = false;
        depthClearValue = g_deferredDepthClear.z;

        const bool sameSize = renderTarget == nullptr ||
            (renderTarget->width == depthStencil->width && renderTarget->height == depthStencil->height);
        if (sameSize && DrawReplacesDepth(depthStencil, depthClearEdges))
        {
            g_profilerDepthClearsSkipped++;
        }
        else
        {
            depthClearNow = true;
            depthClearEdges.count = 0;
        }
    }

    g_resolveCopyTrigger = PASS_PROFILER_COPIES_BEFORE_DRAW;

    // SwitchSkipDeadCopies: the pending resolves nothing will read are dropped before any copy or hand-over.
    DropDeadPendingResolves(renderTarget);

    if (renderTarget != nullptr && !renderTarget->destinationTextures.empty())
        TryCoverageHandOver(renderTarget, depthStencil);

    auto depthToResolve = depthStencil;
    if (g_lazyResolves && depthToResolve != nullptr && !depthToResolve->destinationTextures.empty() &&
        !g_pipelineState.zWriteEnable && CanLeaveDepthResolvesPending(depthToResolve))
    {
        depthToResolve = nullptr;
    }

    DropDeadPendingResolves(depthToResolve);

    bool foundAny = PopulateBarriersForStretchRect(renderTarget, depthToResolve);
#else
    auto depthToResolve = depthStencil;
    bool foundAny = PopulateBarriersForStretchRect(renderTarget, depthStencil);
#endif

    for (const auto surface : g_pendingMsaaResolves)
    {
        bool isDepthStencil = (surface->format == RenderFormat::D32_FLOAT);
        foundAny |= PopulateBarriersForStretchRect(isDepthStencil ? nullptr : surface, isDepthStencil ? surface : nullptr);
    }

    if (foundAny)
    {
        FlushBarriers();
        ExecutePendingStretchRectCommands(renderTarget, depthToResolve);

        for (const auto surface : g_pendingMsaaResolves)
        {
            bool isDepthStencil = (surface->format == RenderFormat::D32_FLOAT);
            ExecutePendingStretchRectCommands(isDepthStencil ? nullptr : surface, isDepthStencil ? surface : nullptr);
        }
    }

    if (!g_pendingMsaaResolves.empty())
        g_pendingMsaaResolves.clear();

#if defined(__SWITCH__)
    // [Switch] SwitchEagerSampleTransitions. The colour surface the previous draw rendered to, now left
    // for another one, has pending resolves: its textures sample it from now on. Its transition to
    // SHADER_READ goes out with this batch, which the change of render target makes anyway, instead of
    // in a batch of its own when a later draw of the new pass first samples it (a wait for idle in the
    // middle of the pass). Only the barrier moves; if the surface is rendered to again first, it goes
    // back with that change's batch.
    GuestSurface* const previousColorSurface = g_lastColorSurface;
    if (g_eagerSampleTransitions && g_lastColorSurface != nullptr && g_lastColorSurface != renderTarget &&
        renderTarget != nullptr && !g_lastColorSurface->destinationTextures.empty() &&
        g_lastColorSurface->sampleCount == RenderSampleCount::COUNT_1 && g_lastColorSurface != g_backBuffer)
    {
        AddBarrier(g_lastColorSurface, RenderTextureLayout::SHADER_READ);
    }

    if (renderTarget != nullptr)
        g_lastColorSurface = renderTarget;

    // Round 9, SwitchEagerDepthTransitions: the same for the depth buffer of the previous draw, left with pending
    // resolves for another depth buffer or another render target (not for a draw that only turns depth off, which
    // keeps the target). The shadow maps are sampled from the middle of the main pass, whose first such draw
    // otherwise ends the pass for a barrier batch of its own. A depth buffer bound again for draws that only test
    // depth still keeps its resolves pending (CanLeaveDepthResolvesPending).
    if (g_eagerDepthTransitions && g_lastDepthSurface != nullptr && g_lastDepthSurface != depthStencil &&
        (depthStencil != nullptr || renderTarget != previousColorSurface) && !g_lastDepthSurface->destinationTextures.empty() &&
        g_lastDepthSurface->sampleCount == RenderSampleCount::COUNT_1)
    {
        AddBarrier(g_lastDepthSurface, RenderTextureLayout::SHADER_READ);
    }

    if (depthStencil != nullptr)
        g_lastDepthSurface = depthStencil;
#endif

    AddBarrier(renderTarget, RenderTextureLayout::COLOR_WRITE);
    AddBarrier(depthStencil, RenderTextureLayout::DEPTH_WRITE);

#if defined(__SWITCH__)
    if (!g_barrierMap.empty() && !g_dirtyStates.renderTargetAndDepthStencil && !g_coverageFixup.pending)
        g_profilerMidPassBarrierBatches++;
#endif
    FlushBarriers();

#if defined(__SWITCH__)
    if (g_coverageFixup.pending)
        BindCoverageFramebuffer();
    else
#endif
    SetFramebuffer(renderTarget, depthStencil, false);
#if defined(__SWITCH__)
    FinishEdgePixels(renderTarget);

    // The waiting depth clear, in this pass (a coverage hand-over, whose framebuffer has no depth buffer of the
    // game's, never happens with a depth buffer bound: TryCoverageHandOver).
    if ((depthClearNow || depthClearEdges.count != 0) && !g_coverageFixup.pending)
    {
        g_commandLists[g_frame]->clearDepth(true, depthClearValue, depthClearEdges.count != 0 ? depthClearEdges.rects : nullptr,
            depthClearEdges.count);
    }
#endif
    FlushViewport();

    auto& commandList = g_commandLists[g_frame];

    // D3D12 resets depth bias values to the pipeline values, even if they are dynamic.
    // We can reduce unnecessary calls by making common depth bias values part of the pipeline.
    if (g_capabilities.dynamicDepthBias && !g_vulkan)
    {
        bool useDepthBias = (g_depthBias != 0) || (g_slopeScaledDepthBias != 0.0f);

        int32_t depthBias = useDepthBias ? COMMON_DEPTH_BIAS_VALUE : 0;
        float slopeScaledDepthBias = useDepthBias ? COMMON_SLOPE_SCALED_DEPTH_BIAS_VALUE : 0.0f;

        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.depthBias, depthBias);
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.slopeScaledDepthBias, slopeScaledDepthBias);
    }

#if defined(__SWITCH__)
    // A coverage hand-over draws with the stencil variant (its fix-up puts the usual pipeline back).
    if (g_coverageFixup.pending)
    {
        commandList->setPipeline(g_coverageFixup.drawPipeline);
        commandList->setStencilReference(g_coverageFixup.reference);
        g_dirtyStates.pipelineState = false;
    }
#endif

    if (g_dirtyStates.pipelineState)
    {
        commandList->setPipeline(CreateGraphicsPipelineInRenderThread(g_pipelineState));

        // D3D12 resets the depth bias values. Check if they need to be set again.
        if (g_capabilities.dynamicDepthBias && !g_vulkan)
            g_dirtyStates.depthBias = (g_depthBias != g_pipelineState.depthBias) || (g_slopeScaledDepthBias != g_pipelineState.slopeScaledDepthBias);
    }

    if (g_dirtyStates.depthBias && g_capabilities.dynamicDepthBias)
        commandList->setDepthBias(g_depthBias, 0.0f, g_slopeScaledDepthBias);

    bool uploadVertexShaderConstants = g_dirtyStates.vertexShaderConstants;
    bool uploadPixelShaderConstants = g_dirtyStates.pixelShaderConstants;
    bool uploadSharedConstants = g_dirtyStates.sharedConstants;

#if defined(__SWITCH__)
    // [Switch] SwitchSkipUnusedPixelConstants: a draw without a fragment stage leaves them dirty for the
    // next draw that has one (the retry below uploads all three blocks together if it has to).
    const bool skippedPixelConstants = uploadPixelShaderConstants && g_skipUnusedPixelConstants && PipelineHasNoPixelStage(g_pipelineState);
    if (skippedPixelConstants)
        uploadPixelShaderConstants = false;

    // [Switch] SwitchTrimConstantUploads: a block uploaded for a shader that reads fewer registers does
    // not cover this one's; upload it again.
    const uint32_t vertexConstantBytes = ConstantBytesToUpload(g_pipelineState.vertexShader, sizeof(g_vertexShaderConstants));
    const uint32_t pixelConstantBytes = ConstantBytesToUpload(g_pipelineState.pixelShader, sizeof(g_pixelShaderConstants));
    if (vertexConstantBytes > g_vertexConstantBytesUploaded)
        uploadVertexShaderConstants = true;
    if (!skippedPixelConstants && pixelConstantBytes > g_pixelConstantBytesUploaded)
        uploadPixelShaderConstants = true;
#endif

    for (uint32_t attempt = 0; attempt < 2; attempt++)
    {
        if (uploadVertexShaderConstants)
        {
#if defined(__SWITCH__)
            auto vertexShaderConstants = g_uploadAllocators[g_frame].allocateConstants(g_vertexShaderConstants, vertexConstantBytes,
                sizeof(g_vertexShaderConstants), 0x100);
            g_vertexConstantBytesUploaded = vertexConstantBytes;
            g_profilerConstantBytes += vertexConstantBytes;
            g_profilerConstantBytesSaved += sizeof(g_vertexShaderConstants) - vertexConstantBytes;
#else
            auto vertexShaderConstants = g_uploadAllocators[g_frame].allocate<false>(g_vertexShaderConstants, sizeof(g_vertexShaderConstants), 0x100);
#endif
            SetRootDescriptorForDraw(vertexShaderConstants, 0);
#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
            g_constantsUboBinding.Record(0, vertexShaderConstants);
#endif
        }

        if (uploadPixelShaderConstants)
        {
#if defined(__SWITCH__)
            auto pixelShaderConstants = g_uploadAllocators[g_frame].allocateConstants(g_pixelShaderConstants, pixelConstantBytes,
                sizeof(g_pixelShaderConstants), 0x100);
            g_pixelConstantBytesUploaded = pixelConstantBytes;
            g_profilerConstantBytes += pixelConstantBytes;
            g_profilerConstantBytesSaved += sizeof(g_pixelShaderConstants) - pixelConstantBytes;
#else
            auto pixelShaderConstants = g_uploadAllocators[g_frame].allocate<false>(g_pixelShaderConstants, sizeof(g_pixelShaderConstants), 0x100);
#endif
            SetRootDescriptorForDraw(pixelShaderConstants, 1);
#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
            g_constantsUboBinding.Record(1, pixelShaderConstants);
#endif
        }

        if (uploadSharedConstants)
        {
            auto sharedConstants = g_uploadAllocators[g_frame].allocate<false>(&g_sharedConstants, sizeof(g_sharedConstants), 0x100);
            SetRootDescriptorForDraw(sharedConstants, 2);
#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
            g_constantsUboBinding.Record(2, sharedConstants);
#endif
        }

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
        if (!g_constantsUbo || g_constantsUboBinding.InOneBuffer())
            break;

        // A block rolled over into a new 16 MB upload buffer while the others stayed in the
        // previous one. Upload all three again; they fit together in the fresh buffer.
        uploadVertexShaderConstants = true;
        uploadPixelShaderConstants = true;
        uploadSharedConstants = true;
#else
        break;
#endif
    }

#ifdef UNLEASHED_RECOMP_CONSTANTS_UBO
    if (g_constantsUbo)
    {
        g_constantsUboBinding.Bind(commandList.get());
        PushRootAddressesIfNeeded();
    }
#endif

    if (g_dirtyStates.vertexStreamFirst <= g_dirtyStates.vertexStreamLast)
    {
        commandList->setVertexBuffers(
            g_dirtyStates.vertexStreamFirst,
            g_vertexBufferViews + g_dirtyStates.vertexStreamFirst,
            g_dirtyStates.vertexStreamLast - g_dirtyStates.vertexStreamFirst + 1,
            g_inputSlots + g_dirtyStates.vertexStreamFirst);
    }

    if (g_dirtyStates.indices && (!g_vulkan || g_indexBufferView.buffer.ref != nullptr))
        commandList->setIndexBuffer(&g_indexBufferView);

    g_dirtyStates = DirtyStates(false);

#if defined(__SWITCH__)
    if (skippedPixelConstants && !uploadPixelShaderConstants)
        g_dirtyStates.pixelShaderConstants = true;
#endif
}

static RenderPrimitiveTopology ConvertPrimitiveType(uint32_t primitiveType)
{
    switch (primitiveType)
    {
    case D3DPT_POINTLIST:
        return RenderPrimitiveTopology::POINT_LIST;
    case D3DPT_LINELIST:
        return RenderPrimitiveTopology::LINE_LIST;
    case D3DPT_LINESTRIP:
        return RenderPrimitiveTopology::LINE_STRIP;
    case D3DPT_TRIANGLELIST:
    case D3DPT_QUADLIST:
        return RenderPrimitiveTopology::TRIANGLE_LIST;
    case D3DPT_TRIANGLESTRIP:
        return RenderPrimitiveTopology::TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN:
        return g_capabilities.triangleFan ? RenderPrimitiveTopology::TRIANGLE_FAN : RenderPrimitiveTopology::TRIANGLE_LIST;
    default:
        assert(false && "Unknown primitive type");
        return RenderPrimitiveTopology::UNKNOWN;
    }
}

static void SetPrimitiveType(uint32_t primitiveType)
{
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.primitiveTopology, ConvertPrimitiveType(primitiveType));
}

static uint32_t CheckInstancing()
{
    uint32_t indexCount = 0;

    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.instancing, g_pipelineState.vertexDeclaration->indexVertexStream != 0);
    if (g_pipelineState.instancing)
    {
        // Index buffer is passed as a vertex stream
        indexCount = g_vertexBufferViews[g_pipelineState.vertexDeclaration->indexVertexStream].size / 4;
    }

    return indexCount;
}

static void UnsetInstancingStream()
{
    bool dirty = false;
    uint32_t index = g_pipelineState.vertexDeclaration->indexVertexStream;

#if defined(__SWITCH__)
    g_vertexStreamBuffers[index] = nullptr;
#endif
    SetDirtyValue(dirty, g_vertexBufferViews[index].buffer, RenderBufferReference{});
    SetDirtyValue(dirty, g_vertexBufferViews[index].size, 0u);
    SetDirtyValue(dirty, g_inputSlots[index].stride, 0u);

    if (dirty)
    {
        g_dirtyStates.vertexStreamFirst = std::min<uint8_t>(g_dirtyStates.vertexStreamFirst, index);
        g_dirtyStates.vertexStreamLast = std::max<uint8_t>(g_dirtyStates.vertexStreamLast, index);
    }
}

static void DrawPrimitive(GuestDevice* device, uint32_t primitiveType, uint32_t startVertex, uint32_t primitiveCount) 
{
    LocalRenderCommandQueue queue;
    FlushRenderStateForMainThread(device, queue);

    auto& cmd = queue.enqueue();
    cmd.type = RenderCommandType::DrawPrimitive;
    cmd.drawPrimitive.primitiveType = primitiveType;
    cmd.drawPrimitive.startVertex = startVertex;
    cmd.drawPrimitive.primitiveCount = primitiveCount;

    queue.submit();
}

static void ProcDrawPrimitive(const RenderCommand& cmd)
{
    const auto& args = cmd.drawPrimitive;

    SetPrimitiveType(args.primitiveType);

    uint32_t indexCount = CheckInstancing();
    if (indexCount > 0)
    {
        auto& vertexBufferView = g_vertexBufferViews[g_pipelineState.vertexDeclaration->indexVertexStream];

        SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.buffer, vertexBufferView.buffer);
        SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.size, vertexBufferView.size);
        SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.format, RenderFormat::R32_UINT);

        UnsetInstancingStream();
    }

#if defined(__SWITCH__)
    if (g_skipNoOpDraws && IsNoOpDraw())
    {
        g_profilerNoOpDraws++;
        FrameLogDraw("prim", args.primitiveCount, "no-op");
        return;
    }

    if (g_skipRestoreDraws)
        IsIdentityRestore(nullptr, 0, 0);
    FrameLogDraw("prim", args.primitiveCount, nullptr);
#endif

    FlushRenderStateForRenderThread();

    auto& commandList = g_commandLists[g_frame];

#if defined(__SWITCH__)
    PassProfilerCountDraw();
#endif

    if (indexCount > 0)
        commandList->drawIndexedInstanced(indexCount, args.primitiveCount / indexCount, 0, 0, 0);
    else
        commandList->drawInstanced(args.primitiveCount, 1, args.startVertex, 0);

#if defined(__SWITCH__)
    DrawProfilerAfterDraw(args.primitiveCount, indexCount > 0);
    FinishCoverageFixup();
#endif
}

static void DrawIndexedPrimitive(GuestDevice* device, uint32_t primitiveType, int32_t baseVertexIndex, uint32_t startIndex, uint32_t primCount)
{
    LocalRenderCommandQueue queue;
    FlushRenderStateForMainThread(device, queue);

    auto& cmd = queue.enqueue();
    cmd.type = RenderCommandType::DrawIndexedPrimitive;
    cmd.drawIndexedPrimitive.primitiveType = primitiveType;
    cmd.drawIndexedPrimitive.baseVertexIndex = baseVertexIndex;
    cmd.drawIndexedPrimitive.startIndex = startIndex;
    cmd.drawIndexedPrimitive.primCount = primCount;

    queue.submit();
}

static void ProcDrawIndexedPrimitive(const RenderCommand& cmd)
{
    const auto& args = cmd.drawIndexedPrimitive;

    uint32_t indexCount = CheckInstancing();
    if (indexCount > 0)
        UnsetInstancingStream();

    SetPrimitiveType(args.primitiveType);

#if defined(__SWITCH__)
    if (g_skipNoOpDraws && IsNoOpDraw())
    {
        g_profilerNoOpDraws++;
        FrameLogDraw("indexed", args.primCount, "no-op");
        return;
    }

    if (g_skipRestoreDraws)
        IsIdentityRestore(nullptr, 0, 0);
    FrameLogDraw("indexed", args.primCount, nullptr);
#endif

    FlushRenderStateForRenderThread();

#if defined(__SWITCH__)
    PassProfilerCountDraw();
#endif
    g_commandLists[g_frame]->drawIndexedInstanced(args.primCount, 1, args.startIndex, args.baseVertexIndex, 0);
#if defined(__SWITCH__)
    DrawProfilerAfterDraw(args.primCount, false);
    FinishCoverageFixup();
#endif
}

static void DrawPrimitiveUP(GuestDevice* device, uint32_t primitiveType, uint32_t primitiveCount, void* vertexStreamZeroData, uint32_t vertexStreamZeroStride)
{
    LocalRenderCommandQueue queue;
    FlushRenderStateForMainThread(device, queue);

    auto& cmd = queue.enqueue();
    cmd.type = RenderCommandType::DrawPrimitiveUP;
    cmd.drawPrimitiveUP.primitiveType = primitiveType;
    cmd.drawPrimitiveUP.primitiveCount = primitiveCount;
    cmd.drawPrimitiveUP.vertexStreamZeroData = g_intermediaryUploadAllocator.allocate(vertexStreamZeroData, primitiveCount * vertexStreamZeroStride);
    cmd.drawPrimitiveUP.vertexStreamZeroSize = primitiveCount * vertexStreamZeroStride;
    cmd.drawPrimitiveUP.vertexStreamZeroStride = vertexStreamZeroStride;
    cmd.drawPrimitiveUP.csdFilterState = g_csdFilterState;
    
    queue.submit();
}

static void ProcDrawPrimitiveUP(const RenderCommand& cmd)
{
    const auto& args = cmd.drawPrimitiveUP;

    uint32_t indexCount = CheckInstancing();
    if (indexCount > 0)
        UnsetInstancingStream();

    SetPrimitiveType(args.primitiveType);
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexStrides[0], uint8_t(args.vertexStreamZeroStride));

    auto allocation = g_uploadAllocators[g_frame].allocate<true>(reinterpret_cast<const uint32_t*>(args.vertexStreamZeroData), args.vertexStreamZeroSize, 0x4);

#if defined(__SWITCH__)
    g_vertexStreamBuffers[0] = nullptr;
#endif
    auto& vertexBufferView = g_vertexBufferViews[0];
    vertexBufferView.size = args.primitiveCount * args.vertexStreamZeroStride;
    vertexBufferView.buffer = allocation.buffer->at(allocation.offset);
    g_inputSlots[0].stride = args.vertexStreamZeroStride;
    g_dirtyStates.vertexStreamFirst = 0;

    indexCount = 0;

    if (args.primitiveType == D3DPT_QUADLIST)
        indexCount = g_quadIndexData.prepare(args.primitiveCount);
    else if (!g_capabilities.triangleFan && args.primitiveType == D3DPT_TRIANGLEFAN)
        indexCount = g_triangleFanIndexData.prepare(args.primitiveCount);

    if (args.csdFilterState != CsdFilterState::Unknown &&
        (g_pipelineState.pixelShader == g_csdShader || g_pipelineState.pixelShader == g_csdFilterShader.get()))
    {
        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.pixelShader,
            args.csdFilterState == CsdFilterState::On ? g_csdFilterShader.get() : g_csdShader);
    }

#if defined(__SWITCH__)
    if (g_skipNoOpDraws && IsNoOpDraw())
    {
        g_profilerNoOpDraws++;
        FrameLogDraw("up", args.primitiveCount, "no-op");
        return;
    }

    if (g_skipRestoreDraws && IsIdentityRestore(reinterpret_cast<const uint8_t*>(args.vertexStreamZeroData), args.primitiveCount,
        args.vertexStreamZeroStride))
    {
        FrameLogDraw("up", args.primitiveCount, "restore");
        return;
    }
    FrameLogDraw("up", args.primitiveCount, nullptr);

    g_currentDrawVertices.data = args.vertexStreamZeroData;
    g_currentDrawVertices.count = args.primitiveCount;
    g_currentDrawVertices.stride = args.vertexStreamZeroStride;
    g_currentDrawVertices.primitiveType = args.primitiveType;
#endif

    FlushRenderStateForRenderThread();

#if defined(__SWITCH__)
    g_currentDrawVertices.data = nullptr;
    PassProfilerCountDraw();
#endif

    if (indexCount != 0)
        g_commandLists[g_frame]->drawIndexedInstanced(indexCount, 1, 0, 0, 0);
    else
        g_commandLists[g_frame]->drawInstanced(args.primitiveCount, 1, 0, 0);

#if defined(__SWITCH__)
    DrawProfilerAfterDraw(indexCount != 0 ? indexCount : args.primitiveCount, false);
    FinishCoverageFixup();
#endif
}

static const char* ConvertDeclUsage(uint32_t usage)
{
    switch (usage)
    {
    case D3DDECLUSAGE_POSITION:
        return "POSITION";
    case D3DDECLUSAGE_BLENDWEIGHT:
        return "BLENDWEIGHT";
    case D3DDECLUSAGE_BLENDINDICES:
        return "BLENDINDICES";
    case D3DDECLUSAGE_NORMAL:
        return "NORMAL";
    case D3DDECLUSAGE_PSIZE:
        return "PSIZE";
    case D3DDECLUSAGE_TEXCOORD:
        return "TEXCOORD";
    case D3DDECLUSAGE_TANGENT:
        return "TANGENT";
    case D3DDECLUSAGE_BINORMAL:
        return "BINORMAL";
    case D3DDECLUSAGE_TESSFACTOR:
        return "TESSFACTOR";
    case D3DDECLUSAGE_POSITIONT:
        return "POSITIONT";
    case D3DDECLUSAGE_COLOR:
        return "COLOR";
    case D3DDECLUSAGE_FOG:
        return "FOG";
    case D3DDECLUSAGE_DEPTH:
        return "DEPTH";
    case D3DDECLUSAGE_SAMPLE:
        return "SAMPLE";
    default:
        assert(false && "Unknown usage");
        return "UNKNOWN";
    }
}

static RenderFormat ConvertDeclType(uint32_t type)
{
    switch (type)
    {
    case D3DDECLTYPE_FLOAT1:
        return RenderFormat::R32_FLOAT;
    case D3DDECLTYPE_FLOAT2:
        return RenderFormat::R32G32_FLOAT;
    case D3DDECLTYPE_FLOAT3:
        return RenderFormat::R32G32B32_FLOAT;
    case D3DDECLTYPE_FLOAT4:
        return RenderFormat::R32G32B32A32_FLOAT;
    case D3DDECLTYPE_D3DCOLOR:
        return RenderFormat::B8G8R8A8_UNORM;
    case D3DDECLTYPE_UBYTE4:
    case D3DDECLTYPE_UBYTE4_2:
        return RenderFormat::R8G8B8A8_UINT;
    case D3DDECLTYPE_SHORT2:
        return RenderFormat::R16G16_SINT;
    case D3DDECLTYPE_SHORT4:
        return RenderFormat::R16G16B16A16_SINT;
    case D3DDECLTYPE_UBYTE4N:
    case D3DDECLTYPE_UBYTE4N_2:
        return RenderFormat::R8G8B8A8_UNORM;
    case D3DDECLTYPE_SHORT2N:
        return RenderFormat::R16G16_SNORM;
    case D3DDECLTYPE_SHORT4N:
        return RenderFormat::R16G16B16A16_SNORM;
    case D3DDECLTYPE_USHORT2N:
        return RenderFormat::R16G16_UNORM;
    case D3DDECLTYPE_USHORT4N:
        return RenderFormat::R16G16B16A16_UNORM;
    case D3DDECLTYPE_UINT1:
        return RenderFormat::R32_UINT;
    case D3DDECLTYPE_DEC3N_2:
    case D3DDECLTYPE_DEC3N_3:
        return RenderFormat::R32_UINT;
    case D3DDECLTYPE_FLOAT16_2:
        return RenderFormat::R16G16_FLOAT;
    case D3DDECLTYPE_FLOAT16_4:
        return RenderFormat::R16G16B16A16_FLOAT;
    default:
        assert(false && "Unknown type");
        return RenderFormat::UNKNOWN;
    }
}

static GuestVertexDeclaration* CreateVertexDeclarationWithoutAddRef(GuestVertexElement* vertexElements) 
{
    size_t vertexElementCount = 0;
    auto vertexElement = vertexElements;

    while (vertexElement->stream != 0xFF && vertexElement->type != D3DDECLTYPE_UNUSED)
    {
        vertexElement->padding = 0;
        ++vertexElement;
        ++vertexElementCount;
    }

    vertexElement->padding = 0; // Clear the padding in D3DDECL_END() 

    std::lock_guard lock(g_vertexDeclarationMutex);

    XXH64_hash_t hash = XXH3_64bits(vertexElements, vertexElementCount * sizeof(GuestVertexElement));
    auto& vertexDeclaration = g_vertexDeclarations[hash];

    if (vertexDeclaration == nullptr)
    {
        vertexDeclaration = g_userHeap.AllocPhysical<GuestVertexDeclaration>(ResourceType::VertexDeclaration);
        vertexDeclaration->hash = hash;

        static std::vector<RenderInputElement> inputElements;
        inputElements.clear();

        struct Location
        {
            uint32_t usage;
            uint32_t usageIndex;
            uint32_t location;
        };

        constexpr Location locations[] =
        {
            { D3DDECLUSAGE_POSITION, 0, 0 },
            { D3DDECLUSAGE_NORMAL, 0, 1 },
            { D3DDECLUSAGE_TANGENT, 0, 2 },
            { D3DDECLUSAGE_BINORMAL, 0, 3 },
            { D3DDECLUSAGE_TEXCOORD, 0, 4 },
            { D3DDECLUSAGE_TEXCOORD, 1, 5 },
            { D3DDECLUSAGE_TEXCOORD, 2, 6 },
            { D3DDECLUSAGE_TEXCOORD, 3, 7 },
            { D3DDECLUSAGE_COLOR, 0, 8 },
            { D3DDECLUSAGE_BLENDINDICES, 0, 9 },
            { D3DDECLUSAGE_BLENDWEIGHT, 0, 10 },
            { D3DDECLUSAGE_COLOR, 1, 11 },
            { D3DDECLUSAGE_TEXCOORD, 4, 12 },
            { D3DDECLUSAGE_TEXCOORD, 5, 13 },
            { D3DDECLUSAGE_TEXCOORD, 6, 14 },
            { D3DDECLUSAGE_TEXCOORD, 7, 15 },
            { D3DDECLUSAGE_POSITION, 1, 15 }
        };

        vertexElement = vertexElements;
        while (vertexElement->stream != 0xFF && vertexElement->type != D3DDECLTYPE_UNUSED)
        {
            if (vertexElement->usage == D3DDECLUSAGE_POSITION && vertexElement->usageIndex == 2)
            {
                ++vertexElement;
                continue;
            }

            auto& inputElement = inputElements.emplace_back();
            
            inputElement.semanticName = ConvertDeclUsage(vertexElement->usage);
            inputElement.semanticIndex = vertexElement->usageIndex;
            inputElement.location = ~0;

            for (auto& location : locations)
            {
                if (location.usage == vertexElement->usage && location.usageIndex == vertexElement->usageIndex)
                {
                    inputElement.location = location.location;
                    break;
                }
            }

            assert(inputElement.location != ~0);

            inputElement.format = ConvertDeclType(vertexElement->type);
            inputElement.slotIndex = vertexElement->stream;
            inputElement.alignedByteOffset = vertexElement->offset;

            switch (vertexElement->usage)
            {
            case D3DDECLUSAGE_POSITION:
                if (vertexElement->usageIndex == 1)
                    vertexDeclaration->indexVertexStream = vertexElement->stream;
                break;

            case D3DDECLUSAGE_NORMAL:
            case D3DDECLUSAGE_TANGENT:
            case D3DDECLUSAGE_BINORMAL:
                if (vertexElement->type == D3DDECLTYPE_FLOAT3)
                    inputElement.format = RenderFormat::R32G32B32_UINT;
                else
                    vertexDeclaration->hasR11G11B10Normal = true;
                break;

            case D3DDECLUSAGE_TEXCOORD:
                switch (vertexElement->type)
                {
                case D3DDECLTYPE_SHORT2:
                case D3DDECLTYPE_SHORT4:
                case D3DDECLTYPE_SHORT2N:
                case D3DDECLTYPE_SHORT4N:
                case D3DDECLTYPE_USHORT2N:
                case D3DDECLTYPE_USHORT4N:
                case D3DDECLTYPE_FLOAT16_2:
                case D3DDECLTYPE_FLOAT16_4:
                    vertexDeclaration->swappedTexcoords |= 1 << vertexElement->usageIndex;
                    break;
                }

                break;
            }

            vertexDeclaration->vertexStreams[vertexElement->stream] = true;

            ++vertexElement;
        }

        auto addInputElement = [&](uint32_t usage, uint32_t usageIndex)
            {
                uint32_t location = ~0;

                for (auto& alsoLocation : locations)
                {
                    if (alsoLocation.usage == usage && alsoLocation.usageIndex == usageIndex)
                    {
                        location = alsoLocation.location;
                        break;
                    }
                }

                assert(location != ~0);

                for (auto& inputElement : inputElements)
                {
                    if (inputElement.location == location)
                        return;
                }

                auto format = RenderFormat::R32_FLOAT;
                switch (usage)
                {
                case D3DDECLUSAGE_NORMAL:
                case D3DDECLUSAGE_TANGENT:
                case D3DDECLUSAGE_BINORMAL:
                case D3DDECLUSAGE_BLENDINDICES:
                    format = RenderFormat::R32_UINT;
                    break;
                }

                inputElements.emplace_back(ConvertDeclUsage(usage), usageIndex, location, format, 15, 0);
            };

        addInputElement(D3DDECLUSAGE_POSITION, 0);
        addInputElement(D3DDECLUSAGE_NORMAL, 0);
        addInputElement(D3DDECLUSAGE_TANGENT, 0);
        addInputElement(D3DDECLUSAGE_BINORMAL, 0);
        addInputElement(D3DDECLUSAGE_TEXCOORD, 0);
        addInputElement(D3DDECLUSAGE_TEXCOORD, 1);
        addInputElement(D3DDECLUSAGE_TEXCOORD, 2);
        addInputElement(D3DDECLUSAGE_TEXCOORD, 3);
        addInputElement(D3DDECLUSAGE_COLOR, 0);
        addInputElement(D3DDECLUSAGE_BLENDWEIGHT, 0);
        addInputElement(D3DDECLUSAGE_BLENDINDICES, 0);

        vertexDeclaration->inputElements = std::make_unique<RenderInputElement[]>(inputElements.size());
        std::copy(inputElements.begin(), inputElements.end(), vertexDeclaration->inputElements.get());

        vertexDeclaration->vertexElements = std::make_unique<GuestVertexElement[]>(vertexElementCount + 1);
        std::copy(vertexElements, vertexElements + vertexElementCount + 1, vertexDeclaration->vertexElements.get());

        vertexDeclaration->inputElementCount = uint32_t(inputElements.size());
        vertexDeclaration->vertexElementCount = vertexElementCount + 1;
    }

    vertexDeclaration->AddRef();
    return vertexDeclaration;
}

static GuestVertexDeclaration* CreateVertexDeclaration(GuestVertexElement* vertexElements)
{
    auto vertexDeclaration = CreateVertexDeclarationWithoutAddRef(vertexElements);
    vertexDeclaration->AddRef();
    return vertexDeclaration;
}

static void SetVertexDeclaration(GuestDevice* device, GuestVertexDeclaration* vertexDeclaration) 
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::SetVertexDeclaration;
    cmd.setVertexDeclaration.vertexDeclaration = vertexDeclaration;
    EnqueueRenderCommand(cmd);

    device->vertexDeclaration = g_memory.MapVirtual(vertexDeclaration);
}

static void ProcSetVertexDeclaration(const RenderCommand& cmd)
{
    auto& args = cmd.setVertexDeclaration;

    if (args.vertexDeclaration != nullptr)
    {
        SetDirtyValue(g_dirtyStates.sharedConstants, g_sharedConstants.swappedTexcoords, args.vertexDeclaration->swappedTexcoords);

        uint32_t specConstants = g_pipelineState.specConstants;
        if (args.vertexDeclaration->hasR11G11B10Normal)
            specConstants |= SPEC_CONSTANT_R11G11B10_NORMAL;
        else
            specConstants &= ~SPEC_CONSTANT_R11G11B10_NORMAL;

        SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.specConstants, specConstants);
    }
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexDeclaration, args.vertexDeclaration);
}

static ShaderCacheEntry* FindShaderCacheEntry(XXH64_hash_t hash)
{
    auto end = g_shaderCacheEntries + g_shaderCacheEntryCount;
    auto findResult = std::lower_bound(g_shaderCacheEntries, end, hash, [](ShaderCacheEntry& lhs, XXH64_hash_t rhs)
        {
            return lhs.hash < rhs;
        });

    return findResult != end && findResult->hash == hash ? findResult : nullptr;
}

static GuestShader* CreateShader(const be<uint32_t>* function, ResourceType resourceType)
{
    XXH64_hash_t hash = XXH3_64bits(function, function[1] + function[2]);

    auto findResult = FindShaderCacheEntry(hash);
    GuestShader* shader = nullptr;

    if (findResult != nullptr)
    {
        if (findResult->guestShader == nullptr)
        {
            shader = g_userHeap.AllocPhysical<GuestShader>(resourceType);

            if (hash == 0x85ED723035ECF535)
            {
                shader->shader = CREATE_SHADER(blend_color_alpha_ps);
                MARK_CONSTANTS_THROUGH_UBO(shader, blend_color_alpha_ps);
            }
            else if (hash == 0xB1086A4947A797DE)
            {
                shader->shader = CREATE_SHADER(csd_no_tex_vs);
                MARK_CONSTANTS_THROUGH_UBO(shader, csd_no_tex_vs);
            }
            else if (hash == 0xB4CAFC034A37C8A8)
            {
                shader->shader = CREATE_SHADER(csd_vs);
                MARK_CONSTANTS_THROUGH_UBO(shader, csd_vs);
            }
            else
            {
                shader->shaderCacheEntry = findResult;
            }

            findResult->guestShader = shader;
        }
        else
        {
            shader = findResult->guestShader;
        }
    }

    if (shader == nullptr)
        shader = g_userHeap.AllocPhysical<GuestShader>(resourceType);
    else
        shader->AddRef();

    if (hash == 0x31173204A896098A)
        g_csdShader = shader;

    return shader;
}

static GuestShader* CreateVertexShader(const be<uint32_t>* function) 
{
    return CreateShader(function, ResourceType::VertexShader);
}

static void SetVertexShader(GuestDevice* device, GuestShader* shader)
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::SetVertexShader;
    cmd.setVertexShader.shader = shader;
    EnqueueRenderCommand(cmd);
}

static void ProcSetVertexShader(const RenderCommand& cmd)
{
    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexShader, cmd.setVertexShader.shader);
}

static void SetStreamSource(GuestDevice* device, uint32_t index, GuestBuffer* buffer, uint32_t offset, uint32_t stride) 
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::SetStreamSource;
    cmd.setStreamSource.index = index;
    cmd.setStreamSource.buffer = buffer;
    cmd.setStreamSource.offset = offset;
    cmd.setStreamSource.stride = stride;
    EnqueueRenderCommand(cmd);
}

static void ProcSetStreamSource(const RenderCommand& cmd)
{
    const auto& args = cmd.setStreamSource;

    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.vertexStrides[args.index], uint8_t(args.buffer != nullptr ? args.stride : 0));

    bool dirty = false;

#if defined(__SWITCH__)
    g_vertexStreamBuffers[args.index] = args.buffer;
    g_vertexStreamOffsets[args.index] = args.offset;
    SetDirtyValue(dirty, g_vertexBufferViews[args.index].buffer, args.buffer != nullptr ? GetBufferReference(args.buffer, args.offset) : RenderBufferReference{});
#else
    SetDirtyValue(dirty, g_vertexBufferViews[args.index].buffer, args.buffer != nullptr ? args.buffer->buffer->at(args.offset) : RenderBufferReference{});
#endif
    SetDirtyValue(dirty, g_vertexBufferViews[args.index].size, args.buffer != nullptr ? (args.buffer->dataSize - args.offset) : 0u);
    SetDirtyValue(dirty, g_inputSlots[args.index].stride, args.buffer != nullptr ? args.stride : 0u);

    if (dirty)
    {
        g_dirtyStates.vertexStreamFirst = std::min<uint8_t>(g_dirtyStates.vertexStreamFirst, args.index);
        g_dirtyStates.vertexStreamLast = std::max<uint8_t>(g_dirtyStates.vertexStreamLast, args.index);
    }
}

static void SetIndices(GuestDevice* device, GuestBuffer* buffer) 
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::SetIndices;
    cmd.setIndices.buffer = buffer;
    EnqueueRenderCommand(cmd);
}

static void ProcSetIndices(const RenderCommand& cmd)
{
    const auto& args = cmd.setIndices;

#if defined(__SWITCH__)
    g_indexStreamBuffer = args.buffer;
    SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.buffer, args.buffer != nullptr ? GetBufferReference(args.buffer, 0) : RenderBufferReference{});
#else
    SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.buffer, args.buffer != nullptr ? args.buffer->buffer->at(0) : RenderBufferReference{});
#endif
    SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.format, args.buffer != nullptr ? args.buffer->format : RenderFormat::R16_UINT);
    SetDirtyValue(g_dirtyStates.indices, g_indexBufferView.size, args.buffer != nullptr ? args.buffer->dataSize : 0u);
}

static GuestShader* CreatePixelShader(const be<uint32_t>* function)
{
    return CreateShader(function, ResourceType::PixelShader);
}

static void SetPixelShader(GuestDevice* device, GuestShader* shader)
{
    RenderCommand cmd;
    cmd.type = RenderCommandType::SetPixelShader;
    cmd.setPixelShader.shader = shader;
    EnqueueRenderCommand(cmd);
}

static void ProcSetPixelShader(const RenderCommand& cmd)
{
    GuestShader* shader = cmd.setPixelShader.shader;
    if (shader != nullptr && 
        shader->shaderCacheEntry != nullptr)
    {
        if (shader->shaderCacheEntry->hash == 0x4294510C775F4EE8)
        {
            size_t shaderIndex = GAUSSIAN_BLUR_3X3;

            switch (Config::DepthOfFieldQuality)
            {
            case EDepthOfFieldQuality::Low:
                shaderIndex = GAUSSIAN_BLUR_3X3;
                break;

            case EDepthOfFieldQuality::Medium:
                shaderIndex = GAUSSIAN_BLUR_5X5;
                break;

            case EDepthOfFieldQuality::High:
                shaderIndex = GAUSSIAN_BLUR_7X7;
                break;

            case EDepthOfFieldQuality::Ultra:
                shaderIndex = GAUSSIAN_BLUR_9X9;
                break;

            default:
            {
                if (g_aspectRatio >= WIDE_ASPECT_RATIO)
                {
                    size_t height = round(Video::s_viewportHeight * Config::ResolutionScale);

                    if (height > 1440)
                        shaderIndex = GAUSSIAN_BLUR_9X9;
                    else if (height > 1080)
                        shaderIndex = GAUSSIAN_BLUR_7X7;
                    else if (height > 720)
                        shaderIndex = GAUSSIAN_BLUR_5X5;
                    else
                        shaderIndex = GAUSSIAN_BLUR_3X3;
                }
                else
                {
                    // Narrow aspect ratios should check for width to account for VERT+.
                    size_t width = round(Video::s_viewportWidth * Config::ResolutionScale);

                    if (width > 2560)
                        shaderIndex = GAUSSIAN_BLUR_9X9;
                    else if (width > 1920)
                        shaderIndex = GAUSSIAN_BLUR_7X7;
                    else if (width > 1280)
                        shaderIndex = GAUSSIAN_BLUR_5X5;
                    else
                        shaderIndex = GAUSSIAN_BLUR_3X3;
                }

                break;
            }
            }

            shader = g_gaussianBlurShaders[shaderIndex].get();
        }
        else if (shader->shaderCacheEntry->hash == 0x6B9732B4CD7E7740 && Config::MotionBlur == EMotionBlur::Enhanced)
        {
            shader = g_enhancedMotionBlurShader.get();
        }
    }

    SetDirtyValue(g_dirtyStates.pipelineState, g_pipelineState.pixelShader, shader);
}

static void ProcessRenderCommand(const RenderCommand& cmd);

static std::thread g_renderThread([]
    {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        GuestThread::SetThreadName(GetCurrentThreadId(), "Render Thread");
#elif defined(__SWITCH__)
        // libnx starts every pthread at 0x3B, the only time-sliced band, where this thread
        // shares 10 ms slices with the guest's worker threads. Put it just below the main
        // (present) thread at 0x2C, like the render thread of nfsmw-nx (docs/platform-notes.md).
        // It blocks on its queue when idle, so it cannot hog a core.
        svcSetThreadPriority(threadGetCurHandle(), 0x2D);
        os::switch_cpu_profiler::RegisterCurrentThread("render");
#endif

#if defined(__SWITCH__)
        bool coreApplied = false;

        // Commands are processed from the look-ahead window (RenderLookahead), which a draw may fill further
        // (SwitchSkipDeadCopies) before the render loop gets to them; in the same order either way.
        auto& lookahead = g_lookahead;
        while (true)
        {
            if (lookahead.head == lookahead.tail)
            {
                lookahead.head = 0;
                // SwitchIdleRenderThreadBatches: the D3D thread holds its batches longer while this thread waits.
                g_renderThreadWaiting.store(true, std::memory_order_relaxed);
                lookahead.tail = g_renderQueue.wait_dequeue_bulk(lookahead.window, 64);
                g_renderThreadWaiting.store(false, std::memory_order_relaxed);
                g_renderCommandsTaken.store(g_renderCommandsTaken.load(std::memory_order_relaxed) + lookahead.tail, std::memory_order_relaxed);

                // The configuration is loaded after this thread starts (static initialisation).
                if (!coreApplied && g_hostThreadCoresReady.load(std::memory_order_acquire))
                {
                    coreApplied = true;
                    SetHostThreadCore(1);

                    // Round 9, SwitchRenderThreadPriority: 0x2D (below the game's threads at 0x2C) unless set;
                    // 0x2B puts it above them, so a guest worker on its core no longer delays it.
                    const int32_t priority = Config::SwitchRenderThreadPriority;
                    if (priority != 0x2D && priority >= 0x1C && priority <= 0x3B)
                        svcSetThreadPriority(threadGetCurHandle(), uint32_t(priority));
                }
            }

            // A copy: a look-ahead may move the window.
            const RenderCommand command = lookahead.window[lookahead.head++];
            if (command.type == RenderCommandType::ExecuteCommandBatch)
            {
                const auto& args = command.executeCommandBatch;
                g_renderCommandsTaken.store(g_renderCommandsTaken.load(std::memory_order_relaxed) + args.count, std::memory_order_relaxed);
                lookahead.batch = args.batch->commands;
                lookahead.batchCount = args.count;
                for (uint32_t k = 0; k < args.count; k++)
                {
                    lookahead.batchNext = k + 1;
                    ProcessRenderCommand(args.batch->commands[k]);
                }
                lookahead.batch = nullptr;
                lookahead.batchNext = 0;
                lookahead.batchCount = 0;
                g_renderCommandBatchPool.GiveBack(args.batch);
                continue;
            }

            ProcessRenderCommand(command);
        }
#else
        RenderCommand commands[64];

        while (true)
        {
            size_t count = g_renderQueue.wait_dequeue_bulk(commands, std::size(commands));

            for (size_t i = 0; i < count; i++)
                ProcessRenderCommand(commands[i]);
        }
#endif
    });

static void ProcessRenderCommand(const RenderCommand& cmd)
{
            {
#if defined(__SWITCH__)
                if (g_deferredClear.pending && !KeepsDeferredClear(cmd))
                    IssueDeferredClear();
                if (g_deferredDepthClear.pending && !KeepsDeferredDepthClear(cmd))
                    IssueDeferredDepthClear();
#endif
                switch (cmd.type)
                {
                case RenderCommandType::SetRenderState:                    ProcSetRenderState(cmd); break;
                case RenderCommandType::DestructResource:                  ProcDestructResource(cmd); break;
                case RenderCommandType::UnlockTextureRect:                 ProcUnlockTextureRect(cmd); break;
                case RenderCommandType::UnlockBuffer16:                    ProcUnlockBuffer16(cmd); break;
                case RenderCommandType::UnlockBuffer32:                    ProcUnlockBuffer32(cmd); break;
                case RenderCommandType::DrawImGui:                         ProcDrawImGui(cmd); break;
                case RenderCommandType::ExecuteCommandList:                ProcExecuteCommandList(cmd); break;
                case RenderCommandType::BeginCommandList:                  ProcBeginCommandList(cmd); break;
                case RenderCommandType::StretchRect:                       ProcStretchRect(cmd); break;
                case RenderCommandType::SetRenderTarget:                   ProcSetRenderTarget(cmd); break;
                case RenderCommandType::SetDepthStencilSurface:            ProcSetDepthStencilSurface(cmd); break;
                case RenderCommandType::ExecutePendingStretchRectCommands: ProcExecutePendingStretchRectCommands(cmd); break;
                case RenderCommandType::Clear:                             ProcClear(cmd); break;
                case RenderCommandType::SetViewport:                       ProcSetViewport(cmd); break;
                case RenderCommandType::SetTexture:                        ProcSetTexture(cmd); break;
                case RenderCommandType::SetScissorRect:                    ProcSetScissorRect(cmd); break;
                case RenderCommandType::SetSamplerState:                   ProcSetSamplerState(cmd); break;
                case RenderCommandType::SetBooleans:                       ProcSetBooleans(cmd); break;
                case RenderCommandType::SetVertexShaderConstants:          ProcSetVertexShaderConstants(cmd); break;
                case RenderCommandType::SetPixelShaderConstants:           ProcSetPixelShaderConstants(cmd); break;
                case RenderCommandType::AddPipeline:                       ProcAddPipeline(cmd); break;
                case RenderCommandType::DrawPrimitive:                     ProcDrawPrimitive(cmd); break;
                case RenderCommandType::DrawIndexedPrimitive:              ProcDrawIndexedPrimitive(cmd); break;
                case RenderCommandType::DrawPrimitiveUP:                   ProcDrawPrimitiveUP(cmd); break;
                case RenderCommandType::SetVertexDeclaration:              ProcSetVertexDeclaration(cmd); break;
                case RenderCommandType::SetVertexShader:                   ProcSetVertexShader(cmd); break;
                case RenderCommandType::SetStreamSource:                   ProcSetStreamSource(cmd); break;
                case RenderCommandType::SetIndices:                        ProcSetIndices(cmd); break;
                case RenderCommandType::SetPixelShader:                    ProcSetPixelShader(cmd); break;
                default:                                                   assert(false && "Unrecognized render command type."); break;
                }
            }
}

static void D3DXFillTexture(GuestTexture* texture, uint32_t function, void* data)
{
    if (texture->width == 1 && texture->height == 1 && texture->format == RenderFormat::R8_UNORM && function == 0x82BA2150)
    {
        auto uploadBuffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(PLACEMENT_ALIGNMENT));

        uint8_t* mappedData = reinterpret_cast<uint8_t*>(uploadBuffer->map());
        *mappedData = 0xFF;
        uploadBuffer->unmap();

        ExecuteCopyCommandList([&]
            {
                g_copyCommandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture->texture, RenderTextureLayout::COPY_DEST));

                g_copyCommandList->copyTextureRegion(
                    RenderTextureCopyLocation::Subresource(texture->texture, 0),
                    RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), texture->format, 1, 1, 1, PLACEMENT_ALIGNMENT, 0));
            });

        texture->layout = RenderTextureLayout::COPY_DEST;
    }
}

static void D3DXFillVolumeTexture(GuestTexture* texture, uint32_t function, void* data)
{
    uint32_t rowPitch0 = (texture->width * 4 + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
    uint32_t slicePitch0 = (rowPitch0 * texture->height * texture->depth + PLACEMENT_ALIGNMENT - 1) & ~(PLACEMENT_ALIGNMENT - 1);

    uint32_t rowPitch1 = ((texture->width / 2) * 4 + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
    uint32_t slicePitch1 = (rowPitch1 * (texture->height / 2) * (texture->depth / 2) + PLACEMENT_ALIGNMENT - 1) & ~(PLACEMENT_ALIGNMENT - 1);

    auto uploadBuffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(slicePitch0 + slicePitch1));
    uint8_t* mappedData = reinterpret_cast<uint8_t*>(uploadBuffer->map());

    thread_local std::vector<float> mipData;
    mipData.resize((texture->width / 2) * (texture->height / 2) * (texture->depth / 2) * 4);
    memset(mipData.data(), 0, mipData.size() * sizeof(float));

    for (size_t z = 0; z < texture->depth; z++)
    {
        for (size_t y = 0; y < texture->height; y++)
        {
            for (size_t x = 0; x < texture->width; x++)
            {
                auto dest = mappedData + z * rowPitch0 * texture->height + y * rowPitch0 + x * sizeof(uint32_t);
                size_t index = z * texture->width * texture->height + y * texture->width + x;
                size_t mipIndex = ((z / 2) * (texture->width / 2) * (texture->height / 2) + (y / 2) * (texture->width / 2) + x / 2) * 4;

                if (function == 0x82BC7820)
                {
                    auto src = reinterpret_cast<be<float>*>(data) + index * 4;

                    float r = static_cast<uint8_t>(src[0] * 255.0f);
                    float g = static_cast<uint8_t>(src[1] * 255.0f);
                    float b = static_cast<uint8_t>(src[2] * 255.0f);
                    float a = static_cast<uint8_t>(src[3] * 255.0f);

                    dest[0] = r;
                    dest[1] = g;
                    dest[2] = b;
                    dest[3] = a;

                    mipData[mipIndex + 0] += r;
                    mipData[mipIndex + 1] += g;
                    mipData[mipIndex + 2] += b;
                    mipData[mipIndex + 3] += a;
                }
                else if (function == 0x82BC78A8)
                {
                    auto src = reinterpret_cast<uint8_t*>(data) + index * 4;

                    dest[0] = src[3];
                    dest[1] = src[2];
                    dest[2] = src[1];
                    dest[3] = src[0];

                    mipData[mipIndex + 0] += src[3];
                    mipData[mipIndex + 1] += src[2];
                    mipData[mipIndex + 2] += src[1];
                    mipData[mipIndex + 3] += src[0];
                }
            }
        }
    }

    for (size_t z = 0; z < texture->depth / 2; z++)
    {
        for (size_t y = 0; y < texture->height / 2; y++)
        {
            for (size_t x = 0; x < texture->width / 2; x++)
            {
                auto dest = mappedData + slicePitch0 + z * rowPitch1 * (texture->height / 2) + y * rowPitch1 + x * sizeof(uint32_t);
                size_t index = (z * (texture->width / 2) * (texture->height / 2) + y * (texture->width / 2) + x) * 4;

                dest[0] = static_cast<uint8_t>(mipData[index + 0] / 8.0f);
                dest[1] = static_cast<uint8_t>(mipData[index + 1] / 8.0f);
                dest[2] = static_cast<uint8_t>(mipData[index + 2] / 8.0f);
                dest[3] = static_cast<uint8_t>(mipData[index + 3] / 8.0f);
            }
        }
    }

    uploadBuffer->unmap();

    ExecuteCopyCommandList([&]
        {
            g_copyCommandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture->texture, RenderTextureLayout::COPY_DEST));

            g_copyCommandList->copyTextureRegion(
                RenderTextureCopyLocation::Subresource(texture->texture, 0),
                RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), texture->format, texture->width, texture->height, texture->depth, rowPitch0 / RenderFormatSize(texture->format), 0));

            g_copyCommandList->copyTextureRegion(
                RenderTextureCopyLocation::Subresource(texture->texture, 1),
                RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), texture->format, texture->width / 2, texture->height / 2, texture->depth / 2, rowPitch1 / RenderFormatSize(texture->format), slicePitch0));
        });

    texture->layout = RenderTextureLayout::COPY_DEST;
}

struct GuestPictureData
{
    be<uint32_t> vtable;
    uint8_t flags;
    be<uint32_t> name;
    be<uint32_t> texture;
    be<uint32_t> type;
};

static RenderTextureDimension ConvertTextureDimension(ddspp::TextureType type)
{
    switch (type) 
    {
    case ddspp::Texture1D:
        return RenderTextureDimension::TEXTURE_1D;
    case ddspp::Texture2D:
    case ddspp::Cubemap:
        return RenderTextureDimension::TEXTURE_2D;
    case ddspp::Texture3D:
        return RenderTextureDimension::TEXTURE_3D;
    default:
        assert(false && "Unknown texture type from DDS.");
        return RenderTextureDimension::UNKNOWN;
    }
}

static RenderTextureViewDimension ConvertTextureViewDimension(ddspp::TextureType type)
{
    switch (type)
    {
    case ddspp::Texture1D:
        return RenderTextureViewDimension::TEXTURE_1D;
    case ddspp::Texture2D:
        return RenderTextureViewDimension::TEXTURE_2D;
    case ddspp::Texture3D:
        return RenderTextureViewDimension::TEXTURE_3D;
    case ddspp::Cubemap:
        return RenderTextureViewDimension::TEXTURE_CUBE;
    default:
        assert(false && "Unknown texture type from DDS.");
        return RenderTextureViewDimension::UNKNOWN;
    }
}

static RenderFormat ConvertDXGIFormat(ddspp::DXGIFormat format) 
{
    switch (format)
    {
    case ddspp::R32G32B32A32_TYPELESS:
        return RenderFormat::R32G32B32A32_TYPELESS;
    case ddspp::R32G32B32A32_FLOAT:
        return RenderFormat::R32G32B32A32_FLOAT;
    case ddspp::R32G32B32A32_UINT:
        return RenderFormat::R32G32B32A32_UINT;
    case ddspp::R32G32B32A32_SINT:
        return RenderFormat::R32G32B32A32_SINT;
    case ddspp::R32G32B32_TYPELESS:
        return RenderFormat::R32G32B32_TYPELESS;
    case ddspp::R32G32B32_FLOAT:
        return RenderFormat::R32G32B32_FLOAT;
    case ddspp::R32G32B32_UINT:
        return RenderFormat::R32G32B32_UINT;
    case ddspp::R32G32B32_SINT:
        return RenderFormat::R32G32B32_SINT;
    case ddspp::R16G16B16A16_TYPELESS:
        return RenderFormat::R16G16B16A16_TYPELESS;
    case ddspp::R16G16B16A16_FLOAT:
        return RenderFormat::R16G16B16A16_FLOAT;
    case ddspp::R16G16B16A16_UNORM:
        return RenderFormat::R16G16B16A16_UNORM;
    case ddspp::R16G16B16A16_UINT:
        return RenderFormat::R16G16B16A16_UINT;
    case ddspp::R16G16B16A16_SNORM:
        return RenderFormat::R16G16B16A16_SNORM;
    case ddspp::R16G16B16A16_SINT:
        return RenderFormat::R16G16B16A16_SINT;
    case ddspp::R32G32_TYPELESS:
        return RenderFormat::R32G32_TYPELESS;
    case ddspp::R32G32_FLOAT:
        return RenderFormat::R32G32_FLOAT;
    case ddspp::R32G32_UINT:
        return RenderFormat::R32G32_UINT;
    case ddspp::R32G32_SINT:
        return RenderFormat::R32G32_SINT;
    case ddspp::R8G8B8A8_TYPELESS:
        return RenderFormat::R8G8B8A8_TYPELESS;
    case ddspp::R8G8B8A8_UNORM:
        return RenderFormat::R8G8B8A8_UNORM;
    case ddspp::R8G8B8A8_UINT:
        return RenderFormat::R8G8B8A8_UINT;
    case ddspp::R8G8B8A8_SNORM:
        return RenderFormat::R8G8B8A8_SNORM;
    case ddspp::R8G8B8A8_SINT:
        return RenderFormat::R8G8B8A8_SINT;
    case ddspp::B8G8R8A8_UNORM:
        return RenderFormat::B8G8R8A8_UNORM;
    case ddspp::B8G8R8X8_UNORM:
        return RenderFormat::B8G8R8A8_UNORM;   
    case ddspp::R16G16_TYPELESS:
        return RenderFormat::R16G16_TYPELESS;
    case ddspp::R16G16_FLOAT:
        return RenderFormat::R16G16_FLOAT;
    case ddspp::R16G16_UNORM:
        return RenderFormat::R16G16_UNORM;
    case ddspp::R16G16_UINT:
        return RenderFormat::R16G16_UINT;
    case ddspp::R16G16_SNORM:
        return RenderFormat::R16G16_SNORM;
    case ddspp::R16G16_SINT:
        return RenderFormat::R16G16_SINT;
    case ddspp::R32_TYPELESS:
        return RenderFormat::R32_TYPELESS;
    case ddspp::D32_FLOAT:
        return RenderFormat::D32_FLOAT;
    case ddspp::R32_FLOAT:
        return RenderFormat::R32_FLOAT;
    case ddspp::R32_UINT:
        return RenderFormat::R32_UINT;
    case ddspp::R32_SINT:
        return RenderFormat::R32_SINT;
    case ddspp::R8G8_TYPELESS:
        return RenderFormat::R8G8_TYPELESS;
    case ddspp::R8G8_UNORM:
        return RenderFormat::R8G8_UNORM;
    case ddspp::R8G8_UINT:
        return RenderFormat::R8G8_UINT;
    case ddspp::R8G8_SNORM:
        return RenderFormat::R8G8_SNORM;
    case ddspp::R8G8_SINT:
        return RenderFormat::R8G8_SINT;
    case ddspp::R16_TYPELESS:
        return RenderFormat::R16_TYPELESS;
    case ddspp::R16_FLOAT:
        return RenderFormat::R16_FLOAT;
    case ddspp::D16_UNORM:
        return RenderFormat::D16_UNORM;
    case ddspp::R16_UNORM:
        return RenderFormat::R16_UNORM;
    case ddspp::R16_UINT:
        return RenderFormat::R16_UINT;
    case ddspp::R16_SNORM:
        return RenderFormat::R16_SNORM;
    case ddspp::R16_SINT:
        return RenderFormat::R16_SINT;
    case ddspp::R8_TYPELESS:
        return RenderFormat::R8_TYPELESS;
    case ddspp::R8_UNORM:
        return RenderFormat::R8_UNORM;
    case ddspp::R8_UINT:
        return RenderFormat::R8_UINT;
    case ddspp::R8_SNORM:
        return RenderFormat::R8_SNORM;
    case ddspp::R8_SINT:
        return RenderFormat::R8_SINT;
    case ddspp::BC1_TYPELESS:
        return RenderFormat::BC1_TYPELESS;
    case ddspp::BC1_UNORM:
        return RenderFormat::BC1_UNORM;
    case ddspp::BC1_UNORM_SRGB:
        return RenderFormat::BC1_UNORM_SRGB;
    case ddspp::BC2_TYPELESS:
        return RenderFormat::BC2_TYPELESS;
    case ddspp::BC2_UNORM:
        return RenderFormat::BC2_UNORM;
    case ddspp::BC2_UNORM_SRGB:
        return RenderFormat::BC2_UNORM_SRGB;
    case ddspp::BC3_TYPELESS:
        return RenderFormat::BC3_TYPELESS;
    case ddspp::BC3_UNORM:
        return RenderFormat::BC3_UNORM;
    case ddspp::BC3_UNORM_SRGB:
        return RenderFormat::BC3_UNORM_SRGB;
    case ddspp::BC4_TYPELESS:
        return RenderFormat::BC4_TYPELESS;
    case ddspp::BC4_UNORM:
        return RenderFormat::BC4_UNORM;
    case ddspp::BC4_SNORM:
        return RenderFormat::BC4_SNORM;
    case ddspp::BC5_TYPELESS:
        return RenderFormat::BC5_TYPELESS;
    case ddspp::BC5_UNORM:
        return RenderFormat::BC5_UNORM;
    case ddspp::BC5_SNORM:
        return RenderFormat::BC5_SNORM;
    case ddspp::BC6H_TYPELESS:
        return RenderFormat::BC6H_TYPELESS;
    case ddspp::BC6H_UF16:
        return RenderFormat::BC6H_UF16;
    case ddspp::BC6H_SF16:
        return RenderFormat::BC6H_SF16;
    case ddspp::BC7_TYPELESS:
        return RenderFormat::BC7_TYPELESS;
    case ddspp::BC7_UNORM:
        return RenderFormat::BC7_UNORM;
    case ddspp::BC7_UNORM_SRGB:
        return RenderFormat::BC7_UNORM_SRGB;
    default:
        assert(false && "Unsupported format from DDS.");
        return RenderFormat::UNKNOWN;
    }
}

static bool LoadTexture(GuestTexture& texture, const uint8_t* data, size_t dataSize, RenderComponentMapping componentMapping, bool forceCubeMap = false)
{
    ddspp::Descriptor ddsDesc;
    if (ddspp::decode_header((unsigned char *)(data), ddsDesc) != ddspp::Error)
    {
        forceCubeMap &= (ddsDesc.type == ddspp::Texture2D) && (ddsDesc.arraySize == 1);
        uint32_t arraySize = ddsDesc.type == ddspp::TextureType::Cubemap ? (ddsDesc.arraySize * 6) : ddsDesc.arraySize;
            
        RenderTextureDesc desc;
        desc.dimension = ConvertTextureDimension(ddsDesc.type);
        desc.width = ddsDesc.width;
        desc.height = ddsDesc.height;
        desc.depth = ddsDesc.depth;
        desc.mipLevels = ddsDesc.numMips;
        desc.arraySize = arraySize;
        desc.format = ConvertDXGIFormat(ddsDesc.format);
        desc.flags = ddsDesc.type == ddspp::TextureType::Cubemap ? RenderTextureFlag::CUBE : RenderTextureFlag::NONE;

        if (forceCubeMap)
        {
            desc.arraySize = 6;
            desc.flags = RenderTextureFlag::CUBE;
        }

        texture.textureHolder = g_device->createTexture(desc);
        texture.texture = texture.textureHolder.get();
        texture.layout = RenderTextureLayout::COPY_DEST;

        RenderTextureViewDesc viewDesc;
        viewDesc.format = desc.format;
        viewDesc.dimension = ConvertTextureViewDimension(ddsDesc.type);
        viewDesc.mipLevels = ddsDesc.numMips;
        viewDesc.componentMapping = componentMapping;

        if (forceCubeMap)
            viewDesc.dimension = RenderTextureViewDimension::TEXTURE_CUBE;

        texture.textureView = texture.texture->createTextureView(viewDesc);
        texture.descriptorIndex = g_textureDescriptorAllocator.allocate();
        SetTextureDescriptor(g_textureDescriptorSet, texture.descriptorIndex, texture.texture, ddsDesc.width, ddsDesc.height, RenderTextureLayout::SHADER_READ, texture.textureView.get(),
            (viewDesc.dimension == RenderTextureViewDimension::TEXTURE_2D && desc.arraySize == 1) ? ddsDesc.numMips : 0);

        texture.width = ddsDesc.width;
        texture.height = ddsDesc.height;
        texture.viewDimension = viewDesc.dimension;

        struct Slice
        {
            uint32_t width;
            uint32_t height;
            uint32_t depth;
            uint32_t srcOffset;
            uint32_t dstOffset;
            uint32_t srcRowPitch;
            uint32_t dstRowPitch;
            uint32_t rowCount;
        };

        std::vector<Slice> slices;
        uint32_t curSrcOffset = 0;
        uint32_t curDstOffset = 0;

        for (uint32_t arraySlice = 0; arraySlice < arraySize; arraySlice++)
        {
            for (uint32_t mipSlice = 0; mipSlice < ddsDesc.numMips; mipSlice++)
            {
                auto& slice = slices.emplace_back();

                slice.width = std::max(1u, ddsDesc.width >> mipSlice);
                slice.height = std::max(1u, ddsDesc.height >> mipSlice);
                slice.depth = std::max(1u, ddsDesc.depth >> mipSlice);
                slice.srcOffset = curSrcOffset;
                slice.dstOffset = curDstOffset;
                uint32_t rowPitch = ((slice.width + ddsDesc.blockWidth - 1) / ddsDesc.blockWidth) * ddsDesc.bitsPerPixelOrBlock;
                slice.srcRowPitch = (rowPitch + 7) / 8;
                slice.dstRowPitch = (slice.srcRowPitch + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
                slice.rowCount = (slice.height + ddsDesc.blockHeight - 1) / ddsDesc.blockHeight;

                curSrcOffset += slice.srcRowPitch * slice.rowCount * slice.depth;
                curDstOffset += (slice.dstRowPitch * slice.rowCount * slice.depth + PLACEMENT_ALIGNMENT - 1) & ~(PLACEMENT_ALIGNMENT - 1);
            }
        }

        auto uploadBuffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(curDstOffset));
        uint8_t* mappedMemory = reinterpret_cast<uint8_t*>(uploadBuffer->map());

        for (auto& slice : slices)
        {
            const uint8_t* srcData = data + ddsDesc.headerSize + slice.srcOffset;
            uint8_t* dstData = mappedMemory + slice.dstOffset;

            if (slice.srcRowPitch == slice.dstRowPitch)
            {
                memcpy(dstData, srcData, slice.srcRowPitch * slice.rowCount * slice.depth);
            }
            else
            {
                for (size_t i = 0; i < slice.rowCount * slice.depth; i++)
                {
                    memcpy(dstData, srcData, slice.srcRowPitch);
                    srcData += slice.srcRowPitch;
                    dstData += slice.dstRowPitch;
                }
            }
        }

        uploadBuffer->unmap();

        ExecuteCopyCommandList([&]
            {
                g_copyCommandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture.texture, RenderTextureLayout::COPY_DEST));

                auto copyTextureRegion = [&](Slice& slice, uint32_t subresourceIndex)
                    {
                        g_copyCommandList->copyTextureRegion(
                            RenderTextureCopyLocation::Subresource(texture.texture, subresourceIndex % ddsDesc.numMips, subresourceIndex / ddsDesc.numMips),
                            RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), desc.format, slice.width, slice.height, slice.depth, (slice.dstRowPitch * 8) / ddsDesc.bitsPerPixelOrBlock * ddsDesc.blockWidth, slice.dstOffset));
                    };

                for (size_t i = 0; i < slices.size(); i++)
                    copyTextureRegion(slices[i], i);

                // Duplicate the first face across the remaining 6 faces.
                if (forceCubeMap)
                {
                    for (size_t i = 1; i < 6; i++)
                    {
                        for (size_t j = 0; j < slices.size(); j++)
                            copyTextureRegion(slices[j], (slices.size() * i) + j);
                    }
                }
            });

        return true;
    }
    else
    {
        int width, height;
        void* stbImage = stbi_load_from_memory(data, dataSize, &width, &height, nullptr, 4);

        if (stbImage != nullptr)
        {
            texture.textureHolder = g_device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, RenderFormat::R8G8B8A8_UNORM));
            texture.texture = texture.textureHolder.get();
            texture.viewDimension = RenderTextureViewDimension::TEXTURE_2D;
            texture.layout = RenderTextureLayout::COPY_DEST;

            texture.descriptorIndex = g_textureDescriptorAllocator.allocate();
            SetTextureDescriptor(g_textureDescriptorSet, texture.descriptorIndex, texture.texture, uint32_t(width), uint32_t(height), RenderTextureLayout::SHADER_READ, nullptr, 1);

            uint32_t rowPitch = (width * 4 + PITCH_ALIGNMENT - 1) & ~(PITCH_ALIGNMENT - 1);
            uint32_t slicePitch = rowPitch * height;

            auto uploadBuffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(slicePitch));
            uint8_t* mappedMemory = reinterpret_cast<uint8_t*>(uploadBuffer->map());

            if (rowPitch == (width * 4))
            {
                memcpy(mappedMemory, stbImage, slicePitch);
            }
            else
            {
                auto data = reinterpret_cast<const uint8_t*>(stbImage);

                for (size_t i = 0; i < height; i++)
                {
                    memcpy(mappedMemory, data, width * 4);
                    data += width * 4;
                    mappedMemory += rowPitch;
                }
            }

            uploadBuffer->unmap();

            stbi_image_free(stbImage);

            ExecuteCopyCommandList([&]
                {
                    g_copyCommandList->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture.texture, RenderTextureLayout::COPY_DEST));

                    g_copyCommandList->copyTextureRegion(
                        RenderTextureCopyLocation::Subresource(texture.texture, 0),
                        RenderTextureCopyLocation::PlacedFootprint(uploadBuffer.get(), RenderFormat::R8G8B8A8_UNORM, width, height, 1, rowPitch / 4, 0));
                });

            return true;
        }
    }

    return false;
}

std::unique_ptr<GuestTexture> LoadTexture(const uint8_t* data, size_t dataSize, RenderComponentMapping componentMapping)
{
    GuestTexture texture(ResourceType::Texture);

    if (LoadTexture(texture, data, dataSize, componentMapping))
        return std::make_unique<GuestTexture>(std::move(texture));

    return nullptr;
}

static void DiffPatchTexture(GuestTexture& texture, uint8_t* data, uint32_t dataSize, XXH64_hash_t hash)
{
    auto header = reinterpret_cast<BlockCompressionDiffPatchHeader*>(g_buttonBcDiff.get());
    auto entries = reinterpret_cast<BlockCompressionDiffPatchEntry*>(g_buttonBcDiff.get() + header->entriesOffset);
    auto end = entries + header->entryCount;
    
    auto findResult = std::lower_bound(entries, end, hash, [](BlockCompressionDiffPatchEntry& lhs, XXH64_hash_t rhs)
        {
            return lhs.hash < rhs;
        });

    if (findResult != end && findResult->hash == hash)
    {
        auto patch = reinterpret_cast<BlockCompressionDiffPatch*>(g_buttonBcDiff.get() + findResult->patchesOffset);
        for (size_t i = 0; i < findResult->patchCount; i++)
        {
            assert(patch->destinationOffset + patch->patchBytesSize <= dataSize);
            memcpy(data + patch->destinationOffset, g_buttonBcDiff.get() + patch->patchBytesOffset, patch->patchBytesSize);
            ++patch;
        }

        GuestTexture patchedTexture(ResourceType::Texture);
        if (LoadTexture(patchedTexture, data, dataSize, {}))
            texture.patchedTexture = std::make_unique<GuestTexture>(std::move(patchedTexture));
    }
}

static void MakePictureData(GuestPictureData* pictureData, uint8_t* data, uint32_t dataSize)
{
    if ((pictureData->flags & 0x1) == 0 && data != nullptr)
    {
        GuestTexture texture(ResourceType::Texture);

        if (LoadTexture(texture, data, dataSize, {}))
        {
#ifdef _DEBUG
            texture.texture->setName(reinterpret_cast<char*>(g_memory.Translate(pictureData->name + 2)));
#endif
            XXH64_hash_t hash = XXH3_64bits(data, dataSize);

            // The whale in Cool Edge has a 2D texture assigned as a cubemap which makes it not display in recomp.
            // The hardware duplicates the first face to the remaining 6 faces, so to simulate that we'll recreate the asset.
            bool forceCubeMap = (dataSize == 0xAB38) && (hash == 0x160E9E250FDE88A9);
            if (forceCubeMap)
            {
                GuestTexture recreatedCubeMapTexture(ResourceType::Texture);
                if (LoadTexture(recreatedCubeMapTexture, data, dataSize, {}, true))
                    texture.recreatedCubeMapTexture = std::make_unique<GuestTexture>(std::move(recreatedCubeMapTexture));
            }

            DiffPatchTexture(texture, data, dataSize, hash);

            pictureData->texture = g_memory.MapVirtual(g_userHeap.AllocPhysical<GuestTexture>(std::move(texture)));
            pictureData->type = 0;
        }
    }
}

void IndexBufferLengthMidAsmHook(PPCRegister& r3)
{
    r3.u64 *= 2;
}

void SetShadowResolutionMidAsmHook(PPCRegister& r11)
{
    auto res = (int32_t)Config::ShadowResolution.Value;

    if (res > 0)
        r11.u64 = res;
}

static void SetResolution(be<uint32_t>* device)
{
    Video::ComputeViewportDimensions();

    uint32_t width = uint32_t(round(Video::s_viewportWidth * Config::ResolutionScale));
    uint32_t height = uint32_t(round(Video::s_viewportHeight * Config::ResolutionScale));
    device[46] = width == 0 ? 880 : width;
    device[47] = height == 0 ? 720 : height;
}

// The game does some weird stuff to render targets if they are above 
// 1024x1024 resolution, setting this bool at address 20 seems to avoid all that.
PPC_FUNC(sub_82E9F048)
{
    PPC_STORE_U8(ctx.r4.u32 + 20, 1);
    PPC_STORE_U32(ctx.r4.u32 + 44, PPC_LOAD_U32(ctx.r4.u32 + 8)); // Width
    PPC_STORE_U32(ctx.r4.u32 + 48, PPC_LOAD_U32(ctx.r4.u32 + 12)); // Height
}

static GuestShader* g_movieVertexShader;
static GuestShader* g_moviePixelShader;
static GuestVertexDeclaration* g_movieVertexDeclaration;

static void ScreenShaderInit(be<uint32_t>* a1, uint32_t a2, uint32_t a3, GuestVertexElement* vertexElements)
{
    if (g_moviePixelShader == nullptr)
    {
        g_moviePixelShader = g_userHeap.AllocPhysical<GuestShader>(ResourceType::PixelShader);
        g_moviePixelShader->shader = CREATE_SHADER(movie_ps);
    }

    if (g_movieVertexShader == nullptr)
    {
        g_movieVertexShader = g_userHeap.AllocPhysical<GuestShader>(ResourceType::VertexShader);
        g_movieVertexShader->shader = CREATE_SHADER(movie_vs);
    }

    if (g_movieVertexDeclaration == nullptr)
        g_movieVertexDeclaration = CreateVertexDeclarationWithoutAddRef(vertexElements);

    g_moviePixelShader->AddRef();
    g_movieVertexShader->AddRef();
    g_movieVertexDeclaration->AddRef();

    a1[2] = g_memory.MapVirtual(g_moviePixelShader);
    a1[3] = g_memory.MapVirtual(g_movieVertexShader);
    a1[4] = g_memory.MapVirtual(g_movieVertexDeclaration);
}

void MovieRendererMidAsmHook(PPCRegister& r3)
{
    auto device = reinterpret_cast<GuestDevice*>(g_memory.Translate(r3.u32));

    // Force linear filtering & clamp addressing
    for (size_t i = 0; i < 3; i++)
    {
        device->samplerStates[i].data[0] = (device->samplerStates[i].data[0].get() & ~0x7fc00) | 0x24800;
        device->samplerStates[i].data[3] = (device->samplerStates[i].data[3].get() & ~0x1f80000) | 0x1280000;
    }

    device->dirtyFlags[3] = device->dirtyFlags[3].get() | 0xe0000000ull;
}

static PPCRegister g_r4;
static PPCRegister g_r5;

// CRenderDirectorFxPipeline::Initialize
PPC_FUNC_IMPL(__imp__sub_8258C8A0);
PPC_FUNC(sub_8258C8A0)
{
    g_r4 = ctx.r4;
    g_r5 = ctx.r5;
    __imp__sub_8258C8A0(ctx, base);
}

// CRenderDirectorFxPipeline::Update
PPC_FUNC_IMPL(__imp__sub_8258CAE0);
PPC_FUNC(sub_8258CAE0)
{
    if (g_needsResize)
    {
        // Backup job values. These get modified by cutscenes, 
        // and resizing will cause the values to be forgotten.
        auto traverseFxJobs = [&]<typename TCallback>(const TCallback& callback)
        {
            uint32_t scheduler = PPC_LOAD_U32(ctx.r3.u32 + 0xE0);
            if (scheduler != NULL)
            {
                uint32_t member = PPC_LOAD_U32(scheduler + 0x8);
                if (member != NULL)
                {
                    for (uint32_t it = PPC_LOAD_U32(member + 0x24); it != PPC_LOAD_U32(member + 0x28); it += 8)
                    {
                        uint32_t job = PPC_LOAD_U32(it);
                        if (job != NULL)
                            callback(job);
                    }
                }
            }
        };

        union JobValues
        {
            struct
            {
                uint8_t field50[0x18];
                uint8_t field88;
            } fade;

            struct
            {
                uint8_t camera[0x120];
                uint8_t field44;
                uint8_t fieldA0;
            } shadowMap;
        };

        std::map<uint32_t, JobValues> jobValuesMap;
        traverseFxJobs([&](uint32_t job)
            {
                uint32_t vfTable = PPC_LOAD_U32(job);

                if (vfTable == 0x820CA6F8) // SWA::CFxFade
                {
                    // NOTE: Intentionally not storing shared pointers here. 
                    // Game sends messages that assign these every frame already.
                    JobValues jobValues{};

                    memcpy(jobValues.fade.field50, base + job + 0x50, sizeof(jobValues.fade.field50));
                    jobValues.fade.field88 = PPC_LOAD_U8(job + 0x88);

                    jobValuesMap.emplace(PPC_LOAD_U32(job + 0x48), jobValues);
                }
                else if (vfTable == 0x820CAC5C) // SWA::CFxShadowMap
                {
                    for (uint32_t it = PPC_LOAD_U32(job + 0x88); it != PPC_LOAD_U32(job + 0x8C); it += 8)
                    {
                        uint32_t camera = PPC_LOAD_U32(it);
                        if (camera != NULL && PPC_LOAD_U32(camera) == 0x820BF83C) // SWA::CShadowMapCameraLiSPSM
                        {
                            JobValues jobValues{};

                            memcpy(jobValues.shadowMap.camera, base + camera, sizeof(jobValues.shadowMap.camera));
                            jobValues.shadowMap.field44 = PPC_LOAD_U8(job + 0x44);
                            jobValues.shadowMap.fieldA0 = PPC_LOAD_U8(job + 0xA0);

                            jobValuesMap.emplace(vfTable, jobValues);
                            break;
                        }
                    }
                }
            });

        auto r3 = ctx.r3;
        ctx.r4 = g_r4;
        ctx.r5 = g_r5;
        __imp__sub_8258C8A0(ctx, base);
        ctx.r3 = r3;

        // Restore job values.
        traverseFxJobs([&](uint32_t job)
            {
                uint32_t vfTable = PPC_LOAD_U32(job);

                if (vfTable == 0x820CA6F8) // SWA::CFxFade
                {
                    auto findResult = jobValuesMap.find(PPC_LOAD_U32(job + 0x48));
                    if (findResult != jobValuesMap.end()) // May NOT actually be found.
                    {
                        memcpy(base + job + 0x50, findResult->second.fade.field50, sizeof(findResult->second.fade.field50));
                        PPC_STORE_U8(job + 0x88, findResult->second.fade.field88);
                    }
                }
                else if (vfTable == 0x820CAC5C) // SWA::CFxShadowMap
                {
                    auto findResult = jobValuesMap.find(vfTable);
                    if (findResult != jobValuesMap.end()) // Would be weird if this one wasn't found.
                    {
                        for (uint32_t it = PPC_LOAD_U32(job + 0x88); it != PPC_LOAD_U32(job + 0x8C); it += 8)
                        {
                            uint32_t camera = PPC_LOAD_U32(it);
                            if (camera != NULL && PPC_LOAD_U32(camera) == 0x820BF83C) // SWA::CShadowMapCameraLiSPSM
                            {
                                memcpy(base + camera, findResult->second.shadowMap.camera, sizeof(findResult->second.shadowMap.camera));
                                PPC_STORE_U32(job + 0x80, camera);
                                PPC_STORE_U8(job + 0x44, findResult->second.shadowMap.field44);
                                PPC_STORE_U8(job + 0xA0, findResult->second.shadowMap.fieldA0);
                                break;
                            }
                        }
                    }
                }
            });

        g_needsResize = false;
    }

    __imp__sub_8258CAE0(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_824EB5B0);
PPC_FUNC(sub_824EB5B0)
{
    g_updateDirectorProfiler.Begin();
    __imp__sub_824EB5B0(ctx, base);
    g_updateDirectorProfiler.End();
}

PPC_FUNC_IMPL(__imp__sub_824EB290);
PPC_FUNC(sub_824EB290)
{
    g_renderDirectorProfiler.Begin();
    __imp__sub_824EB290(ctx, base);
    g_renderDirectorProfiler.End();
}

// World map disables VERT+, so scaling by width does not work for it.
static uint32_t g_forceCheckHeightForPostProcessFix;

// SWA::CWorldMapCamera::CWorldMapCamera
PPC_FUNC_IMPL(__imp__sub_824860E0);
PPC_FUNC(sub_824860E0)
{
    ++g_forceCheckHeightForPostProcessFix;
    __imp__sub_824860E0(ctx, base);
}

// SWA::CCameraController::~CCameraController
PPC_FUNC_IMPL(__imp__sub_824831D0);
PPC_FUNC(sub_824831D0)
{
    if (PPC_LOAD_U32(ctx.r3.u32) == 0x8202BF1C) // SWA::CWorldMapCamera
        --g_forceCheckHeightForPostProcessFix;

    __imp__sub_824831D0(ctx, base);
}

void PostProcessResolutionFix(PPCRegister& r4, PPCRegister& f1, PPCRegister& f2)
{
    auto device = reinterpret_cast<be<uint32_t>*>(g_memory.Translate(r4.u32));

    uint32_t width = device[46].get();
    uint32_t height = device[47].get();
    double aspectRatio = double(width) / double(height);

    double factor;
    if ((aspectRatio >= WIDE_ASPECT_RATIO) || (g_forceCheckHeightForPostProcessFix != 0))
        factor = 720.0 / double(height);
    else
        factor = 1280.0 / double(width);

    f1.f64 *= factor;
    f2.f64 *= factor;
}

void LightShaftAspectRatioFix(PPCRegister& f28, PPCRegister& f0)
{
    f28.f64 = f0.f64;
}

static const be<uint16_t> g_particleTestIndexBuffer[] =
{
    0, 1, 2,
    0, 2, 3,
    0, 3, 4,
    0, 4, 5
};

bool ParticleTestIndexBufferMidAsmHook(PPCRegister& r30)
{
    if (!g_capabilities.triangleFan)
    {
        auto buffer = CreateIndexBuffer(sizeof(g_particleTestIndexBuffer), 0, D3DFMT_INDEX16);
        void* memory = LockIndexBuffer(buffer, 0, 0, 0);
        memcpy(memory, g_particleTestIndexBuffer, sizeof(g_particleTestIndexBuffer));
        UnlockIndexBuffer(buffer);

        r30.u32 = g_memory.MapVirtual(buffer);
        return true;
    }
    return false;
}

void ParticleTestDrawIndexedPrimitiveMidAsmHook(PPCRegister& r7)
{
    if (!g_capabilities.triangleFan)
        r7.u64 = std::size(g_particleTestIndexBuffer);
}

void MotionBlurPrevInvViewProjectionMidAsmHook(PPCRegister& r10)
{
    auto mtxProjection = reinterpret_cast<be<float>*>(g_memory.Translate(r10.u32));

    // Reverse Z. Have to be done on CPU side because the matrix multiplications
    // add up and it loses precision by the time it's sent to GPU.
    mtxProjection[10] = -(mtxProjection[10] + 1.0f);
    mtxProjection[14] = -mtxProjection[14];
}

// Normally, we could delay setting IsMadeOne, but the game relies on that flag
// being present to handle load priority. To work around that, we can prevent
// IsMadeAll from being set until the compilation is finished. Time for a custom flag!
enum
{
    eDatabaseDataFlags_CompilingPipelines = 0x80
};

// This is passed to pipeline compilation threads to keep the loading screen busy until 
// all of them are finished. A shared pointer makes sure the destructor is called only once.
struct PipelineTaskToken
{
    PipelineTaskType type{};
    boost::shared_ptr<Hedgehog::Database::CDatabaseData> databaseData;

    PipelineTaskToken() : databaseData()
    {
    }

    PipelineTaskToken(const PipelineTaskToken&) = delete;

    PipelineTaskToken(PipelineTaskToken&& other)
        : type(std::exchange(other.type, PipelineTaskType::Null))
        , databaseData(std::exchange(other.databaseData, nullptr))
    {
    }

    ~PipelineTaskToken()
    {
        if (type != PipelineTaskType::Null)
        {
            if (databaseData.get() != nullptr)
                databaseData->m_Flags &= ~eDatabaseDataFlags_CompilingPipelines;

            if ((--g_compilingPipelineTaskCount) == 0)
                g_compilingPipelineTaskCount.notify_one();
        }
    }
};

struct PipelineStateQueueItem
{
    XXH64_hash_t pipelineHash;
    PipelineState pipelineState;
    std::shared_ptr<PipelineTaskToken> token;
#ifdef ASYNC_PSO_DEBUG
    std::string pipelineName;
#endif
};

static moodycamel::BlockingConcurrentQueue<PipelineStateQueueItem> g_pipelineStateQueue;

#if defined(__SWITCH__)
// Printed with each stall watchdog dump (os/switch_stall_watch.h), on its thread: only atomics and the
// queues' approximate sizes, nothing that locks.
static void ReportRendererState(std::string& out)
{
    AppendFormat(out, "render queue ~%zu commands (%llu taken so far), pipeline queue ~%zu, pipeline tasks %u pending "
        "and %u compiling, command list %s",
        g_renderQueue.size_approx(), (unsigned long long)g_renderCommandsTaken.load(std::memory_order_relaxed),
        g_pipelineStateQueue.size_approx(), g_pendingPipelineTaskCount.load(std::memory_order_relaxed),
        g_compilingPipelineTaskCount.load(std::memory_order_relaxed),
        g_executedCommandList.load(std::memory_order_relaxed) ? "executed" : "not executed yet");
}

// A pipeline variant the render thread wants but does not wait for (built by the compiler threads).
static void EnqueueSwitchPipelineVariant(const PipelineState& pipelineState, XXH64_hash_t hash)
{
    PipelineStateQueueItem queueItem;
    queueItem.pipelineHash = hash;
    queueItem.pipelineState = pipelineState;
#ifdef ASYNC_PSO_DEBUG
    queueItem.pipelineName = fmt::format("COVERAGE {:X}", hash);
#endif
    g_pipelineStateQueue.enqueue(queueItem);
}
#endif

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
// Render thread only: specialized variants already handed to the compiler threads.
static ankerl::unordered_dense::set<XXH64_hash_t> g_specializedPipelinesRequested;

// The bits a sanitized state gets for the draw; bonesBits returns the skinning part alone.
static uint32_t SpecializationBits(const PipelineState& pipelineState, uint32_t& bonesBits)
{
    bonesBits = 0;

    // mrgHasBone is vertex boolean b0 in every shader carrying the bit (XenosRecomp checks).
    const auto* vertexEntry = pipelineState.vertexShader->shaderCacheEntry;
    if (g_bonesSpecialization && vertexEntry != nullptr && (vertexEntry->specConstantsMask & SPEC_CONSTANT_BONES_SPECIALIZED) != 0)
    {
        bonesBits = SPEC_CONSTANT_BONES_SPECIALIZED;
        if ((g_sharedConstants.booleans & 0x1) != 0)
            bonesBits |= SPEC_CONSTANT_HAS_BONES;
    }

    uint32_t gatherBits = 0;
#if defined(__SWITCH__)
    // [Switch] SwitchShadowGatherSpecialization. UpdateGatherableSlot marks the pipeline dirty whenever
    // g_GatherableSlots changes, so the choice is made again before the next draw.
    if (g_shadowGatherSpecialization && (pipelineState.specConstants & SPEC_CONSTANT_SHADOW_GATHER) != 0 &&
        pipelineState.pixelShader != nullptr && pipelineState.pixelShader->shaderCacheEntry != nullptr)
    {
        const uint32_t slots = pipelineState.pixelShader->shaderCacheEntry->gatherSlots;
        if (slots != 0 && (slots & ~g_sharedConstants.gatherableSlots) == 0)
            gatherBits = SPEC_CONSTANT_SHADOW_GATHER_KNOWN;
    }
#endif

    return bonesBits | gatherBits;
}

static RenderPipeline* FindSpecializedPipeline(const PipelineState& pipelineState, uint32_t bits, bool request)
{
    PipelineState specialized = pipelineState;
    specialized.specConstants |= bits;

    const XXH64_hash_t hash = XXH3_64bits(&specialized, sizeof(specialized));
    auto findResult = g_pipelines.find(hash);
    if (findResult != g_pipelines.end() && findResult->second != nullptr)
        return findResult->second.get();

    // Built by the compiler threads at their low priority, like the loading-time pipelines; it arrives
    // through RenderCommandType::AddPipeline and is used from then on.
    if (request && g_specializedPipelinesRequested.emplace(hash).second)
    {
        PipelineStateQueueItem queueItem;
        queueItem.pipelineHash = hash;
        queueItem.pipelineState = specialized;
#ifdef ASYNC_PSO_DEBUG
        queueItem.pipelineName = fmt::format("SPECIALIZED {:X}", hash);
#endif
        g_pipelineStateQueue.enqueue(queueItem);
    }

    return nullptr;
}

static RenderPipeline* GetSpecializedPipeline(const PipelineState& pipelineState)
{
    uint32_t bonesBits = 0;
    const uint32_t bits = SpecializationBits(pipelineState, bonesBits);
    if (bits == 0)
        return nullptr;

    if (RenderPipeline* pipeline = FindSpecializedPipeline(pipelineState, bits, true))
        return pipeline;

    // While the full variant is being built: the skinning one, if loading built it.
    if (bonesBits != 0 && bonesBits != bits)
        return FindSpecializedPipeline(pipelineState, bonesBits, false);

    return nullptr;
}
#endif

static void CompilePipeline(XXH64_hash_t pipelineHash, const PipelineState& pipelineState
#ifdef ASYNC_PSO_DEBUG
    , const std::string& pipelineName
#endif
)
{
    auto pipeline = CreateGraphicsPipeline(pipelineState);
#ifdef ASYNC_PSO_DEBUG
    pipeline->setName(pipelineName);
#endif

    // Will get dropped in render thread if a different thread already managed to compile this.
    RenderCommand cmd;
    cmd.type = RenderCommandType::AddPipeline;
    cmd.addPipeline.hash = pipelineHash;
    cmd.addPipeline.pipeline = pipeline.release();
    EnqueueRenderCommand(cmd);
}

static void PipelineCompilerThread()
{
#ifdef _WIN32
    int threadPriority = THREAD_PRIORITY_LOWEST;
    SetThreadPriority(GetCurrentThread(), threadPriority);
    GuestThread::SetThreadName(GetCurrentThreadId(), "Pipeline Compiler Thread");
#elif defined(__SWITCH__)
    // Lowest priority: only run when a core is otherwise idle, mirroring the
    // THREAD_PRIORITY_LOWEST intent above. Three shared cores cannot afford
    // shader compilation competing with game threads.
    svcSetThreadPriority(threadGetCurHandle(), 0x3B);
    os::switch_cpu_profiler::RegisterCurrentThread("pipeline compiler");
#endif

    std::unique_ptr<GuestThreadContext> ctx;
#if defined(__SWITCH__)
    bool coreApplied = false;
#endif

    while (true)
    {
        PipelineStateQueueItem queueItem;
        g_pipelineStateQueue.wait_dequeue(queueItem);

        if (ctx == nullptr)
            ctx = std::make_unique<GuestThreadContext>(0);

#if defined(__SWITCH__)
        if (!coreApplied && g_hostThreadCoresReady.load(std::memory_order_acquire))
        {
            coreApplied = true;
            SetHostThreadCore(2);
        }
#endif

#ifdef _WIN32
        int newThreadPriority = threadPriority;

        bool loading = *SWA::SGlobals::ms_IsLoading;
        if (loading)
            newThreadPriority = THREAD_PRIORITY_HIGHEST;
        else
            newThreadPriority = THREAD_PRIORITY_LOWEST;

        if (newThreadPriority != threadPriority)
        {
            SetThreadPriority(GetCurrentThread(), newThreadPriority);
            threadPriority = newThreadPriority;
        }
#endif

        CompilePipeline(queueItem.pipelineHash, queueItem.pipelineState
#ifdef ASYNC_PSO_DEBUG
            , queueItem.pipelineName.c_str()
#endif
        );

        std::this_thread::yield();
    }
}

static std::vector<std::unique_ptr<std::thread>> g_pipelineCompilerThreads = []()
    {
        size_t threadCount = std::max(2u, (std::thread::hardware_concurrency() * 2) / 3);

        std::vector<std::unique_ptr<std::thread>> threads(threadCount);
        for (auto& thread : threads)
            thread = std::make_unique<std::thread>(PipelineCompilerThread);

        return threads;
    }();

static constexpr uint32_t MODEL_DATA_VFTABLE = 0x82073A44;
static constexpr uint32_t TERRAIN_MODEL_DATA_VFTABLE = 0x8211D25C;
static constexpr uint32_t PARTICLE_MATERIAL_VFTABLE = 0x8211F198;

// Allocate the shared pointer only when new compilations are happening.
// If nothing was compiled, the local "token" variable will get destructed with RAII instead.
struct PipelineTaskTokenPair
{
    PipelineTaskToken token;
    std::shared_ptr<PipelineTaskToken> sharedToken;
};

// Having this separate, because I don't want to lock a mutex in the render thread before
// every single draw. Might be worth profiling to see if it actually has an impact and merge them.
static xxHashMap<PipelineState> g_asyncPipelineStates;

static void EnqueueGraphicsPipelineCompilation(
    const PipelineState& pipelineState, 
    PipelineTaskTokenPair& tokenPair, 
    const char* name,
    bool isPrecompiledPipeline = false)
{
    XXH64_hash_t hash = XXH3_64bits(&pipelineState, sizeof(pipelineState));
    bool shouldCompile = g_asyncPipelineStates.emplace(hash, pipelineState).second;

    if (shouldCompile)
    {
        bool loading = *SWA::SGlobals::ms_IsLoading;
        if (!loading && isPrecompiledPipeline)
        {
            // We can just compile here during the logos.
            CompilePipeline(hash, pipelineState
#ifdef ASYNC_PSO_DEBUG
                , fmt::format("CACHE {} {:X}", name, hash)
#endif
            );
        }
        else
        {
            if (tokenPair.sharedToken == nullptr && tokenPair.token.type != PipelineTaskType::Null)
                tokenPair.sharedToken = std::make_shared<PipelineTaskToken>(std::move(tokenPair.token));

            PipelineStateQueueItem queueItem;
            queueItem.pipelineHash = hash;
            queueItem.pipelineState = pipelineState;
            queueItem.token = tokenPair.sharedToken;
#ifdef ASYNC_PSO_DEBUG
            queueItem.pipelineName = fmt::format("ASYNC {} {:X}", name, hash);
#endif
            g_pipelineStateQueue.enqueue(queueItem);
        }
    }

#ifdef PSO_CACHING_CLEANUP
    if (shouldCompile && isPrecompiledPipeline)
    {
        std::lock_guard lock(g_pipelineCacheMutex);
        g_pipelineStatesToCache.emplace(hash, pipelineState);
    }
#endif

#ifdef PSO_CACHING
    if (!isPrecompiledPipeline)
    {
        std::lock_guard lock(g_pipelineCacheMutex);
        g_pipelineStatesToCache.erase(hash);
    }
#endif
}

struct CompilationArgs
{
    PipelineTaskTokenPair tokenPair;
    bool noGI{};
    bool hasMoreThanOneBone{};
    bool velocityMapQuickStep{};
    bool objectIcon{};
    bool instancing{};
};

#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
// [Switch] SwitchPrecompileBonesVariants. The render thread asks for the variant of a pipeline built for
// the draw's mrgHasBone (vertex boolean b0) and draws with the generic one until a compiler thread has
// built it. Loading screens build the variant a model will use too (b0 is set for models with more than
// one node), so it is ready from the first frame. A wrong guess only costs the unused variant.
static void EnqueueBonesVariantCompilation(const PipelineState& sanitizedState, bool hasBones, PipelineTaskTokenPair& tokenPair, const char* name)
{
    if (!g_precompileBonesVariants || sanitizedState.vertexShader == nullptr)
        return;

    const auto* shaderCacheEntry = sanitizedState.vertexShader->shaderCacheEntry;
    if (shaderCacheEntry == nullptr || (shaderCacheEntry->specConstantsMask & SPEC_CONSTANT_BONES_SPECIALIZED) == 0)
        return;

    // The same state as SpecializationBits builds from the sanitized one, so the same hash.
    PipelineState specialized = sanitizedState;
    specialized.specConstants |= SPEC_CONSTANT_BONES_SPECIALIZED;
    if (hasBones)
        specialized.specConstants |= SPEC_CONSTANT_HAS_BONES;

#if defined(__SWITCH__)
    // Its shadow slots will most likely be gatherable (shadow maps are point-sampled, one mip, power of
    // two): build the variant the render thread will ask for then. A wrong guess costs an unused pipeline.
    if (g_shadowGatherSpecialization && (specialized.specConstants & SPEC_CONSTANT_SHADOW_GATHER) != 0 &&
        specialized.pixelShader != nullptr && specialized.pixelShader->shaderCacheEntry != nullptr &&
        specialized.pixelShader->shaderCacheEntry->gatherSlots != 0)
    {
        specialized.specConstants |= SPEC_CONSTANT_SHADOW_GATHER_KNOWN;
    }
#endif

    EnqueueGraphicsPipelineCompilation(specialized, tokenPair, name);
}
#endif

enum class MeshLayer
{
    Opaque,
    Transparent,
    PunchThrough,
    Special
};

struct Mesh
{
    uint32_t vertexSize{};
    uint32_t morphTargetVertexSize{};
    GuestVertexDeclaration* vertexDeclaration{};
    Hedgehog::Mirage::CMaterialData* material{};
    MeshLayer layer{};
    bool morphModel{};
};

static void CompileMeshPipeline(const Mesh& mesh, CompilationArgs& args)
{
    if (mesh.material == nullptr || mesh.material->m_spShaderListData.get() == nullptr)
        return;

    auto& shaderList = mesh.material->m_spShaderListData;

    bool isFur = !mesh.morphModel && !args.instancing &&
        strstr(shaderList->m_TypeAndName.c_str(), "Fur") != nullptr;

    bool isSky = !mesh.morphModel && !args.instancing &&
        strstr(shaderList->m_TypeAndName.c_str(), "Sky") != nullptr;

    bool isSonicMouth = !mesh.morphModel && !args.instancing &&
        strcmp(mesh.material->m_TypeAndName.c_str() + 2, "sonic_gm_mouth_duble") == 0 &&
        strcmp(shaderList->m_TypeAndName.c_str() + 3, "SonicSkin_dspf[b]") == 0;

    bool compiledOutsideMainFramebuffer = !args.instancing && !isFur && !isSky;

    bool constTexCoord;
    if (args.instancing)
    {
        constTexCoord = false;
    }
    else
    {
        constTexCoord = true;
        if (mesh.material->m_spTexsetData.get() != nullptr)
        {
            for (size_t i = 1; i < mesh.material->m_spTexsetData->m_TextureList.size(); i++)
            {
                if (mesh.material->m_spTexsetData->m_TextureList[i]->m_TexcoordIndex !=
                    mesh.material->m_spTexsetData->m_TextureList[0]->m_TexcoordIndex)
                {
                    constTexCoord = false;
                    break;
                }
            }
        }
    }

    // Shadow pipeline.
    if (compiledOutsideMainFramebuffer && (mesh.layer == MeshLayer::Opaque || mesh.layer == MeshLayer::PunchThrough))
    {
        PipelineState pipelineState{};

        if (mesh.layer == MeshLayer::PunchThrough)
        {
            pipelineState.vertexShader = FindShaderCacheEntry(0xDD4FA7BB53876300)->guestShader;
            pipelineState.pixelShader = FindShaderCacheEntry(0xE2ECA594590DDE8B)->guestShader;
        }
        else
        {
            pipelineState.vertexShader = FindShaderCacheEntry(0x8E4BB23465BD909E)->guestShader;
        }

        pipelineState.vertexDeclaration = mesh.vertexDeclaration;
        pipelineState.cullMode = mesh.material->m_DoubleSided ? RenderCullMode::NONE : RenderCullMode::BACK;
        pipelineState.zFunc = RenderComparisonFunction::LESS_EQUAL;
        
        if (g_capabilities.dynamicDepthBias)
        {
            // Put common depth bias values for reducing unnecessary calls.
            if (!g_vulkan)
            {
                pipelineState.depthBias = COMMON_DEPTH_BIAS_VALUE;
                pipelineState.slopeScaledDepthBias = COMMON_SLOPE_SCALED_DEPTH_BIAS_VALUE;
            }
        }
        else 
        {
            pipelineState.depthBias = (1 << 24) * (*reinterpret_cast<be<float>*>(g_memory.Translate(0x83302760)));
            pipelineState.slopeScaledDepthBias = *reinterpret_cast<be<float>*>(g_memory.Translate(0x83302764));
        }

        pipelineState.colorWriteEnable = 0;
        pipelineState.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_STRIP;
        pipelineState.vertexStrides[0] = mesh.vertexSize;
        pipelineState.depthStencilFormat = RenderFormat::D32_FLOAT;

        if (mesh.layer == MeshLayer::PunchThrough)
            pipelineState.specConstants |= SPEC_CONSTANT_ALPHA_TEST;

        const char* name = (mesh.layer == MeshLayer::PunchThrough ? "MakeShadowMapTransparent" : "MakeShadowMap");
        SanitizePipelineState(pipelineState);
        EnqueueGraphicsPipelineCompilation(pipelineState, args.tokenPair, name);
#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
        if (!mesh.morphModel)
            EnqueueBonesVariantCompilation(pipelineState, args.hasMoreThanOneBone, args.tokenPair, name);
#endif

        // Morph models have 4 targets where unused targets default to the first vertex stream.
        if (mesh.morphModel)
        {
            for (size_t i = 0; i < 5; i++)
            {
                for (size_t j = 0; j < 4; j++)
                    pipelineState.vertexStrides[j + 1] = i > j ? mesh.morphTargetVertexSize : mesh.vertexSize;

                SanitizePipelineState(pipelineState);
                EnqueueGraphicsPipelineCompilation(pipelineState, args.tokenPair, name);
            }
        }
    }

    // Motion blur pipeline. We could normally do the player here only, but apparently Werehog enemies also have object blur.
    // TODO: Do punch through meshes get rendered?
    if (!mesh.morphModel && compiledOutsideMainFramebuffer && args.hasMoreThanOneBone && mesh.layer == MeshLayer::Opaque)
    {
        PipelineState pipelineState{};
        pipelineState.vertexShader = FindShaderCacheEntry(0x4620B236DC38100C)->guestShader;
        pipelineState.pixelShader = FindShaderCacheEntry(0xBBDB735BEACC8F41)->guestShader;
        pipelineState.vertexDeclaration = mesh.vertexDeclaration;
        pipelineState.cullMode = RenderCullMode::NONE;
        pipelineState.zFunc = RenderComparisonFunction::GREATER_EQUAL;
        pipelineState.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_STRIP;
        pipelineState.vertexStrides[0] = mesh.vertexSize;
        pipelineState.renderTargetFormat = RenderFormat::R8G8B8A8_UNORM;
        pipelineState.depthStencilFormat = RenderFormat::D32_FLOAT;
        pipelineState.specConstants = SPEC_CONSTANT_REVERSE_Z;

        SanitizePipelineState(pipelineState);
        EnqueueGraphicsPipelineCompilation(pipelineState, args.tokenPair, "FxVelocityMap");
#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
        EnqueueBonesVariantCompilation(pipelineState, args.hasMoreThanOneBone, args.tokenPair, "FxVelocityMap");
#endif

        if (args.velocityMapQuickStep)
        {
            pipelineState.vertexShader = FindShaderCacheEntry(0x99DC3F27E402700D)->guestShader;
            SanitizePipelineState(pipelineState);
            EnqueueGraphicsPipelineCompilation(pipelineState, args.tokenPair, "FxVelocityMapQuickStep");
        }
    }

    uint32_t defaultStr = args.instancing ? 0x820C8734 : 0x8202DDBC; // "instancing" for instancing, "default" for regular
    guest_stack_var<Hedgehog::Base::CStringSymbol> defaultSymbol(reinterpret_cast<const char*>(g_memory.Translate(defaultStr)));
    auto defaultFindResult = shaderList->m_PixelShaderPermutations.find(*defaultSymbol);
    if (defaultFindResult == shaderList->m_PixelShaderPermutations.end())
        return;

    uint32_t pixelShaderSubPermutationsToCompile = 0;
    if (constTexCoord) pixelShaderSubPermutationsToCompile |= 0x1;
    if (args.noGI) pixelShaderSubPermutationsToCompile |= 0x2;

    if ((defaultFindResult->second.m_SubPermutations.get() & (1 << pixelShaderSubPermutationsToCompile)) == 0) pixelShaderSubPermutationsToCompile &= ~0x1;
    if ((defaultFindResult->second.m_SubPermutations.get() & (1 << pixelShaderSubPermutationsToCompile)) == 0) pixelShaderSubPermutationsToCompile &= ~0x2;

    uint32_t noneStr = mesh.morphModel ? 0x820D72F0 : 0x8200D938; // "p" for morph, "none" for regular
    guest_stack_var<Hedgehog::Base::CStringSymbol> noneSymbol(reinterpret_cast<const char*>(g_memory.Translate(noneStr)));
    auto noneFindResult = defaultFindResult->second.m_VertexShaderPermutations.find(*noneSymbol);
    if (noneFindResult == defaultFindResult->second.m_VertexShaderPermutations.end())
        return;

    uint32_t vertexShaderSubPermutationsToCompile = 0;
    if (constTexCoord) vertexShaderSubPermutationsToCompile |= 0x1;

    if ((noneFindResult->second->m_SubPermutations.get() & (1 << vertexShaderSubPermutationsToCompile)) == 0)
        vertexShaderSubPermutationsToCompile &= ~0x1;

    auto vertexDeclaration = mesh.vertexDeclaration;
    bool instancing = args.instancing || isFur;

    if (instancing)
    {
        GuestVertexElement vertexElements[64];
        memcpy(vertexElements, mesh.vertexDeclaration->vertexElements.get(), (mesh.vertexDeclaration->vertexElementCount - 1) * sizeof(GuestVertexElement));

        if (args.instancing)
        {
            vertexElements[mesh.vertexDeclaration->vertexElementCount - 1] = { 1, 0, 0x2A23B9, 0, 5, 4 };
            vertexElements[mesh.vertexDeclaration->vertexElementCount] = { 1, 12, 0x2C2159, 0, 5, 5 };
            vertexElements[mesh.vertexDeclaration->vertexElementCount + 1] = { 1, 16, 0x2C2159, 0, 5, 6 };
            vertexElements[mesh.vertexDeclaration->vertexElementCount + 2] = { 1, 20, 0x182886, 0, 10, 1 };
            vertexElements[mesh.vertexDeclaration->vertexElementCount + 3] = { 2, 0, 0x2C82A1, 0, 0, 1 };
            vertexElements[mesh.vertexDeclaration->vertexElementCount + 4] = D3DDECL_END();
        }
        else if (isFur)
        {
            vertexElements[mesh.vertexDeclaration->vertexElementCount - 1] = { 1, 0, 0x2C82A1, 0, 0, 1 };
            vertexElements[mesh.vertexDeclaration->vertexElementCount] = { 2, 0, 0x2C83A4, 0, 0, 2 };
            vertexElements[mesh.vertexDeclaration->vertexElementCount + 1] = D3DDECL_END();
        }

        vertexDeclaration = CreateVertexDeclarationWithoutAddRef(vertexElements);
    }

    for (auto& [pixelShaderSubPermutations, pixelShader] : defaultFindResult->second.m_PixelShaders)
    {
        if (pixelShader.get() == nullptr || (pixelShaderSubPermutations & 0x3) != pixelShaderSubPermutationsToCompile)
            continue;

        for (auto& [vertexShaderSubPermutations, vertexShader] : noneFindResult->second->m_VertexShaders)
        {
            if (vertexShader.get() == nullptr || (vertexShaderSubPermutations & 0x1) != vertexShaderSubPermutationsToCompile)
                continue;

            PipelineState pipelineState{};
            pipelineState.vertexShader = reinterpret_cast<GuestShader*>(vertexShader->m_spCode->m_pD3DVertexShader.get());
            pipelineState.pixelShader = reinterpret_cast<GuestShader*>(pixelShader->m_spCode->m_pD3DPixelShader.get());
            pipelineState.vertexDeclaration = vertexDeclaration;
            pipelineState.instancing = instancing;
            pipelineState.zWriteEnable = !isSky && mesh.layer != MeshLayer::Transparent;
            pipelineState.srcBlend = RenderBlend::SRC_ALPHA;
            pipelineState.destBlend = mesh.material->m_Additive ? RenderBlend::ONE : RenderBlend::INV_SRC_ALPHA;
            pipelineState.cullMode = mesh.material->m_DoubleSided ? RenderCullMode::NONE : RenderCullMode::BACK;
            pipelineState.zFunc = RenderComparisonFunction::GREATER_EQUAL; // Reverse Z
            pipelineState.alphaBlendEnable = mesh.layer == MeshLayer::Transparent || mesh.layer == MeshLayer::Special;
            pipelineState.srcBlendAlpha = RenderBlend::SRC_ALPHA;
            pipelineState.destBlendAlpha = RenderBlend::INV_SRC_ALPHA;
            pipelineState.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_STRIP;
            pipelineState.vertexStrides[0] = mesh.vertexSize;

            if (args.instancing)
            {
                pipelineState.vertexStrides[1] = 24;
                pipelineState.vertexStrides[2] = 4;
            }
            else if (isFur)
            {
                pipelineState.vertexStrides[1] = 4;
                pipelineState.vertexStrides[2] = 4;
            }

            pipelineState.renderTargetFormat = RenderFormat::R16G16B16A16_FLOAT;
            pipelineState.depthStencilFormat = RenderFormat::D32_FLOAT;
            pipelineState.sampleCount = Config::AntiAliasing != EAntiAliasing::None ? int32_t(Config::AntiAliasing.Value) : 1;

            if (pipelineState.vertexDeclaration->hasR11G11B10Normal)
                pipelineState.specConstants |= SPEC_CONSTANT_R11G11B10_NORMAL;

            if (Config::GITextureFiltering == EGITextureFiltering::Bicubic)
                pipelineState.specConstants |= SPEC_CONSTANT_BICUBIC_GI_FILTER;

            if (mesh.layer == MeshLayer::PunchThrough)
            {
                if (Config::AntiAliasing != EAntiAliasing::None && Config::TransparencyAntiAliasing)
                {
                    pipelineState.enableAlphaToCoverage = true;
                    pipelineState.specConstants |= SPEC_CONSTANT_ALPHA_TO_COVERAGE;
                }
                else
                {
                    pipelineState.specConstants |= SPEC_CONSTANT_ALPHA_TEST;
                }
            }

            if (!isSky)
                pipelineState.specConstants |= SPEC_CONSTANT_REVERSE_Z;

            auto createGraphicsPipeline = [&](PipelineState& pipelineStateToCreate)
                {
                    SanitizePipelineState(pipelineStateToCreate);
                    EnqueueGraphicsPipelineCompilation(pipelineStateToCreate, args.tokenPair, shaderList->m_TypeAndName.c_str() + 3);
#ifdef UNLEASHED_RECOMP_SHADER_SPECIALIZATION
                    if (!mesh.morphModel)
                        EnqueueBonesVariantCompilation(pipelineStateToCreate, args.hasMoreThanOneBone, args.tokenPair, shaderList->m_TypeAndName.c_str() + 3);
#endif

                    // Morph models have 4 targets where unused targets default to the first vertex stream.
                    if (mesh.morphModel)
                    {
                        for (size_t i = 0; i < 5; i++)
                        {
                            for (size_t j = 0; j < 4; j++)
                                pipelineStateToCreate.vertexStrides[j + 1] = i > j ? mesh.morphTargetVertexSize : mesh.vertexSize;

                            SanitizePipelineState(pipelineStateToCreate);
                            EnqueueGraphicsPipelineCompilation(pipelineStateToCreate, args.tokenPair, shaderList->m_TypeAndName.c_str() + 3);
                        }
                    }
                };

            createGraphicsPipeline(pipelineState);

            // We cannot rely on this being accurate during loading as SceneEffect.prm.xml gets loaded a bit later.
            bool planarReflectionEnabled = *reinterpret_cast<bool*>(g_memory.Translate(0x832FA0D8));
            bool loading = *SWA::SGlobals::ms_IsLoading;
            bool compileNoMsaaPipeline = pipelineState.sampleCount != 1 && (loading || planarReflectionEnabled);

            auto noMsaaPipeline = pipelineState;
            noMsaaPipeline.sampleCount = 1;
            noMsaaPipeline.enableAlphaToCoverage = false;

            if ((noMsaaPipeline.specConstants & SPEC_CONSTANT_ALPHA_TO_COVERAGE) != 0)
            {
                noMsaaPipeline.specConstants &= ~SPEC_CONSTANT_ALPHA_TO_COVERAGE;
                noMsaaPipeline.specConstants |= SPEC_CONSTANT_ALPHA_TEST;
            }

            if (compileNoMsaaPipeline)
            {
                // Planar reflections don't use MSAA.
                createGraphicsPipeline(noMsaaPipeline);
            }

            if (args.objectIcon) 
            {
                // Object icons get rendered to a SDR buffer without MSAA.
                auto iconPipelineState = noMsaaPipeline;
                iconPipelineState.renderTargetFormat = RenderFormat::R8G8B8A8_UNORM;
                createGraphicsPipeline(iconPipelineState);
            }

            if (isSonicMouth)
            {
                // Sonic's mouth switches between "SonicSkin_dspf[b]" or "SonicSkinNodeInvX_dspf[b]" depending on the view angle.
                auto mouthPipelineState = pipelineState;
                mouthPipelineState.vertexShader = FindShaderCacheEntry(0x689AA3140AB9EBAA)->guestShader;
                createGraphicsPipeline(mouthPipelineState);

                if (compileNoMsaaPipeline)
                {
                    auto noMsaaMouthPipelineState = noMsaaPipeline;
                    noMsaaMouthPipelineState.vertexShader = mouthPipelineState.vertexShader;
                    createGraphicsPipeline(noMsaaMouthPipelineState);
                }
            }
        }
    }
}

static void CompileMeshPipeline(Hedgehog::Mirage::CMeshData* mesh, MeshLayer layer, CompilationArgs& args)
{
    CompileMeshPipeline(Mesh
        {
            mesh->m_VertexSize,
            0,
            reinterpret_cast<GuestVertexDeclaration*>(mesh->m_VertexDeclarationPtr.m_pD3DVertexDeclaration.get()),
            mesh->m_spMaterial.get(),
            layer,
            false
        }, args);
}

static void CompileMeshPipeline(Hedgehog::Mirage::CMorphModelData* morphModel, Hedgehog::Mirage::CMeshIndexData* mesh, MeshLayer layer, CompilationArgs& args)
{
    CompileMeshPipeline(Mesh
        {
            morphModel->m_VertexSize,
            morphModel->m_MorphTargetVertexSize,
            reinterpret_cast<GuestVertexDeclaration*>(morphModel->m_VertexDeclarationPtr.m_pD3DVertexDeclaration.get()),
            mesh->m_spMaterial.get(),
            layer,
            true
        }, args);
}

template<typename T>
static void CompileMeshPipelines(const T& modelData, CompilationArgs& args)
{
    for (auto& meshGroup : modelData.m_NodeGroupModels)
    {
        for (auto& mesh : meshGroup->m_OpaqueMeshes)
        {
            CompileMeshPipeline(mesh.get(), MeshLayer::Opaque, args);

            if (args.noGI) // For models that can be shown transparent (eg. medals)
                CompileMeshPipeline(mesh.get(), MeshLayer::Transparent, args);
        }

        for (auto& mesh : meshGroup->m_TransparentMeshes)
            CompileMeshPipeline(mesh.get(), MeshLayer::Transparent, args);

        for (auto& mesh : meshGroup->m_PunchThroughMeshes)
            CompileMeshPipeline(mesh.get(), MeshLayer::PunchThrough, args);

        for (auto& specialMeshGroup : meshGroup->m_SpecialMeshGroups)
        {
            for (auto& mesh : specialMeshGroup)
                CompileMeshPipeline(mesh.get(), MeshLayer::Special, args); // TODO: Are there layer types other than water in this game??
        }
    }

    for (auto& mesh : modelData.m_OpaqueMeshes)
    {
        CompileMeshPipeline(mesh.get(), MeshLayer::Opaque, args);

        if (args.noGI)
            CompileMeshPipeline(mesh.get(), MeshLayer::Transparent, args);
    }

    for (auto& mesh : modelData.m_TransparentMeshes)
        CompileMeshPipeline(mesh.get(), MeshLayer::Transparent, args);

    for (auto& mesh : modelData.m_PunchThroughMeshes)
        CompileMeshPipeline(mesh.get(), MeshLayer::PunchThrough, args);

    if constexpr (std::is_same_v<T, Hedgehog::Mirage::CModelData>)
    {
        for (auto& morphModel : modelData.m_MorphModels)
        {
            for (auto& mesh : morphModel->m_OpaqueMeshList)
                CompileMeshPipeline(morphModel.get(), mesh.get(), MeshLayer::Opaque, args);

            for (auto& mesh : morphModel->m_TransparentMeshList)
                CompileMeshPipeline(morphModel.get(), mesh.get(), MeshLayer::Transparent, args);

            for (auto& mesh : morphModel->m_PunchThroughMeshList)
                CompileMeshPipeline(morphModel.get(), mesh.get(), MeshLayer::PunchThrough, args);
        }
    }
}

static void CompileParticleMaterialPipeline(const Hedgehog::Sparkle::CParticleMaterial& material, PipelineTaskTokenPair& tokenPair)
{
    auto& shaderList = material.m_spShaderListData;
    if (shaderList.get() == nullptr)
        return;

    guest_stack_var<Hedgehog::Base::CStringSymbol> defaultSymbol(reinterpret_cast<const char*>(g_memory.Translate(0x8202DDBC)));
    auto defaultFindResult = shaderList->m_PixelShaderPermutations.find(*defaultSymbol);
    if (defaultFindResult == shaderList->m_PixelShaderPermutations.end())
        return;

    guest_stack_var<Hedgehog::Base::CStringSymbol> noneSymbol(reinterpret_cast<const char*>(g_memory.Translate(0x8200D938)));
    auto noneFindResult = defaultFindResult->second.m_VertexShaderPermutations.find(*noneSymbol);
    if (noneFindResult == defaultFindResult->second.m_VertexShaderPermutations.end())
        return;

    // All the particle models in the game come with the unoptimized format, so we can assume it.
    uint8_t unoptimizedVertexElements[144] = 
    {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x2A, 0x23, 0xB9, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x0C, 0x00, 0x2A, 0x23, 0xB9, 0x00, 0x03, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x18, 0x00, 0x2A, 0x23, 0xB9, 0x00, 0x06, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x24, 0x00, 0x2A, 0x23, 0xB9, 0x00, 0x07, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x30, 0x00, 0x2C, 0x23, 0xA5, 0x00, 0x05, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x38, 0x00, 0x2C, 0x23, 0xA5, 0x00, 0x05, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x40, 0x00, 0x2C, 0x23, 0xA5, 0x00, 0x05, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x48, 0x00, 0x2C, 0x23, 0xA5, 0x00, 0x05, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x50, 0x00, 0x1A, 0x23, 0xA6, 0x00, 0x0A, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x60, 0x00, 0x1A, 0x23, 0x86, 0x00, 0x02, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x64, 0x00, 0x1A, 0x20, 0x86, 0x00, 0x01, 0x00, 0x00,
        0x00, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00
    };

    auto unoptimizedVertexDeclaration = CreateVertexDeclarationWithoutAddRef(reinterpret_cast<GuestVertexElement*>(unoptimizedVertexElements));
    auto sparkleVertexDeclaration = CreateVertexDeclarationWithoutAddRef(reinterpret_cast<GuestVertexElement*>(g_memory.Translate(0x8211F540)));

    bool isMeshShader = strstr(shaderList->m_TypeAndName.c_str(), "Mesh") != nullptr;

    PipelineState pipelineState{};
    pipelineState.vertexShader = reinterpret_cast<GuestShader*>(noneFindResult->second->m_VertexShaders.begin()->second->m_spCode->m_pD3DVertexShader.get());
    pipelineState.pixelShader = reinterpret_cast<GuestShader*>(defaultFindResult->second.m_PixelShaders.begin()->second->m_spCode->m_pD3DPixelShader.get());
    pipelineState.vertexDeclaration = isMeshShader ? unoptimizedVertexDeclaration : sparkleVertexDeclaration;
    pipelineState.zWriteEnable = false;
    pipelineState.zFunc = RenderComparisonFunction::GREATER_EQUAL;
    pipelineState.alphaBlendEnable = true;
    pipelineState.srcBlendAlpha = RenderBlend::SRC_ALPHA;
    pipelineState.destBlendAlpha = RenderBlend::INV_SRC_ALPHA;
    pipelineState.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_STRIP;
    pipelineState.vertexStrides[0] = isMeshShader ? 104 : 28;
    pipelineState.depthStencilFormat = RenderFormat::D32_FLOAT;
    pipelineState.specConstants = SPEC_CONSTANT_REVERSE_Z;

    if (pipelineState.vertexDeclaration->hasR11G11B10Normal)
        pipelineState.specConstants |= SPEC_CONSTANT_R11G11B10_NORMAL;

    switch (material.m_BlendMode.get())
    {
    case Hedgehog::Sparkle::CParticleMaterial::eBlendMode_Zero:
        pipelineState.srcBlend = RenderBlend::ZERO;
        pipelineState.destBlend = RenderBlend::ZERO;
        break;
    case Hedgehog::Sparkle::CParticleMaterial::eBlendMode_Typical:
        pipelineState.srcBlend = RenderBlend::SRC_ALPHA;
        pipelineState.destBlend = RenderBlend::INV_SRC_ALPHA;
        break;
    case Hedgehog::Sparkle::CParticleMaterial::eBlendMode_Add:
        pipelineState.srcBlend = RenderBlend::SRC_ALPHA;
        pipelineState.destBlend = RenderBlend::ONE;
        break;
    default:
        pipelineState.srcBlend = RenderBlend::ONE;
        pipelineState.destBlend = RenderBlend::ONE;
        break;
    }

    auto createGraphicsPipeline = [&](PipelineState& pipelineStateToCreate)
        {
            SanitizePipelineState(pipelineStateToCreate);
            EnqueueGraphicsPipelineCompilation(pipelineStateToCreate, tokenPair, shaderList->m_TypeAndName.c_str() + 3);
        };

    // Mesh particles can use both cull modes. Quad particles are only NONE.
    RenderCullMode cullModes[] = { RenderCullMode::NONE, RenderCullMode::BACK };
    uint32_t cullModeCount = isMeshShader ? std::size(cullModes) : 1;
    RenderFormat renderTargetFormats[] = { RenderFormat::R16G16B16A16_FLOAT, RenderFormat::R8G8B8A8_UNORM };

    for (size_t i = 0; i < cullModeCount; i++)
    {
        pipelineState.cullMode = cullModes[i];

        for (auto renderTargetFormat : renderTargetFormats)
        {
            pipelineState.renderTargetFormat = renderTargetFormat;

            if (renderTargetFormat == RenderFormat::R16G16B16A16_FLOAT)
                pipelineState.sampleCount = Config::AntiAliasing != EAntiAliasing::None ? int32_t(Config::AntiAliasing.Value) : 1;
            else
                pipelineState.sampleCount = 1;

            createGraphicsPipeline(pipelineState);

            // Always compile no MSAA variant for particles, as the planar
            // reflection variable isn't reliable at this time of compilation.
            bool compileNoMsaaPipeline = pipelineState.sampleCount != 1;

            auto noMsaaPipelineState = pipelineState;
            noMsaaPipelineState.sampleCount = 1;

            if (compileNoMsaaPipeline)
                createGraphicsPipeline(noMsaaPipelineState);

            if (!isMeshShader)
            {
                // Previous compilation was for locus particles. This one will be for quads.
                auto quadPipelineState = pipelineState;
                quadPipelineState.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
                createGraphicsPipeline(quadPipelineState);

                if (compileNoMsaaPipeline)
                {
                    auto noMsaaQuadPipelineState = noMsaaPipelineState;
                    noMsaaQuadPipelineState.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
                    createGraphicsPipeline(noMsaaQuadPipelineState);
                }
            }
        }
    }
}

static std::thread::id g_mainThreadId = std::this_thread::get_id();

// SWA::CGameModeStage::ExitLoading
PPC_FUNC_IMPL(__imp__sub_825369A0);
PPC_FUNC(sub_825369A0)
{
    assert(std::this_thread::get_id() == g_mainThreadId);

    // Wait for pipeline compilations to finish.
    uint32_t value;
    while ((value = g_compilingPipelineTaskCount.load()) != 0)
    {
        // Pump SDL events to prevent the OS
        // from thinking the process is unresponsive.
#if !defined(__SWITCH__)
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
#endif

        g_compilingPipelineTaskCount.wait(value);
    }

    __imp__sub_825369A0(ctx, base);
}

// CModelData::CheckMadeAll
PPC_FUNC_IMPL(__imp__sub_82E2EFB0);
PPC_FUNC(sub_82E2EFB0)
{   
    if (reinterpret_cast<Hedgehog::Database::CDatabaseData*>(base + ctx.r3.u32)->m_Flags & eDatabaseDataFlags_CompilingPipelines)
    {
        ctx.r3.u64 = 0;
    }
    else
    {
        __imp__sub_82E2EFB0(ctx, base);
    }
}

// CTerrainModelData::CheckMadeAll
PPC_FUNC_IMPL(__imp__sub_82E243D8);
PPC_FUNC(sub_82E243D8)
{   
    if (reinterpret_cast<Hedgehog::Database::CDatabaseData*>(base + ctx.r3.u32)->m_Flags & eDatabaseDataFlags_CompilingPipelines)
    {
        ctx.r3.u64 = 0;
    }
    else
    {
        __imp__sub_82E243D8(ctx, base);
    }
}

// CParticleMaterial::CheckMadeAll
PPC_FUNC_IMPL(__imp__sub_82E87598);
PPC_FUNC(sub_82E87598)
{   
    if (reinterpret_cast<Hedgehog::Database::CDatabaseData*>(base + ctx.r3.u32)->m_Flags & eDatabaseDataFlags_CompilingPipelines)
    {
        ctx.r3.u64 = 0;
    }
    else
    {
        __imp__sub_82E87598(ctx, base);
    }
}

void GetDatabaseDataMidAsmHook(PPCRegister& r1, PPCRegister& r4)
{
    auto& databaseData = *reinterpret_cast<boost::shared_ptr<Hedgehog::Database::CDatabaseData>*>(
        g_memory.Translate(r1.u32 + 0x58));

    if (!databaseData->IsMadeOne() && r4.u32 != NULL)
    {
        if (databaseData->m_pVftable.ptr == MODEL_DATA_VFTABLE)
        {
            // Ignore particle models, the materials they point at don't actually
            // get used and give the threads unnecessary work.
            bool isParticleModel = *reinterpret_cast<be<uint32_t>*>(g_memory.Translate(r4.u32 + 4)) != 5 &&
                strncmp(databaseData->m_TypeAndName.c_str() + 2, "eff_", 4) == 0;

            if (isParticleModel)
                return;

            // Adabat water is broken in original game, which they tried to fix by partially including the files in the update,
            // which then finally fixed for real in the DLC. This confuses the async PSO compiler and causes a hang if the DLC is missing.
            // We'll just ignore it.
            bool isAdabatWater = strcmp(databaseData->m_TypeAndName.c_str() + 2, "evl_sea_obj_st_waterCircle") == 0;
            if (isAdabatWater)
                return;
        }

        databaseData->m_Flags |= eDatabaseDataFlags_CompilingPipelines;
        EnqueuePipelineTask(PipelineTaskType::DatabaseData, databaseData);
    }
}

static bool CheckMadeAll(Hedgehog::Mirage::CMeshData* meshData)
{
    if (!meshData->IsMadeOne())
        return false;

    if (meshData->m_spMaterial.get() != nullptr)
    {
        if (!meshData->m_spMaterial->IsMadeOne())
            return false;

        if (meshData->m_spMaterial->m_spTexsetData.get() != nullptr)
        {
            if (!meshData->m_spMaterial->m_spTexsetData->IsMadeOne())
                return false;

            for (auto& texture : meshData->m_spMaterial->m_spTexsetData->m_TextureList)
            {
                if (!texture->IsMadeOne())
                    return false;
            }
        }
    }

    return true;
}

template<typename T>
static bool CheckMadeAll(const T& modelData)
{
    if (!modelData.IsMadeOne())
        return false;

    for (auto& meshGroup : modelData.m_NodeGroupModels)
    {
        for (auto& mesh : meshGroup->m_OpaqueMeshes)
        {
            if (!CheckMadeAll(mesh.get()))
                return false;
        }     

        for (auto& mesh : meshGroup->m_TransparentMeshes)
        {
            if (!CheckMadeAll(mesh.get()))
                return false;
        }    

        for (auto& mesh : meshGroup->m_PunchThroughMeshes)
        {
            if (!CheckMadeAll(mesh.get()))
                return false;
        }

        for (auto& specialMeshGroup : meshGroup->m_SpecialMeshGroups)
        {
            for (auto& mesh : specialMeshGroup)
            {
                if (!CheckMadeAll(mesh.get()))
                    return false;
            }
        }
    }

    for (auto& mesh : modelData.m_OpaqueMeshes)
    {
        if (!CheckMadeAll(mesh.get()))
            return false;
    }

    for (auto& mesh : modelData.m_TransparentMeshes)
    {
        if (!CheckMadeAll(mesh.get()))
            return false;
    }

    for (auto& mesh : modelData.m_PunchThroughMeshes)
    {
        if (!CheckMadeAll(mesh.get()))
            return false;
    }

    return true;
}

static void PipelineTaskConsumerThread()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
    GuestThread::SetThreadName(GetCurrentThreadId(), "Pipeline Task Consumer Thread");
#elif defined(__SWITCH__)
    svcSetThreadPriority(threadGetCurHandle(), 0x3B);
#endif

    std::vector<PipelineTask> localPipelineTaskQueue;
    std::unique_ptr<GuestThreadContext> ctx;
#if defined(__SWITCH__)
    bool coreApplied = false;
#endif

    while (true)
    {
        // Wait for tasks to arrive.
        uint32_t pendingPipelineTaskCount;
        while ((pendingPipelineTaskCount = g_pendingPipelineTaskCount.load()) == 0)
            g_pendingPipelineTaskCount.wait(pendingPipelineTaskCount);

        if (ctx == nullptr)
            ctx = std::make_unique<GuestThreadContext>(0);

#if defined(__SWITCH__)
        if (!coreApplied && g_hostThreadCoresReady.load(std::memory_order_acquire))
        {
            coreApplied = true;
            SetHostThreadCore(2);
        }
#endif

        {
            std::lock_guard lock(g_pipelineTaskMutex);
            localPipelineTaskQueue.insert(localPipelineTaskQueue.end(), g_pipelineTaskQueue.begin(), g_pipelineTaskQueue.end());
            g_pipelineTaskQueue.clear();
        }

        bool allHandled = true;

        for (auto& [type, databaseData] : localPipelineTaskQueue)
        {
            switch (type)
            {
            case PipelineTaskType::DatabaseData:
            {
                bool ready = false;

                if (databaseData->m_pVftable.ptr == MODEL_DATA_VFTABLE)
                    ready = CheckMadeAll(*reinterpret_cast<Hedgehog::Mirage::CModelData*>(databaseData.get()));
                else
                    ready = databaseData->IsMadeOne();

                if (ready || databaseData.unique())
                {
                    if (databaseData->m_pVftable.ptr == TERRAIN_MODEL_DATA_VFTABLE)
                    {
                        CompilationArgs args{};
                        args.tokenPair.token.type = type;
                        args.tokenPair.token.databaseData = databaseData;
                        args.instancing = strncmp(databaseData->m_TypeAndName.c_str() + 3, "ins", 3) == 0;
                        CompileMeshPipelines(*reinterpret_cast<Hedgehog::Mirage::CTerrainModelData*>(databaseData.get()), args);
                    }
                    else if (databaseData->m_pVftable.ptr == PARTICLE_MATERIAL_VFTABLE)
                    {
                        PipelineTaskTokenPair tokenPair;
                        tokenPair.token.type = type;
                        tokenPair.token.databaseData = databaseData;
                        CompileParticleMaterialPipeline(*reinterpret_cast<Hedgehog::Sparkle::CParticleMaterial*>(databaseData.get()), tokenPair);
                    }
                    else
                    {
                        assert(databaseData->m_pVftable.ptr == MODEL_DATA_VFTABLE);

                        auto modelData = reinterpret_cast<Hedgehog::Mirage::CModelData*>(databaseData.get());

                        CompilationArgs args{};
                        args.tokenPair.token.type = type;
                        args.tokenPair.token.databaseData = databaseData;
                        args.noGI = true;
                        args.hasMoreThanOneBone = modelData->m_NodeNum > 1;
                        args.velocityMapQuickStep = strcmp(databaseData->m_TypeAndName.c_str() + 2, "SonicRoot") == 0;

                        // Check for the on screen items, eg. rings going to HUD.
                        auto items = reinterpret_cast<xpointer<const char>*>(g_memory.Translate(0x832A8DD0));
                        for (size_t i = 0; i < 50; i++)
                        {
                            if (strcmp(databaseData->m_TypeAndName.c_str() + 2, (*items).get()) == 0)
                            {
                                args.objectIcon = true;
                                break;
                            }
                            items += 7;
                        }

                        CompileMeshPipelines(*modelData, args);
                    }

                    type = PipelineTaskType::Null;
                    databaseData = nullptr;

                    --g_pendingPipelineTaskCount;
                }
                else
                {
                    allHandled = false;
                }

                break;
            }

            case PipelineTaskType::PrecompilePipelines:
            {
                // Deliberately leaving the type null to account for the enqueue
                // call not incrementing the compiling pipeline task counter.
                PipelineTaskTokenPair tokenPair;

                for (auto vertexElements : g_vertexDeclarationCache)
                    CreateVertexDeclarationWithoutAddRef(reinterpret_cast<GuestVertexElement*>(vertexElements));

                for (auto pipelineState : g_pipelineStateCache)
                {
                    // The hashes were reinterpret casted to pointers in the cache.
                    pipelineState.vertexShader = FindShaderCacheEntry(reinterpret_cast<XXH64_hash_t>(pipelineState.vertexShader))->guestShader;

                    if (pipelineState.pixelShader != nullptr)
                        pipelineState.pixelShader = FindShaderCacheEntry(reinterpret_cast<XXH64_hash_t>(pipelineState.pixelShader))->guestShader;

                    {
                        std::lock_guard lock(g_vertexDeclarationMutex);
                        pipelineState.vertexDeclaration = g_vertexDeclarations[reinterpret_cast<XXH64_hash_t>(pipelineState.vertexDeclaration)];
                    }

                    if (!g_capabilities.triangleFan && pipelineState.primitiveTopology == RenderPrimitiveTopology::TRIANGLE_FAN)
                        pipelineState.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;

                    // Zero out depth bias for Vulkan, we only store common values for D3D12.
                    if (g_capabilities.dynamicDepthBias && g_vulkan)
                    {
                        pipelineState.depthBias = 0;
                        pipelineState.slopeScaledDepthBias = 0.0f;
                    }

                    if (Config::GITextureFiltering == EGITextureFiltering::Bicubic)
                        pipelineState.specConstants |= SPEC_CONSTANT_BICUBIC_GI_FILTER;

                    auto createGraphicsPipeline = [&](PipelineState& pipelineStateToCreate, const char* name)
                        {
                            SanitizePipelineState(pipelineStateToCreate);
                            EnqueueGraphicsPipelineCompilation(pipelineStateToCreate, tokenPair, name, true);
                        };

                    // Compile both MSAA and non MSAA variants to work with reflection maps. The render formats are an assumption but it should hold true.
                    if (Config::AntiAliasing != EAntiAliasing::None &&
                        pipelineState.renderTargetFormat == RenderFormat::R16G16B16A16_FLOAT && 
                        pipelineState.depthStencilFormat == RenderFormat::D32_FLOAT)
                    {
                        auto msaaPipelineState = pipelineState;
                        msaaPipelineState.sampleCount = int32_t(Config::AntiAliasing.Value);

                        if (Config::TransparencyAntiAliasing && (msaaPipelineState.specConstants & SPEC_CONSTANT_ALPHA_TEST) != 0)
                        {
                            msaaPipelineState.enableAlphaToCoverage = true;
                            msaaPipelineState.specConstants &= ~SPEC_CONSTANT_ALPHA_TEST;
                            msaaPipelineState.specConstants |= SPEC_CONSTANT_ALPHA_TO_COVERAGE;
                        }

                        createGraphicsPipeline(msaaPipelineState, "Precompiled Pipeline MSAA");
                    }

                    if (pipelineState.pixelShader != nullptr &&
                        pipelineState.pixelShader->shaderCacheEntry != nullptr)
                    {
                        XXH64_hash_t hash = pipelineState.pixelShader->shaderCacheEntry->hash;

                        // Compile the custom gaussian blur shaders that we pass to the game.
                        if (hash == 0x4294510C775F4EE8)
                        {
                            for (auto& shader : g_gaussianBlurShaders)
                            {
                                auto newPipelineState = pipelineState;
                                newPipelineState.pixelShader = shader.get();
                                createGraphicsPipeline(newPipelineState, "Precompiled Gaussian Blur Pipeline");
                            }
                        }
                        // Compile enhanced motion blur shader.
                        else if (hash == 0x6B9732B4CD7E7740)
                        {
                            auto newPipelineState = pipelineState;
                            newPipelineState.pixelShader = g_enhancedMotionBlurShader.get();
                            createGraphicsPipeline(newPipelineState, "Precompiled Enhanced Motion Blur Pipeline");
                        }
                    }
                
                    createGraphicsPipeline(pipelineState, "Precompiled Pipeline");

                    // Compile the CSD filter shader that we pass to the game when point filtering is used.
                    if (pipelineState.pixelShader == g_csdShader)
                    {
                        pipelineState.pixelShader = g_csdFilterShader.get();
                        createGraphicsPipeline(pipelineState, "Precompiled CSD Filter Pipeline");
                    }
                }

                type = PipelineTaskType::Null;
                --g_pendingPipelineTaskCount;

                break;
            }

            case PipelineTaskType::RecompilePipelines:
            {
                PipelineTaskTokenPair tokenPair;
                tokenPair.token.type = type;

                auto asyncPipelines = g_asyncPipelineStates.values();

                for (auto& [hash, pipelineState] : asyncPipelines)
                {
                    bool alphaTest = (pipelineState.specConstants & (SPEC_CONSTANT_ALPHA_TEST | SPEC_CONSTANT_ALPHA_TO_COVERAGE)) != 0;
                    bool msaa = pipelineState.sampleCount != 1 || (pipelineState.renderTargetFormat == RenderFormat::R16G16B16A16_FLOAT && pipelineState.depthStencilFormat == RenderFormat::D32_FLOAT);

                    pipelineState.sampleCount = 1;
                    pipelineState.enableAlphaToCoverage = false;
                    pipelineState.specConstants &= ~(SPEC_CONSTANT_BICUBIC_GI_FILTER | SPEC_CONSTANT_ALPHA_TEST | SPEC_CONSTANT_ALPHA_TO_COVERAGE);

                    if (msaa && Config::AntiAliasing != EAntiAliasing::None)
                    {
                        pipelineState.sampleCount = int32_t(Config::AntiAliasing.Value);

                        if (alphaTest)
                        {
                            if (Config::TransparencyAntiAliasing)
                            {
                                pipelineState.enableAlphaToCoverage = true;
                                pipelineState.specConstants |= SPEC_CONSTANT_ALPHA_TO_COVERAGE;
                            }
                            else
                            {
                                pipelineState.specConstants |= SPEC_CONSTANT_ALPHA_TEST;
                            }
                        }
                    }
                    else if (alphaTest)
                    {
                        pipelineState.specConstants |= SPEC_CONSTANT_ALPHA_TEST;
                    }

                    if (Config::GITextureFiltering == EGITextureFiltering::Bicubic)
                        pipelineState.specConstants |= SPEC_CONSTANT_BICUBIC_GI_FILTER;

                    SanitizePipelineState(pipelineState);
                    EnqueueGraphicsPipelineCompilation(pipelineState, tokenPair, "Recompiled Pipeline State");
                }

                type = PipelineTaskType::Null;
                --g_pendingPipelineTaskCount;

                break;
            }
            }
        }

        if (allHandled)
            localPipelineTaskQueue.clear();

#if defined(__SWITCH__)
        // Retry throttle while resources finish loading; a yield spin here
        // burns a shared core for the whole loading screen.
        if (!allHandled)
            svcSleepThread(500000);
#else
        std::this_thread::yield();
#endif
    }
}

static std::thread g_pipelineTaskConsumerThread(PipelineTaskConsumerThread);

#ifdef ASYNC_PSO_DEBUG

PPC_FUNC_IMPL(__imp__sub_82E33330);
PPC_FUNC(sub_82E33330)
{
    auto vertexShaderCode = reinterpret_cast<Hedgehog::Mirage::CVertexShaderCodeData*>(g_memory.Translate(ctx.r4.u32));
    __imp__sub_82E33330(ctx, base);
    reinterpret_cast<GuestShader*>(vertexShaderCode->m_pD3DVertexShader.get())->name = vertexShaderCode->m_TypeAndName.c_str() + 3;
}

PPC_FUNC_IMPL(__imp__sub_82E328D8);
PPC_FUNC(sub_82E328D8)
{
    auto pixelShaderCode = reinterpret_cast<Hedgehog::Mirage::CPixelShaderCodeData*>(g_memory.Translate(ctx.r4.u32));
    __imp__sub_82E328D8(ctx, base);
    reinterpret_cast<GuestShader*>(pixelShaderCode->m_pD3DPixelShader.get())->name = pixelShaderCode->m_TypeAndName.c_str() + 2;
}

#endif

#ifdef PSO_CACHING
class SDLEventListenerForPSOCaching : public SDLEventListener
{
public:
    bool OnSDLEvent(SDL_Event* event) override 
    {
        if (event->type != SDL_QUIT)
            return false;

        std::lock_guard lock(g_pipelineCacheMutex);
        if (g_pipelineStatesToCache.empty())
            return false;

        FILE* f = fopen("send_this_file_to_skyth.txt", "ab");
        if (f != nullptr)
        {
            ankerl::unordered_dense::set<GuestVertexDeclaration*> vertexDeclarations;
            xxHashMap<PipelineState> pipelineStatesToCache;

            for (auto& [hash, pipelineState] : g_pipelineStatesToCache)
            {
                if (pipelineState.vertexShader->shaderCacheEntry == nullptr ||
                    (pipelineState.pixelShader != nullptr && pipelineState.pixelShader->shaderCacheEntry == nullptr))
                {
                    continue;
                }

                vertexDeclarations.emplace(pipelineState.vertexDeclaration);

                // Mask out the config options.
                pipelineState.sampleCount = 1;
                pipelineState.enableAlphaToCoverage = false;

                pipelineState.specConstants &= ~SPEC_CONSTANT_BICUBIC_GI_FILTER;
                if ((pipelineState.specConstants & SPEC_CONSTANT_ALPHA_TO_COVERAGE) != 0)
                {
                    pipelineState.specConstants &= ~SPEC_CONSTANT_ALPHA_TO_COVERAGE;
                    pipelineState.specConstants |= SPEC_CONSTANT_ALPHA_TEST;
                }

                pipelineStatesToCache.emplace(XXH3_64bits(&pipelineState, sizeof(pipelineState)), pipelineState);
            }

            for (auto vertexDeclaration : vertexDeclarations)
            {
                fmt::print(f, "static uint8_t g_vertexElements_{:016X}[] = {{", vertexDeclaration->hash);

                auto bytes = reinterpret_cast<uint8_t*>(vertexDeclaration->vertexElements.get());
                for (size_t i = 0; i < vertexDeclaration->vertexElementCount * sizeof(GuestVertexElement); i++)
                    fmt::print(f, "0x{:X},", bytes[i]);

                fmt::println(f, "}};");
            }

            for (auto& [pipelineHash, pipelineState] : pipelineStatesToCache)
            {
                fmt::println(f, "{{ "
                    "reinterpret_cast<GuestShader*>(0x{:X}),"
                    "reinterpret_cast<GuestShader*>(0x{:X}),"
                    "reinterpret_cast<GuestVertexDeclaration*>(0x{:X}),"
                    "{},"
                    "{},"
                    "{},"
                    "RenderBlend::{},"
                    "RenderBlend::{},"
                    "RenderCullMode::{},"
                    "RenderComparisonFunction::{},"
                    "{},"
                    "RenderBlendOperation::{},"
                    "{},"
                    "{},"
                    "RenderBlend::{},"
                    "RenderBlend::{},"
                    "RenderBlendOperation::{},"
                    "0x{:X},"
                    "RenderPrimitiveTopology::{},"
                    "{{ {},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{} }},"
                    "RenderFormat::{},"
                    "RenderFormat::{},"
                    "{},"
                    "{},"
                    "0x{:X} }},",
                    pipelineState.vertexShader->shaderCacheEntry->hash,
                    pipelineState.pixelShader != nullptr ? pipelineState.pixelShader->shaderCacheEntry->hash : 0,
                    pipelineState.vertexDeclaration->hash,
                    pipelineState.instancing,
                    pipelineState.zEnable,
                    pipelineState.zWriteEnable,
                    magic_enum::enum_name(pipelineState.srcBlend),
                    magic_enum::enum_name(pipelineState.destBlend),
                    magic_enum::enum_name(pipelineState.cullMode),
                    magic_enum::enum_name(pipelineState.zFunc),
                    pipelineState.alphaBlendEnable,
                    magic_enum::enum_name(pipelineState.blendOp),
                    pipelineState.slopeScaledDepthBias,
                    pipelineState.depthBias,
                    magic_enum::enum_name(pipelineState.srcBlendAlpha),
                    magic_enum::enum_name(pipelineState.destBlendAlpha),
                    magic_enum::enum_name(pipelineState.blendOpAlpha),
                    pipelineState.colorWriteEnable,
                    magic_enum::enum_name(pipelineState.primitiveTopology),
                    pipelineState.vertexStrides[0],
                    pipelineState.vertexStrides[1],
                    pipelineState.vertexStrides[2],
                    pipelineState.vertexStrides[3],
                    pipelineState.vertexStrides[4],
                    pipelineState.vertexStrides[5],
                    pipelineState.vertexStrides[6],
                    pipelineState.vertexStrides[7],
                    pipelineState.vertexStrides[8],
                    pipelineState.vertexStrides[9],
                    pipelineState.vertexStrides[10],
                    pipelineState.vertexStrides[11],
                    pipelineState.vertexStrides[12],
                    pipelineState.vertexStrides[13],
                    pipelineState.vertexStrides[14],
                    pipelineState.vertexStrides[15],
                    magic_enum::enum_name(pipelineState.renderTargetFormat),
                    magic_enum::enum_name(pipelineState.depthStencilFormat),
                    pipelineState.sampleCount,
                    pipelineState.enableAlphaToCoverage,
                    pipelineState.specConstants);
            }

            fclose(f);
        }

        return false;
    }
};
SDLEventListenerForPSOCaching g_sdlEventListenerForPSOCaching;
#endif

void VideoConfigValueChangedCallback(IConfigDef* config)
{
    // Config options that require internal resolution resize
    g_needsResize |=
        config == &Config::AspectRatio ||
        config == &Config::ResolutionScale ||
        config == &Config::AntiAliasing ||
        config == &Config::ShadowResolution;

    if (g_needsResize)
        Video::ComputeViewportDimensions();
        
    // Config options that require pipeline recompilation
    bool shouldRecompile =
        config == &Config::AntiAliasing ||
        config == &Config::TransparencyAntiAliasing ||
        config == &Config::GITextureFiltering;

    if (shouldRecompile)
        EnqueuePipelineTask(PipelineTaskType::RecompilePipelines, {});
}

// SWA::CCsdTexListMirage::SetFilter
PPC_FUNC_IMPL(__imp__sub_825E4300);
PPC_FUNC(sub_825E4300)
{
    g_csdFilterState = ctx.r5.u32 == 0 ? CsdFilterState::On : CsdFilterState::Off;
    ctx.r5.u32 = 1;
    __imp__sub_825E4300(ctx, base);
}

// SWA::CCsdPlatformMirage::EndScene
PPC_FUNC_IMPL(__imp__sub_825E2F78);
PPC_FUNC(sub_825E2F78)
{
    g_csdFilterState = CsdFilterState::Unknown;
    __imp__sub_825E2F78(ctx, base);
}

// Game shares surfaces with identical descriptions. We don't want to share shadow maps,
// so we can set its format to a depth format that still resolves to the same type in recomp,
// but manages to keep the surfaces actually separated in guest code.
void FxShadowMapInitMidAsmHook(PPCRegister& r11)
{
    uint8_t* base = g_memory.base;

    uint32_t surface = PPC_LOAD_U32(PPC_LOAD_U32(PPC_LOAD_U32(r11.u32 + 0x24) + 0x4));
    PPC_STORE_U32(surface + 0x20, D3DFMT_D24FS8);
}

// Re-render objects in the terrain shadow map instead of copying the texture.
static bool g_jumpOverStretchRect;

void FxShadowMapNoTerrainMidAsmHook(PPCRegister& r4, PPCRegister& r30)
{
    // Set the no terrain shadow map as the render target.
    uint8_t* base = g_memory.base;
    r4.u64 = PPC_LOAD_U32(r30.u32 + 0x58);
}

bool FxShadowMapMidAsmHook(PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r30)
{
    if (g_jumpOverStretchRect)
    {
        // Reset for the next time shadow maps get rendered.
        g_jumpOverStretchRect = false;

        // Jump over the stretch rect call.
        return false;
    }
    else
    {
        // Mark to jump over the stretch call the next time.
        g_jumpOverStretchRect = true;

        // Jump to the beginning. Set registers accordingly to set the terrain shadow map as the render target.
        uint8_t* base = g_memory.base;
        r6.u64 = 0;
        r5.u64 = 0;
        r4.u64 = PPC_LOAD_U32(r30.u32 + 0x50);

        return true;
    }
}

// There is a bug on AMD where restart indices cause incorrect culling and prevent some triangles from being rendered.
// This seems to happen on both Windows AMD drivers and Mesa. Converting restart indices to degenerate triangles fixes it.
static void ConvertToDegenerateTriangles(uint16_t* indices, uint32_t indexCount, uint16_t*& newIndices, uint32_t& newIndexCount)
{
    newIndices = reinterpret_cast<uint16_t*>(g_userHeap.Alloc(indexCount * sizeof(uint16_t) * 3));
    newIndexCount = 0;

    bool stripStart = true;
    uint32_t stripSize = 0;
    uint16_t lastIndex = 0;

    for (uint32_t i = 0; i < indexCount; i++)
    {
        uint16_t index = indices[i];
        if (index == 0xFFFF)
        {
            if ((stripSize % 2) != 0)
                newIndices[newIndexCount++] = lastIndex;

            stripStart = true;
            stripSize = 0;
        }
        else 
        {
            if (stripStart && newIndexCount != 0)
            {
                newIndices[newIndexCount++] = lastIndex;
                newIndices[newIndexCount++] = index;
            }

            newIndices[newIndexCount++] = index;
            stripStart = false;
            ++stripSize;
            lastIndex = index;
        }
    }
}

struct MeshResource
{
    SWA_INSERT_PADDING(0x4);
    be<uint32_t> indexCount;
    be<uint32_t> indices;
};

static std::vector<uint16_t*> g_newIndicesToFree;

// Hedgehog::Mirage::CMeshData::Make
PPC_FUNC_IMPL(__imp__sub_82E44AF8);
PPC_FUNC(sub_82E44AF8)
{
    uint16_t* newIndicesToFree = nullptr;

    auto databaseData = reinterpret_cast<Hedgehog::Database::CDatabaseData*>(base + ctx.r3.u32);
    if (g_triangleStripWorkaround && !databaseData->IsMadeOne())
    {
        auto meshResource = reinterpret_cast<MeshResource*>(base + ctx.r4.u32);

        if (meshResource->indexCount != 0)
        {
            uint16_t* newIndices;
            uint32_t newIndexCount;

            ConvertToDegenerateTriangles(
                reinterpret_cast<uint16_t*>(base + meshResource->indices),
                meshResource->indexCount,
                newIndices,
                newIndexCount);

            meshResource->indexCount = newIndexCount;
            meshResource->indices = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(newIndices) - base);

            if (PPC_LOAD_U32(0x83396E98) != NULL)
            {
                // If index buffers are getting merged, new indices need to survive until the merge happens.
                g_newIndicesToFree.push_back(newIndices);
            }
            else 
            {
                // Otherwise, we can free it immediately.
                newIndicesToFree = newIndices;
            }
        }
    }

    __imp__sub_82E44AF8(ctx, base);

    if (newIndicesToFree != nullptr)
        g_userHeap.Free(newIndicesToFree);
}

// Hedgehog::Mirage::CShareVertexBuffer::Reset
PPC_FUNC_IMPL(__imp__sub_82E250D0);
PPC_FUNC(sub_82E250D0)
{
    __imp__sub_82E250D0(ctx, base);

    for (auto newIndicesToFree : g_newIndicesToFree)
        g_userHeap.Free(newIndicesToFree);

    g_newIndicesToFree.clear();
}

struct LightAndIndexBufferResourceV1
{
    SWA_INSERT_PADDING(0x4);
    be<uint32_t> indexCount;
    be<uint32_t> indices;
};

// Hedgehog::Mirage::CLightAndIndexBufferData::MakeV1
PPC_FUNC_IMPL(__imp__sub_82E3AFC8);
PPC_FUNC(sub_82E3AFC8)
{
    uint16_t* newIndices = nullptr;

    auto databaseData = reinterpret_cast<Hedgehog::Database::CDatabaseData*>(base + ctx.r3.u32);
    if (g_triangleStripWorkaround && !databaseData->IsMadeOne())
    {
        auto lightAndIndexBufferResource = reinterpret_cast<LightAndIndexBufferResourceV1*>(base + ctx.r4.u32);

        if (lightAndIndexBufferResource->indexCount != 0)
        {
            uint32_t newIndexCount;

            ConvertToDegenerateTriangles(
                reinterpret_cast<uint16_t*>(base + lightAndIndexBufferResource->indices),
                lightAndIndexBufferResource->indexCount,
                newIndices,
                newIndexCount);

            lightAndIndexBufferResource->indexCount = newIndexCount;
            lightAndIndexBufferResource->indices = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(newIndices) - base);
        }
    }

    __imp__sub_82E3AFC8(ctx, base);

    if (newIndices != nullptr)
        g_userHeap.Free(newIndices);
}

struct LightAndIndexBufferResourceV5
{
    SWA_INSERT_PADDING(0x8);
    be<uint32_t> indexCount;
    be<uint32_t> indices;
};

// Hedgehog::Mirage::CLightAndIndexBufferData::MakeV5
PPC_FUNC_IMPL(__imp__sub_82E3B1C0);
PPC_FUNC(sub_82E3B1C0)
{
    uint16_t* newIndices = nullptr;

    auto databaseData = reinterpret_cast<Hedgehog::Database::CDatabaseData*>(base + ctx.r3.u32);
    if (g_triangleStripWorkaround && !databaseData->IsMadeOne())
    {
        auto lightAndIndexBufferResource = reinterpret_cast<LightAndIndexBufferResourceV5*>(base + ctx.r4.u32);

        if (lightAndIndexBufferResource->indexCount != 0)
        {
            uint32_t newIndexCount;

            ConvertToDegenerateTriangles(
                reinterpret_cast<uint16_t*>(base + lightAndIndexBufferResource->indices),
                lightAndIndexBufferResource->indexCount,
                newIndices,
                newIndexCount);

            lightAndIndexBufferResource->indexCount = newIndexCount;
            lightAndIndexBufferResource->indices = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(newIndices) - base);
        }
    }

    __imp__sub_82E3B1C0(ctx, base);

    if (newIndices != nullptr)
        g_userHeap.Free(newIndices);
}

GUEST_FUNCTION_HOOK(sub_82BD99B0, CreateDevice);

GUEST_FUNCTION_HOOK(sub_82BE6230, DestructResource);

GUEST_FUNCTION_HOOK(sub_82BE9300, LockTextureRect);
GUEST_FUNCTION_HOOK(sub_82BE7780, UnlockTextureRect);

GUEST_FUNCTION_HOOK(sub_82BE6B98, LockVertexBuffer);
GUEST_FUNCTION_HOOK(sub_82BE6BE8, UnlockVertexBuffer);
GUEST_FUNCTION_HOOK(sub_82BE61D0, GetVertexBufferDesc);

GUEST_FUNCTION_HOOK(sub_82BE6CA8, LockIndexBuffer);
GUEST_FUNCTION_HOOK(sub_82BE6CF0, UnlockIndexBuffer);
GUEST_FUNCTION_HOOK(sub_82BE6200, GetIndexBufferDesc);

GUEST_FUNCTION_HOOK(sub_82BE96F0, GetSurfaceDesc);

GUEST_FUNCTION_HOOK(sub_82BE04B0, GetVertexDeclaration);
GUEST_FUNCTION_HOOK(sub_82BE0530, HashVertexDeclaration);

GUEST_FUNCTION_HOOK(sub_82BDA8C0, Video::Present);
GUEST_FUNCTION_HOOK(sub_82BDD330, GetBackBuffer);

GUEST_FUNCTION_HOOK(sub_82BE9498, CreateTexture);
GUEST_FUNCTION_HOOK(sub_82BE6AD0, CreateVertexBuffer);
GUEST_FUNCTION_HOOK(sub_82BE6BF8, CreateIndexBuffer);
GUEST_FUNCTION_HOOK(sub_82BE95B8, CreateSurface);

GUEST_FUNCTION_HOOK(sub_82BF6400, StretchRect);

GUEST_FUNCTION_HOOK(sub_82BDD9F0, SetRenderTarget);
GUEST_FUNCTION_HOOK(sub_82BDDD38, SetDepthStencilSurface);

GUEST_FUNCTION_HOOK(sub_82BFE4C8, Clear);

GUEST_FUNCTION_HOOK(sub_82BDD8C0, SetViewport);

GUEST_FUNCTION_HOOK(sub_82BE9818, SetTexture);
GUEST_FUNCTION_HOOK(sub_82BDCFB0, SetScissorRect);

GUEST_FUNCTION_HOOK(sub_82BE5900, DrawPrimitive);
GUEST_FUNCTION_HOOK(sub_82BE5CF0, DrawIndexedPrimitive);
GUEST_FUNCTION_HOOK(sub_82BE52F8, DrawPrimitiveUP);

GUEST_FUNCTION_HOOK(sub_82BE0428, CreateVertexDeclaration);
GUEST_FUNCTION_HOOK(sub_82BE02E0, SetVertexDeclaration);

GUEST_FUNCTION_HOOK(sub_82BE1A80, CreateVertexShader);
GUEST_FUNCTION_HOOK(sub_82BE0110, SetVertexShader);

GUEST_FUNCTION_HOOK(sub_82BDD0F8, SetStreamSource);
GUEST_FUNCTION_HOOK(sub_82BDD218, SetIndices);

GUEST_FUNCTION_HOOK(sub_82BE1990, CreatePixelShader);
GUEST_FUNCTION_HOOK(sub_82BDFE58, SetPixelShader);

GUEST_FUNCTION_HOOK(sub_82C003B8, D3DXFillTexture);
GUEST_FUNCTION_HOOK(sub_82C00910, D3DXFillVolumeTexture);

GUEST_FUNCTION_HOOK(sub_82E43FC8, MakePictureData);

GUEST_FUNCTION_HOOK(sub_82E9EE38, SetResolution);

GUEST_FUNCTION_HOOK(sub_82AE2BF8, ScreenShaderInit);

// This is a buggy function that recreates framebuffers
// if the inverse capture ratio is not 2.0, but the parameter
// is completely unused and not stored, so it ends up
// recreating framebuffers every single frame instead.
GUEST_FUNCTION_STUB(sub_82BAAD38);

GUEST_FUNCTION_STUB(sub_822C15D8);
GUEST_FUNCTION_STUB(sub_822C1810);
GUEST_FUNCTION_STUB(sub_82BD97A8);
GUEST_FUNCTION_STUB(sub_82BD97E8);
GUEST_FUNCTION_STUB(sub_82BDD370); // SetGammaRamp
GUEST_FUNCTION_STUB(sub_82BE05B8);
GUEST_FUNCTION_STUB(sub_82BE9C98);
GUEST_FUNCTION_STUB(sub_82BEA308);
GUEST_FUNCTION_STUB(sub_82CD5D68);
GUEST_FUNCTION_STUB(sub_82BE9B28);
GUEST_FUNCTION_STUB(sub_82BEA018);
GUEST_FUNCTION_STUB(sub_82BEA7C0);
GUEST_FUNCTION_STUB(sub_82BFFF88); // D3DXFilterTexture
GUEST_FUNCTION_STUB(sub_82BD96D0);
