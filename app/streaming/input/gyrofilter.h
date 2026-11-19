#pragma once

#include <cstdint>

// Gyro and accelerometer filtering for one gamepad. Off unless it's turned on for that gamepad in
// its gamepad settings (see GamepadSettings); while off, readings pass through unchanged.
//
// Many gyros (cheap clones especially) read a steady offset at rest, which turns into constant
// camera drift, plus a few deg/s of jitter. An 8BitDo Switch Pro clone measured 7 deg/s of offset
// and 1.5 deg/s of noise per axis, with blips past 6 deg/s, and its accelerometer read 2 g at rest.
// Steam's gyro calibration on the host won't complete with readings like that. The stages:
//
// - Offset removal: once the gyro (and accelerometer, if enabled) has held still for a whole
//   window, the window's average gyro reading becomes the offset subtracted from every reading.
//   Turning left or right while level leaves the accelerometer unchanged, so a slow, steady turn
//   can pass as still: once there's an offset, a still window may only nudge it, and a bigger
//   change takes several such windows in a row that agree with each other.
// - Smoothing of slow movement: below smoothBelowDps the reading is the average of the last
//   smoothSamples readings, which cuts the noise; from there to twice that it blends into the raw
//   reading, so fast movement gets no added latency.
// - Soft deadzone: below deadzoneDps the output fades in proportion to its size instead of
//   cutting off, so what's left of the jitter is damped but slow aiming still moves.
// - Accelerometer correction: scaled so a still window reads 1 g, and smoothed with a short
//   moving average (it's used for gravity and tilt, not aiming, so the delay costs little).
//
// Plain data with no constructor, so it can live in GamepadState, which is cleared with memset.
// A cleared GyroFilter is off.

struct GyroFilterSettings
{
    bool enabled;
    bool removeOffset;
    int smoothSamples;      // readings averaged; 1 = no smoothing
    int smoothBelowDps;     // movement slower than this is smoothed; 0 = no smoothing
    int deadzoneDps;        // 0 = no deadzone
    bool correctAccel;

    // What a gamepad starts with: off, with the settings it gets when it's turned on
    static GyroFilterSettings defaults();
};

class GyroFilter
{
public:
    static const int MaxSmoothSamples = 32;

    // Clears everything (including the offset) and applies new settings
    void reset(const GyroFilterSettings& settings);

    // New settings, keeping the offset and scale found so far (for adjusting while testing)
    void applySettings(const GyroFilterSettings& settings);

    // The host turned the sensors back on: keep the offset and scale, but don't let a stillness
    // window or the smoothing span the gap
    void restart();

    // Gamepad index for log lines about the offset and scale; -1 (the default after reset) = no logging
    void setLogIndex(int index) { m_LogIndex = index; }

    // One gyro reading (rad/s) in, the reading to send (deg/s) out. Every reading should go
    // through here, including ones that won't be sent, since they all feed the calibration.
    void processGyro(uint32_t timestampMs, const float radPerSec[3], float outDps[3]);

    // One accelerometer reading (m/s^2) in, the reading to send out
    void processAccel(const float in[3], float out[3]);

    const GyroFilterSettings& settings() const { return m_Settings; }
    bool hasOffset() const { return m_BiasValid; }
    void offsetDps(float out[3]) const;
    bool hasAccelScale() const { return m_AccelScaleValid; }
    float accelScale() const { return m_AccelScale; }

private:
    void updateCalibration(uint32_t timestampMs, const float radPerSec[3]);
    void finishWindow();
    void startWindow(uint32_t timestampMs);

    GyroFilterSettings m_Settings;
    int m_LogIndex;

    float m_Bias[3];                // rad/s
    bool m_BiasValid;
    float m_LoggedBias[3];
    float m_CandidateBias[3];       // still windows too far from the bias to nudge it
    int m_CandidateCount;

    float m_AccelScale;
    bool m_AccelScaleValid;
    float m_LoggedAccelScale;
    float m_AccelSmoothed[3];
    bool m_AccelSmoothedValid;

    // The stillness window being collected
    uint32_t m_WindowStart;         // ms; 0 = start a new window on the next reading
    int m_GyroSamples;
    double m_GyroSum[3], m_GyroSumSq[3];
    int m_AccelSamples;
    double m_AccelSum[3], m_AccelSumSq[3];

    // Recent offset-corrected readings (deg/s) for smoothing
    float m_History[MaxSmoothSamples][3];
    int m_HistoryCount;
    int m_HistoryNext;
};
