# Client/server implementation plan

This is the concrete phased plan for the architecture in [ARCHITECTURE.md](ARCHITECTURE.md). It is based on the current C++ tree, not a greenfield wishlist.

## Baseline (2026-08-20)

Current layout:

- `client/` — `pacsmith` / `pacsmith-gui` and the transitional C++ `pacsmith_core`.
- `server/` — Go module `github.com/anderson-arlen/pacsmith/server`, binary `pacsmithd`.
- Root CMake/Make still configure, test, and install both.
- Library path: `$XDG_DATA_HOME/pacsmith/projects`.
- Tests: `pacsmith-core-tests`, `pacsmith-import-test`. Recorded passing: **2/2**, ~5.6s, existing `build/` tree.

Security-sensitive C++ that must survive the Go port:

| Area | Where it lives now |
| --- | --- |
| Archive path/symlink policy | `client/src/core/path_safety.cpp` |
| Type detection / AppImage / ELF | `client/src/core/source_analyzer.cpp` |
| DEB parse, no script exec | `client/src/core/deb_analyzer.cpp` |
| RPM header/payload, no `rpm` exec | `client/src/core/rpm_analyzer.cpp` |
| Payload walk / bounded previews | `client/src/core/payload_inspector.cpp`, `payload_review.cpp` |
| APT/RPM signature pinning | `client/src/core/repository_trust.cpp`, `apt_update_service.cpp`, `rpm_update_service.cpp` |
| Lifecycle `.install` policy | `client/src/core/lifecycle_validator.cpp` |
| `makepkg` not as root; fixed argv | `client/src/core/process_services.cpp` |
| `pkexec pacman` only, client-side | `client/src/core/process_services.cpp` (`InstallService`) |
| libalpm xdata / installed query | `client/src/core/managed_package.cpp`, `ProjectStore::queryInstalledVersion` |
| Age/libsecret credentials | `client/src/core/credential_store.cpp` (superseded as daemon backend) |

GUI/CLI coupling to replace in Phase 2+:

- CLI: `client/src/cli/main.cpp` constructs `ProjectStore`, update services, `BuildService`, `InstallService`.
- GUI: `client/src/gui/main_window/*`, `import_worker.cpp`, `application_session.cpp` load/save projects, run analyzers, spawn `pacsmith check --all`.

## Phase 1 — Server foundation (current)

Deliver `pacsmithd` as a real process with shared API shape, without porting analyzers yet.

- Go module `github.com/anderson-arlen/pacsmith/server` in `server/`, `cmd/pacsmithd`.
- XDG server paths; refuse any path under `pacsmith/projects`.
- SQLite + migrations + sqlc; `server_state` initialization marker; `artifacts` metadata.
- Content-addressed object store with stream ingest.
- `net/http` router on a Unix socket.
- `GET /api/v1/version`, `GET /api/v1/health`, artifact upload/download (proves streaming on the real API).
- systemd user unit `pacsmithd.service` (simple, restart-on-failure, not socket-activated; linger for headless).
- CMake/Make/CI build `pacsmithd` and run `go test`.
- Tests: migrations, artifact CAS, crash/orphan, HTTP, Unix-socket contract, legacy tree fingerprint unchanged.

Out of scope here: C++ client cutover, TLS, PKI, full project schema, secrets, analyzers.

## Phase 2 — C++ PacSmith client layer

- Shared C++ HTTP client (libcurl: Unix sockets now; mTLS in Phase 3).
- Client XDG paths under `pacsmith/client/`.
- Connection config: local socket vs remote URL (mutually exclusive).
- GUI/CLI call logical operations, not `ProjectStore` for library I/O.
- Local management requires `pacsmithd`; do not keep a second core implementation.

Until enough library APIs exist, Phase 2 may land the client library and a subset of CLI (`pacsmith server info` once Phase 3 exists; health/version immediately). The hard rule is: new library features go to the daemon, not back into `ProjectStore`.

## Phase 3 — PKI and remote transport

