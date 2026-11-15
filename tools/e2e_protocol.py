#!/usr/bin/env python3
# e2e_protocol.py — protocol-level integration test: drives the real volume.exe
# the way Win顺 does (initialize / query / action / shutdown) and checks what the
# plugin answers, against the machine's real audio sessions.
#
# Exits 77 (ctest SKIP_RETURN_CODE) when this machine has no audio endpoint, so
# a headless CI runner stays green instead of failing.
#
# Usage: python tools/e2e_protocol.py [path\to\volume.exe]
import sys

from e2e_common import (PluginProcess, find_repo_exe, is_muted, master_item,
                        notify_text, parse_percent, session_items)

STEP = 2  # the default percentPerStep


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else find_repo_exe(["volume.exe"])
    expect(exe is not None, "volume.exe not found; pass its path as the first argument")

    plugin = PluginProcess(exe)
    try:
        items = plugin.query()
        if len(items) == 1 and items[0].get("id") == "audio-unavailable":
            print("SKIP: no audio endpoint on this machine")
            return 77

        master = master_item(items)
        expect(master is not None, "the vol list has no master row")
        value = parse_percent(master["subtitle"])
        expect(value is not None and 0 <= value <= 100, f"master percent out of range: {master}")
        expect(master["data"].get("target") == "master", "master row lost its target data")

        for session in session_items(items):
            expect(session["data"].get("pid", 0) > 0, "a session row without a pid")
            expect(0 <= parse_percent(session["subtitle"]) <= 100,
                   f"session percent out of range: {session}")

        # One session is stepped and stepped back: the write must be visible in
        # a fresh read, by exactly percentPerStep, and nothing may be left
        # behind. The direction avoids clamping at 0 / 100.
        sessions = session_items(items)
        if sessions:
            target = sessions[0]
            before = parse_percent(target["subtitle"])
            expect(before is not None, f"unreadable session subtitle: {target}")
            down = before > 0
            action = "volumeDown" if down else "volumeUp"
            expected = before - STEP if down else before + STEP
            undo = "volumeUp" if down else "volumeDown"
            notify_text(plugin.action(action, target))
            after = parse_percent(find_same(plugin, target)["subtitle"])
            expect(after == expected, f"{action} moved {before} to {after}, expected {expected}")
            notify_text(plugin.action(undo, target))
            restored = parse_percent(find_same(plugin, target)["subtitle"])
            expect(restored == before, f"restore left {restored}, expected {before}")
        else:
            print("note: no app session on this machine, stepping not checked")

        # Master mute is toggled and toggled back, whatever the original state.
        before_muted = is_muted(master_item(plugin.query())["subtitle"])
        notify_text(plugin.action("toggleMute", {"data": {"target": "master"}}))
        after_muted = is_muted(master_item(plugin.query())["subtitle"])
        expect(after_muted != before_muted, "toggleMute did not flip the mute state")
        notify_text(plugin.action("toggleMute", {"data": {"target": "master"}}))
        restored_muted = is_muted(master_item(plugin.query())["subtitle"])
        expect(restored_muted == before_muted, "mute state was not restored")

        code, err = plugin.close()
        expect(code == 0, f"volume.exe exited with {code}")
        expect("threw" not in err, f"the plugin threw an exception: {err[:400]}")
        print(f"OK: protocol round-trip ({len(session_items(items))} sessions, "
              f"stepped {'yes' if sessions else 'skipped'}, mute toggled)")
        return 0
    finally:
        plugin.close()


def find_same(plugin, item):
    """Re-reads the list and returns the row with the same session id."""
    for candidate in plugin.query():
        if candidate.get("id") == item.get("id"):
            return candidate
    raise AssertionError(f"session {item.get('id')!r} disappeared from the list")


if __name__ == "__main__":
    sys.exit(main())
