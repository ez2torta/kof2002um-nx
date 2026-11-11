/**
 * @file        core/autotest.cpp
 * @brief       devkitnx autotest (frame times and scripted runs over nxlink) for the Switch build
 *
 * include/rex/autotest.h is a verbatim copy of D:\Ports Swtch\devkitnx\include\autotest.h: change it
 * there and copy it over, never here only.
 *
 * kof2002um-nx (2026-11-05). With --autotest[=scenario] on the command line (nxlink --args), the
 * process opens the sockets and sends stdout to the nxlink server on the PC, where
 * devkitnx/runner/nxtest.py reads the "[AT] ..." lines. Without it nothing changes: no sockets,
 * no stdout, and every at_* call returns at once.
 *
 * Where the port calls it:
 *   main (windowed_app_main_switch.cpp)  rex_autotest_start / rex_autotest_stop
 *   VdSwap (xboxkrnl_video.cpp)          rex_autotest_swap: one frame per guest swap
 *   GPU timestamps (command_processor)   rex_autotest_gpu_frame: GPU time of each swapped frame
 *   pad 1 polling (switch_input_driver)  at_record / at_input
 *   UI loop (windowed_app_context_switch) at_should_quit -> the same quit as closing the app
 */

#define AUTOTEST_IMPLEMENTATION
#include <rex/autotest.h>

#include <atomic>
#include <mutex>
#include <cstring>

namespace {
bool g_owns_stdout = false;
bool g_first_swap = true;
// GPU time of the last frame whose swap the GPU finished, from the timestamp probe (GPU thread).
// Taken (exchanged with 0) by the guest thread at its next swap, so each value counts once.
std::atomic<uint64_t> g_gpu_busy_us{0};
std::atomic<uint64_t> g_gpu_span_us{0};
// Same hand-off for the per-pass split (DeferredCommandBuffer::PassCategory order).
constexpr uint32_t kGpuPasses = 10;
const char* const kGpuPassZones[kGpuPasses] = {
    "gp_other", "gp_draw",     "gp_rt_xfer", "gp_res_clear", "gp_dump",
    "gp_resolve", "gp_readback", "gp_tex_load", "gp_upload",   "gp_swap"};
std::atomic<uint64_t> g_gpu_pass_us[kGpuPasses];
std::atomic<uint64_t> g_gpu_pass_markers{0};
std::atomic<bool> g_gpu_passes_seen{false};
// CPU time of the GPU command processor thread (ns, summed from any thread, taken at each swap):
// cp_idle = waiting for the guest to write the ring buffer, cp_fence = blocked on GPU fences,
// cp_submit = inside vkQueueSubmit, cp_tex = in VulkanTextureCache::RequestTextures.
// n_* are counts, not times: each event adds 1 ms (1000000 ns), so the ms column is the count.
// n_tex = texture loads, n_batch = batches of loads, n_skip = reloads skipped by hash.
// cp_wrm = inside WAIT_REG_MEM packets that did not match on the first poll (cp_wrm_slp = the
// part spent in Sleep), n_wrm = how many of those waits.
// n_draw/n_rp/n_bar/n_copy/n_disp = commands replayed by DeferredCommandBuffer::Execute
// (draws, render pass/rendering begins, pipeline barriers, copies+blits, dispatches).
constexpr uint32_t kCpuZones = 15;
const char* const kCpuZoneNames[kCpuZones] = {"cp_idle", "cp_fence", "cp_submit", "cp_tex",
                                              "n_tex",   "n_batch",  "n_skip",    "cp_wrm",
                                              "cp_wrm_slp", "n_wrm", "n_draw", "n_rp",
                                              "n_bar",   "n_copy",   "n_disp"};
std::atomic<uint64_t> g_cpu_zone_ns[kCpuZones];
// Interval accumulators for the on-console profile report (switch_perf.cpp). They are summed whether or not
// the autotest is running, and taken (zeroed) by rex_autotest_take_interval every 10 s.
std::atomic<uint64_t> g_iv_gpu_frames{0};
std::atomic<uint64_t> g_iv_gpu_busy_us{0};
std::atomic<uint64_t> g_iv_gpu_span_us{0};
std::atomic<uint64_t> g_iv_gpu_pass_us[kGpuPasses];
std::atomic<uint64_t> g_iv_cpu_zone_ns[kCpuZones];
}  // namespace

extern "C" void rex_autotest_cpu_add(uint32_t zone, uint64_t ns) {
  if (zone < kCpuZones) {
    g_cpu_zone_ns[zone].fetch_add(ns, std::memory_order_relaxed);
    g_iv_cpu_zone_ns[zone].fetch_add(ns, std::memory_order_relaxed);
  }
}

extern "C" void rex_autotest_start(int argc, char** argv) {
  bool wanted = false;
  for (int i = 1; argv && i < argc; ++i) {
    if (argv[i] && std::strncmp(argv[i], "--autotest", 10) == 0 &&
        (argv[i][10] == '\0' || argv[i][10] == '=')) {
      wanted = true;
    }
  }
  if (wanted && R_SUCCEEDED(socketInitializeDefault())) {
    // Fails (and stdout stays where it was) when the NRO was not started by nxlink.
    g_owns_stdout = nxlinkStdio() >= 0;
    if (g_owns_stdout) {
      setvbuf(stdout, nullptr, _IOLBF, 0);
    }
  }
  at_init(argc, argv, AT_INIT_NO_SOCKETS);
}

