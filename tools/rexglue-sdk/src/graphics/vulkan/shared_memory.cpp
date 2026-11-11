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
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>

#if REX_PLATFORM_SWITCH
#include <chrono>
// devkitnx zones (core/autotest.cpp): 25 cp_vcmp, 26 cp_vval, 27 cp_vcpy, 28 n_vpg, 30 n_vfb.
extern "C" void rex_autotest_cpu_add(uint32_t zone, uint64_t ns);
namespace {
inline uint64_t SmNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
}  // namespace
#endif

REXCVAR_DEFINE_BOOL(vulkan_sparse_shared_memory, true, "GPU/Vulkan",
                    "Use sparse shared memory on Vulkan")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(shared_memory_cpu_upload, false, "GPU/Vulkan",
                    "UMA (Switch): host-map the shared memory buffer and write uploads with the CPU "
                    "when no pending GPU command reads changed bytes of the range or writes it (no "
                    "copy, barrier or render pass break per upload)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(shared_memory_upload_hoist, false, "GPU/Vulkan",
                    "Move a shared memory upload to the start of the submission when no earlier "
                    "command of the submission touched its range, so it does not break the render "
                    "pass (kof2002um: one upload per sprite draw)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#include <rex/platform.h>
#if REX_PLATFORM_SWITCH
// Counters for the Switch profiler (switch_perf.cpp).
extern "C" void RexSwitchPerfCount(unsigned id);
extern "C" void RexSwitchPerfAdd(unsigned id, uint64_t value);
#endif

namespace rex::graphics::vulkan {

VulkanSharedMemory::VulkanSharedMemory(VulkanCommandProcessor& command_processor,
                                       memory::Memory& memory,
                                       VkPipelineStageFlags guest_shader_pipeline_stages)
    : SharedMemory(memory),
      command_processor_(command_processor),
      guest_shader_pipeline_stages_(guest_shader_pipeline_stages) {}

VulkanSharedMemory::~VulkanSharedMemory() {
  Shutdown(true);
}

bool VulkanSharedMemory::Initialize() {
  InitializeCommon();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  const VkBufferCreateFlags sparse_flags =
      VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;

  // Try to create a sparse buffer.
  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = sparse_flags;
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_create_info.queueFamilyIndexCount = 0;
  buffer_create_info.pQueueFamilyIndices = nullptr;
  if (REXCVAR_GET(vulkan_sparse_shared_memory) &&
      vulkan_device->properties().sparseResidencyBuffer) {
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) == VK_SUCCESS) {
      VkMemoryRequirements buffer_memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, buffer_, &buffer_memory_requirements);
      if (rex::bit_scan_forward(buffer_memory_requirements.memoryTypeBits &
                                    vulkan_device->memory_types().device_local,
                                &buffer_memory_type_)) {
        uint32_t allocation_size_log2;
        rex::bit_scan_forward(std::max(uint64_t(buffer_memory_requirements.alignment), uint64_t(1)),
                              &allocation_size_log2);
        if (allocation_size_log2 < kBufferSizeLog2) {
          // Maximum of 1024 allocations in the worst case for all of the
          // buffer because of the overall 4096 allocation count limit on
          // Windows drivers.
          InitializeSparseHostGpuMemory(std::max(
              allocation_size_log2,
              std::max(kHostGpuMemoryOptimalSparseAllocationLog2, kBufferSizeLog2 - uint32_t(10))));
        } else {
          // Shouldn't happen on any real platform, but no point allocating the
          // buffer sparsely.
          dfn.vkDestroyBuffer(device, buffer_, nullptr);
          buffer_ = VK_NULL_HANDLE;
        }
      } else {
        REXGPU_ERROR(
            "Shared memory: Failed to get a device-local Vulkan memory type "
            "for the sparse buffer");
        dfn.vkDestroyBuffer(device, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
      }
    } else {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB Vulkan sparse buffer",
                   kBufferSize >> 20);
    }
  }

  // Create a non-sparse buffer if there were issues with the sparse buffer.
  if (buffer_ == VK_NULL_HANDLE) {
    REXGPU_INFO(
        "Vulkan sparse binding is not used for shared memory emulation - video "
        "memory usage may increase significantly because a full {} MB buffer "
        "will be created",
        kBufferSize >> 20);
    buffer_create_info.flags &= ~sparse_flags;
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) != VK_SUCCESS) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB Vulkan buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
    VkMemoryRequirements buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, buffer_, &buffer_memory_requirements);
    const uint32_t compatible_memory_types = buffer_memory_requirements.memoryTypeBits;
    uint32_t device_local_memory_types =
        compatible_memory_types & vulkan_device->memory_types().device_local;
    bool cpu_upload = false;
    if (REXCVAR_GET(shared_memory_cpu_upload)) {
      const uint32_t host_visible = compatible_memory_types & vulkan_device->memory_types().host_visible;
      uint32_t host_types = host_visible & device_local_memory_types;
      if (!host_types) {
        host_types = host_visible;
      }
      if (host_types) {
        device_local_memory_types = host_types;
        cpu_upload = true;
      }
    }
    if (!rex::bit_scan_forward(device_local_memory_types, &buffer_memory_type_)) {
      // Integrated GPUs such as Switch's Tegra share system memory with the
      // CPU. Some Vulkan drivers expose compatible memory without the
      // DEVICE_LOCAL property; that property is a placement preference, not a
      // requirement for binding a buffer. Fall back to any type allowed by
      // VkMemoryRequirements and let allocation report whether it is usable.
      REXGPU_WARN(
          "Shared memory: no device-local Vulkan memory type for the buffer; "
          "trying compatible memory type mask {:08X}",
          compatible_memory_types);
      if (!rex::bit_scan_forward(compatible_memory_types, &buffer_memory_type_)) {
        REXGPU_ERROR(
            "Shared memory: no compatible Vulkan memory type for the buffer "
            "(memoryTypeBits {:08X})",
            compatible_memory_types);
        Shutdown();
        return false;
      }
    }
    VkMemoryAllocateInfo buffer_memory_allocate_info;
    VkMemoryAllocateInfo* buffer_memory_allocate_info_last = &buffer_memory_allocate_info;
    buffer_memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buffer_memory_allocate_info.pNext = nullptr;
    buffer_memory_allocate_info.allocationSize = buffer_memory_requirements.size;
    buffer_memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
    VkMemoryDedicatedAllocateInfo buffer_memory_dedicated_allocate_info;
    if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
      buffer_memory_allocate_info_last->pNext = &buffer_memory_dedicated_allocate_info;
      buffer_memory_allocate_info_last =
          reinterpret_cast<VkMemoryAllocateInfo*>(&buffer_memory_dedicated_allocate_info);
      buffer_memory_dedicated_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
      buffer_memory_dedicated_allocate_info.pNext = nullptr;
      buffer_memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
      buffer_memory_dedicated_allocate_info.buffer = buffer_;
    }
    VkDeviceMemory buffer_memory;
    if (dfn.vkAllocateMemory(device, &buffer_memory_allocate_info, nullptr, &buffer_memory) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "Shared memory: Failed to allocate {} MB of memory for the Vulkan "
          "buffer",
          kBufferSize >> 20);
      Shutdown();
      return false;
    }
    buffer_memory_.push_back(buffer_memory);
    if (dfn.vkBindBufferMemory(device, buffer_, buffer_memory, 0) != VK_SUCCESS) {
      REXGPU_ERROR("Shared memory: Failed to bind memory to the Vulkan buffer");
      Shutdown();
      return false;
    }
    if (cpu_upload) {
      void* mapping = nullptr;
      if (dfn.vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapping) == VK_SUCCESS) {
        cpu_mapping_ = static_cast<uint8_t*>(mapping);
        cpu_mapping_coherent_ =
            (vulkan_device->memory_types().host_coherent >> buffer_memory_type_) & 1;
      }
      std::printf("[cpuup] shared memory host-mapped: %s, memory type %u, coherent %u\n",
                  cpu_mapping_ ? "yes" : "FAILED", buffer_memory_type_,
                  uint32_t(cpu_mapping_coherent_));
    }
  }

  // The first usage will likely be uploading.
  last_usage_ = Usage::kTransferDestination;
  last_written_range_ = std::make_pair<uint32_t, uint32_t>(0, 0);

  upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      rex::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize, size_t(1)
                                                                           << page_size_log2()));

  return true;
}

