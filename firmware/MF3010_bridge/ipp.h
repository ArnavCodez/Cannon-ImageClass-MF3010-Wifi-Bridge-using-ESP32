// Minimal IPP Everywhere / AirPrint printer for the MF3010 bridge.
//
// HTTP/1.1 + IPP/2.0 on one resource (/ipp/print). Accepts PWG raster and
// Apple raster (URF), renders them with raster.h into Canon UFR II LT jobs,
// and also passes raw Canon jobs (CPCA, from the Windows driver) through.
//
// Platform independent: the caller supplies connection I/O and the printer
// hooks, so the same code runs on the ESP32 and in a desktop test server.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "ufr_job.h"

typedef struct {
  // Read up to n bytes: >0 = bytes read, 0 = closed or timed out.
  int (*read)(void *ctx, uint8_t *buf, size_t n);
  bool (*write)(void *ctx, const uint8_t *buf, size_t n);
  void *ctx;
} ipp_conn_t;

typedef struct {
  const char *name;        // printer-name, e.g. "Canon MF3010"
  const char *make_model;  // "Canon imageCLASS MF3010"
  const char *location;
  const char *uuid;        // "urn:uuid:..."
  const char *host;        // fallback host[:port] for URIs, e.g. "mf3010.local:631"
  const char *job_host;    // computer name sent to the printer with each job
  const char *admin_url;   // "http://192.168.1.14/"

  // printer output, shared with other inputs (port 9100)
  bool (*acquire)(uint32_t timeout_ms);
  void (*release)(void);
  ufr_write_fn write;  // raw bytes to the printer; (NULL, 0) = flush buffered bytes
  void *write_ctx;
  bool (*online)(void);  // printer attached

  // clock
  uint32_t (*uptime)(void);        // seconds since boot
  uint32_t (*epoch)(void);         // unix time (UTC), 0 if unknown
  void (*time8)(uint8_t t[8]);     // Canon job timestamp, NULL = zeros

  // job table lock (several connections can be served at once)
  void (*lock)(void);
  void (*unlock)(void);
  void (*log)(const char *msg);
  void (*identify)(void);  // Identify-Printer: blink the LED (optional)
} ipp_platform_t;

void ipp_init(const ipp_platform_t *p);
// Serves one HTTP request on the connection; the caller closes it afterwards.
void ipp_serve(ipp_conn_t *c);
// Call every few seconds: expires jobs that were created but never got data.
void ipp_poll(void);

// For the status page.
typedef struct {
  int id;
  int state;  // 3 pending, 4 held, 5 processing, 7 canceled, 8 aborted, 9 completed
  char name[48];
  char user[32];
  char format[24];
  uint32_t pages;
  uint32_t bytes;
  uint32_t created;  // uptime seconds
  char reason[40];
} ipp_job_info_t;
int ipp_jobs(ipp_job_info_t *out, int max);  // newest first
