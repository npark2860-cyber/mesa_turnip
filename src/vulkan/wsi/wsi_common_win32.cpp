/*
 * Copyright 짤 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "util/cnd_monotonic.h"
#include "util/timespec.h"
#include "util/u_thread.h"
#include "vk_format.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"

#include <dxgi1_4.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>

#include <dcomp.h>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"      // warning: cast to pointer from integer of different size
#endif

struct wsi_win32;

static bool
wsi_win32_option_enabled(const char *name)
{
   const char *value = getenv(name);
   return value && !strcmp(value, "1");
}

struct wsi_win32 {
   struct wsi_interface                     base;

   struct wsi_device *wsi;

   const VkAllocationCallbacks *alloc;
   VkPhysicalDevice physical_device;
   struct {
      IDXGIFactory4 *factory;
      IDCompositionDevice *dcomp;
   } dxgi;
};

enum wsi_win32_image_state {
   WSI_IMAGE_IDLE,
   WSI_IMAGE_DRAWING,
   WSI_IMAGE_QUEUED,
};

#define WSI_WIN32_VERIFY_MAX_SAMPLES 4

struct wsi_win32_verify_sample {
   uint64_t serial;
   uint64_t hash;
   uint32_t first_pixel;
   uint32_t center_pixel;
   uint32_t last_pixel;
   HRESULT result;
};

#include "gpu_profile.hpp"
struct NativeGpuProfile;

struct wsi_win32_image {
   struct wsi_image base;
   enum wsi_win32_image_state state;
   struct wsi_win32_swapchain *chain;
   struct {
      ID3D12Resource *swapchain_res;
      ID3D12Resource *buffer;
      ID3D12CommandAllocator *allocator;
      ID3D12GraphicsCommandList *list;
      ID3D12Fence *complete;
      ID3D12Fence *acquire_fence;
      ID3D12CommandAllocator *acquire_allocator;
      ID3D12GraphicsCommandList *acquire_list;
      ID3D12DescriptorHeap *rtv_heap;
      NativeGpuProfile *profile;
      bool native_prepared;
      bool native_initialized;
      uint64_t serial;
      HANDLE event;
      ID3D12QueryHeap *copy_query_heap;
      ID3D12Resource *copy_query_readback;
      uint64_t copy_timing_collected_serial;
      ID3D12Resource *verify_readback;
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT verify_footprint;
      uint64_t verify_size;
      uint64_t verify_pending_serial;
      uint64_t verify_collected_serial;
      uint32_t verify_submitted;
      uint32_t verify_collected;
      struct wsi_win32_verify_sample verify_samples[WSI_WIN32_VERIFY_MAX_SAMPLES];
   } dxgi;
   struct {
      HDC dc;
      HBITMAP bmp;
      int bmp_row_pitch;
      void *ppvBits;
   } sw;
};

struct wsi_win32_surface {
   VkIcdSurfaceWin32 base;

   /* The first time a swapchain is created against this surface, a DComp
    * target/visual will be created for it and that swapchain will be bound.
    * When a new swapchain is created, we delay changing the visual's content
    * until that swapchain has completed its first present once, otherwise the
    * window will flash white. When the currently-bound swapchain is destroyed,
    * the visual's content is unset.
    */
   IDCompositionTarget *target;
   IDCompositionVisual *visual;
   struct wsi_win32_swapchain *current_swapchain;
};

struct wsi_win32_present {
   struct wsi_win32_present *next;
   uint64_t id;
   UINT count;
   ID3D12Fence *copy_fence;
   uint64_t copy_value;
   bool discarded;
   uint64_t timing_serial;
   struct wsi_image *image;
};

#define WSI_WIN32_COPY_TIMING_MAX_SAMPLES 512

struct wsi_win32_copy_timing_sample {
   uint32_t image_index;
   uint64_t serial;
   uint64_t vk_results[4];
   VkResult vk_result;
   uint64_t d3d_results[2];
   bool d3d_valid;
};

struct wsi_win32_swapchain {
   struct wsi_swapchain         base;
   IDXGISwapChain3            *dxgi;
   HANDLE native_latency;
   bool native_latency_ready;
   uint32_t native_max_latency;
   bool native_wait_stats;
   uint64_t native_stats_start;
   uint64_t native_acquires;
   uint64_t native_presents;
   uint64_t native_latency_ns;
   uint64_t native_reuse_ns;
   uint64_t native_present_ns;
   uint64_t native_latency_max;
   uint64_t native_reuse_max;
   uint64_t native_present_max;
   struct wsi_win32           *wsi;
   wsi_win32_surface          *surface;
   mtx_t                      acquire_mutex;
   struct u_cnd_monotonic     acquire_cond;
   uint64_t                     flip_sequence;
   VkResult                     status;
   VkExtent2D                 extent;
   HWND wnd;
   HDC chain_dc;
   struct wsi_win32_present *present_head;
   struct wsi_win32_present *present_tail;
   uint64_t completed_id;
   struct wsi_image_timing_request timing_request;
   bool copy_timing_requested;
   bool copy_timing_full;
   bool verify_requested;
   struct wsi_win32_copy_timing_sample *copy_timing_samples;
   uint32_t copy_timing_sample_count;
   uint64_t copy_timing_observations;
   uint64_t copy_timing_frequency;
   uint32_t copy_timing_initialized_images;
   uint32_t copy_timing_setup_failures;
   VkResult copy_timing_vk_error;
   HRESULT copy_timing_hr;
   struct wsi_win32_image     images[0];
};

#include "native_gpu_profile.inc"

static void
wsi_native_report_waits(struct wsi_win32_swapchain *chain, bool final)
{
   if (!chain->native_wait_stats || !chain->native_stats_start)
      return;
   uint64_t now = os_time_get_nano();
   uint64_t elapsed = now - chain->native_stats_start;
   if (!final && elapsed < 5000000000ULL)
      return;
   if (chain->native_acquires || chain->native_presents)
      fprintf(stderr, "WSI_NATIVE_WAIT_SUMMARY chain=%p final=%u latency=%u width=%u height=%u window_s=%.3f acquires=%llu presents=%llu admission_ms=%.3f admission_max_ms=%.3f reuse_ms=%.3f reuse_max_ms=%.3f present_call_ms=%.3f present_call_max_ms=%.3f scope=cpu_call_wall_time\n",
         (void *)chain, (unsigned)final, chain->native_max_latency,
         chain->extent.width, chain->extent.height, elapsed / 1e9,
         (unsigned long long)chain->native_acquires, (unsigned long long)chain->native_presents,
         chain->native_latency_ns / 1e6, chain->native_latency_max / 1e6,
         chain->native_reuse_ns / 1e6, chain->native_reuse_max / 1e6,
         chain->native_present_ns / 1e6, chain->native_present_max / 1e6);
   chain->native_stats_start = now;
   chain->native_acquires = chain->native_presents = 0;
   chain->native_latency_ns = chain->native_reuse_ns = chain->native_present_ns = 0;
   chain->native_latency_max = chain->native_reuse_max = chain->native_present_max = 0;
}

static void
wsi_win32_collect_verify(struct wsi_win32_image *image)
{
   if (!image->dxgi.verify_readback || !image->dxgi.verify_pending_serial ||
       image->dxgi.verify_pending_serial == image->dxgi.verify_collected_serial ||
       image->dxgi.verify_collected >= WSI_WIN32_VERIFY_MAX_SAMPLES)
      return;
   auto chain = image->chain;
   auto sample = &image->dxgi.verify_samples[image->dxgi.verify_collected++];
   sample->serial = image->dxgi.verify_pending_serial;
   image->dxgi.verify_collected_serial = sample->serial;
   D3D12_RANGE read_range = {0, (SIZE_T)image->dxgi.verify_size};
   void *mapped = NULL;
   sample->result = image->dxgi.verify_readback->Map(0, &read_range, &mapped);
   if (FAILED(sample->result))
      return;
   const uint8_t *pixels = (const uint8_t *)mapped + image->dxgi.verify_footprint.Offset;
   uint64_t hash = 14695981039346656037ULL;
   uint32_t pitch = image->dxgi.verify_footprint.Footprint.RowPitch;
   for (uint32_t y = 0; y < chain->extent.height; y++) {
      const uint8_t *row = pixels + (size_t)y * pitch;
      for (uint32_t x = 0; x < chain->extent.width * 4; x++) {
         hash ^= row[x];
         hash *= 1099511628211ULL;
      }
   }
   sample->hash = hash;
   memcpy(&sample->first_pixel, pixels, 4);
   memcpy(&sample->center_pixel,
          pixels + (size_t)(chain->extent.height / 2) * pitch +
             (chain->extent.width / 2) * 4, 4);
   memcpy(&sample->last_pixel,
          pixels + (size_t)(chain->extent.height - 1) * pitch +
             (chain->extent.width - 1) * 4, 4);
   D3D12_RANGE write_range = {0, 0};
   image->dxgi.verify_readback->Unmap(0, &write_range);
}

static VkResult
wsi_win32_init_verify(struct wsi_win32_swapchain *chain,
                        struct wsi_win32_image *image, ID3D12Device *device)
{
   if (!chain->verify_requested)
      return VK_SUCCESS;
   D3D12_RESOURCE_DESC texture = image->dxgi.swapchain_res->GetDesc();
   UINT rows = 0;
   UINT64 row_size = 0, size = 0;
   device->GetCopyableFootprints(&texture, 0, 1, 0,
                                   &image->dxgi.verify_footprint,
                                   &rows, &row_size, &size);
   if (!size || size > 64ULL * 1024 * 1024 || rows != chain->extent.height ||
       row_size != (uint64_t)chain->extent.width * 4 ||
       image->dxgi.verify_footprint.Footprint.Depth != 1) {
      fprintf(stderr, "WSI_VERIFY_REJECT reason=footprint size=%llu rows=%u row_bytes=%llu\n",
              (unsigned long long)size, rows, (unsigned long long)row_size);
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   D3D12_HEAP_PROPERTIES heap = {};
   heap.Type = D3D12_HEAP_TYPE_READBACK;
   heap.CreationNodeMask = heap.VisibleNodeMask = 1;
   D3D12_RESOURCE_DESC desc = {};
   desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   desc.Width = size;
   desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
   desc.SampleDesc.Count = 1;
   desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                                  &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  NULL, IID_PPV_ARGS(&image->dxgi.verify_readback));
   if (FAILED(hr)) {
      fprintf(stderr, "WSI_VERIFY_REJECT reason=readback_allocation hr=%lx\n", (unsigned long)hr);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   image->dxgi.verify_size = size;
   return VK_SUCCESS;
}

static void
wsi_win32_report_verify(struct wsi_win32_image *image)
{
   if (!image->chain || !image->chain->verify_requested)
      return;
   auto chain = image->chain;
   uint32_t index = image - chain->images;
   for (uint32_t i = 0; i < image->dxgi.verify_collected; i++) {
      const auto sample = &image->dxgi.verify_samples[i];
      fprintf(stderr, "WSI_VERIFY image=%u serial=%llu width=%u height=%u format=%u linear_shared=%u row_pitch=%u bytes=%llu map_hr=%lx fnv1a64=%016llx first=%08x center=%08x last=%08x\n",
              index, (unsigned long long)sample->serial, chain->extent.width,
              chain->extent.height, (unsigned)chain->base.image_info.create.format,
              (unsigned)image->base.blit.linear_shared,
              image->dxgi.verify_footprint.Footprint.RowPitch,
              (unsigned long long)chain->extent.width * chain->extent.height * 4,
              (unsigned long)sample->result, (unsigned long long)sample->hash,
              sample->first_pixel, sample->center_pixel, sample->last_pixel);
   }
}

static void
wsi_win32_init_copy_timing(struct wsi_win32_swapchain *chain,
                            struct wsi_win32_image *image,
                            ID3D12Device *device)
{
   if (!chain->copy_timing_samples)
      return;

   const struct wsi_device *wsi = chain->base.wsi;
   bool supported = !chain->base.blit.queue &&
                    wsi->timestamp_period > 0.0f &&
                    wsi->timestamp_bits > 0 && wsi->timestamp_bits <= 64;
   for (uint32_t i = 0; i < wsi->queue_family_count && supported; i++) {
      if (chain->base.cmd_pools[i] &&
          !(wsi->queue_supports_timestamps & BITFIELD64_BIT(i)))
         supported = false;
   }
   if (!supported) {
      chain->copy_timing_setup_failures++;
      chain->copy_timing_vk_error = VK_ERROR_FEATURE_NOT_PRESENT;
      return;
   }

   auto queue = (ID3D12CommandQueue *)wsi->win32.get_d3d12_command_queue(chain->base.device);
   UINT64 frequency = 0;
   HRESULT hr = queue ? queue->GetTimestampFrequency(&frequency) : E_FAIL;
   if (SUCCEEDED(hr) && !frequency)
      hr = E_FAIL;
   D3D12_QUERY_HEAP_DESC query_desc = {};
   query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
   query_desc.Count = 2;
   if (SUCCEEDED(hr))
      hr = device->CreateQueryHeap(&query_desc,
                                    IID_PPV_ARGS(&image->dxgi.copy_query_heap));

   D3D12_HEAP_PROPERTIES heap = {};
   heap.Type = D3D12_HEAP_TYPE_READBACK;
   heap.CreationNodeMask = heap.VisibleNodeMask = 1;
   D3D12_RESOURCE_DESC desc = {};
   desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   desc.Width = 2 * sizeof(uint64_t);
   desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
   desc.SampleDesc.Count = 1;
   desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   if (SUCCEEDED(hr))
      hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                             &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             NULL, IID_PPV_ARGS(&image->dxgi.copy_query_readback));

