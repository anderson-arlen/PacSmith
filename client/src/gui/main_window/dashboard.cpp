#include "gui/main_window/common.hpp"

#include <optional>
#include <QThreadPool>

namespace pacsmith::gui {
namespace {

struct ProjectLoadResult {
    std::optional<Project> project;
    QString error;
};

struct ProjectListRefreshResult {
    QList<Project> projects;
    QString error;
};

struct RepositoryTaskResult {
    std::optional<ProjectRepository> status;
    QString error;
};

struct ProjectDeletionResult {
    bool succeeded{false};
    QString error;
};

} // namespace

void MainWindow::updateDashboardActions() {
    const bool installed = project_ && !project_->installedVersion.isEmpty();
    if (uninstallButton_ != nullptr) {
        uninstallButton_->setVisible(installed);
        uninstallButton_->setEnabled(installed && !installService_.isRunning());
    }
    const auto *tracker = project_ ? project_->activeTrackingRelease() : nullptr;
    if (projectPrimaryButton_ == nullptr) {
        updateProjectInfoActions();
        syncUpdateCheckButtons();
        return;
    }
    if (tracker == nullptr) {
        projectPrimaryButton_->setVisible(false);
        updateProjectInfoActions();
        syncUpdateCheckButtons();
        return;
    }
    const bool preparing = tracker->id == preparingReleaseId_ && project_->id == preparingProjectId_;
    const bool hasPackage = releaseHasRetainedPackage(*tracker);
    const bool installedHere = tracker->id == project_->installedReleaseId;
    if (preparing) {
        projectPrimaryButton_->setVisible(true);
        projectPrimaryButton_->setText(QStringLiteral("Show Progress"));
        projectPrimaryButton_->setEnabled(true);
    } else if (hasPackage && !installedHere) {
        projectPrimaryButton_->setVisible(true);
        projectPrimaryButton_->setText(project_->installedVersion.isEmpty()
                                           ? QStringLiteral("Install")
                                           : QStringLiteral("Install Update"));
        projectPrimaryButton_->setEnabled(!installService_.isRunning());
    } else if (tracker->state == ReleaseState::Discovered) {
        projectPrimaryButton_->setVisible(true);
        projectPrimaryButton_->setText(QStringLiteral("Download & Prepare"));
        projectPrimaryButton_->setEnabled(!debDownloadService_->isRunning() &&
                                          importThread_ == nullptr);
    } else {
        projectPrimaryButton_->setVisible(false);
    }
    updateProjectInfoActions();
    syncUpdateCheckButtons();
}

const PackageRelease *MainWindow::dashboardActionRelease() const {
    if (!project_) return nullptr;
    if (project_->installedVersion.isEmpty()) {
        const auto *tracker = project_->activeTrackingRelease();
        if (tracker == nullptr || tracker->state == ReleaseState::Discovered ||
            tracker->state == ReleaseState::Preparing) {
            return nullptr;
        }
        return tracker;
    }
    if (!project_->hasAvailableUpdate() || project_->externallyInstalled) return nullptr;
    const auto *installed = project_->installedRelease();
    if (installed == nullptr) return nullptr;
    const PackageRelease *best = nullptr;
    for (const auto &candidate : project_->releases) {
        if (candidate.state == ReleaseState::Discovered ||
            candidate.state == ReleaseState::Preparing) {
            continue;
        }
        if (candidate.id == installed->id) continue;
        if (!candidate.debian.version.isEmpty() && !installed->debian.version.isEmpty() &&
            comparePackageVersions(candidate.sourceType, candidate.debian.version,
                                   installed->debian.version) <= 0) {
            continue;
        }
        if (best == nullptr || compareReleaseVersions(candidate, *best) > 0) best = &candidate;
    }
    return best;
}

const PackageRelease *MainWindow::configurationEditRelease() const {
    if (!project_) return nullptr;
    const auto *target = dashboardActionRelease();
    if (target == nullptr) target = project_->activeTrackingRelease();
    if (target == nullptr) target = project_->newestRelease();
    if (target == nullptr || target->state == ReleaseState::Discovered ||
        target->state == ReleaseState::Preparing) {
        return nullptr;
    }
    return target;
}

std::optional<MainWindow::EditorSection> MainWindow::firstReviewSection(
    const PackageRelease &release) const {
    const bool packageMappingNeedsAttention =
        ((release.sourceType == SourcePackageType::Archive ||
          release.sourceType == SourcePackageType::AppImage) &&
         release.installMapping.archiveLayout == ArchiveLayout::OptBundle &&
         release.installMapping.optDirectory.trimmed().isEmpty()) ||
        (release.sourceType == SourcePackageType::ElfBinary &&
         release.installMapping.binaryDestination.trimmed().isEmpty());
    const bool dependencyReview = std::any_of(
        release.dependencies.cbegin(), release.dependencies.cend(),
        [this](const auto &dependency) {
            return dependency.status == MappingStatus::Unresolved ||
                   repositoryPackageUnavailable(dependency, repositoryDependencyAvailability_);
        });
    const bool lifecycleReview = !release.lifecycleScript.contents.isEmpty() &&
        (!release.lifecycleScript.validationPassed ||
         release.lifecycleScript.requiresAcknowledgement());
    const bool commandReview = std::any_of(
        release.installMapping.launchers.cbegin(), release.installMapping.launchers.cend(),
        [](const auto &launcher) { return launcher.enabled && launcher.missing; }) ||
        (release.sourceType == SourcePackageType::Archive &&
         std::any_of(release.installMapping.desktopEntries.cbegin(),
                     release.installMapping.desktopEntries.cend(),
                     [&](const auto &desktop) {
                         if (!desktop.enabled) return false;
                         const auto command = desktopEntryCommand(desktop.contents);
                         if (command.isEmpty()) return false;
                         return std::none_of(
                             release.installMapping.launchers.cbegin(),
                             release.installMapping.launchers.cend(),
                             [&](const auto &launcher) {
                                 return launcher.enabled && !launcher.missing &&
                                        launcher.commandName.compare(
                                            command, Qt::CaseInsensitive) == 0;
                             });
                     }));
    const bool desktopReview = std::any_of(
        release.installMapping.desktopEntries.cbegin(),
        release.installMapping.desktopEntries.cend(),
        [](const auto &desktop) { return desktop.enabled && desktop.missing; });
    const bool custom = release.pkgbuildManuallyModified;
    if (packageMappingNeedsAttention && !custom) {
        return release.sourceType == SourcePackageType::AppImage
                   ? EditorSection::ResultInstallPlan
                   : EditorSection::ConfigLayout;
    }
    if (dependencyReview && !custom) return EditorSection::ConfigDependencies;
    if (pendingScriptFindings(release) > 0 && !custom) return EditorSection::ConfigScripts;
    if (lifecycleReview && !custom) return EditorSection::ConfigScripts;
    if (pendingPayloadReviews(release) > 0) return EditorSection::SourceContents;
    if (release.installMapping.appRun.requiresReview() && !custom) {
        return EditorSection::ConfigAppRun;
    }
    if (commandReview && !custom) return EditorSection::ConfigCommands;
    if (desktopReview && !custom) return EditorSection::ConfigDesktopEntries;
    if (release.installMapping.icon.missing && !custom) return EditorSection::ConfigIcon;
    return std::nullopt;
}

void MainWindow::updateProjectInfoActions() {
    if (projectActionNotice_ == nullptr || projectActionButton_ == nullptr) return;
    const auto *target = dashboardActionRelease();
    const bool busy = buildInProgress() || installService_.isRunning();
    const auto *editTarget = configurationEditRelease();
    if (editConfigurationButton_ != nullptr) {
        editConfigurationButton_->setVisible(editTarget != nullptr);
        editConfigurationButton_->setEnabled(editTarget != nullptr && !busy);
    }
    if (target == nullptr) {
        projectActionNotice_->setVisible(false);
        projectActionButton_->setVisible(false);
        return;
    }
    const bool isUpdate = !project_->installedVersion.isEmpty();
    const bool needsReview = firstReviewSection(*target).has_value();
    const bool hasPackage = releaseHasRetainedPackage(*target);
    const bool installedHere = target->id == project_->installedReleaseId;
    projectActionNotice_->setVisible(true);
    projectActionButton_->setVisible(true);
    projectActionButton_->setEnabled(!busy);
    if (needsReview) {
        projectActionNotice_->setText(
            isUpdate ? QStringLiteral("This update has changes that need review.")
                     : QStringLiteral("This package has changes that need review."));
        projectActionButton_->setText(QStringLiteral("Review Changes"));
    } else if (hasPackage && !installedHere) {
        projectActionNotice_->setText(
            isUpdate ? QStringLiteral("This update is built and ready to install.")
                     : QStringLiteral("This package is built and ready to install."));
        projectActionButton_->setText(isUpdate ? QStringLiteral("Install Update")
                                               : QStringLiteral("Install"));
    } else {
        projectActionNotice_->setText(
            isUpdate ? QStringLiteral("This update is ready to build and install.")
                     : QStringLiteral("This package is ready to build and install."));
        projectActionButton_->setText(QStringLiteral("Build && Install"));
    }
}

void MainWindow::handleProjectInfoAction() {
    const auto *target = dashboardActionRelease();
    if (target == nullptr) return;
    if (firstReviewSection(*target).has_value()) {
        showReleaseWorkbenchAtFirstAttention(target->id);
        return;
    }
    currentReleaseId_ = target->id;
    if (releaseHasRetainedPackage(*target) && target->id != project_->installedReleaseId) {
        startInstall();
        return;
    }
    startBuild(true);
}

void MainWindow::handleProjectPrimaryAction() {
    if (!project_) return;
    auto *tracker = project_->activeTrackingRelease();
    if (tracker == nullptr) return;
    const bool preparing = tracker->id == preparingReleaseId_ && project_->id == preparingProjectId_;
    if (preparing) {
        if (downloadProgress_ != nullptr) {
            downloadProgress_->show();
            downloadProgress_->raise();
            downloadProgress_->activateWindow();
        } else if (importProgress_ != nullptr) {
            importProgress_->show();
            importProgress_->raise();
            importProgress_->activateWindow();
        }
        return;
    }
    const bool hasPackage = releaseHasRetainedPackage(*tracker);
    const bool installedHere = tracker->id == project_->installedReleaseId;
    if (hasPackage && !installedHere) {
        currentReleaseId_ = tracker->id;
        startInstall();
        return;
    }
    if (tracker->state == ReleaseState::Discovered) {
        beginReleasePreparation(tracker->id, true);
    }
}

void MainWindow::editPackageConfiguration() {
    if (!project_) return;
    const auto *target = configurationEditRelease();
    if (target == nullptr) return;
    showReleaseWorkbench(target->id);
}

void MainWindow::showProjectDashboard() {
    if (rightStack_ == nullptr) return;
    if (projectSidebar_ != nullptr) projectSidebar_->show();
    rightStack_->setCurrentIndex(0);
    placeUpdatesEditor();
    placeRepositoryEditor();
    if (projectTabs_ != nullptr) projectTabs_->setCurrentIndex(0);
    if (project_) refreshCurrentProject();
}

void MainWindow::showReleaseWorkbench(const QString &releaseId) {
    if (!project_) return;
    const auto *release = project_->release(releaseId);
    if (release == nullptr || release->state == ReleaseState::Discovered) return;
    currentReleaseId_ = releaseId;
    if (projectSidebar_ != nullptr) projectSidebar_->hide();
    rightStack_->setCurrentIndex(1);
    placeUpdatesEditor();
    placeRepositoryEditor();
    configureEditorProfile();
    selectSection(EditorSection::SourceOverview);
    refreshCurrentProject();
    workbenchTitle_->setText(
        QStringLiteral("<h2>Set Up %1 %2</h2>")
            .arg(project_->displayName.toHtmlEscaped(), release->debian.version.toHtmlEscaped()));
    updateWorkbenchStageChrome();
}

void MainWindow::showReleaseWorkbenchAtFirstAttention(const QString &releaseId) {
    if (!project_) return;
    const auto *release = project_->release(releaseId);
    if (release == nullptr || release->state == ReleaseState::Discovered) return;
    showReleaseWorkbench(releaseId);

    if (const auto attention = firstReviewSection(*release)) {
        selectSection(*attention);
        return;
    }
    selectSection(release->pkgbuildManuallyModified ? EditorSection::ConfigPkgbuild
                                                    : EditorSection::ResultBuild);
}

void MainWindow::selectDashboardRelease(const QString &releaseId) {
    if (releaseTable_ == nullptr) return;
    if (projectSidebar_ != nullptr) projectSidebar_->show();
    rightStack_->setCurrentIndex(0);
    placeUpdatesEditor();
    placeRepositoryEditor();
    if (projectTabs_ != nullptr) {
        QWidget *historyPage = releaseTable_;
        while (historyPage != nullptr && projectTabs_->indexOf(historyPage) < 0) {
            historyPage = historyPage->parentWidget();
        }
        if (historyPage != nullptr) projectTabs_->setCurrentWidget(historyPage);
    }
    for (int row = 0; row < releaseTable_->rowCount(); ++row) {
        const auto *item = releaseTable_->item(row, 0);
        if (item != nullptr && item->data(Qt::UserRole).toString() == releaseId) {
            releaseTable_->selectRow(row);
            releaseTable_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            break;
        }
    }
}

void MainWindow::updatePreparationIndicators() {
    static const QStringList frames{QStringLiteral("⠋"), QStringLiteral("⠙"),
                                    QStringLiteral("⠹"), QStringLiteral("⠸")};
    const auto frame = frames.at(preparationSpinnerFrame_ % frames.size());
    QString preparingId = preparingProjectId_;
    QString phase = preparationPhase_;
    qint64 received = preparationBytesReceived_;
    qint64 total = preparationBytesTotal_;
    if (preparingId.isEmpty()) {
        const auto updateState = BackgroundUpdateStateStore::load();
        preparingId = updateState.preparingProjectId;
        phase = updateState.preparationPhase;
        received = updateState.preparationBytesReceived;
        total = updateState.preparationBytesTotal;
    }
    const auto activityText = preparingId.isEmpty()
        ? QString{}
        : downloadActivityText(phase, received, total);
    for (int row = 0; projectList_ != nullptr && row < projectList_->count(); ++row) {
        auto *item = projectList_->item(row);
        const auto projectId = item->data(Qt::UserRole).toString();
        const auto currentActivity = item->data(projectActivityRole).toString();
        if (projectId == preparingId && !preparingId.isEmpty()) {
            if (currentActivity != activityText) item->setData(projectActivityRole, activityText);
            projectList_->viewport()->update(projectList_->visualItemRect(item));
        } else if (!currentActivity.isEmpty()) {
            item->setData(projectActivityRole, QString{});
            projectList_->viewport()->update(projectList_->visualItemRect(item));
        }
    }
    if (!project_ || project_->id != preparingProjectId_ || releaseTable_ == nullptr) return;
    for (int row = 0; row < releaseTable_->rowCount(); ++row) {
        auto *versionItem = releaseTable_->item(row, 0);
        if (versionItem == nullptr ||
            versionItem->data(Qt::UserRole).toString() != preparingReleaseId_) continue;
        const auto tablePhase = preparationPhase_.isEmpty() ? QStringLiteral("Downloading")
                                                            : preparationPhase_;
        if (auto *status = releaseTable_->item(row, 1); status != nullptr) {
            status->setText(QStringLiteral("%1 %2").arg(frame, tablePhase));
        }
        if (auto *review = releaseTable_->item(row, 4); review != nullptr) {
            review->setText(preparationBytesTotal_ > 0 && tablePhase == QStringLiteral("Downloading")
                ? QStringLiteral("%1 / %2 MiB")
                      .arg(preparationBytesReceived_ / (1024 * 1024))
                      .arg(preparationBytesTotal_ / (1024 * 1024))
                : QStringLiteral("Processing…"));
        }
        break;
    }
}

void MainWindow::updateUpdateCheckIndicators() {
    if (projectList_ == nullptr) return;
    const auto updateState = BackgroundUpdateStateStore::load();
    QString checkingId = updateState.checkingProjectId;
    QString checkingName = updateState.checkingProjectName;
    const bool inProcess = aptUpdateService_->isRunning() || rpmUpdateService_->isRunning() ||
                           githubUpdateService_->isRunning();
    if (checkingId.isEmpty() && inProcess && project_) {
        checkingId = project_->id;
        checkingName = project_->displayName.isEmpty() ? project_->archPackageName
                                                       : project_->displayName;
    }
    const bool checkBusy = updateState.checking || inProcess;
    const bool checking = checkBusy && !checkingId.isEmpty();
    QString preparingId = preparingProjectId_;
    QString preparingName;
    QString phase = preparationPhase_;
    qint64 received = preparationBytesReceived_;
    qint64 total = preparationBytesTotal_;
    if (preparingId.isEmpty()) {
        preparingId = updateState.preparingProjectId;
        preparingName = updateState.preparingProjectName;
        phase = updateState.preparationPhase;
        received = updateState.preparationBytesReceived;
        total = updateState.preparationBytesTotal;
    } else {
        preparingName = displayNameForProject(preparingId);
    }
    const bool prepareBusy = !preparingId.isEmpty();
    projectList_->setProperty("pacsmithSpinnerFrame", preparationSpinnerFrame_);
    projectList_->viewport()->setProperty("pacsmithSpinnerFrame", preparationSpinnerFrame_);
    for (int row = 0; row < projectList_->count(); ++row) {
        auto *item = projectList_->item(row);
        const auto projectId = item->data(Qt::UserRole).toString();
        const bool itemPreparing = prepareBusy && projectId == preparingId;
        const bool itemChecking = checking && projectId == checkingId && !itemPreparing;
        const bool itemBusy = itemPreparing || itemChecking;
        if (item->data(projectCheckingRole).toBool() != itemBusy) {
            item->setData(projectCheckingRole, itemBusy);
        }
        if (itemBusy) projectList_->viewport()->update(projectList_->visualItemRect(item));
    }
    if (prepareBusy && canShowUpdateCheckStatus()) {
        statusBar()->showMessage(downloadStatusText(preparingName, phase, received, total));
    } else if (checkBusy && canShowUpdateCheckStatus()) {
        updateCheckStatusActive_ = true;
        statusBar()->showMessage(checkingName.isEmpty()
                                     ? QStringLiteral("Checking for updates…")
                                     : QStringLiteral("Checking %1 for updates…").arg(checkingName));
    } else if (!checkBusy && updateCheckStatusActive_) {
        updateCheckStatusActive_ = false;
        if (!prepareBusy && canShowUpdateCheckStatus()) {
            statusBar()->showMessage(finishedUpdateCheckStatus(updateState), 8000);
        }
    }
    if (checkBusy || prepareBusy) projectList_->viewport()->update();
}

void MainWindow::syncActivityTimer() {
    if (preparationSpinnerTimer_ == nullptr) return;
    const bool busy = !preparingProjectId_.isEmpty() || listActivityInProgress();
    if (busy) {
        if (!preparationSpinnerTimer_->isActive()) preparationSpinnerTimer_->start();
        return;
    }
    if (preparationSpinnerTimer_->isActive()) preparationSpinnerTimer_->stop();
    if (updateCheckStatusActive_) updateUpdateCheckIndicators();
}

bool MainWindow::updateCheckInProgress() const {
    if (aptUpdateService_->isRunning() || rpmUpdateService_->isRunning() ||
        githubUpdateService_->isRunning()) {
        return true;
    }
    return BackgroundUpdateStateStore::load().checking;
}

bool MainWindow::listActivityInProgress() const {
    if (!preparingProjectId_.isEmpty()) return true;
    if (aptUpdateService_->isRunning() || rpmUpdateService_->isRunning() ||
        githubUpdateService_->isRunning()) {
        return true;
    }
    const auto updateState = BackgroundUpdateStateStore::load();
    return updateState.checking || !updateState.preparingProjectId.isEmpty();
}

bool MainWindow::canShowUpdateCheckStatus() const {
    return importThread_ == nullptr && importProgress_ == nullptr &&
           !buildInProgress() && !installService_.isRunning();
}

void MainWindow::publishUpdateCheckActivity(const bool running, const QString &projectId,
                                            const QString &projectName) {
    auto updateState = BackgroundUpdateStateStore::load();
    if (running) {
        updateState.checking = true;
        updateState.checkingProjectId = projectId;
        updateState.checkingProjectName = projectName;
        updateState.message = projectName.isEmpty()
            ? QStringLiteral("Checking for updates")
            : QStringLiteral("Checking %1 for updates").arg(projectName);
        static_cast<void>(BackgroundUpdateStateStore::save(updateState));
        syncActivityTimer();
        updateUpdateCheckIndicators();
        return;
    }
    if (!projectId.isEmpty() && !updateState.checkingProjectId.isEmpty() &&
        updateState.checkingProjectId != projectId) {
        return;
    }
    updateState.checking = false;
    updateState.checkingProjectId.clear();
    updateState.checkingProjectName.clear();
    if (updateState.preparingProjectId.isEmpty()) {
        applyAvailableUpdateCensus(updateState, projectCache_.values());
        updateState.message = finishedUpdateCheckStatus(updateState);
    }
    static_cast<void>(BackgroundUpdateStateStore::save(updateState));
    updateUpdateCheckIndicators();
    syncActivityTimer();
}

QString MainWindow::displayNameForProject(const QString &projectId) const {
    if (project_ && project_->id == projectId) {
        return project_->displayName.isEmpty() ? project_->archPackageName : project_->displayName;
    }
    const auto cached = projectCache_.constFind(projectId);
    if (cached != projectCache_.cend()) {
        return cached->displayName.isEmpty() ? cached->archPackageName : cached->displayName;
    }
    return QStringLiteral("Package");
}

void MainWindow::publishPreparationActivity() {
    if (preparingProjectId_.isEmpty()) return;
    auto updateState = BackgroundUpdateStateStore::load();
    updateState.preparingProjectId = preparingProjectId_;
    updateState.preparingProjectName = displayNameForProject(preparingProjectId_);
    updateState.preparationPhase = preparationPhase_;
    updateState.preparationBytesReceived = preparationBytesReceived_;
    updateState.preparationBytesTotal = preparationBytesTotal_;
    updateState.message = downloadStatusText(updateState.preparingProjectName, preparationPhase_,
                                             preparationBytesReceived_, preparationBytesTotal_);
    static_cast<void>(BackgroundUpdateStateStore::save(updateState));
}

void MainWindow::clearPublishedPreparationActivity(const QString &projectId) {
    auto updateState = BackgroundUpdateStateStore::load();
    if (!projectId.isEmpty() && !updateState.preparingProjectId.isEmpty() &&
        updateState.preparingProjectId != projectId) {
        return;
    }
    if (updateState.preparingProjectId.isEmpty() && updateState.preparingProjectName.isEmpty()) {
        return;
    }
    updateState.preparingProjectId.clear();
    updateState.preparingProjectName.clear();
    updateState.preparationPhase.clear();
    updateState.preparationBytesReceived = 0;
    updateState.preparationBytesTotal = -1;
    static_cast<void>(BackgroundUpdateStateStore::save(updateState));
}

void MainWindow::startListDownloadActivity(const QString &projectId, const QString &releaseId) {
    if (projectId.isEmpty()) return;
    const bool known = (project_ && project_->id == projectId) || projectCache_.contains(projectId);
    if (!known) return;
    preparingProjectId_ = projectId;
    preparingReleaseId_ = releaseId;
    preparationPhase_ = QStringLiteral("Downloading");
    preparationBytesReceived_ = 0;
    preparationBytesTotal_ = -1;
    lastPreparationPublish_.invalidate();
    publishPreparationActivity();
    syncActivityTimer();
    updatePreparationIndicators();
    updateUpdateCheckIndicators();
}

void MainWindow::noteBackgroundCheckStarted() {
    syncActivityTimer();
    updatePreparationIndicators();
    updateUpdateCheckIndicators();
}

void MainWindow::resetPreparationState() {
    if (downloadProgress_ != nullptr) {
        downloadProgress_->close();
        downloadProgress_->deleteLater();
        downloadProgress_ = nullptr;
    }
    const auto projectId = preparingProjectId_;
    if (!projectId.isEmpty()) clearPublishedPreparationActivity(projectId);
    preparingProjectId_.clear();
    preparingReleaseId_.clear();
    preparationPhase_.clear();
    preparationBytesReceived_ = 0;
    preparationBytesTotal_ = -1;
    lastPreparationPublish_.invalidate();
    if (!listActivityInProgress()) preparationSpinnerFrame_ = 0;
    syncActivityTimer();
    updatePreparationIndicators();
    updateUpdateCheckIndicators();
}

void MainWindow::reloadVisibleProjects(const bool refreshOpenProject) {
    const auto projectId = project_ ? project_->id : QString{};
    refreshProjectList(projectId, [this, refreshOpenProject](const bool succeeded) {
        if (succeeded && refreshOpenProject && project_) refreshCurrentProject();
        syncActivityTimer();
        updateUpdateCheckIndicators();
    });
}

void MainWindow::refreshLibraryView() {
    if (projectStale_) {
        reloadExternalProject();
        return;
    }
    const auto projectId = project_ ? project_->id : QString{};
    refreshProjectList(projectId, [this](const bool succeeded) {
        if (!succeeded) return;
        if (project_) {
            refreshCurrentProject();
            updateDashboardActions();
            prefetchSigningKeys(*project_);
        }
        syncTrayUpdateCensus();
        statusBar()->showMessage(QStringLiteral("Package list refreshed"), 4000);
    });
}

void MainWindow::syncTrayUpdateCensus() {
    static_cast<void>(BackgroundUpdateStateStore::syncAvailableUpdates(projectCache_.values()));
    static_cast<void>(GuiInstanceServer::requestTray());
}

void MainWindow::setProjectListBusy(const bool busy, const QString &message) {
    projectListRefreshInFlight_ = busy;
    if (projectListBusyLabel_ != nullptr) {
        projectListBusyLabel_->setText(message);
        projectListBusyLabel_->setVisible(busy && !message.isEmpty());
    }
    if (projectListProgress_ != nullptr) projectListProgress_->setVisible(busy);
    if (refreshProjectListButton_ != nullptr) refreshProjectListButton_->setEnabled(!busy);
    if (projectList_ != nullptr) projectList_->setEnabled(!busy);
    updateDeleteButton();
}

void MainWindow::refreshProjectList(const QString &selectId,
                                    std::function<void(bool)> completed) {
    const auto generation = ++projectListRefreshGeneration_;
    setProjectListBusy(true, QStringLiteral("Refreshing…"));
    statusBar()->showMessage(QStringLiteral("Refreshing packages…"));
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<ProjectListRefreshResult>(this);
    connect(watcher, &QFutureWatcher<ProjectListRefreshResult>::finished, this,
            [this, watcher, generation, selectId, completed = std::move(completed)]() mutable {
        auto result = watcher->result();
        watcher->deleteLater();
        if (generation != projectListRefreshGeneration_) return;
        setProjectListBusy(false);
        if (!result.error.isEmpty()) {
            statusBar()->showMessage(result.error, 8000);
            if (completed) completed(false);
            return;
        }
        applyProjectList(std::move(result.projects), selectId);
        if (completed) completed(true);
    });
    watcher->setFuture(QtConcurrent::run([config] {
        LibraryClient client(config);
        ProjectListRefreshResult result;
        result.projects = client.list(&result.error);
        return result;
    }));
}

void MainWindow::applyProjectList(QList<Project> projects, const QString &selectId,
                                  const QString &error, const bool preserveCurrent) {
    const auto previous = selectId.isEmpty() && project_ ? project_->id : selectId;
    QSignalBlocker blocker(projectList_);
    projectList_->clear();
    projectCache_.clear();
    projectCache_.reserve(projects.size());
    for (const auto &project : projects) projectCache_.insert(project.id, project);
    const auto updateState = BackgroundUpdateStateStore::load();
    for (const auto &project : projects) {
        const auto *release = project.activeTrackingRelease();
        if (release == nullptr) release = project.newestRelease();
        auto visualState = ProjectVisualState::NotInstalled;
        QString subtitle = QStringLiteral("Not installed");
        QString statusDescription = QStringLiteral("Not installed");
        if (!project.installedVersion.isEmpty()) {
            if (project.installedRelease() == nullptr || project.externallyInstalled) {
                visualState = ProjectVisualState::Attention;
                subtitle = QStringLiteral("⚠ Installed %1 · tracking needs attention")
                               .arg(project.installedVersion);
                statusDescription = QStringLiteral("Installed package cannot be matched to a retained PacSmith release");
            } else if (project.hasAvailableUpdate()) {
                visualState = ProjectVisualState::UpdateAvailable;
                subtitle = QStringLiteral("⚠ Update available");
                statusDescription = QStringLiteral("Update available");
            } else {
                visualState = ProjectVisualState::Current;
                subtitle = QStringLiteral("✓ Up to date");
                statusDescription = QStringLiteral("Installed and up to date");
            }
        }
        const bool checking = project.id == updateState.checkingProjectId && updateState.checking &&
                              project.id != preparingProjectId_ &&
                              project.id != updateState.preparingProjectId;
        const bool preparing = project.id == preparingProjectId_ ||
                               project.id == updateState.preparingProjectId;
        auto *item = new QListWidgetItem(project.displayName, projectList_);
        item->setIcon(projectIcon(library_, project));
        item->setData(Qt::UserRole, project.id);
        item->setData(projectSubtitleRole, subtitle);
        item->setData(projectVisualStateRole, static_cast<int>(visualState));
        item->setData(projectCheckingRole, checking || preparing);
        item->setData(projectActivityRole, QString{});
        item->setSizeHint(QSize(0, 60));
        item->setToolTip(QStringLiteral("%1 · %2%3")
            .arg(project.archPackageName, statusDescription,
                 release == nullptr ? QString{} : QStringLiteral(" · tracked release %1").arg(release->debian.version)));
        if (project.id == previous) projectList_->setCurrentItem(item);
    }
    if (projectList_->currentItem() == nullptr) {
        for (int row = 0; row < projectList_->count(); ++row) {
            auto *item = projectList_->item(row);
            if (item != nullptr && !item->data(Qt::UserRole).toString().isEmpty()) {
                projectList_->setCurrentItem(item);
                break;
            }
        }
    }
    if (!error.isEmpty()) statusBar()->showMessage(error, 8000);
    if (!preparingProjectId_.isEmpty() || !updateState.preparingProjectId.isEmpty()) {
        updatePreparationIndicators();
    }
    syncActivityTimer();
    updateUpdateCheckIndicators();
    if (projects.isEmpty()) {
        if (preserveCurrent && project_) {
            deleteProjectButton_->setEnabled(false);
            return;
        }
        project_.reset();
        projectCache_.clear();
        stageTabs_->setEnabled(false);
        projectTabs_->setEnabled(false);
        if (projectSidebar_ != nullptr) projectSidebar_->show();
        rightStack_->setCurrentIndex(0);
        deleteProjectButton_->setEnabled(false);
        projectTitle_->setText(QStringLiteral("<h2>PacSmith</h2>"));
        projectSubtitle_->setText(QStringLiteral("Import a vendor artifact or GitHub release to begin."));
        if (projectPrimaryButton_ != nullptr) projectPrimaryButton_->setVisible(false);
        if (uninstallButton_ != nullptr) uninstallButton_->setVisible(false);
        if (editConfigurationButton_ != nullptr) editConfigurationButton_->setVisible(false);
        if (projectActionNotice_ != nullptr) projectActionNotice_->setVisible(false);
        if (projectActionButton_ != nullptr) projectActionButton_->setVisible(false);
        if (dashboardBodyStack_ != nullptr && projectTabs_ != nullptr) {
            dashboardBodyStack_->setCurrentWidget(projectTabs_);
        }
        return;
    }
    QSet<QString> remaining;
    remaining.reserve(projectCache_.size());
    for (auto iterator = projectCache_.cbegin(); iterator != projectCache_.cend(); ++iterator) {
        remaining.insert(iterator.key());
    }
    hydratedProjectIds_.intersect(remaining);
    if (!preserveCurrent && loadingProjectId_.isEmpty() && project_ && projectCache_.contains(project_->id)) {
        const auto releaseId = currentReleaseId_;
        project_ = projectCache_.value(project_->id);
        if (project_->release(releaseId) != nullptr) {
            currentReleaseId_ = releaseId;
        } else {
            const auto *initial = project_->activeTrackingRelease();
            if (initial == nullptr) initial = project_->newestRelease();
            currentReleaseId_ = initial == nullptr ? QString{} : initial->id;
        }
    }
    const auto selectedId = projectList_->currentItem() == nullptr
        ? QString{} : projectList_->currentItem()->data(Qt::UserRole).toString();
    blocker.unblock();
    if (!selectedId.isEmpty() && loadingProjectId_ != selectedId &&
        (!project_ || project_->id != selectedId)) {
        loadProject(selectedId);
    }
    prefetchProjectIcons();
}

void MainWindow::showDashboardLoading(const QString &displayName) {
    if (projectSidebar_ != nullptr) projectSidebar_->show();
    if (rightStack_ != nullptr) rightStack_->setCurrentIndex(0);
    if (dashboardBodyStack_ != nullptr && dashboardLoadingPage_ != nullptr) {
        dashboardBodyStack_->setCurrentWidget(dashboardLoadingPage_);
    }
    if (projectTitle_ != nullptr) {
        projectTitle_->setText(QStringLiteral("<h2>%1</h2>").arg(displayName.toHtmlEscaped()));
    }
    if (projectSubtitle_ != nullptr) {
        projectSubtitle_->setText(QStringLiteral("Loading package…"));
    }
    if (projectPrimaryButton_ != nullptr) projectPrimaryButton_->setVisible(false);
    if (uninstallButton_ != nullptr) uninstallButton_->setVisible(false);
    if (editConfigurationButton_ != nullptr) editConfigurationButton_->setVisible(false);
    if (projectActionNotice_ != nullptr) projectActionNotice_->setVisible(false);
    if (projectActionButton_ != nullptr) projectActionButton_->setVisible(false);
    if (projectTabs_ != nullptr) projectTabs_->setEnabled(false);
    if (stageTabs_ != nullptr) stageTabs_->setEnabled(false);
}

void MainWindow::applyLoadedProject(Project project) {
    lifecycleEditing_ = false;
    projectCache_.insert(project.id, project);
    project_ = std::move(project);
    const auto *initialRelease = project_->activeTrackingRelease();
    if (initialRelease == nullptr) initialRelease = project_->newestRelease();
    currentReleaseId_ = initialRelease == nullptr ? QString{} : initialRelease->id;
    stageTabs_->setEnabled(true);
    projectTabs_->setEnabled(true);
    if (dashboardBodyStack_ != nullptr && projectTabs_ != nullptr) {
        dashboardBodyStack_->setCurrentWidget(projectTabs_);
    }
    hydratedProjectIds_.insert(project_->id);
    showProjectDashboard();
}

void MainWindow::loadProject(const QString &id) {
    ++projectLoadGeneration_;
    loadingProjectId_.clear();
    if (id.isEmpty()) return;
    const auto cached = projectCache_.constFind(id);
    if (cached != projectCache_.cend()) {
        applyLoadedProject(cached.value());
        prefetchSigningKeys(*project_);
        return;
    }
    loadProjectInteractively(id);
}

void MainWindow::loadProjectInteractively(const QString &id) {
    if (id.isEmpty()) return;
    if (loadingProjectId_ == id) return;
    if (project_ && project_->id == id && loadingProjectId_.isEmpty() &&
        hydratedProjectIds_.contains(id)) {
        return;
    }
    const auto generation = ++projectLoadGeneration_;
    loadingProjectId_ = id;
    if (project_ && project_->id != id) {
        project_.reset();
        currentReleaseId_.clear();
    }
    auto displayName = displayNameForProject(id);
    if (displayName.isEmpty() && projectList_ != nullptr && projectList_->currentItem() != nullptr) {
        displayName = projectList_->currentItem()->text();
    }
    if (displayName.isEmpty()) displayName = QStringLiteral("Package");
    showDashboardLoading(displayName);
    if (projectCache_.contains(id)) {
        QTimer::singleShot(0, this, [this, generation, id] {
            if (generation != projectLoadGeneration_ || loadingProjectId_ != id) return;
            const auto found = projectCache_.constFind(id);
            if (found == projectCache_.cend()) {
                startBackgroundProjectLoad(id, generation);
                return;
            }
            applyLoadedProject(found.value());
            loadingProjectId_.clear();
            prefetchSigningKeys(*project_);
        });
        return;
    }
    startBackgroundProjectLoad(id, generation);
}

void MainWindow::startBackgroundProjectLoad(const QString &id, quint64 generation) {
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<ProjectLoadResult>(this);
    connect(watcher, &QFutureWatcher<ProjectLoadResult>::finished, this,
            [this, watcher, generation, id] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (generation != projectLoadGeneration_ || loadingProjectId_ != id) return;
        loadingProjectId_.clear();
        if (!result.project) {
            if (dashboardBodyStack_ != nullptr && projectTabs_ != nullptr) {
                dashboardBodyStack_->setCurrentWidget(projectTabs_);
            }
            QMessageBox::critical(this, QStringLiteral("Could not load project"), result.error);
            return;
        }
        applyLoadedProject(std::move(*result.project));
    });
    watcher->setFuture(QtConcurrent::run([config, id]() -> ProjectLoadResult {
        LibraryClient client(config);
        QString error;
        auto loaded = client.load(id, &error);
        return ProjectLoadResult{std::move(loaded), error};
    }));
}

