#include "core/project_store/project_store.hpp"
#include "core/project_store/internal.hpp"

#include "core/lifecycle_validator.hpp"
#include "core/managed_package.hpp"
#include "core/path_safety.hpp"
#include "core/pkgbuild_generator.hpp"
#include "core/source_analyzer.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include <algorithm>

namespace pacsmith {
using namespace project_store_internal;

ProjectStore::ProjectStore(std::filesystem::path projectsRoot) : root_(std::move(projectsRoot)) {}

std::filesystem::path ProjectStore::defaultProjectsRoot() {
    const auto xdg = qEnvironmentVariable("XDG_DATA_HOME");
    if (!xdg.isEmpty() && QDir::isAbsolutePath(xdg)) {
        return pathFromQString(QDir(xdg).filePath(QStringLiteral("pacsmith/projects")));
    }
    return pathFromQString(QDir::home().filePath(QStringLiteral(".local/share/pacsmith/projects")));
}

const std::filesystem::path &ProjectStore::projectsRoot() const noexcept { return root_; }

std::filesystem::path ProjectStore::projectPath(const QString &id) const {
    return root_ / pathFromQString(id);
}

std::filesystem::path ProjectStore::releasePath(const QString &projectId,
                                                const QString &releaseIdValue) const {
    return projectPath(projectId) / "releases" / pathFromQString(releaseIdValue);
}

std::filesystem::path ProjectStore::releasePath(const PackageRelease &release) const {
    return releasePath(release.projectId, release.id);
}

std::filesystem::path ProjectStore::pkgbuildPath(const PackageRelease &release) const {
    return releasePath(release) / "PKGBUILD";
}

std::filesystem::path ProjectStore::sourcePath(const PackageRelease &release) const {
    return releasePath(release) / "sources" / pathFromQString(release.originalSourceFilename);
}

std::filesystem::path ProjectStore::iconPath(const Project &project) const {
    const auto safe = PathSafety::normalizedArchivePath(project.iconPath);
    if (!safe) return {};
    return projectPath(project.id) / pathFromQString(*safe);
}

std::filesystem::path ProjectStore::releaseIconPath(const PackageRelease &release) const {
    const auto safe = PathSafety::normalizedArchivePath(release.iconPath);
    if (!safe || !safe->startsWith(QStringLiteral("files/"))) return {};
    return releasePath(release) / pathFromQString(*safe);
}

std::filesystem::path ProjectStore::lifecyclePath(const PackageRelease &release) const {
    return releasePath(release) / pathFromQString(release.lifecycleScript.fileName);
}

bool ProjectStore::synchronizeIntegrationSources(const PackageRelease &release,
                                                 QString *error) const {
    const auto &icon = release.installMapping.icon;
    if (icon.missing || icon.projectPath.isEmpty() || icon.sha256.isEmpty()) return true;
    const auto safe = PathSafety::normalizedArchivePath(icon.projectPath);
    if (!safe || !safe->startsWith(QStringLiteral("files/"))) {
        if (error != nullptr) *error = QStringLiteral("Integration icon path is unsafe");
        return false;
    }
    const auto extension = icon.format.isEmpty()
        ? QFileInfo(*safe).suffix().toLower() : icon.format.toLower();
    if (extension != QStringLiteral("png") && extension != QStringLiteral("svg") &&
        extension != QStringLiteral("xpm")) {
        if (error != nullptr) *error = QStringLiteral("Integration icon format is unsupported");
        return false;
    }
    const auto source = releasePath(release) / pathFromQString(*safe);
    QString hashError;
    if (!std::filesystem::is_regular_file(source) ||
        sha256File(source, &hashError) != icon.sha256) {
        if (error != nullptr) {
            *error = hashError.isEmpty()
                ? QStringLiteral("Integration icon is missing or no longer matches its recorded SHA256")
                : hashError;
        }
        return false;
    }
    const auto relativeSource = pathFromQString(*safe);
    const auto alias = releasePath(release) /
                       pathFromQString(QStringLiteral("pacsmith-icon.%1").arg(extension));
    std::error_code filesystemError;
    if (std::filesystem::is_symlink(alias, filesystemError) && !filesystemError &&
        std::filesystem::read_symlink(alias, filesystemError) == relativeSource &&
        !filesystemError) {
        return true;
    }
    filesystemError.clear();
    if (std::filesystem::exists(alias, filesystemError) ||
        std::filesystem::is_symlink(alias, filesystemError)) {
        std::filesystem::remove(alias, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return false;
        }
    }
    std::filesystem::create_symlink(relativeSource, alias, filesystemError);
    if (!filesystemError) return true;
    filesystemError.clear();
    return copyFileAtomically(source, alias, error);
}

QList<Project> ProjectStore::list(QString *error) const {
    QList<Project> result;
    const QDir root(qStringFromPath(root_));
    if (!root.exists()) return result;
    const auto directories = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto &directory : directories) {
        QString loadError;
        auto project = loadById(directory, &loadError);
        if (project) result.append(std::move(*project));
        else if (error != nullptr && error->isEmpty()) *error = loadError;
    }
    return result;
}

