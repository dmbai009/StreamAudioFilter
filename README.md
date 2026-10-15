# StreamAudioFilter

A [Vencord](https://github.com/Vencord/Vencord) plugin that decides **which applications the viewers of
a screen share actually hear**, inside the normal desktop Discord client (Windows 10 x64 / Windows 11).

The original example: you share your whole monitor. The game and the browser end up in the share;
Spotify and Telegram do not. You keep hearing everything, and neither your microphone nor the voice
call is affected.

```
      applications                  plugin engine                     Discord
  ┌────────────────────┐        ┌──────────────────────────┐   ┌──────────────────┐
  │ game        ●──────┼──┐     │ WASAPI process loopback  │   │ screen share     │
  │ browser     ●──────┼──┼────▶│ → own mixer              │──▶│ + audio          │
  │ Spotify     ○──────┼──┘     │ → one PCM stream         │   │ = what viewers   │
  │ Telegram    ○──────┼────┐   │ rules applied on the fly │   │   hear           │
  └────────────────────┘    │   │ without any pause        │   └──────────────────┘
                            └──▶│ (never enters the mix)   │        ▲
  your local playback ──────────┴──────────────────────────┴────────┘ video untouched
```

---

## 1. What to know before installing

**Windows 10 version 2004 (build 19041) or newer, or Windows 11, is required.** Capturing the audio of
individual applications (`WASAPI process loopback`) works from that build on: it is exactly what OBS
requires for its "Application Audio Capture" source. Microsoft's documentation names "build 20348" as
the minimum — that is the build the API was *documented* on, not the one it appeared on; the reasoning,
with links, and a way to test your own system, is in [docs/LIMITATIONS.md](docs/LIMITATIONS.md).

What decides is not a guess and not the build number but a **real activation attempt**: the
"Check support" button in the diagnostics section starts an actual `ProcessLoopbackCapture` and shows
either success or the error code. If the system cannot do it, the plugin does not replace the filtering
with a workaround, it says so.

**The plugin creates no virtual audio devices, needs no VB-Cable/Voicemeeter, changes no Windows sound
settings, does not touch Discord's recording or playback devices, never goes near the microphone and
asks for no administrator rights.**

---

## 2. What it does

### Two modes

| Mode | Behaviour | New applications |
|---|---|---|
| **"Everything except excluded"** (default) | the audio of every application is sent, except the ones you switched off | included automatically |
| **"Only included"** | only what you switched on is sent | silent until you allow them |

Switching the mode **does not reset** the stored selection: every application keeps its own rule, the
mode only decides the fate of the applications you never touched.

### Live changes during a share

Switching Spotify off in the middle of a share makes the viewers stop hearing the music within about
20–40 ms, while you keep hearing it. Switching it back on is just as fast. The video, the voice call and
every other audio source never break: **one continuous stream** is mixed, only its composition changes.

### Failing closed, not open

If the filtering could not be applied (the engine did not start, the addon was not built, the system
does not support the capture), the plugin does **not** hand the viewers the ordinary system audio: it
removes the audio from the share and shows the reason. Otherwise "Spotify is excluded" would turn into
"the viewers hear Spotify the moment something breaks" — the exact opposite of what was asked for.

The opposite behaviour exists, but only as a deliberate choice of the user: the setting "Send plain
system audio when filtering is unavailable" (off by default) — and then the panel honestly says that
unfiltered audio is being sent.

One more case: if Discord obtained the audio in a way the plugin does not intercept, the panel reports
it as an error (such a stream cannot be filtered — the plugin cannot mute what it never saw).

### Entry points (all of them show the same state)

1. **Settings → Plugins → StreamAudioFilter** — the full panel: mode, application list, switches,
   capture state, errors, diagnostics.
2. **A button next to the microphone/headphones** at the bottom of the Discord window — a compact list
   in a popover (can be switched off in the plugin settings).
3. **The floating panel** — a small window that appears by itself when Discord requests the screen share
   audio: handy for changing the composition on the fly without opening anything. It collapses during a
   share so that it stays out of the way (configurable).
4. **Vencord Toolbox** — while the plugin is enabled, the Toolbox menu gains two actions (show the
   panel, reset the filters).

Why there is no button inside the screen share launch window and inside the running share UI: those
screens have no public API, and patching them blindly means breaking the user's screen share with every
Discord update. Instead, the control panel comes to the user by itself at the moment Discord takes the
audio for the share. More: [docs/UI.md](docs/UI.md).

### Application list

* scanned automatically when the panel opens and when new audio sessions appear;
* applications closing are detected;
* applications with active audio first, then separate sections for "running, but silent" and "saved
  rules";
* several processes of one application (Chrome, games, Electron) are merged into one row — but only
  when the affiliation is reliable (by the path of the executable);
* an application started **twice** (two independent process trees) is still one row and one stream:
  both instances are captured, summed once, and the row gains a "2 instances" note. A new instance
  started during a share becomes audible by itself, without restarting the demonstration;
* the state of every row: *sent*, *excluded*, *not playing audio right now*, *application closed*,
  *audio capture failed*, *unavailable*;
* applications whose audio cannot be intercepted are not presented as filterable — they are marked.

### Application identity

The settings are tied to the **path of the executable** (plus the readable name from the version
resources), not to a process id: a PID changes on every start and can be reused. Applications with the
same name are never merged. For UWP applications, where the path is unavailable, the package identity
is used; if even that is missing, the rule is marked as "not saved" and is honestly lost once the
application exits.

---

## 3. Installation

On Windows, from the folder this repository was unpacked into — one command:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -DownloadAddon v0.1.0
```

It checks Node.js, gets the Vencord source, copies the plugin into `src\userplugins`, installs the
dependencies, builds Vencord and verifies that the plugin really ended up in the build, places the
native addon in `%APPDATA%\Vencord\streamAudioFilter\native\` and patches Discord with your build
(that last step downloads the official Vencord installer CLI, and the script asks first). Afterwards:
start Discord, enable the plugin, press "Check support".

`-Reinject` re-patches Discord after a Discord update, `-Update` refreshes Vencord and rebuilds. All
switches, the manual steps and the troubleshooting table: [docs/INSTALL.md](docs/INSTALL.md).

Why there is no "just drop the plugin file in" route: Vencord does not read plugins from a folder at
runtime — its build bundles `src\plugins` and `src\userplugins` into the JavaScript Discord loads, so a
user plugin can only enter through a build of Vencord. The native addon is the one part that really is
a single file, and it is attached to the GitHub releases (built by the same CI that runs the tests,
with its SHA-256 on the release page).

---

## 4. What the plugin deliberately does not do

| Requirement | How it is met |
|---|---|
| No virtual audio devices | the audio never passes through a device at all: the mixer hands PCM straight to the share's stream |
| No VB-Cable / Voicemeeter / VAC | not used, not installed |
| No redirection of application audio | applications keep playing into their own devices as usual; the capture listens from the side (loopback) |
| No changes to the system audio | the Windows sound settings are neither read nor written |
| No changes to Discord's devices | recording/playback devices are neither selected nor modified |
| Never touch the microphone or the voice call | the microphone chain is left alone; Discord itself is excluded from the share by default so that the voice call does not travel back to the other side |
| No administrator rights | only `PROCESS_QUERY_LIMITED_INFORMATION` and user mode COM interfaces are used |
| No third party EXE downloads | everything is built from source on your machine; the build script asks for confirmation and prints the command it runs |
| No damage to the share settings | if "Share audio" is off in Discord, the plugin does not switch it on; video, resolution, FPS, codec and bitrate are never changed |

---

## 5. Project state and verification

Honestly about what has been verified and what has not.

**Verified in this environment (Linux, no Windows and no Discord):**

* the audio interception logic: `npx tsx tests/intercept.test.ts` — **13 tests**, the most important of
  which checks that a failed filtering **removes** the audio instead of sending it unfiltered (plus the
  microphone, the switched off filter, the explicit fallback permission and a share that never went
  through the interceptor);
* the JavaScript ↔ C++ boundary (N-API): `./scripts/run-addon-tests.sh` — **6 scenarios** in which the
  native addon is compiled for the current system and driven through its real entry points: start,
  application list, live rules, switching an application off on the fly, mode B, applications appearing
  and closing, reading PCM (not silence, not overload), statistics, the capture gate, stopping and
  trimming invalid arguments, and two instances of one application as a single row;
* the portable DSP core and the mixing engine: `./scripts/run-native-tests.sh` — **127 checks, 0
  failures** (`dsp_tests` 62, `engine_tests` 65): the u8/s16/s24/s32/f32 formats, downmixing, the
  resampler (SNR > 70 dB at 44.1↔48 kHz), the limiter without clipping when summing, modes A and B,
  changing rules live, applications closing, the limit of simultaneous captures, the "capture gate"
  (nothing is captured while no share runs) and capturing several instances of one application without
  duplicating its audio (checked with a tone detector at the frequencies of both instances);
* the rule logic of the plugin: `npx tsx tests/rules.test.ts` — **11 tests**, including the selection
  surviving a mode change, different applications with the same name not being merged, and a corrupted
  store being tolerated;
* the whole TypeScript of the plugin passes the **real Vencord toolchain**: `tsc --noEmit` and `eslint`
  (0 errors) — `./dev/typecheck.sh`.

**Builds, but does not run, in this environment:** the Windows addon (WASAPI process loopback, audio
session enumeration, N-API) is compiled by MSVC in GitHub Actions
(`.github/workflows/native.yml`, `.github/workflows/release.yml`). The build log is committed into
`ci-out/`, because the Actions logs are not reachable from this network.

**Needs your machine:** the audio itself. No test here can prove that Discord hands the screen share
audio through the very call the plugin intercepts, or that the mixer sounds right on a particular sound
card. For that there is a step by step acceptance checklist: [docs/TESTPLAN.md](docs/TESTPLAN.md). The
plugin deliberately **shows** whether the interception worked: if a share is running while the filtering
was not applied, the panel and the diagnostics say so instead of hiding it.

---

## 6. Repository layout

```
index.tsx              plugin entry point: settings, patches, flux, start/stop
settings.ts            the plugin's settings for Vencord
types.ts               shared types of the renderer and the main process (no imports)
native.ts              main process: loading the native addon, IPC, application icons
nativeBridge.ts        typed access of the renderer to the main process
dataStore.ts           storing the rules in the Vencord DataStore
apps/rules.ts          pure logic of modes, rules and the application list (+ tests)
apps/store.ts          renderer state: environment, rules, list, share status
audio/sourceTrack.ts   turning PCM into a MediaStreamTrack through Web Audio
stream/intercept.ts    replacing the audio track of a screen share, failing closed
ui/                    panels: settings, microphone popover, floating window
components/            the application row, the diagnostics block
native/                C++: DSP, engine, mock backend, WASAPI backend, N-API addon
                       capture_group.* — one capture per process tree of an application
scripts/               install.ps1 (one command install), building the addon, portable tests
dev/typecheck.sh       type and lint check inside a real Vencord
tests/                 tests of the rules, of the interception and of the JS ↔ C++ boundary
docs/                  architecture, limitations, installation, test plan
```

---

## 7. Licence and origin of the code

GPL-3.0-or-later (see [LICENSE](LICENSE)). The code was written for this task; other plugins were only
studied as a source of knowledge about the Vencord and Discord interfaces, their code was not copied.
The only exception is the file header that the Vencord linter requires (`simple-header/header`, template
`scripts/header-new.txt`), which is therefore present verbatim, exactly as in every Vencord plugin.