void VulkanSharedMemory::Shutdown(bool from_destructor) {
  upload_buffer_pool_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, buffer_);
  for (VkDeviceMemory memory : buffer_memory_) {
    dfn.vkFreeMemory(device, memory, nullptr);
  }
  buffer_memory_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanSharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void VulkanSharedMemory::EndSubmission() {
  upload_buffer_pool_->FlushWrites();
}

void VulkanSharedMemory::Use(Usage usage, std::pair<uint32_t, uint32_t> written_range) {
  written_range.first = std::min(written_range.first, kBufferSize);
  written_range.second = std::min(written_range.second, kBufferSize - written_range.first);
  if (!in_upload_ && written_range.second) {
    TrackInflight(written_range.first, written_range.second, true);
    TrackSubmissionRange(written_range.first, written_range.second);
  }
  assert_true(usage != Usage::kRead || !written_range.second);
  if (last_usage_ != usage || last_written_range_.second) {
    VkPipelineStageFlags src_stage_mask, dst_stage_mask;
    VkAccessFlags src_access_mask, dst_access_mask;
    GetUsageMasks(last_usage_, src_stage_mask, src_access_mask);
    GetUsageMasks(usage, dst_stage_mask, dst_access_mask);
    VkDeviceSize offset, size;
    if (last_usage_ == usage) {
      // Committing the previous write, while not changing the access mask
      // (passing false as whether to skip the barrier if no masks are changed
      // for this reason).
      offset = VkDeviceSize(last_written_range_.first);
      size = VkDeviceSize(last_written_range_.second);
    } else {
      // Changing the stage and access mask - all preceding writes must be
      // available not only to the source stage, but to the destination as well.
      offset = 0;
      size = VK_WHOLE_SIZE;
      last_usage_ = usage;
    }
    command_processor_.PushBufferMemoryBarrier(
        buffer_, offset, size, src_stage_mask, dst_stage_mask, src_access_mask, dst_access_mask,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  }
  last_written_range_ = written_range;
}

bool VulkanSharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                          uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = length_allocations
                                        << host_gpu_memory_sparse_granularity_log2();
  memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
    REXGPU_ERROR("Shared memory: Failed to allocate sparse buffer memory");
    return false;
  }
  buffer_memory_.push_back(memory);

  VkSparseMemoryBind bind;
  bind.resourceOffset = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  bind.size = memory_allocate_info.allocationSize;
  bind.memory = memory;
  bind.memoryOffset = 0;
  bind.flags = 0;
  VkPipelineStageFlags bind_wait_stage_mask =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
      VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (vulkan_device->properties().tessellationShader) {
    bind_wait_stage_mask |= VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
  }
  command_processor_.SparseBindBuffer(buffer_, 1, &bind, bind_wait_stage_mask);

  return true;
}

