#pragma once

#include "pyrowavedecoder.h"
#include "streaming/video/ffmpeg-renderers/plvk.h"

#include <mutex>
#include <vector>

struct PyrowaveVkState;
struct PyrowaveVkSlot;

// Decodes PyroWave frames into images PlVkRenderer draws.
//
// PyroWave decodes on its own Vulkan device, into Y, Cb and Cr plane images made on the
// renderer's device and shared with PyroWave's through opaque fd memory. The renderer
// draws the three planes as they are, so unlike with D3D11 nothing is packed. A pool of
// plane sets ("slots") lets the pacer hold frames while the next one is decoded.
//
// Two timeline semaphores, made on the renderer's device and shared the same way, order
// the two devices: PyroWave signals one as it finishes decoding into a slot, and the
// renderer waits for that before drawing it. The renderer signals the other once it has
// drawn a slot, and PyroWave waits for that before decoding into it again. Each has only
// the one device signalling it, so its values always rise in the order they are signalled.
class PyrowaveVkDecoder : public PyrowaveDecoder, public IPlVkFrameSource
{
public:
    PyrowaveVkDecoder();
    virtual ~PyrowaveVkDecoder() override;

    // The renderer must be initialized, with PlVkRenderer::prepareForExternalFrames()
    // called before that
    bool initialize(PlVkRenderer* renderer, PDECODER_PARAMETERS params, int colorspace);

    // IPlVkFrameSource, called on the thread that renders
    virtual bool mapFrame(const AVFrame* frame, pl_frame* mappedFrame) override;
    virtual void unmapFrame(const AVFrame* frame) override;
    virtual uint64_t captureDecodeBoundary(AVFrame* frame) override;
    virtual bool waitForDecode(AVFrame* frame) override;

protected:
    virtual AVFrame* decodePushedFrame() override;

private:
    bool loadVulkanFunctions();
    bool createPyrowaveDevice();
    bool createSemaphores();
    bool createSlot(PyrowaveVkSlot* slot);
    bool createPlane(PyrowaveVkSlot* slot, int plane);
    void destroySlot(PyrowaveVkSlot* slot);
    static void releaseSlot(void* opaque, uint8_t* data);

    PlVkRenderer* m_Renderer;
    PyrowaveVkState* m_State;

    // Slots no frame is using, by index. Frames are freed on whichever thread lets go
    // of them last.
    std::mutex m_FreeSlotsLock;
    std::vector<int> m_FreeSlots;

    bool m_LoggedNoFreeSlot;
    bool m_LoggedHoldFailure;
};
