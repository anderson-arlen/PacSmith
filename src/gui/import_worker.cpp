#include "gui/import_worker.hpp"

#include "core/project_store/project_store.hpp"

#include <QElapsedTimer>
#include <QFileInfo>

#include <exception>

namespace pacsmith::gui {

ImportWorker::ImportWorker(std::filesystem::path projectsRoot, QString sourcePath,
                           ImportOptions options, QObject *parent)
    : QObject(parent), projectsRoot_(std::move(projectsRoot)), sourcePath_(std::move(sourcePath)),
      options_(std::move(options)) {}

QString ImportWorker::descriptionFor(const ImportProgress &progress) const {
    const auto filename = QFileInfo(sourcePath_).fileName();
    switch (progress.stage) {
    case ImportStage::ValidatingSource:
        return QStringLiteral("Validating %1…").arg(filename);
    case ImportStage::ReadingDebContainer:
        return QStringLiteral("Opening Debian archive…");
    case ImportStage::ReadingControlArchive:
        return QStringLiteral("Reading metadata and maintainer scripts…");
    case ImportStage::ReadingPayloadArchive:
        return progress.entriesProcessed == 0
                   ? QStringLiteral("Inspecting package payload…")
                   : QStringLiteral("Inspecting package payload… %1 entries").arg(progress.entriesProcessed);
    case ImportStage::PreparingProject:
        return QStringLiteral("Preparing persistent project…");
    case ImportStage::CopyingSource:
        return QStringLiteral("Copying vendor source package…");
    case ImportStage::GeneratingPkgbuild:
        return QStringLiteral("Generating PKGBUILD…");
    case ImportStage::SavingProject:
        return QStringLiteral("Saving project…");
    }
    return QStringLiteral("Importing package…");
}

void ImportWorker::run() {
    try {
        ProjectStore store(projectsRoot_);
        QString error;
        QElapsedTimer progressTimer;
        QString lastDescription;
        const auto project = store.importSource(
            std::filesystem::path(sourcePath_.toUtf8().constData()), options_, &error,
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
        emit completed(project ? project->project.id : QString{},
                       project ? project->releaseId : QString{}, error);
    } catch (const std::exception &exception) {
        emit completed({}, {}, QStringLiteral("Import failed: %1").arg(QString::fromLocal8Bit(exception.what())));
    } catch (...) {
        emit completed({}, {}, QStringLiteral("Import failed because of an unexpected error"));
    }
}

} // namespace pacsmith::gui
