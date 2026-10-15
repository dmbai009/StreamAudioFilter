# Limitations and how support is checked

This document answers the main question: **can the viewers be given the audio of only some of the
applications at all, without creating virtual devices and without changing system settings.** The
answer: yes, from Windows 10 version 2004 (build 19041) on; older builds simply do not contain the
mechanism, and that limitation cannot be worked around by the "routing" we were asked not to keep quiet
about.

## 1. What Windows can do

Capturing the audio of a single process is implemented in Windows through **WASAPI process loopback**:
`ActivateAudioInterfaceAsync` with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` and
`PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`.

* Microsoft: `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` — "Minimum supported client: Windows 10
  Build 20348"
  (<https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ne-audioclientactivationparams-audioclient_activation_type>);
* the same threshold is named for `IAudioClient` when it is activated with `AUDIOCLIENT_ACTIVATION_PARAMS`
  (<https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdevice-activate>);
* Microsoft's own sample `ApplicationLoopback` is built on exactly this mechanism
  (<https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/>).

The key property: the capture is **not tied to an audio endpoint**. There is no need to know where an
application plays, and no need to redirect its audio. That is the reason the requirement "do not touch
the devices or the system audio" is fulfilable at all.

### Windows builds

| Build | Version | Process loopback |
|---|---|---|
| < 19041 | Windows 10 before version 2004 (1909 and older) | no: the API is not in the system |
| 19041–19045 | Windows 10 version 2004 … 22H2 | **works** |
| 20348 | Windows Server 2022 | works |
| 22000+ | Windows 11 | works |

Why the table has two "lower bounds": Microsoft documents
`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` as "Minimum supported client: Windows 10 Build 20348" —
that is the Server 2022 build on which the API and its helper header `audioclientactivationparams.h`
were published. In reality the mechanism is present in the audio engine from **Windows 10 version 2004
(build 19041)** on:

* OBS (its "Application Audio Capture (BETA)" source, available since version 28) requires exactly
  Windows 10 2004+;
* so do third party implementations built on this API (win-capture-audio and others);
* Microsoft's own sample (`ApplicationLoopback`) and individual projects name the 19041 threshold for a
  working capture.

The conclusion: "it is impossible on Windows 10" is wrong; "Windows 10 version 2004 or newer is
required" is right. On systems older than 2004 there is no per process capture in any application — a
limitation of Windows itself, not of the plugin.

Because the documentation and reality disagree, what decides is **not the build number but a real
activation attempt**: the plugin performs it and shows the result (or the `HRESULT` with an
explanation).

Checking your own build with one command (PowerShell):

```powershell
[System.Environment]::OSVersion.Version.Build
```

or

```powershell
Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' |
    Select-Object ProductName, DisplayVersion, CurrentBuildNumber
