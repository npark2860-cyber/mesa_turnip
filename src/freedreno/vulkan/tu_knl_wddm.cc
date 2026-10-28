#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>

#include "tu_device.h"
#include "tu_knl.h"
#include "tu_cs.h"
#include "util/os_time.h"
#include "vk_fence.h"
#include "vk_sync_dummy.h"
#include "wsi_common_private.h"
#include "util/os_misc.h"
#include <stdio.h>
#include <mutex>
#include <unordered_map>

/* SNAPRYU frame stats (diagnostic DLL only). TU_FRAMESTAT=<file> gets one line per second:
 * submit count and CPU time, longest gap between submits, retire-list work, and how many
 * render passes ran in sysmem/gmem and how often a pass signature switched mode.
 */
struct tu_framestat {
   std::mutex lock;
   FILE *log = nullptr;
   uint64_t start_ns = 0, window_ns = 0, last_submit_ns = 0;
   uint64_t submits = 0, submit_ns = 0, submit_max_ns = 0, gap_max_ns = 0;
   uint64_t waits = 0, signals = 0;
   uint64_t retire_calls = 0, retire_ns = 0, retire_max_ns = 0, retire_len_max = 0, retire_freed = 0;
   uint64_t rp_sysmem = 0, rp_gmem = 0, rp_flips = 0;
   uint64_t big_sysmem = 0, big_gmem = 0, big_flips = 0;
   uint64_t block_waits = 0, block_wait_ns = 0, block_wait_max_ns = 0;
   uint64_t copies = 0, copy_bytes = 0, copy_old_compute = 0, copy_old_compute_bytes = 0;
   std::unordered_map<uint64_t, uint8_t> last_mode;
};

static tu_framestat *
tu_framestat_get(void)
{
   return nullptr;
}

static void
tu_framestat_flush_locked(tu_framestat *s, uint64_t now)
{
   if (!s->window_ns) {
      s->start_ns = s->window_ns = now;
      return;
   }
   if (now - s->window_ns < 1000000000ull)
      return;
   fprintf(s->log,
           "t=%.1f submits=%llu submit_us_avg=%.1f submit_us_max=%.1f gap_ms_max=%.2f waits=%llu signals=%llu "
           "retire_calls=%llu retire_us_avg=%.2f retire_us_max=%.1f retire_len_max=%llu retire_freed=%llu "
           "rp_sysmem=%llu rp_gmem=%llu rp_flips=%llu big_sysmem=%llu big_gmem=%llu big_flips=%llu "
           "block_waits=%llu block_wait_ms_avg=%.2f block_wait_ms_max=%.2f "
           "copies=%llu copy_kb=%llu old_compute_copies=%llu old_compute_kb=%llu\n",
           (now - s->start_ns) / 1e9, (unsigned long long) s->submits,
           s->submits ? s->submit_ns / 1e3 / s->submits : 0.0, s->submit_max_ns / 1e3, s->gap_max_ns / 1e6,
           (unsigned long long) s->waits, (unsigned long long) s->signals,
           (unsigned long long) s->retire_calls, s->retire_calls ? s->retire_ns / 1e3 / s->retire_calls : 0.0,
           s->retire_max_ns / 1e3, (unsigned long long) s->retire_len_max, (unsigned long long) s->retire_freed,
           (unsigned long long) s->rp_sysmem, (unsigned long long) s->rp_gmem, (unsigned long long) s->rp_flips,
           (unsigned long long) s->big_sysmem, (unsigned long long) s->big_gmem, (unsigned long long) s->big_flips,
           (unsigned long long) s->block_waits, s->block_waits ? s->block_wait_ns / 1e6 / s->block_waits : 0.0,
           s->block_wait_max_ns / 1e6, (unsigned long long) s->copies, (unsigned long long) (s->copy_bytes >> 10),
           (unsigned long long) s->copy_old_compute, (unsigned long long) (s->copy_old_compute_bytes >> 10));
   fflush(s->log);
   s->submits = s->submit_ns = s->submit_max_ns = s->gap_max_ns = 0;
   s->waits = s->signals = 0;
   s->retire_calls = s->retire_ns = s->retire_max_ns = s->retire_len_max = s->retire_freed = 0;
   s->rp_sysmem = s->rp_gmem = s->rp_flips = 0;
   s->big_sysmem = s->big_gmem = s->big_flips = 0;
   s->block_waits = s->block_wait_ns = s->block_wait_max_ns = 0;
   s->copies = s->copy_bytes = s->copy_old_compute = s->copy_old_compute_bytes = 0;
   s->window_ns = now;
}

void tu_framestat_rp(uint64_t sig, bool sysmem, uint32_t width, uint32_t height);
void tu_framestat_copy(uint64_t bytes, bool old_compute);

/* Counts CmdCopyBuffer2 recordings; old_compute marks copies the pre-nocc driver sent to
 * the compute path (all regions 16-byte aligned, 64 KiB or more in total). */
void
tu_framestat_copy(uint64_t bytes, bool old_compute)
{
   tu_framestat *s = tu_framestat_get();
   if (!s)
      return;
   std::lock_guard<std::mutex> guard(s->lock);
   s->copies++;
   s->copy_bytes += bytes;
   if (old_compute) {
      s->copy_old_compute++;
      s->copy_old_compute_bytes += bytes;
   }
}


void
tu_framestat_rp(uint64_t sig, bool sysmem, uint32_t width, uint32_t height)
{
   tu_framestat *s = tu_framestat_get();
   if (!s)
      return;
   const bool big = (uint64_t) width * height >= 640ull * 360ull;
   const uint8_t mode = sysmem ? 1 : 2;
   std::lock_guard<std::mutex> guard(s->lock);
   (sysmem ? s->rp_sysmem : s->rp_gmem)++;
   if (big)
      (sysmem ? s->big_sysmem : s->big_gmem)++;
   uint8_t &last = s->last_mode[sig];
   if (last && last != mode) {
      s->rp_flips++;
      if (big)
         s->big_flips++;
   }
   last = mode;
}


struct wddm_retired_bo {
   struct tu_bo *bo;
   std::vector<uint64_t> values;
};

struct tu_wddm_device {
   D3DKMT_HANDLE handle;
   D3DKMT_CREATEPAGINGQUEUE paging;
   SRWLOCK retire_lock;
   SRWLOCK d3d_lock;
   HMODULE d3d_module;
   HMODULE dxgi_module;
   ID3D12Device *d3d;
   ID3D12CommandQueue *present_queue;
   std::vector<struct wddm_sync *> completions;
   std::vector<wddm_retired_bo> retired;
};

struct wddm_sync {
   struct vk_sync base;
   D3DKMT_HANDLE handle;
   volatile uint64_t *cpu;
   uint64_t target;
   ID3D12Fence *d3d_fence;
};

static VkResult wddm_check_status(struct tu_device *dev);

static VkResult
wddm_sync_init(struct vk_device *device, struct vk_sync *sync, uint64_t value)
{
   auto dev = container_of(device, struct tu_device, vk);
   auto s = (struct wddm_sync *)sync;
   D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create = {};
   create.hDevice = dev->wddm->handle;
   create.Info.Type = D3DDDI_MONITORED_FENCE;
   create.Info.Flags.Shared = 1;
   create.Info.Flags.NtSecuritySharing = 1;
   create.Info.MonitoredFence.InitialFenceValue = value;
   create.Info.MonitoredFence.EngineAffinity = 1;
   if (D3DKMTCreateSynchronizationObject2(&create) < 0)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   s->handle = create.hSyncObject;
   s->cpu = (volatile uint64_t *)create.Info.MonitoredFence.FenceValueCPUVirtualAddress;
   s->target = 1;
   return VK_SUCCESS;
}

static void
wddm_sync_finish(struct vk_device *, struct vk_sync *sync)
{
   auto s = (struct wddm_sync *)sync;
   if (s->d3d_fence)
      s->d3d_fence->Release();
   D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {};
   destroy.hSyncObject = ((struct wddm_sync *)sync)->handle;
   D3DKMTDestroySynchronizationObject(&destroy);
}

