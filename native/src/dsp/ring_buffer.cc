// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ring_buffer.h"

#include <algorithm>
#include <cstring>

namespace saf {

FloatRingBuffer::FloatRingBuffer(size_t capacityFrames, size_t channels)
    : storage_(capacityFrames * channels, 0.0f)
    , capacityFrames_(capacityFrames)
    , channels_(channels) {}

size_t FloatRingBuffer::write(const float* interleaved, size_t frames) {
    if (frames == 0 || channels_ == 0) return 0;

    std::lock_guard<std::mutex> lock(mutex_);

    size_t dropped = 0;
    if (frames > capacityFrames_) {
        // Keep only the newest part of an oversized write and report the rest.
        const size_t skip = frames - capacityFrames_;
        interleaved += skip * channels_;
        frames = capacityFrames_;
        dropped = skip;
    }

    if (sizeFrames_ + frames > capacityFrames_) {
        dropped = sizeFrames_ + frames - capacityFrames_;
        dropOldestLocked(dropped);
    }

    const size_t writeFrame = (readFrame_ + sizeFrames_) % capacityFrames_;
    const size_t firstChunk = std::min(frames, capacityFrames_ - writeFrame);
    std::memcpy(&storage_[writeFrame * channels_], interleaved, firstChunk * channels_ * sizeof(float));
    if (firstChunk < frames) {
        std::memcpy(&storage_[0], interleaved + firstChunk * channels_, (frames - firstChunk) * channels_ * sizeof(float));
    }
    sizeFrames_ += frames;
    droppedFrames_ += dropped;
    return dropped;
}

size_t FloatRingBuffer::read(float* out, size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t toRead = std::min(frames, sizeFrames_);
    if (toRead == 0) return 0;

    const size_t firstChunk = std::min(toRead, capacityFrames_ - readFrame_);
    std::memcpy(out, &storage_[readFrame_ * channels_], firstChunk * channels_ * sizeof(float));
    if (firstChunk < toRead) {
        std::memcpy(out + firstChunk * channels_, &storage_[0], (toRead - firstChunk) * channels_ * sizeof(float));
    }

    readFrame_ = (readFrame_ + toRead) % capacityFrames_;
    sizeFrames_ -= toRead;
    return toRead;
}

void FloatRingBuffer::dropOldest(size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    dropOldestLocked(std::min(frames, sizeFrames_));
}

void FloatRingBuffer::dropOldestLocked(size_t frames) {
    readFrame_ = (readFrame_ + frames) % capacityFrames_;
    sizeFrames_ -= frames;
    droppedFrames_ += frames;
}

void FloatRingBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    readFrame_ = 0;
    sizeFrames_ = 0;
}

void FloatRingBuffer::prefillSilence(size_t frames) {
    const size_t zeros = std::min(frames, capacityFrames_);
    std::vector<float> tmp(zeros * channels_, 0.0f);
    // Avoid counting the artificial prefill as dropped audio.
    const size_t before = droppedFrames_;
    write(tmp.data(), zeros);
    std::lock_guard<std::mutex> lock(mutex_);
    droppedFrames_ = before;
}

size_t FloatRingBuffer::availableFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sizeFrames_;
}

size_t FloatRingBuffer::capacityFrames() const { return capacityFrames_; }
size_t FloatRingBuffer::channels() const { return channels_; }

size_t FloatRingBuffer::droppedFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return droppedFrames_;
}

} // namespace saf