std::optional<Project> ProjectStore::loadById(const QString &id, QString *error) const {
    QFile file(qStringFromPath(projectPath(id) / "project.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return std::nullopt;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = parseError.errorString();
        return std::nullopt;
    }
    const auto object = document.object();
    if (object.value(QStringLiteral("formatVersion")).toInt(1) < 4) {
        return migrateLegacyProject(id, object, error);
    }

    auto project = Project::fromJson(object);
    bool changed = project.formatVersion < 5;
    project.formatVersion = 5;
    for (const auto &entry : object.value(QStringLiteral("releaseIds")).toArray()) {
        const auto releaseIdValue = entry.toString();
        if (!validId(releaseIdValue)) continue;
        QFile releaseFile(qStringFromPath(releasePath(id, releaseIdValue) / "release.json"));
        if (!releaseFile.open(QIODevice::ReadOnly)) {
            if (error != nullptr) {
                *error = QStringLiteral("Could not load release %1: %2")
                             .arg(releaseIdValue, releaseFile.errorString());
            }
            return std::nullopt;
        }
        QJsonParseError releaseError;
        const auto releaseDocument = QJsonDocument::fromJson(releaseFile.readAll(), &releaseError);
        if (releaseError.error != QJsonParseError::NoError || !releaseDocument.isObject()) {
            if (error != nullptr) *error = releaseError.errorString();
            return std::nullopt;
        }
        auto release = PackageRelease::fromJson(releaseDocument.object());
        release.projectId = project.id;
        if (release.acquisition.canonicalIdentity.isEmpty()) {
            release.sourceType = SourcePackageType::Debian;
            release.acquisition.kind = release.update.strategy == UpdateStrategy::AptRepository
                ? AcquisitionKind::AptRepository
                : release.update.strategy == UpdateStrategy::RpmRepository
                    ? AcquisitionKind::RpmRepository : AcquisitionKind::LocalFile;
            release.acquisition.canonicalIdentity = sourceIdentity(release.debian);
            release.acquisition.originalUrl = release.sourceUrl;
            changed = true;
        }
        changed = populateAptCandidates(release) || changed;
        changed = migrateScriptEvidence(release) || changed;
        changed = migratePayloadMetadata(release) || changed;
        changed = migrateSigningTrust(release, releasePath(release)) || changed;
        bool mappingsChanged = migrateAppImageIntegration(release);
        mappingsChanged = applyCurrentMappings(release) || mappingsChanged;
        if (release.sourceType == SourcePackageType::Archive &&
            release.installMapping.launchers.isEmpty()) {
            mappingsChanged = SourceAnalyzer::inferArchiveLaunchers(
                                  release.installMapping, release.payload,
                                  release.debian.package.isEmpty() ? release.archPackageName
                                                                   : release.debian.package) ||
                              mappingsChanged;
        }
        if (!release.iconPath.isEmpty() &&
            release.installMapping.icon.projectPath.isEmpty()) {
            release.installMapping.icon.sourceKind = release.iconSourcePath.isEmpty()
                ? IconSourceKind::LocalFile : IconSourceKind::Payload;
            release.installMapping.icon.sourcePath = release.iconSourcePath;
            release.installMapping.icon.projectPath = release.iconPath;
            release.installMapping.icon.sha256 = release.iconSha256;
            release.installMapping.icon.format = QFileInfo(release.iconPath).suffix().toLower();
            release.installMapping.icon.iconName = release.debian.package.isEmpty()
                ? release.archPackageName : release.debian.package;
            release.installMapping.icon.provenance.origin = ValueOrigin::Deterministic;
            release.installMapping.icon.provenance.rationale = QStringLiteral(
                "Migrated the previously selected project icon into the integration recipe");
            mappingsChanged = true;
        }
        changed = mappingsChanged || changed;
        const auto pkgbuild = readPkgbuild(release, nullptr);
        if (pkgbuild) {
            const auto diskHash = sha256Hex(pkgbuild->toUtf8());
            if (diskHash != release.generatedPkgbuildSha256) {
                release.pkgbuildManuallyModified = true;
                if (release.customPkgbuild != *pkgbuild) release.customPkgbuild = *pkgbuild;
            } else if (release.pkgbuildManuallyModified && release.customPkgbuild.isEmpty()) {
                release.customPkgbuild = *pkgbuild;
            }
        }
        const bool generatedRecipeNeedsMigration =
            !release.generatedPkgbuild.contains(QStringLiteral("xdata=(")) ||
            (release.sourceType == SourcePackageType::AppImage &&
             !release.generatedPkgbuild.contains(
                 QStringLiteral("Preserve the complete AppDir below /opt")));
        if (mappingsChanged || project.formatVersion < 5 || generatedRecipeNeedsMigration) {
            release.generatedPkgbuild = PkgbuildGenerator::generate(release);
            release.generatedPkgbuildSha256 = sha256Hex(release.generatedPkgbuild.toUtf8());
            if (!release.pkgbuildManuallyModified &&
                !writeBytes(pkgbuildPath(release), release.generatedPkgbuild.toUtf8(), error)) {
                return std::nullopt;
            }
        }
        project.releases.append(std::move(release));
    }
    changed = reconcileInstalled(project, nullptr) || changed;
    changed = repairSloganDisplayName(project) || changed;
    if (changed && !save(project, error)) return std::nullopt;
    return project;
}

std::optional<Project> ProjectStore::load(const QString &idOrName, QString *error) const {
    if (validId(idOrName) && std::filesystem::exists(projectPath(idOrName) / "project.json")) {
        return loadById(idOrName, error);
    }
    const auto projects = list(error);
    for (const auto &project : projects) {
        if (project.displayName.compare(idOrName, Qt::CaseInsensitive) == 0 ||
            project.archPackageName.compare(idOrName, Qt::CaseInsensitive) == 0) {
            return project;
        }
    }
    if (error != nullptr) *error = QStringLiteral("Project not found: %1").arg(idOrName);
    return std::nullopt;
}

bool ProjectStore::save(Project &project, QString *error) const {
    if (!validId(project.id)) {
        if (error != nullptr) *error = QStringLiteral("Invalid project ID: %1").arg(project.id);
        return false;
    }
    std::error_code filesystemError;
    const auto directory = projectPath(project.id);
    std::filesystem::create_directories(directory / "releases", filesystemError);
    if (filesystemError) {
        if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
        return false;
    }
    project.formatVersion = 5;
    project.modifiedAt = QDateTime::currentDateTimeUtc();
    for (auto &release : project.releases) {
        if (!validId(release.id)) {
            if (error != nullptr) *error = QStringLiteral("Invalid release ID: %1").arg(release.id);
            return false;
        }
        release.projectId = project.id;
        release.formatVersion = std::max(2, release.formatVersion);
        if (release.state != ReleaseState::Discovered && release.state != ReleaseState::Preparing) {
            release.state = release.buildStatus == BuildStatus::Succeeded
                ? ReleaseState::Built
                : releaseNeedsReview(release) ? ReleaseState::NeedsReview : ReleaseState::Ready;
        }
        release.modifiedAt = project.modifiedAt;
        std::filesystem::create_directories(releasePath(release), filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return false;
        }
        if (!synchronizeIntegrationSources(release, error)) return false;
        if (!writeBytes(releasePath(release) / "release.json",
                        QJsonDocument(release.toJson()).toJson(QJsonDocument::Indented), error)) {
            return false;
        }
    }
    return writeBytes(directory / "project.json",
                      QJsonDocument(project.toJson()).toJson(QJsonDocument::Indented), error);
}

std::optional<Project> ProjectStore::migrateLegacyProject(const QString &id,
                                                          const QJsonObject &legacyObject,
                                                          QString *error) const {
    auto release = PackageRelease::fromJson(legacyObject);
    const auto oldInstalled = legacyObject.value(QStringLiteral("installedVersion")).toString();
    release.projectId = id;
    release.id = releaseId(release.debian.version, release.sourceSha256);
    release.formatVersion = 1;

    Project project;
    project.id = id;
    project.displayName = release.displayName;
    project.archPackageName = release.archPackageName;
    project.vendorName = release.vendorName;
    project.sourceIdentity = sourceIdentity(release.debian);
    project.createdAt = release.createdAt;
    project.modifiedAt = release.modifiedAt;
    project.installedVersion = oldInstalled;

    const auto directory = projectPath(id);
    const auto finalDirectory = releasePath(release);
    if (!std::filesystem::exists(finalDirectory / "release.json")) {
        const auto staging = directory / "releases" /
                             pathFromQString(QStringLiteral(".migrate-%1").arg(release.id));
        std::error_code filesystemError;
        std::filesystem::create_directories(staging, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return std::nullopt;
        }
        const auto backup = directory / "project.v3.json.bak";
        if (!std::filesystem::exists(backup) &&
            !writeBytes(backup, QJsonDocument(legacyObject).toJson(QJsonDocument::Indented), error)) {
            return std::nullopt;
        }

        QList<std::pair<std::filesystem::path, std::filesystem::path>> moved;
        auto moveEntry = [&](const std::filesystem::path &source,
                             const std::filesystem::path &target) {
            if (!std::filesystem::exists(source) && !std::filesystem::is_symlink(source)) return true;
            std::error_code moveError;
            std::filesystem::rename(source, target, moveError);
            if (moveError) {
                if (error != nullptr) *error = QString::fromStdString(moveError.message());
                return false;
            }
            moved.append({source, target});
            return true;
        };
        bool moveOk = true;
        for (const auto *name : {"sources", "files", "patches", "build", "history"}) {
            moveOk = moveOk && moveEntry(directory / name, staging / name);
        }
        moveOk = moveOk && moveEntry(directory / "PKGBUILD", staging / "PKGBUILD");
        if (!release.originalSourceFilename.isEmpty()) {
            moveOk = moveOk && moveEntry(directory / pathFromQString(release.originalSourceFilename),
                                         staging / pathFromQString(release.originalSourceFilename));
        }
        if (!release.lifecycleScript.fileName.isEmpty()) {
            moveOk = moveOk && moveEntry(directory / pathFromQString(release.lifecycleScript.fileName),
                                         staging / pathFromQString(release.lifecycleScript.fileName));
        }
        const QDir oldDirectory(qStringFromPath(directory));
        for (const auto &artifact : oldDirectory.entryInfoList(
                 QStringList{QStringLiteral("*.pkg.tar.*")}, QDir::Files)) {
            moveOk = moveOk && moveEntry(pathFromQString(artifact.absoluteFilePath()),
                                         staging / pathFromQString(artifact.fileName()));
        }
        if (!moveOk) {
            for (auto iterator = moved.crbegin(); iterator != moved.crend(); ++iterator) {
                std::error_code rollbackError;
                std::filesystem::rename(iterator->second, iterator->first, rollbackError);
            }
            return std::nullopt;
        }
        for (auto &artifact : release.producedPackages) {
            artifact = qStringFromPath(finalDirectory / pathFromQString(QFileInfo(artifact).fileName()));
        }
        if (!writeBytes(staging / "release.json",
                        QJsonDocument(release.toJson()).toJson(QJsonDocument::Indented), error)) {
            return std::nullopt;
        }
        std::filesystem::rename(staging, finalDirectory, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return std::nullopt;
        }
    } else {
        QFile releaseFile(qStringFromPath(finalDirectory / "release.json"));
        if (releaseFile.open(QIODevice::ReadOnly)) {
            release = PackageRelease::fromJson(
                QJsonDocument::fromJson(releaseFile.readAll()).object());
            release.projectId = id;
        }
    }

    static_cast<void>(populateAptCandidates(release));
    static_cast<void>(migrateScriptEvidence(release));
    static_cast<void>(migratePayloadMetadata(release));
    static_cast<void>(migrateAppImageIntegration(release));
    static_cast<void>(migrateSigningTrust(release, finalDirectory));

    if (!release.iconPath.isEmpty()) {
        project.iconPath = QStringLiteral("releases/%1/%2").arg(release.id, release.iconPath);
        project.iconSourcePath = release.iconSourcePath;
        project.iconSha256 = release.iconSha256;
    }
    project.releases.append(release);
    project.history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("migration"),
                            QStringLiteral("Migrated the original package into release %1")
                                .arg(release.debian.version)});
    static_cast<void>(reconcileInstalled(project, nullptr));
    if (!save(project, error)) return std::nullopt;
    return project;
}

