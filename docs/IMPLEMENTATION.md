# Client/server implementation status

This document summarizes the implemented PacSmith client/server boundaries. The normative design is in [ARCHITECTURE.md](ARCHITECTURE.md).

## Programs

- `pacsmithd` is the Go library owner. It owns SQLite, content-addressed artifacts, inspection, release preparation, builds, update jobs, repository publication, PKI, and server secrets.
- `pacsmith` is the C++/Qt CLI and shared HTTP client. It also hosts the stdio MCP server through `pacsmith mcp`.
- `pacsmith-gui` is the human workbench and external-harness launcher.

All library clients use the same `LibraryClient` and HTTP API. Local clients use the configured Unix socket. Remote clients use the configured HTTPS/mTLS identity and server pin. MCP has no database or direct-daemon shortcut.

## External agent integration

PacSmith does not contain an LLM client. Provider clients, subscription sign-in, API-key settings, model catalogs, one-shot review endpoints/jobs, and embedded AI review UI were removed.

`pacsmith mcp` implements newline-delimited JSON-RPC over stdio. Stdout is protocol-only and diagnostics go to stderr. The tool catalog exposes typed reads and ordinary domain operations rather than a generic release-document patch:

- project and release discovery/details;
- acquisition, artifact, inspection, dependency, payload, lifecycle, AppRun, launcher, desktop, update, recipe, build, and repository state;
- artifact import, normal metadata and Guided edits, lifecycle files, Custom PKGBUILD/support files, builds, reanalysis, deletion, and publication operations.

`get_release_issues` exposes the same structural review rules used to derive the GUI's release review state. It returns typed issue codes, categories, subjects, remediation text, build-blocking hints, and independent `review_complete`, `has_successful_build`, and `maintenance_complete` values. Build records now retain their exact artifact relationships; migration 8 associates legacy retained build artifacts with the most recent successful build so old releases no longer report `never-built` with empty build artifact lists.

Custom support-file operations are also exposed to people through `pacsmith custom-file`; they are not an agent-only capability.

The tool descriptions stand alone, while the Skill under `agent-plugin/skills/pacsmith/` supplies trust and packaging judgment. The source and installed package contain an Agent Plugins 1.0 bundle with a root `plugin.json` and an `mcp.json` stdio declaration for executable `pacsmith` with argument `mcp`; `pacsmith plugin path` prints that bundle directory. `pacsmith skill install` atomically installs or upgrades the standalone user copy at `~/.agents/skills/pacsmith`, and `pacsmith skill path` prints the active Skill directory. An unmanaged existing directory is preserved unless the user explicitly supplies `--force`.

`check_updates` runs the same deterministic update-check operation as the GUI/CLI for one project or all projects, records discovery evidence, and honors automatic-preparation settings without invoking a model. Generic harness profiles have typed list/upsert/remove/default tools backed by `AppSettingsStore`, the same persistence used by the GUI. An executable and each argument remain separate strings; MCP never constructs a shell command.

If the Skill loads without MCP, it requires a consent question before installation and treats missing MCP as a hard stop. It prohibits CLI project commands, direct HTTP/socket/D-Bus access, daemon management, database access, and filesystem scraping as fallbacks. After consent, the harness maps the portable plugin or `pacsmith mcp` command into its own configuration and approval UX. PacSmith emits no harness-specific configuration and uses neither ACP nor an online harness registry.

## MCP permissions

Tool annotations include all four MCP behavior hints. PacSmith additionally enforces mandatory form elicitation for:

- deleting a project;
- deleting a release;
- resetting and reanalyzing a release's maintained setup;
- enabling/disabling or renaming a project's published repository package;
- promoting a repository package to stable;
- changing global repository/listener/signing configuration or trust keys;
- changing automatic update/preparation/retention policy;
- changing desktop-session autostart;
- changing the remote management listener or client enrollment/trust;
- storing or deleting the server-side GitHub credential.

These checks live in the central MCP permission policy. Missing client elicitation support, an elicitation error, cancellation, decline, disconnect, or a response without the explicit boolean confirmation all fail closed. Ordinary maintained-state edits do not require this extra round trip.

All mutating project/release calls require human-readable selectors from `list_projects`; opaque IDs are rejected for those tools. PacSmith resolves selectors internally. For sensitive operations, it also uses the verified display/package name and release version in its elicitation message.

Package installation and repository bootstrap/trust installation are not exposed through MCP. Bootstrap scripts can be read as inert text, but MCP never executes them. If system installation is added later, it must first exist as an ordinary human-facing PacSmith feature and use the same mandatory-confirmation boundary.

## Guided and Custom recipes

Guided mode remains a finite model of behaviors PacSmith understands: Arch package description/homepage/license and compatibility metadata, inspected dependency treatments, evidence-backed additional runtime dependencies, payload decisions, supported layouts, launchers/wrappers, AppRun, desktop entries, lifecycle responsibilities/scripts, icons, and update sources. Arbitrary package-specific Bash or filesystem work requires Custom PKGBUILD.

When preparing a successor to a Custom release:

1. PacSmith copies the preceding applicable Custom PKGBUILD verbatim.
2. PacSmith copies only explicit custom text support files.
3. PacSmith regenerates release-owned identity and `pacsmith.vars` from the new verified artifact.
4. PacSmith does not copy source objects, inspection state, signing-key artifacts, build output, logs, or other release-owned data.

The predecessor remains unchanged. The next update inherits the newest applicable edited Custom recipe. PacSmith never attempts to merge shell code.

Custom recipes should source `pacsmith.vars` and use `_PACSMITH_*` values for moving version, pkgrel, architecture, artifact filename, checksum, AppImage offset, and related identity. Static validation warns when a Custom recipe ignores these variables. Custom mode does not opt out of deterministic update discovery, verification, inspection, or building.

## Compatibility

Migration 7 resets legacy library AI settings, deletes retired provider credential metadata, and terminally fails queued/running legacy AI jobs with a clear message. Existing AI columns remain in SQLite so upgrades do not require a risky table rebuild; the public settings API neither returns nor accepts them.

The client settings loader ignores old provider/model fields. Generic harness profiles are stored in the client settings file as executable plus an argv array and default flag. No shell command string is persisted or evaluated.

`MainWindow` watches the client-settings directory because `QSaveFile` atomically replaces `settings.json`, which invalidates a file-only watch. Changes are debounced, reloaded into the active GUI, and reflected in an open Settings dialog. While that dialog is open it also refreshes revisioned library/repository settings and GitHub credential status through the configured API. Dialog-local dirty tracking prevents an MCP write from overwriting unsaved harness, session, library, or repository edits.

Historical `ValueOrigin::Ai` and AI provenance records remain readable so old project documents can be decoded. The obsolete `previousManualPkgbuild` JSON field is safely ignored. New code does not create provider decisions or `previous-manual-PKGBUILD`; successor Custom releases use copy-forward instead.

## Deterministic updates

Update discovery and automatic preparation never invoke an AI model. The pipeline remains acquire/verify, inspect, compare/carry known decisions, continue when current safety rules allow, and otherwise mark Needs Review. External agents participate only when a user starts an interactive conversation in their harness.

## GUI harness launching

The GUI stores generic per-machine launch profiles with a name, executable, argv template, and default flag. `{prompt}` is substituted inside an existing argv element and passed directly to `QProcess`; no shell parses the prompt. If a profile has no placeholder, the GUI copies the context prompt to the clipboard before launching and tells the user.

Contextual Ask AI actions use stable project/release identifiers and brief focus information. The prompt directs the harness to inspect current state through MCP instead of duplicating large evidence blobs.
