// JBIG1 (T.82) encoder that works line by line.
//
// Same output as JBIG-KIT with 1 plane, no resolution reduction, LRLTWO |
// TPBON, L0 = 128, MX = MY = 0. JBIG-KIT wants the whole image in RAM though
// (~150 KB for one band here), this only keeps the last two lines.
//
// The MF3010 gets every 256-line band as its own JBIG image, and the last
// stripe ends with SDRST (FF 03) instead of SDNORM. That's what this writes.
// The 20-byte BIH is not written by jbs_line(), use jbs_bih() for that.
//
// Arithmetic coder and probability tables are from JBIG-KIT by Markus Kuhn
// (GPL-2.0-or-later), https://www.cl.cam.ac.uk/~mgk25/jbigkit/
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JBS_L0 128
#define JBS_MAX_BPL 1024

typedef void (*jbs_out_fn)(uint8_t b, void *ctx);

typedef struct {
  uint8_t st[4096];
  uint32_t c, a;
  int32_t sc, ct, buffer;
  jbs_out_fn out;
  void *ctx;
  uint32_t zeros;
  int last_ff;
  uint32_t width, bpl, lines, y, stripe, stripe_line;
  int ltp_old;
  uint8_t prev1[JBS_MAX_BPL];
  uint8_t prev2[JBS_MAX_BPL];
} jbs_t;

// Start a band of `lines` lines, `width` pixels wide (width <= 8*JBS_MAX_BPL).
void jbs_begin(jbs_t *s, uint32_t width, uint32_t lines, jbs_out_fn out, void *ctx);
// Feed the next line (bpl = ceil(width/8) bytes, MSB = leftmost, 1 = black).
void jbs_line(jbs_t *s, const uint8_t *row);
// Write a 20-byte BIH for an image of xd x yd pixels with this encoder's options.
void jbs_bih(uint8_t bih[20], uint32_t xd, uint32_t yd);

#ifdef __cplusplus
}
#endif
