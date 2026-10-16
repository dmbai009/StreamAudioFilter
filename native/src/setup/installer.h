// StreamAudioFilter — installer
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one implementation of installing, repairing and removing
// StreamAudioFilter. saf-setup.exe uses it with a window or from the command
// line; scripts/install.ps1 (source build) calls saf-setup.exe with its own
// build as payload, so both ways behave identically.
//
// What an installation is:
//   %APPDATA%\Vencord\dist\                      Vencord built with the plugin
//   %APPDATA%\Vencord\streamAudioFilter\native\  saf-audio-host.exe
//   %APPDATA%\Vencord\streamAudioFilter\install.txt   what was done (for removal)
//   <Discord>\app-*\resources\app.asar           Vencord's loader (the original
//                                                Discord app.asar is _app.asar)
// Nothing outside the user's profile is touched, no administrator rights are
// needed, other plugins' settings are never modified,
// and every step is undone if a later one fails.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace saf::setup {

struct Paths {
    /** %APPDATA% (Roaming) */
    std::wstring appData;
    /** %LOCALAPPDATA% */
    std::wstring localAppData;
};

Paths defaultPaths();

struct DiscordInstall {
    std::wstring name;       // Discord, Discord PTB, Discord Canary
    std::wstring root;       // %LOCALAPPDATA%\Discord
    std::wstring appDir;     // ...\app-1.0.9262 (the newest one)
    std::wstring version;    // 1.0.9262
    bool running = false;
    /** none | vencord (a Vencord loader is installed) */
    std::string injection;
};

std::vector<DiscordInstall> findDiscordInstalls(const Paths& paths);

/** The files to install, relative paths ("dist/renderer.js", "saf-audio-host.exe"). */
struct Payload {
    std::map<std::string, std::string> files;
    std::string version;      // from saf-version.txt
    std::string protocol;
    std::string vencord;
};

bool loadPayloadFromDirectory(const std::wstring& directory, Payload& payload, std::string& error);
bool loadPayloadFromBytes(const char* data, size_t size, Payload& payload, std::string& error);

/** What is currently installed (read from disk, not from the state file alone). */
struct Status {
    bool vencordPresent = false;
    /** dist\saf-version.txt exists: the current Vencord build is ours. */
    bool ourVencord = false;
    std::string installedVersion;
    bool hostPresent = false;
    bool foreignBackup = false;   // the Vencord build that was there before is kept
    bool installed = false;       // includes incomplete installations
};

Status readStatus(const Paths& paths);

using Log = std::function<void(const std::string&)>;

struct Result {
    bool ok = false;
    std::string error;
};

/**
 * Installs or repairs. Refuses while Discord is running, checks that the
 * payload's parts belong to one release, keeps a foreign Vencord build as a
 * backup, and rolls back every completed step on failure.
 */
Result install(const Paths& paths, const DiscordInstall& discord, const Payload& payload, const Log& log);
bool newerVersion(const std::string& available, const std::string& installed);

/**
 * Removes StreamAudioFilter. A Vencord build that existed before the first
 * installation is restored, or plain Vencord is used when no original exists.
 * Other plugins' settings are preserved; own settings can be kept or reset.
 */
Result uninstall(const Paths& paths, const std::vector<DiscordInstall>& discords, const Log& log, bool keepSettings = true);

std::string toUtf8(const std::wstring& value);
std::wstring toWide(const std::string& value);

} // namespace saf::setup
