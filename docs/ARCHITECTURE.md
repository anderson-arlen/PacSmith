# PacSmith Architecture

## Component boundaries

```text
                    pacsmith_core
                   /             \
       pacsmith (Qt Core)     pacsmith-gui (Qt Widgets)
```

`pacsmith_core` owns all domain types, artifact inspection, dependency parsing, JSON persistence, PKGBUILD generation, update-source configuration, and process services. The CLI and GUI call those classes directly. The GUI never shells out to the CLI for normal work, and the CLI has no Qt Widgets dependency.

Normal installation is current-user scoped under `~/.local`: executables use `~/.local/bin`, desktop integration uses `~/.local/share/applications`, and update units use the systemd user search path `~/.local/share/systemd/user`. The service is configured with the actual installation prefix and is managed only with `systemctl --user`. Pacman remains the intentionally separate, explicitly elevated boundary for installing a generated Arch package.

Core code is deliberately composed from small concrete services rather than a speculative package-format hierarchy. `SourcePackageType` currently distinguishes DEB, RPM, Arch package, archive, Type 2 AppImage, and ELF artifacts, while `AcquisitionKind` independently records local file, direct URL, APT, RPM-repository, or GitHub origin. DEB and RPM use concrete analyzers and normalize only the metadata, dependency, script, payload, and recipe concepts genuinely shared by the workbench.

## Project format

The project root follows the XDG base-directory specification:

```text
$XDG_DATA_HOME/pacsmith/projects/<id>/
├── project.json
└── releases/
    └── <vendor-version>-<source-hash>/
        ├── release.json
        ├── PKGBUILD
        ├── pacsmith.vars
        ├── <package-name>.install  # only when configured
        ├── <artifact> → sources/<artifact>
        ├── sources/<artifact>
        ├── files/keys/
        ├── patches/
        ├── build/
        └── history/
```

`project.json` is the application-level index: identity, release IDs, installed package reconciliation, and project history. Every `release.json` independently stores its immutable artifact and acquisition identity, source hash, parsed analysis, install mapping, content-specific review decisions, lifecycle policy, editable update evidence/configuration, generated-recipe baseline, build records, statically inspected artifact metadata, and history. The install mapping includes `/opt` root handling, any number of command launchers and desktop entries, and one selected application icon with content hash/provenance. Acquisition says where those exact bytes came from and is never repurposed as a future-download setting. Future retrieval is configured only through that release's update strategy. The active tracker is the installed known release, or the newest fully analyzed release while the project is not installed. A discovered or still-preparing release cannot take ownership merely by appearing. Checks pause only when no analyzed release exists or pacman's installed version cannot be matched safely. Full AI conversations and credentials are deliberately absent.

The PKGBUILD remains an ordinary text file. Its current hash is compared with the last generated hash when a project is loaded, so edits are recognized without making the JSON file authoritative over user content. PacSmith does not overwrite a manually edited recipe during project load. Per-release identity (`pkgver`, vendor filename, SHA256, and related values) is rewritten into `pacsmith.vars` beside the PKGBUILD; generated and Custom recipes source that file and use `_PACSMITH_*` variables so a copied Custom recipe does not keep stale artifact names.

## Source analysis

`SourceAnalyzer` detects artifact type from bytes. ELF magic selects the standalone executable analyzer; an `ar` archive is passed to strict DEB validation; RPM lead magic selects the bounded RPM header parser; other supported containers are opened through libarchive and classified as an Arch package only when a valid `.PKGINFO` is present. File extensions are hints for the UI, not the trust decision.

AppImage markers are checked before generic ELF classification. Type 1 images are rejected. Type 2 images are inspected by locating a bounded, aligned SquashFS superblock, validating it with `unsquashfs`, and decomposing it in a private temporary directory as the current user. PacSmith never invokes `AppRun`, the AppImage runtime, FUSE mounting, or embedded update code. Paths, links, entry count, expanded size, special files, desktop entries, icons, and `AppRun` are validated as ordinary untrusted payload data. The generated package preserves the complete AppDir below `/opt`, strips setuid/setgid bits from regular files, and installs one host command wrapper that sets `APPDIR`, `OWD`, and `ARGV0`, disables the embedded update contract, and executes the vendor's `AppRun`. The original AppImage is only a makepkg source and is not installed as the application executable.

