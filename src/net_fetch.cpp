#include "net_fetch.h"
#include <atomic>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <esp_rom_crc.h>
#include "app_board.h"
#include "preset_lists.h"
#include "version_gen.h"

namespace NetFetch {

static const char    *URL_LATEST = "https://github.com/san40pan40/p5-portal/releases/latest";
// Preset blocklists (GitHub release "lists"). Override for a local test server:
// -DP5_LISTS_BASE=\"http://192.168.50.x:8000/\" (plain http works there too)
#ifndef P5_LISTS_BASE
#define P5_LISTS_BASE "https://github.com/san40pan40/p5-portal/releases/download/lists/"
#endif
static const char    *URL_FW     = "https://github.com/san40pan40/p5-portal/releases/latest/download/firmware.bin";
static const uint32_t TIMEOUT_MS = 10000;
static const uint32_t STACK      = 10240;      // TLS handshake is stack-hungry; ~2 KB margin over 8 KB

// Firmware jobs and list jobs have different status records. Thus a list job does not erase
// a firmware result (for example "installed, restart"). s points to the record of the current job.
// The job task writes *s while s_busy is set. The web layer reads a snapshot.
// The task fills the text fields BEFORE it sets the final state.
static Status            s_fw, s_list;
static Status           *s = &s_fw;
static std::atomic<bool> s_busy{false};

bool busy() { return s_busy; }
Status fwStatus() { return s_fw; }
Status listStatus() { return s_list; }

static void finish(State st, Err e = E_NONE, int code = 0, const char *detail = "") {
  s->err  = e;
  s->code = code;
  strlcpy(s->detail, detail, sizeof(s->detail));
  s->state = st;
  if (st == FAILED) Serial.printf("[FETCH] job %u failed: err %u code %d %s\n", s->job, e, code, detail);
}

static void httpFail(int code) {
  if (code < 0) finish(FAILED, E_CONNECT, code, HTTPClient::errorToString(code).c_str());
  else          finish(FAILED, E_HTTP, code);
}

// GitHub answers /releases/latest with a redirect to .../releases/tag/<tag>, or to .../releases
// when there are no releases. Only the Location header is read: no API, no rate limit.
static void runCheck(NetworkClient &c, HTTPClient &h) {
  h.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!h.begin(c, URL_LATEST)) return finish(FAILED, E_CONNECT);
  int code = h.GET();
  if (code != 301 && code != 302) return httpFail(code);
  const String &loc = h.getLocation();
  int i = loc.indexOf("/releases/tag/");
  size_t n = 0;
  if (i >= 0)                                    // keep only safe tag characters (shown in HTML/JSON)
    for (const char *p = loc.c_str() + i + 14; *p && n + 1 < sizeof(s->latest); p++) {
      if (!isalnum((unsigned char)*p) && !strchr(".-_+", *p)) break;
      s->latest[n++] = *p;
    }
  s->latest[n] = 0;
  Serial.printf("[FETCH] latest release: %s\n", n ? s->latest : "(none)");
  finish(DONE);
}

// Reads a body of len bytes from h in chunks of up to 1 KB into sink, updating s->progress.
// Stops on a sink error (E_FLASH), a stalled or closed connection (E_TIMEOUT). got = bytes read.
static Err pump(HTTPClient &h, int len, bool (*sink)(const uint8_t *, size_t), int &got) {
  uint8_t *buf = (uint8_t *)malloc(1024);
  NetworkClient *st = h.getStreamPtr();
  Err e = buf ? E_NONE : E_FLASH;
  uint32_t last = millis();
  got = 0;
  while (e == E_NONE && got < len) {
    int a = st->available();
    int r = a > 0 ? st->read(buf, a < 1024 ? a : 1024) : 0;
    if (r > 0) {
      if (!sink(buf, r)) e = E_FLASH;
      got += r;
      last = millis();
      s->progress = (uint64_t)got * 100 / len;
      delay(1);                                  // yield: let the web/DNS loop and IDLE run
    } else if (!st->connected() || millis() - last > TIMEOUT_MS) {
      e = E_TIMEOUT;                             // nothing read: stalled or closed — never spin forever
    } else {
      delay(2);
    }
  }
  free(buf);
  return e;
}

static bool fwSink(const uint8_t *b, size_t n) { return Update.write((uint8_t *)b, n) == n; }

// Download the release asset (GitHub redirects to its asset CDN) straight into the OTA partition.
// Update.end(true) verifies the image; on any failure the running firmware stays the boot one.
static void runInstall(NetworkClient &c, HTTPClient &h) {
  h.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!h.begin(c, URL_FW)) return finish(FAILED, E_CONNECT);
  int code = h.GET();
  if (code != 200) return httpFail(code);
  const int len = h.getSize();
  const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
  if (len <= 0) return finish(FAILED, E_SIZE);
  if (!part || (size_t)len > part->size) return finish(FAILED, E_TOO_BIG, len / 1024);
  Serial.printf("[FETCH] firmware %d bytes -> %s\n", len, part->label);

  Board::setLed(30, 0, 40);                      // purple: update in progress
  if (!Update.begin(len)) return finish(FAILED, E_FLASH, 0, Update.errorString());
  int got;
  Err e = pump(h, len, fwSink, got);
  if (e == E_NONE && !Update.end(true)) e = E_FLASH;
  if (e != E_NONE) {
    String err = Update.errorString();           // read before abort() resets it
    Update.abort();
    Board::refreshLed();
    return finish(FAILED, e, got / 1024, e == E_FLASH ? err.c_str() : "");
  }
  Serial.printf("[FETCH] firmware written, %d bytes\n", got);
  finish(DONE);                                  // the web layer offers the restart; LED stays purple
}

