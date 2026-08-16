#pragma once

#include "core/model.hpp"

#include <QByteArray>
#include <QList>

namespace pacsmith {

struct ExtractedSigningKey {
    QByteArray contents;
    QString sourcePath;
    QString sourceFingerprint;
};

struct ScriptEvidence {
    QList<ScriptFinding> findings;
    QList<AptRepositoryCandidate> aptCandidates;
    QList<RpmRepositoryCandidate> rpmCandidates;
    QList<ExtractedSigningKey> signingKeys;
};

class ScriptEvidenceAnalyzer final {
public:
    [[nodiscard]] static ScriptEvidence analyze(const QList<MaintainerScript> &scripts);
};

} // namespace pacsmith
