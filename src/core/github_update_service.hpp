#pragma once

#include "core/model.hpp"
#include "core/update_source.hpp"

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>

#include <atomic>

class QNetworkReply;

namespace pacsmith {

class GitHubUpdateService final : public QObject {
    Q_OBJECT
public:
    explicit GitHubUpdateService(QObject *parent = nullptr);

    [[nodiscard]] bool isRunning() const noexcept;
    Q_INVOKABLE void start(const PackageRelease &release, const QString &token = {},
                           const QString &requestedTag = {});
    Q_INVOKABLE void cancel();

    [[nodiscard]] static UpdateCheckResult selectRelease(
        const QJsonArray &releases, const PackageRelease &current, QString *error = nullptr,
        const QString &requestedTag = {});

signals:
    void progressChanged(const QString &message);
    void finished(const pacsmith::UpdateCheckResult &result);

private:
    void finishReply();
    void complete(const UpdateCheckResult &result);

    std::atomic<bool> running_{false};
    QNetworkAccessManager network_;
    QNetworkReply *reply_{nullptr};
    PackageRelease current_;
    QString requestedTag_;
};

} // namespace pacsmith
