/* turnip_gather_probe: loader-free check of textureGather footprint selection
 * on a D32_SFLOAT 2D-array image, with and without depth compare, around the
 * texel-centre ambiguity (n + 0.5 +- small offsets).  Runs against any ICD
 * DLL that exports vk_icdGetInstanceProcAddr.
 *
 * usage: turnip_gather_probe.exe <path\to\icd.dll>
 *
 * The image is 16x16x2 and texel (x, y, layer) holds
 * (layer * 256 + y * 16 + x + 1) / 1024.  Every output pixel (ix, iy) gathers
 * at texel coordinate (7.5 + offs[ix], 7.5 + offs[iy]) in layer 1, where
 * offs = {-0.004, -0.002, -0.001, -1/512, 0, +1/512, +0.001, +0.002, +0.004}.
 * The expected footprint is texels x 7..8, y 7..8 everywhere except where an
 * offset of -0.001 or more crosses the centre, for which the footprint moves
 * one texel towards smaller coordinates.  The compare gather uses
 * VK_COMPARE_OP_LESS with reference (256 + 7*16 + 7 + 1 + 0.5) / 1024, so a
 * tap is 1.0 exactly when its texel id is above 376, i.e. for every texel of
 * the expected footprint except (7, 7).
 */

#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "vulkan/vulkan_core.h"
#include "gather_spv.h"

typedef PFN_vkVoidFunction(VKAPI_PTR *PFN_icd_gipa)(VkInstance, const char *);

static PFN_icd_gipa icd_gipa;
static PFN_vkGetDeviceProcAddr gdpa;

#define CHECK(x)                                                            \
   do {                                                                     \
      VkResult r_ = (x);                                                    \
      if (r_ != VK_SUCCESS) {                                               \
         fprintf(stderr, "%s failed: VkResult %d\n", #x, (int)r_);          \
         return 1;                                                          \
      }                                                                     \
   } while (0)

#define GET_I(inst, name)                                                   \
   PFN_##name name = (PFN_##name)icd_gipa(inst, #name);                     \
   if (!name) {                                                             \
      fprintf(stderr, "instance entrypoint %s not found\n", #name);         \
      return 1;                                                             \
   }

#define GET_D(dev, name)                                                    \
   PFN_##name name = (PFN_##name)gdpa(dev, #name);                          \
   if (!name) {                                                             \
      fprintf(stderr, "device entrypoint %s not found\n", #name);           \
      return 1;                                                             \
   }

#define TEX_SIZE 16
#define TEX_LAYERS 2
#define GRID 9

static const float offs[GRID] = { -0.004f, -0.002f, -0.001f, -0.001953125f,
                                  0.0f, 0.001953125f, 0.001f, 0.002f, 0.004f };

static int
find_mem_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits,
              VkMemoryPropertyFlags req)
{
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
      if ((bits & (1u << i)) &&
          (mp->memoryTypes[i].propertyFlags & req) == req)
         return (int)i;
   }
   return -1;
}

static float
texel_value(int x, int y, int layer)
{
   return (float)(layer * 256 + y * TEX_SIZE + x + 1) / 1024.0f;
}

int
main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: %s <icd.dll>\n", argv[0]);
      return 2;
   }

