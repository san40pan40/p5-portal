#include "app_config.h"
#include <Preferences.h>
#include <lwip/ip4_addr.h>   // ip4_addr_netmask_valid
#include "app.h"
#include "anti_bypass.h"

namespace Config {

const char *const DEFAULT_AP_SSID = "ESP32-Portal";
const char *const DEFAULT_AP_PASS = "12345678";

const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_MASK(255, 255, 255, 0);
const IPAddress DHCP_START(192, 168, 4, 2);

Settings settings;

// All NVS keys live here. Some names are historical ("guard_*") — renaming them
// would silently reset users' settings after a firmware update.
static const char *NS           = "portal";
static const char *K_AP_SSID    = "ap_ssid";
static const char *K_AP_PASS    = "ap_pass";
static const char *K_STA_SSID   = "sta_ssid";
static const char *K_STA_PASS   = "sta_pass";
static const char *K_WEB_PASS   = "web_pass";
static const char *K_ST_ON      = "st_on";
static const char *K_ST_IP      = "st_ip";
static const char *K_ST_MASK    = "st_mask";
static const char *K_ST_GW      = "st_gw";
static const char *K_DNS_UP     = "dns_up";
static const char *K_DNS_ON     = "dns_on";
static const char *K_GUARD_ON   = "guard_on";
static const char *K_GUARD_DOH  = "guard_doh_ip";
static const char *K_LANG       = "lang";
static const char *K_NETS       = "nets";    // uplink list; the sta_* / st_* keys are its legacy form

static Preferences prefs;

// ---------------------------------------------------------------- validation
bool sameNet(IPAddress a, IPAddress b, IPAddress mask) {
  return ((uint32_t)a & (uint32_t)mask) == ((uint32_t)b & (uint32_t)mask);
}

bool inApSubnet(IPAddress ip) { return sameNet(ip, AP_IP, AP_MASK); }

bool validApConfig(const String &ssid, const String &pass) {
  return ssid.length() >= 1 && ssid.length() <= 32 && pass.length() >= 8 && pass.length() <= 63;
}

String checkStatic(const String &sip, const String &smask, const String &sgw, StaticIp &out) {
  if (!out.ip.fromString(sip) || !out.mask.fromString(smask) || !out.gw.fromString(sgw)) return T(CS_FORMAT);
  return checkStatic(out);
}

String checkStatic(const StaticIp &st) {
  if (!(uint32_t)st.ip || !(uint32_t)st.gw) return T(CS_FORMAT);
  // The mask must be contiguous: ones on the left, zeros on the right
  if (!(uint32_t)st.mask || !ip4_addr_netmask_valid((uint32_t)st.mask)) return T(CS_MASK);
  if (!sameNet(st.ip, st.gw, st.mask)) return T(CS_SAME_NET);
  if (st.ip == st.gw) return T(CS_IP_IS_GW);
  // The router network must not overlap the AP network, or replies to AP clients get misrouted
  if (inApSubnet(st.ip) || inApSubnet(st.gw) || sameNet(AP_IP, st.ip, st.mask))
    return T(CS_OVERLAP) + AP_IP.toString() + T(CS_OVERLAP2);
  return "";
}

// Stored networks are accepted leniently (any password up to 64 chars, e.g. 5/13-char WEP or
// a 64-hex PSK), so an update never drops a network that worked. New input is checked
// strictly in the settings page.
static bool storedNetOk(const String &ssid, const String &pass) {
  return ssid.length() >= 1 && ssid.length() <= 32 && pass.length() <= 64;
}

// ---------------------------------------------------------------- uplink networks blob
// Key "nets" = [uint8 version][n x NetRec]. One key, so the whole list is written atomically.
static const uint8_t NETS_VER = 1;
struct __attribute__((packed)) NetRec {
  char     ssid[33];
  char     pass[64];     // NUL-terminated, or exactly 64 chars without NUL (hex PSK)
  uint8_t  stOn;
  uint32_t ip, mask, gw;
};

// Written by a newer firmware (unknown blob version): never overwrite it, so a downgrade
// followed by an upgrade does not lose the list
static bool s_netsReadOnly = false;

// Expects prefs open read-only. Returns false if the key does not exist.
static bool loadNets(Settings &s) {
  s.netCount = 0;
  for (auto &n : s.nets) n = KnownNet();
  if (!prefs.isKey(K_NETS)) return false;
  size_t len = prefs.getBytesLength(K_NETS);
  uint8_t *buf = len ? (uint8_t *)malloc(len) : nullptr;
  if (buf) len = prefs.getBytes(K_NETS, buf, len);
  if (!buf || len < 1 || buf[0] < NETS_VER) {
    Serial.println("[CFG] stored network list is unreadable — ignoring it");
    free(buf);
    return true;
  }
  if (buf[0] > NETS_VER) {
    Serial.printf("[CFG] network list format v%u is newer than this firmware — not loaded, kept read-only\n", buf[0]);
    s_netsReadOnly = true;
    free(buf);
    return true;
  }
  size_t count = (len - 1) / sizeof(NetRec);      // a longer blob: read the records that fit
  for (size_t i = 0; i < count && s.netCount < MAX_NETS; i++) {
    NetRec r;
    memcpy(&r, buf + 1 + i * sizeof(NetRec), sizeof(r));
    r.ssid[sizeof(r.ssid) - 1] = 0;
    char pass[sizeof(r.pass) + 1];
    memcpy(pass, r.pass, sizeof(r.pass));
    pass[sizeof(r.pass)] = 0;
    KnownNet n;
    n.ssid = r.ssid;
    n.pass = pass;
    if (!storedNetOk(n.ssid, n.pass)) {
      Serial.printf("[CFG] dropping invalid stored network #%u\n", (unsigned)i);
      continue;
    }
    n.st.on = r.stOn;
    n.st.ip = r.ip; n.st.mask = r.mask; n.st.gw = r.gw;
    if (n.st.on && checkStatic(n.st).length()) {
      Serial.printf("[CFG] static IP of \"%s\" is invalid — using DHCP\n", n.ssid.c_str());
      n.st.on = false;
    }
    s.nets[s.netCount++] = n;
  }
  free(buf);
  return true;
}

// Expects prefs open read-write
static void saveNets(const Settings &s) {
  uint8_t buf[1 + MAX_NETS * sizeof(NetRec)];
  buf[0] = NETS_VER;
  uint8_t n = s.netCount > MAX_NETS ? MAX_NETS : s.netCount;
  for (uint8_t i = 0; i < n; i++) {
    const KnownNet &k = s.nets[i];
    NetRec r;
    memset(&r, 0, sizeof(r));
    strlcpy(r.ssid, k.ssid.c_str(), sizeof(r.ssid));
    strncpy(r.pass, k.pass.c_str(), sizeof(r.pass));   // 64 chars fit without the NUL
    r.stOn = k.st.on;
    r.ip = (uint32_t)k.st.ip; r.mask = (uint32_t)k.st.mask; r.gw = (uint32_t)k.st.gw;
    memcpy(buf + 1 + i * sizeof(NetRec), &r, sizeof(r));
  }
  prefs.putBytes(K_NETS, buf, 1 + n * sizeof(NetRec));
}

// ---------------------------------------------------------------- load / save
void load() {
  Settings &s = settings;
  prefs.begin(NS, true);
  s.apSsid  = prefs.getString(K_AP_SSID, DEFAULT_AP_SSID);
  s.apPass  = prefs.getString(K_AP_PASS, DEFAULT_AP_PASS);
  s.webPass = prefs.getString(K_WEB_PASS, "");
  s.lang    = prefs.getUChar(K_LANG, 0) == 1 ? I18n::Lang::EN : I18n::Lang::UK;
  I18n::setLang(s.lang);

  // Uplink networks. Older firmware kept one network in sta_* / st_* keys: migrate it once.
  bool migrate = false;
  if (!loadNets(s)) {
    KnownNet n;
    n.ssid = prefs.getString(K_STA_SSID, "");
    n.pass = prefs.getString(K_STA_PASS, "");
    n.st.on = prefs.getBool(K_ST_ON, false);
    String err = checkStatic(prefs.getString(K_ST_IP, ""), prefs.getString(K_ST_MASK, "255.255.255.0"),
                             prefs.getString(K_ST_GW, ""), n.st);
    if (n.st.on && err.length()) {
      Serial.println("[CFG] stored static IP is invalid — using DHCP");
      n.st.on = false;
    }
    if (storedNetOk(n.ssid, n.pass)) {
      s.nets[0] = n;
      s.netCount = 1;
      migrate = true;
    }
  }
  prefs.end();

  if (migrate) {
    // Only the new key is written (the legacy keys stay for a downgrade). Not save():
    // DNS settings are not loaded yet and would be overwritten with defaults.
    prefs.begin(NS, false);
    saveNets(s);
    prefs.end();
    Serial.println("[CFG] migrated the uplink network to the new list format");
  }
  if (!validApConfig(s.apSsid, s.apPass)) {
    Serial.println("[CFG] stored AP settings are invalid — using factory defaults");
    s.apSsid = DEFAULT_AP_SSID;
    s.apPass = DEFAULT_AP_PASS;
  }
}

void loadDns() {
  prefs.begin(NS, true);
  dns.setEnabled(prefs.getBool(K_DNS_ON, true));
  IPAddress up;
  if (up.fromString(prefs.getString(K_DNS_UP, ""))) dns.setUpstream(up);   // rejected -> stays 1.1.1.1
  AntiBypass::setEnabled(prefs.getBool(K_GUARD_ON, true));
  AntiBypass::setBlockDohIps(prefs.getBool(K_GUARD_DOH, false));
  prefs.end();
}

void save() {
  const Settings &s = settings;
  prefs.begin(NS, false);
  prefs.putString(K_AP_SSID, s.apSsid);
  prefs.putString(K_AP_PASS, s.apPass);
  if (s_netsReadOnly) {
    Serial.println("[CFG] network list is read-only (newer format) — not saved");
  } else {
    saveNets(s);
    // Mirror the first network into the legacy keys, so an older firmware still connects
    const KnownNet &n0 = s.nets[0];
    const bool any = s.netCount > 0;
    prefs.putString(K_STA_SSID, any ? n0.ssid : String());
    prefs.putString(K_STA_PASS, any ? n0.pass : String());
    prefs.putBool(K_ST_ON, any && n0.st.on);
    prefs.putString(K_ST_IP, any && n0.st.on ? n0.st.ip.toString() : String());
    prefs.putString(K_ST_MASK, any && n0.st.on ? n0.st.mask.toString() : String("255.255.255.0"));
    prefs.putString(K_ST_GW, any && n0.st.on ? n0.st.gw.toString() : String());
  }
  prefs.putString(K_WEB_PASS, s.webPass);
  prefs.putString(K_DNS_UP, dns.upstream().toString());
  prefs.putBool(K_DNS_ON, dns.enabled());
  prefs.putBool(K_GUARD_ON, AntiBypass::enabled());
  prefs.putBool(K_GUARD_DOH, AntiBypass::blockDohIps());
  prefs.putUChar(K_LANG, (uint8_t)s.lang);
  prefs.end();
}

void resetPasswords() {
  prefs.begin(NS, false);
  prefs.remove(K_AP_SSID);   // next boot uses DEFAULT_AP_SSID / DEFAULT_AP_PASS
  prefs.remove(K_AP_PASS);
  prefs.remove(K_WEB_PASS);  // panel without a password
  prefs.end();
}

}  // namespace Config
