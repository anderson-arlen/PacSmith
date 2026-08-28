#include "core/repository_key_download_service.hpp"

#include "core/library_client.hpp"

#include <QEventLoop>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QtConcurrent>

namespace pacsmith {
namespace {

constexpr qsizetype maximumSigningKeySize = 4 * 1024 * 1024;

struct KeyInspectionTask {
    QByteArray contents;
    QUrl requestedUrl;
    QUrl resolvedUrl;
    QString error;
};

} // namespace

bool isAcceptableRepositoryKeyUrl(const QUrl &url) {
    return url.isValid() && url.scheme() == QStringLiteral("https") && !url.host().isEmpty() &&
           url.userInfo().isEmpty() && !url.hasFragment();
}

std::optional<RepositoryKeyDownload> downloadRepositorySigningKey(const QUrl &url, QString *error) {
    RepositoryKeyDownloadService downloader;
    RepositoryKeyDownload downloaded;
    QString failure;
    QEventLoop loop;
    QObject::connect(&downloader, &RepositoryKeyDownloadService::finished,
                     [&](const QByteArray &contents, const QUrl &requestedUrl, const QUrl &resolvedUrl) {
                         downloaded = {contents, requestedUrl, resolvedUrl};
                         loop.quit();
                     });
    QObject::connect(&downloader, &RepositoryKeyDownloadService::failed, [&](const QString &message) {
        failure = message;
        loop.quit();
    });
    downloader.start(url);
    if (downloader.isRunning()) loop.exec();
    if (downloaded.contents.isEmpty()) {
        if (error != nullptr) {
            *error = failure.isEmpty() ? QStringLiteral("The signing-key URL returned an empty response")
                                       : failure;
        }
        return std::nullopt;
    }
    return downloaded;
}

RepositoryKeyDownloadService::RepositoryKeyDownloadService(QObject *parent)
    : QObject(parent) {}

bool RepositoryKeyDownloadService::isRunning() const noexcept { return task_ != nullptr; }

void RepositoryKeyDownloadService::start(const QUrl &url) {
    if (isRunning()) {
        emit failed(QStringLiteral("A signing-key inspection is already running"));
        return;
    }
    if (!isAcceptableRepositoryKeyUrl(url)) {
        emit failed(QStringLiteral(
            "Enter an HTTPS signing-key URL without embedded credentials or a fragment"));
        return;
    }

    auto *watcher = new QFutureWatcher<KeyInspectionTask>(this);
    task_ = watcher;
    emit progress(0, -1);
    connect(watcher, &QFutureWatcher<KeyInspectionTask>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (task_ != watcher) return;
        task_ = nullptr;
        if (!result.error.isEmpty() || result.contents.isEmpty()) {
            emit failed(result.error.isEmpty()
                            ? QStringLiteral("The signing-key URL returned an empty response")
                            : result.error);
            return;
        }
        emit progress(result.contents.size(), result.contents.size());
        emit finished(result.contents, result.requestedUrl, result.resolvedUrl);
    });
    watcher->setFuture(QtConcurrent::run([url] {
        KeyInspectionTask result;
        QString error;
        const auto inspected = LibraryClient().inspectRepositoryKey(url, &error);
        if (!inspected) {
            result.error = error;
            return result;
        }
        result.contents = QByteArray::fromBase64(
            inspected->value(QStringLiteral("contents")).toString().toLatin1());
        result.requestedUrl = QUrl(inspected->value(QStringLiteral("requested_url")).toString());
        result.resolvedUrl = QUrl(inspected->value(QStringLiteral("resolved_url")).toString());
        if (result.contents.isEmpty()) result.error = QStringLiteral("The daemon returned an empty signing key");
        return result;
    }));
}

void RepositoryKeyDownloadService::provide(const QByteArray &contents, const QUrl &source) {
    if (isRunning()) {
        emit failed(QStringLiteral("A signing-key inspection is already running"));
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
    if (task_ == nullptr) return;
    auto *watcher = static_cast<QFutureWatcher<KeyInspectionTask> *>(task_);
    task_ = nullptr;
    disconnect(watcher, nullptr, this, nullptr);
    connect(watcher, &QFutureWatcher<KeyInspectionTask>::finished,
            watcher, &QObject::deleteLater);
    watcher->cancel();
    emit failed(QStringLiteral("Signing-key inspection canceled"));
}

} // namespace pacsmith
