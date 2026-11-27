#include "preset_lists.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "app_network.h"      // Net::internetOk() — downloads only while the internet is up

namespace PresetLists {

static const uint32_t DAY_MS  = 24UL * 3600 * 1000;
static const uint32_t HOUR_MS = 3600UL * 1000;
static const char    *INDEX   = "/bl/index.json";

struct Meta {                                         // one entry of index.json
  char     id[24], name[64], license[24], homepage[64], updated[12];
  uint32_t count, bytes, crc;                         // 0 = unknown
};

struct List {
  Meta     m;
  bool     enabled, want, failed;                    // want = download queued. failed = show err.
  uint32_t retryAt;                                   // failed: millis() of the next try
  NetFetch::Status err;                               // why the last download failed
  uint8_t  *hs;                                       // all hashes (es bytes each, LE), PSRAM; null = not active
  uint32_t count, crc, bytes;                         // of the local file
  uint8_t  es;                                        // entry size: 8 (64-bit) or 6 (48-bit hashes)
};

// Known before the first index.json is downloaded (names on the page)
static const Meta BUILTIN[] = {
  {"p5", "StevenBlack hosts (base) + HaGeZi Multi LIGHT", "MIT + GPL-3.0", "https://github.com/san40pan40/p5-portal/releases/tag/lists", "", 0, 0, 0},
};

static List     L[MAX_LISTS];
static int      s_n;
static int      s_open;                               // number of active lists (fast exit in match)
static NetFetch::Job s_job = NetFetch::JOB_NONE;      // the job we started and wait for
static int      s_cur = -1;                           // list of a JOB_LIST_FETCH
static bool     s_checkDue;                           // fetch index.json at the next chance
static uint32_t s_lastCheck;

int listCount() { return s_n; }
const char *id(int i) { return L[i].m.id; }
bool busy() { return s_job != NetFetch::JOB_NONE; }

int find(const char *id) {
  for (int i = 0; i < s_n; i++)
    if (!strcmp(L[i].m.id, id)) return i;
  return -1;
}

// Ids go into file names, URLs and HTML: a-z 0-9 - _ only
static bool validId(const char *s) {
  size_t n = strlen(s);
  if (!n || n >= sizeof(Meta::id)) return false;
  for (; *s; s++)
    if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '-' && *s != '_') return false;
  return true;
}

static int addList(const char *id) {
  int i = find(id);
  if (i >= 0 || !validId(id) || s_n >= MAX_LISTS) return i;
  strlcpy(L[s_n].m.id, id, sizeof(Meta::id));
  strlcpy(L[s_n].m.name, id, sizeof(Meta::name));
  return s_n++;
}

static void path(char *out, const List &l, const char *ext) { snprintf(out, 40, "/bl/%s.%s", l.m.id, ext); }

uint32_t totalKb() { return LittleFS.totalBytes() / 1024; }
uint32_t freeKb() {
  size_t fr = LittleFS.totalBytes() - LittleFS.usedBytes();
  return fr > FS_RESERVE ? (fr - FS_RESERVE) / 1024 : 0;
}

uint8_t validHeader(const uint8_t *h, size_t len, uint32_t &count, uint32_t &crc) {
  if (len < HDR_SIZE || memcmp(h, "P5BL", 4) || h[4] != 1 || (h[5] != 64 && h[5] != 48)) return 0;
  const uint8_t es = h[5] / 8;
  memcpy(&count, h + 8, 4);                           // little-endian, like the ESP32
  memcpy(&crc, h + 12, 4);
  return count && (uint64_t)count * es + HDR_SIZE == len ? es : 0;
}

// ================================================================ open / close / lookup
static void closeList(List &l) {
  if (l.hs) s_open--;
  free(l.hs);
  l.hs = nullptr;
  l.count = 0;
}

