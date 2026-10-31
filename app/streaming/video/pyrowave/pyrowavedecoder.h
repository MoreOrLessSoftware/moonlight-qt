#pragma once

#include "streaming/video/decoder.h"

#include <atomic>
#include <cstddef>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

struct PyrowaveApi;
struct PyrowaveHandles;
class PyrowaveStageTimer;

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
    // packets that were lost, or the start of it, cut short at its deadline. Not "None",
    // which X11's headers define as a macro on Linux.
    enum class PartialFrame {
        Complete,
        Lost,
        Late,
    };

    // A stretch of a partial frame that didn't arrive
    struct Gap
    {
        size_t offset;
        size_t length;
    };

    // Decodes a frame. A partial frame comes with its gaps, in order, and in record
    // framing (DECODE_UNIT.partialRecords) with the offsets where the host flagged a record
    // as starting, in order, otherwise null. Its records needn't be in block order then, and
    // whatever arrived whole is decoded. Otherwise its blocks are in order, and only what
    // arrived up to its first gap is decoded, since the data after one starts partway into
    // a block. Returns a frame for the renderer, or nullptr if the frame couldn't be decoded.
    AVFrame* decode(const uint8_t* data, size_t length, PartialFrame partial,
                    const std::vector<Gap>& gaps, const std::vector<size_t>* recordStarts);

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
    // decoded into, and the stage timer. Subclasses call this before destroying the
    // images and sync objects they used.
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

    // Null unless ML_PYROWAVE_STAGE_LOG=1. Subclasses create it, and tell it when they
    // submit the GPU work for each frame.
    PyrowaveStageTimer* m_StageTimer;

    bool m_LoggedDecodeFailure;
};
