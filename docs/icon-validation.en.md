# Icon fix 0.1.2

[中文](icon-validation.md)

## Problem

Embedding an ICO alone can leave native windows using a generic icon.

## Cause

Executable resources do not assign window icons. IDI_ICON1 is a string resource name. Qt windows need an explicit application icon resource.

## Changes

Embed the executable icon and include the ICO and resource script in the package. Include the existing workspace fix for UI Automation header ordering. Use the static MSVC runtime for the plugin executable.

## Measured

2026-11-09: built a committed snapshot with MSVC 19.51 / x64 Release; CTest 1/1 passed. Resource and real plugin process checks: 16/16 passed (docs/icon-checks.json), including the named resource, eight sizes, protocol initialization and clean shutdown.

## Not tested

Physical taskbar/Alt+Tab interaction, every plugin feature, hardware paths, dragging across actual monitors, UAC secure desktop and all Windows versions were not retested. This run creates local tags and packages only; nothing is published.
