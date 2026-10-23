#include "core/hle/video_output.h"

#include <chrono>
#include <mutex>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "core/memory.h"
#include "core/system.h"
#include "gpu/formats.h"

namespace video {

namespace {

using Microsoft::WRL::ComPtr;

struct Movie {
    std::mutex mutex;
    fs::path pending;  // movie to (re)start at the next VIC execute
    fs::path path;     // movie being decoded
    std::chrono::steady_clock::time_point started;
    ComPtr<IMFSourceReader> reader;
    u32 width = 0, height = 0;
    LONG stride = 0;
    std::vector<u8> frame; // last decoded picture, B G R X per pixel, rows top-down
    bool eof = false;
    u64 frames = 0;
};

Movie& M() {
    static Movie m;
    return m;
}

bool StartMf() {
    thread_local bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    (void)com;
    static const bool ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return ok;
}

bool Open(Movie& m, const fs::path& path) {
    m.reader.Reset();
    m.frame.clear();
    m.eof = false;
    m.frames = 0;
    m.path = path;
    if (!StartMf())
        return false;
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 1);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE); // decoder output converted to RGB32
    if (FAILED(MFCreateSourceReaderFromURL(path.wstring().c_str(), attr.Get(), &m.reader))) {
        LOG_WARN("Video", "cannot open {}", path.string());
        return false;
    }
    m.reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    m.reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    ComPtr<IMFMediaType> mt;
    MFCreateMediaType(&mt);
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(m.reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, mt.Get()))) {
        LOG_WARN("Video", "{}: no RGB32 output", path.string());
        m.reader.Reset();
        return false;
    }
    ComPtr<IMFMediaType> cur;
    m.reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
    m.width = w;
    m.height = h;
    m.stride = LONG(MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, w * 4));
    LOG_INFO("Video", "playing {} ({}x{})", path.filename().string(), w, h);
    return true;
}

bool NextFrame(Movie& m) {
    if (!m.reader || m.eof)
        return false;
    for (int tries = 0; tries < 8; tries++) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(m.reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &idx, &flags, &ts, &sample)))
            return false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            m.eof = true;
            return false;
        }
        if (!sample)
            continue; // stream tick / format change
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf)))
            return false;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&data, nullptr, &len)))
            return false;
        const u32 row = m.width * 4;
        m.frame.resize(size_t(row) * m.height);
        const LONG stride = m.stride ? m.stride : LONG(row);
        for (u32 y = 0; y < m.height; y++) {
            // Negative stride: bottom-up rows.
            const BYTE* src = stride > 0 ? data + size_t(y) * stride : data + size_t(m.height - 1 - y) * size_t(-stride);
            if (src + row <= data + len)
                std::memcpy(&m.frame[size_t(y) * row], src, row);
        }
        buf->Unlock();
        m.frames++;
        return true;
    }
    return false;
}

// Block-linear (GOB 64x8) store of a linear 2D image.
void Swizzle(const u8* src, u8* dst, u32 width, u32 height, u32 bpb, u32 bh) {
    const u32 width_bytes = width * bpb;
    const u32 gobs_x = (width_bytes + 63) / 64;
    const u64 block_size = 512ull << bh;
    const u32 block_rows = 8u << bh;
    for (u32 y = 0; y < height; y++) {
        const u64 row_base = u64(y / block_rows) * gobs_x * block_size + u64((y % block_rows) / 8) * 512;
        for (u32 x = 0; x < width_bytes; x += 16) {
            const u32 gob_off = ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16;
            std::memcpy(dst + row_base + u64(x / 64) * block_size + gob_off, src + size_t(y) * width_bytes + x,
                        std::min<u32>(16, width_bytes - x));
        }
    }
}

} // namespace

void OnMovieRead(const fs::path& host_path, u64 offset) {
    if (offset != 0)
        return;
    auto& m = M();
    std::lock_guard lk(m.mutex);
    m.pending = host_path;
}

