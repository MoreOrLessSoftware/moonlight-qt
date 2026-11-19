#include "gyrofilter.h"

#include "SDL_compat.h"

#include <QtGlobal>

#include <cstring>

#define RAD_TO_DEG                  57.2957795
#define STANDARD_GRAVITY            9.80665

#define STILL_WINDOW_MS             1000
#define STILL_MIN_SAMPLES           20      // Bluetooth Switch controllers have reported 36 to 200 per second
#define STILL_MAX_STDDEV_DPS        2.5     // per axis; sensor noise alone is ~1-1.5 deg/s
#define STILL_MAX_OFFSET_DPS        20.0    // a larger average is movement, not an offset
#define ACCEL_STILL_MAX_STDDEV      0.5     // m/s^2, per axis
#define MAX_BIAS_STEP_DPS           1.0     // a still window further from the offset than this is
#define REBIAS_WINDOWS              3       // a slow turn, unless this many in a row agree

#define ACCEL_SMOOTH_WEIGHT         0.25f   // weight of each new reading in the moving average
#define ACCEL_SCALE_MIN             0.25f   // beyond these, the reading is too far off to trust
#define ACCEL_SCALE_MAX             4.0f

GyroFilterSettings GyroFilterSettings::defaults()
{
    GyroFilterSettings settings;
    settings.enabled = false;
    settings.removeOffset = true;
    settings.smoothSamples = 16;
    settings.smoothBelowDps = 7;
    settings.deadzoneDps = 3;
    settings.correctAccel = true;
    return settings;
}

namespace {

bool windowIsStill(int samples, const double sum[3], const double sumSq[3], double maxStdDev)
{
    for (int i = 0; i < 3; i++) {
        double mean = sum[i] / samples;
        double variance = sumSq[i] / samples - mean * mean;
        if (variance > maxStdDev * maxStdDev) {
            return false;
        }
    }
    return true;
}

}

void GyroFilter::reset(const GyroFilterSettings& settings)
{
    memset(this, 0, sizeof(*this));
    m_LogIndex = -1;
    applySettings(settings);
}

void GyroFilter::applySettings(const GyroFilterSettings& settings)
{
    m_Settings = settings;
    m_Settings.smoothSamples = qBound(1, m_Settings.smoothSamples, (int)MaxSmoothSamples);
    m_Settings.smoothBelowDps = qMax(m_Settings.smoothBelowDps, 0);
    m_Settings.deadzoneDps = qMax(m_Settings.deadzoneDps, 0);

    // The history's length may have changed
    m_HistoryCount = 0;
    m_HistoryNext = 0;
}

void GyroFilter::restart()
{
    m_WindowStart = 0;
    m_HistoryCount = 0;
    m_AccelSmoothedValid = false;
}

void GyroFilter::offsetDps(float out[3]) const
{
    for (int i = 0; i < 3; i++) {
        out[i] = m_BiasValid ? (float)(m_Bias[i] * RAD_TO_DEG) : 0;
    }
}

void GyroFilter::startWindow(uint32_t timestampMs)
{
    m_WindowStart = timestampMs ? timestampMs : 1;
    m_GyroSamples = m_AccelSamples = 0;
    for (int i = 0; i < 3; i++) {
        m_GyroSum[i] = m_GyroSumSq[i] = m_AccelSum[i] = m_AccelSumSq[i] = 0;
    }
}

