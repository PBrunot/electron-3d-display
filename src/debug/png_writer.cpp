#include "debug/png_writer.h"

#include <cstring>

#include "util/crc32.h"
#include "render/display.h"
#include "esp_heap_caps.h"

namespace png_writer
{
namespace
{
    constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    size_t uncompressedLen(int width, int height)
    {
        return size_t(height) * (1 + 3 * size_t(width));
    }

    /// Worst-case fixed-Huffman-only-literals expansion is 9/8 (see header comment); +64
    /// covers the 3-bit block header, the 7-bit end-of-block symbol, and integer rounding.
    size_t maxDeflateLen(size_t uncompLen)
    {
        return uncompLen + uncompLen / 8 + 64;
    }

    size_t maxZlibLen(size_t uncompLen)
    {
        return 2 + maxDeflateLen(uncompLen) + 4;
    }

    /**
     * @brief LSB-first bit packer into a caller-owned buffer, with the MSB-first Huffman-code
     *        exception DEFLATE requires (RFC 1951 S3.1.1: "Huffman codes are packed starting
     *        with the most-significant bit of the code" -- everything else in DEFLATE packs
     *        least-significant-bit first). Bounds-checked: writes past `capacity` are dropped
     *        and flagged via overflowed() rather than corrupting memory, as a defensive
     *        backstop behind requiredBufferSize()'s upper-bound math.
     */
    class BitWriter
    {
    public:
        BitWriter(uint8_t *buf, size_t capacity) : buf(buf), capacity(capacity) {}

        /// LSB-first: bit 0 of `value` is written first. Used for the block header and the
        /// "extra bits" that follow a length/distance Huffman code.
        void putBitsLsb(uint32_t value, int nbits)
        {
            for (int i = 0; i < nbits; i++)
                putBit((value >> i) & 1u);
        }

        /// MSB-first: bit (nbits-1) of `code` is written first. Used only for Huffman codes
        /// themselves (literal/length and distance symbols).
        void putHuffman(uint32_t code, int nbits)
        {
            for (int i = nbits - 1; i >= 0; i--)
                putBit((code >> i) & 1u);
        }

        /// Pads the final partial byte with zero bits (DEFLATE requires this at the end of
        /// the bitstream) and returns the total bytes written, including the padded one.
        size_t finish()
        {
            if (bitPos != 0)
            {
                bytePos++;
                bitPos = 0;
            }
            return bytePos;
        }

        bool overflowed() const { return failed; }

    private:
        void putBit(uint32_t bit)
        {
            if (bitPos == 0)
            {
                if (bytePos >= capacity)
                {
                    failed = true;
                    return;
                }
                buf[bytePos] = 0;
            }
            if (bit)
                buf[bytePos] = uint8_t(buf[bytePos] | (1u << bitPos));
            bitPos++;
            if (bitPos == 8)
            {
                bitPos = 0;
                bytePos++;
            }
        }

        uint8_t *buf;
        size_t capacity;
        size_t bytePos = 0;
        int bitPos = 0;
        bool failed = false;
    };

    /// RFC 1951 S3.2.6's fixed literal/length Huffman codes -- hardcoded by the spec, no
    /// runtime tree construction needed. `sym` is 0-255 for literal bytes, 256 for
    /// end-of-block, 257-285 for length codes.
    void fixedLitLenCode(int sym, uint32_t &code, int &bits)
    {
        if (sym <= 143)
        {
            code = 0x30u + uint32_t(sym);
            bits = 8;
        }
        else if (sym <= 255)
        {
            code = 0x190u + uint32_t(sym - 144);
            bits = 9;
        }
        else if (sym <= 279)
        {
            code = uint32_t(sym - 256);
            bits = 7;
        }
        else
        {
            code = 0xC0u + uint32_t(sym - 280);
            bits = 8;
        }
    }

    struct LenEntry
    {
        uint16_t minLen;
        uint16_t sym;
        uint8_t extraBits;
    };
    // RFC 1951 S3.2.5.
    constexpr LenEntry kLenTable[29] = {
        {3, 257, 0}, {4, 258, 0}, {5, 259, 0}, {6, 260, 0}, {7, 261, 0}, {8, 262, 0}, {9, 263, 0}, {10, 264, 0},
        {11, 265, 1}, {13, 266, 1}, {15, 267, 1}, {17, 268, 1},
        {19, 269, 2}, {23, 270, 2}, {27, 271, 2}, {31, 272, 2},
        {35, 273, 3}, {43, 274, 3}, {51, 275, 3}, {59, 276, 3},
        {67, 277, 4}, {83, 278, 4}, {99, 279, 4}, {115, 280, 4},
        {131, 281, 5}, {163, 282, 5}, {195, 283, 5}, {227, 284, 5},
        {258, 285, 0},
    };

