// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// N-API (Node-API) entry points. Plain C API on purpose: the addon has no
// dependency other than the Node headers, and because it uses N-API it is ABI
// stable — no rebuild is needed when Discord's Electron updates as long as the
// module was built for the same architecture and N-API version 8 or newer.
//
// The JS bridge is deliberately tiny:
//   start(options)        -> JSON   { ok, error, sampleRate, channels, blockFrames }
//   stop()                -> JSON   { ok }
//   setRules(json)        -> JSON   { ok, error }
//   apps()                -> JSON   [ AppView, ... ]
//   stats()               -> JSON   { ... }
//   readPcm(frames)       -> Uint8Array (may be shorter than requested)
//   probe(backend)        -> JSON   { supported, os, build, reason, detail, checks }
//   setMockApps(array)    -> JSON   { ok }        (test mode)
//   version()             -> JSON   { version, napi }

#include <node_api.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"
#include "json.h"
#include "mock/mock_backend.h"
#include "platform.h"

#define SAF_ADDON_VERSION "0.1.0"

namespace {

using namespace saf;

std::mutex g_mutex;
std::unique_ptr<Engine> g_engine;

// ---------------------------------------------------------------- N-API helpers

napi_value makeString(napi_env env, const std::string& value) {
    napi_value result = nullptr;
    napi_create_string_utf8(env, value.c_str(), value.size(), &result);
    return result;
}

napi_value makeBool(napi_env env, bool value) {
    napi_value result = nullptr;
    napi_get_boolean(env, value, &result);
    return result;
}

std::string getStringArg(napi_env env, napi_value value) {
    size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return {};
    std::string result(length, '\0');
    napi_get_value_string_utf8(env, value, result.data(), length + 1, &length);
    result.resize(length);
    return result;
}

bool getBoolProperty(napi_env env, napi_value object, const char* name, bool fallback = false) {
    napi_value value = nullptr;
    bool result = fallback;
    if (napi_get_named_property(env, object, name, &value) == napi_ok) {
        napi_get_value_bool(env, value, &result);
    }
    return result;
}

int32_t getIntProperty(napi_env env, napi_value object, const char* name, int32_t fallback) {
    napi_value value = nullptr;
    int32_t result = fallback;
    if (napi_get_named_property(env, object, name, &value) == napi_ok) {
        napi_get_value_int32(env, value, &result);
    }
    return result;
}

std::string getStringProperty(napi_env env, napi_value object, const char* name) {
    napi_value value = nullptr;
    if (napi_get_named_property(env, object, name, &value) != napi_ok) return {};
    return getStringArg(env, value);
}

napi_value throwError(napi_env env, const std::string& message, const char* code = nullptr) {
    napi_throw_error(env, code, message.c_str());
    return nullptr;
}

// ---------------------------------------------------------------------- start

napi_value Start(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    EngineOptions options;
    BackendKind backend = BackendKind::Auto;

    if (argc >= 1) {
        options.sampleRate = getIntProperty(env, args[0], "sampleRate", options.sampleRate);
        options.blockMs = getIntProperty(env, args[0], "blockMs", options.blockMs);
        options.maxStreams = static_cast<size_t>(std::max<int32_t>(1, getIntProperty(env, args[0], "maxStreams", static_cast<int32_t>(options.maxStreams))));
        options.outputBufferMs = static_cast<size_t>(std::max<int32_t>(100, getIntProperty(env, args[0], "outputBufferMs", static_cast<int32_t>(options.outputBufferMs))));
        const std::string backendName = getStringProperty(env, args[0], "backend");
        if (!backendName.empty()) backend = parseBackendKind(backendName);
    }

    if (options.sampleRate != 44100 && options.sampleRate != 48000 && options.sampleRate != 96000) {
        options.sampleRate = 48000;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_engine && g_engine->running()) {
        std::string json;
        {
            JsonObject object(json);
            object.boolean("ok", true);
            object.string("info", "движок уже запущен");
        }
        return makeString(env, json);
    }

    std::string enumeratorError, factoryError;
    BackendKind chosen = backend;
    auto enumerator = createEnumerator(chosen, enumeratorError);
    auto factory = createCaptureFactory(chosen, factoryError);

    // If the real backend is unavailable, fall back to the mock so the UI stays
    // usable — the plugin tells the user about it explicitly.
    std::string warning;
    if (!enumerator || !factory) {
        warning = enumeratorError.empty() ? factoryError : enumeratorError;
        enumerator = std::make_unique<mock::MockEnumerator>();
        factory = std::make_unique<mock::MockCaptureFactory>();
    }

    g_engine = std::make_unique<Engine>(options, std::move(enumerator), std::move(factory));

    std::string error;
    if (!g_engine->start(error)) {
        g_engine.reset();
        std::string json;
        {
            JsonObject object(json);
            object.boolean("ok", false);
            object.string("error", error.empty() ? "не удалось запустить аудиодвижок" : error);
        }
        return makeString(env, json);
    }

    std::string json;
    {
        JsonObject object(json);
        object.boolean("ok", true);
        object.number("sampleRate", options.sampleRate);
        object.number("channels", options.channels);
        object.number("blockFrames", options.sampleRate * options.blockMs / 1000);
        if (!warning.empty()) object.string("warning", warning);
    }
    return makeString(env, json);
}

napi_value Stop(napi_env env, napi_callback_info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_engine) {
        g_engine->stop();
        g_engine.reset();
    }
    std::string json;
    {
        JsonObject object(json);
        object.boolean("ok", true);
    }
    return makeString(env, json);
}

