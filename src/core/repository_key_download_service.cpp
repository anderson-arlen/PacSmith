#include "core/repository_key_download_service.hpp"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <utility>

namespace pacsmith {
namespace {

constexpr qsizetype maximumSigningKeySize = 4 * 1024 * 1024;

} // namespace

bool isAcceptableRepositoryKeyUrl(const QUrl &url) {
    return url.isValid() && url.scheme() == QStringLiteral("https") && !url.host().isEmpty() &&
           url.userInfo().isEmpty() && !url.hasFragment();
}

RepositoryKeyDownloadService::RepositoryKeyDownloadService(QObject *parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {}

bool RepositoryKeyDownloadService::isRunning() const noexcept { return reply_ != nullptr; }

void RepositoryKeyDownloadService::start(const QUrl &url) {
    if (isRunning()) {
        emit failed(QStringLiteral("A signing-key download is already running"));
        return;
    }
    if (!isAcceptableRepositoryKeyUrl(url)) {
        emit failed(QStringLiteral(
            "Enter an HTTPS signing-key URL without embedded credentials or a fragment"));
        return;
    }

    contents_.clear();
    requestedUrl_ = url;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30'000);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("PacSmith repository-key downloader"));
    reply_ = network_->get(request);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        if (reply_ == nullptr) return;
        const auto bytes = reply_->readAll();
        if (contents_.size() + bytes.size() > maximumSigningKeySize) {
            fail(QStringLiteral("Signing key exceeds the 4 MiB safety limit"));
            return;
        }
        contents_.append(bytes);
    });
    connect(reply_, &QNetworkReply::downloadProgress, this,
            &RepositoryKeyDownloadService::progress);
    connect(reply_, &QNetworkReply::finished, this,
            &RepositoryKeyDownloadService::finishReply);
}

void RepositoryKeyDownloadService::provide(const QByteArray &contents, const QUrl &source) {
    if (isRunning()) {
        emit failed(QStringLiteral("A signing-key download is already running"));
        return;
    }
    if (contents.isEmpty()) {
        emit failed(QStringLiteral("The supplied signing key is empty"));
        return;
    }
    if (contents.size() > maximumSigningKeySize) {
        emit failed(QStringLiteral("Signing key exceeds the 4 MiB safety limit"));
        return;
    }
    emit finished(contents, source, source);
}

void RepositoryKeyDownloadService::cancel() {
    if (reply_ != nullptr) fail(QStringLiteral("Signing-key download canceled"));
}

void RepositoryKeyDownloadService::finishReply() {
    if (reply_ == nullptr) return;
    auto *reply = reply_;
    reply_ = nullptr;
    contents_.append(reply->readAll());
    const auto networkError = reply->error();
    const auto errorText = reply->errorString();
    const auto resolvedUrl = reply->url();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError) {
        contents_.clear();
        requestedUrl_.clear();
        emit failed(errorText);
        return;
    }
    if (contents_.isEmpty()) {
        requestedUrl_.clear();
        emit failed(QStringLiteral("The signing-key URL returned an empty response"));
        return;
    }
    if (contents_.size() > maximumSigningKeySize) {
        contents_.clear();
        requestedUrl_.clear();
        emit failed(QStringLiteral("Signing key exceeds the 4 MiB safety limit"));
        return;
    }
    if (!isAcceptableRepositoryKeyUrl(resolvedUrl)) {
        contents_.clear();
        requestedUrl_.clear();
        emit failed(QStringLiteral("The signing-key download resolved to an unacceptable URL"));
        return;
    }

    const auto contents = std::exchange(contents_, {});
    const auto requestedUrl = std::exchange(requestedUrl_, {});
    emit finished(contents, requestedUrl, resolvedUrl);
}

void RepositoryKeyDownloadService::fail(const QString &message) {
    if (reply_ != nullptr) {
        auto *reply = reply_;
        reply_ = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    contents_.clear();
    requestedUrl_.clear();
    emit failed(message);
}

} // namespace pacsmith
