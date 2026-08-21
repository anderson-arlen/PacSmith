# PacSmith Architecture

This document is the target architecture. Where it conflicts with the pre-refactor C++ implementation, this document wins.

PacSmith is a conversion workbench from vendor Linux artifacts into ordinary pacman packages. The long-term shape is a single library daemon with thin native clients, not an in-process C++ core shared by the GUI and CLI.

## Status

The client/server rewrite is in place for library ownership. `pacsmithd` stores projects, releases, artifacts, jobs, credentials, and PKI. The GUI and CLI speak HTTP to that daemon (Unix socket locally, HTTPS/mTLS remotely) and must not write `$XDG_DATA_HOME/pacsmith/projects`.

AI provider HTTP (package review, GitHub asset-rule generation, model catalog, ChatGPT token refresh) runs on `pacsmithd`. ChatGPT browser PKCE login stays on the client; the resulting session is stored on the daemon. GitHub REST asset listing for the import chooser stays on the client; generating a regex from those names is a daemon job.

Still on the C++ client, by design: libalpm installed-package queries and `pkexec pacman`. Still not daemon-owned: scheduled upstream update polling (CLI/GUI `check` still uses in-process APT/RPM/GitHub helpers). Physical testing remains for the GUI, polkit install, linger plus the systemd user unit, Secret Service, and mTLS fingerprint enrollment UX.

## Repository layout

```text
client/     C++/Qt GUI, CLI, and the transitional in-process core
server/     Go pacsmithd module (library owner)
docs/       architecture and implementation plan
packaging/  whole-product release packaging
```

The two trees are separate programs with separate toolchains. They share one HTTP API, not a source tree. `pacsmith_core` remains under `client/` until library-side work has been ported to `server/`; new library features go to `pacsmithd`, not back into that C++ core.

## Ownership boundary

`pacsmithd` owns the PacSmith library.

The GUI and CLI MUST NOT directly read or write library storage. Library operations always go through `pacsmithd`, including projects, releases, source and update configuration, upstream checks, dependency mappings, artifact ingestion, package inspection, preparation, builds, AI provider calls, editable build files, background polling, jobs, server-side credentials, client registration, and client authorization/revocation.

There is no in-process local shortcut that invokes a second implementation of library logic. Local and remote management are the same product: the same HTTP API, handlers, authorization/service layer, storage, and artifact-transfer path.

```text
                    ┌─────────────────────────────┐
                    │         pacsmithd           │
                    │  HTTP handlers / services   │
                    │  SQLite + artifact store    │
                    │  jobs + update polling      │
                    └─────────────┬───────────────┘
                                  │
              HTTP / Unix socket  │  HTTPS / mTLS
                                  │
                    ┌─────────────┴───────────────┐
                    │     shared C++ client       │
                    ├─────────────┬───────────────┤
                    │ pacsmith    │ pacsmith-gui  │
                    └─────────────┴───────────────┘
```

