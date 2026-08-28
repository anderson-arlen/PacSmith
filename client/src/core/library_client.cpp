#include "core/library_client.hpp"

#include "core/managed_package.hpp"
#include "core/path_safety.hpp"
#include "core/payload_review.hpp"

#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTime>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

namespace pacsmith {
namespace {

QString cacheRoot() {
    const auto data = qEnvironmentVariable("XDG_DATA_HOME");
    const auto root = (!data.isEmpty() && QDir::isAbsolutePath(data))
                          ? data
                          : QDir::home().filePath(QStringLiteral(".local/share"));
    return QDir(root).filePath(QStringLiteral("pacsmith/client/cache"));
}

QString apiError(const QByteArray &body, const QString &fallback) {
    const auto document = QJsonDocument::fromJson(body);
    const auto message = document.object().value(QStringLiteral("error")).toObject()
                             .value(QStringLiteral("message")).toString();
    return message.isEmpty() ? fallback : message;
}

bool writeBytes(const QString &path, const QByteArray &data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        return false;
    }
    return true;
}

bool completeProjectRequired(const Project &project, QString *error) {
    if (!project.summaryOnly) return true;
    if (error != nullptr) {
        *error = QStringLiteral(
            "Refusing to modify a project summary; load the complete project first");
    }
    return false;
}

void materializeSigningKeys(const LibraryClient &client, PackageRelease &release) {
    for (auto &key : release.update.signingKeys) {
        const auto safe = PathSafety::normalizedArchivePath(key.relativePath);
        if (!safe || !safe->startsWith(QStringLiteral("files/keys/"))) continue;
        const auto destination = QDir(QString::fromUtf8(client.releasePath(release).string().c_str()))
                                     .filePath(*safe);
        QDir().mkpath(QFileInfo(destination).absolutePath());
        if (!QFileInfo::exists(destination) || QFileInfo(destination).size() == 0) {
            if (!key.artifactId.isEmpty()) {
                QString error;
                static_cast<void>(client.downloadArtifact(key.artifactId, destination, &error));
            } else if (!key.contents.isEmpty()) {
                static_cast<void>(writeBytes(destination, key.contents));
            }
        }
        key.contents.clear();
    }
}

Project projectFromObject(const LibraryClient &client, const QJsonObject &object,
                          bool materializeKeys) {
    auto project = Project::fromJson(object);
    for (auto &release : project.releases) {
        PayloadReview::bindDefaultExclusions(release);
        if (materializeKeys) materializeSigningKeys(client, release);
    }
    return project;
}

UpdateCheckResult githubSourceFromObject(const QJsonObject &object) {
    UpdateCheckResult result;
    result.supported = true;
    result.success = object.value(QStringLiteral("success")).toBool();
    result.updateAvailable = object.value(QStringLiteral("update_available")).toBool();
    result.detectedVersion = object.value(QStringLiteral("detected_version")).toString();
    result.filename = object.value(QStringLiteral("filename")).toString();
    result.sha256 = object.value(QStringLiteral("sha256")).toString();
    result.downloadUrl = object.value(QStringLiteral("download_url")).toString();
    result.message = object.value(QStringLiteral("message")).toString();
    result.releaseId = object.value(QStringLiteral("release_id")).toInteger();
    result.assetId = object.value(QStringLiteral("asset_id")).toInteger();
    result.tag = object.value(QStringLiteral("tag")).toString();
    result.publisherDigest = object.value(QStringLiteral("publisher_digest")).toString();
    result.prerelease = object.value(QStringLiteral("prerelease")).toBool();
    for (const auto &asset : object.value(QStringLiteral("available_assets")).toArray()) {
        result.availableAssets.append(asset.toString());
    }
    for (const auto &asset : object.value(QStringLiteral("matching_assets")).toArray()) {
        result.matchingAssets.append(asset.toString());
    }
    return result;
}

QJsonObject releaseConfiguration(const PackageRelease &release) {
    const auto document = release.toJson();
    QJsonObject configuration;
    static const QStringList editableFields{
        QStringLiteral("displayName"), QStringLiteral("iconPath"),
        QStringLiteral("iconSourcePath"), QStringLiteral("iconSha256"),
        QStringLiteral("installMapping"), QStringLiteral("vendorName"),
        QStringLiteral("archPkgrelOverride"), QStringLiteral("packageMetadata"),
        QStringLiteral("dependencies"),
        QStringLiteral("maintainerScripts"), QStringLiteral("scriptFindings"),
        QStringLiteral("payloadRules"), QStringLiteral("generatedPkgbuild"),
        QStringLiteral("generatedPkgbuildSha256"), QStringLiteral("pkgbuildManuallyModified"),
        QStringLiteral("lifecycleScript"), QStringLiteral("fieldProvenance"),
        QStringLiteral("aiChanges"), QStringLiteral("update"),
        QStringLiteral("buildStatus"), QStringLiteral("state"),
        QStringLiteral("lastBuildLog"), QStringLiteral("producedPackages"),
        QStringLiteral("history"),
    };
    for (const auto &field : editableFields) configuration.insert(field, document.value(field));
    return configuration;
}

QJsonObject changedReleaseConfiguration(const PackageRelease &release,
                                        const QJsonObject &current) {
    const auto desired = releaseConfiguration(release);
    QJsonObject changed;
    for (auto iterator = desired.constBegin(); iterator != desired.constEnd(); ++iterator) {
        if (current.value(iterator.key()) != iterator.value()) {
            changed.insert(iterator.key(), iterator.value());
        }
    }
    return changed;
}

} // namespace

namespace {

QStringList jsonStringList(const QJsonValue &value) {
    QStringList out;
    for (const auto &item : value.toArray()) {
        const auto text = item.toString().trimmed();
        if (!text.isEmpty()) out.append(text);
    }
    return out;
}

QJsonArray jsonStringArray(const QStringList &values) {
    QJsonArray array;
    for (const auto &value : values) array.append(value);
    return array;
}

RepoSettings repoSettingsFromObject(const QJsonObject &object) {
    RepoSettings settings;
    settings.revision = object.value(QStringLiteral("revision")).toInteger(1);
    settings.enabled = object.value(QStringLiteral("enabled")).toBool();
    settings.listenHosts = jsonStringList(object.value(QStringLiteral("listen_hosts")));
    if (settings.listenHosts.isEmpty()) settings.listenHosts.append(QStringLiteral("127.0.0.1"));
    settings.listenPort = object.value(QStringLiteral("listen_port")).toInt(8080);
    settings.advertisedUrl = object.value(QStringLiteral("advertised_url")).toString();
    settings.stableEnabled = object.value(QStringLiteral("stable_enabled")).toBool();
    settings.soakSeconds = object.value(QStringLiteral("soak_seconds")).toInteger(2592000);
    settings.packageNamePrefix = object.value(QStringLiteral("package_name_prefix")).toString();
    settings.trustMode = object.value(QStringLiteral("trust_mode")).toString();
    settings.signingInitialized = object.value(QStringLiteral("signing_initialized")).toBool();
    settings.certified = object.value(QStringLiteral("certified")).toBool();
    settings.fingerprint = object.value(QStringLiteral("fingerprint")).toString();
    settings.fingerprintSpaced = object.value(QStringLiteral("fingerprint_spaced")).toString();
    settings.rootFingerprint = object.value(QStringLiteral("root_fingerprint")).toString();
    settings.rootFingerprintSpaced = object.value(QStringLiteral("root_fingerprint_spaced")).toString();
    settings.keyringVersion = object.value(QStringLiteral("keyring_version")).toInteger();
    settings.keyringPackage = object.value(QStringLiteral("keyring_package")).toString();
    settings.keyringUrl = object.value(QStringLiteral("keyring_url")).toString();
    settings.bound = jsonStringList(object.value(QStringLiteral("bound")));
    settings.certificationHelp = object.value(QStringLiteral("certification_help")).toString();
    settings.certificationCommands = object.value(QStringLiteral("certification_commands")).toString();
    if (settings.trustMode.isEmpty()) settings.trustMode = QStringLiteral("direct");
    return settings;
}

QJsonObject repoSettingsToObject(const RepoSettings &settings) {
    QJsonObject object{
        {QStringLiteral("revision"), settings.revision},
        {QStringLiteral("enabled"), settings.enabled},
        {QStringLiteral("listen_hosts"), jsonStringArray(settings.listenHosts)},
        {QStringLiteral("listen_port"), settings.listenPort},
        {QStringLiteral("advertised_url"), settings.advertisedUrl},
        {QStringLiteral("stable_enabled"), settings.stableEnabled},
        {QStringLiteral("soak_seconds"), settings.soakSeconds},
        {QStringLiteral("package_name_prefix"), settings.packageNamePrefix},
    };
    if (settings.trustMode != QStringLiteral("root-certified") || settings.certified) {
        object.insert(QStringLiteral("trust_mode"), settings.trustMode);
    }
    return object;
}

} // namespace

