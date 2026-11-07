#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/memory.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

class VulkanSharedMemory : public SharedMemory {
 public:
  VulkanSharedMemory(VulkanCommandProcessor& command_processor, memory::Memory& memory,
                     VkPipelineStageFlags guest_shader_pipeline_stages);
  ~VulkanSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void CompletedSubmissionUpdated();
  void EndSubmission();
  // shared_memory_upload_hoist: records the uploads moved to the start of the submission
  // (with barriers around them) into the command buffer, before the deferred commands.
  void ExecuteHoistedUploads(VkCommandBuffer command_buffer);

  enum class Usage {
    // Index buffer, vfetch, compute read, transfer source.
    kRead,
    // Index buffer, vfetch, memexport.
    kGuestDrawReadWrite,
    kComputeWrite,
    kTransferDestination,
  };
  // Inserts a pipeline barrier for the target usage, also ensuring consecutive
  // read-write accesses are ordered with each other.
  void Use(Usage usage, std::pair<uint32_t, uint32_t> written_range = {});

  VkBuffer buffer() const { return buffer_; }

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;
  void OnRangesRequested(const std::pair<uint32_t, uint32_t>* ranges, size_t count) override;

 private:
  void GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                     VkAccessFlags& access_mask) const;

  VulkanCommandProcessor& command_processor_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  VkBuffer buffer_ = VK_NULL_HANDLE;
  uint32_t buffer_memory_type_;
  // Single for non-sparse, every allocation so far for sparse.
  std::vector<VkDeviceMemory> buffer_memory_;

  Usage last_usage_;
  std::pair<uint32_t, uint32_t> last_written_range_;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> upload_buffer_pool_;
  std::vector<VkBufferCopy> upload_regions_;

  // shared_memory_upload_hoist: byte ranges the GPU commands of the current submission already
  // read or wrote (an upload overlapping one must stay in order), and the uploads moved to the
  // start of the submission.
  void TrackSubmissionRange(uint32_t start, uint32_t length);
  bool SubmissionRangeTouched(uint32_t start, uint32_t length) const;
  std::vector<std::pair<uint32_t, uint32_t>> submission_touched_;
  bool submission_touched_overflow_ = false;
  struct HoistedUpload {
    VkBuffer source;
    VkBufferCopy region;
  };
  std::vector<HoistedUpload> hoisted_uploads_;
  bool in_upload_ = false;

  // shared_memory_cpu_upload (UMA): the buffer is host-mapped and uploads are written by the CPU,
  // with no copy, barrier or render pass break, when no command not yet completed by the GPU
  // reads bytes of the range that would change or writes the range.
  void TrackInflight(uint32_t start, uint32_t length, bool gpu_write);
  bool TryCpuUpload(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges);
  uint8_t* cpu_mapping_ = nullptr;
  bool cpu_mapping_coherent_ = false;
  struct InflightRange {
    uint64_t submission;
    uint32_t start, end;
    bool gpu_write;
  };
  std::deque<InflightRange> inflight_ranges_;
  uint64_t cpu_uploads_ = 0, cpu_upload_fallbacks_ = 0;
};

}  // namespace rex::graphics::vulkan
