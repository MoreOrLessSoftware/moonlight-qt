#pragma once

#include "streaming/video/decoder.h"

#include <atomic>
#include <array>
#include <vector>

#include <d3d11_4.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
}

class D3D11VARenderer;
struct PyrowaveApi;
struct PyrowaveHandles;

// Decodes PyroWave frames into D3D11 textures that D3D11VARenderer draws like hardware
// decoded ones.
//
// PyroWave decodes in Vulkan compute on the renderer's GPU, into three plane textures
// shared with D3D11. A small D3D11 pass then packs them into a frame from a pool the
// renderer made: NV12 or P010 for 4:2:0. For 4:4:4, AYUV-ordered YUV in an ordinary
// BGRA or R10G10B10A2 texture, since some GPUs (AMD's) have no AYUV/Y410 textures. A
// shared fence orders the two APIs.
//
// The library (libpyrowave-shared-0.dll) is loaded at runtime, so nothing links against
// it and the codec is simply unavailable without it.
class PyrowaveDecoder
{
public:
    PyrowaveDecoder();
    ~PyrowaveDecoder();

    bool initialize(D3D11VARenderer* renderer, PDECODER_PARAMETERS params, int colorspace);

    // How much of a frame arrived (see CAPABILITY_PARTIAL_FRAMES): all of it, all but
    // packets that were lost, or the start of it, cut short at its deadline
    enum class PartialFrame {
        None,
        Lost,
        Late,
    };

    // Decodes a frame. A partial frame's data is what arrived up to the first gap.
    // Returns a frame from the renderer's pool, or nullptr if the frame couldn't be
    // decoded.
    AVFrame* decode(const uint8_t* data, size_t length, PartialFrame partial);

    // The host switched to HDR (PQ) or back to SDR
    void setHdrMode(bool enabled);

    // Submits work queued on the decode context, such as a fence signal after decode()
    void flush();

private:
    bool createSync();
    bool testSharedFence();
    bool createPlanes();
    bool createPackResources();
    bool createPack444Resources();
    void setFrameColorProperties(AVFrame* frame);
    void lockContext();
    void unlockContext();

    const PyrowaveApi* m_Api;
    PyrowaveHandles* m_Handles;

    int m_Width;
    int m_Height;
    bool m_TenBit;
    bool m_Yuv444;
    int m_Colorspace;
    std::atomic<bool> m_HdrMode;

    // The renderer's decode device, and the pool of frames it renders
    AVBufferRef* m_FramesContext;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_DeviceContext;
    void (*m_Lock)(void*);
    void (*m_Unlock)(void*);
    void* m_LockContext;

    // Y, Cb and Cr planes PyroWave decodes into
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 3> m_PlaneTextures;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 3> m_PlaneViews;

    // Ordering between PyroWave and D3D11: PyroWave signals when a frame is decoded, and
    // D3D11 when it has packed it (so the planes can be decoded into again)
    Microsoft::WRL::ComPtr<ID3D11Fence> m_Fence;
    uint64_t m_FenceValue;
    uint64_t m_PackedValue;

    // Without a fence shared with PyroWave, each side waits for the other's signal on
    // the CPU instead. m_Fence is then D3D11's alone. See createSync().
    bool m_CpuSync;
    HANDLE m_FenceEvent;

    // Packing the planes into a frame, with its own pipeline state so the renderer's
    // state on a shared device context isn't disturbed
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> m_PackState;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_PackVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_PackLumaShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_PackChromaShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_Pack444Shader;

    // The planes are packed into this texture, then copied into a frame from the pool.
    // D3D11 doesn't allow NV12/P010 texture arrays to be render targets. 4:4:4 uses
    // just the first target.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_PackTexture;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, 2> m_PackTargets;

    bool m_LoggedDecodeFailure;
};
