#include "core/deb_download_service.hpp"

#include "core/path_safety.hpp"

#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

namespace pacsmith {

DebDownloadService::DebDownloadService(QObject *parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {}

bool DebDownloadService::isRunning() const noexcept { return reply_ != nullptr; }

void DebDownloadService::start(const QUrl &url, const QString &expectedSha256,
                               const std::filesystem::path &targetPath) {
    if (isRunning()) {
        emit failed(QStringLiteral("A source artifact download is already running"));
        return;
    }
    if (!url.isValid() || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http"))) {
        emit failed(QStringLiteral("The vendor package URL is invalid"));
        return;
    }
    static const QRegularExpression shaFormat(QStringLiteral("^[0-9a-fA-F]{64}$"));
    if (!expectedSha256.isEmpty() && !shaFormat.match(expectedSha256).hasMatch()) {
        emit failed(QStringLiteral("The expected SHA256 is invalid"));
        return;
    }
    targetPath_ = QString::fromUtf8(targetPath.string().c_str());
    if (!QDir{}.mkpath(QFileInfo(targetPath_).absolutePath())) {
        emit failed(QStringLiteral("Could not create the download directory"));
        return;
    }
    output_.setFileName(targetPath_);
    if (!output_.open(QIODevice::WriteOnly)) {
        emit failed(output_.errorString());
        return;
    }
    expectedSha256_ = expectedSha256.toLower();
    hash_.reset();
    received_ = 0;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    reply_ = network_->get(request);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        const auto bytes = reply_->readAll();
        received_ += bytes.size();
        // A DEB can be large, but an accidental/untrusted endless response is not allowed.
        constexpr qint64 maximumDownload = 8LL * 1024 * 1024 * 1024;
        if (received_ > maximumDownload || output_.write(bytes) != bytes.size()) {
            fail(received_ > maximumDownload ? QStringLiteral("Vendor package exceeds the 8 GiB safety limit")
                                             : output_.errorString());
            return;
        }
        hash_.addData(bytes);
    });
    connect(reply_, &QNetworkReply::downloadProgress, this, &DebDownloadService::progress);
    connect(reply_, &QNetworkReply::finished, this, &DebDownloadService::finishReply);
}

void DebDownloadService::cancel() {
    if (reply_ != nullptr) reply_->abort();
}

void DebDownloadService::finishReply() {
    if (reply_ == nullptr) return;
    const auto reply = reply_;
    reply_ = nullptr;
    const auto networkError = reply->error();
    const auto message = reply->errorString();
    reply->deleteLater();
    if (networkError != QNetworkReply::NoError) {
        output_.cancelWriting();
        emit failed(message);
        return;
    }
    const auto digest = QString::fromLatin1(hash_.result().toHex());
    if (!expectedSha256_.isEmpty() && digest != expectedSha256_) {
        output_.cancelWriting();
        emit failed(QStringLiteral("Downloaded artifact SHA256 mismatch (expected %1, received %2)")
                        .arg(expectedSha256_, digest));
        return;
    }
    if (!output_.commit()) {
        emit failed(output_.errorString());
        return;
    }
    emit finished(QFileInfo(targetPath_).absoluteFilePath());
}

void DebDownloadService::fail(const QString &message) {
    if (reply_ != nullptr) {
        auto *reply = reply_;
        reply_ = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    output_.cancelWriting();
    emit failed(message);
}

QString defaultDownloadPath(const QString &projectId, const QString &releaseId,
                            const QString &filename) {
    const auto xdg = qEnvironmentVariable("XDG_CACHE_HOME");
    const auto base = !xdg.isEmpty() && QDir::isAbsolutePath(xdg)
        ? xdg : QDir::home().filePath(QStringLiteral(".cache"));
    const auto safeFilename = PathSafety::normalizedArchivePath(filename).value_or(QStringLiteral("vendor-artifact"));
    return QDir(base).filePath(QStringLiteral("pacsmith/downloads/%1/%2/%3")
                                   .arg(projectId, releaseId, QFileInfo(safeFilename).fileName()));
}

} // namespace pacsmith