void MainWindow::prefetchSigningKeys(const Project &project) {
    const auto config = library_.config();
    QThreadPool::globalInstance()->start([config, project] {
        LibraryClient client(config);
        client.prefetchReleaseArtifacts(project);
    });
}

void MainWindow::prefetchProjectIcons() {
    struct IconJob {
        QString projectId;
        QString artifactId;
    };
    QList<IconJob> jobs;
    for (auto iterator = projectCache_.cbegin(); iterator != projectCache_.cend(); ++iterator) {
        for (const auto &release : iterator.value().releases) {
            if (!release.installMapping.icon.isConfigured() ||
                release.iconArtifactId.isEmpty()) continue;
            if (!library_.cachedArtifactPath(release.iconArtifactId,
                                             QStringLiteral("icon")).isEmpty()) {
                break;
            }
            jobs.append({iterator.key(), release.iconArtifactId});
            break;
        }
    }
    if (jobs.isEmpty()) return;
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<QHash<QString, QString>>(this);
    connect(watcher, &QFutureWatcher<QHash<QString, QString>>::finished, this,
            [this, watcher] {
        const auto paths = watcher->result();
        watcher->deleteLater();
        if (paths.isEmpty() || projectList_ == nullptr) return;
        for (int row = 0; row < projectList_->count(); ++row) {
            auto *item = projectList_->item(row);
            if (item == nullptr) continue;
            const auto path = paths.value(item->data(Qt::UserRole).toString());
            if (!path.isEmpty()) item->setIcon(QIcon(path));
        }
        if (project_ && overviewIcon_ != nullptr) {
            const auto path = paths.value(project_->id);
            QPixmap pixmap;
            if (!path.isEmpty() && pixmap.load(path)) {
                overviewIcon_->setPixmap(pixmap.scaled(QSize(96, 96), Qt::KeepAspectRatio,
                                                       Qt::SmoothTransformation));
            }
        }
    });
    watcher->setFuture(QtConcurrent::run([config, jobs] {
        LibraryClient client(config);
        QHash<QString, QString> paths;
        for (const auto &job : jobs) {
            QString error;
            const auto path = client.cacheArtifact(job.artifactId, QStringLiteral("icon"), &error);
            if (!path.isEmpty()) paths.insert(job.projectId, path);
        }
        return paths;
    }));
}