```

The plugin performs the same check itself and, on top of that, **really attempts the activation**: the
"Check support" button in the diagnostics section starts an actual `ProcessLoopbackCapture` and prints
either success or the `HRESULT` with its meaning (`AUDCLNT_E_*`, `E_INVALIDARG`, `E_NOTIMPL` and so on).
The guess "probably unsupported" is never used: if the system can do it you will see it, if it cannot you
will see the error code.

## 2. What happens on older builds (Windows 10 before 2004)

Nothing is substituted and nothing is "worked around": per application filtering is impossible because
the operating system does not contain the API. The plugin:

* shows the reason in the panel together with the exact build and the required threshold (19041 /
  version 2004);
* enables no workarounds at all (it creates no virtual devices, redirects no application audio, changes
  no Discord settings);
* **removes the audio from the share** and explains why: if the filter does not work, unfiltered system
  audio must not reach the viewers (see section 2.1).

A path that was investigated and rejected, so that it does not come back:

| Option | Why it is not used |
|---|---|
| Capture the whole system mix and cut the unwanted part out (EQ/subtraction) | cutting an arbitrary set of applications out of an already mixed signal is mathematically impossible |
| A virtual device plus redirecting applications to it | forbidden by the requirements (items 3–5) |
| Muting or lowering the volume through `ISimpleAudioVolume` during a share | changes what the user himself hears and gives the viewers no separate mix |
| Recording "what you hear" from an endpoint (loopback) and filtering per application | plain loopback has no per process filtering at all; only process loopback has it, and that is what the plugin uses |
| Interception through a virtual driver (APO) | needs a driver installation, signing and administrator rights |

## 2.1. Failing closed: why the audio is switched off instead of passed through

A fair question: if filtering is unavailable, why not simply send the ordinary system audio the way
Discord does? Because that is exactly what the user refused by enabling the plugin:

* an "excluded" application becomes audible to the viewers;
* it happens silently — the audio itself gives no hint that the filter fell off;
* the typical scenario ("I am sharing my screen, the viewers must not hear Telegram") turns into a leak
  of private notifications into a live stream.

That is why, while filtering is enabled, the rule is: **if the filtered audio could not be assembled,
the system audio is removed from the share**, and the reason is shown in the panel (a red card and the
row "Audio at the viewers: off"). Restoring the usual Discord audio is a separate, explicit act of the
user: the setting "Send plain system audio when filtering is unavailable" (**off by default**). While it
is on, the panel warns that the audio is unfiltered.

The same rule applies to the individual failure paths:

| Failure path | What happens |
|---|---|
| The native addon is not loaded / not built | the audio is removed, the panel shows the reason and points at `scripts/build-native.ps1` |
| The system does not support per process capture | the audio is removed, the `HRESULT` of the real activation attempt is shown |
| The engine did not start (IPC error, timeout) | the audio is removed, the error is visible in the panel |
| The filtered stream died during the share (for example because the output device changed) | the audio does not "switch" to the system one: the panel reports that the filtered audio stopped |
| Discord does not use an interceptable way of obtaining the audio | the stream cannot be filtered (the plugin never saw it) — the panel reports it as an error and asks the user to switch "Share audio" off for that share |
| Filtering is switched off by the user in the plugin settings | nothing changes: the audio is sent as usual, because that is a deliberate choice, not a failure |

## 3. Compatibility with Voicemeeter, VB-Cable and other virtual mixers

A section of its own, because many users have been running Discord through Voicemeeter for years and
the obvious question is whether the filtering breaks when "the audio goes somewhere else".

It does not break, and here is why:

* **The capture follows the process, not the device.** An application does not care whether its output
  goes to `Voicemeeter Input (VB-Audio Voicemeeter VAIO)`, to a USB headset or to HDMI: process loopback
  intercepts the stream of the process regardless of its endpoint.
* Our session enumeration covers **all** active render devices, not only the default one
  (`IMMDeviceEnumerator::EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE)`), so applications that moved
  to VAIO or to a cable are still visible in the list.
* No devices are selected or switched: you can keep using Voicemeeter for the microphone and the normal
  output for applications, the plugin occupies nothing and redirects nothing.

Limitations worth knowing:

* If the **mixer application itself** (for example `voicemeeter.exe` playing something into the monitors)
  produces audio, it shows up in the list as an ordinary application and is filtered like any other.
  Passing audio "through" it to the viewers is not possible — we intercept sources, not intermediate
  mixers.
* If a game outputs into VAIO and you listen to it through Voicemeeter, while you want the share to
  carry exactly the **Voicemeeter mix** (what you hear, but without, say, Telegram), that is no longer
  the task "pick applications": a Voicemeeter mix is indistinguishable from a single source. The
  applications that are the sources have to be filtered, not the mixer.
* ASIO applications and applications using exclusive mode may not appear in the list: no WASAPI session
  is created for them. In the panel that shows up as a missing application — with the hint "start
  playing audio in it and refresh the list".

## 4. Other technical limitations of the plugin

| Limitation | Cause and what to do about it |
|---|---|
| Only a limited number of applications can be captured at once (12 by default, configurable up to 32) | every capture is a separate audio client; when the limit is exceeded a warning "part of the audio is not sent" appears instead of a silent loss. The limit counts **applications**: an application started twice is one capture inside which both instances are summed |
| An application started twice (two independent process trees) | both instances are captured and summed once; the row of the application shows "2 instances". If a second instance appears (or one closes) while a share is running, the capture of **that** application is recreated for the new process trees: the old one fades out and the new one starts empty, so that application is quiet for about **0.1 s** (measured by the engine test through the mock backend, which produces audio in real time like a real capture). The other applications, the share and the output stream keep running. Removing that gap would mean letting the old capture play until the new one has data, i.e. briefly mixing the same application twice — the engine keeps the "one instance, one capture" invariant instead of trading a silence for a doubled level |
| Two different applications where one process tree nests inside the other (a launcher and a game, for example) | they count as separate rows, the way the user sees them; if both are sent, the audio of the nested process ends up in the mix twice. That is a property of "one row = one application", not a capture error |
| Only applications that create an audio session can be captured | system sounds (notifications) are a separate pseudo session, "System sounds", which can be filtered as a whole |
| Resampling 44.1↔48 kHz and the 48→96 kHz case are done inside the plugin | systems whose device format differs are not supported "as is", so the format is converted to 48 kHz f32 internally (SNR > 70 dB, see `docs/TESTPLAN.md`) |
| The floating panel is a window inside Discord | if you share the whole monitor, the Discord window can end up in the frame. That is why the panel collapses during a share (a setting), and why the interface appearing in the frame is the behaviour of Discord, not of the plugin |
| There is no button inside the screen share launch window | that window has no public API; details and the alternative are in `docs/UI.md` |
| Rules for applications without an accessible file path (some UWP or protected processes) do not survive a restart | a process id is unfit for long term storage (it gets reused). Such rows carry a "not saved" badge |
