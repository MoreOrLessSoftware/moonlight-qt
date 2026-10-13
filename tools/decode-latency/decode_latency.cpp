// Per-frame decode latency harness.
//
// Feeds a pre-recorded video stream to an FFmpeg decoder one access unit at a time, at a
// stream-like rate, and times each frame from submission until the GPU has really finished
// decoding it (not just until FFmpeg hands back a frame whose decode may still be in flight).
//
// The same clip can be run through D3D11VA, D3D12VA, Vulkan Video and AMF on Windows, and
// VAAPI and Vulkan Video on Linux, so the per-frame numbers are directly comparable.

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3d12video.h>
#endif

#ifdef HARNESS_HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

#ifdef HARNESS_HAVE_VAAPI
#include <va/va.h>
#endif

#ifdef HARNESS_HAVE_AMF
// Only the surface interface is needed: an AV_PIX_FMT_AMF_SURFACE frame carries an
// AMFSurface* in data[0]. (libavutil/hwcontext_amf.h assumes AMF's C interfaces and
// doesn't compile against its C++ ones, so it isn't included.)
#include <AMF/core/Surface.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/hwcontext_d3d12va.h>
#endif
#ifdef HARNESS_HAVE_VULKAN
#include <libavutil/hwcontext_vulkan.h>
#endif
#ifdef HARNESS_HAVE_VAAPI
#include <libavutil/hwcontext_vaapi.h>
#endif
}

using Clock = std::chrono::steady_clock;

static double toMs(Clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

static std::string avErr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

struct Options {
    std::string input;
#ifdef _WIN32
    std::string hwaccel = "d3d11va";
#else
    std::string hwaccel = "vaapi";
#endif
    std::string decoder;
    std::string device;
    std::string csv;
    std::vector<std::pair<std::string, std::string>> codecOpts;
    double fps = 60;
    int loops = 1;
    int warmup = 30;
    int threads = 1;
    bool sync = true;
    bool list = false;
};

static void usage()
{
    std::printf(
        "usage: decode_latency [options] <clip>\n"
        "\n"
        "  --hwaccel NAME   d3d11va, d3d12va, dxva2, vulkan, vaapi, amf, or none (software)\n"
        "                   [default: %s]. amf selects the <codec>_amf decoder.\n"
        "  --decoder NAME   force an FFmpeg decoder by name (e.g. hevc_amf)\n"
        "  --device STR     device string for av_hwdevice_ctx_create (adapter index on\n"
        "                   Windows, render node on Linux)\n"
        "  --fps N          submit at N frames per second, like a stream [default: 60].\n"
        "                   0 submits each frame as soon as the last one has finished.\n"
        "  --loops N        play the clip N times [default: 1]\n"
        "  --warmup N       leave the first N frames out of the statistics [default: 30]\n"
        "  --threads N      decoder threads [default: 1, as Moonlight uses]\n"
        "  --no-sync        don't wait for the GPU to finish each frame\n"
        "  -o KEY=VALUE     decoder AVOption (repeatable), e.g. -o decoder_mode=regular\n"
        "  --csv FILE       write one row per frame\n"
        "  --list           list the decoders and hwaccels this FFmpeg has for the clip's codec\n",
        Options().hwaccel.c_str());
}

static bool parseArgs(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "--hwaccel") o.hwaccel = next("--hwaccel");
        else if (a == "--decoder") o.decoder = next("--decoder");
        else if (a == "--device") o.device = next("--device");
        else if (a == "--fps") o.fps = std::atof(next("--fps"));
        else if (a == "--loops") o.loops = std::atoi(next("--loops"));
        else if (a == "--warmup") o.warmup = std::atoi(next("--warmup"));
        else if (a == "--threads") o.threads = std::atoi(next("--threads"));
        else if (a == "--csv") o.csv = next("--csv");
        else if (a == "--no-sync") o.sync = false;
        else if (a == "--list") o.list = true;
        else if (a == "-o") {
            std::string kv = next("-o");
            size_t eq = kv.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr, "-o expects KEY=VALUE, got '%s'\n", kv.c_str());
                return false;
            }
            o.codecOpts.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        else if (a == "-h" || a == "--help") {
            usage();
            std::exit(0);
        }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        }
        else o.input = a;
    }
    return !o.input.empty();
}

