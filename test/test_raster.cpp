// Push a PWG/URF file through the raster decoder in random sized pieces (like
// it arrives over the network) and write the resulting printer job.
// usage: test_raster in.ras out.prn [seed]
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "raster.h"

static bool fwrite_cb(const uint8_t *d, size_t n, void *ctx) { return fwrite(d, 1, n, (FILE *)ctx) == n; }

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  srand(argc > 3 ? atoi(argv[3]) : 1);
  FILE *f = fopen(argv[1], "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = (uint8_t *)malloc(n);
  if (fread(b, 1, n, f) != (size_t)n) return 1;
  fclose(f);
  FILE *o = fopen(argv[2], "wb");
  static ufr_job_t j;
  uint8_t t8[8];
  ufr_time8(t8, 2026, 11, 17, 2, 10, 0, 0, 0);
  bool ok = ufr_job_begin(&j, fwrite_cb, o, "test", "phone", "MF3010-BRIDGE", 1, t8, 1);
  rin_t *r = rin_new(&j);
  clock_t c0 = clock();
  int e = 0;
  for (long i = 0; ok && i < n && !e;) {
    long k = 1 + rand() % 3000;
    if (rand() % 8 == 0) k = 1;
    if (k > n - i) k = n - i;
    e = rin_feed(r, b + i, k);
    i += k;
  }
  if (!e) e = rin_finish(r);
  ok = ok && !e && ufr_job_end(&j);
  double sec = (double)(clock() - c0) / CLOCKS_PER_SEC;
  printf("%s: fmt=%s err=%d pages=%u out=%u bytes, %.2fs\n", argv[1], rin_format(r), e, rin_pages(r), j.total,
         sec);
  rin_free(r);
  ufr_job_free(&j);
  fclose(o);
  return !ok;
}
