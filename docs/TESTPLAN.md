# Verification: what is already checked and what has to be checked on your machine

## 1. Automated tests (they run without Windows and without Discord)

```bash
./scripts/run-native-tests.sh       # C++: DSP + engine
./scripts/run-addon-tests.sh        # N-API: the whole JS → C++ → JS path on the test backend
npx tsx tests/rules.test.ts         # TypeScript: the rule logic
npx tsx tests/intercept.test.ts     # TypeScript: the audio interception, failing closed
./dev/typecheck.sh                  # tsc + eslint with the real Vencord toolchain (all suites at once)
```

What the last run showed:

| Suite | Checks | What it covers |
|---|---|---|
| `native/test/dsp_tests.cc` | 62 | the u8/s16/s24 (3 byte)/s32/f32 formats, downmixing (centre at −3 dB, LFE dropped), the resampler 44.1↔48 and 48→96 (SNR 76.1 / 89.5 / 93.2 dB), the absence of clicks (a 20 ms fade fits inside a block), the limiter: two full scale streams sum to ≤ 1.0 and > 0.85, unity gain 1.0000 ± 0.01 |
| `native/test/engine_tests.cc` | 97 | grouping processes into applications, mode A (including automatically accepting new applications), mode B, switching an application off and on live, rules surviving a mode change, an application closing on the fly, the limit of simultaneous captures, an uninterrupted output while the rules change (≥4800 frames in 150 ms), the "capture gate" (nothing is captured while idle), **several instances of one application**: splitting the request by root process, dropping duplicates, forwarding a single root without overhead, a second instance appearing during a share, an instance disappearing, a nested process not being captured twice; the presence of both instances is checked with a tone detector (Goertzel) at frequencies tied to the PIDs, including a check that neither of them is twice as loud. Because re-creating the capture leaves that application silent for about 0.1 s, the check waits for the settled state and asserts the bound (`< 2000 ms`) plus that no single measured window ever contains an instance twice — asserting a fixed instant instead made the test depend on where the window fell and failed on a loaded Windows runner; **diagnostics**: every HRESULT the plugin can report has a name and an explanation, `0x8000000E` is explained as the agile object requirement and explicitly *not* as a Windows version problem, unknown codes keep their number and are still classified (`AUDCLNT_E_*`, `HRESULT_FROM_WIN32`, `RPC_E_*`) |
| `tests/addon.test.mjs` | 6 scenarios | the real native addon: starting with the test backend, the application list being visible, the capture gate (nothing is captured while idle), audio appearing, reading PCM (stereo f32, not silence, limiter ≤ 1.0), switching an application off on the fly without interrupting the stream, mode B (empty by default, then only what is allowed), automatically accepting a new application in mode A, a closed application disappearing, statistics, stopping, trimming invalid arguments, one entry and one stream for an application started twice (`instances: 2`) |
| `tests/intercept.test.ts` | 13 | **failing closed**: the pure decision table `decideAudio`, the removal of the system audio when filtering is unavailable (the key check of the audit), an exception from `createTrack()` not breaking the start of a share, the unfiltered fallback only with the explicit setting and with a warning, a switched off filter touching nothing, both ways of obtaining the audio (`getDisplayMedia` and `getUserMedia`+`chromeMediaSource: desktop`), the microphone staying untouched, a video only share never getting a track, a share that did not go through the interceptor being reported as an error, the death of the filtered track being visible in the state, installing and uninstalling the interceptor being idempotent |
| `tests/rules.test.ts` | 11 | the modes, an explicit choice beating the mode, the name and path of a closed application surviving, the sections and the sorting of the list, uncapturable applications, the content of the payload, Discord being excluded by default, resetting and forgetting, tolerance of a corrupted store, different applications with the same name not being merged, the "2 instances" note in a single row |
| `tsc --noEmit`, `eslint` | 0 errors | the whole TypeScript of the plugin compiles and passes the Vencord linter |

## 2. What CI checks automatically

`.github/workflows/native.yml` — every job is a gate, not a line in a log file:

* `portable-tests` — the C++ tests on Linux (`g++`) **and** a build of the N-API addon followed by
  `tests/addon.test.mjs`; a failing test fails the job;
* `typescript-tests` — `tests/rules.test.ts` and `tests/intercept.test.ts` (both mandatory);
* `windows-build` — the native addon built by MSVC through `node-gyp` (no `continue-on-error`: a
  compiler error fails the job), then the portable C++ tests compiled and run by the Microsoft compiler
  with the exit code of every `.exe` and the line "0 failure(s)" checked, and finally a check that the
  artifact `streamaudiofilter.node` really exists. The build log is committed into `ci-out/` (the GitHub
  Actions logs are not reachable from the network this project was prepared in); the log publication
  step is the only optional one and it cannot hide a failure, because it runs after the build and the
  tests.

`.github/workflows/release.yml` — triggered by a `v*` tag: it calls the gate workflow above, downloads
the addon that gate produced and attaches it to the release together with its SHA-256 and an install
note. A tag can therefore never publish a binary that a commit build would have rejected.

## 3. Acceptance run on your machine

Every item corresponds to a requirement of the original task. The expected result is written so that
"works" and "does not work" cannot be confused.

