#pragma once

#include "core/deb_download_service.hpp"
#include "core/model.hpp"
#include "core/update_source.hpp"

#include <QNetworkAccessManager>
#include <QObject>

#include <atomic>

class QNetworkReply;

namespace pacsmith {

struct DirectUrlValidators {
    QString etag;
    QString lastModified;
    qint64 contentLength{-1};
    QString vendorName;
    QString vendorValue;

    [[nodiscard]] bool available() const;
};

enum class DirectUrlValidatorComparison { NoCommonValidator, Unchanged, Changed };

class DirectUrlUpdateService final : public QObject {
    Q_OBJECT
public:
    explicit DirectUrlUpdateService(QObject *parent = nullptr);

    [[nodiscard]] bool isRunning() const noexcept;
    Q_INVOKABLE void start(const PackageRelease &release, bool forceFullContentCheck = false);
    Q_INVOKABLE void cancel();

    [[nodiscard]] static DirectUrlValidators storedValidators(const UpdateConfiguration &update);
    [[nodiscard]] static DirectUrlValidatorComparison compareValidators(
        const DirectUrlValidators &stored, const DirectUrlValidators &remote);
    [[nodiscard]] static bool fullContentCheckDue(const UpdateConfiguration &update,
                                                  const QDateTime &now);

signals:
    void progressChanged(const QString &message);
    void downloadProgress(qint64 received, qint64 total);
    void finished(const pacsmith::UpdateCheckResult &result);

private:
    void finishProbe();
    void evaluateProbe();
    void startDownload();
    void finishDownload(const QString &path);
    void fail(const QString &message);
    void complete(UpdateCheckResult result);
    [[nodiscard]] UpdateCheckResult observationResult() const;

    std::atomic<bool> running_{false};
    QNetworkAccessManager network_;
    ArtifactDownloadService downloader_;
    QNetworkReply *reply_{nullptr};
    PackageRelease current_;
    DirectUrlValidators remote_;
    QString filename_;
    bool forceFullContentCheck_{false};
};

[[nodiscard]] QString retainDirectUrlArtifact(const UpdateCheckResult &result,
                                              const QString &projectId,
                                              const QString &releaseId,
                                              QString *error = nullptr);

} // namespace pacsmith
