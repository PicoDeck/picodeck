# perf_rig

Estimates how long firmware-compiled C takes on the device, without the device. Host timings say little, because most Core 0 time goes to XIP cache misses. This tool runs the real Cortex-M33 code in Unicorn, sends every instruction fetch and data access through a model of the XIP cache, and adds up cycles.

Its output is an estimate, never a measurement. Use it to compare two versions of the same code, and confirm anything that matters on the device.

## Running it

```bash
make -C tools/perf_rig run                                 # this tree's gfx3d.c, as the firmware lays it out
make -C tools/perf_rig run GFX3D_SRC=/path/to/worktree     # that tree's src/os/gfx3d.c, gfx3d.h, display_clip.h
make -C tools/perf_rig compare GFX3D_SRC=/path/to/worktree # this tree's, then that one's
make -C tools/perf_rig run MAP=text-sram                   # the same code placed elsewhere (below)
make -C tools/perf_rig run RIG_ARGS="--psram-line 90"      # any option of the host tool
make -C tools/perf_rig run-lua                             # the real Lua core, an empty for-loop
make -C tools/perf_rig run-tex                             # gfx3d's textured rasteriser, a frame of ground
make -C tools/perf_rig run-tmesh                           # its whole textured pipeline, a frame of a valley
make -C tools/perf_rig run-sky                             # a sky image alone, 40,000 pixels of copy
make -C tools/perf_rig run-reflect                         # a reflecting frame: two scenes and blended water
make -C tools/perf_rig check-tex                           # its M33 pixels against this host's
make -C tools/perf_rig sweep                               # how far the estimate moves with each assumption
make -C tools/perf_rig check                               # checks the emulator
make -C tools/perf_rig help                                # every option and default
```

Needs `arm-none-eabi-gcc`, host `lua` 5.4, a simulator build for its static Unicorn (`build_sim/` here or in the main checkout, or `UNICORN_BUILD=<...>/libarm-softmmu.a`), and the game's repository (`NOVA_RAIL=<dir>`, default `~/Projects/PicoDeck/nova-rail`). `FRAMES` (default 60) and `LAP_S` set how much of the lap is sampled; 60 frames take about a second.

The table's last line hashes every frame's pixels: an optimisation that must not change the picture keeps that hash.

The table's device row is the unmodified code's, at 200 MHz. For changed code or another map, compare the estimate with the unmodified code's estimate, not with that row.

## What runs

