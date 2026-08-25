#include "core/pkgbuild_generator.hpp"

#include <QRegularExpression>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <QUrl>

#include <algorithm>

namespace pacsmith {
namespace {

QString doubleQuotedPath(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    value.replace(QLatin1Char('`'), QStringLiteral("\\`"));
    return value;
}

QString doubleQuotedExpandable(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('`'), QStringLiteral("\\`"));
    return value;
}

QString optDirectory(const PackageRelease &project) {
    return project.installMapping.optDirectory.isEmpty()
        ? project.archPackageName : project.installMapping.optDirectory;
}

QString withOptVariable(const QString &path, const QString &opt) {
    const auto prefix = QStringLiteral("/opt/%1").arg(opt);
    if (path == prefix) return QStringLiteral("/opt/${_PACSMITH_OPT}");
    if (path.startsWith(prefix + QLatin1Char('/'))) {
        return QStringLiteral("/opt/${_PACSMITH_OPT}/%1").arg(path.mid(prefix.size() + 1));
    }
    return path;
}

QString iconSourceName(const PackageRelease &project) {
    const auto &icon = project.installMapping.icon;
    if (icon.missing || icon.sha256.isEmpty()) return {};
    auto extension = icon.format.isEmpty()
        ? QFileInfo(icon.projectPath).suffix().toLower() : icon.format.toLower();
    if (extension.isEmpty()) extension = QFileInfo(icon.sourcePath).suffix().toLower();
    if (extension.isEmpty()) return {};
    return QStringLiteral("pacsmith-icon.%1").arg(extension);
}

QString pkgrelValue(const PackageRelease &project) {
    return project.archPkgrelOverride.isEmpty()
        ? QString::number(std::max(1, project.archPkgrel))
        : project.archPkgrelOverride;
}

QString sourceIdentityValue(const PackageRelease &project) {
    return project.acquisition.canonicalIdentity.isEmpty()
        ? QStringLiteral("local:%1").arg(project.sourceSha256)
        : project.acquisition.canonicalIdentity;
}

void appendAssignment(QString &result, const QString &name, const QString &value) {
    result += name;
    result += QLatin1Char('=');
    result += PkgbuildGenerator::shellQuote(value);
    result += QLatin1Char('\n');
}

void appendArray(QString &result, const QString &name, const QStringList &values) {
    result += name;
    result += QStringLiteral("=(");
    for (int i = 0; i < values.size(); ++i) {
        if (i > 0) result += QLatin1Char(' ');
        result += PkgbuildGenerator::shellQuote(values.at(i));
    }
    result += QStringLiteral(")\n");
}

QString xdataValue(const QString &value) {
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

void appendPayloadExclusions(QString &result, const PackageRelease &project) {
    // A decomposed AppImage remains an intact AppDir under /opt. Its internal
    // etc/, usr/, and runtime paths are private bundle paths, not host paths,
    // and selectively pruning them invalidates the vendor runtime contract.
    if (project.sourceType == SourcePackageType::AppImage ||
        project.payloadRules.isEmpty()) return;
    result += QStringLiteral("\n  # Payload files excluded after PacSmith review.\n");
    for (const auto &rule : project.payloadRules) {
        if (rule.excluded) {
            result += QStringLiteral("  rm -rf -- \"${pkgdir}/%1\"\n")
                          .arg(doubleQuotedPath(rule.path));
        }
    }
}

QString installedPayloadPath(const PackageRelease &project, QString path) {
    return PkgbuildGenerator::installedPayloadPath(project, path);
}

void appendLaunchers(QString &result, const PackageRelease &project) {
    // Standalone ELF sources are installed directly at the selected command
    // destination below; emitting a second link would replace that executable.
    if (project.sourceType == SourcePackageType::ElfBinary) return;
    for (const auto &launcher : project.installMapping.launchers) {
        if (!launcher.enabled || launcher.missing || launcher.sourcePath.isEmpty() ||
            launcher.commandName.isEmpty()) continue;
        const auto destination = launcher.destination.isEmpty()
            ? QStringLiteral("/usr/bin/%1").arg(launcher.commandName)
            : launcher.destination;
        result += QStringLiteral("\n  # Expose inspected payload command %1.\n")
                      .arg(launcher.commandName);
        result += QStringLiteral("  install -d \"$pkgdir%1\"\n")
                      .arg(doubleQuotedPath(QFileInfo(destination).path()));
        const auto target = installedPayloadPath(project, launcher.sourcePath);
        // DEB/RPM/root-layout archives commonly already install their command
        // at exactly the requested destination. Replacing it with a symlink to
        // itself would destroy the executable during package assembly.
        if (launcher.kind == LauncherKind::Symlink && target == destination) continue;
        if (launcher.kind == LauncherKind::Wrapper ||
            project.sourceType == SourcePackageType::AppImage) {
            if (project.sourceType == SourcePackageType::AppImage) {
                // Keep APPIMAGE unset so the extracted AppDir cannot self-update
                // or act as a FUSE-mounted AppImage. Vendor AppRun scripts that
                // dispatch on $0/APPIMAGE belong in the editable AppRun recipe.
                // The heredoc is unquoted so _PACSMITH_OPT expands at makepkg time;
                // runtime shell parameters are escaped.
                const auto execTarget = withOptVariable(target, optDirectory(project));
                result += QStringLiteral("  cat > \"$pkgdir%1\" <<PACSMITH_LAUNCHER\n")
                              .arg(doubleQuotedPath(destination));
                result += QStringLiteral("#!/bin/sh\n");
                result += QStringLiteral(
                    "APPDIR='/opt/${_PACSMITH_OPT}'\nexport APPDIR\nOWD=\"\\$PWD\"\nexport OWD\n"
                    "ARGV0=\"\\$0\"\nexport ARGV0\nunset APPIMAGE\n"
                    "exec \"%1\" \"\\$@\"\n")
                              .arg(doubleQuotedExpandable(execTarget));
            } else {
                result += QStringLiteral("  cat > \"$pkgdir%1\" <<'PACSMITH_LAUNCHER'\n")
                              .arg(doubleQuotedPath(destination));
                result += QStringLiteral("#!/bin/sh\n");
                result += QStringLiteral("exec \"%1\" \"$@\"\n")
                              .arg(doubleQuotedPath(target));
            }
            result += QStringLiteral("PACSMITH_LAUNCHER\n");
            result += QStringLiteral("  chmod 0755 \"$pkgdir%1\"\n")
                          .arg(doubleQuotedPath(destination));
        } else {
            // /usr/bin/<name> -> ../../opt/<bundle>/<path>; relative links keep
            // the package relocatable while it is assembled under $pkgdir.
            const auto variablePath = withOptVariable(target, optDirectory(project));
            const auto expandable = variablePath.contains(QStringLiteral("${_PACSMITH_OPT}"));
            const auto relativeTarget = expandable
                ? QStringLiteral("../..%1").arg(variablePath)
                : (target.startsWith(QStringLiteral("/opt/"))
                       ? QStringLiteral("../../%1").arg(target.sliced(1)) : target);
            result += QStringLiteral("  ln -sf %1 \"$pkgdir%2\"\n")
                          .arg(expandable ? QStringLiteral("\"%1\"").arg(doubleQuotedExpandable(relativeTarget))
                                          : PkgbuildGenerator::shellQuote(relativeTarget),
                               doubleQuotedPath(destination));
        }
    }
}

void appendAppRunOverlay(QString &result, const PackageRelease &project) {
    if (project.sourceType != SourcePackageType::AppImage) return;
    const auto &appRun = project.installMapping.appRun;
    if (!appRun.script || !appRun.userModified || appRun.contents.isEmpty() ||
        appRun.contents == appRun.originalContents) {
        return;
    }
    result += QStringLiteral(
        "\n  # Overlay the reviewed AppRun entry point onto the extracted AppDir.\n");
    result += QStringLiteral("  printf '%s' %1 > \"$pkgdir/opt/${_PACSMITH_OPT}/AppRun\"\n")
                  .arg(PkgbuildGenerator::shellQuote(appRun.contents));
    result += QStringLiteral("  chmod 0755 \"$pkgdir/opt/${_PACSMITH_OPT}/AppRun\"\n");
}

void appendDesktopEntries(QString &result, const PackageRelease &project) {
    for (const auto &desktop : project.installMapping.desktopEntries) {
        const auto destination = desktop.destination.isEmpty()
            ? QStringLiteral("/usr/share/applications/%1.desktop").arg(desktop.id)
            : desktop.destination;
        if (!desktop.enabled) {
            // Disabled AppImage candidates were never installed into the host
            // namespace. Leave their original copies untouched inside AppDir.
            if (project.sourceType == SourcePackageType::AppImage) continue;
            if (!desktop.sourcePath.isEmpty()) {
                result += QStringLiteral("  rm -f -- \"$pkgdir/%1\"\n")
                              .arg(doubleQuotedPath(desktop.sourcePath));
            }
            result += QStringLiteral("  rm -f -- \"$pkgdir%1\"\n")
                          .arg(doubleQuotedPath(destination));
            continue;
        }
        if (desktop.contents.isEmpty()) continue;
        result += QStringLiteral("\n  # Install the reviewed desktop entry %1.\n")
                      .arg(desktop.id);
        result += QStringLiteral("  install -d \"$pkgdir%1\"\n")
                      .arg(doubleQuotedPath(QFileInfo(destination).path()));
        result += QStringLiteral("  printf '%s' %1 > \"$pkgdir%2\"\n")
                      .arg(PkgbuildGenerator::shellQuote(desktop.contents),
                           doubleQuotedPath(destination));
        result += QStringLiteral("  chmod 0644 \"$pkgdir%1\"\n")
                      .arg(doubleQuotedPath(destination));
        const auto sourceInstalled = desktop.sourcePath.isEmpty()
            ? QString{} : installedPayloadPath(project, desktop.sourcePath);
        if (project.sourceType != SourcePackageType::AppImage &&
            !sourceInstalled.isEmpty() && sourceInstalled != destination) {
            result += QStringLiteral("  rm -f -- \"$pkgdir%1\"\n")
                          .arg(doubleQuotedPath(sourceInstalled));
        }
    }
}

void appendSelectedIcon(QString &result, const PackageRelease &project) {
    const auto &icon = project.installMapping.icon;
    const auto installed = icon.installedPath();
    if (icon.missing || icon.sha256.isEmpty() || installed.isEmpty()) return;
    result += QStringLiteral("\n  # Install the selected, content-addressed application icon.\n");
    result += QStringLiteral("  install -Dm644 \"$srcdir/${_PACSMITH_ICON}\" \"$pkgdir%1\"\n")
                  .arg(doubleQuotedPath(installed));
}

} // namespace

QString PkgbuildGenerator::shellQuote(const QString &value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString PkgbuildGenerator::sanitizePackageName(const QString &name) {
    QString result = name.toLower().trimmed();
    result.replace(QRegularExpression(QStringLiteral("[^a-z0-9@._+\\-]+")), QStringLiteral("-"));
    result.replace(QRegularExpression(QStringLiteral("-+")), QStringLiteral("-"));
    while (result.startsWith(QLatin1Char('.')) || result.startsWith(QLatin1Char('-'))) result.remove(0, 1);
    while (result.endsWith(QLatin1Char('.')) || result.endsWith(QLatin1Char('-'))) result.chop(1);
    return result.isEmpty() ? QStringLiteral("vendor-package-bin") : result;
}

std::pair<QString, QString> PkgbuildGenerator::splitEpochAndVersion(const QString &debianVersion) {
    QString epoch;
    QString version = debianVersion.trimmed();
    const auto colon = version.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool validEpoch = false;
        version.left(colon).toInt(&validEpoch);
        if (validEpoch) {
            epoch = version.left(colon);
            version = version.mid(colon + 1);
        }
    }
    const auto revisionSeparator = version.lastIndexOf(QLatin1Char('-'));
    if (revisionSeparator > 0) version = version.left(revisionSeparator);
    version.replace(QLatin1Char('~'), QLatin1Char('.'));
    version.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9+._]+")), QStringLiteral("_"));
    version.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    if (version.isEmpty()) version = QStringLiteral("0");
    return {epoch, version};
}

