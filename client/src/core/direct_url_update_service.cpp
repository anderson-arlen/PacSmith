#include "core/direct_url_update_service.hpp"

#include "core/path_safety.hpp"
#include "core/project_store/internal.hpp"
#include "core/project_store/project_store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

#include <filesystem>

namespace pacsmith {
namespace {

QString responseFilename(const QNetworkReply &reply, const PackageRelease &release) {
    const auto disposition = QString::fromUtf8(reply.rawHeader("content-disposition"));
    static const QRegularExpression encoded(
        QStringLiteral("filename\\*\\s*=\\s*UTF-8''([^;]+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression plain(
        QStringLiteral("filename\\s*=\\s*\\\"?([^;\\\"]+)"),
        QRegularExpression::CaseInsensitiveOption);
    auto match = encoded.match(disposition);
    QString filename;
    if (match.hasMatch()) {
        filename = QUrl::fromPercentEncoding(match.captured(1).trimmed().toUtf8());
    } else {
        match = plain.match(disposition);
        if (match.hasMatch()) filename = match.captured(1).trimmed();
    }
    if (filename.isEmpty()) {
        filename = QFileInfo(reply.url().path()).fileName();
    }
    if (filename.isEmpty()) filename = release.originalSourceFilename;
    return PathSafety::normalizedArchivePath(filename)
        .transform([](const QString &safe) { return QFileInfo(safe).fileName(); })
        .value_or(QStringLiteral("vendor-artifact"));
}

DirectUrlValidators responseValidators(const QNetworkReply &reply) {
    DirectUrlValidators result;
    result.etag = QString::fromUtf8(reply.rawHeader("etag")).trimmed();
    result.lastModified = QString::fromUtf8(reply.rawHeader("last-modified")).trimmed();
    bool lengthValid = false;
    const auto length = reply.rawHeader("content-length").toLongLong(&lengthValid);
    if (lengthValid && length >= 0) result.contentLength = length;
    for (const auto &name : {QByteArrayLiteral("x-amz-version-id"),
                             QByteArrayLiteral("x-goog-generation")}) {
        const auto value = QString::fromUtf8(reply.rawHeader(name)).trimmed();
        if (value.isEmpty()) continue;
        result.vendorName = QString::fromLatin1(name);
        result.vendorValue = value;
        break;
    }
    return result;
}

} // namespace

bool DirectUrlValidators::available() const {
    return !etag.isEmpty() || !lastModified.isEmpty() ||
           (!vendorName.isEmpty() && !vendorValue.isEmpty());
}

DirectUrlUpdateService::DirectUrlUpdateService(QObject *parent)
    : QObject(parent), network_(this), downloader_(this) {
    connect(&downloader_, &ArtifactDownloadService::progress, this,
            &DirectUrlUpdateService::downloadProgress);
    connect(&downloader_, &ArtifactDownloadService::finished, this,
            &DirectUrlUpdateService::finishDownload);
    connect(&downloader_, &ArtifactDownloadService::failed, this,
            &DirectUrlUpdateService::fail);
}

bool DirectUrlUpdateService::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

DirectUrlValidators DirectUrlUpdateService::storedValidators(
    const UpdateConfiguration &update) {
    return {update.directUrlEtag, update.directUrlLastModified,
            update.directUrlContentLength, update.directUrlVendorValidatorName,
            update.directUrlVendorValidator};
}

DirectUrlValidatorComparison DirectUrlUpdateService::compareValidators(
    const DirectUrlValidators &stored, const DirectUrlValidators &remote) {
    if (!stored.etag.isEmpty() && !remote.etag.isEmpty()) {
        return stored.etag == remote.etag ? DirectUrlValidatorComparison::Unchanged
                                          : DirectUrlValidatorComparison::Changed;
    }
    if (!stored.vendorName.isEmpty() && stored.vendorName == remote.vendorName &&
        !stored.vendorValue.isEmpty() && !remote.vendorValue.isEmpty()) {
        return stored.vendorValue == remote.vendorValue
                   ? DirectUrlValidatorComparison::Unchanged
                   : DirectUrlValidatorComparison::Changed;
    }
    if (!stored.lastModified.isEmpty() && !remote.lastModified.isEmpty()) {
        const bool sameLength = stored.contentLength < 0 || remote.contentLength < 0 ||
                                stored.contentLength == remote.contentLength;
        return stored.lastModified == remote.lastModified && sameLength
                   ? DirectUrlValidatorComparison::Unchanged
                   : DirectUrlValidatorComparison::Changed;
    }
    return DirectUrlValidatorComparison::NoCommonValidator;
}

bool DirectUrlUpdateService::fullContentCheckDue(const UpdateConfiguration &update,
                                                  const QDateTime &now) {
    if (update.directUrlFullCheckIntervalHours <= 0) return false;
    if (!update.directUrlLastFullCheck.isValid()) return true;
    return update.directUrlLastFullCheck.secsTo(now) >=
           static_cast<qint64>(update.directUrlFullCheckIntervalHours) * 60 * 60;
}

void DirectUrlUpdateService::start(const PackageRelease &release,
                                   const bool forceFullContentCheck) {
    if (isRunning()) {
        UpdateCheckResult result;
        result.supported = true;
        result.message = QStringLiteral("A Direct URL update check is already running");
        emit finished(result);
        return;
    }
    const QUrl url(release.update.url, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != QStringLiteral("https") &&
                           url.scheme() != QStringLiteral("http")) ||
        url.host().isEmpty()) {
        UpdateCheckResult result;
        result.supported = true;
        result.message = QStringLiteral("Direct URL must be an absolute HTTP or HTTPS URL");
        emit finished(result);
        return;
    }
    current_ = release;
    remote_ = {};
    filename_.clear();
    forceFullContentCheck_ = forceFullContentCheck;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "PacSmith/0.2");
    request.setRawHeader("Accept-Encoding", "identity");
    const auto stored = storedValidators(release.update);
    if (!stored.etag.isEmpty()) {
        request.setRawHeader("If-None-Match", stored.etag.toUtf8());
    } else if (!stored.lastModified.isEmpty()) {
        request.setRawHeader("If-Modified-Since", stored.lastModified.toUtf8());
    }
    emit progressChanged(QStringLiteral("Checking Direct URL headers…"));
    running_.store(true, std::memory_order_release);
    reply_ = network_.head(request);
    connect(reply_, &QNetworkReply::finished, this, &DirectUrlUpdateService::finishProbe);
}