PackageRelease *MainWindow::currentRelease() {
    return project_ ? project_->release(currentReleaseId_) : nullptr;
}

const PackageRelease *MainWindow::currentRelease() const {
    return project_ ? project_->release(currentReleaseId_) : nullptr;
}

PackageRelease *MainWindow::activeTrackingRelease() {
    return project_ ? project_->activeTrackingRelease() : nullptr;
}

const PackageRelease *MainWindow::activeTrackingRelease() const {
    return project_ ? project_->activeTrackingRelease() : nullptr;
}

PackageRelease *MainWindow::updateEditorRelease() {
    if (!project_) return nullptr;
    if (rightStack_ != nullptr && rightStack_->currentIndex() == 1 && currentRelease() != nullptr) {
        return currentRelease();
    }
    return activeTrackingRelease();
}

void MainWindow::refreshCurrentProject() {
    if (!project_ || currentRelease() == nullptr) return;
    projectTitle_->setText(QStringLiteral("<h2>%1</h2>").arg(project_->displayName.toHtmlEscaped()));
    QStringList subtitleParts;
    if (!project_->installedVersion.isEmpty()) {
        subtitleParts.append(QStringLiteral("Installed %1").arg(project_->installedVersion));
    }
    subtitleParts.append(QStringLiteral("%1 %2")
                             .arg(sourcePackageTypeTitle(currentRelease()->sourceType),
                                  currentRelease()->debian.version));
    subtitleParts.append(QStringLiteral("Arch package %1").arg(project_->archPackageName));
    projectSubtitle_->setText(subtitleParts.join(QStringLiteral(" · ")));
    workbenchTitle_->setText(
        QStringLiteral("<h2>Set Up %1 %2</h2>")
            .arg(project_->displayName.toHtmlEscaped(), currentRelease()->debian.version.toHtmlEscaped()));
    configureEditorProfile();
    updateDeleteButton();
    if (rightStack_ != nullptr && rightStack_->currentIndex() == 1) {
        populateCurrentWorkbenchPage();
        return;
    }
    populateOverview();
    // Keep the hidden editor synchronized with the active tracking release so a
    // dashboard update check can never save a historical release's values into it.
    populateUpdates();
    populateRepository();
}

