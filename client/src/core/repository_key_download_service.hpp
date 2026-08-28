#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <optional>

namespace pacsmith {

struct RepositoryKeyDownload {
    QByteArray contents;
    QUrl requestedUrl;
    QUrl resolvedUrl;
};

[[nodiscard]] bool isAcceptableRepositoryKeyUrl(const QUrl &url);
[[nodiscard]] std::optional<RepositoryKeyDownload> downloadRepositorySigningKey(const QUrl &url,
                                                                               QString *error = nullptr);

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
    QObject *task_{nullptr};
};

} // namespace pacsmith
