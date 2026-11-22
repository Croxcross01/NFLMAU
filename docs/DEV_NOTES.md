# Developer notes

## Where things stand (Nov 16, 2026)

The game runs headless: boot -> archives -> title flow -> device selection -> content/menus (2,000+ file reads in
the first minute), game loop at ~30 fps on a null GPU, audio engine ticking, XInput polled, save data written.
No crashes in 60 s runs on Linux or under Wine.

### Fixes that unblocked it (in order)
1. **GPU command processor** (`gpu.cpp`, Xenia semantics). It polls CP_RB_WPTR at guest 0x7FC80714 and runs PM4
   from the 32 KB ring (VdInitializeRingBuffer size_log2 is in qwords: bytes = 1 << (log2 + 3)). It writes the
   read pointer back and implements WAIT_REG_MEM, MEM_WRITE, EVENT_WRITE_*, COND_WRITE, INTERRUPT,
   INDIRECT_BUFFER, SET_CONSTANT*, and XE_SWAP (VdSwap writes the swap packet into the 64 dwords the caller
   reserved).
2. **Interrupt CPU number.** The D3D ISR (0x882A53B8) acks `1 << KPCR[0x10C]`, so dispatching an interrupt must
   set the PCR's current-processor byte (CPU 2 for the CP's `INTERRUPT 4`).
3. **Physical memory aliasing.** One 512 MB block is mapped at 0xA0000000, 0xC0000000 and 0xE0000000 (memfd on
   Linux, SEC_RESERVE section views on Windows). The virtual heap moved to 0x40000000-0x7F000000.
4. **Kernel variable imports.** The XEX imports 12 data variables. XenonRecomp only wires functions, so the slots
   were empty and GetTickCount (via KeTimeStampBundle) never advanced. That froze the app threads' tick loops
   (`88A2FBF0`). `xex.cpp` records the slots and `kvars.cpp` fills them; KeTimeStampBundle updates every 1 ms.
   `tools/xeximports.py` lists them.
5. **Timebase.** mftb is recompiled to rdtsc, so KeQueryPerformanceFrequency reports the measured TSC frequency.
6. **XAudio render driver** (`audio.cpp`). Without callbacks the loading flow stalls waiting on the sound
   engine. Each client gets 64 frame credits, refilled one per 5.33 ms; the callback receives a *pointer* to its
   argument.
7. **Vector instructions.** XenonRecomp emitted `__builtin_debugtrap()` for vupkd3d128 types 2-5, vpkd3d128
   types 2/3/4, vcmpbfp(128) and blrl (198 + 6 + 2 + 1 sites). All are implemented in the patch (Xenia's
   PACK/UNPACK semantics, with the reversed host lane order).
8. **XAM content.** Saves are folders under save/content/<type>/<name> with the raw XCONTENT_DATA in .xcontent,
   mounted as the requested drive. License mask = 0xFFFFFFFF (full game). The device selector picks the HDD.

### Debugging recipe
- `gdb -p <pid> -batch -ex "thread apply all bt 30"`: recompiled functions are named `__imp__sub_<guest addr>`,
  so backtraces read as guest call stacks. Guest registers: `frame N` on a host import frame, then
  `p/x ctx.r31.u32`.
- `tools/peek.py <pid> <g_base>` reads guest memory (big-endian) from a live process.
- `MADDEN_GPU_TRACE=1` logs PM4 packets; `MADDEN_AUTOPRESS="Start:10:200,A:13:200"` scripts button presses;
  `MADDEN_EXIT_AFTER_FRAMES=N` stops after N swaps.

### Next
1. Window + present (SDL2/Win32 swapchain), then **draws**: port Xenia's D3D12 backend pieces (shared memory,
   render-target cache / EDRAM, texture cache with untiling, DXBC shader translator, primitive processor). This
   is the bulk of the remaining work.