extern "C" void rex_autotest_stop(void) {
  if (at_enabled()) {
    at_shutdown();
  }
}

// log_network uses these to leave the sockets and stdout alone while the runner owns them.
extern "C" int rex_autotest_owns_stdout(void) {
  return g_owns_stdout ? 1 : 0;
}

extern "C" void rex_autotest_gpu_frame(uint64_t busy_ns, uint64_t span_ns) {
  g_gpu_busy_us.store(busy_ns / 1000, std::memory_order_relaxed);
  g_gpu_span_us.store(span_ns / 1000, std::memory_order_relaxed);
  g_iv_gpu_frames.fetch_add(1, std::memory_order_relaxed);
  g_iv_gpu_busy_us.fetch_add(busy_ns / 1000, std::memory_order_relaxed);
  g_iv_gpu_span_us.fetch_add(span_ns / 1000, std::memory_order_relaxed);
}

extern "C" void rex_autotest_gpu_passes(const uint64_t* ns, uint32_t count, uint64_t markers) {
  for (uint32_t c = 0; c < kGpuPasses && c < count; ++c) {
    g_gpu_pass_us[c].store(ns[c] / 1000, std::memory_order_relaxed);
    g_iv_gpu_pass_us[c].fetch_add(ns[c] / 1000, std::memory_order_relaxed);
  }
  g_gpu_pass_markers.store(markers, std::memory_order_relaxed);
  g_gpu_passes_seen.store(true, std::memory_order_relaxed);
}

// Name of the last stage background file the game opened (data\\B\\bg\\bgXn.bin), for the profile report. Kept apart
// from the "[KOF data I/O]" log, which only covers the first 96 opens.
namespace {
std::mutex g_stage_mutex;
char g_stage_name[40] = "";
}  // namespace
extern "C" void rex_note_stage_file(const char* path) {
  if (path == nullptr) {
    return;
  }
  const char* name = std::strrchr(path, '\\');
  name = name ? name + 1 : path;
  std::lock_guard<std::mutex> lock(g_stage_mutex);
  std::strncpy(g_stage_name, name, sizeof(g_stage_name) - 1);
  g_stage_name[sizeof(g_stage_name) - 1] = '\0';
}
extern "C" void rex_take_stage_name(char* out, uint32_t cap) {
  std::lock_guard<std::mutex> lock(g_stage_mutex);
  std::strncpy(out, g_stage_name, cap - 1);
  out[cap - 1] = '\0';
}

// Profile report (switch_perf.cpp, every 10 s): takes and zeroes the interval accumulators.
// Layout of out[]: [0] GPU frames measured, [1] GPU busy us, [2] GPU span us, [3..12] GPU us per pass
// (kGpuPasses = 10: other, draw, rt_xfer, res_clear, dump, resolve, readback, tex_load, upload, swap),
// [13..27] CPU zones in ns (kCpuZones = 15, same order as kCpuZoneNames; n_* are counts x 1e6).
extern "C" void rex_autotest_take_interval(uint64_t* out, uint32_t cap) {
  if (out == nullptr || cap < 28) {
    return;
  }
  out[0] = g_iv_gpu_frames.exchange(0, std::memory_order_relaxed);
  out[1] = g_iv_gpu_busy_us.exchange(0, std::memory_order_relaxed);
  out[2] = g_iv_gpu_span_us.exchange(0, std::memory_order_relaxed);
  for (uint32_t c = 0; c < kGpuPasses; ++c) {
    out[3 + c] = g_iv_gpu_pass_us[c].exchange(0, std::memory_order_relaxed);
  }
  for (uint32_t c = 0; c < kCpuZones; ++c) {
    out[13 + c] = g_iv_cpu_zone_ns[c].exchange(0, std::memory_order_relaxed);
  }
}

// Vblank thread (GraphicsSystem::MarkVblank): the clock of --at-clock=vblank.
extern "C" void rex_autotest_vblank(void) {
  if (at_enabled()) {
    at_vblank();
  }
}

// Guest thread, in VdSwap: the frame is the time from one swap to the next.
extern "C" void rex_autotest_swap(void) {
  if (!at_enabled()) {
    return;
  }
  if (g_first_swap) {
    g_first_swap = false;
    at_mark("first_frame");
    at_frame_begin();
    return;
  }
  // 0 when the timestamp probe is off (gpu_submission_timestamps) or the GPU is a frame behind.
  at_zone_add("gpu_busy", g_gpu_busy_us.exchange(0, std::memory_order_relaxed));
  at_zone_add("gpu_span", g_gpu_span_us.exchange(0, std::memory_order_relaxed));
  if (g_gpu_passes_seen.load(std::memory_order_relaxed)) {
    // All columns from the first report on, so the header is stable. gp_markers is a count,
    // not a time (at_zone_add takes "us"; the runner shows it as if it were ms/1000).
    for (uint32_t c = 0; c < kGpuPasses; ++c) {
      at_zone_add(kGpuPassZones[c], g_gpu_pass_us[c].exchange(0, std::memory_order_relaxed));
    }
    at_zone_add("gp_markers", g_gpu_pass_markers.exchange(0, std::memory_order_relaxed));
  }
  for (uint32_t c = 0; c < kCpuZones; ++c) {
    at_zone_add(kCpuZoneNames[c], g_cpu_zone_ns[c].exchange(0, std::memory_order_relaxed) / 1000);
  }
  at_frame_end();
  at_frame_begin();
}
