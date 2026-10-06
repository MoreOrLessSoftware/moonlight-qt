#include "pyrowavedecoder.h"
#include "pyrowaveapi.h"

#include <QtGlobal>
#include <QCoreApplication>
#include <QDir>

#include <cstring>
#include <mutex>
#include <type_traits>
#include <vector>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef Q_OS_WIN32
static void* openPyrowaveLibrary()
{
    // Kept loaded for the life of the process
    HMODULE dll = LoadLibraryExW(L"libpyrowave-shared-0.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (dll == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: libpyrowave-shared-0.dll not found, the codec is unavailable");
    }
    return (void*)dll;
}

static void* lookUpPyrowaveSymbol(void* library, const char* name)
{
    return (void*)GetProcAddress((HMODULE)library, name);
}
#else
static void* openPyrowaveLibrary()
{
    // ML_PYROWAVE_LIBRARY names the library to load, such as one built locally.
    // Otherwise the usual library paths are searched, then Moonlight's own directory.
    QByteArray override = qgetenv("ML_PYROWAVE_LIBRARY");
    if (!override.isEmpty()) {
        void* library = dlopen(override.constData(), RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "PyroWave: couldn't load %s: %s",
                        override.constData(),
                        dlerror());
        }
        return library;
    }

    QByteArray appDir = QDir(QCoreApplication::applicationDirPath()).absolutePath().toLocal8Bit();
    const QByteArray candidates[] = {
        "libpyrowave-shared.so.0",
        "libpyrowave-shared.so",
        appDir + "/libpyrowave-shared.so.0",
        appDir + "/libpyrowave-shared.so",
    };

    // Kept loaded for the life of the process
    for (const QByteArray& candidate : candidates) {
        void* library = dlopen(candidate.constData(), RTLD_NOW | RTLD_LOCAL);
        if (library != nullptr) {
            return library;
        }
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave: libpyrowave-shared.so.0 not found, the codec is unavailable");
    return nullptr;
}

static void* lookUpPyrowaveSymbol(void* library, const char* name)
{
    return dlsym(library, name);
}
#endif

const PyrowaveApi* loadPyrowaveApi()
{
    static std::once_flag once;
    static PyrowaveApi api = {};
    static bool loaded = false;

    std::call_once(once, []() {
        void* library = openPyrowaveLibrary();
        if (library == nullptr) {
            return;
        }

        bool resolved = true;
        auto resolve = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(lookUpPyrowaveSymbol(library, name));
            if (fn == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "PyroWave: missing entry point %s",
                             name);
                resolved = false;
            }
        };

        resolve(api.getApiVersion, "pyrowave_get_api_version");
        resolve(api.createDeviceByCompat2, "pyrowave_create_device_by_compat2");
        resolve(api.deviceDestroy, "pyrowave_device_destroy");
        resolve(api.syncObjectCreate, "pyrowave_sync_object_create");
        resolve(api.syncObjectGetSemaphore, "pyrowave_sync_object_get_semaphore");
        resolve(api.syncObjectExportHandle, "pyrowave_sync_object_export_handle");
        resolve(api.syncObjectCpuWait, "pyrowave_sync_object_cpu_wait");
        resolve(api.syncObjectCpuSignal, "pyrowave_sync_object_cpu_signal");
        resolve(api.syncObjectDestroy, "pyrowave_sync_object_destroy");
        resolve(api.imageCreate, "pyrowave_image_create");
        resolve(api.imageGetImageView, "pyrowave_image_get_image_view");
        resolve(api.imageDestroy, "pyrowave_image_destroy");
        resolve(api.decoderCreate, "pyrowave_decoder_create");
        resolve(api.decoderClear, "pyrowave_decoder_clear");
        resolve(api.decoderPushPacket, "pyrowave_decoder_push_packet");
        resolve(api.decoderDecodeIsReady, "pyrowave_decoder_decode_is_ready");
        resolve(api.decoderDecodeGpuBuffer, "pyrowave_decoder_decode_gpu_buffer");
        resolve(api.decoderDestroy, "pyrowave_decoder_destroy");
        if (!resolved) {
            return;
        }

        // Optional: without it, frames cut short at their deadline need as much of their
        // data as frames that lost packets
        api.decoderDecodeIsReadyWithSideband = reinterpret_cast<decltype(api.decoderDecodeIsReadyWithSideband)>(
                    lookUpPyrowaveSymbol(library, "pyrowave_decoder_decode_is_ready_with_sideband"));

        // Optional: only used to log what the driver supports when interop fails
        api.deviceGetVkDeviceHandles = reinterpret_cast<decltype(api.deviceGetVkDeviceHandles)>(
                    lookUpPyrowaveSymbol(library, "pyrowave_device_get_vk_device_handles"));

        // The API and ABI can change between minor versions until 1.0
        uint32_t major, minor, patch;
        api.getApiVersion(&major, &minor, &patch);
        if (major != PYROWAVE_API_VERSION_MAJOR || minor != PYROWAVE_API_VERSION_MINOR) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: library version %u.%u.%u doesn't match the %u.%u API Moonlight was built with",
                         major, minor, patch,
                         PYROWAVE_API_VERSION_MAJOR, PYROWAVE_API_VERSION_MINOR);
            return;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: loaded library version %u.%u.%u",
                    major, minor, patch);
        loaded = true;
    });

    return loaded ? &api : nullptr;
}

