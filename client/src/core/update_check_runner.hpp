#pragma once

#include "core/background_updates.hpp"
#include "core/library_client.hpp"

#include <QList>
#include <QString>

class QTextStream;

namespace pacsmith {

struct UpdateCheckRunResult {
    QString projectId;
    QString status;
    QString message;
    QString detectedVersion;
    bool prepared{false};
    bool built{false};
    int exitCode{0};
};

struct UpdateCheckBatchResult {
    QList<UpdateCheckRunResult> checks;
    QString error;
    int exitCode{0};
};

class UpdateCheckRunner final {
public:
    [[nodiscard]] static std::optional<ImportResult> prepareDiscovered(
        LibraryClient &library, const Project &project, const QString &releaseId,
        QTextStream &diagnostics, QString *error = nullptr,
        BackgroundUpdateState *backgroundState = nullptr);
    [[nodiscard]] static UpdateCheckRunResult run(
        LibraryClient &library, Project project, QTextStream &diagnostics,
        bool forceFullContentCheck = false,
        BackgroundUpdateState *backgroundState = nullptr);
    [[nodiscard]] static UpdateCheckBatchResult runAll(
        LibraryClient &library, QTextStream &diagnostics);
};

} // namespace pacsmith