bool ProjectStore::reconcileInstalled(Project &project, QString *error) const {
    QString queryError;
    const auto installed = queryInstalledVersion(project.archPackageName, &queryError);
    if (!installed) {
        if (error != nullptr) *error = queryError;
        return false;
    }
    const auto previousVersion = project.installedVersion;
    const auto previousRelease = project.installedReleaseId;
    const auto previousExternal = project.externallyInstalled;
    project.installedVersion = *installed;
    project.installedReleaseId.clear();
    project.externallyInstalled = false;
    if (!installed->isEmpty()) {
        bool xdataOwned = false;
        if (const auto managed = ManagedPackageRegistry::find(project.archPackageName, nullptr);
            managed && !managed->projectId().isEmpty()) {
            if (managed->projectId() == project.id) {
                xdataOwned = true;
                if (project.release(managed->releaseId()) != nullptr) {
                    project.installedReleaseId = managed->releaseId();
                }
            }
        } else {
            for (const auto &release : project.releases) {
                if (!project.installedReleaseId.isEmpty()) break;
                bool matches = expectedArchVersion(release) == *installed;
                for (const auto &build : release.builds) {
                    matches = matches || std::any_of(build.artifacts.cbegin(), build.artifacts.cend(),
                                                      [&](const auto &artifact) {
                                                          return artifact.packageName == project.archPackageName &&
                                                                 artifact.packageVersion == *installed;
                                                      });
                }
                if (matches) {
                    project.installedReleaseId = release.id;
                    break;
                }
            }
        }
        project.externallyInstalled = project.installedReleaseId.isEmpty() && !xdataOwned;
    }
    return previousVersion != project.installedVersion ||
           previousRelease != project.installedReleaseId ||
           previousExternal != project.externallyInstalled;
}

