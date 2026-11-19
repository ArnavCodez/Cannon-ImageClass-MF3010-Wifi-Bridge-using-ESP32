#include "raster.h"

#include <stdlib.h>
#include <string.h>

// Ink tone curve: light tints are lightened to offset laser dot gain.
// ink = 255 * (1 - lum/255) ^ 1.5
static uint8_t g_lut[256];
static bool g_lutReady;

// 12 x 8 clustered-dot screen. 12 px at 600 dpi = 8 lines at 400 dpi, so the
// cell is square on paper; two dots per cell give a 45-degree ~71 lpi screen.
static const uint8_t g_screen[8][12] = {
    {1, 22, 65, 129, 214, 238, 244, 220, 134, 70, 28, 6},
    {44, 86, 108, 193, 174, 153, 158, 180, 198, 113, 92, 49},
    {150, 172, 196, 110, 89, 46, 52, 94, 116, 201, 177, 156},
    {236, 217, 132, 68, 25, 4, 9, 30, 73, 137, 222, 241},
    {252, 233, 148, 84, 41, 20, 14, 36, 78, 142, 228, 246},
    {166, 188, 212, 126, 105, 62, 57, 100, 121, 206, 182, 161},
    {60, 102, 124, 209, 190, 169, 164, 185, 204, 118, 97, 54},
    {17, 38, 81, 145, 230, 254, 249, 225, 140, 76, 33, 12},
};

static void lut_init(void) {
  if (g_lutReady) return;
  // (1-x)^1.5 = (1-x) * sqrt(1-x), saves pulling in pow()
  for (int g = 0; g < 256; g++) {
    double v = (255 - g) / 255.0;
    double s = v, r = v;  // sqrt by Newton
    if (v > 0)
      for (int i = 0; i < 20; i++) r = 0.5 * (r + s / r);
    else
      r = 0;
    int ink = (int)(255.0 * v * r + 0.5);
    g_lut[g] = (uint8_t)(ink > 255 ? 255 : ink);
  }
  g_lutReady = true;
}

enum { F_NONE, F_PWG, F_URF };
enum { S_SYNC, S_HDR, S_REP, S_CTRL, S_RUN, S_LIT, S_DEAD };
enum { K_GRAY, K_BLACK, K_RGB };

#define PWG_HDR 1796
#define URF_HDR 32

struct rin_s {
  ufr_job_t *job;
  int fmt, st, err;
  uint8_t hdr[PWG_HDR];
  uint32_t hlen, hneed;
  uint32_t pages;

  // current page
  uint32_t w, h, xdpi, ydpi, bpp, bpl, unit;
  int kind;
  uint8_t white;
  uint8_t *row;       // decoded source line (bpl)
  uint8_t *acc;       // ink per source column, merged over source lines
  uint16_t *xmap;     // output column -> source column (0xFFFF = outside)
  uint32_t pos, rep, run, cnt, srcY, outY;
  uint8_t pix[4];
  uint32_t pixLen;
  bool accValid, accInk;
  uint8_t out[UFR_BPL];
};

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

rin_t *rin_new(ufr_job_t *job) {
  lut_init();
  rin_t *r = (rin_t *)calloc(1, sizeof(rin_t));
  if (!r) return NULL;
  r->job = job;
  r->st = S_SYNC;
  r->hneed = 4;
  return r;
}

static void page_free(rin_t *r) {
  free(r->row);
  free(r->acc);
  free(r->xmap);
  r->row = r->acc = NULL;
  r->xmap = NULL;
}

void rin_free(rin_t *r) {
  if (!r) return;
  page_free(r);
  free(r);
}

uint32_t rin_pages(const rin_t *r) { return r->pages; }
const char *rin_format(const rin_t *r) { return r->fmt == F_PWG ? "pwg" : r->fmt == F_URF ? "urf" : "?"; }

static int fail(rin_t *r, int e) {
  r->err = e;
  r->st = S_DEAD;
  return e;
}