- **Code.** `src/os/gfx3d.c` is its own translation unit, built with the firmware's compiler and flags (`-mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp -mcmse`, `-Os` then its per-file `-O2 -ffp-contract=off`). In `MAP=flash`, its symbols land at the same addresses as in `build/picodeck.elf` (checked by diffing `nm` at the time of writing: identical), so its lines fall in the same cache sets. A changed `gfx3d.c` moves as it would in the firmware, up to what follows it. Textured code (`.gfx3d_tex_text`) comes after the guest's own code, out of the flat code's way, as it comes last in `src/hot_gfx.ld`. `.time_critical*` goes to SRAM, as does newlib's `sqrtf` (where the firmware has it); `memset` and `memcpy` stay near their firmware addresses.
- **Scene.** `scene/dump_scene.lua` runs the game's real `render.lua`, `trackgen`, `trackmesh`, `models` and `camera` under host Lua with `picocalc.gfx3d` stubbed, and records every call. It totals 2353 triangles, the device's `tris_in` max. Only the motion is approximate: the player runs the lap at constant speed, and the rivals hold fixed offsets, with no AI or physics.
- **Phases.** `guest/gfx3d_main.c` replays the calls in the bridge's order. Geometry and raster bracket the same calls as the bridge's `us_geom` and `us_raster` timers. Meshes, the context and the blocks that overflow lent memory come from a bump allocator in PSRAM at 200-byte granularity; the stack is in PSRAM; the 320x320 target is in SRAM with `swap` on, the viewport 320x240. Lent SRAM is as on the firmware (`gfx3d_sram_scratch`, `gfx3d_sram_tier`); `LENT="-DLENT_TIER=0 -DLENT_SCRATCH=0"` keeps either in PSRAM, as an older `gfx3d.c` does anyway.
- **Textured triangles (`run-tex`, `check-tex`).** `guest/gfx3d_tex_main.c` calls `gfx3d.c`'s test-only triangle entry, built with `-DGFX3D_TEST -DPICO_ON_DEVICE=1` so that its spans are in SRAM. `run-tex` draws about 2880 triangles of 50 pixels, the device spike's load, from four 64x64 textures with mips in PSRAM, with the stack in PSRAM; the table's "culled" column is the pixels, "clipped" the affine triangles, "drawn" the perspective ones, and its cost includes the test entry's copy of the vertices and the guest making them (about 7% of the instructions). `check-tex` draws the triangles of `tests/unit/gfx3d_tex_scene.h` in both byte orders and compares the hash with the same code on the host: on the M33 the swapped frame comes from the hand-written pixel loop and the other from the C, so the two are held to each other. The same triangles' CRCs are pinned in `test_gfx3d.c`, and `make test-unit` runs a smaller set of them, with the pixel loop over every multiplier, on qemu-arm (`test_gfx3d_tex_m33`).
- **The textured pipeline (`run-tmesh`).** `guest/gfx3d_tmesh_main.c`: `gfx3d_draw_tmesh` over chunks of hilly ground in fog, drawn twice, then `gfx3d_end`, with a moving camera, a capacity of 8000 and the lent SRAM as on the firmware, built with `-DPICO_ON_DEVICE=1`. About the device spike's load. Its table's "clipped" column is the pixels. Geometry and raster are the two calls, as in `run`. `run-sky` is the same guest with only a 1024x128 sky image over 125 rows, so its raster is what the copy costs. `run-reflect` adds that sky and a frame in two scenes: a mirror pass of the chunks stood upright as tall land and 330 trees, in the water's rows, then the valley and the trees with a blended sheet of water; each phase is the sum over both scenes, and the "culled" column is the triangles of the mirror pass that were drawn.
- **Lua (`run-lua`).** The real core from `third_party/lua-5.4` with the device's defines and `-O2`, and `for i = 1, 1000000 do end`, which is one FORLOOP bytecode per iteration. `luaV_execute` has the firmware's address and size (checked with `nm`); the rest of Lua follows it at other addresses, which an empty loop never reaches once running. The heap is a bump allocator in PSRAM.

## The cost model

Cycles = instructions x 1, + 1 per word loaded, + 1 per word stored, + 2 per taken branch, VDIV and VSQRT 14 in all, + flash line 56, + PSRAM line 80, + write-back 64. All are options. The core figures come from the Cortex-M4 timing table, the nearest published one; the M33's own timing is not published in this form. The miss costs are from the device measurements in the performance brief.

Cache: 16 KB, 2 ways, 8-byte lines, 1024 sets, set = address mod 8 KB, one tag space for flash and PSRAM, write-back, write-allocate.

### Assumptions

