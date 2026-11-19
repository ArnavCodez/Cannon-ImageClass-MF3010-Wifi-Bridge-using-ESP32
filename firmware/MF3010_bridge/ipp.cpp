// IPP Everywhere / AirPrint printer, see ipp.h.
#include "ipp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "icon_png.h"
#include "raster.h"

static const ipp_platform_t *P;

static void logf_(const char *fmt, ...) {
  if (!P || !P->log) return;
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  P->log(b);
}

static void lock(void) {
  if (P->lock) P->lock();
}
static void unlock(void) {
  if (P->unlock) P->unlock();
}
static uint32_t up(void) { return P->uptime ? P->uptime() : 0; }
static uint32_t epoch(void) { return P->epoch ? P->epoch() : 0; }

static void scopy(char *d, size_t n, const char *s, size_t sl) {
  if (sl >= n) sl = n - 1;
  memcpy(d, s, sl);
  d[sl] = 0;
}

// ================================================================ jobs

enum { JS_PENDING = 3, JS_HELD = 4, JS_PROCESSING = 5, JS_STOPPED = 6, JS_CANCELED = 7, JS_ABORTED = 8, JS_COMPLETED = 9 };

#define MAXJOBS 10
typedef struct {
  int id;
  int state;
  volatile bool cancel;
  bool gotDoc;
  char name[48], user[32], format[40], reason[40], message[64];
  uint32_t tCreate, tProc, tDone;  // uptime
  uint32_t eCreate, eProc, eDone;  // epoch
  uint32_t pages, bytes;
  uint16_t copies;
} job_t;

static job_t g_jobs[MAXJOBS];
static int g_nextId = 1;
static int g_lastPState;
static uint32_t g_pStateTime, g_pStateEpoch, g_bootEpoch;

static job_t *job_find(int id) {
  for (int i = 0; i < MAXJOBS; i++)
    if (g_jobs[i].id == id && id > 0) return &g_jobs[i];
  return NULL;
}

// caller holds lock
static job_t *job_new(const char *name, const char *user, const char *format, int copies, int state) {
  job_t *slot = NULL;
  for (int i = 0; i < MAXJOBS && !slot; i++)
    if (g_jobs[i].id == 0) slot = &g_jobs[i];
  if (!slot)  // reuse the oldest finished job
    for (int i = 0; i < MAXJOBS; i++)
      if (g_jobs[i].state >= JS_CANCELED && (!slot || g_jobs[i].id < slot->id)) slot = &g_jobs[i];
  if (!slot) return NULL;
  memset(slot, 0, sizeof *slot);
  slot->id = g_nextId++;
  slot->state = state;
  scopy(slot->name, sizeof slot->name, name && *name ? name : "Untitled", strlen(name && *name ? name : "Untitled"));
  scopy(slot->user, sizeof slot->user, user && *user ? user : "anonymous",
        strlen(user && *user ? user : "anonymous"));
  scopy(slot->format, sizeof slot->format, format && *format ? format : "application/octet-stream",
        strlen(format && *format ? format : "application/octet-stream"));
  strcpy(slot->reason, state == JS_HELD ? "job-incoming" : "none");
  slot->copies = (uint16_t)copies;
  slot->tCreate = up();
  slot->eCreate = epoch();
  return slot;
}

// caller holds lock
static void job_state(job_t *j, int state, const char *reason, const char *msg) {
  j->state = state;
  if (reason) scopy(j->reason, sizeof j->reason, reason, strlen(reason));
  if (msg) scopy(j->message, sizeof j->message, msg, strlen(msg));
  if (state == JS_PROCESSING && !j->tProc) {
    j->tProc = up();
    j->eProc = epoch();
  }
  if (state >= JS_CANCELED) {
    j->tDone = up();
    j->eDone = epoch();
  }
}

static int printer_state_locked(void) {
  int s = 3;
  if (P->online && !P->online())
    s = 5;
  else
    for (int i = 0; i < MAXJOBS; i++)
      if (g_jobs[i].id && g_jobs[i].state == JS_PROCESSING) s = 4;
  if (s != g_lastPState) {
    g_lastPState = s;
    g_pStateTime = up();
    g_pStateEpoch = epoch();
  }
  return s;
}

void ipp_init(const ipp_platform_t *p) {
  P = p;
  memset(g_jobs, 0, sizeof g_jobs);
  g_lastPState = 3;
  g_pStateTime = up();
  g_bootEpoch = 0;
}

void ipp_poll(void) {
  lock();
  uint32_t now = up();
  for (int i = 0; i < MAXJOBS; i++) {
    job_t *j = &g_jobs[i];
    if (j->id && j->state == JS_HELD && now - j->tCreate > 60) {
      job_state(j, JS_ABORTED, "aborted-by-system", "No document received");
      logf_("ipp: job %d expired", j->id);
    }
  }
  printer_state_locked();
  unlock();
}

int ipp_jobs(ipp_job_info_t *out, int max) {
  int n = 0;
  lock();
  // newest first
  for (int k = 0; k < MAXJOBS && n < max; k++) {
    job_t *best = NULL;
    for (int i = 0; i < MAXJOBS; i++) {
      job_t *j = &g_jobs[i];
      if (!j->id) continue;
      bool used = false;
      for (int m = 0; m < n; m++)
        if (out[m].id == j->id) used = true;
      if (!used && (!best || j->id > best->id)) best = j;
    }
    if (!best) break;
    ipp_job_info_t *o = &out[n++];
    o->id = best->id;
    o->state = best->state;
    scopy(o->name, sizeof o->name, best->name, strlen(best->name));
    scopy(o->user, sizeof o->user, best->user, strlen(best->user));
    scopy(o->format, sizeof o->format, best->format, strlen(best->format));
    scopy(o->reason, sizeof o->reason, best->message[0] ? best->message : best->reason,
          strlen(best->message[0] ? best->message : best->reason));
    o->pages = best->pages;
    o->bytes = best->bytes;
    o->created = best->tCreate;
  }
  unlock();
  return n;
}

// ================================================================ HTTP input

typedef struct {
  ipp_conn_t *c;
  uint8_t buf[1024];
  size_t pos, len;
  bool chunked, crlf, eof, err;
  uint32_t remain;  // bytes left in body (content-length) or in the current chunk
  uint32_t total;
} rd_t;

static bool raw_fill(rd_t *r) {
  if (r->pos < r->len) return true;
  int n = r->c->read(r->c->ctx, r->buf, sizeof r->buf);
  if (n <= 0) return false;
  r->pos = 0;
  r->len = (size_t)n;
  return true;
}

static int raw_getc(rd_t *r) { return raw_fill(r) ? r->buf[r->pos++] : -1; }

static size_t raw_read(rd_t *r, uint8_t *d, size_t n) {
  if (!raw_fill(r)) return 0;
  size_t k = r->len - r->pos;
  if (k > n) k = n;
  memcpy(d, r->buf + r->pos, k);
  r->pos += k;
  return k;
}

