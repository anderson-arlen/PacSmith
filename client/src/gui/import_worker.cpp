#include "gui/import_worker.hpp"

#include "core/library_client.hpp"

#include <QFileInfo>

#include <exception>

namespace pacsmith::gui {

ImportWorker::ImportWorker(std::filesystem::path, QString sourcePath,
                           ImportOptions options, QObject *parent)
    : QObject(parent), sourcePath_(std::move(sourcePath)), options_(std::move(options)) {}

QString ImportWorker::descriptionFor(const ImportProgress &progress) const {
    const auto filename = QFileInfo(sourcePath_).fileName();
    switch (progress.stage) {
    case ImportStage::ValidatingSource:
        return QStringLiteral("Uploading %1…").arg(filename);
    case ImportStage::ReadingDebContainer:
        return QStringLiteral("Opening vendor archive…");
    case ImportStage::ReadingControlArchive:
        return QStringLiteral("Reading metadata and maintainer scripts…");
    case ImportStage::ReadingPayloadArchive:
        return QStringLiteral("Inspecting package payload…");
    case ImportStage::PreparingProject:
        return QStringLiteral("Preparing project…");
    case ImportStage::CopyingSource:
        return QStringLiteral("Storing vendor source…");
    case ImportStage::GeneratingPkgbuild:
        return QStringLiteral("Generating PKGBUILD…");
    case ImportStage::SavingProject:
        return QStringLiteral("Saving project…");
    }
    return QStringLiteral("Importing package…");
}

void ImportWorker::run() {
    try {
        LibraryClient library;
        QString error;
        emit progressChanged(QStringLiteral("Uploading %1…").arg(QFileInfo(sourcePath_).fileName()));
        const auto project = library.importSource(sourcePath_, options_, &error);
        emit completed(project ? project->project.id : QString{},
                       project ? project->releaseId : QString{}, error);
    } catch (const std::exception &exception) {
        emit completed({}, {}, QStringLiteral("Import failed: %1").arg(QString::fromLocal8Bit(exception.what())));
    } catch (...) {
        emit completed({}, {}, QStringLiteral("Import failed because of an unexpected error"));
    }
}

} // namespace pacsmith::gui
