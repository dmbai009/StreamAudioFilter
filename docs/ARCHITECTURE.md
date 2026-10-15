# Architecture

## 1. Why it is built this way

The task sounds like "give the viewers the audio of the selected applications without touching anything
around them". These are the options that were studied before the implementation was chosen.

| Approach | Outcome |
|---|---|
| Take the system audio (`getDisplayMedia`/desktopCapturer) and cut the unwanted part out | impossible: after mixing, individual applications are inseparable |
| A virtual device plus redirecting applications to it | forbidden by the requirements |
| Ready made virtual cables (VB-Cable, Voicemeeter, VAC) | the same, plus they require installing third party software |
| A "microphone" track through a virtual microphone (as the Linux solutions with venmic do) | requires a virtual device; Windows has no such public route |
| Capturing every process through WASAPI process loopback and mixing it ourselves | **chosen** — installs nothing, changes no device, works at the level of processes |
| Sending the mix into the share by replacing the audio track of `getDisplayMedia` | **chosen** — the track lives inside the renderer, no device ever appears |

The result: the audio never passes through an audio device at all. The plugin reads the streams of the
processes and hands the resulting PCM straight into the very `MediaStream` Discord sends to the viewers.

## 2. Data flow

```
┌───────────────────────────── Discord (Electron, renderer) ─────────────────────────────┐
│                                                                                        │
│  the "screen share" stream ────────────────────────────────────────────────────────┐   │
│                                                                                     │  │
│   video track (never modified)                                                      │  │
│                                                                                     │  │
│  stream/intercept.ts  ── intercepts getDisplayMedia / getUserMedia ─────────────────┤  │
│         │  replaces the audio track with the plugin's track                         │  │
│         ▼                                                                           │  │
│  audio/sourceTrack.ts  ── Web Audio: AudioBufferSource on a schedule ──▶ MediaStreamDestination
│         ▲                                                                           │  │
│         │ PCM (interleaved float32, 48 kHz, stereo)                                 │  │
└─────────┼───────────────────────────────────────────────────────────────────────────┘  │
          │  IPC: VencordNative.pluginHelpers.StreamAudioFilter.readPcm(frames)
┌─────────┴──────────────── main process (native.ts) ─────────────────────────────────────┐
│  loading streamaudiofilter.node, probe(), application icons                              │
└─────────┬───────────────────────────────────────────────────────────────────────────────┘
          │  N-API
┌─────────┴──────────────────── native addon (C++) ───────────────────────────────────────┐
│                                                                                         │
│  watch thread        parseApps() ── session_watcher ──▶ list of audio sessions          │
│       │  rule (mode + overrides)                                                        │
│       ▼                                                                                 │
│  applyPlan(): add/remove captures, set the gain target (0 = fade out)                   │
│       │                                                                                 │
│       ├── ProcessLoopbackCapture ── WASAPI process loopback ──▶ FIFO (ring buffer)      │
│       ├── ProcessLoopbackCapture ── ...                                                 │
│       └── ProcessLoopbackCapture ── ...                                                 │
│                                       │                                                 │
│  mix thread: 10 ms block ◀────────────┘  SincResampler → decode → stereo → Mixer(limiter)│
│       │                                                                                 │
│       ▼  output ring buffer                                                             │
│  readPcm(n) ──▶ JS                                                                       │
└─────────────────────────────────────────────────────────────────────────────────────────┘
```

## 3. Layers and their contracts

### C++ (`native/src`)