   VkResult result = VK_SUCCESS;
   if (SUCCEEDED(hr)) {
      VkQueryPoolCreateInfo query_info = {};
      query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      query_info.queryCount = 2;
      result = wsi->CreateQueryPool(chain->base.device, &query_info,
                                     &chain->base.alloc, &image->base.blit.copy_query_pool);
   }
   if (SUCCEEDED(hr) && result == VK_SUCCESS) {
      image->base.blit.copy_timing_full = chain->copy_timing_full;
      chain->copy_timing_frequency = frequency;
      chain->copy_timing_initialized_images++;
      return;
   }

   chain->copy_timing_setup_failures++;
   chain->copy_timing_vk_error = result;
   chain->copy_timing_hr = hr;
   if (image->dxgi.copy_query_readback) {
      image->dxgi.copy_query_readback->Release();
      image->dxgi.copy_query_readback = NULL;
   }
   if (image->dxgi.copy_query_heap) {
      image->dxgi.copy_query_heap->Release();
      image->dxgi.copy_query_heap = NULL;
   }
   image->base.blit.copy_query_pool = VK_NULL_HANDLE;
}

static void
wsi_win32_collect_copy_timing(struct wsi_win32_image *image)
{
   wsi_win32_collect_verify(image);
   if (!image->dxgi.copy_query_heap ||
       image->dxgi.serial == image->dxgi.copy_timing_collected_serial)
      return;

   struct wsi_win32_swapchain *chain = image->chain;
   image->dxgi.copy_timing_collected_serial = image->dxgi.serial;
   chain->copy_timing_observations++;
   if (chain->copy_timing_sample_count >= WSI_WIN32_COPY_TIMING_MAX_SAMPLES)
      return;

   auto sample = &chain->copy_timing_samples[chain->copy_timing_sample_count++];
   sample->image_index = image - chain->images;
   sample->serial = image->dxgi.serial;
   sample->vk_result = chain->base.wsi->GetQueryPoolResults(
      chain->base.device, image->base.blit.copy_query_pool, 0, 2,
      sizeof(sample->vk_results), sample->vk_results, 2 * sizeof(uint64_t),
      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

   D3D12_RANGE read_range = {0, sizeof(sample->d3d_results)};
   void *mapped = NULL;
   if (SUCCEEDED(image->dxgi.copy_query_readback->Map(0, &read_range, &mapped))) {
      memcpy(sample->d3d_results, mapped, sizeof(sample->d3d_results));
      D3D12_RANGE write_range = {0, 0};
      image->dxgi.copy_query_readback->Unmap(0, &write_range);
      sample->d3d_valid = sample->d3d_results[1] >= sample->d3d_results[0];
   }
}

static void
wsi_win32_report_copy_timing(struct wsi_win32_swapchain *chain)
{
   if (!chain->copy_timing_requested)
      return;

   const struct wsi_device *wsi = chain->base.wsi;
   fprintf(stderr,
           "WSI_COPY_TIMING_BEGIN chain=%p width=%u height=%u format=%u samples=%u observed=%llu initialized_images=%u failed_images=%u vk_setup_result=%d d3d_setup_hr=%lx vk_period_ns=%.9f vk_valid_bits=%u d3d_hz=%llu sample_limit=%u scope=%s linear_shared=%u\n",
           (void *)chain, chain->extent.width, chain->extent.height,
           (unsigned)chain->base.image_info.create.format,
           chain->copy_timing_sample_count,
           (unsigned long long)chain->copy_timing_observations,
           chain->copy_timing_initialized_images, chain->copy_timing_setup_failures,
           chain->copy_timing_vk_error, (unsigned long)chain->copy_timing_hr,
           (double)wsi->timestamp_period, wsi->timestamp_bits,
           (unsigned long long)chain->copy_timing_frequency,
           WSI_WIN32_COPY_TIMING_MAX_SAMPLES,
           chain->copy_timing_full ? "wsi_commands_and_end_flush" : "copy_only_all_commands",
           (unsigned)chain->base.image_info.win32_linear_shared);
   uint64_t mask = wsi->timestamp_bits > 0 && wsi->timestamp_bits < 64 ?
      UINT64_MAX >> (64 - wsi->timestamp_bits) : UINT64_MAX;
   for (uint32_t i = 0; i < chain->copy_timing_sample_count; i++) {
      const auto sample = &chain->copy_timing_samples[i];
      bool vk_valid = sample->vk_result == VK_SUCCESS &&
                      sample->vk_results[1] && sample->vk_results[3];
      double vk_us = vk_valid ?
         (double)((sample->vk_results[2] - sample->vk_results[0]) & mask) *
            wsi->timestamp_period / 1000.0 : -1.0;
      double d3d_us = sample->d3d_valid && chain->copy_timing_frequency ?
         (double)(sample->d3d_results[1] - sample->d3d_results[0]) *
            1000000.0 / (double)chain->copy_timing_frequency : -1.0;
      fprintf(stderr,
              "WSI_COPY_TIMING sample=%u image=%u serial=%llu row_pitch=%u payload_bytes=%llu vk_result=%d vk_available0=%llu vk_available1=%llu vk_start=%llu vk_end=%llu vk_us=%.6f d3d_valid=%u d3d_start=%llu d3d_end=%llu d3d_us=%.6f\n",
              i, sample->image_index, (unsigned long long)sample->serial,
              chain->images[sample->image_index].base.row_pitches[0],
              (unsigned long long)chain->extent.width * chain->extent.height *
                 vk_format_get_blocksize(chain->base.image_info.create.format),
              sample->vk_result, (unsigned long long)sample->vk_results[1],
              (unsigned long long)sample->vk_results[3],
              (unsigned long long)sample->vk_results[0],
              (unsigned long long)sample->vk_results[2], vk_us,
              (unsigned)sample->d3d_valid,
              (unsigned long long)sample->d3d_results[0],
              (unsigned long long)sample->d3d_results[1], d3d_us);
   }
   fprintf(stderr, "WSI_COPY_TIMING_END chain=%p\n", (void *)chain);
   fflush(stderr);
   free(chain->copy_timing_samples);
   chain->copy_timing_samples = NULL;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
wsi_GetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                 uint32_t queueFamilyIndex)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   struct wsi_device *wsi_device = pdevice->wsi_device;
   return (wsi_device->queue_supports_blit & BITFIELD64_BIT(queueFamilyIndex)) != 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateWin32SurfaceKHR(VkInstance _instance,
                          const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   wsi_win32_surface *surface;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR);

   surface = (wsi_win32_surface *)vk_zalloc2(&instance->alloc, pAllocator, sizeof(*surface), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.base.platform = VK_ICD_WSI_PLATFORM_WIN32;

   surface->base.hinstance = pCreateInfo->hinstance;
   surface->base.hwnd = pCreateInfo->hwnd;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base.base);

   return VK_SUCCESS;
}

void
wsi_win32_surface_destroy(VkIcdSurfaceBase *icd_surface, VkInstance _instance,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   wsi_win32_surface *surface = (wsi_win32_surface *)icd_surface;
   if (surface->visual)
      surface->visual->Release();
   if (surface->target)
      surface->target->Release();
   vk_free2(&instance->alloc, pAllocator, icd_surface);
}

static VkResult
wsi_win32_surface_get_support(VkIcdSurfaceBase *surface,
                              struct wsi_device *wsi_device,
                              uint32_t queueFamilyIndex,
                              VkBool32* pSupported)
{
   *pSupported = true;

   return VK_SUCCESS;
}

static VkResult
wsi_win32_surface_get_capabilities(VkIcdSurfaceBase *surf,
                                   struct wsi_device *wsi_device,
                                   VkSurfaceCapabilities2KHR* caps)
{
   VkIcdSurfaceWin32 *surface = (VkIcdSurfaceWin32 *)surf;

   RECT win_rect;
   if (!GetClientRect(surface->hwnd, &win_rect))
      return VK_ERROR_SURFACE_LOST_KHR;

   caps->surfaceCapabilities.minImageCount = 1;

   if (!wsi_device->sw && wsi_device->win32.get_d3d12_command_queue) {
      /* DXGI doesn't support random presenting order (images need to
       * be presented in the order they were acquired), so we can't
       * expose more than two image per swapchain.
       */
      caps->surfaceCapabilities.minImageCount = caps->surfaceCapabilities.maxImageCount = 2;
   } else {
      caps->surfaceCapabilities.minImageCount = 1;
      /* Software callbacke, there is no real maximum */
      caps->surfaceCapabilities.maxImageCount = 0;
   }

   caps->surfaceCapabilities.currentExtent = {
      (uint32_t)win_rect.right - (uint32_t)win_rect.left,
      (uint32_t)win_rect.bottom - (uint32_t)win_rect.top
   };
   caps->surfaceCapabilities.minImageExtent = { 1u, 1u };
   caps->surfaceCapabilities.maxImageExtent = {
      wsi_device->maxImageDimension2D,
      wsi_device->maxImageDimension2D,
   };

   caps->surfaceCapabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.maxImageArrayLayers = 1;

   caps->surfaceCapabilities.supportedCompositeAlpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR |
      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
   if (wsi_device->win32.create_buffer_memory)
      caps->surfaceCapabilities.supportedCompositeAlpha &=
         ~VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;

   VkImageUsageFlags image_usage = wsi_caps_get_image_usage();

   VK_FROM_HANDLE(vk_physical_device, pdevice, wsi_device->pdevice);
   if (pdevice->supported_extensions.EXT_attachment_feedback_loop_layout)
      image_usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;

   VkImageUsageFlags2CreateInfoKHR *usage2 =
      vk_find_struct(caps->pNext, IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR);
   if (usage2) {
      usage2->usage = image_usage;
   } else {
      caps->surfaceCapabilities.supportedUsageFlags = image_usage;
   }

   return VK_SUCCESS;
}