void VicExecute(const VicState& s, const std::function<u64(u64)>& translate) {
    auto& mem = core::Sys().Mem();
    const u64 cfg_cpu = s.config ? translate(s.config + 0x20) : 0;
    const u64 luma = s.luma ? translate(s.luma) : 0;
    if (!cfg_cpu || !luma || !mem.IsValid(cfg_cpu, 8))
        return;
    const u64 cfg = mem.Read<u64>(cfg_cpu);
    const u32 fmt = u32(cfg & 0x7F), bl_kind = u32((cfg >> 11) & 0xF), bh = u32((cfg >> 15) & 0xF);
    const u32 sw = u32((cfg >> 32) & 0x3FFF) + 1, sh = u32((cfg >> 46) & 0x3FFF) + 1;

    auto& m = M();
    std::lock_guard lk(m.mutex);
    if (!m.pending.empty()) {
        Open(m, m.pending);
        m.pending.clear();
    }
    if (!NextFrame(m) && m.frame.empty())
        return;
    if (m.frames <= 1)
        LOG_INFO("Video", "VIC output {}x{} format {:#x} block-linear {} (height log2 {}) <- {}x{} frames", sw, sh, fmt, bl_kind, bh,
                 m.width, m.height);

    // Scale (nearest) the decoded BGRX frame to the output surface and convert to its format.
    std::vector<u8> out;
    if (fmt == 0x1F || fmt == 0x20) { // RGBA8 / BGRA8
        out.resize(size_t(sw) * sh * 4);
        for (u32 y = 0; y < sh; y++) {
            const u8* srow = &m.frame[size_t(u64(y) * m.height / sh) * m.width * 4];
            u8* drow = &out[size_t(y) * sw * 4];
            for (u32 x = 0; x < sw; x++) {
                const u8* p = srow + size_t(u64(x) * m.width / sw) * 4;
                if (fmt == 0x1F)
                    drow[x * 4 + 0] = p[2], drow[x * 4 + 1] = p[1], drow[x * 4 + 2] = p[0];
                else
                    drow[x * 4 + 0] = p[0], drow[x * 4 + 1] = p[1], drow[x * 4 + 2] = p[2];
                drow[x * 4 + 3] = 0xFF;
            }
        }
        if (bl_kind) {
            std::vector<u8> bl(size_t(gpu::BlockLinearSize(sw, sh, 1, 4, bh, 0)), 0);
            Swizzle(out.data(), bl.data(), sw, sh, 4, bh);
            if (mem.IsValid(luma, bl.size()))
                mem.WriteBlock(luma, bl.data(), bl.size());
        } else if (mem.IsValid(luma, out.size())) {
            mem.WriteBlock(luma, out.data(), out.size());
        }
    } else if (fmt == 0x44) { // YUV 4:2:0, Y plane + interleaved UV plane (pitch aligned to 256)
        const u64 chroma = s.chroma ? translate(s.chroma) : 0;
        const u32 pitch = (sw + 255) & ~255u;
        std::vector<u8> y_plane(size_t(pitch) * sh, 16), uv(size_t(pitch) * ((sh + 1) / 2), 128);
        for (u32 y = 0; y < sh; y++) {
            const u8* srow = &m.frame[size_t(u64(y) * m.height / sh) * m.width * 4];
            for (u32 x = 0; x < sw; x++) {
                const u8* p = srow + size_t(u64(x) * m.width / sw) * 4;
                const int r = p[2], g = p[1], b = p[0];
                y_plane[size_t(y) * pitch + x] = u8(std::clamp((66 * r + 129 * g + 25 * b + 128 >> 8) + 16, 0, 255));
                if (!(y & 1) && !(x & 1)) {
                    u8* c = &uv[size_t(y / 2) * pitch + x];
                    c[0] = u8(std::clamp((-38 * r - 74 * g + 112 * b + 128 >> 8) + 128, 0, 255));
                    c[1] = u8(std::clamp((112 * r - 94 * g - 18 * b + 128 >> 8) + 128, 0, 255));
                }
            }
        }
        if (mem.IsValid(luma, y_plane.size()))
            mem.WriteBlock(luma, y_plane.data(), y_plane.size());
        if (chroma && mem.IsValid(chroma, uv.size()))
            mem.WriteBlock(chroma, uv.data(), uv.size());
    } else {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_WARN("Video", "VIC output format {:#x} not supported", fmt);
        }
    }
}

} // namespace video
