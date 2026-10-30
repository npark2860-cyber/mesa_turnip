/* turnip_lane_probe: loader-free check of fragment-shader subgroup lane
 * layout and helper-lane shuffles on the Windows Turnip ICD.
 *
 * usage: turnip_lane_probe.exe <path\to\vulkan_freedreno.dll> [size]
 */

#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "vulkan/vulkan_core.h"
#include "probe_spv.h"

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

int
main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: %s <vulkan_freedreno.dll> [size]\n", argv[0]);
      return 2;
   }
   uint32_t size = argc > 2 ? (uint32_t)atoi(argv[2]) : 16;
   if (size < 4 || size > 64)
      size = 16;

   HMODULE lib = LoadLibraryA(argv[1]);
   if (!lib) {
      fprintf(stderr, "LoadLibrary(%s) failed: %lu\n", argv[1],
              (unsigned long)GetLastError());
      return 1;
   }
   icd_gipa = (PFN_icd_gipa)GetProcAddress(lib, "vk_icdGetInstanceProcAddr");
   if (!icd_gipa) {
      fprintf(stderr, "vk_icdGetInstanceProcAddr not exported\n");
      return 1;
   }

   GET_I(NULL, vkCreateInstance);

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "turnip_lane_probe",
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
   VkPhysicalDeviceSubgroupProperties sg = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
      .pNext = &drv,
   };
   VkPhysicalDeviceProperties2 props = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &sg,
   };
   vkGetPhysicalDeviceProperties2(pd, &props);
   printf("device: %s\n", props.properties.deviceName);
   printf("driver: %s / %s\n", drv.driverName, drv.driverInfo);
   printf("subgroupSize=%u supportedStages=0x%x supportedOperations=0x%x "
          "quadOperationsInAllStages=%u\n",
          (unsigned)sg.subgroupSize, (unsigned)sg.supportedStages,
          (unsigned)sg.supportedOperations,
          (unsigned)sg.quadOperationsInAllStages);

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
   GET_D(dev, vkCmdClearColorImage);
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

   VkImageCreateInfo color_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .extent = { size, size, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage color;
   CHECK(vkCreateImage(dev, &color_ci, NULL, &color));

   VkImageCreateInfo tex_ci = color_ci;
   tex_ci.format = VK_FORMAT_R8_UNORM;
   tex_ci.extent.width = tex_ci.extent.height = 1;
   tex_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
   VkImage tex;
   CHECK(vkCreateImage(dev, &tex_ci, NULL, &tex));

   VkImage images[2] = { color, tex };
   for (int i = 0; i < 2; i++) {
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

   VkDeviceSize rb_size = (VkDeviceSize)size * size * 16;
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = rb_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer readback;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &readback));
   VkMemoryRequirements rb_mr;
   vkGetBufferMemoryRequirements(dev, readback, &rb_mr);
   int rb_mt = find_mem_type(&mp, rb_mr.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   bool rb_coherent = rb_mt >= 0;
   if (rb_mt < 0)
      rb_mt = find_mem_type(&mp, rb_mr.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   if (rb_mt < 0) {
      fprintf(stderr, "no host-visible memory type\n");
      return 1;
   }
   VkMemoryAllocateInfo rb_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = rb_mr.size,
      .memoryTypeIndex = (uint32_t)rb_mt,
   };
   VkDeviceMemory rb_mem;
   CHECK(vkAllocateMemory(dev, &rb_mai, NULL, &rb_mem));
   CHECK(vkBindBufferMemory(dev, readback, rb_mem, 0));

   VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = color,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   VkImageView color_view;
   CHECK(vkCreateImageView(dev, &vci, NULL, &color_view));
   vci.image = tex;
   vci.format = VK_FORMAT_R8_UNORM;
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
   VkSampler sampler;
   CHECK(vkCreateSampler(dev, &sci, NULL, &sampler));

   VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
   };
   VkDescriptorSetLayoutCreateInfo dslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

   VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
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
   VkDescriptorImageInfo dii = {
      .sampler = sampler,
      .imageView = tex_view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   };
   VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &dii,
   };
   vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);

   VkPushConstantRange pcr = {
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      .offset = 0,
      .size = 4,
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

   VkAttachmentDescription att = {
      .format = VK_FORMAT_R32G32B32A32_UINT,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
   };
   VkAttachmentReference att_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &att_ref,
   };
   VkSubpassDependency deps[2] = {
      {
         .srcSubpass = VK_SUBPASS_EXTERNAL,
         .dstSubpass = 0,
         .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
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
      .attachmentCount = 1,
      .pAttachments = &att,
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
      .attachmentCount = 1,
      .pAttachments = &color_view,
      .width = size,
      .height = size,
      .layers = 1,
   };
   VkFramebuffer fb;
   CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

   VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = vs_spv_size,
      .pCode = vs_spv,
   };
   VkShaderModule vs, fs;
   CHECK(vkCreateShaderModule(dev, &smci, NULL, &vs));
   smci.codeSize = fs_spv_size;
   smci.pCode = fs_spv;
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
   VkViewport viewport = { 0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f };
   VkRect2D scissor = { { 0, 0 }, { size, size } };
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
   VkPipelineColorBlendAttachmentState cba = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
   };
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &cba,
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

   VkImageSubresourceRange full_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   VkImageMemoryBarrier tex_to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = tex,
      .subresourceRange = full_range,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                        &tex_to_dst);
   VkClearColorValue zero = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
   vkCmdClearColorImage(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero,
                        1, &full_range);
   VkImageMemoryBarrier tex_to_read = tex_to_dst;
   tex_to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   tex_to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   tex_to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   tex_to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                        NULL, 1, &tex_to_read);

   VkClearValue clear = { .color = { .uint32 = { 0, 0, 0, 0 } } };
   VkRenderPassBeginInfo rpbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = rp,
      .framebuffer = fb,
      .renderArea = { { 0, 0 }, { size, size } },
      .clearValueCount = 1,
      .pClearValues = &clear,
   };
   vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                           &set, 0, NULL);
   float inv_size = 1.0f / (float)size;
   vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4,
                      &inv_size);
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);

   VkBufferImageCopy region = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageOffset = { 0, 0, 0 },
      .imageExtent = { size, size, 1 },
   };
   vkCmdCopyImageToBuffer(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          readback, 1, &region);
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
   const uint32_t *px = (const uint32_t *)map;