PyrowaveDecoder::PyrowaveDecoder()
    : m_Api(nullptr),
      m_Handles(new PyrowaveHandles()),
      m_Width(0),
      m_Height(0),
      m_TenBit(false),
      m_Yuv444(false),
      m_Colorspace(COLORSPACE_REC_601),
      m_HdrMode(false),
      m_LoggedDecodeFailure(false)
{
}

PyrowaveDecoder::~PyrowaveDecoder()
{
    // Subclasses have destroyed the decoder and everything else on the device by now
    destroyDecoder();
    if (m_Api != nullptr && m_Handles->device != nullptr) {
        m_Api->deviceDestroy(m_Handles->device);
    }

    delete m_Handles;
}

bool PyrowaveDecoder::initializeStream(PDECODER_PARAMETERS params, int colorspace)
{
    m_Api = loadPyrowaveApi();
    if (m_Api == nullptr) {
        return false;
    }

    m_Width = params->width;
    m_Height = params->height;
    m_TenBit = (params->videoFormat & VIDEO_FORMAT_MASK_10BIT) != 0;
    m_Yuv444 = (params->videoFormat & VIDEO_FORMAT_MASK_YUV444) != 0;
    m_Colorspace = colorspace;

    if (!m_Yuv444 && ((m_Width & 1) || (m_Height & 1))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: 4:2:0 needs an even width and height, not %dx%d",
                     m_Width, m_Height);
        return false;
    }

    return true;
}

bool PyrowaveDecoder::createDecoder()
{
    pyrowave_decoder_create_info decoderInfo = {};
    decoderInfo.device = m_Handles->device;
    decoderInfo.width = m_Width;
    decoderInfo.height = m_Height;
    decoderInfo.chroma = m_Yuv444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    decoderInfo.fragment_path = false;
    pyrowave_result result = m_Api->decoderCreate(&decoderInfo, &m_Handles->decoder);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: couldn't create the decoder (%d)",
                     result);
        return false;
    }

    return true;
}

void PyrowaveDecoder::destroyDecoder()
{
    // Waits for the GPU to be done with it
    if (m_Api != nullptr && m_Handles->decoder != nullptr) {
        m_Api->decoderDestroy(m_Handles->decoder);
        m_Handles->decoder = nullptr;
    }
}

void PyrowaveDecoder::setFrameColorProperties(AVFrame* frame)
{
    // The host converts to YCbCr with its usual shaders, with left-sited chroma for
    // 4:2:0, in the range we ask for, which is always full range for PyroWave
    frame->color_range = AVCOL_RANGE_JPEG;
    frame->chroma_location = m_Yuv444 ? AVCHROMA_LOC_UNSPECIFIED : AVCHROMA_LOC_LEFT;

    if (m_TenBit && m_HdrMode) {
        // The host display is in HDR mode: Rec. 2020 with the PQ transfer
        frame->color_primaries = AVCOL_PRI_BT2020;
        frame->color_trc = AVCOL_TRC_SMPTE2084;
        frame->colorspace = AVCOL_SPC_BT2020_NCL;
        return;
    }

    // SDR, in 8 or 10 bits, in the colorspace we asked for
    switch (m_Colorspace) {
    case COLORSPACE_REC_709:
        frame->color_primaries = AVCOL_PRI_BT709;
        frame->color_trc = AVCOL_TRC_BT709;
        frame->colorspace = AVCOL_SPC_BT709;
        break;
    case COLORSPACE_REC_2020:
        frame->color_primaries = AVCOL_PRI_BT2020;
        frame->color_trc = m_TenBit ? AVCOL_TRC_BT2020_10 : AVCOL_TRC_BT709;
        frame->colorspace = AVCOL_SPC_BT2020_NCL;
        break;
    case COLORSPACE_REC_601:
    default:
        frame->color_primaries = AVCOL_PRI_SMPTE170M;
        frame->color_trc = AVCOL_TRC_SMPTE170M;
        frame->colorspace = AVCOL_SPC_SMPTE170M;
        break;
    }
}

