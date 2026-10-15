// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Interfaces between the (portable) audio engine and the (platform specific)
// capture backends. Keeping these abstract is what allows the engine to be
// tested on any OS with a mock backend.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dsp/ring_buffer.h"

namespace saf {

/** One audio session of the Windows audio engine (or of the mock). */
struct SessionInfo {
    /** Unique for the lifetime of the session, used for change detection. */
    std::string sessionId;
    uint32_t pid = 0;
    uint32_t parentPid = 0;
    std::string exePath;
    std::string exeName;
    std::string displayName;

    /** Stable identity of the application the session belongs to. */
    std::string appKey;

    bool isSystemSounds = false;
    bool isUwp = false;

    /** False when we know that this session can not be captured correctly. */
    bool capturable = true;
    std::string notCapturableReason;

    /** True while the session state is "active" (a stream is running). */
    bool playing = false;

    /** Session peak meter, 0..1, or negative when unavailable. */
    float peak = -1.0f;
};

/** Process table entry, needed to compute capture roots per application. */
struct ProcessNode {
    uint32_t pid = 0;
    uint32_t parentPid = 0;
};

/**
 * Provides the current set of applications playing audio, plus change events.
 * Implementations: Windows (IAudioSessionManager2 + notifications), Mock.
 */
class IAppEnumerator {
public:
    virtual ~IAppEnumerator() = default;

    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;

    /** Fresh snapshot of all sessions. */
    virtual std::vector<SessionInfo> enumerate() = 0;

    /** Current process tree, for capture root computation. */
    virtual std::vector<ProcessNode> processes() = 0;

    /** True once after sessions appeared/disappeared (event driven). */
    virtual bool consumeChangeFlag() = 0;

    /** Human readable backend description, shown in diagnostics. */
    virtual std::string describe() const = 0;
};

/** What to capture for one application. */
struct CaptureRequest {
    std::string appKey;
    std::string displayName;
    /** Root processes of the application; their children are included. */
    std::vector<uint32_t> rootPids;
    int sampleRate = 48000;
    int channels = 2;
};

/**
 * A single application capture. Implementations must deliver interleaved
 * float32 stereo data at request.sampleRate through buffer().
 */
class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    /** Starts the capture; may spawn a thread. Returns false + reason on failure. */
    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;

    virtual FloatRingBuffer* buffer() = 0;

    /** Diagnostic: format actually negotiated with the OS. */
    virtual std::string formatDescription() const = 0;

    /** Estimated capture latency in milliseconds, -1 when unknown. */
    virtual int latencyMs() const { return -1; }

    /** Non empty when the capture died (device removed, access lost, ...). */
    virtual std::string lastError() const { return {}; }
};

class ICaptureFactory {
public:
    virtual ~ICaptureFactory() = default;

    /** Creates a capture; does not start it yet. */
    virtual std::unique_ptr<ICaptureSource> create(const CaptureRequest& request) = 0;

    /** Capability probe, used by the diagnostics panel. */
    virtual std::string describe() const = 0;
};

} // namespace saf