void GyroFilter::finishWindow()
{
    if (m_GyroSamples < STILL_MIN_SAMPLES) {
        return;
    }

    bool still = windowIsStill(m_GyroSamples, m_GyroSum, m_GyroSumSq, STILL_MAX_STDDEV_DPS / RAD_TO_DEG) &&
                 (m_AccelSamples < STILL_MIN_SAMPLES ||
                  windowIsStill(m_AccelSamples, m_AccelSum, m_AccelSumSq, ACCEL_STILL_MAX_STDDEV));
    for (int i = 0; still && i < 3; i++) {
        if (qAbs(m_GyroSum[i] / m_GyroSamples) * RAD_TO_DEG > STILL_MAX_OFFSET_DPS) {
            still = false;
        }
    }
    if (!still) {
        return;
    }

    if (m_Settings.correctAccel && m_AccelSamples >= STILL_MIN_SAMPLES) {
        double gravity = 0;
        for (int i = 0; i < 3; i++) {
            double mean = m_AccelSum[i] / m_AccelSamples;
            gravity += mean * mean;
        }
        gravity = SDL_sqrt(gravity);
        if (gravity > 0) {
            float scale = (float)(STANDARD_GRAVITY / gravity);
            if (scale >= ACCEL_SCALE_MIN && scale <= ACCEL_SCALE_MAX) {
                m_AccelScale = m_AccelScaleValid ? (m_AccelScale + scale) / 2 : scale;
                m_AccelScaleValid = true;
                if (m_LogIndex >= 0 && qAbs(m_AccelScale - m_LoggedAccelScale) >= 0.02f) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Gamepad %d accelerometer reads %.2f m/s2 at rest; scaling by %.3f",
                                m_LogIndex, gravity, m_AccelScale);
                    m_LoggedAccelScale = m_AccelScale;
                }
            }
        }
    }

    if (!m_Settings.removeOffset) {
        return;
    }

    float mean[3];
    bool nearBias = m_BiasValid;
    bool nearCandidate = m_CandidateCount > 0;
    for (int i = 0; i < 3; i++) {
        mean[i] = (float)(m_GyroSum[i] / m_GyroSamples);
        if (qAbs(mean[i] - m_Bias[i]) * RAD_TO_DEG > MAX_BIAS_STEP_DPS) {
            nearBias = false;
        }
        if (qAbs(mean[i] - m_CandidateBias[i]) * RAD_TO_DEG > MAX_BIAS_STEP_DPS) {
            nearCandidate = false;
        }
    }

    bool replace = !m_BiasValid;
    if (nearBias) {
        // Small drift: follow it, averaged so no single window takes over
        for (int i = 0; i < 3; i++) {
            m_Bias[i] = (m_Bias[i] + mean[i]) / 2;
        }
        m_CandidateCount = 0;
    }
    else if (m_BiasValid) {
        // Too far to be drift: probably a slow turn, unless it keeps happening
        if (nearCandidate) {
            m_CandidateCount++;
            for (int i = 0; i < 3; i++) {
                m_CandidateBias[i] += (mean[i] - m_CandidateBias[i]) / m_CandidateCount;
            }
        }
        else {
            m_CandidateCount = 1;
            memcpy(m_CandidateBias, mean, sizeof(mean));
        }
        if (m_CandidateCount >= REBIAS_WINDOWS) {
            memcpy(mean, m_CandidateBias, sizeof(mean));
            replace = true;
        }
    }

    if (replace) {
        memcpy(m_Bias, mean, sizeof(mean));
        m_CandidateCount = 0;
        // The smoothing history has readings corrected by the old offset (or none)
        m_HistoryCount = 0;
    }
    m_BiasValid = true;

    float change = 0;
    for (int i = 0; i < 3; i++) {
        change = qMax(change, (float)(qAbs(m_Bias[i] - m_LoggedBias[i]) * RAD_TO_DEG));
    }
    if (m_LogIndex >= 0 && change >= 0.5f) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Gamepad %d gyro offset: %.2f %.2f %.2f deg/s",
                    m_LogIndex,
                    m_Bias[0] * RAD_TO_DEG, m_Bias[1] * RAD_TO_DEG, m_Bias[2] * RAD_TO_DEG);
        memcpy(m_LoggedBias, m_Bias, sizeof(m_Bias));
    }
}