// Waits until the GPU has finished writing a decoded frame. Each path waits on the
// strongest completion signal its API offers for that frame.
class GpuSync {
public:
    ~GpuSync()
    {
#ifdef _WIN32
        releaseD3D11();
        if (m_D3D12Event) CloseHandle(m_D3D12Event);
#endif
    }

    // Returns false if this frame format has no wait implemented (the frame is then
    // timed only up to when FFmpeg returned it).
    bool wait(const AVFrame* frame)
    {
        switch (frame->format) {
#ifdef _WIN32
        case AV_PIX_FMT_D3D11:
            // data[0] is the texture, data[1] the array slice the decoder wrote.
            return waitD3D11((ID3D11Texture2D*)frame->data[0], (int)(intptr_t)frame->data[1]);
        case AV_PIX_FMT_D3D12:
            return waitD3D12((const AVD3D12VAFrame*)frame->data[0]);
#endif
#ifdef HARNESS_HAVE_AMF
        case AV_PIX_FMT_AMF_SURFACE:
            return waitAmf((amf::AMFSurface*)frame->data[0]);
#endif
#ifdef HARNESS_HAVE_VULKAN
        case AV_PIX_FMT_VULKAN:
            return waitVulkan(frame);
#endif
#ifdef HARNESS_HAVE_VAAPI
        case AV_PIX_FMT_VAAPI: {
            auto* fc = (AVHWFramesContext*)frame->hw_frames_ctx->data;
            auto* vdc = (AVVAAPIDeviceContext*)fc->device_ctx->hwctx;
            return vaSyncSurface(vdc->display, (VASurfaceID)(uintptr_t)frame->data[3]) == VA_STATUS_SUCCESS;
        }
#endif
        default:
            return false;
        }
    }

private:
#ifdef _WIN32
    void releaseD3D11()
    {
        if (m_Staging) m_Staging->Release();
        if (m_Context) m_Context->Release();
        if (m_Device) m_Device->Release();
        m_Staging = nullptr;
        m_Context = nullptr;
        m_Device = nullptr;
    }

    // Copies a small corner of the decoded slice (or of every slice, when the slice isn't
    // known) to a staging texture and maps it. The copy reads what the decoder wrote, so
    // the driver can't run it until the decode is done, and Map blocks until the copy is.
    bool waitD3D11(ID3D11Texture2D* tex, int slice)
    {
        ID3D11Device* device = nullptr;
        tex->GetDevice(&device);
        if (device != m_Device) {
            releaseD3D11();
            m_Device = device;
            m_Device->GetImmediateContext(&m_Context);

            // The decoder may use the immediate context from its own threads (AMF does).
            ID3D11Multithread* mt = nullptr;
            if (SUCCEEDED(m_Context->QueryInterface(__uuidof(ID3D11Multithread), (void**)&mt))) {
                mt->SetMultithreadProtected(TRUE);
                mt->Release();
            }
        }
        else {
            device->Release();
        }

        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        // A single staging texture, not an array: drivers can refuse planar (NV12/P010)
        // staging arrays. Every slice is copied into it in turn; the copies run in order,
        // so mapping it waits for all of them.
        if (!m_Staging || m_StagingDesc.Format != desc.Format) {
            if (m_Staging) {
                m_Staging->Release();
                m_Staging = nullptr;
            }
            D3D11_TEXTURE2D_DESC sd = {};
            sd.Width = std::min<UINT>(64, desc.Width);
            sd.Height = std::min<UINT>(64, desc.Height);
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = desc.Format;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            HRESULT hr = m_Device->CreateTexture2D(&sd, nullptr, &m_Staging);
            if (FAILED(hr)) {
                std::fprintf(stderr, "CreateTexture2D (staging) failed: 0x%08lx\n", (unsigned long)hr);
                return false;
            }
            m_StagingDesc = sd;
        }

        UINT first = slice >= 0 ? (UINT)slice : 0;
        UINT last = slice >= 0 ? (UINT)slice : desc.ArraySize - 1;
        D3D11_BOX box = { 0, 0, 0, m_StagingDesc.Width, m_StagingDesc.Height, 1 };
        for (UINT s = first; s <= last; s++) {
            m_Context->CopySubresourceRegion(m_Staging, 0, 0, 0, 0,
                                             tex, D3D11CalcSubresource(0, s, desc.MipLevels), &box);
        }

        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_Context->Map(m_Staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            std::fprintf(stderr, "Map (staging) failed: 0x%08lx\n", (unsigned long)hr);
            return false;
        }
        m_Context->Unmap(m_Staging, 0);
        return true;
    }