bool ProjectStore::deleteProject(const Project &project, QString *error) const {
    QString statusError;
    const auto installed = queryInstalledVersion(project.archPackageName, &statusError);
    if (!installed) {
        if (error != nullptr) *error = QStringLiteral("Could not verify package state: %1").arg(statusError);
        return false;
    }
    if (!installed->isEmpty() && projectOwnsInstalledPackage(project, *installed)) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 is installed (%2). Uninstall it before deleting this project.")
                         .arg(project.archPackageName, *installed);
        }
        return false;
    }
    const auto directory = projectPath(project.id);
    std::error_code filesystemError;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root_, filesystemError);
    const auto canonicalParent = std::filesystem::weakly_canonical(directory.parent_path(), filesystemError);
    if (filesystemError || canonicalParent != canonicalRoot ||
        std::filesystem::is_symlink(std::filesystem::symlink_status(directory, filesystemError))) {
        if (error != nullptr) *error = QStringLiteral("Unsafe project deletion target");
        return false;
    }
    const auto removed = std::filesystem::remove_all(directory, filesystemError);
    if (filesystemError || removed == 0) {
        if (error != nullptr) *error = filesystemError
            ? QString::fromStdString(filesystemError.message())
            : QStringLiteral("No project files were removed");
        return false;
    }
    return true;
}