LibraryClient::LibraryClient(ConnectionConfig config)
    : config_(std::move(config)), transport_(config_) {}

QList<Project> LibraryClient::list(QString *error) const {
    const auto object = getJson(QStringLiteral("/api/v1/projects?summary=1"), error);
    if (!object) return {};
    QString packageError;
    const auto installed = ManagedPackageRegistry::snapshot(&packageError);
    if (!packageError.isEmpty() && error != nullptr && error->isEmpty()) *error = packageError;
    QList<Project> projects;
    const auto array = object->value(QStringLiteral("projects")).toArray();
    for (const auto &entry : array) {
        auto project = projectFromObject(*this, entry.toObject(), false);
        project.summaryOnly = true;
        static_cast<void>(reconcileInstalled(project, installed, nullptr));
        projects.append(std::move(project));
    }
    return projects;
}

std::optional<Project> LibraryClient::load(const QString &idOrName, QString *error) const {
    const auto encoded = QString::fromUtf8(QUrl::toPercentEncoding(idOrName));
    auto object = getJson(QStringLiteral("/api/v1/projects/") + encoded, error);
    if (!object) return std::nullopt;
    auto project = projectFromObject(*this, *object, true);
    static_cast<void>(reconcileInstalled(project, nullptr));
    return project;
}

bool LibraryClient::save(Project &project, QString *error) const {
    if (!completeProjectRequired(project, error)) return false;
    const auto patched = sendJson(QStringLiteral("PATCH"),
                                  QStringLiteral("/api/v1/projects/") + project.id,
                                  {{QStringLiteral("revision"), project.revision},
                                   {QStringLiteral("displayName"), project.displayName},
                                   {QStringLiteral("archPackageName"), project.archPackageName},
                                   {QStringLiteral("vendorName"), project.vendorName},
                                   {QStringLiteral("autoBuildPolicy"),
                                    autoBuildPolicyName(project.autoBuildPolicy)},
                                   {QStringLiteral("compileCachePolicy"),
                                    compileCachePolicyName(project.compileCachePolicy)}},
                                  error, 200);
    if (!patched) return false;
    for (auto &release : project.releases) {
        const auto current = getJson(QStringLiteral("/api/v1/releases/") + release.id, error);
        if (!current) return false;
        const auto configuration = changedReleaseConfiguration(release, *current);
        if (configuration.isEmpty()) continue;
        const auto saved = sendJson(QStringLiteral("PATCH"),
                                    QStringLiteral("/api/v1/releases/") + release.id +
                                        QStringLiteral("/configuration"),
                                    {{QStringLiteral("revision"), release.revision},
                                     {QStringLiteral("configuration"), configuration}},
                                    error, 200);
        if (!saved) return false;
        release = PackageRelease::fromJson(*saved);
        PayloadReview::bindDefaultExclusions(release);
        materializeSigningKeys(*this, release);
    }
    auto reloaded = load(project.id, error);
    if (!reloaded) return false;
    project = *reloaded;
    return true;
}

bool LibraryClient::deleteProject(const QString &id, QString *error) const {
    const auto response = transport_.request(QStringLiteral("DELETE"),
                                             QStringLiteral("/api/v1/projects/") + id);
    return !isError(response, error) && (response.status == 204 || response.status == 200);
}

bool LibraryClient::deleteProject(const Project &project, QString *error) const {
    if (!completeProjectRequired(project, error)) return false;
    return deleteProject(project.id, error);
}

std::optional<ImportResult> LibraryClient::importSource(const QString &sourcePath,
                                                        const ImportOptions &options,
                                                        QString *error) const {
    const QFileInfo info(sourcePath);
    const auto uploaded = transport_.upload(QStringLiteral("/api/v1/artifacts"), info.fileName(),
                                            QStringLiteral("vendor"), info.absoluteFilePath());
    if (isError(uploaded, error) || uploaded.status != 201) {
        if (error != nullptr && error->isEmpty()) {
            *error = apiError(uploaded.body, QStringLiteral("artifact upload failed"));
        }
        return std::nullopt;
    }
    const auto artifact = QJsonDocument::fromJson(uploaded.body).object();
    QJsonObject body{{QStringLiteral("artifact_id"), artifact.value(QStringLiteral("id")).toString()},
                     {QStringLiteral("existing_project_id"), options.existingProjectId},
                     {QStringLiteral("package_name"), options.packageName},
                     {QStringLiteral("version"), options.version},
                     {QStringLiteral("architecture"), options.architecture},
                     {QStringLiteral("description"), options.description},
                     {QStringLiteral("expected_sha256"), options.expectedSha256},
                     {QStringLiteral("acquisition_kind"), acquisitionKindName(options.acquisition.kind)},
                     {QStringLiteral("canonical_identity"), options.acquisition.canonicalIdentity},
                     {QStringLiteral("acquisition"), options.acquisition.toJson()},
                     {QStringLiteral("github_asset_regex"), options.githubAssetRegex},
                     {QStringLiteral("github_include_prereleases"), options.githubIncludePrereleases}};
    if (options.initialUpdate) body.insert(QStringLiteral("update"), options.initialUpdate->toJson());
    if (!options.trustedSigningKey.isEmpty()) {
        body.insert(QStringLiteral("trusted_signing_key"),
                    QString::fromLatin1(options.trustedSigningKey.toBase64()));
        body.insert(QStringLiteral("trusted_signing_key_source"), options.trustedSigningKeySource);
    }
    const auto accepted = sendJson(QStringLiteral("POST"), QStringLiteral("/api/v1/imports"), body,
                                   error, 202);
    if (!accepted) return std::nullopt;
    const auto job = waitForJob(accepted->value(QStringLiteral("job_id")).toString(), error);
    if (!job || job->status != QStringLiteral("succeeded")) {
        if (error != nullptr && error->isEmpty()) {
            *error = job ? job->error : QStringLiteral("import job failed");
        }
        return std::nullopt;
    }
    ImportResult result;
    result.projectCreated = job->result.value(QStringLiteral("project_created")).toBool();
    result.duplicate = job->result.value(QStringLiteral("duplicate")).toBool();
    result.releaseId = job->result.value(QStringLiteral("release_id")).toString();
    const auto projectId = job->result.value(QStringLiteral("project_id")).toString();
    auto project = load(projectId, error);
    if (!project) return std::nullopt;
    result.project = *project;
    result.releaseId = result.releaseId.isEmpty() && !project->releases.isEmpty()
                           ? project->releases.last().id
                           : result.releaseId;
    return result;
}

