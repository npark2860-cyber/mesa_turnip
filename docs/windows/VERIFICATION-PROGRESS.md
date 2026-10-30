# Turnip Windows (WDDM) driver — static verification progress

Resumable checklist for the static verification / bug-fix / performance pass
over the Windows-specific paths of this fork. A fresh session should read this
file first, then continue from the first unchecked item.

Branch: `claude/project-thread-btvgxr`
Baseline: commit 16f87fb ("freedreno: normalize Windows paths in the XML include check")
Policy-note: biblioklept

## Scope

Pass 1 covered the Windows-specific code listed below. Pass 2 (after the
user asked for the whole driver to be reviewed) covered all of `src/freedreno`
(ir3 compiler, fdl layout/tiling, common, vulkan) with the static analyzers
listed under "Whole-driver static analysis".

### Pass 1: Windows-specific code

- `src/freedreno/vulkan/tu_knl_wddm.cc` — WDDM backend (sync objects, BO alloc,
  VA management, submit, present interop, timestamps, adapter enumeration)
- `src/vulkan/wsi/wsi_common_win32.cpp` — DXGI presentation (buffer blit,
  D3D12 copy, present timing)
- Windows ports in `tu_device.cc`, `tu_queue.cc`, `tu_knl.cc`, `tu_util.cc`,
  `tu_shader.cc`, `tu_cs_breadcrumbs.cc`
- Perf commits reviewed: 9ccec68 (compute buffer copy), c00f23d (batched
  2D buffer blits), 977157a (host-coherent cache sync skip)

## How the checks were run (reproducible)

1. Native Linux build (validates all shared code touched by the port):
   `meson setup build-linux -Dgallium-drivers= -Dvulkan-drivers=freedreno
   -Dfreedreno-kmds=msm,kgsl -Dplatforms=x11 -Dglx=disabled -Degl=disabled
   -Dopengl=false -Dllvm=disabled -Dtools= -Dperfetto=false`
   then `ninja -C build-linux src/freedreno/vulkan/libvulkan_freedreno.so`.
   Result: success, only the 5 pre-existing upstream `asprintf` warnings.
2. Windows syntax check of `tu_knl_wddm.cc` and `wsi_common_win32.cpp`:
   take the `tu_knl_kgsl.cc` command from `compile_commands.json`, swap the
   compiler for `x86_64-w64-mingw32-g++` (and `clang++ --target=x86_64-w64-mingw32`
   / `--target=aarch64-w64-mingw32`), drop Linux-only `-D` flags, add
   `-DTU_HAS_WDDM -DVK_USE_PLATFORM_WIN32_KHR -DTU_USE_WSI_PLATFORM`, point an
   `-isystem` at a directory holding the Windows SDK `d3dkmthk.h`, `d3dkmdt.h`,
   `d3dukmdt.h` (from `microsoft/win32metadata`, `generation/WinSDK/RecompiledIdlHeaders/shared/`)
   plus stub `zlib.h`, `directx/d3d12.h`, `dxguids/dxguids.h`, and run with
   `-fsyntax-only`.
   Result: both files compile cleanly on gcc-mingw, clang x86_64 and clang
   ARM64 targets; the only diagnostics are `-Wunused-result` on D3DKMT
   destroy calls (harmless).

## Checklist

