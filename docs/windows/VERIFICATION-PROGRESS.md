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
- [ ] 11b. Locate the remaining PC-build warnings from the clang build log
      (bitfield-constant-conversion x5, invalid-noreturn x1) and decide
- [ ] 12. Hardware validation by the user (Snapdragon X Elite / Eden) of the
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