QString PkgbuildGenerator::translateVersion(const QString &debianVersion) {
    return splitEpochAndVersion(debianVersion).second;
}

QString PkgbuildGenerator::translateArchitecture(const QString &debianArchitecture) {
    const auto architecture = debianArchitecture.toLower();
    if (architecture == QStringLiteral("amd64")) return QStringLiteral("x86_64");
    if (architecture == QStringLiteral("arm64")) return QStringLiteral("aarch64");
    if (architecture == QStringLiteral("i386") || architecture == QStringLiteral("i686")) {
        return QStringLiteral("i686");
    }
    if (architecture == QStringLiteral("all") || architecture == QStringLiteral("noarch")) {
        return QStringLiteral("any");
    }
    return architecture;
}

QString PkgbuildGenerator::identityVariables(const PackageRelease &project) {
    const auto [epoch, version] = splitEpochAndVersion(project.debian.version);
    const auto description = project.packageMetadata.description
                                 .section(QLatin1Char('\n'), 0, 0).trimmed();
    const auto install = (!project.lifecycleScript.contents.isEmpty() &&
                          project.lifecycleScript.validationPassed)
        ? project.lifecycleScript.fileName : QString{};
    QString result;
    result += QStringLiteral(
        "# PacSmith-owned release identity. Do not edit; rewritten for each vendor artifact.\n");
    appendAssignment(result, QStringLiteral("_PACSMITH_PKGNAME"), project.archPackageName);
    appendAssignment(result, QStringLiteral("_PACSMITH_COMPAT_PKGNAME"), QString{});
    appendArray(result, QStringLiteral("_PACSMITH_LICENSES"), project.packageMetadata.licenses);
    appendArray(result, QStringLiteral("_PACSMITH_PROVIDES"), project.packageMetadata.provides);
    appendArray(result, QStringLiteral("_PACSMITH_CONFLICTS"), project.packageMetadata.conflicts);
    appendAssignment(result, QStringLiteral("_PACSMITH_PKGVER"), version);
    appendAssignment(result, QStringLiteral("_PACSMITH_PKGREL"), pkgrelValue(project));
    appendAssignment(result, QStringLiteral("_PACSMITH_EPOCH"), epoch);
    appendAssignment(result, QStringLiteral("_PACSMITH_PKGDESC"),
                     description.isEmpty() ? project.displayName : description);
    appendAssignment(result, QStringLiteral("_PACSMITH_ARCH"),
                     translateArchitecture(project.debian.architecture));
    appendAssignment(result, QStringLiteral("_PACSMITH_URL"), project.packageMetadata.homepage);
    appendAssignment(result, QStringLiteral("_PACSMITH_SOURCE"), project.originalSourceFilename);
    appendAssignment(result, QStringLiteral("_PACSMITH_SHA256"), project.sourceSha256);
    appendAssignment(result, QStringLiteral("_PACSMITH_PROJECT"), xdataValue(project.projectId));
    appendAssignment(result, QStringLiteral("_PACSMITH_RELEASE"), xdataValue(project.id));
    appendAssignment(result, QStringLiteral("_PACSMITH_ACQUISITION"),
                     acquisitionKindName(project.acquisition.kind));
    appendAssignment(result, QStringLiteral("_PACSMITH_SOURCE_IDENTITY"),
                     xdataValue(sourceIdentityValue(project)));
    appendAssignment(result, QStringLiteral("_PACSMITH_ARTIFACT"),
                     sourcePackageTypeName(project.sourceType));
    appendAssignment(result, QStringLiteral("_PACSMITH_OPT"), optDirectory(project));
    appendAssignment(result, QStringLiteral("_PACSMITH_ICON"), iconSourceName(project));
    appendAssignment(result, QStringLiteral("_PACSMITH_ICON_SHA256"),
                     iconSourceName(project).isEmpty() ? QString{} : project.installMapping.icon.sha256);
    appendAssignment(result, QStringLiteral("_PACSMITH_APPIMAGE_OFFSET"),
                     project.sourceType == SourcePackageType::AppImage
                         ? QString::number(project.installMapping.appImageOffset) : QString{});
    appendAssignment(result, QStringLiteral("_PACSMITH_INSTALL"), install);
    return result;
}