void DirectUrlUpdateService::cancel() {
    if (reply_ != nullptr) reply_->abort();
    if (downloader_.isRunning()) downloader_.cancel();
}

void DirectUrlUpdateService::finishProbe() {
    if (reply_ == nullptr) return;
    auto *reply = reply_;
    reply_ = nullptr;
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 304) {
        remote_ = storedValidators(current_.update);
        const auto observed = responseValidators(*reply);
        if (!observed.etag.isEmpty()) remote_.etag = observed.etag;
        if (!observed.lastModified.isEmpty()) remote_.lastModified = observed.lastModified;
        if (observed.contentLength >= 0) remote_.contentLength = observed.contentLength;
        if (!observed.vendorValue.isEmpty()) {
            remote_.vendorName = observed.vendorName;
            remote_.vendorValue = observed.vendorValue;
        }
        reply->deleteLater();
        auto result = observationResult();
        result.success = true;
        result.message = QStringLiteral("Direct URL validators have not changed");
        complete(std::move(result));
        return;
    }
    if (status == 405 || status == 501) {
        remote_ = {};
        filename_ = QFileInfo(QUrl(current_.update.url).path()).fileName();
        if (filename_.isEmpty()) filename_ = current_.originalSourceFilename;
        reply->deleteLater();
        evaluateProbe();
        return;
    }
    if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 400) {
        const auto message = QStringLiteral("Direct URL header check failed (HTTP %1): %2")
                                 .arg(status)
                                 .arg(reply->errorString());
        reply->deleteLater();
        fail(message);
        return;
    }
    remote_ = responseValidators(*reply);
    filename_ = responseFilename(*reply, current_);
    reply->deleteLater();
    evaluateProbe();
}

void DirectUrlUpdateService::evaluateProbe() {
    const auto comparison = compareValidators(storedValidators(current_.update), remote_);
    if (comparison == DirectUrlValidatorComparison::Unchanged) {
        auto result = observationResult();
        result.success = true;
        result.message = QStringLiteral("Direct URL validators have not changed");
        complete(std::move(result));
        return;
    }
    if (!remote_.available() && !forceFullContentCheck_ &&
        !fullContentCheckDue(current_.update, QDateTime::currentDateTimeUtc())) {
        auto result = observationResult();
        result.success = true;
        result.fullContentCheckDeferred = true;
        result.message = current_.update.directUrlFullCheckIntervalHours <= 0
            ? QStringLiteral("The server provides no cheap change validator; full-content checks are manual only")
            : QStringLiteral("The server provides no cheap change validator; the next full-content check is not due yet");
        complete(std::move(result));
        return;
    }
    startDownload();
}

