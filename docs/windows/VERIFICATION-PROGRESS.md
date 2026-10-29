# Turnip Windows (WDDM) driver — static verification progress

Resumable checklist for the static verification / bug-fix / performance pass
over the Windows-specific paths of this fork. A fresh session should read this
file first, then continue from the first unchecked item.

Branch: `claude/project-thread-btvgxr`
Baseline: commit 16f87fb ("freedreno: normalize Windows paths in the XML include check")
Policy-note: biblioklept

## Scope (Windows-specific code)

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
- [ ] 9. Commit, push, open PR
- [ ] 10. Hardware validation by the user (Snapdragon X Elite / Eden) of the
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