2. XMA decoding (Xenia's XmaContext + an MPEG/WMA-Pro decoder) and real audio output (XAudio2/WASAPI).
3. Remaining XAM UI calls (keyboard UI, sign-in UI) as the menus reach them.

## Recompile facts

- XEX: base 0x88000000, image 0x1580000, .text 0x882A0000-0x8940B7D4 (the tail 0x8940AFB8+ is the export
  table and data, which produces harmless "unable to decode" messages).
- Save/restore helpers: see `config/arcade.toml.in`.
- 783 jump tables from `tools/findswitch.py` (stock XenonAnalyse finds 12). 9 are bounded by scanning the
  table because the `cmplwi` bound check sits too far back or doesn't exist.
- About 175 explicit function ranges cover switch bodies that pdata splits into pieces. The patched
  XenonRecomp lets these ranges win over pdata entries, call targets and analysed functions that overlap them.
- The generated code expects the XenonUtils `ppc_context.h`.

## Kernel layer design (host/src)

- **Waitable objects:** every one lives in guest memory as a `DISPATCHER_HEADER` (type, signal state), so
  game code that pokes at them directly still works. All waits and signals use one global mutex +
  condition variable (`threads.cpp`).
- **Handles** start at 0x100 and step by 4. `0xFFFFFFFE` = current thread.
- **Threads:** each guest thread is a real host thread with its own guest stack, a PCR (r13) with
  `+0x100 = KTHREAD`, and a KTHREAD with `+0x14C = thread id` and `+0x160 = last error`.
- **Critical sections** use host recursive mutexes keyed by guest address. The guest fields
  (`RecursionCount +0x14`, `OwningThread +0x18`) are kept up to date.
- **File system:** `fileio.cpp` maps the game's devices onto host folders, case-insensitively.
- **printf family:** `misc.cpp` reads guest varargs. The first 8 arguments come from GPRs, the rest from
  r1+0x54 in 8-byte slots. Doubles arrive as raw 64-bit GPR values; a `va_list` is a pointer to 8-byte slots.

## Next big pieces

1. Get past the allocator crash (above), then keep implementing whatever the trace shows.
2. GPU. The game drives Xenos through the statically linked D3D9-like runtime: ring buffer, PM4 packets,
   registers. There are two options:
   a. Emulate the command processor (Xenia-style). This is general but large: shader microcode translation,
      EDRAM, tiled textures.
   b. Identify the game's D3D entry points (signature-match the XDK D3D library) and replace them with a
      native D3D12/Vulkan renderer (Unleashed Recomp style). This is faster and runs better, but needs
      function identification first.
3. XMA audio decoding into XAudio2/SDL. Controller input through XamInputGetState -> XInput/SDL.
4. Windows build. The host already uses `std::filesystem`, VirtualAlloc and SEH under `_WIN32`. The generated
   code needs clang-cl. Not yet tested on Windows.

## Rendering (in progress)
- `host/src/presenter_vk.cpp`: Vulkan presenter.
  - Windows uses a Win32 window; Linux uses SDL2.
  - On XE_SWAP the front buffer is untiled (Xenos Tiled2D), endian-swapped and converted to RGBA8, then aspect-fit blitted.
- `host/src/settings.cpp`: reads settings.ini.
  - Presets: low = 1x/aniso 1, medium = 2x (2560x1440)/aniso 8, high = 3x (3840x2160)/aniso 16.
  - `--settings <file>`, `--headless`; `MADDEN_TEST_PATTERN=1` shows colour bars.
- Still to do: the Xenos draw pipeline (EDRAM/render targets, texture cache, shader translation; port from Xenia). Until then the window is black. The internal resolution scale applies once render targets exist.

## Draw renderer (first version)
- `host/src/xenos_shader.cpp`: translates Xenos microcode (control flow, ALU vector+scalar, vertex fetch, texture fetch) to GLSL.
  - glslang compiles the GLSL to SPIR-V at runtime.
  - `host/tools/shadertest.cpp` translates and compiles a folder of captured shaders (`MADDEN_GPU_CAPTURE=<dir>` dumps them).
- `host/src/gpu_vk.cpp`: the Vulkan backend.
  - Render targets: EDRAM render targets map to Vulkan images. MSAA surfaces are kept in sample space, so 4X clears and 1X surfaces at the same EDRAM base share one image.
  - Resolves: copy (and MSAA-average) into GPU images keyed by destination address. Texture fetches and the front buffer sample those directly; there is no CPU readback.
  - Textures: decoded from guest memory (mip 0, tiled/linear, endian swap; 8888, DXT1/3/5, DXN, DXT5A, 565, 4444, 1555, 16-bit and float formats, 24_8), cached by content hash.
  - Vertices: copied per draw into an upload ring; the shader fetches and decodes them (endian aware).
  - Rect lists use a geometry shader, quad lists become triangle lists, and primitive restart is supported.
  - Viewport: VTE handling follows Xenia (W0 reciprocal, XY/Z divided-by-W flags, D3D half-pixel offset).
- Depth-only (EDRAM mode 5) draws keep color 0 bound with writes off.
- Debug env:
  - `MADDEN_DUMP_FRAMES=<dir>`: writes every 30th frame as PPM.
  - `MADDEN_TRACE_FRAME=<n>`: per-draw trace for 8 frames.
  - `MADDEN_RENDER_DEBUG=nzbs`: disable stencil / depth / blend, or skip depth-only draws.
- Not done yet:
  - mip levels;
  - cube and 3D textures;
  - depth resolves;
  - memexport;
  - EDRAM aliasing between formats;
  - the D3D12 backend.

## Session notes: smoothness, mipmaps, audio (Nov 2026)

- **Shader/pipeline cache**: `shader_cache.bin` (SPIR-V keyed by a hash of the translated GLSL, so translator
  changes miss and recompile) and `pipeline_cache.bin` (driver VkPipelineCache, saved at most every 5 s), next to the exe.
- **Presentation**: one present per new guest frame (no fixed 16 ms timer). Large downscales (4K internal to a smaller
  window) halve step by step with 2:1 linear blits before the final scale.
- **Mipmaps**: guest mip chains incl. packed mip tails (Xenia texture_util layout). Stacked textures (jersey glyph
  sets) keep level 0 only; their per-layer mip layout is not decoded correctly yet. `mipmaps=false` disables.
  Sampler: fetch-constant mip filter + lod_bias. Shader: with use_computed_lod the register LOD is a *bias*
  (texture(..., bias)); without it, absolute (textureLod). Instruction mip filter "base map" means level 0.
- **Depth clip**: clip_disable (PA_CL_CLIP_CNTL bit 16) and rect lists clamp z to [0, w]; the name-card strip
  clear sat at z = -3.7e-9 and was being clipped.
- **Audio**: audio_out.cpp downmixes the 6-channel planar BE render-driver frames to stereo (WASAPI / SDL2); device
  consumption drives the game's audio clock (3 frames queued, backlog capped). xma.cpp decodes XMA with FFmpeg
  `xmaframes`, frame walking after Xenia's XmaContext; woken on every kick. `[audio] stats` every 10 s in the log.
- **Windows timer**: timeBeginPeriod(1) at startup. Without it every short guest wait rounded up to ~15.6 ms and
  audio fell behind.
- Settings keys added: `mipmaps`, `audio`, `volume` (0-100), `depth_of_field`.

## To do / requirements to remember

- **Depth charts for anything new.** Created players and new teams must get depth chart rows too. The on-field
  lineup is read from the match depth chart (DCGA) by team; a team without rows puts nobody on the field when its
  opponent also has none. `ModBuildDepthCharts` covers matches (builds one from the match players), but the dev tool's
  team/player creation should also write proper depth charts (without adding rows to the game databases: take over
  existing rows) so the game's own screens see them.
- ~~Hall Of Fame / Legends art~~ done (Nov 20): own uniforms, team select logos, large logos and scoreboard plates.
  How it works and the checklist for more teams: docs/MODDING_TEAMS.md.
- **Hall Of Fame / Legends stadiums.** Home games still use the Eagles' / Steelers' stadium (`<NICKNAME>_*`
  stadium and field textures, qkl_stream `EAGLES_big`, fe2ig field bundles).
- In-game lineup / player picker after team and difficulty select.
- The exception logged when quitting to the dashboard (exit button) happens after the window closes; harmless.
