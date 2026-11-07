// Prueba: descomprimir un archivo XCTD (0FF512ED) con libmspack lzxd.
// uso: xctdtest in out offset window_bits [frame_mode]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mspack.h"
#include "lzx.h"
extern struct mspack_system *mspack_default_system;

int main(int argc, char** argv) {
  if (argc < 5) return 2;
  long off = strtol(argv[3], 0, 0);
  int wb = atoi(argv[4]);
  FILE* f = fopen(argv[1], "rb");
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, off, SEEK_SET);
  unsigned char* buf = malloc(n);
  long m = fread(buf, 1, n - off, f); fclose(f);
  FILE* t = fopen("slice.bin", "wb"); fwrite(buf, 1, m, t); fclose(t);
  struct mspack_system* sys = mspack_default_system;
  struct mspack_file* in = sys->open(sys, "slice.bin", MSPACK_SYS_OPEN_READ);
  struct mspack_file* out = sys->open(sys, argv[2], MSPACK_SYS_OPEN_WRITE);
  struct lzxd_stream* s = lzxd_init(sys, in, out, wb, (argc>6?atoi(argv[6]):0), 0x8000, (argc>5?strtol(argv[5],0,0):0x1000000), 0);
  int r = lzxd_decompress(s, (argc>5?strtol(argv[5],0,0):0x1000000));
  printf("result=%d out=%lld\n", r, (long long)s->offset);
  lzxd_free(s); sys->close(in); sys->close(out);
  return 0;
}
