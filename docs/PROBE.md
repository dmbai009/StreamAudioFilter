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

## What it does — and what it refuses to do

It installs nothing, changes no setting, touches no device, and **writes no audio to disk** (there is no
code path in it that opens a file: samples are counted as they are read and immediately dropped). The
loudest thing it can do is play a 1 kHz tone from its own process for a few seconds, and only when you
ask for it with `--self-tone`.

## Usage

```text
process-loopback-probe.exe --list
process-loopback-probe.exe --playing
process-loopback-probe.exe --process spotify.exe --seconds 8
process-loopback-probe.exe --pid 1234
process-loopback-probe.exe --process chrome.exe --exclude
process-loopback-probe.exe --self-tone --system --seconds 8
process-loopback-probe.exe --process chrome.exe --json
```

| Option | Meaning |
|---|---|
| `--list` | list running processes (pid, image name, path) — use it to find the name for `--process` |
| `--playing` | list the processes that hold an **active** audio session right now, and the name of the default output device — use it to pick a target that is really playing something |
| `--pid <n>` | capture the tree of this process |
| `--process <name.exe>` | capture the first running process with this image name |
| `--self-tone` | play 1 kHz from the probe itself for the whole capture window and look for that tone in the captured audio |
| `--system` | capture the **whole system audio** through a classic WASAPI loopback stream instead of a per process capture — the control test |
| `--exclude` | capture everything *except* the target tree (the mode the plugin uses for "all except selected") |
| `--seconds <n>` | capture window, 1–300, default 5 |
| `--json` | one line of machine readable JSON instead of the stage log |

Exit codes: `0` — every stage succeeded; `1` — a stage failed (the stage log names it); `2` — usage
error.

## The stages, and what a failure in each one means

| Stage | What it does | If it fails |
|---|---|---|
| `platform` | prints the real Windows version (ntdll, not the manifest-dependent `GetVersionEx`) | — |
| `com` | `CoInitializeEx(COINIT_MULTITHREADED)` on the probe thread | `CO_E_NOTINITIALIZED` / `RPC_E_CHANGED_MODE`: an apartment problem, not an audio problem |
| `handler` | verifies that the completion handler answers `IAgileObject` | without it Windows returns `E_ILLEGAL_METHOD_CALL` (0x8000000E) from the call below — the plugin bug that this probe exists to make visible |
| `target` | resolves the process to capture | no such process, or an invalid pid (a deliberate `E_INVALIDARG` when nothing was given) |
| `params` | prints the activation payload (`VT_BLOB`, pid, include/exclude mode) | a malformed payload produces `E_INVALIDARG` in the next stage |
| `activation-call` | `ActivateAudioInterfaceAsync` itself | **the interesting stage**: the exact HRESULT with its meaning. `0x8000000E` = handler/apartment, `E_INVALIDARG` = parameters, `E_NOTIMPL` = this build has no such activation |
| `activation-wait` | waits for `ActivateCompleted` (5 s) | the callback never came — a driver/audio-service level problem |
| `activate-result` | `GetActivateResult` and the result it reports | two separate HRESULTs on purpose: the operation failing is not the activation failing |
| `interface` | `QueryInterface(IAudioClient)` | the activated object is not an audio client |
| `client-init` | asks for 48 kHz float stereo with `AUTOCONVERTPCM`, falls back to 16 bit PCM | `AUDCLNT_E_UNSUPPORTED_FORMAT` etc. — a format problem, not a capture problem |
| `capture-client` | `IAudioCaptureClient` | the client cannot deliver buffers |
| `buffer-size` | the packet size, **only when the value is plausible** | a process loopback client reports no meaningful packet size; the line says so instead of printing a number (see "What a green run means") |
| `capture-start` | `SetEventHandle` + `Start` | the stream did not start — the reason is quoted |
| `data` | packets, frames, frames flagged silent, **non-zero samples**, peak, RMS and the negotiated format | only a stream that carries no samples at all is reported as empty; "packets arrived" and "audio arrived" are counted separately |
| `self-tone` | how much this program rendered, and the 1 kHz magnitude found in the captured audio (Goertzel, threshold 0.05) | reported and explained, but **never** turned into a verdict by itself — see below |
| `teardown` | `Stop`, release of every interface, close the apartment | — |

