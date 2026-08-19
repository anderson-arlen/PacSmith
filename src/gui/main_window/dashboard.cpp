#include "gui/main_window/common.hpp"

namespace pacsmith::gui {

void MainWindow::updateDashboardActions() {
    const bool installed = project_ && !project_->installedVersion.isEmpty();
    if (uninstallButton_ != nullptr) {
        uninstallButton_->setVisible(installed);
        uninstallButton_->setEnabled(installed && !installService_.isRunning());
    }
    const auto *tracker = project_ ? project_->activeTrackingRelease() : nullptr;
    const auto *editTarget = dashboardActionRelease();
    if (editTarget == nullptr) editTarget = tracker;
    if (editConfigurationButton_ != nullptr) {
        editConfigurationButton_->setVisible(editTarget != nullptr &&
                                             editTarget->state != ReleaseState::Discovered);
    }
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
    const bool hasPackage = !retainedPackagePath(store_, *tracker).isEmpty();
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
    if (pendingScriptFindings(release) > 0) return EditorSection::ConfigScripts;
    if (lifecycleReview) return EditorSection::ConfigScripts;
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
    const bool busy = buildService_.isRunning() || installService_.isRunning();
    if (target == nullptr) {
        projectActionNotice_->setVisible(false);
        projectActionButton_->setVisible(false);
        return;
    }
    const bool isUpdate = !project_->installedVersion.isEmpty();
    const bool needsReview = firstReviewSection(*target).has_value();
    projectActionNotice_->setVisible(true);
    projectActionButton_->setVisible(true);
    projectActionButton_->setEnabled(!busy);
    if (needsReview) {
        projectActionNotice_->setText(
            isUpdate ? QStringLiteral("This update has changes that need review.")
                     : QStringLiteral("This package has changes that need review."));
        projectActionButton_->setText(QStringLiteral("Review Changes"));
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
    const bool hasPackage = !retainedPackagePath(store_, *tracker).isEmpty();
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
    const auto *target = dashboardActionRelease();
    if (target == nullptr) target = project_->activeTrackingRelease();
    if (target == nullptr || target->state == ReleaseState::Discovered) return;
    showReleaseWorkbench(target->id);
}

void MainWindow::showProjectDashboard() {
    if (rightStack_ == nullptr) return;
    if (projectSidebar_ != nullptr) projectSidebar_->show();
    rightStack_->setCurrentIndex(0);
    placeUpdatesEditor();
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
           !buildService_.isRunning() && !installService_.isRunning();
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
        applyAvailableUpdateCensus(updateState, store_.list());
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
    return projectId;
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
    if (!known && !store_.load(projectId, nullptr)) return;
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

void MainWindow::reloadVisibleProjects() {
    const auto projectId = project_ ? project_->id : QString{};
    refreshProjectList(projectId);
    if (!projectId.isEmpty() && (!project_ || project_->id != projectId)) loadProject(projectId);
    if (project_) refreshCurrentProject();
    syncActivityTimer();
    updateUpdateCheckIndicators();
}

void MainWindow::syncTrayUpdateCensus() {
    static_cast<void>(BackgroundUpdateStateStore::syncAvailableUpdates(store_.list()));
    static_cast<void>(GuiInstanceServer::requestTray());
}

void MainWindow::refreshProjectList(const QString &selectId) {
    const auto previous = selectId.isEmpty() && project_ ? project_->id : selectId;
    projectList_->clear();
    QString error;
    const auto projects = store_.list(&error);
    projectCache_.clear();
    projectCache_.reserve(projects.size());
    for (const auto &project : projects) projectCache_.insert(project.id, project);
    QSet<QString> projectIds;
    const auto updateState = BackgroundUpdateStateStore::load();
    for (const auto &project : projects) {
        projectIds.insert(project.id);
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
        item->setIcon(projectIcon(store_, project));
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
    QString managedError;
    for (const auto &managed : ManagedPackageRegistry::installed(&managedError)) {
        if (projectIds.contains(managed.projectId())) continue;
        auto *item = new QListWidgetItem(managed.packageName, projectList_);
        item->setData(projectSubtitleRole, QStringLiteral("⚠ PacSmith project files are missing"));
        item->setData(projectVisualStateRole,
                      static_cast<int>(ProjectVisualState::Attention));
        item->setSizeHint(QSize(0, 60));
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable & ~Qt::ItemIsEnabled);
        item->setToolTip(
            QStringLiteral("Pacman xdata identifies this as PacSmith-managed release %1, but its local project directory is missing. Source: %2")
                .arg(managed.releaseId(), managed.sourceIdentity()));
    }
    if (projectList_->currentItem() == nullptr && projectList_->count() > 0) projectList_->setCurrentRow(0);
    if (!error.isEmpty()) statusBar()->showMessage(error, 8000);
    if (!managedError.isEmpty()) statusBar()->showMessage(managedError, 8000);
    if (!preparingProjectId_.isEmpty() || !updateState.preparingProjectId.isEmpty()) {
        updatePreparationIndicators();
    }
    syncActivityTimer();
    updateUpdateCheckIndicators();
    if (projects.isEmpty()) {
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
    }
}

void MainWindow::loadProject(const QString &id) {
    lifecycleEditing_ = false;
    const auto cached = projectCache_.constFind(id);
    if (cached != projectCache_.cend()) {
        project_ = cached.value();
    } else {
        QString error;
        auto loaded = store_.load(id, &error);
        if (!loaded) {
            QMessageBox::critical(this, QStringLiteral("Could not load project"), error);
            return;
        }
        project_ = std::move(*loaded);
        projectCache_.insert(project_->id, *project_);
    }
    const auto *initialRelease = project_->activeTrackingRelease();
    if (initialRelease == nullptr) initialRelease = project_->newestRelease();
    currentReleaseId_ = initialRelease == nullptr ? QString{} : initialRelease->id;
    stageTabs_->setEnabled(true);
    projectTabs_->setEnabled(true);
    showProjectDashboard();
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
    const bool busy = buildService_.isRunning() || installService_.isRunning() ||
                      aptUpdateService_->isRunning() || rpmUpdateService_->isRunning() ||
                      githubUpdateService_->isRunning() ||
                      aiService_.isRunning() || debDownloadService_->isRunning() ||
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
    if (!project_) return;
    if (project_->ownsInstalledPackage()) {
        QMessageBox::warning(this, QStringLiteral("Project cannot be deleted"),
                             QStringLiteral("%1 is installed. Uninstall it with pacman before deleting its PacSmith project.")
                                 .arg(project_->archPackageName));
        return;
    }
    if (buildService_.isRunning() || installService_.isRunning() ||
        aptUpdateService_->isRunning() || rpmUpdateService_->isRunning() ||
        githubUpdateService_->isRunning() ||
        aiService_.isRunning() || debDownloadService_->isRunning() ||
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

    const auto deletedName = project_->displayName;
    QString error;
    if (!store_.deleteProject(*project_, &error)) {
        QMessageBox::critical(this, QStringLiteral("Could not delete project"), error);
        return;
    }
    project_.reset();
    refreshProjectList();
    syncTrayUpdateCensus();
    statusBar()->showMessage(QStringLiteral("Deleted project %1").arg(deletedName), 8000);
}

void MainWindow::populateOverview() {
    if (!project_) return;
    const auto icon = projectIcon(store_, *project_);
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
                          ? QStringLiteral("⚠ AI-generated lifecycle script requires exact-content acknowledgement")
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
    resolveWithAiButton_->setEnabled(!aiService_.isRunning() &&
                                     currentRelease()->state != ReleaseState::Discovered);
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

} // namespace pacsmith::gui
