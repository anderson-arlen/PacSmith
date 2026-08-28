#include "core/project_store/project_store.hpp"
#include "core/project_store/internal.hpp"

#include "core/deb_analyzer.hpp"
#include "core/path_safety.hpp"
#include "core/pkgbuild_generator.hpp"
#include "core/source_analyzer.hpp"

#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>

namespace pacsmith {
using namespace project_store_internal;

namespace {

bool verifyExpectedSha256(const ImportOptions &options, const QString &actual,
                          QString *error) {
    const auto expected = options.expectedSha256.trimmed().toLower();
    if (expected.isEmpty()) return true;
    static const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
    if (!sha256.match(expected).hasMatch()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "Expected SHA256 must contain 64 hexadecimal characters");
        }
        return false;
    }
    if (expected != actual.toLower()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "Publisher SHA256 mismatch: expected %1, downloaded %2")
                         .arg(expected, actual.toLower());
        }
        return false;
    }
    return true;
}

void applyVerifiedPublisherDigest(SourceAcquisition &acquisition,
                                  const ImportOptions &options) {
    const auto expected = options.expectedSha256.trimmed().toLower();
    if (expected.isEmpty()) return;
    acquisition.publisherDigest = expected;
    acquisition.publisherVerified = true;
}

} // namespace

