#include "pyrowavedecoder.h"

#include "path.h"
#include "streaming/video/ffmpeg-renderers/d3d11va.h"
#include "streaming/video/ffmpeg-renderers/pacer/pacer.h"

#include <mutex>
#include <vector>

#include <vulkan/vulkan_core.h>
#include "pyrowave.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

using Microsoft::WRL::ComPtr;

// Entry points of the PyroWave library, resolved at runtime
struct PyrowaveApi
{
    decltype(&::pyrowave_get_api_version) getApiVersion;
    decltype(&::pyrowave_create_device_by_compat2) createDeviceByCompat2;
    decltype(&::pyrowave_device_destroy) deviceDestroy;
    decltype(&::pyrowave_sync_object_create) syncObjectCreate;
    decltype(&::pyrowave_sync_object_get_semaphore) syncObjectGetSemaphore;
    decltype(&::pyrowave_sync_object_destroy) syncObjectDestroy;
    decltype(&::pyrowave_image_create) imageCreate;
    decltype(&::pyrowave_image_get_image_view) imageGetImageView;
    decltype(&::pyrowave_image_destroy) imageDestroy;
    decltype(&::pyrowave_decoder_create) decoderCreate;
    decltype(&::pyrowave_decoder_clear) decoderClear;
    decltype(&::pyrowave_decoder_push_packet) decoderPushPacket;
    decltype(&::pyrowave_decoder_decode_is_ready) decoderDecodeIsReady;
    decltype(&::pyrowave_decoder_decode_is_ready_with_sideband) decoderDecodeIsReadyWithSideband; // May be null
    decltype(&::pyrowave_decoder_decode_gpu_buffer) decoderDecodeGpuBuffer;
    decltype(&::pyrowave_decoder_destroy) decoderDestroy;
};

// PyroWave objects owned by the decoder
struct PyrowaveHandles
{
    pyrowave_device device = nullptr;
    pyrowave_sync_object sync = nullptr;
    std::array<pyrowave_image, 3> planes = {};
    pyrowave_decoder decoder = nullptr;
    pyrowave_gpu_buffers buffers = {};
};

static const PyrowaveApi* loadPyrowaveApi()
{
    static std::once_flag once;
    static PyrowaveApi api = {};
    static bool loaded = false;

    std::call_once(once, []() {
        // Kept loaded for the life of the process
        HMODULE dll = LoadLibraryExW(L"libpyrowave-shared-0.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (dll == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "PyroWave: libpyrowave-shared-0.dll not found, the codec is unavailable");
            return;
        }

        bool resolved = true;
        auto resolve = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll, name));
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
                    GetProcAddress(dll, "pyrowave_decoder_decode_is_ready_with_sideband"));

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
      m_Colorspace(COLORSPACE_REC_601),
      m_HdrMode(false),
      m_FramesContext(nullptr),
      m_Lock(nullptr),
      m_Unlock(nullptr),
      m_LockContext(nullptr),
      m_FenceValue(0),
      m_PackedValue(0),
      m_LoggedDecodeFailure(false)
{
}

PyrowaveDecoder::~PyrowaveDecoder()
{
    if (m_Api != nullptr) {
        // Each of these waits for the GPU to be done with it
        if (m_Handles->decoder != nullptr) {
            m_Api->decoderDestroy(m_Handles->decoder);
        }
        for (pyrowave_image plane : m_Handles->planes) {
            if (plane != nullptr) {
                m_Api->imageDestroy(plane);
            }
        }
        if (m_Handles->sync != nullptr) {
            m_Api->syncObjectDestroy(m_Handles->sync);
        }
        if (m_Handles->device != nullptr) {
            m_Api->deviceDestroy(m_Handles->device);
        }
    }

    delete m_Handles;

    av_buffer_unref(&m_FramesContext);
}

void PyrowaveDecoder::lockContext()
{
    m_Lock(m_LockContext);
}

void PyrowaveDecoder::unlockContext()
{
    m_Unlock(m_LockContext);
}

