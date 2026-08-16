# PacSmith

PacSmith is a native Arch Linux workbench for maintaining local Arch packages from official vendor-provided binary artifacts. It imports Debian `.deb` files, RPM packages, existing Arch packages, tar/zip archives, Type 2 AppImages, and standalone ELF executables from local files, direct HTTPS URLs, signed APT/RPM repositories, or GitHub releases. It inspects them as data, generates an editable PKGBUILD, builds with `makepkg` as your normal user, and installs the result through a narrowly scoped `pkexec pacman -U` operation.

PacSmith is **not an AUR helper**. It does not use the AUR and does not download community PKGBUILDs. The intended trust chain is:

```text
official vendor → official binary artifact → persistent local project
                → editable PKGBUILD → unprivileged makepkg
                → explicit privileged pacman -U
```

## Current prototype

The first vertical slice is usable today:

- `pacsmith` and `pacsmith-gui` share one C++ core; the CLI does not initialize Qt Widgets.
- Artifact type is detected from content rather than the filename. DEBs, RPMs, Arch `.pkg.tar.*` files, libarchive-supported tar/zip archives, Type 2 SquashFS AppImages, and ELF executables are inspected without executing package content.
- Acquisition and artifact type are separate persisted concepts: one GitHub release may supply a DEB, RPM, Arch package, archive, or raw executable. Direct HTTPS imports are also supported and clearly marked when no publisher checksum exists.
- The project sidebar keeps its actions compact with **New ▾**, **Delete**, and **Settings**. New offers source-specific **GitHub Link…**, **Package File…**, **Direct Download URL…**, **APT Repository…**, and **RPM Repository…** flows; supported files and links can also be dropped onto the window.
- Debian control fields, multiline fields, dependency alternatives and constraints, maintainer scripts, payload paths, suspicious configuration, and update-source candidates are persisted in JSON.
- A small, reviewed dependency mapping resource is applied automatically. Unresolved mappings remain visible and user mappings are saved per project.
- The package workbench changes with the artifact type. DEB/RPM recipes expose package metadata, dependencies, foreign scripts, payload, commands, desktop entries, icon, updates, PKGBUILD, and build. Generic archives expose bundle layout and selectable commands. AppImages use a deliberately smaller workflow: a read-only installation plan and AppDir contents, editable application desktop entries and icon, updates, PKGBUILD, and build. Standalone ELF imports show only the applicable package and integration steps.
- Commands, desktop entries, and icons are first-class versioned recipe data for every format. Archives may strip one detected top-level directory before installing under `/opt`; users can expose multiple inspected executables. Multiple `.desktop` entries can be detected, created, duplicated, disabled, syntax-highlighted, and edited. Icons can come from inspected payload bytes, a local file, or a reviewed HTTPS download.
- Source-specific PKGBUILDs unpack DEBs and RPM cpio payloads through libarchive, minimally repackage existing Arch packages, preserve recognized archive roots or install application bundles under `/opt`, and install standalone ELF files under `/usr/bin`. Type 2 AppImages are statically decomposed with `unsquashfs` into an intact AppDir below `/opt`; one generated host wrapper recreates the required AppDir environment and invokes the vendor's `AppRun`, while the original AppImage runtime and updater are not installed.
- Same-format updates carry stable user choices such as an `/opt` directory or command destination forward, while archive-internal executable paths are re-detected when versioned directory names change. A GitHub release that changes artifact format is analyzed from scratch.
- Generated packages include pacman xdata linking the installed package to its PacSmith project, immutable release, acquisition identity, artifact type, and source SHA256. PacSmith can therefore identify managed packages whose local project files are missing.
- Selecting an application opens a project dashboard with separate **Project Info**, **Version History**, and **Update Configuration** pages. Every discovered/imported vendor version has its own immutable acquisition record, editable update configuration, review state, PKGBUILD, build records, and retained Arch artifacts. The installed known release owns the active update configuration; before installation, the newest fully analyzed release owns it. **Edit / Set Up Release** opens a separate full-width, numbered workbench that ends with PKGBUILD and Build; **Back to Project** returns to the dashboard.
- Each package-list row uses a large application icon and two text lines: application name plus a muted **Not installed**, green installed-version/current status, amber **Update available**, or animated preparation status. Update rows also receive a persistent amber background. Manual update checks offer **Download & Inspect** or **Later**; automatic preparation skips that prompt. Download/import progress can be hidden without canceling the operation, remains visible in Version History, and a completed update opens the first package-setup page that still needs attention.
- PacSmith reconciles the installed version against static `.PKGINFO` metadata from retained artifacts. It distinguishes uninstalled, known PacSmith releases, and externally installed versions, and supports GUI-confirmed install, rollback, and uninstall operations through pacman.
- GUI imports run on a worker thread with a responsive staged progress dialog and live payload-entry counts.
- Standard application icons referenced by imported desktop entries are safely selected from the DEB payload, cached as an inspectable project file, and shown in the project list and Overview page.
- Maintainer scripts are split into visible responsibilities. Deterministic rules identify APT setup and operations handled by Arch hooks; unresolved responsibilities may be acknowledged as reviewed or resolved through the optional AI packaging advisor. Original Debian scripts are never executed.
- Script and generated Arch lifecycle acknowledgments are bound to the exact content. Changed upstream content therefore restores review automatically.
- Highlighted payload files have an actionable review panel with the file contents, an Arch-specific explanation, and explicit **Keep and acknowledge** or **Exclude** decisions. Decisions are fingerprinted; changed payload content warns again and exclusions become visible PKGBUILD removal rules.
- PKGBUILDs are editable and are never silently regenerated over manual work.
- Builds are asynchronous in the GUI and synchronous/script-friendly in the CLI. Resulting `.pkg.tar.*` files are detected and recorded.
- Installation state is read from pacman. Uninstalled projects can be permanently deleted after confirmation; installed projects must first be uninstalled through pacman.
- APT `.list`/deb822 `.sources` definitions and RPM/Yum repository bootstrap evidence are detected in bounded payload files and package-script text. This includes repository setup deferred to files such as `/etc/cron.daily/*`; PacSmith inspects those files but never runs them. Embedded OpenPGP public keys are copied to project-local keyrings with their fingerprints. APT checks verify `InRelease` or `Release.gpg`, the selected Packages index, and the DEB SHA256. RPM checks verify the detached `repomd.xml.asc` signature, the primary-metadata checksum recorded in `repomd.xml`, and the selected RPM SHA256. Both require `gpgv`, a trusted project-local key, and a pinned signer fingerprint.
- Optional AI resolution supports a ChatGPT subscription through PacSmith-owned browser OAuth, or OpenAI/xAI through API credentials. It receives a bounded evidence bundle only after deterministic analysis, can request narrowly typed local facts with approval for every request, and records applied-field provenance instead of a transcript. The compact progress window keeps request/response details hidden by default; **Show Details** reveals activity, the exact redacted request, and the live streamed response, with copy controls and hard transport/output/deadline limits.
- AI settings expose verified reasoning-effort levels for recognized models and standard or fast priority execution. Fast API processing can carry provider premium pricing and is labeled accordingly in the GUI.
- ChatGPT sign-in uses PKCE and a loopback callback. PacSmith never receives the user's password, stores the resulting session only in PacSmith's selected keyring or age-encrypted credential store, refreshes it there, and queries the signed-in account's model catalog for the model dropdown. It does not require Codex or OpenClaw and never reads another application's data directory.
- AI-generated Arch `.install` lifecycle files are syntax/policy validated, visibly marked, and blocked from installation until the user acknowledges the exact privileged content. They can also be discarded.

