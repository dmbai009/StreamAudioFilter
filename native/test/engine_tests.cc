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
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../src/app_identity.h"
#include "../src/capture_group.h"
#include "../src/engine.h"
#include "../src/hr_advice.h"
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

constexpr double kPi = 3.14159265358979323846;

/**
 * Magnitude of @p frequency in an interleaved stereo buffer (Goertzel).
 * For a sine of amplitude A it returns about A, so the result is directly
 * comparable with the levels the mock backend produces (0.25 per instance).
 */
static double toneMagnitude(const std::vector<float>& interleaved, int sampleRate, double frequency) {
    const size_t frames = interleaved.size() / 2;
    if (frames == 0) return 0.0;

    const double omega = 2.0 * kPi * frequency / sampleRate;
    const double coefficient = 2.0 * std::cos(omega);
    double s1 = 0.0, s2 = 0.0;

    for (size_t i = 0; i < frames; i++) {
        const double sample = (interleaved[i * 2] + interleaved[i * 2 + 1]) * 0.5;
        const double s0 = sample + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, power)) / static_cast<double>(frames);
}

/** Reads exactly @p frames frames, waiting for the engine to produce them. */
static std::vector<float> readFrames(Engine& engine, size_t frames, int timeoutMs = 1500) {
    std::vector<float> buffer(frames * 2, 0.0f);
    size_t total = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (total < frames && std::chrono::steady_clock::now() < deadline) {
        const size_t got = engine.readFrames(buffer.data() + total * 2, frames - total);
        if (got == 0) {
            std::this_thread::sleep_for(milliseconds(10));
            continue;
        }
        total += got;
    }
    buffer.resize(total * 2);
    return buffer;
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

/**
 * Real production wiring: one capture per process tree (the mock backend behaves
 * like WASAPI process loopback here), wrapped by the grouping factory that
 * aggregates the instances of one application.
 */
struct EngineFixture {
    mock::MockEnumerator* enumerator = nullptr;
    mock::MockCaptureFactory* factory = nullptr;
    std::unique_ptr<Engine> engine;

    static EngineOptions defaults() { return EngineOptions { 48000, 2, 10, 8, 200, 100, 25 }; }

    explicit EngineFixture(EngineOptions options = defaults()) {
        auto ownedEnumerator = std::make_unique<mock::MockEnumerator>();
        auto ownedFactory = std::make_unique<mock::MockCaptureFactory>();
        enumerator = ownedEnumerator.get();
        factory = ownedFactory.get();
        engine = std::make_unique<Engine>(options,
                                          std::move(ownedEnumerator),
                                          std::make_unique<GroupingCaptureFactory>(std::move(ownedFactory)));

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
    rules.active = true;
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
    rules.active = true;
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
    rules.active = true;
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
    rules.active = true;
    fixture.engine->setRules(rules);

    CHECK(waitFor([&] { return fixture.engine->stats().activeStreams == 8; }), "limit respected, got %zu",
          fixture.engine->stats().activeStreams);

    const auto stats = fixture.engine->stats();
    const bool warned = std::any_of(stats.warnings.begin(), stats.warnings.end(), [](const std::string& warning) {
        return warning.find("limit") != std::string::npos;
    });
    CHECK(warned, "user visible warning about the limit");
}

static void testMixingLevels() {
    section("mixing output");

    EngineFixture fixture;
    fixture.enumerator->setApps({ makeApp("exe:a", "A", 100, 1), makeApp("exe:b", "B", 200, 1), makeApp("exe:c", "C", 300, 1) });
    RuleSet rules;
    rules.active = true;
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

static void testCaptureGate() {
    section("capture gate (plugin must be quiet while nothing is shared)");

    EngineFixture fixture;
    fixture.enumerator->setApps({ makeApp("exe:game", "Game", 100, 1), makeApp("exe:music", "Music", 200, 1) });

    RuleSet rules;
    rules.mode = RuleSet::Mode::AllExcept;
    rules.active = false;
    fixture.engine->setRules(rules);

    // The application list must still be visible (the panels need it), but no
    // audio may be captured at all.
    CHECK(waitFor([&] {
        auto list = fixture.engine->apps();
        return list.size() == 2;
    }), "applications are listed while inactive");
    std::this_thread::sleep_for(milliseconds(150));
    CHECK(fixture.engine->stats().activeStreams == 0, "nothing captured while inactive, got %zu",
          fixture.engine->stats().activeStreams);

    rules.active = true;
    fixture.engine->setRules(rules);
    CHECK(waitFor([&] { return fixture.engine->stats().activeStreams == 2; }), "capture starts when the gate opens");

    rules.active = false;
    fixture.engine->setRules(rules);
    CHECK(waitFor([&] { return fixture.engine->stats().activeStreams == 0; }, 2000), "capture stops when the gate closes, got %zu",
          fixture.engine->stats().activeStreams);
    std::this_thread::sleep_for(milliseconds(100));
    CHECK(fixture.engine->stats().outputFrames > 0, "output stream stays alive (no restart needed)");
}

static void testRootGrouping() {
    section("multi instance applications: roots, splitting, no duplication");

    // Exact duplicates can not cause a second capture.
    CHECK(uniqueRoots({ 100, 100, 300, 100 }) == (std::vector<uint32_t> { 100, 300 }), "duplicate roots collapsed");
    CHECK(uniqueRoots({ 0, 42 }) == (std::vector<uint32_t> { 42 }), "root 0 is not a process");

    // One application, two unrelated instances -> two roots; a child process is
    // covered by its parent's tree and must not become a root of its own.
    const std::vector<ProcessNode> tree { { 100, 1 }, { 101, 100 }, { 302, 1 } };
    (void)tree;
    CHECK(computeCaptureRoots({ 100, 101, 302 }, tree) == (std::vector<uint32_t> { 100, 302 }),
        "two unrelated instances are both roots, the child is not");

    // The grouping factory turns that into one capture per tree and mixes them.
    auto inner = std::make_unique<mock::MockCaptureFactory>();
    auto* innerPtr = inner.get();
    GroupingCaptureFactory factory(std::move(inner));

    CaptureRequest request;
    request.appKey = "exe:game";
    request.displayName = "Game";
    request.rootPids = { 100, 101, 302 };
    request.sampleRate = 48000;
    request.channels = 2;

    auto grouped = factory.create(request);
    CHECK(grouped != nullptr, "grouped capture created");
    CHECK(innerPtr->createdSources() == 3, "one capture per root pid, got %zu", innerPtr->createdSources());
    CHECK(factory.createdParts() == 3, "parts counted, got %zu", factory.createdParts());
    CHECK(grouped->buffer() != nullptr, "grouped capture exposes one output buffer");
    CHECK(grouped->formatDescription().find("3") != std::string::npos, "format description mentions the instances: %s",
        grouped->formatDescription().c_str());

    // A single root request stays a pass through: no extra capture, no extra
    // mixing stage, so the common case costs nothing.
    auto single = std::make_unique<mock::MockCaptureFactory>();
    auto* singlePtr = single.get();
    GroupingCaptureFactory singleFactory(std::move(single));
    CaptureRequest oneRoot = request;
    oneRoot.rootPids = { 100 };
    auto passthrough = singleFactory.create(oneRoot);
    CHECK(passthrough != nullptr && singlePtr->createdSources() == 1, "single root is a pass through");

    CaptureRequest noRoots = request;
    noRoots.rootPids.clear();
    CHECK(singleFactory.create(noRoots) == nullptr, "a request without roots is refused");
}

static void testMultiInstanceEngine() {
    section("engine: two instances of one application");

    EngineFixture fixture;
    const auto first = makeApp("exe:game", "Game", 100, 1);
    auto second = makeApp("exe:game", "Game", 302, 1);

    fixture.enumerator->setApps({ first });
    RuleSet rules;
    rules.active = true;
    rules.mode = RuleSet::Mode::OnlySelected;
    rules.overrides["exe:game"] = true;
    fixture.engine->setRules(rules);

    const double toneFirst = fixture.factory->toneForPid(100);
    const double toneSecond = fixture.factory->toneForPid(302);
    CHECK(std::fabs(toneFirst - toneSecond) > 1.0, "the two instances use different tones (%f / %f)", toneFirst, toneSecond);

    CHECK(waitFor([&] { return fixture.engine->stats().activeStreams == 1; }), "one stream for one application");

    {
        auto apps = fixture.engine->apps();
        auto* game = findApp(apps, "exe:game");
        CHECK(game && game->instances == 1, "still a single instance");
    }

    // The level one instance produces on its own. Everything after the restart is
    // compared against this instead of a hard coded number, so the check does not
    // depend on how loud the mock renders its tone - and it pins down that the
    // instance that is *not* running yet is not in the mix either.
    // The first window after the stream appeared can still contain its 20 ms fade in,
    // so it is read and dropped before the reference is measured.
    readFrames(*fixture.engine, 4800, 500);
    const auto referenceAudio = readFrames(*fixture.engine, 9600, 500);
    const double referenceLevel = toneMagnitude(referenceAudio, 48000, toneFirst);
    const double low = referenceLevel * 0.8;
    const double high = referenceLevel * 1.2;
    CHECK(referenceLevel > 0.12 && toneMagnitude(referenceAudio, 48000, toneSecond) < 0.05,
          "the single instance is audible on its own and the second one is not in the mix yet (%.3f / %.3f)",
          referenceLevel, toneMagnitude(referenceAudio, 48000, toneSecond));

    // The second instance is started while the first one is already streaming.
    fixture.enumerator->setApps({ first, second });

    CHECK(waitFor([&] {
        auto apps = fixture.engine->apps();
        auto* game = findApp(apps, "exe:game");
        return game && game->instances == 2 && game->captured;
    }, 4000), "the capture follows the new instance");

    // Both instances must be audible, from one stream (the engine must not mix the
    // application twice), each at the level one instance produces.
    //
    // "Settled" means two consecutive windows in which both tones are close to the
    // reference level - not the first moment they cross some low threshold. A
    // window that merely crosses a threshold can still sit inside the change: the
    // old capture fades out while the new one fills its buffers, so a tone can be
    // at half its level there. Comparing magnitudes in such a window is what made
    // this check flaky on a loaded Windows runner, where it failed with 0.122 vs
    // 0.254 right after having "settled" at 0.151 / 0.146 (build 19045, MSVC,
    // 2026-10-16). The quick-settle bound is therefore measured to the settled
    // state, which is what "the application is quiet for about a tenth of a second"
    // in docs/LIMITATIONS.md actually describes.
    const auto restartStart = steady_clock::now();
    double windowFirst = 0.0;
    double windowSecond = 0.0;
    double previousFirst = 0.0;
    double previousSecond = 0.0;
    double peakSum = 0.0;
    bool settled = false;
    while (steady_clock::now() - restartStart < seconds(8)) {
        const auto audio = readFrames(*fixture.engine, 4800, 500);
        previousFirst = windowFirst;
        previousSecond = windowSecond;
        windowFirst = toneMagnitude(audio, 48000, toneFirst);
        windowSecond = toneMagnitude(audio, 48000, toneSecond);
        peakSum = std::max(peakSum, windowFirst + windowSecond);
        const auto atLevel = [&](double value, double previous) {
            return value > low && value < high && previous > low && previous < high;
        };
        if (atLevel(windowFirst, previousFirst) && atLevel(windowSecond, previousSecond)) {
            settled = true;
            break;
        }
    }
    const auto settleMs = duration_cast<milliseconds>(steady_clock::now() - restartStart).count();
    CHECK(settled, "both instances settle at their own level (%.3f / %.3f after %lld ms)", windowFirst, windowSecond,
          static_cast<long long>(settleMs));
    CHECK(settleMs < 3000, "the restart settles quickly (%lld ms)", static_cast<long long>(settleMs));
    // No window during the whole change may contain an instance twice: one instance
    // reads the reference level, so a doubled capture would push the sum of the two
    // tones towards twice that.
    CHECK(peakSum < referenceLevel * 2.9, "no instance is captured twice during the restart (peak sum %.3f, one instance reads %.3f)",
          peakSum, referenceLevel);
    std::printf("  restart took %lld ms, settled levels %.3f / %.3f (one instance reads %.3f, peak sum %.3f)\n",
                static_cast<long long>(settleMs), windowFirst, windowSecond, referenceLevel, peakSum);

    // The settled state, on windows read after the restart finished. The mock
    // sources run on real time threads: on a loaded CI runner a single window
    // can lose a few packets (seen: 0.254 / 0.191 on windows-latest), which is
    // scheduling, not mixing. So the instances must reach their own level in
    // one of up to five windows, while no window may ever show a doubled one.
    double levelFirst = 0.0, levelSecond = 0.0, maxSeen = 0.0;
    for (int attempt = 0; attempt < 5; attempt++) {
        const auto audio = readFrames(*fixture.engine, 9600);
        levelFirst = toneMagnitude(audio, 48000, toneFirst);
        levelSecond = toneMagnitude(audio, 48000, toneSecond);
        maxSeen = std::max({ maxSeen, levelFirst, levelSecond });
        if (levelFirst > low && levelFirst < high && levelSecond > low && levelSecond < high) break;
    }
    CHECK(maxSeen < 0.45, "no window shows a doubled level (max %.3f)", maxSeen);
    CHECK(levelFirst > low && levelFirst < high && levelSecond > low && levelSecond < high,
          "both instances at their own level (%.3f / %.3f, one instance reads %.3f)", levelFirst, levelSecond, referenceLevel);
    CHECK(std::fabs(levelFirst - levelSecond) < referenceLevel * 0.35, "neither instance is captured twice (%.3f vs %.3f)",
          levelFirst, levelSecond);
    CHECK(levelFirst < 0.45 && levelSecond < 0.45, "no doubled level (%.3f / %.3f)", levelFirst, levelSecond);
    CHECK(fixture.engine->stats().activeStreams == 1, "still exactly one stream, got %zu",
        fixture.engine->stats().activeStreams);

    // ... and back to one instance: the closed one must disappear from the
    // capture without touching the remaining instance.
    fixture.enumerator->setApps({ second });
    CHECK(waitFor([&] {
        auto apps = fixture.engine->apps();
        auto* game = findApp(apps, "exe:game");
        return game && game->instances == 1 && game->captured;
    }, 4000), "the capture follows a closed instance");

    CHECK(waitFor([&] {
        const auto audio2 = readFrames(*fixture.engine, 9600, 500);
        return toneMagnitude(audio2, 48000, toneSecond) > 0.1 && toneMagnitude(audio2, 48000, toneFirst) < 0.05;
    }, 4000), "only the remaining instance is transmitted");
}

static void testNestedInstancesOnce() {
    section("engine: child process is not captured as a second instance");

    EngineFixture fixture;
    // Both sessions belong to the same application, the second one is a child
    // of the first: capturing both trees would double that application's audio.
    fixture.enumerator->setApps({ makeApp("exe:game", "Game", 100, 1), makeApp("exe:game", "Game", 101, 100) });

    RuleSet rules;
    rules.active = true;
    rules.mode = RuleSet::Mode::OnlySelected;
    rules.overrides["exe:game"] = true;
    fixture.engine->setRules(rules);

    CHECK(waitFor([&] { return fixture.engine->stats().activeStreams == 1; }), "one stream");
    CHECK(waitFor([&] {
        auto apps = fixture.engine->apps();
        auto* game = findApp(apps, "exe:game");
        return game && game->captured && game->instances == 1;
    }), "child of the same tree is covered by the parent capture");
    CHECK(fixture.factory->createdSources() == 1, "exactly one capture created, got %zu", fixture.factory->createdSources());
}

/**
 * The diagnostics are not decoration: the plugin's whole honesty story rests on
 * naming the real cause of a failure. 0x8000000E in particular was reported to a
 * user as if it were a Windows version problem, while it actually means "the
 * completion handler is not an agile object" — a bug in the plugin, raised before
 * Windows even looks at the build number.
 */
static void testDiagnostics() {
    section("diagnostics");

    struct Known {
        int32_t code;
        const char* name;
    };

    const Known known[] = {
        { hr::Ok, "S_OK" },
        { hr::IllegalMethodCall, "E_ILLEGAL_METHOD_CALL" },
        { hr::Win32InvalidParameter, "E_INVALIDARG" },
        { hr::Fail, "E_FAIL" },
        { hr::NoInterface, "E_NOINTERFACE" },
        { hr::ComNotInitialized, "CO_E_NOTINITIALIZED" },
        { hr::RpcChangedMode, "RPC_E_CHANGED_MODE" },
        { hr::AudclntUnsupportedFormat, "AUDCLNT_E_UNSUPPORTED_FORMAT" },
        { hr::AudclntServiceNotRunning, "AUDCLNT_E_SERVICE_NOT_RUNNING" },
        { hr::AudclntDeviceInvalidated, "AUDCLNT_E_DEVICE_INVALIDATED" },
    };

    for (const Known& entry : known) {
        const HresultAdvice advice = adviseHresult(entry.code);
        CHECK(advice.name == entry.name, "%s: expected name %s", hresultHex(entry.code).c_str(), entry.name);
        CHECK(!advice.advice.empty(), "%s has no explanation text", entry.name);
    }

    CHECK(hresultHex(hr::IllegalMethodCall) == "0x8000000E", "hex: %s", hresultHex(hr::IllegalMethodCall).c_str());
    CHECK(hresultHex(hr::Ok) == "0x00000000", "hex: %s", hresultHex(hr::Ok).c_str());

    const HresultAdvice illegal = adviseHresult(hr::IllegalMethodCall);
    CHECK(illegal.advice.find("agile") != std::string::npos,
        "the explanation of 0x8000000E must name the agile object requirement: %s", illegal.advice.c_str());
    // The explanation must say that the failure precedes any consideration of the
    // system version — that is the whole point of the original misdiagnosis: the
    // user was told the OS build was the problem.
    CHECK(illegal.advice.find("OS build") != std::string::npos,
        "0x8000000E must state that the code is raised before the OS build matters: %s", illegal.advice.c_str());
    CHECK(illegal.advice.find("unsupported") != std::string::npos,
        "0x8000000E must rule out the version explanation explicitly: %s", illegal.advice.c_str());

    const std::string described = describeHresult(hr::IllegalMethodCall);
    CHECK(described.find("E_ILLEGAL_METHOD_CALL") != std::string::npos && described.find("0x8000000E") != std::string::npos,
        "describeHresult must carry the name and the number: %s", described.c_str());

    // Codes the table does not know keep their number and are still classified.
    const HresultAdvice unknownAudio = adviseHresult(static_cast<int32_t>(0x88890099u));
    CHECK(unknownAudio.name == "AUDCLNT_E_<unknown>", "unknown audio code: %s", unknownAudio.name.c_str());
    CHECK(adviseHresult(static_cast<int32_t>(0x80070999u)).name.find("HRESULT_FROM_WIN32(") != std::string::npos,
        "a wrapped Win32 error must be named as one");
    const HresultAdvice unknown = adviseHresult(static_cast<int32_t>(0x8004ABCDu));
    CHECK(unknown.name.empty() && unknown.advice.empty(), "an unknown code has no invented name");

    const std::string fallback = describeHresult(static_cast<int32_t>(0x8004ABCDu));
    CHECK(fallback.find("0x8004ABCD") != std::string::npos && fallback.find("unknown") != std::string::npos,
        "an unknown code must still be quoted: %s", fallback.c_str());
}


// ------------------------------------------------------- process tree planning

static AppGroup makeGroup(const char* key, const char* name, std::vector<uint32_t> pids, std::vector<uint32_t> roots) {
    AppGroup group;
    group.appKey = key;
    group.displayName = name;
    group.pids = std::move(pids);
    group.roots = std::move(roots);
    return group;
}

static void testTreePlanning() {
    section("process trees across applications");

    // launcher (10) started game (11); an unrelated player runs at 30.
    const std::vector<ProcessNode> tree = { { 10, 1 }, { 11, 10 }, { 30, 1 }, { 500, 1 }, { 501, 500 } };
    std::vector<AppGroup> groups = {
        makeGroup("exe:launcher", "Launcher", { 10 }, { 10 }),
        makeGroup("exe:game", "Game", { 11 }, { 11 }),
        makeGroup("exe:player", "Player", { 30 }, { 30 }),
    };

    auto plansFor = [&](std::set<std::string> transmitted) {
        return planCaptures(groups, tree, [&](const std::string& key) { return transmitted.count(key) > 0; });
    };

    {
        // The game is excluded but lives inside the launcher's tree: capturing
        // the launcher would leak the game, so the launcher is blocked.
        auto plans = plansFor({ "exe:launcher", "exe:player" });
        CHECK(!plans["exe:launcher"].blockedReason.empty(), "a transmitted parent of an excluded app is blocked");
        CHECK(plans["exe:launcher"].blockedReason.find("Game") != std::string::npos, "the reason names the excluded app");
        CHECK(plans["exe:launcher"].roots.empty(), "a blocked application has nothing to capture");
        CHECK(plans["exe:player"].blockedReason.empty() && plans["exe:player"].roots == std::vector<uint32_t> { 30 },
              "unrelated applications are unaffected");
        CHECK(plans.count("exe:game") == 0, "excluded applications get no plan");
    }
    {
        // Both transmitted: the game is already inside the launcher's capture.
        auto plans = plansFor({ "exe:launcher", "exe:game" });
        CHECK(plans["exe:launcher"].roots == std::vector<uint32_t> { 10 }, "the parent is captured");
        CHECK(plans["exe:game"].roots.empty() && plans["exe:game"].coveredBy == "Launcher",
              "the child is covered, not captured twice");
    }
    {
        // Excluded parent, transmitted child: the child's own tree has nothing
        // excluded in it.
        auto plans = plansFor({ "exe:game" });
        CHECK(plans["exe:game"].blockedReason.empty() && plans["exe:game"].roots == std::vector<uint32_t> { 11 },
              "a child can be sent on its own while its parent is excluded");
    }
    {
        // An application with one instance under the launcher and one on its own.
        std::vector<AppGroup> partial = {
            makeGroup("exe:launcher", "Launcher", { 10 }, { 10 }),
            makeGroup("exe:game", "Game", { 11, 30 }, { 11, 30 }),
        };
        auto plans = planCaptures(partial, tree, [](const std::string&) { return true; });
        CHECK(plans["exe:game"].roots == std::vector<uint32_t> { 30 } && plans["exe:game"].coveredBy.empty(),
              "only the uncovered instance gets its own capture");
    }
    {
        // Discord (500) and its voice process (501) are protected.
        std::vector<AppGroup> withDiscord = groups;
        withDiscord.push_back(makeGroup("exe:discord", "Discord", { 501 }, { 501 }));
        protectProcessTrees(withDiscord, tree, { 500 }, "protected");
        CHECK(!withDiscord.back().capturable && withDiscord.back().notCapturableReason == "protected",
              "Discord's processes are never capturable");
        CHECK(withDiscord[0].capturable && withDiscord[2].capturable, "other applications stay capturable");

        // Discord started by the launcher: the launcher must not be captured.
        const std::vector<ProcessNode> nested = { { 10, 1 }, { 500, 10 }, { 501, 500 } };
        std::vector<AppGroup> nestedGroups = {
            makeGroup("exe:launcher", "Launcher", { 10 }, { 10 }),
            makeGroup("exe:discord", "Discord", { 501 }, { 501 }),
        };
        protectProcessTrees(nestedGroups, nested, { 500 }, "protected");
        auto plans = planCaptures(nestedGroups, nested, [](const std::string&) { return true; });
        CHECK(!plans["exe:launcher"].blockedReason.empty(), "a launcher that started Discord is blocked (voice would leak)");
    }
    CHECK(isInProcessTree(11, 10, tree) && isInProcessTree(10, 10, tree) && !isInProcessTree(10, 11, tree)
              && !isInProcessTree(999, 10, tree),
          "process tree membership");
}

static void testEngineTreeConflicts() {
    section("engine: excluded child inside a transmitted tree");

    EngineFixture fixture;
    fixture.enumerator->setApps({ makeApp("exe:launcher", "Launcher", 100, 1), makeApp("exe:game", "Game", 101, 100) });

    RuleSet rules;
    rules.active = true;
    rules.mode = RuleSet::Mode::AllExcept;
    rules.overrides["exe:game"] = false;
    fixture.engine->setRules(rules);

    CHECK(waitFor([&] {
        auto apps = fixture.engine->apps();
        auto* launcher = findApp(apps, "exe:launcher");
        auto* game = findApp(apps, "exe:game");
        return launcher && game && launcher->state == "blocked" && !launcher->captured && game->state == "excluded";
    }), "the launcher is blocked, nothing of the excluded game can leak");

    rules.overrides["exe:game"] = true;
    fixture.engine->setRules(rules);
    CHECK(waitFor([&] {
        auto apps = fixture.engine->apps();
        auto* launcher = findApp(apps, "exe:launcher");
        auto* game = findApp(apps, "exe:game");
        return launcher && game && launcher->captured && game->state == "covered" && !game->captured;
    }), "allowing the game unblocks the launcher; the game is covered by its capture");
    CHECK(fixture.engine->stats().activeStreams == 1, "exactly one capture for the shared tree");
}

static void testHiddenAndProtected() {
    section("engine: own output hidden, Discord protected");

    EngineOptions options = EngineFixture::defaults();
    options.selfPid = 300;
    options.protectedRoots = { 400 };
    EngineFixture fixture(options);
    fixture.enumerator->setApps({
        makeApp("exe:game", "Game", 100, 1),
        makeApp("exe:host", "Host", 300, 401),
        makeApp("exe:discord", "Discord", 401, 400),
    });

    RuleSet rules;
    rules.active = true;
    rules.mode = RuleSet::Mode::AllExcept;
    fixture.engine->setRules(rules);

    CHECK(waitFor([&] {
        auto apps = fixture.engine->apps();
        auto* game = findApp(apps, "exe:game");
        auto* discord = findApp(apps, "exe:discord");
        return game && game->captured && discord && discord->state == "unsupported" && !discord->captured
            && !findApp(apps, "exe:host");
    }), "the host is not listed, Discord is listed but never captured");
    CHECK(fixture.engine->stats().activeStreams == 1, "only the game is captured");
}

static void testPullMode() {
    section("engine: output device clock (pull)");

    EngineOptions options = EngineFixture::defaults();
    options.externalClock = true;
    EngineFixture fixture(options);
    fixture.enumerator->setApps({ makeApp("exe:game", "Game", 100, 1) });

    RuleSet rules;
    rules.active = true;
    fixture.engine->setRules(rules);
    CHECK(waitFor([&] {
        auto apps = fixture.engine->apps();
        auto* game = findApp(apps, "exe:game");
        return game && game->captured;
    }), "captured in pull mode");

    // Pull like a device would: odd sized chunks, in real time.
    const double tone = fixture.factory->toneForPid(100);
    std::vector<float> chunk(441 * 2), recent;
    const auto start = steady_clock::now();
    size_t pulled = 0, shortPulls = 0;
    while (steady_clock::now() - start < milliseconds(800)) {
        if (fixture.engine->pull(chunk.data(), 441) != 441) shortPulls++;
        pulled += 441;
        if (steady_clock::now() - start > milliseconds(500)) recent.insert(recent.end(), chunk.begin(), chunk.end());
        std::this_thread::sleep_until(start + microseconds(static_cast<long long>(pulled) * 1000000 / 48000));
    }
    CHECK(shortPulls == 0, "pull always delivers what is asked (%zu short pulls)", shortPulls);
    const double level = toneMagnitude(recent, 48000, tone);
    std::printf("  pull mode level %.3f over %zu frames\n", level, recent.size() / 2);
    CHECK(level > 0.2 && level < 0.3, "the application is heard at its level through pull (%.3f)", level);

    // No puller: a rule change must still release the capture.
    rules.overrides["exe:game"] = false;
    fixture.engine->setRules(rules);
    CHECK(waitFor([&] { return fixture.engine->stats().activeStreams == 0; }, 2000),
          "without an output the excluded capture is released anyway");
}

static void testDeadCaptureRestarts() {
    section("engine: a capture that dies is replaced");

    EngineFixture fixture;
    fixture.enumerator->setApps({ makeApp("exe:game", "Game", 100, 1) });
    RuleSet rules;
    rules.active = true;
    fixture.engine->setRules(rules);
    CHECK(waitFor([&] {
        auto apps = fixture.engine->apps();
        auto* game = findApp(apps, "exe:game");
        return game && game->captured;
    }), "captured");

    const size_t before = fixture.factory->createdSources();
    CHECK(mock::killCaptures(100) == 1, "the capture was killed");
    CHECK(waitFor([&] {
        auto apps = fixture.engine->apps();
        auto* game = findApp(apps, "exe:game");
        return fixture.factory->createdSources() > before && game && game->captured && game->error.empty();
    }, 4000), "a new capture replaces the dead one (created %zu -> %zu)", before, fixture.factory->createdSources());
}

static void testAppsWhileRetiring() {
    section("engine: apps() while captures are retired");

    // Regression for a use after free: apps() used to keep raw stream pointers
    // past the lock while the watch thread destroyed retired streams.
    EngineFixture fixture;
    fixture.enumerator->setApps({ makeApp("exe:a", "A", 100, 1), makeApp("exe:b", "B", 200, 1), makeApp("exe:c", "C", 300, 1) });
    RuleSet rules;
    rules.active = true;

    std::atomic<bool> stop { false };
    std::thread reader([&] {
        while (!stop) {
            auto apps = fixture.engine->apps();
            (void)apps;
            (void)fixture.engine->stats();
        }
    });
    for (int i = 0; i < 40; i++) {
        rules.overrides["exe:a"] = (i % 2) == 0;
        rules.overrides["exe:b"] = (i % 3) == 0;
        fixture.engine->setRules(rules);
        std::this_thread::sleep_for(milliseconds(15));
    }
    stop = true;
    reader.join();
    CHECK(true, "no crash while toggling captures under concurrent apps()");
}

int main() {
    std::printf("StreamAudioFilter engine tests\n\n");
    testAppIdentity();
    testModeA();
    testModeB();
    testClosingApp();
    testStreamLimit();
    testMixingLevels();
    testRootGrouping();
    testMultiInstanceEngine();
    testNestedInstancesOnce();
    testCaptureGate();
    testDiagnostics();
    testTreePlanning();
    testEngineTreeConflicts();
    testHiddenAndProtected();
    testPullMode();
    testDeadCaptureRestarts();
    testAppsWhileRetiring();
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
