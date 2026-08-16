#pragma once

#include <QCryptographicHash>
#include <QObject>
#include <QSaveFile>
#include <QUrl>

#include <filesystem>

class QNetworkAccessManager;
class QNetworkReply;

namespace pacsmith {

class DebDownloadService final : public QObject {
    Q_OBJECT
public:
    explicit DebDownloadService(QObject *parent = nullptr);
    [[nodiscard]] bool isRunning() const noexcept;
    void start(const QUrl &url, const QString &expectedSha256,
               const std::filesystem::path &targetPath);
    void cancel();

signals:
    void progress(qint64 received, qint64 total);
    void finished(const QString &absolutePath);
    void failed(const QString &message);

private:
    void finishReply();
    void fail(const QString &message);

    QNetworkAccessManager *network_{nullptr};
    QNetworkReply *reply_{nullptr};
    QSaveFile output_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    QString expectedSha256_;
    QString targetPath_;
    qint64 received_{0};
};

// Compatibility alias while callers migrate away from the original DEB-only name.
using ArtifactDownloadService = DebDownloadService;

[[nodiscard]] QString defaultDownloadPath(const QString &projectId,
                                          const QString &releaseId,
                                          const QString &filename);

} // namespace pacsmith
