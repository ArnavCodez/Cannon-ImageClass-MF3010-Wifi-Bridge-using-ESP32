// Streaming JBIG1 encoder, see jbig_stream.h.
// Arithmetic coder and tables after JBIG-KIT (Markus Kuhn, GPL-2.0-or-later).
#include "jbig_stream.h"

#include <string.h>

#define TPB2CX 0x195
#define MARKER_STUFF 0x00
#define MARKER_SDNORM 0x02
#define MARKER_SDRST 0x03
#define MARKER_ESC 0xff

static const uint16_t lsztab[113] = {
    0x5a1d, 0x2586, 0x1114, 0x080b, 0x03d8, 0x01da, 0x00e5, 0x006f, 0x0036, 0x001a, 0x000d, 0x0006,
    0x0003, 0x0001, 0x5a7f, 0x3f25, 0x2cf2, 0x207c, 0x17b9, 0x1182, 0x0cef, 0x09a1, 0x072f, 0x055c,
    0x0406, 0x0303, 0x0240, 0x01b1, 0x0144, 0x00f5, 0x00b7, 0x008a, 0x0068, 0x004e, 0x003b, 0x002c,
    0x5ae1, 0x484c, 0x3a0d, 0x2ef1, 0x261f, 0x1f33, 0x19a8, 0x1518, 0x1177, 0x0e74, 0x0bfb, 0x09f8,
    0x0861, 0x0706, 0x05cd, 0x04de, 0x040f, 0x0363, 0x02d4, 0x025c, 0x01f8, 0x01a4, 0x0160, 0x0125,
    0x00f6, 0x00cb, 0x00ab, 0x008f, 0x5b12, 0x4d04, 0x412c, 0x37d8, 0x2fe8, 0x293c, 0x2379, 0x1edf,
    0x1aa9, 0x174e, 0x1424, 0x119c, 0x0f6b, 0x0d51, 0x0bb6, 0x0a40, 0x5832, 0x4d1c, 0x438e, 0x3bdd,
    0x34ee, 0x2eae, 0x299a, 0x2516, 0x5570, 0x4ca9, 0x44d9, 0x3e22, 0x3824, 0x32b4, 0x2e17, 0x56a8,
    0x4f46, 0x47e5, 0x41cf, 0x3c3d, 0x375e, 0x5231, 0x4c0f, 0x4639, 0x415e, 0x5627, 0x50e7, 0x4b85,
    0x5597, 0x504f, 0x5a10, 0x5522, 0x59eb};

static const uint8_t nmpstab[113] = {
    1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,  13, 15,  16,  17,  18,  19,  20,  21,  22,  23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 9,   37, 38,  39,  40,  41,  42,  43,  44,  45,  46,
    47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,  60, 61,  62,  63,  32,  65,  66,  67,  68,  69,
    70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 48, 81, 82,  83, 84,  85,  86,  87,  71,  89,  90,  91,  92,
    93, 94, 86, 96, 97, 98, 99, 100, 93, 102, 103, 104, 99, 106, 107, 103, 109, 107, 111, 109, 111};

// low 7 bits: NLPS, bit 7: SWTCH
static const uint8_t nlpstab[113] = {
    129, 14,  16,  18,  20,  23,  25,  28,  30,  33,  35,  9,   10,  12,  143, 36,  38,  39,  40,
    42,  43,  45,  46,  48,  49,  51,  52,  54,  56,  57,  59,  60,  62,  63,  32,  33,  165, 64,
    65,  67,  68,  69,  70,  72,  73,  74,  75,  77,  78,  79,  48,  50,  50,  51,  52,  53,  54,
    55,  56,  57,  58,  59,  61,  61,  193, 80,  81,  82,  83,  84,  86,  87,  87,  72,  72,  74,
    74,  75,  77,  77,  208, 88,  89,  90,  91,  92,  93,  86,  216, 95,  96,  97,  99,  99,  93,
    223, 101, 102, 103, 104, 99,  105, 106, 107, 103, 233, 108, 109, 110, 111, 238, 112, 240};

