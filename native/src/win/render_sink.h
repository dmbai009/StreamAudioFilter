// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WASAPI output into a muted session, from any source of 48 kHz stereo float.
// Used in the render process (see remote_sink.cc), which is the process
// Discord takes the share audio from.

#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "../output_sink.h"

namespace saf {

class RenderSink : public IOutputSink {
public:
    /** Fills exactly `frames` frames (interleaved stereo float). */
    using Source = std::function<void(float*, size_t)>;

    ~RenderSink() override;

    bool start(Engine& engine, std::string& error) override;
    bool startWithSource(Source source, std::string& error);
    void stop() override;
    OutputStatus status() const override;

private:
    void setError(const std::string& error);
    void threadMain();
    bool run(HANDLE wake);

    Source source_;
    std::thread thread_;
    std::atomic<bool> running_ { false };
    mutable std::mutex mutex_;
    HANDLE wake_ = nullptr;
    OutputStatus status_;
};

/** Entry point of `saf-audio-host --render <mapping> --parent <pid>`. */
int runRenderProcess(const std::wstring& mapping, uint32_t parentPid);

} // namespace saf
