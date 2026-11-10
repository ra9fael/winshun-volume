# Changelog

<<<<<<< HEAD
## [0.1.1]

- Preserve the current plugin behavior and add a reproducible tag/manual build, test and package workflow.
=======
[中文](CHANGELOG.md)

## [Unreleased]

## [0.1.3] - 2026-11-10

### Fixed

- Ctrl+click on the taskbar volume icon sometimes did nothing: the tray button's
  hit area is only 24 px wide and a 4 px dead gap sits to its left, so remote-desktop
  pointer rounding, high-DPI scaling or a touchpad only had to land a couple of pixels
  off. The hit rect is now widened on every side up to the neighbouring buttons
  (measured 24 px → 34 px), covering the gap without stealing clicks from the
  network or clock buttons.
- The volume button is identified by Explorer's class name
  `SystemTray.OmniButtonRight` plus the AutomationId `SystemTrayIcon` instead of the
  localized "音量/Volume" name, so it works under any display language; the name
  list remains only as a fallback.
- A failed UI Automation initialization is no longer silent: it is logged to
  `plugins.log` with the HRESULT text. Before, it looked like "installed but
  completely dead".
- The resolver heals itself: a lost `SetWinEventHook` (Explorer restart,
  remote-session reattach) is re-armed every 5 s, and a failed UIA init retries on
  the same cadence, so the plugin no longer needs a restart.
- `WH_MOUSE_LL` installation retries with 300 ms/1 s/5 s/30 s backoff instead of
  giving up after 3 attempts, and reports when it recovers.
- `plugin.json` now has a top-level `icon`, so the plugin list no longer shows a
  placeholder.
- The taskbar fires `EVENT_OBJECT_NAMECHANGE` every second because the clock's
  accessible name contains the time; that used to leave the app buttons stale
  (passing gestures through) roughly 40 % of the time. Only taskbar name
  changes with `OBJID_WINDOW` / `OBJID_CLIENT` are accepted now: measured
  stale samples dropped from 41–49 % to 2–8 %, with the volume button working
  in 100 % of the 10 s samples.

### Notes

- Local tag and package only. Restart the plugin process after upgrading.
- Measured on a remote session: while RDP is connected the default output is
  `Remote Audio` (`GetDefaultAudioEndpoint` succeeds, master volume is
  readable and writable); after RDP disconnects that endpoint disappears and
  `GetDefaultAudioEndpoint` returns `E_NOTFOUND` with zero active endpoints.
  The gestures are still recognized (UI Automation works), but volume
  operations report "Cannot read the system volume / mute state". The audio
  backend does **not** cache that failure - every operation re-enumerates the
  endpoint - so it recovers when the session reconnects without a restart.

## [0.1.2] - 2026-11-09

### Fixed

- Embed the executable icon and include the ICO and resource script in the package. Include the existing workspace fix for UI Automation header ordering. Use the static MSVC runtime for the plugin executable.

### Notes

- Local tag and package only. See docs/icon-validation.en.md for the measured coverage; no GitHub release. Restart the plugin process after upgrading.
>>>>>>> 08067e4 (fix(volume): package and assign plugin icons)
