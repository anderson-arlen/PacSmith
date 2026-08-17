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
            result += QStringLiteral("  cat > \"$pkgdir%1\" <<'PACSMITH_LAUNCHER'\n")
                          .arg(doubleQuotedPath(destination));
            result += QStringLiteral("#!/bin/sh\n");
            if (project.sourceType == SourcePackageType::AppImage) {
                const auto opt = project.installMapping.optDirectory.isEmpty()
                    ? project.archPackageName : project.installMapping.optDirectory;
                // Keep APPIMAGE unset so the extracted AppDir cannot self-update
                // or act as a FUSE-mounted AppImage. Vendor AppRun scripts that
                // dispatch on $0/APPIMAGE belong in the editable AppRun recipe.
                result += QStringLiteral(
                    "APPDIR='/opt/%1'\nexport APPDIR\nOWD=\"$PWD\"\nexport OWD\n"
                    "ARGV0=\"$0\"\nexport ARGV0\nunset APPIMAGE\n"
                    "exec \"%2\" \"$@\"\n")
                              .arg(doubleQuotedPath(opt), doubleQuotedPath(target));
            } else {
                result += QStringLiteral("exec \"%1\" \"$@\"\n")
                              .arg(doubleQuotedPath(target));
            }
            result += QStringLiteral("PACSMITH_LAUNCHER\n");
            result += QStringLiteral("  chmod 0755 \"$pkgdir%1\"\n")
                          .arg(doubleQuotedPath(destination));
        } else {
            // /usr/bin/<name> -> ../../opt/<bundle>/<path>; relative links keep
            // the package relocatable while it is assembled under $pkgdir.
            const auto relativeTarget = target.startsWith(QStringLiteral("/opt/"))
                ? QStringLiteral("../../%1").arg(target.sliced(1)) : target;
            result += QStringLiteral("  ln -sf %1 \"$pkgdir%2\"\n")
                          .arg(PkgbuildGenerator::shellQuote(relativeTarget),
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
    const auto opt = project.installMapping.optDirectory.isEmpty()
        ? project.archPackageName : project.installMapping.optDirectory;
    result += QStringLiteral(
        "\n  # Overlay the reviewed AppRun entry point onto the extracted AppDir.\n");
    result += QStringLiteral("  printf '%s' %1 > \"$pkgdir/opt/%2/AppRun\"\n")
                  .arg(PkgbuildGenerator::shellQuote(appRun.contents), doubleQuotedPath(opt));
    result += QStringLiteral("  chmod 0755 \"$pkgdir/opt/%1/AppRun\"\n")
                  .arg(doubleQuotedPath(opt));
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
    if (icon.missing || icon.projectPath.isEmpty() || icon.sha256.isEmpty() ||
        icon.iconName.isEmpty()) return;
    const auto extension = icon.format.isEmpty()
        ? QFileInfo(icon.projectPath).suffix().toLower() : icon.format.toLower();
    const auto sourceName = QStringLiteral("pacsmith-icon.%1").arg(extension);
    const auto directory = extension == QStringLiteral("svg")
        ? QStringLiteral("/usr/share/icons/hicolor/scalable/apps")
        : QStringLiteral("/usr/share/icons/hicolor/256x256/apps");
    result += QStringLiteral("\n  # Install the selected, content-addressed application icon.\n");
    result += QStringLiteral("  install -Dm644 \"$srcdir/%1\" \"$pkgdir%2/%3.%4\"\n")
                  .arg(doubleQuotedPath(sourceName), doubleQuotedPath(directory),
                       doubleQuotedPath(icon.iconName), doubleQuotedPath(extension));
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

QString PkgbuildGenerator::generate(const PackageRelease &project) {
    const auto [epoch, version] = splitEpochAndVersion(project.debian.version);
    const auto description = project.debian.description.section(QLatin1Char('\n'), 0, 0).trimmed();
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

    QString result;
    result += QStringLiteral("# Generated by PacSmith. This PKGBUILD belongs to you; review it before building.\n");
    result += QStringLiteral("# Imported lifecycle scripts are data and are never executed automatically.\n");
    result += QStringLiteral("pkgname=%1\n").arg(shellQuote(project.archPackageName));
    result += QStringLiteral("pkgver=%1\n").arg(shellQuote(version));
    result += QStringLiteral("pkgrel=%1\n")
                  .arg(project.archPkgrelOverride.isEmpty()
                           ? QString::number(std::max(1, project.archPkgrel))
                           : project.archPkgrelOverride);
    if (!epoch.isEmpty()) result += QStringLiteral("epoch=%1\n").arg(epoch);
    result += QStringLiteral("pkgdesc=%1\n").arg(shellQuote(description.isEmpty() ? project.displayName : description));
    result += QStringLiteral("arch=(%1)\n").arg(shellQuote(translateArchitecture(project.debian.architecture)));
    result += QStringLiteral("url=%1\n").arg(shellQuote(project.debian.homepage));
    result += QStringLiteral("license=('custom:vendor') # Verify the vendor's license terms.\n");
    result += QStringLiteral("depends=(%1)\n").arg(dependencies.join(QLatin1Char(' ')));
    if (project.sourceType == SourcePackageType::AppImage) {
        result += QStringLiteral("makedepends=('squashfs-tools')\n");
    }
    // makepkg strip/debugedit rewrite vendor ELF files and can break packed
    // payloads. PacSmith only repackages prebuilt artifacts.
    result += QStringLiteral("options=('!strip' '!debug')\n");
    const auto sourceIdentity = project.acquisition.canonicalIdentity.isEmpty()
        ? QStringLiteral("local:%1").arg(project.sourceSha256)
        : project.acquisition.canonicalIdentity;
    result += QStringLiteral("xdata=(\n");
    result += QStringLiteral("  %1\n").arg(shellQuote(QStringLiteral("pacsmith.schema=1")));
    result += QStringLiteral("  %1\n").arg(shellQuote(
        QStringLiteral("pacsmith.project=%1").arg(xdataValue(project.projectId))));
    result += QStringLiteral("  %1\n").arg(shellQuote(
        QStringLiteral("pacsmith.release=%1").arg(xdataValue(project.id))));
    result += QStringLiteral("  %1\n").arg(shellQuote(
        QStringLiteral("pacsmith.acquisition=%1").arg(acquisitionKindName(project.acquisition.kind))));
    result += QStringLiteral("  %1\n").arg(shellQuote(
        QStringLiteral("pacsmith.source=%1").arg(xdataValue(sourceIdentity))));
    result += QStringLiteral("  %1\n").arg(shellQuote(
        QStringLiteral("pacsmith.artifact=%1").arg(sourcePackageTypeName(project.sourceType))));
    result += QStringLiteral("  %1\n").arg(shellQuote(
        QStringLiteral("pacsmith.sha256=%1").arg(project.sourceSha256)));
    result += QStringLiteral(")\n");
    // ProjectStore maintains a visible relative symlink beside the PKGBUILD because makepkg only
    // searches local sources by basename. The authoritative vendor file remains under sources/.
    QStringList sources{shellQuote(project.originalSourceFilename)};
    QStringList sums{shellQuote(project.sourceSha256)};
    if (!project.installMapping.icon.missing &&
        !project.installMapping.icon.projectPath.isEmpty() &&
        !project.installMapping.icon.sha256.isEmpty()) {
        const auto extension = project.installMapping.icon.format.isEmpty()
            ? QFileInfo(project.installMapping.icon.projectPath).suffix().toLower()
            : project.installMapping.icon.format.toLower();
        // ProjectStore maintains this visible, release-local source link just
        // like the primary vendor artifact. makepkg does not accept arbitrary
        // subdirectory paths as local source entries.
        sources.append(shellQuote(QStringLiteral("pacsmith-icon.%1").arg(extension)));
        sums.append(shellQuote(project.installMapping.icon.sha256));
    }
    result += QStringLiteral("source=(%1) # primary source -> sources/%2\n")
                  .arg(sources.join(QLatin1Char(' ')), project.originalSourceFilename);
    result += QStringLiteral("noextract=(%1)\n").arg(shellQuote(project.originalSourceFilename));
    result += QStringLiteral("sha256sums=(%1)\n\n").arg(sums.join(QLatin1Char(' ')));
    if (!project.lifecycleScript.contents.isEmpty() && project.lifecycleScript.validationPassed) {
        result += QStringLiteral("install=%1\n\n").arg(shellQuote(project.lifecycleScript.fileName));
    }
    result += QStringLiteral("package() {\n");
    if (project.sourceType == SourcePackageType::Debian) {
        result += QStringLiteral("  local data_archive=''\n");
        result += QStringLiteral("  while IFS= read -r member; do\n");
        result += QStringLiteral("    case \"$member\" in\n");
        result += QStringLiteral("      data.tar|data.tar.*) data_archive=\"$member\"; break ;;\n");
        result += QStringLiteral("    esac\n");
        result += QStringLiteral("  done < <(bsdtar -tf \"$srcdir/%1\")\n")
                      .arg(doubleQuotedPath(project.originalSourceFilename));
        result += QStringLiteral("  if [[ -z \"$data_archive\" ]]; then\n");
        result += QStringLiteral("    error 'Debian data archive was not found'\n");
        result += QStringLiteral("    return 1\n");
        result += QStringLiteral("  fi\n");
        result += QStringLiteral("  bsdtar -xOf \"$srcdir/%1\" \"$data_archive\" | bsdtar -xpf - --no-same-owner -C \"$pkgdir\"\n")
                      .arg(doubleQuotedPath(project.originalSourceFilename));
    } else if (project.sourceType == SourcePackageType::Rpm) {
        // libarchive treats the RPM container as a filter and exposes its cpio
        // payload without requiring rpm2cpio or executing any package content.
        result += QStringLiteral("  bsdtar -xpf \"$srcdir/%1\" --no-same-owner -C \"$pkgdir\"\n")
                      .arg(doubleQuotedPath(project.originalSourceFilename));
    } else if (project.sourceType == SourcePackageType::ArchPackage) {
        result += QStringLiteral("  bsdtar -xpf \"$srcdir/%1\" --no-same-owner -C \"$pkgdir\" \\\n")
                      .arg(doubleQuotedPath(project.originalSourceFilename));
        result += QStringLiteral("    --exclude './.PKGINFO' --exclude '.PKGINFO' \\\n");
        result += QStringLiteral("    --exclude './.BUILDINFO' --exclude '.BUILDINFO' \\\n");
        result += QStringLiteral("    --exclude './.MTREE' --exclude '.MTREE' \\\n");
        result += QStringLiteral("    --exclude './.INSTALL' --exclude '.INSTALL'\n");
    } else if (project.sourceType == SourcePackageType::Archive) {
        if (project.installMapping.archiveLayout == ArchiveLayout::PreserveRoot) {
            result += QStringLiteral("  bsdtar -xpf \"$srcdir/%1\" --no-same-owner -C \"$pkgdir\"\n")
                          .arg(doubleQuotedPath(project.originalSourceFilename));
        } else {
            const auto opt = project.installMapping.optDirectory.isEmpty()
                ? project.archPackageName : project.installMapping.optDirectory;
            result += QStringLiteral("  install -d \"$pkgdir/opt/%1\"\n").arg(doubleQuotedPath(opt));
            const auto strip = project.installMapping.stripCommonPrefix &&
                               !project.installMapping.commonPrefix.isEmpty()
                ? QStringLiteral(" --strip-components 1") : QString{};
            result += QStringLiteral("  bsdtar -xpf \"$srcdir/%1\" --no-same-owner%2 -C \"$pkgdir/opt/%3\"\n")
                          .arg(doubleQuotedPath(project.originalSourceFilename), strip,
                               doubleQuotedPath(opt));
        }
    } else if (project.sourceType == SourcePackageType::AppImage) {
        const auto opt = project.installMapping.optDirectory.isEmpty()
            ? project.archPackageName : project.installMapping.optDirectory;
        result += QStringLiteral("  # Preserve the complete AppDir below /opt; host integration is emitted separately.\n");
        result += QStringLiteral("  install -d \"$pkgdir/opt/%1\"\n")
                      .arg(doubleQuotedPath(opt));
        result += QStringLiteral("  unsquashfs -no-progress -no-xattrs -f -o %1 -d \"$pkgdir/opt/%2\" \"$srcdir/%3\"\n")
                      .arg(project.installMapping.appImageOffset)
                      .arg(doubleQuotedPath(opt),
                           doubleQuotedPath(project.originalSourceFilename));
        result += QStringLiteral("  find \"$pkgdir/opt/%1\" -type f -exec chmod u-s,g-s {} +\n")
                      .arg(doubleQuotedPath(opt));
    } else if (project.sourceType == SourcePackageType::ElfBinary) {
        const auto launcher = std::find_if(
            project.installMapping.launchers.cbegin(), project.installMapping.launchers.cend(),
            [](const auto &candidate) { return candidate.enabled && !candidate.missing; });
        const auto selectedDestination = launcher != project.installMapping.launchers.cend()
            ? launcher->destination : project.installMapping.binaryDestination;
        const auto destination = selectedDestination.isEmpty()
            ? QStringLiteral("/usr/bin/%1").arg(project.archPackageName)
            : selectedDestination;
        result += QStringLiteral("  install -Dm755 \"$srcdir/%1\" \"$pkgdir%2\"\n")
                      .arg(doubleQuotedPath(project.originalSourceFilename), doubleQuotedPath(destination));
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
    return problems.isEmpty() ? QStringLiteral("Basic structural validation passed (the file was not executed).")
                              : problems.join(QLatin1Char('\n'));
}

} // namespace pacsmith
