/**
 * @file crc32.h
 * @brief Standard CRC-32/ISO-HDLC (the zlib/PNG/gzip variant): poly 0xEDB88320 (reflected),
 *        init and final XOR both 0xFFFFFFFF. Used by png_writer.cpp (PNG chunk CRCs) and
 *        screenshot_console.cpp (transfer integrity check for pulled files).
 */
#pragma once

#include <cstddef>
#include <cstdint>

/// Table-free bit-loop implementation -- this project only needs CRC32 for occasional
/// one-shot screenshot files, not a hot path, so the code-size savings over a 1KB lookup
/// table are worth the (still trivial, sub-millisecond for ~200KB) extra cycles.
uint32_t crc32(const uint8_t *data, size_t len);