For Arch packages, `.PKGINFO` supplies package/version/architecture/dependency metadata and the payload is inspected without extraction. `.INSTALL` is preserved as review-only script evidence and is never copied into the new package implicitly. Generated recipes unpack the payload while excluding package database metadata, then let makepkg produce a new package and PacSmith-specific xdata.

Generic archives reject unsafe paths, duplicate members, device/FIFO/socket entries, unsafe links, and unbounded metadata. A recognized Linux root (`usr`, `etc`, `opt`, and related top-level directories) can be preserved. Otherwise the GUI exposes an `/opt/<name>` bundle mapping, optional removal of one shared versioned top-level directory, and multiple `/usr/bin/<command>` links to exact selected executables. Standalone ELF files are identified only from their static header and installed to explicitly visible command destinations; they are never invoked for identification.

The workbench profile is derived from artifact type rather than pretending every source has Debian concepts. DEB/RPM releases show package metadata, dependencies, foreign scripts, payload, commands, desktop entries, icon, updates, PKGBUILD, and build. Generic archives show bundle layout, contents, commands, desktop entries, icon, updates, PKGBUILD, and build. AppImages instead show an explicit read-only installation plan and AppDir contents, plus only the host desktop-entry and icon customization that is meaningful for this format. Raw ELF imports show only their applicable package and integration steps. Desktop entries remain an ordered list with original and current fingerprints. On a successor release, untouched detected entries refresh from vendor bytes, while user-created or modified entries remain user-owned and become review items if referenced payload paths disappear. Command and icon mappings follow the same content/path-aware carry-forward rule.

### DEB-specific analysis

`DebAnalyzer` uses libarchive twice:

1. Read the outer `ar` archive and verify `debian-binary`, `control.tar.*`, and `data.tar.*`.
2. Copy the compressed inner members to private temporary files and let libarchive auto-detect their compression.
3. Parse `control` as Debian paragraphs, preserving unknown fields and multiline values.
4. Read `preinst`, `postinst`, `prerm`, `postrm`, and `config` as bounded text values. They are never executed. A literal-only evidence scanner recognizes repository stanzas, embedded Base64 public keys, Arch-hook equivalents, AppArmor, and unresolved responsibilities. It does not interpret or run shell.
5. Walk the data archive without extracting it. Persist the payload tree, validate every member path/link, read only bounded candidate APT text files, and flag `/etc`, systemd, APT, and keyring paths. Desktop-entry `Icon=` references are correlated with bounded PNG, SVG, and XPM candidates in standard application-icon locations; only the selected icon is cached under `files/`.
6. Parse `Pre-Depends` and `Depends` into comma-separated groups and pipe-separated alternatives with optional version constraints. Apply only the small reviewed mapping resource; keep everything else unresolved.

### RPM-specific analysis

`RpmAnalyzer` reads the lead, signature header, and main header with explicit entry-count and store-size limits. It imports name/version/release/epoch, architecture, vendor metadata, requirements, conflicts/provides, and RPM lifecycle/trigger scripts without invoking `rpm`, script interpreters, or package binaries. Libarchive's RPM filter exposes the compressed cpio payload to the same path, symlink, special-file, preview, and review checks used by other artifact sources. RPM scripts remain reference-only and enter the same content-bound responsibility workflow as DEB scripts.

The GUI runs project import on a dedicated worker thread. Core analysis reports stage transitions and periodically reports the number of payload entries processed; the GUI converts these events into an indeterminate progress dialog without touching widgets from the worker thread. Project creation remains synchronous in the CLI.

The analyzer currently keeps payload metadata in JSON. For extremely large packages, a future version may use a streamed sidecar index while retaining the readable project format.

