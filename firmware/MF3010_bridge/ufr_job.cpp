// Canon MF3010 job writer, see ufr_job.h.
// Format worked out from MF3010 captures, building on the MF4700 notes in
// https://github.com/pkhr/mf4700-print-scan (FORMAT.md).
#include "ufr_job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- output

static bool out(ufr_job_t *j, const uint8_t *d, size_t n) {
  if (j->failed) return false;
  if (n && !j->write(d, n, j->ctx)) {
    j->failed = true;
    return false;
  }
  j->total += (uint32_t)n;
  return true;
}

static bool packet(ufr_job_t *j, uint8_t kind, uint16_t op, const uint8_t *pl, size_t n) {
  uint8_t h[20] = {0xcd, 0xca, 0x10, kind, (uint8_t)(op >> 8), (uint8_t)op, 0x00, 0x01,
                   (uint8_t)(n >> 8), (uint8_t)n, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0};
  return out(j, h, 20) && out(j, pl, n);
}

// PDL bytes go in data packets: payload = 01 + bytes. `head` is written
// before the chunked `body` inside the same packet group, split if very large.
static bool data(ufr_job_t *j, const uint8_t *head, size_t hn, uint8_t *const *body, size_t bn) {
  const size_t maxpl = 65000;
  size_t total = hn + bn, done = 0;
  do {
    size_t n = total - done;
    if (n > maxpl - 1) n = maxpl - 1;
    uint8_t h[21] = {0xcd, 0xca, 0x10, 0x02, 0x00, 0x1a, 0x00, 0x01, (uint8_t)((n + 1) >> 8), (uint8_t)(n + 1),
                     0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    if (!out(j, h, 21)) return false;
    size_t left = n;
    while (left) {
      size_t k;
      if (done < hn) {
        k = hn - done < left ? hn - done : left;
        if (!out(j, head + done, k)) return false;
      } else {
        size_t off = done - hn, room = UFR_CHUNK - off % UFR_CHUNK;
        k = left < room ? left : room;
        if (!out(j, body[off / UFR_CHUNK] + off % UFR_CHUNK, k)) return false;
      }
      done += k;
      left -= k;
    }
  } while (done < total);
  return true;
}

// ---------------------------------------------------------------- helpers

typedef struct {
  uint8_t d[320];
  size_t n;
} small_t;

static void s8(small_t *b, unsigned v) {
  if (b->n < sizeof b->d) b->d[b->n++] = (uint8_t)v;
}
static void s16(small_t *b, unsigned v) {
  s8(b, v >> 8);
  s8(b, v);
}
static void sstr(small_t *b, const char *str, size_t max) {
  size_t n = strlen(str);
  if (n > max) n = max;
  s16(b, 0x0004);  // ISO-8859-1
  s8(b, n);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)str[i];
    s8(b, c < 0x80 ? c : '?');
  }
}

static bool attr(ufr_job_t *j, uint16_t op, const uint8_t *a, size_t n) { return packet(j, 0x00, op, a, n); }

#define ATTR(op, ...)                          \
  do {                                         \
    static const uint8_t a_[] = {__VA_ARGS__}; \
    if (!attr(j, op, a_, sizeof a_)) return false; \
  } while (0)

void ufr_time8(uint8_t t[8], int year, int month, int day, int wday, int hour, int min, int sec, int ms) {
  uint32_t d = ((uint32_t)year << 20) | ((uint32_t)month << 16) | ((uint32_t)day << 11) | ((uint32_t)wday << 8);
  unsigned m = (unsigned)(sec * 1000 + ms);
  t[0] = d >> 24; t[1] = d >> 16; t[2] = d >> 8; t[3] = d;
  t[4] = (uint8_t)hour; t[5] = (uint8_t)min; t[6] = (uint8_t)(m >> 8); t[7] = (uint8_t)m;
}

// ---------------------------------------------------------------- job