// ---- byte output: hold back 0x00 bytes so trailing ones can be dropped ----

static void put_byte(jbs_t *s, uint8_t b) {
  if (b == 0) {
    s->zeros++;
    return;
  }
  for (; s->zeros; s->zeros--) s->out(0, s->ctx);
  s->out(b, s->ctx);
  s->last_ff = (b == 0xff);
}

// ---- arithmetic encoder ----

static void ar_init(jbs_t *s, int reuse_st) {
  if (!reuse_st) memset(s->st, 0, sizeof s->st);
  s->c = 0;
  s->a = 0x10000UL;
  s->sc = 0;
  s->ct = 11;
  s->buffer = -1;
}

static void ar_flush(jbs_t *s) {
  uint32_t temp;
  if ((temp = (s->a - 1 + s->c) & 0xffff0000UL) < s->c)
    s->c = temp + 0x8000;
  else
    s->c = temp;
  s->c <<= s->ct;
  if (s->c & 0xf8000000UL) {
    if (s->buffer >= 0) {
      put_byte(s, (uint8_t)(s->buffer + 1));
      if (s->buffer + 1 == MARKER_ESC) put_byte(s, MARKER_STUFF);
    }
    if (s->c & 0x7fff800UL)
      for (; s->sc; --s->sc) put_byte(s, 0x00);
  } else {
    if (s->buffer >= 0) put_byte(s, (uint8_t)s->buffer);
    for (; s->sc; --s->sc) {
      put_byte(s, 0xff);
      put_byte(s, MARKER_STUFF);
    }
  }
  if (s->c & 0x7fff800UL) {
    put_byte(s, (uint8_t)((s->c >> 19) & 0xff));
    if (((s->c >> 19) & 0xff) == MARKER_ESC) put_byte(s, MARKER_STUFF);
    if (s->c & 0x7f800UL) {
      put_byte(s, (uint8_t)((s->c >> 11) & 0xff));
      if (((s->c >> 11) & 0xff) == MARKER_ESC) put_byte(s, MARKER_STUFF);
    }
  }
}

static inline void ar_encode(jbs_t *s, int cx, int pix) {
  uint8_t *st = s->st + cx;
  unsigned ss = *st & 0x7f;
  uint32_t lsz = lsztab[ss];

  if (((pix << 7) ^ *st) & 0x80) {
    // less probable symbol
    if ((s->a -= lsz) >= lsz) {
      s->c += s->a;
      s->a = lsz;
    }
    *st &= 0x80;
    *st ^= nlpstab[ss];
  } else {
    // more probable symbol
    if ((s->a -= lsz) & 0xffff8000UL) return;
    if (s->a < lsz) {
      s->c += s->a;
      s->a = lsz;
    }
    *st &= 0x80;
    *st |= nmpstab[ss];
  }

  do {
    s->a <<= 1;
    s->c <<= 1;
    --s->ct;
    if (s->ct == 0) {
      uint32_t temp = s->c >> 19;
      if (temp & 0xffffff00UL) {
        if (s->buffer >= 0) {
          ++s->buffer;
          put_byte(s, (uint8_t)s->buffer);
          if (s->buffer == MARKER_ESC) put_byte(s, MARKER_STUFF);
        }
        for (; s->sc; --s->sc) put_byte(s, 0x00);
        s->buffer = (int32_t)(temp & 0xff);
      } else if (temp == 0xff) {
        ++s->sc;
      } else {
        if (s->buffer >= 0) put_byte(s, (uint8_t)s->buffer);
        for (; s->sc; --s->sc) {
          put_byte(s, 0xff);
          put_byte(s, MARKER_STUFF);
        }
        s->buffer = (int32_t)temp;
      }
      s->c &= 0x7ffffUL;
      s->ct = 8;
    }
  } while (s->a < 0x8000);
}