static VkResult
wddm_sync_signal(struct vk_device *device, struct vk_sync *sync, uint64_t value)
{
   auto dev = container_of(device, struct tu_device, vk);
   auto s = (struct wddm_sync *)sync;
   if (!(sync->flags & VK_SYNC_IS_TIMELINE))
      value = s->target;
   D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU signal = {};
   signal.hDevice = dev->wddm->handle;
   signal.ObjectCount = 1;
   signal.ObjectHandleArray = &s->handle;
   signal.FenceValueArray = &value;
   signal.Flags.AllowFenceRewind = 1;
   NTSTATUS status = D3DKMTSignalSynchronizationObjectFromCpu(&signal);
   if (status < 0)
      mesa_loge("WDDM CPU signal value %llu failed: 0x%lx", (unsigned long long)value, status);
   return status >= 0 ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

static VkResult
wddm_sync_reset(struct vk_device *, struct vk_sync *sync)
{
   ((struct wddm_sync *)sync)->target++;
   return VK_SUCCESS;
}

static VkResult
wddm_sync_get_value(struct vk_device *device, struct vk_sync *sync, uint64_t *value)
{
   *value = *((struct wddm_sync *)sync)->cpu;
   MemoryBarrier();
   return *value == UINT64_MAX ?
      wddm_check_status(container_of(device, struct tu_device, vk)) : VK_SUCCESS;
}

static VkResult
wddm_sync_move(struct vk_device *device, struct vk_sync *dst, struct vk_sync *src)
{
   auto d = (struct wddm_sync *)dst;
   auto s = (struct wddm_sync *)src;
   struct wddm_sync replacement = {};
   VkResult result = wddm_sync_init(device, &replacement.base, 0);
   if (result != VK_SUCCESS)
      return result;
   wddm_sync_finish(device, dst);
   d->handle = s->handle; d->cpu = s->cpu; d->target = s->target;
   d->d3d_fence = s->d3d_fence;
   s->d3d_fence = NULL;
   s->handle = replacement.handle; s->cpu = replacement.cpu; s->target = 1;
   return VK_SUCCESS;
}

static VkResult
wddm_sync_wait(struct vk_device *device, struct vk_sync *sync, uint64_t value,
                enum vk_sync_wait_flags, uint64_t deadline)
{
   auto dev = container_of(device, struct tu_device, vk);
   auto s = (struct wddm_sync *)sync;
   if (!(sync->flags & VK_SYNC_IS_TIMELINE))
      value = s->target;
   uint64_t current = *s->cpu;
   MemoryBarrier();
   if (current == UINT64_MAX)
      return wddm_check_status(dev);
   if (current >= value)
      return VK_SUCCESS;
   uint64_t now = os_time_get_nano();
   if (deadline <= now)
      return VK_TIMEOUT;
   DWORD timeout = deadline == UINT64_MAX ? INFINITE :
      deadline <= now ? 0 : (DWORD)MIN2(DIV_ROUND_UP(deadline - now, 1000000), INFINITE - 1ULL);
   HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
   if (!event)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {};
   wait.hDevice = dev->wddm->handle;
   wait.ObjectCount = 1;
   wait.ObjectHandleArray = &s->handle;
   wait.FenceValueArray = &value;
   wait.hAsyncEvent = event;
   NTSTATUS status = D3DKMTWaitForSynchronizationObjectFromCpu(&wait);
   DWORD result = status >= 0 ? WaitForSingleObject(event, timeout) : WAIT_FAILED;
   CloseHandle(event);
   if (tu_framestat *fs = tu_framestat_get()) {
      const uint64_t dt = os_time_get_nano() - now;
      std::lock_guard<std::mutex> guard(fs->lock);
      fs->block_waits++;
      fs->block_wait_ns += dt;
      fs->block_wait_max_ns = MAX2(fs->block_wait_max_ns, dt);
   }
   MemoryBarrier();
   if (*s->cpu == UINT64_MAX)
      return wddm_check_status(dev);
   return result == WAIT_OBJECT_0 ? VK_SUCCESS : result == WAIT_TIMEOUT ? VK_TIMEOUT : VK_ERROR_DEVICE_LOST;
}

static const struct vk_sync_type wddm_sync_type = {
   .size = sizeof(struct wddm_sync),
   .features = (enum vk_sync_features)(VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_TIMELINE |
      VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL |
      VK_SYNC_FEATURE_GPU_WAIT | VK_SYNC_FEATURE_GPU_MULTI_WAIT |
      VK_SYNC_FEATURE_CPU_WAIT | VK_SYNC_FEATURE_CPU_RESET | VK_SYNC_FEATURE_CPU_SIGNAL),
   .init = wddm_sync_init,
   .finish = wddm_sync_finish,
   .signal = wddm_sync_signal,
   .get_value = wddm_sync_get_value,
   .reset = wddm_sync_reset,
   .move = wddm_sync_move,
   .wait = wddm_sync_wait,
};

static void wddm_bo_release(struct tu_device *dev, struct tu_bo *bo);

static void
wddm_collect_retired(struct tu_device *dev)
{
   if (vk_device_is_lost_no_report(&dev->vk))
      return;
   auto w = dev->wddm;
   tu_framestat *fs = tu_framestat_get();
   const uint64_t fs_start = fs ? os_time_get_nano() : 0;
   uint64_t fs_freed = 0;
   AcquireSRWLockExclusive(&w->retire_lock);
   const uint64_t fs_len = w->retired.size();
   for (size_t i = 0; i < w->retired.size();) {
      auto &entry = w->retired[i];
      bool complete = true;
      for (size_t q = 0; q < entry.values.size(); q++) {
         uint64_t value = *w->completions[q]->cpu;
         complete &= value != UINT64_MAX && value >= entry.values[q];
      }
      if (!complete) {
         i++;
         continue;
      }
      MemoryBarrier();
      wddm_bo_release(dev, entry.bo);
      w->retired.erase(w->retired.begin() + i);
      fs_freed++;
   }
   ReleaseSRWLockExclusive(&w->retire_lock);
   if (fs) {
      const uint64_t dt = os_time_get_nano() - fs_start;
      std::lock_guard<std::mutex> guard(fs->lock);
      fs->retire_calls++;
      fs->retire_ns += dt;
      fs->retire_max_ns = MAX2(fs->retire_max_ns, dt);
      fs->retire_len_max = MAX2(fs->retire_len_max, fs_len);
      fs->retire_freed += fs_freed;
   }
}

static bool
wddm_wait_paging(struct tu_device *dev, uint64_t value)
{
   auto fence = (volatile uint64_t *)dev->wddm->paging.FenceValueCPUVirtualAddress;
   if (*fence < value) {
      HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
      if (!event)
         return false;
      D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {};
      wait.hDevice = dev->wddm->handle;
      wait.ObjectCount = 1;
      wait.ObjectHandleArray = &dev->wddm->paging.hSyncObject;
      wait.FenceValueArray = &value;
      wait.hAsyncEvent = event;
      NTSTATUS status = D3DKMTWaitForSynchronizationObjectFromCpu(&wait);
      DWORD waited = status >= 0 ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
      CloseHandle(event);
      if (waited != WAIT_OBJECT_0)
         return false;
   }
   return *fence != UINT64_MAX && *fence >= value;
}

static VkResult
wddm_device_init(struct tu_device *device)
{
   device->fd = -1;
   auto w = new tu_wddm_device{};
   if (!w)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   D3DKMT_CREATEDEVICE create = {};
   create.hAdapter = device->physical_device->wddm_adapter;
   NTSTATUS status = D3DKMTCreateDevice(&create);
   if (status >= 0) {
      w->handle = create.hDevice;
      w->paging.hDevice = w->handle;
      status = D3DKMTCreatePagingQueue(&w->paging);
   }
   if (status < 0) {
      if (w->handle) {
         D3DKMT_DESTROYDEVICE destroy = {};
         destroy.hDevice = w->handle;
         D3DKMTDestroyDevice(&destroy);
      }
      delete w;
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED, "WDDM device/paging creation: 0x%lx", status);
   }
   device->wddm = w;
   return VK_SUCCESS;
}

static void
wddm_device_finish(struct tu_device *device)
{
   wddm_collect_retired(device);
   for (auto &entry : device->wddm->retired)
      wddm_bo_release(device, entry.bo);
   for (auto sync : device->wddm->completions) {
      wddm_sync_finish(&device->vk, &sync->base);
      free(sync);
   }
   D3DDDI_DESTROYPAGINGQUEUE paging = {};
   paging.hPagingQueue = device->wddm->paging.hPagingQueue;
   D3DKMTDestroyPagingQueue(&paging);
   D3DKMT_DESTROYDEVICE destroy = {};
   destroy.hDevice = device->wddm->handle;
   D3DKMTDestroyDevice(&destroy);
   if (device->wddm->present_queue)
      device->wddm->present_queue->Release();
   if (device->wddm->d3d)
      device->wddm->d3d->Release();
   if (device->wddm->d3d_module)
      FreeLibrary(device->wddm->d3d_module);
   if (device->wddm->dxgi_module)
      FreeLibrary(device->wddm->dxgi_module);
   delete device->wddm;
   device->wddm = NULL;
}

static int
wddm_queue_new(struct tu_device *dev, struct tu_queue *queue)
{
   uint32_t data[16] = {64, 3, 8, 65536, 0, 0, 0, 65536};
   D3DKMT_CREATECONTEXTVIRTUAL create = {};
   create.hDevice = dev->wddm->handle;
   create.EngineAffinity = 1;
   create.pPrivateDriverData = data;
   create.PrivateDriverDataSize = sizeof(data);
   NTSTATUS status = D3DKMTCreateContextVirtual(&create);
   if (status < 0) {
      mesa_loge("WDDM context creation: 0x%lx", status);
      return -1;
   }
   queue->msm_queue_id = create.hContext;
   queue->wddm_completion = (struct wddm_sync *)calloc(1, sizeof(struct wddm_sync));
   if (!queue->wddm_completion ||
       wddm_sync_init(&dev->vk, &queue->wddm_completion->base, 0) != VK_SUCCESS) {
      D3DKMT_DESTROYCONTEXT destroy = {};
      destroy.hContext = create.hContext;
      D3DKMTDestroyContext(&destroy);
      free(queue->wddm_completion);
      return -1;
   }
   queue->wddm_completion->base.flags = VK_SYNC_IS_TIMELINE;
   queue->wddm_completion->target = 0;
   dev->wddm->completions.push_back(queue->wddm_completion);
   return 0;
}

static void
wddm_queue_close(struct tu_device *dev, struct tu_queue *queue)
{
   D3DKMT_DESTROYCONTEXT destroy = {};
   destroy.hContext = queue->msm_queue_id;
   D3DKMTDestroyContext(&destroy);
   wddm_collect_retired(dev);
}

static VkResult
wddm_bo_unmap(struct tu_device *dev, struct tu_bo *bo, bool reserve)
{
   if (reserve)
      return VK_ERROR_MEMORY_MAP_FAILED;
   D3DKMT_UNLOCK2 unlock = {};
   unlock.hDevice = dev->wddm->handle;
   unlock.hAllocation = bo->gem_handle;
   if (D3DKMTUnlock2(&unlock) < 0)
      return VK_ERROR_MEMORY_MAP_FAILED;
   bo->map = NULL;
   return VK_SUCCESS;
}

static void
wddm_bo_release(struct tu_device *dev, struct tu_bo *bo)
{
   if (bo->map)
      wddm_bo_unmap(dev, bo, false);
   if (bo->iova || bo->wddm_reserved_va) {
      D3DKMT_FREEGPUVIRTUALADDRESS release = {};
      release.hAdapter = dev->physical_device->wddm_adapter;
      release.BaseAddress = bo->wddm_reserved_va ? bo->wddm_reserved_va : bo->iova;
      release.Size = bo->wddm_reserved_va ? bo->wddm_reserved_size : bo->size;
      D3DKMTFreeGpuVirtualAddress(&release);
   }
   if (bo->gem_handle) {
      D3DKMT_DESTROYALLOCATION2 destroy = {};
      destroy.hDevice = dev->wddm->handle;
      if (bo->wddm_resource) {
         destroy.hResource = bo->wddm_resource;
      } else {
         destroy.phAllocationList = &bo->gem_handle;
         destroy.AllocationCount = 1;
      }
      D3DKMTDestroyAllocation2(&destroy);
   }
   free(bo);
}

static VkResult
wddm_bo_init(struct tu_device *dev, struct vk_object_base *base,
             struct tu_bo **out, uint64_t size, uint64_t alignment,
             uint64_t requested_va, VkMemoryPropertyFlags properties,
             enum tu_bo_alloc_flags flags, struct tu_sparse_vma *lazy,
             const char *name)
{
   wddm_collect_retired(dev);
   if (lazy || size > UINT32_MAX - 65535)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   alignment = MAX2(alignment, 65536);
   if (alignment & (alignment - 1))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   if (requested_va && (requested_va & (MAX2(alignment, 4096) - 1)))
      return VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS;
   auto bo = (struct tu_bo *)calloc(1, sizeof(struct tu_bo));
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   bo->size = align64(size, 65536);
   alignas(8) uint32_t data[304 / 4] = {};
   data[0] = sizeof(data);
   data[4] = bo->size;
   data[6] = MAX2(alignment, 4096);
   data[14] = 1;
   data[52] = 7;
   if ((properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ||
       debug_get_bool_option("TU_WDDM_CACHED", false))
      data[15] = 1u << 7;
   data[57] = bo->size;
   data[58] = 1;
   data[59] = bo->size;
   data[60] = bo->size;
   data[61] = 1;
   data[62] = 1;
   data[63] = bo->size;
   data[65] = bo->size;
   D3DDDI_ALLOCATIONINFO2 info = {};
   info.pPrivateDriverData = data;
   info.PrivateDriverDataSize = sizeof(data);
   D3DKMT_CREATEALLOCATION create = {};
   create.hDevice = dev->wddm->handle;
   create.NumAllocations = 1;
   create.pAllocationInfo2 = &info;
   NTSTATUS status = D3DKMTCreateAllocation2(&create);
   bo->gem_handle = info.hAllocation;
   bool address_failed = false;
   if (status >= 0) {
      D3DDDI_MAPGPUVIRTUALADDRESS map = {};
      map.hPagingQueue = dev->wddm->paging.hPagingQueue;
      map.hAllocation = bo->gem_handle;
      map.SizeInPages = bo->size / 4096;
      map.BaseAddress = requested_va;
      map.MinimumAddress = debug_get_bool_option("TU_WDDM_HIGH_VA", false) ? (1ULL << 32) : dev->physical_device->va_start;
      map.MaximumAddress = dev->physical_device->va_start + dev->physical_device->va_size - 1;
      map.Protection.Write = 1;
      uint64_t va_alignment = alignment;
      if (flags & TU_BO_ALLOC_NO_32B_ROLLOVER)
         va_alignment = MAX2(va_alignment, util_next_power_of_two64(bo->size));
      if (requested_va || va_alignment > 4096) {
         D3DDDI_RESERVEGPUVIRTUALADDRESS reserve = {};
         reserve.hAdapter = dev->physical_device->wddm_adapter;
         reserve.BaseAddress = requested_va;
         reserve.MinimumAddress = align64(map.MinimumAddress, 65536);
         reserve.MaximumAddress = (map.MaximumAddress + 1) & ~65535ULL;
         reserve.Size = requested_va ? bo->size : bo->size + va_alignment - 65536;
         status = D3DKMTReserveGpuVirtualAddress(&reserve);
         if (status >= 0) {
            bo->wddm_reserved_va = reserve.VirtualAddress;
            bo->wddm_reserved_size = reserve.Size;
            map.BaseAddress = requested_va ? requested_va : align64(reserve.VirtualAddress, va_alignment);
         }
      }
      if (status >= 0)
         status = D3DKMTMapGpuVirtualAddress(&map);
      address_failed = status < 0;
      if (status >= 0) {
         bo->iova = map.VirtualAddress;
         if (!wddm_wait_paging(dev, map.PagingFenceValue))
            status = (NTSTATUS)0xc0000001;
      }
   }
   if (status >= 0) {
      D3DDDI_MAKERESIDENT resident = {};
      resident.hPagingQueue = dev->wddm->paging.hPagingQueue;
      resident.NumAllocations = 1;
      resident.AllocationList = &bo->gem_handle;
      status = D3DKMTMakeResident(&resident);
      if (status >= 0 && !wddm_wait_paging(dev, resident.PagingFenceValue))
         status = (NTSTATUS)0xc0000001;
   }
   if (status < 0) {
      mesa_loge("WDDM allocation %s size=%llu VA=0x%llx handle=%u VA-end=0x%llx mapping=%u failed: 0x%lx", name,
                (unsigned long long)size, (unsigned long long)bo->iova, bo->gem_handle,
                (unsigned long long)(dev->physical_device->va_start + dev->physical_device->va_size),
                address_failed, status);
      wddm_bo_release(dev, bo);
      if (requested_va && address_failed)
         return VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS;
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY, "WDDM allocation %s: 0x%lx", name, status);
   }
   bo->refcnt = 1;
   bo->base = base;
   bo->name = tu_debug_bos_add(dev, bo->size, name);
   tu_dump_bo_init(dev, bo);
   *out = bo;
   return VK_SUCCESS;
}