bool VulkanSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
#if REX_PLATFORM_SWITCH
  {
    uint64_t pages = 0;
    for (const auto& range : upload_page_ranges) {
      pages += range.second;
    }
    RexSwitchPerfAdd(15, pages << page_size_log2());
  }
#endif
  // upload_page_ranges are sorted, use them to determine the range for the
  // ordering barrier.
  const uint32_t span_start = upload_page_ranges.front().first << page_size_log2();
  const uint32_t span_length = (upload_page_ranges.back().first + upload_page_ranges.back().second -
                                upload_page_ranges.front().first)
                               << page_size_log2();
  if (TryCpuUpload(upload_page_ranges)) {
    return true;
  }
  TrackInflight(span_start, span_length, true);
  const bool hoist = REXCVAR_GET(shared_memory_upload_hoist) &&
                     !SubmissionRangeTouched(span_start, span_length);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  if (!hoist) {
    in_upload_ = true;
    Use(Usage::kTransferDestination, std::make_pair(span_start, span_length));
    in_upload_ = false;
    command_processor_.SubmitBarriers(true);
    command_buffer.MarkPass(DeferredCommandBuffer::PassCategory::kUpload);
    TrackSubmissionRange(span_start, span_length);
  }
  uint64_t submission_current = command_processor_.GetCurrentSubmission();
  bool successful = true;
  upload_regions_.clear();
  VkBuffer upload_buffer_previous = VK_NULL_HANDLE;
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    while (upload_range_length) {
      VkBuffer upload_buffer;
      VkDeviceSize upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          submission_current, upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), upload_buffer, upload_buffer_offset, upload_buffer_size);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get a Vulkan upload buffer");
        successful = false;
        break;
      }
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      std::memcpy(upload_buffer_mapping,
                  memory().TranslatePhysical(upload_range_start << page_size_log2()),
                  upload_buffer_size);
      if (upload_buffer_previous != upload_buffer && !upload_regions_.empty()) {
        assert_true(upload_buffer_previous != VK_NULL_HANDLE);
        if (hoist) {
          for (const VkBufferCopy& region : upload_regions_) {
            hoisted_uploads_.push_back({upload_buffer_previous, region});
          }
        } else {
          command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                         uint32_t(upload_regions_.size()), upload_regions_.data());
        }
        upload_regions_.clear();
      }
      upload_buffer_previous = upload_buffer;
      VkBufferCopy& upload_region = upload_regions_.emplace_back();
      upload_region.srcOffset = upload_buffer_offset;
      upload_region.dstOffset = VkDeviceSize(upload_range_start << page_size_log2());
      upload_region.size = upload_buffer_size;
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    if (!successful) {
      break;
    }
  }
  if (!upload_regions_.empty()) {
    assert_true(upload_buffer_previous != VK_NULL_HANDLE);
    if (hoist) {
      for (const VkBufferCopy& region : upload_regions_) {
        hoisted_uploads_.push_back({upload_buffer_previous, region});
      }
    } else {
      command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                     uint32_t(upload_regions_.size()), upload_regions_.data());
    }
    upload_regions_.clear();
  }
  return successful;
}

