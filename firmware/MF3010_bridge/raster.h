// PWG raster ("RaS2") and Apple raster / URF ("UNIRAST") decoder that renders
// each page onto the MF3010 page bitmap (A4, 600 x 400 dpi, 5.08 mm margins)
// and feeds it line by line into a ufr_job.
//
// Bytes are pushed in any chunk sizes; memory use is a few source lines.
// Accepted: 1-bit black, 8-bit gray, 24-bit RGB, 8-bit black; any resolution
// (300 and 600 dpi are what the printer advertises). Gray is halftoned with a
// clustered-dot screen; pure black/white stays crisp.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "ufr_job.h"

enum {
  RIN_OK = 0,
  RIN_ERR_FORMAT,   // not PWG / URF
  RIN_ERR_HEADER,   // unsupported page header (bit depth, size...)
  RIN_ERR_MEM,
  RIN_ERR_OUTPUT,   // ufr_job failed (printer gone)
};

typedef struct rin_s rin_t;

rin_t *rin_new(ufr_job_t *job);
// Returns RIN_OK or an error; after an error the stream is dead.
int rin_feed(rin_t *r, const uint8_t *d, size_t n);
// End of document: closes a page that was cut short. Returns RIN_OK/error.
int rin_finish(rin_t *r);
uint32_t rin_pages(const rin_t *r);  // pages started
const char *rin_format(const rin_t *r);  // "pwg", "urf" or "?"
void rin_free(rin_t *r);