void MainWindow::populateCurrentWorkbenchPage() {
    if (!project_ || currentRelease() == nullptr || stageTabs_ == nullptr) return;
    switch (currentSection()) {
    case EditorSection::SourceOverview:
        populateSourceOverview();
        break;
    case EditorSection::SourceMetadata:
    case EditorSection::ConfigLayout:
        populatePackage();
        break;
    case EditorSection::ConfigMetadata:
        populatePackageMetadata();
        break;
    case EditorSection::ResultInstallPlan:
        populateInstallPlan();
        break;
    case EditorSection::ConfigDependencies:
        populateDependencies();
        break;
    case EditorSection::SourceScripts:
    case EditorSection::ConfigScripts:
        populateScripts();
        break;
    case EditorSection::SourceContents:
        populatePayload();
        break;
    case EditorSection::ConfigCommands:
        populateCommands();
        break;
    case EditorSection::ConfigAppRun:
        populateAppRunEditor();
        break;
    case EditorSection::ConfigDesktopEntries:
        populateDesktopEntries();
        break;
    case EditorSection::ConfigIcon:
        populateIcon();
        break;
    case EditorSection::ConfigUpdates:
        placeUpdatesEditor();
        populateUpdates();
        break;
    case EditorSection::ConfigRepository:
        placeRepositoryEditor();
        populateRepository();
        break;
    case EditorSection::ConfigPkgbuild:
    case EditorSection::ResultPkgbuild:
        populatePkgbuild();
        break;
    case EditorSection::ResultBuild:
        populateBuild();
        break;
    }
}

