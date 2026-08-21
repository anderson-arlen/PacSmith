<p align="center">
  <img src="client/resources/icons/pacsmith-hero.png" alt="PacSmith" width="192">
</p>

# PacSmith

> [!WARNING]
> PacSmith is still experimental. Expect rough edges until the first release.

PacSmith lets you install software on Arch Linux directly from the developer.

It takes the packages the developer actually publishes — Debian `.deb` files, RPMs, AppImages, archives, and related artifacts — and converts them into ordinary pacman packages that you can install, update, and remove like anything else on the system.

It is not an AUR helper. It does not download community PKGBUILDs. You import the developer's own packages, review the generated recipe, and become the package maintainer. The point is a shorter trust chain: developer → you, with no extra packager in the middle.

## Why PacSmith

Arch Linux is excellent at what it ships in the official repositories. Third-party application support outside those repos is another story. The Arch User Repository exists to fill that gap, and for years it has been the default answer to "how do I install this on Arch?" It has real problems.

### You have to trust a stranger

Installing software always requires some trust in the people who wrote it. The AUR adds a second, often invisible, party: the package maintainer.

A typical AUR package is not the developer's release. It is a community PKGBUILD that downloads, unpacks, and installs that release. The person who wrote that recipe is usually not the software author. You do not know them. If they later walk away, someone else can adopt the package and inherit the name, the history, and the trust that accumulated around it.

PacSmith removes that extra person from the chain. You import the developer's package yourself, inspect it, generate the PKGBUILD, and keep the project. The only maintainer you have to trust is you. The optional [AI helper](#the-ai-helper-is-what-makes-this-practical) can take the painful parts of that conversion — especially Debian and RPM install scripts — so you are not doing a packager's job by hand.

### The AUR has become a malware target

That extra-maintainer model is not a theoretical weakness. It is how recent AUR attacks actually worked.

In June 2026, a campaign later called **Atomic Arch** showed how cheap it is to buy existing trust. Attackers did not need to break into Arch's official repositories. They adopted abandoned AUR packages — a normal, documented process — and rewrote the PKGBUILDs. The first wave injected a malicious npm package during install. When that was noticed, they switched delivery methods within a day. Community tracking put the total above 1,500 packages. The payload was a credential stealer aimed at SSH keys, browser data, cloud tokens, password managers, and crypto wallets, with an eBPF rootkit available to hide on compromised machines.

That was not the last wave. In late July and August 2026 another campaign hit, again through compromised maintainers and orphaned-package adoption. A loader installed a Tor client, fetched a second-stage payload from an onion service, then delivered a Rust infostealer with remote-access and SSH-worm behavior. Fairly popular packages were among those reported. Arch's response escalated from disabling new AUR account registrations, to freezing package adoption, to blocking AUR pushes entirely while the mess was cleaned up.

None of this required a novel exploit in pacman. The AUR's trust model *is* the attack surface: recipes from people you do not know, attached to package names users already recognize. Similar orphaned-package takeovers go back years. 2026 simply made the scale impossible to ignore.

If you install from the AUR, you are not only trusting the application developer. You are trusting whoever currently controls that PKGBUILD.

### Updates depend on that same extra party

Trust is only half of the problem. The other half is time.

Once you rely on an AUR package, you also rely on that maintainer to notice upstream releases, update the recipe, and push. Developers ship; the AUR lags. Packages go stale. Maintainers disappear. Orphans sit with a trusted name until someone — possibly not a volunteer you would choose — picks them up.

### The AUR also does not have everything

Even a healthy AUR is incomplete. New applications, niche tools, and Linux builds that only the developer publishes often have no AUR package yet. Waiting for a volunteer to appear puts you back in the same trust-and-latency situation. PacSmith is for those cases too: if the developer publishes a Debian package, RPM, AppImage, GitHub release, or similar artifact, you can turn it into a local Arch package without waiting for the AUR to catch up.

### Flathub is decent. It is still someone else's repo

Flatpak, and Flathub in particular, is a reasonable answer for a lot of desktop software. The distribution model is cleaner than the AUR, updates come from a real repository, and the sandbox is a genuine security feature when it is actually used.

You are still trusting a third party. The application on Flathub is packaged, reviewed, and shipped by people who are usually not the developer. That is a better-run extra party than a random AUR maintainer, but it is the same kind of extra party: another org in the trust chain, another place a recipe or manifest can drift from what the developer published.

The sandbox also helps less often than the marketing implies. Flatpak permissions are powerful when they are tight. In practice, a large share of popular apps request far more than they need: full home-directory access, host filesystem access, unfiltered network, device nodes, and so on. Once an app can read `$HOME` and talk to the network, a lot of the isolation is already gone. You still get some benefits — a separate runtime, cleaner uninstall — but you should not treat "it is a Flatpak" as equivalent to "it is confined."

PacSmith's bet is different. Take the developer's own package, make it a pacman package you control, and keep the update channel pointed at the developer. No Flathub maintainer, no AUR PKGBUILD, and no sandbox you have to pretend is tighter than the permissions on the app.

## What PacSmith is