    struct DistEntry
    {
        uint32_t minDist;
        uint16_t sym;
        uint8_t extraBits;
    };
    // RFC 1951 S3.2.5.
    constexpr DistEntry kDistTable[30] = {
        {1, 0, 0}, {2, 1, 0}, {3, 2, 0}, {4, 3, 0},
        {5, 4, 1}, {7, 5, 1},
        {9, 6, 2}, {13, 7, 2},
        {17, 8, 3}, {25, 9, 3},
        {33, 10, 4}, {49, 11, 4},
        {65, 12, 5}, {97, 13, 5},
        {129, 14, 6}, {193, 15, 6},
        {257, 16, 7}, {385, 17, 7},
        {513, 18, 8}, {769, 19, 8},
        {1025, 20, 9}, {1537, 21, 9},
        {2049, 22, 10}, {3073, 23, 10},
        {4097, 24, 11}, {6145, 25, 11},
        {8193, 26, 12}, {12289, 27, 12},
        {16385, 28, 13}, {24577, 29, 13},
    };

    void emitLength(BitWriter &bw, int length)
    {
        int idx = 0;
        for (int i = 0; i < 29; i++)
            if (kLenTable[i].minLen <= length)
                idx = i;
        uint32_t code;
        int bits;
        fixedLitLenCode(kLenTable[idx].sym, code, bits);
        bw.putHuffman(code, bits);
        if (kLenTable[idx].extraBits > 0)
            bw.putBitsLsb(uint32_t(length - kLenTable[idx].minLen), kLenTable[idx].extraBits);
    }

    void emitDistance(BitWriter &bw, uint32_t dist)
    {
        int idx = 0;
        for (int i = 0; i < 30; i++)
            if (kDistTable[i].minDist <= dist)
                idx = i;
        // Fixed Huffman assigns distance symbols plain 5-bit codes (the symbol value itself).
        bw.putHuffman(kDistTable[idx].sym, 5);
        if (kDistTable[idx].extraBits > 0)
            bw.putBitsLsb(dist - kDistTable[idx].minDist, kDistTable[idx].extraBits);
    }

    constexpr int kMinMatch = 3;
    constexpr int kMaxMatch = 258;
    constexpr size_t kWindowSize = 32768;
    constexpr int kHashBits = 13;
    constexpr size_t kHashSize = size_t(1) << kHashBits;

    uint32_t hash3(const uint8_t *p)
    {
        uint32_t v = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
        return (v * 2654435761u) >> (32 - kHashBits);
    }

    /**
     * @brief Greedy LZ77 + fixed-Huffman DEFLATE of `data` into a single BFINAL block,
     *        written into bw. Match finder is a single-entry-per-bucket hash table over 3-
     *        byte prefixes (32KB window, matching DEFLATE's own distance limit) -- collisions
     *        only cost a missed match, never correctness, since every candidate is verified
     *        byte-by-byte before use. Hash entries are only inserted at the START of each
     *        literal/match (not at every byte inside a match) -- a standard speed/ratio
     *        tradeoff that stays fully spec-valid; it just occasionally misses a match that
     *        would have started mid-way through a previous one.
     */
    void deflateFixedHuffman(const uint8_t *data, size_t len, BitWriter &bw)
    {
        // BFINAL=1 (only block), BTYPE=01 (fixed Huffman) -- packed LSB-first like all
        // non-Huffman-code fields (RFC 1951 S3.2.3).
        bw.putBitsLsb(1, 1);
        bw.putBitsLsb(1, 2);

        int32_t *head = (int32_t *)heap_caps_malloc(kHashSize * sizeof(int32_t), MALLOC_CAP_SPIRAM);
        if (head == nullptr)
        {
            // Falls back to literal-only encoding (still correct, just uncompressed-ish)
            // rather than failing the whole capture over a transient allocation failure.
            for (size_t i = 0; i < len; i++)
            {
                uint32_t code;
                int bits;
                fixedLitLenCode(data[i], code, bits);
                bw.putHuffman(code, bits);
            }
        }
        else
        {
            std::memset(head, 0xFF, kHashSize * sizeof(int32_t)); // -1 = empty
            size_t i = 0;
            while (i < len)
            {
                int bestLen = 0;
                size_t bestDist = 0;
                if (i + kMinMatch <= len)
                {
                    uint32_t h = hash3(&data[i]);
                    int32_t cand = head[h];
                    if (cand >= 0 && (i - size_t(cand)) <= kWindowSize)
                    {
                        size_t maxLen = len - i;
                        if (maxLen > kMaxMatch)
                            maxLen = kMaxMatch;
                        size_t matchLen = 0;
                        const uint8_t *a = data + cand;
                        const uint8_t *b = data + i;
                        // Reads straight from the static original buffer, so this also
                        // correctly detects/verifies self-overlapping matches (dist < len,
                        // e.g. a run of one repeated byte) -- valid per RFC 1951 S3.2.5 ("the
                        // referenced string may overlap the current position").
                        while (matchLen < maxLen && a[matchLen] == b[matchLen])
                            matchLen++;
                        if (matchLen >= kMinMatch)
                        {
                            bestLen = int(matchLen);
                            bestDist = i - size_t(cand);
                        }
                    }
                    head[h] = int32_t(i);
                }

                if (bestLen >= kMinMatch)
                {
                    emitLength(bw, bestLen);
                    emitDistance(bw, uint32_t(bestDist));
                    i += size_t(bestLen);
                }
                else
                {
                    uint32_t code;
                    int bits;
                    fixedLitLenCode(data[i], code, bits);
                    bw.putHuffman(code, bits);
                    i += 1;
                }
            }
            heap_caps_free(head);
        }

        uint32_t endCode;
        int endBits;
        fixedLitLenCode(256, endCode, endBits);
        bw.putHuffman(endCode, endBits);
    }

