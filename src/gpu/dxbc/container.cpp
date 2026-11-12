// DXBC container writer and checksum.
#include <cstring>

#include "gpu/dxbc/dxbc.h"

namespace gpu::dxbc {

namespace {

// Plain MD5 block transform (RFC 1321).
void Md5Transform(u32 buf[4], const u32 in[16]) {
    auto rol = [](u32 x, int c) { return (x << c) | (x >> (32 - c)); };
    u32 a = buf[0], b = buf[1], c = buf[2], d = buf[3];
#define STEP(f, w, x, y, z, data, s) w += f(x, y, z) + (data), w = rol(w, s) + x
#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))
    STEP(F1, a, b, c, d, in[0] + 0xd76aa478, 7);
    STEP(F1, d, a, b, c, in[1] + 0xe8c7b756, 12);
    STEP(F1, c, d, a, b, in[2] + 0x242070db, 17);
    STEP(F1, b, c, d, a, in[3] + 0xc1bdceee, 22);
    STEP(F1, a, b, c, d, in[4] + 0xf57c0faf, 7);
    STEP(F1, d, a, b, c, in[5] + 0x4787c62a, 12);
    STEP(F1, c, d, a, b, in[6] + 0xa8304613, 17);
    STEP(F1, b, c, d, a, in[7] + 0xfd469501, 22);
    STEP(F1, a, b, c, d, in[8] + 0x698098d8, 7);
    STEP(F1, d, a, b, c, in[9] + 0x8b44f7af, 12);
    STEP(F1, c, d, a, b, in[10] + 0xffff5bb1, 17);
    STEP(F1, b, c, d, a, in[11] + 0x895cd7be, 22);
    STEP(F1, a, b, c, d, in[12] + 0x6b901122, 7);
    STEP(F1, d, a, b, c, in[13] + 0xfd987193, 12);
    STEP(F1, c, d, a, b, in[14] + 0xa679438e, 17);
    STEP(F1, b, c, d, a, in[15] + 0x49b40821, 22);
    STEP(F2, a, b, c, d, in[1] + 0xf61e2562, 5);
    STEP(F2, d, a, b, c, in[6] + 0xc040b340, 9);
    STEP(F2, c, d, a, b, in[11] + 0x265e5a51, 14);
    STEP(F2, b, c, d, a, in[0] + 0xe9b6c7aa, 20);
    STEP(F2, a, b, c, d, in[5] + 0xd62f105d, 5);
    STEP(F2, d, a, b, c, in[10] + 0x02441453, 9);
    STEP(F2, c, d, a, b, in[15] + 0xd8a1e681, 14);
    STEP(F2, b, c, d, a, in[4] + 0xe7d3fbc8, 20);
    STEP(F2, a, b, c, d, in[9] + 0x21e1cde6, 5);
    STEP(F2, d, a, b, c, in[14] + 0xc33707d6, 9);
    STEP(F2, c, d, a, b, in[3] + 0xf4d50d87, 14);
    STEP(F2, b, c, d, a, in[8] + 0x455a14ed, 20);
    STEP(F2, a, b, c, d, in[13] + 0xa9e3e905, 5);
    STEP(F2, d, a, b, c, in[2] + 0xfcefa3f8, 9);
    STEP(F2, c, d, a, b, in[7] + 0x676f02d9, 14);
    STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8a, 20);
    STEP(F3, a, b, c, d, in[5] + 0xfffa3942, 4);
    STEP(F3, d, a, b, c, in[8] + 0x8771f681, 11);
    STEP(F3, c, d, a, b, in[11] + 0x6d9d6122, 16);
    STEP(F3, b, c, d, a, in[14] + 0xfde5380c, 23);
    STEP(F3, a, b, c, d, in[1] + 0xa4beea44, 4);
    STEP(F3, d, a, b, c, in[4] + 0x4bdecfa9, 11);
    STEP(F3, c, d, a, b, in[7] + 0xf6bb4b60, 16);
    STEP(F3, b, c, d, a, in[10] + 0xbebfbc70, 23);
    STEP(F3, a, b, c, d, in[13] + 0x289b7ec6, 4);
    STEP(F3, d, a, b, c, in[0] + 0xeaa127fa, 11);
    STEP(F3, c, d, a, b, in[3] + 0xd4ef3085, 16);
    STEP(F3, b, c, d, a, in[6] + 0x04881d05, 23);
    STEP(F3, a, b, c, d, in[9] + 0xd9d4d039, 4);
    STEP(F3, d, a, b, c, in[12] + 0xe6db99e5, 11);
    STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8, 16);
    STEP(F3, b, c, d, a, in[2] + 0xc4ac5665, 23);
    STEP(F4, a, b, c, d, in[0] + 0xf4292244, 6);
    STEP(F4, d, a, b, c, in[7] + 0x432aff97, 10);
    STEP(F4, c, d, a, b, in[14] + 0xab9423a7, 15);
    STEP(F4, b, c, d, a, in[5] + 0xfc93a039, 21);
    STEP(F4, a, b, c, d, in[12] + 0x655b59c3, 6);
    STEP(F4, d, a, b, c, in[3] + 0x8f0ccc92, 10);
    STEP(F4, c, d, a, b, in[10] + 0xffeff47d, 15);
    STEP(F4, b, c, d, a, in[1] + 0x85845dd1, 21);
    STEP(F4, a, b, c, d, in[8] + 0x6fa87e4f, 6);
    STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0, 10);
    STEP(F4, c, d, a, b, in[6] + 0xa3014314, 15);
    STEP(F4, b, c, d, a, in[13] + 0x4e0811a1, 21);
    STEP(F4, a, b, c, d, in[4] + 0xf7537e82, 6);
    STEP(F4, d, a, b, c, in[11] + 0xbd3af235, 10);
    STEP(F4, c, d, a, b, in[2] + 0x2ad7d2bb, 15);
    STEP(F4, b, c, d, a, in[9] + 0xeb86d391, 21);
