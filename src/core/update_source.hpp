#pragma once

#include "core/model.hpp"

#include <QMetaType>
#include <QString>

#include <memory>

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
    bool prerelease{false};
    QStringList availableAssets;
    QStringList matchingAssets;
};

class UpdateSource {
public:
    virtual ~UpdateSource() = default;
    [[nodiscard]] virtual UpdateStrategy strategy() const noexcept = 0;
    [[nodiscard]] virtual UpdateCheckResult check(const PackageRelease &release) const = 0;
};

class UpdateSourceFactory final {
public:
    [[nodiscard]] static std::unique_ptr<UpdateSource> create(UpdateStrategy strategy);
};

} // namespace pacsmith

Q_DECLARE_METATYPE(pacsmith::UpdateCheckResult)