    bool waitD3D12(const AVD3D12VAFrame* f)
    {
        ID3D12Fence* fence = f->sync_ctx.fence;
        UINT64 value = f->sync_ctx.fence_value;
        if (fence->GetCompletedValue() >= value) {
            return true;
        }
        if (!m_D3D12Event) {
            m_D3D12Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        if (FAILED(fence->SetEventOnCompletion(value, m_D3D12Event))) {
            return false;
        }
        WaitForSingleObject(m_D3D12Event, INFINITE);
        return true;
    }

    ID3D11Device* m_Device = nullptr;
    ID3D11DeviceContext* m_Context = nullptr;
    ID3D11Texture2D* m_Staging = nullptr;
    D3D11_TEXTURE2D_DESC m_StagingDesc = {};
    HANDLE m_D3D12Event = nullptr;
#endif

#ifdef HARNESS_HAVE_AMF
    bool waitAmf(amf::AMFSurface* surface)
    {
#ifdef _WIN32
        if (surface->GetMemoryType() == amf::AMF_MEMORY_DX11) {
            // The slice of a decoder texture array isn't exposed, so copy from every slice.
            return waitD3D11((ID3D11Texture2D*)surface->GetPlaneAt(0)->GetNative(), -1);
        }
#endif
        return false;
    }
#endif

#ifdef HARNESS_HAVE_VULKAN
    bool waitVulkan(const AVFrame* frame)
    {
        auto* fc = (AVHWFramesContext*)frame->hw_frames_ctx->data;
        auto* vfc = (AVVulkanFramesContext*)fc->hwctx;
        auto* vdc = (AVVulkanDeviceContext*)fc->device_ctx->hwctx;
        auto* f = (AVVkFrame*)frame->data[0];

        if (!m_WaitSemaphores) {
            auto getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vdc->get_proc_addr(vdc->inst, "vkGetDeviceProcAddr");
            m_WaitSemaphores = (PFN_vkWaitSemaphores)getDeviceProcAddr(vdc->act_dev, "vkWaitSemaphores");
            if (!m_WaitSemaphores) {
                return false;
            }
        }

        VkSemaphore sems[AV_NUM_DATA_POINTERS];
        uint64_t values[AV_NUM_DATA_POINTERS];
        uint32_t count = 0;
        if (vfc->lock_frame) vfc->lock_frame(fc, f);
        for (int i = 0; i < AV_NUM_DATA_POINTERS && f->img[i] != VK_NULL_HANDLE; i++) {
            sems[count] = f->sem[i];
            values[count] = f->sem_value[i];
            count++;
        }
        if (vfc->unlock_frame) vfc->unlock_frame(fc, f);

        VkSemaphoreWaitInfo wi = {};
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = count;
        wi.pSemaphores = sems;
        wi.pValues = values;
        return m_WaitSemaphores(vdc->act_dev, &wi, UINT64_MAX) == VK_SUCCESS;
    }

    PFN_vkWaitSemaphores m_WaitSemaphores = nullptr;
#endif
};

struct FrameRecord {
    int64_t index = -1;
    int size = 0;
    Clock::time_point submitStart;
    double submitMs = 0;  // avcodec_send_packet
    double outputMs = 0;  // submit start -> frame returned by FFmpeg
    double gpuMs = 0;     // frame returned -> GPU decode complete
    double totalMs = 0;   // submit start -> GPU decode complete
    bool received = false;
    bool late = false;    // only came out after a later frame was submitted
};

static AVPixelFormat getHwFormat(AVCodecContext* ctx, const AVPixelFormat* fmts)
{
    auto wanted = *(const AVPixelFormat*)ctx->opaque;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == wanted) {
            return *p;
        }
    }
    std::fprintf(stderr, "Decoder did not offer %s; refusing to fall back to software\n",
                 av_get_pix_fmt_name(wanted));
    return AV_PIX_FMT_NONE;
}

