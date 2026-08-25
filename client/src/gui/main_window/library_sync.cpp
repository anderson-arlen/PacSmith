#include "gui/main_window/common.hpp"

namespace pacsmith::gui {
namespace {

struct EventRefreshResult {
    QList<Project> projects;
    QString error;
    QSet<QString> topics;
};

bool refreshesProjects(const QSet<QString> &topics) {
    return topics.contains(QStringLiteral("all")) ||
           topics.contains(QStringLiteral("projects")) ||
           topics.contains(QStringLiteral("jobs")) ||
           topics.contains(QStringLiteral("repository"));
}

bool revisionsDiffer(const Project &left, const Project &right) {
    if (left.revision != right.revision || left.releases.size() != right.releases.size()) return true;
    for (const auto &release : left.releases) {
        const auto *other = right.release(release.id);
        if (other == nullptr || other->revision != release.revision) return true;
    }
    return false;
}

} // namespace

void MainWindow::handleServerEvent(const ServerEvent &event) {
    const QSet<QString> topics(event.topics.cbegin(), event.topics.cend());
    emit serverTopicsChanged(event.topics);
    if (!event.jobId.isEmpty() && !event.jobStatus.isEmpty()) {
        const bool finished = event.jobStatus == QStringLiteral("succeeded") ||
                              event.jobStatus == QStringLiteral("failed") ||
                              event.jobStatus == QStringLiteral("interrupted");
        const auto message = jobStatusMessage(event);
        if (!message.isEmpty()) statusBar()->showMessage(message, finished ? 6000 : 3000);
    }
    if (!refreshesProjects(topics)) return;
    if (projectDeleteInFlight_) return;
    pendingEventTopics_.unite(topics);
    if (eventRefreshInFlight_) {
        eventRefreshAgain_ = true;
        return;
    }
    eventRefreshTimer_->start();
}

void MainWindow::runEventRefresh() {
    if (eventRefreshInFlight_) {
        eventRefreshAgain_ = true;
        return;
    }
    const auto topics = pendingEventTopics_;
    pendingEventTopics_.clear();
    eventRefreshInFlight_ = true;
    auto *watcher = new QFutureWatcher<EventRefreshResult>(this);
    connect(watcher, &QFutureWatcher<EventRefreshResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        eventRefreshInFlight_ = false;
        applyEventProjects(result.projects, result.error, result.topics);
        if (eventRefreshAgain_ || !pendingEventTopics_.isEmpty()) {
            eventRefreshAgain_ = false;
            eventRefreshTimer_->start();
        }
    });
    const auto config = library_.config();
    watcher->setFuture(QtConcurrent::run([config, topics] {
        EventRefreshResult result;
        result.topics = topics;
        LibraryClient client(config);
        result.projects = client.list(&result.error);
        return result;
    }));
}

void MainWindow::applyEventProjects(QList<Project> projects, const QString &error,
                                    const QSet<QString> &topics) {
    if (!error.isEmpty()) {
        statusBar()->showMessage(error, 8000);
        return;
    }
    const auto selectedId = project_ ? project_->id : QString{};
    const auto found = std::find_if(projects.cbegin(), projects.cend(), [&](const Project &candidate) {
        return candidate.id == selectedId;
    });
    const bool deleted = !selectedId.isEmpty() && found == projects.cend();
    bool externalChange = false;
    if (project_ && found != projects.cend()) {
        externalChange = revisionsDiffer(*project_, *found) ||
                         topics.contains(QStringLiteral("repository"));
    }
    const bool hasDraft = hasUnsavedProjectDraft();
    if (projectStale_ || ((deleted || externalChange) && hasDraft)) {
        pendingExternalDeletion_ = deleted;
        pendingExternalProject_ = deleted ? std::optional<Project>{}
                                          : std::optional<Project>{*found};
        projectStale_ = true;
        showExternalChange(deleted);
    }
    const bool preserve = projectStale_ || hasDraft;
    applyProjectList(std::move(projects), selectedId, error, preserve);
    if (!preserve && project_) {
        pendingExternalProject_.reset();
        pendingExternalDeletion_ = false;
        projectStale_ = false;
        externalChangeBanner_->setVisible(false);
        const QScopedValueRollback applying(applyingServerRefresh_, true);
        refreshCurrentProject();
        static_cast<void>(BackgroundUpdateStateStore::syncAvailableUpdates(projectCache_.values()));
        static_cast<void>(GuiInstanceServer::requestTray());
    }
}

void MainWindow::showExternalChange(const bool deleted) {
    externalChangeLabel_->setText(
        deleted
            ? QStringLiteral("This package was deleted in another client. Your unsaved draft is preserved; reload to leave it before making more changes.")
            : QStringLiteral("This package changed in another client. Your unsaved draft is preserved; reload to use the latest version before saving."));
    externalReloadButton_->setText(deleted ? QStringLiteral("Leave package")
                                           : QStringLiteral("Reload"));
    externalChangeBanner_->setVisible(true);
}