// ---------------------------------------------------------------- rendering

// Source lines whose centre falls inside output line oy: [*a, *b).
// Output line oy covers 120 + 1.5*oy .. 120 + 1.5*(oy+1) in 600-dpi units.
// If no centre falls inside (upscaling), the nearest line is used.
static void src_range(const rin_t *r, uint32_t oy, uint32_t *a, uint32_t *b) {
  uint32_t A = (240 + 3 * oy) * r->ydpi;  // 2 * top * ydpi
  uint32_t B = (243 + 3 * oy) * r->ydpi;
  uint32_t ja = (A - 600 + 1199) / 1200;
  uint32_t jb = (B - 600 + 1199) / 1200;
  if (ja >= jb) {
    ja = ((483 + 6 * oy) * r->ydpi) / 2400;
    jb = ja + 1;
  }
  *a = ja;
  *b = jb;
}

static bool emit_line(rin_t *r) {
  uint8_t *o = r->out;
  if (!r->accInk) {
    memset(o, 0, UFR_BPL);
  } else {
    const uint8_t *T = g_screen[r->outY & 7];
    const uint16_t *xm = r->xmap;
    const uint8_t *acc = r->acc;
    uint32_t tx = 0;
    for (uint32_t ob = 0; ob < UFR_BPL; ob++) {
      uint8_t v = 0;
      for (int bit = 0; bit < 8; bit++) {
        uint16_t sx = *xm++;
        uint8_t ink = sx == 0xFFFF ? 0 : acc[sx];
        v = (uint8_t)((v << 1) | (ink > T[tx]));
        if (++tx == 12) tx = 0;
      }
      o[ob] = v;
    }
  }
  r->outY++;
  return ufr_page_line(r->job, o);
}

// Converts the decoded source line to ink and merges it into acc.
static void merge_line(rin_t *r) {
  uint8_t *acc = r->acc;
  const uint8_t *s = r->row;
  bool fresh = !r->accValid;
  bool any = fresh ? false : r->accInk;
  uint32_t w = r->w;
  if (r->bpp == 1) {
    // 1 = black (K) ; gray-1 would be 1 = white
    uint8_t flip = r->kind == K_BLACK ? 0x00 : 0xFF;
    for (uint32_t x = 0; x < w; x++) {
      uint8_t ink = (((s[x >> 3] ^ flip) >> (7 - (x & 7))) & 1) ? 255 : 0;
      if (fresh || ink > acc[x]) acc[x] = ink;
      any |= ink != 0;
    }
  } else if (r->kind == K_RGB) {
    uint32_t step = r->bpp / 8;
    for (uint32_t x = 0; x < w; x++, s += step) {
      uint32_t lum = (s[0] * 77u + s[1] * 150u + s[2] * 29u) >> 8;
      uint8_t ink = g_lut[lum];
      if (fresh || ink > acc[x]) acc[x] = ink;
      any |= ink != 0;
    }
  } else {
    uint32_t step = r->bpp / 8;  // 8 or 16 bit: use the high byte
    bool blk = r->kind == K_BLACK;
    for (uint32_t x = 0; x < w; x++, s += step) {
      uint8_t ink = g_lut[blk ? 255 - s[0] : s[0]];
      if (fresh || ink > acc[x]) acc[x] = ink;
      any |= ink != 0;
    }
  }
  r->accInk = any;
  r->accValid = true;
}

// A complete source line is in r->row: route it to the output lines.
static bool source_line(rin_t *r) {
  uint32_t j = r->srcY++;
  bool merged = false;
  while (r->outY < UFR_PAGE_H) {
    uint32_t a, b;
    src_range(r, r->outY, &a, &b);
    if (a > j) break;  // not needed yet
    if (!merged) {
      merge_line(r);
      merged = true;
    }
    if (j + 1 < b) break;  // more source lines belong to this output line
    if (!emit_line(r)) return false;
    // next output line may reuse this source line (upscaling)
    uint32_t a2, b2;
    if (r->outY >= UFR_PAGE_H) break;
    src_range(r, r->outY, &a2, &b2);
    if (a2 > j) {
      r->accValid = false;
      break;
    }
  }
  return true;
}