// ---- band encoder ----

void jbs_bih(uint8_t bih[20], uint32_t xd, uint32_t yd) {
  memset(bih, 0, 20);
  bih[2] = 1;  // P = 1 plane
  bih[4] = xd >> 24; bih[5] = xd >> 16; bih[6] = xd >> 8; bih[7] = xd;
  bih[8] = yd >> 24; bih[9] = yd >> 16; bih[10] = yd >> 8; bih[11] = yd;
  bih[15] = JBS_L0;
  bih[19] = 0x48;  // LRLTWO | TPBON
}

void jbs_begin(jbs_t *s, uint32_t width, uint32_t lines, jbs_out_fn out, void *ctx) {
  s->out = out;
  s->ctx = ctx;
  s->width = width;
  s->bpl = (width + 7) / 8;
  s->lines = lines;
  s->y = 0;
  s->stripe = 0;
  s->stripe_line = 0;
  s->ltp_old = 0;
  memset(s->prev1, 0, sizeof s->prev1);
  memset(s->prev2, 0, sizeof s->prev2);
}

static void stripe_start(jbs_t *s) {
  int reset = (s->stripe == 0);
  ar_init(s, !reset);
  s->zeros = 0;
  s->last_ff = 0;
  if (reset) {
    s->ltp_old = 0;
  } else if (s->y > 1) {
    s->ltp_old = memcmp(s->prev1, s->prev2, s->bpl) == 0;
  } else {
    s->ltp_old = 1;
    for (uint32_t k = 0; k < s->bpl; k++)
      if (s->prev1[k]) {
        s->ltp_old = 0;
        break;
      }
  }
}

static void stripe_end(jbs_t *s, int last) {
  ar_flush(s);
  s->zeros = 0;  // drop trailing zero bytes
  if (s->last_ff) s->out(MARKER_STUFF, s->ctx);
  s->out(MARKER_ESC, s->ctx);
  s->out(last ? MARKER_SDRST : MARKER_SDNORM, s->ctx);
}

void jbs_line(jbs_t *s, const uint8_t *row) {
  if (s->y >= s->lines) return;
  if (s->stripe_line == 0) stripe_start(s);

  const uint32_t bpl = s->bpl;
  const int has1 = s->y > 0;

  // typical prediction: is this line identical to the one above?
  int ltp = 1;
  if (has1) {
    ltp = memcmp(row, s->prev1, bpl) == 0;
  } else {
    for (uint32_t k = 0; k < bpl; k++)
      if (row[k]) {
        ltp = 0;
        break;
      }
  }
  ar_encode(s, TPB2CX, ltp == s->ltp_old);
  s->ltp_old = ltp;

  if (!ltp) {
    // two-line template, no adaptive template movement
    uint32_t line_h1 = 0, line_h2 = 0;
    if (has1) line_h2 = (uint32_t)s->prev1[0] << 8;
    uint32_t j = 0;
    for (uint32_t k = 0; j < s->width; k++) {
      line_h1 |= row[k];
      if (j < bpl * 8 - 8 && has1) line_h2 |= s->prev1[k + 1];
      do {
        line_h1 <<= 1;
        line_h2 <<= 1;
        ar_encode(s, (int)(((line_h2 >> 10) & 0x3f0) | ((line_h1 >> 9) & 0x00f)), (int)((line_h1 >> 8) & 1));
      } while ((++j & 7) && j < s->width);
    }
  }

  memcpy(s->prev2, s->prev1, bpl);
  memcpy(s->prev1, row, bpl);
  s->y++;
  s->stripe_line++;
  int last_line = (s->y == s->lines);
  if (s->stripe_line == JBS_L0 || last_line) {
    stripe_end(s, last_line);
    s->stripe_line = 0;
    s->stripe++;
  }
}
