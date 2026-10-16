# Test plan

Automated tests prove the parts; only a real share with a second Discord client proves the result.
Nothing below counts as "works in Discord" unless the viewer heard it.

## Automated (CI, every push)

| Suite | What it proves |
|---|---|
| `native/test/dsp_tests.cc`, `engine_tests.cc` (g++ and MSVC) | mixing, fades, limiter, rules, multi instance capture, process tree conflicts, Discord protection, pull mode level, dead capture restart, `apps()` during retirement |
| `tests/host.test.mjs` (Linux and Windows build) | host protocol: version/protocol, listing, capture gate, rules reaching the mix (output level), protected Discord tree, bad input |
| `tests/setup.test.mjs` (Windows, fake profile) | install, repair, uninstall back to the original `app.asar`, existing Vencord kept/restored, version mismatch refused, rollback, refusal while Discord runs |
| `tests/rules.test.ts` | switch semantics, modes, migration, audio routers |
| `tests/soundshare.test.ts` | fail closed decision table, own stream detection, the patch regexes against Discord's code |
| `dev/typecheck.sh` | tsc + eslint inside Vencord v1.15.11 |

## Real machine

Needs two Discord accounts (the second one can run on a phone) in one voice channel.

### A. Capture

1. Start a share of the whole screen with sound.
2. Play audio in two applications (e.g. a browser video and a music player).
3. In the panel both rows show a moving level and "sent"; no "no audio" chip, no problem.
4. Diagnostics: "Discord attached: true", output level above 0.

### B. What the viewer hears (the criterion)

1. Headline: "Viewers hear only the allowed apps".
2. Viewer hears both applications. You hear both.
3. Switch one application off. Within about a second the viewer stops hearing it; you still hear it.
4. Switch it on again: the viewer hears it again; the share was not restarted.
5. Speak: the viewer hears your microphone as usual. The video is unchanged.
6. Change the default (Allowed / Blocked): applications without a rule follow it.
7. Start a third application while sharing: it follows the default.
8. Stop the share: headline "Not sharing"; in the volume mixer the muted session stays only while you are
   in a voice channel.

### C. Failure behaviour

1. Settings → "Filter the audio of whole screen shares" off → share → headline "Filtering is off",
   viewer hears everything (Discord's normal behaviour).
2. Rename `saf-audio-host.exe`, restart Discord, share → headline "Share audio is off", viewer hears
   nothing. With "Send unfiltered audio when filtering fails" on → "Viewers hear ALL apps".
3. Share a single window → "Sharing a window", viewer hears that application only.

### D. Installer

1. Install over an existing Vencord, check that other plugins' settings are unchanged.
2. Uninstall: the previous Vencord is back.
3. Discord update (or delete the newest `app-*` injection): run the installer again → Repair.

Copy the diagnostic report (full panel) for any failure.
