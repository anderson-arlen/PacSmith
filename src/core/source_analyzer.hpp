#pragma once

#include "core/import_progress.hpp"
#include "core/model.hpp"
#include "core/script_evidence.hpp"

#include <filesystem>
#include <optional>

namespace pacsmith {

struct ExtractedSourceIcon {
    QString sourcePath;
    QByteArray contents;
};

struct SourceAnalysis {
    SourcePackageType type{SourcePackageType::Archive};
    DebianMetadata metadata; // Generic compatibility view used by the existing UI.
    QList<DependencyMapping> dependencies;
    QList<MaintainerScript> maintainerScripts;
    QList<ScriptFinding> scriptFindings;
    QList<PayloadEntry> payload;
    QList<PayloadRule> payloadRules;
    QStringList updateCandidates;
    QList<AptRepositoryCandidate> aptCandidates;
    QList<RpmRepositoryCandidate> rpmCandidates;
    QList<ExtractedSigningKey> signingKeys;
    std::optional<ExtractedSourceIcon> icon;
    InstallMapping installMapping;
    QString upstreamArchPkgrel;
};

class SourceAnalyzer final {
public:
    [[nodiscard]] static std::optional<SourcePackageType> detect(
        const std::filesystem::path &path, QString *error = nullptr);
    [[nodiscard]] static std::optional<SourceAnalysis> analyze(
        const std::filesystem::path &path, QString *error = nullptr,
        const ImportProgressCallback &progress = {});
};

} // namespace pacsmith
