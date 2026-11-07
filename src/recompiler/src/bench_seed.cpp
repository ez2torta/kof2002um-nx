// Fixed RNG seed for the autotest bench (--at-seed=N).
//
// sub_827254F0 is the MSVC CRT srand(): _getptd()->_holdrand (+20) = seed; rand() is
// sub_82725520 (seed * 214013 + 2531011). At boot the game calls srand(GetTickCount())
// (bl at 0x82160C90, lr 0x82160C94; sub_826AEA30 reads KeTimeStampBundle.TickCount), so stage and rival
// change between runs and the bench measures a different fight each time.
// With --at-seed=N only that call gets N; the other srand calls (fixed seeds) are left alone.
// The generated sub_ functions are weak aliases of __imp__sub_*, so this definition wins.

#include "generated/default/kof2002um_nx_pch.h"

#include <cstdio>
#include <cstdlib>

#include <rex/autotest.h>

extern "C" void __imp__sub_827254F0(PPCContext& __restrict ctx, uint8_t* base);

extern "C" void sub_827254F0(PPCContext& __restrict ctx, uint8_t* base) {
  static constexpr uint32_t kBootSeedReturn = 0x82160C94;  // lr of the boot srand(GetTickCount())
  if (ctx.lr == kBootSeedReturn) {
    const char* v = at_arg("at-seed");
    if (v && *v) {
      const uint32_t seed = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
      at_log("seed: srand(%u) -> srand(%u)", ctx.r3.u32, seed);
      ctx.r3.u64 = seed;
    }
  }
  __imp__sub_827254F0(ctx, base);
}

// --at-randlog=N: log every rand() (sub_82725520) call up to rendered frame N, one line per
// frame: "rnd f=<frame> <thread>:<caller lr>[x<repeat>] ..." (for the g_rand8 wrapper, its caller). Comparing two runs
// shows whether the rand sequence diverges and which caller does it first.
extern "C" void __imp__sub_82725520(PPCContext& __restrict ctx, uint8_t* base);

#include <mutex>
#include <string>

namespace {
std::mutex g_rnd_mu;
std::string g_rnd_line;
uint64_t g_rnd_frame = ~0ull;
uint64_t g_rnd_last = 0;
uint32_t g_rnd_rep = 0;
uint32_t g_rnd_threads[16];
int g_rnd_nthreads = 0;

int RndThreadIndex(uint32_t r13) {
  for (int i = 0; i < g_rnd_nthreads; ++i)
    if (g_rnd_threads[i] == r13) return i;
  if (g_rnd_nthreads < 16) { g_rnd_threads[g_rnd_nthreads] = r13; return g_rnd_nthreads++; }
  return 15;
}

void RndFlushRep() {
  if (!g_rnd_rep) return;
  char b[32];
  const unsigned th = unsigned(g_rnd_last >> 32), lr = unsigned(g_rnd_last & 0xFFFFFFFFu);
  if (g_rnd_rep > 1) snprintf(b, sizeof(b), " %u:%08X x%u", th, lr, g_rnd_rep);
  else snprintf(b, sizeof(b), " %u:%08X", th, lr);
  g_rnd_line += b;
  g_rnd_rep = 0;
  if (g_rnd_line.size() > 300) {
    at_log("rnd f=%llu%s", (unsigned long long)g_rnd_frame, g_rnd_line.c_str());
    g_rnd_line.clear();
  }
}

void RndFlushFrame() {
  RndFlushRep();
  if (!g_rnd_line.empty()) at_log("rnd f=%llu%s", (unsigned long long)g_rnd_frame, g_rnd_line.c_str());
  g_rnd_line.clear();
}
}  // namespace

