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

Payload drill-down remains a server-owned inspection operation. `get_payload` returns the immutable inventory, while `get_payload_file_inspection` asks `pacsmithd` to revisit one exact inventory path and return archive metadata, SHA-256, MIME/magic, bounded UTF-8 text, and static ELF identity, interpreter, dependency, path, build-ID, stripped/hardening, program-header, and section evidence. Local and remote agents use the same authenticated endpoint and never need to download or unpack the source artifact. `download_artifact` is reserved for explicit artifact export.

MCP project creation accepts the same remote sources as the GUI. GitHub repository, release, and exact asset URLs are submitted directly to `pacsmithd`; the daemon resolves releases with its server-owned credential, downloads into its artifact store, verifies any publisher digest, and imports the result without routing bytes or secrets through the client. Generic first-party HTTPS artifacts follow the same daemon job path through `import_direct_url`. After the direct response is valid and its first bytes arrive, the daemon persists a preparing project/release and the client returns its IDs with the job ID. Download and inspection continue asynchronously, with persisted 64-bit byte progress, cancellation, and read-inactivity detection instead of a total-transfer timeout. For a Manual release, `import_artifact` uploads a human-supplied local file while `import_direct_url` leaves the remote bytes entirely daemon-owned; both target an existing project and accept an explicit version override and optional publisher SHA-256 enforced by `pacsmithd`. Exact GitHub asset links become exact-match selectors automatically, and ambiguous links report candidates instead of encouraging an agent-side curl workflow.

Custom support-file operations are also exposed to people through `pacsmith custom-file`; they are not an agent-only capability.

The tool descriptions stand alone, while the Skill under `agent-plugin/skills/pacsmith/` supplies trust and packaging judgment. The source and installed package contain an Agent Plugins 1.0 bundle with a root `plugin.json` and an `mcp.json` stdio declaration for executable `pacsmith` with argument `mcp`; `pacsmith plugin path` prints that bundle directory. `pacsmith skill install` atomically installs or upgrades the standalone user copy at `~/.agents/skills/pacsmith`, and `pacsmith skill path` prints the active Skill directory. An unmanaged existing directory is preserved unless the user explicitly supplies `--force`.

`check_updates` runs the same deterministic update-check operation as the GUI/CLI for one project or all projects, records discovery evidence, and honors automatic-preparation settings without invoking a model. `import_repository_signing_key` is marked destructive for MCP host authorization; after authorization `pacsmithd` downloads and normalizes the first-party HTTPS OpenPGP key, while the client presents its inspected fingerprints and submits the user's pinning choice. Generic harness profiles have typed list/upsert/remove/default tools backed by `AppSettingsStore`, the same persistence used by the GUI. An executable and each argument remain separate strings; MCP never constructs a shell command.

If the Skill loads without MCP, it requires a consent question before installation and treats missing MCP as a hard stop. It prohibits CLI project commands, direct HTTP/socket/D-Bus access, daemon management, database access, and filesystem scraping as fallbacks. After consent, the harness maps the portable plugin or `pacsmith mcp` command into its own configuration and approval UX. PacSmith emits no harness-specific configuration and uses neither ACP nor an online harness registry.

## MCP permissions

Tool annotations include all four MCP behavior hints. The following operations carry `destructiveHint` so the MCP host can apply its permission policy:

- deleting a project;
- deleting a release;
- resetting and reanalyzing a release's maintained setup;
- enabling/disabling or renaming a project's published repository package;
- promoting a repository package to stable;
- changing global repository/listener/signing configuration or trust keys;
- changing automatic update/preparation/retention policy;
- changing desktop-session autostart;
- changing the remote management listener or client enrollment/trust;
- storing or deleting the server-side GitHub credential;
- trusting and pinning a vendor APT/RPM repository signing key.

Authorization belongs to the MCP host. PacSmith does not send a second form-elicitation request after the host permits the call, which allows the host's “always allow” choice to work as intended. Ordinary maintained-state edits are not marked destructive.

All mutating project/release calls require human-readable selectors from `list_projects`; opaque IDs are rejected for those tools. PacSmith resolves selectors internally so the host's permission prompt can identify the actual target.

Package installation and repository bootstrap/trust installation are not exposed through MCP. Bootstrap scripts can be read as inert text, but MCP never executes them. The narrow host-local exception is `pacsmith install <project>`: it resolves only a retained PacSmith build and defaults to interactive sudo in the current TTY. `pacsmith install --polkit <project>` explicitly selects graphical authentication and non-interactive pacman after the caller's confirmation. Agent harnesses always select `--polkit`; their pseudo-terminals do not guarantee that a person can enter credentials into the child process. The command accepts no external package path, allowing harnesses to remember the `pacsmith install` prefix without granting arbitrary sudo or pacman access.

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