Reviewable text files are stored with a bounded preview and a content SHA256. The GUI presents an explicit keep/exclude decision rather than a warning-only tree. A decision fingerprint covers the selected path (or a safety-default subtree such as `etc/apt`), so later content/path changes restore pending review. Exclusions regenerate ordinary `rm -rf -- "${pkgdir}/…"` lines in an unmodified generated PKGBUILD. Older projects load missing previews from their saved vendor DEB on a worker thread when the user selects a review item.

## Trust boundaries

```text
untrusted artifact bytes
       │  static format/libarchive parsing only
       ▼
reviewable project + PKGBUILD
       │  explicit user build, never root
       ▼
makepkg output
       │  explicit user install choice; one absolute package path
       ▼
pkexec /usr/bin/pacman -U
```

No package binary or foreign lifecycle script crosses the analysis boundary as executable code. Invalid absolute/traversing paths, duplicate members, unsafe special files, and escaping links abort import. Decomposed AppImages additionally recognize a narrow class of absolute compatibility links into standard system executable/library trees (for example `runtime/compat/usr/bin/env -> /usr/bin/env`); mutable locations such as `/tmp`, `/home`, and `/etc` remain forbidden, and permission cleanup processes regular files only so it cannot dereference those links. Shell interpolation is not used for process launch. PKGBUILD validation remains non-executing. A generated Arch `.install` file is separately checked with `bash -n` (syntax only) and a restrictive policy, then requires an exact-content acknowledgment because pacman will run its lifecycle functions as root.

## Optional AI resolution

Deterministic inspection always runs first. `AiAnalysisService` can then send a size-bounded JSON evidence bundle containing metadata, bounded script text, findings, capped payload manifests/previews, dependency state, integration candidates, the generated PKGBUILD, and trusted-key identifiers. Package content is labeled as untrusted prompt data. Providers are ChatGPT's subscription-backed Responses transport, OpenAI Responses, and xAI Responses. Requests have a fixed transport/output budget and an absolute deadline. Streaming is parsed incrementally so repeated SSE chunks cannot amplify an answer in memory. The GUI starts with a compact status and makes activity, exact redacted request JSON, and live response text available only behind **Show Details**, with copy controls.

`ChatGptLoginService` implements a native-app OAuth authorization-code flow with PKCE, a high-entropy state value, and a loopback-only callback on `localhost:1455`. The browser handles the user's OpenAI credentials; PacSmith receives only the authorization response, exchanges it over TLS, validates the account claim, and stores the access/refresh-token bundle under PacSmith's own credential key. Expired access tokens are refreshed before use and the rotated bundle is written back to the same PacSmith store.

`AiModelCatalogService` queries API providers at their `/v1/models` endpoints. For ChatGPT OAuth it instead queries the account-scoped `chatgpt.com/backend-api/codex/models` catalog with the access token and account ID, filters hidden/non-picker rows, and routes analysis to the matching subscription Responses endpoint. Recognized reasoning models can be configured with a supported `reasoning.effort`; fast execution sends the provider's `service_tier: "priority"` request option and the GUI warns that API providers may charge premium rates. PacSmith neither depends on an external AI client nor directly or indirectly reads another application's state directory. A legacy `codex` provider value in `settings.json` remains disabled rather than importing external state.

The response is constrained to typed field changes, finding dispositions, an empty information-request array, and an optional Arch lifecycle file. Each review makes exactly one provider request. The initial evidence includes the target architecture, bounded payload and script evidence, integration candidates, update trust state, and explicit packaging policy; requests for follow-up host facts are terminal contract violations rather than the start of another round. Required Arch dependency names are checked locally against configured pacman repositories after the response, and unavailable proposals are cleared to unresolved without sending another model request. `AiResolutionApplier` accepts only an explicit field allowlist. It refuses signing-key hashes not already trusted from vendor artifact evidence or a user import, preserves user-owned values unless separately approved, validates package names and payload paths, and records only applied changes plus provider/model/rationale provenance. AI-owned values are green in the GUI. A generated privileged lifecycle file adds a separate yellow acknowledgment lock.