std::optional<ImportResult> ProjectStore::importDeb(const std::filesystem::path &debPath,
                                                    QString *error,
                                                    const ImportProgressCallback &progress,
                                                    const ImportOptions &options) const {
    if (progress) progress({ImportStage::ValidatingSource, 0});
    AnalysisError analysisError;
    auto analysis = DebAnalyzer{}.analyze(debPath, analysisError, progress);
    if (!analysis) {
        if (error != nullptr) *error = analysisError.message;
        return std::nullopt;
    }
    if (!options.packageName.isEmpty()) {
        analysis->metadata.package = options.packageName.trimmed();
    }
    if (!options.version.isEmpty()) analysis->metadata.version = options.version.trimmed();
    if (!options.architecture.isEmpty()) {
        analysis->metadata.architecture = options.architecture.trimmed();
    }
    if (!options.description.isEmpty()) {
        analysis->metadata.description = options.description.trimmed();
    }
    const auto sourceHash = sha256File(debPath, error);
    if (sourceHash.isEmpty() || !verifyExpectedSha256(options, sourceHash, error)) {
        return std::nullopt;
    }

    if (progress) progress({ImportStage::PreparingProject, 0});
    const auto identity = requestedSourceIdentity(options, &analysis->metadata);
    std::optional<Project> matching;
    if (!options.existingProjectId.isEmpty()) {
        matching = loadById(options.existingProjectId, error);
        if (!matching) {
            if (error != nullptr && error->isEmpty()) {
                *error = QStringLiteral("Could not load project %1 for import")
                             .arg(options.existingProjectId);
            }
            return std::nullopt;
        }
    } else {
        matching = findMatchingProject(list(), options, &analysis->metadata);
    }
    bool projectCreated = !matching.has_value();
    Project project;
    if (matching) {
        project = std::move(*matching);
        adoptCanonicalIdentity(project, identity);
        for (const auto &existing : project.releases) {
            if (existing.sourceSha256 == sourceHash) {
                if (existing.state != ReleaseState::Discovered) {
                    return ImportResult{std::move(project), existing.id, false, true, {}};
                }
                break;
            }
        }
    } else {
        project.archPackageName =
            PkgbuildGenerator::sanitizePackageName(analysis->metadata.package) + QStringLiteral("-bin");
        project.id = PkgbuildGenerator::sanitizePackageName(analysis->metadata.package);
        if (project.id.isEmpty()) project.id = QStringLiteral("vendor-package");
        auto candidate = project.id;
        int suffix = 2;
        while (std::filesystem::exists(projectPath(candidate))) {
            candidate = project.id + QLatin1Char('-') + QString::number(suffix++);
        }
        project.id = candidate;
        project.displayName = sourceDisplayName(analysis->metadata, analysis->installMapping.desktopEntries);
        project.vendorName = analysis->metadata.maintainer;
        project.sourceIdentity = identity;
        project.createdAt = QDateTime::currentDateTimeUtc();
        project.modifiedAt = project.createdAt;
    }

    PackageRelease release;
    release.projectId = project.id;
    release.id = releaseId(analysis->metadata.version, sourceHash);
    release.displayName = project.displayName;
    release.archPackageName = project.archPackageName;
    release.sourceType = SourcePackageType::Debian;
    release.installMapping = analysis->installMapping;
    release.acquisition = options.acquisition;
    applyVerifiedPublisherDigest(release.acquisition, options);
    if (release.acquisition.canonicalIdentity.isEmpty()) {
        release.acquisition.kind = AcquisitionKind::LocalFile;
        release.acquisition.canonicalIdentity = sourceIdentity(analysis->metadata);
    } else {
        release.acquisition.canonicalIdentity =
            normalizedSourceIdentity(release.acquisition.canonicalIdentity);
    }
    release.originalSourceFilename = QFileInfo(qStringFromPath(debPath)).fileName();
    release.sourceSha256 = sourceHash;
    release.vendorName = analysis->metadata.maintainer;
    release.debian = analysis->metadata;
    release.packageMetadata.description = analysis->metadata.description;
    release.packageMetadata.homepage = analysis->metadata.homepage;
    release.packageMetadata.licenses = {QStringLiteral("custom:vendor")};
    release.dependencies = analysis->dependencies;
    release.maintainerScripts = analysis->maintainerScripts;
    release.scriptFindings = analysis->scriptFindings;
    release.payload = analysis->payload;
    release.payloadRules = analysis->payloadRules;
    release.update.detectedCandidates = analysis->updateCandidates;
    release.update.aptCandidates = analysis->aptCandidates;
    release.update.rpmCandidates = analysis->rpmCandidates;
    release.update.aptPackageName = release.debian.package;
    release.update.aptArchitecture = release.debian.architecture;
    applyInitialUpdateConfiguration(release, options);
    release.createdAt = QDateTime::currentDateTimeUtc();
    release.modifiedAt = release.createdAt;
    for (const auto &existing : project.releases) {
        if (compareReleaseVersions(existing, release) == 0) {
            release.archPkgrel = std::max(release.archPkgrel, existing.archPkgrel + 1);
        }
    }

    const auto discovered = std::find_if(project.releases.begin(), project.releases.end(),
                                         [&](const auto &candidate) {
                                             return candidate.state == ReleaseState::Discovered &&
                                                    (candidate.sourceSha256 == sourceHash ||
                                                     (release.acquisition.githubAssetId > 0 &&
                                                      candidate.acquisition.githubAssetId ==
                                                          release.acquisition.githubAssetId));
                                         });
    if (discovered != project.releases.end()) release.id = discovered->id;
    const auto *previous = newestPreparedRelease(project);
    if (previous != nullptr) {
        carryForward(*previous, release,
                     previous->pkgbuildManuallyModified
                         ? readPkgbuild(*previous, nullptr).value_or(QString{})
                         : QString{});
    }

    if (options.initialUpdate) {
        // The repository was already selected and cryptographically verified
        // before this artifact was downloaded. Package-contained hints remain
        // visible as candidates but do not replace that explicit configuration.
    } else if (!release.update.aptCandidates.isEmpty()) {
        const auto &candidate = release.update.aptCandidates.first();
        release.update.url = candidate.uri;
        release.update.aptSuite = candidate.suite;
        if (!candidate.components.isEmpty()) release.update.aptComponent = candidate.components.first();
        if (!candidate.architectures.isEmpty()) {
            release.update.aptArchitecture = candidate.architectures.first();
        }
        release.update.strategy = UpdateStrategy::AptRepository;
    } else if (!release.update.rpmCandidates.isEmpty()) {
        static_cast<void>(populateRpmCandidates(release));
    }
    if (release.acquisition.kind == AcquisitionKind::GitHubRelease) {
        release.update.strategy = UpdateStrategy::GitHubRelease;
        release.update.url = release.acquisition.originalUrl;
        release.update.githubOwner = release.acquisition.githubOwner;
        release.update.githubRepository = release.acquisition.githubRepository;
        release.update.githubReleaseId = release.acquisition.githubReleaseId;
        release.update.githubAssetId = release.acquisition.githubAssetId;
        release.update.githubTag = release.acquisition.githubTag;
        release.update.githubPublisherDigest = release.acquisition.publisherDigest;
    }
    if (!options.initialUpdate) {
        if (previous != nullptr) inheritUpdateConfiguration(*previous, release);
        else if (discovered != project.releases.end()) {
            inheritUpdateConfiguration(*discovered, release);
        }
    }

    const auto directory = releasePath(release);
    std::error_code filesystemError;
    for (const auto *subdirectory : {"sources", "files", "patches", "build", "history"}) {
        std::filesystem::create_directories(directory / subdirectory, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return std::nullopt;
        }
    }
    if (!writeImportedLifecycle(directory, release, error)) return std::nullopt;
    if (!materializeIntegrationIcon(
            *this, project, release, debPath,
            analysis->icon ? analysis->icon->sourcePath : QString{},
            analysis->icon ? analysis->icon->contents : QByteArray{}, error)) {
        return std::nullopt;
    }
    if (!storeImportSigningKeys(directory, analysis->signingKeys, options,
                                release.update, error)) return std::nullopt;
    if (previous != nullptr &&
        !copyInheritedSigningKeys(releasePath(*previous), directory, release.update, error)) {
        return std::nullopt;
    }

    if (progress) progress({ImportStage::CopyingSource, 0});
    if (!copyFileAtomically(debPath, sourcePath(release), error)) return std::nullopt;
    const auto sourceLink = directory / pathFromQString(release.originalSourceFilename);
    std::filesystem::create_symlink(
        std::filesystem::path("sources") / pathFromQString(release.originalSourceFilename),
        sourceLink, filesystemError);
    if (filesystemError) {
        filesystemError.clear();
        if (!copyFileAtomically(sourcePath(release), sourceLink, error)) return std::nullopt;
    }
    if (progress) progress({ImportStage::GeneratingPkgbuild, 0});
    release.generatedPkgbuild = PkgbuildGenerator::generate(release);
    release.generatedPkgbuildSha256 = sha256Hex(release.generatedPkgbuild.toUtf8());
    if (!writePkgbuildContents(release, release.pkgbuildManuallyModified
                                           ? release.customPkgbuild
                                           : release.generatedPkgbuild, error)) {
        return std::nullopt;
    }
    release.history.append({release.createdAt, QStringLiteral("created"),
                            QStringLiteral("Imported %1 (%2)")
                                .arg(release.originalSourceFilename, release.debian.version)});
    project.history.append({release.createdAt, QStringLiteral("release-imported"),
                            QStringLiteral("Imported vendor release %1").arg(release.debian.version)});
    if (discovered != project.releases.end()) {
        const auto index = std::distance(project.releases.begin(), discovered);
        project.releases[static_cast<qsizetype>(index)] = release;
    } else {
        project.releases.append(release);
    }
    if (progress) progress({ImportStage::SavingProject, 0});
    if (!save(project, error)) return std::nullopt;
    return ImportResult{std::move(project), release.id, projectCreated, false, {}};
}