// Reads one line without CR/LF; -1 at EOF. Long lines are truncated.
static int read_line(rd_t *r, char *s, size_t max) {
  size_t n = 0;
  for (;;) {
    int ch = raw_getc(r);
    if (ch < 0) return n ? (int)n : -1;
    if (ch == '\n') break;
    if (ch != '\r' && n + 1 < max) s[n++] = (char)ch;
  }
  s[n] = 0;
  return (int)n;
}

static size_t body_read(rd_t *r, uint8_t *d, size_t n) {
  if (r->eof) return 0;
  for (;;) {
    if (r->chunked && r->remain == 0) {
      char line[80];
      if (r->crlf) {
        if (read_line(r, line, sizeof line) < 0) break;
        r->crlf = false;
      }
      if (read_line(r, line, sizeof line) < 0) break;
      uint32_t sz = (uint32_t)strtoul(line, NULL, 16);
      if (sz == 0) {
        while (read_line(r, line, sizeof line) > 0) {
        }
        r->eof = true;
        return 0;
      }
      r->remain = sz;
    }
    if (r->remain == 0) {
      r->eof = true;
      return 0;
    }
    size_t k = n < r->remain ? n : r->remain;
    k = raw_read(r, d, k);
    if (!k) break;
    r->remain -= (uint32_t)k;
    r->total += (uint32_t)k;
    if (r->chunked && r->remain == 0) r->crlf = true;
    return k;
  }
  r->eof = true;
  r->err = true;
  return 0;
}

static bool body_full(rd_t *r, uint8_t *d, size_t n) {
  while (n) {
    size_t k = body_read(r, d, n);
    if (!k) return false;
    d += k;
    n -= k;
  }
  return true;
}

static void body_drain(rd_t *r) {
  uint8_t tmp[512];
  while (body_read(r, tmp, sizeof tmp)) {
  }
}

// ================================================================ IPP request

typedef struct {
  uint8_t vmaj, vmin;
  uint16_t op;
  uint32_t reqId;
  char charset[24], lang[24], user[32], jobName[48], docName[48], format[48], compression[16], which[16];
  int jobId, copies, limit;
  bool lastDoc, lastDocSet, myJobs, haveRa, orderBad, haveUri;
  int opAttrs;
  char ra[1200];  // requested-attributes, NUL separated
  size_t raLen;
} req_t;

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void take_str(char *d, size_t dn, uint8_t tag, const uint8_t *v, size_t vl) {
  // textWithLanguage / nameWithLanguage: u16 len + lang + u16 len + text
  if ((tag == 0x35 || tag == 0x36) && vl >= 4) {
    size_t ll = (size_t)(v[0] << 8 | v[1]);
    if (ll + 4 <= vl) {
      size_t tl = (size_t)(v[2 + ll] << 8 | v[3 + ll]);
      if (ll + 4 + tl <= vl) {
        v += ll + 4;
        vl = tl;
      }
    }
  }
  scopy(d, dn, (const char *)v, vl);
}

static int take_int(const uint8_t *v, size_t vl) { return vl == 4 ? (int)be32(v) : 0; }

static void apply(req_t *q, int group, const char *name, uint8_t tag, const uint8_t *v, size_t vl) {
  if (group == 0x01) {
    if (!strcmp(name, "attributes-charset"))
      take_str(q->charset, sizeof q->charset, tag, v, vl);
    else if (!strcmp(name, "attributes-natural-language"))
      take_str(q->lang, sizeof q->lang, tag, v, vl);
    else if (!strcmp(name, "requesting-user-name"))
      take_str(q->user, sizeof q->user, tag, v, vl);
    else if (!strcmp(name, "job-name"))
      take_str(q->jobName, sizeof q->jobName, tag, v, vl);
    else if (!strcmp(name, "document-name"))
      take_str(q->docName, sizeof q->docName, tag, v, vl);
    else if (!strcmp(name, "document-format"))
      take_str(q->format, sizeof q->format, tag, v, vl);
    else if (!strcmp(name, "compression"))
      take_str(q->compression, sizeof q->compression, tag, v, vl);
    else if (!strcmp(name, "which-jobs"))
      take_str(q->which, sizeof q->which, tag, v, vl);
    else if (!strcmp(name, "job-id") && tag == 0x21)
      q->jobId = take_int(v, vl);
    else if (!strcmp(name, "limit") && tag == 0x21)
      q->limit = take_int(v, vl);
    else if (!strcmp(name, "my-jobs") && tag == 0x22 && vl == 1)
      q->myJobs = v[0] != 0;
    else if (!strcmp(name, "last-document") && tag == 0x22 && vl == 1) {
      q->lastDoc = v[0] != 0;
      q->lastDocSet = true;
    }
    else if (!strcmp(name, "printer-uri"))
      q->haveUri = true;
    else if (!strcmp(name, "job-uri")) {
      q->haveUri = true;
      size_t i = vl;
      while (i > 0 && v[i - 1] >= '0' && v[i - 1] <= '9') i--;
      int id = 0;
      for (size_t k = i; k < vl && k < i + 9; k++) id = id * 10 + (v[k] - '0');
      if (!q->jobId) q->jobId = id;
    } else if (!strcmp(name, "requested-attributes")) {
      q->haveRa = true;
      if (q->raLen + vl + 1 < sizeof q->ra) {
        memcpy(q->ra + q->raLen, v, vl);
        q->raLen += vl;
        q->ra[q->raLen++] = 0;
      }
    }
  } else if (group == 0x02) {
    if (!strcmp(name, "copies") && tag == 0x21) q->copies = take_int(v, vl);
  }
}

static bool skip_body(rd_t *r, size_t n) {
  uint8_t tmp[64];
  while (n) {
    size_t k = n < sizeof tmp ? n : sizeof tmp;
    if (!body_full(r, tmp, k)) return false;
    n -= k;
  }
  return true;
}

static bool parse_request(rd_t *r, req_t *q) {
  uint8_t h[8];
  if (!body_full(r, h, 8)) return false;
  q->vmaj = h[0];
  q->vmin = h[1];
  q->op = (uint16_t)(h[2] << 8 | h[3]);
  q->reqId = be32(h + 4);
  q->copies = 1;
  q->lastDoc = true;
  int group = 0, depth = 0;
  char name[64] = "";
  uint8_t val[256];
  for (int guard = 0; guard < 4000; guard++) {
    uint8_t tag;
    if (!body_full(r, &tag, 1)) return false;
    if (tag == 0x03) return true;
    if (tag < 0x10) {
      group = tag;
      depth = 0;
      name[0] = 0;
      continue;
    }
    uint8_t b2[2];
    if (!body_full(r, b2, 2)) return false;
    size_t nl = (size_t)(b2[0] << 8 | b2[1]);
    if (nl) {
      size_t k = nl < sizeof name - 1 ? nl : sizeof name - 1;
      if (!body_full(r, (uint8_t *)name, k) || !skip_body(r, nl - k)) return false;
      name[k] = 0;
    }
    if (!body_full(r, b2, 2)) return false;
    size_t vl = (size_t)(b2[0] << 8 | b2[1]);
    size_t k = vl < sizeof val ? vl : sizeof val;
    if (!body_full(r, val, k) || !skip_body(r, vl - k)) return false;
    if (tag == 0x34) {
      depth++;
      continue;
    }
    if (tag == 0x37) {
      if (depth) depth--;
      continue;
    }
    if (depth) continue;
    if (group == 0x01 && nl) {  // RFC 8011 4.1.4: charset first, then natural language
      q->opAttrs++;
      if ((q->opAttrs == 1 && strcmp(name, "attributes-charset")) ||
          (q->opAttrs == 2 && strcmp(name, "attributes-natural-language")))
        q->orderBad = true;
    }
    apply(q, group, name, tag, val, k);
  }
  return false;
}