// ---------------------------------------------------------------- headers

static int page_begin(rin_t *r) {
  const uint8_t *h = r->hdr;
  uint32_t cs;
  if (r->fmt == F_PWG) {
    r->xdpi = be32(h + 276);
    r->ydpi = be32(h + 280);
    r->w = be32(h + 372);
    r->h = be32(h + 376);
    uint32_t bpc = be32(h + 384);
    r->bpp = be32(h + 388);
    r->bpl = be32(h + 392);
    cs = be32(h + 400);
    // 0 W, 18 sGray: gray ; 3 K: black ; 1 RGB, 19 sRGB, 20 AdobeRGB
    if (cs == 0 || cs == 18)
      r->kind = K_GRAY;
    else if (cs == 3)
      r->kind = K_BLACK;
    else if (cs == 1 || cs == 19 || cs == 20)
      r->kind = K_RGB;
    else
      return RIN_ERR_HEADER;
    if (r->kind == K_RGB ? (bpc != 8 || r->bpp != 24) : !(r->bpp == 1 || r->bpp == 8 || r->bpp == 16))
      return RIN_ERR_HEADER;
  } else {
    r->bpp = h[0];
    cs = h[1];
    r->w = be32(h + 12);
    r->h = be32(h + 16);
    r->xdpi = r->ydpi = be32(h + 20);
    // 0 sGray, 1 sRGB, 3 AdobeRGB, 4 W, 5 RGB
    if ((cs == 0 || cs == 4) && r->bpp == 8)
      r->kind = K_GRAY;
    else if ((cs == 1 || cs == 3 || cs == 5) && r->bpp == 24)
      r->kind = K_RGB;
    else
      return RIN_ERR_HEADER;
    r->bpl = r->w * r->bpp / 8;
  }
  if (r->w == 0 || r->h == 0 || r->w > 20000 || r->h > 40000 || r->xdpi < 72 || r->ydpi < 72 ||
      r->xdpi > 2400 || r->ydpi > 2400)
    return RIN_ERR_HEADER;
  if (r->bpl < (r->w * r->bpp + 7) / 8 || r->bpl > 65536) return RIN_ERR_HEADER;
  r->unit = r->bpp < 8 ? 1 : r->bpp / 8;
  r->white = r->kind == K_BLACK ? 0x00 : 0xFF;

  page_free(r);
  r->row = (uint8_t *)malloc(r->bpl);
  r->acc = (uint8_t *)malloc(r->w);
  r->xmap = (uint16_t *)malloc(UFR_PAGE_W * sizeof(uint16_t));
  if (!r->row || !r->acc || !r->xmap) return RIN_ERR_MEM;
  for (uint32_t ox = 0; ox < UFR_PAGE_W; ox++) {
    uint32_t sx = ((241 + 2 * ox) * r->xdpi) / 1200;
    r->xmap[ox] = sx < r->w && sx < 0xFFFF ? (uint16_t)sx : 0xFFFF;
  }
  r->srcY = r->outY = 0;
  r->accValid = r->accInk = false;
  r->cnt = 0;
  r->pages++;
  if (!ufr_page_begin(r->job)) return RIN_ERR_OUTPUT;
  return RIN_OK;
}

static int page_end(rin_t *r) {
  page_free(r);
  if (!ufr_page_end(r->job)) return RIN_ERR_OUTPUT;
  return RIN_OK;
}

static void next_header(rin_t *r) {
  r->st = S_HDR;
  r->hlen = 0;
  r->hneed = r->fmt == F_PWG ? PWG_HDR : URF_HDR;
}

// Line r->row complete: emit it r->rep times.
static int line_done(rin_t *r) {
  while (r->rep-- > 0 && r->cnt < r->h) {
    r->cnt++;
    if (!source_line(r)) return RIN_ERR_OUTPUT;
  }
  if (r->cnt >= r->h) {
    int e = page_end(r);
    if (e) return e;
    next_header(r);
  } else {
    r->st = S_REP;
  }
  return RIN_OK;
}