QString PkgbuildGenerator::generate(const PackageRelease &project) {
    QSet<QString> seenDependencies;
    QStringList dependencies;
    for (const auto &dependency : project.dependencies) {
        if (dependency.status == MappingStatus::Resolved && !dependency.archPackage.isEmpty() &&
            !dependency.ignored && !dependency.bundled && !dependency.provided &&
            !seenDependencies.contains(dependency.archPackage)) {
            dependencies.append(shellQuote(dependency.archPackage));
            seenDependencies.insert(dependency.archPackage);
        }
    }
    for (const auto &dependency : project.packageMetadata.additionalDependencies) {
        const auto trimmed = dependency.trimmed();
        if (!trimmed.isEmpty() && !seenDependencies.contains(trimmed)) {
            dependencies.append(shellQuote(trimmed));
            seenDependencies.insert(trimmed);
        }
    }

    QString result;
    result += QStringLiteral("# Generated by PacSmith. This PKGBUILD belongs to you; review it before building.\n");
    result += QStringLiteral("# Imported lifecycle scripts are data and are never executed automatically.\n");
    result += QStringLiteral("# Per-release identity lives in pacsmith.vars and is rewritten for each vendor artifact.\n");
    result += QStringLiteral("source \"${startdir:-.}/pacsmith.vars\"\n");
    result += QStringLiteral("pkgname=\"${_PACSMITH_PKGNAME}\"\n");
    result += QStringLiteral("pkgver=\"${_PACSMITH_PKGVER}\"\n");
    result += QStringLiteral("pkgrel=\"${_PACSMITH_PKGREL}\"\n");
    result += QStringLiteral("[[ -n ${_PACSMITH_EPOCH} ]] && epoch=\"${_PACSMITH_EPOCH}\"\n");
    result += QStringLiteral("pkgdesc=\"${_PACSMITH_PKGDESC}\"\n");
    result += QStringLiteral("arch=(\"${_PACSMITH_ARCH}\")\n");
    result += QStringLiteral("url=\"${_PACSMITH_URL}\"\n");
    result += QStringLiteral("license=(\"${_PACSMITH_LICENSES[@]}\")\n");
    result += QStringLiteral("depends=(%1)\n").arg(dependencies.join(QLatin1Char(' ')));
    result += QStringLiteral("provides=(\"${_PACSMITH_PROVIDES[@]}\")\n");
    result += QStringLiteral("conflicts=(\"${_PACSMITH_CONFLICTS[@]}\")\n");
    result += QStringLiteral("if [[ -n ${_PACSMITH_COMPAT_PKGNAME} ]]; then\n");
    result += QStringLiteral("  provides+=(\"${_PACSMITH_COMPAT_PKGNAME}=${pkgver}\")\n");
    result += QStringLiteral("  conflicts+=(\"${_PACSMITH_COMPAT_PKGNAME}\")\n");
    result += QStringLiteral("fi\n");
    if (project.sourceType == SourcePackageType::AppImage) {
        result += QStringLiteral("makedepends=('squashfs-tools')\n");
    }
    // makepkg strip/debugedit rewrite vendor ELF files and can break packed
    // payloads. PacSmith only repackages prebuilt artifacts.
    result += QStringLiteral("options=('!strip' '!debug')\n");
    result += QStringLiteral("xdata=(\n");
    result += QStringLiteral("  'pacsmith.schema=1'\n");
    result += QStringLiteral("  \"pacsmith.project=${_PACSMITH_PROJECT}\"\n");
    result += QStringLiteral("  \"pacsmith.release=${_PACSMITH_RELEASE}\"\n");
    result += QStringLiteral("  \"pacsmith.acquisition=${_PACSMITH_ACQUISITION}\"\n");
    result += QStringLiteral("  \"pacsmith.source=${_PACSMITH_SOURCE_IDENTITY}\"\n");
    result += QStringLiteral("  \"pacsmith.artifact=${_PACSMITH_ARTIFACT}\"\n");
    result += QStringLiteral("  \"pacsmith.sha256=${_PACSMITH_SHA256}\"\n");
    result += QStringLiteral(")\n");
    // ProjectStore maintains a visible relative symlink beside the PKGBUILD because makepkg only
    // searches local sources by basename. The authoritative vendor file remains under sources/.
    result += QStringLiteral("source=(\"${_PACSMITH_SOURCE}\")\n");
    result += QStringLiteral("sha256sums=(\"${_PACSMITH_SHA256}\")\n");
    result += QStringLiteral("if [[ -n ${_PACSMITH_ICON} ]]; then\n");
    result += QStringLiteral("  source+=(\"${_PACSMITH_ICON}\")\n");
    result += QStringLiteral("  sha256sums+=(\"${_PACSMITH_ICON_SHA256}\")\n");
    result += QStringLiteral("fi\n");
    result += QStringLiteral("noextract=(\"${_PACSMITH_SOURCE}\")\n");
    result += QStringLiteral("[[ -n ${_PACSMITH_INSTALL} ]] && install=\"${_PACSMITH_INSTALL}\"\n\n");
    result += QStringLiteral("package() {\n");
    if (project.sourceType == SourcePackageType::Debian) {
        result += QStringLiteral("  local data_archive=''\n");
        result += QStringLiteral("  while IFS= read -r member; do\n");
        result += QStringLiteral("    case \"$member\" in\n");
        result += QStringLiteral("      data.tar|data.tar.*) data_archive=\"$member\"; break ;;\n");
        result += QStringLiteral("    esac\n");
        result += QStringLiteral("  done < <(bsdtar -tf \"$srcdir/${_PACSMITH_SOURCE}\")\n");
        result += QStringLiteral("  if [[ -z \"$data_archive\" ]]; then\n");
        result += QStringLiteral("    error 'Debian data archive was not found'\n");
        result += QStringLiteral("    return 1\n");
        result += QStringLiteral("  fi\n");
        result += QStringLiteral(
            "  bsdtar -xOf \"$srcdir/${_PACSMITH_SOURCE}\" \"$data_archive\" | bsdtar -xpf - --no-same-owner -C \"$pkgdir\"\n");
    } else if (project.sourceType == SourcePackageType::Rpm) {
        // libarchive treats the RPM container as a filter and exposes its cpio
        // payload without requiring rpm2cpio or executing any package content.
        result += QStringLiteral(
            "  bsdtar -xpf \"$srcdir/${_PACSMITH_SOURCE}\" --no-same-owner -C \"$pkgdir\"\n");
    } else if (project.sourceType == SourcePackageType::ArchPackage) {
        result += QStringLiteral(
            "  bsdtar -xpf \"$srcdir/${_PACSMITH_SOURCE}\" --no-same-owner -C \"$pkgdir\" \\\n");
        result += QStringLiteral("    --exclude './.PKGINFO' --exclude '.PKGINFO' \\\n");
        result += QStringLiteral("    --exclude './.BUILDINFO' --exclude '.BUILDINFO' \\\n");
        result += QStringLiteral("    --exclude './.MTREE' --exclude '.MTREE' \\\n");
        result += QStringLiteral("    --exclude './.INSTALL' --exclude '.INSTALL'\n");
    } else if (project.sourceType == SourcePackageType::Archive) {
        if (project.installMapping.archiveLayout == ArchiveLayout::PreserveRoot) {
            result += QStringLiteral(
                "  bsdtar -xpf \"$srcdir/${_PACSMITH_SOURCE}\" --no-same-owner -C \"$pkgdir\"\n");
        } else {
            const auto strip = project.installMapping.stripCommonPrefix &&
                               !project.installMapping.commonPrefix.isEmpty()
                ? QStringLiteral(" --strip-components 1") : QString{};
            result += QStringLiteral("  install -d \"$pkgdir/opt/${_PACSMITH_OPT}\"\n");
            result += QStringLiteral(
                "  bsdtar -xpf \"$srcdir/${_PACSMITH_SOURCE}\" --no-same-owner%1 -C \"$pkgdir/opt/${_PACSMITH_OPT}\"\n")
                          .arg(strip);
        }
    } else if (project.sourceType == SourcePackageType::AppImage) {
        result += QStringLiteral("  # Preserve the complete AppDir below /opt; host integration is emitted separately.\n");
        result += QStringLiteral("  install -d \"$pkgdir/opt/${_PACSMITH_OPT}\"\n");
        result += QStringLiteral(
            "  unsquashfs -no-progress -no-xattrs -f -o ${_PACSMITH_APPIMAGE_OFFSET} -d \"$pkgdir/opt/${_PACSMITH_OPT}\" \"$srcdir/${_PACSMITH_SOURCE}\"\n");
        result += QStringLiteral("  find \"$pkgdir/opt/${_PACSMITH_OPT}\" -type f -exec chmod u-s,g-s {} +\n");
    } else if (project.sourceType == SourcePackageType::ElfBinary) {
        const auto launcher = std::find_if(
            project.installMapping.launchers.cbegin(), project.installMapping.launchers.cend(),
            [](const auto &candidate) { return candidate.enabled && !candidate.missing; });
        const auto selectedDestination = launcher != project.installMapping.launchers.cend()
            ? launcher->destination : project.installMapping.binaryDestination;
        if (selectedDestination.isEmpty()) {
            result += QStringLiteral(
                "  install -Dm755 \"$srcdir/${_PACSMITH_SOURCE}\" \"$pkgdir/usr/bin/${_PACSMITH_PKGNAME}\"\n");
        } else {
            result += QStringLiteral("  install -Dm755 \"$srcdir/${_PACSMITH_SOURCE}\" \"$pkgdir%1\"\n")
                          .arg(doubleQuotedPath(selectedDestination));
        }
    }
    appendPayloadExclusions(result, project);
    appendLaunchers(result, project);
    appendAppRunOverlay(result, project);
    appendDesktopEntries(result, project);
    appendSelectedIcon(result, project);
    result += QStringLiteral("}\n");
    return result;
}

