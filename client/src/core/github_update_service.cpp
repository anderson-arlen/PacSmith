#include "core/github_update_service.hpp"

#include "core/apt_repository.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

#include <optional>

namespace pacsmith {
namespace {

QString versionFromTag(QString tag) {
    tag = tag.trimmed();
    if (tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive) && tag.size() > 1 &&
        tag.at(1).isDigit()) tag.remove(0, 1);
    tag.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9+._]+")), QStringLiteral("_"));
    tag.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    while (tag.startsWith(QLatin1Char('_'))) tag.remove(0, 1);
    while (tag.endsWith(QLatin1Char('_'))) tag.chop(1);
    return tag.isEmpty() ? QStringLiteral("0") : tag;
}

QString rateLimitSuffix(const QNetworkReply *reply) {
    const auto remaining = reply->rawHeader("x-ratelimit-remaining");
    const auto reset = reply->rawHeader("x-ratelimit-reset");
    if (remaining.isEmpty()) return {};
    return QStringLiteral(" (GitHub rate limit remaining: %1%2)")
        .arg(QString::fromLatin1(remaining),
             reset.isEmpty() ? QString{} : QStringLiteral(", reset epoch %1").arg(QString::fromLatin1(reset)));
}

bool isSidecarAsset(const QString &name) {
    const auto lower = name.toLower();
    return lower.endsWith(QStringLiteral(".sig")) ||
           lower.endsWith(QStringLiteral(".asc")) ||
           lower.endsWith(QStringLiteral(".sha256")) ||
           lower.endsWith(QStringLiteral(".sha512")) ||
           lower.contains(QStringLiteral("checksums")) ||
           lower.contains(QStringLiteral("sha256sums")) ||
           lower == QStringLiteral("manifest.json");
}

QString sourceArchiveFilename(const PackageRelease &current, const QString &tag,
                              const QString &extension) {
    auto repository = current.update.githubRepository.trimmed();
    repository.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._+-]+")),
                       QStringLiteral("-"));
    while (repository.startsWith(QLatin1Char('-'))) repository.remove(0, 1);
    while (repository.endsWith(QLatin1Char('-'))) repository.chop(1);
    if (repository.isEmpty()) repository = QStringLiteral("source");
    return QStringLiteral("%1-%2.%3")
        .arg(repository, versionFromTag(tag), extension);
}

void appendSourceArchive(QList<QJsonObject> &artifacts, const QJsonObject &release,
                         const PackageRelease &current, const QString &urlField,
                         const QString &label, const QString &extension) {
    const auto url = release.value(urlField).toString();
    if (url.isEmpty()) return;
    artifacts.append(QJsonObject{
        {QStringLiteral("name"), label},
        {QStringLiteral("browser_download_url"), url},
        {QStringLiteral("pacsmith_filename"),
         sourceArchiveFilename(current, release.value(QStringLiteral("tag_name")).toString(),
                               extension)}});
}

QString numericVersionCore(const QString &value) {
    static const QRegularExpression numericCore(
        QStringLiteral("([0-9]+(?:\\.[0-9]+)+)"));
    return numericCore.match(value).captured(1);
}

bool tagLooksLikePrerelease(const QString &value) {
    static const QRegularExpression marker(
        QStringLiteral("(?:^|[._+~\\-])(?:alpha|beta|rc|pre|preview|dev|nightly)(?:$|[._+~\\-]?[0-9])"),
        QRegularExpression::CaseInsensitiveOption);
    return marker.match(value).hasMatch();
}

} // namespace

GitHubUpdateService::GitHubUpdateService(QObject *parent) : QObject(parent), network_(this) {}

