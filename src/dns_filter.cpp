#include "dns_filter.h"
#include "anti_bypass.h"      // AntiBypass::enabled() — whether the DoH list and the canary apply
#include "i18n.h"             // translated reasons for a rejected upstream
#include "metrics.h"          // charts history + top tables
#include <esp_random.h>
#include <iterator>           // std::size
#include <lwip/prot/dns.h>    // DNS constants from lwIP: SIZEOF_DNS_HDR, DNS_RRTYPE_A, ...

// Default blocklist — a few well-known ad/tracking domains
static const char *const DEFAULT_BLOCKLIST[] = {
  "doubleclick.net", "googlesyndication.com", "googleadservices.com",
  "adservice.google.com", "app-measurement.com", "ads.yahoo.com",
  "adnxs.com", "taboola.com", "outbrain.com", "criteo.com",
};
// Default DoH/DoT servers for the Guard. A browser or phone first resolves their address through
// the system DNS (us); on 0.0.0.0 it falls back to plain DNS.
static const char *const DEFAULT_BYPASS[] = {
  "dns.google", "cloudflare-dns.com", "one.one.one.one", "doh.opendns.com",
  "dns.quad9.net", "dns.adguard.com", "dns.adguard-dns.com", "dns.nextdns.io",
  "doh.cleanbrowsing.org", "dns.mullvad.net", "doh.dns.sb", "dns.twnic.tw",
};

static const uint16_t UPSTREAM_LOCAL_PORT = 53530;
static const uint32_t UPSTREAM_TIMEOUT_MS = 3000;
static const uint8_t  RRTYPE_OPT = 41;          // EDNS0 OPT record (lwIP has no constant for it)
// Largest UDP answer we ask the upstream for. 1232 is the "DNS Flag Day 2020" value:
// such a packet always fits into a single Wi-Fi frame.
static const uint16_t MAX_EDNS_UDP = 1232;
// Response codes (RCODE)
static const uint8_t RC_OK = 0, RC_FORMERR = 1, RC_SERVFAIL = 2, RC_NXDOMAIN = 3, RC_NOTIMP = 4;

// DNS is big-endian: most significant byte first
static inline uint16_t rd16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static inline void     wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }

// Built on first use (safe regardless of global initialisation order)
const std::string &DnsFilter::firefoxCanary() {
  static const std::string s("use-application-dns.net");
  return s;
}

DnsFilter::DnsFilter()
  : _block("/blocklist.txt", DEFAULT_BLOCKLIST, std::size(DEFAULT_BLOCKLIST)),
    _bypass("/doh.txt", DEFAULT_BYPASS, std::size(DEFAULT_BYPASS)) {}

bool DnsFilter::loadLists() { return _block.load() & _bypass.load(); }   // `&`, not `&&`: always load both

// ================================================================ start and upstream
void DnsFilter::begin(IPAddress apIp, IPAddress apMask) {
  _apIp   = apIp;
  _apMask = apMask;
  _srv.begin(DNS_SERVER_PORT);               // all interfaces: AP + router network
  _up.begin(UPSTREAM_LOCAL_PORT);            // source port for upstream queries
}

// The upstream must not be ourselves or the router that asks us (a loop), nor 0.0.0.0,
// loopback or multicast/broadcast. Returns a reason (shown on the page) or nullptr.
// This is the ONLY place with these rules: setUpstream and setStaAddrs guarantee that
// _upstream is never forbidden, so forwarding does not re-check it.
const char *DnsFilter::forbiddenUpstream(IPAddress ip) const {
  if (ip[0] == 0 || ip[0] == 127 || ip[0] >= 224) return T(UP_SPECIAL);
  if (((uint32_t)ip & (uint32_t)_apMask) == ((uint32_t)_apIp & (uint32_t)_apMask)) return T(UP_AP_NET);
  if ((uint32_t)_staSelf && ip == _staSelf) return T(UP_SELF);
  if ((uint32_t)_staGw && ip == _staGw) return T(UP_ROUTER);
  return nullptr;
}

const char *DnsFilter::setUpstream(IPAddress ip) {
  const char *why = forbiddenUpstream(ip);
  if (!why) _upstream = ip;
  return why;
}

bool DnsFilter::setStaAddrs(IPAddress self, IPAddress gateway) {
  _staSelf = self;
  _staGw   = gateway;
  if (!forbiddenUpstream(_upstream)) return false;
  _upstream = defaultUpstream();
  return true;
}