static void listDecoders(AVCodecID codecId)
{
    std::printf("Decoders for %s:\n", avcodec_get_name(codecId));
    void* it = nullptr;
    const AVCodec* c;
    while ((c = av_codec_iterate(&it))) {
        if (!av_codec_is_decoder(c) || c->id != codecId) {
            continue;
        }
        std::printf("  %s%s\n", c->name, (c->capabilities & AV_CODEC_CAP_HARDWARE) ? " (hardware decoder)" : "");
        for (int i = 0;; i++) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(c, i);
            if (!cfg) break;
            std::printf("      hwaccel %-10s -> %s%s\n",
                        av_hwdevice_get_type_name(cfg->device_type),
                        av_get_pix_fmt_name(cfg->pix_fmt),
                        (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) ? "" : " (no device ctx method)");
        }
        if (c->priv_class) {
            const AVOption* opt = nullptr;
            bool header = false;
            while ((opt = av_opt_next(&c->priv_class, opt))) {
                if (opt->type == AV_OPT_TYPE_CONST) continue;
                if (!header) {
                    std::printf("      options:\n");
                    header = true;
                }
                std::printf("        %-20s %s\n", opt->name, opt->help ? opt->help : "");
            }
        }
    }
}

static void waitUntil(Clock::time_point deadline)
{
    // Sleep most of the way, then spin, so the submit time is accurate to well under 1 ms.
    for (;;) {
        auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) {
            return;
        }
        if (remaining > std::chrono::milliseconds(2)) {
            std::this_thread::sleep_for(remaining - std::chrono::milliseconds(2));
        }
        else {
            std::this_thread::yield();
        }
    }
}

struct Summary {
    double mean, min, p50, p90, p99, max;
};

