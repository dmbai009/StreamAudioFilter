// StreamAudioFilter — native audio core
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the filtered mix goes.
//
// Discord Desktop captures screen share audio natively, inside discord_voice:
// no JavaScript stream ever carries it. What the plugin *can* choose is which
// process Discord captures (the "soundshare" pid). So the mix is played by
// this host process into a session that is muted locally, and Discord is
// pointed at this process. Discord's hook based soundshare reads the audio an
// application hands to Windows *before* the session volume is applied, so the
// viewers receive the mix while the user does not hear it a second time.
// (Verified on Windows 10 19045 with a second Discord client; Windows' own
// process loopback, by contrast, captures after the session mute.)

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace saf {

class Engine;

struct OutputStatus {
    bool running = false;
    /** wasapi | null */
    std::string kind;
    std::string device;
    std::string format;
    std::string error;
    uint64_t renderedFrames = 0;
    /** Peak of the last rendered second, 0..1 (what the viewers receive). */
    float peak = 0.0f;
    /** The local session is muted (the user does not hear the mix twice). */
    bool sessionMuted = false;
    /** How often the session had to be muted again (unmuted in the mixer). */
    uint32_t remutes = 0;
    /**
     * Discord's soundshare hook is loaded into this process: Discord really
     * reads this output. The one signal that the share audio comes from here.
     */
    bool discordAttached = false;
    uint32_t restarts = 0;
};

class IOutputSink {
public:
    virtual ~IOutputSink() = default;
    /** Starts pulling from @p engine (external clock mode). */
    virtual bool start(Engine& engine, std::string& error) = 0;
    /** Stops pulling; returns only once the engine is no longer touched. */
    virtual void stop() = 0;
    virtual OutputStatus status() const = 0;
};

enum class OutputKind {
    /** WASAPI render into a muted session (Windows). */
    Wasapi,
    /** Pulls in real time and discards (tests, non Windows). */
    Null,
};

std::unique_ptr<IOutputSink> createOutputSink(OutputKind kind, std::string& error);

/** True when a module of Discord's soundshare hook is loaded into this process. */
bool discordHookLoaded();

} // namespace saf