#undef STEP
#undef F1
#undef F2
#undef F3
#undef F4
    buf[0] += a;
    buf[1] += b;
    buf[2] += c;
    buf[3] += d;
}

} // namespace

void Checksum(const u8* blob, size_t size, u32 out[4]) {
    // Same as MD5 except for the final block: the bit count goes in the first word and (bits >> 2) | 1 in the
    // last, instead of a 64-bit length at the end.
    const u8* data = blob + 20;
    const u32 len = u32(size - 20);
    u32 buf[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    u32 block[16];
    u32 off = 0;
    for (; off + 64 <= len; off += 64) {
        std::memcpy(block, data + off, 64);
        Md5Transform(buf, block);
    }
    const u32 left = len - off;
    const u32 bits = len * 8;
    u8 tail[64]{};
    if (left >= 56) {
        std::memcpy(tail, data + off, left);
        tail[left] = 0x80;
        std::memcpy(block, tail, 64);
        Md5Transform(buf, block);
        std::memset(block, 0, sizeof(block));
    } else {
        std::memcpy(tail + 4, data + off, left);
        tail[4 + left] = 0x80;
        std::memcpy(block, tail, 64);
    }
    block[0] = bits;
    block[15] = (bits >> 2) | 1;
    Md5Transform(buf, block);
    std::memcpy(out, buf, 16);
}

// ISGN / OSGN: count, offset of the element table (8), 24-byte elements, then the names (each once), padded to 4.
static std::vector<u8> Signature(const std::vector<SigElement>& els) {
    std::vector<u8> out(8 + els.size() * 24, 0);
    const u32 head[2] = {u32(els.size()), 8};
    std::memcpy(out.data(), head, 8);
    std::vector<std::pair<std::string, u32>> names;
    for (size_t i = 0; i < els.size(); i++) {
        const SigElement& e = els[i];
        u32 name_off = 0;
        for (auto& [n, o] : names)
            if (n == e.name)
                name_off = o;
        if (!name_off) {
            name_off = u32(out.size());
            names.push_back({e.name, name_off});
            out.insert(out.end(), e.name.begin(), e.name.end());
            out.push_back(0);
        }
        const u32 w[5] = {name_off, e.index, e.sys_value, e.comp_type, e.reg};
        u8* p = out.data() + 8 + i * 24;
        std::memcpy(p, w, 20);
        p[20] = e.mask;
        p[21] = e.rw_mask;
    }
    while (out.size() % 4)
        out.push_back(0xAB); // (FXC pads names with 0xAB)
    return out;
}

std::vector<u8> Container(const std::vector<u32>& shex, const std::vector<SigElement>& isgn,
                          const std::vector<SigElement>& osgn) {
    struct Chunk {
        char tag[4];
        std::vector<u8> data;
    };
    std::vector<Chunk> chunks;
    chunks.push_back({{'I', 'S', 'G', 'N'}, Signature(isgn)});
    chunks.push_back({{'O', 'S', 'G', 'N'}, Signature(osgn)});
    chunks.push_back({{'S', 'H', 'E', 'X'}, std::vector<u8>((const u8*)shex.data(), (const u8*)shex.data() + shex.size() * 4)});
    const u32 header = 32 + 4 * u32(chunks.size());
    u32 total = header;
    for (auto& c : chunks)
        total += 8 + u32(c.data.size());
    std::vector<u8> out(total, 0);
    std::memcpy(&out[0], "DXBC", 4);
    const u32 one = 1, count = u32(chunks.size());
    std::memcpy(&out[20], &one, 4);
    std::memcpy(&out[24], &total, 4);
    std::memcpy(&out[28], &count, 4);
    u32 pos = header;
    for (u32 i = 0; i < count; i++) {
        std::memcpy(&out[32 + i * 4], &pos, 4);
        std::memcpy(&out[pos], chunks[i].tag, 4);
        const u32 sz = u32(chunks[i].data.size());
        std::memcpy(&out[pos + 4], &sz, 4);
        std::memcpy(&out[pos + 8], chunks[i].data.data(), sz);
        pos += 8 + sz;
    }
    u32 sum[4];
    Checksum(out.data(), out.size(), sum);
    std::memcpy(&out[4], sum, 16);
    return out;
}

} // namespace gpu::dxbc