static Summary summarize(std::vector<double> v)
{
    Summary s = {};
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    auto pct = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))]; };
    s.mean = sum / v.size();
    s.min = v.front();
    s.p50 = pct(0.50);
    s.p90 = pct(0.90);
    s.p99 = pct(0.99);
    s.max = v.back();
    return s;
}

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        usage();
        return 2;
    }

    av_log_set_level(AV_LOG_WARNING);

    // Load the whole clip up front so file I/O never lands inside a timed region.
    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, opt.input.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "Can't open %s: %s\n", opt.input.c_str(), avErr(ret).c_str());
        return 1;
    }
    avformat_find_stream_info(fmt, nullptr);
    int streamIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        std::fprintf(stderr, "No video stream in %s\n", opt.input.c_str());
        return 1;
    }
    AVStream* stream = fmt->streams[streamIndex];

    if (opt.list) {
        listDecoders(stream->codecpar->codec_id);
        return 0;
    }

    std::vector<AVPacket*> packets;
    int64_t totalBytes = 0;
    {
        AVPacket* pkt = av_packet_alloc();
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == streamIndex) {
                totalBytes += pkt->size;
                packets.push_back(av_packet_clone(pkt));
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    if (packets.empty()) {
        std::fprintf(stderr, "No packets read from %s\n", opt.input.c_str());
        return 1;
    }

    // Choose the decoder
    const AVCodec* codec = nullptr;
    bool amf = opt.hwaccel == "amf";
    if (!opt.decoder.empty()) {
        codec = avcodec_find_decoder_by_name(opt.decoder.c_str());
    }
    else if (amf) {
        std::string name = std::string(avcodec_get_name(stream->codecpar->codec_id)) + "_amf";
        codec = avcodec_find_decoder_by_name(name.c_str());
    }
    else {
        codec = avcodec_find_decoder(stream->codecpar->codec_id);
    }
    if (!codec) {
        std::fprintf(stderr, "Decoder not found in this FFmpeg build (try --list)\n");
        return 1;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, stream->codecpar);
    // Packets are restamped with their frame index; a coarse time base keeps decoders that
    // convert timestamps internally (AMF uses 100 ns units) from rounding them.
    ctx->pkt_timebase = AVRational{ 1, 1000 };
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_count = opt.threads;

    // Set up the hardware device
    AVPixelFormat hwFormat = AV_PIX_FMT_NONE;
    AVBufferRef* hwDevice = nullptr;
    if (opt.hwaccel != "none") {
        AVHWDeviceType type = av_hwdevice_find_type_by_name(opt.hwaccel.c_str());
        if (type == AV_HWDEVICE_TYPE_NONE) {
            std::fprintf(stderr, "Unknown hwaccel '%s' (try --list)\n", opt.hwaccel.c_str());
            return 1;
        }
        for (int i = 0;; i++) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if (cfg->device_type == type && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                hwFormat = cfg->pix_fmt;
                break;
            }
        }
        if (hwFormat == AV_PIX_FMT_NONE) {
            std::fprintf(stderr, "Decoder %s doesn't support hwaccel %s (try --list)\n", codec->name, opt.hwaccel.c_str());
            return 1;
        }
        ret = av_hwdevice_ctx_create(&hwDevice, type, opt.device.empty() ? nullptr : opt.device.c_str(), nullptr, 0);
        if (ret < 0) {
            std::fprintf(stderr, "Can't create %s device: %s\n", opt.hwaccel.c_str(), avErr(ret).c_str());
            return 1;
        }
        ctx->hw_device_ctx = av_buffer_ref(hwDevice);
        ctx->opaque = &hwFormat;
        ctx->get_format = getHwFormat;
    }

    // AMF defaults to its low-latency mode, which is what a streaming client would use.
    // Pass -o decoder_mode=regular -o lowlatency=0 to compare against its default behaviour.
    AVDictionary* codecDict = nullptr;
    if (amf) {
        av_dict_set(&codecDict, "decoder_mode", "low_latency", 0);
        av_dict_set(&codecDict, "lowlatency", "1", 0);
    }
    for (auto& kv : opt.codecOpts) {
        av_dict_set(&codecDict, kv.first.c_str(), kv.second.c_str(), 0);
    }
    std::string optsUsed;
    {
        const AVDictionaryEntry* e = nullptr;
        while ((e = av_dict_get(codecDict, "", e, AV_DICT_IGNORE_SUFFIX))) {
            optsUsed += std::string(optsUsed.empty() ? "" : " ") + e->key + "=" + e->value;
        }
    }

    ret = avcodec_open2(ctx, codec, &codecDict);
    if (ret < 0) {
        std::fprintf(stderr, "avcodec_open2 failed: %s\n", avErr(ret).c_str());
        return 1;
    }
    {
        const AVDictionaryEntry* e = nullptr;
        while ((e = av_dict_get(codecDict, "", e, AV_DICT_IGNORE_SUFFIX))) {
            std::fprintf(stderr, "Warning: decoder ignored option %s=%s\n", e->key, e->value);
        }
        av_dict_free(&codecDict);
    }

    // Raw elementary streams carry no frame rate (the demuxer guesses 25), so the bitrate is
    // reported at the rate frames are actually fed.
    double clipFps = opt.fps > 0 ? opt.fps : av_q2d(stream->avg_frame_rate);
    if (clipFps <= 0) clipFps = 60;
    std::printf("clip     %s: %dx%d %s, %zu frames, avg %.1f KB/frame (%.1f Mbps at %.0f fps)\n",
                opt.input.c_str(), stream->codecpar->width, stream->codecpar->height,
                avcodec_get_name(stream->codecpar->codec_id), packets.size(),
                totalBytes / 1024.0 / packets.size(), totalBytes * 8.0 / packets.size() * clipFps / 1e6, clipFps);
    std::printf("decoder  %s, hwaccel %s%s%s, threads %d%s%s\n", codec->name, opt.hwaccel.c_str(),
                opt.device.empty() ? "" : ", device ", opt.device.c_str(), opt.threads,
                optsUsed.empty() ? "" : ", options: ", optsUsed.c_str());
    if (opt.fps > 0) std::printf("pacing   %.2f fps (%.2f ms per frame)\n", opt.fps, 1000.0 / opt.fps);
    else std::printf("pacing   back to back\n");

