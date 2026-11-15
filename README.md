# TaskbarVolume — Win顺 taskbar per-app volume plugin

Ctrl+wheel over a taskbar app icon adjusts **that app's** volume; over the
tray volume icon it adjusts the **master** volume. Ctrl+left-click toggles
mute. The `vol` keyword does the same thing from the keyboard.

English code/comments per project convention; user-facing strings follow the
host locale (zh/en).

## Usage

Mouse (only plain Ctrl is recognized; Alt/Win/Shift combos, simulated input
and everything outside the taskbar pass through untouched):

| Pointer over        | Ctrl+wheel                    | Ctrl+left-click          |
| ------------------- | ----------------------------- | ------------------------ |
| Taskbar app icon    | that app's session volume ±N% | toggle that app's mute   |
| Tray volume icon    | master volume ±N%             | toggle master mute       |
| Recognized, no target | event swallowed, overlay explains why | same           |
| Anything else       | passthrough                   | passthrough              |

Keyboard: type `vol` (or `音量`). An empty query lists master volume and all
per-app sessions; typing filters sessions by name (read-only — nothing
changes until an action runs). Enter toggles mute, Ctrl+Enter volume up,
Shift+Enter volume down.

A small non-activating overlay shows the new value while you adjust: wheel
changes show the app name and a large percentage with a bar (`Chrome 35%`),
mute and blocked results show a one-line message. Its position is the
`overlayAnchor` setting (center by default) on the monitor under the pointer,
nudged by `overlayOffsetX/Y`; the optional `mouse` anchor follows the pointer
but can be covered by the taskbar's hover preview. It disappears
automatically.

### How the tray volume icon is found

The tray button is recognized by Explorer's XAML class name
`SystemTray.OmniButtonRight` together with the AutomationId `SystemTrayIcon`,
so detection does not depend on the Windows display language (the localized
`音量`/`Volume` accessible name is only a fallback). Its reported rectangle is
24×48 px on a 100 % taskbar, and the gap to the network button is 4 px, so the
hit area is widened by a quarter of the button's short side — never past a
neighbouring tray button. That keeps a click that lands a few pixels off (a
remote-desktop pointer, a scaled high-DPI taskbar, a touchpad) working while
the adjacent network and clock buttons keep their own clicks.

## Requirements

- Windows 10/11 (taskbar UIA: `Shell_TrayWnd`, Win11 XAML buttons,
  classic `MSTaskListWClass`).
- Win顺 0.22.0 or newer: this is a community native package (`exe`) and
  the plugin library lists it as `unreviewed-native`.
- The Win顺 host running this resident plugin (`volume.exe` must sit
  next to `plugin.json`).
- Build from source with CMake + a C++17 compiler (MinGW-w64 or MSVC):

  ```powershell
  cmake -S . -B build
  cmake --build build --config Release
  ```

  `volume.exe` is placed in the repository root next to
  `plugin.json`; `tvb_tests.exe` and `VolumeTestTarget.exe` land in the
  build tree (`build/`, or `build/<config>/` with a multi-config
  generator such as Visual Studio).

## Release and provenance

`v0.1.1` is the source tag for the package attached to this repository's
release. It is built from this repository at that tag with CMake 3.29,
Ninja and MinGW-w64 GCC 13.2 (UCRT):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The exe is statically linked (the `-static` link option in
[CMakeLists.txt](CMakeLists.txt)), so `volume.exe` does not need
libstdc++/libgcc/winpthread DLLs on the target machine. The release
archive `ra9fael.volume-0.1.3.wsplugin` contains `plugin.json`,
`volume.exe`, this README and the LICENSE; its SHA-256 is in the release
notes.

Only the MinGW `-static` build is published. Do not ship an MSVC build of
this plugin: its default dynamic CRT pulls in `VCRUNTIME140.dll` /
`MSVCP140.dll`, which the plugin host does not bundle, and the plugin then
fails to load on a machine without the Visual C++ redistributable. If you
do build with MSVC, keep the `MSVC_RUNTIME_LIBRARY` setting in
[CMakeLists.txt](CMakeLists.txt) and verify with
`objdump -p volume.exe | findstr VCRUNTIME` that nothing is listed.

This is a community native plugin: Win顺 0.22.0 or newer, and the plugin
library marks it `unreviewed-native`. Installing and every update require
the user's confirmation; the public source, build command and SHA-256
document provenance but are not a code review.

## Permissions

`permissions: ["ui"]` — that is all this plugin needs and all it gets:

- UI Automation to read taskbar buttons and the tray volume icon.
- Window hit-testing / `EnumWindows` for the title→process mapping.
- A global low-level mouse hook and the overlay window.

It deliberately does **not** request `elevated`, `processes`, `network`,
`system:settings` or clipboard permissions, and it never injects input.

## Data location

Nothing is persisted. The plugin writes no files; `WS_DATA_DIR` is unused.
No keys, no network, no telemetry.

## Failure handling

- Audio service unavailable → query returns a grayed "音频服务不可用" row;
  actions notify the failure instead of pretending to succeed.
