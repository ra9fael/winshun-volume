# Build and release

Push a new version tag to test, package and publish automatically. Updating or deleting an existing tag skips the pipeline. Branch pushes and pull requests do not run Actions.

For verification, run `release.yml` manually with `publish=false` (the default) and `ref` set to the source branch or SHA. The artifact is built once from the resolved SHA and includes a wsplugin and its SHA-256. All existing CTest suites run; project-specific validation remains enabled.

For manual publication, set `ref` to an existing version tag and `publish=true`. Its version, changelog, variant and branch ancestry must match. Use `vX.Y.Z` for single-variant plugins, `gdi-vX.Y.Z` / `qml-vX.Y.Z` for BinaryLab, and `gdi/vX.Y.Z` / `qml/vX.Y.Z` for EnvHosts. The tag is checked again before publishing the same verified artifact. Existing releases are never overwritten; a rerun reports that a release already exists.

Commit subjects and bodies must be English Angular style, for example `ci(release): restrict workflows to tags and manual runs`. The selected source commit is checked; unrelated historical commits are not rewritten.
