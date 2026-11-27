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
  // The mask must be contiguous: ones on the left, zeros on the right
  if (!(uint32_t)out.mask || !ip4_addr_netmask_valid((uint32_t)out.mask)) return T(CS_MASK);
  if (!sameNet(out.ip, out.gw, out.mask)) return T(CS_SAME_NET);
  if (out.ip == out.gw) return T(CS_IP_IS_GW);
  // The router network must not overlap the AP network, or replies to AP clients get misrouted
  if (inApSubnet(out.ip) || inApSubnet(out.gw) || sameNet(AP_IP, out.ip, out.mask))
    return T(CS_OVERLAP) + AP_IP.toString() + T(CS_OVERLAP2);
  return "";
}

// ---------------------------------------------------------------- load / save
void load() {
  Settings &s = settings;
  prefs.begin(NS, true);
  s.apSsid  = prefs.getString(K_AP_SSID, DEFAULT_AP_SSID);
  s.apPass  = prefs.getString(K_AP_PASS, DEFAULT_AP_PASS);
  s.staSsid = prefs.getString(K_STA_SSID, "");
  s.staPass = prefs.getString(K_STA_PASS, "");
  s.webPass = prefs.getString(K_WEB_PASS, "");
  s.st.on   = prefs.getBool(K_ST_ON, false);
  s.lang    = prefs.getUChar(K_LANG, 0) == 1 ? I18n::Lang::EN : I18n::Lang::UK;
  I18n::setLang(s.lang);
  String err = checkStatic(prefs.getString(K_ST_IP, ""), prefs.getString(K_ST_MASK, "255.255.255.0"),
                           prefs.getString(K_ST_GW, ""), s.st);
  prefs.end();

  if (s.st.on && err.length()) {
    Serial.println("[CFG] stored static IP is invalid — using DHCP");
    s.st.on = false;
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
  prefs.putString(K_STA_SSID, s.staSsid);
  prefs.putString(K_STA_PASS, s.staPass);
  prefs.putString(K_WEB_PASS, s.webPass);
  prefs.putBool(K_ST_ON, s.st.on);
  prefs.putString(K_ST_IP, (uint32_t)s.st.ip ? s.st.ip.toString() : String());
  prefs.putString(K_ST_MASK, (uint32_t)s.st.mask ? s.st.mask.toString() : String("255.255.255.0"));
  prefs.putString(K_ST_GW, (uint32_t)s.st.gw ? s.st.gw.toString() : String());
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