static void put_pixels(rin_t *r, const uint8_t *px, uint32_t count) {
  uint32_t u = r->unit;
  while (count-- && r->pos < r->bpl) {
    uint32_t n = r->bpl - r->pos < u ? r->bpl - r->pos : u;
    memcpy(r->row + r->pos, px, n);
    r->pos += n;
  }
}

int rin_feed(rin_t *r, const uint8_t *d, size_t n) {
  if (r->st == S_DEAD) return r->err ? r->err : RIN_ERR_FORMAT;
  const uint8_t *end = d + n;
  int e;
  while (d < end) {
    switch (r->st) {
      case S_SYNC:
        r->hdr[r->hlen++] = *d++;
        if (r->hlen == 4) {
          if (!memcmp(r->hdr, "RaS2", 4)) {
            r->fmt = F_PWG;
            next_header(r);
          } else if (!memcmp(r->hdr, "UNIR", 4)) {
            r->fmt = F_URF;
            r->hneed = 12;  // "UNIRAST\0" + page count
          } else {
            return fail(r, RIN_ERR_FORMAT);
          }
        } else if (r->hlen == 12) {
          if (memcmp(r->hdr, "UNIRAST", 8)) return fail(r, RIN_ERR_FORMAT);
          next_header(r);
        }
        break;

      case S_HDR: {
        uint32_t k = r->hneed - r->hlen;
        if ((size_t)(end - d) < k) k = (uint32_t)(end - d);
        memcpy(r->hdr + r->hlen, d, k);
        r->hlen += k;
        d += k;
        if (r->hlen == r->hneed) {
          if ((e = page_begin(r))) return fail(r, e);
          r->st = S_REP;
        }
        break;
      }

      case S_REP:
        r->rep = (uint32_t)*d++ + 1;
        r->pos = 0;
        r->st = S_CTRL;
        break;

      case S_CTRL: {
        uint8_t c = *d++;
        if (c == 128) {  // rest of line is white
          memset(r->row + r->pos, r->white, r->bpl - r->pos);
          r->pos = r->bpl;
        } else if (c < 128) {
          r->pixLen = 0;
          r->run = c + 1u;
          r->st = S_RUN;
          break;
        } else {
          r->run = (257u - c) * r->unit;  // literal bytes to copy
          r->st = S_LIT;
          break;
        }
        if (r->pos >= r->bpl && (e = line_done(r))) return fail(r, e);
        break;
      }

      case S_RUN:
        r->pix[r->pixLen++] = *d++;
        if (r->pixLen == r->unit) {
          put_pixels(r, r->pix, r->run);
          r->st = S_CTRL;
          if (r->pos >= r->bpl && (e = line_done(r))) return fail(r, e);
        }
        break;

      case S_LIT: {
        uint32_t k = r->run;
        if ((size_t)(end - d) < k) k = (uint32_t)(end - d);
        uint32_t room = r->bpl - r->pos;
        uint32_t c = k < room ? k : room;
        memcpy(r->row + r->pos, d, c);
        r->pos += c;
        d += k;
        r->run -= k;
        if (r->run == 0) {
          r->st = S_CTRL;
          if (r->pos >= r->bpl && (e = line_done(r))) return fail(r, e);
        }
        break;
      }

      default:
        return fail(r, RIN_ERR_FORMAT);
    }
  }
  return RIN_OK;
}

int rin_finish(rin_t *r) {
  if (r->st == S_DEAD) return r->err;
  if (r->st == S_REP || r->st == S_CTRL || r->st == S_RUN || r->st == S_LIT) {
    // truncated page: print what arrived
    int e = page_end(r);
    if (e) return fail(r, e);
  }
  r->st = S_DEAD;
  r->err = RIN_OK;
  return RIN_OK;
}
