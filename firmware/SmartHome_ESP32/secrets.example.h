// secrets.example.h
// Copy this file to secrets.h (same folder) and fill in your own values.
// secrets.h is listed in .gitignore, so your credentials never reach GitHub.
#pragma once

// Wi-Fi: WPA2-Enterprise (PEAP), e.g. a university network
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_USERNAME  "YOUR_WIFI_USERNAME"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// Firebase (Project settings > General > Web API key)
#define API_KEY        "YOUR_FIREBASE_WEB_API_KEY"
#define USER_EMAIL     "YOUR_ESP32_USER_EMAIL"      // a Firebase Auth user for the ESP32
#define USER_PASSWORD  "YOUR_ESP32_USER_PASSWORD"
#define DATABASE_URL   "https://YOUR_PROJECT_ID-default-rtdb.firebaseio.com/"
