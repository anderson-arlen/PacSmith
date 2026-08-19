#pragma once

#include "core/project_store/project_store.hpp"
#include "core/repository_trust.hpp"
#include "core/script_evidence.hpp"
#include "core/source_analyzer.hpp"

#include <QByteArray>
#include <QList>
#include <QString>

#include <filesystem>
#include <optional>

namespace pacsmith::project_store_internal {

[[nodiscard]] std::filesystem::path pathFromQString(const QString &value);
[[nodiscard]] QString qStringFromPath(const std::filesystem::path &value);
[[nodiscard]] bool validId(const QString &id);
[[nodiscard]] bool payloadPathCovers(const QString &parent, const QString &child);
[[nodiscard]] bool releaseWasBuilt(const PackageRelease &release);
bool writeBytes(const std::filesystem::path &path, const QByteArray &contents, QString *error);
bool writeImportedLifecycle(const std::filesystem::path &releaseDirectory,
                            PackageRelease &release, QString *error);
void applyInitialUpdateConfiguration(PackageRelease &release, const ImportOptions &options);
void inheritUpdateConfiguration(const PackageRelease &previous, PackageRelease &next);
bool copyInheritedSigningKeys(const std::filesystem::path &previousDirectory,
                              const std::filesystem::path &nextDirectory,
                              const UpdateConfiguration &update, QString *error);
bool storeImportSigningKeys(const std::filesystem::path &directory,
                            const QList<ExtractedSigningKey> &detectedKeys,
                            const ImportOptions &options, UpdateConfiguration &update,
                            QString *error);
[[nodiscard]] std::optional<SourceAnalysis> analyzeArtifact(
    const std::filesystem::path &path, QString *error,
    const ImportProgressCallback &progress);
bool copyFileAtomically(const std::filesystem::path &source,
                        const std::filesystem::path &destination, QString *error);
bool materializeIntegrationIcon(const ProjectStore &store, Project &project,
                                PackageRelease &release,
                                const std::filesystem::path &artifactPath,
                                const QString &fallbackSourcePath,
                                const QByteArray &fallbackContents, QString *error);
[[nodiscard]] QString sourceDisplayName(const DebianMetadata &metadata,
                                        const QList<DesktopEntryConfiguration> &desktops = {});
[[nodiscard]] QString sourceIdentity(const DebianMetadata &metadata);
[[nodiscard]] QString githubSourceIdentity(const QString &owner, const QString &repository);
[[nodiscard]] QString normalizedSourceIdentity(const QString &identity);
[[nodiscard]] QString githubIdentityFromProject(const Project &project);
[[nodiscard]] QString requestedSourceIdentity(const ImportOptions &options,
                                              const DebianMetadata *metadata);
[[nodiscard]] std::optional<Project> findMatchingProject(const QList<Project> &projects,
                                                         const ImportOptions &options,
                                                         const DebianMetadata *metadata);
void adoptCanonicalIdentity(Project &project, const QString &identity);
[[nodiscard]] QString releaseId(const QString &version, const QString &sha256);
[[nodiscard]] QString expectedArchVersion(const PackageRelease &release);
[[nodiscard]] QString repackagedPkgrel(const QString &upstream);
[[nodiscard]] bool releaseMatchesInstalledVersion(const Project &project,
                                                  const QString &installedVersion);
[[nodiscard]] bool projectOwnsInstalledPackage(const Project &project,
                                               const QString &installedVersion);
[[nodiscard]] FieldProvenance detected(const QString &fingerprint, const QString &rationale);
bool populateAptCandidates(PackageRelease &release);
bool populateRpmCandidates(PackageRelease &release);
[[nodiscard]] ScriptEvidence payloadRepositoryEvidence(const PackageRelease &release);
bool migrateScriptEvidence(PackageRelease &release);
bool migratePayloadMetadata(PackageRelease &release);
[[nodiscard]] QString desktopField(const QString &contents, const QString &name);
bool migrateAppImageIntegration(PackageRelease &release);
bool migrateSigningTrust(PackageRelease &release, const std::filesystem::path &directory);
bool applyCurrentMappings(PackageRelease &release);
[[nodiscard]] const PackageRelease *newestPreparedRelease(const Project &project);
bool repairSloganDisplayName(Project &project);
void carryForward(const PackageRelease &previous, PackageRelease &next,
                  const QString &previousPkgbuild);
[[nodiscard]] bool archiveDesktopCommandUnmapped(const PackageRelease &release);
[[nodiscard]] bool releaseNeedsReview(const PackageRelease &release);

} // namespace pacsmith::project_store_internal
