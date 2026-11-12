#pragma once
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

#include <array>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/hash.h>
#include <rex/ui/vulkan/mem_alloc.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

class VulkanTextureCache final : public TextureCache {
 public:
  // texture_cpu_copy_worker_core: waits for the queued CPU texture copies and flushes them. Must
  // be called before submitting commands that may read those images, and before destroying one.
  void FinishCpuCopies() const;
  void StopCpuCopyWorker();

  // Sampler parameters that can be directly converted to a host sampler or used
  // for checking whether samplers bindings are up to date.
  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;         // 3
      xenos::ClampMode clamp_y : 3;         // 6
      xenos::ClampMode clamp_z : 3;         // 9
      xenos::BorderColor border_color : 2;  // 11
      uint32_t mag_linear : 1;              // 12
      uint32_t min_linear : 1;              // 13
      uint32_t mip_linear : 1;              // 14
      xenos::AnisoFilter aniso_filter : 3;  // 17
      uint32_t mip_min_level : 4;           // 21
      uint32_t mip_base_map : 1;            // 22
      // Maximum mip level is in the texture resource itself, but mip_base_map
      // can be used to limit fetching to mip_min_level.
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    struct Hasher {
      size_t operator()(const SamplerParameters& parameters) const {
        return std::hash<uint32_t>{}(parameters.value);
      }
    };
    bool operator==(const SamplerParameters& parameters) const { return value == parameters.value; }
    bool operator!=(const SamplerParameters& parameters) const { return value != parameters.value; }
  };

  // Transient descriptor set layouts must be initialized in the command
  // processor.
  static std::unique_ptr<VulkanTextureCache> Create(
      const RegisterFile& register_file, VulkanSharedMemory& shared_memory,
      uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
      VulkanCommandProcessor& command_processor,
      VkPipelineStageFlags guest_shader_pipeline_stages) {
    std::unique_ptr<VulkanTextureCache> texture_cache(new VulkanTextureCache(
        register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y,
        command_processor, guest_shader_pipeline_stages));
    if (!texture_cache->Initialize()) {
      return nullptr;
    }
    return std::move(texture_cache);
  }

  ~VulkanTextureCache();

  void BeginSubmission(uint64_t new_submission_index) override;
  void BeginFrame() override;
  void EndFrame();

  // Must be called within a frame - creates and untiles textures needed by
  // shaders, and enqueues transitioning them into the sampled usage. This may
  // bind compute pipelines (notifying the command processor about that), and
  // also since it may insert deferred barriers, before flushing the barriers
  // preceding host GPU work.
  void RequestTextures(uint32_t used_texture_mask) override;

  VkImageView GetActiveBindingOrNullImageView(uint32_t fetch_constant_index,
                                              xenos::FetchOpDimension dimension, bool is_signed);

  SamplerParameters GetSamplerParameters(const VulkanShader::SamplerBinding& binding) const;

  // Must be called for every used sampler at least once in a single submission,
  // and a submission must be open for this to be callable.
  // Returns:
  // - The sampler, if obtained successfully - and increases its last usage
  //   submission index - and has_overflown_out = false.
  // - VK_NULL_HANDLE and has_overflown_out = true if there's a total sampler
  //   count overflow in a submission that potentially hasn't completed yet.
  // - VK_NULL_HANDLE and has_overflown_out = false in case of a general failure
  //   to create a sampler.
  VkSampler UseSampler(SamplerParameters parameters, bool& has_overflown_out);
  // Returns the submission index to await (may be the current submission in
  // case of an overflow within a single submission - in this case, it must be
  // ended, and a new one must be started) in case of sampler count overflow, so
  // samplers may be freed, and UseSamplers may take their slots.
  uint64_t GetSubmissionToAwaitOnSamplerOverflow(uint32_t overflowed_sampler_count) const;

  // Returns the 2D view of the front buffer texture (for fragment shader
  // reading - the barrier will be pushed in the command processor if needed),
  // or VK_NULL_HANDLE in case of failure. May call LoadTextureData.
  // If swap_source_needs_rb_swap_out is not nullptr, writes whether the final
  // guest-to-host swizzle requires swapping red and blue (R <- B, B <- R) with
  // green preserved, which is needed by the presentation fallback path on
  // devices without imageViewFormatSwizzle.
  VkImageView RequestSwapTexture(uint32_t& width_scaled_out, uint32_t& height_scaled_out,
                                 xenos::TextureFormat& format_out,
                                 uint32_t* width_unscaled_out = nullptr,
                                 uint32_t* height_unscaled_out = nullptr,
                                 bool* swap_source_needs_rb_swap_out = nullptr);

  bool GetScaledResolveRange(uint32_t start_unscaled, uint32_t length_unscaled,
                             uint32_t length_scaled_alignment_log2, uint64_t& start_scaled_out,
                             uint64_t& length_scaled_out) const;
  bool CommitScaledResolveRange(uint32_t start_unscaled, uint32_t length_unscaled,
                                uint32_t length_scaled_alignment_log2 = 0) {
    return EnsureScaledResolveMemoryCommitted(start_unscaled, length_unscaled,
                                              length_scaled_alignment_log2);
  }
  VkBuffer scaled_resolve_buffer() const { return scaled_resolve_buffer_; }
  void UseScaledResolveBufferForRead();
  void UseScaledResolveBufferForWrite(uint64_t written_start_scaled,
                                      uint64_t written_length_scaled);

  // resolve_copy_to_texture: copies a resolved rectangle straight from the host render target
  // (already in TRANSFER_SRC_OPTIMAL) into the existing unscaled 2D RGBA8 textures based at
  // dest_base, so they don't have to be reloaded from guest memory. Call before
  // MarkRangeAsResolved, then FinishResolveCopyToTextures after it. Returns how many were copied.
  uint32_t ResolveCopyToTextures(VkImage src_image, int32_t src_x, int32_t src_y, uint32_t width,
                                 uint32_t height, uint32_t dest_base, uint32_t dest_pitch_div_32,
                                 uint32_t dest_x, uint32_t dest_y, uint32_t dest_endian,
                                 bool swap_red_blue = false);
  void FinishResolveCopyToTextures();

  // resolve_texture_store_pages (kof2002um): for the listed texture pages (hex, comma separated) that
  // only the GPU writes, ResolveCopyToTextures copies the render target into an image the store owns
  // (no red/blue swap blit: the view swaps the channels) and bindings sample that image. Returns
  // VK_NULL_HANDLE when the store does not cover the texture.
  VkImageView ResolveStoreView(const TextureKey& key, uint32_t host_swizzle);
  bool ResolveStoreCovers(const TextureKey& key);

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;

  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;

  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;

  bool EnsureScaledResolveMemoryCommitted(uint32_t start_unscaled, uint32_t length_unscaled,
                                          uint32_t length_scaled_alignment_log2 = 0) override;

  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;
  // texture_load_cpu_direct: fills a VulkanTexture with cpu_direct() from guest memory on the CPU.
  // False if this load can't be done that way (the caller uses the GPU path).
  bool LoadTextureCpuDirect(Texture& texture, LoadShaderIndex load_shader);
  bool LoadsBaseWithoutSharedMemory(const Texture& texture) const override;
  bool SupportsPartialBaseLoad() const override { return true; }

  void UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) override;

 private:
  enum LoadDescriptorSetIndex {
    kLoadDescriptorSetIndexDestination,
    kLoadDescriptorSetIndexSource,
    kLoadDescriptorSetCount,
  };

  struct HostFormat {
    LoadShaderIndex load_shader;
    // Do NOT add integer formats to this - they are not filterable, can only be
    // read with ImageFetch, not ImageSample! If any game is seen using
    // num_format 1 for fixed-point formats (for floating-point, it's normally
    // set to 1 though), add a constant buffer containing multipliers for the
    // textures and multiplication to the tfetch implementation.
    VkFormat format;
    // Whether the format is block-compressed on the host (the host block size
    // matches the guest format block size in this case), and isn't decompressed
    // on load.
    bool block_compressed;

    // Set up dynamically based on what's supported by the device.
    bool linear_filterable;
  };

  struct HostFormatPair {
    HostFormat format_unsigned;
    HostFormat format_signed;
    // Mapping of Xenos swizzle components to Vulkan format components.
    uint32_t swizzle;
    // Whether the unsigned and the signed formats are compatible for one image
    // and the same image data (on a portability subset device, this should also
    // take imageViewFormatReinterpretation into account).
    bool unsigned_signed_compatible;
  };

  class VulkanTexture final : public Texture {
   public:
    enum class Usage {
      kUndefined,
      kTransferDestination,
      kGuestShaderSampled,
      kSwapSampled,
    };

    // Takes ownership of the image and its memory.
    explicit VulkanTexture(VulkanTextureCache& texture_cache, const TextureKey& key, VkImage image,
                           VmaAllocation allocation, bool track_usage = true);
    ~VulkanTexture();

    VkImage image() const { return image_; }

    // texture_load_linear_direct: LINEAR image whose memory is also bound to a storage buffer, so
    // the load shader writes the texels in place (no buffer -> image copy).
    void SetLinearDirect(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize row_pitch,
                         VkDeviceSize size) {
      linear_buffer_ = buffer;
      linear_offset_ = offset;
      linear_row_pitch_ = row_pitch;
      linear_size_ = size;
    }
    VkBuffer linear_buffer() const { return linear_buffer_; }
    VkDeviceSize linear_offset() const { return linear_offset_; }
    VkDeviceSize linear_row_pitch() const { return linear_row_pitch_; }
    VkDeviceSize linear_size() const { return linear_size_; }
    Usage usage() const { return usage_; }

    // texture_load_cpu_direct: host-mapped LINEAR image that the CPU fills straight from guest
    // memory. Every reload after the first goes into another version of the image, one no draw
    // recorded or in flight still reads, so the GPU never waits for or runs the load.
    void SetCpuDirect(void* mapped, VkDeviceSize offset, VkDeviceSize row_pitch,
                      const VkImageCreateInfo& create_info, const VkFormat formats[2]) {
      cpu_mapped_ = mapped;
      cpu_offset_ = offset;
      cpu_row_pitch_ = row_pitch;
      cpu_create_info_ = create_info;
      cpu_create_info_.pNext = nullptr;
      cpu_formats_[0] = formats[0];
      cpu_formats_[1] = formats[1];
    }
    bool cpu_direct() const { return cpu_mapped_ != nullptr; }
    // Last submission whose draws bind the current version (set when the binding's view is taken):
    // once it's completed and not the open one, the next load may write the version in place.
    uint64_t cpu_last_use_submission = 0;
    uint8_t* cpu_mapped() const { return static_cast<uint8_t*>(cpu_mapped_); }
    VkDeviceSize cpu_offset() const { return cpu_offset_; }
    VkDeviceSize cpu_row_pitch() const { return cpu_row_pitch_; }
    VmaAllocation allocation() const { return allocation_; }
    // Makes a version that no submission after `completed_submission` uses current (creating one
    // if needed); the old one is retired at `current_submission`. False if none can be had.
    bool SwitchToFreeCpuVersion(uint64_t completed_submission, uint64_t current_submission,
                                bool& created);

    // Doesn't transition (the caller must insert the barrier).
    Usage SetUsage(Usage new_usage) {
      Usage old_usage = usage_;
      usage_ = new_usage;
      return old_usage;
    }

    VkImageView GetView(bool is_signed, uint32_t host_swizzle, bool is_array = true);
    VkImageView GetOrCreate3DAs2DImageView(bool is_signed, uint32_t host_swizzle);

   private:
    union ViewKey {
      uint32_t key;
      struct {
        uint32_t is_signed_separate_view : 1;
        uint32_t host_swizzle : 12;
        uint32_t is_array : 1;
      };

      ViewKey() : key(0) { static_assert_size(*this, sizeof(key)); }

      struct Hasher {
        size_t operator()(const ViewKey& key) const {
          return std::hash<decltype(key.key)>{}(key.key);
        }
      };
      bool operator==(const ViewKey& other_key) const { return key == other_key.key; }
      bool operator!=(const ViewKey& other_key) const { return !(*this == other_key); }
    };

    static constexpr VkComponentSwizzle GetComponentSwizzle(uint32_t texture_swizzle,
                                                            uint32_t component_index) {
      xenos::XE_GPU_TEXTURE_SWIZZLE texture_component_swizzle =
          xenos::XE_GPU_TEXTURE_SWIZZLE((texture_swizzle >> (3 * component_index)) & 0b111);
      if (texture_component_swizzle == xenos::XE_GPU_TEXTURE_SWIZZLE(component_index)) {
        // The portability subset requires all swizzles to be IDENTITY, return
        // IDENTITY specifically, not R, G, B, A.
        return VK_COMPONENT_SWIZZLE_IDENTITY;
      }
      switch (texture_component_swizzle) {
        case xenos::XE_GPU_TEXTURE_SWIZZLE_R:
          return VK_COMPONENT_SWIZZLE_R;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_G:
          return VK_COMPONENT_SWIZZLE_G;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_B:
          return VK_COMPONENT_SWIZZLE_B;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_A:
          return VK_COMPONENT_SWIZZLE_A;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_0:
          return VK_COMPONENT_SWIZZLE_ZERO;
        case xenos::XE_GPU_TEXTURE_SWIZZLE_1:
          return VK_COMPONENT_SWIZZLE_ONE;
        default:
          // An invalid value.
          return VK_COMPONENT_SWIZZLE_IDENTITY;
      }
    }

    VkImage image_;
    VmaAllocation allocation_;

    Usage usage_ = Usage::kUndefined;

    VkBuffer linear_buffer_ = VK_NULL_HANDLE;
    VkDeviceSize linear_offset_ = 0;
    VkDeviceSize linear_row_pitch_ = 0;
    VkDeviceSize linear_size_ = 0;

    std::unordered_map<ViewKey, VkImageView, ViewKey::Hasher> views_;
    std::unique_ptr<VulkanTexture> texture_3d_as_2d_;
    VkImageView image_view_3d_as_2d_unsigned_ = VK_NULL_HANDLE;
    VkImageView image_view_3d_as_2d_signed_ = VK_NULL_HANDLE;

    struct CpuVersion {
      VkImage image;
      VmaAllocation allocation;
      void* mapped;
      std::unordered_map<ViewKey, VkImageView, ViewKey::Hasher> views;
      uint64_t retired_submission;
      uint64_t gen = 0;  // cpu_gen of the load its data reflects (0 = none)
    };
   public:
    // Load generation of the current version's data, and the rows each recent load wrote (by
    // generation modulo kCpuHistory): a reused version only needs the rows written since its own.
    static constexpr uint32_t kCpuHistory = 128;
    struct CpuLoadRows {
      uint64_t gen = 0;
      uint32_t first = 0, end = 0;
    };
    uint64_t cpu_gen = 0;
    CpuLoadRows cpu_history[kCpuHistory];
    uint64_t cpu_switched_from_gen = 0;  // set by SwitchToFreeCpuVersion: the new current's gen
   private:
    void* cpu_mapped_ = nullptr;
    VkDeviceSize cpu_offset_ = 0;
    VkDeviceSize cpu_row_pitch_ = 0;
    VkImageCreateInfo cpu_create_info_{};
    VkFormat cpu_formats_[2] = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    uint64_t cpu_versions_bytes_ = 0;
    std::vector<CpuVersion> cpu_versions_;
  };

  struct VulkanTextureBinding {
    VkImageView image_view_unsigned;
    VkImageView image_view_signed;

    VulkanTextureBinding() { Reset(); }

    void Reset() {
      image_view_unsigned = VK_NULL_HANDLE;
      image_view_signed = VK_NULL_HANDLE;
    }
  };

  struct Sampler {
    VkSampler sampler;
    bool uses_custom_border_color;
    uint64_t last_usage_submission;
    std::pair<const SamplerParameters, Sampler>* used_previous;
    std::pair<const SamplerParameters, Sampler>* used_next;
  };

  static constexpr bool AreDimensionsCompatible(xenos::FetchOpDimension binding_dimension,
                                                xenos::DataDimension resource_dimension) {
    switch (binding_dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return resource_dimension == xenos::DataDimension::k1D ||
               resource_dimension == xenos::DataDimension::k2DOrStacked ||
               resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::k3DOrStacked:
        return resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return resource_dimension == xenos::DataDimension::kCube;
      default:
        return false;
    }
  }

  explicit VulkanTextureCache(const RegisterFile& register_file, VulkanSharedMemory& shared_memory,
                              uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
                              VulkanCommandProcessor& command_processor,
                              VkPipelineStageFlags guest_shader_pipeline_stages);

  bool Initialize();
  bool InitializeScaledResolveBuffer();
  void ShutdownScaledResolveBuffer();

  const HostFormatPair& GetHostFormatPair(TextureKey key) const;

  void GetTextureUsageMasks(VulkanTexture::Usage usage, VkPipelineStageFlags& stage_mask,
                            VkAccessFlags& access_mask, VkImageLayout& layout);
  bool EnsureScaledResolveBufferAllocated(uint64_t start_scaled, uint64_t length_scaled);
  void GetScaledResolveUsageMasks(VkPipelineStageFlags& stage_mask_out,
                                  VkAccessFlags& access_mask_out, bool write) const;

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  VulkanCommandProcessor& command_processor_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  // Using the Vulkan Memory Allocator because texture count in games is
  // naturally pretty much unbounded, while Vulkan implementations, especially
  // on Windows versions before 10, may have an allocation count limit as low as
  // 4096.
  VmaAllocator vma_allocator_ = VK_NULL_HANDLE;

  static const HostFormatPair kBestHostFormats[64];
  static const HostFormatPair kHostFormatGBGRUnaligned;
  static const HostFormatPair kHostFormatBGRGUnaligned;
  static const HostFormatPair kHostFormatDXT1Unaligned;
  static const HostFormatPair kHostFormatDXT2_3Unaligned;
  static const HostFormatPair kHostFormatDXT4_5Unaligned;
  static const HostFormatPair kHostFormatDXNUnaligned;
  static const HostFormatPair kHostFormatDXT5AUnaligned;
  HostFormatPair host_formats_[64];

  VkPipelineLayout load_pipeline_layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_{};
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_scaled_{};

  // If both images can be placed in the same allocation, it's one allocation,
  // otherwise it's two separate.
  std::array<VkDeviceMemory, 2> null_images_memory_{};
  VkImage null_image_2d_array_cube_ = VK_NULL_HANDLE;
  VkImage null_image_3d_ = VK_NULL_HANDLE;
  VkImageView null_image_view_2d_array_ = VK_NULL_HANDLE;
  VkImageView null_image_view_cube_ = VK_NULL_HANDLE;
  VkImageView null_image_view_3d_ = VK_NULL_HANDLE;
  bool null_images_cleared_ = false;

  std::array<VulkanTextureBinding, xenos::kTextureFetchConstantCount> vulkan_texture_bindings_;

  // Unsupported texture formats used during this frame (for research and
  // testing).
  enum : uint8_t {
    kUnsupportedResourceBit = 1,
    kUnsupportedUnormBit = kUnsupportedResourceBit << 1,
    kUnsupportedSnormBit = kUnsupportedUnormBit << 1,
  };
  uint8_t unsupported_format_features_used_[64] = {};

  uint32_t sampler_max_count_;

  xenos::AnisoFilter max_anisotropy_;

  std::unordered_map<SamplerParameters, Sampler, SamplerParameters::Hasher> samplers_;
  std::pair<const SamplerParameters, Sampler>* sampler_used_first_ = nullptr;
  std::pair<const SamplerParameters, Sampler>* sampler_used_last_ = nullptr;
  uint32_t custom_border_color_sampler_count_ = 0;

  // Batched texture loads (texture_load_batch): while RequestTextures commits
  // the pending loads, every load shader writes into its own range of
  // load_ring_buffer_ (no write-after-read barrier against the previous load),
  // and the buffer -> image copies of all of them are recorded together after
  // a single barrier in FlushTextureLoadBatch.
  struct LoadBatchCopy {
    VulkanTexture* texture;
    uint32_t region_first;
    uint32_t region_count;
  };
  static constexpr VkDeviceSize kLoadRingSize = VkDeviceSize(32) << 20;
  static constexpr VkDeviceSize kLoadRingAlignment = 256;
  bool AllocateLoadRingRange(VkDeviceSize size, VkDeviceSize& offset_out);
  void FlushTextureLoadBatch();
  bool load_batch_active_ = false;
  std::vector<LoadBatchCopy> load_batch_copies_;
  std::vector<VkBufferImageCopy> load_batch_regions_;
  // texture_load_linear_direct: textures written in place in the current batch (barrier to the
  // samplers recorded in FlushTextureLoadBatch), and whether the write-after-read execution
  // barrier against earlier draws was already recorded in this batch.
  std::vector<VulkanTexture*> load_batch_linear_;
  // Textures ResolveCopyToTextures wrote, to mark up to date after the resolve invalidation.
  std::vector<VulkanTexture*> resolve_copied_textures_;
  // B8G8R8A8 scratch for resolves with copy_dest_swap: a blit from the RGBA8 render target into it
  // swaps red and blue in memory, then a raw copy puts those bytes into the RGBA8 texture.
  static constexpr uint32_t kRttSwapWidth = 1280, kRttSwapHeight = 720;
  VkImage rtt_swap_image_ = VK_NULL_HANDLE;
  VkDeviceMemory rtt_swap_memory_ = VK_NULL_HANDLE;
  bool rtt_swap_image_used_ = false;
  bool rtt_swap_creation_failed_ = false;

  // resolve_texture_store: see ResolveStoreView.
  struct ResolveStoreEntry {
    uint32_t base_page = 0;
    uint32_t width = 0, height = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool in_sampled_layout = false;
    bool rb_swapped = false;  // the image holds the render target channel order, the view swaps R/B
    bool valid = false;       // holds a complete resolve of the texture
    std::unordered_map<uint32_t, VkImageView> views;  // by the effective host swizzle
  };
  std::vector<ResolveStoreEntry> resolve_store_;
  std::vector<uint32_t> resolve_store_pages_;
  bool resolve_store_parsed_ = false;
  bool ResolveStoreAllowsPage(uint32_t base_page);
  ResolveStoreEntry* ResolveStoreFind(uint32_t base_page);
  ResolveStoreEntry* ResolveStoreGetOrCreate(uint32_t base_page, uint32_t width, uint32_t height);
  void ResolveStoreDropMatching(const TextureKey& key);
  void ResolveStoreDestroy();
  bool load_batch_linear_war_done_ = false;
  VkBuffer load_ring_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory load_ring_memory_ = VK_NULL_HANDLE;
  bool load_ring_creation_failed_ = false;
  bool load_ring_ever_used_ = false;
  VkDeviceSize load_ring_offset_ = 0;
  uint64_t load_ring_submission_ = UINT64_MAX;

  VkBuffer scaled_resolve_buffer_ = VK_NULL_HANDLE;
  uint64_t scaled_resolve_buffer_size_ = 0;
  bool scaled_resolve_buffer_sparse_ = false;
  uint32_t scaled_resolve_buffer_memory_type_ = UINT32_MAX;
  std::vector<VkDeviceMemory> scaled_resolve_buffer_memory_;
  uint32_t scaled_resolve_sparse_granularity_log2_ = UINT32_MAX;
  std::vector<uint64_t> scaled_resolve_sparse_allocated_;
  bool scaled_resolve_last_usage_write_ = false;
  std::pair<uint64_t, uint64_t> scaled_resolve_last_written_range_{0, 0};
};

}  // namespace rex::graphics::vulkan
