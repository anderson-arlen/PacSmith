#include "core/remote_import_service.hpp"

#include "core/deb_download_service.hpp"
#include "core/github_update_service.hpp"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <filesystem>

namespace pacsmith {
namespace {

bool validHttpsUrl(const QUrl &url) {
    return url.isValid() && url.scheme() == QStringLiteral("https") &&
           !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment();
}

QString availableAssetSuffix(const UpdateCheckResult &result) {
    if (result.availableAssets.isEmpty()) return {};
    return QStringLiteral(" Available assets: %1. Supply asset_regex that full-matches exactly one artifact.")
        .arg(result.availableAssets.join(QStringLiteral(", ")));
}

std::optional<QString> downloadArtifact(const QUrl &url, const QString &filename,
                                        const QString &expectedSha256,
                                        QTemporaryDir &temporary, QString *error) {
    if (!validHttpsUrl(url)) {
        if (error != nullptr) *error = QStringLiteral("Resolved artifact URL must be HTTPS");
        return std::nullopt;
    }
    const auto safeFilename = QFileInfo(filename).fileName();
    if (safeFilename.isEmpty() || safeFilename == QStringLiteral(".") ||
        safeFilename == QStringLiteral("..")) {
        if (error != nullptr) *error = QStringLiteral("Remote artifact has no usable filename");
        return std::nullopt;
    }
    const auto target = temporary.filePath(safeFilename);
    ArtifactDownloadService downloader;
    QString downloadedPath;
    QString downloadError;
    QEventLoop loop;
    QObject::connect(&downloader, &ArtifactDownloadService::finished,
                     [&downloadedPath, &loop](const QString &path) {
                         downloadedPath = path;
                         loop.quit();
                     });
    QObject::connect(&downloader, &ArtifactDownloadService::failed,
                     [&downloadError, &loop](const QString &message) {
                         downloadError = message;
                         loop.quit();
                     });
    downloader.start(url, expectedSha256,
                     std::filesystem::path(target.toUtf8().constData()));
    if (downloader.isRunning()) loop.exec();
    if (downloadedPath.isEmpty()) {
        if (error != nullptr) {
            *error = downloadError.isEmpty()
                ? QStringLiteral("Remote artifact download did not complete") : downloadError;
        }
        return std::nullopt;
    }
    return downloadedPath;
}

} // namespace

std::optional<GitHubImportRequest> RemoteImportService::parseGitHubUrl(
    const QUrl &input, const QString &assetRegex, const bool includePrereleases,
    QString *error) {
    auto url = input;
    if (url.host().compare(QStringLiteral("www.github.com"), Qt::CaseInsensitive) == 0) {
        url.setHost(QStringLiteral("github.com"));
    }
    if (!validHttpsUrl(url) || (url.port() != -1 && url.port() != 443) ||
        url.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Enter an HTTPS github.com repository, release, or release-asset URL without credentials or a fragment");
        }
        return std::nullopt;
    }
    const auto parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        if (error != nullptr) *error = QStringLiteral("GitHub URL must identify an owner and repository");
        return std::nullopt;
    }
    GitHubImportRequest request;
    request.owner = parts.at(0);
    request.repository = parts.at(1);
    if (request.repository.endsWith(QStringLiteral(".git"))) request.repository.chop(4);
    request.assetRegex = assetRegex.trimmed();
    request.includePrereleases = includePrereleases;
    if (parts.size() >= 6 && parts.at(2) == QStringLiteral("releases") &&
        parts.at(3) == QStringLiteral("download")) {
        request.requestedTag = parts.at(4);
        request.assetRegex = QRegularExpression::escape(parts.mid(5).join(QLatin1Char('/')));
    } else if (parts.size() >= 5 && parts.at(2) == QStringLiteral("releases") &&
               parts.at(3) == QStringLiteral("tag")) {
        request.requestedTag = parts.mid(4).join(QLatin1Char('/'));
    }
    if (request.assetRegex.isEmpty()) request.assetRegex = QStringLiteral(".*");
    const QRegularExpression expression(request.assetRegex);
    if (!expression.isValid() || request.assetRegex.size() > 512) {
        if (error != nullptr) {
            *error = QStringLiteral("GitHub asset regular expression is invalid or too long: %1")
                         .arg(expression.errorString());
        }
        return std::nullopt;
    }
    return request;
}