void VulkanSharedMemory::TrackInflight(uint32_t start, uint32_t length, bool gpu_write) {
  if (!cpu_mapping_ || !length) {
    return;
  }
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  while (!inflight_ranges_.empty() && inflight_ranges_.front().submission <= completed) {
    inflight_ranges_.pop_front();
  }
  const uint64_t submission = command_processor_.GetCurrentSubmission();
  const uint32_t end = uint32_t(std::min(uint64_t(start) + length, uint64_t(kBufferSize)));
  if (!inflight_ranges_.empty()) {
    InflightRange& last = inflight_ranges_.back();
    if (last.submission == submission && last.gpu_write == gpu_write && start <= last.end &&
        end >= last.start) {
      last.start = std::min(last.start, start);
      last.end = std::max(last.end, end);
      return;
    }
  }
  inflight_ranges_.push_back({submission, start, end, gpu_write});
}

bool VulkanSharedMemory::TryCpuUpload(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (!cpu_mapping_) {
    return false;
  }
  const VkDevice device = command_processor_.GetVulkanDevice()->device();
  const auto& dfn = command_processor_.GetVulkanDevice()->functions();
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  while (!inflight_ranges_.empty() && inflight_ranges_.front().submission <= completed) {
    inflight_ranges_.pop_front();
  }
  auto mapped_range = [&](uint32_t lo, uint32_t hi) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = buffer_memory_.front();
    range.offset = lo & ~UINT32_C(0xFFF);
    range.size = ((uint64_t(hi) + 0xFFF) & ~UINT64_C(0xFFF)) - range.offset;
    return range;
  };
  const uint32_t page_log2 = page_size_log2();
#if REX_PLATFORM_SWITCH
  uint64_t t_cmp = SmNowNs();
  struct CmpZone {
    uint64_t& t;
    ~CmpZone() { if (t) rex_autotest_cpu_add(25, SmNowNs() - t); }
  } cmp_zone{t_cmp};
  auto fallback_count = [] { rex_autotest_cpu_add(30, 1000000ull); };
#else
  auto fallback_count = [] {};
#endif
  // Pending commands must see the same bytes they were recorded with.
  for (const auto& pages : upload_page_ranges) {
    const uint32_t start = pages.first << page_log2;
    const uint32_t end = (pages.first + pages.second) << page_log2;
    for (const InflightRange& inflight : inflight_ranges_) {
      const uint32_t lo = std::max(start, inflight.start);
      const uint32_t hi = std::min(end, inflight.end);
      if (lo >= hi) {
        continue;
      }
      if (inflight.gpu_write) {
        ++cpu_upload_fallbacks_;
        fallback_count();
        return false;
      }
      if (!cpu_mapping_coherent_) {
        VkMappedMemoryRange range = mapped_range(lo, hi);
        dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
      }
      if (std::memcmp(cpu_mapping_ + lo, memory().TranslatePhysical(lo), hi - lo) != 0) {
        ++cpu_upload_fallbacks_;
        fallback_count();
        return false;
      }
    }
  }
#if REX_PLATFORM_SWITCH
  rex_autotest_cpu_add(25, SmNowNs() - t_cmp);
  t_cmp = 0;
  uint64_t n_pages = 0;