- Server CA + Client CA on first genuine init; fail if missing on an initialized server.
- Verification fingerprint from Server CA SPKI SHA-256.
- SNI-bounded leaf certificates; IP-SAN for direct IP.
- TLS listener, off by default; same HTTP router. Enable/interfaces/port persist on the daemon and are applied without `--listen`.
- Registration CSR API, polling, local-only approve/revoke. Approval CLI/GUI only while listening.
- C++ mTLS + pin Server CA; bootstrap exception only for enrollment.
- Client connection mode: status-bar connection control, `pacsmith connect status|local|remote <host>[:port]`. Local enables `pacsmithd.service`; remote disables and stops it.
- Rate limits and small-body limits on unauthenticated routes.
- Same API contract tests on Unix and HTTPS/mTLS.

CLI (same binary): `pacsmith connect …`, `pacsmith server info`, `pacsmith server listen [on|off]`, `pacsmith clients list|pending|approve|reject|revoke`. PKI/listen administration is local-Unix only.

## Phase 4 — New library model

Design schema from the domain section in ARCHITECTURE.md. Particular mappings from current types:

| Current | New |
| --- | --- |
| `Project` JSON | `projects` (no installed\* columns) |
| `PackageRelease` JSON | `releases` + child tables / JSON for bounded nested docs |
| `UpdateConfiguration` | `update_sources` + `update_check_state` |
| `SourceAcquisition` | immutable columns on `releases` |
| `DependencyMapping` | `dependency_mappings` |
| `BuildRecord` / artifacts | `builds`, `artifacts`, `release_artifacts` |
| `RepositorySigningKey` files | `trusted_keys` + artifact rows |
| PKGBUILD / `.install` / patches | artifacts or `work/` plus metadata, never client paths |
| `Project.installed*` | omitted; client libalpm only |

Carry-forward of install mappings across releases remains a server-side operation during import/prepare.

## Phase 5 — Secret storage

- `SecretStore` interface; Secret Service; `0700`/`0600` file fallback; env read-only.
- Persist backend choice; no silent downgrade.
- API: set/replace/delete/status; no get.
- Store CA keys here.
- Tests: backend persistence, failure when Secret Service disappears, no secret readback.

## Phase 6 — Core feature port

Move library-side C++ into Go incrementally, with tests for the security properties above:

1. Artifact ingest + type detection + path safety.
2. DEB/RPM/archive/AppImage/ELF inspection.
3. Update providers + daemon scheduler (replace GUI/`check --all` poller).
4. Recipe generation, identity vars, lifecycle validation.
5. Build jobs (`makepkg` unprivileged in `work/`).
6. AI evidence bundle, provider HTTP, and catalog on `pacsmithd` (credentials from SecretStore). Clients enqueue jobs and keep `AiResolutionApplier`.

Do not drop checks to make the port smaller.

## Phase 7 — Client host integration

Reconnect server release/package metadata with:

- libalpm installed version / xdata (`ManagedPackageRegistry` stays C++).
- Streamed package download to a client temp path.
- Existing `InstallService` argv (`pkexec pacman -U/--remove`).
- Dashboard install-state display that is computed locally and not saved to the server.

## Suggested first API surface beyond Phase 1

Keep one contract suite reused on Unix and later TLS:

- `/api/v1/version`, `/api/v1/health`
- `/api/v1/artifacts` (already in Phase 1)
- `/api/v1/projects`, `/api/v1/projects/{id}` with `revision`
- `/api/v1/projects/{id}/releases`, editable-file get/put
- `/api/v1/jobs/{id}`, `/api/v1/jobs/{id}/log?after=`
- `/api/v1/releases/{id}/ai`, `/api/v1/ai/github-asset-rule`, `/api/v1/ai/models`
- `/api/v1/credentials/{name}` status/set/delete
- `/api/v1/registrations` (Phase 3)
- `/api/v1/clients` (Phase 3, local-admin only)

## Non-goals (do not do)

Automatic legacy migration, Syncthing/S3/cloud sync, PostgreSQL, Kubernetes, OAuth/OIDC login accounts, browser UI, Cloudflare integration, a `pacsmith-server` binary, manual/enterprise CA, remote PKI admin, v1 RBAC, SQLite BLOBs for vendor files, cgo as the architecture, split local/remote implementations.
