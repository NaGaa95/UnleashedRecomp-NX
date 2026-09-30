# Switch performance changes

Performance work for the Switch build that does not change what is drawn: no graphics setting, resolution or shader
output changes, and the game behaves the same. Nearly every change has a `[Switch]` key in `config.toml` that turns it
off. [SWITCH-PERFORMANCE-AUDIT.md](SWITCH-PERFORMANCE-AUDIT.md) says, change by change, why the image stays the same.
[SWITCH-MESA.md](SWITCH-MESA.md) covers the driver.

Every round is built on the PC and measured on the console with a test set: an NRO plus one `config.toml` per
configuration, run at the same places. Contents:

- [Results by test set](#results-by-test-set)
- [Building](#building)
- [What changed](#what-changed): every change, its switch and its default
- The rounds, in order ([Round 4](#round-4-shader-specialization-streaming-buffers-driver-linking) to
  [Round 10](#round-10-dead-copies-more-sinking-pipelined-present-pgo-test-set-unleashed-test-round9)); the first
  rounds are described under [Notes on each change](#notes-on-each-change)
- [Final build](#final-build): the defaults it ships with, the docked and handheld sizes, logging and crash reports
- [Considered and not done](#considered-and-not-done), [Further opportunities](#further-opportunities-not-implemented),
  [How to measure](#how-to-measure)

## Results by test set

The test set `unleashed-test-roundN` measures the build of round N + 1 in this document. Its log lines say
"Switch round N".

"1080p hub" is `ResolutionScale = 1.5`, standing at the same Apotos hub spot. There the GPU limits the frame, so
the GPU frame time is the figure to compare. The CPU runs render at 858x482 (`ResolutionScale = 0.67`); from test set
8 on they also have the CPU clocked down to 1020 MHz with the GPU at its maximum, so that the game's main thread
limits the frame.

| Test set | Build | 1080p hub | CPU-bound run | Notes |
|---|---|---|---|---|
| `unleashed-test-round4` | round 5 | GPU frame 29.5 ms (~34 FPS) | — | resolve copies ~5.8 ms of the frame |
| `unleashed-test-round5` | round 6 | 26.0 ms (38.3 FPS); 28.9 ms without hand-over at draws | 480p: main thread 70-97 % busy | 24 of 26 resolve copies became hand-overs; no audio gaps |
| `unleashed-test-round6` | round 7 | 25.45 ms (39.3 FPS) | 480p: 54 FPS (round 5: 48) | 37 barrier batches per frame (60 before) |
| `unleashed-test-round7` | round 8 | 42 FPS | 480p: 59.3 FPS (60 FPS cap); 56.3 without the round 8 CPU changes | the 480p runs froze: the CPU sampler held a lock during SD writes (fixed in round 9) |
| `unleashed-test-round8` | round 9 | 41 FPS; 42 with the quad sinking off | 1020 MHz: 51 FPS; 53.3 with the guest-kernel sync changes | game thread 17.64 ms of work per frame, 1.06 ms in Present |
| `unleashed-test-round9` | round 10 | 44.0 FPS; 42.4 without the four round 10 renderer changes | 1020 MHz: 53.6 FPS; 52 with the present on the main thread | PGO + LTO + `-O2` + `-fipa-pta` build. Long play with the four renderer changes on lost the GPU twice, so they are off in the [final build](#final-build) |

## Building

`tools/build-switch.sh` builds the NRO (see its header for the tools it needs). Its performance options:

| Variable | Default | Effect |
|---|---|---|
| `SWITCH_DIRECT_CALLS` | 1 | calls between recompiled functions made direct, so GCC can inline them |
| `SWITCH_LTO`, `SWITCH_LTO_JOBS` | 0, 2 | link-time optimisation (test builds since round 7 use it) |
| `SWITCH_PGO`, `SWITCH_PGO_DIR` | empty, `pgo/` | `generate` for an instrumented build (no LTO), `use` to build with its profile |
| `SWITCH_O2` | 0 | all code with `-O2` instead of `-O3` (round 10) |
| `SWITCH_IPA_PTA` | 0 | `-fipa-pta` at compile and LTO link (round 10) |
| `SWITCH_RECOMP_O2` | 0 | `-O2` for the recompiled code only |
| `SWITCH_CLASSIC_CODEGEN` | 0 | the pre-round-8 code generation, for A/B builds |
| `SWITCH_BUILD_ID` | date and options | printed as the first line of `stderr.log` |

The script resets the submodules and applies `patches/*.patch`, so changes to XenosRecomp, XenonRecomp or plume
have to be written back into their patch files before a build. The shader cache is regenerated whenever the
translator or `shader_common.h` changes.

## What changed

| Change | Files | Affects | Default | Turn off |
|---|---|---|---|---|
| Shader constants through dynamic uniform buffers instead of 64-bit pointers | `patches/XenosRecomp-switch-perf.patch`, `patches/plume-switch-perf.patch`, `gpu/video.cpp` | GPU | on | `[Switch] SwitchConstantsUBO = false` |
| `max(a, a)` emitted as `a` (Xenos "move") | `patches/XenosRecomp-switch-perf.patch` | GPU | on | revert the patch |
| Depth targets without `TRANSFER_DST` (ZCULL eligibility) | `patches/plume-switch-perf.patch` | GPU, needs a Mesa with ZCULL | on | — |
| Persistent `VkPipelineCache` (`cache/pipelines.bin`) | `patches/plume-switch-perf.patch`, `gpu/video.cpp` | load times, hitches | on | `[Switch] SwitchPipelineCache = false` |
| Direct calls between recompiled functions | `tools/switch-direct-calls.py`, `tools/build-switch.sh` | CPU (all guest threads) | on | `SWITCH_DIRECT_CALLS=0` |
| Link-time optimisation | `CMakeLists.txt`, `tools/build-switch.sh` | CPU | **off** | `SWITCH_LTO=1` to enable |
| Render thread at priority 0x2D | `gpu/video.cpp` | frame pacing, CPU | on | — |
| Render queue spins ~1,000 times instead of 10,000 before sleeping | `gpu/video.cpp` | CPU | on | — |
| Redundant shader constant updates skipped | `gpu/video.cpp` | CPU (render thread), memory bandwidth | on | — |
| Render-thread buffer unlocks staged through the upload ring | `gpu/video.cpp` | CPU (render thread) | on | — |
| GPU timer corrected (NVK timestamps are 1.627 ns, not 1 ns) | `gpu/video.cpp` | measurement only | on | — |
| Stock 460.8 MHz handheld GPU profile (memory stays at 1331.2 MHz) | `os/switch/perf_switch.cpp` | GPU in handheld | **off** | `[Switch] SwitchHandheldGpuBoost = true` to enable |
| FPS, frame times and resolution for Status Monitor / SaltyNX overlays | `os/switch/overlay_switch.cpp`, `gpu/video.cpp` | overlays | on | `[Switch] SwitchOverlayFps = false` |
| Requests the patched driver's self-checking draw-path fast paths (it runs them only for games that ask; see SWITCH-MESA.md) | `gpu/video.cpp` | CPU (render thread), needs that driver | **off** since round 7 | `[Switch] SwitchNvkFastPaths = true` |
| Profile-guided optimisation (instrumented build + profile use) | `UnleashedRecomp/CMakeLists.txt`, `os/switch/pgo_switch.cpp`, `tools/build-switch.sh` | CPU | **off** | `SWITCH_PGO=generate` / `use` |
| `-O2` instead of `-O3` for the recompiled code (A/B experiment; round 10 builds all code with `-O2`, `SWITCH_O2`) | `UnleashedRecomp/CMakeLists.txt` | CPU, instruction cache | **off** | `SWITCH_RECOMP_O2=1` |
| Built-in profiler shown without F1 | `gpu/video.cpp` | measurement | off | `[Switch] SwitchShowProfiler = true` |
| plume: redundant binds filtered (pipeline, index/vertex buffers, viewport, scissor, depth bias) | `patches/plume-switch-perf.patch` | CPU (render thread) | on | — |
| plume: host-visible buffers persistently mapped | `patches/plume-switch-perf.patch` | CPU | on | — |
| Push-constant pointers only pushed for shaders that still read them | `gpu/video.cpp`, `gpu/video.h` | CPU (render thread) | on with the UBO path | `SwitchConstantsUBO = false` |
| Consecutive instructions under the same predicate share one `if (p0)` block | `patches/XenosRecomp-switch-perf.patch` | GPU | on | revert the patch |
| Texcoord swap test with a 32-bit mask instead of a 64-bit shift | `patches/XenosRecomp-switch-perf.patch` | GPU (vertex) | on | revert the patch |
| Depth-only draws built without a fragment stage when the pixel shader has no effect | `gpu/video.cpp`, `gpu/video.h` | GPU | on | `[Switch] SwitchDepthOnlyWithoutPixelShader = false` |
| plume: barrier access masks derived from layouts | `patches/plume-switch-perf.patch`, `gpu/video.cpp` | GPU | on | `[Switch] SwitchPreciseBarriers = false` |
| 2D texture sizes from the shared constants instead of `GetDimensions()` queries | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp` | GPU | on | `[Switch] SwitchTextureSizeConstants = false` |
| D3D-thread render commands sent in one batch per draw | `gpu/video.cpp` | CPU (game thread) | on | `[Switch] SwitchBatchRenderCommands = false` |
| Shader constants kept in host byte order (swap on set, copy on upload) | `gpu/video.cpp` | CPU (render thread) | on | — |
| `-fno-math-errno -fno-trapping-math` for the recompiled code | `UnleashedRecompLib/CMakeLists.txt` | CPU (game code) | on | remove the block |
| Driver link for current mesa-switch builds (optional expat/drm_nouveau, link group, weak libelf stubs) | `patches/plume-switch-perf.patch`, `os/switch/nvk_switch_stubs.c`, `tools/build-switch.sh` | build | on | — |
| stderr written to `sdmc:/switch/UnleashedRecomp/stderr.log` | `main.cpp` | diagnostics | on | — |
| Driver environment variables from the config | `gpu/video.cpp` | testing | empty | `[Switch] SwitchMesaEnvironment` |
| Vertex shaders skip the reverse-Z pass when reverse Z is off | `patches/XenosRecomp-switch-perf.patch` | GPU (vertex) | on | revert the patch |
| Per-pass GPU profiler | `gpu/video.cpp` | measurement | off | `[Switch] SwitchGpuPassProfiler = true` |
| Vertex outputs the pixel shader never reads are not computed (all but the position without a pixel shader) | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp` | GPU (vertex) | on | `[Switch] SwitchTrimVertexOutputs = false` |
| Per-draw GPU profiler (shader pairs of the most expensive passes) | `gpu/video.cpp` | measurement | off | `[Switch] SwitchGpuDrawProfiler = true` |
| Texture descriptor high-water mark on stderr | `gpu/video.cpp` | measurement | on | — |
| `switch-direct-calls.py` writes only files whose text changes | `tools/switch-direct-calls.py` | build time | on | — |
| 16,384-entry texture descriptor heap (read by NVK from a constant bank) | `gpu/video.cpp` | GPU | on | `[Switch] SwitchCompactTextureHeap = false` |
| Shadow-map filters (4 or 9 point fetches) read with 1 or 4 gathers where exact | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp` | GPU (pixel) | on | `[Switch] SwitchShadowGather = false` |
| Skinning branch specialized per pipeline (variant built in the background) | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp` | GPU (vertex) | on | `[Switch] SwitchBonesSpecialization = false` |
| a0-indexed constant arrays (bone palettes) read through memory instead of the constant bank | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp` | GPU (vertex), A/B | **off** | `[Switch] SwitchIndexedConstantsFromMemory = true` to enable |
| Colour channels nothing writes or blends are not computed | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp` | GPU (pixel) | on | `[Switch] SwitchTrimPixelOutputs = false` |
| Unread vertex outputs per component (translator constants 1-3; driver link) | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp`, Mesa `nvk_shader.c` | GPU (vertex) | on | `SwitchTrimVertexOutputs` / `SwitchLinkVaryings` |
| Vertex/index buffers rewritten mid-frame drawn from the upload ring, written back once per frame | `gpu/video.cpp`, `gpu/video.h` | GPU (fewer pipeline drains) | on | `[Switch] SwitchStreamingBuffers = false` |
| Hand-written shaders (CSD UI, blur, motion blur, blend) read constants from the constant bank; CSD filter takes its texture size from the constants | `gpu/shader/*.hlsl`, `gpu/video.cpp` | GPU | on with the UBO path | `SwitchConstantsUBO = false` |
| ZCULL in `greater` mode (reverse Z) | `gpu/video.cpp` | GPU | on | `[Switch] SwitchZcullGreater = false` |
| Profiler reports written to the SD card from a background thread, in one write | `gpu/video.cpp` | stutter with the profilers on | on | — |
| Mesa's own disk cache off whenever the pipeline cache is on | `gpu/video.cpp` | SD writes, stale shaders | on | `SwitchPipelineCache = false` |
| Submodule patches re-applied only when they changed; ppc/ no longer regenerated by CMake | `tools/build-switch.sh`, `UnleashedRecompLib/CMakeLists.txt` | build time; direct calls now actually built in | on | — |
| Resolve copies made only when needed: a depth copy waits while draws only test depth; a clear makes only the copies of what it clears | `gpu/video.cpp` | GPU | on | `[Switch] SwitchLazyResolves = false` |
| A render target resolved into one texture and then cleared gives the texture its image instead of being copied | `gpu/video.cpp`, `gpu/video.h` | GPU | on | `[Switch] SwitchResolveHandOver = false` |
| Colour resolves still pending at the end of a frame stay pending (no copy at every frame's end) | `gpu/video.cpp` | GPU | on | `[Switch] SwitchKeepResolvesPending = false` |
| Pixel shader constants not uploaded for draws without a fragment stage | `gpu/video.cpp` | CPU (render thread) | on | `[Switch] SwitchSkipUnusedPixelConstants = false` |
| Skinning variants (`SwitchBonesSpecialization`) also built during loading screens | `gpu/video.cpp` | fewer background compiles after a load | on | `[Switch] SwitchPrecompileBonesVariants = false` |
| Pipeline cache saved when a loading screen ends only (no save every minute during play) | `gpu/video.cpp` | hitches | on | `[Switch] SwitchPipelineCacheSaveDuringPlay = true` |
| The velocity-map vertex shader's if/else emitted as if/else instead of a `switch (pc)` state machine | `patches/XenosRecomp-switch-perf.patch` | GPU (vertex) | on | revert the patch |
| Profiler: resolve copies as passes of their own; resolves, barrier batches and render target changes per frame; game shader names fixed | `gpu/video.cpp` | measurement | off | — |
| Driver: every varying of a pipeline without a fragment shader dropped | Mesa `nvk_shader.c` | GPU (vertex) | on | `NVK_SWITCH_VS_ONLY_VARYINGS=0` |
| Driver (NAK revision 4): operand reuse also on FMNMX/FSET/FSETP/SEL; scheduler latency knobs | Mesa `nak/*.rs`, `nvk_shader.c` | GPU | on / 200 cycles | `NAK_DEBUG=reusebasic`; `NAK_TEX_LATENCY`, `NAK_MEM_LATENCY`, `NAK_ATTR_LATENCY` |
| A render target resolved into one texture and then drawn into (no blending, every channel written, no depth buffer) hands its image over; an 8-bit stencil marks the pixels the draw wrote and only the others are copied afterwards | `gpu/video.cpp`, `patches/plume-switch-perf.patch` (S8_UINT) | GPU (bandwidth) | on | `[Switch] SwitchCoverageHandOver = false` |
| Alpha-tested pixel shaders skip the arithmetic between their alpha output and the alpha test for discarded pixels (646 of 1,096 pixel shaders) | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp` | GPU (pixel) | on | `[Switch] SwitchAlphaTestEarlyOut = false` |
| Shader constant uploads copy only the registers the shaders can read (whole block for relative addressing and hand-written shaders) | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp`, `shader_cache.h` | CPU (render thread) | on | `[Switch] SwitchTrimConstantUploads = false` |
| Game audio through `audout` from the pump thread instead of SDL's lowest-priority audio thread (fixes permanent silence under CPU load) | `apu/driver/sdl2_driver.cpp` | audio, CPU | on | `[Switch] SwitchAudioOut = false` |
| Per-thread CPU use in the pass profiler report; sampling CPU profiler with an offline symbolizer | `os/switch/cpu_profiler_switch.cpp`, `tools/switch-cpu-profile.py` | measurement | per-thread use with the pass profiler; sampler off | `[Switch] SwitchCpuProfiler = true` |
| Hand-over at draws: the stencil buffer cleared once per 255 hand-overs (rolling reference), not before each | `gpu/video.cpp`, `patches/plume-switch-perf.patch` (dynamic stencil reference) | GPU (bandwidth) | on with the hand-over | `[Switch] SwitchCoverageHandOver = false` |
| Hand-over at draws also for blending that ignores the target (UNORM targets) | `gpu/video.cpp` | GPU | on with the hand-over | `[Switch] SwitchCoverageHandOver = false` |
| Shadow-map gathers also when an unrelated fetch writes other components of the same register (9 more shaders) | `patches/XenosRecomp-switch-perf.patch` | GPU (pixel) | on | `[Switch] SwitchShadowGather = false` |
| Pipelines for known-gatherable shadow slots drop the check and the point-fetch fallback | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp` | GPU (pixel) | on | `[Switch] SwitchShadowGatherSpecialization = false` |
| Hand-overs share the draw's barrier batch; a render target with pending resolves moves to the texture layout at the change of target | `gpu/video.cpp` | GPU (fewer waits for idle) | on | `[Switch] SwitchEagerSampleTransitions = false` |
| Guest atomics (lwarx/stwcx.) without a full barrier each, as on PowerPC | `patches/XenonRecomp.patch`, `UnleashedRecompLib/ppc`, `cpu/guest_thread.cpp` | CPU (all game threads) | on | `[Switch] SwitchRelaxedAtomics = false` |
| Game thread copies only the changed shader constant registers | `gpu/video.cpp` | CPU (game thread) | on | `[Switch] SwitchSparseConstantCopies = false` |
| Game thread hands its render commands over every few draws, not after each (fewer render-thread wake-ups) | `gpu/video.cpp` | CPU (game thread) | on | `[Switch] SwitchBatchSeveralDraws = false` |
| Render thread swaps, compares and stores constants in one pass | `gpu/video.cpp` | CPU (render thread) | on | — |
| Recompiled code without frame pointers | `UnleashedRecompLib/CMakeLists.txt` | CPU (all game threads) | on | remove the flag |
| Link-time optimisation for the test NROs | `tools/build-switch.sh` (`SWITCH_LTO=1`) | CPU | on for every test NRO since round 7 | build without `SWITCH_LTO=1` for an A/B |
| Game threads start on the core named by the game's SetThreadIdealProcessor calls | `cpu/guest_thread.cpp` | CPU (scheduling) | **off** (A/B) | `[Switch] SwitchThreadIdealCores = true` |
| Port's full-screen copies draw one triangle (not a second one over half the target) | `gpu/video.cpp` | GPU (bandwidth) | on | `[Switch] SwitchSingleCopyTriangle = false` |
| Hand-over at a draw that provably covers the whole target: no stencil marks, no fix-up (edge pixels copied first) | `gpu/video.cpp`, `patches/XenosRecomp-switch-perf.patch` (`SHADER_FLAG_*`), `shader_cache.h` | GPU | on | `[Switch] SwitchExactCoverage = false` |
| Colour clears such a draw overwrites are not made | `gpu/video.cpp` | GPU (bandwidth) | on | `[Switch] SwitchSkipOverwrittenClears = false` |
| Draws that write no colour and no depth are not sent | `gpu/video.cpp` | GPU, CPU (render thread) | on | `[Switch] SwitchSkipNoOpDraws = false` |
| Alpha-test early-out: shadow gathers and arithmetic that only feed kept pixels move into it (361 shaders; 18 more shaders get the early-out) | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp` | GPU (pixel) | on | `[Switch] SwitchAlphaTestSink = false` |
| Blended pixels that leave a UNORM target unchanged are discarded before the blend | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp` | GPU (bandwidth) | on | `[Switch] SwitchSkipTransparentPixels = false` |
| Guest memory accesses not volatile (loop barriers instead), dead callee-saved stores dropped, FPSCR mode tracked across labels, hot functions in `.text.hot` | `patches/XenonRecomp.patch`, `tools/switch-direct-calls.py`, `UnleashedRecompLib/config/hot_functions.txt` | CPU (all game threads) | on | `SWITCH_CLASSIC_CODEGEN=1` build |
| Render and sampler states the render thread already has are not sent again | `gpu/video.cpp` | CPU (game thread) | on | `[Switch] SwitchSkipRedundantRenderStates` / `SwitchSkipRedundantSamplerStates = false` |
| Render command batches of 256 (sent at 128), through a producer token of their own | `gpu/video.cpp` | CPU (game thread) | on | `[Switch] SwitchLargerCommandBatches` / `SwitchRenderQueueToken = false` |
| `type_info::operator==`, `__RTtypeid` and the D3D constant setters as native code | `misc_impl.cpp`, `main.cpp` | CPU (game thread) | on | `[Switch] SwitchNativeRtti` / `SwitchNativeShaderConstants = false` |
| UI aspect-ratio modifier lookups cached | `patches/aspect_ratio_patches.cpp` | CPU (UI) | on | `[Switch] SwitchModifierCache = false` |
| Render thread on core 1, pipeline compilers on core 2 | `gpu/video.cpp` | CPU (scheduling) | on | `[Switch] SwitchHostThreadCores = false` |
| Render-thread pipeline and sampler lookups cached | `gpu/video.cpp` | CPU (render thread) | on | `[Switch] SwitchPipelineLookupCache` / `SwitchSamplerCache = false` |
| Build ID as the first line of stderr.log | `main.cpp`, `UnleashedRecomp/CMakeLists.txt`, `tools/build-switch.sh` | diagnostics | on | — |
| Alpha-test sinking also for fetches and derivatives, per 2x2 quad whose pixels are all discarded (round 9) | `patches/XenosRecomp-switch-perf.patch`, `gpu/video.cpp` | GPU (pixel) | **off** since round 10 (slower) | `[Switch] SwitchAlphaTestQuadSink = true` to enable |
| Draws that copy a target's own pending resolve back into it (EDRAM restores) skipped (round 9) | `patches/XenosRecomp-switch-perf.patch` (`SHADER_FLAG_*`), `gpu/video.cpp` | GPU | on | `[Switch] SwitchSkipRestoreDraws = false` |
| The game's LZX decompression as native code (round 9) | `misc_impl.cpp`, `os/switch/lzx_switch.cpp` | CPU (loading, streaming) | on | `[Switch] SwitchNativeDecompress = false`; `SwitchVerifyNativeDecompress = true` compares |
| Guest critical sections skip the wake-up call when nobody waits (round 9) | `kernel/imports.cpp` | CPU (all game threads) | **off** (audio test pending) | `[Switch] SwitchFastCriticalSections = true` to enable |
| Guest events and semaphores skip their wake-up calls when nobody waits (round 9) | `kernel/imports.cpp` | CPU (all game threads) | **off** (audio test pending) | `[Switch] SwitchFastEvents = true` to enable |
| Command batches handed to the render thread as their buffer, not copied (round 9) | `gpu/video.cpp` | CPU (game thread) | on | `[Switch] SwitchZeroCopyBatches = false` |
| Stall watchdog ("[stall]", "[hitch]" lines) (round 9) | `os/switch/stall_watch_switch.cpp` | diagnostics | 1 s | `[Switch] SwitchStallWatchSeconds = 0` |
| Frame log: one frame's GPU work, in order, once a minute (round 9) | `gpu/video.cpp` | diagnostics | off | `[Switch] SwitchFrameLog = true` |
| Pending resolves whose texture is resolved into again before any read are dropped, not copied (round 10) | `gpu/video.cpp` | GPU (bandwidth) | on | `[Switch] SwitchSkipDeadCopies = false` |
| More of each alpha-tested shader after the per-pixel test: shared temporaries for shadow filters, per-component statements, movable light loops (round 10) | `patches/XenosRecomp-switch-perf.patch` | GPU (pixel) | on with the sinking | `[Switch] SwitchAlphaTestSink = false` |
| Depth clears the next draw overwrites entirely not made; others made in the draw's pass (round 10) | `gpu/video.cpp` | GPU | on | `[Switch] SwitchSkipOverwrittenDepthClears = false` |
| Waiting colour clears made in the pass that draws into their target (round 10) | `gpu/video.cpp` | GPU (fewer passes) | on | `[Switch] SwitchCarryClears = false` |
| Depth buffers with pending resolves ready for sampling at the change of target (round 10) | `gpu/video.cpp` | GPU (no mid-pass barrier) | on | `[Switch] SwitchEagerDepthTransitions = false` |
| Submit, present, frame fence and next image on the render thread (round 10) | `gpu/video.cpp` | CPU (game thread) | **off** (test configs on) | `[Switch] SwitchPresentOnRenderThread = true` to enable |
| Batches of up to 512 commands while the render thread waits (round 10) | `gpu/video.cpp` | CPU (game thread) | on | `[Switch] SwitchIdleRenderThreadBatches = false` |
| Render thread priority (round 10) | `gpu/video.cpp` | CPU (scheduling) | 0x2D (45) | `[Switch] SwitchRenderThreadPriority` |
| Guest spin locks spin ~2 µs before sleeping (round 10) | `kernel/imports.cpp` | CPU (all game threads) | **off** (audio test pending) | `[Switch] SwitchGuestSpinBeforeSleep = true` to enable |
| Thread-local reads removed from hot paths: D3D-thread test by register, critical sections, RTTI and UI caches (round 10) | `gpu/video.cpp`, `kernel/imports.cpp`, `misc_impl.cpp`, `patches/aspect_ratio_patches.cpp` | CPU (game thread) | on | — |
| Pipeline cache keyed by the driver build, so it survives rebuilds of the rest of the NRO (round 10) | `gpu/video.cpp`, `patches/plume-switch-perf.patch`, `UnleashedRecomp/CMakeLists.txt` | loading, first use | on | `SwitchPipelineCache = false` |
| PGO, LTO, `-O2` and `-fipa-pta` together (round 10) | `CMakeLists.txt`, `tools/build-switch.sh` | CPU | the round 10 test NRO | `SWITCH_PGO`, `SWITCH_LTO`, `SWITCH_O2`, `SWITCH_IPA_PTA` |

The constant-buffer path needs a shader cache generated by the patched XenosRecomp. `tools/build-switch.sh`
now regenerates the cache when the translator inputs change (`shader_cache.cpp.translator` stamp). With an
older cache, or with a prebuilt `SWITCH_SHADER_CACHE_OBJECT`, the path is simply inactive.

### Notes on each change

**Constants through dynamic uniform buffers.** The translated shaders read every constant, texture index and
sampler index with `vk::RawBufferLoad` through pointers pushed as push constants. NAK compiles those to global
memory loads. NVK promotes dynamic uniform buffers to the hardware constant banks, also on Maxwell. The same
upload-buffer bytes are now also bound as set 4 (vertex, pixel and shared blocks), and a specialization bit
picks that path at pipeline creation; the pointer path is compiled out. The bit is ORed in after the per-shader
mask in `SanitizePipelineState`, so D3D12, DXIL linking and the precompiled pipeline list are unaffected.
Push constants are still written, so the hand-written shaders (blur, CSD, motion blur, gamma) keep working
through pointers. Cost: one `vkCmdBindDescriptorSets` with three dynamic offsets whenever a constant block moves.
Frame generation (`FrameGeneration = true`) forces `NVK_DEBUG=no_cbuf`, which disables the constant-bank
promotion and cancels this gain.

**ZCULL.** NVK only gives a depth image a hierarchical-Z plane if its only write usage is depth attachment.
plume created every image with `TRANSFER_DST`. Unleashed never writes depth with transfer operations on Vulkan
(clears use `vkCmdClearAttachments`, depth copies and resolves are shader draws), so it is dropped for depth
targets. The driver side is not in this repository: stock mesa-switch compiles ZCULL out on Horizon, and the
patched driver of [SWITCH-MESA.md](SWITCH-MESA.md) enables it (kernel ZCULL geometry, ZCULL context bound to the 3D
channel). Its log then prints one `[nvk] ZCULL:` line per depth size, with the reason when a depth image gets no
plane. Unleashed uses reverse Z, so the direction is configurable (`SwitchZcullGreater`, see
[Considered and not done](#considered-and-not-done)).

**Pipeline cache.** plume passed `VK_NULL_HANDLE`, so only Mesa's disk cache (`sdmc:/.mesa`) persisted compiled
shaders. The cache is loaded when the device is created and saved from a background thread when a loading
screen ends and at most once a minute (closing from HOME kills the process, so there is no save at exit). It is
invalidated when the NRO changes (the Mesa driver is linked into it, and on Horizon its cache UUID does not
change between driver builds). Once `pipelines.bin` exists, `MESA_SHADER_CACHE_DISABLE=true` stops the
duplicate Mesa disk cache and its writer threads. The cache stays in memory while the game runs.

**Direct calls.** XenonRecomp emits every function as a `weak, noinline` alias so hooks can replace it, and
every call site goes through that alias. GCC cannot inline any of them. The pass calls `__imp__sub_X` directly
when `sub_X` has no hook (an address counts as hooked if it appears anywhere in `UnleashedRecomp/` or
`UnleashedRecompLib/config/`). It is idempotent, and the build checks the final ELF: every strong `sub_X` is a
hook and must not have been called directly.

**LTO.** Off by default in the script because the link of the whole recompiled game needs a lot of build RAM; it
uses `-flto-partition=balanced` with `SWITCH_LTO_JOBS` parallel jobs. Both CMake projects pass
`-fno-strict-aliasing` (xxHash, among others, relies on it). The LTO link used to stop at lsfg-vk's `get_mpa()`, which on
Switch returned `vkGetInstanceProcAddr`: in this binary that name is volk's function pointer variable, not a
function (a non-LTO build would have jumped into data had lsfg-vk created its own instance). It now returns the
driver's `vk_icdGetInstanceProcAddr`, as plume's own fallback does.

**Thread priorities.** libnx starts every pthread (including `std::thread` and all guest threads) at 0x3B, the
only time-sliced band. The render thread now runs at 0x2D, just below the main/present thread (0x2C).
Because 0x2D is not time-sliced, the render queue's spin before sleeping was
cut from 10,000 to 1,000 iterations. `KeSetBasePriorityThread` still maps guest priorities around 0x2C,
while the actual default is 0x3B, so any call lifts a guest thread out of the time-sliced band; that mapping
was left alone because the audio priorities look deliberately tuned. Try it as a separate A/B test.

**Status Monitor / SaltyNX FPS.** Status Monitor Overlay (and forks such as Horizon OC Monitor) does not
count frames itself: it reads an NX-FPS block from SaltyNX's shared memory, which the NX-FPS plugin fills by
hooking `nvnQueuePresentTexture`, `eglSwapBuffers` or `vkQueuePresentKHR` in the SDK. This NRO links its own
Vulkan driver, so there is nothing to hook and the overlay showed no FPS. The game now writes that block
itself:

- A background thread connects to SaltyNX's `SaltySD` port once per second until it gets the shared page,
  allocates the 174-byte `NxFpsSharedBlock` (magic `0x465053`) or reuses one already there, and releases the
  port session at once (the port accepts a single session, which the overlay also needs).
- On every present the game answers the overlay's handshakes (it clears `pluginActive` and waits 100 ms; it
  writes `0xFFFF` into the resolution calls) and fills `FPSticks[]`, from which the overlay averages FPS.
- The FPS row is the game's rendered frames per second; the RES row is the internal render resolution.
- A process started through hbloader or a forwarder may only have one port session, already used for `sm:`.
  The code raises the process session limit when the loader allows `svcSetResourceLimitLimitValue`, and
  otherwise releases `sm:` for a few milliseconds (not during the first seconds, then once a minute).
- Needs SaltyNX installed. Without it nothing happens. Opening the overlay late, or SaltyNX starting after the
  game, is handled: the block is re-attached within a second. Look for `[overlay]` lines on stderr.

**NVK fast paths** (off by default since round 7). The patched driver of [SWITCH-MESA.md](SWITCH-MESA.md)
contains draw-path improvements that an application requests through weak symbols (`nvk_switch_set4`,
`nvk_switch_dibujo`): set 4 written as a delta against the previous draw, each draw command emitted in one go,
constant-buffer rebinding without repeated work, dynamic state by dirty groups, and pipeline prefetch. Set 4 here
has exactly the layout they optimise (three dynamic uniform buffers: vertex, pixel, shared); the fast set 4 path
itself (improvement 3) stays off. Every path compares itself with the original one on sampled draws and switches
itself off on a difference, which the game logs once. With any other driver the symbols are absent and nothing
happens; the log line `NVK fast paths: ... (set 4 contract found/absent ...)` says which. The round 4 test set
measured no difference at the hub, so the game no longer asks for them unless `SwitchNvkFastPaths = true`.

**PGO.** Two builds from the same build folder, without regenerating code or editing sources in between:

1. `SWITCH_PGO=generate tools/build-switch.sh` (LTO off). Play for as long as possible: title screen, menus,
   day and night stages, hub worlds, cutscenes, the werehog. Every 3 minutes the game writes `.gcda` files to
   `sdmc:/switch/UnleashedRecomp/pgo/` (the timings go to `dumps.txt` there).
2. Copy those files into `pgo/` in the repository (or `SWITCH_PGO_DIR`), then
   `SWITCH_PGO=use SWITCH_LTO=1 tools/build-switch.sh`.

The profile only applies to the same generated code, compiled at the same `-O` level: collect it again after
regenerating the PPC sources, after changing the direct-calls hook list, and with the same `SWITCH_O2` setting as the
build that uses it. Functions whose control flow does not match their profile are built without one; the build
log lists them (`-Wcoverage-mismatch` warnings, "does not match its profile data").

**-O2.** `-O3` grows the recompiled code (more inlining, unrolling and vectorisation), while the Cortex-A57 has a
48 KB instruction cache. `SWITCH_RECOMP_O2=1` builds only the recompiled code with `-O2`; since round 10,
`SWITCH_O2=1` builds all code with it, together with PGO, which turns the `-O3` inlining and loop work back on
where the profile says the code is hot.

**plume audit.** The render thread spends its time in plume's Vulkan backend, so it was reviewed call by call:

- *Redundant binds.* plume forwarded every bind to the driver. The game marks the pipeline dirty when any
  render state changes, and states that cancel out (or that `SanitizePipelineState` removes) end in the same
  `VkPipeline`. On NVK, binding a pipeline copies and re-emits its whole state even when it is the one already
  bound. The command list now
  remembers what is bound and skips identical pipeline, index buffer, vertex buffer, viewport, scissor and depth
  bias calls. Binding a pipeline whose depth bias is static invalidates the remembered bias, as Vulkan requires.
  Everything is reset in `begin()`.
- *Persistent mapping.* `map()`/`unmap()` went through `vmaMapMemory`/`vmaUnmapMemory`, which map and unmap
  the whole memory block when its reference count goes to zero. Upload, read-back and GPU-upload buffers are now
  created mapped, so `map()` returns a pointer (vertex and index buffers live in GPU-upload memory on the Switch,
  and are unlocked from loading threads).
- *TLS in hot paths.* `barriers()` and `setVertexBuffers()` used `thread_local` vectors; libnx builds use
  software TLS (`-mtp=soft`), so each access is a function call. They use per-command-list storage now.
- *Push constants.* With the uniform-buffer path, the translated shaders never read the three push-constant
  pointers, yet every draw that changed a constant block pushed one (each push dirties NVK's root table, which is
  re-emitted before the draw). Pointers are now pushed lazily, only before draws whose shaders still read them:
  the hand-written shaders, or shaders from a cache without the uniform-buffer path. Which shaders qualify is
  decided from their SPIR-V (a `DescriptorSet 4` decoration, or no pointer capability at all); anything
  unknown keeps receiving the pointers.

Found and not changed at first:

- *Barriers are maximal.* Every barrier used `MEMORY_READ | MEMORY_WRITE` access masks and accumulated source
  stages. NVK turns that into a shader-cache flush plus invalidation of the texture, constant, shader-data and
  MME caches on every barrier. Masks derived from the old and new layouts keep the wait-for-idle that real
  dependencies need but drop the extra flush and invalidations. (Done later: precise barriers, below.)
- *Buffer updates mid-frame.* Each render-thread buffer update recorded two barriers, i.e. two pipeline drains.
  (Done in round 4: streaming buffers, which draw the new contents from the upload ring and write them back once
  per frame.)
- *Clears inside LOAD render passes.* Clears use `vkCmdClearAttachments` in render passes created with
  `LOAD_OP_LOAD`. Once ZCULL works, starting depth passes with `LOAD_OP_CLEAR` may let NVK initialise ZCULL
  directly; measure before changing it.
- *Read-back memory.* On NVK/Horizon the read-back heap lands in the cached, non-coherent memory type and plume
  never invalidates it before the CPU reads; a correctness risk, not a performance one.
- Submission (one submit per frame, one fence wait and reset per frame) and render passes (fixed layouts, no
  implicit transitions) are already lean.

**Predicate blocks.** XenosRecomp emitted one `if (p0) { }` per predicated instruction. NAK flattens almost no
branches on the current driver (its `peephole_select` limit is 0), so each became a real branch, and texture
fetches in separate blocks could not be overlapped: the nine fetches of a 3x3 shadow filter could pay the texture
latency nine times. Consecutive instructions with the same predicate now share one block. A block is
closed before an unpredicated instruction or one with the other condition, right after any instruction that
writes `p0`, and at the end of each exec clause, so every instruction still runs under exactly the same value of
`p0`.

**Depth-only draws.** When a draw has no colour target (colour writes off), no alpha test and no alpha to
coverage, its pixel shader can only matter through a depth write, a sample-mask write or a kill. Each translated
pixel shader's SPIR-V is checked once: no `FragDepth`/`SampleMask` output, no image writes or atomics, and no kill
beyond the single alpha-test `clip` XenosRecomp adds. Such pipelines are built without a fragment stage, so shadow
map and other depth-only passes skip fragment shading entirely; the depth they produce is the same. The checks
were tested on DXC output (a depth-writing shader and a shader with two kills are rejected).

**Precise barriers.** plume declared `MEMORY_READ | MEMORY_WRITE` on both sides of every barrier. On NVK that means
a shader-cache flush plus invalidation of the texture, constant, shader-data and MME caches for every barrier,
including transitions into render-target or copy-destination layouts that need none of it. Access masks now
follow the layouts (the writes possible in the old layout, the accesses possible in the new one) and the buffer
accesses the barrier declares; stage masks are unchanged, so every wait-for-idle that a real dependency needs is
still there. When the barrier's stages cannot perform the derived access, the barrier keeps the old conservative
masks. A wrong mask would corrupt rendering, which is why it has its own switch; validating it on PC with the
Vulkan synchronization validation layer is a good idea if you build a PC version.

**Texture sizes.** Offset texture fetches (`tfetch2D` with an offset), bilinear-weight fetches, the bicubic GI
filter and the alpha-to-coverage mip estimate asked the texture for its size with `GetDimensions()`, a
texture-unit query per use. The
renderer now records the size of every texture it writes into the descriptor heap (every write goes through
`SetTextureDescriptor`; all views start at mip 0, so that size is exactly what `GetDimensions()` returns) and
writes each 2D slot's size into the shared constants next to its descriptor index. The shaders read it there and
do the same division or multiplication as before, so results are bit-identical. For a console check,
`[Switch] SwitchVerifyTextureSizes = true` keeps the queries and draws magenta on any 2D sample whose constant
size differs; one session through menus, stages and hubs without magenta confirms the table.

**Batched render commands.** Every D3D call of the game (`SetTexture`, `SetRenderState`, `SetStreamSource`,
`SetViewport`...) used to be its own enqueue into the render queue. The render thread usually goes to sleep
between draws (it catches up faster than the game produces), so each of those enqueues could mean waking it: a
kernel signal on the game's D3D thread, which is the one the frame waits for, plus a context switch. Commands from
the D3D thread now collect in a batch that goes out with the draw that follows them, in one bulk enqueue.
Commands the game or the frame waits on (texture and buffer unlocks, ImGui, the Present sequence) flush the batch
immediately. The order of everything the D3D thread sends is unchanged; other threads enqueue directly as before.

**Constants in host byte order.** The render thread kept the vertex (4 KB) and pixel (3.5 KB) constant arrays
big-endian and byte-swapped the whole block on every upload. It now swaps only the registers the game sets, when it
sets them, and each upload is a plain copy of the same bytes.

**Math flags for the recompiled code.** XenonRecomp translates `fsqrt` to `sqrt()`. With GCC's default
`-fmath-errno`, every call becomes the square-root instruction plus a check and a branch to libm to set `errno`
for negative inputs. The guest never sees the host's `errno` or floating-point exception flags, so the recompiled
library is built with `-fno-math-errno -fno-trapping-math`: a single `fsqrt`, and floating-point code that GCC may
if-convert. No value changes: `-frounding-math` stays, and nothing that reassociates or contracts differently is
enabled.

**Reverse-Z vertex pass.** Vertex shaders that use the projection matrix run their whole body twice: pass 0
with the reverse-Z projection (only its `oPos` is kept, and only when reverse Z is on) and pass 1 with the normal
one. Without reverse Z (depth ranges that are not reversed, e.g. shadow maps), pass 1 rewrites every output of
pass 0, so pass 0 is now skipped through the specialization constant instead of relying on the driver's compiler
to prove it dead across the shader's branches. Outputs are unchanged, except in the pathological case of an output
that only pass 0 writes, which already carried a value computed with the wrong projection.

**GPU pass profiler.** `[Switch] SwitchGpuPassProfiler = true` writes a GPU timestamp at the start of the frame
and at every framebuffer change, and every 300 frames prints the average GPU time of each render pass to
stderr.log, grouped by target (size, format, samples, depth buffer) and by their order in the frame, with draw
counts. Copies and resolves done inside a pass count towards that pass. This is what the next GPU work should be
aimed with; the timestamps themselves cost a little GPU time, so it is off by default.

**Unread vertex outputs.** NVK compiles every stage on its own: it never removes a vertex shader output that
the pixel shader does not read (RADV, Intel and Asahi link stages; NVK does not), and a pipeline without a
fragment stage still runs the whole vertex shader. XenosRecomp now ends every vertex shader with one guarded
store per interpolator (`if (spec bit 11 + k) o<k> = 0`), wrapped in `#ifdef __spirv__`. When the renderer creates
a pipeline it sets the bits of the outputs nothing reads: every one of them when the pipeline has no pixel shader
(the depth-only pipelines above, i.e. the shadow maps), otherwise those whose location the pixel shader's SPIR-V
never loads. NAK turns the outputs into temporaries before optimising, so the constant store at the exit makes
everything that only fed that output dead code, including its vertex fetches. The location order is the
translator's interpolator order in both stages, which is also how the two stages are linked today. Hand-written
replacement pixel shaders (blur, motion blur, CSD) have no cache entry and keep every output. Nothing reads the
trimmed outputs, so the image is unchanged; the depth-only case is where it should show (shadow passes).

**GPU draw profiler.** `[Switch] SwitchGpuDrawProfiler = true` adds a timestamp after every draw (pools of 128,
so only the unused tail of the last pool is written at the end of the frame) and prints, after each pass
table, the most expensive groups of draws of the eight most expensive passes: GPU time and draws per frame,
vertices, pixel and vertex shader (Xbox 360 shader hash and the start of the SPIR-V's BLAKE3), depth
function and write, blending, alpha test, reverse Z. The BLAKE3 is what the driver prints in its
`NVK_SHADER_STATS` lines, so registers, occupancy and spills of each expensive shader can be looked up.
The flush in front of every timestamp costs GPU time, so totals read higher than with the pass profiler.

**Texture descriptor heap.** Every texture fetch first loads its descriptor. The heap has 65,536 entries
(256 KB with NVK's 4-byte descriptors); NVK reads a descriptor set from a hardware constant bank only when
the binding fits in 64 KB, so the loads go to memory. `Texture descriptors: N in use at most so far` lines
(every 1,024 new entries) give the real high-water mark (below 2,048 in the sessions played so far). With
`SwitchCompactTextureHeap` (on) the heap has 16,384 entries, 64 KB, which NVK reads from a constant bank:
GPU frame 22.65 -> 21.30 ms at the hub. A texture that would find the heap full gets the null texture and a
log line.

**Direct-calls pass.** The pass undid its previous run on disk and then rewrote the same text, so every
build touched all 261 generated files and Ninja recompiled all the recompiled code. It now undoes in memory
and writes only files whose final text differs. Its ELF check after linking looked for symbols named
`sub_X`, but the guest functions are C++ functions with mangled names (`_Z12sub_82DFA2E8R10PPCContextPh`), so
it found no hooks at all and could never fail; it now reads mangled names, fails if it finds no hook, and
reports `191 hooks in the ELF, none of them bypassed` for the current build.

**Audit.** `docs/SWITCH-PERFORMANCE-AUDIT.md` reviews every change: what makes it output-identical, its risks,
and how to check it.

**GPU timer.** NVK on Horizon reports `timestampPeriod = 1 ns`; one tick is about 1.627 ns, measured against
wall-clock time. The profiler's GPU Frame value was about 60 % of the real GPU time.

## Round 4: shader specialization, streaming buffers, driver linking

Everything in this round keeps the image bit for bit; the notes say why for each change. The debug switch
`SwitchVerifyShadowGather` checks the only change whose exactness depends on runtime data.

**Shadow-map gathers.** The shaders filter shadow maps with 4 point fetches at (±0.5, ±0.5) texels or 9 at
(−1..1, −1..1) texels of one coordinate. A gather at the coordinate returns the texels floor(c − 0.5) and
floor(c + 0.5) on each axis, which are exactly the texels of the 2x2 fetches; four gathers half a texel
up-left, up-right, down-left and down-right return the 3x3 block. This holds bit for bit when the offsets
divided by the texture size are exact (power-of-two size), the texture has one mip level and the sampler is
point filtered both ways without anisotropy: the renderer checks all of that per slot (`g_GatherableSlots`,
byte 276 of the shared constants) and the shader keeps the original fetches otherwise. The rewrite is a
pass over the finished HLSL (the fetches of one filter often straddle two exec clauses); fetches of other
textures in between are moved in front of the block when they neither read nor write the filter's
registers. 195 of 233 shaders of the test slice got the rewrite. `SwitchVerifyShadowGather = true` runs both
and draws magenta wherever a texel differs.

**Skinning specialization.** Vertex shaders that branch on `mrgHasBone` (vertex boolean b0; the translator
only specializes when it is b0) get a pipeline variant with the branch resolved: smaller code and register
allocation for one path only. The variant is compiled by the pipeline compiler threads at their low
priority; until it arrives the generic pipeline draws, so no draw waits for a compile. The render thread
chooses the pipeline again whenever b0 changes.

**Indexed constants from memory (A/B, off).** Bone palettes are indexed per vertex; from a constant bank,
different indices within a warp are served one after the other. With the switch on they are read through
the constant pointer (L1) instead. Same bytes either way.

**Unused colour channels.** Channels of `oC0` that the colour write mask drops (or that have no target) and
that neither blending (through a source-alpha factor), the alpha test nor alpha to coverage reads are set
to 0 at the shader's exits, so their math is removed. Nothing can observe them.

**Unused vertex outputs.** The translator's zeroing is now per component (72 bits in specialization
constants 1-3); the renderer still marks whole locations from its SPIR-V analysis, and the driver now does
the per-component part itself when it links the vertex and fragment shaders of a pipeline (see
[SWITCH-MESA.md](SWITCH-MESA.md)).

**Streaming vertex/index buffers.** Unlocking a buffer from the game's D3D thread copied the new contents
into the buffer between draws, with a barrier before and after each copy: a pipeline drain per unlock. Now
the draws after an unlock bind the unlock's copy in the upload ring (untouched until the frame slot comes
around again), and at the end of the frame the last version of each such buffer is copied into place behind
one barrier pair. Each draw reads the bytes it read before. The pass profiler prints how many unlocks per
frame were streamed and how many were copied, and the texture updates between draws.

**Hand-written shaders.** The CSD (UI) shaders, the gaussian blurs, the enhanced motion blur and the colour
blend shader read their constants from the set 4 uniform buffers when the pipeline has the UBO bit, like
the translated shaders (they had used 64-bit pointers), so draws with them also skip the root-address
pushes. The CSD filter takes its texture size from the size table instead of a `GetDimensions()` query.

**Profiler output.** Reports are formatted on the render thread and written by a background thread with a
single `fwrite`; the render thread never waits for the SD card.

**Build.** `apply_patch` in `tools/build-switch.sh` skips a submodule whose working tree is exactly what its
patches produced last time (a stamp in `build/patch-stamps`), so the files keep their timestamps. For the
Switch, CMake no longer regenerates `ppc/` when XenonRecomp or `ppc_context.h` are newer: every patch step
rewrote them, so ppc/ was regenerated on every build, which also threw away the direct-calls rewrite. This
build is the first with direct calls actually compiled in (`365513 calls to 41315 functions made direct`).

### Not done in this round, and why

- **Shadow draws grouped by pipeline.** Reordering the game's draws changes the order of depth writes with
  equal depth and of any draw with side effects; proving it cannot change the shadow map needs the draw
  stream from the console (draw profiler), not guesses.
- **Rewrites of the hottest shaders.** Needs the draw profiler's list of the most expensive shaders (run
  config 2 of the test set) before anything is rewritten by hand.
- **Stall-free texture updates.** Renaming a texture means a new descriptor per update and patching every
  bound slot; the counters in the profiler report now show whether texture updates between draws happen
  often enough to matter.
- **Depth resolve copy removal.** The resolve copies feed passes that sample depth; removing them needs the
  pass list confirmed on hardware. (Round 5: depth copies now wait while draws only test depth; see there.)
- **Reverse-Z vertex dedup by hand.** With reverse Z the vertex body runs twice (the first pass only for
  `oPos`, with the reverse-Z matrix). After unrolling, the driver drops the first pass's other outputs as dead
  stores and CSE merges the math both passes share when it is straight-line code, which the skinning
  specialization now makes it for skinned shaders. What remains differs (the projection matrix).
- **NAK register budget knob / wider operand reuse.** Capping registers trades occupancy for spills to
  local memory, which is very slow on the Tegra X1; extending operand reuse to more instruction forms
  changes instruction encoding and needs its own hardware validation round first.

## Round 5: resolve copies, vertex fetch, early depth test (test set `unleashed-test-round4`)

The round 3 hardware results (test set `unleashed-test-round3`) put the GPU frame at the hub at 17.6-18.1 ms,
with three kinds of time left that no shader change touches: copies from render targets into textures
("resolves"; about 1 ms after the main pass and 1.15 ms at the end of the frame), the shadow passes (2.5 ms for
~285,000 vertices, drawn without a pixel shader, so bound by vertex work) and the velocity-map pass (0.6 ms for
29,000 vertices, whose vertex shader was the only one compiled as a state machine).

**How resolves work here.** A `StretchRect` from a surface into a texture only records the texture as a
destination of the surface; draws that sample the texture sample the surface directly. The copy is made when the
surface is about to change: before a draw into it, before a clear, and for colour surfaces at the end of every
frame (depth resolves still pending then are dropped: they are transient in this game).

**Lazy resolves (`SwitchLazyResolves`).** A draw with depth testing but no depth writes does not change the depth
surface, so its pending copies can wait. They wait only while the surface is still a depth attachment and no
texture slot holds one of its textures (then no layout changes back and forth and nothing samples the surface
while it is bound); otherwise the copy is made exactly as before. The copy made later, or the drop at the end of
the frame, sees the same depth values. A clear now makes only the copies of the surfaces it clears (a depth-only
clear leaves the colour target's copies pending).

**Hand-over at clears (`SwitchResolveHandOver`).** When a surface resolved into exactly one texture is about to be
cleared, the copy would write the surface's texels into the texture and the clear would then overwrite the surface.
Instead the two swap images: the texture takes the surface's image, which holds exactly what the copy would have
written, and the surface takes the texture's old image, which the clear overwrites completely. This is only done
when both images were created alike (same format and size, one mip level, a 2D render/depth-target texture, one
sample; `CreateTexture` records this), never for the back buffer. The texture is then sampled through a view built
from its own view description (component mapping included), as after a copy. Both get new descriptor indices; the
old views, descriptors and framebuffers stay alive until the frame that may still use them has finished, and a
depth surface's cached framebuffers (which name its old image) are retired with them. Framebuffer caches are keyed
by image, so images that moved are dropped from every surface's cache when they are destroyed.

**Resolves kept pending over the end of the frame (`SwitchKeepResolvesPending`).** A colour resolve still pending at
the end of a frame is no longer copied there: the texture keeps sampling the surface, as it did all frame, and the
copy is made when the surface is about to change in a later frame (or handed over, if that is a clear). Before the
game releases a surface its pending copies are made; a released texture is unlinked; a CPU update of a texture whose
copy was kept over a Present makes the copy first, so the update lands on top as before. Excluded: the back buffer
(its image changes every frame) and multisampled surfaces. Depth resolves are dropped at the end of the frame
exactly when they were before.

**Velocity-map vertex shader.** Its control flow is an if/else on `mrgHasBone` (a conditional jump over the "then"
part landing right after an unconditional jump over the "else" part) with two bone loops inside; the unconditional
jump made the translator emit the whole shader as `while (true) switch (pc)`, compiled by NAK to 96-104 registers.
The translator now recognizes that shape (every block and loop must nest; otherwise the state machine is kept) and
emits `if/else`, with the loops as `do/while`, which is what the state machine ran (the body always ran once). The
operations and their order are the same. Two caveats: the compiler may now fuse a multiply and an add that used to
sit in different `case` blocks (at most a last-bit difference, the same kind the round 4 skinning specialization
can cause); and this is the only shader whose output changes: all other shaders translate byte for byte as before.

**Driver: vertex fetch (`NVK_SWITCH_VI_READ_ONLY`).** NVK told the vertex fetch unit to load every attribute of the
pipeline's vertex input. Attributes the bound vertex shader does not read (its SPH input map) are now marked
inactive, so they are not loaded. The shadow passes' vertex shader keeps only the position (plus bone data when
skinned) after the output trimming, while the vertices carry normals, tangents, texture coordinates and colours.

**Driver: early depth test for discarding shaders (`NVK_SWITCH_EARLY_Z_KILL`).** Alpha-tested draws that do not
write depth (transparent foliage, particles) test depth after the shader because the shader can discard. With no
depth writes, no stencil test, no occlusion query and a shader without depth output or stores, testing first keeps
exactly the same fragments, and hidden fragments are not shaded.

**Driver: pipelines without a fragment shader (`NVK_SWITCH_VS_ONLY_VARYINGS`).** `NVK_LINK_VARYINGS` now also covers
them: all generic outputs are dropped. For the translated shaders the renderer's output trimming already does this;
it matters for any pipeline it does not cover.

**Driver: NAK revision 4.** Operand reuse (round 3, validated on the console) also covers FMNMX, FSET, FSETP and SEL
(`NAK_DEBUG=reusebasic` limits it to the round 3 set). The scheduler's assumed latencies for texture fetches, memory
accesses and attribute loads/interpolation (200 cycles each) can be changed with `NAK_TEX_LATENCY`,
`NAK_MEM_LATENCY` and `NAK_ATTR_LATENCY` for A/B runs; they only change instruction order. Every shader is compiled
again once with the new driver.

**Pixel constants, skinning variants, pipeline cache saves.** Draws without a fragment stage (the shadow passes)
leave the pixel shader constants dirty for the next draw that has one instead of uploading 3.5 KB. Loading screens
build the skinning variant each model will use (b0 is set for models with more than one node; a wrong guess only
costs an unused pipeline), so it is ready on the first frame instead of being compiled in the background while
playing. The pipeline cache is saved when a loading screen ends; the save every minute during play (which holds the
driver's cache lock while the render thread may be creating a pipeline) is now opt-in.

**Profiler.** Resolve copies are passes of their own in the pass table (`copies before-draw`, `at-clear`,
`at-present`, `other`), instead of being counted in whatever pass came before them. A second `per frame:` line
counts the copies by trigger and their pixels, the hand-overs, the copies kept over a Present, the depth copies
dropped, the barrier batches and the render target changes. Game shaders whose cache hash has the top bit set were
named `port:XXXX` in the draw table; they now show their hash like the others.

### Not done in this round, and why

- **Varying packing after linking.** NAK already allocates attribute storage per component (input/output maps), so
  packing two vec2 into one slot would not reduce the attribute work; replaced by the vertex fetch change.
- **Copy engine for resolves.** Switching to the copy engine costs a wait for idle on each switch; the copies are
  made on the 3D engine as before.
- **Register budget.** Still not: spills to local memory are very slow on the Tegra X1.

## Round 6: hand-over at draws, alpha-test early-out, audio (test set `unleashed-test-round5`)

**Round 4 test set results (1080p, `ResolutionScale = 1.5`, hub spot).** GPU frame 29.5 ms with everything on
(configs 5-7; the FPS shown, ~34, is 1000 / GPU frame: the spot is GPU-bound), 31.1 ms with the three resolve
changes off (config 4, ~32.2 FPS), 30.1 ms with the draw profiler (config 3). The driver's vertex-fetch trimming and
early depth test (config 5), the driver's draw-path fast paths (config 6) and small-target compression (config 7) made no
measurable difference there (29.5-29.6 ms each). At 1080p the frame was: main pass 14.3-14.8 ms (about 8 ms of it
alpha-tested draws), resolve copies ~5.8 ms (2 x 1.0 ms RGBA16F, 4 x 0.68 ms RGBA8 at 1920x1080, 6 x 0.17 ms at
960x540), full-screen post passes ~4.5 ms, shadow maps ~1.1 ms. The copies run close to the memory bandwidth
(~25-33 GB/s).

**Hand-over at draws (`SwitchCoverageHandOver`).** The 1080p RGBA8 chain resolves a surface into a texture and
then draws a full-screen pass into the same surface, so each pass started with a full copy. When the draw that
forces the copy does not blend, writes every channel of the target, has no depth buffer bound and uses one sample,
and the surface has exactly one pending texture that can take its image (the conditions of the hand-over at
clears), the texture takes the surface's image (the copy's exact result) and the surface takes the texture's old
image. The draw runs with a variant of its pipeline that also writes stencil 1 into an 8-bit stencil buffer of the
surface's size (cleared first; no depth test, so nothing else changes). Right after the draw, the copy shader runs
over the whole surface with the stencil test "equal 0": the pixels the draw did not write (outside its triangles,
viewport or scissor, or discarded by the shader, which on this GPU updates stencil after the shader when it can
discard) get the old contents from the texture's image. Every pixel ends up with what the copy followed by the draw
gave. For a full-screen pass the fix-up is rejected by the stencil test before shading; the cost is about 6 MB of
stencil traffic at 1080p instead of 16.6 MB (RGBA8) or 33 MB (RGBA16F) of copy. The stencil variant of a pipeline is
compiled in the background the first time; until then the copy is made. plume gained `RenderFormat::S8_UINT`
(stencil-only views and barriers), which NVK supports as a depth/stencil attachment from Maxwell B on. The pass
report counts the hand-overs and why the other copies before draws could not be avoided. The RGBA16F copies of the
main pass are followed by depth-tested draws and stay copies.

**Alpha-test early-out (`SwitchAlphaTestEarlyOut`).** NAK lowers discard to demote, so a pixel that fails the alpha
test runs its whole shader. Many of the game's pixel shaders write `oC0.w` early and then compute lighting. The
translator (after the rest of the translation) wraps the straight-line arithmetic between the last write of `oC0.w`
and the alpha test in `if (alpha test off, or early-out off, or !((oC0.w - g_AlphaThreshold) < 0))`: the same
comparison as `clip()`, so NaN alpha still runs it. Only when that region has at least 16 lines, no texture fetch,
no derivative, no loop, no other exit and balanced braces. Kept pixels run exactly the same code; discarded pixels
skip it (whole warps of them skip the work). 646 of the 1,096 pixel shaders qualify, among them the main pass's
most expensive alpha-tested shader (0D4D1534DD857403, 5.2 ms at 1080p). Removing the guard gives back the round 4
translation of every one of them byte for byte; the other 450 pixel and 289 vertex shaders produce the same SPIR-V
as in round 4, and all 646 changed modules pass `spirv-val`.

**Constant upload trimming (`SwitchTrimConstantUploads`).** Each upload copied the whole vertex (4 KB) and pixel
(3.5 KB) constant blocks. The translator now records, per shader, the end of the last float4 constant it declares
(the whole block when any constant is read relative to a0 or aL); unnamed registers are literals in the shader. The
renderer copies only those registers, rounded up to 256 bytes, still reserving the whole block in the upload ring
(the uniform buffer binding covers it), and uploads again when a shader needs more registers than the last upload
held. Hand-written shaders have no cache entry and get the whole block.

**Audio (`SwitchAudioOut`).** The long-standing "audio goes silent for good when the CPU is busy" bug: SDL's Switch
backend plays from its own thread, started with `SDL_THREAD_PRIORITY_TIME_CRITICAL`, which devkitPro's SDL maps to
Horizon priority 0x3B, the lowest. After queuing a buffer, its play loop waits until the buffer is reported
*playing*; a thread starved for longer than one buffer finds it already *done* and waits forever. The pump then saw
the queue full and stopped calling the game's mixer. The pump thread (0x20) now feeds `audout` directly (16
0x1000-byte buffers, 3 blocks kept queued, same downmix and F32-to-S16 conversion as SDL). An underrun is a short gap
counted in the pass report (`audio gaps since start`), and playback continues.

**CPU accounting and sampler.** Registered threads (main, render, audio pump, pipeline compilers, every guest thread,
named by its entry point) report their share of CPU time in the pass report. With `SwitchCpuProfiler = true` a
thread at priority 0x1E pauses each registered thread that has run since the last sample every 2 ms, reads its
program counter and resumes it; every 30 s it writes the 150 hottest 16-byte code lines per thread in one write.
`tools/switch-cpu-profile.py stderr.log` names them from the ELF (recompiled functions as `sub_82XXXXXX`).

### Not done in this round, and why

- **Hand-over at draws with a depth buffer bound** (the main pass's RGBA16F copies): the coverage mark would have to
  live in the game's depth buffer, which is D32F without stencil; a D32F+S8 depth buffer costs 8 bytes per pixel on
  every depth access.
- **Several textures resolved from one surface** (counted as "several textures" in the report): would need images
  shared between textures; waiting for the counters to show whether it happens in the costly passes.
- **Link-time optimisation / PGO builds**: implemented options, but a link of the whole recompiled game needs ~1 hour
  and several GB of free disk on the build PC; to be A/B tested as a separate NRO. (LTO in every test NRO since
  round 7; PGO in round 10.)
- **Guessing CPU hot spots**: the sampler is there so the CPU work targets measured functions.

## Round 7: fewer driver tweaks, more game-side work (test set `unleashed-test-round6`)

**Round 5 test set results (1080p).** GPU frame at the hub 26.0 ms with everything on (38.3-38.4 FPS), 28.9 ms with
the hand-over at draws off (34.5 FPS), 26.1 ms with the constant trimming off, 29.5 ms with the three round 6
renderer changes off (33.9 FPS, the round 4 number). 24 of the 26 resolve copies per frame became hand-overs; the
two left are a colour copy before a blended draw and a depth copy, made back to back. Audio: 0 gaps in every
run. At 480p (the CPU run) the game's main thread was busy 70-97 % of the time, the render thread 25-44 %, the
busiest game worker (82E5C678) 25-39 %. The CPU sampler of that build recorded no addresses: it took the absolute
linker symbol `__start__` (0) as the module base, so every sample fell outside the module; it now uses the
relocated `_start`.

**Driver tweaks undone.** See `SWITCH-MESA.md`: small-target compression, vertex-attribute trimming and the early
depth test for discarding shaders are removed from the driver; its draw-path fast paths run only for games
that ask, and UnleashedRecomp no longer asks by default (a 480p A/B measures them).

**Hand-over at draws without stencil clears.** Each hand-over cleared its 8-bit stencil buffer before the draw.
The draw now writes a reference value that no pixel holds since the last clear (1 to 255, one per hand-over),
and the fix-up copies the pixels that do not hold it; the buffer is cleared only when it is new and every 255
hand-overs. plume gained a dynamic stencil reference for this.

**Hand-over at draws for blending that ignores the target.** Destination factors zero, ADD/SUBTRACT/REV_SUBTRACT
and no source factor that reads the target: the result does not depend on the target, as without blending. Only
UNORM targets (a zero factor times an infinity or NaN of a float target is not zero).

**Shadow gathers in 9 more shaders.** The rewrite rejected a filter when a fetch of another texture between its
taps wrote the same register (e.g. `r1.w` from a specular map next to the shadow tap `r1.x`). It now tracks
components: the other fetch moves in front of the filter when it neither reads a component a tap before it wrote
nor writes one. 874793F8CC9C7EB0 (1.8 ms of the main pass at 1080p in round 4) is one of them.

**Known-gatherable shadow pipelines.** Every rewritten shadow filter keeps a runtime check of `g_GatherableSlots`
and the original point fetches as fallback. The cache entry now lists the slots a shader gathers from; when all
of them are gatherable for the draw, the render thread uses a pipeline built with
`SPEC_CONSTANT_SHADOW_GATHER_KNOWN`, which compiles the check and the fallback out (built in the background; the
generic pipeline draws until then). Any change of the gatherable slots re-selects the pipeline.

**Barriers.** A hand-over no longer flushes a barrier batch of its own before the swap (pending barriers are keyed
by image and stay right). A colour target that the next draw leaves while it has pending resolves moves to the
texture layout in the batch of that change of target, instead of mid-pass when its texture is first sampled. The
report counts the batches still flushed inside a pass.

**Guest atomics.** Every `stwcx.`/`stdcx.` of the game (10,695 sites) was a `__sync` compare-and-swap, which GCC
compiles with a store-release and a full `dmb ish` barrier. PowerPC's `stwcx.` orders no other access; the game's
`sync`, `lwsync` and `eieio` are real fences and stay. `SwitchRelaxedAtomics` makes them relaxed
compare-and-swaps (exclusive load/store only). It changes the memory ordering the game sees back to PowerPC's, so
it has its own switch: a hang, crash or audio problem that goes away with it off points at the game relying on
more ordering than PowerPC gives.

**Constants.** The game's thread copied, for every draw, the span from the first to the last changed constant
group; it now copies each run of changed groups (up to four, else the span). The render thread swaps, compares and
stores in one pass instead of three.

**Hand-over of render commands.** The game's thread gathered its commands per draw and handed each draw's batch to
the render thread (one enqueue per draw). The render thread, busy 25-44 % at 480p, usually sleeps between two draws,
so most hand-overs woke it: a kernel signal on the game's thread, which is the limit when the CPU is. With
`SwitchBatchSeveralDraws` the batch goes out when it holds half its 128 commands (a few draws), at every flush point
(Present, buffer and texture unlocks, ImGui, anything a caller waits for) or when the next draw would not fit.
Commands carry their data (constants and DrawPrimitiveUP vertices are copied) or are flushed, so the order and
content are the same.

**Build.** `-fomit-frame-pointer` for the recompiled code (AArch64 GCC keeps x29 as frame pointer otherwise), and
the NRO is linked with LTO (`SWITCH_LTO=1`), with a non-LTO NRO of the same code for the A/B.

**CPU placement (A/B).** `SetThreadIdealProcessor` was a no-op. With `SwitchThreadIdealCores` a game thread starts
on core n / 2 for the Xbox hardware thread n it asks for, still allowed on every core.

## Round 8: exact full-screen draws, alpha-test sinking, fewer commands (test set `unleashed-test-round7`)

The log lines of this build say "Switch round 7" (the test-set numbering), and the first line of `stderr.log` now
names the build (`Build: round7-<date>-lto, guest code round 7, LTO on`), so a log from another NRO shows at once.

**Round 6 test set results.** The no-LTO logs of that set were written by the round 5 NRO (its format strings, no
"Switch round 6" line, the round-5 pipeline cache), so only the LTO runs measured round 6: at 1080p the hub GPU
frame is 25.45 ms (39.3 FPS) against round 5's 26.07 ms, with 37 barrier batches per frame instead of 60; at 480p
54 FPS against round 5's 48 (round 6's CPU changes and LTO together). The shadow-gather check showed no magenta.

### GPU (image-identical, each with its switch)

1. **One copy triangle** (`SwitchSingleCopyTriangle`). `copy_vs` builds one triangle over the whole target from
   vertices 0-2; the port drew 6 vertices, and vertices 3-5 made a second triangle over half the target again.
   Every shader drawn with it (copies, MSAA resolves, the depth copy, the coverage fix-up, the gamma pass) reads
   its texel at the pixel position (`Load`), so each pixel gets the same value once instead of 1.5 times on
   average: a third less work in the two 1080p RGBA16F copies of the main pass (2.1 ms) and the gamma pass.
2. **Exact full-screen hand-overs** (`SwitchExactCoverage`). 24 of the frame's resolve copies became hand-overs at
   draws (round 6), each with 8-bit stencil marks on the draw and a full-screen fix-up draw. A draw that provably
   writes every pixel needs neither: a DrawPrimitiveUP of one axis-aligned rectangle split on its diagonal (a
   4-vertex strip, fan or quad), through a vertex shader that passes the position through (the translator now
   flags these, `SHADER_FLAG_POSITION_PASS_THROUGH`: 5 post-process vertex shaders), a pixel shader that cannot
   discard (no kill instruction, `SHADER_FLAG_PIXEL_KILL`; no alpha test, alpha to coverage or blend skip; of the
   port's own shaders, the Gaussian and motion blur replacements), inside
   the depth range, not culled, all channels written, no blending that reads the target, no depth buffer. The
   renderer computes the rectangle in framebuffer coordinates exactly as the GPU does (half-pixel offset,
   `-fvk-invert-y`, viewport, scissor); the pixels whose centre lies within 1/64 pixel of an outer edge depend on
   the rasterizer's tie rule, so those (at most two rows or columns per side) get the old contents first with
   scissored copies, and the draw overwrites those it covers. The report counts these hand-overs and why others
   were not proven exact.
3. **Overwritten clears skipped** (`SwitchSkipOverwrittenClears`). A colour clear waits for the next command. If it
   is such a draw into the same target, the clear is not made (only those edge pixels are cleared); any command
   that reads or writes images, binds targets or records GPU work makes it first, exactly as before.
4. **No-op draws skipped** (`SwitchSkipNoOpDraws`). A draw with no colour channel written and no depth written
   changes nothing (the port has no stencil, occlusion queries or shader stores); its state stays for the next draw.
5. **Early-out for late alpha.** The alpha-test early-out (round 6) needed 16 lines of arithmetic after the last
   write of the alpha; shaders that compute the alpha at the end (18 of them) now get it when the sinking below
   fills the branch.
6. **Alpha-test sinking** (`SwitchAlphaTestSink`, `SPEC_CONSTANT_ALPHA_TEST_SINK`). Everything before the early-out
   branch that only feeds the kept pixels' colour moves into it: the shadow gathers (no implicit level of detail,
   so exact in a branch that some pixels of a quad skip) and the arithmetic; fetches with implicit level of detail,
   derivatives and all the alpha, the alpha test and the code after the branch read stay. 361 pixel shaders, among
   them 0D4D1534 (the main pass's alpha-tested group, 4.3-4.7 ms at 1080p).
7. **Transparent pixels skipped** (`SwitchSkipTransparentPixels`, `SPEC_CONSTANT_BLEND_SKIP_ALPHA/ZERO`). A blended
   pixel that the blend leaves unchanged is discarded before the blend: for UNORM targets (sources clamped to
   [0, 1], finite texels), `_ALPHA` when the source alpha is 0 or less with SRC_ALPHA/ZERO colour source factors and
   ONE/INV_SRC_ALPHA destination factors, `_ZERO` when all four outputs are 0 or less with destination factors that
   are then 1; add or reverse subtract; no depth write, no alpha to coverage. In early-out shaders such a pixel also
   skips the rest of its arithmetic. The depth-only analysis (`SpirvRemovableInDepthOnlyPass`) allows these two
   discards (they never run without a colour target): the same 1,085 of 1,096 pixel shaders stay removable.

Checks: all 1,385 SPIR-V modules pass `spirv-val`; shaders without these rewrites are unchanged.

### CPU (14)

The game's main thread was the limit at 480p (97 % busy).

1-4. **Code generation** (XenonRecomp, `SWITCH_CLASSIC_CODEGEN=1` builds the previous code for an A/B NRO):
   - Guest memory accesses are plain instead of volatile (`ppc_context.h`): the compiler can forward stores, drop
     dead loads and pair and schedule accesses. PowerPC orders nothing without `sync`/`lwsync`/`eieio` or a
     reservation, and those stay fences; what volatile also did, re-reading memory on every pass of a wait loop,
     `PPC_LOOP_BARRIER()` does on every backward branch. Memory-mapped accesses stay volatile, and the relaxed
     compare-and-swap of `stwcx.`/`stdcx.` (`SwitchRelaxedAtomics`) is a compiler barrier, so the accesses around
     the game's 10,676 atomics keep their program order as they did when volatile.
   - Callee-saved register stores in prologues are dropped: with non-volatile registers as locals they stored
     fresh zeros that nothing reads.
   - The floating-point mode (FPSCR flush-to-zero, switched between scalar and vector code) is tracked across
     labels, a fixpoint over the branch edges, instead of being unknown after every branch target, where the
     recompiled code set the mode again before its next floating-point instruction.
   - The 300 hottest functions of the round-6 CPU profile (`UnleashedRecompLib/config/hot_functions.txt`) are
     `__attribute__((hot))` and grouped in `.text.hot` (instruction cache and TLB).
5. **Redundant render states** (`SwitchSkipRedundantRenderStates`): the D3D thread remembers the last value it
   sent of each render state and does not send it again (the render thread applies each as an assignment). Not
   `D3DRS_ALPHATESTENABLE` (depends on the render target at the time). A state sent by another thread, or a change
   of D3D thread, makes it forget everything.
6. **Redundant sampler states** (`SwitchSkipRedundantSamplerStates`): the same for the 16 samplers, while the
   anisotropic filtering setting stays the same; the dirty samplers are walked by bit instead of all 16.
7. **Larger command batches** (`SwitchLargerCommandBatches`): up to 256 commands, handed over at 128 (was 128/64).
8. **Render queue token** (`SwitchRenderQueueToken`): the D3D thread's batches go through a moodycamel producer
   token instead of the per-thread implicit producer lookup on every enqueue.
9. **Native RTTI** (`SwitchNativeRtti`): `type_info::operator==` (sub_831B0AB8, 0.7 % of the main thread) and
   `__RTtypeid` (sub_831B2438) as native code; the throwing cases run the recompiled code.
10. **Native shader constants** (`SwitchNativeShaderConstants`): the D3D device's constant setters (sub_82BDFAA0,
    sub_82BDFB80, 1.4 % of the main thread): a copy into the device's registers and an OR into its dirty word,
    instead of emulated unaligned vector loads.
11. **UI modifier cache** (`SwitchModifierCache`): the aspect-ratio lookups (per scene, cast node and cast drawn)
    cached per thread, invalidated whenever a CSD project is loaded or freed.
12. **Host thread cores** (`SwitchHostThreadCores`): the render thread starts on core 1 and the pipeline threads on
    core 2 instead of the process's default core 0, where the game's main thread runs.
13. **Pipeline lookup cache** (`SwitchPipelineLookupCache`): the render thread keeps 64 recent lookups, keyed by the
    raw state, the skinning boolean and the gatherable slots; any new pipeline starts a new generation.
14. **Sampler cache** (`SwitchSamplerCache`): the converted description and descriptor of recent sampler words.

### Not done in this round, and why

- **Eager depth transitions**: the report shows 1.1 barrier batches inside a pass per frame; moving a depth
  surface's transition to the change of target could add transitions for the 4 depth resolves dropped per frame.
  (Done in round 10: the frame log showed the resolves are all sampled.)
- **Blend skip on float targets**: exact only for a source alpha of exactly 0, and even then a destination of -0
  would become +0.
- **Early-out for 874793F8** (2.6 ms): its alpha is known early, but one swizzled statement that feeds the
  environment-map coordinates also computes a lighting value, which pins the lighting before the branch.
  Splitting such statements needs renamed temporaries. (Done in rounds 9 and 10: renaming, then one statement per
  component.)

## Round 9: the stall, quad-uniform sinking, restores, native LZX (test set `unleashed-test-round8`)

The log lines of this build say "Switch round 8" (the test-set numbering).

**Round 7 test set results.** At 1080p the all-on build ran at 42 FPS (round 6: 39.3); turning the round-7 GPU
changes off gave 39.2, their shader part alone 41.4, the round-6 GPU changes off 40. At 480p (60 FPS cap) 59.3 FPS,
56.3 with the round-7 CPU changes off, 54.4 with the round-6 ones off, 59.0 without the host thread cores. The run
labelled "classic" was made with the non-classic NRO (its first log line names the build).

**The freezes at 480p.** Every 480p configuration had the CPU sampler on, and only those froze (no frame for up
to several seconds, audio still playing, GPU idle). The sampler held its thread list's mutex while it wrote its
30-second report to the SD card, line by line, and the game's main thread takes that mutex once per report window
(per-thread CPU use for the GPU report): it waited for the whole write. The sampler now copies its tables under the
mutex and formats and writes them without it, in one write; the main thread only tries the mutex and skips the
figures when it is taken. A stall watchdog (`SwitchStallWatchSeconds`, 1 s by default, 0 = off, after battd_nx's)
writes what every registered thread is doing whenever no frame has been presented for that long ("[stall]" lines:
program counter, return addresses on its stack, CPU time since the last look, the renderer's queues), and sums up
frames over 100 ms once a second ("[hitch]"). The CPU profile also names, per thread, where its system calls come
from ("wait" lines). `tools/switch-cpu-profile.py` names all of these offsets.

### GPU (image-identical)

1. **Quad-uniform alpha-test sinking** (`SwitchAlphaTestQuadSink`, `SPEC_CONSTANT_ALPHA_TEST_QUAD_SINK`). The round-8
   sinking could not move fetches with an implicit level of detail or derivatives past the early-out: in a branch
   that some pixels of a 2x2 quad skip, the level of detail is undefined. Those now move into a block that runs for
   every quad in which at least one pixel passes the early-out's test (`QuadReadAcrossX/Y` of that test; helper
   pixels take part): all four pixels of such a quad run it as before, so levels of detail and derivatives are
   unchanged, and a quad whose four pixels are all discarded skips it, fetches included. 608 pixel shaders; in
   874793F8 (1.7 ms for 3 draws at 1080p) 37 statements stay before the test (the diffuse fetch, the alpha, the
   shadow coordinates), in 0D4D1534 the normal map and the GI fetches move too. These shaders are compiled for
   Vulkan 1.1 (SPIR-V 1.3, quad operations); the others are unchanged.
2. **Renamed values for the sinking.** Before, a statement could not move when a register it wrote was written
   again by one that stays (the scalar result `ps` is rewritten by nearly every scalar instruction). The statements
   before the early-out now give every value a name of its own (`_tN`, written once) and copy back into the
   registers what the rest of the shader reads; only the real flow of values ties statements. Sinking now applies
   to 853 pixel shaders (343 before).
3. **The sinking bits only where the early-out can skip.** Pipelines without alpha test or transparent-pixel skip
   get neither sinking bit, so they run the code in place without the quad variant's two quad shuffles.
4. **Colour restores skipped** (`SwitchSkipRestoreDraws`). On the Xbox 360 a render target lives in EDRAM, which
   other targets reuse: to draw into it again after a resolve, the game first copies the resolved texture back.
   Here a surface keeps its own image, and while a resolve is pending the texture reads that image itself. A draw
   whose pixel shader only copies a texel (`SHADER_FLAG_PIXEL_COPY`, 5 shaders among them 6C50210C, drawn five
   times a frame in the post-processing, and 891B8684) of such a texture back into its own surface, through a vertex
   shader that passes position and texture coordinates through (`SHADER_FLAG_TEXCOORD_PASS_THROUGH`: 0965929B and
   6DE86503), with point filtering and
   texture coordinates equal at every vertex to its framebuffer position over the surface's size (1/64 texel
   tolerance, against the half texel needed to reach another texel), no blending and no depth write, writes every
   pixel with the value it holds: it is skipped, and with it the copy of the pending resolve it would have forced.
5. **Depth restores skipped** (same switch): the depth version (`SHADER_FLAG_DEPTH_COPY`, A305C47E, 0.40 ms at
   1080p): `oDepth = saturate(depth texel)`, no colour written, viewport depth range [0, 1].
   The GPU report counts both and, for copy draws that were not skipped, why.

A frame log (`SwitchFrameLog`) writes, once a minute, everything the render thread does in one frame ("[frame]"
lines: framebuffers, clears, resolves and their copies, barrier batches, draws with shaders, state and textures,
skipped draws), to find what a frame does that it need not.

Checks: all 1,385 SPIR-V modules pass `spirv-val` (the quad ones for Vulkan 1.1); the 488 pixel and vertex shaders
without sinking keep the same bodies.

### CPU

1. **Native LZX decompression** (`SwitchNativeDecompress`; `SwitchVerifyNativeDecompress` compares every result
   with the game's decoder). XMemDecompress (sub_831CE0D0, the XDK's LZX decoder linked into the game) took 42 % of
   the loader thread (sub_831D8A50). For LZX contexts without the streaming flag, which the guest decoder resets on
   every call, a native port of the guest's own framing and decoder (`os/switch/lzx_switch.cpp`) writes the same
   bytes, size and S_OK; anything it does not reproduce with certainty (a frame the guest would drop, data before
   the start of the stream or past the source, E8 translation) runs the guest decoder instead. Identical to the
   reference output on the game's shader archive, 370 MB/s on the build PC. "[lzx]" lines report the throughput.
2. **Critical sections without the wake-up call** (`SwitchFastCriticalSections`, off). Every final
   RtlLeaveCriticalSection made a system call (svcSignalToAddress), whether anyone waited or not; in the CPU
   profile the 16-byte line that holds it and svcWaitForAddress took 10 % of the main thread. A thread about to wait
   now counts itself in the critical section's LockCount first (a field the guest never reads, -1 when nobody waits),
   with full barriers on both sides; the leave calls only when someone waits. No spinning; the 1 ms safety timeout
   stays.
3. **Events and semaphores without the wake-up calls** (`SwitchFastEvents`, off). Setting an event or releasing a
   semaphore made two notifies (the object's condition variable and the dispatcher's), each a system call on
   Horizon. Each wait now counts its waiters under the object's mutex, and the notifies happen only when there are
   some.
   Both are guest-kernel changes: an earlier attempt (critical-section spinning with waiter counting, lock-free object
   lookup and a TLS array) ended the audio at the first in-game cutscene. These are narrower, off by default, and
   each needs the cutscene test.
4. **Zero-copy command batches** (`SwitchZeroCopyBatches`). The D3D thread's batches were copied command by command
   into the render queue and out of it again (FlushDeferredRenderCommands 1.4 % of the main thread). The batch's
   buffer now goes to the render thread as one command; it runs the commands in place and gives the buffer back to a
   pool of 32; with none free the batch is copied as before.
5. **The D3D-thread test without the thread id**: every render command and state asked for the thread's id
   (pthread_self through newlib, 0.3 % of the main thread); each thread now remembers the answer until the D3D thread
   changes.
6. **Statistics without atomic read-modify-writes**: the counters the game's thread keeps for the GPU report (render
   states skipped, constant bytes copied), an exclusive load/store loop per render state and draw, are now plain
   loads and stores (their only writer is that thread).
7. **RTTI comparison memo**: the type_info comparisons of the game's dynamic_casts (strcmp 0.4 % of the main thread)
   remember, per thread, the result for pairs of type_infos in the executable's image, whose names never change;
   the same object is the same name.
8. **PGO, collecting**: an instrumented NRO (`SWITCH_PGO=generate`, no LTO) writes its profile to
   `sdmc:/switch/UnleashedRecomp/pgo/` every 3 minutes; the next build uses it with LTO (`SWITCH_PGO=use`).

The GPU report also shows where the game's thread spends a frame: its own work, then in Present the waits for the
render thread, the present, the GPU and the next image, and the frame limiter. At the 60 FPS cap the frame rate
hides CPU savings; the working time shows them.

### Not done in this round, and why

- **Present on the render thread** (the main thread starting the next frame while the render thread finishes and
  presents): the frame breakdown will show how long the main thread waits in Present first. (Done in round 10: it
  showed 1.06 ms a frame at 1020 MHz.)
- **Blend skip on float targets**: still not exact (NaN sources, -0 destinations).
- **A depth pre-pass for the main pass's overdraw**: exact only if every pipeline computes positions bit for bit
  alike (NIR may fuse multiply-adds differently when a pipeline has no fragment stage).

## Round 10: dead copies, more sinking, pipelined present, PGO (test set `unleashed-test-round9`)

The log lines of this build say "Switch round 9" (the test-set numbering). Built with PGO (the round-8 collection
run's profile), LTO, `-O2` instead of CMake's `-O3` and `-fipa-pta` (`tools/build-switch.sh` `SWITCH_PGO=use
SWITCH_LTO=1 SWITCH_O2=1 SWITCH_IPA_PTA=1`); the build takes about 25 minutes on the build PC. The profile was
collected with `-O3`: 87 % of the recompiled functions matched it, and the other 8,846 of 66,629 were built without
one, because `-O2` inlines differently before instrumentation (a collection build with `SWITCH_O2=1` would match).
The Cortex-A53 erratum workarounds (`-mfix-cortex-a53-*`) are already off in devkitA64's GCC, so there was nothing
to turn off.

**Round 8 test set results.** 1080p: 40 FPS with the per-draw profiler (config 5), 41 all on (2), 42 without the
quad sinking (3), 41 without the restore skips (4). At 858x482 with the CPU at 1020 MHz: 51 FPS (6), 53.3 with the
guest-kernel sync changes (7), 50 without native LZX and zero-copy batches (8). The freezes are gone. The quad sinking
cost 0.57 ms in the main pass (it is off by default now); skipping the restores saved their draws but the next
blended draw into the target then copied the pending resolve instead (0.46 ms each at 1080p). The game thread worked
17.64 ms per frame at 1020 MHz and spent 1.06 ms in Present: 0.64 waiting for the render thread, 0.23 presenting,
0.07 for the GPU, 0.12 for the next image.

### GPU (image-identical)

1. **Dead resolve copies** (`SwitchSkipDeadCopies`). A texture resolved from a surface reads the surface itself until
   the surface changes; then its contents are copied. When the commands after the draw or clear that changes the
   surface show the texture resolved into again (every texel rewritten) or destroyed before any draw with it in a
   texture slot, and before any CPU update of it, the copy is dropped: nothing ever reads what it would have written.
   The render thread looks at the commands it has taken, and takes more from the queue; for copies of 4 MB and more it
   waits up to 1 ms for them (2 ms per frame), never past a command the D3D thread may wait for (Present, an
   unlock). At the hub these are the two 1920x1080 copies of the post-processing (tone-mapped image before bloom and
   DOF, 0.46 ms each); the GPU report counts copies dropped, copies read first and undecided ones.
2. **More of each alpha-tested shader skipped by discarded pixels** (XenosRecomp). (a) The two halves of a shadow
   filter (the gathers and the point-fetch fallback, one of which runs) write shared temporaries instead of the
   registers, so reusing those registers no longer pins the filter. (b) A statement computing several components with
   component-wise operations is written as one statement per component, each of which moves or stays by what reads
   that component: in 874793F8 one statement computed a lighting value and the environment map's coordinates, and
   the map's fetch (implicit level of detail, which stays) kept all the lighting and shadowing before the test.
   (c) A loop with its own counter that nothing else reads (the local-light loops: arithmetic only) moves as one
   statement. Same operations on the same operands in each case. Statements moved past the per-pixel test, over all
   1,096 pixel shaders with an early-out: 24 % of those before it (44,414 of 185,044) before, 54 % after (140,606 of
   261,994: split statements count once per component); shadow filters whose gathers move: 594 of 926 (262 before);
   local-light loops that move: 275 of 470. All 1,385 SPIR-V modules pass `spirv-val`.
3. **Depth clears overwritten by the next draw** (`SwitchSkipOverwrittenDepthClears`). A clear of a depth buffer
   alone waits for the next command like a colour clear; the next draw into that depth buffer makes it inside its own
   pass, and when that draw writes every depth pixel regardless of the old values (depth test ALWAYS with writes, a
   position pass-through vertex shader on a proven full-screen quad, nothing discarded; D32F has no stencil) it is not
   made at all, but for the edge pixels the draw may miss. At the hub: the clears before the depth copies into the
   960x540 velocity depth (EE1670) and the 1920x1080 depth of the late objects (D1E270).
4. **Colour clears made in their pass** (`SwitchCarryClears`). A waiting colour clear (`SwitchSkipOverwrittenClears`)
   was made at the next change of target, as a pass of its own; it now also waits through target changes, depth
   clears and draws into other targets, none of which can touch its surface (a resolve from that surface makes it
   first), and is made in the pass that draws into it (the grey clear of the velocity buffer EE1470, 0.06 ms as a
   pass of its own). A clear replaced by another clear of the same surface is dropped.
5. **Depth buffers ready for sampling at the change of target** (`SwitchEagerDepthTransitions`). The shadow maps are
   sampled from the middle of the main pass; the first draw that samples the second one ended the pass for a barrier
   batch of its own (the report's "1.0 barrier batches inside a pass"). A depth buffer left with pending resolves now
   gets its transition with the barriers of the change of target, like colour surfaces since round 6.
6. **Quad sinking off by default** (`SwitchAlphaTestQuadSink`): 0.57 ms slower in the main pass than the per-pixel
   sinking alone.

### CPU

1. **PGO** with the round-8 collection profile, with LTO; 2. **`-O2`** for all the code (PGO turns on the `-O3`
   inlining and loop work for the hot code); 3. **`-fipa-pta`**, whole-program points-to analysis at the LTO link.
4. **Present on the render thread** (`SwitchPresentOnRenderThread`, on in the test configs). Present waited until the
   render thread had submitted the frame, then presented, waited for the GPU and acquired the next image itself (1.06
   ms a frame at 1020 MHz). It now waits only until the render thread has recorded the frame; the render thread then
   submits it, presents, waits for the GPU to finish the frame slot it reuses, resets that slot and acquires the next
   image, while the game thread starts its next frame, whose commands queue up meanwhile. The D3D thread waits for
   that before it locks a buffer or texture (the render thread copies their unlocks from guest memory), and the swap
   chain is the render thread's alone. Only once the game's own D3D thread renders (the installer is unchanged).
5. **The D3D-thread test by register**: every render command and state asks whether it comes from the D3D thread;
   the answer is now a comparison of the thread's TLS region pointer (TPIDRRO_EL0, a register every thread has its
   own of) instead of thread-local variables, each access of which is a call with `-mtp=soft` (`__aarch64_read_tp`,
   0.7 % of the main thread).
6. **Critical sections without a thread-local read**: RtlEnterCriticalSection and RtlTryEnterCriticalSection take the
   caller's r13 (its thread id) from the caller's context instead of `g_ppcContext`, a thread-local variable; the
   game enters critical sections thousands of times a frame.
7. **One type_info comparison table for all threads**: the RTTI memo of round 8 is a lock-free table of 64-bit
   entries (pair and result in one word), 1024 of them, instead of 256 per thread behind a thread-local access.
8. **UI modifier lookups**: the first thread to look one up (the game's, which draws the UI) gets a cache of 512
   entries outside TLS instead of 64 in it; FindModifierUncached (a lock and a map lookup) was 0.3 % of the main
   thread.
9. **Larger batches while the render thread sleeps** (`SwitchIdleRenderThreadBatches`). Each hand-over of a batch to
   a sleeping render thread wakes it with a system call on the game thread; while it waits, batches now go out when
   nearly full (up to 512 commands) instead of at 128.
10. **Render thread priority** (`SwitchRenderThreadPriority`, 0x2D; config 7 tries 0x2B): above the game's threads a
   guest worker on its core no longer delays it.
11. **Guest spin locks spin before sleeping** (`SwitchGuestSpinBeforeSleep`, off by default, on in the test configs):
   KfAcquireSpinLock and KeAcquireSpinLockAtRaisedIrql slept 10 µs at the first failed attempt; they now watch the
   lock word for up to 512 iterations (~2 µs) first, as its owner usually runs on another core.
12. **The round-8 guest-kernel changes** (`SwitchFastCriticalSections`, `SwitchFastEvents`, -1.2 ms of the game
   thread's work at 1020 MHz in config 7) are on in the test configs, with the cutscene test; config 9 turns them off.

Diagnostics: the CPU sampler and the stall watchdog now see system calls where Horizon reports blocked threads (at the
SVC instruction itself; they only looked right after it, so no wait was ever attributed), and
`tools/switch-cpu-profile.py` names a 16-byte bucket after both functions when another one starts in it (round 8's
"__libnx_exception_returnentry" 0.7 % was `__aarch64_read_tp`). The translator writes why each statement before the
alpha test stays (`XENOS_SINK_DEBUG=1` with `XENOS_RECOMP_DUMP_DIR`).

### Not done in this round, and why

- **Fusing the bloom add and the depth-of-field alpha** (two full-screen passes over the same RGBA8 target): the fused
  draw needs the alpha written with blending enabled (source 1, destination 0), whose rounding in the blend unit is
  not guaranteed to match a plain write.
- **Filtering repeated texture, stream and shader binds on the game thread**: the render thread re-derives some
  bindings from state the game thread does not see (streamed buffers, pending resolves); little gain for the risk.
- **Caching KeSetBasePriorityThread**: the thread's own priority changes and another thread's changes of it go
  through different objects, so a cache could go stale; the fixed sampler will show whether the calls matter.

## Final build

Built as round 10 (PGO, LTO, `-O2`, `-fipa-pta`), with these defaults.

**The four round 10 renderer changes are off** (`SwitchSkipDeadCopies`, `SwitchCarryClears`,
`SwitchSkipOverwrittenDepthClears`, `SwitchEagerDepthTransitions`). With them on, two long play sessions (config 2,
21,000 and 28,000 frames) ended with `VK_ERROR_DEVICE_LOST` from a queue submission: the GPU channel was gone, and the
game then froze inside the present, which waited for a frame the channel would never finish. With the four off
(config 3) the same play did not. They cost 1.6 FPS at 1080p (44.0 against 42.4). Which one does it is not known.
Two of them change how depth buffers are cleared and when they leave the attachment layout, and the driver's ZCULL
saves and reloads each depth buffer's culling data at every pass (`LOAD_ZCULL` on bad data kills the GPU context, as
its own comment says), so those two are the first suspects. To find out, turn one on at a time and play for 30
minutes or more; with the crash reports below, a lost GPU now leaves the driver's reason in `crash.log`.

The shader changes of round 10 and the pipelined present were on in both runs and stay on.
`SwitchPresentOnRenderThread`, `SwitchFastCriticalSections`, `SwitchFastEvents` and `SwitchGuestSpinBeforeSleep`
are on by default now, as in the configs that were played.

**Docked and handheld.** The window follows the console's output: 1920x1080 docked, 1280x720 in handheld mode (a
dock set to 720p counts as handheld). The mode comes from the default display resolution, whose change event
`GameWindow::Update` polls (nothing runs an applet message loop, so `appletGetOperationMode()` would keep its first
value). A change goes to the swap chain: plume makes it again at the new size (the NWindow cannot be resized while a
swap chain has buffers on it; the WSI sets its size when it makes the new one), and the game remakes its render
targets as after a window resize on the PC. The internal resolution is kept per mode:
`SwitchDockedResolutionScale` (0.8, 1536x864) and `SwitchHandheldResolutionScale` (0.9, 1152x648). The Resolution
Scale option shows and changes the current mode's, and its reset gives that mode's default.

**Logging off.** `SwitchLog` (off) writes `stderr.log` and `UnleashedRecomp.log`; without it neither file is opened
(the logger keeps its first lines in memory until the config is read, then writes or drops them). The profilers
(`SwitchCpuProfiler`, `SwitchGpuPassProfiler`, `SwitchGpuDrawProfiler`) and the frame log stay off, and the stall
watchdog is off (`SwitchStallWatchSeconds = 0`; 1 for tests, with `SwitchLog`).

**Crash reports** (`os/switch/crash_switch.cpp`), whatever `SwitchLog` says, appended to
`sdmc:/switch/UnleashedRecomp/crash.log`:

- A CPU exception: the libnx user exception handler (after `nx_crash_handler.c` of the battd_nx port) writes the
  thread, the fault (pc, lr, far, esr), the registers, the frame-pointer chain, the return addresses found on the
  stack and a dump of it, with the file system service directly (the faulting thread may hold stdio's or the heap's
  lock), then breaks so that Atmosphère writes its report too.
- A lost GPU: plume calls back on the first `VK_ERROR_DEVICE_LOST` from a submission, fence wait, query read, present
  or acquire. The report has the driver's error messages from just before it, among them the channel error
  (`nvkmd-switch: channel N lost: notification={type info status} error={type info}`; type 8 is a GPU idle timeout,
  a hang, the others faults). A release Mesa hands those messages only to a `VK_EXT_debug_utils` messenger, which
  plume now makes on Switch (errors only). Then it breaks, instead of the freeze.

`python tools/switch-cpu-profile.py crash.log --elf <the ELF of that NRO>` names the `+0x` offsets of the `[crash]`
lines.

## Considered and not done

- **Guest-kernel changes, reverted.** Critical sections that spun and skipped the kernel signal without waiters,
  lock-free lookup of guest events/semaphores, and a plain TLS array were tried and taken out again: on the first
  hardware run the game's audio stopped for good at the first in-game cutscene, and the game's audio path takes
  critical sections and waits on events every frame. `kernel/` and `apu/` went back to the repository's own code,
  and the audio survived that cutscene again (confirmed on the console), so these changes were the cause. Round 9
  brought back two narrower ones and round 10 a third, each behind its own switch and off by default until the
  cutscene test passes (see the audit).
- **Compact texture heap: briefly reverted, now on.** The half-second stutters first blamed on it happen in
  every run with the GPU pass profiler on and never with it off: the profiler wrote its tables to
  `stderr.log` on the SD card line by line from the render thread. The reports are now written from a
  background thread in one write, and the heap is on by default.
- **ZCULL direction.** `NVK_ZCULL=off` changed no pass at the hub, so ZCULL does not cull anything that
  matters there; `greater` gave −0.17 ms, mostly in the half-resolution pass with depth, and is now the
  default (`SwitchZcullGreater`).
- **More dynamic state (cull mode, depth test/write/compare).** Fewer pipeline variants and binds, but every
  pipeline, including the copy and ImGui ones, would have to set that state; plume and the pipeline key change.
- **A depth pre-pass for the main pass.** It would be exact only if two pipelines computed positions bit for bit
  alike, which the driver does not promise (NIR may fuse multiply-adds differently).
- **Blend skip on float targets.** Exact only for a source alpha of exactly 0, and even then a −0 destination would
  become +0.

## Further opportunities (not implemented)

All output-identical:

1. **Where the game thread waits.** In the round-8 test set the game thread was in system calls in about 13 % of its
   samples at 1020 MHz: sleeping 4.3 %, IPC 4.2 %, condition variables 3.1 %, address waits 1.5 %. Since round 10 the
   CPU sampler attributes these to their callers ("wait" lines), which shows which guest sleeps, locks or IPC calls
   to go after.
2. **A PGO profile from an `-O2` build.** The round 10 build used the round-8 profile, collected from an `-O3`
   instrumented build: 87 % of the recompiled functions matched it, and the other 8,846 of 66,629 (two of the seven
   hottest among them) were built without a profile, because `-O2` inlines differently before instrumentation. A
   collection build with `SWITCH_PGO=generate SWITCH_O2=1` matches the `-O2` release build.
3. **Batched streaming uploads.** Textures loaded on worker threads are uploaded with one submission and one fence
   wait each, on the same single queue as rendering (NVK on Horizon has one queue, and every submit is an IPC call).
   Recording those copies into the next frame's command list would shorten streaming and loading.
4. **KeSetBasePriorityThread mapping.** Centre guest priorities on 0x3B (the time-sliced band) instead of 0x2C,
   keeping boosted threads below the render (0x2D) and present (0x2C) threads. A/B test it; the audio priorities look
   deliberately tuned.
5. **The rest of the alpha-tested shaders' work before the test.** 332 of the 1,096 shaders with an early-out still
   keep their shadow filter before it, mostly because the alpha itself, or a kept environment-map fetch, depends on
   it; `XENOS_SINK_DEBUG=1` lists why for each statement.
6. **The transparent pass's copies** (about 1.4 ms at 1080p). The water draw samples the colour and depth it writes,
   so both are copied first; only a copy limited to the draw's screen area could save part of it, which needs that
   area on the CPU.

## How to measure

Each test set holds an NRO and one folder per configuration, each with its `config.toml`; after a run its `stderr.log`
goes back into that folder. The first line of `stderr.log` names the build (`Build: <id>, ...`): check it first.

- **GPU-bound runs** (1080p, `ResolutionScale = 1.5`). Stand at the same spot in the Apotos hub for about 15 s.
  `SwitchGpuPassProfiler = true` prints, every 300 frames (`[gpu passes]`), the GPU time of each render pass and
  per-frame counters: resolve copies and why, hand-overs, clears, barrier batches, restores, dead copies.
  `SwitchGpuDrawProfiler = true` adds the most expensive draws of the eight most expensive passes; its flushes make
  totals read higher, so it gets a run of its own.
- **CPU-bound runs.** 858x482 (`ResolutionScale = 0.67`), the CPU clocked down to 1020 MHz and the GPU at its maximum,
  so the game's main thread limits the frame. At the 60 FPS cap the frame rate hides CPU savings. The pass report's
  line `game thread per frame: X ms working (...), then in Present ...` shows them: compare X between configurations.
- **Where CPU time goes.** `SwitchCpuProfiler = true` samples every registered thread every 2 ms and writes the hottest
  code lines per thread every 30 s. `tools/switch-cpu-profile.py stderr.log --elf <ELF of that build>` names them
  (recompiled functions as `sub_82XXXXXX`, with both names when a sampled line spans two functions) and the call
  chains of system calls ("wait" lines). Keep the ELF of every test NRO.
- **What a frame does.** `SwitchFrameLog = true` writes, once a minute (five times), every framebuffer bind, clear,
  resolve, resolve copy, barrier batch and draw of one frame (`[frame]` lines).
- **Stalls.** The stall watchdog (on by default) writes `[stall]` lines when no frame comes for a second, and `[hitch]`
  lines counting frames over 100 ms.
- **The spread of frame times,** not only the average: output is quantised to 16.7 ms steps, so frames that stop
  crossing 33.3 ms or 50 ms change what the player sees even when the average moves little.

Checks that each change is active, in `stderr.log`:

- The "Switch renderer", "Switch shaders", "Switch resolves" and "Switch round N" lines list which switches are on.
- Constants: a regenerated shader cache (`shader_cache.cpp.translator` exists) and `constants in uniform buffers on`.
- Pipeline cache: `Pipeline cache: ... (N bytes loaded).`
- Direct calls: `switch-direct-calls: N calls ... made direct` during the build, and `OK, N hooks in the ELF, none of
  them bypassed` after linking (N is about 190; 0 would mean the check saw nothing).
- ZCULL: `[nvk] ZCULL mode: ...` at the first render pass, and `[nvk] ZCULL:` lines with a driver that has it.
- GPU profile (handheld): `[apm] GPU 307.2 -> 460.8 MHz, memory stays at 1331.2 MHz`.
- Overlay FPS: `[overlay] Connected to SaltyNX through SaltySD` and `[overlay] FPS block published ...`.
- Native LZX: `[lzx]` lines every 64 archives, with the throughput.
- Audio: `audio gaps since start N` in the pass report.
- NVK fast paths, when requested: `NVK fast paths: requested (set 4 contract found, draw contract found)`.
- PGO: `Switch PGO: generate (...)` or `use (...)` at configure time; a collection build writes `.gcda` files and
  `dumps.txt` to `sdmc:/switch/UnleashedRecomp/pgo/`.
