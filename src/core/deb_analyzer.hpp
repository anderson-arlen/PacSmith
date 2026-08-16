#pragma once

#include "core/import_progress.hpp"
#include "core/model.hpp"
#include "core/script_evidence.hpp"

#include <QByteArray>
#include <QString>

#include <filesystem>
#include <optional>

namespace pacsmith {

struct ExtractedPackageIcon {
    QString sourcePath;
    QByteArray contents;
};

struct DebAnalysis {
    DebianMetadata metadata;
    QList<DependencyMapping> dependencies;
    QList<MaintainerScript> maintainerScripts;
    QList<PayloadEntry> payload;
    QList<PayloadRule> payloadRules;
    QStringList updateCandidates;
    QList<AptRepositoryCandidate> aptCandidates;
    QList<RpmRepositoryCandidate> rpmCandidates;
    QList<ScriptFinding> scriptFindings;
    QList<ExtractedSigningKey> signingKeys;
    std::optional<ExtractedPackageIcon> icon;
    InstallMapping installMapping;
};

struct AnalysisError {
    QString message;
};

class DebAnalyzer final {
public:
    [[nodiscard]] std::optional<DebAnalysis> analyze(const std::filesystem::path &path,
                                                     AnalysisError &error,
                                                     const ImportProgressCallback &progress = {}) const;
};

} // namespace pacsmith