// --------------------------------------------------------------------- rules

napi_value SetRules(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    RuleSet rules;
    if (argc >= 1) {
        napi_valuetype type = napi_undefined;
        napi_typeof(env, args[0], &type);
        if (type == napi_object) {
            const int32_t mode = getIntProperty(env, args[0], "mode", 0);
            rules.mode = mode == 1 ? RuleSet::Mode::OnlySelected : RuleSet::Mode::AllExcept;
            rules.active = getBoolProperty(env, args[0], "active", true);

            napi_value overrides = nullptr;
            if (napi_get_named_property(env, args[0], "overrides", &overrides) == napi_ok) {
                napi_valuetype overridesType = napi_undefined;
                napi_typeof(env, overrides, &overridesType);
                if (overridesType == napi_object) {
                    napi_value keys = nullptr;
                    if (napi_get_property_names(env, overrides, &keys) == napi_ok) {
                        uint32_t count = 0;
                        napi_get_array_length(env, keys, &count);
                        for (uint32_t i = 0; i < count; i++) {
                            napi_value key = nullptr;
                            if (napi_get_element(env, keys, i, &key) != napi_ok) continue;
                            const std::string name = getStringArg(env, key);
                            napi_value value = nullptr;
                            if (napi_get_property(env, overrides, key, &value) != napi_ok) continue;
                            bool transmit = true;
                            if (napi_get_value_bool(env, value, &transmit) != napi_ok) continue;
                            rules.overrides[name] = transmit;
                        }
                    }
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_engine) g_engine->setRules(rules);

    std::string json;
    {
        JsonObject object(json);
        object.boolean("ok", true);
        object.number("overrides", static_cast<long long>(rules.overrides.size()));
    }
    return makeString(env, json);
}

// ------------------------------------------------------------------- queries

napi_value Apps(napi_env env, napi_callback_info) {
    std::vector<AppView> apps;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_engine) apps = g_engine->apps();
    }

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
        object.number("starvedFrames", static_cast<long long>(app.starvedFrames));
        object.string("state", app.state);
    }
    json += ']';
    return makeString(env, json);
}

napi_value Stats(napi_env env, napi_callback_info) {
    EngineStats stats;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_engine) stats = g_engine->stats();
    }

    std::string json;
    {
        JsonObject object(json);
        object.boolean("running", stats.running);
        object.string("backend", stats.backend);
        object.string("captureBackend", stats.captureBackend);
        object.number("sampleRate", stats.sampleRate);
        object.number("channels", stats.channels);
        object.number("blockMs", stats.blockMs);
        object.number("blocksMixed", static_cast<long long>(stats.blocksMixed));
        object.number("outputFrames", static_cast<long long>(stats.outputFrames));
        object.number("outputDroppedFrames", static_cast<long long>(stats.outputDroppedFrames));
        object.number("starvedFrames", static_cast<long long>(stats.starvedFrames));
        object.number("limitedBlocks", static_cast<long long>(stats.limitedBlocks));
        object.number("clampedSamples", static_cast<long long>(stats.clampedSamples));
        object.number("activeStreams", static_cast<long long>(stats.activeStreams));
        object.number("outputBufferedFrames", static_cast<long long>(stats.outputBufferedFrames));
        object.raw("warnings", jsonStringArray(stats.warnings));
    }
    return makeString(env, json);
}

