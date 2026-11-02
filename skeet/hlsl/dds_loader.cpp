#include "dds_loader.h"
#include <cstdio>
#include <cstring>

namespace dds {
namespace {

constexpr unsigned kMaxDim = 2048;
constexpr unsigned kMaxPixels = kMaxDim * kMaxDim;

struct Header {
    DWORD magic, size, flags, height, width, pitch, depth, mips;
    DWORD reserved[11];
    DWORD pfSize, pfFlags, fourCC, rgbBits, rMask, gMask, bMask, aMask;
    DWORD caps[4];
};

constexpr DWORD kMagic = 0x20534444; // "DDS "
constexpr DWORD kFourCC = 0x4;
constexpr DWORD kRgb = 0x40;
constexpr DWORD kPitch = 0x8;
constexpr DWORD kDxt1 = 0x31545844; // "DXT1"
constexpr DWORD kDxt5 = 0x35545844; // "DXT5"

inline unsigned rip(unsigned &a, unsigned &b) {
    return a | (b << 8);
}

void decode_dxt_block(const unsigned char *s, unsigned char *d, bool dxt5) {
    if (dxt5) {
        unsigned a0 = s[0], a1 = s[1];
        unsigned long long bits = 0;
        for (int i = 0; i < 6; ++i)
            bits |= (unsigned long long)s[2 + i] << (8 * i);
        unsigned pal[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
        if (a0 > a1) {
            for (int i = 1; i <= 6; ++i)
                pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        } else {
            for (int i = 1; i <= 4; ++i)
                pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
            pal[6] = 0;
            pal[7] = 255;
        }
        for (int i = 0; i < 16; ++i)
            d[i * 4 + 3] = (unsigned char)pal[(bits >> (3 * i)) & 7];
        s += 8;
    } else {
        for (int i = 0; i < 16; ++i)
            d[i * 4 + 3] = 255;
    }
    unsigned c0 = *(const unsigned short *)s, c1 = *(const unsigned short *)(s + 2);
    unsigned r[4], g[4], b[4];
    for (int i = 0; i < 2; ++i) {
        unsigned c = i ? c1 : c0;
        unsigned x = ((c >> 11) & 0x1f) << 3, y = ((c >> 5) & 0x3f) << 2, z = (c & 0x1f) << 3;
        r[i] = x | (x >> 5);
        g[i] = y | (y >> 6);
        b[i] = z | (z >> 5);
    }
    if (c0 > c1 || dxt5) {
        r[2] = (2 * r[0] + r[1]) / 3;
        g[2] = (2 * g[0] + g[1]) / 3;
        b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3;
        g[3] = (g[0] + 2 * g[1]) / 3;
        b[3] = (b[0] + 2 * b[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2;
        g[2] = (g[0] + g[1]) / 2;
        b[2] = (b[0] + b[1]) / 2;
        r[3] = g[3] = b[3] = 0;
    }
    DWORD idx = *(const DWORD *)(s + 4);
    for (int i = 0; i < 16; ++i) {
        unsigned k = (idx >> (2 * i)) & 3;
        d[i * 4 + 0] = (unsigned char)b[k];
        d[i * 4 + 1] = (unsigned char)g[k];
        d[i * 4 + 2] = (unsigned char)r[k];
        if (!dxt5 && k == 3)
            d[i * 4 + 3] = (c0 > c1) ? 255 : 0;
    }
}

} // namespace

bool decode(const wchar_t *path, Image &out, char *why, unsigned cap) {
    auto fail = [&](const char *t) {
        if (why && cap)
            strncpy_s(why, cap, t, _TRUNCATE);
        return false;
    };
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return fail("file not readable");
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    if (size.QuadPart < 128 || size.QuadPart > 32 * 1024 * 1024) {
        CloseHandle(f);
        return fail("bad file size");
    }
    std::vector<unsigned char> data(size_t(size.QuadPart));
    DWORD got = 0;
    bool ok = ReadFile(f, data.data(), DWORD(data.size()), &got, nullptr) && got == data.size();
    CloseHandle(f);
    if (!ok)
        return fail("read failed");

    Header h{};
    memcpy(&h, data.data(), sizeof(h));
    if (h.magic != kMagic || h.size != 124 || h.pfSize != 32)
        return fail("not a DDS file");
    if (h.depth > 1 || (h.caps[1] & (0x200u | 0x200000u)))
        return fail("DDS loader accepts 2D images; create cube/volume textures from raw bytes");
    if (!h.width || !h.height || h.width > kMaxDim || h.height > kMaxDim)
        return fail("bad dimensions (max 2048)");

    bool compressed = (h.pfFlags & kFourCC) && (h.fourCC == kDxt1 || h.fourCC == kDxt5);
    bool raw = (h.pfFlags & kRgb) && !(h.pfFlags & kFourCC) && h.rgbBits == 32;
    if (!compressed && !raw)
        return fail("unsupported format (need 32-bit or DXT1/DXT5)");

    out.width = h.width;
    out.height = h.height;
    out.pixels.assign(size_t(h.width) * h.height, 0xff000000u);
    const unsigned char *src = data.data() + 128;

    if (raw) {
        const size_t row = (h.flags & kPitch) ? h.pitch : size_t(h.width) * 4;
        if (row < size_t(h.width) * 4 || row > 32 * 1024 * 1024 || row > (data.size() - 128) / h.height)
            return fail("invalid/truncated row pitch");
        if (!h.rMask || !h.gMask || !h.bMask || (h.rMask & h.gMask) || (h.rMask & h.bMask) ||
            (h.gMask & h.bMask) || (h.aMask & (h.rMask | h.gMask | h.bMask)))
            return fail("invalid channel masks");
        auto channel = [](unsigned value, unsigned mask, unsigned fallback) {
            if (!mask)
                return fallback;
            unsigned shift = 0;
            while (!(mask & 1)) {
                mask >>= 1;
                ++shift;
            }
            return unsigned((uint64_t((value >> shift) & mask) * 255 + mask / 2) / mask);
        };
        auto contiguous = [](unsigned mask) {
            if (!mask)
                return true;
            while (!(mask & 1))
                mask >>= 1;
            return (mask & (mask + 1)) == 0;
        };
        if (!contiguous(h.rMask) || !contiguous(h.gMask) || !contiguous(h.bMask) || !contiguous(h.aMask))
            return fail("noncontiguous masks");
        for (unsigned y = 0; y < h.height; ++y)
            for (unsigned x = 0; x < h.width; ++x) {
                unsigned pixel = 0;
                memcpy(&pixel, src + y * row + x * 4, 4);
                out.pixels[size_t(y) * h.width + x] =
                    (channel(pixel, h.aMask, 255) << 24) | (channel(pixel, h.rMask, 0) << 16) |
                    (channel(pixel, h.gMask, 0) << 8) | channel(pixel, h.bMask, 0);
            }
        return true;
    }

    const unsigned block = (h.fourCC == kDxt5) ? 16 : 8;
    const unsigned bw = (h.width + 3) / 4, bh = (h.height + 3) / 4;
    if (data.size() < 128 + size_t(bw) * bh * block)
        return fail("truncated block data");
    std::vector<unsigned char> tile(64);
    for (unsigned by = 0; by < bh; ++by) {
        for (unsigned bx = 0; bx < bw; ++bx) {
            decode_dxt_block(src, tile.data(), h.fourCC == kDxt5);
            src += block;
            for (unsigned y = 0; y < 4; ++y) {
                unsigned ty = by * 4 + y;
                if (ty >= h.height)
                    break;
                for (unsigned x = 0; x < 4; ++x) {
                    unsigned tx = bx * 4 + x;
                    if (tx >= h.width)
                        continue;
                    const unsigned char *p = tile.data() + (y * 4 + x) * 4;
                    out.pixels[size_t(ty) * h.width + tx] =
                        (unsigned(p[3]) << 24) | (unsigned(p[2]) << 16) | (unsigned(p[1]) << 8) | p[0];
                }
            }
        }
    }
    return true;
}

} // namespace dds