static VkResult
wsi_win32_surface_get_capabilities2(VkIcdSurfaceBase *surface,
                                    struct wsi_device *wsi_device,
                                    const void *info_next,
                                    VkSurfaceCapabilities2KHR* caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   const VkSurfacePresentModeKHR *present_mode =
      (const VkSurfacePresentModeKHR *)vk_find_struct_const(info_next, SURFACE_PRESENT_MODE_KHR);

   VkResult result =
      wsi_win32_surface_get_capabilities(surface, wsi_device,
                                         caps);

   vk_foreach_struct(sType, ext, caps->pNext) {
      switch (sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected_cap = (VkSurfaceProtectedCapabilitiesKHR *)ext;
         protected_cap->supportsProtected = VK_FALSE;
         break;
      }

      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR: {
         /* Unsupported. */
         VkSurfacePresentScalingCapabilitiesEXT *scaling =
            (VkSurfacePresentScalingCapabilitiesEXT *)ext;
         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;
         scaling->minScaledImageExtent = caps->surfaceCapabilities.minImageExtent;
         scaling->maxScaledImageExtent = caps->surfaceCapabilities.maxImageExtent;
         break;
      }

      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR: {
         /* Unsupported, just report the input present mode. */
         VkSurfacePresentModeCompatibilityKHR *compat =
            (VkSurfacePresentModeCompatibilityKHR *)ext;
         if (compat->pPresentModes) {
            if (compat->presentModeCount) {
               assert(present_mode);
               compat->pPresentModes[0] = present_mode->presentMode;
               compat->presentModeCount = 1;
            }
         } else {
            if (!present_mode)
               wsi_common_vk_warn_once("Use of VkSurfacePresentModeCompatibilityKHR "
                                       "without a VkSurfacePresentModeKHR set. This is an "
                                       "application bug.\n");
            compat->presentModeCount = 1;
         }
         break;
      }

      case VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT: {
         VkPresentTimingSurfaceCapabilitiesEXT *wait = (VkPresentTimingSurfaceCapabilitiesEXT *)ext;

         wait->presentStageQueries = 0;
         wait->presentTimingSupported = VK_FALSE;
         wait->presentAtAbsoluteTimeSupported = VK_FALSE;
         wait->presentAtRelativeTimeSupported = VK_FALSE;
         if (wsi_device->win32.create_buffer_memory) {
            wait->presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
            wait->presentTimingSupported = VK_TRUE;
            wait->presentAtAbsoluteTimeSupported = VK_TRUE;
            wait->presentAtRelativeTimeSupported = VK_TRUE;
         }
         break;
      }

      case VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR:
         ((VkSurfaceCapabilitiesPresentId2KHR *)ext)->presentId2Supported =
            wsi_device->win32.create_buffer_memory != NULL;
         break;

      case VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR:
         ((VkSurfaceCapabilitiesPresentWait2KHR *)ext)->presentWait2Supported =
            wsi_device->win32.create_buffer_memory != NULL;
         break;

      case VK_STRUCTURE_TYPE_SWAPCHAIN_FLAGS_SURFACE_CAPABILITIES_EXT: {
         VkSwapchainFlagsSurfaceCapabilitiesEXT *surface_caps = (VkSwapchainFlagsSurfaceCapabilitiesEXT*)ext;
         VK_FROM_HANDLE(vk_physical_device, pdevice, wsi_device->pdevice);

         if (pdevice->supported_extensions.EXT_multisampled_render_to_swapchain)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT;
         if (pdevice->supported_extensions.KHR_bind_memory2)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR;
         if (pdevice->supported_extensions.KHR_present_id2)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
         if (pdevice->supported_extensions.KHR_present_wait2)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
         if (pdevice->supported_extensions.KHR_swapchain_mutable_format)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
         if (pdevice->supported_extensions.EXT_present_timing)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
         if (pdevice->supported_extensions.KHR_swapchain_maintenance1 ||
             pdevice->supported_extensions.EXT_swapchain_maintenance1)
            surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_EXT;
      }
      break;

      default:
         /* Ignored */
         break;
      }
   }

   return result;
}


static const struct {
   VkFormat     format;
} available_surface_formats[] = {
   { VK_FORMAT_B8G8R8A8_UNORM },
   { VK_FORMAT_R8G8B8A8_UNORM },
   { VK_FORMAT_B8G8R8A8_SRGB },
};


static void
get_sorted_vk_formats(struct wsi_device *wsi_device, VkFormat *sorted_formats)
{
   for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++)
      sorted_formats[i] = available_surface_formats[i].format;

   if (wsi_device->force_bgra8_unorm_first) {
      for (unsigned i = 0; i < ARRAY_SIZE(available_surface_formats); i++) {
         if (sorted_formats[i] == VK_FORMAT_B8G8R8A8_UNORM) {
            sorted_formats[i] = sorted_formats[0];
            sorted_formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
            break;
         }
      }
   }
}

static VkResult
wsi_win32_surface_get_formats(VkIcdSurfaceBase *icd_surface,
                              struct wsi_device *wsi_device,
                              uint32_t* pSurfaceFormatCount,
                              VkSurfaceFormatKHR* pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);

   VkFormat sorted_formats[ARRAY_SIZE(available_surface_formats)];
   get_sorted_vk_formats(wsi_device, sorted_formats);

   for (unsigned i = 0; i < ARRAY_SIZE(sorted_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         f->format = sorted_formats[i];
         f->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_win32_surface_get_formats2(VkIcdSurfaceBase *icd_surface,
                               struct wsi_device *wsi_device,
                               const void *info_next,
                               uint32_t* pSurfaceFormatCount,
                               VkSurfaceFormat2KHR* pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);

   VkFormat sorted_formats[ARRAY_SIZE(available_surface_formats)];
   get_sorted_vk_formats(wsi_device, sorted_formats);

   for (unsigned i = 0; i < ARRAY_SIZE(sorted_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         assert(f->sType == VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR);
         f->surfaceFormat.format = sorted_formats[i];
         f->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static const VkPresentModeKHR present_modes_gdi[] = {
   VK_PRESENT_MODE_FIFO_KHR,
};
static const VkPresentModeKHR present_modes_dxgi[] = {
   VK_PRESENT_MODE_IMMEDIATE_KHR,
   VK_PRESENT_MODE_MAILBOX_KHR,
   VK_PRESENT_MODE_FIFO_KHR,
};

static VkResult
wsi_win32_surface_get_present_modes(VkIcdSurfaceBase *surface,
                                    struct wsi_device *wsi_device,
                                    uint32_t* pPresentModeCount,
                                    VkPresentModeKHR* pPresentModes)
{
   const VkPresentModeKHR *array;
   size_t array_size;
   if (wsi_device->sw || !wsi_device->win32.get_d3d12_command_queue) {
      array = present_modes_gdi;
      array_size = ARRAY_SIZE(present_modes_gdi);
   } else {
      array = present_modes_dxgi;
      array_size = ARRAY_SIZE(present_modes_dxgi);
   }

   if (pPresentModes == NULL) {
      *pPresentModeCount = array_size;
      return VK_SUCCESS;
   }

   *pPresentModeCount = MIN2(*pPresentModeCount, array_size);
   typed_memcpy(pPresentModes, array, *pPresentModeCount);

   if (*pPresentModeCount < array_size)
      return VK_INCOMPLETE;
   else
      return VK_SUCCESS;
}

static VkResult
wsi_win32_surface_get_present_rectangles(VkIcdSurfaceBase *surface,
                                      struct wsi_device *wsi_device,
                                      uint32_t* pRectCount,
                                      VkRect2D* pRects)
{
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);

   vk_outarray_append_typed(VkRect2D, &out, rect) {
      /* We don't know a size so just return the usual "I don't know." */
      *rect = {
         { 0, 0 },
         { UINT32_MAX, UINT32_MAX },
      };
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_dxgi_alloc_buffer(const struct wsi_swapchain *chain,
                      struct wsi_image *image,
                      const VkMemoryAllocateInfo *info)
{
   auto win32_image = container_of(image, struct wsi_win32_image, base);
   return chain->wsi->win32.create_buffer_memory(
      chain->device, info, &chain->alloc,
      (void **)&win32_image->dxgi.buffer, &image->blit.memory);
}

static VkResult
wsi_create_dxgi_linear_image_mem(const struct wsi_swapchain *chain,
                                   const struct wsi_image_info *info,
                                   struct wsi_win32_image *image)
{
   const struct wsi_device *wsi = chain->wsi;
   VkMemoryRequirements reqs;
   wsi->GetImageMemoryRequirements(chain->device, image->base.image, &reqs);
   VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
   VkSubresourceLayout layout = {};
   wsi->GetImageSubresourceLayout(chain->device, image->base.image,
                                   &subresource, &layout);
   uint64_t row_bytes = (uint64_t)info->create.extent.width * 4;
   uint64_t height = info->create.extent.height;
   bool valid = reqs.size && reqs.size <= UINT32_MAX - 65535 &&
                layout.offset <= reqs.size &&
                !(layout.offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) &&
                layout.rowPitch && layout.rowPitch <= UINT32_MAX &&
                !(layout.rowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) &&
                layout.rowPitch >= row_bytes && height &&
                height <= (reqs.size - layout.offset) / layout.rowPitch &&
                height <= layout.size / layout.rowPitch;
   if (!valid) {
      fprintf(stderr, "WSI_LINEAR_SHARED_REJECT reason=layout allocation=%llu offset=%llu pitch=%llu subresource_size=%llu width=%u height=%u\n",
              (unsigned long long)reqs.size, (unsigned long long)layout.offset,
              (unsigned long long)layout.rowPitch, (unsigned long long)layout.size,
              info->create.extent.width, info->create.extent.height);
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   uint32_t memory_type = info->select_image_memory_type(wsi, reqs.memoryTypeBits);
   if (memory_type >= wsi->memory_props.memoryTypeCount ||
       !(reqs.memoryTypeBits & (1u << memory_type))) {
      fprintf(stderr, "WSI_LINEAR_SHARED_REJECT reason=memory_type\n");
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   VkMemoryDedicatedAllocateInfo dedicated = {};
   dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
   dedicated.image = image->base.image;
   VkMemoryAllocateInfo allocation = {};
   allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   allocation.pNext = &dedicated;
   allocation.allocationSize = reqs.size;
   allocation.memoryTypeIndex = memory_type;
   VkResult result = wsi->win32.create_buffer_memory(
      chain->device, &allocation, &chain->alloc,
      (void **)&image->dxgi.buffer, &image->base.memory);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "WSI_LINEAR_SHARED_REJECT reason=shared_allocation result=%d\n", result);
      return result;
   }
   D3D12_RESOURCE_DESC buffer_desc = image->dxgi.buffer->GetDesc();
   D3D12_RESOURCE_DESC texture_desc = image->dxgi.swapchain_res->GetDesc();
   DXGI_FORMAT expected_format = info->create.format == VK_FORMAT_R8G8B8A8_UNORM ?
      DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
   if (buffer_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
       buffer_desc.Width < reqs.size || texture_desc.Format != expected_format ||
       texture_desc.Width != info->create.extent.width ||
       texture_desc.Height != height || texture_desc.SampleDesc.Count != 1 ||
       texture_desc.DepthOrArraySize != 1) {
      fprintf(stderr, "WSI_LINEAR_SHARED_REJECT reason=d3d_resource_shape\n");
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   image->base.blit.linear_shared = true;
   image->base.num_planes = 1;
   image->base.sizes[0] = reqs.size;
   image->base.offsets[0] = layout.offset;
   image->base.row_pitches[0] = layout.rowPitch;
   fprintf(stderr, "WSI_LINEAR_SHARED_IMAGE width=%u height=%u format=%u usage=%llu offset=%llu row_pitch=%llu image_size=%llu allocation=%llu d3d_buffer_size=%llu owner=image_memory\n",
           info->create.extent.width, info->create.extent.height,
           (unsigned)info->create.format, (unsigned long long)info->usage2.usage,
           (unsigned long long)layout.offset, (unsigned long long)layout.rowPitch,
           (unsigned long long)layout.size, (unsigned long long)reqs.size,
           (unsigned long long)buffer_desc.Width);
   return VK_SUCCESS;
}

static VkResult
wsi_create_dxgi_image_mem(const struct wsi_swapchain *drv_chain,
                          const struct wsi_image_info *info,
                          struct wsi_image *image)
{
   struct wsi_win32_swapchain *chain = (struct wsi_win32_swapchain *)drv_chain;
   const struct wsi_device *wsi = chain->base.wsi;

   struct wsi_win32_image *win32_image =
      container_of(image, struct wsi_win32_image, base);
   uint32_t image_idx =
      ((uintptr_t)win32_image - (uintptr_t)chain->images) /
      sizeof(*win32_image);
   if (FAILED(chain->dxgi->GetBuffer(image_idx,
                                     IID_PPV_ARGS(&win32_image->dxgi.swapchain_res))))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;


   if (info->win32_native_tiled) {
      VkResult result = wsi->win32.import_native_image(chain->base.device,
         win32_image->dxgi.swapchain_res, image->image, &chain->base.alloc, &image->memory);
      if (result != VK_SUCCESS)
         return result;
      ID3D12Device *device = NULL;
      HRESULT hr = win32_image->dxgi.swapchain_res->GetDevice(IID_PPV_ARGS(&device));
      if (FAILED(hr))
         return VK_ERROR_DEVICE_LOST;
      hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&win32_image->dxgi.allocator));
      if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, win32_image->dxgi.allocator, NULL, IID_PPV_ARGS(&win32_image->dxgi.list));
      if (SUCCEEDED(hr)) hr = win32_image->dxgi.list->Close();
      if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&win32_image->dxgi.acquire_allocator));
      if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, win32_image->dxgi.acquire_allocator, NULL, IID_PPV_ARGS(&win32_image->dxgi.acquire_list));
      if (SUCCEEDED(hr)) hr = win32_image->dxgi.acquire_list->Close();
      if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&win32_image->dxgi.complete));
      D3D12_DESCRIPTOR_HEAP_DESC heap = {};
      heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      heap.NumDescriptors = 1;
      if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&win32_image->dxgi.rtv_heap));
      if (SUCCEEDED(hr)) device->CreateRenderTargetView(win32_image->dxgi.swapchain_res, NULL, win32_image->dxgi.rtv_heap->GetCPUDescriptorHandleForHeapStart());
      if (SUCCEEDED(hr)) result = wsi->win32.create_native_sync(chain->base.device,
         &image->win32_acquire_sync, (void **)&win32_image->dxgi.acquire_fence);
      if (SUCCEEDED(hr) && result == VK_SUCCESS) result = wsi_win32_init_verify(chain, win32_image, device);
      if (SUCCEEDED(hr) && result == VK_SUCCESS && GpuProfile::Enabled()) {
         win32_image->dxgi.profile = new (std::nothrow) NativeGpuProfile();
         auto q = (ID3D12CommandQueue *)wsi->win32.get_d3d12_command_queue(chain->base.device);
         if(win32_image->dxgi.profile && q) win32_image->dxgi.profile->Init(device,q);
      }
      device->Release();
      win32_image->dxgi.event = CreateEventW(NULL, FALSE, FALSE, NULL);
      return FAILED(hr) || !win32_image->dxgi.event ? VK_ERROR_OUT_OF_HOST_MEMORY : result;
   }

   if (chain->base.blit.type == WSI_SWAPCHAIN_BUFFER_BLIT) {
      VkResult result = info->win32_linear_shared ?
         wsi_create_dxgi_linear_image_mem(drv_chain, info, win32_image) :
         wsi_create_buffer_blit_context(drv_chain, info, image, 0);
      if (result != VK_SUCCESS)
         return result;
      ID3D12Device *device = NULL;
      HRESULT hr = win32_image->dxgi.buffer->GetDevice(IID_PPV_ARGS(&device));
      if (FAILED(hr))
         return VK_ERROR_DEVICE_LOST;
      hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(&win32_image->dxgi.allocator));
      if (SUCCEEDED(hr))
         hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        win32_image->dxgi.allocator, NULL,
                                        IID_PPV_ARGS(&win32_image->dxgi.list));
      if (SUCCEEDED(hr))
         hr = win32_image->dxgi.list->Close();
      if (SUCCEEDED(hr))
         hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&win32_image->dxgi.complete));
      if (SUCCEEDED(hr))
         wsi_win32_init_copy_timing(chain, win32_image, device);
      if (SUCCEEDED(hr))
         result = wsi_win32_init_verify(chain, win32_image, device);
      device->Release();
      if (result != VK_SUCCESS)
         return result;
      win32_image->dxgi.event = CreateEventW(NULL, FALSE, FALSE, NULL);
      return SUCCEEDED(hr) && win32_image->dxgi.event ?
         VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   VkResult result =
      wsi->win32.create_image_memory(chain->base.device,
                                     win32_image->dxgi.swapchain_res,
                                     &chain->base.alloc,
                                     chain->base.blit.type == WSI_SWAPCHAIN_NO_BLIT ?
                                     &image->memory : &image->blit.memory);
   if (result != VK_SUCCESS)
      return result;

   if (chain->base.blit.type == WSI_SWAPCHAIN_NO_BLIT)
      return VK_SUCCESS;

   VkImageCreateInfo create = info->create;

   create.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
   create.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

   result = wsi->CreateImage(chain->base.device, &create,
                             &chain->base.alloc, &image->blit.image);
   if (result != VK_SUCCESS)
      return result;

   result = wsi->BindImageMemory(chain->base.device, image->blit.image,
                                 image->blit.memory, 0);
   if (result != VK_SUCCESS)
      return result;

   VkMemoryRequirements reqs;
   wsi->GetImageMemoryRequirements(chain->base.device, image->image, &reqs);

   const VkMemoryDedicatedAllocateInfo memory_dedicated_info = {
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      nullptr,
      image->blit.image,
      VK_NULL_HANDLE,
   };
   const VkMemoryAllocateInfo memory_info = {
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      &memory_dedicated_info,
      reqs.size,
      info->select_image_memory_type(wsi, reqs.memoryTypeBits),
   };

   return wsi->AllocateMemory(chain->base.device, &memory_info,
                              &chain->base.alloc, &image->memory);
}