Signed APT, signed RPM repository, and GitHub Releases update checking are implemented. APT and RPM repositories can also be the initial source: PacSmith asks for the repository coordinates, exact package name, architecture, and a signing key supplied by vendor HTTPS URL, local file, pasted public-key text, or an existing trusted PacSmith key. It displays the selected fingerprint for explicit approval, copies reused keys into the new release rather than linking projects, verifies signed repository metadata and the published artifact checksum, and only then downloads and imports the package. GitHub tracking ignores drafts, defaults to stable releases, optionally includes prereleases, and requires a user-visible regular expression that matches exactly one asset per release. The import dialog suggests an architecture-specific expression with the version generalized for future releases. PacSmith records a GitHub-provided `sha256:` digest when present and always computes the downloaded bytes' local SHA256; releases without publisher digests remain visibly unsigned. A verified or explicitly unsigned result creates a visible, not-yet-inspected release that is downloaded and re-inspected through the same generic importer. Direct-URL version discovery is not yet implemented.

After import, the original acquisition location is provenance rather than an editable source. Future retrieval is controlled exclusively by that release's update configuration, which can be switched among Manual, Direct URL, signed APT, signed RPM repository, and GitHub Releases. The GitHub asset rule therefore belongs to Update Configuration; its AI assist receives only repository identity and release asset names, proposes one exact-match rule, and leaves saving to the user.

First launch can set the current user's DEB and RPM MIME defaults and offer PacSmith's own official GitHub x86_64 package as the first tracked project. It never fabricates installation state: the project remains **Not installed** until its generated package is actually installed through pacman. A tag-driven GitHub Actions workflow builds, tests, packages, and publishes the official x86_64 `.pkg.tar.zst` asset consumed by that tracker.

Settings include a systemd user-timer schedule, optional automatic preparation, and cleanup retention for old artifacts and complete releases. Cleanup runs after checks and is anchored to the currently installed known release. The optional user tray helper shows check activity and badges available updates; it is enabled only with the user update service.

Type 2 AppImage import is implemented as static decomposition, not as an AppImage manager. Type 1 images are rejected. PacSmith does not run the AppImage, preserve its embedded updater, mount it with FUSE, or install the original executable as the application artifact. Prefer an official DEB, RPM, or Arch artifact when the vendor offers one because those formats generally carry more useful dependencies and integration metadata.

## Security model

Imported packages are untrusted data, even when obtained from a known vendor.

