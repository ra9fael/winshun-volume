# Icon fix 0.1.2

[中文](icon-validation.md)

## Problem

Embedding an ICO alone can leave native windows using a generic icon.

## Cause

Executable resources do not assign window icons. IDI_ICON1 is a string resource name. Qt windows need an explicit application icon resource.

## Changes

Embed the executable icon and include the ICO and resource script in the package. Include the existing workspace fix for UI Automation header ordering. Use the static MSVC runtime for the plugin executable.

## Measured

Final build and window checks will be recorded before local packaging.

## Not tested

This local icon fix does not constitute full hardware or desktop interaction acceptance.
