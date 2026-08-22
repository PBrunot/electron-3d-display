/**
 * @file png_writer.h
 * @brief Dependency-free 8-bit RGB PNG encoder for on-device screenshots (see screenshot.h),
 *        with real compression so screenshots don't burn through the flash partition at
 *        ~173KB/frame uncompressed.
 *
 * No zlib/miniz/libpng dependency: DEFLATE's "fixed Huffman" block type (RFC 1951 S3.2.6)
 * has codes hardcoded by the spec itself, so no runtime Huffman-tree construction is needed
 * -- only a from-scratch LZ77 match finder (single-entry hash table, greedy parsing, 32KB
 * window) and a bit-level packer. This is real, general-purpose LZ77 (not a special-cased
 * run-length trick), so for this project's content -- large flat/black regions around a
 * sparse point cloud -- it compresses well. It forgoes dynamic Huffman (the other half of
 * what a full zlib -9 does), so files are larger than `zlib.compress(..., 9)` would produce
 * for the same pixels, but that's an acceptable trade for staying dependency-free.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace png_writer
{
    /// Safe UPPER BOUND on encodeRgb565()'s output size for a width x height image -- size
    /// `out` to at least this. The actual encoded size (returned by encodeRgb565()) is
    /// usually much smaller once compression kicks in; this bound only has to hold for the
    /// worst case (incompressible input), where fixed Huffman can expand data slightly.
    size_t requiredBufferSize(int width, int height);

    /**
     * @brief Encode a width x height RGB565 pixel buffer (row-major, no row padding, same
     *        layout Display::readAllPixels() fills) into `out` as an 8-bit RGB PNG.
     * @return Bytes written, or 0 on failure (pixels/out null, width/height <= 0,
     *         outCapacity < requiredBufferSize(), or an internal allocation failed).
     */
    size_t encodeRgb565(const uint16_t *pixels, int width, int height, uint8_t *out, size_t outCapacity);
} // namespace png_writer
