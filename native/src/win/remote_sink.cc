// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The output, split into two processes.
//
// Discord's hook based soundshare injects DiscordHook64.dll into the process
// it takes the share audio from and intercepts that process' Windows audio
// calls. The captures of the other applications must not run inside that
// process: in a real share they delivered only silence there, while the very
// same capture worked in any other process. So:
//
//   saf-audio-host               captures, mixes (Engine), feeds a ring in
//                                shared memory — Discord never touches it
//   saf-audio-host --render      plays the ring into the muted session; this
//                                is the pid Discord is pointed at
//
// The render process exits when its parent exits, or when asked to.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../engine.h"
#include "../output_sink.h"
#include "render_sink.h"

namespace saf {

namespace {

constexpr uint32_t kMagic = 0x53414652;  // "SAFR"
constexpr uint32_t kRingFrames = 48000 / 2;  // 0.5 s
constexpr uint32_t kTargetFrames = 2048;     // ~43 ms kept ahead of the device
constexpr uint32_t kBlockFrames = 480;

/** Shared between the two processes. Text fields are guarded by textSeq (seqlock). */
struct Shared {
    uint32_t magic;
    uint32_t ringFrames;
    std::atomic<uint64_t> written;
    std::atomic<uint64_t> read;
    std::atomic<uint32_t> stop;
    std::atomic<uint32_t> running;
    std::atomic<uint32_t> sessionMuted;
    std::atomic<uint32_t> discordAttached;
    std::atomic<uint32_t> remutes;
    std::atomic<uint32_t> restarts;
    std::atomic<uint64_t> renderedFrames;
    std::atomic<uint64_t> underrunFrames;
    std::atomic<uint32_t> peakMilli;
    std::atomic<uint32_t> textSeq;
    char device[256];
    char format[128];
    char error[512];
    float ring[kRingFrames * 2];
};

void copyText(char* target, size_t size, const std::string& value) {
    const size_t n = std::min(size - 1, value.size());
    std::memcpy(target, value.data(), n);
    target[n] = '\0';
}

class RemoteSink : public IOutputSink {
public:
    ~RemoteSink() override { stop(); }

    bool start(Engine& engine, std::string& error) override {
        if (running_) return true;
        engine_ = &engine;
        name_ = L"Local\\StreamAudioFilter-render-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name_.c_str());
        if (!mapping_) {
            error = "the shared memory for the audio output could not be created";
            return false;
        }
        shared_ = static_cast<Shared*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!shared_) {
            error = "the shared memory for the audio output could not be mapped";
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }
        new (shared_) Shared {};
        shared_->ringFrames = kRingFrames;
        shared_->magic = kMagic;

        if (!spawn(error)) {
            stop();
            return false;
        }
        running_ = true;
        thread_ = std::thread([this] { feed(); });
        return true;
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (shared_) shared_->stop = 1;
        if (child_) {
            if (WaitForSingleObject(child_, 3000) != WAIT_OBJECT_0) TerminateProcess(child_, 1);
            CloseHandle(child_);
            child_ = nullptr;
        }
        childPid_ = 0;
        if (shared_) {
            UnmapViewOfFile(shared_);
            shared_ = nullptr;
        }
        if (mapping_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        engine_ = nullptr;
    }

