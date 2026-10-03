// Web remote (net/web_remote.cpp): the board runs its own Wi-Fi access point -- no router,
// no credentials to configure per venue -- and a captive-portal DNS that answers every name
// with the board's own address, so a phone that joins the network gets the control page
// popped up automatically (the OS's "sign in to network" sheet), or can open any http:// URL.
#pragma once

#include <cstdint>

#include "config/board.h" // kIsCYD

/// CYD has no PSRAM and its internal RAM is already fully spoken for (see CYD-branch.md) --
/// the Wi-Fi driver + lwIP + httpd need tens of KB of internal heap, so the remote is S3-only.
inline constexpr bool kWebRemoteEnabled = !kIsCYD;

/// Extreme-tilt navigation gestures (ux/tilt_gesture.h) are off whenever the web remote is on:
/// the phone drives element/orbital selection, and the full tilt range is left to
/// ux/orientation_tracker.h's continuous rotation instead of firing a menu move at ~27 degrees.
/// Also skips the boot-time direction calibration, which only exists for those gestures.
inline constexpr bool kTiltNavigationEnabled = !kWebRemoteEnabled;

inline constexpr const char *kWebRemoteSsid = "Ologramma-Atomi";
/// Empty = open network (friendliest for walk-up visitors). Otherwise WPA2, min 8 chars.
inline constexpr const char *kWebRemotePassword = "";
inline constexpr uint8_t kWebRemoteChannel = 6;
inline constexpr uint8_t kWebRemoteMaxClients = 4;

/// Default ESP-IDF soft-AP address; the captive DNS answers every query with it.
inline constexpr uint8_t kWebRemoteIp[4] = {192, 168, 4, 1};

/// Server tasks run on core 1 so they never compete with the render loop (app_main, core 0).
inline constexpr int kWebRemoteTaskCore = 1;
inline constexpr uint32_t kWebRemoteDnsStackBytes = 3072;
