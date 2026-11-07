// texture_poll_write_hook: report guest memcpy writes to the texture cache.
//
// sub_82726240 is the CRT memcpy (r3 = dst, r4 = src, r5 = size). The sprite texture (640x448 R8,
// guest physical ~0x1745B000) and its palette (~0x179A2000) are written through it by sub_8217EE38
// (found with --diag_write_fault_range, 2026-11-06). The texture cache polls those textures (they
// have no page protection, texture_load_cpu_poll); with the hook it only re-checks a texture whose
// pages got a write that changed the data, plus every texture_poll_hook_backstop_frames frames for
// writers that do not go through memcpy. About half of the game's copies there rewrite the same
// bytes, so the copy is compared first (only when it touches a polled texture).
// The generated sub_ functions are weak aliases of __imp__sub_*, so this definition wins.

#include "generated/default/kof2002um_nx_pch.h"

#include <cstring>

extern "C" void __imp__sub_82726240(PPCContext& __restrict ctx, uint8_t* base);
extern "C" int rex_texture_poll_watched(uint32_t guest_address, uint32_t size);
extern "C" void rex_texture_note_cpu_write(uint32_t guest_address, uint32_t size);

extern "C" void sub_82726240(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32;
  const uint32_t src = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  if (dst < 0xA0000000u || !size || !rex_texture_poll_watched(dst, size)) {
    __imp__sub_82726240(ctx, base);
    return;
  }
  const bool same = std::memcmp(base + dst, base + src, size) == 0;
  __imp__sub_82726240(ctx, base);
  if (!same) {
    rex_texture_note_cpu_write(dst, size);
  }
}