QString PkgbuildGenerator::installedPayloadPath(const PackageRelease &project,
                                                const QString &payloadPath) {
    QString path = payloadPath;
    if (project.sourceType == SourcePackageType::Archive &&
        project.installMapping.archiveLayout == ArchiveLayout::OptBundle) {
        if (project.installMapping.stripCommonPrefix &&
            !project.installMapping.commonPrefix.isEmpty()) {
            const auto prefix = project.installMapping.commonPrefix + QLatin1Char('/');
            if (path.startsWith(prefix)) path.remove(0, prefix.size());
        }
        const auto opt = project.installMapping.optDirectory.isEmpty()
            ? project.archPackageName : project.installMapping.optDirectory;
        return QStringLiteral("/opt/%1/%2").arg(opt, path);
    }
    if (project.sourceType == SourcePackageType::AppImage) {
        const auto opt = project.installMapping.optDirectory.isEmpty()
            ? project.archPackageName : project.installMapping.optDirectory;
        return QStringLiteral("/opt/%1/%2").arg(opt, path);
    }
    if (project.sourceType == SourcePackageType::ElfBinary) {
        const auto destination = project.installMapping.binaryDestination.isEmpty()
            ? QStringLiteral("/usr/bin/%1").arg(project.archPackageName)
            : project.installMapping.binaryDestination;
        return destination;
    }
    if (path.startsWith(QLatin1Char('/'))) return path;
    return QLatin1Char('/') + path;
}

