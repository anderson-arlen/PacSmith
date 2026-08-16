#pragma once

#include "core/model.hpp"

#include <filesystem>
#include <optional>

namespace pacsmith {

// Reads Arch package metadata as archive data. Nothing from the package is executed.
class PackageArtifactInspector final {
public:
    [[nodiscard]] static std::optional<PackageArtifact> inspect(
        const std::filesystem::path &packagePath,
        const std::filesystem::path &releaseDirectory,
        QString *error = nullptr);
};

[[nodiscard]] BuildRecord buildRecordFromResult(const QString &id,
                                                 BuildStatus status,
                                                 const QString &log,
                                                 const QStringList &packagePaths,
                                                 const std::filesystem::path &releaseDirectory,
                                                 const QDateTime &startedAt,
                                                 const QDateTime &finishedAt);

} // namespace pacsmith