std::optional<ImportResult> ProjectStore::importSource(
    const std::filesystem::path &inputPath, const ImportOptions &options, QString *error,
    const ImportProgressCallback &progress) const {
    if (progress) progress({ImportStage::ValidatingSource, 0});
    const auto detectedType = SourceAnalyzer::detect(inputPath, error);
    if (!detectedType) return std::nullopt;
    if (*detectedType == SourcePackageType::Debian) {
        auto imported = importDeb(inputPath, error, progress, options);
        if (!imported) return std::nullopt;
        auto *release = imported->project.release(imported->releaseId);
        if (release != nullptr) {
            release->sourceType = SourcePackageType::Debian;
            release->acquisition = options.acquisition;
            applyVerifiedPublisherDigest(release->acquisition, options);
            if (release->acquisition.canonicalIdentity.isEmpty()) {
                release->acquisition.kind = AcquisitionKind::LocalFile;
                release->acquisition.canonicalIdentity = sourceIdentity(release->debian);
            } else {
                release->acquisition.canonicalIdentity =
                    normalizedSourceIdentity(release->acquisition.canonicalIdentity);
            }
            release->sourceUrl = release->acquisition.originalUrl;
            if (release->acquisition.kind == AcquisitionKind::GitHubRelease &&
                (release->update.strategy == UpdateStrategy::Manual ||
                 release->update.strategy == UpdateStrategy::GitHubRelease)) {
                release->update.strategy = UpdateStrategy::GitHubRelease;
                release->update.githubOwner = release->acquisition.githubOwner;
                release->update.githubRepository = release->acquisition.githubRepository;
                release->update.githubReleaseId = release->acquisition.githubReleaseId;
                release->update.githubAssetId = release->acquisition.githubAssetId;
                release->update.githubTag = release->acquisition.githubTag;
                release->update.githubPublisherDigest = release->acquisition.publisherDigest;
                release->update.githubAssetRegex = options.githubAssetRegex;
                release->update.githubIncludePrereleases = options.githubIncludePrereleases;
            }
            imported->project.formatVersion = 5;
            if (!save(imported->project, error)) return std::nullopt;
        }
        return imported;
    }

    auto analysis = SourceAnalyzer::analyze(inputPath, error, progress);
    if (!analysis) return std::nullopt;
    if (!options.packageName.isEmpty()) analysis->metadata.package = options.packageName;
    if (!options.version.isEmpty()) analysis->metadata.version = options.version;
    if (!options.architecture.isEmpty()) analysis->metadata.architecture = options.architecture;
    if (!options.description.isEmpty()) analysis->metadata.description = options.description;
    if (analysis->metadata.package.isEmpty() || analysis->metadata.version.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Package name and version are required");
        return std::nullopt;
    }
    const auto sourceHash = sha256File(inputPath, error);
    if (sourceHash.isEmpty() || !verifyExpectedSha256(options, sourceHash, error)) {
        return std::nullopt;
    }
    auto acquisition = options.acquisition;
    applyVerifiedPublisherDigest(acquisition, options);
    if (acquisition.canonicalIdentity.isEmpty()) {
        acquisition.kind = AcquisitionKind::LocalFile;
        acquisition.canonicalIdentity = QStringLiteral("%1:%2:%3")
            .arg(sourcePackageTypeName(analysis->type), analysis->metadata.package.toLower(),
                 analysis->metadata.architecture.toLower());
    }
    const auto identity = requestedSourceIdentity(options, &analysis->metadata);
    std::optional<Project> matching;
    if (!options.existingProjectId.isEmpty()) {
        matching = loadById(options.existingProjectId, error);
        if (!matching) {
            if (error != nullptr && error->isEmpty()) {
                *error = QStringLiteral("Could not load project %1 for import")
                             .arg(options.existingProjectId);
            }
            return std::nullopt;
        }
    } else {
        matching = findMatchingProject(list(), options, &analysis->metadata);
    }
    Project project;
    const bool projectCreated = !matching.has_value();
    if (matching) {
        project = std::move(*matching);
        adoptCanonicalIdentity(project, identity);
        for (auto &existing : project.releases) {
            if (existing.sourceSha256 == sourceHash) {
                // Re-importing an already analyzed artifact is also a safe way to
                // backfill metadata learned by newer PacSmith analyzers. Keep the
                // immutable release and source bytes, but persist a newly detected
                // icon when older imports did not have one.
                if (analysis->icon && existing.iconPath.isEmpty()) {
                    const auto suffix = QFileInfo(analysis->icon->sourcePath).suffix().toLower();
                    const auto directory = releasePath(existing);
                    std::error_code filesystemError;
                    std::filesystem::create_directories(directory / "files", filesystemError);
                    if (filesystemError) {
                        if (error != nullptr) {
                            *error = QString::fromStdString(filesystemError.message());
                        }
                        return std::nullopt;
                    }
                    existing.iconPath = QStringLiteral("files/icon.%1").arg(suffix);
                    existing.iconSourcePath = analysis->icon->sourcePath;
                    existing.iconSha256 = sha256Hex(analysis->icon->contents);
                    existing.installMapping.icon.sourceKind = IconSourceKind::Payload;
                    existing.installMapping.icon.sourcePath = existing.iconSourcePath;
                    existing.installMapping.icon.projectPath = existing.iconPath;
                    existing.installMapping.icon.sha256 = existing.iconSha256;
                    existing.installMapping.icon.format = suffix;
                    existing.installMapping.icon.iconName = existing.debian.package.isEmpty()
                        ? existing.archPackageName : existing.debian.package;
                    if (!writeBytes(directory / pathFromQString(existing.iconPath),
                                    analysis->icon->contents, error)) {
                        return std::nullopt;
                    }
                    project.iconPath = QStringLiteral("releases/%1/%2")
                                           .arg(existing.id, existing.iconPath);
                    project.iconSourcePath = existing.iconSourcePath;
                    project.iconSha256 = existing.iconSha256;
                    if (!existing.pkgbuildManuallyModified) {
                        existing.generatedPkgbuild = PkgbuildGenerator::generate(existing);
                        existing.generatedPkgbuildSha256 =
                            sha256Hex(existing.generatedPkgbuild.toUtf8());
                        if (!writePkgbuildContents(existing, existing.generatedPkgbuild, error)) {
                            return std::nullopt;
                        }
                    } else if (!writeIdentityVariables(existing, error)) {
                        return std::nullopt;
                    }
                    if (!save(project, error)) return std::nullopt;
                }
                return ImportResult{std::move(project), existing.id, false, true, {}};
            }
        }
    } else {
        project.formatVersion = 5;
        const auto baseName = PkgbuildGenerator::sanitizePackageName(analysis->metadata.package);
        project.archPackageName = analysis->type == SourcePackageType::ArchPackage
            ? baseName : baseName + QStringLiteral("-bin");
        project.id = baseName.isEmpty() ? QStringLiteral("vendor-package") : baseName;
        auto candidate = project.id;
        int suffix = 2;
        while (std::filesystem::exists(projectPath(candidate))) {
            candidate = project.id + QLatin1Char('-') + QString::number(suffix++);
        }
        project.id = candidate;
        project.displayName = !options.displayName.isEmpty()
            ? options.displayName
            : sourceDisplayName(analysis->metadata, analysis->installMapping.desktopEntries);
        if (project.displayName.isEmpty()) project.displayName = analysis->metadata.package;
        project.vendorName = analysis->metadata.maintainer;
        project.sourceIdentity = identity;
        project.createdAt = QDateTime::currentDateTimeUtc();
        project.modifiedAt = project.createdAt;
    }

    PackageRelease release;
    release.formatVersion = 2;
    release.projectId = project.id;
    release.id = releaseId(analysis->metadata.version, sourceHash);
    release.displayName = project.displayName;
    release.archPackageName = project.archPackageName;
    release.sourceType = analysis->type;
    release.acquisition = acquisition;
    release.installMapping = analysis->installMapping;
    release.originalSourceFilename = QFileInfo(qStringFromPath(inputPath)).fileName();
    release.sourceUrl = acquisition.originalUrl;
    release.sourceSha256 = sourceHash;
    release.vendorName = project.vendorName;
    release.debian = analysis->metadata;
    release.packageMetadata.description = analysis->metadata.description;
    release.packageMetadata.homepage = analysis->metadata.homepage;
    release.packageMetadata.licenses = {QStringLiteral("custom:vendor")};
    release.dependencies = analysis->dependencies;
    release.maintainerScripts = analysis->maintainerScripts;
    release.scriptFindings = analysis->scriptFindings;
    release.payload = analysis->payload;
    release.payloadRules = analysis->payloadRules;
    release.update.detectedCandidates = analysis->updateCandidates;
    release.update.aptCandidates = analysis->aptCandidates;
    release.update.rpmCandidates = analysis->rpmCandidates;
    release.update.aptPackageName = release.debian.package;
    release.update.aptArchitecture = release.debian.architecture;
    release.update.rpmPackageName = release.debian.package;
    release.update.rpmArchitecture = release.debian.architecture;
    applyInitialUpdateConfiguration(release, options);
    if (analysis->type == SourcePackageType::ArchPackage) {
        release.archPkgrelOverride = repackagedPkgrel(analysis->upstreamArchPkgrel);
    }
    release.createdAt = QDateTime::currentDateTimeUtc();
    release.modifiedAt = release.createdAt;
    release.state = releaseNeedsReview(release) ? ReleaseState::NeedsReview : ReleaseState::Ready;
    if (options.initialUpdate) {
        // Keep the explicitly verified repository acquisition configuration.
    } else if (acquisition.kind == AcquisitionKind::GitHubRelease) {
        release.update.strategy = UpdateStrategy::GitHubRelease;
        release.update.url = acquisition.originalUrl;
        release.update.githubOwner = acquisition.githubOwner;
        release.update.githubRepository = acquisition.githubRepository;
        release.update.githubReleaseId = acquisition.githubReleaseId;
        release.update.githubAssetId = acquisition.githubAssetId;
        release.update.githubTag = acquisition.githubTag;
        release.update.githubPublisherDigest = acquisition.publisherDigest;
        release.update.githubAssetRegex = options.githubAssetRegex;
        release.update.githubIncludePrereleases = options.githubIncludePrereleases;
    } else if (!release.update.aptCandidates.isEmpty()) {
        static_cast<void>(populateAptCandidates(release));
    } else if (!release.update.rpmCandidates.isEmpty()) {
        static_cast<void>(populateRpmCandidates(release));
    }
    const auto *previous = newestPreparedRelease(project);
    if (previous != nullptr) {
        carryForward(*previous, release,
                     previous->pkgbuildManuallyModified
                         ? readPkgbuild(*previous, nullptr).value_or(QString{})
                         : QString{});
        if (!options.initialUpdate) inheritUpdateConfiguration(*previous, release);
    }
    const auto discovered = std::find_if(project.releases.begin(), project.releases.end(),
                                         [&](const auto &candidate) {
                                             return candidate.state == ReleaseState::Discovered &&
                                                    ((acquisition.githubAssetId > 0 &&
                                                      candidate.acquisition.githubAssetId ==
                                                          acquisition.githubAssetId) ||
                                                     (!candidate.sourceSha256.isEmpty() &&
                                                      candidate.sourceSha256 == sourceHash));
                                         });
    if (discovered != project.releases.end()) release.id = discovered->id;

    const auto directory = releasePath(release);
    std::error_code filesystemError;
    for (const auto *subdirectory : {"sources", "files", "patches", "build", "history"}) {
        std::filesystem::create_directories(directory / subdirectory, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return std::nullopt;
        }
    }
    if (!writeImportedLifecycle(directory, release, error)) return std::nullopt;
    if (!materializeIntegrationIcon(
            *this, project, release, inputPath,
            analysis->icon ? analysis->icon->sourcePath : QString{},
            analysis->icon ? analysis->icon->contents : QByteArray{}, error)) {
        return std::nullopt;
    }
    if (!storeImportSigningKeys(directory, analysis->signingKeys, options,
                                release.update, error)) return std::nullopt;
    if (previous != nullptr &&
        !copyInheritedSigningKeys(releasePath(*previous), directory, release.update, error)) {
        return std::nullopt;
    }
    if (progress) progress({ImportStage::CopyingSource, 0});
    if (!copyFileAtomically(inputPath, sourcePath(release), error)) return std::nullopt;
    const auto sourceLink = directory / pathFromQString(release.originalSourceFilename);
    std::filesystem::create_symlink(
        std::filesystem::path("sources") / pathFromQString(release.originalSourceFilename),
        sourceLink, filesystemError);
    if (filesystemError) {
        filesystemError.clear();
        if (!copyFileAtomically(sourcePath(release), sourceLink, error)) return std::nullopt;
    }
    if (progress) progress({ImportStage::GeneratingPkgbuild, 0});
    release.generatedPkgbuild = PkgbuildGenerator::generate(release);
    release.generatedPkgbuildSha256 = sha256Hex(release.generatedPkgbuild.toUtf8());
    if (!writePkgbuildContents(release, release.pkgbuildManuallyModified
                                           ? release.customPkgbuild
                                           : release.generatedPkgbuild, error)) {
        return std::nullopt;
    }
    release.history.append({release.createdAt, QStringLiteral("import"),
                            QStringLiteral("Imported %1 source %2")
                                .arg(sourcePackageTypeName(release.sourceType),
                                     release.originalSourceFilename)});
    project.history.append({release.createdAt, QStringLiteral("release-imported"),
                            QStringLiteral("Imported release %1").arg(release.debian.version)});
    if (discovered != project.releases.end()) {
        const auto index = std::distance(project.releases.begin(), discovered);
        project.releases[static_cast<qsizetype>(index)] = release;
    } else {
        project.releases.append(release);
    }
    project.formatVersion = 5;
    if (progress) progress({ImportStage::SavingProject, 0});
    if (!save(project, error)) return std::nullopt;
    return ImportResult{std::move(project), release.id, projectCreated, false, {}};
}