UpdateCheckResult LibraryClient::resolveGitHub(const QUrl &url, const QString &assetRegex,
                                               const bool includePrereleases,
                                               const PackageRelease *current,
                                               QString *error) const {
    QJsonObject body{{QStringLiteral("url"), url.toString()},
                     {QStringLiteral("asset_regex"), assetRegex},
                     {QStringLiteral("include_prereleases"), includePrereleases}};
    if (current != nullptr) {
        const auto providerRelease = current->acquisition.githubReleaseId > 0
            ? current->acquisition.githubReleaseId : current->update.githubReleaseId;
        body.insert(QStringLiteral("current_version"), current->debian.version);
        body.insert(QStringLiteral("current_release_id"), providerRelease);
        body.insert(QStringLiteral("current_prerelease"),
                    current->acquisition.githubPrerelease);
    }
    const auto response = sendJson(QStringLiteral("POST"), QStringLiteral("/api/v1/github/resolve"),
                                   body, error, 200);
    if (!response) {
        UpdateCheckResult result;
        result.supported = true;
        result.message = error == nullptr ? QStringLiteral("GitHub resolution failed") : *error;
        return result;
    }
    return githubSourceFromObject(*response);
}

std::optional<QJsonObject> LibraryClient::inspectRepositoryKey(const QUrl &url,
                                                               QString *error) const {
    return sendJson(QStringLiteral("POST"),
                    QStringLiteral("/api/v1/repository-keys/inspect"),
                    {{QStringLiteral("url"), url.toString()}}, error, 200);
}

std::optional<GitHubImportResult> LibraryClient::importGitHub(
    const QUrl &url, const QString &assetRegex, const bool includePrereleases,
    const QString &existingProjectId, QString *error) const {
    const auto accepted = sendJson(
        QStringLiteral("POST"), QStringLiteral("/api/v1/github/imports"),
        {{QStringLiteral("url"), url.toString()},
         {QStringLiteral("asset_regex"), assetRegex},
         {QStringLiteral("include_prereleases"), includePrereleases},
         {QStringLiteral("existing_project_id"), existingProjectId}},
        error, 202);
    if (!accepted) return std::nullopt;
    const auto job = waitForJob(accepted->value(QStringLiteral("job_id")).toString(), error);
    if (!job || job->status != QStringLiteral("succeeded")) {
        if (error != nullptr && error->isEmpty()) {
            *error = job ? job->error : QStringLiteral("GitHub import job failed");
        }
        return std::nullopt;
    }
    GitHubImportResult result;
    result.imported.projectCreated = job->result.value(QStringLiteral("project_created")).toBool();
    result.imported.duplicate = job->result.value(QStringLiteral("duplicate")).toBool();
    result.imported.releaseId = job->result.value(QStringLiteral("release_id")).toString();
    const auto projectId = job->result.value(QStringLiteral("project_id")).toString();
    auto project = load(projectId, error);
    if (!project) return std::nullopt;
    result.imported.project = *project;
    result.source = githubSourceFromObject(job->result.value(QStringLiteral("source")).toObject());
    return result;
}

std::optional<ImportResult> LibraryClient::importRemoteUrl(
    const QUrl &url, const QString &existingProjectId, const QString &version,
    const QString &expectedSha256, QString *error) const {
    const auto accepted = sendJson(
        QStringLiteral("POST"), QStringLiteral("/api/v1/remote-imports"),
        {{QStringLiteral("url"), url.toString()},
         {QStringLiteral("existing_project_id"), existingProjectId},
         {QStringLiteral("version"), version},
         {QStringLiteral("expected_sha256"), expectedSha256}}, error, 202);
    if (!accepted) return std::nullopt;
    const auto jobId = accepted->value(QStringLiteral("job_id")).toString();
    std::optional<JobStatus> job;
    for (int attempt = 0; attempt < 12000; ++attempt) {
        job = getJob(jobId, error);
        if (!job) return std::nullopt;
        if (!job->projectId.isEmpty() || job->status == QStringLiteral("failed") ||
            job->status == QStringLiteral("interrupted")) {
            break;
        }
        QThread::msleep(50);
    }
    if (!job || job->projectId.isEmpty()) {
        if (error != nullptr && error->isEmpty()) {
            *error = job && !job->error.isEmpty()
                ? job->error : QStringLiteral("remote import did not create a project");
        }
        return std::nullopt;
    }
    ImportResult result;
    result.projectCreated = existingProjectId.isEmpty();
    result.releaseId = job->releaseId;
    result.jobId = jobId;
    const auto project = load(job->projectId, error);
    if (!project) return std::nullopt;
    result.project = *project;
    return result;
}

std::optional<ImportResult> LibraryClient::importRepository(
    const UpdateConfiguration &update, const QByteArray &trustedSigningKey,
    const QString &signingKeySource, const QString &pinnedFingerprint,
    QString *error) const {
    const auto accepted = sendJson(
        QStringLiteral("POST"), QStringLiteral("/api/v1/repository-imports"),
        {{QStringLiteral("update"), update.toJson()},
         {QStringLiteral("trusted_signing_key"),
          QString::fromLatin1(trustedSigningKey.toBase64())},
         {QStringLiteral("signing_key_source"), signingKeySource},
         {QStringLiteral("pinned_fingerprint"), pinnedFingerprint}}, error, 202);
    if (!accepted) return std::nullopt;
    const auto job = waitForJob(accepted->value(QStringLiteral("job_id")).toString(), error);
    if (!job || job->status != QStringLiteral("succeeded")) {
        if (error != nullptr && error->isEmpty()) {
            *error = job ? job->error : QStringLiteral("repository import job failed");
        }
        return std::nullopt;
    }
    ImportResult result;
    result.projectCreated = job->result.value(QStringLiteral("project_created")).toBool();
    result.duplicate = job->result.value(QStringLiteral("duplicate")).toBool();
    result.releaseId = job->result.value(QStringLiteral("release_id")).toString();
    const auto project = load(job->result.value(QStringLiteral("project_id")).toString(), error);
    if (!project) return std::nullopt;
    result.project = *project;
    return result;
}

std::optional<ImportResult> LibraryClient::reanalyzeRelease(const QString &releaseId,
                                                            QString *error) const {
    const auto accepted = sendJson(QStringLiteral("POST"),
                                   QStringLiteral("/api/v1/releases/") + releaseId +
                                       QStringLiteral("/reanalyze"),
                                   {}, error, 202);
    if (!accepted) return std::nullopt;
    const auto job = waitForJob(accepted->value(QStringLiteral("job_id")).toString(), error);
    if (!job || job->status != QStringLiteral("succeeded")) {
        if (error != nullptr && error->isEmpty()) {
            *error = job ? job->error : QStringLiteral("reanalyze job failed");
        }
        return std::nullopt;
    }
    ImportResult result;
    result.releaseId = job->result.value(QStringLiteral("release_id")).toString();
    const auto projectId = job->result.value(QStringLiteral("project_id")).toString();
    auto project = load(projectId, error);
    if (!project) return std::nullopt;
    result.project = *project;
    return result;
}

std::optional<QString> LibraryClient::readFile(const QString &releaseId, const QString &name,
                                               QString *error) const {
    const auto encoded = QString::fromUtf8(QUrl::toPercentEncoding(name));
    const auto response = transport_.request(
        QStringLiteral("GET"),
        QStringLiteral("/api/v1/releases/") + releaseId + QStringLiteral("/files/") + encoded);
    if (isError(response, error) || response.status != 200) {
        if (error != nullptr && error->isEmpty()) {
            *error = apiError(response.body, QStringLiteral("could not read file"));
        }
        return std::nullopt;
    }
    return QString::fromUtf8(response.body);
}

