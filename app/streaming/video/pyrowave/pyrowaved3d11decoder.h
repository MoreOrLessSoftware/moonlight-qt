#pragma once

#include "pyrowavedecoder.h"

#include <array>

#include <d3d11_4.h>
#include <wrl/client.h>

class D3D11VARenderer;
struct PyrowaveD3D11Handles;

// Decodes PyroWave frames into D3D11 textures that D3D11VARenderer draws like hardware
// decoded ones.
//
// PyroWave decodes into three plane textures shared with D3D11. A small D3D11 pass then
// packs them into a frame from a pool the renderer made: NV12 or P010 for 4:2:0. For
// 4:4:4, AYUV-ordered YUV in an ordinary BGRA or R10G10B10A2 texture, since some GPUs
// (AMD's) have no AYUV/Y410 textures. A shared fence orders the two APIs.
class PyrowaveD3D11Decoder : public PyrowaveDecoder
{
public:
    PyrowaveD3D11Decoder();
    virtual ~PyrowaveD3D11Decoder() override;

    bool initialize(D3D11VARenderer* renderer, PDECODER_PARAMETERS params, int colorspace);

    virtual void flush() override;

protected:
    virtual AVFrame* decodePushedFrame() override;

private:
    bool createSync();
    bool testSharedFence();
    bool createPlanes();
    bool createPackResources();
    bool createPack444Resources();
    void lockContext();
    void unlockContext();

    PyrowaveD3D11Handles* m_D3D11Handles;

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
};
