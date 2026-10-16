# StreamAudioFilter

A [Vencord](https://github.com/Vencord/Vencord) plugin that decides **which applications the viewers of
your screen share hear**, in the normal Discord desktop client on Windows 10 (2004+) and Windows 11.

You share your whole monitor. The game and the browser end up in the share; Spotify and Telegram do
not. You keep hearing everything, the microphone and the voice call are untouched, the video is
Discord's own, and the rules can be changed while the share is running.

```
 applications ──► saf-audio-host ───────────────────────► Discord (native voice module)
  game     ●      WASAPI per application capture            takes the screen share audio
  browser  ●      → mix of the allowed ones                 from the host's process
  Spotify  ○      → played into a session muted for you     (hook capture: reads it
  Telegram ○        (you do not hear it twice)                before the mute)
```

## Install

**Recommended:** download `StreamAudioFilter-Setup.exe` from
[Releases](https://github.com/dmbai009/StreamAudioFilter/releases), quit Discord (tray icon → Quit
Discord), run it, choose your Discord, press **Install**, start Discord.

- No administrator rights, nothing is downloaded, nothing outside your user profile is changed.
- An existing Vencord build is kept and restored on uninstall; Vencord's settings are never touched.
- Run the installer again to **repair** (a Discord update can remove Vencord) or to **uninstall**.
- Manual installation and building from source: [docs/INSTALL.md](docs/INSTALL.md).

## Use

The button next to the microphone opens the quick panel; the full panel is in Settings → Plugins →
StreamAudioFilter. Each application has one switch: **on = the viewers may hear it**. The default for
applications without a switch of their own is "allowed" or "blocked" (top of the panel).

The headline always says what the viewers get, and only claims what is verified:

| Headline | Meaning |
|---|---|
| Viewers hear only the allowed apps | Discord is attached to the filtered audio (confirmed by the host) |
| Viewers do not hear some allowed apps | Attached, but Windows delivers silence for an application (see the problem) |
| Share audio is off | Filtering could not be applied, so **no** audio is sent (fail closed) |
| Viewers hear ALL apps | Filtering failed and you enabled "send unfiltered audio when filtering fails" |
| Audio not controlled by the plugin | A share started without passing through the plugin (Discord changed) |
| Sharing a window | Discord sends only that window's application; the rules apply to whole screen shares |

"Copy diagnostic report" in the full panel puts everything needed for a bug report on the clipboard.
Nothing is ever sent anywhere by the plugin.

## How it works, and its limits

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — why Discord can only be steered through the process it
  captures, and how the plugin, the host and Discord fit together.
- [docs/LIMITATIONS.md](docs/LIMITATIONS.md) — what can not work (read this before relying on it).
- [docs/TESTPLAN.md](docs/TESTPLAN.md) — how to verify a build on a real machine with a second client.

## Licence

GPL-3.0-or-later, like Vencord.
