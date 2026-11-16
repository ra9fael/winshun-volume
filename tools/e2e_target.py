#!/usr/bin/env python3
# e2e_target.py — closed-loop test with a controlled audio session: the plugin's
# write must be visible inside the process that owns the stream, and the
# controlled session must be visible to the plugin's read.
#
# VolumeTestTarget opens a window and streams silence, then answers `get` with
# its own {"pid", "volume", "muted"} - the truth from inside the target process,
# not the plugin's opinion about it.
#
# Exits 77 (ctest SKIP_RETURN_CODE) when this machine has no audio endpoint.
#
# Usage: python tools/e2e_target.py <volume.exe> <VolumeTestTarget.exe>
import json
import subprocess
import sys
import time
import uuid

from e2e_common import (PluginProcess, session_items, notify_text, SKIP_EXIT_CODE)

STEP = 2


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else None
    target_exe = sys.argv[2] if len(sys.argv) > 2 else None
    expect(exe is not None and target_exe is not None,
           "usage: e2e_target.py <volume.exe> <VolumeTestTarget.exe>")

    title = "TVB E2E " + uuid.uuid4().hex[:8]
    target = subprocess.Popen([target_exe, title], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, encoding="utf-8", errors="replace", bufsize=1)

    def ask(command):
        target.stdin.write(command + "\n")
        target.stdin.flush()
        line = target.stdout.readline()
        return json.loads(line) if line else None

    try:
        state = ask("get")
        expect(state is not None and state.get("pid", 0) > 0,
               "the test target produced no audio session")
    except AssertionError:
        # A target that exits says why: "session=0" is a machine without an
        # audio endpoint (skip), anything else is a broken helper (fail). A
        # target with no endpoint exits before the quit command, so the write
        # may hit an already-closed pipe - the diagnosis is on stderr either
        # way, and must be read after the process is reaped.
        try:
            target.stdin.write("quit\n")
            target.stdin.flush()
        except OSError:
            pass
        try:
            target.wait(timeout=5)
        except subprocess.TimeoutExpired:
            target.kill()
            target.wait(timeout=5)
        reason = target.stderr.read()
        if "session=0" in reason:
            print("SKIP: no audio endpoint on this machine")
            return SKIP_EXIT_CODE
        raise AssertionError(f"the test target failed to start: {reason.strip()[:300]}")

    plugin = PluginProcess(exe)
    try:
        # The session is created by an active stream, so it may take a moment to
        # show up in the session manager's enumeration.
        item = None
        for _ in range(20):
            items = plugin.query(text="VolumeTestTarget")
            item = next((s for s in session_items(items)
                         if s["data"].get("pid") == state["pid"]), None)
            if item is not None:
                break
            time.sleep(0.3)
        expect(item is not None,
               "the controlled session never appeared in the plugin's list")

        # The plugin's own read must agree with what the target reports.
        expect(parse_ok(item["subtitle"], state["volume"]),
               f"plugin says {item['subtitle']}, target says {state['volume']}%")

        notify_text(plugin.action("volumeDown", item))
        after = ask("get")
        expect(after["volume"] == state["volume"] - STEP,
               f"target reports {after['volume']}%, expected {state['volume'] - STEP}")

        notify_text(plugin.action("volumeUp", item))
        expect(ask("get")["volume"] == state["volume"], "volume was not restored")

        # A continuous gesture is a burst of read-modify-write steps back to
        # back. The engine applies session volume asynchronously, so each step
        # must follow the intended target, not the lagging read-back, or steps
        # get lost and the value stutters.
        burst = 6
        for _ in range(burst):
            notify_text(plugin.action("volumeDown", item))
            time.sleep(0.08)
        mid = ask("get")
        expect(mid["volume"] == state["volume"] - burst * STEP,
               f"after a burst of {burst} down steps the target reports "
               f"{mid['volume']}%, expected {state['volume'] - burst * STEP}")
        for _ in range(burst):
            notify_text(plugin.action("volumeUp", item))
            time.sleep(0.08)
        expect(ask("get")["volume"] == state["volume"],
               "the burst was not undone exactly")

        notify_text(plugin.action("toggleMute", item))
        expect(ask("get")["muted"] is True, "target did not see itself muted")

        notify_text(plugin.action("toggleMute", item))
        expect(ask("get")["muted"] is False, "mute state was not restored")
        expect(ask("get")["volume"] == state["volume"], "final volume drifted")

        target.stdin.write("quit\n")
        target.stdin.flush()
        target.wait(timeout=5)
        code, err = plugin.close()
        expect(code == 0, f"volume.exe exited with {code}")
        expect("threw" not in err, f"the plugin threw an exception: {err[:400]}")
        print(f"OK: closed loop with pid {state['pid']} "
              f"(stepped, muted, unmuted, restored)")
        return 0
    finally:
        try:
            target.stdin.write("quit\n")
            target.stdin.flush()
            target.wait(timeout=5)
        except Exception:
            target.kill()
        plugin.close()


def parse_ok(subtitle, volume):
    import re
    match = re.search(r"(\d+)%", subtitle)
    return match is not None and int(match.group(1)) == volume


if __name__ == "__main__":
    sys.exit(main())
