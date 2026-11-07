// Descomprime un archivo XCTD (0FF512ED) a bytes planos, como hacía el kernel de la 360 al leer.
// Cabecera: +0 magia, +4 ver, +8 hash, +0C flags BE (ventana = 15 + ((flags>>4)&3), n = (flags&0xFFFF)>>6),
// +10 tabla de n tamaños de 20 bits MSB-first (padding a u32), o de u32 BE si flags & 0x00400000
// (bloques de 1 MB o más); después los frames XMem de cada bloque.
// Cada bloque es un stream LZX independiente que arranca en un múltiplo de 0x20000 del archivo (relleno con
// ceros); cada frame da 0x8000 bytes salvo el último del bloque.
// n = 0: sin frames, el archivo son <primer campo de 20 bits> bytes en cero (data/vic/vic0_00.bin; supuesto).
// uso: xctd in out [-k]   -> imprime "ok <tamaño>" o "error ..." (exit 1)
//   -k: si un bloque no decodifica, lo deja en ceros y sigue (data/B/bg/bgj1.bin, al que le falta un bloque).
// build (WSL): gcc -O2 -Ims xctd.c ms/lzxd.c ms/system.c -o xctd
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mspack.h"
#include "lzx.h"

struct memf { unsigned char* p; size_t len, pos; };

static int m_read(struct mspack_file* f, void* buf, int n) {
  struct memf* m = (struct memf*)f;
  size_t k = m->len - m->pos; if ((size_t)n < k) k = n;
  memcpy(buf, m->p + m->pos, k); m->pos += k; return (int)k;
}
static int m_write(struct mspack_file* f, void* buf, int n) {
  struct memf* m = (struct memf*)f;
  if (m->pos + n > m->len) return -1;
  memcpy(m->p + m->pos, buf, n); m->pos += n; return n;
}
static void* m_alloc(struct mspack_system* s, size_t n) { (void)s; return malloc(n); }
static void m_free(void* p) { free(p); }
static void m_copy(void* a, void* b, size_t n) { memmove(b, a, n); }
static void m_msg(struct mspack_file* f, const char* fmt, ...) { (void)f; (void)fmt; }
static struct mspack_system msys = { 0, 0, m_read, m_write, 0, 0, m_msg, m_alloc, m_free, m_copy, 0 };

static unsigned be16(const unsigned char* p) { return (p[0] << 8) | p[1]; }
static unsigned be32(const unsigned char* p) { return ((unsigned)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  FILE* f = fopen(argv[1], "rb"); if (!f) { puts("error open"); return 1; }
  fseek(f, 0, SEEK_END); size_t n = ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char* d = malloc(n + 64); memset(d + n, 0, 64);
  if (fread(d, 1, n, f) != n) { puts("error read"); return 1; }
  fclose(f);
  if (n < 0x14 || be32(d) != 0x0FF512ED) { puts("error magic"); return 1; }
  unsigned flags = be32(d + 0x0C);
  int wb = 15 + ((flags >> 4) & 3);
  unsigned nb = (flags & 0xFFFF) >> 6, zero = !nb; if (zero) nb = 1;
  int keep = argc > 3 && !strcmp(argv[3], "-k"), bad = 0;
  unsigned w = (flags & 0x00400000) ? 32 : 20;
  unsigned* bs = calloc(nb, sizeof(unsigned)); size_t total = 0;
  for (unsigned i = 0; i < nb; i++) {
    unsigned v = 0;
    for (unsigned b = 0; b < w; b++) {
      unsigned bit = i * w + b;
      v = (v << 1) | ((d[0x10 + bit / 8] >> (7 - bit % 8)) & 1);
    }
    bs[i] = v; total += v;
  }
  size_t q = 0x10 + ((nb * w + 31) / 32) * 4;
  unsigned char* out = calloc(total + 1, 1);
  if (zero) {
    f = fopen(argv[2], "wb"); fwrite(out, 1, total, f); fclose(f);
    printf("ok %zu zero\n", total); return 0;
  }
  unsigned char* raw = malloc(n + 64);
  size_t op = 0;
  for (unsigned i = 0; i < nb; i++) {
    if (i) q = (q + 0x1FFFF) & ~(size_t)0x1FFFF;
    size_t rl = 0, left = bs[i];
    while (left) {
      if (q + 2 > n) { printf("error eof block %u\n", i); return 1; }
      unsigned u = left < 0x8000 ? (unsigned)left : 0x8000;
      if (d[q] == 0xFF) { u = be16(d + q + 1); q += 3; }
      unsigned c = be16(d + q); q += 2;
      if (q + c > n || u == 0 || u > left) { printf("error frame block %u\n", i); return 1; }
      memcpy(raw + rl, d + q, c); rl += c; q += c; left -= u;
    }
    // lzxd lee por adelantado al final del bloque: le damos ceros de relleno
    memset(raw + rl, 0, 64);
    struct memf in = { raw, rl + 64, 0 }, o = { out + op, bs[i], 0 };
    struct lzxd_stream* s = lzxd_init(&msys, (struct mspack_file*)&in, (struct mspack_file*)&o, wb, 0, 0x8000, bs[i], 0);
    if (!s) { puts("error init"); return 1; }
    int r = lzxd_decompress(s, bs[i]);
    lzxd_free(s);
    if (r || o.pos != bs[i]) {
      printf("error lzx block %u r=%d out=%zu/%u\n", i, r, o.pos, bs[i]);
      if (!keep) return 1;
      memset(out + op, 0, bs[i]); bad++;
    }
    op += bs[i];
  }
  if (q != n) { printf("error trailing %zu/%zu\n", q, n); return 1; }
  f = fopen(argv[2], "wb"); fwrite(out, 1, total, f); fclose(f);
  printf("ok %zu blocks=%u%s\n", total, nb, bad ? " ZEROED" : "");
  return 0;
}
