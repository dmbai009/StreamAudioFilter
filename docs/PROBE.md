# The standalone process loopback probe

`process-loopback-probe.exe` is a console program that answers one question, on your machine, without
Discord: **does this Windows really provide the per application audio capture the plugin needs, and if
not, which step refuses it?**

It is built from source by CI (see `.github/workflows/native.yml`) and attached to every release next to
the addon, so no compiler is needed to get a diagnosis. The source is
`native/tools/process_loopback_probe/main.cc`; it uses the plugin's own agile completion handler
(`native/src/win/com_ptr.h`), the plugin's HRESULT explanations (`native/src/hr_advice.cc`) and the
plugin's packet measurement (`native/src/dsp/packet_measure.h`), but nothing else — no Discord, no
Vencord, no capture engine.

The measurement module is the part that decides what a capture actually contains, and it is covered by
the portable tests (`native/test/dsp_tests.cc`). It counts non-zero samples, the peak and the RMS, reports
the level in dBFS, classifies a window as **silence / noise floor / audio** at a documented threshold, and
looks for the 1 kHz tone **after downmixing the interleaved channels** — a tone searched as if the
interleaved buffer were mono lands at half its frequency, which is a mistake this probe made itself in its
first real runs (see the end of this file).

## What it does — and what it refuses to do

It installs nothing, changes no setting, touches no device, and **never writes audio anywhere**: samples
are counted as they are read and immediately dropped. The only file it can create is the text log you ask
for with `--report <file>` — without that flag it opens no file at all. The loudest thing it can do is
play a 1 kHz tone for a few seconds, and only when you ask for it (`--self-tone`, `--spawn-tone`, or
`--tone-only` in a second console).

## Usage

```text
process-loopback-probe.exe --list
process-loopback-probe.exe --playing
process-loopback-probe.exe --spawn-tone --seconds 8
process-loopback-probe.exe --spawn-tone --system --seconds 8
process-loopback-probe.exe --process spotify.exe --seconds 8
process-loopback-probe.exe --pid 1234
process-loopback-probe.exe --process chrome.exe --exclude
process-loopback-probe.exe --self-tone --system --seconds 8
process-loopback-probe.exe --process chrome.exe --json
process-loopback-probe.exe --spawn-tone --seconds 8 --report probe-run.txt
```

| Option | Meaning |
|---|---|
| `--list` | list running processes (pid, image name, path) — use it to find the name for `--process` |
| `--playing` | list **every active output device** with the processes that hold an active audio session on it right now, and the name of the default output device — use it to pick a target that is really playing something. A device name that looks like a virtual cable (VB-Audio, Voicemeeter, "virtual") is called out: it is not a problem for the plugin, which captures applications and changes no routing, but a `--system` run on such a line measures what is fed *into* the cable |
| `--pid <n>` | capture the tree of this process |
| `--process <name.exe>` | capture the first running process with this image name |
| `--self-tone` | play 1 kHz from the probe itself for the whole capture window and look for that tone in the captured audio — the **weakest** form of the check, see below |
| `--spawn-tone` | start a **second copy of the probe** (`--tone-only`) that plays 1 kHz, and capture *it*: the tone comes from another process, so it **has** to appear in the capture. This is the decisive content check |
| `--tone-only` | only play 1 kHz and capture nothing (the child mode of `--spawn-tone`; useful from a second console window) |
| `--system` | capture the **whole system audio** through a classic WASAPI loopback stream instead of a per process capture — the control test |
| `--exclude` | capture everything *except* the target tree (the mode the plugin uses for "all except selected") |
| `--seconds <n>` | capture window, 1–300, default 5 |
| `--json` | one line of machine readable JSON instead of the stage log |
| `--report <file>` | also write the stage log of this run to this text file (UTF-8). Nothing else is ever written, and without this flag the probe opens no file |

`--report <file>` writes the same stage log to a text file in UTF-8, so a run can be handed over without
copying anything out of a console window. The file is opened through its wide path, which means a path with
Cyrillic letters in it works: the command line reaches the program as bytes in the console's code page, so
a path is read as UTF-8 when the bytes really are UTF-8 and as the ANSI code page otherwise.

Text that is not plain ASCII (device names on a non-English Windows, for example) is converted to the code
page the console actually uses, so names come out readable instead of as block noise; when the output is a
file or a pipe, the text stays UTF-8. That conversion is also what `--report` gives you verbatim.

Exit codes: `0` — every stage succeeded; `1` — a stage failed (the stage log names it); `2` — usage
error (for example no target at all: that prints what to do instead of blaming the audio engine, and the
`--json` output uses the same three codes).