void MainWindow::updateDeleteButton() {
    if (!project_) {
        deleteProjectButton_->setEnabled(false);
        deleteProjectButton_->setToolTip({});
        if (reanalyzeButton_ != nullptr) reanalyzeButton_->setEnabled(false);
        return;
    }
    const bool busy = projectListRefreshInFlight_ || projectDeleteInFlight_ ||
                      buildInProgress() || installService_.isRunning() ||
                      aptUpdateService_->isRunning() || rpmUpdateService_->isRunning() ||
                      githubUpdateService_->isRunning() || debDownloadService_->isRunning() ||
                      importThread_ != nullptr;
    if (reanalyzeButton_ != nullptr) {
        reanalyzeButton_->setEnabled(currentRelease() != nullptr &&
                                     currentRelease()->state != ReleaseState::Discovered &&
                                     currentRelease()->state != ReleaseState::Preparing && !busy);
    }
    deleteProjectButton_->setEnabled(!project_->ownsInstalledPackage() && !busy);
    if (project_->ownsInstalledPackage()) {
        deleteProjectButton_->setToolTip(
            QStringLiteral("Uninstall %1 with pacman before deleting this project").arg(project_->archPackageName));
    } else if (!project_->installedVersion.isEmpty()) {
        deleteProjectButton_->setToolTip(
            QStringLiteral("Delete this extra project. Installed %1 is managed by another PacSmith project or an external install")
                .arg(project_->archPackageName));
    } else if (busy) {
        deleteProjectButton_->setToolTip(QStringLiteral("Wait for the current operation to finish"));
    } else {
        deleteProjectButton_->setToolTip(QStringLiteral("Permanently delete this local PacSmith project"));
    }
}

