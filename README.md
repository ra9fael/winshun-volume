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

## Requirements

- Windows 10/11 (taskbar UIA: `Shell_TrayWnd`, Win11 XAML buttons,
  classic `MSTaskListWClass`).
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
- An app button is mapped to its process by the UIA AutomationId
  (`Appid: <AppUserModelID>`), the implicit `{GUID}\…\app.exe` id, or the
  executable name; the window title is only the last-resort fallback.
- Audio sessions are matched by process id and by executable path: Chromium
  apps (Edge/Chrome) play audio in a renderer/utility process whose pid
  differs from the window's browser process.
- Ambiguous button (several distinct processes match it) → the mouse event
  is swallowed and the overlay says the target cannot be determined; the
  plugin never guesses.
- No audio session for a resolved app → swallowed with a "no audio session"
  overlay; neither half of the click reaches the taskbar as a phantom click.
  Windows only exposes per-app volume for live sessions, so a silent app
  shows that message until it plays sound (Chromium apps drop their session
  when playback stops).
- While a taskbar change is queued for the next snapshot rebuild (at most
  ~0.6 s, usually less), app-button gestures pass through instead of acting
  on the previous layout; the tray volume button keeps working.
- Device invalidation → the backend rebinds once and retries; a failed
  rebind is reported instead of retried against a released device.
- Setting values out of range → fall back to defaults.
- Win11/Win10 taskbars, multiple monitors and secondary taskbars are all
  refreshed every second and shortly after window/taskbar changes on a
  background thread while mouse gestures are enabled; turning the gestures
  off stops both the hook and the refresher.

## Testing

```powershell
ctest --test-dir build          # unit tests (pure logic, 43 cases)
```

Layered suite: 43 pure-logic unit tests (mouse decisions, wheel
accumulation, volume math, settings, stale-snapshot pass-through) run
anywhere; protocol round-trips were verified against the real process
(initialize/query/action/shutdown over JSON-RPC); `tools/VolumeTestTarget`
provides a controlled window + real audio session for interactive E2E.
Taskbar-button mapping, Ctrl+wheel/click over app and tray icons, the
overlay and the event-driven snapshot refresh were checked manually on
Windows 11. `wsbench` remains optional.

## License

MIT — see [LICENSE](LICENSE).
