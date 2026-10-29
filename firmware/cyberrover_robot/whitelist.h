// CyberRover — Network whitelist (القائمة البيضاء)
// Fill this with the facility's real access points before a patrol.
// Get BSSIDs from the controller / AP labels, or run one patrol and copy them from the log.
// The values below are EXAMPLES.
#pragma once
#include <WiFi.h>
#include <string.h>
#include <strings.h>

struct WhitelistEntry {
  const char *ssid;
  const char *bssid;          // "AA:BB:CC:DD:EE:FF" (uppercase)
  int channel;                // 0 = any channel (for APs with auto channel)
  wifi_auth_mode_t minAuth;   // weakest security this AP is allowed to use
};

// Network names that belong to the facility. Any radio broadcasting one of these
// names without a whitelisted BSSID is reported as an Evil Twin.
static const char *PROTECTED_SSIDS[] = { "Hotel-Guest", "Hotel-Staff" };

static const WhitelistEntry WHITELIST[] = {
  { "Hotel-Guest", "AC:84:C6:10:20:01", 1,  WIFI_AUTH_WPA2_PSK },
  { "Hotel-Guest", "AC:84:C6:10:20:02", 6,  WIFI_AUTH_WPA2_PSK },
  { "Hotel-Guest", "AC:84:C6:10:20:03", 11, WIFI_AUTH_WPA2_PSK },
  { "Hotel-Staff", "AC:84:C6:10:30:01", 0,  WIFI_AUTH_WPA2_ENTERPRISE },
};

inline const WhitelistEntry *findByBssid(const char *bssid) {
  for (const auto &w : WHITELIST)
    if (strcasecmp(w.bssid, bssid) == 0) return &w;
  return nullptr;
}

inline bool isProtectedSsid(const char *ssid) {
  for (const char *s : PROTECTED_SSIDS)
    if (strcmp(s, ssid) == 0) return true;
  return false;
}