std::optional<ImportResult> ProjectStore::reanalyzeRelease(
    const QString &projectId, const QString &releaseIdValue, QString *error,
    const ImportProgressCallback &progress) const {
    auto loaded = load(projectId, error);
    if (!loaded) return std::nullopt;
    auto &project = *loaded;
    const auto iterator = std::find_if(
        project.releases.begin(), project.releases.end(),
        [&](const auto &candidate) { return candidate.id == releaseIdValue; });
    if (iterator == project.releases.end()) {
        if (error != nullptr) *error = QStringLiteral("Release not found: %1").arg(releaseIdValue);
        return std::nullopt;
    }
    if (iterator->state == ReleaseState::Discovered ||
        iterator->state == ReleaseState::Preparing) {
        if (error != nullptr) *error = QStringLiteral("The release artifact has not been prepared yet");
        return std::nullopt;
    }

    const auto artifact = sourcePath(*iterator);
    if (!std::filesystem::is_regular_file(artifact)) {
        if (error != nullptr) {
            *error = QStringLiteral("The immutable source artifact is missing: %1")
                         .arg(qStringFromPath(artifact));
        }
        return std::nullopt;
    }
    QString hashError;
    const auto currentHash = sha256File(artifact, &hashError);
    if (currentHash.isEmpty() || currentHash != iterator->sourceSha256) {
        if (error != nullptr) {
            *error = hashError.isEmpty()
                ? QStringLiteral("The stored source artifact no longer matches its recorded SHA256")
                : hashError;
        }
        return std::nullopt;
    }

    auto analysis = analyzeArtifact(artifact, error, progress);
    if (!analysis) return std::nullopt;
    if (analysis->metadata.package.isEmpty()) analysis->metadata.package = iterator->debian.package;
    if (analysis->metadata.version.isEmpty()) analysis->metadata.version = iterator->debian.version;
    if (analysis->metadata.architecture.isEmpty()) {
        analysis->metadata.architecture = iterator->debian.architecture;
    }

    if (progress) progress({ImportStage::PreparingProject, 0});
    const auto previous = *iterator;
    PackageRelease reset;
    reset.formatVersion = std::max(2, previous.formatVersion);
    reset.id = previous.id;
    reset.projectId = project.id;
    reset.displayName = previous.displayName;
    reset.archPackageName = previous.archPackageName;
    reset.sourceType = analysis->type;
    reset.acquisition = previous.acquisition;
    reset.originalSourceFilename = previous.originalSourceFilename;
    reset.sourceUrl = previous.sourceUrl;
    reset.sourceSha256 = previous.sourceSha256;
    reset.vendorName = analysis->metadata.maintainer.isEmpty()
        ? previous.vendorName : analysis->metadata.maintainer;
    // pkgrel belongs to the release identity rather than an editor decision. In
    // particular, retaining it avoids turning a reset of an installed release
    // into an accidental package downgrade.
    reset.archPkgrel = previous.archPkgrel;
    reset.debian = analysis->metadata;
    reset.packageMetadata = previous.packageMetadata;
    reset.dependencies = analysis->dependencies;
    reset.maintainerScripts = analysis->maintainerScripts;
    reset.scriptFindings = analysis->scriptFindings;
    reset.payload = analysis->payload;
    reset.payloadRules = analysis->payloadRules;
    reset.installMapping = analysis->installMapping;
    // Update discovery is configured independently from the package recipe and
    // may be the only way to acquire the next artifact, so a package reset must
    // not destroy it.
    reset.update = previous.update;
    reset.buildStatus = BuildStatus::NeverBuilt;
    reset.builds = previous.builds;
    reset.history = previous.history;
    reset.createdAt = previous.createdAt;
    reset.modifiedAt = QDateTime::currentDateTimeUtc();
    reset.history.append(
        {reset.modifiedAt, QStringLiteral("reanalyzed"),
         QStringLiteral("Reset package setup and reran static artifact analysis")});
    project.history.append(
        {reset.modifiedAt, QStringLiteral("release-reanalyzed"),
         QStringLiteral("Reset and reanalyzed release %1").arg(reset.debian.version)});

    const auto directory = releasePath(reset);
    std::error_code filesystemError;
    for (const auto *subdirectory : {"files", "build", "history", "patches", "sources"}) {
        std::filesystem::create_directories(directory / subdirectory, filesystemError);
        if (filesystemError) {
            if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
            return std::nullopt;
        }
    }
    if (project.iconPath.startsWith(
            QStringLiteral("releases/%1/").arg(previous.id))) {
        project.iconPath.clear();
        project.iconSourcePath.clear();
        project.iconSha256.clear();
    }
    if (!materializeIntegrationIcon(
            *this, project, reset, artifact,
            analysis->icon ? analysis->icon->sourcePath : QString{},
            analysis->icon ? analysis->icon->contents : QByteArray{}, error)) {
        return std::nullopt;
    }
    if (!storeImportSigningKeys(directory, analysis->signingKeys, {}, reset.update, error)) {
        return std::nullopt;
    }
    if (progress) progress({ImportStage::GeneratingPkgbuild, 0});
    reset.generatedPkgbuild = PkgbuildGenerator::generate(reset);
    reset.generatedPkgbuildSha256 = sha256Hex(reset.generatedPkgbuild.toUtf8());
    if (!writePkgbuildContents(reset, reset.generatedPkgbuild, error)) {
        return std::nullopt;
    }

    *iterator = reset;
    if (progress) progress({ImportStage::SavingProject, 0});
    if (!save(project, error)) return std::nullopt;

    // Remove obsolete generated/editor files after the replacement state is
    // safely persisted. Source bytes, signing keys, prior package artifacts,
    // and history are deliberately untouched.
    const auto removeObsolete = [](const std::filesystem::path &path) {
        std::error_code cleanupError;
        if (std::filesystem::is_regular_file(path, cleanupError) ||
            std::filesystem::is_symlink(path, cleanupError)) {
            std::filesystem::remove(path, cleanupError);
        }
    };
    if (!previous.lifecycleScript.fileName.isEmpty()) {
        removeObsolete(directory / pathFromQString(previous.lifecycleScript.fileName));
    }
    removeObsolete(directory / "files" / "previous-manual-PKGBUILD");
    const auto previousIcon = PathSafety::normalizedArchivePath(
        previous.installMapping.icon.projectPath);
    if (previousIcon && previousIcon->startsWith(QStringLiteral("files/")) &&
        *previousIcon != reset.installMapping.icon.projectPath) {
        removeObsolete(directory / pathFromQString(*previousIcon));
    }
    for (const auto *suffix : {"png", "svg", "xpm"}) {
        if (reset.installMapping.icon.format != QString::fromLatin1(suffix)) {
            removeObsolete(directory / QStringLiteral("pacsmith-icon.%1").arg(
                                        QString::fromLatin1(suffix)).toStdString());
        }
    }
    return ImportResult{std::move(project), releaseIdValue, false, false, {}};
}

} // namespace pacsmith