void GyroFilter::updateCalibration(uint32_t timestampMs, const float radPerSec[3])
{
    if (m_WindowStart == 0 || SDL_TICKS_PASSED(timestampMs, m_WindowStart + STILL_WINDOW_MS)) {
        if (m_WindowStart != 0) {
            finishWindow();
        }
        startWindow(timestampMs);
    }

    m_GyroSamples++;
    for (int i = 0; i < 3; i++) {
        m_GyroSum[i] += radPerSec[i];
        m_GyroSumSq[i] += (double)radPerSec[i] * radPerSec[i];
    }
}

void GyroFilter::processGyro(uint32_t timestampMs, const float radPerSec[3], float outDps[3])
{
    if (!m_Settings.enabled) {
        for (int i = 0; i < 3; i++) {
            outDps[i] = (float)(radPerSec[i] * RAD_TO_DEG);
        }
        return;
    }

    if (m_Settings.removeOffset || m_Settings.correctAccel) {
        updateCalibration(timestampMs, radPerSec);
    }

    for (int i = 0; i < 3; i++) {
        float value = radPerSec[i];
        if (m_Settings.removeOffset && m_BiasValid) {
            value -= m_Bias[i];
        }
        outDps[i] = (float)(value * RAD_TO_DEG);
    }

    if (m_Settings.smoothSamples > 1 && m_Settings.smoothBelowDps > 0) {
        memcpy(m_History[m_HistoryNext], outDps, sizeof(m_History[0]));
        m_HistoryNext = (m_HistoryNext + 1) % m_Settings.smoothSamples;
        m_HistoryCount = qMin(m_HistoryCount + 1, m_Settings.smoothSamples);

        float smoothed[3] = {};
        for (int n = 0; n < m_HistoryCount; n++) {
            for (int i = 0; i < 3; i++) {
                smoothed[i] += m_History[n][i] / m_HistoryCount;
            }
        }

        // Raw from twice the threshold up, smoothed below it, blended in between
        float threshold = (float)m_Settings.smoothBelowDps;
        float magnitude = SDL_sqrtf(outDps[0] * outDps[0] + outDps[1] * outDps[1] + outDps[2] * outDps[2]);
        float rawWeight = qBound(0.0f, (magnitude - threshold) / threshold, 1.0f);
        for (int i = 0; i < 3; i++) {
            outDps[i] = outDps[i] * rawWeight + smoothed[i] * (1 - rawWeight);
        }
    }

    if (m_Settings.deadzoneDps > 0) {
        float deadzone = (float)m_Settings.deadzoneDps;
        float magnitude = SDL_sqrtf(outDps[0] * outDps[0] + outDps[1] * outDps[1] + outDps[2] * outDps[2]);
        if (magnitude < deadzone) {
            float scale = magnitude / deadzone;
            for (int i = 0; i < 3; i++) {
                outDps[i] *= scale;
            }
        }
    }
}

void GyroFilter::processAccel(const float in[3], float out[3])
{
    memcpy(out, in, sizeof(float) * 3);
    if (!m_Settings.enabled) {
        return;
    }

    // Feed the stillness window that processGyro() runs
    if (m_WindowStart != 0) {
        m_AccelSamples++;
        for (int i = 0; i < 3; i++) {
            m_AccelSum[i] += in[i];
            m_AccelSumSq[i] += (double)in[i] * in[i];
        }
    }

    if (!m_Settings.correctAccel) {
        return;
    }

    if (m_AccelScaleValid) {
        for (int i = 0; i < 3; i++) {
            out[i] *= m_AccelScale;
        }
    }

    if (!m_AccelSmoothedValid) {
        memcpy(m_AccelSmoothed, out, sizeof(m_AccelSmoothed));
        m_AccelSmoothedValid = true;
    }
    for (int i = 0; i < 3; i++) {
        m_AccelSmoothed[i] += (out[i] - m_AccelSmoothed[i]) * ACCEL_SMOOTH_WEIGHT;
        out[i] = m_AccelSmoothed[i];
    }
}