enum wsi_swapchain_blit_type
wsi_dxgi_image_needs_blit(const struct wsi_device *wsi,
                          const struct wsi_dxgi_image_params *params,
                          VkDevice device)
{
   if (params->native_tiled)
      return WSI_SWAPCHAIN_NO_BLIT;
   if (wsi->win32.create_buffer_memory)
      return WSI_SWAPCHAIN_BUFFER_BLIT;
   if (wsi->win32.requires_blits && wsi->win32.requires_blits(device))
      return WSI_SWAPCHAIN_IMAGE_BLIT;
   else if (params->storage_image)
      return WSI_SWAPCHAIN_IMAGE_BLIT;
   return WSI_SWAPCHAIN_NO_BLIT;
}

VkResult
wsi_dxgi_configure_image(const struct wsi_swapchain *chain,
                         const VkSwapchainCreateInfoKHR *pCreateInfo,
                         const struct wsi_dxgi_image_params *params,
                         struct wsi_image_info *info)
{
   VkResult result =
      wsi_configure_image(chain, pCreateInfo, 0, info);
   if (result != VK_SUCCESS)
      return result;

   info->create_mem = wsi_create_dxgi_image_mem;
   info->win32_native_tiled = params->native_tiled;
   if (params->native_tiled) {
      fprintf(stderr, "WSI_NATIVE_MODE requested=1 active=1 format=%u width=%u height=%u\n",
         (unsigned)pCreateInfo->imageFormat, pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
      return VK_SUCCESS;
   }

   if (chain->blit.type != WSI_SWAPCHAIN_NO_BLIT) {
      if (chain->blit.type == WSI_SWAPCHAIN_BUFFER_BLIT) {
         wsi_configure_buffer_image(chain, pCreateInfo, 256, 65536, info);
         info->alloc_blit_memory = wsi_dxgi_alloc_buffer;
      } else {
         wsi_configure_image_blit_image(chain, info);
      }
      info->select_image_memory_type = wsi_select_device_memory_type;
      info->select_blit_dst_memory_type = wsi_select_device_memory_type;
   }

   const bool linear_shared = wsi_win32_option_enabled("TU_WSI_LINEAR_SHARED") &&
      !wsi_win32_option_enabled("TU_WSI_NATIVE_TILED");
   const bool verify = wsi_win32_option_enabled("TU_WSI_VERIFY");
   if (verify && wsi_win32_option_enabled("TU_WSI_COPY_TIMING")) {
      fprintf(stderr, "WSI_PROTOTYPE_REJECT reason=verify_and_timing_are_exclusive\n");
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   bool configuration_supported =
      chain->blit.type == WSI_SWAPCHAIN_BUFFER_BLIT &&
      chain->wsi->win32.create_buffer_memory && !pCreateInfo->flags &&
      pCreateInfo->imageArrayLayers == 1 && pCreateInfo->minImageCount <= 8 &&
      pCreateInfo->imageSharingMode == VK_SHARING_MODE_EXCLUSIVE &&
      (pCreateInfo->imageFormat == VK_FORMAT_R8G8B8A8_UNORM ||
       pCreateInfo->imageFormat == VK_FORMAT_B8G8R8A8_UNORM) &&
      !info->img_compr_ctrl.sType;
   if (verify && !configuration_supported)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   if (!linear_shared) {
      fprintf(stderr, "WSI_LINEAR_SHARED_MODE requested=0 active=0 reason=baseline\n");
      return VK_SUCCESS;
   }
   if (!configuration_supported) {
      fprintf(stderr, "WSI_LINEAR_SHARED_MODE requested=1 active=0 reason=unsupported_configuration format=%u\n",
              (unsigned)pCreateInfo->imageFormat);
      return VK_SUCCESS;
   }
   if (info->usage2.usage > UINT32_MAX) {
      fprintf(stderr, "WSI_LINEAR_SHARED_MODE requested=1 active=0 reason=usage_width\n");
      return VK_SUCCESS;
   }
   VkPhysicalDeviceImageFormatInfo2 query = {};
   query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
   query.format = info->create.format;
   query.type = info->create.imageType;
   query.tiling = VK_IMAGE_TILING_LINEAR;
   query.usage = info->usage2.usage;
   query.flags = info->create.flags;
   VkImageFormatProperties2 properties = {};
   properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
   result = chain->wsi->GetPhysicalDeviceImageFormatProperties2(
      chain->wsi->pdevice, &query, &properties);
   if (result != VK_SUCCESS && result != VK_ERROR_FORMAT_NOT_SUPPORTED)
      return result;
   const VkImageFormatProperties *limits = &properties.imageFormatProperties;
   if (result != VK_SUCCESS ||
       info->create.extent.width > limits->maxExtent.width ||
       info->create.extent.height > limits->maxExtent.height ||
       !limits->maxMipLevels || !limits->maxArrayLayers ||
       !(limits->sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
      fprintf(stderr, "WSI_LINEAR_SHARED_MODE requested=1 active=0 reason=linear_usage_support result=%d\n", result);
      return VK_SUCCESS;
   }
   VkImageCreateInfo probe_info = info->create;
   probe_info.tiling = VK_IMAGE_TILING_LINEAR;
   VkImage probe = VK_NULL_HANDLE;
   result = chain->wsi->CreateImage(chain->device, &probe_info, &chain->alloc, &probe);
   if (result == VK_ERROR_FORMAT_NOT_SUPPORTED || result == VK_ERROR_FEATURE_NOT_PRESENT) {
      fprintf(stderr, "WSI_LINEAR_SHARED_MODE requested=1 active=0 reason=linear_image_create result=%d\n", result);
      return VK_SUCCESS;
   }
   if (result != VK_SUCCESS)
      return result;
   VkMemoryRequirements reqs = {};
   chain->wsi->GetImageMemoryRequirements(chain->device, probe, &reqs);
   VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
   VkSubresourceLayout layout = {};
   chain->wsi->GetImageSubresourceLayout(chain->device, probe, &subresource, &layout);
   chain->wsi->DestroyImage(chain->device, probe, &chain->alloc);
   uint64_t row_bytes = (uint64_t)info->create.extent.width * 4;
   uint64_t height = info->create.extent.height;
   bool layout_supported = reqs.size && reqs.size <= UINT32_MAX - 65535 &&
      reqs.memoryTypeBits && layout.offset <= reqs.size &&
      !(layout.offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) &&
      layout.rowPitch && layout.rowPitch <= UINT32_MAX &&
      !(layout.rowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) &&
      layout.rowPitch >= row_bytes && height &&
      height <= (reqs.size - layout.offset) / layout.rowPitch &&
      height <= layout.size / layout.rowPitch;
   if (!layout_supported) {
      fprintf(stderr, "WSI_LINEAR_SHARED_MODE requested=1 active=0 reason=linear_layout\n");
      return VK_SUCCESS;
   }
   info->create.tiling = VK_IMAGE_TILING_LINEAR;
   info->win32_linear_shared = true;
   fprintf(stderr, "WSI_LINEAR_SHARED_MODE requested=1 active=1 reason=preflight_pass width=%u height=%u format=%u\n",
           info->create.extent.width, info->create.extent.height, (unsigned)info->create.format);

   return VK_SUCCESS;
}

static void wsi_win32_finish_dxgi_image(struct wsi_win32_image *image);

static VkResult
wsi_win32_image_init(VkDevice device_h,
                     struct wsi_win32_swapchain *chain,
                     const VkSwapchainCreateInfoKHR *create_info,
                     const VkAllocationCallbacks *allocator,
                     struct wsi_win32_image *image)
{
   image->chain = chain;
   VkResult result = wsi_create_image(&chain->base, &chain->base.image_info,
                                      &image->base);
   if (result != VK_SUCCESS) {
      wsi_win32_finish_dxgi_image(image);
      return result;
   }

   VkIcdSurfaceWin32 *win32_surface = (VkIcdSurfaceWin32 *)create_info->surface;
   chain->wnd = win32_surface->hwnd;
   image->chain = chain;

   if (chain->dxgi)
      return VK_SUCCESS;

   chain->chain_dc = GetDC(chain->wnd);
   image->sw.dc = CreateCompatibleDC(chain->chain_dc);
   HBITMAP bmp = NULL;

   BITMAPINFO info = { 0 };
   info.bmiHeader.biSize = sizeof(BITMAPINFO);
   info.bmiHeader.biWidth = create_info->imageExtent.width;
   info.bmiHeader.biHeight = -create_info->imageExtent.height;
   info.bmiHeader.biPlanes = 1;
   info.bmiHeader.biBitCount = 32;
   info.bmiHeader.biCompression = BI_RGB;

   bmp = CreateDIBSection(image->sw.dc, &info, DIB_RGB_COLORS, &image->sw.ppvBits, NULL, 0);
   assert(bmp && image->sw.ppvBits);

   SelectObject(image->sw.dc, bmp);

   BITMAP header;
   int status = GetObject(bmp, sizeof(BITMAP), &header);
   (void)status;
   image->sw.bmp_row_pitch = header.bmWidthBytes;
   image->sw.bmp = bmp;

   return VK_SUCCESS;
}

static VkResult
wsi_win32_wait_copy(struct wsi_win32_image *image, uint64_t timeout)
{
   if (!image->dxgi.complete)
      return VK_SUCCESS;
   uint64_t value = image->dxgi.complete->GetCompletedValue();
   if (value == UINT64_MAX)
      return VK_ERROR_DEVICE_LOST;
   if (value >= image->dxgi.serial)
      return VK_SUCCESS;
   if (!timeout)
      return VK_NOT_READY;
   ResetEvent(image->dxgi.event);
   if (FAILED(image->dxgi.complete->SetEventOnCompletion(image->dxgi.serial,
                                                         image->dxgi.event)))
      return VK_ERROR_DEVICE_LOST;
   DWORD ms = timeout == UINT64_MAX ? INFINITE :
      (DWORD)MIN2(timeout / 1000000 + (timeout % 1000000 != 0), INFINITE - 1ULL);
   DWORD result = WaitForSingleObject(image->dxgi.event, ms);
   if (result == WAIT_TIMEOUT)
      return VK_TIMEOUT;
   if (result != WAIT_OBJECT_0 || image->dxgi.complete->GetCompletedValue() == UINT64_MAX)
      return VK_ERROR_DEVICE_LOST;
   return VK_SUCCESS;
}

static void
wsi_win32_finish_dxgi_image(struct wsi_win32_image *image)
{
   if (wsi_win32_wait_copy(image, UINT64_MAX) == VK_SUCCESS)
      wsi_win32_collect_copy_timing(image);
   if(image->dxgi.profile) image->dxgi.profile->Collect(image);
   wsi_win32_report_verify(image);
   if (image->dxgi.acquire_fence && image->dxgi.event &&
       image->dxgi.acquire_fence->GetCompletedValue() < image->base.win32_acquire_value) {
      ResetEvent(image->dxgi.event);
      if (SUCCEEDED(image->dxgi.acquire_fence->SetEventOnCompletion(image->base.win32_acquire_value, image->dxgi.event)))
         WaitForSingleObject(image->dxgi.event, INFINITE);
   }
   if (image->base.win32_acquire_sync) {
      image->chain->wsi->wsi->win32.destroy_native_sync(image->chain->base.device, image->base.win32_acquire_sync);
      image->base.win32_acquire_sync = NULL;
   }
   if(image->dxgi.profile && image->dxgi.profile->enabled) image->dxgi.profile->Calibrate(image->chain,true);
   delete image->dxgi.profile;
   if (image->dxgi.acquire_fence) image->dxgi.acquire_fence->Release();
   if (image->dxgi.acquire_list) image->dxgi.acquire_list->Release();
   if (image->dxgi.acquire_allocator) image->dxgi.acquire_allocator->Release();
   if (image->dxgi.rtv_heap) image->dxgi.rtv_heap->Release();
   if (image->dxgi.verify_readback)
      image->dxgi.verify_readback->Release();
   if (image->dxgi.copy_query_readback)
      image->dxgi.copy_query_readback->Release();
   if (image->dxgi.copy_query_heap)
      image->dxgi.copy_query_heap->Release();
   if (image->dxgi.list)
      image->dxgi.list->Release();
   if (image->dxgi.allocator)
      image->dxgi.allocator->Release();
   if (image->dxgi.complete)
      image->dxgi.complete->Release();
   if (image->dxgi.event)
      CloseHandle(image->dxgi.event);
   if (image->dxgi.buffer)
      image->dxgi.buffer->Release();
   if (image->dxgi.swapchain_res)
      image->dxgi.swapchain_res->Release();
   memset(&image->dxgi, 0, sizeof(image->dxgi));
}

static void
wsi_win32_image_finish(struct wsi_win32_swapchain *chain,
                       const VkAllocationCallbacks *allocator,
                       struct wsi_win32_image *image)
{
   wsi_win32_finish_dxgi_image(image);
   if (image->sw.dc)
      DeleteDC(image->sw.dc);
   if(image->sw.bmp)
      DeleteObject(image->sw.bmp);
   wsi_destroy_image(&chain->base, &image->base);
}

static VkResult
wsi_win32_swapchain_destroy(struct wsi_swapchain *drv_chain,
                            const VkAllocationCallbacks *allocator)
{
   struct wsi_win32_swapchain *chain =
      (struct wsi_win32_swapchain *) drv_chain;

   for (uint32_t i = 0; i < chain->base.image_count; i++)
      wsi_win32_image_finish(chain, allocator, &chain->images[i]);

   wsi_win32_report_copy_timing(chain);

   DeleteDC(chain->chain_dc);

   if (chain->surface->current_swapchain == chain) {
      chain->surface->visual->SetContent(NULL);
      chain->wsi->dxgi.dcomp->Commit();
      chain->surface->current_swapchain = NULL;
   }

   wsi_native_report_waits(chain, true);
   if (chain->native_latency)
      CloseHandle(chain->native_latency);
   if (chain->dxgi)
      chain->dxgi->Release();
   while (chain->present_head) {
      auto record = chain->present_head;
      chain->present_head = record->next;
      record->copy_fence->Release();
      vk_free(&chain->base.alloc, record);
   }

   wsi_swapchain_finish(&chain->base);

   u_cnd_monotonic_destroy(&chain->acquire_cond);
   mtx_destroy(&chain->acquire_mutex);

   vk_free(allocator, chain);
   return VK_SUCCESS;
}

static struct wsi_image *
wsi_win32_get_wsi_image(struct wsi_swapchain *drv_chain,
                        uint32_t image_index)
{
   struct wsi_win32_swapchain *chain =
      (struct wsi_win32_swapchain *) drv_chain;

   return &chain->images[image_index].base;
}

static void
wsi_win32_set_image_idle(struct wsi_win32_swapchain *chain,
                         struct wsi_win32_image *image)
{
   if (!chain->dxgi)
      mtx_lock(&chain->acquire_mutex);

   image->state = WSI_IMAGE_IDLE;
   if (chain->base.image_info.win32_native_tiled && image->dxgi.native_prepared)
      chain->native_latency_ready = true;

   if (!chain->dxgi) {
      u_cnd_monotonic_broadcast(&chain->acquire_cond);
      mtx_unlock(&chain->acquire_mutex);
   }
}

static VkResult
wsi_win32_release_images(struct wsi_swapchain *drv_chain,
                         uint32_t count, const uint32_t *indices)
{
   struct wsi_win32_swapchain *chain =
      (struct wsi_win32_swapchain *)drv_chain;

   if (p_atomic_read(&chain->status) == VK_ERROR_SURFACE_LOST_KHR)
      return VK_ERROR_SURFACE_LOST_KHR;

   for (uint32_t i = 0; i < count; i++) {
      uint32_t index = indices[i];
      assert(index < chain->base.image_count);
      assert(chain->images[index].state == WSI_IMAGE_DRAWING);
      wsi_win32_set_image_idle(chain, &chain->images[index]);
   }

   return VK_SUCCESS;
}

static bool
wsi_win32_find_idle_image(struct wsi_win32_swapchain *chain,
                          uint32_t *out_image_index)
{
   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].state == WSI_IMAGE_IDLE) {
         *out_image_index = i;
         chain->images[i].state = WSI_IMAGE_DRAWING;
         return true;
      }
   }
   return false;
}

