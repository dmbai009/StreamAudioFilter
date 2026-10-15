# Remediation: stages, status and evidence

This document tracks the work items of the audit that followed the first real test run on
**Windows 10 Pro 22H2 (build 19045.7725)**. It exists so that "fixed", "verified" and "still open" are
never confused: every row names the evidence, and anything that has not been executed on a real machine
says so.

Statuses: **done** (changed and, where possible, verified by an automated test or a command whose output
is quoted), **in progress**, **open**, **blocked on the user's machine** (needs Windows + Discord).

| # | Stage | Status | Evidence |
|---|---|---|---|
| 1 | WASAPI activation failure (`0x8000000E`) | done, and reproduced/fixed on the real machine | root cause: the completion handler was not agile; fixed in `native/src/win/com_ptr.h`. The user's run on 19045.7725 with the fixed build reports `[handler] ... IAgileObject: yes`, `[activation-call] accepted`, `[activate-result] S_OK` — the failure is gone |
| 2 | Separated diagnostics per stage | done | twelve stages in the probe, four activation stages in the capture, every HRESULT named and explained (`native/src/hr_advice.cc`); `systemMessage()` no longer prefixes its own separator (`E_FAIL - - text`) |
| 2.1 | What "audio arrived" means | done | the `AUDCLNT_BUFFERFLAGS_SILENT` flag is never set for a process loopback stream, so both the probe and the capability walk used to report samples for a stream of zeros. Both now count non-zero samples and the peak through one portable, tested implementation: `native/src/dsp/packet_measure.h`, 33 new checks in `native/test/dsp_tests.cc` (95 total). `GetBufferSize()` is reported only when the value is plausible (the probe saw `1478376832` frames) |
| 2.2 | The tone search itself (found by re-reading the first real run) | done | the first run's "1 kHz magnitude 0.000025 / 0.000007" was a measurement bug, not a property of the capture: the search treated the interleaved stereo capture as mono, and sample repetition moves a 1 kHz tone to 500 Hz. `saf::dsp::toneMagnitude(samples, count, sampleRate, frequency, channels)` now downmixes first. Portable proof: the same stereo buffer reads `0.1999` at 500 Hz (mono reading) and `0.0000` at 1 kHz, and `0.1999` at 1 kHz with `channels=2`. The probe passes the negotiated channel count and reports it in `--json` |
| 3 | Standalone process loopback probe | done, and run on the real machine | `native/tools/process_loopback_probe/main.cc`, `docs/PROBE.md`; released as `process-loopback-probe.exe`; `--playing` now lists every output device with the processes playing on it (and warns when the default output is a virtual cable), `--system` is the control over the whole system audio, `--spawn-tone` is the decisive content check (a second copy of the probe plays the tone and is the capture target), `--tone-only` is its child mode, a missing target is a usage error (`exit 2`) instead of an HRESULT report |
| 4 | Discord interception (the share audio never reaches the plugin) | open | two warnings were observed; the pipeline has not been investigated yet — next stage |
| 5 | Honest safety: fail closed, explicit unfiltered opt-in, distinct states | partially done (contract exists) | `stream/intercept.ts` `decideAudio(...)`, tests 13/13; the UI states and the opt-in default are part of stage 7 |
| 6 | Engine / multi-app re-verification | done for the engine | 97 portable checks, including a two-instance restart with measured peaks (`restart took 111 ms, peak levels 0.256 / 0.256`) |
| 7 | UI / UX (theme tokens, one panel, message classes, localisation) | open | the screenshot shows two overlapping panels, one almost transparent |
| 8 | Stability (single source of truth for the share state, no duplicate listeners) | open | — |
| 9 | Install / update (ZIP + minimal steps, version compatibility gate) | partially done | ZIP route and the pinned bundle exist; the TS/native version gate is open |
| 10 | Tests / CI (no skipped critical steps, releases gated) | mostly done, extending | CI builds the addon with MSVC, runs the portable tests under `g++` *and* `cl.exe`, runs the TypeScript tests, and refuses to publish when any of them fails |
| 11 | Security / complexity audit | open | scheduled after the functional stages, as requested |

## Stage 1 — the `0x8000000E` failure

### Confirmed, with the evidence

1. **The completion handler was not an agile COM object.** `ActivateAudioInterfaceAsync` is documented to
   return `E_ILLEGAL_METHOD_CALL` (0x8000000E) when the handler does not implement `IAgileObject` (or
   aggregate the free threaded marshaler), because Windows calls `ActivateCompleted` from a worker thread
   of the multi threaded apartment. The plugin's handler was a plain reference counted object, so *every*
   activation failed, before the audio engine ever considered the process, the format or the build
   number. Fixed: `ComCallback` answers `IAgileObject`, and the probe verifies it as a stage of its own.
