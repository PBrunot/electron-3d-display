/**
 * @file web_remote.h
 * @brief Phone remote control: Wi-Fi soft-AP + captive-portal DNS + HTTP server serving a
 *        touch-friendly page (net/web_remote_page.h) to pick elements/orbitals.
 *
 * Runs entirely on its own tasks (httpd's, plus one DNS task, both on kWebRemoteTaskCore);
 * the only coupling to the render loop is ux/remote_command.h's lock-free mailbox, which
 * chooser.cpp/atom_view.cpp/orbital_view.cpp poll once per frame. Tilt gestures keep working
 * unchanged alongside it. Network name/password/channel: config/network_constants.h.
 */
#pragma once

/// Starts the access point and servers, then returns. Failures are logged, not fatal -- the
/// hologram keeps running tilt-only. Callers gate on config/network_constants.h's
/// kWebRemoteEnabled with `if constexpr`, so the CYD build never references (and the linker
/// drops) the Wi-Fi/httpd code.
void startWebRemote();
