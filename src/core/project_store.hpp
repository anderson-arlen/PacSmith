#pragma once

#include "core/import_progress.hpp"
#include "core/model.hpp"

#include <QList>
#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <filesystem>
#include <optional>

namespace pacsmith {

struct ImportResult {
    Project project;
    QString releaseId;
    bool projectCreated{false};
    bool duplicate{false};
};

struct ImportOptions {
    SourceAcquisition acquisition;
    QString packageName;
    QString version;
    QString architecture;
    QString displayName;
    QString description;
    QString githubAssetRegex;
    bool githubIncludePrereleases{false};
    // Repository-first imports verify metadata before downloading the artifact,
    // then carry the exact update configuration and explicitly trusted key into
    // the persistent release. The key bytes are never stored in project.json.
    std::optional<UpdateConfiguration> initialUpdate;
    QByteArray trustedSigningKey;
    QString trustedSigningKeySource;
};

struct RetentionPolicy {
    // -1 means unlimited.
    int packageVersions{2};
    int completeReleases{3};
};

struct CleanupResult {
    QStringList removedArtifacts;
    QStringList removedReleases;
    QString message;
    bool skipped{false};
};

class ProjectStore final {
public:
    explicit ProjectStore(std::filesystem::path projectsRoot = defaultProjectsRoot());

    [[nodiscard]] static std::filesystem::path defaultProjectsRoot();
    [[nodiscard]] const std::filesystem::path &projectsRoot() const noexcept;
    [[nodiscard]] std::filesystem::path projectPath(const QString &id) const;
    [[nodiscard]] std::filesystem::path releasePath(const QString &projectId,
                                                    const QString &releaseId) const;
    [[nodiscard]] std::filesystem::path releasePath(const PackageRelease &release) const;
    [[nodiscard]] std::filesystem::path pkgbuildPath(const PackageRelease &release) const;
    [[nodiscard]] std::filesystem::path sourcePath(const PackageRelease &release) const;
    [[nodiscard]] std::filesystem::path iconPath(const Project &project) const;
    [[nodiscard]] std::filesystem::path releaseIconPath(const PackageRelease &release) const;
    [[nodiscard]] std::filesystem::path lifecyclePath(const PackageRelease &release) const;

    [[nodiscard]] QList<Project> list(QString *error = nullptr) const;
    [[nodiscard]] std::optional<Project> load(const QString &idOrName,
                                              QString *error = nullptr) const;
    [[nodiscard]] bool save(Project &project, QString *error = nullptr) const;
    [[nodiscard]] bool deleteProject(const Project &project, QString *error = nullptr) const;
    [[nodiscard]] bool deleteRelease(Project &project, const QString &releaseId,
                                     QString *error = nullptr) const;
    [[nodiscard]] std::optional<ImportResult> importDeb(
        const std::filesystem::path &debPath, QString *error = nullptr,
        const ImportProgressCallback &progress = {},
        const ImportOptions &options = {}) const;
    [[nodiscard]] std::optional<ImportResult> importSource(
        const std::filesystem::path &sourcePath, const ImportOptions &options = {},
        QString *error = nullptr, const ImportProgressCallback &progress = {}) const;
    // Rerun the current analyzers over the immutable stored artifact and reset the
    // release recipe to their deterministic output. Acquisition/update tracking,
    // installation bookkeeping, and historical build records are retained; all
    // editable package-setup decisions are intentionally discarded.
    [[nodiscard]] std::optional<ImportResult> reanalyzeRelease(
        const QString &projectId, const QString &releaseId,
        QString *error = nullptr, const ImportProgressCallback &progress = {}) const;

    [[nodiscard]] bool savePkgbuild(Project &project, PackageRelease &release,
                                    const QString &contents, QString *error = nullptr) const;
    [[nodiscard]] bool saveLifecycle(Project &project, PackageRelease &release,
                                     QString *error = nullptr) const;
    [[nodiscard]] bool removeLifecycle(Project &project, PackageRelease &release,
                                       QString *error = nullptr) const;
    [[nodiscard]] bool synchronizeLifecycle(Project &project, PackageRelease &release,
                                            bool *changed = nullptr,
                                            QString *error = nullptr) const;
    [[nodiscard]] std::optional<QString> readPkgbuild(const PackageRelease &release,
                                                     QString *error = nullptr) const;

    [[nodiscard]] bool reconcileInstalled(Project &project, QString *error = nullptr) const;
    [[nodiscard]] CleanupResult cleanup(Project &project, const RetentionPolicy &policy,
                                        QString *error = nullptr) const;
    [[nodiscard]] PackageRelease *recordDiscoveredRelease(
        Project &project, const PackageRelease &tracker, const QString &version,
        const QString &filename, const QString &sha256, const QString &downloadUrl,
        QString *error = nullptr, qint64 providerReleaseId = 0,
        qint64 providerAssetId = 0, const QString &providerTag = {},
        const QString &publisherDigest = {}, bool providerPrerelease = false) const;

    // nullopt means pacman could not be queried; an empty string means the package is not installed.
    [[nodiscard]] static std::optional<QString> queryInstalledVersion(
        const QString &archPackageName, QString *error = nullptr);

private:
    [[nodiscard]] std::optional<Project> loadById(const QString &id,
                                                  QString *error = nullptr) const;
    [[nodiscard]] std::optional<Project> migrateLegacyProject(
        const QString &id, const QJsonObject &legacyObject, QString *error) const;
    [[nodiscard]] bool synchronizeIntegrationSources(
        const PackageRelease &release, QString *error) const;

    std::filesystem::path root_;
};

[[nodiscard]] QString sha256Hex(const QByteArray &contents);
[[nodiscard]] QString sha256File(const std::filesystem::path &path, QString *error = nullptr);

} // namespace pacsmith