void MainWindow::reloadExternalProject() {
    if (!projectStale_) return;
    if (hasUnsavedProjectDraft() &&
        QMessageBox::warning(
            this, QStringLiteral("Discard local draft?"),
            pendingExternalDeletion_
                ? QStringLiteral("This package was deleted elsewhere. Discard the local draft and leave it?")
                : QStringLiteral("Discard the local draft and reload the version saved by the other client?"),
            QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Discard) {
        return;
    }
    const auto releaseId = currentReleaseId_;
    const auto section = currentSection();
    lifecycleEditing_ = false;
    if (pkgbuildEditor_ != nullptr) pkgbuildEditor_->document()->setModified(false);
    if (appRunEditor_ != nullptr) appRunEditor_->document()->setModified(false);
    if (desktopEntryEditor_ != nullptr) desktopEntryEditor_->document()->setModified(false);
    projectStale_ = false;
    pendingExternalDeletion_ = false;
    externalChangeBanner_->setVisible(false);
    if (pendingExternalProject_) {
        project_ = std::move(pendingExternalProject_);
        projectCache_.insert(project_->id, *project_);
        if (project_->release(releaseId) != nullptr) currentReleaseId_ = releaseId;
        else {
            const auto *release = project_->activeTrackingRelease();
            if (release == nullptr) release = project_->newestRelease();
            currentReleaseId_ = release == nullptr ? QString{} : release->id;
        }
        refreshCurrentProject();
        selectSection(section);
    } else {
        project_.reset();
        currentReleaseId_.clear();
        applyProjectList(projectCache_.values(), {}, {});
    }
    pendingExternalProject_.reset();
}

bool MainWindow::hasUnsavedProjectDraft() const {
    if (!project_) return false;
    if (lifecycleEditing_) return true;
    for (const auto *editor : {pkgbuildEditor_, appRunEditor_, desktopEntryEditor_}) {
        if (editor != nullptr && editor->document()->isModified()) return true;
    }
    const auto *release = currentRelease();
    if (release == nullptr) return false;
    const bool workbench = rightStack_ != nullptr && rightStack_->currentIndex() == 1;
    if (workbench && currentSection() == EditorSection::ConfigMetadata &&
        packageDescription_ != nullptr &&
        (packageDisplayName_->text() != project_->displayName ||
         packageArchName_->text() != project_->archPackageName ||
         packageVendorName_->text() != project_->vendorName ||
         packageDescription_->text() != release->packageMetadata.description ||
         packageHomepage_->text() != release->packageMetadata.homepage ||
         packageLicenses_->text() != release->packageMetadata.licenses.join(QStringLiteral(", ")) ||
         packageProvides_->text() != release->packageMetadata.provides.join(QStringLiteral(", ")) ||
         packageConflicts_->text() != release->packageMetadata.conflicts.join(QStringLiteral(", ")))) {
        return true;
    }
    const auto *updateRelease = workbench ? release : project_->activeTrackingRelease();
    if (updateRelease != nullptr && updateStrategy_ != nullptr &&
        (!workbench || currentSection() == EditorSection::ConfigUpdates)) {
        const auto &update = updateRelease->update;
        if (updateStrategy_->currentIndex() != (update.strategy == UpdateStrategy::Manual ? 0
                                           : update.strategy == UpdateStrategy::DirectUrl ? 1
                                           : update.strategy == UpdateStrategy::AptRepository ? 2
                                           : update.strategy == UpdateStrategy::RpmRepository ? 3 : 4) ||
         updateUrl_->text() != update.url || aptSuite_->text() != update.aptSuite ||
         aptComponent_->text() != update.aptComponent ||
         aptArchitecture_->text() != update.aptArchitecture ||
         aptPackageName_->text() != update.aptPackageName ||
         rpmArchitecture_->text() != update.rpmArchitecture ||
         rpmPackageName_->text() != update.rpmPackageName ||
         githubOwner_->text() != update.githubOwner ||
         githubRepository_->text() != update.githubRepository ||
         githubAssetRegex_->text() != update.githubAssetRegex ||
            githubPrereleases_->isChecked() != update.githubIncludePrereleases) {
            return true;
        }
    }
    if ((!workbench || currentSection() == EditorSection::ConfigRepository) &&
        repoPublishCheck_ != nullptr &&
        (repoPublishCheck_->isChecked() != project_->repository.publish ||
         repoOverrideEdit_->text().trimmed() != project_->repository.packageNameOverride)) {
        return true;
    }
    return false;
}

bool MainWindow::ensureCurrentProjectWritable() {
    if (!projectStale_) return true;
    QMessageBox::warning(
        this, QStringLiteral("Reload required"),
        QStringLiteral("This package changed in another client. Reload or leave it before saving changes."));
    return false;
}

} // namespace pacsmith::gui
