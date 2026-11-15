# e2e_common.py — shared driver for the protocol-level tests: talks to a real
# volume.exe over the JSON-lines protocol, one request per line, answers matched
# by id. Notifications (winshun/log, query/partial, ...) are ignored.
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time

DEFAULT_TIMEOUT = 15.0
# ctest's SKIP_RETURN_CODE: the test reports "Not Run" instead of failing.
SKIP_EXIT_CODE = 77


class Skipped(Exception):
    """Raised when the machine cannot run the test (no audio endpoint)."""


class PluginProcess:
    def __init__(self, exe, settings=None, locale="en"):
        if settings is None:
            settings = {"enableMouse": False}  # never install hooks from a test
        self._out = queue.Queue()
        self._stderr_lock = threading.Lock()
        self._stderr = []
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        self.proc = subprocess.Popen(
            [exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1, creationflags=flags)
        threading.Thread(target=self._pump, args=(self.proc.stdout, self._out), daemon=True).start()
        threading.Thread(target=self._pump, args=(self.proc.stderr, None), daemon=True).start()
        self._next_id = 1
        self.request("initialize", {"settings": settings, "locale": locale, "theme": "dark",
                                    "pluginDir": ".", "dataDir": "."})

    def _pump(self, stream, sink):
        for line in stream:
            if sink is None:
                with self._stderr_lock:
                    self._stderr.append(line.rstrip("\r\n"))
            else:
                self._out.put(line)

    def request(self, method, params, timeout=DEFAULT_TIMEOUT):
        rid = self._next_id
        self._next_id += 1
        line = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError(f"no answer to {method} within {timeout}s")
            try:
                raw = self._out.get(timeout=left)
            except queue.Empty:
                continue
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue
            if msg.get("id") == rid:
                if "error" in msg:
                    raise RuntimeError(f"{method} answered with an error: {msg['error']}")
                return msg["result"]

    def query(self, text=""):
        result = self.request("query", {"queryId": self._next_id, "command": "vol",
                                        "trigger": "keyword", "text": text, "keyword": "vol"})
        return result["items"]

    def action(self, action, item):
        result = self.request("action", {"command": "vol", "action": action, "text": "",
                                         "item": item})
        return result["effects"]

    def stderr_text(self):
        with self._stderr_lock:
            return "".join(self._stderr)

    def close(self):
        """Shuts down and returns (exit code, everything the plugin logged)."""
        try:
            self.request("shutdown", {})
        except Exception:
            pass
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()
        return self.proc.returncode, self.stderr_text()


def notify_text(effects):
    for effect in effects:
        if effect.get("type") == "notify":
            return effect.get("text", "")
    return ""


def master_item(items):
    for item in items:
        if item.get("data", {}).get("target") == "master":
            return item
    return None


def session_items(items):
    return [item for item in items if item.get("data", {}).get("target") == "session"]


def parse_percent(subtitle):
    match = re.search(r"(\d+)%", subtitle)
    return int(match.group(1)) if match else None


def is_muted(subtitle):
    return "muted" in subtitle


def find_repo_exe(names):
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    for name in names:
        for candidate in (os.path.join(root, name),
                          os.path.join(root, "build", name),
                          os.path.join(root, "build", "Release", name)):
            if os.path.isfile(candidate):
                return candidate
    return None
