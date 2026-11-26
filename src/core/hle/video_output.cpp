#include "core/hle/video_output.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "core/memory.h"
#include "core/system.h"
#include "gpu/formats.h"
#include "gpu/gpu.h"

#ifdef MUA3_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#endif

namespace video {

namespace {

using Microsoft::WRL::ComPtr;

struct Movie {
    std::mutex mutex;
    fs::path pending;  // movie to (re)start at the next VIC execute
    fs::path path;     // movie being decoded
    std::chrono::steady_clock::time_point started;
    ComPtr<IMFSourceReader> reader;
#ifdef MUA3_HAVE_FFMPEG
    AVFormatContext* fmt = nullptr; // FFmpeg (Wine/Proton, or MUA3_VIDEO=ffmpeg): demuxer, decoder, buffers
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frm = nullptr;
    int stream = -1;
    bool drained = false;
#endif
    u32 width = 0, height = 0;
    LONG stride = 0;
    // last decoded picture: Media Foundation B G R X per pixel, rows top-down; FFmpeg (yuv) I420: the Y plane
    // (width x height), then U and V ((width+1)/2 x (height+1)/2 each)
    std::vector<u8> frame;
    bool yuv = false;
    bool eof = false;
    u64 frames = 0;
};

Movie& M() {
    static Movie m;
    return m;
}

bool UnderWine() {
    static const bool wine = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version");
    }();
    return wine;
}

#ifdef MUA3_HAVE_FFMPEG
// FFmpeg's DLLs (LGPL, scripts/build_ffmpeg.sh: H.264 decoder + MP4 demuxer only) are loaded from the game's folder at
// run time, so the game runs without them and players can swap them. Wine/Proton has no H.264 decoder in Media
// Foundation (it hands back a 320x240 placeholder), so FFmpeg decodes the movies there; Windows keeps Media
// Foundation. MUA3_VIDEO=ffmpeg / mf picks one by hand.
struct FfApi {
    decltype(&avformat_open_input) open_input;
    decltype(&avformat_find_stream_info) find_stream_info;
    decltype(&av_find_best_stream) find_best_stream;
    decltype(&av_read_frame) read_frame;
    decltype(&avformat_close_input) close_input;
    decltype(&avcodec_alloc_context3) alloc_context;
    decltype(&avcodec_parameters_to_context) params_to_context;
    decltype(&avcodec_open2) open2;
    decltype(&avcodec_send_packet) send_packet;
    decltype(&avcodec_receive_frame) receive_frame;
    decltype(&avcodec_free_context) free_context;
    decltype(&av_packet_alloc) packet_alloc;
    decltype(&av_packet_unref) packet_unref;
    decltype(&av_packet_free) packet_free;
    decltype(&av_frame_alloc) frame_alloc;
    decltype(&av_frame_free) frame_free;
    decltype(&av_version_info) version_info;
};

const FfApi* Ff() {
    static const std::optional<FfApi> api = []() -> std::optional<FfApi> {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const fs::path dir = fs::path(exe).parent_path();
        auto load = [&](const char* base, int major) {
            const fs::path p = dir / fmt::format("{}-{}.dll", base, major);
            return LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        };
        const HMODULE util = load("avutil", LIBAVUTIL_VERSION_MAJOR);
        const HMODULE codec = util ? load("avcodec", LIBAVCODEC_VERSION_MAJOR) : nullptr;
        const HMODULE format = codec ? load("avformat", LIBAVFORMAT_VERSION_MAJOR) : nullptr;
        if (!format) {
            LOG_INFO("Video", "FFmpeg DLLs (avutil-{} avcodec-{} avformat-{}) not next to the game: movies use Media "
                              "Foundation only", LIBAVUTIL_VERSION_MAJOR, LIBAVCODEC_VERSION_MAJOR, LIBAVFORMAT_VERSION_MAJOR);
            return std::nullopt;
        }
        FfApi a{};
        bool ok = true;
        auto get = [&](HMODULE h, auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(h, name));
            ok = ok && fn != nullptr;
        };
        get(format, a.open_input, "avformat_open_input");
        get(format, a.find_stream_info, "avformat_find_stream_info");
        get(format, a.find_best_stream, "av_find_best_stream");
        get(format, a.read_frame, "av_read_frame");
        get(format, a.close_input, "avformat_close_input");
        get(codec, a.alloc_context, "avcodec_alloc_context3");
        get(codec, a.params_to_context, "avcodec_parameters_to_context");
        get(codec, a.open2, "avcodec_open2");
        get(codec, a.send_packet, "avcodec_send_packet");
        get(codec, a.receive_frame, "avcodec_receive_frame");
        get(codec, a.free_context, "avcodec_free_context");
        get(codec, a.packet_alloc, "av_packet_alloc");
        get(codec, a.packet_unref, "av_packet_unref");
        get(codec, a.packet_free, "av_packet_free");
        get(util, a.frame_alloc, "av_frame_alloc");
        get(util, a.frame_free, "av_frame_free");
        get(util, a.version_info, "av_version_info");
        if (!ok) {
            LOG_WARN("Video", "FFmpeg DLLs next to the game lack functions this build needs: movies use Media Foundation only");
            return std::nullopt;
        }
        LOG_INFO("Video", "FFmpeg {} loaded (H.264 movie decoder)", a.version_info());
        return a;
    }();
    return api ? &*api : nullptr;
}