bool ProjectStore::deleteRelease(Project &project, const QString &releaseIdValue,
                                 QString *error) const {
    if (releaseIdValue == project.installedReleaseId) {
        if (error != nullptr) *error = QStringLiteral("The installed release cannot be deleted");
        return false;
    }
    const auto iterator = std::find_if(project.releases.begin(), project.releases.end(),
                                       [&](const auto &release) {
                                           return release.id == releaseIdValue;
                                       });
    if (iterator == project.releases.end()) {
        if (error != nullptr) *error = QStringLiteral("Release not found: %1").arg(releaseIdValue);
        return false;
    }
    const auto directory = releasePath(*iterator);
    const auto tombstone = directory.parent_path() /
                           pathFromQString(QStringLiteral(".delete-%1").arg(releaseIdValue));
    std::error_code filesystemError;
    std::filesystem::rename(directory, tombstone, filesystemError);
    if (filesystemError) {
        if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
        return false;
    }
    const auto removedRelease = *iterator;
    project.releases.erase(iterator);
    if (!save(project, error)) {
        project.releases.append(removedRelease);
        filesystemError.clear();
        std::filesystem::rename(tombstone, directory, filesystemError);
        return false;
    }
    std::filesystem::remove_all(tombstone, filesystemError);
    if (filesystemError && error != nullptr) *error = QString::fromStdString(filesystemError.message());
    return !filesystemError;
}
PackageRelease *ProjectStore::recordDiscoveredRelease(
    Project &project, const PackageRelease &tracker, const QString &version,
    const QString &filename, const QString &sha256, const QString &downloadUrl,
    QString *error, const qint64 providerReleaseId, const qint64 providerAssetId,
    const QString &providerTag, const QString &publisherDigest,
    const bool providerPrerelease) const {
    if (version.isEmpty() || filename.isEmpty() || downloadUrl.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("The update result is missing version, filename, or download URL");
        return nullptr;
    }
    const auto trackerCopy = tracker;
    const auto existing = std::find_if(project.releases.begin(), project.releases.end(),
                                       [&](const auto &candidate) {
                                           return (!sha256.isEmpty() && candidate.sourceSha256 == sha256) ||
                                                  (providerAssetId > 0 &&
                                                   candidate.acquisition.githubAssetId == providerAssetId) ||
                                                  (candidate.debian.version == version &&
                                                   candidate.sourceUrl == downloadUrl);
                                       });
    if (existing != project.releases.end()) {
        // The caller may have just updated the tracking release's last-check result.
        // Persist that state even when this vendor release was already discovered.
        if (!save(project, error)) return nullptr;
        return &*existing;
    }
    PackageRelease release;
    release.projectId = project.id;
    const auto identityHash = !sha256.isEmpty()
        ? sha256 : sha256Hex(QStringLiteral("%1:%2:%3").arg(providerReleaseId).arg(providerAssetId).arg(downloadUrl).toUtf8());
    release.id = releaseId(version, identityHash);
    release.displayName = project.displayName;
    release.archPackageName = project.archPackageName;
    release.vendorName = project.vendorName;
    release.originalSourceFilename = filename;
    release.sourceUrl = downloadUrl;
    release.sourceSha256 = sha256;
    // GitHub can change artifact formats between releases. Do not claim a type until the
    // downloaded bytes have passed content-based detection and static analysis.
    release.sourceType = SourcePackageType::Unknown;
    release.acquisition = trackerCopy.acquisition;
    release.acquisition.originalUrl = downloadUrl;
    if (trackerCopy.update.strategy == UpdateStrategy::AptRepository) {
        release.acquisition.kind = AcquisitionKind::AptRepository;
    } else if (trackerCopy.update.strategy == UpdateStrategy::RpmRepository) {
        release.acquisition.kind = AcquisitionKind::RpmRepository;
    }
    if (trackerCopy.update.strategy == UpdateStrategy::GitHubRelease) {
        release.acquisition.kind = AcquisitionKind::GitHubRelease;
        release.acquisition.githubOwner = trackerCopy.update.githubOwner;
        release.acquisition.githubRepository = trackerCopy.update.githubRepository;
        release.acquisition.githubReleaseId = providerReleaseId;
        release.acquisition.githubAssetId = providerAssetId;
        release.acquisition.githubTag = providerTag;
        release.acquisition.githubPrerelease = providerPrerelease;
        release.acquisition.githubAssetName = filename;
        release.acquisition.publisherDigest = publisherDigest;
        release.acquisition.canonicalIdentity = QStringLiteral("github:%1/%2")
            .arg(trackerCopy.update.githubOwner.toLower(),
                 trackerCopy.update.githubRepository.toLower());
    }
    release.debian.package = trackerCopy.debian.package;
    release.debian.version = version;
    release.debian.architecture = trackerCopy.debian.architecture;
    release.update = trackerCopy.update;
    release.update.githubReleaseId = providerReleaseId;
    release.update.githubAssetId = providerAssetId;
    release.update.githubTag = providerTag;
    release.update.githubPublisherDigest = publisherDigest;
    release.state = ReleaseState::Discovered;
    release.createdAt = QDateTime::currentDateTimeUtc();
    release.modifiedAt = release.createdAt;
    const auto discoveryMessage = (trackerCopy.update.strategy == UpdateStrategy::AptRepository ||
                                   trackerCopy.update.strategy == UpdateStrategy::RpmRepository)
        ? QStringLiteral("Discovered signature-verified vendor release %1").arg(version)
        : !publisherDigest.isEmpty()
            ? QStringLiteral("Discovered GitHub release %1 with publisher digest").arg(version)
            : QStringLiteral("Discovered GitHub release %1 without a publisher digest; downloaded bytes still require a local SHA256")
                  .arg(version);
    release.history.append({release.createdAt, QStringLiteral("discovered"), discoveryMessage});
    project.history.append({release.createdAt, QStringLiteral("release-discovered"),
                            discoveryMessage});
    project.releases.append(release);
    if (!save(project, error)) {
        project.releases.removeLast();
        return nullptr;
    }
    return project.release(release.id);
}