static void *
wddm_present_queue(VkDevice device)
{
   VK_FROM_HANDLE(tu_device, dev, device);
   auto w = dev->wddm;
   AcquireSRWLockExclusive(&w->d3d_lock);
   if (!w->present_queue) {
      if (!w->d3d_module)
         w->d3d_module = LoadLibraryW(L"d3d12.dll");
      if (!w->dxgi_module)
         w->dxgi_module = LoadLibraryW(L"dxgi.dll");
      auto create_device = w->d3d_module ?
         (PFN_D3D12_CREATE_DEVICE)GetProcAddress(w->d3d_module, "D3D12CreateDevice") : NULL;
      auto create_factory = w->dxgi_module ?
         (HRESULT (WINAPI *)(UINT, REFIID, void **))GetProcAddress(w->dxgi_module, "CreateDXGIFactory2") : NULL;
      IDXGIFactory4 *factory = NULL;
      IDXGIAdapter *adapter = NULL;
      LUID luid;
      memcpy(&luid, &dev->physical_device->wddm_luid, sizeof(luid));
      HRESULT hr = E_FAIL;
      if (create_device && create_factory)
         hr = create_factory(0, IID_PPV_ARGS(&factory));
      if (SUCCEEDED(hr))
         hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
      if (SUCCEEDED(hr) && !w->d3d)
         hr = create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&w->d3d));
      D3D12_COMMAND_QUEUE_DESC desc = {};
      desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
      if (SUCCEEDED(hr))
         hr = w->d3d->CreateCommandQueue(&desc, IID_PPV_ARGS(&w->present_queue));
      if (adapter)
         adapter->Release();
      if (factory)
         factory->Release();
      if (FAILED(hr))
         mesa_loge("WDDM D3D12 presentation queue: 0x%lx", hr);
   }
   ReleaseSRWLockExclusive(&w->d3d_lock);
   return w->present_queue;
}

static void *
wddm_present_fence(VkDevice device, VkFence fence, uint64_t *value)
{
   VK_FROM_HANDLE(tu_device, dev, device);
   VK_FROM_HANDLE(vk_fence, f, fence);
   auto sync = vk_fence_get_active_sync(f);
   if (sync->type != &wddm_sync_type || !dev->wddm->d3d)
      return NULL;
   auto s = (struct wddm_sync *)sync;
   if (!s->d3d_fence) {
      OBJECT_ATTRIBUTES attributes = {};
      attributes.Length = sizeof(attributes);
      HANDLE handle = NULL;
      if (D3DKMTShareObjects(1, &s->handle, &attributes, GENERIC_ALL, &handle) < 0)
         return NULL;
      HRESULT hr = dev->wddm->d3d->OpenSharedHandle(handle, IID_PPV_ARGS(&s->d3d_fence));
      CloseHandle(handle);
      if (FAILED(hr))
         return NULL;
   }
   *value = s->target;
   return s->d3d_fence;
}

static VkResult
wddm_present_buffer(VkDevice device, const VkMemoryAllocateInfo *info,
                    const VkAllocationCallbacks *alloc, void **resource,
                    VkDeviceMemory *out)
{
   VK_FROM_HANDLE(tu_device, dev, device);
   *resource = NULL;
   *out = VK_NULL_HANDLE;
   if (!wddm_present_queue(device))
      return VK_ERROR_INITIALIZATION_FAILED;
   if (info->allocationSize > UINT32_MAX - 65535)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   auto bo = (struct tu_bo *)calloc(1, sizeof(struct tu_bo));
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   bo->size = align64(info->allocationSize, 65536);
   auto mem = (struct tu_device_memory *)vk_device_memory_create(
      &dev->vk, info, alloc, sizeof(struct tu_device_memory));
   if (!mem) {
      free(bo);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   D3D12_HEAP_PROPERTIES heap = {};
   heap.Type = D3D12_HEAP_TYPE_DEFAULT;
   heap.CreationNodeMask = heap.VisibleNodeMask = 1;
   D3D12_RESOURCE_DESC desc = {};
   desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   desc.Width = bo->size;
   desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
   desc.SampleDesc.Count = 1;
   desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   ID3D12Resource *buffer = NULL;
   HANDLE handle = NULL;
   HRESULT hr = dev->wddm->d3d->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
      NULL, IID_PPV_ARGS(&buffer));
   if (SUCCEEDED(hr))
      hr = dev->wddm->d3d->CreateSharedHandle(buffer, NULL, GENERIC_ALL, NULL, &handle);
   NTSTATUS status = (NTSTATUS)0xc0000001;
   if (SUCCEEDED(hr)) {
      D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
      query.hDevice = dev->wddm->handle;
      query.hNtHandle = handle;
      status = D3DKMTQueryResourceInfoFromNtHandle(&query);
      if (status >= 0 && query.NumAllocations != 1)
         status = (NTSTATUS)0xc000000d;
      if (status >= 0) {
         std::vector<uint8_t> runtime(query.PrivateRuntimeDataSize);
         std::vector<uint8_t> private_data(query.ResourcePrivateDriverDataSize);
         std::vector<uint8_t> allocation_data(query.TotalPrivateDriverDataSize);
         D3DDDI_OPENALLOCATIONINFO2 allocation = {};
         D3DKMT_OPENRESOURCEFROMNTHANDLE open = {};
         open.hDevice = dev->wddm->handle;
         open.hNtHandle = handle;
         open.NumAllocations = 1;
         open.pOpenAllocationInfo2 = &allocation;
         open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
         open.pPrivateRuntimeData = runtime.data();
         open.ResourcePrivateDriverDataSize = query.ResourcePrivateDriverDataSize;
         open.pResourcePrivateDriverData = private_data.data();
         open.TotalPrivateDriverDataBufferSize = query.TotalPrivateDriverDataSize;
         open.pTotalPrivateDriverDataBuffer = allocation_data.data();
         status = D3DKMTOpenResourceFromNtHandle(&open);
         if (status >= 0) {
            bo->gem_handle = allocation.hAllocation;
            bo->wddm_resource = open.hResource;
         }
      }
   }
   if (handle)
      CloseHandle(handle);
   if (status >= 0) {
      D3DDDI_MAPGPUVIRTUALADDRESS map = {};
      map.hPagingQueue = dev->wddm->paging.hPagingQueue;
      map.hAllocation = bo->gem_handle;
      map.SizeInPages = bo->size / 4096;
      map.MinimumAddress = dev->physical_device->va_start;
      map.MaximumAddress = map.MinimumAddress + dev->physical_device->va_size - 1;
      map.Protection.Write = 1;
      status = D3DKMTMapGpuVirtualAddress(&map);
      if (status >= 0) {
         bo->iova = map.VirtualAddress;
         if (!wddm_wait_paging(dev, map.PagingFenceValue))
            status = (NTSTATUS)0xc0000001;
      }
   }
   if (status >= 0) {
      D3DDDI_MAKERESIDENT resident = {};
      resident.hPagingQueue = dev->wddm->paging.hPagingQueue;
      resident.NumAllocations = 1;
      resident.AllocationList = &bo->gem_handle;
      status = D3DKMTMakeResident(&resident);
      if (status >= 0 && !wddm_wait_paging(dev, resident.PagingFenceValue))
         status = (NTSTATUS)0xc0000001;
   }
   if (status < 0) {
      mesa_loge("WDDM presentation buffer: HRESULT 0x%lx NTSTATUS 0x%lx", hr, status);
      wddm_bo_release(dev, bo);
      if (buffer)
         buffer->Release();
      vk_device_memory_destroy(&dev->vk, alloc, &mem->vk);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   mem->size = info->allocationSize;
   mem->refcnt = 1;
   mem->bo = bo;
   mem->iova = bo->iova;
   bo->refcnt = 1;
   bo->base = &mem->vk.base;
   bo->name = tu_debug_bos_add(dev, bo->size, "DXGI presentation buffer");
   tu_dump_bo_init(dev, bo);
   p_atomic_add(&dev->physical_device->heap.used, bo->size);
   *resource = buffer;
   *out = tu_device_memory_to_handle(mem);
   return VK_SUCCESS;
}

static VkResult
wddm_bo_map(struct tu_device *dev, struct tu_bo *bo, void *address)
{
   if (address)
      return VK_ERROR_MEMORY_MAP_FAILED;
   D3DKMT_LOCK2 lock = {};
   lock.hDevice = dev->wddm->handle;
   lock.hAllocation = bo->gem_handle;
   NTSTATUS status = D3DKMTLock2(&lock);
   if (status < 0)
      return vk_errorf(dev, VK_ERROR_MEMORY_MAP_FAILED, "WDDM Lock2: 0x%lx", status);
   bo->map = lock.pData;
   return VK_SUCCESS;
}

static void
wddm_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   if (!p_atomic_dec_zero(&bo->refcnt))
      return;
   tu_debug_bos_del(dev, bo);
   tu_dump_bo_del(dev, bo);
   AcquireSRWLockExclusive(&dev->wddm->retire_lock);
   wddm_retired_bo entry = {bo, {}};
   for (auto sync : dev->wddm->completions)
      entry.values.push_back(p_atomic_read(&sync->target));
   dev->wddm->retired.push_back(std::move(entry));
   ReleaseSRWLockExclusive(&dev->wddm->retire_lock);
   wddm_collect_retired(dev);
}