Usage errors go to **stderr**: a missing target, a typo in an option, an option without its value. With
`--json` that matters, because stdout then holds exactly one line — the machine readable result — and a
script can parse it without filtering. `--json` describes a capture run; `--list`, `--playing` and
`--help` always print text (`--help` to stdout, because it was asked for). The `[report] this run is
written to ...` notice follows the same rule: it appears on stdout normally and on stderr in `--json`
mode. That was a real defect: the notice used to be printed to stdout in `--json` mode as well, which put
a second line in front of the JSON.

`--json` prints one line with `ok`, `pcmArrived`, `toneMagnitude`, `packets`, `frames`, `silentFrames`,
`nonZeroSamples`, `peak`, `peakDbfs`, `content` (`silent` / `noiseFloor` / `audio`), `rms`, `format`,
`channels`, `toneFrames` (how much this program rendered for the check), `toneProcessExit` (the exit code
of the `--spawn-tone` child, `-1` when there is none), `failureStage`, `failure` and `failures`.

## The stages, and what a failure in each one means

| Stage | What it does | If it fails |
|---|---|---|
| `platform` | prints the real Windows version (ntdll, not the manifest-dependent `GetVersionEx`) | — |
| `com` | `CoInitializeEx(COINIT_MULTITHREADED)` on the probe thread | `CO_E_NOTINITIALIZED` / `RPC_E_CHANGED_MODE`: an apartment problem, not an audio problem |
| `handler` | verifies that the completion handler answers `IAgileObject` | without it Windows returns `E_ILLEGAL_METHOD_CALL` (0x8000000E) from the call below — the plugin bug that this probe exists to make visible |
| `target` | resolves the process to capture | no such process, or an invalid pid. When *nothing* was given at all, the probe prints the options and exits `2` — a missing argument is a usage mistake, not an audio engine failure |
| `params` | prints the activation payload (`VT_BLOB`, pid, include/exclude mode) | a malformed payload produces `E_INVALIDARG` in the next stage |
| `activation-call` | `ActivateAudioInterfaceAsync` itself | **the interesting stage**: the exact HRESULT with its meaning. `0x8000000E` = handler/apartment, `E_INVALIDARG` = parameters, `E_NOTIMPL` = this build has no such activation |
| `activation-wait` | waits for `ActivateCompleted` (5 s) | the callback never came — a driver/audio-service level problem |
| `activate-result` | `GetActivateResult` and the result it reports | two separate HRESULTs on purpose: the operation failing is not the activation failing |
| `interface` | `QueryInterface(IAudioClient)` | the activated object is not an audio client |
| `client-init` | asks for 48 kHz float stereo with `AUTOCONVERTPCM`, falls back to 16 bit PCM | `AUDCLNT_E_UNSUPPORTED_FORMAT` etc. — a format problem, not a capture problem |
| `capture-client` | `IAudioCaptureClient` | the client cannot deliver buffers |
| `buffer-size` | the packet size, **only when the value is plausible** | a process loopback client reports no meaningful packet size; the line says so instead of printing a number (see "What a green run means") |
| `capture-start` | `SetEventHandle` + `Start` | the stream did not start — the reason is quoted |
| `data` | packets, frames, frames flagged silent and the negotiated format | only a stream that carries no samples at all is reported as empty; this line says whether the *stream* runs |
| `levels` | peak (as a number and in dBFS), RMS, the count of non-zero samples, and one short sentence about what the window contains: audio, only a noise floor, or pure silence | the two lines are deliberately short; the earlier single line was 185 characters, and the first report that came back from a real machine lost its second half to line wrapping when it was copied out of the console |
| `tone` | how much this program rendered, and the 1 kHz magnitude found in the captured audio (Goertzel over the downmixed channels, threshold 0.05) | reported with its numbers and explained; a missing tone is never blamed on the capture when the probe played it itself, and `--spawn-tone` turns it into a verdict |
| `teardown` | `Stop`, release of every interface, close the apartment | — |

## Reading the result

* **All stages green and `[levels]` says audio is in it** — the capture delivered audio: the peak is
  above -60 dBFS. Values that are *not* proof, and are called out as such in the output: a high frame
  count, `0 frames flagged silent` (a process loopback stream never sets that flag), and **a large count
  of non-zero samples**: the noise floor of the capture path is made of samples that are not exactly zero.
  A real run of this probe measured 580972 non-zero samples at a peak of -90.2 dBFS.
* **`[levels]` says only a noise floor arrived** — packets came at exactly the right rate and the samples
  are not zero, but nothing audible was in the window. The stream itself works; the target played nothing
  audible (or played something far below the threshold). This is the case the level reporting exists for,
  and it is reported as *no audio*, never as audio. -60 dBFS is a **reporting convention**, not a claim
  about the machine: it is printed next to the classification so the reading can be judged, and `--json`
  carries both `peakDbfs` and `content` for anything that wants to apply its own threshold.