bool ProjectStore::savePkgbuild(Project &project, PackageRelease &release,
                                const QString &contents, QString *error) const {
    if (sha256Hex(contents.toUtf8()) == release.generatedPkgbuildSha256) {
        return activateGuidedPkgbuild(project, release, error);
    }
    return saveCustomPkgbuild(project, release, contents, error);
}

bool ProjectStore::activateGuidedPkgbuild(Project &project, PackageRelease &release,
                                         QString *error) const {
    if (!writeBytes(pkgbuildPath(release), release.generatedPkgbuild.toUtf8(), error)) return false;
    release.pkgbuildManuallyModified = false;
    return save(project, error);
}

bool ProjectStore::activateCustomPkgbuild(Project &project, PackageRelease &release,
                                         QString *error) const {
    if (release.customPkgbuild.isEmpty()) release.customPkgbuild = release.generatedPkgbuild;
    if (!writeBytes(pkgbuildPath(release), release.customPkgbuild.toUtf8(), error)) return false;
    release.pkgbuildManuallyModified = true;
    return save(project, error);
}

bool ProjectStore::saveCustomPkgbuild(Project &project, PackageRelease &release,
                                      const QString &contents, QString *error) const {
    release.customPkgbuild = contents;
    release.pkgbuildManuallyModified = true;
    if (!writeBytes(pkgbuildPath(release), contents.toUtf8(), error)) return false;
    return save(project, error);
}