// Loads /bl/<id>.bin into a new PSRAM buffer, then frees the old buffer of the list.
// On failure the old buffer stays active. It deletes a broken file (the list downloads it again).
// Returns E_NONE, E_FS (no file or a bad file) or E_MEM (no PSRAM for the new buffer).
static NetFetch::Err openList(List &l) {
  char p[40];
  path(p, l, "bin");
  if (!LittleFS.exists(p)) return NetFetch::E_FS;
  const uint32_t t0 = millis();
  File f = LittleFS.open(p, "r");
  uint8_t h[HDR_SIZE];
  uint32_t cnt, crc;
  uint8_t es = 0;
  if (!f || f.read(h, HDR_SIZE) != HDR_SIZE || !(es = validHeader(h, f.size(), cnt, crc))) {
    Serial.printf("[BL] %s: bad file, deleted\n", p);
    f.close();
    LittleFS.remove(p);
    return NetFetch::E_FS;
  }
  uint8_t *hs = (uint8_t *)ps_malloc(cnt * es);
  NetFetch::Err e = hs ? NetFetch::E_NONE : NetFetch::E_MEM;
  if (hs && f.read(hs, cnt * es) != cnt * es) {
    free(hs);
    e = NetFetch::E_FS;
  }
  const uint32_t bytes = f.size();
  f.close();
  if (e != NetFetch::E_NONE) return e;
  if (l.hs) free(l.hs);                               // swap: the new data replaces the old data
  else s_open++;
  l.hs = hs;
  l.count = cnt;
  l.crc = crc;
  l.es = es;
  l.bytes = bytes;
  Serial.printf("[BL] %s: %u hashes loaded in %u ms, PSRAM free %u\n", l.m.id, (unsigned)cnt,
                (unsigned)(millis() - t0), (unsigned)ESP.getFreePsram());
  return NetFetch::E_NONE;
}

// h = FNV-1a 64; a 48-bit list stores (h ^ h >> 48) & 0xFFFFFFFFFFFF (see build_lists.py)
static bool inList(const List &l, uint64_t h) {
  if (l.es == 6) h = (h ^ (h >> 48)) & 0xFFFFFFFFFFFFULL;
  int a = 0, b = l.count - 1;
  while (a <= b) {
    int m = (a + b) / 2;
    uint64_t v = 0;
    memcpy(&v, l.hs + (size_t)m * l.es, l.es);        // little-endian entry
    if (v == h) return true;
    if (v < h) a = m + 1; else b = m - 1;
  }
  return false;
}

#ifdef P5_LISTS_BASE                                  // test builds: lookup timing on the serial port
static uint32_t s_us, s_max, s_cnt;
#endif

