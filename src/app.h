#pragma once
// Objects shared by several modules. They are defined once in main.cpp.
#include <WebServer.h>
#include "dns_filter.h"

extern WebServer server;   // HTTP server for the web panel (port 80)
extern DnsFilter dns;      // DNS server with ad blocking (port 53)