bool ProjectStore::saveLifecycle(Project &project, PackageRelease &release,
                                 QString *error) const {
    static const QRegularExpression safeName(QStringLiteral("^[a-z0-9@._+\\-]+\\.install$"));
    if (release.lifecycleScript.contents.isEmpty()) return true;
    if (!safeName.match(release.lifecycleScript.fileName).hasMatch()) {
        if (error != nullptr) *error = QStringLiteral("Unsafe Arch lifecycle filename");
        return false;
    }
    const auto validation = LifecycleValidator::validate(release.lifecycleScript.contents);
    release.lifecycleScript.validationPassed = validation.passed;
    release.lifecycleScript.validationMessage = validation.message();
    release.lifecycleScript.manuallyModified = false;
    release.buildStatus = BuildStatus::NeverBuilt;
    release.producedPackages.clear();
    if (!writeBytes(lifecyclePath(release), release.lifecycleScript.contents.toUtf8(), error)) {
        return false;
    }
    return save(project, error);
}

bool ProjectStore::removeLifecycle(Project &project, PackageRelease &release,
                                   QString *error) const {
    if (!release.lifecycleScript.fileName.isEmpty()) {
        const auto path = lifecyclePath(release);
        if (QFileInfo::exists(qStringFromPath(path)) && !QFile::remove(qStringFromPath(path))) {
            if (error != nullptr) *error = QStringLiteral("Could not remove lifecycle file");
            return false;
        }
    }
    release.lifecycleScript = {};
    release.buildStatus = BuildStatus::NeverBuilt;
    release.producedPackages.clear();
    return save(project, error);
}

bool ProjectStore::synchronizeLifecycle(Project &project, PackageRelease &release,
                                        bool *changed, QString *error) const {
    if (changed != nullptr) *changed = false;
    if (release.lifecycleScript.contents.isEmpty()) return true;
    QFile file(qStringFromPath(lifecyclePath(release)));
    bool missing = !file.open(QIODevice::ReadOnly);
    auto contents = missing ? QString{} : QString::fromUtf8(file.read(128 * 1024 + 1));
    bool wasChanged = false;
    if (missing) {
        // Release imports before format v5 could carry the lifecycle metadata and
        // generate install=... without copying the exact file. Restore only those
        // already-recorded bytes; package content is never executed here.
        if (!writeImportedLifecycle(releasePath(release), release, error)) return false;
        missing = false;
        contents = release.lifecycleScript.contents;
        release.history.append(
            {QDateTime::currentDateTimeUtc(), QStringLiteral("lifecycle-restored"),
             QStringLiteral("Restored the project-local lifecycle file from its exact recorded content")});
        wasChanged = true;
    }
    if (!missing && contents != release.lifecycleScript.contents) {
        release.lifecycleScript.contents = contents;
        release.lifecycleScript.manuallyModified = true;
        release.lifecycleScript.acknowledgedFingerprint.clear();
        release.lifecycleScript.provenance = {
            ValueOrigin::User, {}, {}, release.sourceSha256,
            QStringLiteral("Edited directly in the project release directory"),
            QDateTime::currentDateTimeUtc(), true};
        wasChanged = true;
    }
    const auto validation = missing
        ? LifecycleValidation{false, {QStringLiteral("The project-local lifecycle file is missing")}}
        : LifecycleValidator::validate(release.lifecycleScript.contents);
    if (release.lifecycleScript.validationPassed != validation.passed ||
        release.lifecycleScript.validationMessage != validation.message()) {
        release.lifecycleScript.validationPassed = validation.passed;
        release.lifecycleScript.validationMessage = validation.message();
        wasChanged = true;
    }
    if (wasChanged) {
        release.buildStatus = BuildStatus::NeverBuilt;
        release.producedPackages.clear();
        if (!save(project, error)) return false;
    }
    if (changed != nullptr) *changed = wasChanged;
    return true;
}

std::optional<QString> ProjectStore::readPkgbuild(const PackageRelease &release,
                                                  QString *error) const {
    QFile file(qStringFromPath(pkgbuildPath(release)));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return std::nullopt;
    }
    return QString::fromUtf8(file.readAll());
}