## Reading the result

* **All stages green and `non-zero samples` > 0** — audio really flows through the capture. Values that
  are *not* proof: a `data` line with a high frame count, or `0 frames flagged silent`. A process
  loopback stream does not set the silent flag, so that number says nothing; the count of non-zero
  samples and the peak are what separate audio from a correctly paced stream of zeros.
* **`activation-call` fails with `0x8000000E`** — the completion handler was not usable from the calling
  apartment. In this probe that cannot happen any more (it verifies the agility one stage earlier), so
  the output would show `handler: ... yes/no` in the same run.
* **`activation-call` fails with `E_NOTIMPL` / `E_INVALIDARG`** — the build does not provide this
  activation, or the parameters were rejected. This is the honest "not supported here" case.
* **`data` reports `no audio arrived`** — the capture works; the chosen process did not play anything
  during the window. `--playing` finds a process that is playing, `--system` checks the path with the
  whole system audio.
* **`self-tone` says the tone is not in the capture** — this happens, and it does **not** mean the
  capture is broken. The probe captures *itself* (its own process tree), and the audio of the process
  that performs the capturing is not guaranteed to appear in its own capture. The line tells you which
  half is responsible: the tone was never rendered, the stream carried only zeros, or the stream carried
  other audio and not the tone. Use `--system` for the decisive check of that path, or `--pid <player>`
  with a player that is really playing.

### Why `--system` exists

`--system` runs the identical measurement over the whole system audio with a classic WASAPI loopback
stream. It plays the tone through the same output device and looks for it there:

* tone found → the device, the audio engine and this program are all fine; any problem in a per process
  run is specific to per process capture;
* tone not found although it was rendered → the audio never reached the device at all, which has nothing
  to do with per process capture (and would also explain a silent share in Discord).

That is the control test which makes a `--self-tone` result in the per process mode interpretable. A
control test that fails is a real failure and exits with `1`.

## What was measured on Windows 10 Pro 22H2 (build 19045.7725)

The first run of this probe on the target machine, before the measurement was fixed (34 lines of stage
log, quoted from the user's console):

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

and the same run with `--process chrome.exe` (pid 12340) reached exactly the same stages, then reported
"the process loopback capture works on this system".

What that proves and what it does not:

* **Proven:** on build 19045 the activation succeeds, `IAudioClient.Initialize` accepts 48 kHz float
  stereo, the capture starts and the engine delivers packets at the correct rate. The Windows version is
  not the reason anything failed — `0x8000000E` was the non-agile completion handler, and the
  documentation's "minimum 20348" is a documentation statement, not a runtime limit.
* **Not proven by those runs:** that the packets contained a specific application's audio. The old
  "0 frames flagged silent" line was read as "audio arrived", which was wrong — the flag is never set for
  this kind of stream, so it says nothing at all.
* **Explained afterwards:** the missing 1 kHz tone was the probe's own tone, played by the very process
  that was doing the capturing; that arrangement is not a valid test, and the probe now says so instead of
  failing.
* `1478376832 frames per packet` is not a latency, it is the engine's meaningless answer for a process
  loopback client. The probe validates the value now and prints "not a usable value" instead of a number
  that looks like evidence.

The current build of the probe reports the four numbers that make a run interpretable — packets, frames,
frames flagged silent, non-zero samples — plus the peak and RMS, and it adds `--system` and `--playing`
so that a real answer can be produced without guessing.

## Building it yourself

MSVC (Developer Command Prompt), from `native/tools/process_loopback_probe`:

```bat
cl /nologo /std:c++17 /EHsc /W4 /D_USE_MATH_DEFINES /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Fe:process-loopback-probe.exe main.cc ..\..\src\hr_advice.cc ^
   /link ole32.lib oleaut32.lib uuid.lib mmdevapi.lib
```

Nothing is downloaded, and no Visual Studio project is required.