QString PkgbuildGenerator::validate(const QString &contents) {
    QStringList problems;
    const QStringList required{QStringLiteral("pkgname="), QStringLiteral("pkgver="),
                               QStringLiteral("pkgrel="), QStringLiteral("arch=("),
                               QStringLiteral("source=("), QStringLiteral("sha256sums=("),
                               QStringLiteral("package()")};
    for (const auto &field : required) {
        if (!contents.contains(field)) problems.append(QStringLiteral("Missing %1").arg(field));
    }
    int braceDepth = 0;
    for (const auto character : contents) {
        if (character == QLatin1Char('{')) ++braceDepth;
        else if (character == QLatin1Char('}')) --braceDepth;
        if (braceDepth < 0) break;
    }
    if (braceDepth != 0) problems.append(QStringLiteral("Unbalanced braces"));
    if (problems.isEmpty()) {
        auto message = QStringLiteral("Basic structural validation passed (the file was not executed).");
        if (!contents.contains(QStringLiteral("pacsmith.vars")) &&
            !contents.contains(QStringLiteral("_PACSMITH_"))) {
            message += QStringLiteral(
                "\nThis recipe does not use PacSmith identity variables; pkgver, the vendor filename, and checksums will go stale on the next vendor version.");
        }
        return message;
    }
    return problems.join(QLatin1Char('\n'));
}

} // namespace pacsmith