// ---- preset blocklists: index.json -> /bl/index.json, <id>.bin -> /bl/<id>.tmp (verified)
static File     s_file;
static uint32_t s_crc;
static uint8_t  s_hdr[PresetLists::HDR_SIZE];
static size_t   s_hdrN;

static bool listSink(const uint8_t *b, size_t n) {
  size_t i = 0;
  while (s_hdrN < sizeof(s_hdr) && i < n) s_hdr[s_hdrN++] = b[i++];
  if (i < n) s_crc = esp_rom_crc32_le(s_crc, b + i, n - i);   // same as zlib.crc32 of the payload
  return s_file.write(b, n) == n;
}

static void runList(NetworkClient &c, HTTPClient &h) {
  const bool idx = s->job == JOB_LIST_INDEX;
  char url[160], path[40];
  if (idx) {
    snprintf(url, sizeof(url), "%sindex.json", P5_LISTS_BASE);
    strlcpy(path, "/bl/index.tmp", sizeof(path));   // replaces /bl/index.json when complete
  } else {
    snprintf(url, sizeof(url), "%s%s.bin", P5_LISTS_BASE, s->arg);
    snprintf(path, sizeof(path), "/bl/%s.tmp", s->arg);
  }
  h.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!h.begin(c, url)) return finish(FAILED, E_CONNECT);
  int code = h.GET();
  if (code != 200) return httpFail(code);
  const int len = h.getSize();
  if (len <= (idx ? 0 : (int)PresetLists::HDR_SIZE)) return finish(FAILED, E_SIZE);
  if (idx && len > 8192) return finish(FAILED, E_BAD_FILE);
  LittleFS.remove(path);                         // a leftover of a failed attempt
  const uint32_t freeKb = PresetLists::freeKb(), needKb = (len + 1023) / 1024;
  if (needKb > freeKb) {
    char d[12];
    snprintf(d, sizeof(d), "%u", (unsigned)freeKb);
    return finish(FAILED, E_SPACE, needKb, d);
  }
  s_file = LittleFS.open(path, "w");
  if (!s_file) return finish(FAILED, E_FS);
  s_crc = 0;
  s_hdrN = 0;
  int got;
  Err e = pump(h, len, listSink, got);
  s_file.close();
  if (e == E_FLASH) e = E_FS;
  uint32_t cnt, crc;
  if (e == E_NONE && !idx && (!PresetLists::validHeader(s_hdr, got, cnt, crc) || crc != s_crc)) e = E_BAD_FILE;
  if (e != E_NONE) {
    LittleFS.remove(path);
    return finish(FAILED, e);
  }
  if (idx) {
    LittleFS.remove("/bl/index.json");
    if (!LittleFS.rename(path, "/bl/index.json")) {
      LittleFS.remove(path);
      return finish(FAILED, E_FS);
    }
  }
  Serial.printf("[FETCH] %s: %d bytes\n", path, got);
  finish(DONE);
}

static void task(void *) {
  {
    NetworkClientSecure c;
    NetworkClient plain;                         // only for a test server (P5_LISTS_BASE = http://...)
    c.useBuiltinCACertBundle();
    c.setHandshakeTimeout(TIMEOUT_MS / 1000);    // seconds
    HTTPClient h;
    h.setUserAgent("P5-Portal/" FW_VERSION);
    h.setConnectTimeout(TIMEOUT_MS);
    h.setTimeout(TIMEOUT_MS);
    if (s->job == JOB_FW_CHECK)        runCheck(c, h);
    else if (s->job == JOB_FW_INSTALL) runInstall(c, h);
    else runList(strncmp(P5_LISTS_BASE, "http:", 5) ? (NetworkClient &)c : plain, h);
    h.end();
  }                                              // TLS context freed here
  Serial.printf("[FETCH] done, stack left %u B, heap %u B\n", (unsigned)uxTaskGetStackHighWaterMark(NULL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  s_busy = false;
  vTaskDelete(NULL);
}

Err start(Job job, const char *arg) {
  bool expected = false;
  if (!s_busy.compare_exchange_strong(expected, true)) return E_BUSY;
  if (Update.isRunning()) { s_busy = false; return E_BUSY; }    // a file OTA upload is in progress
  const size_t blk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  Status n;                                       // fresh status; an install keeps the tag it installs
  if (job == JOB_FW_INSTALL) strlcpy(n.latest, s_fw.latest, sizeof(n.latest));
  n.job = job;
  strlcpy(n.arg, arg, sizeof(n.arg));
  s = job >= JOB_LIST_INDEX ? &s_list : &s_fw;
  *s = n;
  if (blk < MIN_HEAP) {
    finish(FAILED, E_HEAP, blk / 1024);
    s_busy = false;
    return E_HEAP;
  }
  s->state = RUNNING;
  Serial.printf("[FETCH] job %u start, largest internal block %u B\n", job, (unsigned)blk);
  if (xTaskCreate(task, "fetch", STACK, nullptr, 1, nullptr) != pdPASS) {
    finish(FAILED, E_TASK);
    s_busy = false;
    return E_TASK;
  }
  return E_NONE;
}

}  // namespace NetFetch