napi_value ReadPcm(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    int32_t requested = 480;
    if (argc >= 1) napi_get_value_int32(env, args[0], &requested);
    if (requested <= 0) requested = 480;
    if (requested > 480000) requested = 480000;

    std::vector<float> buffer(static_cast<size_t>(requested) * 2, 0.0f);
    size_t frames = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_engine) frames = g_engine->readFrames(buffer.data(), static_cast<size_t>(requested));
    }

    void* data = nullptr;
    napi_value result = nullptr;
    const size_t bytes = frames * 2 * sizeof(float);
    if (napi_create_buffer_copy(env, bytes, buffer.data(), &data, &result) != napi_ok) {
        return throwError(env, "не удалось выделить буфер PCM");
    }
    return result;
}

// ------------------------------------------------------------------- probing

napi_value Probe(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    BackendKind backend = BackendKind::Auto;
    if (argc >= 1) {
        const std::string name = getStringArg(env, args[0]);
        if (!name.empty()) backend = parseBackendKind(name);
    }

    SupportInfo support;
    if (backend == BackendKind::Mock) {
        support.supported = true;
        support.os = describePlatform();
        support.reason = "Тестовый режим: звук генерируется сигнал-генератором.";
        support.detail = "Тестовый режим служит только для проверки интерфейса и звукового тракта.";
        support.checks.push_back("Сигнал-генератор вместо WASAPI");
    } else {
        support = probeSupport();
    }

    std::string json;
    {
        JsonObject object(json);
        object.boolean("supported", support.supported);
        object.string("os", support.os);
        object.number("build", support.build);
        object.string("reason", support.reason);
        object.string("detail", support.detail);
        object.raw("checks", jsonStringArray(support.checks));
        object.string("platform", describePlatform());
    }
    return makeString(env, json);
}

napi_value Version(napi_env env, napi_callback_info) {
    std::string json;
    {
        JsonObject object(json);
        object.string("version", SAF_ADDON_VERSION);
        object.number("napi", NAPI_VERSION);
    }
    return makeString(env, json);
}

// -------------------------------------------------------------- mock backend

napi_value SetMockApps(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    std::vector<mock::MockEnumerator::App> apps;

    napi_valuetype type = napi_undefined;
    if (argc >= 1) napi_typeof(env, args[0], &type);
    if (argc >= 1 && type == napi_object) {
        uint32_t count = 0;
        if (napi_get_array_length(env, args[0], &count) == napi_ok) {
            for (uint32_t i = 0; i < count; i++) {
                napi_value element = nullptr;
                if (napi_get_element(env, args[0], i, &element) != napi_ok) continue;

                mock::MockEnumerator::App app;
                app.appKey = getStringProperty(env, element, "appKey");
                app.displayName = getStringProperty(env, element, "displayName");
                app.exePath = getStringProperty(env, element, "exePath");
                app.exeName = getStringProperty(env, element, "exeName");
                app.pid = static_cast<uint32_t>(getIntProperty(env, element, "pid", static_cast<int32_t>(i + 100)));
                app.parentPid = static_cast<uint32_t>(getIntProperty(env, element, "parentPid", 1));
                app.playing = getBoolProperty(env, element, "playing", true);
                app.capturable = getBoolProperty(env, element, "capturable", true);
                app.notCapturableReason = getStringProperty(env, element, "notCapturableReason");
                if (app.appKey.empty()) app.appKey = "exe:" + std::to_string(app.pid);
                if (app.displayName.empty()) app.displayName = app.appKey;
                apps.push_back(std::move(app));
            }
        }
    }

    // The engine owns its enumerator; every live mock enumerator is updated
    // through the registry, so no dangling pointer is needed here.
    mock::setAppsOnAllInstances(std::move(apps));

    std::string json;
    {
        JsonObject object(json);
        object.boolean("ok", true);
    }
    return makeString(env, json);
}

// --------------------------------------------------------------------- module

napi_value Init(napi_env env, napi_value exports) {
    const napi_property_descriptor properties[] = {
        { "start", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "stop", nullptr, Stop, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setRules", nullptr, SetRules, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "apps", nullptr, Apps, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "stats", nullptr, Stats, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "readPcm", nullptr, ReadPcm, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "probe", nullptr, Probe, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "version", nullptr, Version, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setMockApps", nullptr, SetMockApps, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
    return exports;
}

} // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