#ifdef _WIN32
   HMODULE lib = LoadLibraryExA(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
   if (!lib) {
      fprintf(stderr, "LoadLibraryEx(%s) failed: %lu\n", argv[1],
              (unsigned long)GetLastError());
      return 1;
   }
   icd_gipa = (PFN_icd_gipa)GetProcAddress(lib, "vk_icdGetInstanceProcAddr");
#else
   void *lib = dlopen(argv[1], RTLD_NOW);
   if (!lib) {
      fprintf(stderr, "dlopen(%s) failed: %s\n", argv[1], dlerror());
      return 1;
   }
   icd_gipa = (PFN_icd_gipa)dlsym(lib, "vk_icdGetInstanceProcAddr");
#endif
   if (!icd_gipa) {
      fprintf(stderr, "vk_icdGetInstanceProcAddr not exported\n");
      return 1;
   }

   GET_I(NULL, vkCreateInstance);

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "turnip_gather_probe",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance;
   CHECK(vkCreateInstance(&ici, NULL, &instance));

   GET_I(instance, vkEnumeratePhysicalDevices);
   GET_I(instance, vkGetPhysicalDeviceProperties2);
   GET_I(instance, vkGetPhysicalDeviceQueueFamilyProperties);
   GET_I(instance, vkGetPhysicalDeviceMemoryProperties);
   GET_I(instance, vkGetPhysicalDeviceFormatProperties);
   GET_I(instance, vkCreateDevice);
   GET_I(instance, vkGetDeviceProcAddr);
   gdpa = vkGetDeviceProcAddr;

   uint32_t pd_count = 0;
   CHECK(vkEnumeratePhysicalDevices(instance, &pd_count, NULL));
   if (pd_count == 0) {
      fprintf(stderr, "no physical devices\n");
      return 1;
   }
   VkPhysicalDevice pds[8];
   if (pd_count > 8)
      pd_count = 8;
   CHECK(vkEnumeratePhysicalDevices(instance, &pd_count, pds));
   VkPhysicalDevice pd = pds[0];

   VkPhysicalDeviceDriverProperties drv = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
   };
   VkPhysicalDeviceProperties2 props = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &drv,
   };
   vkGetPhysicalDeviceProperties2(pd, &props);
   printf("device: %s\n", props.properties.deviceName);
   printf("driver: %s / %s\n", drv.driverName, drv.driverInfo);

   VkFormatProperties fp;
   vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_D32_SFLOAT, &fp);
   printf("D32_SFLOAT optimal features: 0x%x (sampled %d, linear filter %d, "
          "transfer dst %d)\n", (unsigned)fp.optimalTilingFeatures,
          !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
          !!(fp.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),
          !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT));

   uint32_t qf_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, NULL);
   VkQueueFamilyProperties qfs[8];
   if (qf_count > 8)
      qf_count = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs);
   uint32_t qfi = UINT32_MAX;
   for (uint32_t i = 0; i < qf_count; i++) {
      if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         qfi = i;
         break;
      }
   }
   if (qfi == UINT32_MAX) {
      fprintf(stderr, "no graphics queue family\n");
      return 1;
   }

   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = qfi,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
   };
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

   GET_D(dev, vkGetDeviceQueue);
   GET_D(dev, vkCreateImage);
   GET_D(dev, vkGetImageMemoryRequirements);
   GET_D(dev, vkAllocateMemory);
   GET_D(dev, vkBindImageMemory);
   GET_D(dev, vkCreateImageView);
   GET_D(dev, vkCreateBuffer);
   GET_D(dev, vkGetBufferMemoryRequirements);
   GET_D(dev, vkBindBufferMemory);
   GET_D(dev, vkMapMemory);
   GET_D(dev, vkUnmapMemory);
   GET_D(dev, vkFlushMappedMemoryRanges);
   GET_D(dev, vkInvalidateMappedMemoryRanges);
   GET_D(dev, vkCreateSampler);
   GET_D(dev, vkCreateDescriptorSetLayout);
   GET_D(dev, vkCreateDescriptorPool);
   GET_D(dev, vkAllocateDescriptorSets);
   GET_D(dev, vkUpdateDescriptorSets);
   GET_D(dev, vkCreatePipelineLayout);
   GET_D(dev, vkCreateRenderPass);
   GET_D(dev, vkCreateFramebuffer);
   GET_D(dev, vkCreateShaderModule);
   GET_D(dev, vkCreateGraphicsPipelines);
   GET_D(dev, vkCreateCommandPool);
   GET_D(dev, vkAllocateCommandBuffers);
   GET_D(dev, vkBeginCommandBuffer);
   GET_D(dev, vkCmdPipelineBarrier);
   GET_D(dev, vkCmdCopyBufferToImage);
   GET_D(dev, vkCmdBeginRenderPass);
   GET_D(dev, vkCmdBindPipeline);
   GET_D(dev, vkCmdBindDescriptorSets);
   GET_D(dev, vkCmdPushConstants);
   GET_D(dev, vkCmdDraw);
   GET_D(dev, vkCmdEndRenderPass);
   GET_D(dev, vkCmdCopyImageToBuffer);
   GET_D(dev, vkEndCommandBuffer);
   GET_D(dev, vkQueueSubmit);
   GET_D(dev, vkQueueWaitIdle);
   GET_D(dev, vkDeviceWaitIdle);

   VkQueue queue;
   vkGetDeviceQueue(dev, qfi, 0, &queue);

   /* two R32G32B32A32_SFLOAT render targets, GRID x GRID */
   VkImageCreateInfo color_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .extent = { GRID, GRID, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage color[2];
   CHECK(vkCreateImage(dev, &color_ci, NULL, &color[0]));
   CHECK(vkCreateImage(dev, &color_ci, NULL, &color[1]));

   /* the D32_SFLOAT 2D-array texture, filled by buffer-to-image copy as
    * Eden's DepthCompareTwin does */
   VkImageCreateInfo tex_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_D32_SFLOAT,
      .extent = { TEX_SIZE, TEX_SIZE, 1 },
      .mipLevels = 1,
      .arrayLayers = TEX_LAYERS,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage tex;
   CHECK(vkCreateImage(dev, &tex_ci, NULL, &tex));

   VkImage images[3] = { color[0], color[1], tex };
   for (int i = 0; i < 3; i++) {
      VkMemoryRequirements mr;
      vkGetImageMemoryRequirements(dev, images[i], &mr);
      int mt = find_mem_type(&mp, mr.memoryTypeBits,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (mt < 0)
         mt = find_mem_type(&mp, mr.memoryTypeBits, 0);
      VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = mr.size,
         .memoryTypeIndex = (uint32_t)mt,
      };
      VkDeviceMemory mem;
      CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
      CHECK(vkBindImageMemory(dev, images[i], mem, 0));
   }

   /* host buffers: upload (texels) and readback (two targets) */
   VkDeviceSize up_size = (VkDeviceSize)TEX_SIZE * TEX_SIZE * TEX_LAYERS * 4;
   VkDeviceSize rb_size = (VkDeviceSize)GRID * GRID * 16 * 2;
   VkBuffer upload, readback;
   VkDeviceMemory up_mem, rb_mem;
   bool up_coherent, rb_coherent;
   {
      VkBufferCreateInfo bci = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = up_size,
         .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      };
      CHECK(vkCreateBuffer(dev, &bci, NULL, &upload));
      bci.size = rb_size;
      bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      CHECK(vkCreateBuffer(dev, &bci, NULL, &readback));

      VkBuffer bufs[2] = { upload, readback };
      VkDeviceMemory *mems[2] = { &up_mem, &rb_mem };
      bool *coh[2] = { &up_coherent, &rb_coherent };
      for (int i = 0; i < 2; i++) {
         VkMemoryRequirements mr;
         vkGetBufferMemoryRequirements(dev, bufs[i], &mr);
         int mt = find_mem_type(&mp, mr.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
         *coh[i] = mt >= 0;
         if (mt < 0)
            mt = find_mem_type(&mp, mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
         if (mt < 0) {
            fprintf(stderr, "no host-visible memory type\n");
            return 1;
         }
         VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = (uint32_t)mt,
         };
         CHECK(vkAllocateMemory(dev, &mai, NULL, mems[i]));
         CHECK(vkBindBufferMemory(dev, bufs[i], *mems[i], 0));
      }
   }

   {
      void *map;
      CHECK(vkMapMemory(dev, up_mem, 0, VK_WHOLE_SIZE, 0, &map));
      float *f = (float *)map;
      for (int l = 0; l < TEX_LAYERS; l++)
         for (int y = 0; y < TEX_SIZE; y++)
            for (int x = 0; x < TEX_SIZE; x++)
               f[(l * TEX_SIZE + y) * TEX_SIZE + x] = texel_value(x, y, l);
      if (!up_coherent) {
         VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = up_mem,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
         };
         CHECK(vkFlushMappedMemoryRanges(dev, 1, &range));
      }
      vkUnmapMemory(dev, up_mem);
   }

   VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = color[0],
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   VkImageView color_view[2];
   CHECK(vkCreateImageView(dev, &vci, NULL, &color_view[0]));
   vci.image = color[1];
   CHECK(vkCreateImageView(dev, &vci, NULL, &color_view[1]));
   vci.image = tex;
   vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
   vci.format = VK_FORMAT_D32_SFLOAT;
   vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
   vci.subresourceRange.layerCount = TEX_LAYERS;
   VkImageView tex_view;
   CHECK(vkCreateImageView(dev, &vci, NULL, &tex_view));

   VkSamplerCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = VK_LOD_CLAMP_NONE,
   };
   VkSampler sampler_plain, sampler_cmp;
   CHECK(vkCreateSampler(dev, &sci, NULL, &sampler_plain));
   sci.compareEnable = VK_TRUE;
   sci.compareOp = VK_COMPARE_OP_LESS;
   CHECK(vkCreateSampler(dev, &sci, NULL, &sampler_cmp));

   VkDescriptorSetLayoutBinding bindings[2] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
   };
   VkDescriptorSetLayoutCreateInfo dslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings,
   };
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

   VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 2,
   };
   VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool pool;
   CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
   VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dsl,
   };
   VkDescriptorSet set;
   CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
   VkDescriptorImageInfo dii[2] = {
      {
         .sampler = sampler_plain,
         .imageView = tex_view,
         .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
      },
      {
         .sampler = sampler_cmp,
         .imageView = tex_view,
         .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
      },
   };
   VkWriteDescriptorSet writes[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &dii[0],
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &dii[1],
      },
   };
   vkUpdateDescriptorSets(dev, 2, writes, 0, NULL);

   VkPushConstantRange pcr = {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = 16,
   };
   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dsl,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcr,
   };
   VkPipelineLayout layout;
   CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));

   VkAttachmentDescription att[2];
   for (int i = 0; i < 2; i++) {
      att[i] = (VkAttachmentDescription){
         .format = VK_FORMAT_R32G32B32A32_SFLOAT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      };
   }
   VkAttachmentReference att_ref[2] = {
      { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
      { .attachment = 1, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
   };
   VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 2,
      .pColorAttachments = att_ref,
   };
   VkSubpassDependency deps[2] = {
      {
         .srcSubpass = VK_SUBPASS_EXTERNAL,
         .dstSubpass = 0,
         .srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      },
      {
         .srcSubpass = 0,
         .dstSubpass = VK_SUBPASS_EXTERNAL,
         .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      },
   };
   VkRenderPassCreateInfo rpci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = att,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 2,
      .pDependencies = deps,
   };
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));

   VkFramebufferCreateInfo fbci = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = rp,
      .attachmentCount = 2,
      .pAttachments = color_view,
      .width = GRID,
      .height = GRID,
      .layers = 1,
   };
   VkFramebuffer fb;
   CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

   VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = gvs_spv_size,
      .pCode = gvs_spv,
   };
   VkShaderModule vs, fs;
   CHECK(vkCreateShaderModule(dev, &smci, NULL, &vs));
   smci.codeSize = gfs_spv_size;
   smci.pCode = gfs_spv;
   CHECK(vkCreateShaderModule(dev, &smci, NULL, &fs));

   VkPipelineShaderStageCreateInfo stages[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs,
         .pName = "main",
      },
      {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs,
         .pName = "main",
      },
   };
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   };
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
   };
   VkViewport viewport = { 0.0f, 0.0f, (float)GRID, (float)GRID, 0.0f, 1.0f };
   VkRect2D scissor = { { 0, 0 }, { GRID, GRID } };
   VkPipelineViewportStateCreateInfo vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &scissor,
   };
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
   };
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   VkPipelineColorBlendAttachmentState cba[2];
   for (int i = 0; i < 2; i++) {
      cba[i] = (VkPipelineColorBlendAttachmentState){
         .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
      };
   }
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = cba,
   };
   VkGraphicsPipelineCreateInfo gpci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vi,
      .pInputAssemblyState = &ia,
      .pViewportState = &vp,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms,
      .pColorBlendState = &cb,
      .layout = layout,
      .renderPass = rp,
      .subpass = 0,
   };
   VkPipeline pipeline;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL,
                                   &pipeline));

   VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = qfi,
   };
   VkCommandPool cmd_pool;
   CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cmd_pool));
   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = cmd_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK(vkBeginCommandBuffer(cmd, &cbbi));

   VkImageSubresourceRange tex_range = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0,
                                         TEX_LAYERS };
   VkImageMemoryBarrier tex_to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = tex,
      .subresourceRange = tex_range,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                        &tex_to_dst);
   VkBufferImageCopy up_region = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, TEX_LAYERS },
      .imageOffset = { 0, 0, 0 },
      .imageExtent = { TEX_SIZE, TEX_SIZE, 1 },
   };
   vkCmdCopyBufferToImage(cmd, upload, tex,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up_region);
   VkImageMemoryBarrier tex_to_read = tex_to_dst;
   tex_to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   tex_to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   tex_to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   tex_to_read.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                        NULL, 1, &tex_to_read);

   VkClearValue clears[2] = {
      { .color = { .float32 = { -1.0f, -1.0f, -1.0f, -1.0f } } },
      { .color = { .float32 = { -1.0f, -1.0f, -1.0f, -1.0f } } },
   };
   VkRenderPassBeginInfo rpbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = rp,
      .framebuffer = fb,
      .renderArea = { { 0, 0 }, { GRID, GRID } },
      .clearValueCount = 2,
      .pClearValues = clears,
   };
   vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                           &set, 0, NULL);
   const float ref = (256.0f + 7.0f * TEX_SIZE + 7.0f + 1.0f + 0.5f) / 1024.0f;
   float pc[4] = { 1.0f / (float)TEX_SIZE, ref, 1.0f, 0.0f };
   vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);

   for (int i = 0; i < 2; i++) {
      VkBufferImageCopy region = {
         .bufferOffset = (VkDeviceSize)i * GRID * GRID * 16,
         .bufferRowLength = 0,
         .bufferImageHeight = 0,
         .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
         .imageOffset = { 0, 0, 0 },
         .imageExtent = { GRID, GRID, 1 },
      };
      vkCmdCopyImageToBuffer(cmd, color[i],
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
                             &region);
   }
   VkBufferMemoryBarrier to_host = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = readback,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &to_host, 0,
                        NULL);
   CHECK(vkEndCommandBuffer(cmd));

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));
   CHECK(vkDeviceWaitIdle(dev));

   void *map;
   CHECK(vkMapMemory(dev, rb_mem, 0, VK_WHOLE_SIZE, 0, &map));
   if (!rb_coherent) {
      VkMappedMemoryRange range = {
         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
         .memory = rb_mem,
         .offset = 0,
         .size = VK_WHOLE_SIZE,
      };
      CHECK(vkInvalidateMappedMemoryRanges(dev, 1, &range));
   }
   const float *ids = (const float *)map;
   const float *cmp = ids + GRID * GRID * 4;

   /* textureGather component order: (x0,y1) (x1,y1) (x1,y0) (x0,y0) */
   static const int gx[4] = { 0, 1, 1, 0 };
   static const int gy[4] = { 1, 1, 0, 0 };

   unsigned id_ok = 0, id_shift = 0, id_bad = 0, cmp_consistent = 0,
            cmp_bad = 0;
   printf("\nfootprint per coordinate offset (texel units); entry = x0,y0 of the "
          "2x2 footprint, '*' = compare result inconsistent with the ids, "
          "'?' = not a texel id\n");
   printf("      dy\\dx");
   for (int ix = 0; ix < GRID; ix++)
      printf(" %+9.6f", offs[ix]);
   printf("\n");
   for (int iy = 0; iy < GRID; iy++) {
      printf(" %+9.6f ", offs[iy]);
      for (int ix = 0; ix < GRID; ix++) {
         const float *p = ids + (iy * GRID + ix) * 4;
         const float *c = cmp + (iy * GRID + ix) * 4;
         int tx[4], ty[4], tl[4];
         bool valid = true;
         for (int k = 0; k < 4; k++) {
            float idf = p[k] - 1.0f;
            int id = (int)floorf(idf + 0.5f);
            if (fabsf(idf - (float)id) > 0.01f || id < 0 ||
                id >= TEX_SIZE * TEX_SIZE * TEX_LAYERS) {
               valid = false;
               break;
            }
            tl[k] = id / (TEX_SIZE * TEX_SIZE);
            ty[k] = (id / TEX_SIZE) % TEX_SIZE;
            tx[k] = id % TEX_SIZE;
         }
         if (!valid) {
            id_bad++;
            printf("     ?    ");
            continue;
         }
         int x0 = tx[3], y0 = ty[3];
         bool shape_ok = true;
         for (int k = 0; k < 4; k++) {
            if (tx[k] != x0 + gx[k] || ty[k] != y0 + gy[k] || tl[k] != 1)
               shape_ok = false;
         }
         if (!shape_ok) {
            id_bad++;
            printf("     ?    ");
            continue;
         }
         /* expected footprint: 7,7 unless the offset moved the coordinate
          * below the texel centre */
         int ex = offs[ix] < 0.0f ? 6 : 7;
         int ey = offs[iy] < 0.0f ? 6 : 7;
         /* an offset of -1/512 or -0.001 may legitimately be absorbed by the
          * coordinate precision of the texture unit (8 fractional bits on
          * most hardware), so count those as 'shift' rather than 'bad' */
         bool exact = (x0 == ex && y0 == ey);
         bool unshifted = (x0 == 7 && y0 == 7);
         if (exact)
            id_ok++;
         else if (unshifted)
            id_shift++;
         else
            id_bad++;
         bool consistent = true;
         for (int k = 0; k < 4; k++) {
            float expect = (ref < texel_value(tx[k], ty[k], tl[k])) ? 1.0f : 0.0f;
            if (fabsf(c[k] - expect) > 0.001f)
               consistent = false;
         }
         if (consistent)
            cmp_consistent++;
         else
            cmp_bad++;
         printf("   %d,%d%s%s  ", x0, y0, consistent ? " " : "*",
                exact ? " " : (unshifted ? "u" : "!"));
      }
      printf("\n");
   }

   printf("\nraw compare results at the centre (offset 0,0): %.2f %.2f %.2f %.2f "
          "(expected 1 1 1 0 for footprint 7,7 with VK_COMPARE_OP_LESS)\n",
          cmp[(4 * GRID + 4) * 4 + 0], cmp[(4 * GRID + 4) * 4 + 1],
          cmp[(4 * GRID + 4) * 4 + 2], cmp[(4 * GRID + 4) * 4 + 3]);
   printf("footprints: as the offset implies %u; offset absorbed (still 7,7) %u; "
          "other %u\n", id_ok, id_shift, id_bad);
   printf("compare gather vs plain gather: consistent %u, inconsistent %u\n",
          cmp_consistent, cmp_bad);
   printf("\nverdict: D32 2D-array upload and plain gather %s; depth-compare "
          "gather %s\n",
          id_bad == 0 ? "ok" : "BROKEN",
          cmp_bad == 0 ? "consistent with the plain gather"
                       : "DISAGREES with the plain gather");

   return (id_bad || cmp_bad) ? 3 : 0;
}