The current machine's optional security configuration is not treated as package policy. In particular, vendor AppArmor profiles remain packaged even when AppArmor is not currently installed or enabled, because it may be enabled later. Any package-specific load/unload lifecycle proposed by AI must use conditional runtime checks; PacSmith never asks the model whether AppArmor happens to be active during analysis.

Provider settings live below `$XDG_CONFIG_HOME/pacsmith`, not inside projects. API credentials may be read from environment variables, stored with libsecret, or stored in an `age --passphrase` encrypted file. ChatGPT OAuth credentials are never accepted from an environment variable and are stored only in PacSmith's libsecret or age entry. PacSmith never puts the age password in process arguments, environment variables, or a plaintext temporary file.

Generated PKGBUILDs use the relative source link maintained beside the recipe and verify its SHA256 through makepkg. Every recipe sets `options=('!strip' '!debug')` so makepkg does not rewrite prebuilt vendor ELF files. DEB recipes find `data.tar` or `data.tar.*` dynamically; RPM recipes extract the libarchive-exposed cpio payload; Arch packages are minimally repackaged without their old database metadata; archives use the reviewed root or `/opt` mapping; Type 2 AppImages are decomposed with `unsquashfs`; ELF files use `install -Dm755`. Reviewed command launchers, desktop entries, and a content-addressed icon are emitted from the shared integration model. The authoritative vendor file stays under `sources/`, and the relative link keeps the project movable and usable even when its path contains spaces. Foreign lifecycle scripts are not involved. Detected `/etc/apt` content and repository keyring files receive explicit removal rules in generated DEB recipes.

Every generated package carries an ALPM `xdata` array with schema version, project/release IDs, acquisition identity, artifact type, and source SHA256. `ManagedPackageRegistry` reads these fields through libalpm rather than parsing command output. Exact xdata is preferred for installed-release reconciliation, and orphaned managed packages remain visible if their XDG project directories are lost.

## Build and installation

`BuildService` owns an asynchronous `QProcess`, sets the project as its working directory, invokes `makepkg --force --nodeps` with no shell, captures both output streams and timestamps, and scans for produced `.pkg.tar.*` files. `--nodeps` skips makepkg's host-package check: PacSmith only unpacks prebuilt vendor files, so `depends=` are runtime metadata for `pacman -U` rather than libraries needed to assemble the package. Source hashes and `package()` still run. The CLI drives it with a `QCoreApplication` event loop; the GUI consumes the same signals without blocking its event loop.

`InstallService` is separate so building never implies elevation. It accepts only an existing absolute `.pkg.tar.*` file and invokes `/usr/bin/pkexec` with a fixed pacman operation and that path. It cannot run an arbitrary privileged command. After an explicit GUI confirmation, GUI operations add pacman's `--noconfirm` option and capture output without opening a terminal; direct CLI installs remain interactive.

The legacy `TerminalInstallService` and authenticated local-session protocol remain available for the internal interactive CLI transport, but the GUI no longer depends on a terminal emulator. Package paths and names remain individual `QProcess` arguments and never pass through a shell.

## Updates and acquisition

The persisted strategies are Manual, Direct URL, APT repository, RPM repository, and GitHub Releases. Manual checks are no-ops and direct URLs remain configurable with version discovery pending. A signed APT or RPM repository may also be the first acquisition source: the user supplies repository coordinates and package/architecture, then selects a key from a vendor HTTPS URL, local file, pasted text, or an existing trusted PacSmith key. PacSmith shows the inspected fingerprint for explicit trust, copies reused keys into the new release so projects have no hidden file dependency, uses a temporary project-local keyring to verify repository metadata, downloads only the selected artifact with its signed-metadata SHA256, and carries the same configuration and key into the new release. There is intentionally no editable source configuration after creation: acquisition identity is immutable release provenance, while update configuration is the sole definition of how PacSmith looks for a successor. Each successor receives its own copied configuration snapshot, which can later be changed without rewriting the predecessor. A GitHub asset is re-analyzed on every release and may itself be a DEB, RPM, Arch package, archive, Type 2 AppImage, or ELF file. The analyzers parse APT definitions plus RPM/Yum bootstrap files and scripts—including bounded `/etc/cron.*` payload text—without executing them. RPM checks require a pinned OpenPGP key, verify `repodata/repomd.xml.asc`, validate the primary metadata checksum recorded in signed `repomd.xml`, and accept only a package with a SHA256 recorded in primary metadata. Older saved projects are migrated from their stored script and payload previews when loaded.

