# The NVK driver with the nfsmw-nx improvements

UnleashedRecomp links Mesa's NVK statically. The nfsmw-nx changes to that driver (ZCULL, NAK scheduling and
branch flattening, the FADD32I saturation fix, the draw-path fast paths, dynamic uniform buffers by differences)
are ported to mesa-switch 26.2.2 in the separate `mesa-switch-main` source folder, whose `nfsmw/README.md` has
the details, the build requirements and the list of run-time switches.

## Build the driver

Windows, from an MSYS2 shell in the Mesa folder (start scripts with `bash`: the folder is not on `PATH`):

```sh
bash build-nfsmw.sh --check                          # lists what is missing and how to install it
bash build-nfsmw.sh --install-packages --setup-rust  # installs it (pacman, and Rust into build/deps)
bash build-nfsmw.sh                                  # builds
```

Linux: `./build-switch.sh` (Docker).

On an x64 PC (not Windows on ARM), `tools/build-switch.sh`'s default CLANGARM64 tools do not run: pass
`CLANGARM64=/clang64/bin CLANG64=/clang64/bin` (the CLANG64 clang, cmake and ninja the Mesa build installed), as
the Mesa script prints at the end.

## Build UnleashedRecomp against it

```sh
rm -rf build
NVK_ROOT=/c/path/to/mesa-switch-main tools/build-switch.sh
```

`NVK_ROOT` can be the Mesa folder after either build, an extracted Mesa Switch SDK, or its
`opt/devkitpro/portlibs/switch` folder; the script prints the `libvulkan.a` it picked. Remove `build/` first:
Ninja does not notice that the driver archive changed.

What changed on this side for the current mesa-switch:

- plume links `expat` and `drm_nouveau` only when devkitPro has them (the Horizon backend no longer uses
  `switch-libdrm_nouveau`; `build-unified.sh` builds the driver with xmlconfig, which needs expat), inside one
  `--start-group`/`--end-group` so the linker rescans the driver archive.
- `os/switch/nvk_switch_stubs.c` gained weak libelf stubs (the driver's CUBIN parser references libelf, which
  devkitPro does not ship and which never runs on Horizon), and its existing stubs are now weak, so a driver that
  brings its own definition of one of them no longer fails the link.
- stderr goes to `sdmc:/switch/UnleashedRecomp/stderr.log`: the renderer's and the driver's diagnostics were lost
  before, since Horizon has no console behind stderr.
- `[Switch] SwitchMesaEnvironment = "NAME=value;NAME=value"` sets driver variables before the instance is
  created, to A/B the driver's changes without rebuilding (`NVK_SWITCH_DIBUJO=0`, `NVK_SWITCH_DYN_UBO_DELTA=0`,
  `NVK_COPY_ENGINE=1`, `NVK_SUBTILING_KNOB=0x20164010`...).

The renderer already does what the driver changes need: depth render targets are created without
`TRANSFER_DST` (ZCULL planes). The draw-path and set 4 paths of nfsmw-nx run only for a game that asks for them
through the driver's shared structures: nfsmw-nx does on every submit, UnleashedRecomp with
`[Switch] SwitchNvkFastPaths = true` (off by default since the round 6 test set, see below).

## Check it on the console

In `stderr.log`:

- `nvkmd-switch: ZCULL activado` and `nvkmd-switch: contexto de ZCULL atado`: ZCULL is available and bound.
- `[nvk] ZCULL: plano de ... para profundidad WxH`: depth targets received ZCULL planes. `SIN plano` lines give
  the reason for any that did not.
- `NVK fast paths: off (set 4 contract found, draw contract found)` (`requested` with `SwitchNvkFastPaths = true`).
- `NVK fast paths: a driver self-check saw a difference ...` would mean one of the driver's paths turned itself off
  for the session (rendering stays correct; report it).

Then compare frame times with the old driver in the same places, and with `SwitchMesaEnvironment` switching the
driver's paths off one at a time.

## Maxwell operand reuse in the shader compiler (on by default)

NAK sets Maxwell's operand-reuse control bits between neighbouring FADD/FMUL/FFMA instructions that
read the same register in the same operand slot, so the second takes it from the reuse cache instead
of the register file, avoiding register-bank conflicts. Same results, fewer stalls in math-heavy
shaders: on the console the GPU frame at the hub went from 22.63 to 22.49 ms, and nothing rendered
differently. It started as the opt-in `NAK_DEBUG=reuse` and is now the default; `NAK_DEBUG=noreuse`
turns it off (the old `reuse` flag is accepted and does nothing). The driver's NAK revision in the
shader cache key went to 2, so the first launch with this driver recompiles every shader.

## ZCULL direction experiments

`NVK_ZCULL` (through `[Switch] SwitchMesaEnvironment`) picks how the driver programs ZCULL:

- `less` (default): `ZDIR_LESS` for every depth target, as NVIDIA's own driver does.
- `greater`: `ZDIR_GREATER` for every depth target. The main scene uses reversed depth (`GREATER`
  tests), so this is the setting where ZCULL could reject hidden pixels in the main pass; the shadow
  maps (`LESS` tests) lose it instead. If the main pass gets faster, a per-target direction is next.
- `off`: ZCULL never enabled, to measure what it gives today.

