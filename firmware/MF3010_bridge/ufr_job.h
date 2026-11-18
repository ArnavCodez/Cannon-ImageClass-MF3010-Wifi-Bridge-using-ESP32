// Canon MF3010 job writer: CPCA framing + UFR II LT page description with
// JBIG-compressed 256-line bands. Pages are fed line by line (1-bit,
// 1 = black), so only one compressed band is held in memory at a time.
//
// A4 page bitmap: 4720 x 4516 pixels at 600 x 400 dpi, covering the sheet
// minus 120 px (at 600 dpi) on every side.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "jbig_stream.h"

#define UFR_PAGE_W 4720
#define UFR_PAGE_H 4516
#define UFR_PAGE_H600 6774
#define UFR_BPL (UFR_PAGE_W / 8)
#define UFR_BAND 256
#define UFR_MARGIN600 120
// Compressed bands are kept in 8 KB pieces (no big contiguous block needed).
#define UFR_CHUNK 8192
#define UFR_MAX_CHUNKS 32

// Output: returns false to abort the job (printer gone, ...).
typedef bool (*ufr_write_fn)(const uint8_t *d, size_t n, void *ctx);

typedef struct {
  ufr_write_fn write;
  void *ctx;
  uint32_t total;   // bytes written so far
  uint32_t pages;
  uint16_t copies;
  bool failed;
  bool inPage;
  bool endPagePending;
  uint32_t line;    // next output line within the page
  uint32_t bandY;   // first line of the band being encoded
  uint8_t *chunk[UFR_MAX_CHUNKS];  // compressed band
  uint16_t nChunks;
  size_t bandLen;
  bool bandOom;
  bool oom;             // failed because no memory at all
  uint32_t blankBands;  // bands sent blank because memory ran out
  jbs_t jbs;
} ufr_job_t;

// time8: CPCA timestamp field (8 bytes) or NULL for zeros.
bool ufr_job_begin(ufr_job_t *j, ufr_write_fn write, void *ctx, const char *jobName, const char *user,
                   const char *host, uint16_t copies, const uint8_t *time8, uint32_t jobNumber);
bool ufr_page_begin(ufr_job_t *j);
bool ufr_page_line(ufr_job_t *j, const uint8_t *row);  // UFR_BPL bytes
bool ufr_page_end(ufr_job_t *j);                       // pads the rest with white
bool ufr_job_end(ufr_job_t *j);
void ufr_job_free(ufr_job_t *j);
// CPCA timestamp from calendar values (UTC).
void ufr_time8(uint8_t t[8], int year, int month, int day, int wday, int hour, int min, int sec, int ms);