| File | Responsibility |
|---|---|
| `dsp/audio_format.h`, `dsp/convert.{h,cc}` | the format description, decoding u8/s16/s24/s32/f32, downmixing to stereo |
| `dsp/ring_buffer.{h,cc}` | a thread safe FIFO of frames that counts the dropped ones |
| `dsp/resampler.{h,cc}` | a sinc resampler (Blackman, cutoff 0.95·min(1, out/in)), 44.1↔48↔96 kHz |
| `dsp/mixer.{h,cc}` | summing N sources, fading in and out (20 ms), the limiter |
| `dsp/packet_measure.h` | measuring a packet: non-zero samples, peak, RMS, and the Goertzel tone search — portable, so the tests can hold it to its numbers |
| `capture.h` | the `IAppEnumerator`, `ICaptureSource`, `ICaptureFactory` interfaces — the seam between the engine and the OS |
| `app_identity.{h,cc}` | grouping sessions into applications, the `exe:<path>` key, computing the root processes |
| `capture_group.{h,cc}` | one capture per process tree of an application + summing the instances into one stream |
| `engine.{h,cc}` | rules, the capture thread, the mixing thread, applying the plan, statistics |
| `platform.{h,cc}` | choosing the backend (`auto`/`wasapi`/`mock`), `probeSupport()` |
| `mock/mock_backend.{h,cc}` | the test backend: a programmable application list + a signal generator |
| `win/session_watcher.cc` | enumerating sessions over all render devices, notifications about new sessions, level meters |
| `win/process_loopback.cc` | `ActivateAudioInterfaceAsync` + capturing a process and its tree |
| `win/windows_util.cc` | UTF-8/UTF-16 strings, HRESULT → text, the OS version, the process table, version resources |
| `addon.cc` | N-API: JSON strings outwards, PCM as a Buffer, one entry point for JS |
| `json.h` | a minimal JSON writer (no dependencies) |

### TypeScript (the plugin)

| File | Responsibility |
|---|---|
| `native.ts` (main) | finding and loading the `.node`, `probe`, application icons through Electron, the test scenario |
| `nativeBridge.ts` | a typed wrapper around `VencordNative`, normalising peaks from 0..1000 to 0..1 |
| `apps/rules.ts` | modes, rules, the application list, sorting — pure functions with tests |
| `apps/store.ts` | the single state: environment, rules, list, statistics, share status; polling only while a panel is open or a share is running |
| `stream/intercept.ts` | replacing the audio track of a share, the fail closed policy, watching out for "Discord never asked for audio itself" |
| `audio/sourceTrack.ts` | scheduling PCM through Web Audio into one continuous track, counting underruns and drops |
| `ui/*`, `components/*` | the panels and the application rows |
| `index.tsx` | the plugin definition: settings, the patch of the button next to the microphone, flux, toolbox, start/stop |

## 4. Key decisions

### One continuous stream instead of a restart

The requirement "change the composition on the fly" is solved like this: the engine always mixes **one**
stream into **one** ring buffer, and changes of the composition are applied as adding and removing
sources inside the mix. Switching an application off is a 20 ms fade out followed by the removal of the
source, not a restart of the share. The limiter and the fades rule out clicks, and not recreating the
stream rules out a video hiccup and a reconnect of the voice call.

### Delay and robustness

* the capture is requested in the format 48 kHz stereo float32 with `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY`;
  if the driver refuses, the mix format of the device is taken and converted internally (the decoder
  plus the sinc resampler);
* buffer budget: 200 ms in the WASAPI client, 250 ms per application, 500 ms at the output of the
  engine, 30–180 ms in the renderer (the "audio buffer headroom" setting, 60 ms by default);
* underruns and overruns are counted and shown, never swallowed.

### The capture gate

The engine knows whether a share is running (`RuleSet::active`). While no share runs, only the watching
of the application list is active: no audio client is activated and no audio is read. That rules out any
background load and any "special" interaction with the audio outside a share.

### Failing closed inside the interceptor

`stream/intercept.ts` is the only place where the fate of the share audio is decided, and that decision
is described by one pure function, `decideAudio()`:

| Filtering enabled | Our track is ready | The fallback is allowed | What the viewers get |
|---|---|---|---|
| no | — | — | the audio of Discord as is (a deliberate choice of the user, not a failure) |
| yes | yes | — | our filtered mix |
| yes | no | no | **the audio is removed from the share** + an error in the panel |
| yes | no | yes | the audio of Discord as is + a warning in the panel |

The third row is the essence of the audit: before it, a failed filtering silently returned the
unfiltered system audio, which made the excluded applications audible to the viewers again. That can no
longer happen, except with the explicitly enabled setting (the fourth row), which is off by default.

On top of that:

* creating the track is wrapped in `try/catch` — a filtering error must not break the start of a share;
* the death of the filtered track in the middle of a share does not "switch" the audio to the system
  one: the panel reports that the filtered audio stopped;
* if a share started without going through the intercepted calls (`noteStreamStart` on `STREAM_CREATE`),
  the panel reports it as an error: the plugin cannot mute what it never saw.

### Several instances of one application

