# N+ (XBLA) on the Switch: port notes

nplus-nx starts from the renut-nx ultimate base (renut_nx_ultimate_base, Beautiful Katamari's port with every
port's fixes folded in), as docs/porting-a-360-game-prompt.md says, for N+ (2008, XBLA). Primary target: the Switch NRO. The PC build (Vulkan, MoltenVK on macOS) is the test step that
captures the shaders N+ compiles at run time.

Second attempt, on macOS (Apple Silicon). The first attempt ran on the Linux PC (the "n_plus switch port analysis"
Claude Code session); its repo was never committed or copied off that machine, so this one starts again from its
summary, recorded below.

## Inputs

- The N+ PC port's download (N_Plus.zip, a closed-source Windows build of the same game). Only its `data/` is used:
  - `data/Original/default.xex`: the XEX2 header alone (0x3000 bytes; file format info: encryption 1, compression 2).
  - `data/Original/NPlus.exe`: the decrypted, decompressed basefile (0x7C0000 bytes = the image size in the security
    info; starts with MZ).
  - `data/Extracted/Data0.zip`, `Data1.zip`: the game's content archives (visual effects, chapters and levels).
  - `data/DLC/*`: three DLC packs (Content.header + Data0.zip). Not wired up yet.
- `tools/nplus_make_xex.py` joins the header and the basefile into a loadable XEX: it sets the file format info to
  encryption 0 / compression 0 (the loader's ReadImageUncompressed then copies the image in) and appends the image.
  Nothing is decrypted. Output: `~/nplus/game/default.xex` (app/assets links to ~/nplus/game).
- No game data is committed (app/assets, app/generated, out/ are git-ignored).

## Why the ultimate base, not rehatched-nx

The first macOS attempt began from rehatched-nx (as the Linux one did) and was moved to the ultimate base on
2026-11-20: N+ is XDK 2.0.6274 (the XEX's static libraries: D3D9LTCG, XGRAPHC, XAPILIB ... all 2.0.6274), close to
Katamari's 2.0.5632, for which the base's Renut is written (device layout, hook forms), while rehatched-nx's Renut was
moved to Angry Birds' 2.0.9407. The base is also newer (it carries rehatched-nx's fixes, port 176). The rehatched-based
tree is kept at ~/nplus/nplus-nx-rehatched for reference.

## The device layout (Katamari -> N+)

N+'s CDevice is Katamari's with two 8-byte insertions; each boundary is bracketed by fields read in N+'s own code:
- Katamari < 13448: same. Checked: render targets 12432 (SetRenderTarget's (3108+i)*4), depth 12448, index buffer
  12428, textures slot 3134 (x4), streams 3113 / strides 12520, shaders 12684 / 12688, viewport 12640, scissor
  12668.., predication 12700, InKickOff 13216, tiling 12720.. / 13172, vertices ring 13428..13444, fence tag 10908,
  swap flags 10941, thread id 10888 / 10892.
- Katamari 13448..22251: +8. Export fields 13480.. -> 13488.. (the draws' sub_8227A970 reads 13488), CommandBuffer
  13492 -> 13500 (BeginCommandBuffer sub_82273D28: stores the buffer at 13500, zeroes 13504..13524 as the base's
  0x34B8..0x34CC), presentation params copy 13520 -> 13528, PresentInterval 13572 -> 13580, PresentFenceTag 14916 ->
  14924 and SwapCount 16536 -> 16544 (Swap), SideBuffer 16704 -> 16712, the -1 pair 21532/6 -> 21540/4,
  PerfFrequency 21560 -> 21568, TitleMinusOne 21672 -> 21680 (41 words), gamma pending 22244/8 -> 22252/6.
- Katamari >= 22252: +16. BehaviorFlags 22264 -> 22280.
- Allocation 0x5700 -> 0x5780 (Direct3D_CreateDevice: li r3,22400).
Applied by tools/nplus/device_offsets.py (the lines with katamari-nx's own remap note, plus the scaler copy at
0x3584; every other literal in range is a mask, alignment or time), each line marked `nplus: X->Y`.

## Guest addresses Renut reads

| | Katamari | N+ | evidence |
|---|---|---|---|
| VdGlobalDevice slot | 0x82000750 | 0x82000854 | IsFencePending sub_82277E58 |
| caps table | 0x820FC1F8 | 0x8208DDA0 | GetDeviceCaps sub_8227FCF8: memcpy(out, it, 304) |
| format table | 0x820FA9D0 | 0x82086688 | u16 bpp/flags per format, indexed by the texture helpers |
| full viewport | 0x820FAEB4 | 0x82086714 | {0, 0, 0xFFFF, 0xFFFF, 0.0, 1.0} |
| global qword (+10880) | 0x82AD93F8 | 0x82753990 | sub_822870F0 |
| HiZ owner | 0x8315E830 | 0x82775FC0 | Destroy sub_8226C148, CreateSurface sub_8227DAC8 |
| no large pages | 0x8315DFE0 | 0x827539B4 | XPhysicalAlloc sub_82318E08 clears 0x20000000 |
| render / sampler state tables | 0x82AD8E48 / 0x82AD9308 | 0x8274D980 / 0x8274DE40 | 101 and 20 {get, set, default}, 0x4C0 apart as Katamari's |
| XMemFree | sub_824CAB98 | sub_82315760 | CreateDevice's failure path (ptr, 0x24800000) |
| GetCurrentThreadId | sub_824C1C10 | sub_82315790 | its bytes, once in the image; the device init calls it |

## The hooks

app/config/nplus_hook_map.tsv: the base's 101 active named hooks, each with N+'s address or '-' and the evidence
(tools/nplus/hookmap.py; tools/nplus/prof.py, q.py, grepi.py, predict.py read the image and the call graph). 48 are
placed (the device, ring, fences, Present / Swap, draws, shaders, resources, state setters, tiling); 53 left to N+'s
own code (pure layout helpers, wrappers that end in the hooked LockResource / UnlockResource, functions LTCG compiled
into game code, and the device-init internals the hooked CreateDevice replaces). The 222 render / sampler state
getters and setters come from N+'s state tables. tools/nplus/hook_apply.py writes them into the hook files; then
tools/hook_arity_check.py (from rehatched-nx, with N+'s save / restore helpers skipped): 268 hooks, 2 flagged and read
by hand (SetTexture r3-r5, SetStreamSource r3-r7: the extra registers are never read). Then tools/post_codegen.py in full.
Flags forms: N+'s SetGammaRamp / SetPWLGamma take (device, flags, ramp) (InitializePresentationParameters calls them
with 0): bound to the base's _flags_hook functions.

## macOS build fixes (SDK)

- (rehatched-nx only: its FFmpeg configure outputs are git-ignored; the base commits them.)
- `sdk/src/audio/xma_context.cpp`: `__builtin_aarch64_crc32cx/cb` are GCC-only; Apple clang has none. Replaced by the
  ACLE intrinsics `__crc32cd` / `__crc32cb` (`<arm_acle.h>`), the same instructions under GCC (the Switch build).
- The host generator: `cmake -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release` in sdk/, then
  `cmake --build build-host --target rexglue` -> `sdk/out/mac-arm64/rexglue` (rpath is absolute: rebuild after moving
  the repo).
- On macOS the guest memory is a POSIX shm object, held in RAM: no 4.8 GB file on disk as on Linux's /dev/shm.

## What the first attempt found (to redo)

From the Linux session's summary; each item has to be found again with evidence here.

1. Code generation succeeded: 42,445 functions, all post-codegen passes, both calling-convention censuses clean.
2. Missing functions: the first PC runs crashed on functions the analysis missed. 257 were added, each checked
   (a blind import of a whole function table does not work: most extra entries are padding or data). The first
   attempt found them with the closed PC port's function table; this attempt finds them from the game's own code and
   the runtime's unregistered-call reports instead.
   One addition at 0x8271204C sat in the kernel import-thunk area at the end of .text (handled separately by the
   generator): additions in that range must be removed.
3. Direct3D layout: N+'s device matches Beautiful Katamari's (XDK 5632, the base's example game), not Angry Birds'
   (XDK 2.0.9407.0). The base's offsets apply, with N+'s device size, 0x5780.
4. The seven guest addresses the renderer reads, each N+'s own: the caps table, the format table, the viewport,
   the VdGlobalDevice slot, the HiZ flag, XMemFree, GetCurrentThreadId.
5. Renderer hooks: 41 placed (16 checked by hand: CreateDevice, Swap, KickOff, BeginRing, the shader and texture
   create functions, the main setters; 25 strong signature matches), all passing tools/hook_arity_check.py. Kept as a
   data file (app/config/nplus_hook_map.tsv) applied by a script. About 29 still unidentified: Clear, Resolve,
   SetViewport, the Lock/Unlock family.
6. Where it stopped: the PC build started, loaded its data, created its device through the renderer's hooks, started
   the render thread and captured 17 shaders, then a worker thread called a null pointer in sub_823DA2C0 (the JPEG
   decoder's error handler). Just before, the self-checks warned twice that a function returned with r1 608 bytes off:
   suspected the generator's stack-pointer analysis around libjpeg's setjmp/longjmp error path.

## Next, in order

1. Code generation on macOS; post_codegen; the calling-convention censuses.
2. The PC build on macOS (Vulkan through MoltenVK): get it to the point the first attempt reached.
3. Fix the JPEG / r1 crash; play past the menus until every shader is captured.
4. tools/switch_shaders.sh; the first Switch build (devkitA64 at /opt/devkitpro).
5. Hand-check the hooks; find the missing ones.

## macOS PC build fixes (app)

- `REX_ADDR_D`: the pch defines it only on platforms without a physical-window offset, but tools/float_loads.py's AArch64
  forms use it, and Apple Silicon (REX_PLATFORM_MAC && REX_ARCH_ARM64) is both. float_loads.py now adds a fallback
  with the plain macros' address (base + (u32)(r + d) + REX_PHYS_HOST_OFFSET); the Switch and Windows are unchanged.
- DXC for XenosRecomp: the base carries only dxc-bin's headers (its binaries were a git submodule,
  github.com/renderbag/dxc-bin, not committed). Its macOS universal `lib/arm64/libdxcompiler.dylib` and
  `bin/arm64/dxc-macos` (commit 12395e3, dxcompiler 1.8) are copied into
  app/thirdparty/reblue-XenosRecomp/thirdparty/dxc-bin (not committed: third-party binaries).
- PC build: `cmake -S app -B app/out/pc_host -G Ninja -DCMAKE_BUILD_TYPE=Release -DREXSDK_DIR=$PWD/sdk
  -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=OFF -DPython3_EXECUTABLE=~/nplus/.venv/bin/python`, then
  `cmake --build app/out/pc_host -j5` (16 GB: 5 jobs keep the large generated files in memory).

## First PC runs (macOS)

- Run 1 (`tools/nplus/run_pc.sh 60 first`): Vulkan on the M3 through MoltenVK, swapchain 2560x1440, guest memory mapped,
  the game mounted, guest code running; then `[FATAL] Call to invalid or unregistered function at guest address
  0x8238A258` (a one-instruction `blr` stub between two functions, reached through a pointer).
- The codegen config still had Katamari's per-game values:
  - `setjmp_address` / `longjmp_address` were Katamari's (0x827EB5D0 / 0x827EB3B0). N+'s: `_setjmp` sub_824839D0 (checks
    a hook, mflr / mfcr, saves f14-f31, r14-r31 and the VMX registers into r3; libjpeg's callers at 0x823BD.. 0x823DD..)
    and `longjmp` sub_824836B0 (val 0 -> 1, `mr r7,r3`, `lfd f14,0(r7)` ...: the same shape as Katamari's). The first
    attempt's JPEG crash (a null call in libjpeg's error path after r1 warnings) is likely this.
  - non_volatile_as_local, non_argument_as_local and elide_lr_stores carried Katamari's audits (and a keep list of
    Katamari's addresses): off for N+ until audited (docs/codegen-crash-fixes.md #3, tools/read_before_write.py,
    tools/non_argument_audit.py).
- Missing functions, found from N+'s own code (not the PC port's table): tools/pointer_fixpoint.py 67 (pointers stored
  in data or built in code, 0x8238A258 among them; fixpoint after one regeneration) and tools/gap_sweep.py 281
  (complete functions in unclaimed gaps; fixpoint on pass 2): 348 in app/overrides.toml, none in the import-thunk
  area. Both tools now call tools/codegen.sh where there is no PowerShell.
- Run 2 (after the 348 functions and setjmp / longjmp): 90 s at 60 fps, audio, every generated-code self-check 0
  differences (the r1 mirror included: the first attempt's r1 warnings are gone), 17 shaders captured (the first
  attempt's count); every draw skipped for "no host shader" (expected before the shaders are built in); a grey frame
  (the clear colour: clears and resolves work through the hooks).
- Run 3 (the 17 shaders built in): the main menu draws correctly, attract level behind it; 125k-208k draws / 10 s, 0
  new shaders. "UNLOCK FULL GAME" on the menu: N+ believes it is the trial (the license the kernel reports).
- Run 4 (app/out/pc_host/renut.toml's input_script into Single Player): gameplay, the timer, "OUT OF TIME! Press A
  to retry" and the HUD draw; still 0 new shaders (N+ seems to use about 17 in all). Open:
  - mid-level frames looked half drawn: not a fault. N+'s gameplay camera zooms in on the ninja and solid level
    geometry is flat grey (the floor and walls); "OUT OF TIME" zooms out to the whole level. Checked with a temporary
    log in the BeginTiling hook: 4 tiles of 320x720 (4x MSAA), Katamari's argument form (r4 flags 0, r5 count stored
    at 12740 = TileCount 0x31C4, r6 rects, r7 colour, f1 Z), the bound target 0xBBCA0370 and depth 0xBBCA03F0.
  - 120 fps from about 50 s in: N+ paces itself only by Present's vsync wait, which stops when the macOS window is
    covered. frame_cap now defaults to "60" (app/src/katamari_keys.cpp; N+ steps its physics per frame). Run 4's
    route again (cap1, 150 s with the random sticks): 59.3-60.0 fps in every 10 s window. The Switch tomls must not
    set frame_cap = "Off" (the Katamari ones do).
  - the trial: the SDK's license_mask (XamContentGetLicenseMask) defaulted to 0; now 1 (bit 0, purchased:
    sdk/src/kernel/xam/xam_content.cpp). The main menu's item 7 is "DOWNLOAD CONTENT" instead of "UNLOCK FULL GAME".
- Not wired up yet: the three DLC packs of the download (data/DLC/<id>/Content.header + Data0.zip).

## The Switch build (macOS host)

- Shaders: tools/nplus/switch_shaders.sh (switch_shaders.ps1's steps; Homebrew's spirv-cross and the PC uam linked
  under out/deko3d_tools/*/*.exe, Homebrew's zstd): 17 shaders, 17 compiled (9 vs, 8 ps), 0 failed -> app/out/renut_switch.
- deko3d only (REXGLUE_SWITCH_NVK OFF, the default: no patched Mesa needed).
- N+'s identity: RENUT_GAME_NAME "N+", author slicey64, version 0.1.0, /switch/nplus-nx/nplus-nx.nro, the player's
  nplus.toml (app/nplus_switch.toml: frame_cap "60", no katamari_dlc_unlock_packs; app/nplus_switch_defaults.toml
  as Katamari's), no icon yet (tools/switch/icon/nplus_icon.jpg).
- dxc-macos finds libdxcompiler through its rpath @executable_path/../lib: dxc-bin/bin/lib -> ../lib/arm64 (a link,
  not committed with the binaries); gpu.cmake passes the plain path (tools/deko3d_host_shader.py takes one executable).
- Build: `cmake -S app -B app/out/switch -G Ninja -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_TOOLCHAIN_FILE=$PWD/tools/switch/cmake/switch-devkitA64.cmake -DREXSDK_DIR=$PWD/sdk
  -DPython3_EXECUTABLE=~/nplus/.venv/bin/python -DBANJO_GLSLANG_VALIDATOR=$PWD/out/glslang/StandAlone/glslangValidator`
  (DEVKITPRO=/opt/devkitpro), `cmake --build app/out/switch -j4`: app/out/switch/renut.nro, 47.7 MB (about 7 minutes).

## Ryujinx (macOS)

- N+'s own data folder ~/nplus/ryu-data/nplus (copies of the keys, firmware and Config.json, its own SD card with
  /switch/nplus-nx/game_root), tools/nplus/run_ryujinx.sh. The user's Ryujinx is 1.3.3 (logs) in
  ~/Downloads/Ryujinx.app.
- emu1 / emu2: the NRO starts deko3d (swapchain 1280x720, GPU region) and then cannot map the guest memory:
  `AllocFixed: could not commit ... Failure 0xE401`, then Ryujinx dies (Apple Hypervisor backend:
  InvalidMemoryRegionException; the JIT backend: the same refusal). Ryujinx's log: `MapProcessCodeMemory() =
  InvalidHandle`. Without hbloader there is no process handle, and the SDK passes the current-process pseudo-handle
  (guest_memory_switch.cpp Proc(), banjo-nx's), which Horizon's kernel accepts; stock Ryujinx looks the handle up
  without resolving pseudo-handles. The base's ports were tested on "the user's patched Ryubing"
  (docs/katamari-notes.md), which accepts it. Not an N+ fault: a console (hbloader's handle) or a patched Ryujinx.
- N+'s Ryujinx: Ryubing 1.3.3 (git.ryujinx.app/ryubing/ryujinx, tag 1.3.3) with tools/nplus/ryujinx-1.3.3-nplus.patch,
  in ~/nplus/ryujinx-src/ryujinx (branch nplus-pseudo-handle), built with Homebrew's dotnet@9
  (`dotnet build src/Ryujinx -c Release -r osx-arm64`; DOTNET_ROOT=$(brew --prefix dotnet@9)/libexec). Its nuget.config
  points LibHac at the moved feed (git.ryujinx.app/api/packages/projects/nuget) and takes the update client from
  nuget.org (the old project feeds answer 404). The two changes, both what the Switch does:
  1. SetProcessMemoryPermission, Map/UnmapProcessMemory and Map/UnmapProcessCodeMemory resolve the current-process
     pseudo-handle (GetKProcess, as the kernel's GetObject<KProcess>); stock Ryujinx answered InvalidHandle.
  2. Bit 31 of the vertex attribute word (Maxwell's BGRA swap, deko3d's isBgra) selects B8G8R8A8Unorm for 4x8 unorm
     attributes; stock Ryujinx drops it, and N+'s D3DCOLOR vertex colours came out red / blue exchanged (a cyan timer
     bar, a blue ninja).
  Run it with RYUJINX=~/nplus/ryujinx-src/ryujinx/src/Ryujinx/bin/Release/net9.0/osx-arm64/Ryujinx tools/nplus/run_ryujinx.sh.
- sdk/src/ui/switch_code_blocks.cpp: without a loader (an emulator) the swap self-test is skipped. With the pseudo-handle
  accepted, its page mapped and the jump into it landed on code Ryujinx's JIT never saw (a fault in
  __libc_init_array before the log opened). The console (the loader's handle) never takes this path.
- Ryujinx's JIT backend fails the vperm (perm_epi8) instruction-form self-check (TBL); the Apple Hypervisor backend
  (use_hypervisor true in N+'s data folder's Config.json) runs the code natively: 45134 of 45134 forms as expected.
- emu5-emu7 (patched Ryujinx): the boot, the menus, "who dat ninja" (Episode 0, Level 0) and gameplay as on the PC,
  57-60 fps after the first seconds; colours as the PC's after patch 2.

## DLC (Levelpacks 1-3)

- The download's data/DLC/<package>/ are extracted packs: Content.header is one XCONTENT_AGGREGATE_DATA (328 bytes:
  device 1, type 2 marketplace, "N+ Levelpack N", the package name, xuid 0, title 5841085C; no license word) beside
  the package's Data0.zip.
- tools/nplus/install_dlc.sh <data/DLC> <user-data root> puts them in the SDK's content layout:
  <root>/0000000000000000/5841085C/00000002/<package>/ and <root>/0000000000000000/5841085C/Headers/00000002/<package>.header.
  The roots: the PC's --user_data_root (out/pc_user in tools/nplus/run_pc.sh); the Switch's
  sdmc:/switch/nplus-nx/renut (the SDK's user folder / the app's name), so on a console the same files go in
  /switch/nplus-nx/renut/0000000000000000/5841085C/... (and on N+'s Ryujinx SD card the same).
- PC (dlc1-dlc3) and Ryujinx (emu_dlc): the three packs listed, opened and read; Single Player's categories show
  N+ LEVELPACK 1, 2 and 3; Levelpack 1's episodes 00-25 and its first level ("we're sorry! to all those stuck on
  'Master Control'") load and play. N+ needs no license word in the headers.
- The base's own DLC installer (renut_engine/dlc_install.cpp: STFS packages in a "dlc" folder next to the NRO) is for
  original packages; the extracted ones above go straight into the layout.