void MainWindow::deleteCurrentProject() {
    if (!project_ || !ensureCurrentProjectWritable()) return;
    if (project_->ownsInstalledPackage()) {
        QMessageBox::warning(this, QStringLiteral("Project cannot be deleted"),
                             QStringLiteral("%1 is installed. Uninstall it with pacman before deleting its PacSmith project.")
                                 .arg(project_->archPackageName));
        return;
    }
    if (buildInProgress() || installService_.isRunning() ||
        aptUpdateService_->isRunning() || rpmUpdateService_->isRunning() ||
        githubUpdateService_->isRunning() || debDownloadService_->isRunning() ||
        importThread_ != nullptr) {
        QMessageBox::warning(this, QStringLiteral("Project is busy"),
                             QStringLiteral("Wait for the current operation to finish before deleting the project."));
        return;
    }
    QMessageBox confirmation(QMessageBox::Warning, QStringLiteral("Delete project"),
                             QStringLiteral("Delete “%1”?").arg(project_->displayName),
                             QMessageBox::NoButton, this);
    confirmation.setInformativeText(project_->installedVersion.isEmpty()
        ? QStringLiteral("This permanently removes the local vendor artifact, PKGBUILD, mappings, patches, build output, and history. This cannot be undone.")
        : QStringLiteral("This permanently removes the local vendor artifact, PKGBUILD, mappings, patches, build output, and history. The installed %1 package will be left in place. This cannot be undone.")
              .arg(project_->archPackageName));
    auto *deleteButton = confirmation.addButton(QStringLiteral("Delete Project"), QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != deleteButton) return;

    const auto deletedId = project_->id;
    const auto deletedName = project_->displayName.isEmpty()
        ? project_->archPackageName : project_->displayName;
    projectDeleteInFlight_ = true;
    setProjectListBusy(true, QStringLiteral("Deleting…"));
    statusBar()->showMessage(QStringLiteral("Deleting package %1…").arg(deletedName));
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<ProjectDeletionResult>(this);
    connect(watcher, &QFutureWatcher<ProjectDeletionResult>::finished, this,
            [this, watcher, deletedId, deletedName] {
        const auto result = watcher->result();
        watcher->deleteLater();
        projectDeleteInFlight_ = false;
        if (!result.succeeded) {
            setProjectListBusy(false);
            QMessageBox::critical(
                this, QStringLiteral("Could not delete project"),
                result.error.isEmpty() ? QStringLiteral("The library rejected the deletion")
                                       : result.error);
            return;
        }
        if (project_ && project_->id == deletedId) project_.reset();
        projectCache_.remove(deletedId);
        refreshProjectList({}, [this, deletedName](const bool succeeded) {
            if (succeeded) syncTrayUpdateCensus();
            statusBar()->showMessage(QStringLiteral("Deleted package %1").arg(deletedName), 8000);
        });
    });
    watcher->setFuture(QtConcurrent::run([config, deletedId] {
        LibraryClient client(config);
        ProjectDeletionResult result;
        result.succeeded = client.deleteProject(deletedId, &result.error);
        return result;
    }));
}

