# Changelog

[中文](CHANGELOG.md)

## [0.1.6] - 2026-11-16

### Fixed

- Adjusting a single app's volume still dropped a step now and then: session
  volume is applied by the audio engine asynchronously, so a read right after
  a write can still return the previous value. The old read-back verification
  turned such good writes into dropped steps, and read-modify-write computed
  the next step from the stale value. A write now counts as done when
  `SetMasterVolume` returns S_OK, and the intended target is remembered so a
  continuous gesture advances from it (expires after 1.2 s; external changes
  are still picked up). The master volume is unaffected — endpoint volume
  applies synchronously. New closed-loop regression: a rapid burst of 6 down
  and 6 up steps must restore the target process's own volume exactly.
- The overlay no longer dodges the taskbar's hover band — it covers it: a
  bottom-anchored panel (and a follow-mouse panel that lands there) sits flush
  on the work area's bottom edge and keeps re-asserting topmost while visible,
  so the tray's own `Device: 65%` bubble stays underneath.
- Only events that really move taskbar button rectangles invalidate gestures
  (a window being destroyed or hidden reflows the row, as does the taskbar's
  own geometry); a window *appearing* — apps that show titled helper windows
  continuously, like desktop lyrics or OSD popups — now schedules a silent
  rebuild instead of interrupting a gesture.

## [0.1.5] - 2026-11-16

### Fixed

- Gestures often died mid-scroll (the overlay vanished, the volume stopped
  moving, then worked again): any window renaming itself marked the snapshot
  stale and dropped app-button gestures. Some apps rename windows every few
  hundred milliseconds (a VPN client's status line, browser tab titles,
  notification toasts), so the snapshot was stale a large share of the time.
  A rename never moves or removes a taskbar button; it now schedules a silent
  rebuild without invalidating gestures, while events that can actually
  change the layout (a window appearing or closing, the taskbar's own
  geometry) keep the old behaviour. Toolwindows — tooltips, toasts, hover
  previews — cannot own a taskbar button and no longer trigger a rebuild at
  all.
- The master volume panel was titled "System volume"; it now names the output
  device the value applies to (`Headphones (SHEYIN X5)`), matching the tray's
  own tooltip. "System volume" remains the fallback when the device cannot be
  named, and the mute messages use the name too.
- A bottom-anchored panel was covered by the taskbar's hover tooltip (the
  `Device: 65%` bubble the tray pops up while you scroll). Bottom placements
  — and a follow-mouse panel that lands there — now sit one taskbar height
  above the work area's bottom edge, clear of the tooltip / hover-preview
  band the taskbar draws above itself; monitors without a bottom taskbar are
  unaffected.
- The overlay's hide timer could fire for a deadline that had already been
  replaced (a queued `WM_TIMER` survives a `SetTimer` reset), hiding a panel a
  fresh wheel step had just updated. The hide now checks the recorded
  deadline and re-arms for the remaining time.

### Added

- The overlay's placement math is a pure function with unit tests (anchors,
  offsets, edge clamping, hover-band clearance — previously untested); the
  suite grows to 80 tests.

## [0.1.4] - 2026-11-16

### Fixed

- Taskbar gestures kept adjusting the **previous** output device after the default
  one changed. A device that merely stopped being the default does not fail: it
  stays alive and keeps accepting `SetMasterVolumeLevelScalar`, and the read-back
  agrees, so the overlay showed the new value while the volume and mute did
  nothing until the plugin process was restarted. The bound endpoint is now
  compared with the current default before every operation and rebound when they
  differ; `plugins.log` gets one `following output device …` line per device, so
  the log shows which device the plugin follows.
- Sessions were only enumerated on the default device. When an app's output was
  pointed elsewhere (Settings → System → Sound → Volume Mixer), the plugin either
  reported "no audio session" or wrote the value into a same-named stale session
  on the default device (numbers moving, sound unchanged). Every active output
  device is enumerated now.
- With several sessions of the same app, only the first one was read - which can
  be one that stopped playing long ago (one process can leave an idle session at
  100 % and another at 92 %), so the wheel started from a stale value. The
  session that is actually playing is read first; the default device only breaks
  ties.
- Master volume only tried the `eConsole` role and failed outright when that role
  had no default. It now tries `eConsole`, then `eMultimedia`, then
  `eCommunications`.
- A WebView2 host's audio could not be controlled from its taskbar button: the
  audio session belongs to `msedgewebview2.exe`, the button resolves to the host
  (for example `ms-teams.exe`), and matching only accepted the same pid or the
  same executable path. The session's parent chain is now walked (up to three
  steps) when neither matches; descendants of system hosts (explorer, svchost,
  SearchHost, RuntimeBroker, ShellExperienceHost) are excluded, since that chain
  identifies no app.
- Removed the leftover merge-conflict markers from `CHANGELOG.md` and
  `CHANGELOG.en.md` (the 0.1.1 section was trapped inside a conflict block).

### Changed

- A session listed by `vol` that is not on the default device now names its device
  in the subtitle.
- The plugin no longer packages a custom app.ico; it uses the same system glyph
  icon as the other plugins. The volume.exe icon itself is unchanged.

### Added

- Layered automated tests. 73 unit tests: the worker's queued operations are
  exercised against a fake backend (stepping, clamping, mute toggling, the
  failure and "no session" wording in both languages) - this path had no
  coverage at all before - and so is app matching (pid, executable path,
  parent chain with its depth cap).
- `tools/e2e_protocol.py`: drives the real `volume.exe` the way the host does
  (initialize / query / action / shutdown over JSON lines). Checks the list's
  shape, steps one session down and back up by exactly `percentPerStep`, toggles
  the master mute and restores it, and asserts a clean exit code with no
  exception in the log.
- `tools/e2e_target.py`: a closed loop with `tools/VolumeTestTarget`, a
  controlled process streaming silence. The plugin's write must be visible in
  the target's own `ISimpleAudioVolume` read, and the target's session must
  appear in the plugin's list - the plugin's opinion is checked against the
  process that owns the stream.
- `tools/e2e_device_switch.py`: the regression test for the stale binding.
  Uses the same undocumented `IPolicyConfig` interface as the Settings app to
  switch the system default output while a real `volume.exe` runs, and asserts
  that the plugin logs a rebind, reads the new device's master volume and
  returns to the original value. It only runs with `TVB_E2E_DEVICE_SWITCH=1`
  (it changes the global default) and must declare the interface as IUnknown
  based: the default IDispatch layout puts every method on the wrong vtable
  slot and crashes the caller. All three are wired into ctest (Windows only);
  the audio-backed ones skip with exit code 77 when the machine has no audio
  endpoint, and the switch test skips unless the environment variable is set.

### Notes

- Restart the plugin process after upgrading.

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

## [0.1.1]

- Preserve the current plugin behavior and add a reproducible tag/manual build, test and package workflow.
