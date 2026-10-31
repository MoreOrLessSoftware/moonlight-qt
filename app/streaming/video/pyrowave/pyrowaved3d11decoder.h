#pragma once

#include "pyrowavedecoder.h"

#include <array>
#include <mutex>
#include <vector>

#include <d3d11_4.h>
#include <wrl/client.h>

class D3D11VARenderer;
struct PyrowaveD3D11Handles;

// Decodes PyroWave frames into D3D11 textures that D3D11VARenderer draws directly.
//
// Each frame is decoded into a set of Y, Cb and Cr plane textures shared with D3D11, one
// of a few sets so the pacer can hold on to its outstanding frames while another is
// decoded. The renderer draws a frame straight from its planes. A shared fence orders
// the drawing after PyroWave's decode, and a D3D11 fence says when the renderer is done
// with a set so it can be decoded into again.
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
    struct PlaneSet {
        int index;
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 3> textures;

        // m_ReleaseFence's value once the renderer is done with the planes. Written when
        // the frame holding them is freed, and read on the decoder thread after it takes
        // the set from the free list.
        uint64_t releasedValue;
    };

    bool createSync();
    bool testSharedFence();
    bool createPlaneSets();
    void lockContext();
    void unlockContext();

    static void releaseSet(void* opaque, uint8_t* data);

    PyrowaveD3D11Handles* m_D3D11Handles;

    // The renderer's decode device
    Microsoft::WRL::ComPtr<ID3D11Device5> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_DeviceContext;
    void (*m_Lock)(void*);
    void (*m_Unlock)(void*);
    void* m_LockContext;

    std::vector<PlaneSet> m_PlaneSets;
    std::mutex m_FreeSetsLock;
    std::vector<int> m_FreeSets;
    bool m_LoggedNoFreeSet;

    // PyroWave signals this when it has decoded a frame, for D3D11 to wait on before
    // drawing it. Null with CPU sync. See createSync().
    Microsoft::WRL::ComPtr<ID3D11Fence> m_Fence;
    uint64_t m_FenceValue;

    // Without a fence shared with PyroWave, the decoder thread waits for PyroWave to
    // finish each decode on the CPU instead
    bool m_CpuSync;

    // Signalled on the decode device's context when a frame's planes are released, which
    // is after the renderer's last drawing from them. The decoder thread waits for it on
    // the CPU before decoding into them again, which it rarely has to.
    Microsoft::WRL::ComPtr<ID3D11Fence> m_ReleaseFence;
    uint64_t m_ReleaseValue;
    HANDLE m_ReleaseEvent;
};