bool LibraryClient::writeFile(const QString &releaseId, const QString &name,
                              const QString &contents, qint64 revision, QString *error) const {
    const auto encoded = QString::fromUtf8(QUrl::toPercentEncoding(name));
    return sendJson(QStringLiteral("PUT"),
                    QStringLiteral("/api/v1/releases/") + releaseId + QStringLiteral("/files/") + encoded,
                    {{QStringLiteral("revision"), revision}, {QStringLiteral("contents"), contents}},
                    error, 200).has_value();
}

bool LibraryClient::deleteFile(const QString &releaseId, const QString &name,
                               const qint64 revision, QString *error) const {
    const auto path = QStringLiteral("/api/v1/releases/") + releaseId +
                      QStringLiteral("/files/") + QString::fromUtf8(QUrl::toPercentEncoding(name)) +
                      QStringLiteral("?revision=") + QString::number(revision);
    const auto response = transport_.request(QStringLiteral("DELETE"), path);
    if (isError(response, error) || response.status != 200) {
        if (error != nullptr && error->isEmpty()) {
            *error = apiError(response.body, QStringLiteral("could not delete support file"));
        }
        return false;
    }
    return true;
}

std::optional<JobStatus> LibraryClient::startBuild(const QString &releaseId, QString *error,
                                                   const bool automatic) const {
    const auto accepted = sendJson(QStringLiteral("POST"),
                                   QStringLiteral("/api/v1/releases/") + releaseId +
                                       QStringLiteral("/builds"),
                                   {{QStringLiteral("automatic"), automatic}}, error, 202);
    if (!accepted) return std::nullopt;
    JobStatus job;
    job.id = accepted->value(QStringLiteral("job_id")).toString();
    job.status = QStringLiteral("queued");
    return job;
}

std::optional<JobStatus> LibraryClient::startUpdateCheck(const QString &releaseId,
                                                         const bool force,
                                                         QString *error) const {
    const auto accepted = sendJson(
        QStringLiteral("POST"), QStringLiteral("/api/v1/update-checks"),
        {{QStringLiteral("release_id"), releaseId}, {QStringLiteral("force"), force}},
        error, 202);
    if (!accepted) return std::nullopt;
    JobStatus job;
    job.id = accepted->value(QStringLiteral("job_id")).toString();
    job.kind = QStringLiteral("update_check");
    job.status = QStringLiteral("queued");
    job.releaseId = releaseId;
    return job;
}

std::optional<JobStatus> LibraryClient::startUpdatePreparation(const QString &releaseId,
                                                               QString *error) const {
    const auto accepted = sendJson(
        QStringLiteral("POST"),
        QStringLiteral("/api/v1/releases/") + releaseId + QStringLiteral("/prepare"),
        {}, error, 202);
    if (!accepted) return std::nullopt;
    JobStatus job;
    job.id = accepted->value(QStringLiteral("job_id")).toString();
    job.kind = QStringLiteral("update_prepare");
    job.status = QStringLiteral("queued");
    job.releaseId = releaseId;
    return job;
}

std::optional<JobStatus> LibraryClient::getJob(const QString &jobId, QString *error) const {
    const auto object = getJson(QStringLiteral("/api/v1/jobs/") + jobId, error);
    if (!object) return std::nullopt;
    JobStatus job;
    job.id = object->value(QStringLiteral("id")).toString();
    job.kind = object->value(QStringLiteral("kind")).toString();
    job.status = object->value(QStringLiteral("status")).toString();
    job.projectId = object->value(QStringLiteral("project_id")).toString();
    job.projectName = object->value(QStringLiteral("project_name")).toString();
    job.packageName = object->value(QStringLiteral("package_name")).toString();
    job.releaseId = object->value(QStringLiteral("release_id")).toString();
    job.error = object->value(QStringLiteral("error")).toString();
    job.message = object->value(QStringLiteral("message")).toString();
    job.current = object->value(QStringLiteral("current")).toInteger();
    job.total = object->value(QStringLiteral("total")).toInteger();
    job.failedItems = object->value(QStringLiteral("failed_items")).toInteger();
    job.pausedItems = object->value(QStringLiteral("paused_items")).toInteger();
    job.result = object->value(QStringLiteral("result")).toObject();
    return job;
}

QList<JobStatus> LibraryClient::activeJobs(const QString &kind, QString *error) const {
    const auto encoded = QString::fromUtf8(QUrl::toPercentEncoding(kind));
    const auto object = getJson(QStringLiteral("/api/v1/jobs?kind=") + encoded, error);
    if (!object) return {};
    QList<JobStatus> result;
    for (const auto &entry : object->value(QStringLiteral("jobs")).toArray()) {
        const auto value = entry.toObject();
        JobStatus job;
        job.id = value.value(QStringLiteral("id")).toString();
        job.kind = value.value(QStringLiteral("kind")).toString();
        job.status = value.value(QStringLiteral("status")).toString();
        job.projectId = value.value(QStringLiteral("project_id")).toString();
        job.projectName = value.value(QStringLiteral("project_name")).toString();
        job.packageName = value.value(QStringLiteral("package_name")).toString();
        job.releaseId = value.value(QStringLiteral("release_id")).toString();
        job.error = value.value(QStringLiteral("error")).toString();
        job.message = value.value(QStringLiteral("message")).toString();
        job.current = value.value(QStringLiteral("current")).toInteger();
        job.total = value.value(QStringLiteral("total")).toInteger();
        job.failedItems = value.value(QStringLiteral("failed_items")).toInteger();
        job.pausedItems = value.value(QStringLiteral("paused_items")).toInteger();
        job.result = value.value(QStringLiteral("result")).toObject();
        result.append(std::move(job));
    }
    return result;
}

std::optional<JobStatus> LibraryClient::waitForJob(const QString &jobId, QString *error) const {
    for (int attempt = 0; attempt < 12000; ++attempt) {
        const auto job = getJob(jobId, error);
        if (!job) return std::nullopt;
        if (job->status == QStringLiteral("succeeded") || job->status == QStringLiteral("failed") ||
            job->status == QStringLiteral("interrupted")) {
            return job;
        }
        QThread::msleep(50);
    }
    if (error != nullptr) *error = QStringLiteral("timed out waiting for job");
    return std::nullopt;
}

bool LibraryClient::cancelJob(const QString &jobId, QString *error) const {
    const auto response = transport_.request(
        QStringLiteral("POST"), QStringLiteral("/api/v1/jobs/") + jobId + QStringLiteral("/cancel"));
    return !isError(response, error) && response.status == 200;
}

QString LibraryClient::jobLog(const QString &jobId, QString *error) const {
    qint64 offset = 0;
    return jobLog(jobId, 0, &offset, error);
}

QString LibraryClient::jobLog(const QString &jobId, qint64 after, qint64 *offset,
                              QString *error) const {
    const auto object = getJson(QStringLiteral("/api/v1/jobs/") + jobId +
                                    QStringLiteral("/log?after=") + QString::number(after),
                                error);
    if (!object) return {};
    if (offset != nullptr) *offset = object->value(QStringLiteral("offset")).toInteger();
    return object->value(QStringLiteral("chunk")).toString();
}

bool LibraryClient::downloadArtifact(const QString &artifactId, const QString &destination,
                                     QString *error) const {
    return transport_.downloadToFile(
        QStringLiteral("/api/v1/artifacts/") + artifactId + QStringLiteral("/content"),
        destination, error);
}

