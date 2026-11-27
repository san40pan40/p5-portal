#include "net_fetch.h"
#include <atomic>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include "app_board.h"
#include "version_gen.h"

namespace NetFetch {

static const char    *URL_LATEST = "https://github.com/san40pan40/p5-portal/releases/latest";
static const char    *URL_FW     = "https://github.com/san40pan40/p5-portal/releases/latest/download/firmware.bin";
static const uint32_t TIMEOUT_MS = 10000;
static const size_t   MIN_HEAP   = 60 * 1024;  // TLS needs ~45-55 KB of internal RAM in one piece
static const uint32_t STACK      = 8192;

// s is written by the job task while s_busy is set, read by the web layer (snapshot).
// The task fills the text fields BEFORE it sets the final state.
static Status            s;
static std::atomic<bool> s_busy{false};

bool busy() { return s_busy; }
Status status() { return s; }

static void finish(State st, Err e = E_NONE, int code = 0, const char *detail = "") {
  s.err  = e;
  s.code = code;
  strlcpy(s.detail, detail, sizeof(s.detail));
  s.state = st;
  if (st == FAILED) Serial.printf("[FETCH] job %u failed: err %u code %d %s\n", s.job, e, code, detail);
}

static void httpFail(int code) {
  if (code < 0) finish(FAILED, E_CONNECT, code, HTTPClient::errorToString(code).c_str());
  else          finish(FAILED, E_HTTP, code);
}

// GitHub answers /releases/latest with a redirect to .../releases/tag/<tag>, or to .../releases
// when there are no releases. Only the Location header is read: no API, no rate limit.
static void runCheck(NetworkClientSecure &c, HTTPClient &h) {
  h.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!h.begin(c, URL_LATEST)) return finish(FAILED, E_CONNECT);
  int code = h.GET();
  if (code != 301 && code != 302) return httpFail(code);
  const String &loc = h.getLocation();
  int i = loc.indexOf("/releases/tag/");
  size_t n = 0;
  if (i >= 0)                                    // keep only safe tag characters (shown in HTML/JSON)
    for (const char *p = loc.c_str() + i + 14; *p && n + 1 < sizeof(s.latest); p++) {
      if (!isalnum((unsigned char)*p) && !strchr(".-_+", *p)) break;
      s.latest[n++] = *p;
    }
  s.latest[n] = 0;
  Serial.printf("[FETCH] latest release: %s\n", n ? s.latest : "(none)");
  finish(DONE);
}

// Download the release asset (GitHub redirects to its asset CDN) straight into the OTA partition.
// Update.end(true) verifies the image; on any failure the running firmware stays the boot one.
static void runInstall(NetworkClientSecure &c, HTTPClient &h) {
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
  uint8_t *buf = (uint8_t *)malloc(1024);
  NetworkClient *st = h.getStreamPtr();
  Err e = buf ? E_NONE : E_FLASH;
  int got = 0;
  uint32_t last = millis();
  while (e == E_NONE && got < len) {
    int a = st->available();
    if (a > 0) {
      int r = st->read(buf, a < 1024 ? a : 1024);
      if (r <= 0) continue;
      if (Update.write(buf, r) != (size_t)r) e = E_FLASH;
      got += r;
      last = millis();
      s.progress = (uint32_t)got * 100 / len;
    } else if (!st->connected() || millis() - last > TIMEOUT_MS) {
      e = E_TIMEOUT;
    } else {
      delay(2);
    }
  }
  free(buf);
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

static void task(void *) {
  {
    NetworkClientSecure c;
    c.useBuiltinCACertBundle();
    c.setHandshakeTimeout(TIMEOUT_MS / 1000);    // seconds
    HTTPClient h;
    h.setUserAgent("P5-Portal/" FW_VERSION);
    h.setConnectTimeout(TIMEOUT_MS);
    h.setTimeout(TIMEOUT_MS);
    if (s.job == JOB_FW_CHECK) runCheck(c, h);
    else                       runInstall(c, h);
    h.end();
  }                                              // TLS context freed here
  Serial.printf("[FETCH] done, stack left %u B, heap %u B\n", (unsigned)uxTaskGetStackHighWaterMark(NULL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  s_busy = false;
  vTaskDelete(NULL);
}

Err start(Job job) {
  bool expected = false;
  if (!s_busy.compare_exchange_strong(expected, true)) return E_BUSY;
  if (Update.isRunning()) { s_busy = false; return E_BUSY; }    // a file OTA upload is in progress
  const size_t blk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  Status n;                                       // fresh status; an install keeps the tag it installs
  if (job == JOB_FW_INSTALL) strlcpy(n.latest, s.latest, sizeof(n.latest));
  n.job = job;
  s = n;
  if (blk < MIN_HEAP) {
    finish(FAILED, E_HEAP, blk / 1024);
    s_busy = false;
    return E_HEAP;
  }
  s.state = RUNNING;
  Serial.printf("[FETCH] job %u start, largest internal block %u B\n", job, (unsigned)blk);
  if (xTaskCreate(task, "fetch", STACK, nullptr, 1, nullptr) != pdPASS) {
    finish(FAILED, E_TASK);
    s_busy = false;
    return E_TASK;
  }
  return E_NONE;
}

}  // namespace NetFetch