static bool want(const req_t *q, const char *name) {
  if (!q->haveRa) return true;
  for (size_t i = 0; i < q->raLen; i += strlen(q->ra + i) + 1) {
    const char *s = q->ra + i;
    if (!strcmp(s, name) || !strcmp(s, "all") || !strcmp(s, "printer-description") ||
        !strcmp(s, "job-template") || !strcmp(s, "job-description"))
      return true;
  }
  return false;
}

// ================================================================ IPP output

typedef struct {
  uint8_t *d;
  size_t n, cap;
  bool oom;
} ob_t;

static void ob_put(ob_t *o, const void *p, size_t n) {
  if (o->oom) return;
  if (o->n + n > o->cap) {
    size_t c = o->cap ? o->cap * 2 : 4096;
    while (c < o->n + n) c *= 2;
    uint8_t *d = (uint8_t *)realloc(o->d, c);
    if (!d) {
      o->oom = true;
      return;
    }
    o->d = d;
    o->cap = c;
  }
  if (n) memcpy(o->d + o->n, p, n);
  o->n += n;
}
static void ob_u8(ob_t *o, unsigned v) {
  uint8_t b = (uint8_t)v;
  ob_put(o, &b, 1);
}
static void ob_u16(ob_t *o, unsigned v) {
  uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
  ob_put(o, b, 2);
}
static void ob_u32(ob_t *o, uint32_t v) {
  uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  ob_put(o, b, 4);
}

enum {
  T_INT = 0x21, T_BOOL = 0x22, T_ENUM = 0x23, T_DATE = 0x31, T_RES = 0x32, T_RANGE = 0x33, T_BEGC = 0x34,
  T_ENDC = 0x37, T_TEXT = 0x41, T_NAME = 0x42, T_KW = 0x44, T_URI = 0x45, T_CHARSET = 0x47, T_LANG = 0x48,
  T_MIME = 0x49, T_MEMBER = 0x4a, T_UNKNOWN = 0x12, T_NOVALUE = 0x13
};