#endif
  for (const auto& pages : upload_page_ranges) {
    const uint32_t start = pages.first << page_log2;
    const uint32_t length = pages.second << page_log2;
#if REX_PLATFORM_SWITCH
    const uint64_t t_val = SmNowNs();
#endif
    MakeRangeValid(start, length, false);
#if REX_PLATFORM_SWITCH
    const uint64_t t_cpy = SmNowNs();
    rex_autotest_cpu_add(26, t_cpy - t_val);
    n_pages += pages.second;
#endif
    std::memcpy(cpu_mapping_ + start, memory().TranslatePhysical(start), length);
    if (!cpu_mapping_coherent_) {
      VkMappedMemoryRange range = mapped_range(start, start + length);
      dfn.vkFlushMappedMemoryRanges(device, 1, &range);
    }
#if REX_PLATFORM_SWITCH
    rex_autotest_cpu_add(27, SmNowNs() - t_cpy);
#endif
  }
#if REX_PLATFORM_SWITCH
  rex_autotest_cpu_add(28, n_pages * 1000000ull);
#endif
  if ((++cpu_uploads_ & 8191) == 0) {
    std::printf("[cpuup] %llu cpu uploads, %llu fell back to copies\n",
                (unsigned long long)cpu_uploads_, (unsigned long long)cpu_upload_fallbacks_);
  }
  return true;
}

void VulkanSharedMemory::OnRangesRequested(const std::pair<uint32_t, uint32_t>* ranges,
                                           size_t count) {
  for (size_t i = 0; i < count; ++i) {
    TrackInflight(ranges[i].first, ranges[i].second, false);
    TrackSubmissionRange(ranges[i].first, ranges[i].second);
  }
}

void VulkanSharedMemory::TrackSubmissionRange(uint32_t start, uint32_t length) {
  if (!length || submission_touched_overflow_) {
    return;
  }
  const uint64_t end = uint64_t(start) + length;
  // Extend an existing range that overlaps or touches this one (the common case: a ring).
  for (auto& range : submission_touched_) {
    const uint64_t range_end = uint64_t(range.first) + range.second;
    if (start <= range_end && end >= range.first) {
      const uint32_t new_start = std::min(range.first, start);
      range.second = uint32_t(std::max(range_end, end) - new_start);
      range.first = new_start;
      return;
    }
  }
  if (submission_touched_.size() >= 256) {
    submission_touched_overflow_ = true;  // too many: nothing more is hoisted this submission
    return;
  }
  submission_touched_.emplace_back(start, length);
}

bool VulkanSharedMemory::SubmissionRangeTouched(uint32_t start, uint32_t length) const {
  if (submission_touched_overflow_) {
    return true;
  }
  const uint64_t end = uint64_t(start) + length;
  for (const auto& range : submission_touched_) {
    if (start < uint64_t(range.first) + range.second && end > range.first) {
      return true;
    }
  }
  return false;
}

void VulkanSharedMemory::ExecuteHoistedUploads(VkCommandBuffer command_buffer) {
  submission_touched_.clear();
  submission_touched_overflow_ = false;
  if (hoisted_uploads_.empty()) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn =
      command_processor_.GetVulkanDevice()->functions();
  // Earlier submissions may still read or write these ranges; the deferred commands of this one
  // read them after the copies.
  VkMemoryBarrier barrier;
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  size_t i = 0;
  while (i < hoisted_uploads_.size()) {
    size_t j = i;
    upload_regions_.clear();
    while (j < hoisted_uploads_.size() && hoisted_uploads_[j].source == hoisted_uploads_[i].source) {
      upload_regions_.push_back(hoisted_uploads_[j].region);
      ++j;
    }
    dfn.vkCmdCopyBuffer(command_buffer, hoisted_uploads_[i].source, buffer_,
                        uint32_t(upload_regions_.size()), upload_regions_.data());
    i = j;
  }
  upload_regions_.clear();
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                           nullptr);
  hoisted_uploads_.clear();
}

void VulkanSharedMemory::GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                                       VkAccessFlags& access_mask) const {
  switch (usage) {
    case Usage::kComputeWrite:
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      // The resolve writes here: make those writes visible before the next use.
      access_mask = VK_ACCESS_SHADER_WRITE_BIT;
      return;
    case Usage::kTransferDestination:
      stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
      return;
    default:
      break;
  }
  stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_;
  access_mask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  switch (usage) {
    case Usage::kRead:
      stage_mask |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask |= VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case Usage::kGuestDrawReadWrite:
      access_mask |= VK_ACCESS_SHADER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

}  // namespace rex::graphics::vulkan
