#pragma once

#include "streaming/video/decoder.h"

#include <atomic>

extern "C" {
#include <libavutil/frame.h>
}

struct PyrowaveApi;
struct PyrowaveHandles;

// Decodes PyroWave frames on the GPU, into frames the renderer draws like hardware
// decoded ones.
//
// PyroWave decodes in Vulkan compute, on its own Vulkan device on the renderer's GPU.
// This class handles the library and the bitstream. Its subclasses decode into frames
// the renderer can draw: PyrowaveD3D11Decoder for the D3D11 renderer on Windows, and
// PyrowaveVkDecoder for the Vulkan renderer elsewhere.
//
// The library (libpyrowave-shared-0.dll, libpyrowave-shared.so.0) is loaded at runtime,
// so nothing links against it and the codec is simply unavailable without it.
class PyrowaveDecoder
{
public:
    virtual ~PyrowaveDecoder();

    // How much of a frame arrived (see CAPABILITY_PARTIAL_FRAMES): all of it, all but
    // packets that were lost, or the start of it, cut short at its deadline
    enum class PartialFrame {
        None,
        Lost,
        Late,
    };

    // Decodes a frame. A partial frame's data is what arrived up to the first gap.
    // Returns a frame for the renderer, or nullptr if the frame couldn't be decoded.
    AVFrame* decode(const uint8_t* data, size_t length, PartialFrame partial);

    // The host switched to HDR (PQ) or back to SDR
    void setHdrMode(bool enabled);

    // Submits work queued for the renderer's device, such as a fence signal after
    // decode()
    virtual void flush() {}

protected:
    PyrowaveDecoder();

    // Loads the library and takes the stream's format. Returns false if PyroWave can't
    // decode it.
    bool initializeStream(PDECODER_PARAMETERS params, int colorspace);

    // Creates the decoder once the subclass has created m_Handles->device
    bool createDecoder();

    // Destroys the decoder, which waits for the GPU to finish with everything it
    // decoded into. Subclasses call this before destroying the images it used.
    void destroyDecoder();

    // Decodes the frame whose data decode() pushed, into a new frame for the renderer.
    // Returns nullptr on failure.
    virtual AVFrame* decodePushedFrame() = 0;

    void setFrameColorProperties(AVFrame* frame);

    const PyrowaveApi* m_Api;
    PyrowaveHandles* m_Handles;

    int m_Width;
    int m_Height;
    bool m_TenBit;
    bool m_Yuv444;
    int m_Colorspace;
    std::atomic<bool> m_HdrMode;

    bool m_LoggedDecodeFailure;
};
