#include "bandwidth.h"

#include <algorithm>

using namespace std::chrono;

BandwidthTracker::BandwidthTracker(uint32_t windowSeconds, uint32_t bucketIntervalMs)
  : windowSeconds(seconds(std::max<uint32_t>(windowSeconds, 1))),
    bucketIntervalMs(bucketIntervalMs > 0 ? (int)bucketIntervalMs : 250)
{
    bucketCount = std::max<uint32_t>((uint32_t)(this->windowSeconds.count() * 1000 / this->bucketIntervalMs), 4);
    buckets.resize(bucketCount);
}

// Add bytes recorded at the current time.
void BandwidthTracker::AddBytes(size_t bytes) {
    std::lock_guard<std::mutex> lock(mtx);
    updateBucket(bytes, steady_clock::now());
}

// We don't want to average the entire window used for peak,
// so average only the newest 25% of complete buckets.
//
// Intervals with nothing recorded count as zero, and the interval in progress is left
// out of both the bytes and the time, so the result isn't dragged down while it fills.
double BandwidthTracker::GetAverageMbps() {
    std::lock_guard<std::mutex> lock(mtx);
    auto current = bucketStartFor(steady_clock::now());
    uint32_t intervals = std::max<uint32_t>(bucketCount / 4, 1);
    size_t totalBytes = 0;

    for (uint32_t i = 1; i <= intervals; i++) {
        totalBytes += bytesInInterval(current - milliseconds(bucketIntervalMs) * i);
    }

    return totalBytes * 8.0 / 1000000.0 / (intervals * bucketIntervalMs / 1000.0);
}

// The busiest complete interval in the window
double BandwidthTracker::GetPeakMbps() {
    std::lock_guard<std::mutex> lock(mtx);
    auto current = bucketStartFor(steady_clock::now());
    size_t peakBytes = 0;

    for (uint32_t i = 1; i < bucketCount; i++) {
        peakBytes = std::max(peakBytes, bytesInInterval(current - milliseconds(bucketIntervalMs) * i));
    }

    return peakBytes * 8.0 / 1000000.0 / (bucketIntervalMs / 1000.0);
}

unsigned int BandwidthTracker::GetWindowSeconds() {
    return (unsigned int)windowSeconds.count();
}

/// private methods

steady_clock::time_point BandwidthTracker::bucketStartFor(steady_clock::time_point now) const {
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
    return steady_clock::time_point(milliseconds(ms - (ms % bucketIntervalMs)));
}

size_t BandwidthTracker::bytesInInterval(steady_clock::time_point start) const {
    auto ms = duration_cast<milliseconds>(start.time_since_epoch()).count();
    const Bucket &bucket = buckets[(ms / bucketIntervalMs) % bucketCount];
    return bucket.start == start ? bucket.bytes : 0;
}

void BandwidthTracker::updateBucket(size_t bytes, steady_clock::time_point now) {
    auto bucketStart = bucketStartFor(now);
    auto ms = duration_cast<milliseconds>(bucketStart.time_since_epoch()).count();
    Bucket &bucket = buckets[(ms / bucketIntervalMs) % bucketCount];

    // A bucket still holding an older interval starts over
    if (bucket.start != bucketStart) {
        bucket.start = bucketStart;
        bucket.bytes = 0;
    }

    bucket.bytes += bytes;
}