static VkResult
wsi_win32_acquire_idle_cpu_image_locked(struct wsi_win32_swapchain *chain,
                                        const VkAcquireNextImageInfoKHR *info,
                                        uint32_t *out_image_index)
{
   if (wsi_win32_find_idle_image(chain, out_image_index))
      return VK_SUCCESS;

   if (info->timeout == 0)
      return VK_NOT_READY;

   const uint64_t abs_timeout = os_time_get_absolute_timeout(info->timeout);
   struct timespec abs_timespec;
   timespec_from_nsec(&abs_timespec, abs_timeout);
   do {
      int ret = u_cnd_monotonic_timedwait(
         &chain->acquire_cond, &chain->acquire_mutex, &abs_timespec);
      if (ret == thrd_timedout)
         return VK_TIMEOUT;
      else if (ret != thrd_success)
         return VK_ERROR_OUT_OF_DATE_KHR;
   } while (!wsi_win32_find_idle_image(chain, out_image_index));

   return VK_SUCCESS;
}

static inline VkResult
wsi_win32_acquire_idle_cpu_image(struct wsi_win32_swapchain *chain,
                                 const VkAcquireNextImageInfoKHR *info,
                                 uint32_t *out_image_index)
{
   mtx_lock(&chain->acquire_mutex);
   VkResult result = wsi_win32_acquire_idle_cpu_image_locked(chain, info,
                                                             out_image_index);
   mtx_unlock(&chain->acquire_mutex);
   return result;
}

static VkResult
wsi_native_acquire(struct wsi_win32_swapchain *chain,
                   const VkAcquireNextImageInfoKHR *info, uint32_t *image_index)
{
   uint64_t deadline = os_time_get_absolute_timeout(info->timeout);
   if (chain->native_wait_stats) {
      if (!chain->native_stats_start)
         chain->native_stats_start = os_time_get_nano();
      chain->native_acquires++;
   }
   uint32_t index = chain->dxgi->GetCurrentBackBufferIndex();
   if (index >= chain->base.image_count)
      return VK_ERROR_OUT_OF_DATE_KHR;
   auto image = &chain->images[index];
   if(image->dxgi.profile) image->dxgi.profile->Event(image,"native_acquire_begin");
   if (image->state == WSI_IMAGE_DRAWING)
      return info->timeout ? VK_TIMEOUT : VK_NOT_READY;
   if (!chain->native_latency_ready) {
      DWORD ms = info->timeout == UINT64_MAX ? INFINITE :
         (DWORD)MIN2(info->timeout / 1000000, INFINITE - 1ULL);
      uint64_t begin = chain->native_wait_stats ? os_time_get_nano() : 0;
      if(image->dxgi.profile) image->dxgi.profile->Event(image,"admission_begin");
      DWORD r = WaitForSingleObject(chain->native_latency, ms);
      if(image->dxgi.profile) image->dxgi.profile->Event(image,"admission_end",r);
      if (chain->native_wait_stats) {
         uint64_t delta = os_time_get_nano() - begin;
         chain->native_latency_ns += delta;
         chain->native_latency_max = MAX2(chain->native_latency_max, delta);
      }
      if (r == WAIT_TIMEOUT)
         return info->timeout ? VK_TIMEOUT : VK_NOT_READY;
      if (r != WAIT_OBJECT_0)
         return VK_ERROR_DEVICE_LOST;
      chain->native_latency_ready = true;
   }
   uint64_t now = os_time_get_nano();
   uint64_t remaining = deadline == UINT64_MAX ? UINT64_MAX :
      deadline > now ? deadline - now : 0;
   uint64_t begin = chain->native_wait_stats ? os_time_get_nano() : 0;
   if(image->dxgi.profile) image->dxgi.profile->Event(image,"reuse_begin");
   VkResult result = wsi_win32_wait_copy(image, remaining);
   if(image->dxgi.profile) image->dxgi.profile->Event(image,"reuse_end",uint64_t(int64_t(result)));
   if (chain->native_wait_stats) {
      uint64_t delta = os_time_get_nano() - begin;
      chain->native_reuse_ns += delta;
      chain->native_reuse_max = MAX2(chain->native_reuse_max, delta);
   }
   if (result != VK_SUCCESS)
      return result == VK_NOT_READY && info->timeout ? VK_TIMEOUT : result;
   wsi_win32_collect_copy_timing(image);
   if(image->dxgi.profile) image->dxgi.profile->Collect(image);
   if (!image->dxgi.native_prepared) {
      auto queue = (ID3D12CommandQueue *)chain->base.wsi->win32.get_d3d12_command_queue(chain->base.device);
      if (!queue || FAILED(image->dxgi.acquire_allocator->Reset()) ||
          FAILED(image->dxgi.acquire_list->Reset(image->dxgi.acquire_allocator, NULL)))
         return VK_ERROR_DEVICE_LOST;
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = image->dxgi.swapchain_res;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      if(image->dxgi.profile) image->dxgi.profile->Mark(image->dxgi.acquire_list,0);
      image->dxgi.acquire_list->ResourceBarrier(1, &barrier);
      if (!image->dxgi.native_initialized) {
         const float clear[4] = {0, 0, 0, 1};
         image->dxgi.acquire_list->ClearRenderTargetView(
            image->dxgi.rtv_heap->GetCPUDescriptorHandleForHeapStart(), clear, 0, NULL);
      }
      if(image->dxgi.profile) {
         image->dxgi.profile->Mark(image->dxgi.acquire_list,1);
         image->dxgi.profile->Resolve(image->dxgi.acquire_list,0);
         image->dxgi.profile->acquire_cpu=GpuProfile::Qpc();
      }
      if (FAILED(image->dxgi.acquire_list->Close()))
         return VK_ERROR_DEVICE_LOST;
      ID3D12CommandList *list = image->dxgi.acquire_list;
      queue->ExecuteCommandLists(1, &list);
      uint64_t value = image->base.win32_acquire_value + 1;
      if (!value || FAILED(queue->Signal(image->dxgi.acquire_fence, value)))
         return VK_ERROR_DEVICE_LOST;
      image->base.win32_acquire_value = value;
      image->dxgi.native_prepared = true;
      image->dxgi.native_initialized = true;
   }
   chain->native_latency_ready = false;
   image->state = WSI_IMAGE_DRAWING;
   *image_index = index;
   if(image->dxgi.profile) image->dxgi.profile->Event(image,"native_acquire_end",image->base.win32_acquire_value);
   if (wsi_win32_option_enabled("TU_WDDM_WSI_TRACE"))
      fprintf(stderr, "WSI_NATIVE_ACQUIRE image=%u value=%llu completed=%llu\n", index,
         (unsigned long long)image->base.win32_acquire_value,
         (unsigned long long)image->dxgi.acquire_fence->GetCompletedValue());
   return VK_SUCCESS;
}