* **`activation-call` fails with `0x8000000E`** — the completion handler was not usable from the calling
  apartment. In this probe that cannot happen any more (it verifies the agility one stage earlier), so
  the output would show `handler: ... yes/no` in the same run.
* **`activation-call` fails with `E_NOTIMPL` / `E_INVALIDARG`** — the build does not provide this
  activation, or the parameters were rejected. This is the honest "not supported here" case.
* **`[levels]` reports pure silence** — the capture works; the chosen process did not play anything
  during the window. `--playing` finds a process that is playing, `--system` checks the path with the
  whole system audio.
* **`--spawn-tone` says the tone is not in the capture** — this is a real failure (`exit 1`): the tone
  was played by a different process, into the same default output device, for the whole window, and the
  capture of that very process did not contain it. Unless the child failed to start or to render (the
  probe checks its exit code and says so), the capture did not deliver that content.
* **`--self-tone` says the tone is not in the capture** — this does **not** mean the capture is broken,
  and the probe never fails a run on it. One process is both the source and the observer here, so the
  result cannot distinguish a capture that missed the content from a tone that never reached the mix of
  the captured device. The line says which of the two usable halves you are looking at: the tone was
  never rendered, the stream carried only zeros, or the stream carried other audio and not the tone.
  A *found* self tone is a positive result (the capture followed the device mix); for a decisive answer,
  use `--spawn-tone`.

### Why `--system` exists

`--system` runs the identical measurement over the whole system audio with a classic WASAPI loopback
stream. Combined with `--spawn-tone` it is the control test of the device path:

* `--spawn-tone --system`: a second process plays 1 kHz into the default output device, and the loopback
  of that device must contain it. Found → the device, its driver and the audio engine deliver what
  applications play, so any remaining problem is specific to per process capture. Missing although the
  child rendered it → the audio never reached the device at all (this also exits `1`), which has nothing
  to do with per process capture and would explain a silent share in Discord as well.
* `--system --self-tone`: usable only in the positive direction — a tone that is found proves the path.
  A missing tone proves nothing here either, because the same process played and captured it.

Note that on a machine whose default output is a virtual cable, `--system` measures what applications
render *into* that line, not what you hear on your speakers — which is still a valid control for the
capture path, and the probe says so when it recognises such a device name.

## What was measured on Windows 10 Pro 22H2 (build 19045.7725)

The run of this probe on the target machine, quoted from the user's console. The stage tags and the
wording are those of the build that produced it (`[self-tone]` is `[tone]` today, and the `data` line
carried fewer numbers):

```text
[platform] Windows 10.0 build 19045 (x64)
[com] CoInitializeEx(COINIT_MULTITHREADED): S_OK
[handler] completion handler at ... answers IAgileObject: yes
[params] target pid 30384, mode INCLUDE_TARGET_PROCESS_TREE, VT_BLOB 12 bytes
[activation-call] accepted (the audio engine will call back on a worker thread)
[activation-wait] the completion callback fired
[activate-result] S_OK - the capture client exists
[interface] IAudioClient obtained
[client-init] 48 kHz / 2 channels / 32 bit float accepted
[buffer-size] 1478376832 frames per packet (...)      <- meaningless, see below
[capture-start] the capture is running
[data] 799 packet(s), 383520 frames, 0 frames flagged silent
[self-tone] 1 kHz magnitude in the captured audio: 0.000007 - the tone was NOT found
```

The same build with `--process chrome.exe` (pid 12340) reached exactly the same stages and then reported
`799 packet(s), 383520 frames, peak 0.317145, RMS 0.079421, non-zero samples 764032` — while Chrome was
playing, which is why that run was read as "the process loopback capture works on this system".

The same build with `--system --self-tone --seconds 8` reported `800 packet(s), 384000 frames, peak
0.473357, RMS 0.142907, non-zero samples 766910 - the stream carries audio`, and the tone it had rendered
itself as `1 kHz magnitude 0.000025 - the tone was NOT there`. `--playing` on that machine printed
`default output: Line 1 (Virtual Audio Cable)`, with Discord (pid 27020) as the only active session —
which is why the probe now names such a device and enumerates every output device instead of only the
default one.

What that proves and what it does not:

* **Proven:** on build 19045 the activation succeeds, `IAudioClient.Initialize` accepts 48 kHz float
  stereo, the capture starts and the engine delivers packets at the correct rate. The Windows version is
  not the reason anything failed — `0x8000000E` was the non-agile completion handler, and the
  documentation's "minimum 20348" is a documentation statement, not a runtime limit.
