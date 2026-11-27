#pragma once
/*
 * Config — persistent settings stored in NVS (non-volatile storage), plus the rules that
 * validate them. The same validation is used for values loaded from NVS and for values
 * submitted from the web form, so a bad value can never stop the access point from starting.
 */
#include <Arduino.h>
#include <IPAddress.h>
#include "i18n.h"

namespace Config {

// Factory defaults (first boot and after a password reset)
extern const char *const DEFAULT_AP_SSID;
extern const char *const DEFAULT_AP_PASS;

// Our own access point network
extern const IPAddress AP_IP;       // 192.168.4.1 — also the panel and DNS address for AP clients
extern const IPAddress AP_MASK;
extern const IPAddress DHCP_START;

// Fixed address in the router's network, so the router can use the board as its DNS server
struct StaticIp {
  bool      on = false;
  IPAddress ip, mask, gw;
};

struct Settings {
  String   apSsid, apPass;    // our access point
  String   staSsid, staPass;  // home Wi-Fi (uplink); empty SSID = do not connect
  String   webPass;           // web panel password (login "admin"); empty = no password
  StaticIp st;
  I18n::Lang lang = I18n::Lang::UK;   // web panel language
};

extern Settings settings;

void load();          // read settings from NVS (invalid values fall back to defaults)
void loadDns();       // apply DNS / anti-bypass settings — call after dns.begin()
void save();          // write everything (including DNS state) to NVS
void resetPasswords();// erase only AP name/password and panel password

// ---- Validation (shared by NVS loading and the web form) ----
bool sameNet(IPAddress a, IPAddress b, IPAddress mask);
bool inApSubnet(IPAddress ip);
bool validApConfig(const String &ssid, const String &pass);
// Parses the strings into out. Returns an error message, or "" if the address is usable.
String checkStatic(const String &ip, const String &mask, const String &gw, StaticIp &out);

}  // namespace Config