static VkResult
wsi_native_present(struct wsi_win32_swapchain *chain, struct wsi_win32_image *image)
{
   const auto wsi = chain->base.wsi;
   uint32_t index = image - chain->images;
   if (chain->dxgi->GetCurrentBackBufferIndex() != index || !image->dxgi.native_prepared)
      return VK_ERROR_OUT_OF_DATE_KHR;
   auto queue = (ID3D12CommandQueue *)wsi->win32.get_d3d12_command_queue(chain->base.device);
   uint64_t value = 0;
   auto ready = (ID3D12Fence *)wsi->win32.get_d3d12_fence(chain->base.device, chain->base.fences[index], &value);
   if (!queue || !ready || !value || FAILED(image->dxgi.allocator->Reset()) ||
       FAILED(image->dxgi.list->Reset(image->dxgi.allocator, NULL)))
      return VK_ERROR_DEVICE_LOST;
   bool verify = image->dxgi.verify_readback && image->dxgi.verify_submitted < WSI_WIN32_VERIFY_MAX_SAMPLES;
   D3D12_RESOURCE_BARRIER barrier = {};
   barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   barrier.Transition.pResource = image->dxgi.swapchain_res;
   barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
   barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
   barrier.Transition.StateAfter = verify ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_PRESENT;
   if(image->dxgi.profile) image->dxgi.profile->Mark(image->dxgi.list,2);
   image->dxgi.list->ResourceBarrier(1, &barrier);
   if (verify) {
      D3D12_TEXTURE_COPY_LOCATION src = {};
      src.pResource = image->dxgi.swapchain_res;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_TEXTURE_COPY_LOCATION dst = {};
      dst.pResource = image->dxgi.verify_readback;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = image->dxgi.verify_footprint;
      image->dxgi.list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
      image->dxgi.list->ResourceBarrier(1, &barrier);
   }
   if(image->dxgi.profile) {
      image->dxgi.profile->Mark(image->dxgi.list,3);
      image->dxgi.profile->Resolve(image->dxgi.list,2);
      image->dxgi.profile->present_cpu=GpuProfile::Qpc();
   }
   if (FAILED(image->dxgi.list->Close())) return VK_ERROR_DEVICE_LOST;
   if(image->dxgi.profile) image->dxgi.profile->Event(image,"queue_wait_api_begin",value,ready->GetCompletedValue());
   HRESULT wait_hr=queue->Wait(ready,value);
   if(image->dxgi.profile) image->dxgi.profile->Event(image,"queue_wait_api_end",value,(uint64_t)wait_hr);
   if(FAILED(wait_hr)) return VK_ERROR_DEVICE_LOST;
   ID3D12CommandList *list = image->dxgi.list;
   queue->ExecuteCommandLists(1, &list);
   uint64_t serial = image->dxgi.serial + 1;
   if (!serial || FAILED(queue->Signal(image->dxgi.complete, serial)))
      return VK_ERROR_DEVICE_LOST;
   image->dxgi.serial = serial;
   if(image->dxgi.profile) image->dxgi.profile->Event(image,"native_present_submitted",value,serial);
   image->dxgi.native_prepared = false;
   if (verify) {
      image->dxgi.verify_submitted++;
      image->dxgi.verify_pending_serial = serial;
   }
   if (wsi_win32_option_enabled("TU_WDDM_WSI_TRACE"))
      fprintf(stderr, "WSI_NATIVE_PRESENT image=%u serial=%llu render_value=%llu output_copies=0\n",
         index, (unsigned long long)serial, (unsigned long long)value);
   return VK_SUCCESS;
}

static VkResult
wsi_win32_acquire_next_image(struct wsi_swapchain *drv_chain,
                             const VkAcquireNextImageInfoKHR *info,
                             uint32_t *image_index)
{
   struct wsi_win32_swapchain *chain =
      (struct wsi_win32_swapchain *)drv_chain;

   /* Bail early if the swapchain is broken */
   VkResult status = p_atomic_read(&chain->status);
   if (status != VK_SUCCESS)
      return status;

   /* acquire timeout has to be explicitly handled for sw wsi */
   if (!chain->dxgi)
      return wsi_win32_acquire_idle_cpu_image(chain, info, image_index);

   if (chain->base.image_info.win32_native_tiled) {
      mtx_lock(&chain->acquire_mutex);
      VkResult result = wsi_native_acquire(chain, info, image_index);
      if (result < 0) p_atomic_set(&chain->status, result);
      mtx_unlock(&chain->acquire_mutex);
      return result;
   }

   if (chain->base.blit.type == WSI_SWAPCHAIN_BUFFER_BLIT) {
      uint32_t selected = UINT32_MAX;
      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         if (chain->images[i].state == WSI_IMAGE_DRAWING)
            continue;
         VkResult result = wsi_win32_wait_copy(&chain->images[i], 0);
         if (result == VK_SUCCESS) {
            wsi_win32_collect_copy_timing(&chain->images[i]);
            chain->images[i].state = WSI_IMAGE_DRAWING;
            *image_index = i;
            return VK_SUCCESS;
         }
         if (result < 0)
            return result;
         if (selected == UINT32_MAX)
            selected = i;
      }
      if (selected == UINT32_MAX)
         return info->timeout ? VK_TIMEOUT : VK_NOT_READY;
      VkResult result = wsi_win32_wait_copy(&chain->images[selected], info->timeout);
      if (result != VK_SUCCESS)
         return result;
      wsi_win32_collect_copy_timing(&chain->images[selected]);
      chain->images[selected].state = WSI_IMAGE_DRAWING;
      *image_index = selected;
      return VK_SUCCESS;
   }

   if (wsi_win32_find_idle_image(chain, image_index))
      return VK_SUCCESS;

   assert(chain->dxgi);
   uint32_t index = chain->dxgi->GetCurrentBackBufferIndex();
   if (chain->images[index].state == WSI_IMAGE_DRAWING) {
      index = (index + 1) % chain->base.image_count;
      assert(chain->images[index].state == WSI_IMAGE_QUEUED);
   }
   if (chain->wsi->wsi->WaitForFences(chain->base.device, 1,
                                      &chain->base.fences[index],
                                      false, info->timeout) != VK_SUCCESS)
      return VK_TIMEOUT;

   *image_index = index;
   chain->images[index].state = WSI_IMAGE_DRAWING;
   return VK_SUCCESS;
}

static uint64_t
wsi_win32_refresh(struct wsi_swapchain *base, uint64_t *interval)
{
   auto chain = (struct wsi_win32_swapchain *)base;
   DCOMPOSITION_FRAME_STATISTICS stats = {};
   *interval = 0;
   if (FAILED(chain->wsi->dxgi.dcomp->GetFrameStatistics(&stats)) ||
       !stats.currentCompositionRate.Numerator)
      return 0;
   *interval = (1000000000ULL * stats.currentCompositionRate.Denominator) /
               stats.currentCompositionRate.Numerator;
   return *interval;
}

static uint64_t
wsi_win32_qpc_to_ns(uint64_t ticks)
{
   LARGE_INTEGER frequency;
   QueryPerformanceFrequency(&frequency);
   return (ticks / frequency.QuadPart) * 1000000000ULL +
          (ticks % frequency.QuadPart) * 1000000000ULL / frequency.QuadPart;
}

static VkResult
wsi_win32_poll_present_locked(struct wsi_win32_swapchain *chain)
{
   if (!chain->present_head)
      return VK_SUCCESS;
   DXGI_FRAME_STATISTICS stats = {};
   HRESULT hr = chain->dxgi->GetFrameStatistics(&stats);
   if (FAILED(hr) && hr != DXGI_ERROR_FRAME_STATISTICS_DISJOINT)
      return hr == DXGI_ERROR_DEVICE_REMOVED ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_DATE_KHR;
   while (chain->present_head) {
      auto record = chain->present_head;
      record->discarded |= !IsWindowVisible(chain->wnd) || IsIconic(chain->wnd);
      uint64_t copied = record->copy_fence->GetCompletedValue();
      if (copied == UINT64_MAX)
         return VK_ERROR_DEVICE_LOST;
      bool displayed = SUCCEEDED(hr) && (int32_t)(stats.PresentCount - record->count) >= 0;
      if (!(displayed || record->discarded) || copied < record->copy_value)
         break;
      if (record->timing_serial && chain->base.present_timing.active) {
         uint64_t interval;
         uint64_t refresh = wsi_win32_refresh(&chain->base, &interval);
         if (refresh)
            wsi_swapchain_present_timing_update_refresh_rate(&chain->base, refresh, interval, 100000);
         uint64_t timestamp = 0;
         if (displayed && stats.PresentCount == record->count && stats.SyncQPCTime.QuadPart) {
            timestamp = wsi_win32_qpc_to_ns(stats.SyncQPCTime.QuadPart);
            uint64_t elapsed = (uint32_t)(stats.SyncRefreshCount - stats.PresentRefreshCount) * refresh;
            timestamp = timestamp >= elapsed ? timestamp - elapsed : 0;
         }
         wsi_swapchain_present_timing_notify_completion(&chain->base, record->timing_serial,
                                                         timestamp, record->image);
      }
      chain->completed_id = MAX2(chain->completed_id, record->id);
      chain->present_head = record->next;
      if (!chain->present_head)
         chain->present_tail = NULL;
      record->copy_fence->Release();
      vk_free(&chain->base.alloc, record);
   }
   return VK_SUCCESS;
}

static void
wsi_win32_set_timing(struct wsi_swapchain *base, const struct wsi_image_timing_request *request)
{
   auto chain = (struct wsi_win32_swapchain *)base;
   chain->timing_request = *request;
}

static void
wsi_win32_poll_timing(struct wsi_swapchain *base)
{
   auto chain = (struct wsi_win32_swapchain *)base;
   mtx_lock(&chain->acquire_mutex);
   VkResult result = wsi_win32_poll_present_locked(chain);
   if (result != VK_SUCCESS)
      p_atomic_set(&chain->status, result);
   mtx_unlock(&chain->acquire_mutex);
}

static VkResult
wsi_win32_wait_for_present(struct wsi_swapchain *base, uint64_t id, uint64_t timeout)
{
   auto chain = (struct wsi_win32_swapchain *)base;
   uint64_t deadline = os_time_get_absolute_timeout(timeout);
   for (;;) {
      mtx_lock(&chain->acquire_mutex);
      VkResult result = p_atomic_read(&chain->status);
      if (result == VK_SUCCESS)
         result = wsi_win32_poll_present_locked(chain);
      bool complete = chain->completed_id >= id;
      mtx_unlock(&chain->acquire_mutex);
      if (result != VK_SUCCESS)
         return result;
      if (complete)
         return VK_SUCCESS;
      if ((uint64_t)os_time_get_nano() >= deadline)
         return VK_TIMEOUT;
      Sleep(1);
   }
}