std::optional<QJsonObject> LibraryClient::inspectPayloadFile(
    const QString &releaseId, const QString &path, QString *error) const {
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("path"), path);
    const auto endpoint = QStringLiteral("/api/v1/releases/%1/payload-inspection?%2")
                              .arg(releaseId, query.query(QUrl::FullyEncoded));
    const auto response = transport_.request(QStringLiteral("GET"), endpoint);
    if (isError(response, error) || response.status != 200) {
        if (error != nullptr && error->isEmpty()) {
            *error = apiError(response.body, QStringLiteral("could not inspect payload file"));
        }
        return std::nullopt;
    }
    const auto document = QJsonDocument::fromJson(response.body);
    if (!document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("invalid payload inspection response");
        return std::nullopt;
    }
    return document.object();
}

void LibraryClient::prefetchReleaseArtifacts(const Project &project) const {
    auto copy = project;
    for (auto &release : copy.releases) materializeSigningKeys(*this, release);
}

QString LibraryClient::cachedArtifactPath(const QString &artifactId,
                                          const QString &filename) const {
    if (artifactId.isEmpty()) return {};
    const auto path = QDir(cacheRoot()).filePath(artifactId + QLatin1Char('-') + filename);
    return QFileInfo::exists(path) ? path : QString{};
}

QString LibraryClient::cacheArtifact(const QString &artifactId, const QString &filename,
                                     QString *error) const {
    if (artifactId.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("missing artifact");
        return {};
    }
    const auto cached = cachedArtifactPath(artifactId, filename);
    if (!cached.isEmpty()) return cached;
    const auto path = QDir(cacheRoot()).filePath(artifactId + QLatin1Char('-') + filename);
    if (!downloadArtifact(artifactId, path, error)) return {};
    return path;
}

std::optional<ServerInfo> LibraryClient::serverInfo(QString *error) const {
    const auto response = transport_.request(QStringLiteral("GET"), QStringLiteral("/api/v1/server"));
    if (isError(response, error) || response.status != 200) {
        if (error != nullptr && error->isEmpty()) {
            *error = apiError(response.body, QStringLiteral("could not read server info"));
        }
        return std::nullopt;
    }
    const auto object = QJsonDocument::fromJson(response.body).object();
    ServerInfo info;
    info.fingerprint = object.value(QStringLiteral("fingerprint")).toString();
    info.fingerprintSha256 = object.value(QStringLiteral("fingerprint_sha256")).toString();
    info.secretBackend = object.value(QStringLiteral("secret_backend")).toString();
    info.pkiReady = object.value(QStringLiteral("pki_ready")).toBool();
    const auto listen = object.value(QStringLiteral("listen")).toObject();
    info.listen.enabled = listen.value(QStringLiteral("enabled")).toBool();
    info.listen.port = listen.value(QStringLiteral("port")).toInt(8443);
    info.listen.hosts.clear();
    info.listen.bound.clear();
    for (const auto &value : listen.value(QStringLiteral("hosts")).toArray()) {
        const auto host = value.toString().trimmed();
        if (!host.isEmpty()) info.listen.hosts.append(host);
    }
    if (info.listen.hosts.isEmpty()) info.listen.hosts.append(QStringLiteral("0.0.0.0"));
    for (const auto &value : listen.value(QStringLiteral("bound")).toArray()) {
        const auto bound = value.toString().trimmed();
        if (!bound.isEmpty()) info.listen.bound.append(bound);
    }
    return info;
}

std::optional<ListenSettings> LibraryClient::saveListen(const ListenSettings &settings,
                                                        QString *error) const {
    QJsonArray hosts;
    for (const auto &host : settings.hosts) hosts.append(host);
    const auto response = transport_.request(
        QStringLiteral("PATCH"), QStringLiteral("/api/v1/server"),
        {{QStringLiteral("Content-Type"), QStringLiteral("application/json")}},
        QJsonDocument(QJsonObject{
                          {QStringLiteral("listen"),
                           QJsonObject{{QStringLiteral("enabled"), settings.enabled},
                                       {QStringLiteral("port"), settings.port},
                                       {QStringLiteral("hosts"), hosts}}}})
            .toJson(QJsonDocument::Compact));
    if (isError(response, error) || response.status != 200) {
        if (error != nullptr && error->isEmpty()) {
            *error = apiError(response.body, QStringLiteral("could not update listen settings"));
        }
        return std::nullopt;
    }
    const auto listen = QJsonDocument::fromJson(response.body).object().value(QStringLiteral("listen")).toObject();
    ListenSettings saved;
    saved.enabled = listen.value(QStringLiteral("enabled")).toBool();
    saved.port = listen.value(QStringLiteral("port")).toInt(settings.port);
    saved.hosts.clear();
    for (const auto &value : listen.value(QStringLiteral("hosts")).toArray()) {
        const auto host = value.toString().trimmed();
        if (!host.isEmpty()) saved.hosts.append(host);
    }
    if (saved.hosts.isEmpty()) saved.hosts = settings.hosts;
    for (const auto &value : listen.value(QStringLiteral("bound")).toArray()) {
        const auto bound = value.toString().trimmed();
        if (!bound.isEmpty()) saved.bound.append(bound);
    }
    return saved;
}

void LibrarySettings::applyTo(AppSettings &settings) const {
    settings.updates.enabled = updatesEnabled;
    settings.updates.daily = updatesDaily;
    settings.updates.weekDay = weekDay;
    settings.updates.localTime = localTime;
    settings.updates.automaticallyPrepare = automaticallyPrepare;
    settings.updates.retentionVersions = retentionVersions;
}

LibrarySettings librarySettingsFromObject(const QJsonObject &object) {
    LibrarySettings settings;
    settings.revision = object.value(QStringLiteral("revision")).toInteger(1);
    const auto updates = object.value(QStringLiteral("updates")).toObject();
    settings.updatesEnabled = updates.value(QStringLiteral("enabled")).toBool();
    settings.updatesDaily = updates.value(QStringLiteral("daily")).toBool(true);
    settings.weekDay = std::clamp(updates.value(QStringLiteral("weekday")).toInt(1), 1, 7);
    settings.localTime = QTime(std::clamp(updates.value(QStringLiteral("hour")).toInt(2), 0, 23),
                               std::clamp(updates.value(QStringLiteral("minute")).toInt(0), 0, 59));
    settings.automaticallyPrepare = updates.value(QStringLiteral("automatically_prepare")).toBool();
    settings.retentionVersions =
        std::max(-1, updates.value(QStringLiteral("retention_versions")).toInt(2));
    const auto build = object.value(QStringLiteral("build")).toObject();
    settings.availableBuildCores =
        std::max(1, build.value(QStringLiteral("available_cores")).toInt(1));
    settings.buildParallelism = std::clamp(
        build.value(QStringLiteral("parallelism")).toInt(1), 1, settings.availableBuildCores);
    return settings;
}

QJsonObject librarySettingsToObject(const LibrarySettings &settings) {
    return {
        {QStringLiteral("revision"), settings.revision},
        {QStringLiteral("updates"),
         QJsonObject{{QStringLiteral("enabled"), settings.updatesEnabled},
                     {QStringLiteral("daily"), settings.updatesDaily},
                     {QStringLiteral("weekday"), settings.weekDay},
                     {QStringLiteral("hour"), settings.localTime.hour()},
                     {QStringLiteral("minute"), settings.localTime.minute()},
                     {QStringLiteral("automatically_prepare"), settings.automaticallyPrepare},
                     {QStringLiteral("retention_versions"), settings.retentionVersions}}},
        {QStringLiteral("build"),
         QJsonObject{{QStringLiteral("parallelism"), settings.buildParallelism}}},
    };
}

