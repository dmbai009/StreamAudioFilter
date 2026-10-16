# Architecture

## 1. How Discord Desktop captures screen share audio

Discord Desktop does **not** capture screen share audio in JavaScript. `getDisplayMedia` /
`getUserMedia` are not involved, and no `MediaStream` ever carries it. The native voice module
(`discord_voice.node`, loaded in the renderer) captures it itself, from a **process id**:

```
flux MEDIA_ENGINE_SET_GO_LIVE_SOURCE { sourceId: "screen:0:0", sound: true, context: "stream" }
  └─ getPidFromDesktopSource(sourceId)      screen:* / prepicked → 1 (the whole system)
                                            window:<hwnd>        → that window's pid
  └─ soundshare id (+ getAudioPid in hook mode)
  └─ desktopDescription.soundshareId → conn.setTransportOptions({ soundsharePid, soundshareLoopback })
discord_voice:
  soundshareLoopback = getExperimentalSoundshare()
    true  → ActivateApplicationLoopbackForProcessTree(pid)   (Windows process loopback)
    false → DiscordHook64.dll injected into pid              (reads the app's render buffers)
```

Verified against desktop 1.0.9262 (strings in `discord_voice.node`, the MediaEngineStore code) and on
Windows 10 19045 with a second Discord client (October 2026):

| Test | Result |
|---|---|
| Window share of a test app, hook mode, app session **muted** in the volume mixer | viewer hears it |
| Process loopback of a process whose session is muted | silence (loopback is taken after the session volume) |
| Whole screen share with the soundshare pid patched to a test app | viewer sees the whole screen and hears only that app; browser music not heard; microphone unaffected |

The first plugin generation replaced a `MediaStreamTrack` from `getDisplayMedia`; that can never
change what viewers hear, which is why it reported "Discord obtained the audio in a way the plugin
cannot intercept".

## 2. The design

```
renderer (Vencord plugin)                main process (native.ts)        saf-audio-host.exe
─────────────────────────                ────────────────────────        ──────────────────
patch: soundshare pid ──► host pid       spawns the host,                engine: per app WASAPI
patch: hook mode while redirected        line protocol on stdio  ◄────►  process loopback, mixer
store: rules, share state machine                                         output: muted session on
UI: status, list, diagnostics                                             the default device,
                                                                          device clocked (pull)
Discord: DiscordHook64.dll in the host ─► reads the mix before the mute ─► viewers
```

- **One patch** in `MediaEngineStore` (`index.tsx`): the pid passed to the soundshare and
  `getExperimentalSoundshare()`. Both regexes are unit tested against Discord's current code
  (`tests/soundshare.test.ts`), and at start the plugin checks that both replacements are present.
- **The decision** (`stream/soundshare.ts`, pure, tested): only Go Live (`context === "stream"`) of a
  whole screen with sound is redirected. Window shares, shares without sound, clips and activities are
  left alone. Filtering wanted but no host → `pid = null`, i.e. no share audio (fail closed), unless the
  user enabled the unfiltered fallback.
- **The host** (`native/src/host`) is a separate process because Discord captures a process, because
  a crash in audio code must not take Discord down, and because it does not depend on Discord's
  Electron/Node ABI. It exits when its stdin closes. Protocol and version are checked at start; a host of
  another protocol is refused with a message.
- **Two processes.** Discord injects `DiscordHook64.dll` into the process it reads and intercepts its
  audio calls. In a real share, captures running inside that process delivered only silence (the same
  capture in any other process worked). So `saf-audio-host` captures and mixes, and feeds a ring in
  shared memory; a child `saf-audio-host --render` plays it, and *its* pid is the one Discord gets
  (`native/src/win/remote_sink.cc`). The child exits with its parent.
- **Output** (`native/src/win/render_sink.cc`): 48 kHz float into a session muted *before* the first
  sample, re-muted within 0.5 s if somebody unmutes it, reopened on device loss. It reports whether
  `DiscordHook64.dll` is loaded into the host — the only state in which the plugin shows "viewers hear
  only the allowed apps".
- **Engine** (`native/src/engine.cc`): one capture per allowed application (union of its process
  trees), mixed with 20 ms fades, a limiter, and in pull mode a 20 ms cushion and a 120 ms backlog cap
  per application. The output device clocks the mix: no drift, no growing delay.
- **Process trees** (`app_identity.cc`): Windows captures whole trees. An excluded application inside
  an allowed one's tree (a game started by a launcher) blocks the launcher (it would leak the game);
  an allowed application inside another allowed tree is not captured twice. Discord's own tree
  (`--protect <main pid>`) is never captured. Audio re-players (Audio Repeater, Voicemeeter, …) are
  blocked by default because they contain everything.
- **Lifecycle**: the engine runs while a panel is open, while in a voice channel, or while sharing; the
  output runs while in a voice channel (so Discord can attach at once); the capture gate is open only
  while the plugin provides the audio of the current share.

## 3. Installation layout

```
%APPDATA%\Vencord\dist\                          Vencord built with the plugin (+ saf-version.txt)
%APPDATA%\Vencord\dist.saf-backup\               a Vencord build that was there before (if any)
%APPDATA%\Vencord\streamAudioFilter\native\      saf-audio-host.exe
%APPDATA%\Vencord\streamAudioFilter\install.txt  what the installer did (for repair and removal)
%LOCALAPPDATA%\Discord\app-*\resources\app.asar  Vencord's loader; Discord's own is _app.asar
```

`saf-setup.exe` (`native/src/setup`) is the only code that changes these; `scripts/install.ps1` builds
from source and calls it with `--payload-dir`.