// ================================================================ main loop
void DnsFilter::loop() {
  // 1) client queries (at most 8 per pass, so the web server gets its turn too)
  for (int i = 0; i < 8; i++) {
    if (_srv.parsePacket() <= 0) break;
    IPAddress ip = _srv.remoteIP();
    uint16_t port = _srv.remotePort();
    int len = _srv.read(_buf, sizeof(_buf));
    handleQuery(len, ip, port);
  }

  // 2) upstream answers
  for (int i = 0; i < 8; i++) {
    if (_up.parsePacket() <= 0) break;
    IPAddress from = _up.remoteIP();
    if (_up.remotePort() != DNS_SERVER_PORT) {
      _up.clear();      // stray packet: must be discarded, or the socket gets stuck on it
      continue;
    }
    int len = _up.read(_buf, sizeof(_buf));
    handleUpstreamReply(len, from);
  }

  // 3) expire queries the upstream never answered
  uint32_t now = millis();
  for (auto &p : _pend) {
    if (p.used && now - p.ms > UPSTREAM_TIMEOUT_MS) {
      p.used = false;
      statFailed++;
    }
  }
}

// Decode a name from a DNS packet: [3]ads[6]google[3]com[0] -> "ads.google.com".
// Writes into a stack buffer (out, at least 256 bytes) — no heap allocation per query.
static bool parseName(const uint8_t *b, int len, int &off, char *out, size_t &n) {
  n = 0;
  while (off < len) {
    uint8_t l = b[off++];
    if (l == 0) { out[n] = 0; return true; }      // end of name
    if (l & 0xC0) return false;                   // compression is not expected in a question
    if (off + l > len || n + l + 1 > 253) return false;
    if (n) out[n++] = '.';
    for (int i = 0; i < l; i++) {
      char c = (char)b[off + i];
      if (c >= 'A' && c <= 'Z') c += 32;          // DNS names are case-insensitive
      out[n++] = c;
    }
    off += l;
  }
  return false;
}

// ================================================================ client query
void DnsFilter::handleQuery(int len, IPAddress ip, uint16_t port) {
  if (len < SIZEOF_DNS_HDR) return;              // shorter than a header — garbage
  if (_buf[2] & DNS_FLAG1_RESPONSE) return;      // an answer, not a query
  if ((_buf[2] >> 3) & 0x0F) return sendHeaderOnly(ip, port, RC_NOTIMP);   // UPDATE, NOTIFY... unsupported
  if (rd16(_buf + 4) != 1) return sendHeaderOnly(ip, port, RC_FORMERR);    // exactly one question

  int off = SIZEOF_DNS_HDR;
  char name[256];
  size_t n;
  if (!parseName(_buf, len, off, name, n) || off + 4 > len) return sendHeaderOnly(ip, port, RC_FORMERR);
  uint16_t qtype = rd16(_buf + off);
  int qEnd = off + 4;                            // end of the question section

  statTotal++;
  const std::string *rule = nullptr;
  RuleSource src = RuleSource::None;
  uint8_t rcode = RC_OK;
  if (_enabled) {
    bool guard = AntiBypass::enabled();
    const std::string &canary = firefoxCanary();
    if (guard && n == canary.size() && memcmp(name, canary.data(), n) == 0) {
      rule = &canary;                            // Firefox canary -> "does not exist"
      src = RuleSource::Canary;
      rcode = RC_NXDOMAIN;
    } else if (guard && (rule = _bypass.match(name, n))) {
      src = RuleSource::Bypass;                  // DoH/DoT server
    } else if ((rule = _block.match(name, n))) {
      src = RuleSource::Blocklist;               // user blocklist
    }
  }
  addLog(name, rule, src, ip);
  Metrics::dnsQuery((uint32_t)ip, rule ? name : nullptr);

  if (rule) {
    statBlocked++;
    sendReply(qEnd, qtype, ip, port, rcode);
  } else {
    forward(len, qEnd, qtype, ip, port);
  }
}

// Build an answer from the query in _buf: copy ID and question, set rcode.
// rcode=0 for A/AAAA adds a 0.0.0.0 / :: answer ("sinkhole"); otherwise no records.
void DnsFilter::sendReply(int qEnd, uint16_t qtype, IPAddress ip, uint16_t port, uint8_t rcode) {
  bool isA = rcode == RC_OK && qtype == DNS_RRTYPE_A;
  bool isAAAA = rcode == RC_OK && qtype == DNS_RRTYPE_AAAA;
  uint8_t r[320];
  if (qEnd > 280) return;
  memcpy(r, _buf, qEnd);                         // ID + question as-is
  r[2] = DNS_FLAG1_RESPONSE | (_buf[2] & 0x79);  // response, same opcode and RD
  r[3] = 0x80 | rcode;                           // RA=1 + response code
  wr16(r + 4, 1);                                // 1 question
  wr16(r + 6, (isA || isAAAA) ? 1 : 0);          // 1 answer or none
  wr16(r + 8, 0);                                // no authority
  wr16(r + 10, 0);                               // no additional

  int p = qEnd;
  if (isA || isAAAA) {
    r[p++] = 0xC0; r[p++] = SIZEOF_DNS_HDR;      // name = pointer to the question
    wr16(r + p, qtype);          p += 2;
    wr16(r + p, DNS_RRCLASS_IN); p += 2;
    r[p++] = 0; r[p++] = 0; r[p++] = 0; r[p++] = 60;   // TTL 60 s
    int rdlen = isA ? 4 : 16;
    wr16(r + p, rdlen);          p += 2;
    memset(r + p, 0, rdlen);                     // 0.0.0.0 or ::
    p += rdlen;
  }
  _srv.beginPacket(ip, port);
  _srv.write(r, p);
  _srv.endPacket();
}