std::optional<LibrarySettings> LibraryClient::librarySettings(QString *error) const {
    const auto object = getJson(QStringLiteral("/api/v1/settings"), error);
    if (!object) return std::nullopt;
    return librarySettingsFromObject(*object);
}

std::optional<LibrarySettings> LibraryClient::saveLibrarySettings(const LibrarySettings &settings,
                                                                  QString *error) const {
    const auto object = sendJson(QStringLiteral("PATCH"), QStringLiteral("/api/v1/settings"),
                                 librarySettingsToObject(settings), error, 200);
    if (!object) return std::nullopt;
    return librarySettingsFromObject(*object);
}

std::optional<RepoSettings> LibraryClient::repoSettings(QString *error) const {
    const auto object = getJson(QStringLiteral("/api/v1/repo"), error);
    if (!object) return std::nullopt;
    return repoSettingsFromObject(*object);
}

std::optional<RepoSettings> LibraryClient::saveRepoSettings(const RepoSettings &settings,
                                                            QString *error) const {
    const auto object = sendJson(QStringLiteral("PATCH"), QStringLiteral("/api/v1/repo"),
                                 repoSettingsToObject(settings), error, 200);
    if (!object) return std::nullopt;
    return repoSettingsFromObject(*object);
}

std::optional<QString> LibraryClient::repoBootstrapScript(const QString &channel,
                                                          QString *error) const {
    const auto object = getJson(QStringLiteral("/api/v1/repo/bootstrap?channel=") + channel, error);
    if (!object) return std::nullopt;
    const auto script = object->value(QStringLiteral("script")).toString();
    if (script.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("The library daemon returned an empty bootstrap script");
        return std::nullopt;
    }
    return script;
}

std::optional<RepoSettings> LibraryClient::initRepoSigning(QString *error) const {
    const auto object = sendJson(QStringLiteral("POST"), QStringLiteral("/api/v1/repo/signing/init"),
                                 {}, error, 200);
    if (!object) return std::nullopt;
    return repoSettingsFromObject(*object);
}

bool LibraryClient::downloadRepoPublicKey(const QString &destination, QString *error) const {
    return transport_.downloadToFile(QStringLiteral("/api/v1/repo/signing/public-key"),
                                     destination, error);
}

std::optional<RepoSettings> LibraryClient::uploadRepoRootKey(const QString &publicKey,
                                                             QString *error) const {
    const auto object = sendJson(QStringLiteral("POST"), QStringLiteral("/api/v1/repo/signing/root"),
                                 {{QStringLiteral("public_key"), publicKey}}, error, 200);
    if (!object) return std::nullopt;
    return repoSettingsFromObject(*object);
}

std::optional<RepoSettings> LibraryClient::uploadRepoCertifiedKey(const QString &publicKey,
                                                                 QString *error) const {
    const auto object =
        sendJson(QStringLiteral("POST"), QStringLiteral("/api/v1/repo/signing/certified"),
                 {{QStringLiteral("public_key"), publicKey}}, error, 200);
    if (!object) return std::nullopt;
    return repoSettingsFromObject(*object);
}

std::optional<ProjectRepository> LibraryClient::projectRepo(const QString &projectId,
                                                           QString *error) const {
    const auto object = getJson(QStringLiteral("/api/v1/projects/") + projectId +
                                    QStringLiteral("/repo"),
                                error);
    if (!object) return std::nullopt;
    return ProjectRepository::fromJson(*object);
}

std::optional<ProjectRepository> LibraryClient::saveProjectRepo(const QString &projectId,
                                                               bool publish, bool automaticSoak,
                                                               qint64 soakSecondsOverride,
                                                               const QString &packageNameOverride,
                                                               qint64 revision,
                                                               QString *error) const {
    QJsonObject body{{QStringLiteral("revision"), revision},
                     {QStringLiteral("publish"), publish},
                     {QStringLiteral("automatic_soak"), automaticSoak},
                     {QStringLiteral("soak_seconds_override"), soakSecondsOverride},
                     {QStringLiteral("package_name_override"), packageNameOverride}};
    const auto object = sendJson(QStringLiteral("PATCH"),
                                 QStringLiteral("/api/v1/projects/") + projectId +
                                     QStringLiteral("/repo"),
                                 body, error, 200);
    if (!object) return std::nullopt;
    return ProjectRepository::fromJson(*object);
}

std::optional<ProjectRepository> LibraryClient::promoteProjectRepo(const QString &projectId,
                                                                  QString *error) const {
    const auto object = sendJson(QStringLiteral("POST"),
                                 QStringLiteral("/api/v1/projects/") + projectId +
                                     QStringLiteral("/repo/promote"),
                                 {}, error, 200);
    if (!object) return std::nullopt;
    return ProjectRepository::fromJson(*object);
}

QList<RemoteClient> LibraryClient::clients(QString *error) const {
    const auto response = transport_.request(QStringLiteral("GET"), QStringLiteral("/api/v1/clients"));
    if (isError(response, error) || response.status != 200) return {};
    QList<RemoteClient> out;
    const auto array = QJsonDocument::fromJson(response.body).object().value(QStringLiteral("clients")).toArray();
    for (const auto &entry : array) {
        const auto object = entry.toObject();
        out.append({object.value(QStringLiteral("id")).toString(),
                    object.value(QStringLiteral("name")).toString(),
                    object.value(QStringLiteral("cert_sha256")).toString(),
                    object.value(QStringLiteral("revoked")).toBool()});
    }
    return out;
}

QList<Registration> LibraryClient::pendingRegistrations(QString *error) const {
    const auto response = transport_.request(QStringLiteral("GET"), QStringLiteral("/api/v1/registrations"));
    if (isError(response, error) || response.status != 200) return {};
    QList<Registration> out;
    const auto array = QJsonDocument::fromJson(response.body).object()
                           .value(QStringLiteral("registrations")).toArray();
    for (const auto &entry : array) {
        const auto object = entry.toObject();
        out.append({object.value(QStringLiteral("id")).toString(),
                    object.value(QStringLiteral("name")).toString(),
                    object.value(QStringLiteral("status")).toString(),
                    object.value(QStringLiteral("client_id")).toString(),
                    object.value(QStringLiteral("cert_pem")).toString()});
    }
    return out;
}

bool LibraryClient::approveRegistration(const QString &id, QString *error) const {
    const auto response = transport_.request(QStringLiteral("POST"),
                                        QStringLiteral("/api/v1/registrations/") + id +
                                            QStringLiteral("/approve"));
    return !isError(response, error) && response.status == 200;
}

bool LibraryClient::rejectRegistration(const QString &id, QString *error) const {
    const auto response = transport_.request(QStringLiteral("POST"),
                                        QStringLiteral("/api/v1/registrations/") + id +
                                            QStringLiteral("/reject"));
    return !isError(response, error) && response.status == 200;
}

bool LibraryClient::revokeClient(const QString &id, QString *error) const {
    const auto response = transport_.request(QStringLiteral("POST"),
                                        QStringLiteral("/api/v1/clients/") + id +
                                            QStringLiteral("/revoke"));
    return !isError(response, error) && response.status == 200;
}

std::optional<CredentialStatus> LibraryClient::credentialStatus(const QString &name,
                                                                QString *error) const {
    const auto object = getJson(QStringLiteral("/api/v1/credentials/") + name, error);
    if (!object) return std::nullopt;
    return CredentialStatus{object->value(QStringLiteral("name")).toString(),
                            object->value(QStringLiteral("configured")).toBool(),
                            object->value(QStringLiteral("backend")).toString()};
}

bool LibraryClient::setCredential(const QString &name, const QString &value, QString *error) const {
    return sendJson(QStringLiteral("PUT"), QStringLiteral("/api/v1/credentials/") + name,
                    {{QStringLiteral("value"), value}}, error, 200).has_value();
}

