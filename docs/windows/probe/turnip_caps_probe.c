#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vulkan/vulkan_core.h"

typedef PFN_vkVoidFunction(VKAPI_PTR *PFN_icd_gipa)(VkInstance, const char *);

static PFN_icd_gipa icd_gipa;

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

static const struct { const char *name; VkFormat fmt; } formats[] = {
   { "VK_FORMAT_R4G4_UNORM_PACK8", (VkFormat)1 },
   { "VK_FORMAT_R4G4B4A4_UNORM_PACK16", (VkFormat)2 },
   { "VK_FORMAT_B4G4R4A4_UNORM_PACK16", (VkFormat)3 },
   { "VK_FORMAT_R5G6B5_UNORM_PACK16", (VkFormat)4 },
   { "VK_FORMAT_B5G6R5_UNORM_PACK16", (VkFormat)5 },
   { "VK_FORMAT_R5G5B5A1_UNORM_PACK16", (VkFormat)6 },
   { "VK_FORMAT_B5G5R5A1_UNORM_PACK16", (VkFormat)7 },
   { "VK_FORMAT_A1R5G5B5_UNORM_PACK16", (VkFormat)8 },
   { "VK_FORMAT_R8_UNORM", (VkFormat)9 },
   { "VK_FORMAT_R8_SNORM", (VkFormat)10 },
   { "VK_FORMAT_R8_USCALED", (VkFormat)11 },
   { "VK_FORMAT_R8_SSCALED", (VkFormat)12 },
   { "VK_FORMAT_R8_UINT", (VkFormat)13 },
   { "VK_FORMAT_R8_SINT", (VkFormat)14 },
   { "VK_FORMAT_R8_SRGB", (VkFormat)15 },
   { "VK_FORMAT_R8G8_UNORM", (VkFormat)16 },
   { "VK_FORMAT_R8G8_SNORM", (VkFormat)17 },
   { "VK_FORMAT_R8G8_USCALED", (VkFormat)18 },
   { "VK_FORMAT_R8G8_SSCALED", (VkFormat)19 },
   { "VK_FORMAT_R8G8_UINT", (VkFormat)20 },
   { "VK_FORMAT_R8G8_SINT", (VkFormat)21 },
   { "VK_FORMAT_R8G8_SRGB", (VkFormat)22 },
   { "VK_FORMAT_R8G8B8_UNORM", (VkFormat)23 },
   { "VK_FORMAT_R8G8B8_SNORM", (VkFormat)24 },
   { "VK_FORMAT_R8G8B8_USCALED", (VkFormat)25 },
   { "VK_FORMAT_R8G8B8_SSCALED", (VkFormat)26 },
   { "VK_FORMAT_R8G8B8_UINT", (VkFormat)27 },
   { "VK_FORMAT_R8G8B8_SINT", (VkFormat)28 },
   { "VK_FORMAT_R8G8B8_SRGB", (VkFormat)29 },
   { "VK_FORMAT_B8G8R8_UNORM", (VkFormat)30 },
   { "VK_FORMAT_B8G8R8_SNORM", (VkFormat)31 },
   { "VK_FORMAT_B8G8R8_USCALED", (VkFormat)32 },
   { "VK_FORMAT_B8G8R8_SSCALED", (VkFormat)33 },
   { "VK_FORMAT_B8G8R8_UINT", (VkFormat)34 },
   { "VK_FORMAT_B8G8R8_SINT", (VkFormat)35 },
   { "VK_FORMAT_B8G8R8_SRGB", (VkFormat)36 },
   { "VK_FORMAT_R8G8B8A8_UNORM", (VkFormat)37 },
   { "VK_FORMAT_R8G8B8A8_SNORM", (VkFormat)38 },
   { "VK_FORMAT_R8G8B8A8_USCALED", (VkFormat)39 },
   { "VK_FORMAT_R8G8B8A8_SSCALED", (VkFormat)40 },
   { "VK_FORMAT_R8G8B8A8_UINT", (VkFormat)41 },
   { "VK_FORMAT_R8G8B8A8_SINT", (VkFormat)42 },
   { "VK_FORMAT_R8G8B8A8_SRGB", (VkFormat)43 },
   { "VK_FORMAT_B8G8R8A8_UNORM", (VkFormat)44 },
   { "VK_FORMAT_B8G8R8A8_SNORM", (VkFormat)45 },
   { "VK_FORMAT_B8G8R8A8_USCALED", (VkFormat)46 },
   { "VK_FORMAT_B8G8R8A8_SSCALED", (VkFormat)47 },
   { "VK_FORMAT_B8G8R8A8_UINT", (VkFormat)48 },
   { "VK_FORMAT_B8G8R8A8_SINT", (VkFormat)49 },
   { "VK_FORMAT_B8G8R8A8_SRGB", (VkFormat)50 },
   { "VK_FORMAT_A8B8G8R8_UNORM_PACK32", (VkFormat)51 },
   { "VK_FORMAT_A8B8G8R8_SNORM_PACK32", (VkFormat)52 },
   { "VK_FORMAT_A8B8G8R8_USCALED_PACK32", (VkFormat)53 },
   { "VK_FORMAT_A8B8G8R8_SSCALED_PACK32", (VkFormat)54 },
   { "VK_FORMAT_A8B8G8R8_UINT_PACK32", (VkFormat)55 },
   { "VK_FORMAT_A8B8G8R8_SINT_PACK32", (VkFormat)56 },
   { "VK_FORMAT_A8B8G8R8_SRGB_PACK32", (VkFormat)57 },
   { "VK_FORMAT_A2R10G10B10_UNORM_PACK32", (VkFormat)58 },
   { "VK_FORMAT_A2R10G10B10_SNORM_PACK32", (VkFormat)59 },
   { "VK_FORMAT_A2R10G10B10_USCALED_PACK32", (VkFormat)60 },
   { "VK_FORMAT_A2R10G10B10_SSCALED_PACK32", (VkFormat)61 },
   { "VK_FORMAT_A2R10G10B10_UINT_PACK32", (VkFormat)62 },
   { "VK_FORMAT_A2R10G10B10_SINT_PACK32", (VkFormat)63 },
   { "VK_FORMAT_A2B10G10R10_UNORM_PACK32", (VkFormat)64 },
   { "VK_FORMAT_A2B10G10R10_SNORM_PACK32", (VkFormat)65 },
   { "VK_FORMAT_A2B10G10R10_USCALED_PACK32", (VkFormat)66 },
   { "VK_FORMAT_A2B10G10R10_SSCALED_PACK32", (VkFormat)67 },
   { "VK_FORMAT_A2B10G10R10_UINT_PACK32", (VkFormat)68 },
   { "VK_FORMAT_A2B10G10R10_SINT_PACK32", (VkFormat)69 },
   { "VK_FORMAT_R16_UNORM", (VkFormat)70 },
   { "VK_FORMAT_R16_SNORM", (VkFormat)71 },
   { "VK_FORMAT_R16_USCALED", (VkFormat)72 },
   { "VK_FORMAT_R16_SSCALED", (VkFormat)73 },
   { "VK_FORMAT_R16_UINT", (VkFormat)74 },
   { "VK_FORMAT_R16_SINT", (VkFormat)75 },
   { "VK_FORMAT_R16_SFLOAT", (VkFormat)76 },
   { "VK_FORMAT_R16G16_UNORM", (VkFormat)77 },
   { "VK_FORMAT_R16G16_SNORM", (VkFormat)78 },
   { "VK_FORMAT_R16G16_USCALED", (VkFormat)79 },
   { "VK_FORMAT_R16G16_SSCALED", (VkFormat)80 },
   { "VK_FORMAT_R16G16_UINT", (VkFormat)81 },
   { "VK_FORMAT_R16G16_SINT", (VkFormat)82 },
   { "VK_FORMAT_R16G16_SFLOAT", (VkFormat)83 },
   { "VK_FORMAT_R16G16B16_UNORM", (VkFormat)84 },
   { "VK_FORMAT_R16G16B16_SNORM", (VkFormat)85 },
   { "VK_FORMAT_R16G16B16_USCALED", (VkFormat)86 },
   { "VK_FORMAT_R16G16B16_SSCALED", (VkFormat)87 },
   { "VK_FORMAT_R16G16B16_UINT", (VkFormat)88 },
   { "VK_FORMAT_R16G16B16_SINT", (VkFormat)89 },
   { "VK_FORMAT_R16G16B16_SFLOAT", (VkFormat)90 },
   { "VK_FORMAT_R16G16B16A16_UNORM", (VkFormat)91 },
   { "VK_FORMAT_R16G16B16A16_SNORM", (VkFormat)92 },
   { "VK_FORMAT_R16G16B16A16_USCALED", (VkFormat)93 },
   { "VK_FORMAT_R16G16B16A16_SSCALED", (VkFormat)94 },
   { "VK_FORMAT_R16G16B16A16_UINT", (VkFormat)95 },
   { "VK_FORMAT_R16G16B16A16_SINT", (VkFormat)96 },
   { "VK_FORMAT_R16G16B16A16_SFLOAT", (VkFormat)97 },
   { "VK_FORMAT_R32_UINT", (VkFormat)98 },
   { "VK_FORMAT_R32_SINT", (VkFormat)99 },
   { "VK_FORMAT_R32_SFLOAT", (VkFormat)100 },
   { "VK_FORMAT_R32G32_UINT", (VkFormat)101 },
   { "VK_FORMAT_R32G32_SINT", (VkFormat)102 },
   { "VK_FORMAT_R32G32_SFLOAT", (VkFormat)103 },
   { "VK_FORMAT_R32G32B32_UINT", (VkFormat)104 },
   { "VK_FORMAT_R32G32B32_SINT", (VkFormat)105 },
   { "VK_FORMAT_R32G32B32_SFLOAT", (VkFormat)106 },
   { "VK_FORMAT_R32G32B32A32_UINT", (VkFormat)107 },
   { "VK_FORMAT_R32G32B32A32_SINT", (VkFormat)108 },
   { "VK_FORMAT_R32G32B32A32_SFLOAT", (VkFormat)109 },
   { "VK_FORMAT_R64_UINT", (VkFormat)110 },
   { "VK_FORMAT_R64_SINT", (VkFormat)111 },
   { "VK_FORMAT_R64_SFLOAT", (VkFormat)112 },
   { "VK_FORMAT_R64G64_UINT", (VkFormat)113 },
   { "VK_FORMAT_R64G64_SINT", (VkFormat)114 },
   { "VK_FORMAT_R64G64_SFLOAT", (VkFormat)115 },
   { "VK_FORMAT_R64G64B64_UINT", (VkFormat)116 },
   { "VK_FORMAT_R64G64B64_SINT", (VkFormat)117 },
   { "VK_FORMAT_R64G64B64_SFLOAT", (VkFormat)118 },
   { "VK_FORMAT_R64G64B64A64_UINT", (VkFormat)119 },
   { "VK_FORMAT_R64G64B64A64_SINT", (VkFormat)120 },
   { "VK_FORMAT_R64G64B64A64_SFLOAT", (VkFormat)121 },
   { "VK_FORMAT_B10G11R11_UFLOAT_PACK32", (VkFormat)122 },
   { "VK_FORMAT_E5B9G9R9_UFLOAT_PACK32", (VkFormat)123 },
   { "VK_FORMAT_D16_UNORM", (VkFormat)124 },
   { "VK_FORMAT_X8_D24_UNORM_PACK32", (VkFormat)125 },
   { "VK_FORMAT_D32_SFLOAT", (VkFormat)126 },
   { "VK_FORMAT_S8_UINT", (VkFormat)127 },
   { "VK_FORMAT_D16_UNORM_S8_UINT", (VkFormat)128 },
   { "VK_FORMAT_D24_UNORM_S8_UINT", (VkFormat)129 },
   { "VK_FORMAT_D32_SFLOAT_S8_UINT", (VkFormat)130 },
   { "VK_FORMAT_BC1_RGB_UNORM_BLOCK", (VkFormat)131 },
   { "VK_FORMAT_BC1_RGB_SRGB_BLOCK", (VkFormat)132 },
   { "VK_FORMAT_BC1_RGBA_UNORM_BLOCK", (VkFormat)133 },
   { "VK_FORMAT_BC1_RGBA_SRGB_BLOCK", (VkFormat)134 },
   { "VK_FORMAT_BC2_UNORM_BLOCK", (VkFormat)135 },
   { "VK_FORMAT_BC2_SRGB_BLOCK", (VkFormat)136 },
   { "VK_FORMAT_BC3_UNORM_BLOCK", (VkFormat)137 },
   { "VK_FORMAT_BC3_SRGB_BLOCK", (VkFormat)138 },
   { "VK_FORMAT_BC4_UNORM_BLOCK", (VkFormat)139 },
   { "VK_FORMAT_BC4_SNORM_BLOCK", (VkFormat)140 },
   { "VK_FORMAT_BC5_UNORM_BLOCK", (VkFormat)141 },
   { "VK_FORMAT_BC5_SNORM_BLOCK", (VkFormat)142 },
   { "VK_FORMAT_BC6H_UFLOAT_BLOCK", (VkFormat)143 },
   { "VK_FORMAT_BC6H_SFLOAT_BLOCK", (VkFormat)144 },
   { "VK_FORMAT_BC7_UNORM_BLOCK", (VkFormat)145 },
   { "VK_FORMAT_BC7_SRGB_BLOCK", (VkFormat)146 },
   { "VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK", (VkFormat)147 },
   { "VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK", (VkFormat)148 },
   { "VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK", (VkFormat)149 },
   { "VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK", (VkFormat)150 },
   { "VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK", (VkFormat)151 },
   { "VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK", (VkFormat)152 },
   { "VK_FORMAT_EAC_R11_UNORM_BLOCK", (VkFormat)153 },
   { "VK_FORMAT_EAC_R11_SNORM_BLOCK", (VkFormat)154 },
   { "VK_FORMAT_EAC_R11G11_UNORM_BLOCK", (VkFormat)155 },
   { "VK_FORMAT_EAC_R11G11_SNORM_BLOCK", (VkFormat)156 },
   { "VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG", (VkFormat)1000054000 },
   { "VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG", (VkFormat)1000054001 },
   { "VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG", (VkFormat)1000054002 },
   { "VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG", (VkFormat)1000054003 },
   { "VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG", (VkFormat)1000054004 },
   { "VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG", (VkFormat)1000054005 },
   { "VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG", (VkFormat)1000054006 },
   { "VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG", (VkFormat)1000054007 },
   { "VK_FORMAT_G8B8G8R8_422_UNORM", (VkFormat)1000156000 },
   { "VK_FORMAT_B8G8R8G8_422_UNORM", (VkFormat)1000156001 },
   { "VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM", (VkFormat)1000156002 },
   { "VK_FORMAT_G8_B8R8_2PLANE_420_UNORM", (VkFormat)1000156003 },
   { "VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM", (VkFormat)1000156004 },
   { "VK_FORMAT_G8_B8R8_2PLANE_422_UNORM", (VkFormat)1000156005 },
   { "VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM", (VkFormat)1000156006 },
   { "VK_FORMAT_R10X6_UNORM_PACK16", (VkFormat)1000156007 },
   { "VK_FORMAT_R10X6G10X6_UNORM_2PACK16", (VkFormat)1000156008 },
   { "VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16", (VkFormat)1000156009 },
   { "VK_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16", (VkFormat)1000156010 },
   { "VK_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16", (VkFormat)1000156011 },
   { "VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16", (VkFormat)1000156012 },
   { "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16", (VkFormat)1000156013 },
   { "VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16", (VkFormat)1000156014 },
   { "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16", (VkFormat)1000156015 },
   { "VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16", (VkFormat)1000156016 },
   { "VK_FORMAT_R12X4_UNORM_PACK16", (VkFormat)1000156017 },
   { "VK_FORMAT_R12X4G12X4_UNORM_2PACK16", (VkFormat)1000156018 },
   { "VK_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16", (VkFormat)1000156019 },
   { "VK_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16", (VkFormat)1000156020 },
   { "VK_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16", (VkFormat)1000156021 },
   { "VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16", (VkFormat)1000156022 },
   { "VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16", (VkFormat)1000156023 },
   { "VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16", (VkFormat)1000156024 },
   { "VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16", (VkFormat)1000156025 },
   { "VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16", (VkFormat)1000156026 },
   { "VK_FORMAT_G16B16G16R16_422_UNORM", (VkFormat)1000156027 },
   { "VK_FORMAT_B16G16R16G16_422_UNORM", (VkFormat)1000156028 },
   { "VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM", (VkFormat)1000156029 },
   { "VK_FORMAT_G16_B16R16_2PLANE_420_UNORM", (VkFormat)1000156030 },
   { "VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM", (VkFormat)1000156031 },
   { "VK_FORMAT_G16_B16R16_2PLANE_422_UNORM", (VkFormat)1000156032 },
   { "VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM", (VkFormat)1000156033 },
   { "VK_FORMAT_G8_B8R8_2PLANE_444_UNORM", (VkFormat)1000330000 },
   { "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16", (VkFormat)1000330001 },
   { "VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16", (VkFormat)1000330002 },
   { "VK_FORMAT_G16_B16R16_2PLANE_444_UNORM", (VkFormat)1000330003 },
   { "VK_FORMAT_A4R4G4B4_UNORM_PACK16", (VkFormat)1000340000 },
   { "VK_FORMAT_A4B4G4R4_UNORM_PACK16", (VkFormat)1000340001 },
   { "VK_FORMAT_R8_BOOL_ARM", (VkFormat)1000460000 },
   { "VK_FORMAT_R16_SFLOAT_FPENCODING_BFLOAT16_ARM", (VkFormat)1000460001 },
   { "VK_FORMAT_R8_SFLOAT_FPENCODING_FLOAT8E4M3_ARM", (VkFormat)1000460002 },
   { "VK_FORMAT_R8_SFLOAT_FPENCODING_FLOAT8E5M2_ARM", (VkFormat)1000460003 },
   { "VK_FORMAT_R16G16_SFIXED5_NV", (VkFormat)1000464000 },
   { "VK_FORMAT_A1B5G5R5_UNORM_PACK16", (VkFormat)1000470000 },
   { "VK_FORMAT_A8_UNORM", (VkFormat)1000470001 },
   { "VK_FORMAT_R10X6_UINT_PACK16_ARM", (VkFormat)1000609000 },
   { "VK_FORMAT_R10X6G10X6_UINT_2PACK16_ARM", (VkFormat)1000609001 },
   { "VK_FORMAT_R10X6G10X6B10X6A10X6_UINT_4PACK16_ARM", (VkFormat)1000609002 },
   { "VK_FORMAT_R12X4_UINT_PACK16_ARM", (VkFormat)1000609003 },
   { "VK_FORMAT_R12X4G12X4_UINT_2PACK16_ARM", (VkFormat)1000609004 },
   { "VK_FORMAT_R12X4G12X4B12X4A12X4_UINT_4PACK16_ARM", (VkFormat)1000609005 },
   { "VK_FORMAT_R14X2_UINT_PACK16_ARM", (VkFormat)1000609006 },
   { "VK_FORMAT_R14X2G14X2_UINT_2PACK16_ARM", (VkFormat)1000609007 },
   { "VK_FORMAT_R14X2G14X2B14X2A14X2_UINT_4PACK16_ARM", (VkFormat)1000609008 },
   { "VK_FORMAT_R14X2_UNORM_PACK16_ARM", (VkFormat)1000609009 },
   { "VK_FORMAT_R14X2G14X2_UNORM_2PACK16_ARM", (VkFormat)1000609010 },
   { "VK_FORMAT_R14X2G14X2B14X2A14X2_UNORM_4PACK16_ARM", (VkFormat)1000609011 },
   { "VK_FORMAT_G14X2_B14X2R14X2_2PLANE_420_UNORM_3PACK16_ARM", (VkFormat)1000609012 },
   { "VK_FORMAT_G14X2_B14X2R14X2_2PLANE_422_UNORM_3PACK16_ARM", (VkFormat)1000609013 },
};