#define PX(x, y, c) px[(((y) * size) + (x)) * 4 + (c)]
#define COVERED(x, y) (PX(x, y, 3) != 0)
#define LANE(x, y) (PX(x, y, 0) - 1u)

   printf("\nlane map (gl_SubgroupInvocationID per covered pixel, '.' = not "
          "covered), size %ux%u:\n", (unsigned)size, (unsigned)size);
   for (uint32_t y = 0; y < size; y++) {
      printf("y=%2u:", (unsigned)y);
      for (uint32_t x = 0; x < size; x++) {
         if (COVERED(x, y))
            printf(" %3u", (unsigned)LANE(x, y));
         else
            printf("   .");
      }
      printf("\n");
   }

   unsigned covered = 0, h_ok = 0, h_bad = 0, v_ok = 0, v_bad = 0,
            q_ok = 0, q_bad = 0;
   unsigned x1_ok_cov = 0, x1_bad_cov = 0, x1_ok_help = 0, x1_bad_help = 0;
   unsigned x2_ok_cov = 0, x2_bad_cov = 0, x2_ok_help = 0, x2_bad_help = 0;
   unsigned examples = 0;
   for (uint32_t y = 0; y < size; y++) {
      for (uint32_t x = 0; x < size; x++) {
         if (!COVERED(x, y))
            continue;
         covered++;
         uint32_t lane = LANE(x, y);
         if ((x & 1) == 0 && COVERED(x + 1, y)) {
            if (LANE(x + 1, y) == (lane ^ 1u))
               h_ok++;
            else
               h_bad++;
         }
         if ((y & 1) == 0 && COVERED(x, y + 1)) {
            if (LANE(x, y + 1) == (lane ^ 2u))
               v_ok++;
            else
               v_bad++;
         }
         if ((x & 1) == 1 && (y & 1) == 1 && COVERED(x - 1, y - 1)) {
            if ((LANE(x - 1, y - 1) & ~3u) == (lane & ~3u))
               q_ok++;
            else
               q_bad++;
         }
         uint32_t exp1 = ((lane ^ 1u) + 1u) * 1024u;
         uint32_t exp2 = ((lane ^ 2u) + 1u) * 1024u;
         bool n1_cov = COVERED(x ^ 1u, y);
         bool n2_cov = COVERED(x, y ^ 1u);
         bool ok1 = PX(x, y, 1) == exp1;
         bool ok2 = PX(x, y, 2) == exp2;
         if (n1_cov) {
            if (ok1) x1_ok_cov++; else x1_bad_cov++;
         } else {
            if (ok1) x1_ok_help++; else x1_bad_help++;
         }
         if (n2_cov) {
            if (ok2) x2_ok_cov++; else x2_bad_cov++;
         } else {
            if (ok2) x2_ok_help++; else x2_bad_help++;
         }
         if ((!ok1 || !ok2) && examples < 8) {
            examples++;
            printf("  mismatch at (%u,%u) lane %u: xor1 got %u expected %u "
                   "(neighbour %s), xor2 got %u expected %u (neighbour %s)\n",
                   (unsigned)x, (unsigned)y, (unsigned)lane,
                   (unsigned)PX(x, y, 1), (unsigned)exp1,
                   n1_cov ? "covered" : "helper", (unsigned)PX(x, y, 2),
                   (unsigned)exp2, n2_cov ? "covered" : "helper");
         }
      }
   }

   printf("\ncovered pixels: %u\n", covered);
   printf("layout: horizontal pairs lane^1 ok=%u bad=%u; vertical pairs lane^2 "
          "ok=%u bad=%u; quad base (lane & ~3) shared ok=%u bad=%u\n",
          h_ok, h_bad, v_ok, v_bad, q_ok, q_bad);
   printf("shuffleXor(1): neighbour covered ok=%u bad=%u; neighbour helper "
          "ok=%u bad=%u\n", x1_ok_cov, x1_bad_cov, x1_ok_help, x1_bad_help);
   printf("shuffleXor(2): neighbour covered ok=%u bad=%u; neighbour helper "
          "ok=%u bad=%u\n", x2_ok_cov, x2_bad_cov, x2_ok_help, x2_bad_help);

   printf("\nverdict: layout %s; shuffles between covered pixels %s; shuffles "
          "from helper lanes %s\n",
          (h_bad == 0 && v_bad == 0 && q_bad == 0) ? "matches the Vulkan quad order"
                                                   : "DOES NOT match the Vulkan quad order",
          (x1_bad_cov == 0 && x2_bad_cov == 0) ? "ok" : "BROKEN",
          (x1_bad_help == 0 && x2_bad_help == 0) ? "ok" : "return stale data");

   return (h_bad || v_bad || q_bad || x1_bad_cov || x2_bad_cov || x1_bad_help ||
           x2_bad_help) ? 3 : 0;
}