int match(const char *name, size_t n, size_t &off) {
  if (!s_open) return -1;
#ifdef P5_LISTS_BASE
  const uint32_t t0 = micros();
#endif
  int hit = -1;
  for (size_t o = 0; hit < 0;) {
    const char *dot = (const char *)memchr(name + o, '.', n - o);
    if (!dot) break;                                  // a bare TLD is never looked up
    uint64_t h = 0xcbf29ce484222325ULL;               // FNV-1a 64 of name[o..n)
    for (size_t i = o; i < n; i++) h = (h ^ (uint8_t)name[i]) * 0x100000001b3ULL;
    for (int i = 0; i < s_n; i++)
      if (L[i].hs && inList(L[i], h)) { hit = i; off = o; break; }
    o = dot - name + 1;
  }
#ifdef P5_LISTS_BASE
  const uint32_t us = micros() - t0;
  s_us += us;
  s_max = max(s_max, us);
  if (++s_cnt % 50 == 0) {
    Serial.printf("[BL] lookup avg %u us, max %u us (%u queries), heap %u, PSRAM %u\n", (unsigned)(s_us / s_cnt),
                  (unsigned)s_max, (unsigned)s_cnt, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  }
#endif
  return hit;
}

// ================================================================ index.json (minimal JSON reader)
// Only what index.json needs: {"lists":[{flat object}, ...]}; unknown keys and values are skipped.
static void ws(const char *&p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; }

// String at p into out (n bytes; n = 0: just skip). Escapes keep the escaped char (\" -> ").
static bool jStr(const char *&p, char *out, size_t n) {
  if (*p != '"') return false;
  size_t k = 0;
  for (p++; *p != '"'; p++) {
    if (!*p || (*p == '\\' && !*++p)) return false;
    if (k + 1 < n) out[k++] = *p;
  }
  p++;
  if (n) out[k] = 0;
  return true;
}

static bool jSkip(const char *&p) {
  if (*p == '"') return jStr(p, nullptr, 0);
  if (*p == '{' || *p == '[') {
    int depth = 0;
    do {
      if (*p == '"') {
        if (!jStr(p, nullptr, 0)) return false;
        continue;
      }
      if (!*p) return false;
      if (*p == '{' || *p == '[') depth++;
      else if (*p == '}' || *p == ']') depth--;
      p++;
    } while (depth);
    return true;
  }
  while (*p && !strchr(",}] \r\n\t", *p)) p++;       // number / true / false / null
  return true;
}

// Calls f(key, p) with p at each member's value; a value f does not read is skipped.
template <class F> static bool jObject(const char *&p, F f) {
  ws(p);
  if (*p++ != '{') return false;
  ws(p);
  if (*p == '}') {
    p++;
    return true;
  }
  for (;;) {
    char key[16];
    ws(p);
    if (!jStr(p, key, sizeof(key))) return false;
    ws(p);
    if (*p++ != ':') return false;
    ws(p);
    const char *v = p;
    if (!f(key, p) || (p == v && !jSkip(p))) return false;
    ws(p);
    if (*p == ',') { p++; continue; }
    return *p++ == '}';
  }
}

static bool readEntry(const char *&p, Meta &e) {
  return jObject(p, [&e](const char *k, const char *&p) {
    struct { const char *k; char *dst; size_t n; } S[] = {{"id", e.id, sizeof(e.id)}, {"name", e.name, sizeof(e.name)},
        {"license", e.license, sizeof(e.license)}, {"homepage", e.homepage, sizeof(e.homepage)},
        {"updated", e.updated, sizeof(e.updated)}};
    for (auto &s : S)
      if (!strcmp(k, s.k)) return *p == '"' ? jStr(p, s.dst, s.n) : true;
    uint32_t *num = !strcmp(k, "count") ? &e.count : !strcmp(k, "bytes") ? &e.bytes : !strcmp(k, "crc") ? &e.crc : nullptr;
    if (num && isdigit((unsigned char)*p)) *num = strtoul(p, (char **)&p, 10);
    return true;
  });
}

static void parseIndex() {
  File f = LittleFS.open(INDEX, "r");
  if (!f) return;
  String js = f.size() <= 8192 ? f.readString() : String();
  f.close();
  const char *p = js.c_str();
  bool ok = jObject(p, [](const char *k, const char *&p) {
    if (strcmp(k, "lists")) return true;
    if (*p++ != '[') return false;
    for (;;) {
      ws(p);
      if (*p == ']') {
        p++;
        return true;
      }
      Meta e{};
      if (!readEntry(p, e)) return false;
      int i = addList(e.id);
      if (i >= 0) {
        if (!e.name[0]) strlcpy(e.name, e.id, sizeof(e.name));
        L[i].m = e;
      }
      ws(p);
      if (*p == ',') p++;
    }
  });
  Serial.printf("[BL] index.json %s, %d lists\n", ok ? "ok" : "malformed", s_n);
}

// ================================================================ settings, begin
static void saveEnabled() {
  String v;
  for (int i = 0; i < s_n; i++)
    if (L[i].enabled) v += (v.length() ? "," : "") + String(L[i].m.id);
  Preferences p;
  p.begin("portal", false);
  p.putString("bl", v);
  p.end();
}

void begin() {
  LittleFS.mkdir("/bl");
  for (const Meta &b : BUILTIN) L[s_n++].m = b;
  // Add the enabled ids first. Then a full index.json cannot push an enabled list out of the table.
  Preferences pr;
  pr.begin("portal", true);
  String v = pr.getString("bl", "");
  pr.end();
  for (int a = 0; a < (int)v.length();) {
    int b = v.indexOf(',', a);
    if (b < 0) b = v.length();
    int i = addList(v.substring(a, b).c_str());
    if (i >= 0) L[i].enabled = true;
    a = b + 1;
  }
  parseIndex();
  char p[40];
  for (int i = 0; i < s_n; i++) {
    List &l = L[i];
    path(p, l, "tmp");
    LittleFS.remove(p);                               // a download cut off by a reboot
    if (l.enabled) {
      l.want = openList(l) != NetFetch::E_NONE;
      s_checkDue = true;
    } else {
      path(p, l, "bin");
      LittleFS.remove(p);                             // disabled lists keep no file
    }
  }
}

uint32_t setEnabled(int i, bool on) {
  List &l = L[i];
  if (on == l.enabled) return 0;
  if (on) {
    const uint32_t need = (l.m.bytes + 1023) / 1024;  // 0 = size unknown yet: the download checks it
    if (need > freeKb()) return need;
    l.want = true;
    s_checkDue = true;
  } else {
    char p[40];
    closeList(l);
    path(p, l, "bin");
    LittleFS.remove(p);
    l.want = false;
    Serial.printf("[BL] %s disabled, free %u KB\n", l.m.id, (unsigned)freeKb());
  }
  l.enabled = on;
  l.failed = false;
  saveEnabled();
  return 0;
}

Info info(int i) {
  const List &l = L[i];
  Info r{l.m.id, l.m.name, l.m.license, l.m.homepage, l.m.updated, OFF, l.hs ? l.count : l.m.count,
         l.hs ? l.bytes : l.m.bytes, 0, l.hs != nullptr, l.err};
  if (!l.enabled) r.state = OFF;
  else if (s_job == NetFetch::JOB_LIST_FETCH && s_cur == i) {
    r.state = LOADING;
    r.progress = NetFetch::status().progress;
  } else if (l.failed) r.state = FAILED;
  else if (l.want || !l.hs) r.state = QUEUED;
  else r.state = READY;
  return r;
}

uint32_t stateCode() {
  uint32_t c = busy();
  for (int i = 0; i < s_n; i++) c = c << 3 | info(i).state;
  return c;
}

bool pending() {
  for (int i = 0; i < s_n; i++) {
    const State st = info(i).state;
    if (st == QUEUED || st == LOADING) return true;
  }
  return busy();
}

// ================================================================ scheduler (loop thread)
// A finished download: swap the verified .tmp in. If the user disabled the list meanwhile, delete the .tmp.
// The old PSRAM data stays active until the new file is in PSRAM.
// On failure the old data stays active, and the list tries again in an hour.
static void fetchDone(List &l, const NetFetch::Status &st) {
  char tmp[40], bin[40];
  path(tmp, l, "tmp");
  path(bin, l, "bin");
  l.want = false;
  l.err = st;
  if (st.state == NetFetch::DONE && l.enabled) {
    LittleFS.remove(bin);
    l.err.err = LittleFS.rename(tmp, bin) ? openList(l) : NetFetch::E_FS;
    if (l.err.err == NetFetch::E_NONE) {
      l.failed = false;
      return;
    }
  }
  LittleFS.remove(tmp);
  if (!l.enabled) return;
  l.failed = l.want = true;
  l.retryAt = millis() + HOUR_MS;
  Serial.printf("[BL] %s: download failed (err %u), %s, retry in 1 h\n", l.m.id, l.err.err,
                l.hs ? "old data stays active" : "not active");
}

void loop() {
  using namespace NetFetch;
  if (s_job != JOB_NONE) {
    if (NetFetch::busy()) return;
    const Status st = status();
    if (s_job == JOB_LIST_FETCH) {
      fetchDone(L[s_cur], st);
    } else if (st.state == DONE) {
      s_lastCheck = millis();
      parseIndex();
      for (int i = 0; i < s_n; i++) {                 // missing or changed files -> download
        List &l = L[i];
        if (l.enabled && (!l.hs || (l.m.crc && l.m.crc != l.crc))) {
          l.want = true;
          l.failed = false;
        }
      }
    } else {
      s_lastCheck = millis() - DAY_MS + HOUR_MS;      // no index (offline, no release): retry in an hour
    }
    s_job = JOB_NONE;
    return;
  }
  static uint32_t lastTry;
  if (millis() - lastTry < 1000 || NetFetch::busy() || !Net::internetOk()) return;
  lastTry = millis();
  bool any = false;
  for (int i = 0; i < s_n; i++) any |= L[i].enabled;
  if (!any) return;

  if (s_checkDue || millis() - s_lastCheck > DAY_MS) {
    if (start(JOB_LIST_INDEX) == E_BUSY) return;      // a file OTA upload: try again later
    s_checkDue = false;
    s_job = JOB_LIST_INDEX;
    return;
  }
  for (int i = 0; i < s_n; i++) {
    List &l = L[i];
    if (!l.enabled || !l.want || (l.failed && (int32_t)(millis() - l.retryAt) < 0)) continue;
    // An update with no room for two files: delete the old file. Its PSRAM data stays active.
    // With too little RAM for TLS, start() refuses the job. Then the file must stay.
    if (l.hs && (l.m.bytes + 1023) / 1024 > freeKb() &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= NetFetch::MIN_HEAP) {
      char bin[40];
      path(bin, l, "bin");
      LittleFS.remove(bin);
    }
    if (start(JOB_LIST_FETCH, l.m.id) == E_BUSY) return;
    s_job = JOB_LIST_FETCH;
    s_cur = i;
    return;
  }
}

}  // namespace PresetLists
