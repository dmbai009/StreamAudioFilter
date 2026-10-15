// StreamAudioFilter — engine / identity unit tests
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the real Engine against the mock backends, so the rule semantics
// (mode A / mode B), the live re-planning and the mixing plan are verified
// without Windows. Run with ./scripts/run-native-tests.sh

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../src/app_identity.h"
#include "../src/engine.h"
#include "../src/mock/mock_backend.h"

using namespace saf;
using namespace std::chrono;

static int failures = 0;
static int checks = 0;

#define CHECK(condition, ...)                                                  \
    do {                                                                       \
        checks++;                                                              \
        if (!(condition)) {                                                     \
            failures++;                                                        \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            std::printf("       " __VA_ARGS__);                                \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

static void section(const char* name) { std::printf("== %s\n", name); }

static mock::MockEnumerator::App makeApp(const char* key, const char* name, uint32_t pid, uint32_t parent = 0, bool playing = true) {
    mock::MockEnumerator::App app;
    app.appKey = key;
    app.displayName = name;
    app.exePath = std::string("C:\\Program Files\\") + name + "\\app.exe";
    app.exeName = "app.exe";
    app.pid = pid;
    app.parentPid = parent;
    app.playing = playing;
    return app;
}

static AppView* findApp(std::vector<AppView>& apps, const std::string& key) {
    for (auto& app : apps) {
        if (app.appKey == key) return &app;
    }
    return nullptr;
}

/** Waits until @p predicate is true, at most @p timeoutMs milliseconds. */
template <typename Predicate>
static bool waitFor(Predicate predicate, int timeoutMs = 1500) {
    const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
    while (steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(milliseconds(10));
    }
    return predicate();
}

// ------------------------------------------------------------------ identity

static void testAppIdentity() {
    section("app identity / grouping");

    // Equal display names must not merge, different paths must not merge.
    CHECK(makeAppKey("C:\\Games\\A\\game.exe", "game.exe", "", false, 1) != makeAppKey("D:\\Other\\game.exe", "game.exe", "", false, 2),
          "same file name, different path -> different keys");
    CHECK(makeAppKey("C:\\Games\\A\\game.exe", "game.exe", "", false, 1) == makeAppKey("C:\\Games\\A\\GAME.EXE", "game.exe", "", false, 99),
          "case insensitive, PID independent");
    CHECK(makeAppKey("", "WWAHost.exe", "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", false, 5) == "uwp:SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify",
          "UWP hosts are identified by their app id");
    CHECK(makeAppKey("", "", "", false, 42) == "pid:42", "PID fallback");
    CHECK(isEphemeralKey("pid:42") && !isEphemeralKey("exe:c:\\a.exe"), "ephemeral detection");
    CHECK(makeAppKey("", "", "", true, 4) == "system:sounds", "system sounds key");

    // Chrome: browser + audio utility + renderers must collapse into one entry
    // with exactly one capture root (the browser process).
    std::vector<SessionInfo> sessions;
    for (uint32_t pid : { 100u, 101u, 102u }) {
        SessionInfo info;
        info.sessionId = std::to_string(pid);
        info.pid = pid;
        info.exePath = "C:\\Program Files\\Google\\Chrome\\chrome.exe";
        info.exeName = "chrome.exe";
        info.displayName = "Google Chrome";
        info.appKey = makeAppKey(info.exePath, info.exeName, "", false, pid);
        info.playing = true;
        sessions.push_back(info);
    }
    // Two independent instances of the same game: two roots, no merging of the
    // trees (they are unrelated processes that happen to share a path).
    SessionInfo gameA;
    gameA.sessionId = "g1";
    gameA.pid = 200;
    gameA.exePath = "D:\\Games\\Game\\game.exe";
    gameA.exeName = "game.exe";
    gameA.displayName = "Game";
    gameA.appKey = makeAppKey(gameA.exePath, gameA.exeName, "", false, 200);
    sessions.push_back(gameA);
    SessionInfo gameB = gameA;
    gameB.sessionId = "g2";
    gameB.pid = 300;
    sessions.push_back(gameB);

    std::vector<ProcessNode> tree {
        { 100, 1 }, { 101, 100 }, { 102, 100 }, { 200, 1 }, { 300, 1 }
    };

    const auto groups = groupSessions(sessions, tree);
    CHECK(groups.size() == 2, "two applications, got %zu", groups.size());

    bool chromeChecked = false, gameChecked = false;
    for (const auto& group : groups) {
        if (group.appKey.rfind("exe:c:\\program files\\google\\chrome", 0) == 0) {
            chromeChecked = true;
            CHECK(group.pids.size() == 3, "chrome has 3 processes");
            CHECK(group.roots.size() == 1 && group.roots[0] == 100, "chrome has exactly one capture root");
            CHECK(group.displayName == "Google Chrome", "display name");
        } else {
            gameChecked = true;
            CHECK(group.pids.size() == 2, "game merged by path");
            CHECK(group.roots.size() == 2, "two unrelated instances -> two roots, got %zu", group.roots.size());
        }
    }
    CHECK(chromeChecked && gameChecked, "both applications grouped");

    // Roots stay deterministic for a deep chain.
    CHECK(computeCaptureRoots({ 5, 6, 7 }, { { 5, 1 }, { 6, 5 }, { 7, 6 } }) == std::vector<uint32_t> { 5 }, "deep chain root");
}

// -------------------------------------------------------------------- engine

struct EngineFixture {
    mock::MockEnumerator* enumerator = nullptr;
    mock::MockCaptureFactory* factory = nullptr;
    std::unique_ptr<Engine> engine;

    EngineFixture() {
        auto ownedEnumerator = std::make_unique<mock::MockEnumerator>();
        auto ownedFactory = std::make_unique<mock::MockCaptureFactory>();
        enumerator = ownedEnumerator.get();
        factory = ownedFactory.get();
        engine = std::make_unique<Engine>(EngineOptions { 48000, 2, 10, 8, 200, 100, 25 },
                                          std::move(ownedEnumerator), std::move(ownedFactory));

        std::string error;
        if (!engine->start(error)) {
            std::printf("  FATAL: engine did not start: %s\n", error.c_str());
            failures++;
        }
    }
};

static void testModeA() {
    section("mode A (all except selected)");

    EngineFixture fixture;
    fixture.enumerator->setApps({ makeApp("exe:game", "Game", 100, 1), makeApp("exe:spotify", "Spotify", 200, 1) });

    RuleSet rules;
    rules.mode = RuleSet::Mode::AllExcept;
    fixture.engine->setRules(rules);

    CHECK(waitFor([&] {
        auto apps = fixture.engine->apps();
        auto* game = findApp(apps, "exe:game");
        auto* spotify = findApp(apps, "exe:spotify");
        return game && spotify && game->captured && spotify->captured;
    }), "both applications captured by default");

    // A new application appears while streaming: it must be captured without
    // any user interaction (requirement: new apps are transmitted).
    auto apps = fixture.enumerator->enumerate();
    (void)apps;
    fixture.enumerator->setApps({
        makeApp("exe:game", "Game", 100, 1),
        makeApp("exe:spotify", "Spotify", 200, 1),
        makeApp("exe:browser", "Browser", 300, 1),
    });

    CHECK(waitFor([&] {
        auto list = fixture.engine->apps();
        auto* browser = findApp(list, "exe:browser");
        return browser && browser->captured;
    }), "newly started application is transmitted automatically");

    // Turn Spotify off: its stream must fade out and disappear, everything
    // else keeps playing.
    rules.overrides["exe:spotify"] = false;
    fixture.engine->setRules(rules);

    CHECK(waitFor([&] {
        auto list = fixture.engine->apps();
        auto* spotify = findApp(list, "exe:spotify");
        auto* game = findApp(list, "exe:game");
        return spotify && !spotify->captured && spotify->state == "excluded" && game && game->captured;
    }), "excluded application stops being captured, others unaffected");

    // ... and back on again, without restarting anything.
    rules.overrides["exe:spotify"] = true;
    fixture.engine->setRules(rules);
    CHECK(waitFor([&] {
        auto list = fixture.engine->apps();
        auto* spotify = findApp(list, "exe:spotify");
        return spotify && spotify->captured && spotify->state == "transmitting";
    }), "application can be re-enabled live");

    // The output must have been continuous: audio keeps flowing while rules
    // change (no restart of the engine, no stalled stream).
    const auto before = fixture.engine->stats().outputFrames;
    std::this_thread::sleep_for(milliseconds(150));
    const auto after = fixture.engine->stats().outputFrames;
    CHECK(after - before >= 48000 / 100 * 10, "output stays continuous during rule changes (%llu frames)",
          static_cast<unsigned long long>(after - before));
}

static void testModeB() {
    section("mode B (only selected)");

    EngineFixture fixture;
    fixture.enumerator->setApps({ makeApp("exe:game", "Game", 100, 1), makeApp("exe:spotify", "Spotify", 200, 1) });

    RuleSet rules;
    rules.mode = RuleSet::Mode::OnlySelected;
    fixture.engine->setRules(rules);

    std::this_thread::sleep_for(milliseconds(200));
    {
        auto list = fixture.engine->apps();
        auto* game = findApp(list, "exe:game");
        CHECK(game && !game->captured, "nothing is captured by default in mode B");
        CHECK(game && game->state == "excluded", "state is excluded");
    }

    rules.overrides["exe:game"] = true;
    fixture.engine->setRules(rules);
    CHECK(waitFor([&] {
        auto list = fixture.engine->apps();
        auto* game = findApp(list, "exe:game");
        auto* spotify = findApp(list, "exe:spotify");
        return game && game->captured && spotify && !spotify->captured;
    }), "only the allowed application is captured");

    // Mode switch back and forth must not reset the stored choice.
    rules.mode = RuleSet::Mode::AllExcept;
    fixture.engine->setRules(rules);
    CHECK(waitFor([&] {
        auto list = fixture.engine->apps();
        auto* game = findApp(list, "exe:game");
        auto* spotify = findApp(list, "exe:spotify");
        return game && game->captured && spotify && spotify->captured;
    }), "mode switch keeps rules and applies mode defaults");
    CHECK(rules.overrides.size() == 1 && rules.overrides.at("exe:game"), "overrides untouched by mode switch");
}

static void testClosingApp() {
    section("applications closing / disappearing");

    EngineFixture fixture;
    fixture.enumerator->setApps({ makeApp("exe:game", "Game", 100, 1), makeApp("exe:music", "Music", 200, 1) });

    RuleSet rules;
    rules.mode = RuleSet::Mode::AllExcept;
    fixture.engine->setRules(rules);
    CHECK(waitFor([&] { return fixture.engine->stats().activeStreams == 2; }), "two streams running");

    // The music application closes (its session disappears).
    fixture.enumerator->setApps({ makeApp("exe:game", "Game", 100, 1) });
    CHECK(waitFor([&] {
        auto list = fixture.engine->apps();
        return findApp(list, "exe:music") == nullptr && findApp(list, "exe:game") != nullptr;
    }), "closed application is removed from the list");

    CHECK(waitFor([&] { return fixture.engine->stats().activeStreams == 1; }), "its stream is stopped, got %zu",
          fixture.engine->stats().activeStreams);
    std::this_thread::sleep_for(milliseconds(100));
    CHECK(fixture.engine->readFrames(std::vector<float>(2048).data(), 1024) > 0, "engine still produces audio");
}

static void testStreamLimit() {
    section("stream limit");

    EngineFixture fixture;
    std::vector<mock::MockEnumerator::App> apps;
    for (int i = 0; i < 12; i++) apps.push_back(makeApp(("exe:app" + std::to_string(i)).c_str(), ("App" + std::to_string(i)).c_str(), 100 + i, 1));
    fixture.enumerator->setApps(apps);

    RuleSet rules;
    fixture.engine->setRules(rules);

    CHECK(waitFor([&] { return fixture.engine->stats().activeStreams == 8; }), "limit respected, got %zu",
          fixture.engine->stats().activeStreams);

    const auto stats = fixture.engine->stats();
    const bool warned = std::any_of(stats.warnings.begin(), stats.warnings.end(), [](const std::string& warning) {
        return warning.find("предел") != std::string::npos;
    });
    CHECK(warned, "user visible warning about the limit");
}

static void testMixingLevels() {
    section("mixing output");

    EngineFixture fixture;
    fixture.enumerator->setApps({ makeApp("exe:a", "A", 100, 1), makeApp("exe:b", "B", 200, 1), makeApp("exe:c", "C", 300, 1) });
    RuleSet rules;
    fixture.engine->setRules(rules);

    CHECK(waitFor([&] { return fixture.engine->stats().activeStreams == 3; }, 2000), "three streams captured");
    std::this_thread::sleep_for(milliseconds(200));

    std::vector<float> buffer(48000);
    const size_t frames = fixture.engine->readFrames(buffer.data(), 4800);
    CHECK(frames > 0, "output available");

    float peak = 0;
    for (size_t i = 0; i < frames * 2; i++) peak = std::max(peak, std::fabs(buffer[i]));
    CHECK(peak <= 1.0f, "mixed output never exceeds full scale, peak %f", peak);
    CHECK(peak > 0.1f, "mixed output has signal, peak %f", peak);

    const auto stats = fixture.engine->stats();
    CHECK(stats.starvedFrames == 0, "no starvation with a healthy backend, got %llu",
          static_cast<unsigned long long>(stats.starvedFrames));

    // The output stream is one continuous entity: a blocking read pattern of
    // the renderer (2 pending requests of 10 ms) never sees a hole.
    size_t total = 0;
    for (int i = 0; i < 20; i++) {
        std::this_thread::sleep_for(milliseconds(10));
        total += fixture.engine->readFrames(buffer.data(), 480);
    }
    CHECK(total >= 480 * 15, "sustained output, got %zu frames", total);
}

int main() {
    std::printf("StreamAudioFilter engine tests\n\n");
    testAppIdentity();
    testModeA();
    testModeB();
    testClosingApp();
    testStreamLimit();
    testMixingLevels();
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
