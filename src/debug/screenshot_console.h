/**
 * @file screenshot_console.h
 * @brief Line-oriented command console over the existing USB/UART console (the same link
 *        `pio device monitor` already uses for logs and flashing) for triggering and pulling
 *        screenshots without any new hardware, gesture, or USB peripheral. See
 *        pc/pull_screenshots.py for the PC-side counterpart.
 *
 * Protocol: commands are plain text lines terminated by '\n'; every reply line is prefixed
 * "SS_" so a PC script can pick protocol lines out of interleaved ESP_LOG output (anything
 * NOT starting with "SS_" is just a log line and safe to ignore).
 *
 *   s | SS_CAP        -> capture the current frame; replies "SS_CAPTURED <name> <size>"
 *   l | SS_LIST        -> one "SS_FILE <name> <size>" line per screenshot, then "SS_LIST_END"
 *   a | SS_CAP_ALL      -> renders and saves every orbital preset + curated element, plus a
 *                          couple of Fe shell-dissection stills (see screenshot_batch.h) --
 *                          replies "SS_CAP_ALL_START", then blocks (progress via ESP_LOGI,
 *                          not the SS_ protocol) until done, then "SS_CAP_ALL_DONE"
 *   SS_GET <name>       -> "SS_BEGIN <name> <size> <crc32-hex>", then base64 data in
 *                          "SS_DATA <chunk>" lines, then "SS_END"
 *   SS_DEL <name>       -> deletes the file; replies "SS_DELETED <name>" or "SS_ERR <why>"
 * Unrecognized input gets "SS_ERR unknown command".
 */
#pragma once

#include "render/display.h"

/// Mounts /storage (via screenshot::init()) and starts the console task. Call once at boot,
/// after `Display display{};` -- the task reads display.getFrameBuf() directly (not thread-
/// safe against the render loop, but a screenshot is a rare, human-triggered debug action:
/// worst case is a single frame with visible tearing between the two halves of the buffer,
/// which is a non-issue for a debug snapshot).
void startScreenshotConsole(Display &display);