bool ufr_job_begin(ufr_job_t *j, ufr_write_fn write, void *ctx, const char *jobName, const char *user,
                   const char *host, uint16_t copies, const uint8_t *time8, uint32_t jobNumber) {
  memset(j, 0, sizeof *j);
  j->write = write;
  j->ctx = ctx;
  j->copies = copies ? copies : 1;
  if (!jobName || !*jobName) jobName = "Document";

  small_t p = {{0}, 0};
  s16(&p, user && *user ? 9 : 8);
  s16(&p, 0x00f0); s16(&p, 1); s8(&p, 0x01);
  s16(&p, 0x0130); s16(&p, 3); s8(&p, 0x03); s16(&p, 0x0000);
  {
    small_t s = {{0}, 0};
    sstr(&s, jobName, 32);
    s16(&p, 0x0004); s16(&p, s.n);
    for (size_t i = 0; i < s.n; i++) s8(&p, s.d[i]);
    s16(&p, 0x0117); s16(&p, s.n);
    for (size_t i = 0; i < s.n; i++) s8(&p, s.d[i]);
    if (user && *user) {
      s.n = 0;
      sstr(&s, user, 32);
      s16(&p, 0x0006); s16(&p, s.n);
      for (size_t i = 0; i < s.n; i++) s8(&p, s.d[i]);
    }
  }
  s16(&p, 0x000d); s16(&p, 1); s8(&p, 0x04);
  {
    static const uint8_t drv[16] = {0x8b, 0xf2, 0x3a, 0x27, 0x2e, 0x1e, 0x44, 0xd4,
                                    0xb2, 0x1f, 0x57, 0x32, 0x97, 0xd7, 0x93, 0xab};
    uint8_t f[64] = {0};
    memcpy(f, drv, 16);
    char num[17];
    snprintf(num, sizeof num, "%016lu", (unsigned long)jobNumber);
    memcpy(f + 16, num, 16);
    size_t hn = host ? strlen(host) : 0;
    if (hn > 15) hn = 15;
    if (hn) {
      memcpy(f + 32, host, hn);
      memcpy(f + 48, host, hn);
    }
    s16(&p, 0x0118); s16(&p, 64);
    for (int i = 0; i < 64; i++) s8(&p, f[i]);
  }
  s16(&p, 0x000c); s16(&p, 1); s8(&p, 0x32);
  s16(&p, 0x0009); s16(&p, 8);
  for (int i = 0; i < 8; i++) s8(&p, time8 ? time8[i] : 0);
  if (!attr(j, 0x6b, p.d, p.n)) return false;

  ATTR(0x12, 0x07, 0xd7, 0x00, 0x01);  // job copies stay 1, the binder carries the count
  uint8_t cp[4] = {0x07, 0xd7, (uint8_t)(j->copies >> 8), (uint8_t)j->copies};
  ATTR(0x14, 0x00, 0x00, 0x00, 0x00);
  ATTR(0x15, 0x08, 0xb3, 0x02);
  {
    small_t s = {{0}, 0};
    s16(&s, 0x082b);
    sstr(&s, jobName, 32);
    if (!attr(j, 0x15, s.d, s.n)) return false;
  }
  if (!attr(j, 0x15, cp, 4)) return false;
  ATTR(0x15, 0x07, 0xd8, 0x0f);
  ATTR(0x15, 0x07, 0xd9, 0x17, 0x00, 0x00, 0x00, 0x00);
  ATTR(0x15, 0x08, 0x4a, 0x04);
  ATTR(0x15, 0x07, 0xda, 0x00);
  ATTR(0x15, 0x08, 0xa5, 0xfe, 0xfe, 0xff, 0xfe, 0, 0, 0, 0, 0, 0, 0, 0, 0x03);
  ATTR(0x17, 0x00, 0x00, 0x00, 0x00);
  ATTR(0x18, 0x00, 0x2e, 0x83, 0x00, 0x00);
  ATTR(0x18, 0x08, 0xb5, 0x02);
  ATTR(0x18, 0x08, 0x6e, 0x01, 0x0b, 0x00, 0x00);
  ATTR(0x18, 0x07, 0xed, 0x03, 0xfe, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0x03);  // plain, auto, A4
  ATTR(0x18, 0x07, 0xd7, 0x00, 0x01);
  ATTR(0x18, 0x07, 0xe0, 0x04);
  ATTR(0x18, 0x07, 0xe1, 0x00, 0x00, 0x00, 0x00);
  ATTR(0x18, 0x00, 0x3a, 0x08, 0x02, 0x58, 0x01, 0x90);  // 600 x 400 dpi
  ATTR(0x18, 0x09, 0x72, 0x00);
  ATTR(0x18, 0x09, 0x71, 0x01);

  static const uint8_t session[] = {0x01, 0xc1, 0x85, 0x10, 0x00, 0x10, 0x84, 0xc2, 0x00, 0xd8, 0x84, 0x02,
                                    0x58, 0xdd, 0x80, 0xc8, 0xf0, 0x84, 0x08, 0x00, 0xe9, 0x84, 0x01, 0x90,
                                    0x02, 0xc3, 0x00, 0xc4, 0x00, 0xc5, 0x00, 0xc6, 0x00, 0x51, 0xf2, 0x00};
  return data(j, session, sizeof session, NULL, 0);
}