int
main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: %s <icd dll>\n", argv[0]);
      return 2;
   }
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
   printf("dll=%s\n", argv[1]);

   GET_I(NULL, vkCreateInstance);
   GET_I(NULL, vkEnumerateInstanceVersion);
   uint32_t iver = 0;
   vkEnumerateInstanceVersion(&iver);
   printf("instanceVersion=%u.%u.%u\n", VK_API_VERSION_MAJOR(iver),
          VK_API_VERSION_MINOR(iver), VK_API_VERSION_PATCH(iver));

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "yuzu Emulator",
      .applicationVersion = VK_MAKE_VERSION(1, 3, 0),
      .pEngineName = "yuzu Emulator",
      .engineVersion = VK_MAKE_VERSION(1, 3, 0),
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
   GET_I(instance, vkGetPhysicalDeviceFeatures2);
   GET_I(instance, vkGetPhysicalDeviceFormatProperties2);
   GET_I(instance, vkGetPhysicalDeviceQueueFamilyProperties);
   GET_I(instance, vkGetPhysicalDeviceMemoryProperties);
   GET_I(instance, vkEnumerateDeviceExtensionProperties);

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
   printf("physicalDevices=%u\n", pd_count);

   VkPhysicalDeviceVulkan13Properties p13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
   };
   VkPhysicalDeviceVulkan12Properties p12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
      .pNext = &p13,
   };
   VkPhysicalDeviceVulkan11Properties p11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
      .pNext = &p12,
   };
   VkPhysicalDeviceProperties2 props = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &p11,
   };
   vkGetPhysicalDeviceProperties2(pd, &props);
   printf("deviceName=%s\n", props.properties.deviceName);
   printf("apiVersion=%u.%u.%u driverVersion=0x%x vendorID=0x%x deviceID=0x%x deviceType=%d\n",
          VK_API_VERSION_MAJOR(props.properties.apiVersion),
          VK_API_VERSION_MINOR(props.properties.apiVersion),
          VK_API_VERSION_PATCH(props.properties.apiVersion),
          props.properties.driverVersion, props.properties.vendorID,
          props.properties.deviceID, (int)props.properties.deviceType);
   printf("pipelineCacheUUID=");
   for (int i = 0; i < 16; i++)
      printf("%02x", props.properties.pipelineCacheUUID[i]);
   printf("\n");
   printf("driverName=%s\ndriverInfo=%s\n", p12.driverName, p12.driverInfo);

   VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
   };
   VkPhysicalDeviceVulkan12Features f12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = &f13,
   };
   VkPhysicalDeviceVulkan11Features f11 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
      .pNext = &f12,
   };
   VkPhysicalDeviceFeatures2 feat = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &f11,
   };
   vkGetPhysicalDeviceFeatures2(pd, &feat);

   printf("VkPhysicalDeviceLimits.maxImageDimension1D=%llu\n", (unsigned long long)props.properties.limits.maxImageDimension1D);
   printf("VkPhysicalDeviceLimits.maxImageDimension2D=%llu\n", (unsigned long long)props.properties.limits.maxImageDimension2D);
   printf("VkPhysicalDeviceLimits.maxImageDimension3D=%llu\n", (unsigned long long)props.properties.limits.maxImageDimension3D);
   printf("VkPhysicalDeviceLimits.maxImageDimensionCube=%llu\n", (unsigned long long)props.properties.limits.maxImageDimensionCube);
   printf("VkPhysicalDeviceLimits.maxImageArrayLayers=%llu\n", (unsigned long long)props.properties.limits.maxImageArrayLayers);
   printf("VkPhysicalDeviceLimits.maxTexelBufferElements=%llu\n", (unsigned long long)props.properties.limits.maxTexelBufferElements);
   printf("VkPhysicalDeviceLimits.maxUniformBufferRange=%llu\n", (unsigned long long)props.properties.limits.maxUniformBufferRange);
   printf("VkPhysicalDeviceLimits.maxStorageBufferRange=%llu\n", (unsigned long long)props.properties.limits.maxStorageBufferRange);
   printf("VkPhysicalDeviceLimits.maxPushConstantsSize=%llu\n", (unsigned long long)props.properties.limits.maxPushConstantsSize);
   printf("VkPhysicalDeviceLimits.maxMemoryAllocationCount=%llu\n", (unsigned long long)props.properties.limits.maxMemoryAllocationCount);
   printf("VkPhysicalDeviceLimits.maxSamplerAllocationCount=%llu\n", (unsigned long long)props.properties.limits.maxSamplerAllocationCount);
   printf("VkPhysicalDeviceLimits.bufferImageGranularity=%llu\n", (unsigned long long)props.properties.limits.bufferImageGranularity);
   printf("VkPhysicalDeviceLimits.sparseAddressSpaceSize=%llu\n", (unsigned long long)props.properties.limits.sparseAddressSpaceSize);
   printf("VkPhysicalDeviceLimits.maxBoundDescriptorSets=%llu\n", (unsigned long long)props.properties.limits.maxBoundDescriptorSets);
   printf("VkPhysicalDeviceLimits.maxPerStageDescriptorSamplers=%llu\n", (unsigned long long)props.properties.limits.maxPerStageDescriptorSamplers);
   printf("VkPhysicalDeviceLimits.maxPerStageDescriptorUniformBuffers=%llu\n", (unsigned long long)props.properties.limits.maxPerStageDescriptorUniformBuffers);
   printf("VkPhysicalDeviceLimits.maxPerStageDescriptorStorageBuffers=%llu\n", (unsigned long long)props.properties.limits.maxPerStageDescriptorStorageBuffers);
   printf("VkPhysicalDeviceLimits.maxPerStageDescriptorSampledImages=%llu\n", (unsigned long long)props.properties.limits.maxPerStageDescriptorSampledImages);
   printf("VkPhysicalDeviceLimits.maxPerStageDescriptorStorageImages=%llu\n", (unsigned long long)props.properties.limits.maxPerStageDescriptorStorageImages);
   printf("VkPhysicalDeviceLimits.maxPerStageDescriptorInputAttachments=%llu\n", (unsigned long long)props.properties.limits.maxPerStageDescriptorInputAttachments);
   printf("VkPhysicalDeviceLimits.maxPerStageResources=%llu\n", (unsigned long long)props.properties.limits.maxPerStageResources);
   printf("VkPhysicalDeviceLimits.maxDescriptorSetSamplers=%llu\n", (unsigned long long)props.properties.limits.maxDescriptorSetSamplers);
   printf("VkPhysicalDeviceLimits.maxDescriptorSetUniformBuffers=%llu\n", (unsigned long long)props.properties.limits.maxDescriptorSetUniformBuffers);
   printf("VkPhysicalDeviceLimits.maxDescriptorSetUniformBuffersDynamic=%llu\n", (unsigned long long)props.properties.limits.maxDescriptorSetUniformBuffersDynamic);
   printf("VkPhysicalDeviceLimits.maxDescriptorSetStorageBuffers=%llu\n", (unsigned long long)props.properties.limits.maxDescriptorSetStorageBuffers);
   printf("VkPhysicalDeviceLimits.maxDescriptorSetStorageBuffersDynamic=%llu\n", (unsigned long long)props.properties.limits.maxDescriptorSetStorageBuffersDynamic);
   printf("VkPhysicalDeviceLimits.maxDescriptorSetSampledImages=%llu\n", (unsigned long long)props.properties.limits.maxDescriptorSetSampledImages);
   printf("VkPhysicalDeviceLimits.maxDescriptorSetStorageImages=%llu\n", (unsigned long long)props.properties.limits.maxDescriptorSetStorageImages);
   printf("VkPhysicalDeviceLimits.maxDescriptorSetInputAttachments=%llu\n", (unsigned long long)props.properties.limits.maxDescriptorSetInputAttachments);
   printf("VkPhysicalDeviceLimits.maxVertexInputAttributes=%llu\n", (unsigned long long)props.properties.limits.maxVertexInputAttributes);
   printf("VkPhysicalDeviceLimits.maxVertexInputBindings=%llu\n", (unsigned long long)props.properties.limits.maxVertexInputBindings);
   printf("VkPhysicalDeviceLimits.maxVertexInputAttributeOffset=%llu\n", (unsigned long long)props.properties.limits.maxVertexInputAttributeOffset);
   printf("VkPhysicalDeviceLimits.maxVertexInputBindingStride=%llu\n", (unsigned long long)props.properties.limits.maxVertexInputBindingStride);
   printf("VkPhysicalDeviceLimits.maxVertexOutputComponents=%llu\n", (unsigned long long)props.properties.limits.maxVertexOutputComponents);
   printf("VkPhysicalDeviceLimits.maxTessellationGenerationLevel=%llu\n", (unsigned long long)props.properties.limits.maxTessellationGenerationLevel);
   printf("VkPhysicalDeviceLimits.maxTessellationPatchSize=%llu\n", (unsigned long long)props.properties.limits.maxTessellationPatchSize);
   printf("VkPhysicalDeviceLimits.maxTessellationControlPerVertexInputComponents=%llu\n", (unsigned long long)props.properties.limits.maxTessellationControlPerVertexInputComponents);
   printf("VkPhysicalDeviceLimits.maxTessellationControlPerVertexOutputComponents=%llu\n", (unsigned long long)props.properties.limits.maxTessellationControlPerVertexOutputComponents);
   printf("VkPhysicalDeviceLimits.maxTessellationControlPerPatchOutputComponents=%llu\n", (unsigned long long)props.properties.limits.maxTessellationControlPerPatchOutputComponents);
   printf("VkPhysicalDeviceLimits.maxTessellationControlTotalOutputComponents=%llu\n", (unsigned long long)props.properties.limits.maxTessellationControlTotalOutputComponents);
   printf("VkPhysicalDeviceLimits.maxTessellationEvaluationInputComponents=%llu\n", (unsigned long long)props.properties.limits.maxTessellationEvaluationInputComponents);
   printf("VkPhysicalDeviceLimits.maxTessellationEvaluationOutputComponents=%llu\n", (unsigned long long)props.properties.limits.maxTessellationEvaluationOutputComponents);
   printf("VkPhysicalDeviceLimits.maxGeometryShaderInvocations=%llu\n", (unsigned long long)props.properties.limits.maxGeometryShaderInvocations);
   printf("VkPhysicalDeviceLimits.maxGeometryInputComponents=%llu\n", (unsigned long long)props.properties.limits.maxGeometryInputComponents);
   printf("VkPhysicalDeviceLimits.maxGeometryOutputComponents=%llu\n", (unsigned long long)props.properties.limits.maxGeometryOutputComponents);
   printf("VkPhysicalDeviceLimits.maxGeometryOutputVertices=%llu\n", (unsigned long long)props.properties.limits.maxGeometryOutputVertices);
   printf("VkPhysicalDeviceLimits.maxGeometryTotalOutputComponents=%llu\n", (unsigned long long)props.properties.limits.maxGeometryTotalOutputComponents);
   printf("VkPhysicalDeviceLimits.maxFragmentInputComponents=%llu\n", (unsigned long long)props.properties.limits.maxFragmentInputComponents);
   printf("VkPhysicalDeviceLimits.maxFragmentOutputAttachments=%llu\n", (unsigned long long)props.properties.limits.maxFragmentOutputAttachments);
   printf("VkPhysicalDeviceLimits.maxFragmentDualSrcAttachments=%llu\n", (unsigned long long)props.properties.limits.maxFragmentDualSrcAttachments);
   printf("VkPhysicalDeviceLimits.maxFragmentCombinedOutputResources=%llu\n", (unsigned long long)props.properties.limits.maxFragmentCombinedOutputResources);
   printf("VkPhysicalDeviceLimits.maxComputeSharedMemorySize=%llu\n", (unsigned long long)props.properties.limits.maxComputeSharedMemorySize);
   printf("VkPhysicalDeviceLimits.maxComputeWorkGroupCount=");  for (int i_ = 0; i_ < 3; i_++) printf("%llu ", (unsigned long long)props.properties.limits.maxComputeWorkGroupCount[i_]); printf("\n");
   printf("VkPhysicalDeviceLimits.maxComputeWorkGroupInvocations=%llu\n", (unsigned long long)props.properties.limits.maxComputeWorkGroupInvocations);
   printf("VkPhysicalDeviceLimits.maxComputeWorkGroupSize=");  for (int i_ = 0; i_ < 3; i_++) printf("%llu ", (unsigned long long)props.properties.limits.maxComputeWorkGroupSize[i_]); printf("\n");
   printf("VkPhysicalDeviceLimits.subPixelPrecisionBits=%llu\n", (unsigned long long)props.properties.limits.subPixelPrecisionBits);
   printf("VkPhysicalDeviceLimits.subTexelPrecisionBits=%llu\n", (unsigned long long)props.properties.limits.subTexelPrecisionBits);
   printf("VkPhysicalDeviceLimits.mipmapPrecisionBits=%llu\n", (unsigned long long)props.properties.limits.mipmapPrecisionBits);
   printf("VkPhysicalDeviceLimits.maxDrawIndexedIndexValue=%llu\n", (unsigned long long)props.properties.limits.maxDrawIndexedIndexValue);
   printf("VkPhysicalDeviceLimits.maxDrawIndirectCount=%llu\n", (unsigned long long)props.properties.limits.maxDrawIndirectCount);
   printf("VkPhysicalDeviceLimits.maxSamplerLodBias=%g\n", (double)props.properties.limits.maxSamplerLodBias);
   printf("VkPhysicalDeviceLimits.maxSamplerAnisotropy=%g\n", (double)props.properties.limits.maxSamplerAnisotropy);
   printf("VkPhysicalDeviceLimits.maxViewports=%llu\n", (unsigned long long)props.properties.limits.maxViewports);
   printf("VkPhysicalDeviceLimits.maxViewportDimensions=");  for (int i_ = 0; i_ < 2; i_++) printf("%llu ", (unsigned long long)props.properties.limits.maxViewportDimensions[i_]); printf("\n");
   printf("VkPhysicalDeviceLimits.viewportBoundsRange=");  for (int i_ = 0; i_ < 2; i_++) printf("%g ", (double)props.properties.limits.viewportBoundsRange[i_]); printf("\n");
   printf("VkPhysicalDeviceLimits.viewportSubPixelBits=%llu\n", (unsigned long long)props.properties.limits.viewportSubPixelBits);
   printf("VkPhysicalDeviceLimits.minMemoryMapAlignment=%llu\n", (unsigned long long)props.properties.limits.minMemoryMapAlignment);
   printf("VkPhysicalDeviceLimits.minTexelBufferOffsetAlignment=%llu\n", (unsigned long long)props.properties.limits.minTexelBufferOffsetAlignment);
   printf("VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment=%llu\n", (unsigned long long)props.properties.limits.minUniformBufferOffsetAlignment);
   printf("VkPhysicalDeviceLimits.minStorageBufferOffsetAlignment=%llu\n", (unsigned long long)props.properties.limits.minStorageBufferOffsetAlignment);
   printf("VkPhysicalDeviceLimits.minTexelOffset=%d\n", (int)props.properties.limits.minTexelOffset);
   printf("VkPhysicalDeviceLimits.maxTexelOffset=%llu\n", (unsigned long long)props.properties.limits.maxTexelOffset);
   printf("VkPhysicalDeviceLimits.minTexelGatherOffset=%d\n", (int)props.properties.limits.minTexelGatherOffset);
   printf("VkPhysicalDeviceLimits.maxTexelGatherOffset=%llu\n", (unsigned long long)props.properties.limits.maxTexelGatherOffset);
   printf("VkPhysicalDeviceLimits.minInterpolationOffset=%g\n", (double)props.properties.limits.minInterpolationOffset);
   printf("VkPhysicalDeviceLimits.maxInterpolationOffset=%g\n", (double)props.properties.limits.maxInterpolationOffset);
   printf("VkPhysicalDeviceLimits.subPixelInterpolationOffsetBits=%llu\n", (unsigned long long)props.properties.limits.subPixelInterpolationOffsetBits);
   printf("VkPhysicalDeviceLimits.maxFramebufferWidth=%llu\n", (unsigned long long)props.properties.limits.maxFramebufferWidth);
   printf("VkPhysicalDeviceLimits.maxFramebufferHeight=%llu\n", (unsigned long long)props.properties.limits.maxFramebufferHeight);
   printf("VkPhysicalDeviceLimits.maxFramebufferLayers=%llu\n", (unsigned long long)props.properties.limits.maxFramebufferLayers);
   printf("VkPhysicalDeviceLimits.framebufferColorSampleCounts=%llu\n", (unsigned long long)props.properties.limits.framebufferColorSampleCounts);
   printf("VkPhysicalDeviceLimits.framebufferDepthSampleCounts=%llu\n", (unsigned long long)props.properties.limits.framebufferDepthSampleCounts);
   printf("VkPhysicalDeviceLimits.framebufferStencilSampleCounts=%llu\n", (unsigned long long)props.properties.limits.framebufferStencilSampleCounts);
   printf("VkPhysicalDeviceLimits.framebufferNoAttachmentsSampleCounts=%llu\n", (unsigned long long)props.properties.limits.framebufferNoAttachmentsSampleCounts);
   printf("VkPhysicalDeviceLimits.maxColorAttachments=%llu\n", (unsigned long long)props.properties.limits.maxColorAttachments);
   printf("VkPhysicalDeviceLimits.sampledImageColorSampleCounts=%llu\n", (unsigned long long)props.properties.limits.sampledImageColorSampleCounts);
   printf("VkPhysicalDeviceLimits.sampledImageIntegerSampleCounts=%llu\n", (unsigned long long)props.properties.limits.sampledImageIntegerSampleCounts);
   printf("VkPhysicalDeviceLimits.sampledImageDepthSampleCounts=%llu\n", (unsigned long long)props.properties.limits.sampledImageDepthSampleCounts);
   printf("VkPhysicalDeviceLimits.sampledImageStencilSampleCounts=%llu\n", (unsigned long long)props.properties.limits.sampledImageStencilSampleCounts);
   printf("VkPhysicalDeviceLimits.storageImageSampleCounts=%llu\n", (unsigned long long)props.properties.limits.storageImageSampleCounts);
   printf("VkPhysicalDeviceLimits.maxSampleMaskWords=%llu\n", (unsigned long long)props.properties.limits.maxSampleMaskWords);
   printf("VkPhysicalDeviceLimits.timestampComputeAndGraphics=%llu\n", (unsigned long long)props.properties.limits.timestampComputeAndGraphics);
   printf("VkPhysicalDeviceLimits.timestampPeriod=%g\n", (double)props.properties.limits.timestampPeriod);
   printf("VkPhysicalDeviceLimits.maxClipDistances=%llu\n", (unsigned long long)props.properties.limits.maxClipDistances);
   printf("VkPhysicalDeviceLimits.maxCullDistances=%llu\n", (unsigned long long)props.properties.limits.maxCullDistances);
   printf("VkPhysicalDeviceLimits.maxCombinedClipAndCullDistances=%llu\n", (unsigned long long)props.properties.limits.maxCombinedClipAndCullDistances);
   printf("VkPhysicalDeviceLimits.discreteQueuePriorities=%llu\n", (unsigned long long)props.properties.limits.discreteQueuePriorities);
   printf("VkPhysicalDeviceLimits.pointSizeRange=");  for (int i_ = 0; i_ < 2; i_++) printf("%g ", (double)props.properties.limits.pointSizeRange[i_]); printf("\n");
   printf("VkPhysicalDeviceLimits.lineWidthRange=");  for (int i_ = 0; i_ < 2; i_++) printf("%g ", (double)props.properties.limits.lineWidthRange[i_]); printf("\n");
   printf("VkPhysicalDeviceLimits.pointSizeGranularity=%g\n", (double)props.properties.limits.pointSizeGranularity);
   printf("VkPhysicalDeviceLimits.lineWidthGranularity=%g\n", (double)props.properties.limits.lineWidthGranularity);
   printf("VkPhysicalDeviceLimits.strictLines=%llu\n", (unsigned long long)props.properties.limits.strictLines);
   printf("VkPhysicalDeviceLimits.standardSampleLocations=%llu\n", (unsigned long long)props.properties.limits.standardSampleLocations);
   printf("VkPhysicalDeviceLimits.optimalBufferCopyOffsetAlignment=%llu\n", (unsigned long long)props.properties.limits.optimalBufferCopyOffsetAlignment);
   printf("VkPhysicalDeviceLimits.optimalBufferCopyRowPitchAlignment=%llu\n", (unsigned long long)props.properties.limits.optimalBufferCopyRowPitchAlignment);
   printf("VkPhysicalDeviceLimits.nonCoherentAtomSize=%llu\n", (unsigned long long)props.properties.limits.nonCoherentAtomSize);

   printf("VkPhysicalDeviceFeatures.robustBufferAccess=%llu\n", (unsigned long long)feat.features.robustBufferAccess);
   printf("VkPhysicalDeviceFeatures.fullDrawIndexUint32=%llu\n", (unsigned long long)feat.features.fullDrawIndexUint32);
   printf("VkPhysicalDeviceFeatures.imageCubeArray=%llu\n", (unsigned long long)feat.features.imageCubeArray);
   printf("VkPhysicalDeviceFeatures.independentBlend=%llu\n", (unsigned long long)feat.features.independentBlend);
   printf("VkPhysicalDeviceFeatures.geometryShader=%llu\n", (unsigned long long)feat.features.geometryShader);
   printf("VkPhysicalDeviceFeatures.tessellationShader=%llu\n", (unsigned long long)feat.features.tessellationShader);
   printf("VkPhysicalDeviceFeatures.sampleRateShading=%llu\n", (unsigned long long)feat.features.sampleRateShading);
   printf("VkPhysicalDeviceFeatures.dualSrcBlend=%llu\n", (unsigned long long)feat.features.dualSrcBlend);
   printf("VkPhysicalDeviceFeatures.logicOp=%llu\n", (unsigned long long)feat.features.logicOp);
   printf("VkPhysicalDeviceFeatures.multiDrawIndirect=%llu\n", (unsigned long long)feat.features.multiDrawIndirect);
   printf("VkPhysicalDeviceFeatures.drawIndirectFirstInstance=%llu\n", (unsigned long long)feat.features.drawIndirectFirstInstance);
   printf("VkPhysicalDeviceFeatures.depthClamp=%llu\n", (unsigned long long)feat.features.depthClamp);
   printf("VkPhysicalDeviceFeatures.depthBiasClamp=%llu\n", (unsigned long long)feat.features.depthBiasClamp);
   printf("VkPhysicalDeviceFeatures.fillModeNonSolid=%llu\n", (unsigned long long)feat.features.fillModeNonSolid);
   printf("VkPhysicalDeviceFeatures.depthBounds=%llu\n", (unsigned long long)feat.features.depthBounds);
   printf("VkPhysicalDeviceFeatures.wideLines=%llu\n", (unsigned long long)feat.features.wideLines);
   printf("VkPhysicalDeviceFeatures.largePoints=%llu\n", (unsigned long long)feat.features.largePoints);
   printf("VkPhysicalDeviceFeatures.alphaToOne=%llu\n", (unsigned long long)feat.features.alphaToOne);
   printf("VkPhysicalDeviceFeatures.multiViewport=%llu\n", (unsigned long long)feat.features.multiViewport);
   printf("VkPhysicalDeviceFeatures.samplerAnisotropy=%llu\n", (unsigned long long)feat.features.samplerAnisotropy);
   printf("VkPhysicalDeviceFeatures.textureCompressionETC2=%llu\n", (unsigned long long)feat.features.textureCompressionETC2);
   printf("VkPhysicalDeviceFeatures.textureCompressionASTC_LDR=%llu\n", (unsigned long long)feat.features.textureCompressionASTC_LDR);
   printf("VkPhysicalDeviceFeatures.textureCompressionBC=%llu\n", (unsigned long long)feat.features.textureCompressionBC);
   printf("VkPhysicalDeviceFeatures.occlusionQueryPrecise=%llu\n", (unsigned long long)feat.features.occlusionQueryPrecise);
   printf("VkPhysicalDeviceFeatures.pipelineStatisticsQuery=%llu\n", (unsigned long long)feat.features.pipelineStatisticsQuery);
   printf("VkPhysicalDeviceFeatures.vertexPipelineStoresAndAtomics=%llu\n", (unsigned long long)feat.features.vertexPipelineStoresAndAtomics);
   printf("VkPhysicalDeviceFeatures.fragmentStoresAndAtomics=%llu\n", (unsigned long long)feat.features.fragmentStoresAndAtomics);
   printf("VkPhysicalDeviceFeatures.shaderTessellationAndGeometryPointSize=%llu\n", (unsigned long long)feat.features.shaderTessellationAndGeometryPointSize);
   printf("VkPhysicalDeviceFeatures.shaderImageGatherExtended=%llu\n", (unsigned long long)feat.features.shaderImageGatherExtended);
   printf("VkPhysicalDeviceFeatures.shaderStorageImageExtendedFormats=%llu\n", (unsigned long long)feat.features.shaderStorageImageExtendedFormats);
   printf("VkPhysicalDeviceFeatures.shaderStorageImageMultisample=%llu\n", (unsigned long long)feat.features.shaderStorageImageMultisample);
   printf("VkPhysicalDeviceFeatures.shaderStorageImageReadWithoutFormat=%llu\n", (unsigned long long)feat.features.shaderStorageImageReadWithoutFormat);
   printf("VkPhysicalDeviceFeatures.shaderStorageImageWriteWithoutFormat=%llu\n", (unsigned long long)feat.features.shaderStorageImageWriteWithoutFormat);
   printf("VkPhysicalDeviceFeatures.shaderUniformBufferArrayDynamicIndexing=%llu\n", (unsigned long long)feat.features.shaderUniformBufferArrayDynamicIndexing);
   printf("VkPhysicalDeviceFeatures.shaderSampledImageArrayDynamicIndexing=%llu\n", (unsigned long long)feat.features.shaderSampledImageArrayDynamicIndexing);
   printf("VkPhysicalDeviceFeatures.shaderStorageBufferArrayDynamicIndexing=%llu\n", (unsigned long long)feat.features.shaderStorageBufferArrayDynamicIndexing);
   printf("VkPhysicalDeviceFeatures.shaderStorageImageArrayDynamicIndexing=%llu\n", (unsigned long long)feat.features.shaderStorageImageArrayDynamicIndexing);
   printf("VkPhysicalDeviceFeatures.shaderClipDistance=%llu\n", (unsigned long long)feat.features.shaderClipDistance);
   printf("VkPhysicalDeviceFeatures.shaderCullDistance=%llu\n", (unsigned long long)feat.features.shaderCullDistance);
   printf("VkPhysicalDeviceFeatures.shaderFloat64=%llu\n", (unsigned long long)feat.features.shaderFloat64);
   printf("VkPhysicalDeviceFeatures.shaderInt64=%llu\n", (unsigned long long)feat.features.shaderInt64);
   printf("VkPhysicalDeviceFeatures.shaderInt16=%llu\n", (unsigned long long)feat.features.shaderInt16);
   printf("VkPhysicalDeviceFeatures.shaderResourceResidency=%llu\n", (unsigned long long)feat.features.shaderResourceResidency);
   printf("VkPhysicalDeviceFeatures.shaderResourceMinLod=%llu\n", (unsigned long long)feat.features.shaderResourceMinLod);
   printf("VkPhysicalDeviceFeatures.sparseBinding=%llu\n", (unsigned long long)feat.features.sparseBinding);
   printf("VkPhysicalDeviceFeatures.sparseResidencyBuffer=%llu\n", (unsigned long long)feat.features.sparseResidencyBuffer);
   printf("VkPhysicalDeviceFeatures.sparseResidencyImage2D=%llu\n", (unsigned long long)feat.features.sparseResidencyImage2D);
   printf("VkPhysicalDeviceFeatures.sparseResidencyImage3D=%llu\n", (unsigned long long)feat.features.sparseResidencyImage3D);
   printf("VkPhysicalDeviceFeatures.sparseResidency2Samples=%llu\n", (unsigned long long)feat.features.sparseResidency2Samples);
   printf("VkPhysicalDeviceFeatures.sparseResidency4Samples=%llu\n", (unsigned long long)feat.features.sparseResidency4Samples);
   printf("VkPhysicalDeviceFeatures.sparseResidency8Samples=%llu\n", (unsigned long long)feat.features.sparseResidency8Samples);
   printf("VkPhysicalDeviceFeatures.sparseResidency16Samples=%llu\n", (unsigned long long)feat.features.sparseResidency16Samples);
   printf("VkPhysicalDeviceFeatures.sparseResidencyAliased=%llu\n", (unsigned long long)feat.features.sparseResidencyAliased);
   printf("VkPhysicalDeviceFeatures.variableMultisampleRate=%llu\n", (unsigned long long)feat.features.variableMultisampleRate);
   printf("VkPhysicalDeviceFeatures.inheritedQueries=%llu\n", (unsigned long long)feat.features.inheritedQueries);

   printf("VkPhysicalDeviceVulkan11Features.storageBuffer16BitAccess=%llu\n", (unsigned long long)f11.storageBuffer16BitAccess);
   printf("VkPhysicalDeviceVulkan11Features.uniformAndStorageBuffer16BitAccess=%llu\n", (unsigned long long)f11.uniformAndStorageBuffer16BitAccess);
   printf("VkPhysicalDeviceVulkan11Features.storagePushConstant16=%llu\n", (unsigned long long)f11.storagePushConstant16);
   printf("VkPhysicalDeviceVulkan11Features.storageInputOutput16=%llu\n", (unsigned long long)f11.storageInputOutput16);
   printf("VkPhysicalDeviceVulkan11Features.multiview=%llu\n", (unsigned long long)f11.multiview);
   printf("VkPhysicalDeviceVulkan11Features.multiviewGeometryShader=%llu\n", (unsigned long long)f11.multiviewGeometryShader);
   printf("VkPhysicalDeviceVulkan11Features.multiviewTessellationShader=%llu\n", (unsigned long long)f11.multiviewTessellationShader);
   printf("VkPhysicalDeviceVulkan11Features.variablePointersStorageBuffer=%llu\n", (unsigned long long)f11.variablePointersStorageBuffer);
   printf("VkPhysicalDeviceVulkan11Features.variablePointers=%llu\n", (unsigned long long)f11.variablePointers);
   printf("VkPhysicalDeviceVulkan11Features.protectedMemory=%llu\n", (unsigned long long)f11.protectedMemory);
   printf("VkPhysicalDeviceVulkan11Features.samplerYcbcrConversion=%llu\n", (unsigned long long)f11.samplerYcbcrConversion);
   printf("VkPhysicalDeviceVulkan11Features.shaderDrawParameters=%llu\n", (unsigned long long)f11.shaderDrawParameters);

   printf("VkPhysicalDeviceVulkan12Features.samplerMirrorClampToEdge=%llu\n", (unsigned long long)f12.samplerMirrorClampToEdge);
   printf("VkPhysicalDeviceVulkan12Features.drawIndirectCount=%llu\n", (unsigned long long)f12.drawIndirectCount);
   printf("VkPhysicalDeviceVulkan12Features.storageBuffer8BitAccess=%llu\n", (unsigned long long)f12.storageBuffer8BitAccess);
   printf("VkPhysicalDeviceVulkan12Features.uniformAndStorageBuffer8BitAccess=%llu\n", (unsigned long long)f12.uniformAndStorageBuffer8BitAccess);
   printf("VkPhysicalDeviceVulkan12Features.storagePushConstant8=%llu\n", (unsigned long long)f12.storagePushConstant8);
   printf("VkPhysicalDeviceVulkan12Features.shaderBufferInt64Atomics=%llu\n", (unsigned long long)f12.shaderBufferInt64Atomics);
   printf("VkPhysicalDeviceVulkan12Features.shaderSharedInt64Atomics=%llu\n", (unsigned long long)f12.shaderSharedInt64Atomics);
   printf("VkPhysicalDeviceVulkan12Features.shaderFloat16=%llu\n", (unsigned long long)f12.shaderFloat16);
   printf("VkPhysicalDeviceVulkan12Features.shaderInt8=%llu\n", (unsigned long long)f12.shaderInt8);
   printf("VkPhysicalDeviceVulkan12Features.descriptorIndexing=%llu\n", (unsigned long long)f12.descriptorIndexing);
   printf("VkPhysicalDeviceVulkan12Features.shaderInputAttachmentArrayDynamicIndexing=%llu\n", (unsigned long long)f12.shaderInputAttachmentArrayDynamicIndexing);
   printf("VkPhysicalDeviceVulkan12Features.shaderUniformTexelBufferArrayDynamicIndexing=%llu\n", (unsigned long long)f12.shaderUniformTexelBufferArrayDynamicIndexing);
   printf("VkPhysicalDeviceVulkan12Features.shaderStorageTexelBufferArrayDynamicIndexing=%llu\n", (unsigned long long)f12.shaderStorageTexelBufferArrayDynamicIndexing);
   printf("VkPhysicalDeviceVulkan12Features.shaderUniformBufferArrayNonUniformIndexing=%llu\n", (unsigned long long)f12.shaderUniformBufferArrayNonUniformIndexing);
   printf("VkPhysicalDeviceVulkan12Features.shaderSampledImageArrayNonUniformIndexing=%llu\n", (unsigned long long)f12.shaderSampledImageArrayNonUniformIndexing);
   printf("VkPhysicalDeviceVulkan12Features.shaderStorageBufferArrayNonUniformIndexing=%llu\n", (unsigned long long)f12.shaderStorageBufferArrayNonUniformIndexing);
   printf("VkPhysicalDeviceVulkan12Features.shaderStorageImageArrayNonUniformIndexing=%llu\n", (unsigned long long)f12.shaderStorageImageArrayNonUniformIndexing);
   printf("VkPhysicalDeviceVulkan12Features.shaderInputAttachmentArrayNonUniformIndexing=%llu\n", (unsigned long long)f12.shaderInputAttachmentArrayNonUniformIndexing);
   printf("VkPhysicalDeviceVulkan12Features.shaderUniformTexelBufferArrayNonUniformIndexing=%llu\n", (unsigned long long)f12.shaderUniformTexelBufferArrayNonUniformIndexing);
   printf("VkPhysicalDeviceVulkan12Features.shaderStorageTexelBufferArrayNonUniformIndexing=%llu\n", (unsigned long long)f12.shaderStorageTexelBufferArrayNonUniformIndexing);
   printf("VkPhysicalDeviceVulkan12Features.descriptorBindingUniformBufferUpdateAfterBind=%llu\n", (unsigned long long)f12.descriptorBindingUniformBufferUpdateAfterBind);
   printf("VkPhysicalDeviceVulkan12Features.descriptorBindingSampledImageUpdateAfterBind=%llu\n", (unsigned long long)f12.descriptorBindingSampledImageUpdateAfterBind);
   printf("VkPhysicalDeviceVulkan12Features.descriptorBindingStorageImageUpdateAfterBind=%llu\n", (unsigned long long)f12.descriptorBindingStorageImageUpdateAfterBind);
   printf("VkPhysicalDeviceVulkan12Features.descriptorBindingStorageBufferUpdateAfterBind=%llu\n", (unsigned long long)f12.descriptorBindingStorageBufferUpdateAfterBind);
   printf("VkPhysicalDeviceVulkan12Features.descriptorBindingUniformTexelBufferUpdateAfterBind=%llu\n", (unsigned long long)f12.descriptorBindingUniformTexelBufferUpdateAfterBind);
   printf("VkPhysicalDeviceVulkan12Features.descriptorBindingStorageTexelBufferUpdateAfterBind=%llu\n", (unsigned long long)f12.descriptorBindingStorageTexelBufferUpdateAfterBind);
   printf("VkPhysicalDeviceVulkan12Features.descriptorBindingUpdateUnusedWhilePending=%llu\n", (unsigned long long)f12.descriptorBindingUpdateUnusedWhilePending);
   printf("VkPhysicalDeviceVulkan12Features.descriptorBindingPartiallyBound=%llu\n", (unsigned long long)f12.descriptorBindingPartiallyBound);
   printf("VkPhysicalDeviceVulkan12Features.descriptorBindingVariableDescriptorCount=%llu\n", (unsigned long long)f12.descriptorBindingVariableDescriptorCount);
   printf("VkPhysicalDeviceVulkan12Features.runtimeDescriptorArray=%llu\n", (unsigned long long)f12.runtimeDescriptorArray);
   printf("VkPhysicalDeviceVulkan12Features.samplerFilterMinmax=%llu\n", (unsigned long long)f12.samplerFilterMinmax);
   printf("VkPhysicalDeviceVulkan12Features.scalarBlockLayout=%llu\n", (unsigned long long)f12.scalarBlockLayout);
   printf("VkPhysicalDeviceVulkan12Features.imagelessFramebuffer=%llu\n", (unsigned long long)f12.imagelessFramebuffer);
   printf("VkPhysicalDeviceVulkan12Features.uniformBufferStandardLayout=%llu\n", (unsigned long long)f12.uniformBufferStandardLayout);
   printf("VkPhysicalDeviceVulkan12Features.shaderSubgroupExtendedTypes=%llu\n", (unsigned long long)f12.shaderSubgroupExtendedTypes);
   printf("VkPhysicalDeviceVulkan12Features.separateDepthStencilLayouts=%llu\n", (unsigned long long)f12.separateDepthStencilLayouts);
   printf("VkPhysicalDeviceVulkan12Features.hostQueryReset=%llu\n", (unsigned long long)f12.hostQueryReset);
   printf("VkPhysicalDeviceVulkan12Features.timelineSemaphore=%llu\n", (unsigned long long)f12.timelineSemaphore);
   printf("VkPhysicalDeviceVulkan12Features.bufferDeviceAddress=%llu\n", (unsigned long long)f12.bufferDeviceAddress);
   printf("VkPhysicalDeviceVulkan12Features.bufferDeviceAddressCaptureReplay=%llu\n", (unsigned long long)f12.bufferDeviceAddressCaptureReplay);
   printf("VkPhysicalDeviceVulkan12Features.bufferDeviceAddressMultiDevice=%llu\n", (unsigned long long)f12.bufferDeviceAddressMultiDevice);
   printf("VkPhysicalDeviceVulkan12Features.vulkanMemoryModel=%llu\n", (unsigned long long)f12.vulkanMemoryModel);
   printf("VkPhysicalDeviceVulkan12Features.vulkanMemoryModelDeviceScope=%llu\n", (unsigned long long)f12.vulkanMemoryModelDeviceScope);
   printf("VkPhysicalDeviceVulkan12Features.vulkanMemoryModelAvailabilityVisibilityChains=%llu\n", (unsigned long long)f12.vulkanMemoryModelAvailabilityVisibilityChains);
   printf("VkPhysicalDeviceVulkan12Features.shaderOutputViewportIndex=%llu\n", (unsigned long long)f12.shaderOutputViewportIndex);
   printf("VkPhysicalDeviceVulkan12Features.shaderOutputLayer=%llu\n", (unsigned long long)f12.shaderOutputLayer);
   printf("VkPhysicalDeviceVulkan12Features.subgroupBroadcastDynamicId=%llu\n", (unsigned long long)f12.subgroupBroadcastDynamicId);

   printf("VkPhysicalDeviceVulkan13Features.robustImageAccess=%llu\n", (unsigned long long)f13.robustImageAccess);
   printf("VkPhysicalDeviceVulkan13Features.inlineUniformBlock=%llu\n", (unsigned long long)f13.inlineUniformBlock);
   printf("VkPhysicalDeviceVulkan13Features.descriptorBindingInlineUniformBlockUpdateAfterBind=%llu\n", (unsigned long long)f13.descriptorBindingInlineUniformBlockUpdateAfterBind);
   printf("VkPhysicalDeviceVulkan13Features.pipelineCreationCacheControl=%llu\n", (unsigned long long)f13.pipelineCreationCacheControl);
   printf("VkPhysicalDeviceVulkan13Features.privateData=%llu\n", (unsigned long long)f13.privateData);
   printf("VkPhysicalDeviceVulkan13Features.shaderDemoteToHelperInvocation=%llu\n", (unsigned long long)f13.shaderDemoteToHelperInvocation);
   printf("VkPhysicalDeviceVulkan13Features.shaderTerminateInvocation=%llu\n", (unsigned long long)f13.shaderTerminateInvocation);
   printf("VkPhysicalDeviceVulkan13Features.subgroupSizeControl=%llu\n", (unsigned long long)f13.subgroupSizeControl);
   printf("VkPhysicalDeviceVulkan13Features.computeFullSubgroups=%llu\n", (unsigned long long)f13.computeFullSubgroups);
   printf("VkPhysicalDeviceVulkan13Features.synchronization2=%llu\n", (unsigned long long)f13.synchronization2);
   printf("VkPhysicalDeviceVulkan13Features.textureCompressionASTC_HDR=%llu\n", (unsigned long long)f13.textureCompressionASTC_HDR);
   printf("VkPhysicalDeviceVulkan13Features.shaderZeroInitializeWorkgroupMemory=%llu\n", (unsigned long long)f13.shaderZeroInitializeWorkgroupMemory);
   printf("VkPhysicalDeviceVulkan13Features.dynamicRendering=%llu\n", (unsigned long long)f13.dynamicRendering);
   printf("VkPhysicalDeviceVulkan13Features.shaderIntegerDotProduct=%llu\n", (unsigned long long)f13.shaderIntegerDotProduct);
   printf("VkPhysicalDeviceVulkan13Features.maintenance4=%llu\n", (unsigned long long)f13.maintenance4);

   printf("VkPhysicalDeviceVulkan11Properties.deviceUUID=");  for (int i_ = 0; i_ < VK_UUID_SIZE; i_++) printf("%llu ", (unsigned long long)p11.deviceUUID[i_]); printf("\n");
   printf("VkPhysicalDeviceVulkan11Properties.driverUUID=");  for (int i_ = 0; i_ < VK_UUID_SIZE; i_++) printf("%llu ", (unsigned long long)p11.driverUUID[i_]); printf("\n");
   printf("VkPhysicalDeviceVulkan11Properties.deviceLUID=");  for (int i_ = 0; i_ < VK_LUID_SIZE; i_++) printf("%llu ", (unsigned long long)p11.deviceLUID[i_]); printf("\n");
   printf("VkPhysicalDeviceVulkan11Properties.deviceNodeMask=%llu\n", (unsigned long long)p11.deviceNodeMask);
   printf("VkPhysicalDeviceVulkan11Properties.deviceLUIDValid=%llu\n", (unsigned long long)p11.deviceLUIDValid);
   printf("VkPhysicalDeviceVulkan11Properties.subgroupSize=%llu\n", (unsigned long long)p11.subgroupSize);
   printf("VkPhysicalDeviceVulkan11Properties.subgroupSupportedStages=%llu\n", (unsigned long long)p11.subgroupSupportedStages);
   printf("VkPhysicalDeviceVulkan11Properties.subgroupSupportedOperations=%llu\n", (unsigned long long)p11.subgroupSupportedOperations);
   printf("VkPhysicalDeviceVulkan11Properties.subgroupQuadOperationsInAllStages=%llu\n", (unsigned long long)p11.subgroupQuadOperationsInAllStages);
   printf("VkPhysicalDeviceVulkan11Properties.pointClippingBehavior=%d\n", (int)p11.pointClippingBehavior);
   printf("VkPhysicalDeviceVulkan11Properties.maxMultiviewViewCount=%llu\n", (unsigned long long)p11.maxMultiviewViewCount);
   printf("VkPhysicalDeviceVulkan11Properties.maxMultiviewInstanceIndex=%llu\n", (unsigned long long)p11.maxMultiviewInstanceIndex);
   printf("VkPhysicalDeviceVulkan11Properties.protectedNoFault=%llu\n", (unsigned long long)p11.protectedNoFault);
   printf("VkPhysicalDeviceVulkan11Properties.maxPerSetDescriptors=%llu\n", (unsigned long long)p11.maxPerSetDescriptors);
   printf("VkPhysicalDeviceVulkan11Properties.maxMemoryAllocationSize=%llu\n", (unsigned long long)p11.maxMemoryAllocationSize);

   printf("VkPhysicalDeviceVulkan12Properties.driverID=%d\n", (int)p12.driverID);
   printf("VkPhysicalDeviceVulkan12Properties.driverName=%s\n", p12.driverName);
   printf("VkPhysicalDeviceVulkan12Properties.driverInfo=%s\n", p12.driverInfo);
   printf("VkPhysicalDeviceVulkan12Properties.conformanceVersion=%u.%u.%u.%u\n", p12.conformanceVersion.major, p12.conformanceVersion.minor, p12.conformanceVersion.subminor, p12.conformanceVersion.patch);
   printf("VkPhysicalDeviceVulkan12Properties.denormBehaviorIndependence=%d\n", (int)p12.denormBehaviorIndependence);
   printf("VkPhysicalDeviceVulkan12Properties.roundingModeIndependence=%d\n", (int)p12.roundingModeIndependence);
   printf("VkPhysicalDeviceVulkan12Properties.shaderSignedZeroInfNanPreserveFloat16=%llu\n", (unsigned long long)p12.shaderSignedZeroInfNanPreserveFloat16);
   printf("VkPhysicalDeviceVulkan12Properties.shaderSignedZeroInfNanPreserveFloat32=%llu\n", (unsigned long long)p12.shaderSignedZeroInfNanPreserveFloat32);
   printf("VkPhysicalDeviceVulkan12Properties.shaderSignedZeroInfNanPreserveFloat64=%llu\n", (unsigned long long)p12.shaderSignedZeroInfNanPreserveFloat64);
   printf("VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat16=%llu\n", (unsigned long long)p12.shaderDenormPreserveFloat16);
   printf("VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat32=%llu\n", (unsigned long long)p12.shaderDenormPreserveFloat32);
   printf("VkPhysicalDeviceVulkan12Properties.shaderDenormPreserveFloat64=%llu\n", (unsigned long long)p12.shaderDenormPreserveFloat64);
   printf("VkPhysicalDeviceVulkan12Properties.shaderDenormFlushToZeroFloat16=%llu\n", (unsigned long long)p12.shaderDenormFlushToZeroFloat16);
   printf("VkPhysicalDeviceVulkan12Properties.shaderDenormFlushToZeroFloat32=%llu\n", (unsigned long long)p12.shaderDenormFlushToZeroFloat32);
   printf("VkPhysicalDeviceVulkan12Properties.shaderDenormFlushToZeroFloat64=%llu\n", (unsigned long long)p12.shaderDenormFlushToZeroFloat64);
   printf("VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTEFloat16=%llu\n", (unsigned long long)p12.shaderRoundingModeRTEFloat16);
   printf("VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTEFloat32=%llu\n", (unsigned long long)p12.shaderRoundingModeRTEFloat32);
   printf("VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTEFloat64=%llu\n", (unsigned long long)p12.shaderRoundingModeRTEFloat64);
   printf("VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTZFloat16=%llu\n", (unsigned long long)p12.shaderRoundingModeRTZFloat16);
   printf("VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTZFloat32=%llu\n", (unsigned long long)p12.shaderRoundingModeRTZFloat32);
   printf("VkPhysicalDeviceVulkan12Properties.shaderRoundingModeRTZFloat64=%llu\n", (unsigned long long)p12.shaderRoundingModeRTZFloat64);
   printf("VkPhysicalDeviceVulkan12Properties.maxUpdateAfterBindDescriptorsInAllPools=%llu\n", (unsigned long long)p12.maxUpdateAfterBindDescriptorsInAllPools);
   printf("VkPhysicalDeviceVulkan12Properties.shaderUniformBufferArrayNonUniformIndexingNative=%llu\n", (unsigned long long)p12.shaderUniformBufferArrayNonUniformIndexingNative);
   printf("VkPhysicalDeviceVulkan12Properties.shaderSampledImageArrayNonUniformIndexingNative=%llu\n", (unsigned long long)p12.shaderSampledImageArrayNonUniformIndexingNative);
   printf("VkPhysicalDeviceVulkan12Properties.shaderStorageBufferArrayNonUniformIndexingNative=%llu\n", (unsigned long long)p12.shaderStorageBufferArrayNonUniformIndexingNative);
   printf("VkPhysicalDeviceVulkan12Properties.shaderStorageImageArrayNonUniformIndexingNative=%llu\n", (unsigned long long)p12.shaderStorageImageArrayNonUniformIndexingNative);
   printf("VkPhysicalDeviceVulkan12Properties.shaderInputAttachmentArrayNonUniformIndexingNative=%llu\n", (unsigned long long)p12.shaderInputAttachmentArrayNonUniformIndexingNative);
   printf("VkPhysicalDeviceVulkan12Properties.robustBufferAccessUpdateAfterBind=%llu\n", (unsigned long long)p12.robustBufferAccessUpdateAfterBind);
   printf("VkPhysicalDeviceVulkan12Properties.quadDivergentImplicitLod=%llu\n", (unsigned long long)p12.quadDivergentImplicitLod);
   printf("VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindSamplers=%llu\n", (unsigned long long)p12.maxPerStageDescriptorUpdateAfterBindSamplers);
   printf("VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindUniformBuffers=%llu\n", (unsigned long long)p12.maxPerStageDescriptorUpdateAfterBindUniformBuffers);
   printf("VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindStorageBuffers=%llu\n", (unsigned long long)p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers);
   printf("VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindSampledImages=%llu\n", (unsigned long long)p12.maxPerStageDescriptorUpdateAfterBindSampledImages);
   printf("VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindStorageImages=%llu\n", (unsigned long long)p12.maxPerStageDescriptorUpdateAfterBindStorageImages);
   printf("VkPhysicalDeviceVulkan12Properties.maxPerStageDescriptorUpdateAfterBindInputAttachments=%llu\n", (unsigned long long)p12.maxPerStageDescriptorUpdateAfterBindInputAttachments);
   printf("VkPhysicalDeviceVulkan12Properties.maxPerStageUpdateAfterBindResources=%llu\n", (unsigned long long)p12.maxPerStageUpdateAfterBindResources);
   printf("VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindSamplers=%llu\n", (unsigned long long)p12.maxDescriptorSetUpdateAfterBindSamplers);
   printf("VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindUniformBuffers=%llu\n", (unsigned long long)p12.maxDescriptorSetUpdateAfterBindUniformBuffers);
   printf("VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic=%llu\n", (unsigned long long)p12.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic);
   printf("VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindStorageBuffers=%llu\n", (unsigned long long)p12.maxDescriptorSetUpdateAfterBindStorageBuffers);
   printf("VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic=%llu\n", (unsigned long long)p12.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic);
   printf("VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindSampledImages=%llu\n", (unsigned long long)p12.maxDescriptorSetUpdateAfterBindSampledImages);
   printf("VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindStorageImages=%llu\n", (unsigned long long)p12.maxDescriptorSetUpdateAfterBindStorageImages);
   printf("VkPhysicalDeviceVulkan12Properties.maxDescriptorSetUpdateAfterBindInputAttachments=%llu\n", (unsigned long long)p12.maxDescriptorSetUpdateAfterBindInputAttachments);
   printf("VkPhysicalDeviceVulkan12Properties.supportedDepthResolveModes=%llu\n", (unsigned long long)p12.supportedDepthResolveModes);
   printf("VkPhysicalDeviceVulkan12Properties.supportedStencilResolveModes=%llu\n", (unsigned long long)p12.supportedStencilResolveModes);
   printf("VkPhysicalDeviceVulkan12Properties.independentResolveNone=%llu\n", (unsigned long long)p12.independentResolveNone);
   printf("VkPhysicalDeviceVulkan12Properties.independentResolve=%llu\n", (unsigned long long)p12.independentResolve);
   printf("VkPhysicalDeviceVulkan12Properties.filterMinmaxSingleComponentFormats=%llu\n", (unsigned long long)p12.filterMinmaxSingleComponentFormats);
   printf("VkPhysicalDeviceVulkan12Properties.filterMinmaxImageComponentMapping=%llu\n", (unsigned long long)p12.filterMinmaxImageComponentMapping);
   printf("VkPhysicalDeviceVulkan12Properties.maxTimelineSemaphoreValueDifference=%llu\n", (unsigned long long)p12.maxTimelineSemaphoreValueDifference);
   printf("VkPhysicalDeviceVulkan12Properties.framebufferIntegerColorSampleCounts=%llu\n", (unsigned long long)p12.framebufferIntegerColorSampleCounts);

   printf("VkPhysicalDeviceVulkan13Properties.minSubgroupSize=%llu\n", (unsigned long long)p13.minSubgroupSize);
   printf("VkPhysicalDeviceVulkan13Properties.maxSubgroupSize=%llu\n", (unsigned long long)p13.maxSubgroupSize);
   printf("VkPhysicalDeviceVulkan13Properties.maxComputeWorkgroupSubgroups=%llu\n", (unsigned long long)p13.maxComputeWorkgroupSubgroups);
   printf("VkPhysicalDeviceVulkan13Properties.requiredSubgroupSizeStages=%llu\n", (unsigned long long)p13.requiredSubgroupSizeStages);
   printf("VkPhysicalDeviceVulkan13Properties.maxInlineUniformBlockSize=%llu\n", (unsigned long long)p13.maxInlineUniformBlockSize);
   printf("VkPhysicalDeviceVulkan13Properties.maxPerStageDescriptorInlineUniformBlocks=%llu\n", (unsigned long long)p13.maxPerStageDescriptorInlineUniformBlocks);
   printf("VkPhysicalDeviceVulkan13Properties.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks=%llu\n", (unsigned long long)p13.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks);
   printf("VkPhysicalDeviceVulkan13Properties.maxDescriptorSetInlineUniformBlocks=%llu\n", (unsigned long long)p13.maxDescriptorSetInlineUniformBlocks);
   printf("VkPhysicalDeviceVulkan13Properties.maxDescriptorSetUpdateAfterBindInlineUniformBlocks=%llu\n", (unsigned long long)p13.maxDescriptorSetUpdateAfterBindInlineUniformBlocks);
   printf("VkPhysicalDeviceVulkan13Properties.maxInlineUniformTotalSize=%llu\n", (unsigned long long)p13.maxInlineUniformTotalSize);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct8BitUnsignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct8BitUnsignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct8BitSignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct8BitSignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct8BitMixedSignednessAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct8BitMixedSignednessAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct4x8BitPackedUnsignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct4x8BitPackedUnsignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct4x8BitPackedSignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct4x8BitPackedSignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct4x8BitPackedMixedSignednessAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct4x8BitPackedMixedSignednessAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct16BitUnsignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct16BitUnsignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct16BitSignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct16BitSignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct16BitMixedSignednessAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct16BitMixedSignednessAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct32BitUnsignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct32BitUnsignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct32BitSignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct32BitSignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct32BitMixedSignednessAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct32BitMixedSignednessAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct64BitUnsignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct64BitUnsignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct64BitSignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct64BitSignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProduct64BitMixedSignednessAccelerated=%llu\n", (unsigned long long)p13.integerDotProduct64BitMixedSignednessAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating8BitUnsignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating8BitUnsignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating8BitSignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating8BitSignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating16BitUnsignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating16BitUnsignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating16BitSignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating16BitSignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating32BitUnsignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating32BitUnsignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating32BitSignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating32BitSignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating64BitUnsignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating64BitUnsignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating64BitSignedAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating64BitSignedAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated=%llu\n", (unsigned long long)p13.integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated);
   printf("VkPhysicalDeviceVulkan13Properties.storageTexelBufferOffsetAlignmentBytes=%llu\n", (unsigned long long)p13.storageTexelBufferOffsetAlignmentBytes);
   printf("VkPhysicalDeviceVulkan13Properties.storageTexelBufferOffsetSingleTexelAlignment=%llu\n", (unsigned long long)p13.storageTexelBufferOffsetSingleTexelAlignment);
   printf("VkPhysicalDeviceVulkan13Properties.uniformTexelBufferOffsetAlignmentBytes=%llu\n", (unsigned long long)p13.uniformTexelBufferOffsetAlignmentBytes);
   printf("VkPhysicalDeviceVulkan13Properties.uniformTexelBufferOffsetSingleTexelAlignment=%llu\n", (unsigned long long)p13.uniformTexelBufferOffsetSingleTexelAlignment);
   printf("VkPhysicalDeviceVulkan13Properties.maxBufferSize=%llu\n", (unsigned long long)p13.maxBufferSize);

   uint32_t ext_count = 0;
   CHECK(vkEnumerateDeviceExtensionProperties(pd, NULL, &ext_count, NULL));
   VkExtensionProperties *exts = calloc(ext_count, sizeof(*exts));
   CHECK(vkEnumerateDeviceExtensionProperties(pd, NULL, &ext_count, exts));
   printf("deviceExtensions=%u\n", ext_count);
   for (uint32_t i = 0; i < ext_count; i++)
      printf("ext %s %u\n", exts[i].extensionName, exts[i].specVersion);

   uint32_t qf_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, NULL);
   VkQueueFamilyProperties qfs[16];
   if (qf_count > 16)
      qf_count = 16;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs);
   for (uint32_t i = 0; i < qf_count; i++)
      printf("queueFamily %u flags=0x%x count=%u timestampValidBits=%u minImageTransferGranularity=%ux%ux%u\n",
             i, qfs[i].queueFlags, qfs[i].queueCount, qfs[i].timestampValidBits,
             qfs[i].minImageTransferGranularity.width,
             qfs[i].minImageTransferGranularity.height,
             qfs[i].minImageTransferGranularity.depth);

   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
      printf("memoryHeap %u size=%llu flags=0x%x\n", i,
             (unsigned long long)mp.memoryHeaps[i].size, mp.memoryHeaps[i].flags);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      printf("memoryType %u heap=%u flags=0x%x\n", i, mp.memoryTypes[i].heapIndex,
             mp.memoryTypes[i].propertyFlags);

   for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
      VkFormatProperties3 fp3 = {
         .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
      };
      VkFormatProperties2 fp = {
         .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
         .pNext = &fp3,
      };
      vkGetPhysicalDeviceFormatProperties2(pd, formats[i].fmt, &fp);
      printf("format %s linear=0x%x optimal=0x%x buffer=0x%x linear3=0x%llx optimal3=0x%llx buffer3=0x%llx\n",
             formats[i].name, fp.formatProperties.linearTilingFeatures,
             fp.formatProperties.optimalTilingFeatures,
             fp.formatProperties.bufferFeatures,
             (unsigned long long)fp3.linearTilingFeatures,
             (unsigned long long)fp3.optimalTilingFeatures,
             (unsigned long long)fp3.bufferFeatures);
   }
   printf("done\n");
   return 0;
}
