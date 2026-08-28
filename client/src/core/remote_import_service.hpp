#pragma once

#include "core/library_client.hpp"
#include "core/update_source.hpp"

#include <QUrl>

#include <optional>

namespace pacsmith {

struct RemoteArtifactImportResult {
    ImportResult imported;
    UpdateCheckResult source;
};

class RemoteImportService final {
public:
    [[nodiscard]] static std::optional<RemoteArtifactImportResult> importDirectUrl(
        LibraryClient &library, const QUrl &url, const QString &existingProjectId = {},
        const QString &version = {}, const QString &expectedSha256 = {},
        QString *error = nullptr);
};

} // namespace pacsmith