static void band_sink(uint8_t b, void *ctx) {
  ufr_job_t *j = (ufr_job_t *)ctx;
  if (j->bandOom) return;
  size_t idx = j->bandLen / UFR_CHUNK;
  if (idx >= j->nChunks) {
    uint8_t *c = idx < UFR_MAX_CHUNKS ? (uint8_t *)malloc(UFR_CHUNK) : NULL;
    if (!c) {
      j->bandOom = true;
      return;
    }
    j->chunk[j->nChunks++] = c;
  }
  j->chunk[idx][j->bandLen % UFR_CHUNK] = b;
  j->bandLen++;
}

static void band_start(ufr_job_t *j) {
  unsigned bh = UFR_PAGE_H - j->bandY < UFR_BAND ? UFR_PAGE_H - j->bandY : UFR_BAND;
  j->bandLen = 0;
  j->bandOom = false;
  if (j->bandY == 0) {
    uint8_t bih[20];
    jbs_bih(bih, UFR_PAGE_W, UFR_PAGE_H);
    for (int i = 0; i < 20; i++) band_sink(bih[i], j);
  }
  jbs_begin(&j->jbs, UFR_PAGE_W, bh, band_sink, j);
}

static bool band_flush(ufr_job_t *j) {
  unsigned bh = UFR_PAGE_H - j->bandY < UFR_BAND ? UFR_PAGE_H - j->bandY : UFR_BAND;
  if (j->bandOom) {
    // Out of memory for this band: send it blank so the printer still gets
    // a complete page instead of a job that stops halfway.
    if (j->nChunks == 0) {
      j->oom = true;
      j->failed = true;
      return false;
    }
    static const uint8_t white[UFR_BPL] = {0};
    j->bandOom = false;
    j->bandLen = 0;
    if (j->bandY == 0) {
      uint8_t bih[20];
      jbs_bih(bih, UFR_PAGE_W, UFR_PAGE_H);
      for (int i = 0; i < 20; i++) band_sink(bih[i], j);
    }
    jbs_begin(&j->jbs, UFR_PAGE_W, bh, band_sink, j);
    for (unsigned i = 0; i < bh; i++) jbs_line(&j->jbs, white);
    if (j->bandOom) {
      j->oom = true;
      j->failed = true;
      return false;
    }
    j->blankBands++;
  }
  small_t h = {{0}, 0};
  if (j->bandY == 0) {
    s8(&h, 0x61);
    s8(&h, 0xe6); s8(&h, 0x80); s8(&h, 0x01);
    s8(&h, 0xe5); s8(&h, 0x00);
  }
  s8(&h, 0x62);
  s8(&h, 0xe3); s8(&h, 0x85); s16(&h, UFR_PAGE_W); s16(&h, bh);
  if (j->bandY == 0) {
    s8(&h, 0xe8); s8(&h, 0xa1); s8(&h, 0x00); s8(&h, 0x00);
  } else {
    s8(&h, 0xe8); s8(&h, 0xa5); s16(&h, 0); s16(&h, j->bandY);
  }
  s8(&h, 0xe1); s8(&h, 0x05);
  size_t n = j->bandLen;
  s8(&h, 0xd7);
  if (n < 0x80) {
    s8(&h, 0xa0); s8(&h, n);
  } else if (n < 0x8000) {
    s8(&h, 0xa4); s16(&h, n);
  } else {
    s8(&h, 0xa8); s16(&h, n >> 16); s16(&h, n & 0xffff);
  }
  if (n < 0x100) {
    s8(&h, 0x9c); s8(&h, n);
  } else if (n < 0x10000) {
    s8(&h, 0x9d); s16(&h, n);
  } else {
    s8(&h, 0x9e); s16(&h, n >> 16); s16(&h, n & 0xffff);
  }
  return data(j, h.d, h.n, j->chunk, n);
}