- [x] 1. Survey repo, identify Windows-specific files and port commits
- [x] 2. Native Linux build of turnip
- [x] 3. mingw-w64 / clang syntax check of the two Windows files (x86_64 + ARM64)
- [x] 4. Code review of WDDM backend
- [x] 5. Code review of Win32 WSI DXGI path
- [x] 6. Apply bug fixes
- [x] 7. Apply performance improvements
- [x] 8. Re-run build + syntax checks after changes (all pass)
- [x] 9. Commit, push, open PR (https://github.com/npark2860-cyber/mesa_turnip/pull/1)
- [x] 10. Whole-driver static analysis (clang -Wshadow/-Wextra build, clang-tidy
      clang-analyzer + bugprone over all 107 freedreno sources, clang-tidy over
      the two Windows files with the mingw clang flags); triage of all 526
      findings; fixes F5/F6 committed
- [x] 11. mingw-w64 cross build of the whole Windows driver (gcc 13, LLP64) to
      reproduce the warnings seen in the native PC build; found and fixed the
      two pointer-to-int casts (F7), identified the macro redefinition (F6)
- [x] 11b. Locate the remaining PC-build warnings from the clang build log:
      the 5 bitfield-constant-conversion warnings are a real MS-ABI bug in
      `shader_info.h` (F8); invalid-noreturn, missing-braces and the
      `#pragma message` are benign (see "Benign PC-build warnings")
- [x] 12. Independent cross-review of the PR diff (three reviewer agents with
      distinct lenses, each finding then handed to two refuters). The WDDM
      submit lens and the shared-code lens completed and reported two
      follow-ups, both fixed (F9, F10). The presentation/performance lens
      ran after the usage limit reset and reported the acquire wait-target
      policy; both refuters classed it as a pre-existing performance
      property rather than a defect of this diff, and it is improved as F11
- [ ] 13. Hardware validation by the user (Snapdragon X Elite / Eden) of the
      items marked NEEDS-HW-TEST below

## Findings

### FIXED in this branch

- F1. `wddm_queue_submit`: one `D3DKMTWaitForSynchronizationObjectFromGpu`
  kernel call per wait semaphore and one
  `D3DKMTSignalSynchronizationObjectFromGpu2` per signal semaphore. Now
  batched into chunks of `D3DDDI_MAX_OBJECT_WAITED_ON` /
  `D3DDDI_MAX_OBJECT_SIGNALED` (32), and the queue completion fence rides in
  the same signal batch. Saves (waits + signals) kernel transitions per
  vkQueueSubmit. NEEDS-HW-TEST.
- F2. `wddm_queue_submit`: the per-IB size check ran after the GPU waits had
  already been enqueued and binary-semaphore targets bumped, so an oversized
  IB left the queue in an inconsistent state. All validation now happens
  before any kernel call.
- F3. `wddm_queue_submit` / `wddm_bo_init`: `debug_get_bool_option` /
  `debug_get_num_option` (which call `getenv`) ran on every submit and every
  allocation. Cached in function-local statics.
- F4. `wsi_common_win32.cpp` surface capabilities: `maxImageCount` was pinned
  to 2 for every DXGI swapchain. The buffer-blit path (the one Turnip uses:
  Vulkan image -> shared buffer -> D3D12 copy into the current back buffer)
  does not have the acquire-order restriction, so it now reports
  `minImageCount = 2, maxImageCount = 4`, letting Eden triple-buffer instead
  of being forced into double buffering. NEEDS-HW-TEST (frame pacing / FPS).
- F5. `src/freedreno/common/freedreno_rd_output.c` `fd_rd_output_fini`: freed
  `output->name` and then used it to build the trigger file path for
  `unlink` (use after free; only reached with `FD_RD_DUMP` trigger mode at
  device teardown). Found by clang-analyzer (unix.Malloc). The free now
  happens after the path is built. Present in upstream Mesa as well.
- F6. `tu_query_pool.cc` defined `WAIT_TIMEOUT`, which collides with the
  Windows SDK `WAIT_TIMEOUT` (0x102) macro and produced the
  `-Wmacro-redefined` warning in the PC build. The local definition came
  last so the 5 s value was in effect, but it depended on include order.
  Renamed to `TU_QUERY_WAIT_TIMEOUT_SEC`.
- F7. `src/freedreno/common/freedreno_common.h` `U642VOID` / `VOID2U64` and
  `ir3.c` `block_id()` cast pointers through `unsigned long`, which is 32
  bits on Windows (LLP64): the `-Wpointer-to-int-cast` pair from the PC
  build. `VOID2U64` is expanded in `freedreno_perfcntr.c`, which is linked
  into the Windows driver; the truncated pointer only ever reaches the msm
  DRM ioctl (not compiled on Windows), so no crash today, but the cast is
  now `uintptr_t`, which is correct on every ABI.
- F8. `src/compiler/shader_info.h`: `derivative_group:2`, `depth_layout:3`,
  `stencil_front_layout:3`, `stencil_back_layout:3` (and `hash_type:2`) are
  bit-fields of enum type. Under the Microsoft ABI (the native PC build:
  clang targeting `aarch64-pc-windows-msvc`) an enum bit-field is signed, so
  `DERIVATIVE_GROUP_LINEAR` (2) reads back as -2 and
  `FRAG_DEPTH_LAYOUT_UNCHANGED` / `FRAG_STENCIL_LAYOUT_UNCHANGED` (4) read
  back as -4; these are exactly the five `-Wbitfield-constant-conversion`
  warnings of the PC build (`nir_lower_system_values.c:833`,
  `spirv_to_nir.c:5972/6135/6269/6284`). Consequence on Windows:
  `ir3_nir.c` never sees `DERIVATIVE_GROUP_LINEAR`, so compute shaders using
  `DerivativeGroupLinearKHR` were not dispatched linearly (wrong derivative
  results); `depth_layout` falls into the "unchanged is always OK" branch of
  `tu_lrz.cc` by accident, so no visible effect there. Each field is now one
  bit wider so the largest enumerator fits even when the field is signed.
  Linux (GCC/Itanium ABI, unsigned enum bit-fields) is unaffected either
  way. shader_info is only serialized into caches keyed by the driver build.
  Verified with a 5-line test (`enum e {A,B,C}; struct { enum e f:2; }`):
  clang 18 `--target=aarch64-pc-windows-msvc` and `x86_64-pc-windows-msvc`
  compile `x.f = C; return x.f == C;` to a constant 0 with the 2-bit field
  and to 1 with a 3-bit field; `x86_64-w64-mingw32` and `x86_64-linux-gnu`
  return 1 in both cases.
- F9. `src/freedreno/drm/freedreno_priv.h` carried its own copies of
  `U642VOID` / `VOID2U64`; after F7 they no longer matched the
  `freedreno_common.h` definitions it includes, which would have produced
  `macro redefined` warnings (an error under Mesa CI's `-Dwerror=true`) in
  every `src/freedreno/drm` translation unit on Linux. Found by the
  cross-review; the duplicates are removed. (`src/freedreno/drm` is not
  built for Windows, which is why the Linux turnip-only build and the mingw
  build did not show it.)
- F10. `wddm_queue_submit`: F2 moved the per-IB size check ahead of the GPU
  waits, but the `TU_WDDM_IB_LIMIT` diagnostic truncation stayed after them,
  so an oversized single IB was rejected before the debug knob could shorten
  it (pre-change order was truncate, then check). Found by the cross-review;
  the truncation now runs right after the IB-count check and before the
  per-IB validation, restoring the original semantics.
- F11 (performance, pre-existing policy). `wsi_common_win32.cpp` buffer-blit
  acquire: when no image was free, `wsi_win32_acquire_next_image` waited on
  the lowest-indexed busy image for the whole timeout. The D3D12 copies
  retire in present order on one queue, so when the lowest index was the
  most recently presented image the acquire drained every frame in flight
  instead of only the oldest one, and with a finite timeout it could return
  VK_TIMEOUT while an older image had already become free. This existed with
  2 images too. The cross-review's refuters confirmed it is not a defect of
  F4: in their discrete-event simulation of the old policy (CPU frame 1.0,
  GPU frame 1.2 / 1.6 / 2.5 / 4.0) GPU idle time was 22.6 / 17.9 / 12.3 /
  8.0 % with 2 images, 11.1 / 14.1 / 9.5 / 6.2 % with 3 and 0.9 / 7.6 / 7.7
  / 5.0 % with 4, so 3-4 images already beat the forced 2, while an
  oldest-presented policy reaches 0 %. Each image now records a chain-wide
  present sequence number when its copy is queued, and the acquire waits on
  the busy image with the smallest one, the one whose copy completes first.
  NEEDS-HW-TEST together with F4.

### Reviewed, OK as-is

- Timestamp readback in `wddm_timestamp_sample` is 64-bit: the
  `CP_ALWAYS_ON_COUNTER(CHIP)` macro expands to the two-register overload of
  `tu_cs::reg_to_mem`, which sets `cnt = 2, is_64b`.
- Binary vk_sync emulation (`target` counter, bumped on reset and after a GPU
  wait) is consistent across init / signal / wait / move.
- Retired-BO list (`wddm_collect_retired`) indexes `completions[q]` with the
  length captured at retire time; `completions` only ever grows, so indexing
  stays valid after queues are closed.
- Heap accounting: `tu_device.cc` adds/subtracts `heap.used` generically;
  `wddm_present_buffer` adds explicitly because it bypasses `tu_bo_init`, and
  `_tu_destroy_memory` subtracts. Balanced.
- Compute buffer-copy path (9ccec68) saves/restores compute shader state,
  load state and push constants around `vk_meta_copy_buffer`, and marks
  `TU_CMD_DIRTY_SHADER_CONSTS | TU_CMD_DIRTY_COMPUTE_DESC_SETS`. Matches what
  `tu_CmdBindPipeline` touches for compute.
- Mutex/condvar port (C11 `mtx_t` / `cnd_t`) is complete; no remaining
  `pthread_*` uses in Windows-compiled paths.

### Whole-driver static analysis (pass 2) — how it was run and what it found

1. clang 18 build of the Linux turnip with `-Wshadow -Wextra` added
   (`build-clang`): 266 warnings, all pre-existing `-Wshadow` in freedreno
   plus one deprecated `reg_builder` pragma. No new warnings from the fork.
2. `run-clang-tidy-18 -p build-clang` over all 107 `src/freedreno` sources with
   `-checks=-*,clang-analyzer-*,bugprone-*` minus the style-only bugprone
   checks (easily-swappable-parameters, narrowing-conversions,
   reserved-identifier, macro-parentheses, branch-clone, implicit-widening,
   assignment-in-if-condition, switch-missing-default-case,
   suspicious-include, misplaced-widening-cast, signed-char-misuse,
   multi-level-implicit-pointer-conversion, casting-through-void,
   not-null-terminated-result, unchecked-optional-access, empty-catch,
   optin.performance.Padding, security.insecureAPI). 526 findings:
   - 303 `optin.core.EnumCastOutOfRange`: all intentional flag-combining or
     `~0` sentinel casts (`enum tu_bo_alloc_flags`, `gl_access_qualifier`,
     `nir_variable_mode`, `enum global_shader`, ...). No action.
   - 100 `core.NullDereference`: analyzer paths through `ir3_get_dst`,
     `VK_FROM_HANDLE` null handles, `subpass->pDepthStencilAttachment`
     when a DS resolve is present (spec guarantees non-NULL),
     `image_view_info` when cubic props are requested (spec VUID), the
     `attachments` array with zero attachments, the `mem == NULL` branch in
     `tu_BindBufferMemory2` (memory is a required handle in vk.xml). No
     action.
   - 57 `bugprone-sizeof-expression`: `sizeof(struct x *)` for arrays of
     pointers and `ARRAY_SIZE` on `DRAW_REGS<CHIP>` templates. No action.
   - 12 `non-zero-enum-to-bool-conversion`: `tu_cmd_flush_bits` tests. No
     action.
   - 10 `deadcode.DeadStores`, 10 `CallAndMessage`, 7
     `UndefinedBinaryOperatorResult`, 7 `NonNullParamChecker`, 4
     `uninitialized.Assign`, 2 `uninitialized.Branch`, 1 `DivideZero`, 1
     `VLASize`, 1 `UnixAPI`: VLA/array fills the analyzer cannot follow
     (`srcs[]`, `bs[]`, `srcs_rpt[]`, `comps[]`), `memcpy`/`qsort` with a
     NULL pointer and zero size, `% file_size` with a non-empty register
     file. No action.
   - 4 `suspicious-string-compare` / 2 `suspicious-memory-comparison`:
     `memcmp` on `struct ir3_legalize_state` and `regmask_t`, compared
     against a copy of the same struct so padding bytes match. No action.
   - 2 `bugprone-integer-division` in `tu_subsampled_image.cc` (the bin start
     divided by the fragment area before conversion to float): upstream
     fragment-density-map code, bins are fragment-area aligned, not used by
     Eden. Left as is.
   - 2 `unix.Malloc`: `tu_rmv.cc` `pool_sizes` ownership passes to the RMV
     token (no leak); `freedreno_rd_output.c` use after free -> F5.
3. clang-tidy with the same checks over `tu_knl_wddm.cc` and
   `wsi_common_win32.cpp` using the mingw clang command from the syntax
   check: 4 findings, all paths excluded by the callers (`wddm_bo_map` is
   only called with a live BO; the DXGI swapchain path is only entered when
   `get_d3d12_command_queue`, the DXGI factory and the DComposition device
   exist, and `surface->current_swapchain` is only set on that path).
4. Cosmetic, not changed: `tu_device.cc` `sizeof(struct
   tu_debug_bos_entryos_entry *)` typo in the debug BO statistics (pointer
   size is the same, upstream has the same text); `tu_shader.cc`
   `tu_upload_shader` tests `v` for NULL once and then dereferences it
   unconditionally.

### Benign PC-build warnings (native clang 22 build, not changed)

- `-Winvalid-noreturn` at `src/c11/impl/threads_win32.c:347`: `thrd_exit` is
  `_Noreturn` and ends in `_endthreadex`, which the UCRT headers do not mark
  noreturn; `_endthreadex` never returns, so nothing happens.
- `-Wmissing-braces` at `wsi_common_win32.cpp` (line 1245 of the patched PC
  source): brace style of a nested aggregate initializer.
- `-W#pragma-messages` "Deprecated reg builder" at `tu_cmd_buffer.cc:2265`:
  an upstream reminder pragma.
- 201 `-Wdeprecated-declarations`: POSIX names (`strdup`, `getenv`, ...)
  under the MSVC CRT.

### mingw-w64 cross build (pass 2) — how it was run and what it found

`apt install libz-mingw-w64-dev`, then a meson cross file for
`x86_64-w64-mingw32-gcc`/`g++` (`system = 'windows'`, `pkg_config_libdir =
/usr/x86_64-w64-mingw32/lib/pkgconfig`), the PC build's meson options
(`-Dplatforms=windows -Dvulkan-drivers=freedreno -Dfreedreno-kmds=wddm
-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dllvm=disabled
-Dbuild-tests=false -Dperfetto=false -Dc_args=-D_USE_MATH_DEFINES
-Dcpp_args=-DNOMINMAX`) and `-isystem` pointing at the win32metadata
`d3dkmthk.h`/`d3dkmdt.h`/`d3dukmdt.h` copies. `ninja -k 0` compiled 689 of 693
objects, including `tu_knl_wddm.cc`, `wsi_common_win32.cpp`, `tu_device.cc`
and every ir3/fdl/common file. The four that did not: the DirectX-Headers
`dxguids.cpp` (mingw's own older `d3d12.h` shadows the subproject's, an
environment issue), the `ir3_disasm` / `ir3_delay_test` test programs
(`fmemopen`, `err.h` are POSIX-only; they are not part of the driver), and the
final DLL link that depends on them. Warnings: 2 `-Wpointer-to-int-cast` (F7),
63 `-Waddress` on `if (ir3_assert_handler)` (the handler is non-weak on
Windows so the test is always true; harmless), 1 `PACKED` redefinition
between `instr-a3xx.h` and the mingw-specific `gcc_struct` variant in
`util/macros.h` (mingw only; the PC clang build defines both identically),
1 unused variable in `nir_validate.c` (upstream).

### Hardware-test builds (native clang 22, MSVC target, Windows ARM64 PC)

Both builds use the user's usual Turnip recipe (clang `--target=aarch64-pc-windows-msvc`,
`-fms-runtime-lib=dll`, `--buildtype=release -Db_ndebug=true`) with the
SnapRYU patches 0001-0007 applied on top of this branch. Each build lives in
its own new folder under `opus-spot\`; nothing existing was modified.

- `turnip-fable-pr1-release\` — source 40f8ba7 + 0001-0007.
  `vulkan_freedreno.dll` 13,324,288 bytes, SHA-256
  F5A869E3AB3DFF74C8A6D629147EAE71A540F50CEB2A5EBC06CA9A8765BB3FA6.
- `turnip-fable-pr1b-release\` — source 3dce589 + 0001-0007 (patch 0006
  needed a manual 3-way merge in `wsi_win32_copy_to_dxgi`: the F11
  `present_seq` line is kept ahead of the SnapRYU `if (verify)` block; the
  applied patch is saved next to the source as
  `applied-snapryu-0001-0007-on-3dce589.diff`, 3065 lines).
  `vulkan_freedreno.dll` 13,325,824 bytes, SHA-256
  4C99BC7774A0E42E519A7989D78BEF17C5387E318BDA8D1ED2A17CB4547D04A3.
  Compile + link 68.5 s (710 ninja steps). `build-info.json` records the
  source commit, the patch list and the hand merge.
  2026-10-29: the user bundled this DLL into the SnapRYU z8 and z8d builds
  as the driver under hardware test.

- `dist-static-20261030\turnip-pr1b-static\` — same source as pr1b (3dce589 +
  0001-0007, tree unchanged), built for the public SnapRYU release with a
  static CRT so the package needs no VC++ redistributable: the pr1b script
  plus `-Db_vscrt=mt` (clang gets `-fms-runtime-lib=static`) and
  `-Dc_link_args=-Dcpp_link_args=-Wl,/nodefaultlib:msvcrt`, which is what
  the b110 public build did. `vulkan_freedreno.dll` 13,581,312 bytes,
  SHA-256 CB5658821A118681A85F3C95938322649DBFD5ED63674FC64339A066AA1BD378,
  compile + link 78.2 s. Import table: KERNEL32, USER32, GDI32,
  api-ms-win-core-synch-l1-2-0 only, identical to the b110 static DLL; the
  dll-runtime pr1b DLL additionally imported MSVCP140, VCRUNTIME140 and 10
  api-ms-win-crt-* DLLs. All 579 compile commands carry `-DNDEBUG -O3
  -fms-runtime-lib=static`; link rule has `/nodefaultlib:msvcrt` and no
  `/nodefaultlib:libcmt`; PE machine ARM64; no `_wassert`, no assert text,
  no UTF-16 source paths; 0 errors, 204 compiler warnings (201
  deprecation, the known benign ones, and 0 of the three fixed flags).
  `SNAPRYU_REGSPILL_DIAGNOSTIC` is at its header default 0 in both pr1b
  builds. Not yet run in a game itself.

Checks run on the pr1b build: `meson-setup.log` shows `buildtype : release`
and `b_ndebug : true`; all 579 commands in `compile_commands.json` carry
`-DNDEBUG` and `-O3` (none `-O2`/`-g`, 561 also `-DMESA_DEBUG=0`) and all
point at the pr1b source tree; the DLL contains no `_wassert` import, no
"Assertion failed" string and no UTF-16 freedreno source paths (0 hits for
`ir3_ra.c`, `tu_cs.h`, `ir3_spill.c`), same as the 40f8ba7 release DLL.
Errors: 0 (DLL linked; a second, no-op run of the script completed with 0
errors / 0 warnings). Gap: the full build's compiler log was overwritten by
that second run, so the per-flag warning counts (the
`bitfield-constant-conversion`, `pointer-to-int-cast` and `WAIT_TIMEOUT`
macro-redefinition warnings are expected to be gone after F6/F7/F8) were
not re-counted from compiler output; the source carries the fixes. To
re-count, remove `turnip-fable-pr1b-release-build\meson` and run the
script once more.

Hardware test log (item 13):

- 2026-10-29 18:42 UTC, SnapRYU S3z8d + pr1b DLL, Tears of the Kingdom: a
  stall of about half a minute shortly after launch, then the game resumed.
  The Eden log shows "Total Pipeline Count: 0" followed by 853 shaders
  compiled over 32 s and no error or device loss: the driver build changed,
  so the Vulkan pipeline cache was rebuilt. Expected on the first launch
  with every new DLL (the pipeline cache UUID follows the driver build);
  measure performance from the second launch on.
- 2026-10-29 18:50 UTC, same setup after the cache rebuild: the user reports
  Tears of the Kingdom holding 60 fps on screen almost regardless of camera
  angle; with the previous driver DLL, looking down always dropped it to
  about 40. The 60 is the LSFG (frame generation) output rate: the native
  game rate in the Eden log is 30.02 fps, which is the game's cap, at 71%
  GPU load; the earlier 40 corresponds to roughly 20 fps native
  (estimate). Native figures of 21-29 fps also appear in the log. Same
  device and SnapRYU build; no hang or device loss so far; the 1-hour
  stability run is pending.
- Attribution of the gain (analysis, no A/B test): the buffer-blit
  path copies each presented image into the current DXGI back buffer on
  the D3D12 queue, and the image becomes acquirable only once that copy
  has executed; the copy in turn waits in the D3D12 queue until DWM hands
  the back buffer back (vsync cadence). With two images the render thread
  blocks on that every other frame and the GPU idles; with three (F4) the
  wait is hidden, and F11 makes the wait target the oldest copy. That
  matches a GPU-bound scene going from about 20 fps to the 30 fps cap at
  71% GPU load. The other changes cannot explain it: F1 saves a few kernel
  calls per submit (well under 1 ms per frame), F8 only affects compute
  shaders with `DerivativeGroupLinear`, which Eden does not emit, F7's
  macros are not used on the WDDM path, the rest are correctness fixes.
  An A/B check (the 40f8ba7 release DLL, or a build with `maxImageCount`
  back at 2, on the same scene) would confirm it; the user chose not to
  run one, so this stays the likely cause, unconfirmed. The comparison is
  also only fair if the Qualcomm KMD/driver package version was the same
  for the "before" measurement.

- 2026-10-30 03:18 UTC, Bayonetta 3 1.2.0 cutscenes on SnapRYU 1.1 (S3z8 +
  the pr1b dll-runtime DLL, SHA 4C99BC77): character skin renders as a
  flat saturated violet wash with its texture and shading gone, with fine
  1-2 px bright speckles/streaks that follow the surface curvature in lit
  areas; cloth (cap) turns magenta but keeps its fabric texture; hair,
  metal, background and UI are correct. The Qualcomm driver renders the
  same cut clean. Across 6 consecutive captured frames the pattern is
  surface-locked and identical, so it does not look like a submit/acquire
  race (F1/F2/F4/F11), and the shader_info fix (F8) is excluded because
  Eden's shader recompiler emits none of the execution modes those fields
  carry (no DerivativeGroup, DepthUnchanged/Greater/Less or
  StencilRef*Layout anywhere in its tree; DepthReplacing maps to a value
  the old field already held). Open lead: Turnip reports
  VK_EXT_4444_formats / formatA4B4G4R4 unsupported while Qualcomm supports
  it, so A4B4G4R4 textures take Eden's fallback path only on Turnip. Log:
  Mesa 26.3.0-devel git-3dce589585, Adreno X1-85, cold pipeline cache, no
  errors, LSFG frame generation on. Pending: one A/B run of the same cut
  with the b110 public Turnip DLL (SHA 574F71ED, a different Mesa base,
  regspill-v3-r1 + overrides); if that is clean, a build of the PR base
  16f87fb + the same SnapRYU 0001-0007 patches will separate the PR from
  the base change.

Hardware test plan (item 13): install `turnip-fable-pr1b-release\turnip_icd.json`
in Eden, compare with the Qualcomm 178 baseline / SnapRYU S3z8 DLL under
the 1-hour stability rule; watch for the NEEDS-HW-TEST items (F1 submit
batching, F4 + F11 triple buffering and acquire order, F8 compute dispatch).

### OPEN (possible follow-ups, not changed)

- O1. `wddm_bo_init` always reserves a VA range
  (`D3DKMTReserveGpuVirtualAddress`) before mapping because `alignment` is
  clamped to >= 64 KiB, so every allocation costs two VA kernel calls. If
  `D3DKMTMapGpuVirtualAddress` honours the allocation alignment on QCT's KMD,
  the reserve could be skipped for the common case. Needs a hardware
  experiment; left untouched.
- O2. `wddm_sync_wait` creates and closes an event per CPU wait. Could use a
  thread-local event with a re-check loop; cost is a few microseconds per
  vkWaitForFences so not worth the risk right now.
- O3. `wddm_check_status` (`D3DKMTGetDeviceState`) runs after every submit.
  Cheap but it is one more kernel call; could be rate-limited.
- O4. `tu_GetCalibratedTimestampsKHR` with the device domain allocates a BO,
  context and sync object per call. Fine unless an app calls it per frame.
- O5. `wsi_win32_wait_for_present` polls with `Sleep(1)`.