static void wddm_bo_allow_dump(struct tu_device *, struct tu_bo *) {}
static int
wddm_timestamp_sample(struct tu_device *dev, uint64_t *value,
                      uint64_t *cpu_value, uint64_t *deviation)
{
   struct tu_bo *bo = NULL;
   if (wddm_bo_init(dev, &dev->vk.base, &bo, 65536, 65536, 0,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     TU_BO_ALLOC_NO_FLAGS, NULL, "timestamp calibration") != VK_SUCCESS)
      return -1;
   D3DKMT_CREATECONTEXTVIRTUAL context = {};
   struct wddm_sync sync = {};
   bool sync_created = false;
   int result = -1;
   uint32_t context_data[16] = {64, 3, 8, 65536, 0, 0, 0, 65536};
   context.hDevice = dev->wddm->handle;
   context.EngineAffinity = 1;
   context.pPrivateDriverData = context_data;
   context.PrivateDriverDataSize = sizeof(context_data);
   if (wddm_bo_map(dev, bo, NULL) == VK_SUCCESS &&
       D3DKMTCreateContextVirtual(&context) >= 0 &&
       wddm_sync_init(&dev->vk, &sync.base, 0) == VK_SUCCESS) {
      sync_created = true;
      auto words = (uint32_t *)bo->map;
      auto timestamp = (volatile uint64_t *)(words + 1024);
      *timestamp = 0;
      struct tu_cs cs;
      tu_cs_init_external(&cs, dev, words, words + 4, bo->iova, true);
      tu_cs_begin(&cs);
      tu_cs_reserve_space(&cs, 4);
      cs.reg_to_mem(bo->iova + 4096, CP_ALWAYS_ON_COUNTER(A6XX));
      tu_cs_end(&cs);
      uint32_t data[56] = {};
      data[0] = 0xccaabbee; data[2] = sizeof(data); data[4] = 3;
      data[13] = 0xfadcab01; data[14] = 52; data[17] = 1;
      data[26] = 0xfadcab02; data[27] = 112;
      data[28] = 0xcccc0001; data[29] = 104; data[38] = 1;
      data[49] = cs.cur - cs.start;
      data[50] = bo->iova; data[51] = bo->iova >> 32;
      data[54] = 0xfadcab00; data[55] = 8;
      D3DKMT_SUBMITCOMMAND submit = {};
      submit.Commands = bo->iova;
      submit.CommandLength = data[49] * 4;
      submit.BroadcastContextCount = 1;
      submit.BroadcastContext[0] = context.hContext;
      submit.pPrivateDriverData = data;
      submit.PrivateDriverDataSize = sizeof(data);
      MemoryBarrier();
      UINT64 one = 1;
      D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {};
      signal.BroadcastContextCount = 1;
      signal.BroadcastContextArray = &context.hContext;
      signal.ObjectCount = 1;
      signal.ObjectHandleArray = &sync.handle;
      signal.MonitoredFenceValueArray = &one;
      LARGE_INTEGER before, after, frequency;
      QueryPerformanceFrequency(&frequency);
      QueryPerformanceCounter(&before);
      if (D3DKMTSubmitCommand(&submit) >= 0 &&
          D3DKMTSignalSynchronizationObjectFromGpu2(&signal) >= 0 &&
          wddm_sync_wait(&dev->vk, &sync.base, 1, VK_SYNC_WAIT_COMPLETE,
                          os_time_get_absolute_timeout(5000000000ULL)) == VK_SUCCESS) {
         MemoryBarrier();
         *value = *timestamp;
         QueryPerformanceCounter(&after);
         if (cpu_value)
            *cpu_value = after.QuadPart;
         if (deviation)
            *deviation = (after.QuadPart - before.QuadPart) * 1000000000ULL / frequency.QuadPart + 53;
         result = 0;
      }
   }
   if (context.hContext) {
      D3DKMT_DESTROYCONTEXT destroy = {};
      destroy.hContext = context.hContext;
      D3DKMTDestroyContext(&destroy);
   }
   if (sync_created)
      wddm_sync_finish(&dev->vk, &sync.base);
   tu_debug_bos_del(dev, bo);
   tu_dump_bo_del(dev, bo);
   wddm_bo_release(dev, bo);
   return result;
}

