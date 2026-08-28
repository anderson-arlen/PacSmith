#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace pacsmith {

struct UpdateCheckResult {
    bool supported{false};
    bool success{false};
    bool updateAvailable{false};
    QString detectedVersion;
    QString filename;
    QString sha256;
    QString downloadUrl;
    bool signatureVerified{false};
    QString message;
    qint64 releaseId{0};
    qint64 assetId{0};
    QString tag;
    QString publisherDigest;
    QString etag;
    QString directUrlEtag;
    QString directUrlLastModified;
    qint64 directUrlContentLength{-1};
    QString directUrlVendorValidatorName;
    QString directUrlVendorValidator;
    QString directUrlLastSha256;
    QDateTime directUrlLastFullCheck;
    QString localArtifactPath;
    bool fullContentCheckDeferred{false};
    bool prerelease{false};
    QStringList availableAssets;
    QStringList matchingAssets;
};

} // namespace pacsmith

Q_DECLARE_METATYPE(pacsmith::UpdateCheckResult)
