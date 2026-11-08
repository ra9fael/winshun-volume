#!/usr/bin/env python3
"""Package this plugin as dist/<id>-<version>.wsplugin.

Usage:
  python tools/package.py              # package the committed files + the built exe
  python tools/package.py --tag v1.2.3 # also require the tag to match plugin.json

What goes in: every file tracked by git (``git ls-files``), except paths that
start with a dot (.github, .gitignore, ...), plus ``runtime.program`` from
plugin.json (the built exe, which is not committed). The zip is deterministic
(sorted names, fixed timestamps, stored, not compressed), so the same inputs
always give the same sha256. plugin.json sits at the archive root, which is
what WinShun's "install from archive" expects.

Outputs in dist/: <id>-<version>.wsplugin, its .sha256, and release-notes.md
(the matching '## [version]' section of CHANGELOG.md plus the checksum). With
--tag, a missing CHANGELOG section is an error.

Standard library only, Python 3.10+.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXED_TIME = (2020, 1, 1, 0, 0, 0)
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")
MAX_BYTES = 256 << 20  # WinShun's archive limit (src/plugin/Install.h)


def fail(msg: str) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return 1


def tracked_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, check=True,
                         capture_output=True).stdout.decode("utf-8")
    names = [n for n in out.split("\0") if n]
    # tools/verify/ holds Windows-only dev harnesses; they are not part of
    # the shipped plugin.
    return [n for n in names
            if not any(part.startswith(".") for part in n.split("/"))
            and not n.startswith("tools/verify/")]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", help="release tag; must be v<version from plugin.json>")
    ap.add_argument("--out", default=str(ROOT / "dist"), help="output folder (default: dist/)")
    args = ap.parse_args()

    manifest = json.loads((ROOT / "plugin.json").read_text(encoding="utf-8"))
    pid, version = manifest.get("id", ""), manifest.get("version", "")
    if not VERSION_RE.fullmatch(version):
        return fail(f"plugin.json version {version!r} must be three numbers like 1.2.3")
    if ROOT.name != pid:
        print(f"note: folder name {ROOT.name!r} differs from id {pid!r} (fine for a clone; "
              f"the installed folder is named after the id)")
    if args.tag is not None and args.tag != f"v{version}":
        return fail(f"tag {args.tag!r} does not match plugin.json version {version!r} (expected v{version})")

    program = str(manifest.get("runtime", {}).get("program", ""))
    if not program.lower().endswith(".exe"):
        return fail(f"runtime.program {program!r} is not an exe; this script packages native plugins")
    if not (ROOT / program).is_file():
        return fail(f"{program} not found; build first (cmake -B build && cmake --build build)")

    files = sorted(set(tracked_files()) | {program})
    if "plugin.json" not in files:
        return fail("plugin.json is not committed")
    missing = [f for f in files if not (ROOT / f).is_file()]
    if missing:
        return fail(f"tracked but missing on disk: {missing}")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    target = out / f"{pid}-{version}.wsplugin"
    total = 0
    with zipfile.ZipFile(target, "w", zipfile.ZIP_STORED) as z:
        for name in files:
            data = (ROOT / name).read_bytes()
            total += len(data)
            info = zipfile.ZipInfo(name, FIXED_TIME)
            info.compress_type = zipfile.ZIP_STORED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            z.writestr(info, data)
    if total > MAX_BYTES or target.stat().st_size > MAX_BYTES:
        target.unlink()
        return fail(f"package exceeds WinShun's {MAX_BYTES >> 20} MB limit")

    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    (out / f"{target.name}.sha256").write_text(f"{digest}  {target.name}\n", encoding="ascii")
    print(f"{target.name}: {len(files)} files, {target.stat().st_size} bytes, sha256 {digest}")

    notes = changelog_section(version)
    if notes is None:
        if args.tag is not None:
            return fail(f"CHANGELOG.md has no '## [{version}]' section; add one before tagging")
        print(f"note: CHANGELOG.md has no '## [{version}]' section yet")
    else:
        notes += f"\n\n**sha256** `{digest}`  `{target.name}`\n"
        (out / "release-notes.md").write_text(notes, encoding="utf-8")
    return 0


def changelog_section(version: str) -> str | None:
    """Return the body of '## [version] ...' in CHANGELOG.md, or None."""
    path = ROOT / "CHANGELOG.md"
    if not path.is_file():
        return None
    body: list[str] = []
    inside = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            if inside:
                break
            inside = line.startswith(f"## [{version}]")
            continue
        if inside:
            body.append(line)
    text = "\n".join(body).strip()
    return text if inside and text else None


if __name__ == "__main__":
    sys.exit(main())