bool LibraryClient::deleteCredential(const QString &name, QString *error) const {
    const auto response = transport_.request(QStringLiteral("DELETE"),
                                             QStringLiteral("/api/v1/credentials/") + name);
    return !isError(response, error) && (response.status == 204 || response.status == 200);
}

bool LibraryClient::reconcileInstalled(Project &project, QString *error) const {
    QString packageError;
    const auto packages = ManagedPackageRegistry::snapshot(&packageError);
    if (!packageError.isEmpty()) {
        if (error != nullptr) *error = packageError;
        return false;
    }
    return reconcileInstalled(project, packages, error);
}

bool LibraryClient::reconcileInstalled(
    Project &project, const QHash<QString, ManagedPackageInfo> &packages, QString *error) const {
    Q_UNUSED(error);
    const auto installed = packages.constFind(project.archPackageName);
    project.installedVersion = installed == packages.cend() ? QString{} : installed->packageVersion;
    project.installedReleaseId.clear();
    project.externallyInstalled = false;
    if (project.installedVersion.isEmpty()) return true;
    if (installed->projectId() == project.id) {
        project.installedReleaseId = installed->releaseId();
    } else {
        project.externallyInstalled = true;
    }
    return true;
}

bool LibraryClient::reachable(QString *error) const {
    const auto response = transport_.request(QStringLiteral("GET"), QStringLiteral("/api/v1/health"));
    if (isError(response, error)) return false;
    if (response.status != 200) {
        if (error != nullptr) {
            *error = apiError(response.body, QStringLiteral("library host is unavailable"));
        }
        return false;
    }
    return true;
}

QList<QPair<QString, QString>> LibraryClient::statusRows() const {
    QList<QPair<QString, QString>> rows;
    const auto add = [&](const QString &key, const QString &value) {
        rows.append({key, value});
    };
    if (config_.mode == ConnectionConfig::Mode::Remote) {
        add(QStringLiteral("mode"), QStringLiteral("remote"));
        add(QStringLiteral("url"), config_.remoteUrl.toString());
        add(QStringLiteral("enrolled"),
            QFileInfo::exists(config_.clientCertPath) && QFileInfo::exists(config_.serverCaPath)
                ? QStringLiteral("true")
                : QStringLiteral("false"));
    } else {
        add(QStringLiteral("mode"), QStringLiteral("local"));
        add(QStringLiteral("socket"), config_.socketPath);
    }
    const auto health = transport_.request(QStringLiteral("GET"), QStringLiteral("/api/v1/health"));
    const bool connected = health.error.isEmpty() && (health.status == 200 || health.status == 503);
    if (!connected) {
        add(QStringLiteral("reachable"), QStringLiteral("false"));
        add(QStringLiteral("error"),
            health.error.isEmpty() ? apiError(health.body, QStringLiteral("library host is unavailable"))
                                   : health.error);
        return rows;
    }
    const auto healthObject = QJsonDocument::fromJson(health.body).object();
    add(QStringLiteral("reachable"), health.status == 200 ? QStringLiteral("true") : QStringLiteral("false"));
    const auto healthStatus = healthObject.value(QStringLiteral("status")).toString();
    const auto database = healthObject.value(QStringLiteral("database")).toString();
    if (!healthStatus.isEmpty()) add(QStringLiteral("health"), healthStatus);
    if (!database.isEmpty()) add(QStringLiteral("database"), database);
    const auto version = transport_.request(QStringLiteral("GET"), QStringLiteral("/api/v1/version"));
    if (version.error.isEmpty() && version.status == 200) {
        const auto object = QJsonDocument::fromJson(version.body).object();
        const auto api = object.value(QStringLiteral("api_version")).toString();
        const auto server = object.value(QStringLiteral("server_version")).toString();
        if (!api.isEmpty()) add(QStringLiteral("api_version"), api);
        if (!server.isEmpty()) add(QStringLiteral("server_version"), server);
    }
    if (config_.mode != ConnectionConfig::Mode::Local) return rows;
    QString infoError;
    const auto info = serverInfo(&infoError);
    if (!info) {
        if (!infoError.isEmpty()) add(QStringLiteral("server"), infoError);
        return rows;
    }
    add(QStringLiteral("fingerprint"), info->fingerprint);
    add(QStringLiteral("fingerprint_sha256"), info->fingerprintSha256);
    add(QStringLiteral("secret_backend"), info->secretBackend);
    add(QStringLiteral("pki_ready"), info->pkiReady ? QStringLiteral("true") : QStringLiteral("false"));
    add(QStringLiteral("enabled"), info->listen.enabled ? QStringLiteral("true") : QStringLiteral("false"));
    add(QStringLiteral("port"), QString::number(info->listen.port));
    add(QStringLiteral("hosts"), info->listen.hosts.join(QLatin1Char(',')));
    add(QStringLiteral("bound"), info->listen.bound.join(QLatin1Char(',')));
    return rows;
}

std::optional<QJsonObject> LibraryClient::getJson(const QString &path, QString *error) const {
    const auto response = transport_.request(QStringLiteral("GET"), path);
    if (isError(response, error) || response.status != 200) {
        if (error != nullptr && error->isEmpty()) {
            *error = apiError(response.body, QStringLiteral("request failed"));
        }
        return std::nullopt;
    }
    return QJsonDocument::fromJson(response.body).object();
}