Daemon-owned changes are announced through the authenticated `/api/v1/events` SSE stream. Events carry a process-local sequence, affected topics, and optional resource identifiers; they are invalidations rather than an audit log. Every connection begins with an `all` sync event, heartbeat comments keep intermediaries from idling the stream, and reconnect uses bounded exponential backoff. The same libcurl connection setup is used for the local Unix socket and remote HTTPS/mTLS modes.

The GUI coalesces event topics and reloads current state on a worker thread. Project selection, release selection, and the active workbench section are preserved. If the open project has an unsaved PKGBUILD, lifecycle, AppRun, desktop-entry, metadata, update, or repository draft, the GUI preserves it, shows an external-change banner, and rejects further project writes until the user explicitly reloads or leaves the remotely deleted project. Settings, credential, repository, listener, enrollment, and client-administration events replace the former one-second Settings polling loop.

Job events include their operation kind and human-readable project/package identity. GUI activity messages describe the work, such as `Building package Parsec…`; opaque job IDs remain correlation data and are never used as user-facing labels.

Daemon and remote-host latency must not block Qt's GUI thread. Package-list refresh, project/release deletion, connection probes, build polling/final reload, repository status and promotion, settings initialization, source-artifact caching for previews, icon-member reads, install artifact acquisition/lifecycle synchronization, and post-install or post-uninstall reconciliation/persistence run on background workers. Interactive operations expose an indeterminate progress bar and contextual status text while controls that would race the operation are disabled. Rendering build summaries only inspects the local artifact cache and never initiates a download. Automatic event refresh remains unobtrusive and preserves drafts.

Direct URL update checks run as daemon jobs and use an HTTP `HEAD` probe with identity encoding and safe redirects. ETag is preferred, with Last-Modified/Content-Length and whitelisted object-generation headers as fallbacks. Missing validators defer automatic full downloads according to the release-owned interval; manual checks override that interval. Downloaded bytes are capped, SHA-256 compared, retained in the daemon artifact store, and only then passed through normal static source inspection. Validator changes with identical bytes merely refresh the stored validator baseline.

Historical `ValueOrigin::Ai` and AI provenance records remain readable so old project documents can be decoded. The obsolete `previousManualPkgbuild` JSON field is safely ignored. New code does not create provider decisions or `previous-manual-PKGBUILD`; successor Custom releases use copy-forward instead.

## Deterministic updates

Update discovery, preparation, and review-free building remain deterministic daemon work. Each project chooses `never`, `review_free`, or `ai` automatic-build handling. Both build-capable policies require a previously successful, fully reviewed package configuration; unchanged vendor dependency declarations and lifecycle behavior; no current structured review issues; and repository publication enabled for the project. Custom PKGBUILDs cannot pass deterministic review because arbitrary recipe compatibility cannot be inferred. With `ai`, a deterministically clean successor still builds automatically; evidence requiring judgment remains prepared and awaiting an external harness. The daemon never launches a client-owned harness or gains that client's model credentials. While checks run, `pacsmithd` publishes the current project, phase, and batch position through the ordinary authenticated event stream; completion events name every failed check and every paused automatic build with its reason. Tray, settings, status-bar, package-list indicators, and the persisted release update status are projections of those events and outcomes. A review-free prepared release is marked `ready`; a paused automatic build remains visibly actionable instead of appearing ready with no explanation. A successful build is marked as automatic and published to unstable. PacSmith never installs an update automatically.

The repository-wide `stable_enabled` setting is the sole active channel-existence switch. The older `project_repo_policies.stable_enabled` column remains populated only to preserve compatibility with existing databases and its historical constraint; project APIs and clients cannot use it to add or remove a channel.

The Updates settings page also owns a single `retention_versions` policy. For each project, cleanup resolves every active channel entry back to its release and uses the oldest pointer as the boundary. A populated Stable pointer therefore protects Stable and every release through Unstable; without Stable, Unstable is the boundary. Cleanup walks completed releases behind that boundary from newest to oldest, keeps the configured number, deletes excess release records, and garbage-collects their source and built-package artifacts in the same pass. Disabling HTTP serving does not remove the internal Unstable pointer. Superseded retention columns remain in SQLite for compatibility but are not part of the current API or UI.

## GUI harness launching

The GUI stores generic per-machine launch profiles with a name, executable, argv template, and default flag. `{prompt}` is substituted inside an existing argv element and passed directly to `QProcess`; no shell parses the prompt. If a profile has no placeholder, the GUI copies the context prompt to the clipboard before launching and tells the user.

Contextual Ask AI actions use stable project/release identifiers and brief focus information. The prompt directs the harness to inspect current state through MCP instead of duplicating large evidence blobs.