- Remote sessions: while RDP is connected the default output device is
  `Remote Audio` and everything works. When the session disconnects that
  endpoint disappears (`GetDefaultAudioEndpoint` returns `E_NOTFOUND`, zero
  active render endpoints), so gestures are still recognized but report
  "cannot read the system volume / mute state". The failure is never cached —
  each operation re-enumerates the endpoint — so reconnecting restores it
  without restarting the plugin.
- An app button is mapped to its process by the UIA AutomationId
  (`Appid: <AppUserModelID>`), the implicit `{GUID}\…\app.exe` id, or the
  executable name; the window title is only the last-resort fallback.
- Audio sessions are matched by process id and by executable path: Chromium
  apps (Edge/Chrome) play audio in a renderer/utility process whose pid
  differs from the window's browser process.
- Sessions are collected from **every active output device**, not only the
  default one: an app whose output was pointed elsewhere (Settings → Volume
  Mixer, an app's own device choice) is still controllable. The value the
  overlay shows and steps from belongs to the session that is actually playing
  (an active session wins over an idle one of the same app, then the default
  device breaks ties), so a leftover silent session can no longer make the
  numbers move while nothing is heard. The `vol` list names the device when a
  session is not on the default one.
- Output device changes are followed. Core Audio does not report an error for
  a device that merely stopped being the default - it stays alive and keeps
  accepting `SetMasterVolumeLevelScalar`, and the read-back agrees - so the
  bound endpoint is compared with the current default before every operation
  and rebound when they differ. Switching output, plugging in a headset or
  changing the default in Settings takes effect on the next wheel notch;
  `plugins.log` gets one `following output device …` line per device. The
  default is the `eConsole` endpoint, with `eMultimedia` and then
  `eCommunications` as fallbacks for a role that has no default.
- Ambiguous button (several distinct processes match it) → the mouse event
  is swallowed and the overlay says the target cannot be determined; the
  plugin never guesses.
- No audio session for a resolved app → swallowed with a "no audio session"
  overlay; neither half of the click reaches the taskbar as a phantom click.
  Windows only creates a per-app session once the app opens a stream, and drops
  it again after the last one ends, so an app that has never played sound since
  it started shows that message until it does. An app that has played keeps an
  idle session, which is still controllable: setting its volume before playback
  works.
- While a taskbar change is queued for the next snapshot rebuild (at most
  ~0.6 s, usually less), app-button gestures pass through instead of acting
  on the previous layout; the tray volume button keeps working. The taskbar's
  per-second clock name change no longer triggers a rebuild, so this window is
  now rare (measured 2–8 % of samples instead of ~40 %).
- Device invalidation → the backend rebinds once and retries; a failed
  rebind is reported instead of retried against a released device.
- Setting values out of range → fall back to defaults.
- A failed UI Automation init or a lost `SetWinEventHook` is logged once to
  `plugins.log` and retried every 5 s on the resolver thread, so a plugin that
  started before the taskbar was ready recovers without a restart.
- `WH_MOUSE_LL` installation retries with growing backoff (300 ms, 1 s, 5 s,
  then every 30 s) instead of leaving the plugin gesture-less.
- Win11/Win10 taskbars, multiple monitors and secondary taskbars are all
  refreshed every second and shortly after window/taskbar changes on a
  background thread while mouse gestures are enabled; turning the gestures
  off stops both the hook and the refresher.

## Testing

```powershell
ctest --test-dir build          # 67 unit tests + 2 audio-backed integration tests
```

Layered suite:

- **Unit tests (67, run anywhere)**: mouse decisions, hit-rect widening, wheel
  accumulation, volume math, settings, session ordering (`session_pick.h`) and
  the worker's queued operations against a fake backend (stepping, clamping,
  mute toggling, failure and "no session" wording in both languages).
- **`tools/e2e_protocol.py`** (needs an audio endpoint): drives the real
  `volume.exe` the way the host does — initialize / query / action / shutdown
  over JSON lines. Checks the list's shape, steps one session down and back up
  by exactly `percentPerStep`, toggles the master mute and restores it, and
  asserts a clean exit code with no exception in the log.
- **`tools/e2e_target.py`** (needs an audio endpoint): a closed loop with
  `tools/VolumeTestTarget`, a controlled process streaming silence. The plugin's
  write must show up in the target's own `ISimpleAudioVolume` read and the
  target's session must appear in the plugin's list — the plugin's opinion is
  checked against the process that owns the stream.
- **Manual**: taskbar-button mapping, Ctrl+wheel/click over app and tray icons,
  the overlay and the event-driven snapshot refresh were checked by hand on
  Windows 11 (injecting real mouse events would fight the user's desktop, so
  these stay manual). `wsbench` remains optional.

Both integration tests exit with code 77 on a machine without an audio
endpoint, which ctest reports as *Not Run* instead of failing — a headless CI
runner stays green without weakening the assertions themselves.

## License

MIT — see [LICENSE](LICENSE).