bool PyrowaveDecoder::initialize(D3D11VARenderer* renderer, PDECODER_PARAMETERS params, int colorspace)
{
    HRESULT hr;

    m_Api = loadPyrowaveApi();
    if (m_Api == nullptr) {
        return false;
    }

    m_Width = params->width;
    m_Height = params->height;
    m_TenBit = (params->videoFormat & VIDEO_FORMAT_PYROWAVE_10BIT) != 0;
    m_Colorspace = colorspace;

    if ((m_Width & 1) || (m_Height & 1)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: 4:2:0 needs an even width and height, not %dx%d",
                     m_Width, m_Height);
        return false;
    }

    // The pool of frames the renderer draws, on its decode device. The pacer can hold
    // on to all of its outstanding frames while we pack another. NV12/P010 texture
    // arrays must be decoder outputs, like a hardware decoder's pool.
    m_FramesContext = renderer->createFramesContext(m_TenBit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12,
                                                    m_Width, m_Height,
                                                    PACER_MAX_OUTSTANDING_FRAMES + 2,
                                                    D3D11_BIND_DECODER);
    if (m_FramesContext == nullptr) {
        return false;
    }

    {
        auto framesContext = (AVHWFramesContext*)m_FramesContext->data;
        auto d3d11vaDeviceContext = (AVD3D11VADeviceContext*)framesContext->device_ctx->hwctx;

        hr = d3d11vaDeviceContext->device->QueryInterface(IID_PPV_ARGS(&m_Device));
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: ID3D11Device::QueryInterface(ID3D11Device5) failed: %x",
                         hr);
            return false;
        }

        hr = d3d11vaDeviceContext->device_context->QueryInterface(IID_PPV_ARGS(&m_DeviceContext));
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: ID3D11DeviceContext::QueryInterface(ID3D11DeviceContext4) failed: %x",
                         hr);
            return false;
        }

        m_Lock = d3d11vaDeviceContext->lock;
        m_Unlock = d3d11vaDeviceContext->unlock;
        m_LockContext = d3d11vaDeviceContext->lock_ctx;
    }

    // Decode on the same GPU in Vulkan
    DXGI_ADAPTER_DESC adapterDesc;
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        hr = m_Device.As(&dxgiDevice);
        if (SUCCEEDED(hr)) {
            hr = dxgiDevice->GetAdapter(&adapter);
        }
        if (SUCCEEDED(hr)) {
            hr = adapter->GetDesc(&adapterDesc);
        }
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: couldn't find the D3D11 device's adapter: %x",
                         hr);
            return false;
        }
    }

    static_assert(sizeof(LUID) == sizeof(pyrowave_luid), "LUID size mismatch");
    pyrowave_luid luid;
    memcpy(luid.luid, &adapterDesc.AdapterLuid, sizeof(luid.luid));

    // High priority puts PyroWave on an async compute queue. Without the privilege for
    // it, the driver gives a lower one.
    pyrowave_result result = m_Api->createDeviceByCompat2(0, 0, nullptr, nullptr, &luid,
                                                          VK_QUEUE_GLOBAL_PRIORITY_HIGH,
                                                          &m_Handles->device);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: couldn't create a Vulkan device for %S (%d). A Vulkan 1.3 driver is required.",
                     adapterDesc.Description,
                     result);
        return false;
    }

    // Shared with PyroWave to order access to the planes
    hr = m_Device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_Fence));
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: ID3D11Device5::CreateFence() failed: %x",
                     hr);
        return false;
    }

    HANDLE fenceHandle;
    hr = m_Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: ID3D11Fence::CreateSharedHandle() failed: %x",
                     hr);
        return false;
    }

    pyrowave_sync_object_create_info syncInfo = {};
    syncInfo.device = m_Handles->device;
    syncInfo.external_handle = (pyrowave_os_handle)fenceHandle;
    syncInfo.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    syncInfo.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
    result = m_Api->syncObjectCreate(&syncInfo, &m_Handles->sync);
    if (result != PYROWAVE_SUCCESS) {
        CloseHandle(fenceHandle);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: couldn't import the shared fence (%d)",
                     result);
        return false;
    }

    if (!createPlanes(m_Width, m_Height, m_TenBit) || !createPackResources()) {
        return false;
    }

    pyrowave_decoder_create_info decoderInfo = {};
    decoderInfo.device = m_Handles->device;
    decoderInfo.width = m_Width;
    decoderInfo.height = m_Height;
    decoderInfo.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    decoderInfo.fragment_path = false;
    result = m_Api->decoderCreate(&decoderInfo, &m_Handles->decoder);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: couldn't create the decoder (%d)",
                     result);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave: decoding %dx%d %s 4:2:0 on %S",
                m_Width, m_Height,
                m_TenBit ? "10-bit" : "8-bit",
                adapterDesc.Description);
    return true;
}