CleanupResult ProjectStore::cleanup(Project &project, const RetentionPolicy &policy,
                                    QString *error) const {
    CleanupResult result;
    static_cast<void>(reconcileInstalled(project, nullptr));
    const auto *installed = project.installedRelease();
    const bool haveInstalled = installed != nullptr && !project.externallyInstalled &&
                               !project.installedVersion.isEmpty();
    if (haveInstalled) {
        QList<QString> olderIds;
        for (const auto &release : project.releases) {
            if (release.id != installed->id &&
                compareReleaseVersions(release, *installed) < 0) {
                olderIds.append(release.id);
            }
        }
        std::sort(olderIds.begin(), olderIds.end(), [&](const auto &leftId, const auto &rightId) {
            const auto *left = project.release(leftId);
            const auto *right = project.release(rightId);
            return compareReleaseVersions(*left, *right) > 0;
        });

        if (policy.packageVersions >= 0) {
            int artifactReleaseIndex = 0;
            for (const auto &id : olderIds) {
                auto *release = project.release(id);
                if (release == nullptr || release->producedPackages.isEmpty()) continue;
                if (artifactReleaseIndex++ < policy.packageVersions) continue;
                const auto base = std::filesystem::weakly_canonical(releasePath(*release));
                for (const auto &package : std::as_const(release->producedPackages)) {
                    std::error_code pathError;
                    const auto path = std::filesystem::weakly_canonical(pathFromQString(package), pathError);
                    if (!pathError && path.parent_path() == base && QFile::remove(package)) {
                        result.removedArtifacts.append(package);
                    }
                }
                release->producedPackages.clear();
                for (auto &build : release->builds) build.artifacts.clear();
            }
        }

        const auto completeLimit = policy.packageVersions < 0 || policy.completeReleases < 0
                                       ? -1
                                       : std::max(policy.completeReleases, policy.packageVersions);
        if (completeLimit >= 0 && olderIds.size() > completeLimit) {
            const auto removeIds = olderIds.sliced(completeLimit);
            for (const auto &id : removeIds) {
                if (!deleteRelease(project, id, error)) return result;
                result.removedReleases.append(id);
            }
        } else if (!save(project, error)) {
            return result;
        }
    } else {
        result.skipped = true;
        result.message = QStringLiteral("Cleanup skipped because no known PacSmith release is installed");
    }

    if (policy.dropUnbuiltIntermediateUpdates) {
        const auto *newest = project.newestRelease();
        const bool hasAnchor = newest != nullptr &&
            std::any_of(project.releases.cbegin(), project.releases.cend(),
                        [&](const auto &release) {
                            return release.id == project.installedReleaseId || releaseWasBuilt(release);
                        });
        if (hasAnchor && newest != nullptr) {
            const auto newestId = newest->id;
            const auto installedId = project.installedReleaseId;
            QStringList dropIds;
            for (const auto &release : project.releases) {
                if (release.id == newestId || release.id == installedId) continue;
                if (release.state == ReleaseState::Preparing || releaseWasBuilt(release)) continue;
                dropIds.append(release.id);
            }
            for (const auto &id : dropIds) {
                if (!deleteRelease(project, id, error)) return result;
                result.removedReleases.append(id);
            }
            if (!dropIds.isEmpty()) result.skipped = false;
        }
    }

    if (!result.skipped || !result.removedReleases.isEmpty() || !result.removedArtifacts.isEmpty()) {
        result.message = QStringLiteral("Removed %1 package artifact(s) and %2 complete release(s)")
                             .arg(result.removedArtifacts.size())
                             .arg(result.removedReleases.size());
    }
    return result;
}

std::optional<QString> ProjectStore::queryInstalledVersion(const QString &archPackageName,
                                                           QString *error) {
    if (archPackageName.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("The project has no Arch package name");
        return std::nullopt;
    }
    QProcess process;
    process.setProgram(QStringLiteral("/usr/bin/pacman"));
    process.setArguments({QStringLiteral("-Q"), QStringLiteral("--"), archPackageName});
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.start();
    if (!process.waitForStarted(3000)) {
        if (error != nullptr) *error = QStringLiteral("Could not start pacman: %1").arg(process.errorString());
        return std::nullopt;
    }
    if (!process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished();
        if (error != nullptr) *error = QStringLiteral("Timed out while asking pacman for installation status");
        return std::nullopt;
    }
    const auto output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    const auto errorOutput = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
        const auto prefix = archPackageName + QLatin1Char(' ');
        if (!output.startsWith(prefix)) {
            if (error != nullptr) *error = QStringLiteral("pacman returned an unexpected package status");
            return std::nullopt;
        }
        return output.mid(prefix.size()).trimmed();
    }
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 1 &&
        errorOutput.contains(QStringLiteral("was not found"))) {
        return QString{};
    }
    if (error != nullptr) {
        *error = errorOutput.isEmpty() ? QStringLiteral("pacman could not determine installation status")
                                      : errorOutput;
    }
    return std::nullopt;
}

QString sha256Hex(const QByteArray &contents) {
    return QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
}

QString sha256File(const std::filesystem::path &path, QString *error) {
    QFile file(qStringFromPath(path));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        if (error != nullptr) *error = file.errorString();
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace pacsmith
