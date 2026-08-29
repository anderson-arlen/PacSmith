#pragma once

#include "core/http_transport.hpp"
#include "core/import_progress.hpp"
#include "core/managed_package.hpp"
#include "core/model.hpp"
#include "core/app_settings.hpp"
#include "core/project_store/project_store.hpp"
#include "core/update_source.hpp"

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTime>

#include <filesystem>
#include <optional>

namespace pacsmith {

struct JobStatus {
    QString id;
    QString kind;
    QString status;
    QString projectId;
    QString projectName;
    QString packageName;
    QString releaseId;
    QString error;
    QString message;
    qint64 current{0};
    qint64 total{0};
    qint64 failedItems{0};
    qint64 pausedItems{0};
    QJsonObject result;
};

struct CredentialStatus {
    QString name;
    bool configured{false};
    QString backend;
};

struct GitHubImportResult {
    ImportResult imported;
    UpdateCheckResult source;
};

struct ListenSettings {
    bool enabled{false};
    int port{8443};
    QStringList hosts{QStringLiteral("0.0.0.0")};
    QStringList bound;
};

struct ServerInfo {
    QString fingerprint;
    QString fingerprintSha256;
    QString secretBackend;
    bool pkiReady{false};
    ListenSettings listen;
};

struct RemoteClient {
    QString id;
    QString name;
    QString certSha256;
    bool revoked{false};
};

struct Registration {
    QString id;
    QString name;
    QString status;
    QString clientId;
    QString certPem;
};

struct LibrarySettings {
    qint64 revision{1};
    bool updatesEnabled{false};
    bool updatesDaily{true};
    int weekDay{1};
    QTime localTime{2, 0};
    bool automaticallyPrepare{false};
    int retentionVersions{2};
    int buildParallelism{1};
    int availableBuildCores{1};

    void applyTo(AppSettings &settings) const;
};

struct RepoSettings {
    qint64 revision{1};
    bool enabled{false};
    QStringList listenHosts{QStringLiteral("127.0.0.1")};
    int listenPort{8080};
    QString advertisedUrl;
    bool stableEnabled{false};
    qint64 soakSeconds{2592000};
    QString packageNamePrefix;
    QString trustMode{QStringLiteral("direct")};
    bool signingInitialized{false};
    bool certified{false};
    QString fingerprint;
    QString fingerprintSpaced;
    QString rootFingerprint;
    QString rootFingerprintSpaced;
    qint64 keyringVersion{0};
    QString keyringPackage;
    QString keyringUrl;
    QStringList bound;
    QString certificationHelp;
    QString certificationCommands;
};

class LibraryClient final {
public:
    explicit LibraryClient(ConnectionConfig config = ConnectionConfig::load());