`greater` measured −0.17 ms at the hub (mostly the half-resolution pass with depth) and is now what the game
sets by default (`[Switch] SwitchZcullGreater = true`; an `NVK_ZCULL` in `SwitchMesaEnvironment` wins).

Each mode keeps one direction for the whole run, so ZCULL data saved at the end of a pass is always read
back with the direction it was written with. The driver prints `[nvk] ZCULL mode: ...` at the first pass.
Watch the shadows and distant geometry in `greater` mode: wrong culling would show as missing pixels.

## Shader statistics

`NVK_SHADER_STATS=1` prints one line per compiled shader: stage, `spirv` (the first 8 hex digits of the
BLAKE3 of its SPIR-V, the same as `MESA_SPIRV_DUMP_PATH` file names and the draw profiler's shader names),
registers, local memory, warps per SM, code size, instructions, static cycles and spills/fills. It prints
only when a shader is compiled, so use it on a launch that compiles (the first one after a driver change,
or with `cache/pipelines.bin` deleted and `MESA_SHADER_CACHE_DISABLE=true`).

## Unread varyings dropped when a pipeline is linked (`NVK_LINK_VARYINGS`, on)

NVK compiled the vertex and fragment shader of a pipeline independently. Now, for every pipeline that has
both, it finds which components of each generic input the fragment shader actually reads and removes the
other components from the vertex shader's output stores; dead-code elimination then removes the math and
vertex fetches that fed them, and the vertex shader writes fewer attributes. The fragment shader reads the same
values as before, so rendering cannot change. `[Switch] SwitchLinkVaryings` sets it (default on;
`NVK_LINK_VARYINGS=0` in `SwitchMesaEnvironment` also turns it off). The setting is part of the pipeline cache
UUID and the NAK revision went to 3, so the first launch with this driver recompiles every shader.

## Compression for small render targets (removed)

`NVK_SWITCH_COMPRESS_MIN_KB` lowered the size from which an image gets its own allocation, and so compression,
from 1 MiB to what the game asked (`SwitchCompressSmallTargets`, 256 KiB). The round 4 test set measured no
difference at 1080p (config 7), so the knob and the option are gone: the driver uses 1 MiB again, as nfsmw-nx's.

## Mesa's disk cache

With `SwitchPipelineCache` on (the default) the game now always sets `MESA_SHADER_CACHE_DISABLE=true`. The disk
cache only duplicated `cache/pipelines.bin`, wrote to the SD card from Mesa's threads, and is keyed on the
driver's build id, which is the Mesa git commit: a rebuilt driver with uncommitted changes could have been handed
shaders compiled by the previous one.

## Round 5 driver changes (NAK revision 4)

- **Only the vertex attributes a vertex shader reads are fetched** (`NVK_SWITCH_VI_READ_ONLY`) and **early depth
  test for discarding shaders without depth writes** (`NVK_SWITCH_EARLY_Z_KILL`): removed again after the round 4
  test set (config 5 measured no difference at 1080p), so that no other game running this driver depends on
  them.
- **Pipelines without a fragment shader** (`NVK_SWITCH_VS_ONLY_VARYINGS`, on): their vertex shader's generic
  outputs are dropped too. Needs `NVK_LINK_VARYINGS`.
- **NAK**: operand reuse also on FMNMX/FSET/FSETP/SEL (`NAK_DEBUG=reusebasic` limits it to the round 3 set), and the
  scheduler latencies can be set with `NAK_TEX_LATENCY`, `NAK_MEM_LATENCY`, `NAK_ATTR_LATENCY` (default 200).
  These change the compiled shaders, so every value gets its own cache entries and the first launch with a new
  value compiles every shader again.

These are set through `[Switch] SwitchMesaEnvironment` for A/B runs, e.g.
`SwitchMesaEnvironment = "NAK_DEBUG=reusebasic"`. The first launch with this driver recompiles every shader (NAK
revision 4).

## Round 6: tweaks without a measured gain undone

The round 4 test set (1080p, GPU-bound) showed no difference for three driver changes of ours and for nfsmw-nx's
draw-path paths. To keep this driver as close as possible to what other games expect:

- `NVK_SWITCH_VI_READ_ONLY`, `NVK_SWITCH_EARLY_Z_KILL` and `NVK_SWITCH_COMPRESS_MIN_KB` are removed (their code,
  the command-buffer state they kept and the occlusion-query tracking).
- nfsmw-nx's draw-path improvements (`nvk_switch_dibujo`) and set 4 by differences (`nvk_switch_set4`) ran for
  every application unless it said no (`pedido = -1` meant yes). They now run only for an application that asks
  (`pedido > 0`): nfsmw-nx writes 1 on every submit, so it keeps them; UnleashedRecomp asks only with
  `SwitchNvkFastPaths = true`. These work on the CPU side (the render thread's time inside the driver), which a
  GPU-bound test cannot show; the round 6 test set measures them at 480p.

What stays of ours: Maxwell operand reuse (FADD/FMUL/FFMA, and FMNMX/FSET/FSETP/SEL), the ZCULL direction modes
(`NVK_ZCULL`; the game uses `greater`), unread varyings dropped when a pipeline is linked (`NVK_LINK_VARYINGS`) and
for pipelines without a fragment shader (`NVK_SWITCH_VS_ONLY_VARYINGS`), and the scheduler latency knobs (default
200, nfsmw-nx's value). No change to the compiled shaders, so the shader cache stays valid.