static void a_raw(ob_t *o, uint8_t tag, const char *name, const void *v, size_t vl) {
  size_t nl = name ? strlen(name) : 0;
  ob_u8(o, tag);
  ob_u16(o, (unsigned)nl);
  ob_put(o, name, nl);
  ob_u16(o, (unsigned)vl);
  ob_put(o, v, vl);
}
static void a_str(ob_t *o, uint8_t tag, const char *name, const char *s) { a_raw(o, tag, name, s, strlen(s)); }
static void a_int(ob_t *o, uint8_t tag, const char *name, int32_t v) {
  uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  a_raw(o, tag, name, b, 4);
}
static void a_bool(ob_t *o, const char *name, bool v) {
  uint8_t b = v ? 1 : 0;
  a_raw(o, T_BOOL, name, &b, 1);
}
static void a_range(ob_t *o, const char *name, int32_t lo, int32_t hi) {
  uint8_t b[8] = {(uint8_t)(lo >> 24), (uint8_t)(lo >> 16), (uint8_t)(lo >> 8), (uint8_t)lo,
                  (uint8_t)(hi >> 24), (uint8_t)(hi >> 16), (uint8_t)(hi >> 8), (uint8_t)hi};
  a_raw(o, T_RANGE, name, b, 8);
}
static void a_res(ob_t *o, const char *name, int32_t dpi) {
  uint8_t b[9] = {(uint8_t)(dpi >> 24), (uint8_t)(dpi >> 16), (uint8_t)(dpi >> 8), (uint8_t)dpi,
                  (uint8_t)(dpi >> 24), (uint8_t)(dpi >> 16), (uint8_t)(dpi >> 8), (uint8_t)dpi, 3};
  a_raw(o, T_RES, name, b, 9);
}
static void a_date(ob_t *o, const char *name, uint32_t t) {
  time_t tt = (time_t)t;
  struct tm tm;
  gmtime_r(&tt, &tm);
  uint8_t b[11] = {(uint8_t)((tm.tm_year + 1900) >> 8), (uint8_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1),
                   (uint8_t)tm.tm_mday, (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec, 0, '+', 0, 0};
  a_raw(o, T_DATE, name, b, 11);
}
static void a_strs(ob_t *o, uint8_t tag, const char *name, const char *const *v, int n) {
  for (int i = 0; i < n; i++) a_str(o, tag, i ? NULL : name, v[i]);
}
static void a_ints(ob_t *o, uint8_t tag, const char *name, const int *v, int n) {
  for (int i = 0; i < n; i++) a_int(o, tag, i ? NULL : name, v[i]);
}
static void c_member(ob_t *o, const char *m) { a_str(o, T_MEMBER, NULL, m); }
static void c_end(ob_t *o) { a_raw(o, T_ENDC, NULL, NULL, 0); }

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MARGIN 508  // 5.08 mm = 120 px at 600 dpi

static void media_size(ob_t *o) {
  a_raw(o, T_BEGC, NULL, NULL, 0);
  c_member(o, "x-dimension");
  a_int(o, T_INT, NULL, 21000);
  c_member(o, "y-dimension");
  a_int(o, T_INT, NULL, 29700);
  c_end(o);
}

static void media_col(ob_t *o, const char *name) {
  a_raw(o, T_BEGC, name, NULL, 0);
  c_member(o, "media-bottom-margin");
  a_int(o, T_INT, NULL, MARGIN);
  c_member(o, "media-left-margin");
  a_int(o, T_INT, NULL, MARGIN);
  c_member(o, "media-right-margin");
  a_int(o, T_INT, NULL, MARGIN);
  c_member(o, "media-size");
  media_size(o);
  c_member(o, "media-source");
  a_str(o, T_KW, NULL, "main");
  c_member(o, "media-top-margin");
  a_int(o, T_INT, NULL, MARGIN);
  c_member(o, "media-type");
  a_str(o, T_KW, NULL, "stationery");
  c_end(o);
}

static const char *state_reason(int ps) { return ps == 5 ? "offline-report" : "none"; }

static void printer_attrs(ob_t *o, const req_t *q, const char *uri, const char *icon) {
  lock();
  int ps = printer_state_locked();
  int queued = 0;
  for (int i = 0; i < MAXJOBS; i++)
    if (g_jobs[i].id && g_jobs[i].state <= JS_PROCESSING) queued++;
  uint32_t stTime = g_pStateTime, stEpoch = g_pStateEpoch;
  unlock();
  uint32_t now = up(), ep = epoch();
  if (!g_bootEpoch && ep) g_bootEpoch = ep - now;

#define W(n) if (want(q, n))
  W("charset-configured") a_str(o, T_CHARSET, "charset-configured", "utf-8");
  W("charset-supported") a_str(o, T_CHARSET, "charset-supported", "utf-8");
  W("color-supported") a_bool(o, "color-supported", false);
  W("compression-supported") a_str(o, T_KW, "compression-supported", "none");
  W("copies-default") a_int(o, T_INT, "copies-default", 1);
  W("copies-supported") a_range(o, "copies-supported", 1, 99);
  W("document-format-default") a_str(o, T_MIME, "document-format-default", "application/octet-stream");
  W("document-format-supported") {
    static const char *const f[] = {"application/octet-stream", "image/pwg-raster", "image/urf"};
    a_strs(o, T_MIME, "document-format-supported", f, N(f));
  }
  W("finishings-default") a_int(o, T_ENUM, "finishings-default", 3);
  W("finishings-supported") a_int(o, T_ENUM, "finishings-supported", 3);
  W("generated-natural-language-supported") a_str(o, T_LANG, "generated-natural-language-supported", "en");
  W("identify-actions-default") a_str(o, T_KW, "identify-actions-default", "flash");
  W("identify-actions-supported") a_str(o, T_KW, "identify-actions-supported", "flash");
  W("ipp-features-supported") a_str(o, T_KW, "ipp-features-supported", "ipp-everywhere");
  W("ipp-versions-supported") {
    static const char *const v[] = {"1.1", "2.0"};
    a_strs(o, T_KW, "ipp-versions-supported", v, N(v));
  }
  W("job-creation-attributes-supported") {
    static const char *const v[] = {"copies",        "media",       "media-col",          "orientation-requested",
                                    "print-color-mode", "print-quality", "printer-resolution", "sides"};
    a_strs(o, T_KW, "job-creation-attributes-supported", v, N(v));
  }
  W("job-ids-supported") a_bool(o, "job-ids-supported", true);
  W("media-bottom-margin-supported") a_int(o, T_INT, "media-bottom-margin-supported", MARGIN);
  W("media-col-database") media_col(o, "media-col-database");
  W("media-col-default") media_col(o, "media-col-default");
  W("media-col-ready") media_col(o, "media-col-ready");
  W("media-col-supported") {
    static const char *const v[] = {"media-bottom-margin", "media-left-margin", "media-right-margin", "media-size",
                                    "media-source",        "media-top-margin",  "media-type"};
    a_strs(o, T_KW, "media-col-supported", v, N(v));
  }
  W("media-default") a_str(o, T_KW, "media-default", "iso_a4_210x297mm");
  W("media-left-margin-supported") a_int(o, T_INT, "media-left-margin-supported", MARGIN);
  W("media-ready") a_str(o, T_KW, "media-ready", "iso_a4_210x297mm");
  W("media-right-margin-supported") a_int(o, T_INT, "media-right-margin-supported", MARGIN);
  W("media-size-supported") {
    a_raw(o, T_BEGC, "media-size-supported", NULL, 0);
    c_member(o, "x-dimension");
    a_int(o, T_INT, NULL, 21000);
    c_member(o, "y-dimension");
    a_int(o, T_INT, NULL, 29700);
    c_end(o);
  }
  W("media-source-supported") a_str(o, T_KW, "media-source-supported", "main");
  W("media-supported") a_str(o, T_KW, "media-supported", "iso_a4_210x297mm");
  W("media-top-margin-supported") a_int(o, T_INT, "media-top-margin-supported", MARGIN);
  W("media-type-supported") a_str(o, T_KW, "media-type-supported", "stationery");
  W("multiple-document-jobs-supported") a_bool(o, "multiple-document-jobs-supported", false);
  W("multiple-operation-time-out") a_int(o, T_INT, "multiple-operation-time-out", 60);
  W("multiple-operation-time-out-action") a_str(o, T_KW, "multiple-operation-time-out-action", "abort-job");
  W("natural-language-configured") a_str(o, T_LANG, "natural-language-configured", "en");
  W("operations-supported") {
    static const int v[] = {0x02, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x0b, 0x39, 0x3b, 0x3c};
    a_ints(o, T_ENUM, "operations-supported", v, N(v));
  }
  W("orientation-requested-default") a_int(o, T_ENUM, "orientation-requested-default", 3);
  W("orientation-requested-supported") {
    static const int v[] = {3, 4, 5, 6};
    a_ints(o, T_ENUM, "orientation-requested-supported", v, N(v));
  }
  W("output-bin-default") a_str(o, T_KW, "output-bin-default", "face-down");
  W("output-bin-supported") a_str(o, T_KW, "output-bin-supported", "face-down");
  W("pages-per-minute") a_int(o, T_INT, "pages-per-minute", 18);
  W("pdl-override-supported") a_str(o, T_KW, "pdl-override-supported", "attempted");
  W("preferred-attributes-supported") a_bool(o, "preferred-attributes-supported", false);
  W("print-color-mode-default") a_str(o, T_KW, "print-color-mode-default", "monochrome");
  W("print-color-mode-supported") {
    static const char *const v[] = {"auto", "monochrome"};
    a_strs(o, T_KW, "print-color-mode-supported", v, N(v));
  }
  W("print-content-optimize-default") a_str(o, T_KW, "print-content-optimize-default", "auto");
  W("print-content-optimize-supported") a_str(o, T_KW, "print-content-optimize-supported", "auto");
  W("print-rendering-intent-default") a_str(o, T_KW, "print-rendering-intent-default", "auto");
  W("print-rendering-intent-supported") a_str(o, T_KW, "print-rendering-intent-supported", "auto");
  W("print-quality-default") a_int(o, T_ENUM, "print-quality-default", 4);
  W("print-quality-supported") {
    static const int v[] = {3, 4, 5};
    a_ints(o, T_ENUM, "print-quality-supported", v, N(v));
  }
  W("printer-config-change-date-time") a_date(o, "printer-config-change-date-time", g_bootEpoch);
  W("printer-config-change-time") a_int(o, T_INT, "printer-config-change-time", 1);
  W("printer-current-time") a_date(o, "printer-current-time", ep);
  W("printer-device-id")
  a_str(o, T_TEXT, "printer-device-id", "MFG:Canon;MDL:MF3010 (Wi-Fi);CMD:PWGRaster,URF;CLS:PRINTER;");
  W("printer-dns-sd-name") a_str(o, T_NAME, "printer-dns-sd-name", P->name);
  W("printer-geo-location") a_raw(o, T_UNKNOWN, "printer-geo-location", NULL, 0);
  W("printer-get-attributes-supported") a_str(o, T_KW, "printer-get-attributes-supported", "document-format");
  W("printer-icons") a_str(o, T_URI, "printer-icons", icon);
  W("printer-info") a_str(o, T_TEXT, "printer-info", P->name);
  W("printer-is-accepting-jobs") a_bool(o, "printer-is-accepting-jobs", ps != 5);
  W("printer-kind") a_str(o, T_KW, "printer-kind", "document");
  W("printer-location") a_str(o, T_TEXT, "printer-location", P->location ? P->location : "");
  W("printer-make-and-model") a_str(o, T_TEXT, "printer-make-and-model", P->make_model);
  W("printer-more-info") a_str(o, T_URI, "printer-more-info", P->admin_url);
  W("printer-name") a_str(o, T_NAME, "printer-name", P->name);
  W("printer-organization") a_str(o, T_TEXT, "printer-organization", "");
  W("printer-organizational-unit") a_str(o, T_TEXT, "printer-organizational-unit", "");
  W("printer-resolution-default") a_res(o, "printer-resolution-default", 600);
  W("printer-resolution-supported") {
    a_res(o, "printer-resolution-supported", 300);
    a_res(o, NULL, 600);
  }
  W("printer-state") a_int(o, T_ENUM, "printer-state", ps);
  W("printer-state-change-date-time") a_date(o, "printer-state-change-date-time", stEpoch ? stEpoch : ep);
  W("printer-state-change-time") a_int(o, T_INT, "printer-state-change-time", (int32_t)(stTime ? stTime : 1));
  W("printer-state-message")
  a_str(o, T_TEXT, "printer-state-message",
        ps == 5 ? "Printer is off or not connected" : ps == 4 ? "Printing" : "Ready");
  W("printer-state-reasons") a_str(o, T_KW, "printer-state-reasons", state_reason(ps));
  W("printer-supply")
  a_str(o, 0x30, "printer-supply",
        "index=1;class=supplyThatIsConsumed;type=toner;unit=percent;maxcapacity=100;level=-3;colorantname=black;");
  W("printer-supply-description") a_str(o, T_TEXT, "printer-supply-description", "Black Toner");
  W("printer-supply-info-uri") a_str(o, T_URI, "printer-supply-info-uri", P->admin_url);
  W("printer-up-time") a_int(o, T_INT, "printer-up-time", (int32_t)(now ? now : 1));
  W("printer-uri-supported") a_str(o, T_URI, "printer-uri-supported", uri);
  W("printer-uuid") a_str(o, T_URI, "printer-uuid", P->uuid);
  W("pwg-raster-document-resolution-supported") {
    a_res(o, "pwg-raster-document-resolution-supported", 300);
    a_res(o, NULL, 600);
  }
  W("pwg-raster-document-sheet-back") a_str(o, T_KW, "pwg-raster-document-sheet-back", "normal");
  W("pwg-raster-document-type-supported") {
    static const char *const v[] = {"black_1", "sgray_8", "srgb_8"};
    a_strs(o, T_KW, "pwg-raster-document-type-supported", v, N(v));
  }
  W("queued-job-count") a_int(o, T_INT, "queued-job-count", queued);
  W("sides-default") a_str(o, T_KW, "sides-default", "one-sided");
  W("sides-supported") a_str(o, T_KW, "sides-supported", "one-sided");
  W("uri-authentication-supported") a_str(o, T_KW, "uri-authentication-supported", "none");
  W("uri-security-supported") a_str(o, T_KW, "uri-security-supported", "none");
  W("urf-supported") {
    static const char *const v[] = {"V1.4", "CP99", "PQ4", "RS300-600", "SRGB24", "W8"};
    a_strs(o, T_KW, "urf-supported", v, N(v));
  }
  W("which-jobs-supported") {
    static const char *const v[] = {"completed", "not-completed", "all"};
    a_strs(o, T_KW, "which-jobs-supported", v, N(v));
  }
#undef W
}

static void job_attrs(ob_t *o, const job_t *j, const req_t *q, const char *puri, bool brief) {
  char juri[192];
  snprintf(juri, sizeof juri, "%s/%d", puri, j->id);
#define W(n) if (brief ? 1 : want(q, n))
  W("job-id") a_int(o, T_INT, "job-id", j->id);
  W("job-uri") a_str(o, T_URI, "job-uri", juri);
  W("job-state") a_int(o, T_ENUM, "job-state", j->state);
  W("job-state-reasons") {
    const char *r = j->reason;
    if (j->state == JS_COMPLETED && !strcmp(r, "none")) r = "job-completed-successfully";
    if (j->state == JS_PROCESSING && !strcmp(r, "none")) r = "job-printing";
    a_str(o, T_KW, "job-state-reasons", r);
  }
  W("job-state-message")
  a_str(o, T_TEXT, "job-state-message",
        j->message[0] ? j->message
        : j->state == JS_COMPLETED ? "Printed"
        : j->state == JS_PROCESSING ? "Printing"
        : j->state <= JS_HELD ? "Waiting"
                              : "Stopped");
  if (brief) return;
  W("job-printer-uri") a_str(o, T_URI, "job-printer-uri", puri);
  W("job-name") a_str(o, T_NAME, "job-name", j->name);
  W("job-originating-user-name") a_str(o, T_NAME, "job-originating-user-name", j->user);
  W("document-format") a_str(o, T_MIME, "document-format", j->format);
  W("copies") a_int(o, T_INT, "copies", j->copies);
  W("job-impressions-completed") a_int(o, T_INT, "job-impressions-completed", (int32_t)(j->pages * j->copies));
  W("job-media-sheets-completed") a_int(o, T_INT, "job-media-sheets-completed", (int32_t)(j->pages * j->copies));
  W("job-k-octets") a_int(o, T_INT, "job-k-octets", (int32_t)((j->bytes + 1023) / 1024));
  W("job-printer-up-time") a_int(o, T_INT, "job-printer-up-time", (int32_t)(up() ? up() : 1));
  W("time-at-creation") a_int(o, T_INT, "time-at-creation", (int32_t)j->tCreate);
  W("time-at-processing") {
    if (j->tProc)
      a_int(o, T_INT, "time-at-processing", (int32_t)j->tProc);
    else
      a_raw(o, T_NOVALUE, "time-at-processing", NULL, 0);
  }
  W("time-at-completed") {
    if (j->state >= JS_CANCELED)
      a_int(o, T_INT, "time-at-completed", (int32_t)j->tDone);
    else
      a_raw(o, T_NOVALUE, "time-at-completed", NULL, 0);
  }
  W("date-time-at-creation") a_date(o, "date-time-at-creation", j->eCreate);
  W("date-time-at-processing") {
    if (j->tProc)
      a_date(o, "date-time-at-processing", j->eProc);
    else
      a_raw(o, T_NOVALUE, "date-time-at-processing", NULL, 0);
  }
  W("date-time-at-completed") {
    if (j->state >= JS_CANCELED)
      a_date(o, "date-time-at-completed", j->eDone);
    else
      a_raw(o, T_NOVALUE, "date-time-at-completed", NULL, 0);
  }
#undef W
}

// ================================================================ printing

enum { DOC_NONE, DOC_RASTER, DOC_CPCA };

static bool format_ok(const char *f) {
  return !*f || !strcmp(f, "application/octet-stream") || !strcmp(f, "image/pwg-raster") || !strcmp(f, "image/urf");
}

// Reads the first bytes of the document and classifies it.
static int sniff(rd_t *r, uint8_t *head, size_t *n) {
  *n = 0;
  while (*n < 8) {
    size_t k = body_read(r, head + *n, 8 - *n);
    if (!k) break;
    *n += k;
  }
  if (*n >= 4 && (!memcmp(head, "RaS2", 4) || !memcmp(head, "UNIR", 4))) return DOC_RASTER;
  if (*n >= 2 && head[0] == 0xcd && head[1] == 0xca) return DOC_CPCA;
  return DOC_NONE;
}

typedef struct {
  ufr_job_t uj;
  uint8_t buf[4096];
} printbuf_t;

// Prints the document whose first bytes are in head. The job must exist.
static void print_doc(rd_t *r, int jobId, int kind, const uint8_t *head, size_t hn, int copies) {
  job_t *j;
  char name[48], user[32];
  lock();
  j = job_find(jobId);
  if (j) {
    j->gotDoc = true;
    job_state(j, JS_PENDING, "none", "Waiting for printer");
    strcpy(name, j->name);
    strcpy(user, j->user);
  }
  unlock();
  if (!j) {
    body_drain(r);
    return;
  }
  // wait for the printer (another job or port 9100 may be using it)
  bool got = false;
  for (int t = 0; t < 120 && !got && !j->cancel; t++) got = P->acquire(1000);
  if (!got) {
    body_drain(r);
    lock();
    if (j->id == jobId)
      job_state(j, j->cancel ? JS_CANCELED : JS_ABORTED, j->cancel ? "job-canceled-by-user" : "aborted-by-system",
                j->cancel ? "Canceled" : "Printer busy");
    unlock();
    return;
  }
  lock();
  job_state(j, JS_PROCESSING, "job-printing", "Printing");
  unlock();
  logf_("ipp: job %d printing (%s, %d copies)", jobId, kind == DOC_CPCA ? "canon" : "raster", copies);

  printbuf_t *pb = (printbuf_t *)malloc(sizeof(printbuf_t));
  const char *fail = NULL;
  const char *failReason = "aborted-by-system";
  uint32_t bytes = (uint32_t)hn, pages = 0;
  bool canceled = false;
  if (!pb) {
    fail = "Out of memory";
  } else if (kind == DOC_CPCA) {
    // already a Canon job (Windows driver): pass straight through
    bool ok = P->write(head, hn, P->write_ctx);
    size_t k;
    while (ok && (k = body_read(r, pb->buf, sizeof pb->buf)) > 0) {
      bytes += (uint32_t)k;
      ok = P->write(pb->buf, k, P->write_ctx);
    }
    if (ok) ok = P->write(NULL, 0, P->write_ctx);  // flush
    if (!ok) fail = "Printer disconnected";
  } else {
    uint8_t t8[8] = {0};
    if (P->time8) P->time8(t8);
    bool ok = ufr_job_begin(&pb->uj, P->write, P->write_ctx, name, user, P->job_host, (uint16_t)copies, t8,
                            (uint32_t)jobId);
    rin_t *rin = ok ? rin_new(&pb->uj) : NULL;
    int e = rin ? rin_feed(rin, head, hn) : RIN_ERR_MEM;
    size_t k;
    while (!e && !(canceled = j->cancel) && (k = body_read(r, pb->buf, sizeof pb->buf)) > 0) {
      bytes += (uint32_t)k;
      e = rin_feed(rin, pb->buf, k);
      if (rin) {
        lock();
        if (j->id == jobId) {
          j->pages = rin_pages(rin);
          j->bytes = bytes;
        }
        unlock();
      }
    }
    if (rin && (!e || e == RIN_ERR_HEADER || e == RIN_ERR_FORMAT)) {
      int e2 = rin_finish(rin);  // close a page cut short
      if (!e) e = e2;
    }
    if (rin) pages = rin_pages(rin);
    rin_free(rin);
    if (e != RIN_ERR_OUTPUT && !pb->uj.failed && ufr_job_end(&pb->uj) && !P->write(NULL, 0, P->write_ctx))
      pb->uj.failed = true;  // final flush
    if (pb->uj.blankBands) logf_("ipp: job %d: low memory, %u bands left blank", jobId, (unsigned)pb->uj.blankBands);
    if (pb->uj.oom)
      fail = "Out of memory";
    else if (pb->uj.failed || e == RIN_ERR_OUTPUT)
      fail = "Printer disconnected";
    else if (e == RIN_ERR_MEM)
      fail = "Out of memory";
    else if (e) {
      fail = "Unsupported raster data";
      failReason = "document-format-error";
    }
    ufr_job_free(&pb->uj);
  }
  free(pb);
  P->release();
  body_drain(r);
  if (r->err && !fail && !canceled) fail = "Upload interrupted";

  lock();
  if (j->id == jobId) {
    j->pages = pages;
    j->bytes = bytes;
    if (canceled)
      job_state(j, JS_CANCELED, "job-canceled-by-user", "Canceled");
    else if (fail)
      job_state(j, JS_ABORTED, failReason, fail);
    else
      job_state(j, JS_COMPLETED, "job-completed-successfully", "Printed");
  }
  unlock();
  logf_("ipp: job %d %s, %u pages, %u bytes", jobId, canceled ? "canceled" : fail ? fail : "done", (unsigned)pages, (unsigned)bytes);
}

// ================================================================ operations

enum {
  OK = 0x0000, OK_SUBST = 0x0001, BAD_REQUEST = 0x0400, NOT_POSSIBLE = 0x0404, NOT_FOUND = 0x0406,
  FORMAT_UNSUPPORTED = 0x040a, ATTR_UNSUPPORTED = 0x040b, COMPRESSION_UNSUPPORTED = 0x040f,
  INTERNAL = 0x0500, OP_UNSUPPORTED = 0x0501, VERSION_UNSUPPORTED = 0x0503, NOT_ACCEPTING = 0x0506, BUSY = 0x0507
};

typedef struct {
  int status;
  char msg[96];
  int jobId;     // job group to return
  bool jobBrief;
} res_t;

static void set_err(res_t *s, int st, const char *m) {
  s->status = st;
  scopy(s->msg, sizeof s->msg, m, strlen(m));
}

// common checks for document-carrying requests
static bool check_doc_req(const req_t *q, res_t *s) {
  if (!format_ok(q->format)) {
    set_err(s, FORMAT_UNSUPPORTED, "Send PWG raster (image/pwg-raster) or URF (image/urf)");
    return false;
  }
  if (q->compression[0] && strcmp(q->compression, "none")) {
    set_err(s, COMPRESSION_UNSUPPORTED, "Compression not supported");
    return false;
  }
  return true;
}

static int clamp_copies(const req_t *q, res_t *s) {
  int c = q->copies;
  if (c < 1 || c > 99) {
    s->status = OK_SUBST;
    c = c < 1 ? 1 : 99;
  }
  return c;
}

static bool online(void) { return !P->online || P->online(); }

static void op_print(rd_t *r, const req_t *q, res_t *s) {
  if (!check_doc_req(q, s)) return;
  if (!online()) {
    set_err(s, NOT_ACCEPTING, "Printer is off or not connected");
    return;
  }
  int copies = clamp_copies(q, s);
  uint8_t head[8];
  size_t hn;
  int kind = sniff(r, head, &hn);
  if (kind == DOC_NONE) {
    set_err(s, hn ? FORMAT_UNSUPPORTED : BAD_REQUEST, hn ? "Unsupported document format" : "No document data");
    return;
  }
  lock();
  job_t *j = job_new(q->jobName[0] ? q->jobName : q->docName, q->user, q->format, copies, JS_PENDING);
  int id = j ? j->id : 0;
  unlock();
  if (!id) {
    set_err(s, BUSY, "Too many jobs");
    return;
  }
  print_doc(r, id, kind, head, hn, copies);
  s->jobId = id;
  s->jobBrief = true;
}

static void op_create(const req_t *q, res_t *s) {
  if (!check_doc_req(q, s)) return;
  if (!online()) {
    set_err(s, NOT_ACCEPTING, "Printer is off or not connected");
    return;
  }
  int copies = clamp_copies(q, s);
  lock();
  job_t *j = job_new(q->jobName[0] ? q->jobName : q->docName, q->user, q->format, copies, JS_HELD);
  int id = j ? j->id : 0;
  unlock();
  if (!id) {
    set_err(s, BUSY, "Too many jobs");
    return;
  }
  s->jobId = id;
  s->jobBrief = true;
}

static void op_send(rd_t *r, const req_t *q, res_t *s) {
  if (!q->lastDocSet) {
    set_err(s, BAD_REQUEST, "Missing last-document");
    return;
  }
  if (!check_doc_req(q, s)) return;
  lock();
  job_t *j = job_find(q->jobId);
  int st = j ? j->state : 0;
  bool got = j ? j->gotDoc : false;
  int copies = j ? j->copies : 1;
  if (j && q->format[0]) scopy(j->format, sizeof j->format, q->format, strlen(q->format));
  if (j && q->docName[0] && !strcmp(j->name, "Untitled")) scopy(j->name, sizeof j->name, q->docName, strlen(q->docName));
  unlock();
  if (!j) {
    set_err(s, NOT_FOUND, "Job not found");
    return;
  }
  if (st != JS_HELD || got) {
    set_err(s, NOT_POSSIBLE, "Job does not accept documents");
    return;
  }
  uint8_t head[8];
  size_t hn;
  int kind = sniff(r, head, &hn);
  if (kind == DOC_NONE) {
    if (hn == 0 && q->lastDoc) {  // empty last document: just close the job
      lock();
      job_state(j, JS_COMPLETED, "job-completed-successfully", "Nothing to print");
      unlock();
      s->jobId = q->jobId;
      s->jobBrief = true;
      return;
    }
    lock();
    job_state(j, JS_ABORTED, "document-format-error", "Unsupported document format");
    unlock();
    set_err(s, FORMAT_UNSUPPORTED, "Unsupported document format");
    return;
  }
  print_doc(r, q->jobId, kind, head, hn, copies);
  s->jobId = q->jobId;
  s->jobBrief = true;
}

static void op_cancel(const req_t *q, res_t *s) {
  lock();
  job_t *j = job_find(q->jobId);
  if (!j) {
    set_err(s, NOT_FOUND, "Job not found");
  } else if (j->state >= JS_CANCELED) {
    set_err(s, NOT_POSSIBLE, "Job already finished");
  } else if (j->state == JS_PROCESSING || j->gotDoc) {
    j->cancel = true;
  } else {
    job_state(j, JS_CANCELED, "job-canceled-by-user", "Canceled");
  }
  unlock();
}

static void op_close(const req_t *q, res_t *s) {
  lock();
  job_t *j = job_find(q->jobId);
  if (!j)
    set_err(s, NOT_FOUND, "Job not found");
  else if (j->state == JS_HELD && !j->gotDoc)
    job_state(j, JS_COMPLETED, "job-completed-successfully", "Nothing to print");
  unlock();
}

static void op_cancel_mine(const req_t *q, res_t *s) {
  (void)s;
  lock();
  for (int i = 0; i < MAXJOBS; i++) {
    job_t *j = &g_jobs[i];
    if (!j->id || j->state >= JS_CANCELED || (q->user[0] && strcmp(q->user, j->user))) continue;
    if (j->state == JS_PROCESSING || j->gotDoc)
      j->cancel = true;
    else
      job_state(j, JS_CANCELED, "job-canceled-by-user", "Canceled");
  }
  unlock();
}

static bool which_match(const char *which, int state) {
  if (!strcmp(which, "all")) return true;
  if (!strcmp(which, "completed")) return state >= JS_CANCELED;
  return state < JS_CANCELED;  // not-completed (default)
}

// ================================================================ HTTP

static bool send_all(ipp_conn_t *c, const void *d, size_t n) { return c->write(c->ctx, (const uint8_t *)d, n); }

static void http_simple(ipp_conn_t *c, const char *status, const char *type, const char *body) {
  char h[256];
  int n = snprintf(h, sizeof h,
                   "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n"
                   "Server: MF3010-bridge\r\n\r\n",
                   status, type, (unsigned)strlen(body));
  send_all(c, h, (size_t)n);
  send_all(c, body, strlen(body));
}

static void handle_ipp(rd_t *r, ipp_conn_t *c, const char *host) {
  req_t *q = (req_t *)calloc(1, sizeof(req_t));
  ob_t ob = {NULL, 0, 0, false};
  res_t s;
  memset(&s, 0, sizeof s);
  char uri[160], icon[160];
  snprintf(uri, sizeof uri, "ipp://%s/ipp/print", host);
  // icon over plain HTTP on the same port (631 is implied in ipp:// URIs)
  const char *colon = strrchr(host, ':');
  bool hasPort = colon && (host[0] != '[' || colon > strchr(host, ']'));
  snprintf(icon, sizeof icon, "http://%s%s/icon.png", host, hasPort ? "" : ":631");

  if (!q) {
    body_drain(r);
    http_simple(c, "500 Internal Server Error", "text/plain", "out of memory\n");
    return;
  }
  if (!parse_request(r, q)) {
    body_drain(r);
    free(q);
    http_simple(c, "400 Bad Request", "text/plain", "bad IPP request\n");
    return;
  }

  if (q->vmaj < 1 || q->vmaj > 2) {
    set_err(&s, VERSION_UNSUPPORTED, "IPP version not supported");
  } else if (q->reqId == 0) {
    set_err(&s, BAD_REQUEST, "Bad request-id");
  } else if (!q->charset[0] || !q->lang[0] || q->orderBad || !q->haveUri) {
    set_err(&s, BAD_REQUEST, "Missing attributes-charset or attributes-natural-language");
  } else {
    switch (q->op) {
      case 0x0002: op_print(r, q, &s); break;                           // Print-Job
      case 0x0004:                                                       // Validate-Job
        if (check_doc_req(q, &s)) clamp_copies(q, &s);
        break;
      case 0x0005: op_create(q, &s); break;                             // Create-Job
      case 0x0006: op_send(r, q, &s); break;                            // Send-Document
      case 0x0008: op_cancel(q, &s); break;                             // Cancel-Job
      case 0x0009:                                                       // Get-Job-Attributes
        lock();
        if (job_find(q->jobId))
          s.jobId = q->jobId;
        else
          set_err(&s, NOT_FOUND, "Job not found");
        unlock();
        break;
      case 0x000a: break;                                                // Get-Jobs (below)
      case 0x000b: break;                                                // Get-Printer-Attributes
      case 0x0039: op_cancel_mine(q, &s); break;                        // Cancel-My-Jobs
      case 0x003b: op_close(q, &s); break;                              // Close-Job
      case 0x003c:                                                       // Identify-Printer
        if (P->identify) P->identify();
        break;
      default: set_err(&s, OP_UNSUPPORTED, "Operation not supported"); break;
    }
  }
  body_drain(r);

  // response
  ob_u8(&ob, q->vmaj >= 1 && q->vmaj <= 2 ? q->vmaj : 1);
  ob_u8(&ob, q->vmaj >= 1 && q->vmaj <= 2 ? q->vmin : 1);
  ob_u16(&ob, (unsigned)s.status);
  ob_u32(&ob, q->reqId);
  ob_u8(&ob, 0x01);
  a_str(&ob, T_CHARSET, "attributes-charset", "utf-8");
  a_str(&ob, T_LANG, "attributes-natural-language", "en");
  if (s.msg[0]) a_str(&ob, T_TEXT, "status-message", s.msg);
  if (s.status < 0x0400) {
    if (q->op == 0x000b) {
      ob_u8(&ob, 0x04);
      printer_attrs(&ob, q, uri, icon);
    } else if (q->op == 0x000a) {
      req_t qq = *q;  // default attributes for Get-Jobs: job-id, job-uri
      if (!qq.haveRa) {
        qq.haveRa = true;
        memcpy(qq.ra, "job-id\0job-uri\0", 15);
        qq.raLen = 15;
      }
      lock();
      int n = 0;
      // simple selection: sort ids descending
      int ids[MAXJOBS], cnt = 0;
      for (int i = 0; i < MAXJOBS; i++) {
        job_t *j = &g_jobs[i];
        if (!j->id || !which_match(q->which, j->state)) continue;
        if (q->myJobs && q->user[0] && strcmp(q->user, j->user)) continue;
        ids[cnt++] = j->id;
      }
      for (int a = 0; a < cnt; a++)
        for (int b = a + 1; b < cnt; b++)
          if (ids[b] > ids[a]) {
            int t = ids[a];
            ids[a] = ids[b];
            ids[b] = t;
          }
      for (int a = 0; a < cnt && (q->limit <= 0 || n < q->limit); a++, n++) {
        ob_u8(&ob, 0x02);
        job_attrs(&ob, job_find(ids[a]), &qq, uri, false);
      }
      unlock();
    } else if (s.jobId) {
      lock();
      job_t *j = job_find(s.jobId);
      if (j) {
        ob_u8(&ob, 0x02);
        job_attrs(&ob, j, q, uri, s.jobBrief);
      }
      unlock();
    }
  }
  ob_u8(&ob, 0x03);

  if (ob.oom) {
    http_simple(c, "500 Internal Server Error", "text/plain", "out of memory\n");
  } else {
    char h[200];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: %u\r\n"
                     "Connection: close\r\nCache-Control: no-cache\r\nServer: MF3010-bridge\r\n\r\n",
                     (unsigned)ob.n);
    if (send_all(c, h, (size_t)n)) send_all(c, ob.d, ob.n);
  }
  if (q->op != 0x000b && q->op != 0x0009 && q->op != 0x000a)
    logf_("ipp: op 0x%04x -> 0x%04x %s", q->op, s.status, s.msg);
  free(ob.d);
  free(q);
}

