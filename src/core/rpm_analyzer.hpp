#pragma once

#include "core/model.hpp"

#include <filesystem>
#include <optional>

namespace pacsmith {

struct RpmAnalysis {
    DebianMetadata metadata; // Generic package metadata view shared by the UI.
    QList<DependencyMapping> dependencies;
    QList<MaintainerScript> maintainerScripts;
    QList<ScriptFinding> scriptFindings;
    QMap<QString, QString> fileCapabilities;
};

// Parses only the RPM lead and header stores. Package code and lifecycle
// scripts are never executed. Payload traversal remains the responsibility of
// SourceAnalyzer/libarchive so the usual path-safety checks are applied.
class RpmAnalyzer final {
public:
    [[nodiscard]] static std::optional<RpmAnalysis> analyzeHeader(
        const std::filesystem::path &path, QString *error = nullptr);
};

} // namespace pacsmith