### 3.1 Preparation

1. Install the plugin (`docs/INSTALL.md`) and restart Discord.
2. Open the panel and press **"Check support"**.
   * Windows 10 version 2004+ (build ≥ 19041) and Windows 11 → expect "supported: yes" ("real activation
     of a per process capture: success") and a line with the build number saying that the real threshold
     is 19041 while 20348 is only what the documentation states.
   * Windows 10 before 2004 (build < 19041) → expect "supported: no" with a text about build 19041 and
     the real `HRESULT` of the attempt, and no attempt to circumvent anything. That is the only honest
     result: see `LIMITATIONS.md`.
   * A wrong result would be "supported: no" on Windows 10 2004+ without an error code: it means the
     system refused for a concrete reason (the audio service, a driver) that has to be diagnosed from
     the `HRESULT`, not from the Windows version.

### 3.1.1 Failing closed

Check it in any of these ways:

1. Temporarily rename `%APPDATA%\Vencord\streamAudioFilter\native\streamaudiofilter.node` (or set a
   wrong path to the addon in the settings) and start a share of the **whole screen** with audio.
   * Expected: the panel says in red that the filtering was not applied and that **the share audio is
     not being sent**; the "Audio at the viewers" row of the diagnostics reads "off".
   * The viewers must hear **neither** the excluded applications **nor** the other ones.
2. Enable the "Send plain system audio when filtering is unavailable" setting and start the share again.
   * Expected: the audio is back (as in Discord without the plugin) and the panel warns that it is
     unfiltered.
3. Switch the filtering off ("Filter the screen share audio" → off) and start a share.
   * Expected: the panel says the filtering is off, the usual audio is sent, and the microphone, the
     other applications and every Discord setting stay untouched.

### 3.2 The main scenario (sharing the whole monitor)

1. Play music (Spotify/a player), a game and a messenger — all with sound.
2. Open the panel and press "Refresh list": all four applications must appear in the "Currently playing
   audio" section (Discord separately, in the excluded ones).
3. Start a share of the **whole screen** with "Share audio" enabled.
4. Make sure the panel says **"Sharing: …"** — that is the proof that the interception worked (if it
   says "filtering not applied", Discord took another path: send the diagnostics section).
5. Switch Spotify off with its switch, **without interrupting the share**.
   * viewers: the music disappears within about 50 ms, the game keeps playing;
   * you: the music stays audible the whole time;
   * video: no flicker, no restart; the voice call does not reconnect.
6. Switch Spotify back on — the music returns just as quickly.
7. Change to the **"Only included"** mode: only the applications you explicitly enabled must remain.
   Then go back to "Everything except excluded": the switched off Spotify must stay switched off (a mode
   change does not reset the rules).
8. Start a new application with sound (a browser playing a video, for example):
   * in mode A it must appear and play without any action from you;
   * in mode B it must stay silent until you switch it on.
9. Close the application: its row moves to "Saved rules (application closed)" and the capture is
   released (the "Applications captured" number in the diagnostics goes down).
10. End the share: the captures must stop (the engine stops by itself after 30 seconds) and the panel
    must return to the "Ready" state.

### 3.3 Checking the constraints of the task

| What is checked | How | Expectation |
|---|---|---|
| The microphone and the voice call are untouched | talk while sharing | the others hear you as usual; the Discord voice call never ends up in the share |
| The local audio is unchanged | switch an application out of the share | you keep hearing it, its volume in the Windows mixer does not change |
| No devices are changed | open the Discord audio settings before and after | the same device list and the same selected devices |
| No system settings are changed | compare the Windows "Sound" panel before and after | unchanged |
| No virtual device appeared | `Get-PnpDevice -Class MEDIA`, Device Manager | no new devices |
| No administrator rights are needed | run Discord without administrator rights | everything works (there is not a single `runas` or UAC manifest in the plugin) |
| Nothing is downloaded | installing the plugin and building | the network is used only for `npx node-gyp`, after an explicit confirmation |
| The share parameters are unchanged | compare the quality/resolution before and after | the video settings were not changed |
| "Share audio" is respected | switch it off in Discord and start a share | the plugin does not force it on and says so in the panel |

### 3.4 Edge cases

* **Several applications with the same name** (two different `chrome.exe` from different folders) — two
  separate rows, independent rules.
* **Many applications with audio** — more than the limit (12 by default): the warning "part of the audio
  is not sent" appears and the stream does not break.
* **44.1 kHz** (an output device at 44.1): no clicks, no speeding up or slowing down.
* **Voicemeeter as the default device**: applications playing into VAIO are visible and can be filtered;
  see the Voicemeeter section of `LIMITATIONS.md`.
* **Restarting Discord** — the saved rules come back, PIDs do not matter.
* **Switching the plugin off and on** — the captures stop, and the list is collected again on enable.

## 4. If something does not work

Collect and attach:

1. the output of the diagnostics section (including the "Check support" result);
2. the Windows version and build (`[Environment]::OSVersion.Version.Build`);
3. the Discord version and the Vencord branch;
4. the exact step of this checklist where the behaviour differed from the expectation.

That makes it possible to tell "unsupported by the system" from "a bug in the plugin" in a single step.
