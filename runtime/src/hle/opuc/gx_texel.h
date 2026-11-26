// GX texture texel formats for the PC PlayGuide pictures (hle/opuc/pc_pictures.cpp): decode the game's
// big-endian tiled texels to straight RGBA, and encode RGBA back as GX RGBA8 (format 6). Formats as in the GX
// hardware docs: I4 0, I8 1, IA4 2, IA8 3, RGB565 4, RGB5A3 5, RGBA8 6, CMPR 14.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace OpucGx {

inline uint8_t X4(uint32_t v) { return static_cast<uint8_t>((v << 4) | v); }
inline uint8_t X5(uint32_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
inline uint8_t X6(uint32_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }
inline uint8_t X3(uint32_t v) { return static_cast<uint8_t>((v << 5) | (v << 2) | (v >> 1)); }

inline void Rgb565(uint16_t c, uint8_t* o) { o[0] = X5(c >> 11); o[1] = X6((c >> 5) & 63); o[2] = X5(c & 31); o[3] = 255; }

inline bool Supported(uint32_t fmt) { return fmt <= 6 || fmt == 14; }

// Bytes of a w x h texture in `fmt` (tiles padded).
inline size_t ByteSize(uint32_t fmt, int w, int h) {
    int bw = 8, bh = 8, bytes = 32;
    switch (fmt) {
    case 0: bw = 8; bh = 8; break;
    case 1: case 2: bw = 8; bh = 4; break;
    case 3: case 4: case 5: bw = 4; bh = 4; break;
    case 6: bw = 4; bh = 4; bytes = 64; break;
    case 14: bw = 8; bh = 8; break;
    default: return 0;
    }
    return static_cast<size_t>((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

// Decodes `src` (big-endian GX texels) to straight RGBA, w * h * 4 bytes.
inline std::vector<uint8_t> Decode(uint32_t fmt, int w, int h, const uint8_t* src) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    auto put = [&](int x, int y, const uint8_t* p) { if (x < w && y < h) std::memcpy(&out[(static_cast<size_t>(y) * w + x) * 4], p, 4); };
    const uint8_t* s = src;
    auto be16 = [](const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); };
    switch (fmt) {
    case 0:  // I4
        for (int ty = 0; ty < h; ty += 8) for (int tx = 0; tx < w; tx += 8)
            for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; x += 2, ++s) {
                const uint8_t a = X4(*s >> 4), b = X4(*s & 15), pa[4] = {a, a, a, a}, pb[4] = {b, b, b, b};
                put(tx + x, ty + y, pa); put(tx + x + 1, ty + y, pb);
            }
        break;
    case 1:  // I8
        for (int ty = 0; ty < h; ty += 4) for (int tx = 0; tx < w; tx += 8)
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x, ++s) { const uint8_t p[4] = {*s, *s, *s, *s}; put(tx + x, ty + y, p); }
        break;
    case 2:  // IA4
        for (int ty = 0; ty < h; ty += 4) for (int tx = 0; tx < w; tx += 8)
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x, ++s) {
                const uint8_t i = X4(*s & 15), p[4] = {i, i, i, X4(*s >> 4)};
                put(tx + x, ty + y, p);
            }
        break;
    case 3:  // IA8
        for (int ty = 0; ty < h; ty += 4) for (int tx = 0; tx < w; tx += 4)
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x, s += 2) { const uint8_t p[4] = {s[1], s[1], s[1], s[0]}; put(tx + x, ty + y, p); }
        break;
    case 4:  // RGB565
        for (int ty = 0; ty < h; ty += 4) for (int tx = 0; tx < w; tx += 4)
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x, s += 2) { uint8_t p[4]; Rgb565(be16(s), p); put(tx + x, ty + y, p); }
        break;
    case 5:  // RGB5A3
        for (int ty = 0; ty < h; ty += 4) for (int tx = 0; tx < w; tx += 4)
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x, s += 2) {
                const uint16_t c = be16(s);
                uint8_t p[4];
                if (c & 0x8000) { p[0] = X5((c >> 10) & 31); p[1] = X5((c >> 5) & 31); p[2] = X5(c & 31); p[3] = 255; }
                else { p[3] = X3((c >> 12) & 7); p[0] = X4((c >> 8) & 15); p[1] = X4((c >> 4) & 15); p[2] = X4(c & 15); }
                put(tx + x, ty + y, p);
            }
        break;
    case 6:  // RGBA8: per 4 x 4 tile 32 bytes of AR, then 32 of GB
        for (int ty = 0; ty < h; ty += 4) for (int tx = 0; tx < w; tx += 4, s += 64)
            for (int i = 0; i < 16; ++i) {
                const uint8_t p[4] = {s[i * 2 + 1], s[32 + i * 2], s[32 + i * 2 + 1], s[i * 2]};
                put(tx + (i & 3), ty + (i >> 2), p);
            }
        break;
    case 14:  // CMPR: 8 x 8 tiles of four 4 x 4 DXT1 blocks
        for (int ty = 0; ty < h; ty += 8) for (int tx = 0; tx < w; tx += 8)
            for (int b = 0; b < 4; ++b, s += 8) {
                const uint16_t c0 = be16(s), c1 = be16(s + 2);
                uint8_t pal[4][4];
                Rgb565(c0, pal[0]); Rgb565(c1, pal[1]);
                if (c0 > c1) {
                    for (int k = 0; k < 3; ++k) { pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3; pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3; }
                    pal[2][3] = pal[3][3] = 255;
                } else {
                    for (int k = 0; k < 3; ++k) { pal[2][k] = (pal[0][k] + pal[1][k]) / 2; pal[3][k] = 0; }
                    pal[2][3] = 255; pal[3][3] = 0;
                }
                const int bx = tx + (b & 1) * 4, by = ty + (b >> 1) * 4;
                for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) put(bx + x, by + y, pal[(s[4 + y] >> (6 - 2 * x)) & 3]);
            }
        break;
    default: break;
    }
    return out;
}

// Encodes straight RGBA (w * h * 4) as GX RGBA8, tiles padded with transparent texels.
inline std::vector<uint8_t> EncodeRgba8(int w, int h, const uint8_t* rgba) {
    std::vector<uint8_t> out(ByteSize(6, w, h), 0);
    uint8_t* d = out.data();
    for (int ty = 0; ty < h; ty += 4) for (int tx = 0; tx < w; tx += 4, d += 64)
        for (int i = 0; i < 16; ++i) {
            const int x = tx + (i & 3), y = ty + (i >> 2);
            uint8_t p[4] = {0, 0, 0, 0};
            if (x < w && y < h) std::memcpy(p, &rgba[(static_cast<size_t>(y) * w + x) * 4], 4);
            d[i * 2] = p[3]; d[i * 2 + 1] = p[0]; d[32 + i * 2] = p[1]; d[32 + i * 2 + 1] = p[2];
        }
    return out;
}

}  // namespace OpucGx
