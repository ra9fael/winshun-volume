# Repository conventions

Use English Angular commit messages: `type(scope): imperative English summary`.
Use English bodies and `BREAKING CHANGE:` for breaking changes. User documentation may be bilingual.

Public GitHub Actions entry points are new version tag pushes and `workflow_dispatch` only.
Never add branch pushes, pull requests, schedules or other automatic triggers.
Check `github.event.created && !github.event.deleted` before tag jobs run.
Manual verification accepts a source ref and defaults `publish` to false; publishing requires an existing matching version tag.
Preserve all existing tests, variant identity checks and pinned Qt/host dependencies.
Only the publish job may request `contents: write`. Never overwrite existing release assets.
