#include "gui/reanalyze_worker.hpp"

#include "core/library_client.hpp"

#include <exception>

namespace pacsmith::gui {

ReanalyzeWorker::ReanalyzeWorker(std::filesystem::path, QString projectId, QString releaseId,
                                 QObject *parent)
    : QObject(parent), projectId_(std::move(projectId)), releaseId_(std::move(releaseId)) {}

QString ReanalyzeWorker::descriptionFor(const ImportProgress &) {
    return QStringLiteral("Reanalyzing artifact on pacsmithd…");
}

void ReanalyzeWorker::run() {
    try {
        LibraryClient library;
        QString error;
        emit progressChanged(descriptionFor({}));
        const auto result = library.reanalyzeRelease(releaseId_, &error);
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