* **Not proven by those runs:** that the packets contained a specific application's audio. The old
  "0 frames flagged silent" line was read as "audio arrived", which was wrong — the flag is never set for
  this kind of stream, so it says nothing at all.
* **Explained afterwards, and it was the probe's own bug:** the "tone was NOT found" line of that run
  was wrong. The captured buffer is interleaved stereo, and the Goertzel search analysed it as if it were
  mono; doubling every sample moves a 1 kHz tone to 500 Hz, so a search at 1 kHz finds nothing at all —
  measured with the fixed function, the same buffer reads `0.1999` at 500 Hz and `0.0000` at 1 kHz when it
  is read as mono. `saf::dsp::toneMagnitude` now takes the channel count, the probe passes the negotiated
  one and prints it in `--json`, and `native/test/dsp_tests.cc` locks the difference down. The tone of
  that run therefore proves nothing either way; the honest reading is that a tone played by the same
  process is at best weak evidence, which is why `--spawn-tone` (a second process) exists.
* `1478376832 frames per packet` is not a latency, it is the engine's meaningless answer for a process
  loopback client. The probe validates the value now and prints "not a usable value" instead of a number
  that looks like evidence.

The current build of the probe reports the numbers that make a run interpretable — packets, frames,
frames flagged silent, non-zero samples, peak, RMS and the level in dBFS — prints whether any of them is
missing (`--json` carries the same fields), and adds `--spawn-tone`, `--tone-only`, `--system`, `--playing`
and `--report` so that a real answer can be produced without guessing.

### The decisive run (probe v0.1.4, same machine, 2026-10-16)

Numbers as they came back from the user's console, one line per run:

```text
--spawn-tone --seconds 8      798 packet(s), 383040 frames, peak 0.200000, 760190 non-zero samples
                              tone process exited with code 0
                              1 kHz magnitude 0.198461 - the tone is in the capture
--spawn-tone --system         800 packet(s), 384000 frames, peak 0.200000, 766910 non-zero samples
                              1 kHz magnitude 0.199965 - the tone is in the system audio
--process audiorepeater.exe   799 packet(s), 383520 frames, peak 0.000031, 580972 non-zero samples
                              (the Virtual Audio Cable repeater, with nothing audible to repeat)
--playing                     default output: Line 1 (Virtual Audio Cable)
                              six active output devices listed, one active session:
                              audiorepeater.exe (pid 16696) on "Speakers (Realtek(R) Audio)"
```

What this establishes, and why it changed the probe:

* **The process loopback capture follows the content of another process on build 19045.** A second copy of
  the probe played 1 kHz for the whole window; the capture of *that* process reported a peak of exactly
  0.200000 (the tone's amplitude) and a Goertzel magnitude of `0.198461` for 1 kHz. That is content, not a
  correctly paced stream of zeros — the question this whole probe exists to answer.
* **The device path is fine too.** The same check through the whole system loopback on the machine's
  default output device came back at `0.199965`.
* **The level reporting exists because of the third run.** `audiorepeater.exe` delivered 580972 samples
  that were not exactly zero at a peak of `0.000031` — that is −90.2 dBFS, the noise floor of the capture
  path. Reading that as "audio arrived" would have been wrong, so a window is now reported as *audio* only
  above −60 dBFS, and the classification is printed next to the numbers it was derived from.
* **The machine's default output is a virtual cable** (`Line 1`) with a repeater feeding the Realtek
  speakers. The plugin captures applications and changes no routing, so this does not affect it; a
  `--system` test on such a line measures what is fed *into* the cable, which the probe now states.
* **The first report lost the end of its `[data]` line to line wrapping.** The numbers are on their own
  shorter lines now (`[data]` + two `[levels]` lines, each under ~110 characters), and `--report <file>`
  writes the same log to a file in UTF-8 so a run can be handed over without going through a console
  window at all. Non-ASCII device names are converted to the console's code page instead of being printed
  as UTF-8 into a code page 866 console, which is what turned the Steam device names into block noise in
  that same run.

What is still needed from this machine is listed in [REMEDIATION.md](REMEDIATION.md): the plugin's own
support check and the OBS control next to it, and then the Discord interception experiment (stage 4).

## Building it yourself

MSVC (Developer Command Prompt), from `native/tools/process_loopback_probe`:

```bat
cl /nologo /std:c++17 /EHsc /W4 /D_USE_MATH_DEFINES /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Fe:process-loopback-probe.exe main.cc ..\..\src\hr_advice.cc ^
   /link ole32.lib oleaut32.lib uuid.lib mmdevapi.lib
```

Nothing is downloaded, and no Visual Studio project is required.
