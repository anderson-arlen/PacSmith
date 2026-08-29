#include "gui/main_window/common.hpp"

namespace pacsmith::gui {
namespace {

struct EventRefreshResult {
    QList<Project> projects;
    QSet<QString> missingProjectIds;
    QString error;
    QSet<QString> topics;
    bool fullRefresh{false};
};

bool refreshesProjects(const QSet<QString> &topics) {
    return topics.contains(QStringLiteral("all")) ||
           topics.contains(QStringLiteral("projects")) ||
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

bool projectListStateDiffers(const Project &left, const Project &right) {
    return revisionsDiffer(left, right) ||
           left.displayName != right.displayName ||
           left.archPackageName != right.archPackageName ||
           left.installedVersion != right.installedVersion ||
           left.installedReleaseId != right.installedReleaseId ||
           left.externallyInstalled != right.externallyInstalled;
}

} // namespace

void MainWindow::handleServerEvent(const ServerEvent &event) {
    const QSet<QString> topics(event.topics.cbegin(), event.topics.cend());
    emit serverTopicsChanged(event.topics);
    updateBuildJobStatus(event);
    if (event.jobKind == QStringLiteral("remote_import") &&
        (remoteImportJobId_.isEmpty() || event.jobId == remoteImportJobId_) &&
        !event.projectId.isEmpty()) {
        if (remoteImportJobId_.isEmpty()) remoteImportJobId_ = event.jobId;
        serverImportRunning_ = event.jobStatus == QStringLiteral("queued") ||
                               event.jobStatus == QStringLiteral("running");
        preparingProjectId_ = event.projectId;
        preparingReleaseId_ = event.releaseId;
        preparationPhase_ = event.jobMessage.startsWith(QStringLiteral("Inspecting"))
            ? QStringLiteral("Inspecting") : QStringLiteral("Downloading");
        preparationBytesReceived_ = event.jobCurrent;
        preparationBytesTotal_ = event.jobTotal;
        if (downloadProgress_ != nullptr) {
            downloadProgress_->setLabelText(
                downloadActivityText(preparationPhase_, preparationBytesReceived_,
                                     preparationBytesTotal_));
            if (preparationBytesTotal_ > 0) {
                const auto receivedMiB = preparationBytesReceived_ / (1024 * 1024);
                const auto totalMiB = std::max<qint64>(1, preparationBytesTotal_ / (1024 * 1024));
                downloadProgress_->setRange(0, static_cast<int>(std::min<qint64>(totalMiB, INT_MAX)));
                downloadProgress_->setValue(
                    static_cast<int>(std::min<qint64>(receivedMiB, totalMiB)));
            }
        }
        updatePreparationIndicators();
        updateDashboardActions();
        const bool finished = event.jobStatus == QStringLiteral("succeeded") ||
                              event.jobStatus == QStringLiteral("failed") ||
                              event.jobStatus == QStringLiteral("interrupted");
        if (finished && downloadProgress_ == nullptr) {
            const auto projectId = preparingProjectId_;
            resetPreparationState();
            refreshProjectList(projectId);
        }
    }
    if (event.jobKind == QStringLiteral("update_check")) {
        const bool active = event.jobStatus == QStringLiteral("queued") ||
                            event.jobStatus == QStringLiteral("running");
        const bool failed = event.jobStatus == QStringLiteral("failed") ||
                            event.jobStatus == QStringLiteral("interrupted") ||
                            event.jobFailedItems > 0 || event.jobPausedItems > 0;
        const auto message = jobStatusMessage(event);
        if (active) {
            if (event.jobStatus == QStringLiteral("queued")) {
                updateCheckErrorDetails_.clear();
                updateCheckErrorsButton_->setVisible(false);
            }
            publishUpdateCheckActivity(true, event.projectId,
                                       !event.projectName.isEmpty() ? event.projectName
                                                                    : event.packageName,
                                       message);
        } else {
            publishUpdateCheckActivity(false);
            if (failed && !message.isEmpty()) {
                updateCheckErrorDetails_ = message;
                const qint64 count = std::max<qint64>(1, event.jobFailedItems + event.jobPausedItems);
                updateCheckErrorsButton_->setText(count == 1 ? QStringLiteral("1 Update Issue")
                                                              : QStringLiteral("%1 Update Issues").arg(count));
                updateCheckErrorsButton_->setVisible(true);
            } else if (event.jobStatus == QStringLiteral("succeeded")) {
                updateCheckErrorDetails_.clear();
                updateCheckErrorsButton_->setVisible(false);
            }
        }
        emit updateCheckActivityChanged(message, active, failed);
    }
    if (!event.jobId.isEmpty() && !event.jobStatus.isEmpty()) {
        if (!updateRepositoryDistributionStatus(event)) {
            const bool finished = event.jobStatus == QStringLiteral("succeeded") ||
                                  event.jobStatus == QStringLiteral("failed") ||
                                  event.jobStatus == QStringLiteral("interrupted");
            const auto message = jobStatusMessage(event);
            if (!message.isEmpty()) statusBar()->showMessage(message, finished ? 6000 : 3000);
        }
    }
    if (!refreshesProjects(topics)) return;
    if (projectDeleteInFlight_) return;
    pendingEventTopics_.unite(topics);
    QString projectId = event.projectId;
    if (projectId.isEmpty() && !event.releaseId.isEmpty()) {
        for (auto iterator = projectCache_.cbegin(); iterator != projectCache_.cend(); ++iterator) {
            if (iterator.value().release(event.releaseId) == nullptr) continue;
            projectId = iterator.key();
            break;
        }
    }
    if (topics.contains(QStringLiteral("all")) || projectId.isEmpty()) {
        pendingFullEventRefresh_ = true;
    } else {
        pendingEventProjectIds_.insert(projectId);
    }
    if (eventRefreshInFlight_) {
        eventRefreshAgain_ = true;
        return;
    }
    eventRefreshTimer_->start();
}

void MainWindow::updateBuildJobStatus(const ServerEvent &event) {
    if (event.jobKind != QStringLiteral("build") || event.jobId.isEmpty()) return;
    const bool finished = event.jobStatus == QStringLiteral("succeeded") ||
                          event.jobStatus == QStringLiteral("failed") ||
                          event.jobStatus == QStringLiteral("interrupted");
    if (finished) {
        activeBuildJobs_.remove(event.jobId);
    } else if (event.jobStatus == QStringLiteral("queued") ||
               event.jobStatus == QStringLiteral("running")) {
        activeBuildJobs_.insert(event.jobId, event);
        if (buildJobId_.isEmpty()) {
            buildJobId_ = event.jobId;
            buildProjectId_ = event.projectId;
            buildReleaseId_ = event.releaseId;
            buildProjectName_ = !event.projectName.isEmpty() ? event.projectName
                                                             : event.packageName;
            buildLogAfter_ = 0;
            buildLogContents_.clear();
            if (buildPollTimer_ == nullptr) {
                buildPollTimer_ = new QTimer(this);
                connect(buildPollTimer_, &QTimer::timeout, this, &MainWindow::pollBuildJob);
            }
            buildPollTimer_->start(250);
        }
    }
    updatePreparationIndicators();
    updateDashboardActions();
    if (currentRelease() != nullptr) populateBuild();
    syncActivityTimer();
}

void MainWindow::restoreActiveBuildJobs() {
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<QList<JobStatus>>(this);
    connect(watcher, &QFutureWatcher<QList<JobStatus>>::finished, this, [this, watcher] {
        const auto jobs = watcher->result();
        watcher->deleteLater();
        for (const auto &job : jobs) {
            ServerEvent event;
            event.jobId = job.id;
            event.jobKind = job.kind;
            event.jobStatus = job.status;
            event.projectId = job.projectId;
            event.projectName = job.projectName;
            event.packageName = job.packageName;
            event.releaseId = job.releaseId;
            if (job.kind == QStringLiteral("build")) {
                updateBuildJobStatus(event);
            } else {
                handleServerEvent(event);
            }
        }
    });
    watcher->setFuture(QtConcurrent::run([config] {
        LibraryClient client(config);
        auto jobs = client.activeJobs(QStringLiteral("build"));
        jobs.append(client.activeJobs(QStringLiteral("remote_import")));
        jobs.append(client.activeJobs(QStringLiteral("repository_distribution")));
        return jobs;
    }));
}

bool MainWindow::updateRepositoryDistributionStatus(const ServerEvent &event) {
    if (event.jobKind != QStringLiteral("repository_distribution")) return false;
    const bool finished = event.jobStatus == QStringLiteral("succeeded") ||
                          event.jobStatus == QStringLiteral("failed") ||
                          event.jobStatus == QStringLiteral("interrupted");
    if (finished) {
        repositoryDistributionJobs_.remove(event.jobId);
        repositoryDistributionJobProjects_.remove(event.jobId);
    } else {
        auto name = !event.projectName.isEmpty() ? event.projectName : event.packageName;
        if (name.isEmpty()) name = event.projectId;
        if (!name.isEmpty()) repositoryDistributionJobs_.insert(event.jobId, name);
        if (!event.projectId.isEmpty()) {
            repositoryDistributionJobProjects_.insert(event.jobId, event.projectId);
        }
    }
    for (int row = 0; projectList_ != nullptr && row < projectList_->count(); ++row) {
        auto *item = projectList_->item(row);
        const auto projectId = item->data(Qt::UserRole).toString();
        item->setData(projectRepositoryBusyRole,
                      repositoryDistributionJobProjects_.values().contains(projectId));
    }
    if (projectList_ != nullptr) projectList_->viewport()->update();
    if (project_ && project_->id == event.projectId && projectRepositoryStateLabel_ != nullptr) {
        populateOverview();
    }
    syncActivityTimer();
    auto names = repositoryDistributionJobs_.values();
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    if (!names.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("Enabling repository distribution for %1").arg(names.join(QStringLiteral(", "))));
    } else if (event.jobStatus == QStringLiteral("succeeded")) {
        statusBar()->showMessage(QStringLiteral("Repository distribution is up to date"), 6000);
    } else if (event.jobStatus == QStringLiteral("failed")) {
        const auto name = !event.projectName.isEmpty() ? event.projectName
                                                       : !event.packageName.isEmpty() ? event.packageName
                                                                                      : event.projectId;
        QString message = name.isEmpty()
            ? QStringLiteral("Repository distribution failed")
            : QStringLiteral("Repository distribution failed for %1").arg(name);
        if (!event.jobMessage.isEmpty()) message += QStringLiteral(": %1").arg(event.jobMessage);
        statusBar()->showMessage(message, 12000);
    } else if (event.jobStatus == QStringLiteral("interrupted")) {
        statusBar()->showMessage(QStringLiteral("Repository distribution was canceled"), 6000);
    }
    return true;
}

void MainWindow::runEventRefresh() {
    if (updateConfigurationSaveInFlight_) {
        eventRefreshAgain_ = true;
        return;
    }
    if (eventRefreshInFlight_) {
        eventRefreshAgain_ = true;
        return;
    }
    if (projectListRefreshInFlight_) {
        eventRefreshAgain_ = true;
        return;
    }
    const auto topics = pendingEventTopics_;
    const auto projectIds = pendingEventProjectIds_;
    const bool fullRefresh = pendingFullEventRefresh_;
    pendingEventTopics_.clear();
    pendingEventProjectIds_.clear();
    pendingFullEventRefresh_ = false;
    eventRefreshInFlight_ = true;
    auto *watcher = new QFutureWatcher<EventRefreshResult>(this);
    connect(watcher, &QFutureWatcher<EventRefreshResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        eventRefreshInFlight_ = false;
        applyEventProjects(result.projects, result.error, result.topics,
                           result.missingProjectIds, result.fullRefresh);
        if (eventRefreshAgain_ || !pendingEventTopics_.isEmpty() ||
            !pendingEventProjectIds_.isEmpty() || pendingFullEventRefresh_) {
            eventRefreshAgain_ = false;
            eventRefreshTimer_->start();
        }
    });
    const auto config = library_.config();
    const auto selectedProjectId = project_ ? project_->id : QString{};
    watcher->setFuture(QtConcurrent::run([config, topics, projectIds, fullRefresh,
                                          selectedProjectId] {
        EventRefreshResult result;
        result.topics = topics;
        result.fullRefresh = fullRefresh;
        LibraryClient client(config);
        if (fullRefresh) {
            result.projects = client.list(&result.error);
            const auto selected = std::find_if(
                result.projects.begin(), result.projects.end(),
                [&selectedProjectId](const Project &candidate) {
                    return candidate.id == selectedProjectId;
                });
            if (result.error.isEmpty() && selected != result.projects.end()) {
                auto loaded = client.load(selectedProjectId, &result.error);
                if (loaded) *selected = std::move(*loaded);
            }
            return result;
        }
        QStringList errors;
        for (const auto &projectId : projectIds) {
            QString error;
            auto project = client.load(projectId, &error);
            if (project) {
                result.projects.append(std::move(*project));
            } else if (error.contains(QStringLiteral("not found"), Qt::CaseInsensitive)) {
                result.missingProjectIds.insert(projectId);
            } else if (!error.isEmpty()) {
                errors.append(error);
            }
        }
        result.error = errors.join(QLatin1Char('\n'));
        return result;
    }));
}

