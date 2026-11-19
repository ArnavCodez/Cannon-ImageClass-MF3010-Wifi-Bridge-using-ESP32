// Makes PWG raster / URF files out of a PGM or PPM image, for testing
// without a phone.
// usage: rasgen pwg|urf  in.pnm  out  dpi  [mode]
//   mode: gray (8-bit sgray, default for PGM), rgb (24-bit srgb, PPM), black1
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void be32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

static void enc_line(FILE *o, const uint8_t *l, uint32_t bpl, uint32_t u) {
  uint32_t n = bpl / u, i = 0;
  while (i < n) {
    uint32_t j = i + 1;
    while (j < n && j - i < 128 && !memcmp(l + j * u, l + i * u, u)) j++;
    if (j - i >= 2) {
      fputc((int)(j - i - 1), o);
      fwrite(l + i * u, 1, u, o);
      i = j;
      continue;
    }
    // literal run until two equal units or 128
    j = i + 1;
    while (j < n && j - i < 128 && !(j + 1 < n && !memcmp(l + j * u, l + (j + 1) * u, u))) j++;
    fputc((int)(257 - (j - i)), o);
    fwrite(l + i * u, 1, (j - i) * u, o);
    i = j;
  }
}

int main(int argc, char **argv) {
  if (argc < 5) return 2;
  int urf = !strcmp(argv[1], "urf");
  FILE *f = fopen(argv[2], "rb");
  char m[3];
  unsigned w, h, mx;
  if (!f || fscanf(f, "%2s %u %u %u", m, &w, &h, &mx) != 4) return 1;
  fgetc(f);
  int ch = m[1] == '6' ? 3 : 1;
  uint8_t *img = malloc((size_t)w * h * ch);
  if (fread(img, 1, (size_t)w * h * ch, f) != (size_t)w * h * ch) return 1;
  fclose(f);
  unsigned dpi = atoi(argv[4]);
  const char *mode = argc > 5 ? argv[5] : (ch == 3 ? "rgb" : "gray");
  int black1 = !strcmp(mode, "black1");
  uint32_t bpp = black1 ? 1 : ch * 8;
  uint32_t bpl = black1 ? (w + 7) / 8 : w * ch;
  uint32_t u = black1 ? 1 : ch;
  FILE *o = fopen(argv[3], "wb");
  if (urf) {
    uint8_t fh[12] = "UNIRAST";
    be32(fh + 8, 1);
    fwrite(fh, 1, 12, o);
    uint8_t ph[32] = {0};
    ph[0] = bpp;
    ph[1] = ch == 3 ? 1 : 0;
    ph[2] = 1;
    ph[3] = 4;
    be32(ph + 12, w);
    be32(ph + 16, h);
    be32(ph + 20, dpi);
    fwrite(ph, 1, 32, o);
  } else {
    fwrite("RaS2", 1, 4, o);
    uint8_t ph[1796] = {0};
    strcpy((char *)ph, "PwgRaster");
    strcpy((char *)ph + 64, "");
    be32(ph + 276, dpi);
    be32(ph + 280, dpi);
    be32(ph + 352, 595);
    be32(ph + 356, 842);
    be32(ph + 372, w);
    be32(ph + 376, h);
    be32(ph + 384, black1 ? 1 : 8);
    be32(ph + 388, bpp);
    be32(ph + 392, bpl);
    be32(ph + 400, black1 ? 3 : ch == 3 ? 19 : 18);
    be32(ph + 420, 1);  // TotalPageCount (cupsInteger[0]) - not used
    strcpy((char *)ph + 1732, "iso_a4_210x297mm");
    fwrite(ph, 1, sizeof ph, o);
  }
  uint8_t *line = malloc(bpl), *prev = malloc(bpl);
  uint32_t y = 0;
  while (y < h) {
    // build line y
    uint32_t rep = 0;
    for (;;) {
      uint8_t *L = rep ? prev : line;
      const uint8_t *s = img + (size_t)(y + rep) * w * ch;
      if (black1) {
        memset(L, 0, bpl);
        for (uint32_t x = 0; x < w; x++)
          if (s[x] < 128) L[x >> 3] |= 0x80 >> (x & 7);
      } else {
        memcpy(L, s, bpl);
      }
      if (rep && memcmp(prev, line, bpl)) break;
      rep++;
      if (y + rep >= h || rep == 256) break;
    }
    fputc((int)(rep - 1), o);
    enc_line(o, line, bpl, u);
    y += rep;
  }
  fclose(o);
  return 0;
}