    void writeBe32(uint8_t *p, uint32_t v)
    {
        p[0] = uint8_t(v >> 24);
        p[1] = uint8_t(v >> 16);
        p[2] = uint8_t(v >> 8);
        p[3] = uint8_t(v);
    }

} // namespace

size_t requiredBufferSize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return 0;
    size_t idatDataLen = maxZlibLen(uncompressedLen(width, height));
    size_t idatChunkLen = 4 + 4 + idatDataLen + 4;
    size_t ihdrChunkLen = 4 + 4 + 13 + 4;
    size_t iendChunkLen = 4 + 4 + 0 + 4;
    return sizeof(kSignature) + ihdrChunkLen + idatChunkLen + iendChunkLen;
}

size_t encodeRgb565(const uint16_t *pixels, int width, int height, uint8_t *out, size_t outCapacity)
{
    if (pixels == nullptr || out == nullptr || width <= 0 || height <= 0)
        return 0;
    size_t need = requiredBufferSize(width, height);
    if (need == 0 || outCapacity < need)
        return 0;

    size_t uncompLen = uncompressedLen(width, height);
    uint8_t *uncomp = (uint8_t *)heap_caps_malloc(uncompLen, MALLOC_CAP_SPIRAM);
    if (uncomp == nullptr)
        return 0;

    uint8_t *w = uncomp;
    for (int y = 0; y < height; y++)
    {
        *w++ = 0; // filter type: None
        const uint16_t *row = pixels + size_t(y) * size_t(width);
        for (int x = 0; x < width; x++)
        {
            uint8_t r, g, b;
            Display::unpackColor565(row[x], &r, &g, &b);
            *w++ = r;
            *w++ = g;
            *w++ = b;
        }
    }

    // Adler32 (zlib's uncompressed-data checksum), plain single-pass accumulator.
    uint32_t a = 1, b32 = 0;
    for (size_t i = 0; i < uncompLen; i++)
    {
        a = (a + uncomp[i]) % 65521u;
        b32 = (b32 + a) % 65521u;
    }
    uint32_t adler = (b32 << 16) | a;

    uint8_t *p = out;
    std::memcpy(p, kSignature, sizeof(kSignature));
    p += sizeof(kSignature);

    // IHDR
    {
        uint8_t data[13];
        writeBe32(data, uint32_t(width));
        writeBe32(data + 4, uint32_t(height));
        data[8] = 8;  // bit depth
        data[9] = 2;  // color type: RGB (truecolor, no alpha)
        data[10] = 0; // compression method (always 0 = deflate)
        data[11] = 0; // filter method (always 0)
        data[12] = 0; // interlace method: none
        writeBe32(p, 13);
        p += 4;
        uint8_t *typeStart = p;
        std::memcpy(p, "IHDR", 4);
        p += 4;
        std::memcpy(p, data, 13);
        p += 13;
        uint32_t crc = crc32(typeStart, 4 + 13);
        writeBe32(p, crc);
        p += 4;
    }

    // IDAT: chunk length is patched in below, once the compressed size is known -- it can't
    // be written up front like IHDR/IEND's fixed-size data.
    uint8_t *lenField = p;
    p += 4;
    uint8_t *typeStart = p;
    std::memcpy(p, "IDAT", 4);
    p += 4;
    // zlib header: CM=8 (deflate), CINFO=7 (32K window); FLG's low 5 bits (FCHECK) make
    // (CMF*256+FLG) a multiple of 31, as the zlib format (RFC 1950) requires.
    *p++ = 0x78;
    *p++ = 0x01;

    size_t remaining = outCapacity - size_t(p - out) - 4 /* adler32 */ - 12 /* IEND */;
    BitWriter bw(p, remaining);
    deflateFixedHuffman(uncomp, uncompLen, bw);
    size_t deflateLen = bw.finish();
    heap_caps_free(uncomp);
    if (bw.overflowed())
        return 0;
    p += deflateLen;

    writeBe32(p, adler);
    p += 4;

    size_t idatDataLen = size_t(p - typeStart);
    writeBe32(lenField, uint32_t(idatDataLen - 4)); // chunk length excludes the 4-byte type
    uint32_t crc = crc32(typeStart, idatDataLen);
    writeBe32(p, crc);
    p += 4;

    // IEND
    {
        writeBe32(p, 0);
        p += 4;
        uint8_t *typeStart2 = p;
        std::memcpy(p, "IEND", 4);
        p += 4;
        uint32_t crc2 = crc32(typeStart2, 4);
        writeBe32(p, crc2);
        p += 4;
    }

    return size_t(p - out);
}

} // namespace png_writer