void MainWindow::applyEventProjects(QList<Project> projects, const QString &error,
                                    const QSet<QString> &topics,
                                    const QSet<QString> &missingProjectIds,
                                    const bool fullRefresh) {
    if (!error.isEmpty()) {
        statusBar()->showMessage(error, 8000);
        return;
    }
    const bool containsSavingProject = updateConfigurationSaveInFlight_ &&
        (fullRefresh || missingProjectIds.contains(updateConfigurationSaveProjectId_) ||
         std::any_of(projects.cbegin(), projects.cend(), [this](const Project &candidate) {
             return candidate.id == updateConfigurationSaveProjectId_;
         }));
    if (containsSavingProject) {
        pendingEventTopics_.unite(topics);
        if (fullRefresh) {
            pendingFullEventRefresh_ = true;
        } else {
            pendingEventProjectIds_.insert(updateConfigurationSaveProjectId_);
        }
        eventRefreshAgain_ = true;
        return;
    }
    auto removedIds = missingProjectIds;
    if (fullRefresh) {
        QSet<QString> incomingIds;
        incomingIds.reserve(projects.size());
        for (const auto &candidate : projects) incomingIds.insert(candidate.id);
        for (auto iterator = projectCache_.cbegin(); iterator != projectCache_.cend(); ++iterator) {
            if (!incomingIds.contains(iterator.key())) removedIds.insert(iterator.key());
        }
    }
    const auto selectedId = project_ ? project_->id : QString{};
    const auto found = std::find_if(projects.cbegin(), projects.cend(), [&](const Project &candidate) {
        return candidate.id == selectedId;
    });
    const bool selectedWasFetched = found != projects.cend();
    const bool deleted = !selectedId.isEmpty() && removedIds.contains(selectedId);
    bool externalChange = false;
    if (project_ && selectedWasFetched) {
        externalChange = projectListStateDiffers(*project_, *found) ||
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
    const bool preserveSelected = projectStale_ ||
        (hasDraft && (fullRefresh || deleted || externalChange));
    const auto updateState = BackgroundUpdateStateStore::load();
    QSignalBlocker blocker(projectList_);
    for (const auto &projectId : removedIds) {
        removeProjectListItem(projectId);
        projectCache_.remove(projectId);
        hydratedProjectIds_.remove(projectId);
    }
    for (const auto &candidate : projects) {
        const auto cached = projectCache_.constFind(candidate.id);
        const bool listItemChanged = cached == projectCache_.cend() ||
                                     projectListStateDiffers(cached.value(), candidate);
        if (fullRefresh && preserveSelected && project_ && project_->id == candidate.id &&
            hydratedProjectIds_.contains(candidate.id)) {
            auto hydrated = *project_;
            hydrated.installedVersion = candidate.installedVersion;
            hydrated.installedReleaseId = candidate.installedReleaseId;
            hydrated.externallyInstalled = candidate.externallyInstalled;
            projectCache_.insert(candidate.id, std::move(hydrated));
        } else {
            projectCache_.insert(candidate.id, candidate);
            if (candidate.summaryOnly) hydratedProjectIds_.remove(candidate.id);
            else hydratedProjectIds_.insert(candidate.id);
        }
        if (listItemChanged) updateProjectListItem(candidate, updateState);
    }
    if (auto *selectedItem = projectListItem(selectedId); selectedItem != nullptr) {
        projectList_->setCurrentItem(selectedItem);
    } else if (!preserveSelected && projectList_->currentItem() == nullptr &&
               projectList_->count() > 0) {
        projectList_->setCurrentRow(0);
    }
    const auto nextSelectedId = projectList_->currentItem() == nullptr
        ? QString{} : projectList_->currentItem()->data(Qt::UserRole).toString();
    blocker.unblock();

    if (projectCache_.isEmpty()) {
        applyProjectList({}, {}, {}, preserveSelected);
    } else if (!preserveSelected && selectedWasFetched) {
        const auto releaseId = currentReleaseId_;
        project_ = projectCache_.value(selectedId);
        if (project_->release(releaseId) != nullptr) {
            currentReleaseId_ = releaseId;
        } else {
            const auto *release = project_->activeTrackingRelease();
            if (release == nullptr) release = project_->newestRelease();
            currentReleaseId_ = release == nullptr ? QString{} : release->id;
        }
        pendingExternalProject_.reset();
        pendingExternalDeletion_ = false;
        projectStale_ = false;
        externalChangeBanner_->setVisible(false);
        const QScopedValueRollback applying(applyingServerRefresh_, true);
        refreshCurrentProject();
    } else if (!preserveSelected && deleted) {
        project_.reset();
        currentReleaseId_.clear();
        if (!nextSelectedId.isEmpty()) loadProject(nextSelectedId);
    } else if (!preserveSelected && !project_ && !nextSelectedId.isEmpty()) {
        loadProject(nextSelectedId);
    }
    static_cast<void>(BackgroundUpdateStateStore::syncAvailableUpdates(projectCache_.values()));
    static_cast<void>(GuiInstanceServer::requestTray());
    syncActivityTimer();
    updateUpdateCheckIndicators();
    prefetchProjectIcons();
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
         directUrlFullCheckInterval_->currentData().toInt() !=
             update.directUrlFullCheckIntervalHours ||
            githubPrereleases_->isChecked() != update.githubIncludePrereleases) {
            return true;
        }
    }
    if ((!workbench || currentSection() == EditorSection::ConfigRepository) &&
        repoPublishCheck_ != nullptr &&
        repoSoakOverrideCheck_ != nullptr && repoSoakDays_ != nullptr &&
        (repoPublishCheck_->isChecked() != project_->repository.publish ||
         repoAutomaticSoakCheck_->isChecked() != project_->repository.automaticSoak ||
         repoSoakOverrideCheck_->isChecked() !=
             (project_->repository.soakSecondsOverride >= 0) ||
         (repoSoakOverrideCheck_->isChecked() &&
          project_->repository.soakSecondsOverride >= 0 &&
          repoSoakDays_->value() !=
              (project_->repository.soakSecondsOverride + 86399) / 86400) ||
         repoOverrideEdit_->text().trimmed() != project_->repository.packageNameOverride)) {
        return true;
    }
    return false;
}

bool MainWindow::ensureCurrentProjectWritable() {
    if (project_ && project_->summaryOnly) {
        const auto projectId = project_->id;
        hydratedProjectIds_.remove(projectId);
        loadProjectInteractively(projectId);
        QMessageBox::warning(
            this, QStringLiteral("Loading package"),
            QStringLiteral("PacSmith is loading the complete package before changes can be saved."));
        return false;
    }
    if (!projectStale_) return true;
    QMessageBox::warning(
        this, QStringLiteral("Reload required"),
        QStringLiteral("This package changed in another client. Reload or leave it before saving changes."));
    return false;
}

} // namespace pacsmith::gui
