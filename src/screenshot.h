/**
 * @file screenshot.h
 * @brief Owns the "storage" SPIFFS partition (see partitions_16M.csv) and the file lifecycle
 *        for on-device screenshots: mount, capture (PNG-encode via png_writer.h and write to
 *        flash), list, read, delete. Pure filesystem/encoding logic -- the serial trigger and
 *        pull protocol live in screenshot_console.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace screenshot
{
    /// Longest filename this module hands back (matches CONFIG_SPIFFS_OBJ_NAME_LEN).
    constexpr size_t kMaxNameLen = 32;

    /// Mounts the "storage" SPIFFS partition at /storage, formatting it on first use if no
    /// filesystem is found there yet. Call once at boot before capture()/forEachFile()/etc.
    /// Logs and returns without mounting on failure (screenshot features become no-ops, not a
    /// boot-time abort -- this is a debug utility, not core functionality).
    void init();

    /// Captures `frameBuf` (Display::kDisplayWidth x Display::kDisplayHeight RGB565, row-
    /// major, same layout as Display::getFrameBuf()) as a new PNG file (auto-numbered
    /// shot_0001.png, shot_0002.png, ...). On success, fills outName (bare filename, no
    /// "/storage/" prefix) and outSize and returns true.
    bool capture(const uint16_t *frameBuf, char *outName, size_t outNameCapacity, size_t *outSize);

    /// Like capture(), but under a caller-chosen bare filename (e.g. "orbital_1s.png")
    /// instead of an auto-numbered one -- used by screenshot_batch.cpp's preset gallery, so
    /// pulled files land with the same names pc/screenshot.py's still images use. Overwrites
    /// an existing file of the same name.
    bool captureAs(const uint16_t *frameBuf, const char *name, size_t *outSize);

    /// Callback invoked once per file found by forEachFile(), given its bare name (no
    /// "/storage/" prefix) and size in bytes.
    using FileVisitor = void (*)(const char *name, size_t size, void *ctx);

    /// Lists every file directly under /storage, calling visit(name, size, ctx) for each.
    void forEachFile(FileVisitor visit, void *ctx);

    /// Reads `name` (bare, no "/storage/" prefix) fully into a heap_caps_malloc'd
    /// (MALLOC_CAP_SPIRAM) buffer. Caller must heap_caps_free() the result. Returns nullptr
    /// (leaving *outSize unset) if the file doesn't exist or can't be read.
    uint8_t *readFile(const char *name, size_t *outSize);

    /// Deletes `name` (bare, no "/storage/" prefix). Returns true on success.
    bool deleteFile(const char *name);
} // namespace screenshot