bool PyrowaveDecoder::createPlanes(int width, int height, bool tenBit)
{
    for (int i = 0; i < 3; i++) {
        HRESULT hr;

        // Cb and Cr are half size for 4:2:0
        int planeWidth = i == 0 ? width : width / 2;
        int planeHeight = i == 0 ? height : height / 2;

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = planeWidth;
        texDesc.Height = planeHeight;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        // PyroWave writes these as storage images
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        hr = m_Device->CreateTexture2D(&texDesc, nullptr, &m_PlaneTextures[i]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: ID3D11Device::CreateTexture2D() failed: %x",
                         hr);
            return false;
        }

        hr = m_Device->CreateShaderResourceView(m_PlaneTextures[i].Get(), nullptr, &m_PlaneViews[i]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: ID3D11Device::CreateShaderResourceView() failed: %x",
                         hr);
            return false;
        }

        ComPtr<IDXGIResource1> resource;
        HANDLE textureHandle;
        hr = m_PlaneTextures[i].As(&resource);
        if (SUCCEEDED(hr)) {
            hr = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &textureHandle);
        }
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: IDXGIResource1::CreateSharedHandle() failed: %x",
                         hr);
            return false;
        }

        // Must match the D3D11 texture closely enough for the driver to import it
        VkImageCreateInfo imageCreateInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
        imageCreateInfo.format = tenBit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
        imageCreateInfo.extent = { (uint32_t)planeWidth, (uint32_t)planeHeight, 1 };
        imageCreateInfo.mipLevels = 1;
        imageCreateInfo.arrayLayers = 1;
        imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageCreateInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        pyrowave_image_create_info imageInfo = {};
        imageInfo.device = m_Handles->device;
        imageInfo.external_handle = (pyrowave_os_handle)textureHandle;
        imageInfo.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        imageInfo.image_create_info = &imageCreateInfo;
        pyrowave_result result = m_Api->imageCreate(&imageInfo, &m_Handles->planes[i]);
        if (result != PYROWAVE_SUCCESS) {
            // Whether PyroWave closed the handle on failure isn't specified, so it is left open
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: couldn't import plane %d (%d)",
                         i, result);
            return false;
        }

        result = m_Api->imageGetImageView(m_Handles->planes[i],
                                          VK_IMAGE_ASPECT_COLOR_BIT,
                                          VK_IMAGE_USAGE_STORAGE_BIT,
                                          &m_Handles->buffers.planes[i]);
        if (result != PYROWAVE_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: couldn't view plane %d (%d)",
                         i, result);
            return false;
        }
    }

    return true;
}

bool PyrowaveDecoder::createPackResources()
{
    HRESULT hr;

    ComPtr<ID3D11Device1> device1;
    hr = m_Device.As(&device1);
    if (SUCCEEDED(hr)) {
        D3D_FEATURE_LEVEL featureLevel = m_Device->GetFeatureLevel();
        hr = device1->CreateDeviceContextState(0, &featureLevel, 1, D3D11_SDK_VERSION,
                                               __uuidof(ID3D11Device1), nullptr, &m_PackState);
    }
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: ID3D11Device1::CreateDeviceContextState() failed: %x",
                     hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_Width;
    texDesc.Height = m_Height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = m_TenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = m_Device->CreateTexture2D(&texDesc, nullptr, &m_PackTexture);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: ID3D11Device::CreateTexture2D() failed: %x",
                     hr);
        return false;
    }

    // The luma and chroma planes of an NV12/P010 texture are picked by the view format
    const DXGI_FORMAT planeFormats[2] = {
        m_TenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM,
        m_TenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM,
    };
    for (int plane = 0; plane < 2; plane++) {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = planeFormats[plane];
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = m_Device->CreateRenderTargetView(m_PackTexture.Get(), &rtvDesc, &m_PackTargets[plane]);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: ID3D11Device::CreateRenderTargetView() failed: %x",
                         hr);
            return false;
        }
    }

    QByteArray vertexShaderBytecode = Path::readDataFile("d3d11_pyrowave_vertex.fxc");
    hr = m_Device->CreateVertexShader(vertexShaderBytecode.constData(), vertexShaderBytecode.length(), nullptr, &m_PackVertexShader);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: ID3D11Device::CreateVertexShader() failed: %x",
                     hr);
        return false;
    }

    QByteArray lumaShaderBytecode = Path::readDataFile("d3d11_pyrowave_luma_pixel.fxc");
    hr = m_Device->CreatePixelShader(lumaShaderBytecode.constData(), lumaShaderBytecode.length(), nullptr, &m_PackLumaShader);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: ID3D11Device::CreatePixelShader() failed: %x",
                     hr);
        return false;
    }

    QByteArray chromaShaderBytecode = Path::readDataFile("d3d11_pyrowave_chroma_pixel.fxc");
    hr = m_Device->CreatePixelShader(chromaShaderBytecode.constData(), chromaShaderBytecode.length(), nullptr, &m_PackChromaShader);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: ID3D11Device::CreatePixelShader() failed: %x",
                     hr);
        return false;
    }

    return true;
}

