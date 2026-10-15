# The standalone process loopback probe

`process-loopback-probe.exe` is a console program that answers one question, on your machine, without
Discord: **does this Windows really provide the per application audio capture the plugin needs, and if
not, which step refuses it?**

It is built from source by CI (see `.github/workflows/native.yml`) and attached to every release next to
the addon, so no compiler is needed to get a diagnosis. The source is
`native/tools/process_loopback_probe/main.cc`; it uses the plugin's own agile completion handler
(`native/src/win/com_ptr.h`) and the plugin's HRESULT explanations (`native/src/hr_advice.cc`), but
nothing else — no Discord, no Vencord, no capture engine.

## What it does — and what it refuses to do

It installs nothing, changes no setting, touches no device, and **writes no audio to disk** (there is no
code path in it that opens a file). The loudest thing it can do is play a 1 kHz tone from its own process
for a few seconds, and only when you ask for it with `--self-tone`.

## Usage

```text
process-loopback-probe.exe --list
process-loopback-probe.exe --self-tone
process-loopback-probe.exe --self-tone --seconds 8
process-loopback-probe.exe --process spotify.exe --seconds 8
process-loopback-probe.exe --pid 1234
process-loopback-probe.exe --process chrome.exe --exclude
process-loopback-probe.exe --self-tone --json
```

| Option | Meaning |
|---|---|
| `--list` | list running processes (pid, image name, path) — use it to find the name for `--process` |
| `--pid <n>` | capture the tree of this process |
| `--process <name.exe>` | capture the first running process with this image name |
| `--self-tone` | play 1 kHz from the probe itself for the whole capture window and look for that tone in the captured audio |
| `--exclude` | capture everything *except* the target tree (the mode the plugin uses for "all except selected") |
| `--seconds <n>` | capture window, 1–300, default 5 |
| `--json` | one line of machine readable JSON instead of the stage log |

Exit codes: `0` — every stage succeeded (and, with `--self-tone`, the tone was found in the capture);
`1` — a stage failed (the stage log names it); `2` — usage error.

## The stages, and what a failure in each one means

| Stage | What it does | If it fails |
|---|---|---|
| `platform` | prints the real Windows version (ntdll, not the manifest-dependent `GetVersionEx`) | — |
| `com` | `CoInitializeEx(COINIT_MULTITHREADED)` on the probe thread | `CO_E_NOTINITIALIZED` / `RPC_E_CHANGED_MODE`: an apartment problem, not an audio problem |
| `handler` | verifies that the completion handler answers `IAgileObject` | without it Windows returns `E_ILLEGAL_METHOD_CALL` (0x8000000E) from the call below — the plugin bug that this probe exists to make visible |
| `target` | resolves the process to capture | no such process, or an invalid pid |
| `params` | prints the activation payload (`VT_BLOB`, pid, include/exclude mode) | a malformed payload produces `E_INVALIDARG` in the next stage |
| `activation-call` | `ActivateAudioInterfaceAsync` itself | **the interesting stage**: the exact HRESULT with its meaning. `0x8000000E` = handler/apartment, `E_INVALIDARG` = parameters, `E_NOTIMPL` = this build has no such activation |
| `activation-wait` | waits for `ActivateCompleted` (5 s) | the callback never came — a driver/audio-service level problem |
| `activate-result` | `GetActivateResult` and the result it reports | two separate HRESULTs on purpose: the operation failing is not the activation failing |
| `interface` | `QueryInterface(IAudioClient)` | the activated object is not an audio client |
| `client-init` | asks for 48 kHz float stereo with `AUTOCONVERTPCM`, falls back to 16 bit PCM | `AUDCLNT_E_UNSUPPORTED_FORMAT` etc. — a format problem, not a capture problem |
| `capture-client` | `IAudioCaptureClient` | the client cannot deliver buffers |
| `buffer-size` | prints the packet size | — |
| `capture-start` | `SetEventHandle` + `Start` | the stream did not start — the reason is quoted |
| `data` | reads packets for the capture window and counts frames, silent frames and the negotiated format | no packet at all means the stream exists but nothing arrives: silence of the source, or a paused/stopped process |
| `self-tone` | the 1 kHz magnitude found in the captured audio (Goertzel) | the tone was played but not captured — the capture is not following the process |
| `teardown` | `Stop`, release, close the apartment | — |

## Reading the result

* **All stages green, `--self-tone` finds the tone** — the machine supports the capture, end to end. If
  the plugin then fails, the difference is in the plugin or in Discord, not in Windows.
* **`activation-call` fails with `0x8000000E`** — the completion handler was not usable from the calling
  apartment. In this probe that cannot happen any more (it verifies the agility one stage earlier), so
  the output would show `handler: NO` in the same run.
* **`activation-call` fails with `E_NOTIMPL` / `E_INVALIDARG`** — the build does not provide this
  activation, or the parameters were rejected. This is the honest "not supported here" case.
* **Only `data` is empty / silent** — the capture works; the chosen process simply did not play anything
  during the window. Use `--self-tone`, or start playback in the application and retry.

## Building it yourself

MSVC (Developer Command Prompt), from `native/tools/process_loopback_probe`:

```bat
cl /nologo /std:c++17 /EHsc /W4 /D_USE_MATH_DEFINES /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Fe:process-loopback-probe.exe main.cc ..\..\src\hr_advice.cc ^
   /link ole32.lib oleaut32.lib uuid.lib mmdevapi.lib
```

Nothing is downloaded, and no Visual Studio project is required.
