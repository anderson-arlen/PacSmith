#pragma once

#include "core/model.hpp"
#include "core/rpm_repository.hpp"
#include "core/update_source.hpp"

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

#include <atomic>
#include <filesystem>

class QNetworkReply;

namespace pacsmith {

class RpmUpdateService final : public QObject {
    Q_OBJECT
public:
    explicit RpmUpdateService(QObject *parent = nullptr);

    [[nodiscard]] bool isRunning() const noexcept;
    Q_INVOKABLE void start(const PackageRelease &release, const std::filesystem::path &releaseDirectory);
    Q_INVOKABLE void cancel();

signals:
    void progressChanged(const QString &message);
    void finished(const pacsmith::UpdateCheckResult &result);

private:
    enum class RequestKind { Repomd, Signature, Primary };

    void fetch(const QUrl &url, RequestKind kind, qsizetype maximumBytes);
    void requestFinished(QNetworkReply *reply, RequestKind kind);
    [[nodiscard]] bool verifyRepomdSignature(const QByteArray &signature, QString &error) const;
    [[nodiscard]] QString resolvedKeyring(QString &error) const;
    [[nodiscard]] QStringList allowedSigningFingerprints() const;
    void processPrimary(const QByteArray &compressed);
    void complete(const UpdateCheckResult &result);
    void fail(const QString &message);

    std::atomic<bool> running_{false};
    QNetworkAccessManager network_;
    QNetworkReply *reply_{nullptr};
    PackageRelease release_;
    std::filesystem::path releaseDirectory_;
    QUrl repositoryBase_;
    QByteArray response_;
    QByteArray repomd_;
    RpmRepomdPrimary primary_;
    qsizetype maximumBytes_{0};
    bool responseTooLarge_{false};
    bool cancelled_{false};
};

} // namespace pacsmith