    OutputStatus status() const override {
        OutputStatus status;
        status.kind = "wasapi";
        status.pid = childPid_;
        status.restarts = spawns_ > 0 ? spawns_ - 1 : 0;
        if (!shared_) return status;
        status.running = running_ && shared_->running != 0;
        status.sessionMuted = shared_->sessionMuted != 0;
        status.discordAttached = shared_->discordAttached != 0;
        status.remutes = shared_->remutes;
        status.restarts += shared_->restarts;
        status.renderedFrames = shared_->renderedFrames;
        status.underrunFrames = shared_->underrunFrames;
        status.peak = static_cast<float>(shared_->peakMilli.load()) / 1000.0f;
        for (int attempt = 0; attempt < 4; attempt++) {
            const uint32_t before = shared_->textSeq.load(std::memory_order_acquire);
            if (before & 1) continue;
            const std::string device = shared_->device, format = shared_->format, error = shared_->error;
            if (shared_->textSeq.load(std::memory_order_acquire) == before) {
                status.device = device;
                status.format = format;
                status.error = error.empty() ? childError_ : error;
                break;
            }
        }
        if (status.error.empty()) status.error = childError_;
        return status;
    }

private:
    bool spawn(std::string& error) {
        wchar_t self[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
        std::wstring command = L"\"" + std::wstring(self) + L"\" --render " + name_ + L" --parent " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW si { sizeof(si) };
        PROCESS_INFORMATION pi {};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            error = "the audio output process could not be started (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        CloseHandle(pi.hThread);
        child_ = pi.hProcess;
        childPid_ = pi.dwProcessId;
        spawns_++;
        return true;
    }

    void feed() {
        std::vector<float> block(kBlockFrames * 2);
        while (running_) {
            if (child_ && WaitForSingleObject(child_, 0) == WAIT_OBJECT_0) {
                // The output process died. A new one gets a new pid, which a
                // running share does not know: the plugin reports that.
                CloseHandle(child_);
                child_ = nullptr;
                childPid_ = 0;
                childError_ = "the audio output process exited; it was started again";
                shared_->running = 0;
                std::string error;
                if (!spawn(error)) childError_ = error;
            }
            const uint64_t read = shared_->read.load(std::memory_order_acquire);
            uint64_t written = shared_->written.load(std::memory_order_relaxed);
            if (written < read) written = read;  // the reader ran dry and skipped ahead
            while (written - read < kTargetFrames) {
                engine_->pull(block.data(), kBlockFrames);
                for (uint32_t i = 0; i < kBlockFrames; i++) {
                    const uint64_t index = ((written + i) % kRingFrames) * 2;
                    shared_->ring[index] = block[i * 2];
                    shared_->ring[index + 1] = block[i * 2 + 1];
                }
                written += kBlockFrames;
                shared_->written.store(written, std::memory_order_release);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }

    Engine* engine_ = nullptr;
    std::wstring name_;
    HANDLE mapping_ = nullptr;
    Shared* shared_ = nullptr;
    HANDLE child_ = nullptr;
    std::atomic<uint32_t> childPid_ { 0 };
    uint32_t spawns_ = 0;
    std::string childError_;
    std::atomic<bool> running_ { false };
    std::thread thread_;
};

} // namespace

std::unique_ptr<IOutputSink> createRemoteSink() { return std::make_unique<RemoteSink>(); }

int runRenderProcess(const std::wstring& mapping, uint32_t parentPid) {
    HANDLE handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping.c_str());
    if (!handle) return 3;
    auto* shared = static_cast<Shared*>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared || shared->magic != kMagic) return 4;
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);

    RenderSink sink;
    std::string error;
    float windowPeak = 0.0f;
    uint64_t windowFrames = 0;
    sink.startWithSource([&](float* out, size_t frames) {
        const uint64_t written = shared->written.load(std::memory_order_acquire);
        uint64_t read = shared->read.load(std::memory_order_relaxed);
        const uint64_t available = written > read ? written - read : 0;
        const size_t take = static_cast<size_t>(std::min<uint64_t>(available, frames));
        for (size_t i = 0; i < take; i++) {
            const uint64_t index = ((read + i) % shared->ringFrames) * 2;
            out[i * 2] = shared->ring[index];
            out[i * 2 + 1] = shared->ring[index + 1];
        }
        if (take < frames) {
            std::memset(out + take * 2, 0, (frames - take) * 2 * sizeof(float));
            shared->underrunFrames += frames - take;
        }
        read += take;
        shared->read.store(read, std::memory_order_release);
        for (size_t i = 0; i < frames * 2; i++) windowPeak = std::max(windowPeak, std::fabs(out[i]));
        windowFrames += frames;
        if (windowFrames >= 48000) {
            shared->peakMilli = static_cast<uint32_t>(windowPeak * 1000.0f);
            windowPeak = 0.0f;
            windowFrames = 0;
        }
    }, error);

    while (!shared->stop) {
        if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) break;
        const OutputStatus status = sink.status();
        shared->running = status.running ? 1 : 0;
        shared->sessionMuted = status.sessionMuted ? 1 : 0;
        shared->discordAttached = status.discordAttached ? 1 : 0;
        shared->remutes = status.remutes;
        shared->restarts = status.restarts;
        shared->renderedFrames = status.renderedFrames;
        shared->textSeq.fetch_add(1, std::memory_order_acq_rel);
        copyText(shared->device, sizeof(shared->device), status.device);
        copyText(shared->format, sizeof(shared->format), status.format);
        copyText(shared->error, sizeof(shared->error), status.error);
        shared->textSeq.fetch_add(1, std::memory_order_acq_rel);
        Sleep(100);
    }
    sink.stop();
    shared->running = 0;
    UnmapViewOfFile(shared);
    CloseHandle(handle);
    if (parent) CloseHandle(parent);
    return 0;
}

} // namespace saf