void MainWindow::populateOverview() {
    if (!project_) return;
    const auto icon = projectIcon(library_, *project_);
    const auto pixmap = icon.pixmap(96, 96);
    overviewIcon_->setPixmap(pixmap);
    overviewIcon_->setVisible(!pixmap.isNull());
    const auto *latest = project_->newestRelease();
    const auto latestText = latest == nullptr ? QStringLiteral("unknown")
                                               : latest->debian.version.toHtmlEscaped();
    projectStateLabel_->setText(project_->installedVersion.isEmpty()
        ? QStringLiteral("<b>Not installed.</b> Latest known vendor release: %1. Retained releases remain available for building or installation.")
              .arg(latestText)
        : project_->externallyInstalled
            ? QStringLiteral("<b>Externally installed:</b> %1. Latest known vendor release: %2. This pacman version does not match a retained PacSmith build; automatic update tracking is paused.")
                  .arg(project_->installedVersion.toHtmlEscaped(), latestText)
            : project_->hasAvailableUpdate()
                ? QStringLiteral("<b>Update available.</b> Installed: %1 from release %2. Latest known vendor release: %3.")
                      .arg(project_->installedVersion.toHtmlEscaped(),
                           project_->installedRelease() == nullptr
                               ? QStringLiteral("unknown")
                               : project_->installedRelease()->debian.version.toHtmlEscaped(),
                           latestText)
                : QStringLiteral("<b>Installed and up to date:</b> %1 from release %2. Latest known vendor release: %3.")
                  .arg(project_->installedVersion.toHtmlEscaped(),
                       project_->installedRelease() == nullptr
                           ? QStringLiteral("unknown")
                           : project_->installedRelease()->debian.version.toHtmlEscaped(),
                       latestText));
    const auto *tracker = project_->activeTrackingRelease();
    if (tracker == nullptr) {
        activeTrackerLabel_->setText(QStringLiteral("<b>Update monitoring:</b> paused — %1")
            .arg(project_->externallyInstalled || !project_->installedVersion.isEmpty()
                     ? QStringLiteral("installed package is not a retained PacSmith release")
                     : QStringLiteral("no analyzed release is available")));
        projectAcquisitionLabel_->setText(QStringLiteral("<b>Acquisition:</b> no active release"));
    } else if (tracker->update.strategy == UpdateStrategy::Manual) {
        activeTrackerLabel_->setText(QStringLiteral("<b>Active update configuration:</b> release %1 · Manual (no automatic checks)%2")
                                         .arg(tracker->debian.version.toHtmlEscaped(),
                                              project_->installedRelease() == tracker
                                                  ? QStringLiteral(" · installed")
                                                  : QStringLiteral(" · newest analyzed fallback")));
    } else {
        activeTrackerLabel_->setText(QStringLiteral("<b>Active update configuration:</b> release %1 · %2 · %3%4")
            .arg(tracker->debian.version.toHtmlEscaped(),
                 tracker->update.strategy == UpdateStrategy::AptRepository
                     ? QStringLiteral("signed APT repository")
                 : tracker->update.strategy == UpdateStrategy::RpmRepository
                     ? QStringLiteral("signed RPM repository")
                 : tracker->update.strategy == UpdateStrategy::GitHubRelease
                     ? QStringLiteral("GitHub releases") : QStringLiteral("direct URL"),
                 tracker->update.url.toHtmlEscaped(),
                 project_->installedRelease() == tracker
                     ? QStringLiteral(" · installed")
                     : QStringLiteral(" · newest analyzed fallback")));
    }
    if (tracker != nullptr) {
        const auto acquisitionLocation = !tracker->acquisition.originalUrl.isEmpty()
            ? tracker->acquisition.originalUrl
            : !tracker->sourceUrl.isEmpty() ? tracker->sourceUrl : tracker->originalSourceFilename;
        projectAcquisitionLabel_->setText(
            QStringLiteral("<b>Acquisition for release %1:</b> %2 · %3 · %4<br><b>Recorded SHA256:</b> %5")
                .arg(tracker->debian.version.toHtmlEscaped(),
                     acquisitionKindName(tracker->acquisition.kind).toHtmlEscaped(),
                     sourcePackageTypeName(tracker->sourceType).toHtmlEscaped(),
                     acquisitionLocation.toHtmlEscaped(), tracker->sourceSha256.toHtmlEscaped()));
    }

    const auto selectedBefore = selectedDashboardReleaseId().isEmpty()
        ? currentReleaseId_ : selectedDashboardReleaseId();
    QSignalBlocker tableBlocker(releaseTable_);
    QList<const PackageRelease *> ordered;
    for (const auto &release : project_->releases) ordered.append(&release);
    std::sort(ordered.begin(), ordered.end(), [](const auto *left, const auto *right) {
        return compareReleaseVersions(*left, *right) > 0;
    });
    releaseTable_->setRowCount(static_cast<int>(ordered.size()));
    int selectedRow = -1;
    for (int row = 0; row < ordered.size(); ++row) {
        const auto &release = *ordered.at(row);
        const bool preparing = project_->id == preparingProjectId_ &&
                               release.id == preparingReleaseId_;
        const auto unresolvedForRelease = std::count_if(
            release.dependencies.cbegin(), release.dependencies.cend(),
            [this](const auto &dependency) {
                return dependency.status == MappingStatus::Unresolved ||
                       repositoryPackageUnavailable(dependency,
                                                    repositoryDependencyAvailability_);
            });
        const auto reviewCount = release.state == ReleaseState::Discovered ? 0
            : unresolvedForRelease + pendingScriptFindings(release) + pendingPayloadReviews(release) +
                  (release.installMapping.appRun.requiresReview() ? 1 : 0) +
                  ((!release.lifecycleScript.contents.isEmpty() &&
                    (!release.lifecycleScript.validationPassed || release.lifecycleScript.requiresAcknowledgement())) ? 1 : 0);
        QString packageVersion;
        for (auto build = release.builds.crbegin(); build != release.builds.crend() && packageVersion.isEmpty(); ++build) {
            if (!build->artifacts.isEmpty()) packageVersion = build->artifacts.first().packageVersion;
        }
        const QStringList values{
            release.debian.version,
            preparing ? QStringLiteral("Preparing") : releaseStateName(release.state),
            release.state == ReleaseState::Discovered
                ? QStringLiteral("%1%2")
                      .arg(acquisitionKindName(release.acquisition.kind),
                           release.sourceSha256.isEmpty() ? QStringLiteral(" · unsigned")
                                                          : QStringLiteral(" · checksum"))
                : release.originalSourceFilename,
            release.update.strategy == UpdateStrategy::Manual ? QStringLiteral("Manual")
                : release.update.strategy == UpdateStrategy::DirectUrl ? QStringLiteral("Direct URL")
                : release.update.strategy == UpdateStrategy::AptRepository ? QStringLiteral("APT")
                : release.update.strategy == UpdateStrategy::RpmRepository ? QStringLiteral("RPM")
                                                                           : QStringLiteral("GitHub"),
            preparing ? QStringLiteral("Processing…")
            : release.state == ReleaseState::Discovered ? QStringLiteral("Not prepared")
                : reviewCount == 0 ? QStringLiteral("Ready")
                                   : QStringLiteral("%1 item(s)").arg(reviewCount),
            QString::number(release.builds.size()),
            packageVersion.isEmpty() ? QStringLiteral("—") : packageVersion,
            release.id == project_->installedReleaseId ? QStringLiteral("Yes") : QStringLiteral("—")};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setData(Qt::UserRole, release.id);
            releaseTable_->setItem(row, column, item);
        }
        if (release.id == selectedBefore) selectedRow = row;
    }
    releaseTable_->resizeColumnsToContents();
    if (selectedRow < 0 && !ordered.isEmpty()) selectedRow = 0;
    if (selectedRow >= 0) releaseTable_->selectRow(selectedRow);

    const auto unresolved = std::count_if(currentRelease()->dependencies.cbegin(), currentRelease()->dependencies.cend(),
                                          [this](const auto &dependency) {
                                              return dependency.status == MappingStatus::Unresolved ||
                                                     repositoryPackageUnavailable(
                                                         dependency,
                                                         repositoryDependencyAvailability_);
                                          });
    const auto scriptReviews = pendingScriptFindings(*currentRelease());
    const auto payloadReviews = pendingPayloadReviews(*currentRelease());
    QStringList lines{QStringLiteral("✓ Source analyzed and SHA256 recorded"),
                      currentRelease()->sourceType == SourcePackageType::Debian
                          ? QStringLiteral("✓ Debian metadata imported")
                      : currentRelease()->sourceType == SourcePackageType::Rpm
                          ? QStringLiteral("✓ RPM metadata imported")
                          : QStringLiteral("✓ Artifact metadata imported"),
                      unresolved == 0 ? QStringLiteral("✓ Dependencies resolved, available, or explicitly treated")
                                      : QStringLiteral("⚠ %1 dependency group(s) need attention").arg(unresolved),
                      currentRelease()->maintainerScripts.isEmpty()
                          ? QStringLiteral("✓ No maintainer scripts detected")
                          : scriptReviews == 0
                                ? QStringLiteral("✓ Maintainer-script responsibilities resolved")
                                : QStringLiteral("⚠ %1 script responsibility item(s) require resolution").arg(scriptReviews),
                      payloadReviews == 0
                          ? QStringLiteral("✓ Flagged payload files have explicit decisions")
                          : QStringLiteral("⚠ %1 payload file(s) need a keep/exclude decision").arg(payloadReviews),
                      currentRelease()->lifecycleScript.contents.isEmpty()
                          ? QStringLiteral("✓ No generated privileged lifecycle script")
                      : !currentRelease()->lifecycleScript.validationPassed
                          ? QStringLiteral("⚠ Generated lifecycle script failed validation")
                      : currentRelease()->lifecycleScript.requiresAcknowledgement()
                          ? QStringLiteral("⚠ Lifecycle script requires exact-content acknowledgement")
                          : QStringLiteral("✓ Generated lifecycle script acknowledged"),
                      QStringLiteral("✓ PKGBUILD present"),
                      currentRelease()->update.strategy == UpdateStrategy::Manual
                          ? QStringLiteral("○ Automatic update source not configured")
                      : currentRelease()->update.strategy == UpdateStrategy::DirectUrl
                          ? QStringLiteral("○ Direct URL configured; version discovery is not implemented")
                      : currentRelease()->update.strategy == UpdateStrategy::GitHubRelease
                          ? currentRelease()->update.lastChecked.isValid()
                              ? QStringLiteral("✓ GitHub releases checked: %1")
                                .arg(currentRelease()->update.detectedVersion.isEmpty()
                                         ? QStringLiteral("no version recorded")
                                         : currentRelease()->update.detectedVersion)
                              : QStringLiteral("○ GitHub release tracking configured; not checked yet")
                      : currentRelease()->update.lastChecked.isValid()
                          ? QStringLiteral("✓ APT repository checked: %1")
                                .arg(currentRelease()->update.detectedVersion.isEmpty()
                                         ? QStringLiteral("no version recorded")
                                         : currentRelease()->update.detectedVersion)
                          : currentRelease()->update.aptSigningKeyring.isEmpty() ||
                                    currentRelease()->update.trustedSigningFingerprint.isEmpty()
                              ? QStringLiteral("⚠ APT repository needs a trusted signing key")
                              : QStringLiteral("○ APT repository configured with pinned key; not checked yet")};
    overviewChecklist_->setText(QStringLiteral("<b>Selected release %1</b><br>%2")
                                    .arg(currentRelease()->debian.version.toHtmlEscaped(),
                                         lines.join(QStringLiteral("<br>"))));
    askAiButton_->setEnabled(currentRelease()->state != ReleaseState::Discovered);
    updateDashboardActions();
    tableBlocker.unblock();
    if (selectedRow >= 0) emit releaseTable_->itemSelectionChanged();
    if (!preparingReleaseId_.isEmpty()) updatePreparationIndicators();
}

QString MainWindow::selectedDashboardReleaseId() const {
    if (releaseTable_ == nullptr || releaseTable_->currentRow() < 0) return {};
    const auto *item = releaseTable_->item(releaseTable_->currentRow(), 0);
    return item == nullptr ? QString{} : item->data(Qt::UserRole).toString();
}

namespace {

QString repoPackageText(bool present, const RepoPackageRef &ref) {
    if (!present || ref.pkgname.isEmpty()) return QStringLiteral("None");
    const auto version = ref.version.isEmpty()
                             ? QStringLiteral("%1-%2").arg(ref.pkgver, ref.pkgrel)
                             : ref.version;
    return QStringLiteral("%1 %2 (%3)").arg(ref.pkgname, version, ref.arch);
}

QString repoTimestampText(const QString &value) {
    if (value.isEmpty()) return QStringLiteral("—");
    const auto parsed = QDateTime::fromString(value, Qt::ISODate);
    if (!parsed.isValid()) return value;
    return formatLocalDateTime(parsed);
}

QString soakStatusLabel(const QString &status) {
    if (status == QStringLiteral("soaking")) return QStringLiteral("Soaking");
    if (status == QStringLiteral("eligible")) return QStringLiteral("Eligible");
    if (status == QStringLiteral("promoted")) return QStringLiteral("Promoted");
    if (status == QStringLiteral("skipped")) return QStringLiteral("Skipped");
    return status;
}

} // namespace