extern "C" void sub_82725520(PPCContext& __restrict ctx, uint8_t* base) {
  static const uint64_t limit = [] {
    const char* v = at_arg("at-randlog");
    return (v && *v) ? std::strtoull(v, nullptr, 0) : 0ull;
  }();
  if (limit) {
    const uint64_t f = at_frame();
    if (f <= limit) {
      std::lock_guard<std::mutex> lock(g_rnd_mu);
      if (f != g_rnd_frame) { RndFlushFrame(); g_rnd_frame = f; }
      // sub_825F89D8 = "g_rand8 (0x84ADB7E8) = rand() & 0xFF", called from ~100 places: log its
      // caller instead (the wrapper saved its lr at r1+88 before calling rand).
      uint32_t lr = uint32_t(ctx.lr);
      if (lr == 0x825F89E8) lr = REX_LOAD_U32(ctx.r1.u32 + 88);
      const uint64_t key = (uint64_t(RndThreadIndex(ctx.r13.u32)) << 32) | lr;
      if (g_rnd_rep && key == g_rnd_last) ++g_rnd_rep;
      else { RndFlushRep(); g_rnd_last = key; g_rnd_rep = 1; }
    }
  }
  // --at-seed: reseed right before the stage choice (sub_8261E450, caller lr 0x8261E4B0 through
  // the g_rand8 wrapper) and the later pick from 0x82636BEC. Some rand() calls happen once per
  // rendered frame, so at a different fps the sequence drifts and the bench lands on another stage
  // (factory or park, 2026-11-07); reseeding there makes the choice independent of the fps.
  // Experimental, off unless --at-seed-picks: with it both runs left the replay off the fight
  // (team intro at match_start + 13 s) and the rivals still differed, see PORT_STATUS 2026-11-07.
  static const char* seed_arg = at_arg("at-seed-picks") ? at_arg("at-seed") : nullptr;
  if (seed_arg && *seed_arg) {
    uint32_t caller = uint32_t(ctx.lr);
    if (caller == 0x825F89E8) caller = REX_LOAD_U32(ctx.r1.u32 + 88);
    uint32_t offset = caller == 0x8261E4B0 ? 1 : caller == 0x82636BEC ? 2 : 0;
    // Only the first call of a pick: the caller retries (rejection sampling) within the same
    // frame, and reseeding every time would return the same value forever (hang, 2026-11-07).
    static uint64_t last_frame[3] = {~0ull, ~0ull, ~0ull};
    if (offset && last_frame[offset] != at_frame()) {
      last_frame[offset] = at_frame();
      static int logged = 0;
      const uint32_t seed = uint32_t(std::strtoul(seed_arg, nullptr, 0)) + offset;
      PPCContext saved = ctx;
      ctx.r3.u64 = seed;
      __imp__sub_827254F0(ctx, base);  // srand(seed) on this thread
      ctx = saved;
      if (logged < 8) {
        ++logged;
        at_log("seed: reseed %u before the pick from %08X (frame %llu)", seed, caller,
               (unsigned long long)at_frame());
      }
    }
  }
  __imp__sub_82725520(ctx, base);
}

// --at-timelog: the game's GetTickCount (sub_826AEA30 reads KeTimeStampBundle from memory, so the
// clock hook in the SDK does not see it): note its guest caller as kind 'g'.
extern "C" void rex_timelog_note(char kind, uintptr_t ra);
extern "C" void __imp__sub_826AEA30(PPCContext& __restrict ctx, uint8_t* base);
// --at-seed also fixes the bench's rival team: sub_8216A278 (lr 0x8216A2BC) picks one of two
// fixed CPU teams with GetTickCount() & 1 -- (30,31,32) or (36,51,38) -- so the fight landed on
// the factory or the park ~50/50 between runs (found with --at-timelog, 2026-11-07). Force that
// bit to seed & 1.
extern "C" void sub_826AEA30(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t lr = uint32_t(ctx.lr);
  rex_timelog_note('g', uintptr_t(lr));
  __imp__sub_826AEA30(ctx, base);
  static constexpr uint32_t kTeamPickReturn = 0x8216A2BC;
  if (lr == kTeamPickReturn) {
    static const char* seed_arg = at_arg("at-seed");
    if (seed_arg && *seed_arg) {
      const uint32_t bit = uint32_t(std::strtoul(seed_arg, nullptr, 0)) & 1u;
      at_log("seed: team pick GetTickCount %u -> bit %u", ctx.r3.u32, bit);
      ctx.r3.u64 = (ctx.r3.u32 & ~1u) | bit;
    }
  }
}
