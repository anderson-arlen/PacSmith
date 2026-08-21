#pragma once

#include <QByteArray>
#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace pacsmith {

[[nodiscard]] bool isAcceptableRepositoryKeyUrl(const QUrl &url);

class RepositoryKeyDownloadService final : public QObject {
    Q_OBJECT
public:
    explicit RepositoryKeyDownloadService(QObject *parent = nullptr);

    [[nodiscard]] bool isRunning() const noexcept;
    void start(const QUrl &url);
    void provide(const QByteArray &contents, const QUrl &source);
    void cancel();

signals:
    void progress(qint64 received, qint64 total);
    void finished(const QByteArray &contents, const QUrl &requestedUrl,
                  const QUrl &resolvedUrl);
    void failed(const QString &message);

private:
    void finishReply();
    void fail(const QString &message);

    QNetworkAccessManager *network_{nullptr};
    QNetworkReply *reply_{nullptr};
    QByteArray contents_;
    QUrl requestedUrl_;
};

} // namespace pacsmith
