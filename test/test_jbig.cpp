// Checks jbig_stream against JBIG-KIT on random images and real pages.
// needs libjbig-dev. Optional args: PBM pages to also test band by band.
extern "C" {
#include <jbig.h>
}
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jbig_stream.h"

typedef struct {
  uint8_t *d;
  size_t n, cap;
} buf_t;
static void bput(buf_t *b, uint8_t c) {
  if (b->n == b->cap) {
    b->cap = b->cap * 2 + 1024;
    b->d = (uint8_t *)realloc(b->d, b->cap);
  }
  b->d[b->n++] = c;
}
static void sink_ref(unsigned char *p, size_t n, void *f) {
  for (size_t i = 0; i < n; i++) bput((buf_t *)f, p[i]);
}
static void sink_jbs(uint8_t c, void *f) { bput((buf_t *)f, c); }

// reference: JBIG-KIT, BIH stripped, final SDNORM patched to SDRST
static void ref_band(buf_t *o, uint8_t *rows, unsigned w, unsigned h) {
  unsigned char *planes[1] = {rows};
  struct jbg_enc_state se;
  buf_t t = {0};
  jbg_enc_init(&se, w, h, 1, planes, sink_ref, &t);
  jbg_enc_options(&se, 0, JBG_LRLTWO | JBG_TPBON, 128, 0, 0);
  jbg_enc_out(&se);
  jbg_enc_free(&se);
  if (t.n >= 22 && t.d[t.n - 2] == 0xff && t.d[t.n - 1] == 0x02) t.d[t.n - 1] = 0x03;
  for (size_t i = 20; i < t.n; i++) bput(o, t.d[i]);
  free(t.d);
}

static void jbs_band(buf_t *o, const uint8_t *rows, unsigned w, unsigned h) {
  static jbs_t s;
  size_t bpl = (w + 7) / 8;
  jbs_begin(&s, w, h, sink_jbs, o);
  for (unsigned y = 0; y < h; y++) jbs_line(&s, rows + y * bpl);
}

static unsigned rnd_state = 12345;
static unsigned rnd(void) {
  rnd_state = rnd_state * 1103515245u + 12345u;
  return rnd_state >> 8;
}

static int compare(const char *what, uint8_t *rows, unsigned w, unsigned h) {
  buf_t a = {0}, b = {0};
  ref_band(&a, rows, w, h);
  jbs_band(&b, rows, w, h);
  int ok = a.n == b.n && memcmp(a.d, b.d, a.n) == 0;
  if (!ok) {
    size_t k = 0;
    while (k < a.n && k < b.n && a.d[k] == b.d[k]) k++;
    printf("MISMATCH %s: ref %zu bytes, jbs %zu bytes, first diff at %zu\n", what, a.n, b.n, k);
  }
  free(a.d);
  free(b.d);
  return ok;
}

static uint8_t *read_pbm(const char *path, unsigned *w, unsigned *h) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  char m[3] = {0};
  if (fscanf(f, "%2s %u %u", m, w, h) != 3) return NULL;
  fgetc(f);
  size_t n = (size_t)((*w + 7) / 8) * *h;
  uint8_t *d = (uint8_t *)malloc(n);
  if (fread(d, 1, n, f) != n) return NULL;
  fclose(f);
  return d;
}

int main(int argc, char **argv) {
  int pass = 0, fail = 0;
  // random images of various kinds and sizes
  const unsigned widths[] = {8, 16, 100, 590 * 8, 4720, 4721, 7, 1};
  for (int t = 0; t < 400; t++) {
    unsigned w = widths[t % 8];
    unsigned h = 1 + rnd() % 300;
    size_t bpl = (w + 7) / 8;
    uint8_t *img = (uint8_t *)calloc(bpl, h);
    int kind = t % 5;
    for (unsigned y = 0; y < h; y++) {
      for (size_t x = 0; x < bpl; x++) {
        uint8_t v = 0;
        if (kind == 0) v = (uint8_t)rnd();                                  // noise
        if (kind == 1) v = (rnd() % 23 == 0) ? (uint8_t)rnd() : 0;          // sparse
        if (kind == 2) v = ((y / 7) % 3 == 0) ? (uint8_t)(x * 37 + y) : 0;  // text-like stripes
        if (kind == 3) v = (y % 9 < 4) ? 0xff : 0x00;                       // repeated lines
        if (kind == 4) v = (rnd() % 3) ? 0xff : (uint8_t)rnd();             // mostly black
        img[y * bpl + x] = v;
      }
      if (w & 7) img[y * bpl + bpl - 1] &= (uint8_t)(0xff << (8 - (w & 7)));
      if (kind == 3 && y > 0 && rnd() % 2) memcpy(img + y * bpl, img + (y - 1) * bpl, bpl);
    }
    char what[64];
    snprintf(what, sizeof what, "random kind=%d w=%u h=%u", kind, w, h);
    if (compare(what, img, w, h)) pass++; else fail++;
    free(img);
  }
  // real pages, cut into 256-line bands like the printer job
  for (int i = 1; i < argc; i++) {
    unsigned w, h;
    uint8_t *pg = read_pbm(argv[i], &w, &h);
    if (!pg) {
      printf("cannot read %s\n", argv[i]);
      fail++;
      continue;
    }
    size_t bpl = (w + 7) / 8;
    for (unsigned y = 0; y < h; y += 256) {
      unsigned bh = h - y < 256 ? h - y : 256;
      char what[300];
      snprintf(what, sizeof what, "%s band y=%u", argv[i], y);
      if (compare(what, pg + y * bpl, w, bh)) pass++; else fail++;
    }
    free(pg);
  }
  printf("pass=%d fail=%d\n", pass, fail);
  return fail != 0;
}