`AptUpdateService` performs network I/O through Qt Network without blocking the GUI. A check is blocked until the project has a vendor-embedded or user-imported public key and pinned fingerprint. It verifies clear-signed `InRelease` or detached `Release.gpg` using `gpgv`, rejects a valid signature from any non-pinned key, selects a component/architecture Packages index only from the signed Release SHA256 table, caps response/decompression sizes, verifies compressed size and SHA256, parses matching package stanzas, and compares versions using Debian ordering rules. The resulting upstream version, official filename, download URL, and package SHA256 are persisted.

`GitHubUpdateService` uses GitHub's public REST API with an optional PAT from `PACSMITH_GITHUB_TOKEN`, PacSmith's Secret Service entry, or PacSmith's age file. Drafts are ignored; prereleases are opt-in except when the user explicitly imports a tagged prerelease URL. A user-visible regular expression must full-match exactly one asset in a release; the GUI's initial suggestion keeps the platform/format portions literal while generalizing the version. Repository, tagged-release, and direct release-asset URLs are accepted. ETags and rate-limit diagnostics are persisted. When GitHub exposes an asset `sha256:` digest PacSmith verifies it during download; otherwise the UI explicitly identifies the source as unsigned and PacSmith records the locally computed hash. Discovered releases deliberately have an unknown artifact type until downloaded bytes pass content-based detection. Stable install destinations are carried across same-format releases, but versioned paths inside archives are re-detected.

The dashboard edits only the active release's update configuration. GitHub owner, repository, prerelease policy, and asset regular expression live there rather than in acquisition provenance or the package-building workbench. An optional AI assist lists the latest release's asset names, asks for an optional preferred artifact, and proposes a regular expression. PacSmith independently rejects invalid, ambiguous, unsupported, or preference-mismatching proposals and does not persist the result until the user saves it.

Background checks use the optional systemd user timer calling `pacsmith check --all`. Its persistent daily/weekly schedule is a user-owned drop-in with no randomized delay. Discoveries become release timeline entries with their trust evidence; optional preparation downloads to the user cache and imports through the same generic analyzer. Cleanup runs only after checks and counts older versions relative to the installed known release. A separate optional systemd user tray helper reads the bounded XDG-state summary, displays check activity, and badges available updates; it never performs privileged work.

In the GUI, discovery and preparation are separate states. A manual check persists the discovered release and prompts before downloading unless automatic preparation is enabled. Downloading and static inspection remain asynchronous; their progress dialogs may be hidden without canceling them, while transient spinner/progress state is also rendered in the package list and release timeline. Transient preparation is deliberately not written as durable `Preparing` state, so interruption leaves a retryable discovered release. Once inspection replaces the discovery entry with a complete release, the workbench opens at the first unresolved package mapping, dependency, script/lifecycle, payload, or PKGBUILD step, falling through to Build when no review remains.

On first launch PacSmith may set only the current user's DEB and RPM MIME defaults and offer its official `anderson-arlen/pacsmith` GitHub release as the first project. This does not synthesize an installed record. The tag workflow builds and tests in an Arch container, creates an x86_64 `.pkg.tar.zst`, and publishes it with `SHA256SUMS`; after a PacSmith-generated repack is installed, normal ALPM xdata reconciliation and installed-release update tracking apply.

## Package-format evolution

RPM support remains intentionally concrete rather than introducing a deep package-format inheritance tree. Future formats should follow the same rule: static bounded parsing first, normalization only where concepts actually match, and a source-specific readable PKGBUILD recipe. No format may broaden PacSmith's privilege or execution boundary.
