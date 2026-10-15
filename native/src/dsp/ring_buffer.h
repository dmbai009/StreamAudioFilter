// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bounded, thread safe FIFO of interleaved float frames.
//
// Producers are audio capture threads (a few writes per 10 ms), consumers are
// the mixer thread and the JS bridge thread. Because both sides are slow and
// the buffers are tiny, a plain mutex is used instead of a lock free
// implementation: simpler to reason about and measurably not a bottleneck.
// The mutex is never held while allocating or calling into foreign code, so
// it can not block an audio thread for a meaningful amount of time.

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace saf {

class FloatRingBuffer {
public:
    FloatRingBuffer(size_t capacityFrames, size_t channels);
    FloatRingBuffer(const FloatRingBuffer&) = delete;
    FloatRingBuffer& operator=(const FloatRingBuffer&) = delete;

    /**
     * Appends frames. If the buffer would overflow, the oldest frames are
     * dropped - a late consumer must never be able to stall a capture thread
     * nor grow memory without bound.
     *
     * @returns number of frames that were dropped to make room.
     */
    size_t write(const float* interleaved, size_t frames);

    /**
     * Reads up to @p frames frames. Returns the number of frames actually
     * read; the remainder of @p out is left untouched.
     */
    size_t read(float* out, size_t frames);

    /** Discards the oldest @p frames frames. */
    void dropOldest(size_t frames);

    void clear();

    size_t availableFrames() const;
    size_t capacityFrames() const;
    size_t droppedFrames() const;
    size_t channels() const;

    /** Fills the whole buffer with silence, marking it as "full". */
    void prefillSilence(size_t frames);

private:
    /** Requires the mutex to be held by the caller. */
    void dropOldestLocked(size_t frames);

    mutable std::mutex mutex_;
    std::vector<float> storage_;
    size_t capacityFrames_;
    size_t channels_;
    size_t readFrame_ = 0;
    size_t sizeFrames_ = 0;
    size_t droppedFrames_ = 0;
};

} // namespace saf