void PyrowaveDecoder::setFrameColorProperties(AVFrame* frame)
{
    // The host converts to YCbCr with its usual shaders, with left-sited chroma, in
    // the range we ask for, which is always full range for PyroWave
    frame->color_range = AVCOL_RANGE_JPEG;
    frame->chroma_location = AVCHROMA_LOC_LEFT;

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

void PyrowaveDecoder::flush()
{
    lockContext();
    m_DeviceContext->Flush();
    unlockContext();
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

    if (partial != PartialFrame::None) {
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
        if (partial != PartialFrame::None && m_Api->decoderDecodeIsReadyWithSideband != nullptr) {
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

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        return nullptr;
    }

    // Fails when every frame in the pool is still queued for rendering
    int err = av_hwframe_get_buffer(m_FramesContext, frame, 0);
    if (err < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: no free frame to decode into: %d",
                    err);
        av_frame_free(&frame);
        return nullptr;
    }

    VkSemaphore semaphore = m_Api->syncObjectGetSemaphore(m_Handles->sync);

    // The planes' old contents can be discarded, but not before D3D11 has packed them
    std::array<pyrowave_gpu_external_reference, 3> acquireRefs, releaseRefs;
    for (int i = 0; i < 3; i++) {
        acquireRefs[i] = { m_Handles->planes[i], VK_QUEUE_FAMILY_IGNORED };
        releaseRefs[i] = { m_Handles->planes[i], VK_QUEUE_FAMILY_EXTERNAL };
    }

    uint64_t decodedValue = ++m_FenceValue;

    pyrowave_gpu_sync_operation acquire = {};
    acquire.images = acquireRefs.data();
    acquire.num_images = acquireRefs.size();
    acquire.sync = { semaphore, m_PackedValue };

    pyrowave_gpu_sync_operation release = {};
    release.images = releaseRefs.data();
    release.num_images = releaseRefs.size();
    release.sync = { semaphore, decodedValue };

    result = m_Api->decoderDecodeGpuBuffer(m_Handles->decoder, &acquire, &release, &m_Handles->buffers);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: decoding failed (%d)",
                     result);
        av_frame_free(&frame);
        return nullptr;
    }

    lockContext();

    ComPtr<ID3DDeviceContextState> previousState;
    m_DeviceContext->SwapDeviceContextState(m_PackState.Get(), &previousState);

    // Wait on the GPU for PyroWave to finish, then pack Y, and Cb with Cr, into the frame
    m_DeviceContext->Wait(m_Fence.Get(), decodedValue);

    m_DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_DeviceContext->VSSetShader(m_PackVertexShader.Get(), nullptr, 0);

    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (float)m_Width, (float)m_Height, 0.0f, 1.0f };
    m_DeviceContext->RSSetViewports(1, &viewport);
    m_DeviceContext->OMSetRenderTargets(1, m_PackTargets[0].GetAddressOf(), nullptr);
    m_DeviceContext->PSSetShader(m_PackLumaShader.Get(), nullptr, 0);
    m_DeviceContext->PSSetShaderResources(0, 1, m_PlaneViews[0].GetAddressOf());
    m_DeviceContext->Draw(3, 0);

    viewport.Width = (float)(m_Width / 2);
    viewport.Height = (float)(m_Height / 2);
    m_DeviceContext->RSSetViewports(1, &viewport);
    m_DeviceContext->OMSetRenderTargets(1, m_PackTargets[1].GetAddressOf(), nullptr);
    m_DeviceContext->PSSetShader(m_PackChromaShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* chromaViews[] = { m_PlaneViews[1].Get(), m_PlaneViews[2].Get() };
    m_DeviceContext->PSSetShaderResources(0, 2, chromaViews);
    m_DeviceContext->Draw(3, 0);

    // Don't keep the planes bound
    ID3D11ShaderResourceView* nullViews[2] = {};
    m_DeviceContext->PSSetShaderResources(0, 2, nullViews);
    m_DeviceContext->OMSetRenderTargets(0, nullptr, nullptr);

    // Into the frame from the pool
    m_DeviceContext->CopySubresourceRegion((ID3D11Texture2D*)frame->data[0], (UINT)(intptr_t)frame->data[1],
                                           0, 0, 0, m_PackTexture.Get(), 0, nullptr);

    // PyroWave decodes the next frame into the planes once they're packed. The signal
    // only reaches the GPU when the context is flushed, which also starts the packing now.
    m_PackedValue = ++m_FenceValue;
    m_DeviceContext->Signal(m_Fence.Get(), m_PackedValue);
    m_DeviceContext->Flush();

    m_DeviceContext->SwapDeviceContextState(previousState.Get(), nullptr);

    unlockContext();

    setFrameColorProperties(frame);
    return frame;
}