// Header-only answer (for queries we do not parse)
void DnsFilter::sendHeaderOnly(IPAddress ip, uint16_t port, uint8_t rcode) {
  uint8_t r[SIZEOF_DNS_HDR];
  memcpy(r, _buf, 2);                            // same ID
  r[2] = DNS_FLAG1_RESPONSE | (_buf[2] & 0x79);
  r[3] = 0x80 | rcode;
  memset(r + 4, 0, 8);                           // no sections
  _srv.beginPacket(ip, port);
  _srv.write(r, sizeof(r));
  _srv.endPacket();
}

DnsFilter::Pending *DnsFilter::findPending(uint16_t id) {
  for (auto &p : _pend)
    if (p.used && p.newId == id) return &p;
  return nullptr;
}

// Forward to the upstream. The ID is replaced with ours so queries from different clients never mix.
void DnsFilter::forward(int len, int qEnd, uint16_t qtype, IPAddress ip, uint16_t port) {
  // A free slot; if all are busy, evict the oldest (by age now-ms, which stays correct
  // across millis() overflow) and count it as a failure
  uint32_t now = millis();
  int slot = -1, oldest = 0;
  uint32_t oldestAge = 0;
  for (int i = 0; i < MAX_PENDING; i++) {
    if (!_pend[i].used) { slot = i; break; }
    uint32_t age = now - _pend[i].ms;
    if (age >= oldestAge) { oldestAge = age; oldest = i; }
  }
  if (slot < 0) { slot = oldest; statFailed++; }

  // Random ID, so a spoofed answer cannot be guessed
  uint16_t id;
  do { id = esp_random() & 0xFFFF; } while (findPending(id));

  Pending &p = _pend[slot];
  p.used     = true;
  p.origId   = rd16(_buf);
  p.newId    = id;
  p.ip       = ip;
  p.port     = port;
  p.ms       = now;
  p.upstream = _upstream;

  // EDNS0: if the client allows large answers, clamp to 1232 bytes — larger packets would not
  // pass through us intact. The OPT record follows the question (exactly one, checked in
  // handleQuery): name 0x00, type 41, "class" = max UDP size.
  static_assert(MAX_EDNS_UDP <= sizeof(_buf), "the buffer must hold the largest answer");
  if (rd16(_buf + 10) && !rd16(_buf + 6) && !rd16(_buf + 8) && qEnd + 11 <= len &&
      _buf[qEnd] == 0 && rd16(_buf + qEnd + 1) == RRTYPE_OPT &&
      rd16(_buf + qEnd + 3) > MAX_EDNS_UDP) {
    wr16(_buf + qEnd + 3, MAX_EDNS_UDP);
  }

  wr16(_buf, id);
  _up.beginPacket(_upstream, DNS_SERVER_PORT);
  _up.write(_buf, len);
  if (_up.endPacket()) {
    statForwarded++;
    return;
  }
  // Could not send (no router): answer SERVFAIL right away so the client does not wait 5 s
  p.used = false;
  wr16(_buf, p.origId);
  statFailed++;
  sendReply(qEnd, qtype, ip, port, RC_SERVFAIL);
}

// Upstream answer: find whose query it is (and that the same server we asked replied),
// restore the original ID and pass it to the client
void DnsFilter::handleUpstreamReply(int len, IPAddress from) {
  if (len < SIZEOF_DNS_HDR) return;
  Pending *p = findPending(rd16(_buf));
  if (!p || p->upstream != from) return;
  wr16(_buf, p->origId);
  _srv.beginPacket(p->ip, p->port);
  _srv.write(_buf, len);
  _srv.endPacket();
  p->used = false;
  statUpMs += millis() - p->ms;
  statUpN++;
}

// ================================================================ log
void DnsFilter::addLog(const char *domain, const std::string *rule, RuleSource src, IPAddress ip) {
  DnsLogEntry &e = _log.next();                  // filled in place: strings reuse their memory
  e.domain = domain;
  e.rule   = rule ? rule->c_str() : "";
  e.src    = src;
  e.client = ip;
  e.ms     = millis();
}
