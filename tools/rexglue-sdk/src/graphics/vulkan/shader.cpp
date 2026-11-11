/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>
#include <cstdio>

#include <rex/assert.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/provider.h>

namespace rex::graphics::vulkan {

int32_t g_diag_ps_const_hash = 0;  // set by VulkanCommandProcessor from a cvar

VulkanShader::VulkanTranslation::~VulkanTranslation() {
  if (shader_module_) {
    const ui::vulkan::VulkanDevice* const vulkan_device =
        static_cast<const VulkanShader&>(shader()).vulkan_device_;
    vulkan_device->functions().vkDestroyShaderModule(vulkan_device->device(), shader_module_,
                                                     nullptr);
  }
}

VkShaderModule VulkanShader::VulkanTranslation::GetOrCreateShaderModule() {
  if (!is_valid()) {
    return VK_NULL_HANDLE;
  }
  if (shader_module_ != VK_NULL_HANDLE) {
    return shader_module_;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device =
      static_cast<const VulkanShader&>(shader()).vulkan_device_;
  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = nullptr;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = translated_binary().size();
  shader_module_create_info.pCode = reinterpret_cast<const uint32_t*>(translated_binary().data());
  // Diagnostic (diag_ps_const_hash): replace the pixel shader with these low
  // 32 hash bits by a minimal one writing constant magenta with early tests.
  static const uint32_t kConstPs[] = {
      0x07230203, 0x00010000, 0, 12, 0,
      0x00020011, 1,                                      // OpCapability Shader
      0x0003000E, 0, 1,                                   // OpMemoryModel Logical GLSL450
      0x0006000F, 4, 1, 0x6E69616D, 0, 2,                 // OpEntryPoint Fragment %1 "main" %2
      0x00030010, 1, 7,                                   // OriginUpperLeft
      0x00030010, 1, 9,                                   // EarlyFragmentTests
      0x00040047, 2, 30, 0,                               // %2 Location 0
      0x00020013, 3,                                      // %3 void
      0x00030021, 4, 3,                                   // %4 fn void
      0x00030016, 5, 32,                                  // %5 float
      0x00040017, 6, 5, 4,                                // %6 vec4
      0x00040020, 7, 3, 6,                                // %7 ptr Output vec4
      0x0004003B, 7, 2, 3,                                // %2 out
      0x0004002B, 5, 8, 0x3F800000,                       // %8 1.0
      0x0004002B, 5, 9, 0,                                // %9 0.0
      0x0007002C, 6, 10, 8, 9, 8, 8,                      // %10 (1,0,1,1)
      0x00050036, 3, 1, 0, 4,                             // %1 main
      0x000200F8, 11,                                     // label
      0x0003003E, 2, 10,                                  // store
      0x000100FD, 0x00010038};                            // return, end
  if (g_diag_ps_const_hash != 0 && shader().type() == xenos::ShaderType::kPixel &&
      int32_t(uint32_t(shader().ucode_data_hash())) == g_diag_ps_const_hash) {
    shader_module_create_info.codeSize = sizeof(kConstPs);
    shader_module_create_info.pCode = kConstPs;
    printf("[psconst] ps%016llX mod%016llX replaced\n",
           (unsigned long long)shader().ucode_data_hash(), (unsigned long long)modification());
  }
  if (vulkan_device->functions().vkCreateShaderModule(vulkan_device->device(),
                                                      &shader_module_create_info, nullptr,
                                                      &shader_module_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanShader::VulkanTranslation: Failed to create a Vulkan shader "
        "module for shader {:016X} modification {:016X}",
        shader().ucode_data_hash(), modification());
    MakeInvalid();
    return VK_NULL_HANDLE;
  }
  return shader_module_;
}

VulkanShader::VulkanShader(const ui::vulkan::VulkanDevice* const vulkan_device,
                           const xenos::ShaderType shader_type, const uint64_t ucode_data_hash,
                           const uint32_t* const ucode_dwords, const size_t ucode_dword_count,
                           const std::endian ucode_source_endian)
    : SpirvShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian),
      vulkan_device_(vulkan_device) {
  assert_not_null(vulkan_device);
}

Shader::Translation* VulkanShader::CreateTranslationInstance(uint64_t modification) {
  return new VulkanTranslation(*this, modification);
}

}  // namespace rex::graphics::vulkan
