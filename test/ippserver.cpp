// Runs the firmware's IPP code on a PC (localhost only) so it can be tested
// with ipptool without flashing anything. Each job the "printer" gets is saved
// as outN.prn in outdir. touch /tmp/printer_off to pretend the printer is off.
//
//   ippserver 8631 /tmp/out
//   ipptool -t -f page.pwg ipp://localhost:8631/ipp/print ipp-everywhere.test
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ipp.h"

static const char *g_dir;
static pthread_mutex_t g_print = PTHREAD_MUTEX_INITIALIZER, g_jobs = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_out;
static int g_outN;
static time_t g_boot;

static bool acquire(uint32_t ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000;
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000;
  }
  if (pthread_mutex_timedlock(&g_print, &ts)) return false;
  char p[256];
  snprintf(p, sizeof p, "%s/out%d.prn", g_dir, ++g_outN);
  g_out = fopen(p, "wb");
  return true;
}
static void release(void) {
  if (g_out) fclose(g_out);
  g_out = NULL;
  pthread_mutex_unlock(&g_print);
}
static bool pwrite_(const uint8_t *d, size_t n, void *ctx) {
  (void)ctx;
  usleep(n / 64);  // roughly USB full speed
  return g_out && fwrite(d, 1, n, g_out) == n;
}
static bool online(void) { return access("/tmp/printer_off", F_OK) != 0; }
static uint32_t uptime_(void) { return (uint32_t)(time(NULL) - g_boot + 1); }
static uint32_t epoch_(void) { return (uint32_t)time(NULL); }
static void time8(uint8_t t[8]) {
  time_t now = time(NULL);
  struct tm tm;
  gmtime_r(&now, &tm);
  ufr_time8(t, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_wday, tm.tm_hour, tm.tm_min, tm.tm_sec, 0);
}
static void lk(void) { pthread_mutex_lock(&g_jobs); }
static void ulk(void) { pthread_mutex_unlock(&g_jobs); }
static void lg(const char *m) { fprintf(stderr, "%s\n", m); }

static int rd(void *ctx, uint8_t *b, size_t n) {
  ssize_t k = recv((int)(intptr_t)ctx, b, n, 0);
  return k > 0 ? (int)k : 0;
}
static bool wr(void *ctx, const uint8_t *b, size_t n) {
  while (n) {
    ssize_t k = send((int)(intptr_t)ctx, b, n, MSG_NOSIGNAL);
    if (k <= 0) return false;
    b += k;
    n -= (size_t)k;
  }
  return true;
}

static void *conn_thread(void *arg) {
  int fd = (int)(intptr_t)arg;
  struct timeval tv = {15, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ipp_conn_t c = {rd, wr, (void *)(intptr_t)fd};
  ipp_serve(&c);
  shutdown(fd, SHUT_WR);
  char tmp[256];
  while (recv(fd, tmp, sizeof tmp, 0) > 0) {
  }
  close(fd);
  return NULL;
}

static void *poll_thread(void *a) {
  (void)a;
  for (;;) {
    sleep(2);
    ipp_poll();
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  signal(SIGPIPE, SIG_IGN);
  g_dir = argv[2];
  g_boot = time(NULL);
  static ipp_platform_t p;
  p.name = "Canon MF3010";
  p.make_model = "Canon imageCLASS MF3010";
  p.location = "Desk";
  p.uuid = "urn:uuid:4d463330-3130-4272-6964-676500000001";
  p.host = "localhost:631";
  p.job_host = "MF3010-BRIDGE";
  p.admin_url = "http://localhost/";
  p.acquire = acquire;
  p.release = release;
  p.write = pwrite_;
  p.online = online;
  p.uptime = uptime_;
  p.epoch = epoch_;
  p.time8 = time8;
  p.lock = lk;
  p.unlock = ulk;
  p.log = lg;
  ipp_init(&p);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)atoi(argv[1]));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8)) {
    perror("bind");
    return 1;
  }
  pthread_t pt;
  pthread_create(&pt, NULL, poll_thread, NULL);
  fprintf(stderr, "listening on %s\n", argv[1]);
  for (;;) {
    int fd = accept(s, NULL, NULL);
    if (fd < 0) continue;
    pthread_t t;
    pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd);
    pthread_detach(t);
  }
}
