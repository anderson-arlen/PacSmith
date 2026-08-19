#pragma once

#include "core/apt_repository.hpp"
#include "core/model.hpp"
#include "core/update_source.hpp"

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

#include <atomic>
#include <filesystem>

class QNetworkReply;

namespace pacsmith {

class AptSignatureVerifier final {
public:
    [[nodiscard]] static bool verifyInRelease(const QByteArray &data,
                                              const QString &keyring,
                                              const QStringList &allowedFingerprints,
                                              QString &error);
    [[nodiscard]] static bool verifyDetachedRelease(const QByteArray &release,
                                                    const QByteArray &signature,
                                                    const QString &keyring,
                                                    const QStringList &allowedFingerprints,
                                                    QString &error);
};

class AptUpdateService final : public QObject {
    Q_OBJECT
public:
    explicit AptUpdateService(QObject *parent = nullptr);

    [[nodiscard]] bool isRunning() const noexcept;
    Q_INVOKABLE void start(const PackageRelease &release, const std::filesystem::path &releaseDirectory);
    Q_INVOKABLE void cancel();

signals:
    void progressChanged(const QString &message);
    void finished(const pacsmith::UpdateCheckResult &result);

private:
    enum class RequestKind { InRelease, Release, ReleaseSignature, Packages };

    void fetch(const QUrl &url, RequestKind kind, qsizetype maximumBytes);
    void requestFinished(QNetworkReply *reply, RequestKind kind);
    void processRelease(const QByteArray &data, bool inRelease);
    void processPackages(const QByteArray &data);
    [[nodiscard]] bool verifyInRelease(const QByteArray &data, QString &error) const;
    [[nodiscard]] bool verifyDetachedRelease(const QByteArray &release, const QByteArray &signature,
                                             QString &error) const;
    [[nodiscard]] QStringList allowedSigningFingerprints() const;
    [[nodiscard]] QString resolvedKeyring(QString &error) const;
    void complete(const UpdateCheckResult &result);
    void fail(const QString &message);

    std::atomic<bool> running_{false};
    QNetworkAccessManager network_;
    QNetworkReply *reply_{nullptr};
    PackageRelease project_;
    std::filesystem::path projectDirectory_;
    QUrl repositoryBase_;
    QUrl releaseRoot_;
    bool flatRepository_{false};
    bool cancelled_{false};
    bool responseTooLarge_{false};
    qsizetype maximumBytes_{0};
    QByteArray response_;
    QByteArray releaseData_;
    AptReleaseEntry packagesIndex_;
    bool signatureVerified_{false};
};

} // namespace pacsmith
