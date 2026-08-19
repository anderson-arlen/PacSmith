#include "gui/reanalyze_worker.hpp"

#include "core/project_store.hpp"

#include <QElapsedTimer>

#include <exception>

namespace pacsmith::gui {

ReanalyzeWorker::ReanalyzeWorker(std::filesystem::path projectsRoot,
                                 QString projectId, QString releaseId,
                                 QObject *parent)
    : QObject(parent), projectsRoot_(std::move(projectsRoot)),
      projectId_(std::move(projectId)), releaseId_(std::move(releaseId)) {}

QString ReanalyzeWorker::descriptionFor(const ImportProgress &progress) {
    switch (progress.stage) {
    case ImportStage::ValidatingSource:
        return QStringLiteral("Verifying the stored artifact…");
    case ImportStage::ReadingDebContainer:
        return QStringLiteral("Opening the vendor package…");
    case ImportStage::ReadingControlArchive:
        return QStringLiteral("Re-reading metadata and package scripts…");
    case ImportStage::ReadingPayloadArchive:
        return progress.entriesProcessed == 0
            ? QStringLiteral("Re-inspecting the artifact payload…")
            : QStringLiteral("Re-inspecting the artifact payload… %1 entries")
                  .arg(progress.entriesProcessed);
    case ImportStage::PreparingProject:
        return QStringLiteral("Resetting package setup decisions…");
    case ImportStage::CopyingSource:
        return QStringLiteral("Verifying stored source bytes…");
    case ImportStage::GeneratingPkgbuild:
        return QStringLiteral("Regenerating the PKGBUILD…");
    case ImportStage::SavingProject:
        return QStringLiteral("Saving the reset release…");
    }
    return QStringLiteral("Reanalyzing artifact…");
}

void ReanalyzeWorker::run() {
    try {
        ProjectStore store(projectsRoot_);
        QString error;
        QElapsedTimer progressTimer;
        QString lastDescription;
        const auto result = store.reanalyzeRelease(
            projectId_, releaseId_, &error,
            [this, &progressTimer, &lastDescription](const ImportProgress &progress) {
                const auto description = descriptionFor(progress);
                if (description == lastDescription && progressTimer.isValid() &&
                    progressTimer.elapsed() < 100) {
                    return;
                }
                lastDescription = description;
                progressTimer.restart();
                emit progressChanged(description);
            });
        emit completed(result ? result->project.id : QString{},
                       result ? result->releaseId : QString{}, error);
    } catch (const std::exception &exception) {
        emit completed({}, {}, QStringLiteral("Reanalysis failed: %1")
                                   .arg(QString::fromLocal8Bit(exception.what())));
    } catch (...) {
        emit completed({}, {}, QStringLiteral(
                                   "Reanalysis failed because of an unexpected error"));
    }
}

} // namespace pacsmith::gui