One application can occupy several independent process trees: a program started twice has two root
processes, while the activation of a capture accepts **one** `TargetProcessId`. That is why a
`GroupingCaptureFactory` sits between the engine and the platform backend:

```
CaptureRequest{rootPids: {100, 302}}
        capture(100) + capture(302)  ──▶  GroupedCaptureSource  ──▶  one StreamState of the engine
```

* one root — the request goes to the backend unchanged (no extra thread, no extra buffer);
* several roots — one capture per tree, summed by the same `Mixer` (fades, limiter, counting of
  starvation), so the application stays **one** row, **one** stream and **one** item of the capture
  limit;
* duplicate roots are dropped (`uniqueRoots`), and nested trees never enter the list of roots:
  `computeCaptureRoots()` only treats a process as a root when it has no participating parent —
  otherwise the audio of the nested process would end up in the mix twice;
* if the set of roots of a live application changes (a second instance started or closed), the engine
  marks **only that** capture for a restart: it fades out in 20 ms, is removed and is created again with
  the new set of trees. The other applications, the mixer and the share itself are untouched;
* the number of instances is visible in the interface (`instances` in `AppView`) and in the row of the
  application.

### Polling instead of constant scanning

The session enumeration lives in a thread of its own with an MTA, subscribes to
`IAudioSessionNotification` (the event about new sessions) and wakes up once a second to compare. Changes
are handed outwards as a flag "something changed"; the list is enumerated once per change. The renderer
polls the engine once a second, and only while a panel is open or a share is running; 30 seconds after
the last panel closes, the engine stops by itself.

### Application identity

The key of an application is `exe:<normalised path>`. The path comes from the process of the session
(`QueryFullProcessImageNameW` with `PROCESS_QUERY_LIMITED_INFORMATION`, no administrator rights needed).
For UWP applications, where the path is unavailable, the Application User Model ID
(`GetApplicationUserModelId`) is used, and if even that is missing the process counts as unsaveable
("not saved" in the interface). Several processes of one path are merged into one row, and **every** root
process of the group enters the capture — one capture per tree (children belong to the tree of their
root). That way no instance is lost and none is summed twice.

### How system support is determined

The build number is not the verdict: `probeSupport()` performs a real `ActivateAudioInterfaceAsync`
attempt with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` for its own process tree and returns either
success or the `HRESULT` with its meaning. The build number is added to the result as context (and as a
reminder that "per the documentation" the threshold is 20348 while in practice the capture works from
19041). An activation without a process filter is performed as well: it shows that the audio engine is
alive at all, even if the system refuses a per process capture.

### What "audio arrived" means, and how a failure is named

Two lessons from the first real test run, both of them about honesty rather than about audio:

* A capture can be perfectly healthy and still carry nothing. The plugin used to decide "audio arrived"
  from the `AUDCLNT_BUFFERFLAGS_SILENT` packet flag, and a process loopback stream never sets that flag:
  the capability walk reported samples for streams of zeros. The decision is now based on counting the
  samples that are not zero, and the peak (`native/src/dsp/packet_measure.h`, covered by the portable
  tests). `GetBufferSize()` is not used as a packet size either — on a process loopback client it returns
  a meaningless value (the probe saw `1478376832` frames), so it is only reported when it is plausible.
* Every failure prints the stage it happened in and the `HRESULT` with its meaning, never a bare code: the
  capture activation distinguishes `[activation-call]`, `[activation-wait]`, `[activate-result]` and
  `[interface]`, and the capability walk has twelve stages (`native/src/win/windows_util.cc`). The one
  code that used to be misread as "your Windows version is unsupported" —
  `E_ILLEGAL_METHOD_CALL`/`0x8000000E` — now explains itself (`native/src/hr_advice.cc`).

The stages and their meanings are listed in [PROBE.md](PROBE.md), which also documents the standalone
probe that runs the same activation without Discord.

### Why the renderer and not the main process

Discord takes the audio for a share from the renderer. So the addon returns PCM into JS and Web Audio
assembles the track inside the renderer: no extra IPC per buffer between processes is needed, and the
`MediaStreamTrack` is created exactly where Discord expects it.

### What happens when Discord does not ask for audio

If "Share audio" is switched off in the interface, Discord calls the capture without audio parameters.
In that case the plugin does **not** add an audio track and writes into the panel that the audio is
switched off in Discord: it has no right to switch it on for the user.