static bool istarts(const char *s, const char *p) {
  for (; *p; s++, p++) {
    char a = *s, b = *p;
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    if (a != b) return false;
  }
  return true;
}

void ipp_serve(ipp_conn_t *c) {
  rd_t *r = (rd_t *)calloc(1, sizeof(rd_t));
  if (!r) return;
  r->c = c;
  char line[512], method[8] = "", path[128] = "", host[96] = "";
  if (read_line(r, line, sizeof line) <= 0 || sscanf(line, "%7s %127s", method, path) != 2) {
    free(r);
    return;
  }
  bool chunked = false, expect = false, haveLen = false, ippType = false;
  uint32_t len = 0;
  int hl;
  while ((hl = read_line(r, line, sizeof line)) > 0) {
    const char *v = strchr(line, ':');
    if (!v) continue;
    v++;
    while (*v == ' ' || *v == '\t') v++;
    if (istarts(line, "content-length:")) {
      len = (uint32_t)strtoul(v, NULL, 10);
      haveLen = true;
    } else if (istarts(line, "transfer-encoding:")) {
      chunked = istarts(v, "chunked");
    } else if (istarts(line, "expect:")) {
      expect = istarts(v, "100-continue");
    } else if (istarts(line, "host:")) {
      scopy(host, sizeof host, v, strlen(v));
    } else if (istarts(line, "content-type:")) {
      ippType = istarts(v, "application/ipp");
    }
  }
  if (hl < 0) {
    free(r);
    return;
  }
  if (!host[0]) scopy(host, sizeof host, P->host, strlen(P->host));
  for (char *p = host; *p; p++)
    if (*p == '"' || *p == '<' || *p == '>' || *p == ' ') *p = '_';

  if ((!strcmp(method, "GET") || !strcmp(method, "HEAD")) && !strcmp(path, "/icon.png")) {
    char h[200];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %u\r\nConnection: close\r\n"
                     "Cache-Control: max-age=86400\r\n\r\n",
                     ICON_PNG_LEN);
    if (send_all(c, h, (size_t)n) && !strcmp(method, "GET")) send_all(c, ICON_PNG, ICON_PNG_LEN);
    free(r);
    return;
  }
  if (!strcmp(method, "GET") || !strcmp(method, "HEAD")) {
    char body[400];
    snprintf(body, sizeof body,
             "<!DOCTYPE html><html><head><meta http-equiv=\"refresh\" content=\"0; url=%s\"></head>"
             "<body><p>%s - IPP printer at ipp://%s/ipp/print. <a href=\"%s\">Status page</a></p></body></html>\n",
             P->admin_url, P->name, host, P->admin_url);
    http_simple(c, "200 OK", "text/html", !strcmp(method, "HEAD") ? "" : body);
    free(r);
    return;
  }
  if (strcmp(method, "POST")) {
    http_simple(c, "405 Method Not Allowed", "text/plain", "method not allowed\n");
    free(r);
    return;
  }
  r->chunked = chunked;
  r->remain = chunked ? 0 : haveLen ? len : 0xffffffffu;
  if (!chunked && haveLen && len == 0) r->eof = true;

  bool pathOk = !strcmp(path, "/") || !strncmp(path, "/ipp/print", 10) || !strcmp(path, "/ipp");
  if (!pathOk || !ippType) {
    body_drain(r);
    http_simple(c, pathOk ? "400 Bad Request" : "404 Not Found", "text/plain", "not an IPP request\n");
    free(r);
    return;
  }
  if (expect) send_all(c, "HTTP/1.1 100 Continue\r\n\r\n", 25);
  handle_ipp(r, c, host);
  free(r);
}