2. **The error was reported without its meaning.** The user saw
   `0x8000000E — Метод вызван в неожиданное время` in the panel: the code, plus the localized system
   text, plus — worse — text that suggested a version problem. Every HRESULT the plugin can produce now
   has a name and an explanation, and the explanation of `0x8000000E` explicitly rules the version
   explanation out (`native/src/hr_advice.cc`, asserted by the tests).
3. **Four different failures were collapsed into one.** The call, the completion callback,
   `GetActivateResult` and `QueryInterface` were one `if`. They are four stages now, each with its own
   HRESULT, and the capability probe walks twelve stages on a thread of its own.
4. **A lifetime hazard in the activation.** The handler was released before the asynchronous operation,
   although Windows holds a reference to the handler until the operation is gone; the declaration order
   is fixed and commented.

### Hypotheses — status after the first probe run

| Hypothesis | Status |
|---|---|
| The machine (19045.7725) supports process loopback, and the agility bug was the only reason the capture failed | **Confirmed for the stream, in part by content.** The user's run reached `[capture-start]`, `[data] 799 packet(s), 383520 frames` for the probe's own tree and the same for `chrome.exe`, i.e. activation, format negotiation, `Start` and packet delivery all work on 19045; the `chrome.exe` capture reported `peak 0.317145, RMS 0.079421, 764032 non-zero samples`, so real audio was in the packets |
| The packets contain the audio of the captured process | **Answered for one application, better test pending.** `--process chrome.exe` reported `764032 non-zero samples` with `peak 0.317145` while Chrome was playing, so a process loopback stream does follow a real application's audio on 19045. The remaining question — whether a *specific* tone of a *specific* other process comes through — is what `--spawn-tone` answers decisively, and the tone-search bug of row 2.2 had made every earlier tone reading useless |
| The tone the probe played was missing from both captures | **Falsified as a fact about the capture.** It was the measurement bug of row 2.2 (interleaved stereo searched as mono). The probe now downmixes, reports the channel count, and keeps the honest reading: a tone the probe plays *itself* is advisory, a tone from a second process (`--spawn-tone`) is decisive |
| The unfiltered audio the viewer heard came from Discord's own capture, because Discord desktop does not request the share audio through the patched `mediaDevices` API | the interception investigation (stage 4) — a real experiment with a second Discord client, and a hook that either exists or does not |
| The see-through panel is a CSS stacking/opacity problem plus duplicated mounts rather than a Discord theme incompatibility | stage 7, verified against the theme tokens and by counting panel mounts |
| "Excluded from the share" was shown while the toggle was on because the row text describes the mode, not the toggle state | stage 7 — the row text will describe the toggle, and the mode only sets the default for applications without a rule |

### What only the user's machine can confirm

```text
1. process-loopback-probe.exe --playing                            (who is playing, and on which device)
2. process-loopback-probe.exe --spawn-tone --seconds 8             (decisive content check)
3. process-loopback-probe.exe --spawn-tone --system --seconds 8    (the same on the device path)
4. process-loopback-probe.exe --process <that process>.exe --seconds 8   (the real application)
5. the same, then the plugin, with OBS's "Application Audio Capture" as a control
```

The first version of this list was run and its output is quoted in [PROBE.md](PROBE.md) and in section 1
above. What is still needed from the machine: run 3 with `--playing` output, because only that shows
whether a real application's audio is inside the packets (the `non-zero samples` and `peak` numbers), and
run 2, because the control test is the one that can prove the device path. Send back the stage log (or
`--json`).

### What is *not* claimed

* No result in this repository has been produced on a machine with a real Discord screen share except
  where explicitly stated; the failures quoted above come from the user's screenshots.
* The fix was not "verified working on Windows" by CI: CI proves that the sources compile with MSVC and
  that the portable tests pass. A green CI is not a green capture — that is exactly what the probe and
  the user's run are for.

## Standing rules this work keeps to

* Fail closed: if the filtered track is missing and the user has not opted in, the share is sent without
  audio instead of with everybody's audio.
* The unfiltered fallback is opt-in, off by default, and carries the warning
  «Если фильтрация не работает, зрители смогут слышать звук всех приложений».
* No virtual audio device, no change to Windows or Discord audio settings, no per application volume
  change, no administrator rights, no telemetry, and no audio written to disk without consent.
* Nothing is reported as working because it compiles or because a component renders, and a measurement
  that cannot distinguish "packets arrived" from "audio arrived" is not evidence.