bool GitHubUpdateService::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void GitHubUpdateService::start(const PackageRelease &release, const QString &token,
                                const QString &requestedTag) {
    if (running_.load(std::memory_order_acquire)) {
        UpdateCheckResult result;
        result.supported = true;
        result.message = QStringLiteral("A GitHub update check is already running");
        emit finished(result);
        return;
    }
    if (release.update.githubOwner.isEmpty() || release.update.githubRepository.isEmpty() ||
        release.update.githubAssetRegex.isEmpty()) {
        UpdateCheckResult result;
        result.supported = true;
        result.message = QStringLiteral("GitHub owner, repository, and artifact regular expression are required");
        emit finished(result);
        return;
    }
    const QRegularExpression expression(release.update.githubAssetRegex);
    if (!expression.isValid() || release.update.githubAssetRegex.size() > 512) {
        UpdateCheckResult result;
        result.supported = true;
        result.message = QStringLiteral("GitHub artifact regular expression is invalid or too long: %1")
                             .arg(expression.errorString());
        emit finished(result);
        return;
    }
    current_ = release;
    requestedTag_ = requestedTag;
    const auto owner = QString::fromLatin1(QUrl::toPercentEncoding(release.update.githubOwner));
    const auto repository = QString::fromLatin1(
        QUrl::toPercentEncoding(release.update.githubRepository));
    QUrl url(requestedTag.isEmpty()
                 ? QStringLiteral("https://api.github.com/repos/%1/%2/releases?per_page=50")
                       .arg(owner, repository)
                 : QStringLiteral("https://api.github.com/repos/%1/%2/releases/tags/%3")
                       .arg(owner, repository,
                            QString::fromLatin1(QUrl::toPercentEncoding(requestedTag))));
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setRawHeader("User-Agent", "PacSmith/0.2");
    if (!token.isEmpty()) request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + token.toUtf8());
    if (requestedTag.isEmpty() && !release.update.githubEtag.isEmpty()) {
        request.setRawHeader("If-None-Match", release.update.githubEtag.toUtf8());
    }
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    emit progressChanged(QStringLiteral("Querying GitHub releases for %1/%2…")
                             .arg(release.update.githubOwner, release.update.githubRepository));
    running_.store(true, std::memory_order_release);
    reply_ = network_.get(request);
    connect(reply_, &QNetworkReply::finished, this, &GitHubUpdateService::finishReply);
}

void GitHubUpdateService::cancel() {
    if (reply_ != nullptr) reply_->abort();
}

UpdateCheckResult GitHubUpdateService::selectRelease(const QJsonArray &releases,
                                                      const PackageRelease &current,
                                                      QString *error,
                                                      const QString &requestedTag) {
    UpdateCheckResult result;
    result.supported = true;
    const QRegularExpression assetPattern(current.update.githubAssetRegex);
    if (!assetPattern.isValid()) {
        if (error != nullptr) *error = assetPattern.errorString();
        result.message = QStringLiteral("Invalid asset regular expression: %1").arg(assetPattern.errorString());
        return result;
    }
    QList<QJsonObject> orderedReleases;
    const auto appendMatching = [&](const std::optional<bool> prereleaseFilter) {
        for (const auto &value : releases) {
            const auto release = value.toObject();
            if (release.value(QStringLiteral("draft")).toBool()) continue;
            if (!requestedTag.isEmpty() &&
                release.value(QStringLiteral("tag_name")).toString() != requestedTag) continue;
            if (prereleaseFilter.has_value() &&
                release.value(QStringLiteral("prerelease")).toBool() != *prereleaseFilter) {
                continue;
            }
            orderedReleases.append(release);
        }
    };
    if (!requestedTag.isEmpty() || current.update.githubIncludePrereleases) {
        appendMatching(std::nullopt);
    } else {
        // Stable-first with a prerelease fallback lets a project bootstrap before its
        // first stable release without permanently pinning it to the preview channel.
        appendMatching(false);
        appendMatching(true);
    }

    for (const auto &release : orderedReleases) {
        const bool prerelease = release.value(QStringLiteral("prerelease")).toBool();
        QList<QJsonObject> matches;
        QStringList sidecarMatches;
        result.availableAssets.clear();
        result.matchingAssets.clear();
        QList<QJsonObject> artifacts;
        for (const auto &assetValue : release.value(QStringLiteral("assets")).toArray()) {
            artifacts.append(assetValue.toObject());
        }
        appendSourceArchive(artifacts, release, current, QStringLiteral("tarball_url"),
                            QStringLiteral("Source code (tar.gz)"),
                            QStringLiteral("tar.gz"));
        appendSourceArchive(artifacts, release, current, QStringLiteral("zipball_url"),
                            QStringLiteral("Source code (zip)"), QStringLiteral("zip"));
        for (const auto &asset : artifacts) {
            const auto name = asset.value(QStringLiteral("name")).toString();
            result.availableAssets.append(name);
            const auto match = assetPattern.match(name);
            if (match.hasMatch() && match.capturedLength() == name.size()) {
                if (isSidecarAsset(name)) {
                    sidecarMatches.append(name);
                    continue;
                }
                matches.append(asset);
                result.matchingAssets.append(name);
            }
        }
        if (matches.isEmpty()) {
            if (!sidecarMatches.isEmpty()) {
                result.message = sidecarMatches.size() == 1
                    ? QStringLiteral("The GitHub asset rule selected verification sidecar '%1', not an installable package artifact")
                          .arg(sidecarMatches.first())
                    : QStringLiteral("The GitHub asset rule selected only verification sidecars, not an installable package artifact");
                if (error != nullptr) *error = result.message;
                return result;
            }
            continue;
        }
        if (matches.size() != 1) {
            result.message = QStringLiteral("Release %1 has %2 artifacts matching /%3/; exactly one is required")
                                 .arg(release.value(QStringLiteral("tag_name")).toString())
                                 .arg(matches.size())
                                 .arg(current.update.githubAssetRegex);
            if (error != nullptr) *error = result.message;
            return result;
        }
        const auto asset = matches.first();
        result.success = true;
        result.releaseId = static_cast<qint64>(release.value(QStringLiteral("id")).toDouble());
        result.assetId = static_cast<qint64>(asset.value(QStringLiteral("id")).toDouble());
        result.tag = release.value(QStringLiteral("tag_name")).toString();
        result.detectedVersion = versionFromTag(result.tag);
        result.filename = asset.value(QStringLiteral("pacsmith_filename")).toString();
        if (result.filename.isEmpty()) {
            result.filename = asset.value(QStringLiteral("name")).toString();
        }
        result.downloadUrl = asset.value(QStringLiteral("browser_download_url")).toString();
        result.publisherDigest = asset.value(QStringLiteral("digest")).toString();
        if (result.publisherDigest.startsWith(QStringLiteral("sha256:"))) {
            result.sha256 = result.publisherDigest.mid(7).toLower();
        }
        result.prerelease = prerelease;
        const auto currentProviderRelease = current.acquisition.githubReleaseId > 0
            ? current.acquisition.githubReleaseId : current.update.githubReleaseId;
        const bool differentProviderRelease = currentProviderRelease > 0 && result.releaseId > 0 &&
                                              currentProviderRelease != result.releaseId;
        const bool currentIsPrerelease = current.acquisition.githubPrerelease ||
                                         tagLooksLikePrerelease(current.acquisition.githubTag);
        const bool promotesMatchingPrerelease = !prerelease && currentIsPrerelease &&
            differentProviderRelease &&
            !numericVersionCore(current.debian.version).isEmpty() &&
            numericVersionCore(current.debian.version) == numericVersionCore(result.detectedVersion);
        result.updateAvailable = DebianVersion::compare(result.detectedVersion,
                                                        current.debian.version) > 0 ||
                                 promotesMatchingPrerelease;
        const auto fallback = prerelease && requestedTag.isEmpty() &&
                              !current.update.githubIncludePrereleases;
        result.message = result.updateAvailable
            ? fallback
                ? QStringLiteral("No matching stable release is available; GitHub prerelease %1 is available (%2)")
                      .arg(result.tag, result.filename)
                : QStringLiteral("GitHub release %1 is available (%2)").arg(result.tag, result.filename)
            : fallback
                ? QStringLiteral("No matching stable release is available; GitHub prerelease %1 is current")
                      .arg(result.tag)
                : QStringLiteral("GitHub release %1 is current").arg(result.tag);
        return result;
    }
    result.success = true;
    result.message = QStringLiteral("No published GitHub release has an artifact matching /%1/")
                         .arg(current.update.githubAssetRegex);
    return result;
}