- PacSmith never executes an imported binary, shared object, or maintainer script during analysis.
- Original maintainer scripts remain untrusted evidence. Acknowledgment records only a content fingerprint, never permission to execute the Debian script.
- Archive member paths are normalized; traversal, absolute paths, duplicate members, special device entries, unsafe hard links, and escaping symlinks are rejected.
- External processes use `QProcess` with explicit argument lists—never `/bin/sh -c` with imported values.
- `makepkg` is run in the project directory as the current user. PacSmith refuses to start it as root.
- Only an explicit Install action runs `/usr/bin/pkexec /usr/bin/pacman --noconfirm -U -- <absolute-package-path>`. The GUI displays its own transaction confirmation first, polkit handles authorization, and PacSmith captures pacman's output directly. CLI operations remain interactive unless their caller explicitly chooses otherwise.
- APT source files and repository keyrings are flagged and excluded from generated packages unless explicitly retained. Repository checks do not trust transport alone: a project-local vendor-embedded or user-imported public key and pinned fingerprint are mandatory.
- AI cannot invent a signing key, elevate privileges, run a package manager, or silently overwrite a user-owned field. It may select only an evidenced trusted key. Every proposed replacement of a manually edited value requires confirmation.
- API keys come from `OPENAI_API_KEY`/`XAI_API_KEY`, a desktop Secret Service when available, or a password-encrypted `age` file. An optional GitHub PAT similarly comes from `PACSMITH_GITHUB_TOKEN`, PacSmith's keyring entry, or PacSmith's age file. ChatGPT OAuth access and refresh tokens are accepted only from PacSmith's own sign-in flow and stored in the keyring or age file. The age password is sent through a private PTY rather than command-line arguments or environment variables.
- PKGBUILD validation is intentionally static and does not source or execute the file.

## Build on Arch Linux

Install dependencies from the official repositories:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-svg libarchive squashfs-tools polkit gnupg age libsecret desktop-file-utils
```

The top-level Makefile can perform the complete Arch-only setup, build, test, and current-user installation. It checks `/etc/arch-release` and `/etc/os-release` plus the system `pacman` before changing anything. Run it as your normal user. Pacman elevation is used only to install official repository dependencies; PacSmith itself is installed without elevation:

```bash
make install
```

Individual stages are also available:

```bash
make deps
make build
make test
```

The default installation prefix is `~/.local`. Executables go to `~/.local/bin`, desktop integration to `~/.local/share/applications`, and the update timer/service to `~/.local/share/systemd/user`. The timer is not enabled automatically; opt in with:

```bash
systemctl --user enable --now pacsmith-update.timer
```

`SUDO=doas` may be supplied on systems using `doas` instead of `sudo`. `PREFIX=/another/user/writable/prefix` can override the installation location.
If `~/.local/bin` is not already on the shell's `PATH`, add it or launch `~/.local/bin/pacsmith-gui` directly.

Build and test:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

AddressSanitizer and UndefinedBehaviorSanitizer can be enabled together:

```bash
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DPACSMITH_SANITIZERS=ON
cmake --build build-asan
```

Run directly from the build tree:

```bash
./build/pacsmith add /path/to/vendor-package.deb
./build/pacsmith add /path/to/vendor-tool.tar.gz
./build/pacsmith add https://github.com/owner/project --asset-regex 'project-.*-linux-x86_64\.tar\.gz'
./build/pacsmith add apt https://vendor.example/debian stable main amd64 vendor-package https://vendor.example/signing-key.gpg
./build/pacsmith add rpm https://vendor.example/rpm/x86_64 x86_64 vendor-package https://vendor.example/signing-key.asc
./build/pacsmith list
./build/pacsmith versions <project-id>
./build/pacsmith info <project-id>
./build/pacsmith scripts <project-id> --acknowledge postinst
./build/pacsmith lifecycle <project-id>
./build/pacsmith check <project-id>
./build/pacsmith ai status
./build/pacsmith ai resolve <project-id>
./build/pacsmith build <project-id>
./build/pacsmith rollback <project-id> <release-id-or-version>
./build/pacsmith uninstall <project-id>
./build/pacsmith-gui --import /path/to/vendor-package.deb
./build/pacsmith-gui --import https://github.com/owner/project/releases/latest
```

Direct CMake builds also default to the current user's `~/.local` prefix. An explicit `-DCMAKE_INSTALL_PREFIX=...` remains available for packagers and staging builds. No license has been added because the repository did not specify one.

## Persistent projects

Projects use `$XDG_DATA_HOME/pacsmith/projects`, falling back to `~/.local/share/pacsmith/projects`. Each application has `project.json` plus `releases/<version-hash>/`. A release directory contains `release.json`, `PKGBUILD`, `sources/`, `files/`, `patches/`, `build/`, and `history/`. Acquisition identity, source kind, trusted keys, install mapping, and lifecycle files are release-specific. A relative source link keeps normal `makepkg` use working from the release directory. Older single-release projects migrate into the current format with a JSON backup retained.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for component boundaries, the project format, artifact analysis, and update trust flows.
