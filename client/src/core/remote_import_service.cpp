#include "core/remote_import_service.hpp"

#include <QFileInfo>

namespace pacsmith {
namespace {

bool validHttpsUrl(const QUrl &url) {
    return url.isValid() && url.scheme() == QStringLiteral("https") &&
           !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment();
}

} // namespace

std::optional<RemoteArtifactImportResult> RemoteImportService::importDirectUrl(
    LibraryClient &library, const QUrl &url, const QString &existingProjectId,
    const QString &version, const QString &expectedSha256, QString *error) {
    if (!validHttpsUrl(url)) {
        if (error != nullptr) {
            *error = QStringLiteral("Direct artifact URL must be HTTPS without credentials or a fragment");
        }
        return std::nullopt;
    }
    const auto imported = library.importRemoteUrl(
        url, existingProjectId, version, expectedSha256, error);
    if (!imported) return std::nullopt;
    UpdateCheckResult source;
    source.success = true;
    source.supported = true;
    source.detectedVersion = version;
    source.filename = QFileInfo(url.path()).fileName();
    source.downloadUrl = url.toString();
    source.publisherDigest = expectedSha256.trimmed().toLower();
    return RemoteArtifactImportResult{*imported, source};
}

} // namespace pacsmith