    [[nodiscard]] QList<Project> list(QString *error = nullptr) const;
    [[nodiscard]] std::optional<Project> load(const QString &idOrName, QString *error = nullptr) const;
    [[nodiscard]] bool save(Project &project, QString *error = nullptr) const;
    [[nodiscard]] bool deleteProject(const QString &id, QString *error = nullptr) const;
    [[nodiscard]] bool deleteProject(const Project &project, QString *error = nullptr) const;
    [[nodiscard]] std::optional<ImportResult> importSource(
        const QString &sourcePath, const ImportOptions &options = {},
        QString *error = nullptr) const;
    [[nodiscard]] UpdateCheckResult resolveGitHub(
        const QUrl &url, const QString &assetRegex = {}, bool includePrereleases = false,
        const PackageRelease *current = nullptr, QString *error = nullptr) const;
    [[nodiscard]] std::optional<QJsonObject> inspectRepositoryKey(
        const QUrl &url, QString *error = nullptr) const;
    [[nodiscard]] std::optional<GitHubImportResult> importGitHub(
        const QUrl &url, const QString &assetRegex = {}, bool includePrereleases = false,
        const QString &existingProjectId = {}, QString *error = nullptr) const;
    [[nodiscard]] std::optional<ImportResult> importRemoteUrl(
        const QUrl &url, const QString &existingProjectId = {},
        const QString &version = {}, const QString &expectedSha256 = {},
        QString *error = nullptr) const;
    [[nodiscard]] std::optional<ImportResult> importRepository(
        const UpdateConfiguration &update, const QByteArray &trustedSigningKey,
        const QString &signingKeySource, const QString &pinnedFingerprint,
        QString *error = nullptr) const;
    [[nodiscard]] std::optional<ImportResult> importSource(
        const std::filesystem::path &sourcePath, const ImportOptions &options = {},
        QString *error = nullptr) const;
    [[nodiscard]] bool savePkgbuild(Project &project, PackageRelease &release,
                                    const QString &contents, QString *error = nullptr) const;
    [[nodiscard]] bool saveLifecycle(Project &project, PackageRelease &release,
                                     QString *error = nullptr) const;
    [[nodiscard]] bool removeLifecycle(Project &project, PackageRelease &release,
                                       QString *error = nullptr) const;
    [[nodiscard]] bool saveCustomPkgbuild(Project &project, PackageRelease &release,
                                          const QString &contents, QString *error = nullptr) const;
    [[nodiscard]] bool activateGuidedPkgbuild(Project &project, PackageRelease &release,
                                             QString *error = nullptr) const;
    [[nodiscard]] bool activateCustomPkgbuild(Project &project, PackageRelease &release,
                                             QString *error = nullptr) const;
    [[nodiscard]] bool synchronizeLifecycle(Project &project, PackageRelease &release,
                                            bool *changed = nullptr, QString *error = nullptr) const;
    [[nodiscard]] bool deleteRelease(Project &project, const QString &releaseId,
                                     QString *error = nullptr) const;
    [[nodiscard]] std::optional<QString> readIdentityVariables(const PackageRelease &release,
                                                               QString *error = nullptr) const;
    [[nodiscard]] bool setReleaseIcon(PackageRelease &release, const QString &filePath,
                                      QString *error = nullptr) const;
    [[nodiscard]] std::filesystem::path sourcePath(const PackageRelease &release) const;
    [[nodiscard]] std::filesystem::path iconPath(const Project &project) const;
    [[nodiscard]] std::filesystem::path iconPath(const PackageRelease &release) const;
    [[nodiscard]] std::filesystem::path releasePath(const PackageRelease &release) const;
    [[nodiscard]] std::filesystem::path releasePath(const QString &projectId,
                                                    const QString &releaseId) const;
    [[nodiscard]] std::filesystem::path projectsRoot() const;
    [[nodiscard]] PackageRelease *recordDiscoveredRelease(
        Project &project, const PackageRelease &tracker, const QString &version,
        const QString &filename, const QString &sha256, const QString &downloadUrl,
        QString *error = nullptr, qint64 providerReleaseId = 0,
        qint64 providerAssetId = 0, const QString &providerTag = {},
        const QString &publisherDigest = {}, bool providerPrerelease = false) const;
    [[nodiscard]] CleanupResult cleanup(QString *error = nullptr) const;
    [[nodiscard]] std::optional<ImportResult> reanalyzeRelease(
        const QString &releaseId, QString *error = nullptr) const;
    [[nodiscard]] std::optional<QString> readFile(const QString &releaseId, const QString &name,
                                                  QString *error = nullptr) const;
    [[nodiscard]] std::optional<QString> readPkgbuild(const PackageRelease &release,
                                                      QString *error = nullptr) const;
    [[nodiscard]] bool writeFile(const QString &releaseId, const QString &name,
                                 const QString &contents, qint64 revision,
                                 QString *error = nullptr) const;
    [[nodiscard]] bool deleteFile(const QString &releaseId, const QString &name,
                                  qint64 revision, QString *error = nullptr) const;
    [[nodiscard]] std::optional<JobStatus> startBuild(const QString &releaseId,
                                                      QString *error = nullptr,
                                                      bool automatic = false) const;
    [[nodiscard]] std::optional<JobStatus> startUpdateCheck(
        const QString &releaseId = {}, bool force = false,
        QString *error = nullptr) const;
    [[nodiscard]] std::optional<JobStatus> startUpdatePreparation(
        const QString &releaseId, QString *error = nullptr) const;
    [[nodiscard]] std::optional<JobStatus> getJob(const QString &jobId,
                                                  QString *error = nullptr) const;
    [[nodiscard]] QList<JobStatus> activeJobs(const QString &kind,
                                              QString *error = nullptr) const;
    [[nodiscard]] std::optional<JobStatus> waitForJob(const QString &jobId,
                                                      QString *error = nullptr) const;
    [[nodiscard]] bool cancelJob(const QString &jobId, QString *error = nullptr) const;
    [[nodiscard]] QString jobLog(const QString &jobId, QString *error = nullptr) const;
    [[nodiscard]] QString jobLog(const QString &jobId, qint64 after, qint64 *offset,
                                 QString *error = nullptr) const;
    [[nodiscard]] bool downloadArtifact(const QString &artifactId, const QString &destination,
                                        QString *error = nullptr) const;
    [[nodiscard]] std::optional<QJsonObject> inspectPayloadFile(
        const QString &releaseId, const QString &path, QString *error = nullptr) const;
    void prefetchReleaseArtifacts(const Project &project) const;
    [[nodiscard]] QString cacheArtifact(const QString &artifactId, const QString &filename,
                                        QString *error = nullptr) const;
    [[nodiscard]] QString cachedArtifactPath(const QString &artifactId,
                                             const QString &filename) const;
    [[nodiscard]] std::optional<ServerInfo> serverInfo(QString *error = nullptr) const;
    [[nodiscard]] std::optional<ListenSettings> saveListen(const ListenSettings &settings,
                                                           QString *error = nullptr) const;
    [[nodiscard]] std::optional<LibrarySettings> librarySettings(QString *error = nullptr) const;
    [[nodiscard]] std::optional<LibrarySettings> saveLibrarySettings(const LibrarySettings &settings,
                                                                     QString *error = nullptr) const;
    [[nodiscard]] std::optional<RepoSettings> repoSettings(QString *error = nullptr) const;
    [[nodiscard]] std::optional<RepoSettings> saveRepoSettings(const RepoSettings &settings,
                                                               QString *error = nullptr) const;
    [[nodiscard]] std::optional<QString> repoBootstrapScript(const QString &channel,
                                                             QString *error = nullptr) const;
    [[nodiscard]] std::optional<RepoSettings> initRepoSigning(QString *error = nullptr) const;
    [[nodiscard]] bool downloadRepoPublicKey(const QString &destination, QString *error = nullptr) const;
    [[nodiscard]] std::optional<RepoSettings> uploadRepoRootKey(const QString &publicKey,
                                                                QString *error = nullptr) const;
    [[nodiscard]] std::optional<RepoSettings> uploadRepoCertifiedKey(const QString &publicKey,
                                                                    QString *error = nullptr) const;
    [[nodiscard]] std::optional<ProjectRepository> projectRepo(const QString &projectId,
                                                              QString *error = nullptr) const;
    [[nodiscard]] std::optional<ProjectRepository> saveProjectRepo(const QString &projectId,
                                                                  bool publish, bool automaticSoak,
                                                                  qint64 soakSecondsOverride,
                                                                  const QString &packageNameOverride,
                                                                  qint64 revision,
                                                                  QString *error = nullptr) const;
    [[nodiscard]] std::optional<ProjectRepository> promoteProjectRepo(const QString &projectId,
                                                                     QString *error = nullptr) const;
    [[nodiscard]] QList<RemoteClient> clients(QString *error = nullptr) const;
    [[nodiscard]] QList<Registration> pendingRegistrations(QString *error = nullptr) const;
    [[nodiscard]] bool approveRegistration(const QString &id, QString *error = nullptr) const;
    [[nodiscard]] bool rejectRegistration(const QString &id, QString *error = nullptr) const;
    [[nodiscard]] bool revokeClient(const QString &id, QString *error = nullptr) const;
    [[nodiscard]] std::optional<CredentialStatus> credentialStatus(const QString &name,
                                                                   QString *error = nullptr) const;
    [[nodiscard]] bool setCredential(const QString &name, const QString &value,
                                     QString *error = nullptr) const;
    [[nodiscard]] bool deleteCredential(const QString &name, QString *error = nullptr) const;
    [[nodiscard]] bool recordPackageOperation(Project &project, const QString &releaseId,
                                              const QString &operation, int exitCode,
                                              bool canceled, const QString &failure,
                                              QString *error = nullptr) const;
    [[nodiscard]] bool reconcileInstalled(Project &project, QString *error = nullptr) const;
    [[nodiscard]] bool reachable(QString *error = nullptr) const;
    [[nodiscard]] QList<QPair<QString, QString>> statusRows() const;
    [[nodiscard]] const ConnectionConfig &config() const noexcept { return config_; }

private:
    [[nodiscard]] bool reconcileInstalled(
        Project &project, const QHash<QString, ManagedPackageInfo> &packages,
        QString *error = nullptr) const;
    [[nodiscard]] std::optional<QJsonObject> getJson(const QString &path, QString *error) const;
    [[nodiscard]] std::optional<QJsonObject> sendJson(const QString &method, const QString &path,
                                                      const QJsonObject &body,
                                                      QString *error, int expected) const;
    [[nodiscard]] static bool isError(const HttpResponse &response, QString *error);

    ConnectionConfig config_;
    HttpTransport transport_;
};

} // namespace pacsmith