void DirectUrlUpdateService::startDownload() {
    emit progressChanged(remote_.available()
                             ? QStringLiteral("Remote artifact changed; downloading for SHA256 comparison…")
                             : QStringLiteral("No cheap remote validator; downloading for SHA256 comparison…"));
    if (filename_.isEmpty()) filename_ = current_.originalSourceFilename;
    const auto checkFilename = QStringLiteral("direct-url-check-%1").arg(filename_);
    const auto target = defaultDownloadPath(current_.projectId, current_.id, checkFilename);
    downloader_.start(QUrl(current_.update.url), {},
                      std::filesystem::path(target.toUtf8().constData()));
}

void DirectUrlUpdateService::finishDownload(const QString &path) {
    QString error;
    const auto filesystemPath = std::filesystem::path(path.toUtf8().constData());
    const auto digest = sha256File(filesystemPath, &error);
    if (digest.isEmpty()) {
        static_cast<void>(QFile::remove(path));
        fail(error.isEmpty() ? QStringLiteral("Could not hash the downloaded artifact") : error);
        return;
    }
    auto result = observationResult();
    result.directUrlLastFullCheck = QDateTime::currentDateTimeUtc();
    result.directUrlLastSha256 = digest;
    const auto baseline = current_.update.directUrlLastSha256.isEmpty()
        ? current_.sourceSha256 : current_.update.directUrlLastSha256;
    if (!baseline.isEmpty() && digest == baseline) {
        static_cast<void>(QFile::remove(path));
        result.success = true;
        result.sha256 = digest;
        result.message = QStringLiteral("Direct URL artifact bytes are unchanged (SHA256 matched)");
        complete(std::move(result));
        return;
    }
    emit progressChanged(QStringLiteral("Inspecting changed vendor artifact…"));
    const auto analysis = project_store_internal::analyzeArtifact(filesystemPath, &error, {});
    if (!analysis || analysis->metadata.version.trimmed().isEmpty()) {
        static_cast<void>(QFile::remove(path));
        fail(error.isEmpty()
                 ? QStringLiteral("The changed artifact did not expose a usable package version")
                 : error);
        return;
    }
    result.success = true;
    result.updateAvailable = true;
    result.detectedVersion = analysis->metadata.version;
    result.filename = filename_;
    result.sha256 = digest;
    result.downloadUrl = current_.update.url;
    result.localArtifactPath = path;
    result.message = QStringLiteral("Direct URL artifact changed; version %1 is available")
                         .arg(result.detectedVersion);
    complete(std::move(result));
}

void DirectUrlUpdateService::fail(const QString &message) {
    UpdateCheckResult result;
    result.supported = true;
    result.message = message;
    complete(std::move(result));
}

UpdateCheckResult DirectUrlUpdateService::observationResult() const {
    UpdateCheckResult result;
    result.supported = true;
    result.directUrlEtag = remote_.etag;
    result.directUrlLastModified = remote_.lastModified;
    result.directUrlContentLength = remote_.contentLength;
    result.directUrlVendorValidatorName = remote_.vendorName;
    result.directUrlVendorValidator = remote_.vendorValue;
    return result;
}

void DirectUrlUpdateService::complete(UpdateCheckResult result) {
    running_.store(false, std::memory_order_release);
    emit finished(result);
}

QString retainDirectUrlArtifact(const UpdateCheckResult &result, const QString &projectId,
                                const QString &releaseId, QString *error) {
    if (result.localArtifactPath.isEmpty() || result.filename.isEmpty() ||
        projectId.isEmpty() || releaseId.isEmpty()) {
        return {};
    }
    const auto destination = defaultDownloadPath(projectId, releaseId, result.filename);
    if (QFileInfo(result.localArtifactPath).absoluteFilePath() ==
        QFileInfo(destination).absoluteFilePath()) {
        return destination;
    }
    if (!QDir{}.mkpath(QFileInfo(destination).absolutePath())) {
        if (error != nullptr) *error = QStringLiteral("Could not create the retained download directory");
        return {};
    }
    if (QFileInfo::exists(destination)) {
        const auto digest = sha256File(
            std::filesystem::path(destination.toUtf8().constData()), error);
        if (!result.sha256.isEmpty() && digest == result.sha256) {
            static_cast<void>(QFile::remove(result.localArtifactPath));
            return destination;
        }
        if (error != nullptr && error->isEmpty()) {
            *error = QStringLiteral("A different retained download already exists for this release");
        }
        return {};
    }
    if (QFile::rename(result.localArtifactPath, destination)) return destination;
    if (QFile::copy(result.localArtifactPath, destination)) {
        static_cast<void>(QFile::remove(result.localArtifactPath));
        return destination;
    }
    if (error != nullptr) *error = QStringLiteral("Could not retain the downloaded update artifact");
    return {};
}

} // namespace pacsmith
