// Turn 1-bit PBM pages into an MF3010 print job, using the same code as the
// firmware. Pages are 4720 x 4516 at 600 x 400 dpi (A4 minus 5 mm margins),
// smaller images get padded with white, bigger ones get cut off.
//
//   pbm2ufr [options] page1.pbm [page2.pbm ...] out.prn
//   pbm2ufr page.pbm - | nc <bridge ip> 9100
//
// options: --name NAME --user USER --host HOST --job N --copies N
//          --time HEX16 (raw 8-byte CPCA timestamp, default all zeros)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ufr_job.h"

static bool out_cb(const uint8_t *d, size_t n, void *ctx) { return fwrite(d, 1, n, (FILE *)ctx) == n; }

static int skip_space(FILE *f) {
  int c;
  for (;;) {
    c = fgetc(f);
    if (c == '#')
      while (c != '\n' && c != EOF) c = fgetc(f);
    else if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      return c;
  }
}

static bool read_num(FILE *f, unsigned *v) {
  int c = skip_space(f);
  if (c < '0' || c > '9') return false;
  *v = 0;
  while (c >= '0' && c <= '9') {
    *v = *v * 10 + (unsigned)(c - '0');
    c = fgetc(f);
  }
  return true;
}

static bool print_pbm(ufr_job_t *j, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "can't open %s\n", path);
    return false;
  }
  unsigned w, h;
  if (fgetc(f) != 'P' || fgetc(f) != '4' || !read_num(f, &w) || !read_num(f, &h)) {
    fprintf(stderr, "%s: not a binary PBM (P4)\n", path);
    fclose(f);
    return false;
  }
  size_t bpl = (w + 7) / 8;
  uint8_t *in = (uint8_t *)malloc(bpl);
  uint8_t row[UFR_BPL];
  bool ok = ufr_page_begin(j);
  for (unsigned y = 0; ok && y < h && y < UFR_PAGE_H; y++) {
    if (fread(in, 1, bpl, f) != bpl) break;
    memset(row, 0, sizeof row);
    memcpy(row, in, bpl < UFR_BPL ? bpl : UFR_BPL);
    if (w < UFR_PAGE_W && (w & 7)) row[w / 8] &= (uint8_t)(0xff << (8 - (w & 7)));
    ok = ufr_page_line(j, row);
  }
  free(in);
  fclose(f);
  return ok && ufr_page_end(j);
}

int main(int argc, char **argv) {
  const char *name = "Document", *user = "", *host = "PC";
  unsigned long jobn = 1;
  unsigned copies = 1;
  uint8_t t8[8] = {0};
  int i = 1;
  for (; i < argc && !strncmp(argv[i], "--", 2); i += 2) {
    if (i + 1 >= argc) break;
    const char *k = argv[i], *v = argv[i + 1];
    if (!strcmp(k, "--name")) name = v;
    else if (!strcmp(k, "--user")) user = v;
    else if (!strcmp(k, "--host")) host = v;
    else if (!strcmp(k, "--job")) jobn = strtoul(v, NULL, 10);
    else if (!strcmp(k, "--copies")) copies = (unsigned)atoi(v);
    else if (!strcmp(k, "--time") && strlen(v) == 16) {
      for (int b = 0; b < 8; b++) {
        unsigned x;
        sscanf(v + 2 * b, "%2x", &x);
        t8[b] = (uint8_t)x;
      }
    } else {
      fprintf(stderr, "unknown option %s\n", k);
      return 2;
    }
  }
  if (argc - i < 2) {
    fprintf(stderr, "usage: pbm2ufr [options] page.pbm [...] out.prn|-\n");
    return 2;
  }
  const char *outPath = argv[argc - 1];
  FILE *o = strcmp(outPath, "-") ? fopen(outPath, "wb") : stdout;
  if (!o) {
    fprintf(stderr, "can't write %s\n", outPath);
    return 1;
  }
  static ufr_job_t j;
  bool ok = ufr_job_begin(&j, out_cb, o, name, user, host, (uint16_t)copies, t8, (uint32_t)jobn);
  for (int k = i; ok && k < argc - 1; k++) ok = print_pbm(&j, argv[k]);
  ok = ok && ufr_job_end(&j);
  ufr_job_free(&j);
  if (o != stdout) fclose(o);
  if (!ok) fprintf(stderr, "failed\n");
  else if (o != stdout) fprintf(stderr, "%s: %u page(s), %u bytes\n", outPath, (unsigned)j.pages, (unsigned)j.total);
  return ok ? 0 : 1;
}
