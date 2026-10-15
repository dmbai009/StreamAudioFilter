# The interface: four entry points

Every panel draws the same component (`ui/FilterControls.tsx`), so the state cannot drift apart:
switching an application in the popover next to the microphone is immediately visible in the settings.

| # | Where | What it is | How to switch it off |
|---|---|---|---|
| 1 | Settings → Plugins → StreamAudioFilter | the full panel: mode, list, state, errors, diagnostics | — (this is the standard place) |
| 2 | A button next to the microphone and headphones at the bottom of the window | a compact popover with the same list | the "button next to the microphone" setting |
| 3 | A floating window inside Discord | a 400×… panel on top of the client, appearing by itself when a share starts | the "floating panel" setting; collapse/expand with the button in its title bar, hide with the cross |
| 4 | Vencord Toolbox | two entries: "show the panel", "reset the filters" | switch the Toolbox off |

## What the panel says about the audio the viewers get

The audio state is not the same thing as the filter state, and the panel shows exactly what the viewers
really receive:

| Status line | When | What happens then |
|---|---|---|
| "Sharing: audio sent for N apps" | the filter was applied | the viewers hear our mix |
| "Stream audio is off: filtering could not be applied" | the filter did not work | the system audio is **removed** from the share and a red card explains why |
| "Sending unfiltered system audio" | the "Send plain system audio when filtering is unavailable" setting is on | the audio of Discord as usual, but with a warning |
| "Watching applications" / "Ready" | no share is running | nothing is captured at all |

The diagnostics section carries the same information in its own "Audio at the viewers" row, with the
reason (for example "the native addon was not found") in the row below. That is deliberate: the state
"the filter does not work" must not be mistaken for "the filter works" on the strength of a switch alone.

## The full panel

```
Stream audio
Choose which applications the viewers hear while you share your screen. …

Watching applications                            ← state
Windows 11 (build 26100) · capture: wasapi · 48000 Hz · buffer delay ~60 ms

[ Everything except excluded ] [ Only included ]   [ Refresh list ] [ Reset filters ]

SECTION "Currently playing audio" ─────────────────────────── 3
  [icon]  Google Chrome          sent to the viewers · chrome.exe · 48 kHz   ▮▮▮▯  sent       (●)
  [icon]  Dota 2                 sent to the viewers · dota2.exe            ▮▮▯▯  sent       (●)
  [icon]  Spotify                excluded from the share · Spotify.exe      ▮▮▮▮  excluded   (○)

SECTION "Running, but silent" ─────────────────────────────── 1
  [icon]  Telegram               not playing audio right now                ▯▯▯▯  no audio   (●)

SECTION "Saved rules (application closed)" ────────────────── 1
  [icon]  Discord (voice call)   Application closed · excluded by default…    (○)

Cannot see the application you need? Start playing audio in it, then refresh the list.
The setting only affects what the viewers hear: your own audio stays as it is.

▸ Diagnostics and details
```

The states of a row: **sent** (green), **excluded**, **no audio**, **waiting for audio** (switched on
but the application is silent), **error**, **unavailable**. The indicator next to the name is the
application's level in the last mixed block (cheap: the value is already computed for the limiter).

## Why there is no button in the share launch window

Separately, because it was part of the wish list.

* The screen share launch window and the panel of a running share are internal Discord components. They
  have neither a public API nor stable selectors; the only way to put a button there is to patch code by
  matching substrings that Discord changes between builds.
* The price of a mistake is high: a bad patch can break the very window a share is started from, and
  debugging that on somebody else's machine is close to impossible.
* So a predictable route was chosen: **the panel comes to the user at the right moment**. As soon as
  Discord asks for the screen share audio, the plugin raises the floating window (if enabled) and
  highlights the state on the button next to the microphone. Functionally it is the same action —
  changing the composition of the audio on the fly — but it does not depend on the internals of the
  client.
* If Discord ever grows a stable API for those screens, the panel can be added there without touching
  the rest of the code: the whole interface is one component that does not care where it is rendered.

## The floating window

* dragged by its title bar, the position is not remembered between launches (deliberately: screens
  differ);
* collapses to the strip "Stream audio · 3 cap." — it collapses **by itself** while a share is running
  so that it does not end up in the frame when the whole monitor is shared (the "collapse the floating
  panel while a share is running" setting);
* the cross does not only hide the window, it also switches the setting off so that the panel does not
  come back unexpectedly;
* while a screen is shared, any part of the Discord interface can end up in the frame — that is the
  behaviour of the client, not of the plugin, which is why the panel is minimal by default.