static int
wddm_timestamp(struct tu_device *dev, uint64_t *value)
{
   return wddm_timestamp_sample(dev, value, NULL, NULL);
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_GetCalibratedTimestampsKHR(VkDevice device, uint32_t count,
                             const VkCalibratedTimestampInfoKHR *infos,
                             uint64_t *timestamps, uint64_t *max_deviation)
{
   VK_FROM_HANDLE(tu_device, dev, device);
   bool need_gpu = false;
   std::vector<VkTimeDomainKHR> domains(count);
   for (uint32_t i = 0; i < count; i++) {
      domains[i] = infos[i].timeDomain;
      if (domains[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT) {
         auto info = (const VkSwapchainCalibratedTimestampInfoEXT *)vk_find_struct_const(
            infos[i].pNext, SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT);
         domains[i] = wsi_common_get_time_domain(info->swapchain,
            (VkPresentStageFlagBitsEXT)info->presentStage, info->timeDomainId);
      }
      if (domains[i] == VK_TIME_DOMAIN_DEVICE_KHR)
         need_gpu = true;
      else if (domains[i] != VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR)
         return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   uint64_t gpu = 0, cpu = 0;
   LARGE_INTEGER frequency, now;
   QueryPerformanceFrequency(&frequency);
   *max_deviation = DIV_ROUND_UP(1000000000ULL, frequency.QuadPart);
   if (need_gpu) {
      if (wddm_timestamp_sample(dev, &gpu, &cpu, max_deviation))
         return VK_ERROR_DEVICE_LOST;
   } else {
      QueryPerformanceCounter(&now);
      cpu = now.QuadPart;
   }
   for (uint32_t i = 0; i < count; i++) {
      timestamps[i] = domains[i] == VK_TIME_DOMAIN_DEVICE_KHR ? gpu : cpu;
      if (infos[i].timeDomain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT) {
         if (domains[i] == VK_TIME_DOMAIN_DEVICE_KHR)
            timestamps[i] = (uint64_t)((double)gpu * dev->physical_device->vk.properties.timestampPeriod);
         else
            timestamps[i] = (cpu / frequency.QuadPart) * 1000000000ULL +
                             (cpu % frequency.QuadPart) * 1000000000ULL / frequency.QuadPart;
      }
   }
   return VK_SUCCESS;
}
static int wddm_suspend_count(struct tu_device *, uint64_t *value) { *value = 0; return 0; }
static VkResult wddm_check_status(struct tu_device *dev)
{
   D3DKMT_GETDEVICESTATE state = {};
   state.hDevice = dev->wddm->handle;
   state.StateType = D3DKMT_DEVICESTATE_EXECUTION;
   if (D3DKMTGetDeviceState(&state) < 0 || state.ExecutionState != D3DKMT_DEVICEEXECUTION_ACTIVE)
      return vk_device_set_lost(&dev->vk, "WDDM device is not active");
   return VK_SUCCESS;
}


using wddm_submit = std::vector<struct tu_cs_entry>;
static void *wddm_submit_create(struct tu_device *) { return new wddm_submit; }
static void wddm_submit_finish(struct tu_device *, void *submit) { delete (wddm_submit *)submit; }
static void wddm_submit_add(struct tu_device *, void *submit, struct tu_cs_entry *entries, unsigned count)
{
   auto s = (wddm_submit *)submit;
   s->insert(s->end(), entries, entries + count);
}

static VkResult
wddm_queue_submit_impl(struct tu_queue *queue, void *submission,
                   struct vk_sync_wait *waits, uint32_t wait_count,
                   struct vk_sync_signal *signals, uint32_t signal_count,
                   struct tu_u_trace_submission_data *)
{
   auto dev = queue->device;
   for (uint32_t i = 0; i < wait_count; i++) {
      auto sync = (struct wddm_sync *)waits[i].sync;
      UINT64 value = (sync->base.flags & VK_SYNC_IS_TIMELINE) ? waits[i].wait_value : sync->target;
      D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait = {};
      wait.hContext = queue->msm_queue_id;
      wait.ObjectCount = 1;
      wait.ObjectHandleArray = &sync->handle;
      wait.MonitoredFenceValueArray = &value;
      if (D3DKMTWaitForSynchronizationObjectFromGpu(&wait) < 0)
         return vk_device_set_lost(&dev->vk, "WDDM GPU wait failed");
      if (!(sync->base.flags & VK_SYNC_IS_TIMELINE))
         sync->target++;
   }
   auto entries = (wddm_submit *)submission;
   if (debug_get_bool_option("TU_WDDM_CAPTURE_ONLY", false))
      return vk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED, "WDDM capture-only mode");
   if (!entries->empty()) {
      uint32_t count = entries->size();
      uint32_t limit = debug_get_num_option("TU_WDDM_IB_LIMIT", 0);
      if (limit && count == 1) {
         (*entries)[0].size = MIN2((*entries)[0].size, limit * 4);
         mesa_logi("WDDM diagnostic IB prefix: %u dwords", (*entries)[0].size / 4);
      }
      uint32_t size = 200 + count * 24;
      if (size > 65536)
         return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY, "WDDM submit has too many IBs");
      std::vector<uint32_t> data(size / 4, 0);
      data[0] = 0xccaabbee;
      data[2] = size;
      data[4] = 3;
      data[13] = 0xfadcab01;
      data[14] = 52;
      data[17] = dev->submit_count + 1;
      data[26] = 0xfadcab02;
      data[27] = 88 + count * 24;
      data[28] = 0xcccc0001;
      data[29] = 80 + count * 24;
      data[38] = count;
      for (uint32_t i = 0; i < count; i++) {
         const auto &entry = (*entries)[i];
         if (entry.size / 4 >= (1u << 20))
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
         uint64_t address = entry.bo->iova + entry.offset;
         data[49 + 6 * i] = entry.size / 4;
         data[50 + 6 * i] = address;
         data[51 + 6 * i] = address >> 32;
      }
      data[size / 4 - 2] = 0xfadcab00;
      data[size / 4 - 1] = 8;
      D3DKMT_SUBMITCOMMAND submit = {};
      submit.Commands = (*entries)[0].bo->iova + (*entries)[0].offset;
      submit.CommandLength = (*entries)[0].size;
      submit.BroadcastContextCount = 1;
      submit.BroadcastContext[0] = queue->msm_queue_id;
      submit.pPrivateDriverData = data.data();
      submit.PrivateDriverDataSize = size;
      MemoryBarrier();
      NTSTATUS status = D3DKMTSubmitCommand(&submit);
      if (status < 0) {
         mesa_loge("WDDM submit/fence failed: 0x%lx, IB count=%u", status, count);
         return vk_device_set_lost(&dev->vk, "WDDM submit/fence failed: 0x%lx", status);
      }
   }
   for (uint32_t i = 0; i <= signal_count; i++) {
      auto sync = i < signal_count ? (struct wddm_sync *)signals[i].sync : queue->wddm_completion;
      UINT64 value = i < signal_count ?
         ((sync->base.flags & VK_SYNC_IS_TIMELINE) ? signals[i].signal_value : sync->target) :
         ++queue->gfx_timepoint;
      D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {};
      signal.BroadcastContextCount = 1;
      signal.BroadcastContextArray = &queue->msm_queue_id;
      signal.Flags.AllowFenceRewind = 1;
      signal.ObjectCount = 1;
      signal.ObjectHandleArray = &sync->handle;
      signal.MonitoredFenceValueArray = &value;
      if (D3DKMTSignalSynchronizationObjectFromGpu2(&signal) < 0)
         return vk_device_set_lost(&dev->vk, "WDDM GPU signal failed");
      if (i == signal_count)
         p_atomic_set(&sync->target, value);
   }
   queue->fence = (uint32_t)queue->gfx_timepoint;
   wddm_collect_retired(dev);
   return wddm_check_status(dev);
}

static VkResult
wddm_queue_submit(struct tu_queue *queue, void *submission,
                   struct vk_sync_wait *waits, uint32_t wait_count,
                   struct vk_sync_signal *signals, uint32_t signal_count,
                   struct tu_u_trace_submission_data *u_trace)
{
   tu_framestat *fs = tu_framestat_get();
   if (!fs)
      return wddm_queue_submit_impl(queue, submission, waits, wait_count, signals, signal_count, u_trace);
   const uint64_t start = os_time_get_nano();
   VkResult result = wddm_queue_submit_impl(queue, submission, waits, wait_count, signals, signal_count, u_trace);
   const uint64_t now = os_time_get_nano();
   std::lock_guard<std::mutex> guard(fs->lock);
   fs->submits++;
   fs->submit_ns += now - start;
   fs->submit_max_ns = MAX2(fs->submit_max_ns, now - start);
   if (fs->last_submit_ns)
      fs->gap_max_ns = MAX2(fs->gap_max_ns, start - fs->last_submit_ns);
   fs->last_submit_ns = start;
   fs->waits += wait_count;
   fs->signals += signal_count + 1;
   tu_framestat_flush_locked(fs, now);
   return result;
}

static VkResult wddm_queue_wait(struct tu_queue *queue, uint32_t fence, uint64_t timeout)
{
   if (!queue->gfx_timepoint)
      return wddm_check_status(queue->device);
   uint64_t value = queue->gfx_timepoint - (uint32_t)((uint32_t)queue->gfx_timepoint - fence);
   return wddm_sync_wait(&queue->device->vk, &queue->wddm_completion->base,
                         value, VK_SYNC_WAIT_COMPLETE, os_time_get_absolute_timeout(timeout));
}

static const struct tu_knl wddm_knl = {
   .name = "wddm",
   .device_init = wddm_device_init,
   .device_finish = wddm_device_finish,
   .device_get_gpu_timestamp = wddm_timestamp,
   .device_get_suspend_count = wddm_suspend_count,
   .device_check_status = wddm_check_status,
   .submitqueue_new = wddm_queue_new,
   .submitqueue_close = wddm_queue_close,
   .bo_init = wddm_bo_init,
   .bo_map = wddm_bo_map,
   .bo_unmap = wddm_bo_unmap,
   .bo_allow_dump = wddm_bo_allow_dump,
   .bo_finish = wddm_bo_finish,
   .submit_create = wddm_submit_create,
   .submit_finish = wddm_submit_finish,
   .submit_add_entries = wddm_submit_add,
   .queue_submit = wddm_queue_submit,
   .queue_wait_fence = wddm_queue_wait,
};

void
tu_knl_wddm_physical_finish(struct tu_physical_device *device)
{
   if (device->wddm_adapter) {
      D3DKMT_CLOSEADAPTER close = {};
      close.hAdapter = device->wddm_adapter;
      D3DKMTCloseAdapter(&close);
      device->wddm_adapter = 0;
   }
}

bool
tu_knl_wddm_memory_budget(struct tu_physical_device *device, uint64_t *budget, uint64_t *usage)
{
   D3DKMT_QUERYVIDEOMEMORYINFO query = {};
   query.hAdapter = device->wddm_adapter;
   query.MemorySegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
   if (D3DKMTQueryVideoMemoryInfo(&query) < 0)
      return false;
   *budget = query.Budget;
   *usage = query.CurrentUsage;
   query.MemorySegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL;
   if (D3DKMTQueryVideoMemoryInfo(&query) >= 0) {
      *budget += query.Budget;
      *usage += query.CurrentUsage;
   }
   *budget = MIN2(*budget, device->heap.size);
   return true;
}

#include "tu_image.h"

static VkResult
wddm_native_import(VkDevice device, void *resource,
                              const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *alloc,
                              VkDeviceMemory *out)
{
   *out = VK_NULL_HANDLE;
   VK_FROM_HANDLE(tu_device, dev, device);
   if (!wddm_present_queue(device) || !resource ||
       !info->allocationSize || info->allocationSize > 64 * 1024 * 1024 ||
       info->allocationSize % 65536)
      return VK_ERROR_INITIALIZATION_FAILED;
   auto texture = (ID3D12Resource *)resource;
   auto desc = texture->GetDesc();
   ID3D12Device *owner = NULL;
   if (FAILED(texture->GetDevice(IID_PPV_ARGS(&owner))))
      return VK_ERROR_INITIALIZATION_FAILED;
   auto luid = owner->GetAdapterLuid();
   auto allocation = owner->GetResourceAllocationInfo(0, 1, &desc);
   owner->Release();
   if (memcmp(&luid, &dev->physical_device->wddm_luid, sizeof(luid)) ||
       desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
       desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1 ||
       desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
       allocation.SizeInBytes != info->allocationSize)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   HANDLE handle = NULL;
   if (FAILED(texture->GetDevice(IID_PPV_ARGS(&owner))))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   HRESULT share_result = owner->CreateSharedHandle(texture, NULL, GENERIC_ALL, NULL, &handle);
   owner->Release();
   if (FAILED(share_result))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   auto bo = (struct tu_bo *)calloc(1, sizeof(struct tu_bo));
   auto mem = (struct tu_device_memory *)vk_device_memory_create(
      &dev->vk, info, alloc, sizeof(struct tu_device_memory));
   if (!bo || !mem) {
      free(bo);
      if (mem) vk_device_memory_destroy(&dev->vk, alloc, &mem->vk);
      CloseHandle(handle);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   bo->size = info->allocationSize;
   D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
   query.hDevice = dev->wddm->handle;
   query.hNtHandle = handle;
   NTSTATUS status = D3DKMTQueryResourceInfoFromNtHandle(&query);
   if (status >= 0 && (query.NumAllocations != 1 ||
       query.TotalPrivateDriverDataSize != 304 ||
       query.PrivateRuntimeDataSize > 1048576 ||
       query.ResourcePrivateDriverDataSize > 1048576))
      status = (NTSTATUS)0xc000000d;
   if (status >= 0) {
      std::vector<uint8_t> runtime(query.PrivateRuntimeDataSize);
      std::vector<uint8_t> private_data(query.ResourcePrivateDriverDataSize);
      std::vector<uint8_t> allocation_data(query.TotalPrivateDriverDataSize);
      D3DDDI_OPENALLOCATIONINFO2 ai = {};
      D3DKMT_OPENRESOURCEFROMNTHANDLE open = {};
      open.hDevice = dev->wddm->handle;
      open.hNtHandle = handle;
      open.NumAllocations = 1;
      open.pOpenAllocationInfo2 = &ai;
      open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
      open.pPrivateRuntimeData = runtime.data();
      open.ResourcePrivateDriverDataSize = query.ResourcePrivateDriverDataSize;
      open.pResourcePrivateDriverData = private_data.data();
      open.TotalPrivateDriverDataBufferSize = query.TotalPrivateDriverDataSize;
      open.pTotalPrivateDriverDataBuffer = allocation_data.data();
      status = D3DKMTOpenResourceFromNtHandle(&open);
      if (status >= 0) {
         bo->gem_handle = ai.hAllocation;
         bo->wddm_resource = open.hResource;
         uint32_t private_size = 0;
         memcpy(&private_size, allocation_data.data() + 16, sizeof(private_size));
         if (private_size != bo->size)
            status = (NTSTATUS)0xc000000d;
      }
   }
   CloseHandle(handle);
   if (status >= 0) {
      D3DDDI_MAPGPUVIRTUALADDRESS map = {};
      map.hPagingQueue = dev->wddm->paging.hPagingQueue;
      map.hAllocation = bo->gem_handle;
      map.SizeInPages = bo->size / 4096;
      map.MinimumAddress = dev->physical_device->va_start;
      map.MaximumAddress = map.MinimumAddress + dev->physical_device->va_size - 1;
      map.Protection.Write = 1;
      status = D3DKMTMapGpuVirtualAddress(&map);
      if (status >= 0) {
         bo->iova = map.VirtualAddress;
         if (!wddm_wait_paging(dev, map.PagingFenceValue))
            status = (NTSTATUS)0xc0000001;
      }
   }
   if (status >= 0) {
      D3DDDI_MAKERESIDENT resident = {};
      resident.hPagingQueue = dev->wddm->paging.hPagingQueue;
      resident.NumAllocations = 1;
      resident.AllocationList = &bo->gem_handle;
      status = D3DKMTMakeResident(&resident);
      if (status >= 0 && !wddm_wait_paging(dev, resident.PagingFenceValue))
         status = (NTSTATUS)0xc0000001;
   }
   if (status < 0) {
      fprintf(stderr, "DXGI_WRITE_IMPORT_FAILED status=0x%lx\n", status);
      wddm_bo_release(dev, bo);
      vk_device_memory_destroy(&dev->vk, alloc, &mem->vk);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
   mem->size = info->allocationSize;
   mem->refcnt = 1;
   mem->bo = bo;
   mem->iova = bo->iova;
   bo->refcnt = 1;
   bo->base = &mem->vk.base;
   bo->name = tu_debug_bos_add(dev, bo->size, "DXGI native tiled image");
   tu_dump_bo_init(dev, bo);
   p_atomic_add(&dev->physical_device->heap.used, bo->size);
   *out = tu_device_memory_to_handle(mem);
   fprintf(stderr, "DXGI_WRITE_IMPORT_PASS bytes=%llu write_protection=1\n", (unsigned long long)bo->size);
   return VK_SUCCESS;
}


static VkResult
wddm_native_image(VkDevice device, void *resource, VkImage image_h,
                  const VkAllocationCallbacks *alloc, VkDeviceMemory *out)
{
   VK_FROM_HANDLE(tu_device, dev, device);
   VK_FROM_HANDLE(tu_image, image, image_h);
   auto texture = (ID3D12Resource *)resource;
   auto desc = texture->GetDesc();
   const auto &layout = image->layout[0];
   ID3D12Device *owner = NULL;
   if (FAILED(texture->GetDevice(IID_PPV_ARGS(&owner))))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   auto allocation = owner->GetResourceAllocationInfo(0, 1, &desc);
   owner->Release();
   if (image->vk.format != VK_FORMAT_B8G8R8A8_UNORM ||
       image->vk.extent.width != desc.Width || image->vk.extent.height != desc.Height ||
       layout.tile_mode != TILE6_3 || !layout.ubwc ||
       image->total_size > allocation.SizeInBytes || !layout.slices[0].offset ||
       layout.ubwc_slices[0].offset || image->vk.array_layers != 1 || image->vk.mip_levels != 1)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   VkMemoryAllocateInfo info = {};
   info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   info.allocationSize = allocation.SizeInBytes;
   bool found = false;
   for (uint32_t i = 0; i < dev->physical_device->memory.type_count; i++) {
      if (dev->physical_device->memory.types[i] & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
         info.memoryTypeIndex = i;
         found = true;
         break;
      }
   }
   if (!found)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   VkResult result = wddm_native_import(device, resource, &info, alloc, out);
   if (result == VK_SUCCESS)
      fprintf(stderr, "WSI_NATIVE_IMAGE width=%u height=%u tile=%u ubwc=%u pitch=%u offset=%llu size=%llu allocation=%llu\n",
         image->vk.extent.width, image->vk.extent.height, (unsigned)layout.tile_mode,
         (unsigned)layout.ubwc, fdl_pitch(&layout, 0), (unsigned long long)layout.slices[0].offset,
         (unsigned long long)image->total_size, (unsigned long long)allocation.SizeInBytes);
   return result;
}

static VkResult
wddm_native_create_sync(VkDevice device, void **native, void **d3d_fence)
{
   VK_FROM_HANDLE(tu_device, dev, device);
   *native = NULL;
   *d3d_fence = NULL;
   struct vk_sync *sync = NULL;
   VkResult result = vk_sync_create(&dev->vk, &wddm_sync_type, (vk_sync_flags)0, 0, &sync);
   if (result != VK_SUCCESS)
      return result;
   auto s = (struct wddm_sync *)sync;
   OBJECT_ATTRIBUTES attributes = {};
   attributes.Length = sizeof(attributes);
   HANDLE handle = NULL;
   HRESULT hr = E_FAIL;
   if (D3DKMTShareObjects(1, &s->handle, &attributes, GENERIC_ALL, &handle) >= 0) {
      hr = dev->wddm->d3d->OpenSharedHandle(handle, IID_PPV_ARGS(&s->d3d_fence));
      CloseHandle(handle);
   }
   if (FAILED(hr)) {
      vk_sync_destroy(&dev->vk, sync);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
   s->d3d_fence->AddRef();
   *native = sync;
   *d3d_fence = s->d3d_fence;
   return VK_SUCCESS;
}

static VkResult
wddm_native_copy_sync(VkDevice device, void *native, uint64_t value, struct vk_sync **out)
{
   VK_FROM_HANDLE(tu_device, dev, device);
   *out = NULL;
   auto source = (struct wddm_sync *)native;
   OBJECT_ATTRIBUTES attributes = {};
   attributes.Length = sizeof(attributes);
   HANDLE handle = NULL;
   if (!value || D3DKMTShareObjects(1, &source->handle, &attributes, GENERIC_ALL, &handle) < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 open = {};
   open.hNtHandle = handle;
   open.hDevice = dev->wddm->handle;
   open.MonitoredFence.EngineAffinity = 1;
   NTSTATUS status = D3DKMTOpenSyncObjectFromNtHandle2(&open);
   CloseHandle(handle);
   if (status < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   auto s = (struct wddm_sync *)vk_zalloc(&dev->vk.alloc, sizeof(struct wddm_sync), 8,
                                         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!s) {
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {};
      destroy.hSyncObject = open.hSyncObject;
      D3DKMTDestroySynchronizationObject(&destroy);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   s->base.type = &wddm_sync_type;
   s->handle = open.hSyncObject;
   s->cpu = (volatile uint64_t *)open.MonitoredFence.FenceValueCPUVirtualAddress;
   s->target = value;
   *out = &s->base;
   return VK_SUCCESS;
}

static void
wddm_native_destroy_sync(VkDevice device, void *native)
{
   VK_FROM_HANDLE(tu_device, dev, device);
   vk_sync_destroy(&dev->vk, (struct vk_sync *)native);
}

static VkResult
wddm_add_adapter(struct tu_instance *instance, D3DKMT_ADAPTERINFO *adapter)
{
   D3DKMT_ADAPTERREGISTRYINFO registry = {};
   D3DKMT_QUERYADAPTERINFO query = {};
   query.hAdapter = adapter->hAdapter;
   query.Type = KMTQAITYPE_ADAPTERREGISTRYINFO;
   query.pPrivateDriverData = &registry;
   query.PrivateDriverDataSize = sizeof(registry);
   if (D3DKMTQueryAdapterInfo(&query) < 0 ||
       (!wcsstr(registry.AdapterString, L"Qualcomm") &&
        !wcsstr(registry.AdapterString, L"Adreno")))
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   alignas(8) uint32_t data[1840 / 4] = {};
   query.Type = KMTQAITYPE_UMDRIVERPRIVATE;
   query.pPrivateDriverData = data;
   query.PrivateDriverDataSize = sizeof(data);
   if (D3DKMTQueryAdapterInfo(&query) < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   if (data[1] != 4 || data[2] != 50 || data[4] != 0x60c512)
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "Unsupported QCT interface %u.%u chip 0x%x",
                       data[1], data[2], data[4]);

   struct tu_physical_device *device = (struct tu_physical_device *)
      vk_zalloc(&instance->vk.alloc, sizeof(*device), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   device->instance = instance;
   device->local_fd = -1;
   device->master_fd = -1;
   device->kgsl_dma_fd = -1;
   device->dev_id.chip_id = 0xffff43050c01ULL;
   device->gmem_size = data[400 / 4];
   device->uche_trap_base = 0x1fffffffff000ULL;
   device->submitqueue_priority_count = 1;
   device->kernel_sets_magic_regs = true;
   device->has_cached_coherent_memory = true;
   device->heap.size = tu_get_system_heap_size(device);
   device->heap.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
   device->va_start = 0x10000;
   device->va_size = (1ULL << data[960 / 4]) - device->va_start;
   device->ubwc_config.bank_swizzle_levels = ~0u;
   device->ubwc_config.macrotile_mode = FDL_MACROTILE_INVALID;
   device->sync_types[0] = &wddm_sync_type;
   device->sync_types[1] = &vk_sync_dummy_type;
   instance->knl = &wddm_knl;

   VkResult result = tu_physical_device_init(device, instance);
   if (result != VK_SUCCESS) {
      vk_free(&instance->vk.alloc, device);
      return result;
   }
   device->wddm_adapter = adapter->hAdapter;
   memcpy(&device->wddm_luid, &adapter->AdapterLuid, sizeof(device->wddm_luid));
   device->wsi_device.win32.get_d3d12_command_queue = wddm_present_queue;
   device->wsi_device.win32.get_d3d12_fence = wddm_present_fence;
   device->wsi_device.win32.create_buffer_memory = wddm_present_buffer;
   device->wsi_device.win32.import_native_image = wddm_native_image;
   device->wsi_device.win32.create_native_sync = wddm_native_create_sync;
   device->wsi_device.win32.copy_native_sync = wddm_native_copy_sync;
   device->wsi_device.win32.destroy_native_sync = wddm_native_destroy_sync;
   const auto available_features = device->vk.supported_features;
   const auto available_extensions = device->vk.supported_extensions;
   device->vk.supported_extensions = {};
   auto &extensions = device->vk.supported_extensions;
   extensions.KHR_storage_buffer_storage_class = true;
   extensions.KHR_swapchain = true;
   extensions.KHR_present_id = true;
   extensions.KHR_present_wait = true;
   extensions.KHR_present_id2 = true;
   extensions.KHR_present_wait2 = true;
   extensions.EXT_present_timing = true;
   extensions.KHR_calibrated_timestamps = true;
   extensions.KHR_timeline_semaphore = true;
   extensions.KHR_buffer_device_address = true;
   extensions.EXT_memory_budget = true;
   extensions.KHR_synchronization2 = true;
   extensions.KHR_maintenance1 = true;
   extensions.KHR_maintenance2 = true;
   extensions.KHR_maintenance3 = true;
   extensions.KHR_maintenance4 = true;
   extensions.KHR_get_memory_requirements2 = true;
   extensions.KHR_dedicated_allocation = true;
   extensions.KHR_bind_memory2 = true;
   extensions.KHR_device_group = true;
   extensions.KHR_shader_float_controls = true;
   extensions.KHR_spirv_1_4 = true;
   extensions.KHR_relaxed_block_layout = true;
   extensions.KHR_uniform_buffer_standard_layout = true;
   extensions.EXT_scalar_block_layout = true;
   extensions.KHR_variable_pointers = true;
   extensions.KHR_vulkan_memory_model = true;
   extensions.KHR_shader_subgroup_extended_types = true;
   extensions.EXT_subgroup_size_control = true;
   extensions.KHR_workgroup_memory_explicit_layout = true;
   extensions.KHR_zero_initialize_workgroup_memory = true;
   extensions.KHR_push_descriptor = true;
   extensions.KHR_descriptor_update_template = true;
   extensions.KHR_16bit_storage = device->info->props.storage_16bit;
   extensions.KHR_8bit_storage = device->info->props.storage_8bit;
   extensions.KHR_shader_float16_int8 = true;
   extensions.KHR_shader_integer_dot_product = true;
   extensions.KHR_copy_commands2 = true;
   extensions.KHR_create_renderpass2 = true;
   extensions.KHR_depth_stencil_resolve = true;
   extensions.KHR_driver_properties = true;
   extensions.KHR_dynamic_rendering = true;
   extensions.KHR_external_fence = true;
   extensions.KHR_external_memory = true;
   extensions.KHR_external_semaphore = true;
   extensions.KHR_format_feature_flags2 = true;
   extensions.KHR_image_format_list = true;
   extensions.KHR_imageless_framebuffer = true;
   extensions.KHR_multiview = true;
   extensions.KHR_sampler_mirror_clamp_to_edge = true;
   extensions.KHR_sampler_ycbcr_conversion = true;
   extensions.KHR_separate_depth_stencil_layouts = true;
   extensions.KHR_shader_draw_parameters = true;
   extensions.KHR_shader_terminate_invocation = true;
   extensions.EXT_host_query_reset = true;
   extensions.EXT_image_robustness = true;
   extensions.EXT_inline_uniform_block = true;
   extensions.EXT_pipeline_creation_cache_control = true;
   extensions.EXT_private_data = true;
   extensions.EXT_shader_demote_to_helper_invocation = true;
   extensions.EXT_tooling_info = true;
   extensions.EXT_descriptor_indexing = available_extensions.EXT_descriptor_indexing;
   extensions.EXT_descriptor_buffer = available_extensions.EXT_descriptor_buffer;
   extensions.EXT_mutable_descriptor_type = available_extensions.EXT_mutable_descriptor_type;
   extensions.VALVE_mutable_descriptor_type = available_extensions.VALVE_mutable_descriptor_type;
   extensions.KHR_draw_indirect_count = available_extensions.KHR_draw_indirect_count;
   extensions.EXT_sampler_filter_minmax = available_extensions.EXT_sampler_filter_minmax;
   extensions.KHR_pipeline_library = available_extensions.KHR_pipeline_library;
   extensions.EXT_graphics_pipeline_library = available_extensions.EXT_graphics_pipeline_library;
   extensions.EXT_transform_feedback = available_extensions.EXT_transform_feedback;
   extensions.EXT_vertex_input_dynamic_state = available_extensions.EXT_vertex_input_dynamic_state;
   extensions.EXT_extended_dynamic_state = available_extensions.EXT_extended_dynamic_state;
   extensions.EXT_extended_dynamic_state2 = available_extensions.EXT_extended_dynamic_state2;
   extensions.EXT_extended_dynamic_state3 = available_extensions.EXT_extended_dynamic_state3;
   extensions.KHR_dynamic_rendering_local_read = available_extensions.KHR_dynamic_rendering_local_read;
   extensions.KHR_depth_clamp_zero_one = available_extensions.KHR_depth_clamp_zero_one;
   extensions.EXT_depth_clamp_zero_one = available_extensions.EXT_depth_clamp_zero_one;
   extensions.KHR_index_type_uint8 = available_extensions.KHR_index_type_uint8;
   extensions.EXT_index_type_uint8 = available_extensions.EXT_index_type_uint8;
   extensions.KHR_line_rasterization = available_extensions.KHR_line_rasterization;
   extensions.EXT_line_rasterization = available_extensions.EXT_line_rasterization;
   extensions.KHR_load_store_op_none = available_extensions.KHR_load_store_op_none;
   extensions.EXT_load_store_op_none = available_extensions.EXT_load_store_op_none;
   extensions.KHR_vertex_attribute_divisor = available_extensions.KHR_vertex_attribute_divisor;
   extensions.EXT_vertex_attribute_divisor = available_extensions.EXT_vertex_attribute_divisor;
   extensions.EXT_attachment_feedback_loop_dynamic_state = available_extensions.EXT_attachment_feedback_loop_dynamic_state;
   extensions.EXT_attachment_feedback_loop_layout = available_extensions.EXT_attachment_feedback_loop_layout;
   extensions.EXT_border_color_swizzle = available_extensions.EXT_border_color_swizzle;
   extensions.EXT_color_write_enable = available_extensions.EXT_color_write_enable;
   extensions.EXT_conditional_rendering = available_extensions.EXT_conditional_rendering;
   extensions.EXT_custom_border_color = available_extensions.EXT_custom_border_color;
   extensions.EXT_depth_clamp_control = available_extensions.EXT_depth_clamp_control;
   extensions.EXT_depth_clip_control = available_extensions.EXT_depth_clip_control;
   extensions.EXT_depth_clip_enable = available_extensions.EXT_depth_clip_enable;
   extensions.EXT_dynamic_rendering_unused_attachments = available_extensions.EXT_dynamic_rendering_unused_attachments;
   extensions.EXT_image_2d_view_of_3d = available_extensions.EXT_image_2d_view_of_3d;
   extensions.EXT_image_view_min_lod = available_extensions.EXT_image_view_min_lod;
   extensions.EXT_multi_draw = available_extensions.EXT_multi_draw;
   extensions.EXT_multisampled_render_to_single_sampled = available_extensions.EXT_multisampled_render_to_single_sampled;
   extensions.EXT_non_seamless_cube_map = available_extensions.EXT_non_seamless_cube_map;
   extensions.EXT_pipeline_creation_feedback = available_extensions.EXT_pipeline_creation_feedback;
   extensions.EXT_primitive_topology_list_restart = available_extensions.EXT_primitive_topology_list_restart;
   extensions.EXT_primitives_generated_query = available_extensions.EXT_primitives_generated_query;
   extensions.EXT_provoking_vertex = available_extensions.EXT_provoking_vertex;
   extensions.EXT_rasterization_order_attachment_access = available_extensions.EXT_rasterization_order_attachment_access;
   extensions.ARM_rasterization_order_attachment_access = available_extensions.ARM_rasterization_order_attachment_access;
   extensions.EXT_sample_locations = available_extensions.EXT_sample_locations;
   extensions.EXT_shader_stencil_export = available_extensions.EXT_shader_stencil_export;
   extensions.EXT_shader_viewport_index_layer = available_extensions.EXT_shader_viewport_index_layer;
   device->vk.supported_features = {};
   auto &features = device->vk.supported_features;
   features.presentId = true;
   features.presentWait = true;
   features.presentId2 = true;
   features.presentWait2 = true;
   features.presentTiming = true;
   features.presentAtAbsoluteTime = true;
   features.presentAtRelativeTime = true;
   features.timelineSemaphore = true;
   features.bufferDeviceAddress = true;
   features.bufferDeviceAddressCaptureReplay = true;
   features.synchronization2 = available_features.synchronization2;
   features.maintenance4 = available_features.maintenance4;
   features.scalarBlockLayout = available_features.scalarBlockLayout;
   features.uniformBufferStandardLayout = available_features.uniformBufferStandardLayout;
   features.variablePointersStorageBuffer = available_features.variablePointersStorageBuffer;
   features.variablePointers = available_features.variablePointers;
   features.vulkanMemoryModel = available_features.vulkanMemoryModel;
   features.vulkanMemoryModelDeviceScope = available_features.vulkanMemoryModelDeviceScope;
   features.vulkanMemoryModelAvailabilityVisibilityChains = available_features.vulkanMemoryModelAvailabilityVisibilityChains;
   features.shaderSubgroupExtendedTypes = available_features.shaderSubgroupExtendedTypes;
   features.subgroupSizeControl = available_features.subgroupSizeControl;
   features.computeFullSubgroups = available_features.computeFullSubgroups;
   features.workgroupMemoryExplicitLayout = available_features.workgroupMemoryExplicitLayout;
   features.workgroupMemoryExplicitLayoutScalarBlockLayout = available_features.workgroupMemoryExplicitLayoutScalarBlockLayout;
   features.workgroupMemoryExplicitLayout8BitAccess = available_features.workgroupMemoryExplicitLayout8BitAccess;
   features.workgroupMemoryExplicitLayout16BitAccess = available_features.workgroupMemoryExplicitLayout16BitAccess;
   features.shaderZeroInitializeWorkgroupMemory = available_features.shaderZeroInitializeWorkgroupMemory;
   features.shaderInt16 = available_features.shaderInt16;
   features.shaderInt64 = available_features.shaderInt64;
   features.shaderFloat16 = available_features.shaderFloat16;
   features.shaderInt8 = available_features.shaderInt8;
   features.shaderIntegerDotProduct = available_features.shaderIntegerDotProduct;
   features.storageBuffer16BitAccess = available_features.storageBuffer16BitAccess;
   features.uniformAndStorageBuffer16BitAccess = available_features.uniformAndStorageBuffer16BitAccess;
   features.storagePushConstant16 = available_features.storagePushConstant16;
   features.storageInputOutput16 = available_features.storageInputOutput16;
   features.storageBuffer8BitAccess = available_features.storageBuffer8BitAccess;
   features.uniformAndStorageBuffer8BitAccess = available_features.uniformAndStorageBuffer8BitAccess;
   features.storagePushConstant8 = available_features.storagePushConstant8;
   features.robustBufferAccess = available_features.robustBufferAccess;
   features.samplerMirrorClampToEdge = available_features.samplerMirrorClampToEdge;
   features.largePoints = available_features.largePoints;
   features.wideLines = available_features.wideLines;
   features.fullDrawIndexUint32 = available_features.fullDrawIndexUint32;
   features.textureCompressionETC2 = available_features.textureCompressionETC2;
   features.textureCompressionASTC_LDR = available_features.textureCompressionASTC_LDR;
   features.textureCompressionBC = available_features.textureCompressionBC;
   features.shaderUniformBufferArrayDynamicIndexing = available_features.shaderUniformBufferArrayDynamicIndexing;
   features.shaderStorageBufferArrayDynamicIndexing = available_features.shaderStorageBufferArrayDynamicIndexing;
   features.shaderSampledImageArrayDynamicIndexing = available_features.shaderSampledImageArrayDynamicIndexing;
   features.shaderStorageImageArrayDynamicIndexing = available_features.shaderStorageImageArrayDynamicIndexing;
   features.shaderImageGatherExtended = available_features.shaderImageGatherExtended;
   features.shaderStorageImageExtendedFormats = available_features.shaderStorageImageExtendedFormats;
   features.shaderStorageImageReadWithoutFormat = available_features.shaderStorageImageReadWithoutFormat;
   features.shaderStorageImageWriteWithoutFormat = available_features.shaderStorageImageWriteWithoutFormat;
   features.multiview = available_features.multiview;
   features.samplerYcbcrConversion = available_features.samplerYcbcrConversion;
   features.shaderDrawParameters = available_features.shaderDrawParameters;
   features.subgroupBroadcastDynamicId = available_features.subgroupBroadcastDynamicId;
   features.imagelessFramebuffer = available_features.imagelessFramebuffer;
   features.separateDepthStencilLayouts = available_features.separateDepthStencilLayouts;
   features.hostQueryReset = available_features.hostQueryReset;
   features.shaderTerminateInvocation = available_features.shaderTerminateInvocation;
   features.shaderDemoteToHelperInvocation = available_features.shaderDemoteToHelperInvocation;
   features.privateData = available_features.privateData;
   features.pipelineCreationCacheControl = available_features.pipelineCreationCacheControl;
   features.robustImageAccess = available_features.robustImageAccess;
   features.dynamicRendering = available_features.dynamicRendering;
   features.inlineUniformBlock = available_features.inlineUniformBlock;
   features.descriptorIndexing = available_features.descriptorIndexing;
   features.shaderInputAttachmentArrayDynamicIndexing = available_features.shaderInputAttachmentArrayDynamicIndexing;
   features.shaderUniformTexelBufferArrayDynamicIndexing = available_features.shaderUniformTexelBufferArrayDynamicIndexing;
   features.shaderStorageTexelBufferArrayDynamicIndexing = available_features.shaderStorageTexelBufferArrayDynamicIndexing;
   features.shaderUniformBufferArrayNonUniformIndexing = available_features.shaderUniformBufferArrayNonUniformIndexing;
   features.shaderSampledImageArrayNonUniformIndexing = available_features.shaderSampledImageArrayNonUniformIndexing;
   features.shaderStorageBufferArrayNonUniformIndexing = available_features.shaderStorageBufferArrayNonUniformIndexing;
   features.shaderStorageImageArrayNonUniformIndexing = available_features.shaderStorageImageArrayNonUniformIndexing;
   features.shaderInputAttachmentArrayNonUniformIndexing = available_features.shaderInputAttachmentArrayNonUniformIndexing;
   features.shaderUniformTexelBufferArrayNonUniformIndexing = available_features.shaderUniformTexelBufferArrayNonUniformIndexing;
   features.shaderStorageTexelBufferArrayNonUniformIndexing = available_features.shaderStorageTexelBufferArrayNonUniformIndexing;
   features.descriptorBindingUniformBufferUpdateAfterBind = available_features.descriptorBindingUniformBufferUpdateAfterBind;
   features.descriptorBindingSampledImageUpdateAfterBind = available_features.descriptorBindingSampledImageUpdateAfterBind;
   features.descriptorBindingStorageImageUpdateAfterBind = available_features.descriptorBindingStorageImageUpdateAfterBind;
   features.descriptorBindingStorageBufferUpdateAfterBind = available_features.descriptorBindingStorageBufferUpdateAfterBind;
   features.descriptorBindingUniformTexelBufferUpdateAfterBind = available_features.descriptorBindingUniformTexelBufferUpdateAfterBind;
   features.descriptorBindingStorageTexelBufferUpdateAfterBind = available_features.descriptorBindingStorageTexelBufferUpdateAfterBind;
   features.descriptorBindingUpdateUnusedWhilePending = available_features.descriptorBindingUpdateUnusedWhilePending;
   features.descriptorBindingPartiallyBound = available_features.descriptorBindingPartiallyBound;
   features.descriptorBindingVariableDescriptorCount = available_features.descriptorBindingVariableDescriptorCount;
   features.descriptorBindingInlineUniformBlockUpdateAfterBind = available_features.descriptorBindingInlineUniformBlockUpdateAfterBind;
   features.runtimeDescriptorArray = available_features.runtimeDescriptorArray;
   features.descriptorBuffer = available_features.descriptorBuffer;
   features.descriptorBufferCaptureReplay = true;
   features.descriptorBufferImageLayoutIgnored = available_features.descriptorBufferImageLayoutIgnored;
   features.descriptorBufferPushDescriptors = available_features.descriptorBufferPushDescriptors;
   features.mutableDescriptorType = available_features.mutableDescriptorType;
   features.imageCubeArray = available_features.imageCubeArray;
   features.independentBlend = available_features.independentBlend;
   features.geometryShader = available_features.geometryShader;
   features.tessellationShader = available_features.tessellationShader;
   features.sampleRateShading = available_features.sampleRateShading;
   features.dualSrcBlend = available_features.dualSrcBlend;
   features.logicOp = available_features.logicOp;
   features.multiDrawIndirect = available_features.multiDrawIndirect;
   features.drawIndirectFirstInstance = available_features.drawIndirectFirstInstance;
   features.depthClamp = available_features.depthClamp;
   features.depthBiasClamp = available_features.depthBiasClamp;
   features.fillModeNonSolid = available_features.fillModeNonSolid;
   features.depthBounds = available_features.depthBounds;
   features.alphaToOne = available_features.alphaToOne;
   features.multiViewport = available_features.multiViewport;
   features.samplerAnisotropy = available_features.samplerAnisotropy;
   features.occlusionQueryPrecise = available_features.occlusionQueryPrecise;
   features.pipelineStatisticsQuery = available_features.pipelineStatisticsQuery;
   features.vertexPipelineStoresAndAtomics = available_features.vertexPipelineStoresAndAtomics;
   features.fragmentStoresAndAtomics = available_features.fragmentStoresAndAtomics;
   features.shaderTessellationAndGeometryPointSize = available_features.shaderTessellationAndGeometryPointSize;
   features.shaderClipDistance = available_features.shaderClipDistance;
   features.shaderCullDistance = available_features.shaderCullDistance;
   features.shaderResourceMinLod = available_features.shaderResourceMinLod;
   features.variableMultisampleRate = available_features.variableMultisampleRate;
   features.inheritedQueries = available_features.inheritedQueries;
   features.multiviewGeometryShader = available_features.multiviewGeometryShader;
   features.multiviewTessellationShader = available_features.multiviewTessellationShader;
   features.drawIndirectCount = available_features.drawIndirectCount;
   features.samplerFilterMinmax = available_features.samplerFilterMinmax;
   features.shaderOutputViewportIndex = available_features.shaderOutputViewportIndex;
   features.shaderOutputLayer = available_features.shaderOutputLayer;
   features.graphicsPipelineLibrary = available_features.graphicsPipelineLibrary;
   features.transformFeedback = available_features.transformFeedback;
   features.geometryStreams = available_features.geometryStreams;
   features.vertexInputDynamicState = available_features.vertexInputDynamicState;
   features.extendedDynamicState = available_features.extendedDynamicState;
   features.extendedDynamicState2 = available_features.extendedDynamicState2;
   features.extendedDynamicState2LogicOp = available_features.extendedDynamicState2LogicOp;
   features.extendedDynamicState2PatchControlPoints = available_features.extendedDynamicState2PatchControlPoints;
   features.extendedDynamicState3PolygonMode = available_features.extendedDynamicState3PolygonMode;
   features.extendedDynamicState3TessellationDomainOrigin = available_features.extendedDynamicState3TessellationDomainOrigin;
   features.extendedDynamicState3DepthClampEnable = available_features.extendedDynamicState3DepthClampEnable;
   features.extendedDynamicState3DepthClipEnable = available_features.extendedDynamicState3DepthClipEnable;
   features.extendedDynamicState3LogicOpEnable = available_features.extendedDynamicState3LogicOpEnable;
   features.extendedDynamicState3SampleMask = available_features.extendedDynamicState3SampleMask;
   features.extendedDynamicState3RasterizationSamples = available_features.extendedDynamicState3RasterizationSamples;
   features.extendedDynamicState3AlphaToCoverageEnable = available_features.extendedDynamicState3AlphaToCoverageEnable;
   features.extendedDynamicState3AlphaToOneEnable = available_features.extendedDynamicState3AlphaToOneEnable;
   features.extendedDynamicState3DepthClipNegativeOneToOne = available_features.extendedDynamicState3DepthClipNegativeOneToOne;
   features.extendedDynamicState3RasterizationStream = available_features.extendedDynamicState3RasterizationStream;
   features.extendedDynamicState3ConservativeRasterizationMode = available_features.extendedDynamicState3ConservativeRasterizationMode;
   features.extendedDynamicState3ExtraPrimitiveOverestimationSize = available_features.extendedDynamicState3ExtraPrimitiveOverestimationSize;
   features.extendedDynamicState3LineRasterizationMode = available_features.extendedDynamicState3LineRasterizationMode;
   features.extendedDynamicState3LineStippleEnable = available_features.extendedDynamicState3LineStippleEnable;
   features.extendedDynamicState3ProvokingVertexMode = available_features.extendedDynamicState3ProvokingVertexMode;
   features.extendedDynamicState3SampleLocationsEnable = available_features.extendedDynamicState3SampleLocationsEnable;
   features.extendedDynamicState3ColorBlendEnable = available_features.extendedDynamicState3ColorBlendEnable;
   features.extendedDynamicState3ColorBlendEquation = available_features.extendedDynamicState3ColorBlendEquation;
   features.extendedDynamicState3ColorWriteMask = available_features.extendedDynamicState3ColorWriteMask;
   features.extendedDynamicState3ViewportWScalingEnable = available_features.extendedDynamicState3ViewportWScalingEnable;
   features.extendedDynamicState3ViewportSwizzle = available_features.extendedDynamicState3ViewportSwizzle;
   features.extendedDynamicState3ShadingRateImageEnable = available_features.extendedDynamicState3ShadingRateImageEnable;
   features.extendedDynamicState3CoverageToColorEnable = available_features.extendedDynamicState3CoverageToColorEnable;
   features.extendedDynamicState3CoverageToColorLocation = available_features.extendedDynamicState3CoverageToColorLocation;
   features.extendedDynamicState3CoverageModulationMode = available_features.extendedDynamicState3CoverageModulationMode;
   features.extendedDynamicState3CoverageModulationTableEnable = available_features.extendedDynamicState3CoverageModulationTableEnable;
   features.extendedDynamicState3CoverageModulationTable = available_features.extendedDynamicState3CoverageModulationTable;
   features.extendedDynamicState3CoverageReductionMode = available_features.extendedDynamicState3CoverageReductionMode;
   features.extendedDynamicState3RepresentativeFragmentTestEnable = available_features.extendedDynamicState3RepresentativeFragmentTestEnable;
   features.extendedDynamicState3ColorBlendAdvanced = available_features.extendedDynamicState3ColorBlendAdvanced;
   features.dynamicRenderingLocalRead = available_features.dynamicRenderingLocalRead;
   features.depthClampZeroOne = available_features.depthClampZeroOne;
   features.indexTypeUint8 = available_features.indexTypeUint8;
   features.rectangularLines = available_features.rectangularLines;
   features.bresenhamLines = available_features.bresenhamLines;
   features.smoothLines = available_features.smoothLines;
   features.stippledRectangularLines = available_features.stippledRectangularLines;
   features.stippledBresenhamLines = available_features.stippledBresenhamLines;
   features.stippledSmoothLines = available_features.stippledSmoothLines;
   features.vertexAttributeInstanceRateDivisor = available_features.vertexAttributeInstanceRateDivisor;
   features.vertexAttributeInstanceRateZeroDivisor = available_features.vertexAttributeInstanceRateZeroDivisor;
   features.attachmentFeedbackLoopDynamicState = available_features.attachmentFeedbackLoopDynamicState;
   features.attachmentFeedbackLoopLayout = available_features.attachmentFeedbackLoopLayout;
   features.borderColorSwizzle = available_features.borderColorSwizzle;
   features.borderColorSwizzleFromImage = available_features.borderColorSwizzleFromImage;
   features.colorWriteEnable = available_features.colorWriteEnable;
   features.conditionalRendering = available_features.conditionalRendering;
   features.inheritedConditionalRendering = available_features.inheritedConditionalRendering;
   features.customBorderColors = available_features.customBorderColors;
   features.customBorderColorWithoutFormat = available_features.customBorderColorWithoutFormat;
   features.depthClampControl = available_features.depthClampControl;
   features.depthClipControl = available_features.depthClipControl;
   features.depthClipEnable = available_features.depthClipEnable;
   features.dynamicRenderingUnusedAttachments = available_features.dynamicRenderingUnusedAttachments;
   features.image2DViewOf3D = available_features.image2DViewOf3D;
   features.sampler2DViewOf3D = available_features.sampler2DViewOf3D;
   features.minLod = available_features.minLod;
   features.multiDraw = available_features.multiDraw;
   features.multisampledRenderToSingleSampled = available_features.multisampledRenderToSingleSampled;
   features.nonSeamlessCubeMap = available_features.nonSeamlessCubeMap;
   features.primitiveTopologyListRestart = available_features.primitiveTopologyListRestart;
   features.primitiveTopologyPatchListRestart = available_features.primitiveTopologyPatchListRestart;
   features.primitivesGeneratedQuery = available_features.primitivesGeneratedQuery;
   features.primitivesGeneratedQueryWithRasterizerDiscard = available_features.primitivesGeneratedQueryWithRasterizerDiscard;
   features.primitivesGeneratedQueryWithNonZeroStreams = available_features.primitivesGeneratedQueryWithNonZeroStreams;
   features.provokingVertexLast = available_features.provokingVertexLast;
   features.rasterizationOrderColorAttachmentAccess = available_features.rasterizationOrderColorAttachmentAccess;
   features.rasterizationOrderDepthAttachmentAccess = available_features.rasterizationOrderDepthAttachmentAccess;
   features.rasterizationOrderStencilAttachmentAccess = available_features.rasterizationOrderStencilAttachmentAccess;
   device->vk.properties.apiVersion = VK_MAKE_VERSION(1, 3, 0);
   device->vk.properties.maxMemoryAllocationSize = UINT32_MAX - 65535;
   device->vk.properties.maxBufferSize = UINT32_MAX - 65535;
   memcpy(device->vk.properties.deviceLUID, &adapter->AdapterLuid, VK_LUID_SIZE);
   device->vk.properties.deviceNodeMask = 1;
   device->vk.properties.deviceLUIDValid = true;
   static const VkQueueFamilyProperties compute_queue = {
      .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
      .queueCount = 2,
      .timestampValidBits = 48,
      .minImageTransferGranularity = {1, 1, 1},
   };
   device->queue_families[0].properties = &compute_queue;
   adapter->hAdapter = 0;
   list_addtail(&device->vk.link, &instance->vk.physical_devices.list);
   return VK_SUCCESS;
}

VkResult
tu_knl_wddm_load(struct tu_instance *instance)
{
   D3DKMT_ENUMADAPTERS2 enumeration = {};
   if (D3DKMTEnumAdapters2(&enumeration) < 0)
      return VK_ERROR_INITIALIZATION_FAILED;
   std::vector<D3DKMT_ADAPTERINFO> adapters(enumeration.NumAdapters);
   enumeration.pAdapters = adapters.data();
   if (D3DKMTEnumAdapters2(&enumeration) < 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   VkResult result = VK_SUCCESS;
   for (UINT i = 0; i < enumeration.NumAdapters; i++) {
      if (result == VK_SUCCESS) {
         VkResult added = wddm_add_adapter(instance, &adapters[i]);
         if (added != VK_SUCCESS && added != VK_ERROR_INCOMPATIBLE_DRIVER)
            result = added;
      }
      if (adapters[i].hAdapter) {
         D3DKMT_CLOSEADAPTER close = {};
         close.hAdapter = adapters[i].hAdapter;
         D3DKMTCloseAdapter(&close);
      }
   }
   return result;
}