- **Replacement is LRU, and that is a guess:** the policy on the silicon is not known. `--policy fifo|random` tries others (see the sweep).
- **Write-allocate** is inferred from the measured cost of writing fresh PSRAM (fill plus write-back), not read from a datasheet.
- **Between frames the cache is emptied**, since 19 ms of Lua sweeps 16 KB many times over. Nothing is charged for that. `--dirty-frac F` leaves a fraction of the lines dirty for gfx3d's misses to write back, and `--warm` keeps the cache.
- **Fetches are per line touched.** The core's prefetch runs ahead past taken branches on the device, and the rig has no such speculation, so it undercounts fetch misses.
- **A miss stalls the core for its whole cost.** The write buffer can absorb some store misses on the device.
- **Not modelled:** interrupts (the 1 ms service timer lands inside the device's timers), Core 1 and DMA sharing the cache, QMI contention, SRAM contention, the bridge's `now_us()` calls, the real heap's layout (umm addresses depend on the app's history), Lua touching the cache between `gfx3d_draw` calls, and the pico_float `tanf`, `sinf`, `cosf` (newlib's run here, at most a few calls a frame).

## Maps

| `MAP=` | What moves |
|---|---|
| `flash` | Nothing: the firmware's layout. |
| `text-sram` | gfx3d's code to SRAM. There is no room for it on the device, so this is an upper bound. |
| `stack-sram` | The stack to SRAM. |
| `scratch-sram` | The stack and every gfx3d allocation to SRAM: no PSRAM data at all. Also impossible on the device. |

Individual bases can be set too: `TEXT_BASE`, `RAMTEXT_BASE`, `LIB_BASE`, `RODATA_BASE`, `DATA_BASE`, `STACK_TOP`, `HEAP_BASE`, `FB_BASE`, and `LUAV_BASE` for `run-lua`.

The firmware addresses are defaults in the Makefile, read from the build at commit b5ac305, and firmware changes move them. To refresh them: `arm-none-eabi-nm -n build/picodeck.elf | grep -E ' (mat_mul|sqrtf|qsort|luaV_execute)$'`.

## Emulator checks

`make check` runs both of these, and both pass:

- `guest/selftest.c` executes VFP instructions (including VRINT, VMAXNM, VFMA) and DSP ones (QADD, SMLAD, SADD16, UXTAB16, USAT) with known results. It also checks that an LDM, a VLDM and a VPUSH count one transfer per word, and that the cache model gives the expected misses on a conflict pattern.
- Every frame's pixels, emulated, hash the same as the same C built for the host. So the VFP paths gfx3d actually takes are right.

## Calibration

Against `hw_lap` on the device at 200 MHz (Nova Rail, 5 rivals, one lap, no audio; `--clock 200000`, since the game's own clock is 300 MHz). Nothing was tuned, and the miss costs are the stated ones. Each row is a real firmware, against the rig running that firmware's `gfx3d.c` and lent memory:

| gfx3d | device geometry | rig | device raster | rig | device total | rig |
|---|---:|---:|---:|---:|---:|---:|
| Before any of this (`integration/issues-2026-11-05`) | 8.3 | 5.40 | 5.94 | 5.63 | 14.2 | 11.04 |
| Bitmap walk, span fill, one slope per edge, vertex scratch in SRAM | 5.9 | 3.90 | 3.66 | 3.60 | 9.6 | 7.50 |
| The same, and the scene's records in lent SRAM | 4.5 | 2.91 | 3.01 | 3.10 | 7.5 | 6.02 |

- **Comparisons held:** the rig said -32% and then -46% from the first row; the device did -33% and -47%.
- **Geometry reads about 1.5x low, steadily** (1.54, 1.51, 1.54): prefetch misses, the interrupts inside its timers and dirty lines left by Lua are all left out, and all add time.
- **Raster is within 5%,** on all three.

The scene's own numbers: `drawn` avg 349 (device 358), max 544 (device 587), `tris_in` max 2353 (device 2353).

**Lua's empty for-loop checks the core figures alone**, since it has no misses once running. The estimate is 40.0 cycles an iteration (200 ns): 25 instructions, 7 words loaded, 4 stored, 2 taken branches. The device's is ~180 ns (36 cycles), so the estimate is 11% high. That agrees in direction with raster: the core figures run somewhat high on the M33. One short loop cannot fix them, so they stay as they are.

## How far to trust it

- **Exact, for this scene and build:** instruction, FP and load/store counts. They are what the code does.
- **Good for comparing versions:** fetch and data misses and write-backs, given the assumptions above. In the sweep the replacement policy moves geometry by about 6% and raster by under 1%. A change that removes misses or instructions by much more than that is probably real.
- **Rough:** the absolute milliseconds. On the three builds above, device time is about 1.5x the estimate for geometry and within 5% of it for raster, and that may not hold for code that behaves differently. For the same reason this model overstates raster against geometry.
- **Not captured:** a change's effect on the Lua code around it (cache sets freed or taken), the layout of the rest of the firmware, and what a call costs in Lua. The last matters: splitting Nova Rail's skyline into 8 meshes saved 0.22 ms here, and cost 0.17 ms on the device (six laps each), because the extra `drawBackground` calls outweighed it.
