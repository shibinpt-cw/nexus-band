// Network settings. Copy the four lines below into include/config_local.h and
// fill them in; config_local.h is git-ignored so passwords stay off GitHub.
#pragma once

#if __has_include("config_local.h")
#include "config_local.h"
#else
#define WIFI_SSID      "your-wifi"
#define WIFI_PASSWORD  "your-password"
#define LAPTOP_HOST    "192.168.1.10"   // PC running ATS 10 (shown on its dashboard page)
#define PAIR_KEY       "ABC123"         // one-time key the laptop shows under "connect a device"
#endif

#define LAPTOP_PORT     8001            // ATS 10 dashboard, HTTPS + WSS
#define MIC_SAMPLE_RATE 16000