bool ufr_page_begin(ufr_job_t *j) {
  if (j->failed) return false;
  if (j->endPagePending) {
    static const uint8_t ep[] = {0x13};
    if (!data(j, ep, 1, NULL, 0)) return false;
    j->endPagePending = false;
  }
  static const uint8_t pb[] = {0x03, 0xe7, 0x85, UFR_PAGE_W >> 8, UFR_PAGE_W & 0xff, UFR_PAGE_H600 >> 8,
                               UFR_PAGE_H600 & 0xff, 0xde, 0x80, 0x00, 0xc8, 0x00, 0xca, 0xa1,
                               UFR_MARGIN600, UFR_MARGIN600, 0xcb, 0x00};
  if (!data(j, pb, sizeof pb, NULL, 0)) return false;
  j->inPage = true;
  j->line = 0;
  j->bandY = 0;
  band_start(j);
  return true;
}

bool ufr_page_line(ufr_job_t *j, const uint8_t *row) {
  if (j->failed || !j->inPage || j->line >= UFR_PAGE_H) return !j->failed;
  jbs_line(&j->jbs, row);
  j->line++;
  if (j->line - j->bandY == UFR_BAND || j->line == UFR_PAGE_H) {
    if (!band_flush(j)) return false;
    j->bandY = j->line;
    if (j->line < UFR_PAGE_H) band_start(j);
  }
  return true;
}

bool ufr_page_end(ufr_job_t *j) {
  if (j->failed || !j->inPage) return !j->failed;
  static const uint8_t white[UFR_BPL] = {0};
  while (j->line < UFR_PAGE_H)
    if (!ufr_page_line(j, white)) return false;
  // "end page" goes out with the next page, or with "end session" like Canon
  j->endPagePending = true;
  j->inPage = false;
  j->pages++;
  return true;
}

bool ufr_job_end(ufr_job_t *j) {
  if (j->inPage && !ufr_page_end(j)) return false;
  if (j->endPagePending) {
    static const uint8_t es[] = {0x13, 0x12, 0x11};
    if (!data(j, es, 3, NULL, 0)) return false;
    j->endPagePending = false;
  } else {
    static const uint8_t es[] = {0x12, 0x11};
    if (!data(j, es, 2, NULL, 0)) return false;
  }
  ATTR(0x1a, 0x01);
  if (!attr(j, 0x19, NULL, 0)) return false;
  uint32_t imp = j->pages * j->copies;
  uint8_t ia[6] = {0x01, 0x13, (uint8_t)(imp >> 24), (uint8_t)(imp >> 16), (uint8_t)(imp >> 8), (uint8_t)imp};
  if (!attr(j, 0x15, ia, 6)) return false;
  if (!attr(j, 0x16, NULL, 0)) return false;
  if (!attr(j, 0x12, ia, 6)) return false;
  uint32_t total = j->total + 26 + 21;
  uint8_t ta[6] = {0x07, 0xd6, (uint8_t)(total >> 24), (uint8_t)(total >> 16), (uint8_t)(total >> 8),
                   (uint8_t)total};
  if (!attr(j, 0x12, ta, 6)) return false;
  ATTR(0x13, 0x00);
  return !j->failed;
}

void ufr_job_free(ufr_job_t *j) {
  for (unsigned i = 0; i < j->nChunks; i++) free(j->chunk[i]);
  j->nChunks = 0;
  j->bandLen = 0;
}