std::optional<RemoteArtifactImportResult> RemoteImportService::importGitHub(
    LibraryClient &library, const QUrl &url, const QString &assetRegex,
    const bool includePrereleases, const QString &existingProjectId,
    const QString &token, QString *error) {
    const auto request = parseGitHubUrl(url, assetRegex, includePrereleases, error);
    if (!request) return std::nullopt;

    PackageRelease probe;
    probe.debian.version = QStringLiteral("0");
    probe.update.strategy = UpdateStrategy::GitHubRelease;
    probe.update.githubOwner = request->owner;
    probe.update.githubRepository = request->repository;
    probe.update.githubAssetRegex = request->assetRegex;
    probe.update.githubIncludePrereleases = request->includePrereleases;
    UpdateCheckResult source;
    QEventLoop queryLoop;
    GitHubUpdateService github;
    QObject::connect(&github, &GitHubUpdateService::finished,
                     [&source, &queryLoop](const UpdateCheckResult &result) {
                         source = result;
                         queryLoop.quit();
                     });
    github.start(probe, token, request->requestedTag);
    if (github.isRunning()) queryLoop.exec();
    if (!source.success || source.downloadUrl.isEmpty()) {
        if (error != nullptr) {
            *error = (source.message.isEmpty()
                          ? QStringLiteral("GitHub did not resolve an importable release artifact")
                          : source.message) + availableAssetSuffix(source);
        }
        return std::nullopt;
    }

    QTemporaryDir temporary(
        QDir::temp().filePath(QStringLiteral("pacsmith-github-import-XXXXXX")));
    if (!temporary.isValid()) {
        if (error != nullptr) *error = QStringLiteral("Could not create a temporary import directory");
        return std::nullopt;
    }
    const auto downloaded = downloadArtifact(QUrl(source.downloadUrl), source.filename,
                                             source.sha256, temporary, error);
    if (!downloaded) return std::nullopt;

    ImportOptions options;
    options.version = source.detectedVersion;
    options.existingProjectId = existingProjectId;
    options.githubAssetRegex = request->assetRegex;
    options.githubIncludePrereleases = request->includePrereleases;
    options.acquisition.kind = AcquisitionKind::GitHubRelease;
    options.acquisition.canonicalIdentity =
        QStringLiteral("github:%1/%2").arg(request->owner, request->repository);
    options.acquisition.originalUrl = source.downloadUrl;
    options.acquisition.githubOwner = request->owner;
    options.acquisition.githubRepository = request->repository;
    options.acquisition.githubReleaseId = source.releaseId;
    options.acquisition.githubAssetId = source.assetId;
    options.acquisition.githubTag = source.tag;
    options.acquisition.githubAssetName = source.filename;
    options.acquisition.githubPrerelease = source.prerelease;
    options.acquisition.publisherDigest = source.publisherDigest;
    const auto imported = library.importSource(*downloaded, options, error);
    if (!imported) return std::nullopt;
    return RemoteArtifactImportResult{*imported, source};
}

std::optional<RemoteArtifactImportResult> RemoteImportService::importDirectUrl(
    LibraryClient &library, const QUrl &url, const QString &existingProjectId,
    QString *error) {
    if (!validHttpsUrl(url)) {
        if (error != nullptr) {
            *error = QStringLiteral("Direct artifact URL must be HTTPS without credentials or a fragment");
        }
        return std::nullopt;
    }
    auto filename = QFileInfo(url.path()).fileName();
    if (filename.isEmpty()) filename = QStringLiteral("vendor-artifact");
    QTemporaryDir temporary(
        QDir::temp().filePath(QStringLiteral("pacsmith-direct-import-XXXXXX")));
    if (!temporary.isValid()) {
        if (error != nullptr) *error = QStringLiteral("Could not create a temporary import directory");
        return std::nullopt;
    }
    const auto downloaded = downloadArtifact(url, filename, {}, temporary, error);
    if (!downloaded) return std::nullopt;
    ImportOptions options;
    options.existingProjectId = existingProjectId;
    options.acquisition.kind = AcquisitionKind::DirectUrl;
    options.acquisition.canonicalIdentity =
        url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
    options.acquisition.originalUrl = url.toString();
    const auto imported = library.importSource(*downloaded, options, error);
    if (!imported) return std::nullopt;
    UpdateCheckResult source;
    source.success = true;
    source.supported = true;
    source.filename = filename;
    source.downloadUrl = url.toString();
    return RemoteArtifactImportResult{*imported, source};
}

} // namespace pacsmith