PacSmith is a conversion wizard from foreign Linux artifacts into pacman-managed installs.

You point it at a `.deb`, RPM, AppImage, Arch package, tar/zip archive, or standalone executable — from a local file, a direct HTTPS URL, a signed APT or RPM repository, or a GitHub release. PacSmith inspects the artifact as data, without executing it. It extracts metadata, dependencies, desktop entries, icons, payload layout, and (for DEB/RPM) maintainer scripts as review evidence. It then generates an editable PKGBUILD. Foreign install scripts and dependency mapping are the slow part of that review; the [AI helper](#the-ai-helper-is-what-makes-this-practical) can resolve them automatically if you want.

From there the workbench walks you through ordinary Arch packaging:

1. Review what the developer shipped.
2. Map dependencies, commands, desktop files, and anything suspicious — or let the [AI helper](#the-ai-helper-is-what-makes-this-practical) propose those mappings.
3. Build with `makepkg` as your normal user.
4. Install with a narrowly scoped `pkexec pacman -U`.
5. Uninstall or roll back later through pacman, because the result is a real package, not a pile of files in `/opt` that you will forget about.

The last step is the point of using pacman at all. Third-party Linux software is often distributed as an AppImage, a tarball, or a Debian package "you can just extract." Those leave you with ad-hoc files, leftover systemd units, and no clean inverse operation. PacSmith's output is a pacman package, so the application shows up in the package database and comes off the system the same way official packages do.

### Updates are the feature that actually matters

Creating a one-off PKGBUILD from a developer's `.deb` or RPM is not why this app exists. Nobody would go to that trouble if the next release meant doing it again by hand. Keeping the package current, without handing the job back to an AUR maintainer, is the part most Arch users do not have a good answer for.

PacSmith is the update authority for the packages it creates. After import, each project has an update configuration. You can watch:

- **Signed Debian/Ubuntu APT repositories** — the same developer channel the `.deb` originally came from
- **Signed RPM / Red Hat-style repositories** — Fedora, RHEL, OpenSUSE, and the developer's own Yum/DNF repos
- **GitHub Releases** — tagged assets from the developer's repository, including cases where there is no Linux package repo at all.

When a newer upstream version appears, PacSmith can notify you, optionally download and inspect it, and walk you through a new release of *your* package. Your reviewed choices — command names, `/opt` layout, desktop entries, dependency mappings — carry forward across same-format updates. You still review the new artifact, and the [AI helper](#the-ai-helper-is-what-makes-this-practical) can convert any new lifecycle scripts the same way it did on the first import. You do not start from a blank PKGBUILD, and you do not wait for a stranger to package the bump.

That is the difference between "I converted a `.deb` once" and "I can actually live without the AUR." The first is a weekend script. The second is why PacSmith exists.

Update checks can run on demand or on a systemd user timer. An optional tray helper badges available updates. Cleanup can retain older built artifacts so rollback stays possible.

### The AI helper is what makes this practical

Debian and RPM packages do not just drop files. They ship `preinst`, `postinst`, `prerm`, and `postrm` scripts that enable services, write configuration, set up repositories, install alternatives, and otherwise finish the job. Those scripts are written for apt or rpm. They do not belong on Arch, and converting them by hand is miserable: read a shell script you did not write, decide which parts are Debian-specific, which parts Arch already handles with hooks, and which parts still need an `.install` file.

PacSmith already does the mechanical conversion: metadata, payload layout, desktop entries, icons, a generated PKGBUILD. The remaining pain is those lifecycle scripts. If every one had to be rewritten by hand, PacSmith would not be worth building. The AI helper exists so you do not have to.

Point it at a `.deb` or RPM and it can:

- Translate foreign install and remove scripts into Arch-appropriate lifecycle handling
- Map Debian/RPM dependencies onto pacman packages
- Propose GitHub release asset-matching rules
- Flag leftover work that still needs a human look

For most packages, that is enough to go from import to a buildable recipe with little more than review and confirm.

The AI is a helper, not a requirement. Every step still works fully manually. Deterministic inspection always runs first, with no model involved. If you turn the helper on, it proposes; you accept, edit, or ignore. It cannot invent signing keys, elevate privileges, run a package manager, or silently overwrite your edits. You can use a ChatGPT subscription through PacSmith's own sign-in, or OpenAI / xAI API keys.

## How the trust chain works

```text
developer → developer's own package → persistent local project
                → editable PKGBUILD → unprivileged makepkg
                → explicit privileged pacman -U
```

Imported packages are untrusted data, even when they come from a known developer. PacSmith never executes an imported binary, shared object, or Debian/RPM maintainer script during analysis. `makepkg` runs as your user. Only an explicit Install action elevates, and it runs one command: `/usr/bin/pkexec /usr/bin/pacman --noconfirm -U -- <absolute-package-path>`.

Signed APT and RPM checks do not trust HTTPS alone. A project-local public key and a pinned signer fingerprint are required. GitHub tracking records a publisher `sha256:` digest when GitHub provides one, always hashes the downloaded bytes, and marks releases without a publisher digest as unsigned.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the client/server architecture, artifact analysis, and update trust flows. The repository keeps those programs in separate trees: `client/` is the C++/Qt GUI and CLI, `server/` is `pacsmithd`.

## Using PacSmith

The GUI is the main workbench. New projects can start from **GitHub Link…**, **Package File…**, **Direct Download URL…**, **APT Repository…**, or **RPM Repository…**. Supported files and links can also be dropped onto the window.

Each application becomes a local project with a dashboard (project info, version history, update configuration) and a numbered setup workbench that ends at PKGBUILD and Build. The package list shows install state: not installed, current, or update available. Use the [AI helper](#the-ai-helper-is-what-makes-this-practical) on any workbench step that still needs script or dependency conversion.

The library daemon (`pacsmithd`) does not accept remote HTTPS clients until you turn that on. From the library host, use Settings → Library or `pacsmith server listen on`. Another computer can then manage that library from the connection control on the status bar or `pacsmith connect remote <host>[:port]`. Local management is the default and starts `pacsmithd.service`; remote management stops it.

## Build on Arch Linux

Install dependencies from the official repositories:

```bash
sudo pacman -S --needed base-devel cmake ninja go qt6-base qt6-svg libarchive squashfs-tools polkit gnupg age libsecret desktop-file-utils openssl
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
make uninstall
```

The development prefix is `~/.local`. Executables go to `~/.local/bin`, desktop integration to `~/.local/share/applications`, and the library daemon unit to `~/.local/share/systemd/user`. `make install` enables `pacsmithd.service` for the current user when systemd is available. The packaged install path is the Arch package from GitHub releases, which ships the same user unit under `/usr/lib/systemd/user`.

`make uninstall` removes the files recorded by the last install. It does not delete the legacy library under `~/.local/share/pacsmith/projects` or new server data under `~/.local/share/pacsmith/server`.

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
./build/pacsmithd --version
./build/pacsmith add /path/to/vendor-package.deb
./build/pacsmith add /path/to/vendor-tool.tar.gz
./build/pacsmith add https://github.com/owner/project --asset-regex 'project-.*-linux-x86_64\.tar\.gz'
./build/pacsmith add apt https://vendor.example/debian stable main amd64 vendor-package https://vendor.example/signing-key.gpg
./build/pacsmith add rpm https://vendor.example/rpm/x86_64 x86_64 vendor-package https://vendor.example/signing-key.asc
./build/pacsmith list
./build/pacsmith versions <project-id>
./build/pacsmith info <project-id>
./build/pacsmith check <project-id>
./build/pacsmith build <project-id>
./build/pacsmith rollback <project-id> <release-id-or-version>
./build/pacsmith uninstall <project-id>
./build/pacsmith-gui --import /path/to/vendor-package.deb
./build/pacsmith-gui --import https://github.com/owner/project/releases/latest
```

Direct CMake builds also default to the current user's `~/.local` prefix. An explicit `-DCMAKE_INSTALL_PREFIX=...` remains available for packagers and staging builds.

## Security model

- PacSmith never executes an imported binary, shared object, or maintainer script during analysis.
- Original maintainer scripts remain untrusted evidence. Acknowledgment records a content fingerprint, never permission to execute the Debian or RPM script.
- Archive member paths are normalized; traversal, absolute paths, duplicate members, special device entries, unsafe hard links, and escaping symlinks are rejected.
- External processes use `QProcess` with explicit argument lists — never `/bin/sh -c` with imported values.
- `makepkg` is run in the project directory as the current user. PacSmith refuses to start it as root.
- Only an explicit Install action runs `/usr/bin/pkexec /usr/bin/pacman --noconfirm -U -- <absolute-package-path>`.
- APT source files and repository keyrings are flagged and excluded from generated packages unless explicitly retained. Repository checks require a trusted project-local key and a pinned signer fingerprint.
- AI cannot invent a signing key, elevate privileges, run a package manager, or silently overwrite a user-owned field.
- PKGBUILD validation is intentionally static and does not source or execute the file.

AppImage import is static decomposition: PacSmith unpacks the SquashFS payload, installs an intact AppDir under `/opt`, and generates a host wrapper that runs the developer's `AppRun`. It does not execute the AppImage, preserve its embedded updater, or mount it with FUSE. Prefer a `.deb`, RPM, or Arch package when the developer offers one; those formats generally carry more useful dependencies and integration metadata.

## Persistent projects

Projects use `$XDG_DATA_HOME/pacsmith/projects`, falling back to `~/.local/share/pacsmith/projects`. Each application has `project.json` plus `releases/<version-hash>/`. A release directory contains `release.json`, `PKGBUILD`, `pacsmith.vars`, `sources/`, `files/`, `patches/`, `build/`, and `history/`. Acquisition identity, source kind, trusted keys, install mapping, and lifecycle files are release-specific.

Generated packages include pacman xdata linking the installed package to its PacSmith project, immutable release, acquisition identity, artifact type, and source SHA256. PacSmith can therefore identify managed packages even if the local project files are later missing.

## License

PacSmith is released under the [MIT License](LICENSE).