Host-local package-manager state stays on the client. See [Host-local operations](#host-local-operations).

## Languages

- **Clients:** C++/Qt. One CLI named `pacsmith`. No separate `pacsmith-server` CLI. GUI and CLI share one C++ client/protocol library.
- **Server:** Go (`pacsmithd`). The server owns the network security boundary, TLS/mTLS, Unix sockets, SQLite, sqlc, streaming I/O, background jobs, concurrency, HTTP, rate limiting, and long-running service behavior.
- Do not keep the server in C++ solely to reuse the old core. Do not use cgo or C++ FFI as the permanent architecture. SQLite access uses a pure-Go driver.

## Local management

Local clients speak HTTP over a Unix domain socket:

```text
$XDG_RUNTIME_DIR/pacsmith/pacsmith.sock
```

Client connection mode is stored on the client in `$XDG_CONFIG_HOME/pacsmith/client/connection.json`. Local is the default. Local and remote are mutually exclusive. `pacsmith connect local` or the GUI connection control (status bar, right side) uses the Unix socket and enables `pacsmithd.service`. `pacsmith connect remote <host>[:port]` enrolls over HTTPS, pins the Server CA, uses mTLS, and disables/stops the local user unit. A remote client does not run `pacsmithd`.

`pacsmithd` runs continuously as a systemd user service (`pacsmithd.service`) under a normal user account. There is no dedicated system user. It is responsible for scheduled upstream polling and other library work even when the GUI is closed. Socket activation is not the primary lifecycle. Restart-on-failure is appropriate.

On a machine that should keep the library up without a login (a headless server, or a workstation after logout), enable lingering for that account with `loginctl enable-linger`. Lingering starts the user systemd instance at boot, so `$XDG_RUNTIME_DIR` exists and the Unix socket path above still works. PKI administration is then SSH as that same user and talk to the local socket. Linger is an operator choice for that host; installers do not enable it.

Local OS access to the Unix socket is the administrative root of trust. PKI administration (approve/reject/revoke, server identity inspection) is local-Unix-only.

## Remote management

Remote clients speak HTTPS with mTLS directly to a remote `pacsmithd`. No local daemon is required or expected in that mode. Choosing remote management disables and stops `pacsmithd.service` on that client machine.

HTTPS/mTLS listening is **off by default**. It is a persisted library-host setting (`PATCH /api/v1/server`, local-Unix only), not a `pacsmithd` process flag. The administrator enables it from the local GUI Settings → Library tab or `pacsmith server listen on [--port N] [--interface ADDR]...`, choosing whether to listen, which interfaces, and which port. Default port is 8443. The daemon applies that setting on start and can hot-reload it without restart. Client registration approval UI and `pacsmith clients pending|approve` are available only while listening is enabled.

Local and remote management are mutually exclusive client backend modes. A client is configured for one library at a time.

PacSmith is intended for private networks (LAN, Tailscale, WireGuard, or similar). Exposing `pacsmithd` on the public Internet is the administrator’s perimeter responsibility. The server still applies built-in body limits, rate limits, and cheap unauthenticated registration handling so a public listener is not trivially DoSable. There is no Cloudflare-specific integration.

## One HTTP API

Both transports use `/api/v1/...` with the same endpoints, JSON, validation, handlers, authorization/service layer, artifact streaming semantics, and errors. Only connection establishment and authentication differ.

```text
Unix listener ─┐
               ├── HTTP router ──> services ──> SQLite + object store
TLS listener ──┘   (optional; off until enabled)
```

`GET /api/v1/version` reports `api_version`, `server_version`, and `capabilities` so incompatible clients fail clearly. `GET /api/v1/health` reports process and database liveness.

Do not invent a separate local RPC protocol.

## Artifact transfer

Artifacts are part of the API. Server filesystem paths are never given to clients. Uploads and downloads are streamed; large files are not loaded fully into RAM.

Upload: client file → HTTP body → temporary file (hashed while streaming) → validate → fsync → atomic move into the object store → SQLite association.

Download: object store → HTTP body → client.

Editable files (PKGBUILD, install scripts, patches, and similar) are also accessed through server APIs.

## Structured storage

SQLite is the authoritative source of truth for structured library/server state. Access is explicit SQL plus sqlc-generated typed queries. Normal migrations are permanent from this architecture onward.

SQLite is configured for a daemon: foreign keys, WAL, busy timeout, explicit transactions, uniqueness constraints, and indexes.

Do not store large artifacts as BLOBs. Do not mechanically translate `project.json` into tables. Host-local observations must not be persisted as shared library state.

Optimistic concurrency uses revision fields on shared mutable resources. An update that does not match the expected revision returns a conflict rather than silently overwriting.

Identifiers for projects, releases, jobs, registrations, clients, and artifacts are opaque UUIDs. Clients must not derive meaning or filesystem layout from them. Artifacts additionally have a SHA-256 content hash.

## Filesystem artifact store

```text
$XDG_DATA_HOME/pacsmith/server/
├── pacsmith.db
├── objects/
│   └── ab/
│       └── abcdef012345...
├── work/
└── tmp/
```

Object paths are derived from SHA-256 (`objects/<first two hex chars>/<full hex>`). SQLite stores artifact metadata (UUID, SHA-256, size, original filename, kind) and relationships. A database row must never point at a partially written object. An unreferenced object is safe orphan data and may be garbage-collected later.

Editable/transient build workspaces live under `work/`.

## XDG layout and legacy isolation

New architecture paths:

```text
$XDG_DATA_HOME/pacsmith/client/
$XDG_DATA_HOME/pacsmith/server/

$XDG_CONFIG_HOME/pacsmith/client/
$XDG_CONFIG_HOME/pacsmith/server/

$XDG_STATE_HOME/pacsmith/client/
$XDG_STATE_HOME/pacsmith/server/

$XDG_RUNTIME_DIR/pacsmith/pacsmith.sock
```

The pre-refactor library:

```text
$XDG_DATA_HOME/pacsmith/projects
```

normally `~/.local/share/pacsmith/projects`, is **legacy data**. New code must not modify, migrate, rename, delete, reinterpret, or write new-format data into it. The new and legacy versions must be able to coexist indefinitely. Tests must prove the legacy tree is never touched.

Pre-refactor client config (`$XDG_CONFIG_HOME/pacsmith/settings.json`, `secrets.age`) and state (`$XDG_STATE_HOME/pacsmith/update-state.json`) likewise remain outside the new ownership boundary. New client/server code uses the `client/` and `server/` subdirectories.

Library-wide settings live on `pacsmithd` (`GET`/`PATCH /api/v1/settings`): AI provider and model, update-check schedule, automatic preparation, and retention. This-machine GUI session options (tray, start at login, MIME onboarding) stay in the new client config tree. Client connection mode (local Unix vs remote host:port) stays in `$XDG_CONFIG_HOME/pacsmith/client/connection.json` and is edited from the GUI connection control on the status bar, not from Settings. HTTPS listen enable/interfaces/port live on `GET`/`PATCH /api/v1/server` (local-admin only). Client enrollment (list/approve/reject/revoke) is local-Unix PKI administration; the GUI Settings Library tab shows registration approval only while the host is listening, and hides that tab entirely when this GUI is itself a remote client.

## Jobs and background work

Long-running library work (update checks, downloads, inspection, preparation, builds, AI reviews) uses a modest server-side job model. A command may return `202 Accepted` with a `job_id`. Clients poll over HTTP. Incremental logs use an offset/cursor. Prefer polling over WebSockets/SSE in v1. Do not log full prompts or provider request bodies; package scripts in evidence are untrusted.

On daemon restart: queued work may resume where sensible; actively running jobs generally become interrupted/failed; update polling is rescheduled. Do not magically resume an interrupted `makepkg` unless an operation is explicitly designed for it. Simultaneous clients must not race duplicate update checks.

Update **configuration** (how to look for a successor) is distinct from update **check state** (last check time, result, discovered version, ETag, job/error state).

## PKI and remote enrollment

PacSmith manages its own PKI. There is no manual CA configuration in v1.

On first startup of a genuinely new server, create two separate CAs with separate private keys:

- **Server CA** — signs server TLS leaf certificates. This is the stable server identity. The human-comparable verification code is derived from `SHA-256(SubjectPublicKeyInfo)` of the Server CA, abbreviated to about 80 bits (for example `A72F 91C4 6B38 2D17 94EA`) with the full SHA-256 also available.
- **Client CA** — signs approved client CSRs. One Client CA for the server, not one CA per client.

On an already initialized server, missing CA material fails loudly. Never silently generate replacement CAs.

Server TLS leaf certificates are generated/cached automatically from TLS ClientHello SNI (and IP SANs for direct-IP access). Leaf certificates are not the stable identity. Generation is bounded so hostile SNI values cannot create unlimited persistent certificates.

Remote enrollment:

1. Client generates a key locally and never transmits it.
2. Client submits a CSR and friendly name to `POST /api/v1/registrations`.
3. Server returns `202` and a registration UUID after cheap CSR validation.
4. Client polls `GET /api/v1/registrations/<uuid>` (`pending`, `approved`, `rejected`, `expired`).
5. First connect is a bootstrap exception used only to observe the Server CA and display the verification fingerprint plus registration UUID. It is not a generic disable-TLS path.
6. The administrator compares that fingerprint with `pacsmith server info` / `pacsmith clients pending` on a local Unix-socket connection, then approves.
7. After approval the client pins that Server CA (not the OS trust store) and uses mTLS.

Registration remains available; an unapproved CSR grants no authority. The unauthenticated surface is abuse-resistant (small body limits, validate before persist, rate limits, pending caps, expiry, cheap polling). Signing happens only on explicit approval.

Revocation lives in SQLite and takes effect immediately even if the X.509 certificate is still cryptographically valid. v1 remote clients are fully trusted library clients; PKI administration stays local-only. No complex RBAC in v1.

## Secrets

Library credentials belong to the server because `pacsmithd` performs the work that needs them (AI keys, GitHub tokens, ChatGPT/session material, CA keys, and similar).

The daemon must restart and do background work without an encryption passphrase. The pre-refactor age-passphrase store is not the daemon secret backend.

`SecretStore` supports:

- **Secret Service** (preferred when a usable non-interactive freedesktop Secret Service is available)
- **Local protected file** (`0700` / `0600`) as an explicit Unix-account fallback
- **Environment** as a read-only source for injected deployment secrets

The selected backend is persisted. If Secret Service later becomes unavailable, secret operations fail clearly. There is no silent plaintext downgrade. Changing backend is an explicit administrative action.

Clients may set, replace, delete, and query **status** of named credentials (`configured`, `backend`). There is no API that returns stored secret values.

The remote client’s mTLS private key stays on the client under the new client data/config hierarchy with owner-only permissions.

Interactive ChatGPT OAuth (PKCE, loopback `localhost:1455`) remains a **client** UX step. The resulting token bundle is stored on the server through the credential API, not kept as GUI-owned authoritative secret state.

## Host-local operations

These remain C++ client responsibilities and must not move into `pacsmithd`:

- Query the local pacman/libalpm database
- Determine whether a package is installed locally and which version
- Reconcile server project/release identity with this machine
- Privileged local install/remove: `pkexec /usr/bin/pacman -U|--remove` with a fixed argv
- GUI/CLI presentation of this machine’s install state

`pacsmithd` must never run `pacman -U` on behalf of a client machine. Client-machine installed-package observations are not shared library state.

Generated packages still carry ALPM `xdata` (schema, project UUID, release UUID, acquisition identity, artifact type, source SHA-256) so a client can identify PacSmith-managed packages even if its library configuration changes.

## Privilege and process execution

```text
untrusted artifact bytes
       │  static format/libarchive parsing only
       ▼
reviewable project + PKGBUILD
       │  explicit user/server build, never root
       ▼
makepkg output
       │  client streams the package; explicit local install choice
       ▼
pkexec /usr/bin/pacman -U -- <absolute-package-path>
```

Invariant:

- Imported packages/artifacts are untrusted data.
- Do not execute imported vendor binaries to inspect them.
- Do not execute maintainer scripts during inspection.
- Reject dangerous archive paths, duplicate members, special files, and unsafe symlink/hardlink behavior.
- Avoid shell-string interpolation; use argument-vector execution.
- Builds remain unprivileged. Never run `makepkg` as root.
- Privilege elevation remains narrow and local to explicit package installation on the **client** machine.
- Preserve signature/trust verification (pinned OpenPGP keys for APT/RPM; GitHub digest when present).
- Apply strict request/file size limits.
- Do not allow path traversal from client- or upstream-supplied filenames.

Builds run on the library host (the machine running `pacsmithd`). In local mode that is the same computer as the client. In remote mode the library host builds; the client only downloads the artifact and, if the user chooses, installs it locally.

## Library domain

The current C++ model is the source of domain concepts. It is not the storage schema.

### Concepts to keep

- **Project** — one application the user maintains. Identity is an opaque UUID, not a filesystem folder name and not the Arch package name (the package name remains a field).
- **Release** — one vendor artifact plus its independently inspectable Arch recipe. Acquisition identity is immutable provenance for those bytes. Update configuration is how PacSmith looks for a successor; each successor stores its own snapshot.
- **Source type** vs **acquisition** — `SourcePackageType` (DEB, RPM, Arch package, archive, Type 2 AppImage, ELF) is independent of `AcquisitionKind` (local file, direct URL, APT, RPM repository, GitHub).
- **Install mapping** — `/opt` vs preserve-root, launchers, desktop entries, icon, AppRun overlay.
- **Dependency mappings**, maintainer-script evidence, payload review rules, lifecycle `.install` acknowledgment, field provenance / AI change records.
- **Release state** — discovered, preparing, needs review, ready, built. Transient preparation should not be durable in a way that crash-leaves a stuck Preparing row; interruption should remain retryable.
- **Retention** — older built packages/releases relative to the installed known release are a library policy, but “installed” is observed by each client.

### Concepts that must not remain shared library state

The pre-refactor `Project` persists `installedVersion`, `installedReleaseId`, and `externallyInstalled` in `project.json`. Those are observations of **this machine’s** pacman database. They must be computed by the client from libalpm plus server release metadata, never written as library rows.

`UpdateConfiguration` currently mixes durable source coordinates with last-check results (`lastChecked`, `detectedVersion`, ETags, and similar). Split those.

Pre-refactor IDs are meaningful: project id is a sanitized package name; release id is `version + sha256[:12]`. New IDs are opaque UUIDs. ALPM xdata must store the new UUIDs.

### Expected SQLite entities (normalized, not a JSON dump)

As the library port proceeds, tables should include approximately: `projects`, `releases`, `update_sources`, `update_check_state`, `dependency_mappings`, `artifacts`, `release_artifacts`, `builds`, `jobs`, `clients`, `client_certificates`, `registrations`, `trusted_keys`, `credential_metadata`, plus revision columns where concurrent edits are possible. Large payload manifests may be JSON columns or sidecar objects; they must not become unbounded SQLite BLOBs of vendor file bytes.

## Source analysis and trust

This behavior currently lives in C++ (`SourceAnalyzer`, `DebAnalyzer`, `RpmAnalyzer`, `PathSafety`, `PayloadInspector`, `LifecycleValidator`, repository trust helpers). It must be preserved or improved when ported to Go. Do not simplify away the checks.

`SourceAnalyzer` detects type from bytes. ELF magic selects the standalone executable analyzer; an `ar` archive is passed to strict DEB validation; RPM lead magic selects the bounded RPM header parser; other supported containers are opened through libarchive and classified as an Arch package only when a valid `.PKGINFO` is present. File extensions are hints, not the trust decision.

AppImage markers are checked before generic ELF classification. Type 1 images are rejected. Type 2 images are inspected by locating a bounded, aligned SquashFS superblock, validating it with `unsquashfs`, and decomposing it in a private temporary directory as the current user. PacSmith never invokes `AppRun`, the AppImage runtime, FUSE mounting, or embedded update code. Paths, links, entry count, expanded size, special files, desktop entries, icons, and `AppRun` are validated as ordinary untrusted payload data. Generated packages preserve the complete AppDir below `/opt`, strip setuid/setgid bits from regular files, and install one host command wrapper that sets `APPDIR`, `OWD`, and `ARGV0`, disables the embedded update contract, and executes the vendor’s `AppRun`. The original AppImage is only a makepkg source.

For Arch packages, `.PKGINFO` supplies metadata and the payload is inspected without extraction. `.INSTALL` is review-only evidence. Generated recipes unpack the payload while excluding package database metadata.

Generic archives reject unsafe paths, duplicate members, device/FIFO/socket entries, unsafe links, and unbounded metadata. A recognized Linux root can be preserved; otherwise the workbench exposes an `/opt/<name>` mapping. Standalone ELF files are identified only from their static header and are never invoked for identification.

### DEB-specific analysis

`DebAnalyzer` uses libarchive twice: outer `ar` (`debian-binary`, `control.tar.*`, `data.tar.*`), then inner compressed members. Control is parsed as Debian paragraphs. `preinst` / `postinst` / `prerm` / `postrm` / `config` are bounded text and are never executed. A literal-only evidence scanner recognizes repository stanzas, embedded keys, Arch-hook equivalents, AppArmor, and unresolved responsibilities. The data archive is walked without extracting it. Payload paths/links are validated. Desktop-entry `Icon=` references are correlated with bounded PNG/SVG/XPM candidates; only the selected icon is stored.

### RPM-specific analysis

`RpmAnalyzer` reads the lead, signature header, and main header with explicit entry-count and store-size limits. It does not invoke `rpm` or script interpreters. Libarchive’s RPM filter exposes the compressed cpio payload to the same path, symlink, special-file, preview, and review checks. RPM scripts remain reference-only.

### Path safety

Archive member paths are normalized. Traversal, absolute paths, NUL bytes, and Windows drive prefixes are rejected. Relative symlinks must not escape the archive. Package payloads may use absolute FHS links only under conventional install roots (`/opt`, `/usr`, `/bin`, `/sbin`, `/lib`, `/lib64`). AppImages additionally allow a narrow class of absolute compatibility links into standard executable/library trees; mutable locations such as `/tmp`, `/home`, and `/etc` remain forbidden. Permission cleanup processes regular files only so it cannot dereference those links.

### Recipe generation

Generated PKGBUILDs use a relative source link and verify SHA-256 through makepkg. Every recipe sets `options=('!strip' '!debug')`. DEB recipes find `data.tar` dynamically; RPM recipes extract the libarchive-exposed cpio payload; Arch packages are minimally repackaged without old database metadata; archives use the reviewed root or `/opt` mapping; Type 2 AppImages are decomposed with `unsquashfs`; ELF files use `install -Dm755`. Foreign lifecycle scripts are not involved. Detected `/etc/apt` content and repository keyring files receive explicit removal rules unless retained.

PKGBUILD validation is static and does not source the file. A generated Arch `.install` file is checked with `bash -n` (syntax only) and a restrictive policy, then requires exact-content acknowledgment because pacman will run its lifecycle functions as root.

`makepkg --force --nodeps` is intentional: PacSmith unpacks prebuilt vendor files, so `depends=` are runtime metadata for `pacman -U` rather than libraries needed to assemble the package. Source hashes and `package()` still run.

## Updates and acquisition

Persisted strategies: Manual, Direct URL, APT repository, RPM repository, GitHub Releases. Acquisition identity is immutable release provenance and is never repurposed as a future-download setting.

A signed APT or RPM repository may be the first acquisition source. Checks require a project-local public key and pinned signer fingerprint. APT verifies clear-signed `InRelease` or detached `Release.gpg` with `gpgv`, rejects a valid signature from any non-pinned key, selects Packages indexes only from the signed Release SHA-256 table, and caps response/decompression sizes. RPM checks verify `repodata/repomd.xml.asc`, the primary metadata checksum recorded in signed `repomd.xml`, and the package SHA-256 in primary metadata.

GitHub uses the public REST API with an optional PAT. Drafts are ignored; prereleases are opt-in except when the user imported a tagged prerelease URL. A user-visible regular expression must full-match exactly one asset. When GitHub exposes a `sha256:` digest it is verified; otherwise the release is explicitly unsigned and PacSmith records the locally computed hash.

In the GUI, discovery and preparation stay separate states. Automatic preparation is a library policy executed by `pacsmithd`, not a second client-side downloader.

## Optional AI resolution

Deterministic inspection always runs first. `pacsmithd` then makes the provider HTTP call. Clients enqueue a job (`POST /api/v1/releases/{id}/ai` or `POST /api/v1/ai/github-asset-rule`), poll it, and apply the result. They do not call OpenAI, xAI, or ChatGPT Responses themselves. Model listing is `GET /api/v1/ai/models` on the daemon, using the stored credential. Missing provider, credential, or model is a `400` before a job is queued.

The evidence bundle is size-bounded JSON. Package content is labeled as untrusted prompt data. Vendor binaries are not sent. Architecture in that bundle is the library host (`pacsmithd`), not the GUI machine. Providers: ChatGPT subscription-backed Responses, OpenAI Responses, xAI Responses. Requests have a fixed transport/output budget and an absolute deadline. Reviews are single-request; a non-empty `informationRequests` array is a failed job, not a local follow-up.

The response is constrained to typed field changes, finding dispositions, an empty information-request array, and an optional Arch lifecycle file. `AiResolutionApplier` stays on the client. It accepts only an explicit field allowlist, refuses signing-key hashes not already trusted from vendor evidence or a user import, preserves user-owned values unless separately approved, and validates package names and payload paths.

Vendor AppArmor profiles remain packaged even when AppArmor is not currently enabled on a particular client machine. PacSmith never asks the model whether AppArmor happens to be active during analysis. AppArmor policy in the evidence bundle is package policy, not host AppArmor state.

## Installation prefix and units

The default install is the Arch package from GitHub releases (`PREFIX=/usr`). Executables use `$PREFIX/bin`. Desktop integration uses `$PREFIX/share/applications`. The daemon unit is a systemd user unit at `$PREFIX/share/systemd/user` (`/usr/lib/systemd/user` in the package) and is managed with `systemctl --user`. It runs as whichever account enables it; there is no dedicated `pacsmith` system user.

`make install` is the development path: current-user scoped under `~/.local`, and it may enable `pacsmithd.service` for that user. It does not enable lingering.

A headless library host enables lingering for the library-owning account, then the user unit, then `pacsmith server listen on` (or the GUI Library tab) to opt into HTTPS/mTLS. Remote clients do not run a local daemon. Encrypted home directories that are unavailable at boot will also block a lingering user manager until that home is unlocked. Headless linger usually has no Secret Service; first init should pin the file backend.

Pacman remains the intentionally separate, explicitly elevated boundary for installing a generated Arch package on a client machine.

## Pre-refactor layout (legacy only)

The following describes the **legacy** on-disk library. New code must not produce it or consume it as a database:

```text
$XDG_DATA_HOME/pacsmith/projects/<id>/
├── project.json
└── releases/
    └── <vendor-version>-<source-hash>/
        ├── release.json
        ├── PKGBUILD
        ├── pacsmith.vars
        ├── <package-name>.install
        ├── sources/<artifact>
        ├── files/keys/
        ├── patches/
        ├── build/
        └── history/
```

Users may keep a pre-refactor PacSmith binary and recreate important package configurations in the new implementation by referring to these directories. That copy is manual. There is no automatic migration.

## Superseded current behavior

These current behaviors are real, and they are intentionally replaced:

| Current | Replacement |
| --- | --- |
| GUI/CLI call `ProjectStore` and analyzers in-process | All library work through `pacsmithd` HTTP |
| JSON files under `pacsmith/projects` | SQLite + CAS under `pacsmith/server` |
| Project/release IDs derived from names/hashes | Opaque UUIDs |
| `installedVersion` / `installedReleaseId` in `project.json` | Client-local reconciliation only |
| Update config mixed with last-check fields | Split configuration vs check state |
| GUI-owned libsecret/age credentials, age passphrase unlock | Server `SecretStore`, no passphrase for daemon start |
| `pacsmith check --all` / GUI timer as the poller | `pacsmithd` owns polling |
| `pacsmith-update.timer` (already disabled in current C++) | Persistent `pacsmithd.service` |
| Clients open PKGBUILD paths on disk | Editable-file APIs + streaming |
| `BuildService` as a client `QProcess` | Server job running `makepkg` unprivileged on the library host |
| GUI/CLI call OpenAI, xAI, or ChatGPT Responses in-process | `pacsmithd` jobs plus `SecretStore`; clients apply the result |

The current C++ `GuiInstanceServer` local socket (`pacsmith-gui-<uid>`) is a single-instance GUI helper, not a library protocol. It may remain a client-side concern.

Interactive CLI `pkexec pacman` and the internal `_install-session` transport remain client-side.

## Package-format evolution

RPM support remains concrete rather than a deep package-format inheritance tree. Future formats should follow the same rule: static bounded parsing first, normalization only where concepts actually match, and a source-specific readable PKGBUILD recipe. No format may broaden PacSmith’s privilege or execution boundary.