void MainWindow::applyProjectRepository(const ProjectRepository &status) {
    if (project_) project_->repository = status;
    if (repoPublishCheck_ == nullptr) return;
    const QSignalBlocker publishBlocker(repoPublishCheck_);
    const QSignalBlocker overrideBlocker(repoOverrideEdit_);
    populating_ = true;
    repoPublishCheck_->setChecked(status.publish);
    repoOriginalName_->setText(status.originalPackageName.isEmpty()
                                   ? QStringLiteral("—")
                                   : status.originalPackageName);
    repoPrefixDefault_->setText(status.prefixDefault.isEmpty()
                                    ? QStringLiteral("—")
                                    : status.prefixDefault);
    repoOverrideEdit_->setText(status.packageNameOverride);
    repoEffectiveName_->setText(status.effectivePackageName.isEmpty()
                                    ? QStringLiteral("—")
                                    : status.effectivePackageName);
    repoPublishedName_->setText(status.publishedPackageName.isEmpty()
                                    ? QStringLiteral("Not published yet")
                                    : status.publishedPackageName);
    repoNameWarning_->setVisible(status.pkgnameChangeWarning || status.reserved);
    if (status.reserved) {
        repoNameWarning_->setText(
            QStringLiteral("%1 is reserved by PacSmith and cannot be used for a user project.")
                .arg(status.effectivePackageName));
    } else if (status.pkgnameChangeWarning) {
        repoNameWarning_->setText(
            QStringLiteral("Changing the published package name is a migration. Machines that already installed %1 will keep that name until they are updated.")
                .arg(status.publishedPackageName));
    }
    repoUnstableLabel_->setText(repoPackageText(status.hasUnstable, status.unstable));
    repoStableLabel_->setText(repoPackageText(status.hasStable, status.stable));
    repoSoakTable_->setRowCount(0);
    for (const auto &soak : status.soaks) {
        if (soak.status == QStringLiteral("promoted") || soak.status == QStringLiteral("skipped")) {
            continue;
        }
        const auto row = repoSoakTable_->rowCount();
        repoSoakTable_->insertRow(row);
        const auto version = soak.version.isEmpty()
                                 ? QStringLiteral("%1-%2").arg(soak.pkgver, soak.pkgrel)
                                 : soak.version;
        repoSoakTable_->setItem(row, 0, new QTableWidgetItem(version));
        repoSoakTable_->setItem(row, 1, new QTableWidgetItem(soak.arch));
        repoSoakTable_->setItem(row, 2, new QTableWidgetItem(soakStatusLabel(soak.status)));
        repoSoakTable_->setItem(row, 3, new QTableWidgetItem(repoTimestampText(soak.startedAt)));
        repoSoakTable_->setItem(row, 4, new QTableWidgetItem(repoTimestampText(soak.eligibleAt)));
    }
    repoSoakTable_->resizeColumnsToContents();
    repoPromoteButton_->setEnabled(status.hasUnstable || !status.soaks.isEmpty());
    repoStatusLabel_->clear();
    populating_ = false;
}

void MainWindow::populateRepository() {
    if (repoPublishCheck_ == nullptr) return;
    if (!project_) {
        applyProjectRepository({});
        if (repoStatusLabel_ != nullptr) {
            repoStatusLabel_->setText(QStringLiteral("Open a project to configure repository publication."));
        }
        if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(false);
        if (repoPromoteButton_ != nullptr) repoPromoteButton_->setEnabled(false);
        return;
    }
    if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(!repositoryOperationInFlight_);
    if (applyingServerRefresh_) {
        applyProjectRepository(project_->repository);
        return;
    }
    const auto generation = ++repositoryLoadGeneration_;
    const auto projectId = project_->id;
    const auto fallback = project_->repository;
    if (repoStatusLabel_ != nullptr) repoStatusLabel_->setText(QStringLiteral("Loading repository status…"));
    if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(false);
    if (repoPromoteButton_ != nullptr) repoPromoteButton_->setEnabled(false);
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<RepositoryTaskResult>(this);
    connect(watcher, &QFutureWatcher<RepositoryTaskResult>::finished, this,
            [this, watcher, generation, projectId, fallback] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != repositoryLoadGeneration_ || !project_ || project_->id != projectId) return;
        if (!result.status) {
            applyProjectRepository(fallback);
            if (repoStatusLabel_ != nullptr) {
                repoStatusLabel_->setText(result.error.isEmpty()
                                              ? QStringLiteral("Could not load repository status.")
                                              : result.error);
            }
            return;
        }
        applyProjectRepository(*result.status);
    });
    watcher->setFuture(QtConcurrent::run([config, projectId] {
        LibraryClient client(config);
        RepositoryTaskResult result;
        result.status = client.projectRepo(projectId, &result.error);
        return result;
    }));
}

bool MainWindow::saveProjectRepository() {
    if (!project_ || repositoryOperationInFlight_ || !ensureCurrentProjectWritable()) return false;
    const auto published = project_->repository.publishedPackageName;
    const auto effective = repoEffectiveName_ != nullptr ? repoEffectiveName_->text().trimmed()
                                                        : project_->repository.effectivePackageName;
    if (!published.isEmpty() && effective != published) {
        if (QMessageBox::warning(this, QStringLiteral("Change published package name?"),
                                 QStringLiteral("This project is already published as %1. Changing the effective package name is a migration, not a cosmetic rename. Installed machines will keep the old name until they are updated.\n\nContinue?")
                                     .arg(published),
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) != QMessageBox::Yes) {
            return false;
        }
    }
    const auto projectId = project_->id;
    const auto publish = repoPublishCheck_ != nullptr && repoPublishCheck_->isChecked();
    const auto overrideName = repoOverrideEdit_ != nullptr
        ? repoOverrideEdit_->text().trimmed() : QString{};
    const auto revision = project_->revision;
    repositoryOperationInFlight_ = true;
    if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(false);
    if (repoPromoteButton_ != nullptr) repoPromoteButton_->setEnabled(false);
    if (repoStatusLabel_ != nullptr) repoStatusLabel_->setText(QStringLiteral("Saving repository settings…"));
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<RepositoryTaskResult>(this);
    connect(watcher, &QFutureWatcher<RepositoryTaskResult>::finished, this,
            [this, watcher, projectId] {
        const auto result = watcher->result();
        watcher->deleteLater();
        repositoryOperationInFlight_ = false;
        if (!project_ || project_->id != projectId) return;
        if (!result.status) {
            if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(true);
            QMessageBox::critical(this, QStringLiteral("Could not save repository settings"), result.error);
            return;
        }
        applyProjectRepository(*result.status);
        project_->revision++;
        projectCache_.insert(project_->id, *project_);
        refreshCurrentProject();
    });
    watcher->setFuture(QtConcurrent::run([config, projectId, publish, overrideName, revision] {
        LibraryClient client(config);
        RepositoryTaskResult result;
        result.status = client.saveProjectRepo(projectId, publish, overrideName, revision, &result.error);
        return result;
    }));
    return true;
}

void MainWindow::promoteProjectRepository() {
    if (!project_ || repositoryOperationInFlight_ || !ensureCurrentProjectWritable()) return;
    if (QMessageBox::question(this, QStringLiteral("Promote to Stable"),
                              QStringLiteral("Promote the newest package that advances stable, bypassing remaining soak time? Stable is never downgraded.")) !=
        QMessageBox::Yes) {
        return;
    }
    const auto projectId = project_->id;
    repositoryOperationInFlight_ = true;
    if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(false);
    if (repoPromoteButton_ != nullptr) repoPromoteButton_->setEnabled(false);
    if (repoStatusLabel_ != nullptr) repoStatusLabel_->setText(QStringLiteral("Promoting package to stable…"));
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<RepositoryTaskResult>(this);
    connect(watcher, &QFutureWatcher<RepositoryTaskResult>::finished, this,
            [this, watcher, projectId] {
        const auto result = watcher->result();
        watcher->deleteLater();
        repositoryOperationInFlight_ = false;
        if (!project_ || project_->id != projectId) return;
        if (!result.status) {
            if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(true);
            QMessageBox::critical(this, QStringLiteral("Could not promote to stable"), result.error);
            return;
        }
        applyProjectRepository(*result.status);
        refreshCurrentProject();
    });
    watcher->setFuture(QtConcurrent::run([config, projectId] {
        LibraryClient client(config);
        RepositoryTaskResult result;
        result.status = client.promoteProjectRepo(projectId, &result.error);
        return result;
    }));
}

} // namespace pacsmith::gui