bool UseFfmpeg() {
    static const bool use = [] {
        const char* v = std::getenv("MUA3_VIDEO");
        if (v && !_stricmp(v, "mf"))
            return false;
        return (v && !_stricmp(v, "ffmpeg")) || UnderWine();
    }();
    return use && Ff();
}

void CloseFf(Movie& m) {
    if (const FfApi* f = m.fmt || m.dec ? Ff() : nullptr) {
        f->frame_free(&m.frm);
        f->packet_free(&m.pkt);
        f->free_context(&m.dec);
        f->close_input(&m.fmt);
    }
    m.stream = -1;
    m.drained = false;
}

bool OpenFf(Movie& m, const fs::path& path) {
    const FfApi* f = Ff();
    const std::u8string u8 = path.u8string();
    if (f->open_input(&m.fmt, reinterpret_cast<const char*>(u8.c_str()), nullptr, nullptr) < 0) {
        LOG_WARN("Video", "FFmpeg cannot open {}", path.string());
        m.fmt = nullptr;
        return false;
    }
    const AVCodec* codec = nullptr;
    if (f->find_stream_info(m.fmt, nullptr) < 0 ||
        (m.stream = f->find_best_stream(m.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0 || !codec ||
        !(m.dec = f->alloc_context(codec)) || f->params_to_context(m.dec, m.fmt->streams[m.stream]->codecpar) < 0) {
        LOG_WARN("Video", "FFmpeg: no decodable video stream in {}", path.string());
        CloseFf(m);
        return false;
    }
    // Frame threads: 1080p H.264 on the Steam Deck's 4 cores, next to the game's own threads.
    m.dec->thread_count = 4;
    m.dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (f->open2(m.dec, codec, nullptr) < 0 || !(m.pkt = f->packet_alloc()) || !(m.frm = f->frame_alloc())) {
        LOG_WARN("Video", "FFmpeg: cannot start the decoder for {}", path.string());
        CloseFf(m);
        return false;
    }
    m.width = u32(m.dec->width);
    m.height = u32(m.dec->height);
    m.yuv = true;
    LOG_INFO("Video", "playing {} ({}x{}, FFmpeg)", path.filename().string(), m.width, m.height);
    return true;
}

bool NextFrameFf(Movie& m) {
    const FfApi* f = Ff();
    for (int guard = 0; guard < 4096; guard++) {
        const int r = f->receive_frame(m.dec, m.frm);
        if (r == 0) {
            if (m.frm->format != AV_PIX_FMT_YUV420P && m.frm->format != AV_PIX_FMT_YUVJ420P) {
                LOG_WARN("Video", "{}: FFmpeg picture format {} (only 8-bit 4:2:0 is handled)", m.path.filename().string(),
                         m.frm->format);
                m.eof = true;
                return false;
            }
            const u32 w = u32(m.frm->width), h = u32(m.frm->height), cw = (w + 1) / 2, ch = (h + 1) / 2;
            m.width = w;
            m.height = h;
            m.frame.resize(size_t(w) * h + 2 * size_t(cw) * ch);
            u8* d = m.frame.data();
            for (u32 y = 0; y < h; y++, d += w)
                std::memcpy(d, m.frm->data[0] + size_t(y) * m.frm->linesize[0], w);
            for (int p = 1; p <= 2; p++)
                for (u32 y = 0; y < ch; y++, d += cw)
                    std::memcpy(d, m.frm->data[p] + size_t(y) * m.frm->linesize[p], cw);
            m.frames++;
            return true;
        }
        if (r != AVERROR(EAGAIN) || m.drained) {
            m.eof = true;
            return false;
        }
        // the decoder wants more input: the next packet of the video stream, or the drain signal at the end
        for (;;) {
            if (f->read_frame(m.fmt, m.pkt) < 0) {
                f->send_packet(m.dec, nullptr);
                m.drained = true;
                break;
            }
            const bool video = m.pkt->stream_index == m.stream;
            if (video)
                f->send_packet(m.dec, m.pkt);
            f->packet_unref(m.pkt);
            if (video)
                break;
        }
    }
    return false;
}
#endif

bool Decoding(const Movie& m) {
#ifdef MUA3_HAVE_FFMPEG
    if (m.dec)
        return true;
#endif
    return m.reader != nullptr;
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
    m.yuv = false;
    m.eof = false;
    m.frames = 0;
    m.path = path;
#ifdef MUA3_HAVE_FFMPEG
    CloseFf(m);
    if (UseFfmpeg() && OpenFf(m, path))
        return true;
#endif
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
    if (UnderWine() && w == 320 && h == 240) {
        // Proton has no H.264 decoder: its Media Foundation hands back share/media/blank.mkv (AV1, 320x240) for every
        // movie. All of this game's movies are 1920x1080, so show black instead of the test picture; the game still
        // draws subtitles over the movie surface and plays the sound itself.
        LOG_WARN("Video", "{}: Wine/Proton gave its 320x240 placeholder (no H.264 decoder), showing black instead",
                 path.filename().string());
        m.reader.Reset();
        m.width = m.height = 16;
        m.stride = 16 * 4;
        m.frame.assign(size_t(16) * 16 * 4, 0);
        m.eof = true;
        return false;
    }
    m.width = w;
    m.height = h;
    m.stride = LONG(MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, w * 4));
    LOG_INFO("Video", "playing {} ({}x{})", path.filename().string(), w, h);
    return true;
}

bool NextFrame(Movie& m) {
#ifdef MUA3_HAVE_FFMPEG
    if (m.dec)
        return !m.eof && NextFrameFf(m);
#endif
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
    if (m.frames <= 1) {
        const u64 pitch = (u64(sw) + 255) & ~255ull; // (0x44: Y plane then a half-height UV plane, as written below)
        gpu::NoteMoviePlanes(luma, fmt == 0x44 ? pitch * sh : u64(sw) * sh * 4, s.chroma ? translate(s.chroma) : 0,
                             pitch * ((sh + 1) / 2));
        LOG_INFO("Video", "VIC output {}x{} format {:#x} block-linear {} (height log2 {}) <- {}x{} frames; luma gpu {:#x} "
                          "chroma gpu {:#x} (cpu {:#x} / {:#x})", sw, sh, fmt, bl_kind, bh, m.width, m.height, s.luma, s.chroma,
                 luma, s.chroma ? translate(s.chroma) : 0);
        if (!Decoding(m))
            m.frames = 2; // black stand-in frame (no decoder): note the planes and log once
    }

    // FFmpeg's I420 picture: straight into the YUV surface (the game converts it as it does the console's decoder
    // output, no RGB round trip), or to BGRX (BT.601, limited range) for the RGBA surfaces below.
    std::vector<u8> bgrx;
    const std::vector<u8>* rgb = &m.frame;
    if (m.yuv) {
        const u32 w = m.width, h = m.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
        const u8* Y = m.frame.data();
        const u8* U = Y + size_t(w) * h;
        const u8* V = U + size_t(cw) * ch;
        if (fmt == 0x44) {
            const u64 chroma = s.chroma ? translate(s.chroma) : 0;
            const u32 pitch = (sw + 255) & ~255u, sch = (sh + 1) / 2, scw = (sw + 1) / 2;
            std::vector<u8> y_plane(size_t(pitch) * sh, 16), uv(size_t(pitch) * sch, 128);
            for (u32 y = 0; y < sh; y++) {
                const u8* srow = Y + size_t(u64(y) * h / sh) * w;
                u8* d = &y_plane[size_t(y) * pitch];
                if (sw == w)
                    std::memcpy(d, srow, w);
                else
                    for (u32 x = 0; x < sw; x++)
                        d[x] = srow[u64(x) * w / sw];
            }
            for (u32 y = 0; y < sch; y++) {
                const size_t sy = size_t(u64(y) * ch / sch) * cw;
                u8* d = &uv[size_t(y) * pitch];
                for (u32 x = 0; x < scw; x++) {
                    const size_t sx = sy + u64(x) * cw / scw;
                    d[x * 2] = U[sx], d[x * 2 + 1] = V[sx];
                }
            }
            if (mem.IsValid(luma, y_plane.size()))
                mem.WriteBlock(luma, y_plane.data(), y_plane.size());
            if (chroma && mem.IsValid(chroma, uv.size()))
                mem.WriteBlock(chroma, uv.data(), uv.size());
            return;
        }
        bgrx.resize(size_t(w) * h * 4);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                const int c = 298 * (Y[size_t(y) * w + x] - 16) + 128;
                const int u = U[size_t(y / 2) * cw + x / 2] - 128, v = V[size_t(y / 2) * cw + x / 2] - 128;
                u8* p = &bgrx[(size_t(y) * w + x) * 4];
                p[0] = u8(std::clamp((c + 516 * u) >> 8, 0, 255));
                p[1] = u8(std::clamp((c - 100 * u - 208 * v) >> 8, 0, 255));
                p[2] = u8(std::clamp((c + 409 * v) >> 8, 0, 255));
                p[3] = 0xFF;
            }
        rgb = &bgrx;
    }

    // Scale (nearest) the decoded BGRX frame to the output surface and convert to its format.
    std::vector<u8> out;
    if (fmt == 0x1F || fmt == 0x20) { // RGBA8 / BGRA8
        out.resize(size_t(sw) * sh * 4);
        for (u32 y = 0; y < sh; y++) {
            const u8* srow = &(*rgb)[size_t(u64(y) * m.height / sh) * m.width * 4];
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
            const u8* srow = &(*rgb)[size_t(u64(y) * m.height / sh) * m.width * 4];
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
