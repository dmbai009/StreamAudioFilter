// StreamAudioFilter — audio host
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small process started by the plugin (Discord's main process) that owns the
// whole audio engine:
//   * it lists the applications that play audio and captures the transmitted
//     ones through WASAPI process loopback,
//   * it plays the filtered mix into a muted session (output_sink.h), and
//   * Discord is pointed at this process as the source of the share audio.
//
// Being a process of its own is not an implementation detail: Discord's
// soundshare captures a *process*, so the mix has to come from one. It also
// means a crash in the audio code can never take Discord down, and no native
// module has to match the Electron version of Discord.
//
// Protocol: one command per line on stdin, fields separated by TAB:
//     <id> TAB <command> [TAB <argument>]...
// One JSON object per line on stdout answers it, carrying the same id. The
// host exits when stdin is closed, i.e. when Discord exits or the plugin stops.
//
//   hello                         version, protocol, pid, backend
//   probe                         can this machine capture per application?
//   start TAB <maxStreams>        start the engine (listing + capture gate)
//   stop                          stop output and engine
//   rules TAB <mode> TAB <active> [TAB <appKey> TAB <0|1>]...
//   apps                          application list with live state
//   stats                         engine and output state
//   refresh                       re-enumerate now
//   output TAB <0|1>              play the mix (into the muted session) or not
//   mockApps [TAB <app>]...       test backend only; an app is US separated:
//                                 key, name, path, exe, pid, parent, playing, capturable
//   quit

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../engine.h"
#include "../json.h"
#include "../mock/mock_backend.h"
#include "../output_sink.h"
#include "../platform.h"
#include "../version.h"
#if defined(_WIN32)
#    include "../win/render_sink.h"
#endif

#if defined(_WIN32)
#    include <fcntl.h>
#    include <io.h>
#    include <objbase.h>
#    include <windows.h>
#else
#    include <unistd.h>
#endif

using namespace saf;

namespace {

uint32_t currentPid() {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

/** Sessions of this executable (the render process) are never listed. */
std::string selfExePath() {
#if defined(_WIN32)
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    const int size = WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(n), out.data(), size, nullptr, nullptr);
    return out;
#else
    return {};
#endif
}

std::vector<std::string> split(const std::string& line, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : line) {
        if (c == separator) {
            parts.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

long toLong(const std::string& value, long fallback) {
    char* end = nullptr;
    const long result = std::strtol(value.c_str(), &end, 10);
    return end && *end == '\0' && !value.empty() ? result : fallback;
}

std::string appsJson(const std::vector<AppView>& apps) {
    std::string json = "[";
    for (size_t i = 0; i < apps.size(); i++) {
        const auto& app = apps[i];
        if (i) json += ',';
        JsonObject object(json);
        object.string("appKey", app.appKey);
        object.string("displayName", app.displayName);
        object.string("exeName", app.exeName);
        object.string("exePath", app.exePath);
        object.boolean("playing", app.playing);
        object.boolean("capturable", app.capturable);
        object.boolean("ephemeral", app.ephemeral);
        object.boolean("transmitting", app.transmitting);
        object.boolean("captured", app.captured);
        object.number("peak", static_cast<int>(app.peak * 1000.0f));
        object.number("latencyMs", app.latencyMs);
        object.string("format", app.format);
        object.string("error", app.error);
        object.string("notCapturableReason", app.notCapturableReason);
        object.string("blockedReason", app.blockedReason);
        object.string("coveredBy", app.coveredBy);
        object.raw("devices", jsonStringArray(app.devices));
        object.boolean("silentCapture", app.silentCapture);
        object.raw("captureRoots", jsonNumberArray(app.captureRoots));
        object.number("inputPeak", static_cast<int>(app.inputPeak * 1000.0f));
        object.number("starvedFrames", static_cast<long long>(app.starvedFrames));
        object.string("state", app.state);
        object.number("instances", static_cast<long long>(app.instances));
    }
    json += ']';
    return json;
}

std::string engineJson(const EngineStats& stats) {
    std::string json;
    JsonObject object(json);
    object.boolean("running", stats.running);
    object.string("backend", stats.backend);
    object.string("captureBackend", stats.captureBackend);
    object.number("sampleRate", stats.sampleRate);
    object.number("channels", stats.channels);
    object.number("blockMs", stats.blockMs);
    object.number("blocksMixed", static_cast<long long>(stats.blocksMixed));
    object.number("outputFrames", static_cast<long long>(stats.outputFrames));
    object.number("starvedFrames", static_cast<long long>(stats.starvedFrames));
    object.number("limitedBlocks", static_cast<long long>(stats.limitedBlocks));
    object.number("clampedSamples", static_cast<long long>(stats.clampedSamples));
    object.number("activeStreams", static_cast<long long>(stats.activeStreams));
    object.raw("warnings", jsonStringArray(stats.warnings));
    return json;
}

std::string outputJson(const OutputStatus& status) {
    std::string json;
    JsonObject object(json);
    object.boolean("running", status.running);
    object.string("kind", status.kind);
    object.string("device", status.device);
    object.string("format", status.format);
    object.string("error", status.error);
    object.number("renderedFrames", static_cast<long long>(status.renderedFrames));
    object.number("peak", static_cast<int>(status.peak * 1000.0f));
    object.boolean("sessionMuted", status.sessionMuted);
    object.number("remutes", status.remutes);
    object.boolean("discordAttached", status.discordAttached);
    object.number("restarts", status.restarts);
    object.number("pid", status.pid);
    object.number("underrunFrames", static_cast<long long>(status.underrunFrames));
    return json;
}

std::string supportJson(const SupportInfo& support) {
    std::string json;
    JsonObject object(json);
    object.boolean("supported", support.supported);
    object.string("os", support.os);
    object.number("build", support.build);
    object.string("reason", support.reason);
    object.string("detail", support.detail);
    object.raw("checks", jsonStringArray(support.checks));
    object.string("platform", describePlatform());
    return json;
}

class Host {
public:
    Host(BackendKind backend, OutputKind outputKind, std::vector<uint32_t> protectedRoots)
        : backend_(backend), outputKind_(outputKind), protectedRoots_(std::move(protectedRoots)) {}

    ~Host() { shutdown(); }

    void shutdown() {
        stopOutput();
        if (engine_) {
            engine_->stop();
            engine_.reset();
        }
    }

    /** Handles one command line and returns the JSON answer (without id). */
    std::string handle(const std::vector<std::string>& args, bool& quit) {
        const std::string& command = args.size() > 1 ? args[1] : std::string();
        std::string json;

        if (command == "hello") {
            JsonObject object(json);
            object.boolean("ok", true);
            object.string("version", SAF_VERSION);
            object.number("protocol", SAF_PROTOCOL);
            object.number("pid", currentPid());
            object.string("backend", backend_ == BackendKind::Mock ? "mock" : "auto");
            object.string("output", outputKind_ == OutputKind::Null ? "null" : "wasapi");
            object.string("platform", describePlatform());
        } else if (command == "probe") {
            SupportInfo support;
            if (backend_ == BackendKind::Mock) {
                support.supported = true;
                support.os = describePlatform();
                support.reason = "Test mode: the audio is produced by a signal generator.";
                support.checks.push_back("A signal generator instead of WASAPI");
            } else {
                support = probeSupport();
            }
            JsonObject object(json);
            object.boolean("ok", true);
            object.raw("support", supportJson(support));
        } else if (command == "start") {
            std::string error, warning;
            const bool ok = startEngine(args.size() > 2 ? toLong(args[2], 12) : 12, error, warning);
            JsonObject object(json);
            object.boolean("ok", ok);
            if (!ok) object.string("error", error);
            if (!warning.empty()) object.string("warning", warning);
        } else if (command == "stop") {
            shutdown();
            JsonObject object(json);
            object.boolean("ok", true);
        } else if (command == "rules") {
            RuleSet rules;
            rules.mode = args.size() > 2 && args[2] == "1" ? RuleSet::Mode::OnlySelected : RuleSet::Mode::AllExcept;
            rules.active = args.size() > 3 && args[3] == "1";
            for (size_t i = 4; i + 1 < args.size(); i += 2) rules.overrides[args[i]] = args[i + 1] == "1";
            if (engine_) engine_->setRules(rules);
            JsonObject object(json);
            object.boolean("ok", engine_ != nullptr);
            if (!engine_) object.string("error", "the engine is not running");
            object.number("overrides", static_cast<long long>(rules.overrides.size()));
        } else if (command == "mute") {
            JsonObject object(json);
            const bool valid = args.size() == 3 && (args[2] == "0" || args[2] == "1");
            object.boolean("ok", valid && engine_ != nullptr);
            if (!engine_) object.string("error", "the engine is not running");
            else if (!valid) object.string("error", "mute expects 0 or 1");
            else engine_->setMasterMuted(args[2] == "1");
        } else if (command == "apps") {
            JsonObject object(json);
            object.boolean("ok", true);
            object.raw("apps", appsJson(engine_ ? engine_->apps() : std::vector<AppView> {}));
        } else if (command == "stats") {
            JsonObject object(json);
            object.boolean("ok", true);
            object.raw("engine", engine_ ? engineJson(engine_->stats()) : std::string("null"));
            object.boolean("masterMuted", engine_ && engine_->masterMuted());
            OutputStatus status;
            status.kind = outputKind_ == OutputKind::Null ? "null" : "wasapi";
            status.discordAttached = discordHookLoaded();
            object.raw("output", outputJson(output_ ? output_->status() : status));
        } else if (command == "refresh") {
            if (engine_) engine_->requestRefresh();
            JsonObject object(json);
            object.boolean("ok", true);
        } else if (command == "output") {
            const bool on = args.size() > 2 && args[2] == "1";
            std::string error;
            bool ok = true;
            if (on) {
                ok = startOutput(error);
            } else {
                stopOutput();
            }
            JsonObject object(json);
            object.boolean("ok", ok);
            if (!ok) object.string("error", error);
            if (ok && on && output_) object.number("pid", output_->status().pid);
        } else if (command == "mockApps") {
            std::vector<mock::MockEnumerator::App> apps;
            for (size_t i = 2; i < args.size(); i++) {
                const auto fields = split(args[i], '\x1f');
                if (fields.size() < 8) continue;
                mock::MockEnumerator::App app;
                app.appKey = fields[0];
                app.displayName = fields[1];
                app.exePath = fields[2];
                app.exeName = fields[3];
                app.pid = static_cast<uint32_t>(toLong(fields[4], 0));
                app.parentPid = static_cast<uint32_t>(toLong(fields[5], 1));
                app.playing = fields[6] == "1";
                app.capturable = fields[7] == "1";
                apps.push_back(std::move(app));
            }
            const bool allowed = backend_ == BackendKind::Mock;
            if (allowed) mock::setAppsOnAllInstances(std::move(apps));
            JsonObject object(json);
            object.boolean("ok", allowed);
            if (!allowed) object.string("error", "scripted applications need the test backend");
        } else if (command == "quit") {
            quit = true;
            JsonObject object(json);
            object.boolean("ok", true);
        } else {
            JsonObject object(json);
            object.boolean("ok", false);
            object.string("error", "unknown command: " + command);
        }
        return json;
    }

private:
    bool startEngine(long maxStreams, std::string& error, std::string& warning) {
        if (engine_ && engine_->running()) return true;

        EngineOptions options;
        options.maxStreams = static_cast<size_t>(std::max(1L, std::min(64L, maxStreams)));
        options.externalClock = true;
        options.selfPid = currentPid();
        options.selfExePath = selfExePath();
        options.protectedRoots = protectedRoots_;

        std::string enumeratorError, factoryError;
        auto enumerator = createEnumerator(backend_, enumeratorError);
        auto factory = createCaptureFactory(backend_, factoryError);
        if (!enumerator || !factory) {
            // No silent fallback to the signal generator: a "filtered" share
            // made of test tones would be a lie. The plugin reports this.
            error = enumeratorError.empty() ? factoryError : enumeratorError;
            if (error.empty()) error = "the capture backend is unavailable";
            return false;
        }
        if (!enumeratorError.empty()) warning = enumeratorError;

        engine_ = std::make_unique<Engine>(options, std::move(enumerator), std::move(factory));
        if (!engine_->start(error)) {
            engine_.reset();
            if (error.empty()) error = "could not start the audio engine";
            return false;
        }
        return true;
    }

    bool startOutput(std::string& error) {
        if (!engine_ || !engine_->running()) {
            error = "the engine is not running";
            return false;
        }
        if (output_) return true;
        std::string note;
        output_ = createOutputSink(outputKind_, note);
        if (!output_ || !output_->start(*engine_, error)) {
            output_.reset();
            if (error.empty()) error = note.empty() ? "the output could not be started" : note;
            return false;
        }
        return true;
    }

    void stopOutput() {
        // The sink stops pulling before anything else happens to the engine.
        if (!output_) return;
        output_->stop();
        output_.reset();
    }

    BackendKind backend_;
    OutputKind outputKind_;
    std::vector<uint32_t> protectedRoots_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<IOutputSink> output_;
};

void printUsage() {
    std::fprintf(stderr,
        "saf-audio-host %s (protocol %d)\n"
        "Started by the StreamAudioFilter Vencord plugin; speaks a line protocol on stdin/stdout.\n"
        "  --backend auto|mock     capture backend (mock = signal generator, for tests)\n"
        "  --output wasapi|null    where the mix is played (null = discarded, for tests)\n"
        "  --protect <pid>         never capture this process tree (Discord); repeatable\n"
        "  --version               print the version and exit\n",
        SAF_VERSION, SAF_PROTOCOL);
}

} // namespace

int main(int argc, char** argv) {
    BackendKind backend = BackendKind::Auto;
#if defined(_WIN32)
    OutputKind output = OutputKind::Wasapi;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    OutputKind output = OutputKind::Null;
#endif
    std::vector<uint32_t> protectedRoots;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--backend" && i + 1 < argc) {
            backend = parseBackendKind(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            const std::string kind = argv[++i];
            output = kind == "null" ? OutputKind::Null : kind == "inprocess" ? OutputKind::InProcess : OutputKind::Wasapi;
#if defined(_WIN32)
        } else if (arg == "--render" && i + 1 < argc) {
            const std::string mapping = argv[++i];
            uint32_t parent = 0;
            if (i + 2 < argc && std::string(argv[i + 1]) == "--parent") parent = static_cast<uint32_t>(toLong(argv[i + 2], 0));
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            return runRenderProcess(std::wstring(mapping.begin(), mapping.end()), parent);
#endif
        } else if (arg == "--protect" && i + 1 < argc) {
            const long pid = toLong(argv[++i], 0);
            if (pid > 0) protectedRoots.push_back(static_cast<uint32_t>(pid));
        } else if (arg == "--version") {
            std::printf("%s %d\n", SAF_VERSION, SAF_PROTOCOL);
            return 0;
        } else {
            printUsage();
            return 2;
        }
    }

    Host host(backend, output, protectedRoots);
    std::string line;
    bool quit = false;
    while (!quit && std::getline(std::cin, line)) {
        if (line.empty() || line == "\r") continue;
        const auto args = split(line, '\t');
        std::string id = args[0];
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) id = "0";
        const std::string body = host.handle(args, quit);
        // body is "{...}": splice the id in front of its first member.
        std::string out = "{\"id\":" + id + (body.size() > 2 ? "," + body.substr(1) : "}");
        out += '\n';
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
    }
    host.shutdown();
    return 0;
}
