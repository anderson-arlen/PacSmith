# Releasing PacSmith

The Git tag is the only source of truth for a PacSmith release version. Do not edit a version number in CMake or application source before releasing.

## Accepted tags

Use one of these exact formats:

| Release type | Tag format | Example |
| --- | --- | --- |
| Alpha | `vMAJOR.MINOR.PATCH-alpha.N` | `v0.1.0-alpha.1` |
| Beta | `vMAJOR.MINOR.PATCH-beta.N` | `v0.1.0-beta.1` |
| Release candidate | `vMAJOR.MINOR.PATCH-rc.N` | `v0.1.0-rc.1` |
| Stable | `vMAJOR.MINOR.PATCH` | `v0.1.0` |

The leading `v`, all three numeric components, the prerelease dot, and the prerelease number are required. For example, use `v0.1.0-rc.1`, not `0.1.0`, `v0.1`, or `v0.1.0-rc1`.

The release workflow preserves the tag version in the application and daemon. Because Arch package versions cannot contain hyphens, it derives a pacman-compatible version without introducing another version to maintain:

| Git tag | Version shown by PacSmith | Arch `pkgver` |
| --- | --- | --- |
| `v0.1.0-alpha.1` | `0.1.0-alpha.1` | `0.1.0alpha1` |
| `v0.1.0-beta.1` | `0.1.0-beta.1` | `0.1.0beta1` |
| `v0.1.0-rc.1` | `0.1.0-rc.1` | `0.1.0rc1` |
| `v0.1.0` | `0.1.0` | `0.1.0` |

Pacman orders those versions as alpha, beta, release candidate, then stable.

## Publish from the GitHub UI

1. Push every intended release change, including the release workflow, to GitHub.
2. Verify the target commit has passed its normal checks.
3. Open **Releases** and choose **Draft a new release**.
4. Choose **Create new tag** and enter an accepted tag from the table above.
5. Confirm the tag targets the exact commit being released.
6. Generate or write the release notes.
7. For an alpha, beta, or release candidate, enable **Set as a pre-release**. Leave it disabled for a stable tag.
8. Publish the release.

Publishing triggers the **Arch x86_64 release** workflow. It validates the tag and prerelease checkbox, embeds the tag version in the GUI, CLI, MCP server, and daemon, runs the complete test suite, builds the Arch package, verifies its install hook, generates `SHA256SUMS`, and uploads both assets to the existing GitHub release.

The GitHub release is visible while its assets are building. If the workflow fails, do not create a new tag or change the version in source. Fix the failure if necessary, push the fix, and create a new prerelease tag. If the tagged source was correct and the failure was transient, rerun the failed workflow from the Actions page.

Do not move or reuse a published tag. Increment the prerelease number instead, such as moving from `v0.1.0-rc.1` to `v0.1.0-rc.2`.

## Development builds

When `PACSMITH_VERSION` is not supplied during configuration, CMake derives a development identity from Git:

```text
development+g07c805ef7185
```

A build from a modified or untracked worktree ends in `.dirty`. A source tree without Git metadata reports `development`.

Packagers can explicitly inject a version when configuring a reproducible out-of-band build:

```bash
cmake -S . -B build -G Ninja -DPACSMITH_VERSION=development+g07c805ef7185
```

Official release builds never use the development fallback. The GitHub workflow always supplies the validated tag version explicitly.