void GitHubUpdateService::finishReply() {
    if (reply_ == nullptr) return;
    auto *reply = reply_;
    reply_ = nullptr;
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    UpdateCheckResult result;
    result.supported = true;
    result.etag = QString::fromUtf8(reply->rawHeader("etag"));
    if (status == 304) {
        result.success = true;
        result.detectedVersion = current_.update.detectedVersion;
        result.filename = current_.update.detectedFilename;
        result.sha256 = current_.update.detectedSha256;
        result.downloadUrl = current_.update.detectedUrl;
        result.releaseId = current_.update.githubReleaseId;
        result.assetId = current_.update.githubAssetId;
        result.tag = current_.update.githubTag;
        result.publisherDigest = current_.update.githubPublisherDigest;
        result.message = QStringLiteral("GitHub release metadata has not changed");
    } else if (reply->error() != QNetworkReply::NoError) {
        const auto body = QString::fromUtf8(reply->readAll()).left(2048);
        result.message = QStringLiteral("GitHub request failed (HTTP %1): %2%3%4")
                             .arg(status).arg(reply->errorString(), rateLimitSuffix(reply),
                                              body.isEmpty() ? QString{} : QStringLiteral(" — %1").arg(body));
    } else {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError ||
            (!document.isArray() && !(document.isObject() && !requestedTag_.isEmpty()))) {
            result.message = QStringLiteral("GitHub returned invalid release metadata: %1")
                                 .arg(parseError.errorString());
        } else {
            QString selectionError;
            const auto releases = document.isArray()
                ? document.array() : QJsonArray{document.object()};
            result = selectRelease(releases, current_, &selectionError, requestedTag_);
            result.etag = QString::fromUtf8(reply->rawHeader("etag"));
            if (!selectionError.isEmpty() && result.message.isEmpty()) result.message = selectionError;
        }
    }
    reply->deleteLater();
    requestedTag_.clear();
    complete(result);
}

void GitHubUpdateService::complete(const UpdateCheckResult &result) {
    running_.store(false, std::memory_order_release);
    emit finished(result);
}

} // namespace pacsmith
