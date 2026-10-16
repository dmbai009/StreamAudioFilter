# Limitations

What can not work, or only works under conditions. The plugin reports each of these instead of hiding
them.

## Depends on Discord internals

- The plugin steers Discord through one value in `MediaEngineStore` (the soundshare pid) and Discord's
  hook based capture (`DiscordHook64.dll`). Both are internal. If Discord renames or removes them:
  - the patch does not apply → the panel reports "could not hook into Discord's screen share code",
    and a share that starts without passing through the plugin is shown as **"Audio not controlled by
    the plugin"** (the viewers may hear everything — the plugin can not mute what it does not control);
  - hook capture removed → Discord does not attach to the host → **"Discord did not take the filtered
    audio"** after 8 s (most likely the viewers hear nothing).
- A Discord update installs a new `app-*` folder without Vencord. Run the installer again (Repair).
- Vencord's own updater is off in the bundled build (it would replace the build and remove the
  plugin). Updates come as new releases of StreamAudioFilter.

## Only whole screen shares are filtered

Window shares keep Discord's behaviour (Discord sends the shared application's own audio). Shares with
Discord's "Sound" switch off stay silent. Clips are not touched.

## Windows audio

- Per application capture (WASAPI process loopback) needs Windows 10 2004 (19041) or newer.
- Windows captures **process trees**. An excluded application started by an allowed one (game from a
  launcher) can not be separated: the launcher is then not sent either (shown as "conflict").
- **Windows remembers volume and mute per executable.** Per application capture delivers an
  application's audio scaled by the capturing program's own session on that device. Early development
  builds of the host muted their output as the executable's default session; Windows kept that mute and
  silenced every capture of that executable path (measured: the same binary captured Chrome exactly from
  another folder; the same test tool captured a tone on "Line 1 (Virtual Audio Cable)" at 0.000 under its
  old name and 0.050 under a new one — virtual cables are *not* a limitation). Since 0.2.0 the output uses
  a session of its own that Windows does not remember, the host resets its capture level before
  capturing, and it lives in a folder per version. If an application is still captured as silence, the
  plugin says so by name ("silent capture").
- Windows system sounds are played by the audio service itself and can not be captured separately.
- Applications that re-play other applications' audio (VAC Audio Repeater, Voicemeeter, Equalizer APO
  tools) are blocked by default: sending one would send everything it re-plays.

## The muted session

The host appears in the Windows volume mixer as "StreamAudioFilter: share audio (muted here)". It must
stay muted (it is re-muted automatically); Discord reads it before the mute.

## Delay

Capture packets (10 ms) + 20 ms cushion + the output buffer (30 ms) + Discord's own encoding: the share
audio is about 50–70 ms behind the local playback. A 0.1 s gap is possible when an application is
started a second time while sharing (its capture is restarted rather than mixed twice).