void PyrowaveDecoder::setHdrMode(bool enabled)
{
    m_HdrMode = enabled;
}

// The length of data up to the end of its last complete block. A frame cut short ends
// wherever its data stopped arriving, usually partway into a block, and PyroWave rejects
// the whole of data if its last block is incomplete. Every block, and the sequence header,
// starts with 8 bytes whose second 16-bit word holds the block's length in 32-bit words in
// its low 12 bits, and in its top bit whether it is a sequence header, which is 8 bytes long.
// A block's index is in the top 24 bits of its second 32-bit word. lastBlockIndex is the
// index of the last complete block, or -1 if there is none.
static size_t completeBlocksLength(const uint8_t* data, size_t length, int64_t* lastBlockIndex)
{
    size_t offset = 0;

    *lastBlockIndex = -1;
    while (length - offset >= 8) {
        uint16_t bits;
        memcpy(&bits, data + offset + 2, sizeof(bits));

        bool sequenceHeader = (bits & 0x8000) != 0;
        size_t size = sequenceHeader ? 8 : (size_t)(bits & 0xFFF) * 4;
        if (size < 8 || size > length - offset) {
            break;
        }
        if (!sequenceHeader) {
            uint32_t word;
            memcpy(&word, data + offset + 4, sizeof(word));
            *lastBlockIndex = word >> 8;
        }
        offset += size;
    }

    return offset;
}

AVFrame* PyrowaveDecoder::decode(const uint8_t* data, size_t length, PartialFrame partial)
{
    pyrowave_result result;
    int64_t lastBlockIndex = -1;

    // Each frame arrives on its own, so anything left from an earlier one is stale
    m_Api->decoderClear(m_Handles->decoder);

    if (partial != PartialFrame::Complete) {
        length = completeBlocksLength(data, length, &lastBlockIndex);
        if (length == 0) {
            return nullptr;
        }
    }

    result = m_Api->decoderPushPacket(m_Handles->decoder, data, length);
    if (result != PYROWAVE_SUCCESS) {
        if (!m_LoggedDecodeFailure) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: invalid frame (%d)",
                         result);
            m_LoggedDecodeFailure = true;
        }
        return nullptr;
    }

    if (!m_Api->decoderDecodeIsReady(m_Handles->decoder, false)) {
        // Missing blocks decode as zero (a little blur), which beats dropping the frame.
        // PyroWave's own test wants the two coarsest bands complete and 90% of the blocks.
        // A frame cut short at its deadline is missing the finest detail at the end of
        // the frame, which is most of the blocks, and was only cut with most of its data
        // here (see LiSetPartialFrameDeadline()). Dropping it would leave the frame before
        // on screen for another frame, worse than the late frame we cut it short to avoid,
        // so it only needs the coarse bands.
        //
        // PyroWave's test takes any block of those bands that it doesn't have as missing,
        // but the encoder leaves out blocks with nothing in them, which flat or dark parts
        // of a frame have even in its coarse bands. A partial frame is what arrived up to
        // its first gap, and blocks are sent in index order, coarsest first, so every block
        // up to the last one here that isn't here was left out on purpose. Only the blocks
        // after it are missing, which the mask says.
        bool ready;
        if (partial != PartialFrame::Complete && m_Api->decoderDecodeIsReadyWithSideband != nullptr) {
            size_t wordCount = (size_t)(lastBlockIndex + 1) / 32 + 1;
            std::vector<uint32_t> expectedBlocks(wordCount, 0);
            expectedBlocks[wordCount - 1] = ~0u << ((lastBlockIndex + 1) % 32);

            ready = m_Api->decoderDecodeIsReadyWithSideband(m_Handles->decoder, true, 2,
                                                            partial == PartialFrame::Late ? 0.0f : 0.9f,
                                                            expectedBlocks.data(), wordCount);
        }
        else {
            ready = m_Api->decoderDecodeIsReady(m_Handles->decoder, true);
        }
        if (!ready) {
            return nullptr;
        }

        if (partial != PartialFrame::Late && !m_LoggedDecodeFailure) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "PyroWave: decoding an incomplete frame");
            m_LoggedDecodeFailure = true;
        }
    }

    AVFrame* frame = decodePushedFrame();
    if (frame != nullptr) {
        setFrameColorProperties(frame);
    }
    return frame;
}