static VkResult
wsi_win32_copy_to_dxgi(struct wsi_win32_swapchain *chain,
                       struct wsi_win32_image *image)
{
   const struct wsi_device *wsi = chain->base.wsi;
   auto queue = (ID3D12CommandQueue *)wsi->win32.get_d3d12_command_queue(chain->base.device);
   uint32_t index = image - chain->images;
   uint64_t ready_value;
   auto ready = (ID3D12Fence *)wsi->win32.get_d3d12_fence(
      chain->base.device, chain->base.fences[index], &ready_value);
   if (!queue || !ready)
      return VK_ERROR_DEVICE_LOST;
   if (FAILED(image->dxgi.allocator->Reset()) ||
       FAILED(image->dxgi.list->Reset(image->dxgi.allocator, NULL)))
      return VK_ERROR_DEVICE_LOST;
   auto destination = chain->images[chain->dxgi->GetCurrentBackBufferIndex()].dxgi.swapchain_res;
   D3D12_RESOURCE_BARRIER barrier = {};
   barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   barrier.Transition.pResource = destination;
   barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
   barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
   barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
   image->dxgi.list->ResourceBarrier(1, &barrier);
   D3D12_TEXTURE_COPY_LOCATION dst = {};
   dst.pResource = destination;
   dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   D3D12_TEXTURE_COPY_LOCATION src = {};
   src.pResource = image->dxgi.buffer;
   src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
   src.PlacedFootprint.Offset = image->base.offsets[0];
   src.PlacedFootprint.Footprint.Format = destination->GetDesc().Format;
   src.PlacedFootprint.Footprint.Width = chain->extent.width;
   src.PlacedFootprint.Footprint.Height = chain->extent.height;
   src.PlacedFootprint.Footprint.Depth = 1;
   src.PlacedFootprint.Footprint.RowPitch = image->base.row_pitches[0];
   if (image->dxgi.copy_query_heap)
      image->dxgi.list->EndQuery(image->dxgi.copy_query_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
   image->dxgi.list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
   if (image->dxgi.copy_query_heap) {
      image->dxgi.list->EndQuery(image->dxgi.copy_query_heap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
      image->dxgi.list->ResolveQueryData(image->dxgi.copy_query_heap,
                                          D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                          image->dxgi.copy_query_readback, 0);
   }
   bool verify = image->dxgi.verify_readback &&
                 image->dxgi.verify_submitted < WSI_WIN32_VERIFY_MAX_SAMPLES;
   if (verify) {
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      image->dxgi.list->ResourceBarrier(1, &barrier);
      D3D12_TEXTURE_COPY_LOCATION readback = {};
      readback.pResource = image->dxgi.verify_readback;
      readback.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      readback.PlacedFootprint = image->dxgi.verify_footprint;
      image->dxgi.list->CopyTextureRegion(&readback, 0, 0, 0, &dst, NULL);
   }
   barrier.Transition.StateBefore = verify ? D3D12_RESOURCE_STATE_COPY_SOURCE :
                                            D3D12_RESOURCE_STATE_COPY_DEST;
   barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
   image->dxgi.list->ResourceBarrier(1, &barrier);
   if (FAILED(image->dxgi.list->Close()) || FAILED(queue->Wait(ready, ready_value)))
      return VK_ERROR_DEVICE_LOST;
   ID3D12CommandList *list = image->dxgi.list;
   queue->ExecuteCommandLists(1, &list);
   uint64_t serial = image->dxgi.serial + 1;
   if (FAILED(queue->Signal(image->dxgi.complete, serial)))
      return VK_ERROR_DEVICE_LOST;
   image->dxgi.serial = serial;
   if (verify) {
      image->dxgi.verify_submitted++;
      image->dxgi.verify_pending_serial = serial;
   }
   return VK_SUCCESS;
}

static VkResult
wsi_win32_queue_present_dxgi(struct wsi_win32_swapchain *chain,
                             struct wsi_win32_image *image,
                             uint64_t present_id,
                             const VkPresentRegionKHR *damage)
{
   if (chain->base.image_info.win32_native_tiled) {
      VkResult result = wsi_native_present(chain, image);
      if (result != VK_SUCCESS)
         return result;
   }
   if (image->dxgi.buffer) {
      VkResult result = wsi_win32_copy_to_dxgi(chain, image);
      if (result != VK_SUCCESS)
         return result;
   }
   uint32_t rect_count = damage ? damage->rectangleCount : 0;
   STACK_ARRAY(RECT, rects, rect_count);

   for (uint32_t r = 0; r < rect_count; r++) {
      rects[r].left = damage->pRectangles[r].offset.x;
      rects[r].top = damage->pRectangles[r].offset.y;
      rects[r].right = damage->pRectangles[r].offset.x + damage->pRectangles[r].extent.width;
      rects[r].bottom = damage->pRectangles[r].offset.y + damage->pRectangles[r].extent.height;
   }

   DXGI_PRESENT_PARAMETERS params = {
      rect_count,
      rects,
   };

   image->state = WSI_IMAGE_QUEUED;
   UINT sync_interval = chain->base.present_mode == VK_PRESENT_MODE_FIFO_KHR ? 1 : 0;
   UINT present_flags = chain->base.present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ?
      DXGI_PRESENT_ALLOW_TEARING : 0;

   uint64_t target = chain->timing_request.time;
   if (target && (chain->timing_request.flags & VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT)) {
      mtx_lock(&chain->base.present_timing.lock);
      uint64_t previous = chain->base.present_timing.latest_present_time;
      mtx_unlock(&chain->base.present_timing.lock);
      if (!previous)
         previous = os_time_get_nano();
      target = target <= UINT64_MAX - previous ? target + previous : UINT64_MAX;
   }
   if (target) {
      uint64_t now;
      while ((now = os_time_get_nano()) < target) {
         if (!IsWindow(chain->wnd))
            return VK_ERROR_SURFACE_LOST_KHR;
         uint64_t remaining = target - now;
         Sleep((DWORD)MIN2(remaining / 1000000 + (remaining % 1000000 != 0), 50));
      }
   }
   bool needs_record = present_id || chain->timing_request.serial;
   auto record = needs_record ? (struct wsi_win32_present *)vk_zalloc(
      &chain->base.alloc, sizeof(struct wsi_win32_present), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT) : NULL;
   if (needs_record && !record)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if(image->dxgi.profile) image->dxgi.profile->Event(image,"dxgi_mutex_begin");
   mtx_lock(&chain->acquire_mutex);
   if(image->dxgi.profile) image->dxgi.profile->Event(image,"dxgi_mutex_end");
   uint64_t present_begin = chain->native_wait_stats ? os_time_get_nano() : 0;
   if(image->dxgi.profile) image->dxgi.profile->Event(image,"present1_begin",sync_interval,present_flags);
   HRESULT hres = chain->dxgi->Present1(sync_interval, present_flags, &params);
   if(image->dxgi.profile) {
      image->dxgi.profile->Event(image,"present1_end",uint64_t(hres));
      image->dxgi.profile->DisplayStats(image,hres,sync_interval,present_flags);
   }
   if (chain->native_wait_stats) {
      uint64_t delta = os_time_get_nano() - present_begin;
      chain->native_present_ns += delta;
      chain->native_present_max = MAX2(chain->native_present_max, delta);
      chain->native_presents++;
      wsi_native_report_waits(chain, false);
   }
   if (SUCCEEDED(hres) && hres != DXGI_STATUS_OCCLUDED && record) {
      hres = chain->dxgi->GetLastPresentCount(&record->count);
      if (SUCCEEDED(hres)) {
         record->id = present_id;
         record->timing_serial = chain->timing_request.serial;
         record->image = &image->base;
         record->copy_fence = image->dxgi.complete;
         record->copy_fence->AddRef();
         record->copy_value = image->dxgi.serial;
         record->discarded = !IsWindowVisible(chain->wnd) || IsIconic(chain->wnd);
         if (chain->present_tail)
            chain->present_tail->next = record;
         else
            chain->present_head = record;
         chain->present_tail = record;
         record = NULL;
      }
   }
   VkResult poll_result = wsi_win32_poll_present_locked(chain);
   chain->timing_request = {};
   mtx_unlock(&chain->acquire_mutex);
   vk_free(&chain->base.alloc, record);
   if (poll_result != VK_SUCCESS)
      return poll_result;
   if (image->dxgi.buffer && getenv("TU_WDDM_WSI_TRACE")) {
      DXGI_FRAME_STATISTICS stats = {};
      UINT count = 0;
      HRESULT stats_hr = chain->dxgi->GetFrameStatistics(&stats);
      chain->dxgi->GetLastPresentCount(&count);
      fprintf(stderr, "DXGI present hr=%lx count=%u stats=%lx displayed=%u qpc=%llu copy=%llu\n",
              hres, count, stats_hr, stats.PresentCount,
              (unsigned long long)stats.SyncQPCTime.QuadPart,
              (unsigned long long)image->dxgi.serial);
   }
   switch (hres) {
   case DXGI_STATUS_OCCLUDED: return VK_ERROR_OUT_OF_DATE_KHR;
   case DXGI_ERROR_DEVICE_REMOVED: return VK_ERROR_DEVICE_LOST;
   case E_OUTOFMEMORY: return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   default:
      if (FAILED(hres))
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      break;
   }

   if (chain->surface->current_swapchain != chain) {
      if (FAILED(chain->surface->visual->SetContent(chain->dxgi)) ||
          FAILED(chain->wsi->dxgi.dcomp->Commit()))
         return VK_ERROR_OUT_OF_DATE_KHR;
      chain->surface->current_swapchain = chain;
   }

   /* Mark the other image idle */
   return VK_SUCCESS;
}

static VkResult
wsi_win32_queue_present(struct wsi_swapchain *drv_chain,
                        uint32_t image_index,
                        uint64_t present_id,
                        const VkPresentRegionKHR *damage)
{
   struct wsi_win32_swapchain *chain = (struct wsi_win32_swapchain *) drv_chain;
   assert(image_index < chain->base.image_count);
   struct wsi_win32_image *image = &chain->images[image_index];

   assert(image->state == WSI_IMAGE_DRAWING);

   if (chain->dxgi) {
      VkResult result = wsi_win32_queue_present_dxgi(chain, image, present_id, damage);
      if (result < 0)
         p_atomic_set(&chain->status, result);
      return result;
   }

   char *ptr = (char *)image->base.cpu_map;
   char *dptr = (char *)image->sw.ppvBits;

   for (unsigned h = 0; h < chain->extent.height; h++) {
      memcpy(dptr, ptr, chain->extent.width * 4);
      dptr += image->sw.bmp_row_pitch;
      ptr += image->base.row_pitches[0];
   }
   if (!StretchBlt(chain->chain_dc, 0, 0, chain->extent.width, chain->extent.height, image->sw.dc, 0, 0, chain->extent.width, chain->extent.height, SRCCOPY))
      p_atomic_set(&chain->status, VK_ERROR_MEMORY_MAP_FAILED);

   wsi_win32_set_image_idle(chain, image);

   return p_atomic_read(&chain->status);
}

static VkResult
wsi_win32_surface_create_swapchain_dxgi(
   wsi_win32_surface *surface,
   VkDevice device,
   struct wsi_win32 *wsi,
   const VkSwapchainCreateInfoKHR *create_info,
   struct wsi_win32_swapchain *chain)
{
   IDXGIFactory4 *factory = wsi->dxgi.factory;
   ID3D12CommandQueue *queue =
      (ID3D12CommandQueue *)wsi->wsi->win32.get_d3d12_command_queue(device);
   if (!queue)
      return VK_ERROR_INITIALIZATION_FAILED;

   DXGI_ALPHA_MODE alpha_mode;
   switch (create_info->compositeAlpha) {
   case VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR:
      alpha_mode = DXGI_ALPHA_MODE_IGNORE;
      break;
   case VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR:
      alpha_mode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      break;
   case VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR:
      alpha_mode = DXGI_ALPHA_MODE_STRAIGHT;
      break;
   default:
      alpha_mode = DXGI_ALPHA_MODE_UNSPECIFIED;
      break;
   }

   DXGI_SWAP_CHAIN_DESC1 desc = {
      create_info->imageExtent.width,
      create_info->imageExtent.height,
      create_info->imageFormat == VK_FORMAT_R8G8B8A8_UNORM ?
         DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM,
      create_info->imageArrayLayers > 1,  // Stereo
      { 1 },                              // SampleDesc
      0,                                  // Usage (filled in below)
      create_info->minImageCount,
      DXGI_SCALING_STRETCH,
      DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
      alpha_mode,
      chain->base.present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ?
         DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u
   };

   const VkImageUsageFlags2KHR image_usage = vk_swapchain_usage_flags(create_info);

   if (chain->base.image_info.win32_native_tiled) {
      desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
      desc.BufferUsage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
   }

   if (image_usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      desc.BufferUsage |= DXGI_USAGE_SHADER_INPUT;

   if (image_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      desc.BufferUsage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;

   IDXGISwapChain1 *swapchain1;
   if (FAILED(factory->CreateSwapChainForComposition(queue, &desc, NULL, &swapchain1)))
      return VK_ERROR_INITIALIZATION_FAILED;
   HRESULT query_result = swapchain1->QueryInterface(&chain->dxgi);
   swapchain1->Release();
   if (FAILED(query_result))
      return VK_ERROR_INITIALIZATION_FAILED;

   if (chain->base.image_info.win32_native_tiled) {
      const char *option = getenv("TU_WSI_NATIVE_MAX_LATENCY");
      if (option && strcmp(option, "1") && strcmp(option, "2"))
         return VK_ERROR_INITIALIZATION_FAILED;
      chain->native_max_latency = option && !strcmp(option, "1") ? 1 : 2;
      chain->native_wait_stats = wsi_win32_option_enabled("TU_WSI_NATIVE_WAIT_STATS");
      if (FAILED(chain->dxgi->SetMaximumFrameLatency(chain->native_max_latency)))
         return VK_ERROR_INITIALIZATION_FAILED;
      UINT actual = 0;
      if (FAILED(chain->dxgi->GetMaximumFrameLatency(&actual)) || actual != chain->native_max_latency)
         return VK_ERROR_INITIALIZATION_FAILED;
      fprintf(stderr, "WSI_NATIVE_POLICY revision=2 buffers=%u requested_latency=%u actual_latency=%u wait_stats=%u\n",
         create_info->minImageCount, chain->native_max_latency, actual, (unsigned)chain->native_wait_stats);
      chain->native_latency = chain->dxgi->GetFrameLatencyWaitableObject();
      if (!chain->native_latency)
         return VK_ERROR_INITIALIZATION_FAILED;
   }

   if (!surface->target &&
       FAILED(wsi->dxgi.dcomp->CreateTargetForHwnd(surface->base.hwnd, false, &surface->target)))
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!surface->visual) {
      if (FAILED(wsi->dxgi.dcomp->CreateVisual(&surface->visual)) ||
          FAILED(surface->target->SetRoot(surface->visual)) ||
          FAILED(surface->visual->SetContent(chain->dxgi)) ||
          FAILED(wsi->dxgi.dcomp->Commit()))
         return VK_ERROR_INITIALIZATION_FAILED;

      surface->current_swapchain = chain;
   }
   return VK_SUCCESS;
}

static VkResult
wsi_win32_surface_create_swapchain(
   VkIcdSurfaceBase *icd_surface,
   VkDevice device,
   struct wsi_device *wsi_device,
   const VkSwapchainCreateInfoKHR *create_info,
   const VkAllocationCallbacks *allocator,
   struct wsi_swapchain **swapchain_out)
{
   wsi_win32_surface *surface = (wsi_win32_surface *)icd_surface;
   struct wsi_win32 *wsi =
      (struct wsi_win32 *) wsi_device->wsi[VK_ICD_WSI_PLATFORM_WIN32];

   assert(create_info->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   const unsigned num_images = create_info->minImageCount;
   struct wsi_win32_swapchain *chain;
   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);

   chain = (wsi_win32_swapchain *)vk_zalloc(allocator, size,
                     8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   int ret = mtx_init(&chain->acquire_mutex, mtx_plain);
   if (ret != thrd_success) {
      vk_free(allocator, chain);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   ret = u_cnd_monotonic_init(&chain->acquire_cond);
   if (ret != thrd_success) {
      mtx_destroy(&chain->acquire_mutex);
      vk_free(allocator, chain);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   const VkImageUsageFlags2KHR image_usage = vk_swapchain_usage_flags(create_info);

   struct wsi_dxgi_image_params dxgi_image_params = {
      { WSI_IMAGE_TYPE_DXGI },
   };
   dxgi_image_params.storage_image = (image_usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
   const bool native_requested = wsi_win32_option_enabled("TU_WSI_NATIVE_TILED");
   dxgi_image_params.native_tiled = native_requested &&
      wsi_device->win32.import_native_image && wsi_device->win32.create_native_sync &&
      !create_info->flags && create_info->imageArrayLayers == 1 &&
      create_info->minImageCount == 2 && create_info->imageSharingMode == VK_SHARING_MODE_EXCLUSIVE &&
      create_info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM &&
      create_info->imageExtent.width >= 16 && create_info->imageExtent.width <= 4096 &&
      create_info->imageExtent.height && create_info->imageExtent.height <= 4096 &&
      (uint64_t)create_info->imageExtent.width * create_info->imageExtent.height * 4 <= 56 * 1024 * 1024 &&
      !(image_usage & ~(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) &&
      !vk_find_struct_const(create_info->pNext, IMAGE_COMPRESSION_CONTROL_EXT);
   if (native_requested && !dxgi_image_params.native_tiled)
      fprintf(stderr, "WSI_NATIVE_MODE requested=1 active=0 reason=unsupported_configuration format=%u\n", (unsigned)create_info->imageFormat);

   struct wsi_cpu_image_params cpu_image_params = {
      { WSI_IMAGE_TYPE_CPU },
   };

   bool supports_dxgi = wsi->dxgi.factory &&
                        wsi->dxgi.dcomp &&
                        wsi->wsi->win32.get_d3d12_command_queue;
   if (wsi_win32_option_enabled("TU_WSI_VERIFY") &&
       (!supports_dxgi || !wsi_device->win32.create_buffer_memory)) {
      fprintf(stderr, "WSI_PROTOTYPE_REJECT reason=dxgi_buffer_backend_required\n");
      u_cnd_monotonic_destroy(&chain->acquire_cond);
      mtx_destroy(&chain->acquire_mutex);
      vk_free(allocator, chain);
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   struct wsi_base_image_params *image_params = supports_dxgi ?
      &dxgi_image_params.base : &cpu_image_params.base;

   VkResult result = wsi_swapchain_init(wsi_device, &chain->base, device,
                                        create_info, image_params,
                                        allocator);
   if (result != VK_SUCCESS) {
      u_cnd_monotonic_destroy(&chain->acquire_cond);
      mtx_destroy(&chain->acquire_mutex);
      vk_free(allocator, chain);
      return result;
   }

   chain->base.destroy = wsi_win32_swapchain_destroy;
   chain->base.get_wsi_image = wsi_win32_get_wsi_image;
   chain->base.acquire_next_image = wsi_win32_acquire_next_image;
   chain->base.release_images = wsi_win32_release_images;
   chain->base.queue_present = wsi_win32_queue_present;
   if (supports_dxgi && wsi_device->win32.create_buffer_memory) {
      chain->base.wait_for_present = wsi_win32_wait_for_present;
      chain->base.wait_for_present2 = wsi_win32_wait_for_present;
      chain->base.set_timing_request = wsi_win32_set_timing;
      chain->base.poll_timing_request = wsi_win32_poll_timing;
      chain->base.poll_early_refresh = wsi_win32_refresh;
      chain->base.present_timing.time_domain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
   }
   chain->base.present_mode = wsi_swapchain_get_present_mode(wsi_device, create_info);
   chain->extent = create_info->imageExtent;

   chain->wsi = wsi;
   chain->status = VK_SUCCESS;

   chain->surface = surface;

   if (image_params->image_type == WSI_IMAGE_TYPE_DXGI) {
      result = wsi_win32_surface_create_swapchain_dxgi(surface, device, wsi, create_info, chain);
      if (result != VK_SUCCESS)
         goto fail;
   }

   {
      const char *timing = getenv("TU_WSI_COPY_TIMING");
      chain->copy_timing_requested = timing && !strcmp(timing, "1");
      chain->copy_timing_full = wsi_win32_option_enabled("TU_WSI_TIMING_FULL");
      chain->verify_requested = wsi_win32_option_enabled("TU_WSI_VERIFY");
      if (chain->copy_timing_requested && supports_dxgi &&
          chain->base.blit.type == WSI_SWAPCHAIN_BUFFER_BLIT &&
          !(create_info->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)) {
         chain->copy_timing_samples = (wsi_win32_copy_timing_sample *)
            calloc(WSI_WIN32_COPY_TIMING_MAX_SAMPLES, sizeof(*chain->copy_timing_samples));
         if (!chain->copy_timing_samples)
            chain->copy_timing_vk_error = VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   for (uint32_t image = 0; image < num_images; image++) {
      result = wsi_win32_image_init(device, chain,
                                    create_info, allocator,
                                    &chain->images[image]);
      if (result != VK_SUCCESS)
         goto fail;

      chain->base.image_count++;
   }

   *swapchain_out = &chain->base;

   return VK_SUCCESS;

fail:
   wsi_win32_swapchain_destroy(&chain->base, allocator);
   return result;
}

static IDXGIFactory4 *
dxgi_get_factory(bool debug)
{
   HMODULE dxgi_mod = LoadLibraryA("DXGI.DLL");
   if (!dxgi_mod) {
      return NULL;
   }

   typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY2)(UINT flags, REFIID riid, void **ppFactory);
   PFN_CREATE_DXGI_FACTORY2 CreateDXGIFactory2;

   CreateDXGIFactory2 = (PFN_CREATE_DXGI_FACTORY2)GetProcAddress(dxgi_mod, "CreateDXGIFactory2");
   if (!CreateDXGIFactory2) {
      return NULL;
   }

   UINT flags = 0;
   if (debug)
      flags |= DXGI_CREATE_FACTORY_DEBUG;

   IDXGIFactory4 *factory;
   HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory));
   if (FAILED(hr)) {
      return NULL;
   }

   return factory;
}

static IDCompositionDevice *
dcomp_get_device()
{
   HMODULE dcomp_mod = LoadLibraryA("DComp.DLL");
   if (!dcomp_mod) {
      return NULL;
   }

   typedef HRESULT (STDAPICALLTYPE *PFN_DCOMP_CREATE_DEVICE)(IDXGIDevice *, REFIID, void **);
   PFN_DCOMP_CREATE_DEVICE DCompositionCreateDevice;

   DCompositionCreateDevice = (PFN_DCOMP_CREATE_DEVICE)GetProcAddress(dcomp_mod, "DCompositionCreateDevice");
   if (!DCompositionCreateDevice) {
      return NULL;
   }

   IDCompositionDevice *device;
   HRESULT hr = DCompositionCreateDevice(NULL, IID_PPV_ARGS(&device));
   if (FAILED(hr)) {
      return NULL;
   }

   return device;
}

VkResult
wsi_win32_init_wsi(struct wsi_device *wsi_device,
                   const VkAllocationCallbacks *alloc,
                   VkPhysicalDevice physical_device)
{
   struct wsi_win32 *wsi;
   VkResult result;

   wsi = (wsi_win32 *)vk_zalloc(alloc, sizeof(*wsi), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   wsi->physical_device = physical_device;
   wsi->alloc = alloc;
   wsi->wsi = wsi_device;

   if (!wsi_device->sw) {
      wsi->dxgi.factory = dxgi_get_factory(WSI_DEBUG & WSI_DEBUG_DXGI);
      if (!wsi->dxgi.factory) {
         vk_free(alloc, wsi);
         result = VK_ERROR_INITIALIZATION_FAILED;
         goto fail;
      }
      wsi->dxgi.dcomp = dcomp_get_device();
      if (!wsi->dxgi.dcomp) {
         wsi->dxgi.factory->Release();
         vk_free(alloc, wsi);
         result = VK_ERROR_INITIALIZATION_FAILED;
         goto fail;
      }
   }

   wsi->base.get_support = wsi_win32_surface_get_support;
   wsi->base.get_capabilities2 = wsi_win32_surface_get_capabilities2;
   wsi->base.get_formats = wsi_win32_surface_get_formats;
   wsi->base.get_formats2 = wsi_win32_surface_get_formats2;
   wsi->base.get_present_modes = wsi_win32_surface_get_present_modes;
   wsi->base.get_present_rectangles = wsi_win32_surface_get_present_rectangles;
   wsi->base.create_swapchain = wsi_win32_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_WIN32] = &wsi->base;

   return VK_SUCCESS;

fail:
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_WIN32] = NULL;

   return result;
}

void
wsi_win32_finish_wsi(struct wsi_device *wsi_device,
                  const VkAllocationCallbacks *alloc)
{
   struct wsi_win32 *wsi =
      (struct wsi_win32 *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_WIN32];
   if (!wsi)
      return;

   if (wsi->dxgi.factory)
      wsi->dxgi.factory->Release();
   if (wsi->dxgi.dcomp)
      wsi->dxgi.dcomp->Release();

   vk_free(alloc, wsi);
}
