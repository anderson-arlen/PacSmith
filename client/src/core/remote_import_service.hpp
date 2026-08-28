#pragma once

#include "core/library_client.hpp"
#include "core/update_source.hpp"

#include <QUrl>

#include <optional>

namespace pacsmith {

struct GitHubImportRequest {
    QString owner;
    QString repository;
    QString requestedTag;
    QString assetRegex;
    bool includePrereleases{false};
};

struct RemoteArtifactImportResult {
    ImportResult imported;
    UpdateCheckResult source;
};

class RemoteImportService final {
public:
    [[nodiscard]] static std::optional<GitHubImportRequest> parseGitHubUrl(
        const QUrl &url, const QString &assetRegex = {},
        bool includePrereleases = false, QString *error = nullptr);

    [[nodiscard]] static std::optional<RemoteArtifactImportResult> importGitHub(
        LibraryClient &library, const QUrl &url, const QString &assetRegex = {},
        bool includePrereleases = false, const QString &existingProjectId = {},
        const QString &token = {}, QString *error = nullptr);

    [[nodiscard]] static std::optional<RemoteArtifactImportResult> importDirectUrl(
        LibraryClient &library, const QUrl &url, const QString &existingProjectId = {},
        const QString &version = {}, const QString &expectedSha256 = {},
        QString *error = nullptr);
};

} // namespace pacsmith