#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    const int64_t totalFrames = (int64_t)packets.size() * std::max(1, opt.loops);
    std::vector<FrameRecord> records((size_t)totalFrames);
    GpuSync gpu;
    bool warnedNoSync = false;
    int64_t overruns = 0;
    int64_t lastSubmitted = -1;
    int64_t nextExpected = 0;
    AVFrame* frame = av_frame_alloc();
    std::string outFormat;

    // Pulls every frame FFmpeg has ready. If waitFor is set, keeps polling until that
    // frame comes out or the timeout passes, since an asynchronous decoder (AMF) may need
    // a few polls before its output is ready.
    auto drain = [&](int64_t waitFor, Clock::time_point timeout) -> bool {
        for (;;) {
            int r = avcodec_receive_frame(ctx, frame);
            if (r == AVERROR(EAGAIN)) {
                if (waitFor < 0 || records[(size_t)waitFor].received || Clock::now() >= timeout) {
                    return true;
                }
                continue;
            }
            if (r == AVERROR_EOF) {
                return true;
            }
            if (r < 0) {
                std::fprintf(stderr, "avcodec_receive_frame failed: %s\n", avErr(r).c_str());
                return false;
            }

            auto returned = Clock::now();
            if (opt.sync && !gpu.wait(frame) && !warnedNoSync) {
                warnedNoSync = true;
                std::fprintf(stderr, "Warning: no GPU completion wait for %s frames; gpu column is 0\n",
                             av_get_pix_fmt_name((AVPixelFormat)frame->format));
            }
            auto done = Clock::now();

            if (outFormat.empty()) {
                outFormat = av_get_pix_fmt_name((AVPixelFormat)frame->format);
                if (frame->hw_frames_ctx) {
                    auto* fc = (AVHWFramesContext*)frame->hw_frames_ctx->data;
                    outFormat += std::string(" (") + av_get_pix_fmt_name(fc->sw_format) + ")";
                }
            }

            // Match the frame to its submission by timestamp. If the decoder didn't carry
            // the timestamp through, fall back to submission order, which is output order
            // for a stream without B-frames.
            int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
            int64_t which = (pts >= 0 && pts <= lastSubmitted && !records[(size_t)pts].received) ? pts : nextExpected;
            if (which <= lastSubmitted) {
                FrameRecord& rec = records[(size_t)which];
                rec.received = true;
                rec.outputMs = toMs(returned - rec.submitStart);
                rec.gpuMs = toMs(done - returned);
                rec.totalMs = toMs(done - rec.submitStart);
                rec.late = which < lastSubmitted;
                nextExpected = which + 1;
            }
            av_frame_unref(frame);
        }
    };

    const auto period = opt.fps > 0
        ? std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / opt.fps))
        : Clock::duration::zero();
    // Wait most of a frame interval for an asynchronous decoder's output before moving on.
    const auto pollWindow = opt.fps > 0
        ? std::chrono::duration_cast<Clock::duration>(period * 0.9)
        : std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(100));

    auto nextSubmit = Clock::now();
    AVPacket* pkt = av_packet_alloc();
    for (int64_t index = 0; index < totalFrames; index++) {
        if (opt.fps > 0) {
            waitUntil(nextSubmit);
            nextSubmit += period;
            if (Clock::now() > nextSubmit) {
                // We fell more than a frame behind; don't burst to catch up.
                overruns++;
                nextSubmit = Clock::now();
            }
        }

        av_packet_ref(pkt, packets[(size_t)(index % (int64_t)packets.size())]);
        pkt->pts = pkt->dts = index;

        FrameRecord& rec = records[(size_t)index];
        rec.index = index;
        rec.size = pkt->size;
        rec.submitStart = Clock::now();
        ret = avcodec_send_packet(ctx, pkt);
        while (ret == AVERROR(EAGAIN)) {
            // The decoder wants its output taken first
            if (!drain(-1, Clock::now())) return 1;
            ret = avcodec_send_packet(ctx, pkt);
        }
        rec.submitMs = toMs(Clock::now() - rec.submitStart);
        lastSubmitted = index;
        av_packet_unref(pkt);
        if (ret < 0) {
            std::fprintf(stderr, "avcodec_send_packet failed on frame %" PRId64 ": %s\n", index, avErr(ret).c_str());
            return 1;
        }

        if (!drain(index, rec.submitStart + pollWindow)) return 1;
    }

    avcodec_send_packet(ctx, nullptr);
    drain(-1, Clock::now());

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    // Report
    std::vector<double> submit, output, gpuWait, total;
    int64_t missing = 0, late = 0;
    for (const auto& r : records) {
        if (r.index < opt.warmup) continue;
        if (!r.received) {
            missing++;
            continue;
        }
        if (r.late) late++;
        submit.push_back(r.submitMs);
        output.push_back(r.outputMs);
        gpuWait.push_back(r.gpuMs);
        total.push_back(r.totalMs);
    }

    std::printf("output   %s\n", outFormat.empty() ? "(none)" : outFormat.c_str());
    std::printf("\n%" PRId64 " frames measured (first %d skipped)", (int64_t)total.size(), opt.warmup);
    if (missing) std::printf(", %" PRId64 " never came out", missing);
    if (late) std::printf(", %" PRId64 " only came out after the next submit", late);
    if (overruns) std::printf(", %" PRId64 " pacing overruns", overruns);
    std::printf("\n\n%-28s %7s %7s %7s %7s %7s %7s\n", "ms", "mean", "min", "p50", "p90", "p99", "max");
    auto row = [](const char* name, const std::vector<double>& v) {
        Summary s = summarize(v);
        std::printf("%-28s %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f\n", name, s.mean, s.min, s.p50, s.p90, s.p99, s.max);
    };
    row("send_packet", submit);
    row("submit -> frame returned", output);
    row("frame returned -> gpu done", gpuWait);
    row("total (submit -> gpu done)", total);

    if (!opt.csv.empty()) {
        FILE* f = std::fopen(opt.csv.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "Can't write %s\n", opt.csv.c_str());
        }
        else {
            std::fprintf(f, "frame,bytes,send_ms,returned_ms,gpu_wait_ms,total_ms,late,received\n");
            for (const auto& r : records) {
                std::fprintf(f, "%" PRId64 ",%d,%.3f,%.3f,%.3f,%.3f,%d,%d\n", r.index, r.size, r.submitMs,
                             r.outputMs, r.gpuMs, r.totalMs, r.late ? 1 : 0, r.received ? 1 : 0);
            }
            std::fclose(f);
            std::printf("\nper-frame rows written to %s\n", opt.csv.c_str());
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    av_buffer_unref(&hwDevice);
    for (auto* p : packets) av_packet_free(&p);
    avformat_close_input(&fmt);
    return 0;
}