std::optional<QJsonObject> LibraryClient::sendJson(const QString &method, const QString &path,
                                                   const QJsonObject &body, QString *error,
                                                   int expected) const {
    const auto response = transport_.request(
        method, path, {{QStringLiteral("Content-Type"), QStringLiteral("application/json")}},
        QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (isError(response, error) || response.status != expected) {
        if (error != nullptr && error->isEmpty()) {
            *error = apiError(response.body, QStringLiteral("request failed"));
        }
        return std::nullopt;
    }
    if (response.body.isEmpty()) return QJsonObject{};
    return QJsonDocument::fromJson(response.body).object();
}

bool LibraryClient::isError(const HttpResponse &response, QString *error) {
    if (response.error.isEmpty()) return false;
    if (error != nullptr) *error = response.error;
    return true;
}

std::optional<ImportResult> LibraryClient::importSource(const std::filesystem::path &sourcePath,
                                                        const ImportOptions &options,
                                                        QString *error) const {
    return importSource(QString::fromUtf8(sourcePath.string().c_str()), options, error);
}

bool LibraryClient::savePkgbuild(Project &project, PackageRelease &release,
                                 const QString &contents, QString *error) const {
    const auto releaseId = release.id;
    const auto custom = release.pkgbuildManuallyModified;
    const auto pkgbuild = contents;
    // Persist the in-memory document first. The PKGBUILD write reloads the
    // project; without this, unsaved payload rules and other fields are lost.
    if (!save(project, error)) return false;
    const auto *saved = project.release(releaseId);
    if (saved == nullptr) {
        if (error != nullptr) *error = QStringLiteral("Release was missing after save");
        return false;
    }
    if (!sendJson(QStringLiteral("PUT"),
                  QStringLiteral("/api/v1/releases/") + releaseId + QStringLiteral("/files/PKGBUILD"),
                  {{QStringLiteral("revision"), saved->revision},
                   {QStringLiteral("contents"), pkgbuild},
                   {QStringLiteral("pkgbuildManuallyModified"), custom}},
                  error, 200).has_value()) {
        return false;
    }
    auto reloaded = load(project.id, error);
    if (!reloaded) return false;
    project = *reloaded;
    return true;
}

bool LibraryClient::saveLifecycle(Project &project, PackageRelease &release, QString *error) const {
    if (auto *saved = project.release(release.id)) {
        saved->lifecycleScript = release.lifecycleScript;
    }
    return save(project, error);
}

bool LibraryClient::removeLifecycle(Project &project, PackageRelease &release, QString *error) const {
    release.lifecycleScript = {};
    return save(project, error);
}

std::optional<QString> LibraryClient::readPkgbuild(const PackageRelease &release, QString *error) const {
    return readFile(release.id, QStringLiteral("PKGBUILD"), error);
}

bool LibraryClient::saveCustomPkgbuild(Project &project, PackageRelease &release,
                                       const QString &contents, QString *error) const {
    release.customPkgbuild = contents;
    release.pkgbuildManuallyModified = true;
    return savePkgbuild(project, release, contents, error);
}

bool LibraryClient::activateGuidedPkgbuild(Project &project, PackageRelease &release,
                                           QString *error) const {
    release.pkgbuildManuallyModified = false;
    return savePkgbuild(project, release, release.generatedPkgbuild, error);
}

bool LibraryClient::activateCustomPkgbuild(Project &project, PackageRelease &release,
                                           QString *error) const {
    return saveCustomPkgbuild(project, release, release.customPkgbuild, error);
}

bool LibraryClient::synchronizeLifecycle(Project &project, PackageRelease &release, bool *changed,
                                         QString *error) const {
    if (changed != nullptr) *changed = false;
    static_cast<void>(release);
    return save(project, error);
}

bool LibraryClient::deleteRelease(Project &project, const QString &releaseId, QString *error) const {
    if (!completeProjectRequired(project, error)) return false;
    const auto response = transport_.request(
        QStringLiteral("DELETE"), QStringLiteral("/api/v1/releases/") + releaseId);
    if (isError(response, error) || (response.status != 204 && response.status != 200)) {
        if (error != nullptr && error->isEmpty()) {
            *error = apiError(response.body, QStringLiteral("could not delete release"));
        }
        return false;
    }
    project.releases.erase(std::remove_if(project.releases.begin(), project.releases.end(),
                                          [&](const auto &item) { return item.id == releaseId; }),
                           project.releases.end());
    return true;
}

std::optional<QString> LibraryClient::readIdentityVariables(const PackageRelease &release,
                                                            QString *error) const {
    return readFile(release.id, QStringLiteral("pacsmith.vars"), error);
}

std::filesystem::path LibraryClient::iconPath(const Project &project) const {
    for (const auto &release : project.releases) {
        const auto path = iconPath(release);
        if (!path.empty()) return path;
    }
    static_cast<void>(project.iconSha256);
    return {};
}

std::filesystem::path LibraryClient::iconPath(const PackageRelease &release) const {
    if (!release.installMapping.icon.isConfigured() || release.iconArtifactId.isEmpty()) return {};
    QString error;
    const auto cached = cacheArtifact(release.iconArtifactId, QStringLiteral("icon"), &error);
    if (cached.isEmpty()) return {};
    return std::filesystem::path(cached.toUtf8().constData());
}

bool LibraryClient::setReleaseIcon(PackageRelease &release, const QString &filePath,
                                   QString *error) const {
    const QFileInfo info(filePath);
    const auto uploaded = transport_.upload(QStringLiteral("/api/v1/artifacts"), info.fileName(),
                                            QStringLiteral("icon"), info.absoluteFilePath());
    if (isError(uploaded, error) || uploaded.status != 201) {
        if (error != nullptr && error->isEmpty()) {
            *error = apiError(uploaded.body, QStringLiteral("icon upload failed"));
        }
        return false;
    }
    const auto artifactId = QJsonDocument::fromJson(uploaded.body).object()
                                .value(QStringLiteral("id")).toString();
    if (artifactId.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("icon upload returned no artifact id");
        return false;
    }
    const auto accepted = sendJson(QStringLiteral("PUT"),
                                   QStringLiteral("/api/v1/releases/") + release.id +
                                       QStringLiteral("/icon"),
                                   {{QStringLiteral("artifact_id"), artifactId}}, error, 200);
    if (!accepted) return false;
    release.iconArtifactId = accepted->value(QStringLiteral("iconArtifactId")).toString();
    if (release.iconArtifactId.isEmpty()) release.iconArtifactId = artifactId;
    return true;
}

std::filesystem::path LibraryClient::projectsRoot() const {
    return std::filesystem::path(cacheRoot().toUtf8().constData());
}

std::filesystem::path LibraryClient::sourcePath(const PackageRelease &release) const {
    QString error;
    const auto cached = cacheArtifact(release.sourceArtifactId, release.originalSourceFilename, &error);
    return std::filesystem::path(cached.toUtf8().constData());
}

std::filesystem::path LibraryClient::releasePath(const PackageRelease &release) const {
    return releasePath(release.projectId, release.id);
}

std::filesystem::path LibraryClient::releasePath(const QString &projectId, const QString &releaseId) const {
    const auto path = QDir(cacheRoot()).filePath(projectId + QLatin1Char('/') + releaseId);
    QDir().mkpath(path);
    return std::filesystem::path(path.toUtf8().constData());
}

PackageRelease *LibraryClient::recordDiscoveredRelease(
    Project &project, const PackageRelease &tracker, const QString &version,
    const QString &filename, const QString &sha256, const QString &downloadUrl,
    QString *error, qint64 providerReleaseId, qint64 providerAssetId,
    const QString &providerTag, const QString &publisherDigest, bool providerPrerelease) const {
    for (const auto &existing : project.releases) {
        if (existing.sourceSha256 == sha256 ||
            (existing.debian.version == version && existing.sourceSha256.isEmpty())) {
            const auto existingId = existing.id;
            if (!save(project, error)) return nullptr;
            return project.release(existingId);
        }
    }
    PackageRelease discovered = tracker;
    discovered.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    discovered.revision = 1;
    discovered.state = ReleaseState::Discovered;
    discovered.debian.version = version;
    discovered.originalSourceFilename = filename;
    discovered.sourceSha256 = sha256;
    discovered.sourceUrl = downloadUrl;
    discovered.acquisition.githubReleaseId = providerReleaseId;
    discovered.acquisition.githubAssetId = providerAssetId;
    discovered.acquisition.githubTag = providerTag;
    discovered.acquisition.publisherDigest = publisherDigest;
    discovered.acquisition.githubPrerelease = providerPrerelease;
    if (tracker.update.strategy == UpdateStrategy::DirectUrl) {
        discovered.acquisition.kind = AcquisitionKind::DirectUrl;
        discovered.acquisition.originalUrl = downloadUrl;
    }
    if (!save(project, error)) return nullptr;
    const auto created = sendJson(
        QStringLiteral("POST"),
        QStringLiteral("/api/v1/projects/") + project.id + QStringLiteral("/releases"),
        {{QStringLiteral("document"), discovered.toJson()}}, error, 201);
    if (!created) return nullptr;
    discovered = PackageRelease::fromJson(*created);
    project.releases.append(std::move(discovered));
    if (error != nullptr) error->clear();
    return &project.releases.last();
}

CleanupResult LibraryClient::cleanup(QString *error) const {
    CleanupResult result;
    const auto response = transport_.request(QStringLiteral("POST"), QStringLiteral("/api/v1/cleanup"));
    if (isError(response, error) || response.status != 200) {
        if (error != nullptr && error->isEmpty()) {
            *error = apiError(response.body, QStringLiteral("could not clean up outdated versions"));
        }
        result.skipped = true;
        return result;
    }
    result.message = QStringLiteral("Excess outdated versions and artifacts were cleaned up.");
    return result;
}

} // namespace pacsmith
