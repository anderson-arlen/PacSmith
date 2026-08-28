#include "gui/main_window/common.hpp"
#include "core/harness_launcher.hpp"

#include <QClipboard>
#include <QGuiApplication>

namespace pacsmith::gui {
namespace {

struct BuildPollResult {
    std::optional<JobStatus> job;
    QString logChunk;
    QString error;
    qint64 nextOffset{0};
};

struct BuildFinishResult {
    std::optional<JobStatus> job;
    std::optional<Project> project;
    QString error;
};

struct ReleaseDeletionResult {
    bool succeeded{false};
    QString error;
};

struct RetentionCleanupResult {
    bool succeeded{false};
    QString error;
};

struct UpdateCheckJobTask {
    std::optional<JobStatus> job;
    QString log;
    QString error;
};

struct UpdatePreparationJobTask {
    std::optional<JobStatus> job;
    QString error;
};

struct InstallPreparationResult {
    std::optional<Project> project;
    QString releaseId;
    QString packagePath;
    QString error;
    bool lifecycleChanged{false};
};

} // namespace

void MainWindow::startReanalysis() {
    if (!project_ || currentRelease() == nullptr || importThread_ != nullptr ||
        buildInProgress() || packageOperationInProgress() ||
        serverImportRunning_ || repositoryImportRunning_) {
        return;
    }
    if (!ensureCurrentProjectWritable()) return;
    const auto projectId = project_->id;
    const auto releaseId = currentRelease()->id;
    const auto version = currentRelease()->debian.version;

    QMessageBox confirmation(
        QMessageBox::Warning, QStringLiteral("Reset and reanalyze release?"),
        QStringLiteral("Reset the package setup for %1?").arg(version),
        QMessageBox::NoButton, this);
    confirmation.setInformativeText(QStringLiteral(
        "PacSmith will verify and reread the stored artifact, then discard this release's dependency overrides, "
        "script and payload acknowledgements, lifecycle script, command mappings, desktop entries, "
        "icon selection, and manual PKGBUILD edits.\n\n"
        "The source artifact, update configuration, installed-package record, prior build history, and package "
        "artifacts are retained. This setup reset cannot be undone."));
    auto *resetButton = confirmation.addButton(
        QStringLiteral("Reset & Reanalyze"), QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != resetButton) return;

    importProgress_ = new QProgressDialog(
        QStringLiteral("Verifying the stored artifact…"), QString{}, 0, 0, this);
    importProgress_->setWindowTitle(QStringLiteral("Resetting Package Setup"));
    importProgress_->setWindowModality(Qt::WindowModal);
    importProgress_->setCancelButton(nullptr);
    importProgress_->setMinimumDuration(0);
    importProgress_->setAutoClose(false);
    importProgress_->setAutoReset(false);
    importProgress_->setMinimumWidth(460);
    importProgress_->show();

    auto *thread = new QThread(this);
    auto *worker = new ReanalyzeWorker(
        library_.projectsRoot(), projectId, releaseId);
    importThread_ = thread;
    projectList_->setEnabled(false);
    askAiButton_->setEnabled(false);
    reanalyzeButton_->setEnabled(false);
    updateDeleteButton();
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &ReanalyzeWorker::run);
    connect(worker, &ReanalyzeWorker::progressChanged, this,
            [this](const QString &description) {
                if (importProgress_ != nullptr) importProgress_->setLabelText(description);
                statusBar()->showMessage(description);
            });
    connect(worker, &ReanalyzeWorker::completed, this,
            [this, projectId, releaseId](const QString &completedProjectId,
                                         const QString &completedReleaseId,
                                         const QString &error) {
                if (importProgress_ != nullptr) {
                    importProgress_->close();
                    importProgress_->deleteLater();
                    importProgress_ = nullptr;
                }
                projectList_->setEnabled(!projectListRefreshInFlight_ && !projectDeleteInFlight_);
                if (completedProjectId.isEmpty()) {
                    statusBar()->clearMessage();
                    QMessageBox::critical(this, QStringLiteral("Reanalysis failed"), error);
                    updateDeleteButton();
                    return;
                }
                const auto completedId = completedReleaseId.isEmpty()
                    ? releaseId : completedReleaseId;
                refreshProjectList(projectId, [this, projectId, completedId](const bool succeeded) {
                    if (!succeeded || !project_ || project_->id != projectId) return;
                    currentReleaseId_ = completedId;
                    const auto *release = currentRelease();
                    const auto commands = release == nullptr
                        ? 0 : release->installMapping.launchers.size();
                    const auto desktopEntries = release == nullptr
                        ? 0 : release->installMapping.desktopEntries.size();
                    const auto iconDetected = release != nullptr &&
                        release->installMapping.icon.sourceKind != IconSourceKind::None;
                    showReleaseWorkbenchAtFirstAttention(currentReleaseId_);
                    statusBar()->showMessage(
                        QStringLiteral("Artifact reanalyzed from a blank package setup"), 12000);
                    QMessageBox::information(
                        this, QStringLiteral("Artifact reanalyzed"),
                        QStringLiteral(
                            "PacSmith rebuilt this release's setup from the stored artifact.\n\n"
                            "Detected commands: %1\nDesktop entries: %2\nIcon: %3\n\n"
                            "The first section that still needs attention is now open.")
                            .arg(commands)
                            .arg(desktopEntries)
                            .arg(iconDetected ? QStringLiteral("detected")
                                              : QStringLiteral("not detected")));
                    updateDeleteButton();
                });
            });
    connect(worker, &ReanalyzeWorker::completed, thread, &QThread::quit);
    connect(worker, &ReanalyzeWorker::completed, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread] {
        if (importProgress_ != nullptr) {
            importProgress_->close();
            importProgress_->deleteLater();
            importProgress_ = nullptr;
        }
        if (importThread_ == thread) importThread_ = nullptr;
        projectList_->setEnabled(!projectListRefreshInFlight_ && !projectDeleteInFlight_);
        askAiButton_->setEnabled(currentRelease() != nullptr &&
                                 currentRelease()->state != ReleaseState::Discovered);
        updateDeleteButton();
        thread->deleteLater();
    });
    thread->start();
}

void MainWindow::askExternalHarness() {
    if (!project_ || currentRelease() == nullptr) return;
    const auto *profile = appSettings_.defaultHarness();
    if (profile == nullptr) {
        QMessageBox::information(
            this, QStringLiteral("Configure an AI harness"),
            QStringLiteral("Add a generic external harness launch profile in Settings → AI Harnesses first."));
        return;
    }
    const auto projectId = project_->id;
    const auto releaseId = currentRelease()->id;
    QString prompt;
    if (currentSection() == EditorSection::ConfigDependencies && dependenciesTable_ != nullptr &&
        dependenciesTable_->currentRow() >= 0 &&
        dependenciesTable_->currentRow() < currentRelease()->dependencies.size()) {
        prompt = HarnessLauncher::dependencyPrompt(
            projectId, releaseId,
            currentRelease()->dependencies.at(dependenciesTable_->currentRow()).rawExpression);
    } else if (currentSection() == EditorSection::ConfigAppRun) {
        prompt = HarnessLauncher::appImagePrompt(projectId, releaseId);
    } else if (currentRelease()->pkgbuildManuallyModified &&
               (currentSection() == EditorSection::ConfigPkgbuild ||
                currentSection() == EditorSection::ResultPkgbuild)) {
        prompt = HarnessLauncher::customPkgbuildPrompt(projectId, releaseId);
    } else if (currentSection() == EditorSection::ResultBuild &&
               currentRelease()->buildStatus == BuildStatus::Failed) {
        prompt = HarnessLauncher::buildFailurePrompt(projectId, releaseId);
    } else {
        prompt = HarnessLauncher::projectPrompt(projectId, releaseId);
    }
    const auto launched = HarnessLauncher::launch(*profile, prompt);
    if (!launched.started) {
        QMessageBox::critical(this, QStringLiteral("Could not launch AI harness"), launched.error);
        return;
    }
    if (launched.promptNeedsClipboard) {
        QGuiApplication::clipboard()->setText(prompt);
        QMessageBox::information(
            this, QStringLiteral("AI harness launched"),
            QStringLiteral("The profile has no {prompt} placeholder, so the PacSmith context prompt was copied to the clipboard."));
    }
}

void MainWindow::applyRetentionCleanup() {
    if (!project_ || retentionCleanupInFlight_) return;
    const auto projectId = project_->id;
    const auto releaseId = currentReleaseId_;
    const auto config = library_.config();
    retentionCleanupInFlight_ = true;
    statusBar()->showMessage(QStringLiteral("Cleaning up excess outdated package versions…"));
    auto *watcher = new QFutureWatcher<RetentionCleanupResult>(this);
    connect(watcher, &QFutureWatcher<RetentionCleanupResult>::finished, this,
            [this, watcher, projectId, releaseId] {
        const auto cleanup = watcher->result();
        watcher->deleteLater();
        retentionCleanupInFlight_ = false;
        if (!cleanup.succeeded) {
            statusBar()->showMessage(
                cleanup.error.isEmpty() ? QStringLiteral("Package cleanup failed") : cleanup.error,
                8000);
            return;
        }
        statusBar()->showMessage(QStringLiteral("Excess outdated package versions cleaned up"), 5000);
        refreshProjectList(projectId);
        if (!project_ || project_->id != projectId) return;
        if (project_->release(releaseId) == nullptr) {
            if (const auto *newest = project_->newestRelease()) currentReleaseId_ = newest->id;
            else currentReleaseId_.clear();
        }
        refreshCurrentProject();
    });
    watcher->setFuture(QtConcurrent::run([config] {
        LibraryClient client(config);
        RetentionCleanupResult result;
        const auto cleanup = client.cleanup(&result.error);
        result.succeeded = !cleanup.skipped && result.error.isEmpty();
        return result;
    }));
}

void MainWindow::startUpdateCheck() {
    if (!project_ || updateCheckRunning_ || updateConfigurationSaveInFlight_ ||
        serverImportRunning_ || repositoryImportRunning_ || importThread_ != nullptr) return;
    auto *tracker = updateEditorRelease();
    if (tracker == nullptr) return;
    const auto projectId = project_->id;
    const auto releaseId = tracker->id;
    const auto projectName = project_->displayName.isEmpty() ? project_->archPackageName
                                                             : project_->displayName;
    const auto automaticStrategy = updateStrategy_->currentIndex() >= 1 &&
                                   updateStrategy_->currentIndex() <= 4;
    saveUpdateConfigurationThen(
        [this, projectId, releaseId, projectName, automaticStrategy](const bool saved) {
            if (!saved) return;
            if (!automaticStrategy) {
                QMessageBox::information(
                    this, QStringLiteral("Update check"),
                    QStringLiteral("Select Direct URL, an APT or RPM repository, or GitHub releases to check automatically."));
                return;
            }
            startUpdateCheckRequest(projectId, releaseId, projectName);
        });
}

void MainWindow::startUpdateCheckRequest(const QString &projectId, const QString &releaseId,
                                         const QString &projectName) {
    if (updateCheckRunning_) return;
    updateCheckButton_->setEnabled(false);
    if (historyCheckUpdatesButton_ != nullptr) historyCheckUpdatesButton_->setEnabled(false);
    projectList_->setEnabled(false);
    updateCheckRunning_ = true;
    setUpdateCheckStatus(QStringLiteral("Requesting daemon update check…"));
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<UpdateCheckJobTask>(this);
    connect(watcher, &QFutureWatcher<UpdateCheckJobTask>::finished, this,
            [this, watcher, projectId] {
        const auto task = watcher->result();
        watcher->deleteLater();
        updateCheckRunning_ = false;
        publishUpdateCheckActivity(false, projectId);
        projectList_->setEnabled(!projectListRefreshInFlight_ && !projectDeleteInFlight_);
        syncUpdateCheckButtons();
        updateDeleteButton();
        if (!task.job || task.job->status != QStringLiteral("succeeded")) {
            const auto message = !task.error.isEmpty() ? task.error
                : task.job && !task.job->error.isEmpty() ? task.job->error
                                                        : QStringLiteral("The daemon update check failed");
            if (project_ && project_->id == projectId) setUpdateCheckStatus(message, true);
            statusBar()->showMessage(message, 10000);
            return;
        }
        const auto checks = task.job->result.value(QStringLiteral("checks")).toArray();
        const auto checked = checks.isEmpty() ? QJsonObject{} : checks.first().toObject();
        const auto message = checked.value(QStringLiteral("message")).toString(
            QStringLiteral("Update check completed"));
        if (project_ && project_->id == projectId) setUpdateCheckStatus(message);
        statusBar()->showMessage(message, 10000);
        refreshProjectList(projectId);
        syncTrayUpdateCensus();
    });
    watcher->setFuture(QtConcurrent::run([config, releaseId] {
        UpdateCheckJobTask task;
        LibraryClient client(config);
        const auto started = client.startUpdateCheck(releaseId, true, &task.error);
        if (!started) return task;
        task.job = client.waitForJob(started->id, &task.error);
        task.log = client.jobLog(started->id, nullptr);
        return task;
    }));
    publishUpdateCheckActivity(true, projectId, projectName);
    updateDeleteButton();
}

void MainWindow::showCommandProgress(const QString &title, const QString &status,
                                     const bool cancelable) {
    if (commandProgress_ != nullptr) {
        auto *previous = commandProgress_;
        commandProgress_ = nullptr;
        previous->hide();
        previous->deleteLater();
    }
    auto *dialog = new CommandProgressDialog(this);
    commandProgress_ = dialog;
    dialog->setWindowTitle(title);
    dialog->setStatus(status);
    dialog->setCancelable(cancelable);
    connect(dialog, &CommandProgressDialog::cancelRequested, this, [this, dialog] {
        if (commandProgress_ != dialog || !buildInProgress()) return;
        cancelRemoteBuild();
        dialog->setStatus(QStringLiteral("Canceling build…"));
        dialog->setCancelable(false);
        dialog->appendOutput(QStringLiteral("\n[PacSmith] Cancel requested.\n"));
    });
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (commandProgress_ == dialog) commandProgress_ = nullptr;
        dialog->deleteLater();
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::finishCommandProgress(const bool success, const QString &summary) {
    if (commandProgress_ == nullptr) return;
    commandProgress_->markFinished(success, summary);
}

void MainWindow::startBuild(const bool installWhenSuccessful, const bool automatic) {
    installAfterSuccessfulBuild_ = false;
    if (!project_ || currentRelease() == nullptr || buildInProgress() ||
        !ensureCurrentProjectWritable()) return;
    bool lifecycleChanged = false;
    QString lifecycleError;
    if (!library_.synchronizeLifecycle(*project_, *currentRelease(), &lifecycleChanged, &lifecycleError)) {
        QMessageBox::critical(this, QStringLiteral("Could not inspect lifecycle script"), lifecycleError);
        return;
    }
    if (lifecycleChanged) {
        populateScripts();
        populateOverview();
        populateBuild();
        QMessageBox::information(
            this, QStringLiteral("Lifecycle file synchronized"),
            QStringLiteral("PacSmith restored a missing project-local .install file from its exact recorded content, or detected a direct edit. Prior build results were cleared. Review Configuration → Scripts, then build again."));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ConfigScripts);
        return;
    }
    if (!currentRelease()->lifecycleScript.contents.isEmpty() &&
        !currentRelease()->lifecycleScript.validationPassed) {
        QMessageBox::warning(
            this, QStringLiteral("Lifecycle script is not buildable"),
            QStringLiteral("The PKGBUILD expects '%1', but its recorded lifecycle content is missing or failed validation. Open Configuration → Scripts and repair or remove the lifecycle script before building.\n\n%2")
                .arg(currentRelease()->lifecycleScript.fileName,
                     currentRelease()->lifecycleScript.validationMessage));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ConfigScripts);
        return;
    }
    QStringList unavailablePackages;
    QStringList uncheckedPackages;
    if (repositoryCatalogLoaded_) {
        for (const auto &dependency : currentRelease()->dependencies) {
            const bool required = !dependency.ignored && !dependency.bundled &&
                                  !dependency.provided &&
                                  dependency.status != MappingStatus::Ignored &&
                                  dependency.status != MappingStatus::Bundled &&
                                  dependency.status != MappingStatus::Provided;
            if (required && !dependency.archPackage.isEmpty()) {
                if (!repositoryDependencyAvailability_.contains(dependency.archPackage)) {
                    uncheckedPackages.append(dependency.archPackage);
                } else if (!repositoryDependencyAvailability_.value(dependency.archPackage)) {
                    unavailablePackages.append(dependency.archPackage);
                }
            }
        }
    }
    uncheckedPackages.removeDuplicates();
    if (!uncheckedPackages.isEmpty()) {
        scheduleRepositoryPackageValidation(uncheckedPackages);
        QMessageBox::information(
            this, QStringLiteral("Checking Arch dependencies"),
            QStringLiteral("PacSmith is still checking these dependency names against the configured pacman repositories:\n\n%1\n\nThe build has not started. Try again after the Dependencies page finishes checking them.")
                .arg(uncheckedPackages.join(QLatin1Char('\n'))));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ConfigDependencies);
        return;
    }
    unavailablePackages.removeDuplicates();
    if (!unavailablePackages.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("Unavailable Arch dependencies"),
            QStringLiteral("PacSmith will not start makepkg because these required package names are absent from every configured pacman repository:\n\n%1\n\nOpen Dependencies and choose an available package from the suggestions, mark the dependency with an evidence-backed treatment, or leave it unresolved for explicit review.")
                .arg(unavailablePackages.join(QLatin1Char('\n'))));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ConfigDependencies);
        return;
    }
    const auto unresolved = std::count_if(currentRelease()->dependencies.cbegin(), currentRelease()->dependencies.cend(),
                                          [](const auto &dependency) {
                                              return dependency.status == MappingStatus::Unresolved;
                                          });
    if (unresolved > 0 && QMessageBox::warning(this, QStringLiteral("Unresolved dependencies"),
                                               QStringLiteral("%1 dependency group(s) are unresolved. Build anyway?").arg(unresolved),
                                               QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    if (!savePkgbuild()) return;
    const auto pkgbuildText = currentPkgbuildText();
    auto lifecycleReference = pkgbuildLifecycleReference(pkgbuildText);
    if (lifecycleReference == QStringLiteral("$_PACSMITH_INSTALL") ||
        lifecycleReference == QStringLiteral("${_PACSMITH_INSTALL}")) {
        lifecycleReference = currentRelease()->lifecycleScript.fileName;
    }
    if (!lifecycleReference.isEmpty() &&
        (lifecycleReference.contains(QLatin1Char('/')) ||
         (!lifecycleReference.contains(QLatin1Char('$')) &&
          currentRelease()->lifecycleScript.fileName != lifecycleReference &&
          currentRelease()->lifecycleScript.contents.isEmpty()))) {
        QMessageBox::warning(
            this, QStringLiteral("PKGBUILD lifecycle file is unavailable"),
            QStringLiteral("The PKGBUILD references install='%1', but PacSmith cannot verify a regular project-local file with that literal name. Repair the lifecycle file on Configuration → Scripts or correct the Custom PKGBUILD before building.")
                .arg(lifecycleReference));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(currentRelease()->lifecycleScript.contents.isEmpty()
                          ? (currentRelease()->pkgbuildManuallyModified
                                 ? EditorSection::ConfigPkgbuild : EditorSection::ResultPkgbuild)
                          : EditorSection::ConfigScripts);
        return;
    }
    if (!currentRelease()->lifecycleScript.contents.isEmpty() &&
        currentRelease()->lifecycleScript.validationPassed &&
        !pkgbuildReferencesLifecycle(pkgbuildText,
                                     currentRelease()->lifecycleScript.fileName)) {
        QMessageBox::warning(
            this, QStringLiteral("PKGBUILD omits lifecycle script"),
            QStringLiteral("The validated lifecycle script '%1' is not referenced by the PKGBUILD. "
                           "Add install='%1' or install=\"${_PACSMITH_INSTALL}\" in Custom mode or switch back to Guided before building.")
                .arg(currentRelease()->lifecycleScript.fileName));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(currentRelease()->pkgbuildManuallyModified
                          ? EditorSection::ConfigPkgbuild : EditorSection::ConfigScripts);
        return;
    }
    currentRelease()->buildStatus = BuildStatus::Building;
    persistCurrent();
    populateOverview();
    if (!installWhenSuccessful) {
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ResultBuild);
    }
    installAfterSuccessfulBuild_ = installWhenSuccessful;
    showCommandProgress(QStringLiteral("Building %1").arg(project_->displayName),
                        QStringLiteral("Running makepkg…"), true);
    QString error;
    const auto job = library_.startBuild(currentRelease()->id, &error, automatic);
    if (!job || job->id.isEmpty()) {
        currentRelease()->buildStatus = BuildStatus::Failed;
        finishCommandProgress(false, QStringLiteral("Build could not start: %1").arg(error));
        populateBuild();
        updateDashboardActions();
        updateDeleteButton();
        return;
    }
    buildJobId_ = job->id;
    buildProjectId_ = project_->id;
    buildReleaseId_ = currentRelease()->id;
    buildProjectName_ = project_->displayName.isEmpty() ? project_->archPackageName
                                                       : project_->displayName;
    buildLogContents_.clear();
    buildLogAfter_ = 0;
    if (buildPollTimer_ == nullptr) {
        buildPollTimer_ = new QTimer(this);
        connect(buildPollTimer_, &QTimer::timeout, this, &MainWindow::pollBuildJob);
    }
    buildPollTimer_->start(100);
    updatePreparationIndicators();
    syncActivityTimer();
    populateBuild();
    updateDashboardActions();
    updateDeleteButton();
}

bool MainWindow::buildInProgress() const {
    return !buildJobId_.isEmpty();
}

bool MainWindow::releaseBuildInProgress(const QString &releaseId) const {
    if (releaseId.isEmpty()) return false;
    if (buildInProgress() && buildReleaseId_ == releaseId) return true;
    return std::any_of(activeBuildJobs_.cbegin(), activeBuildJobs_.cend(),
                       [&](const auto &event) { return event.releaseId == releaseId; });
}

QString MainWindow::buildActivityForProject(const QString &projectId) const {
    if (projectId.isEmpty()) return {};
    const bool active = (buildInProgress() && buildProjectId_ == projectId) ||
        std::any_of(activeBuildJobs_.cbegin(), activeBuildJobs_.cend(),
                    [&](const auto &event) { return event.projectId == projectId; });
    return active ? QStringLiteral("Building...") : QString{};
}

void MainWindow::showBuildOutput() {
    if (!buildInProgress()) return;
    if (commandProgress_ == nullptr) {
        showCommandProgress(QStringLiteral("Building %1").arg(buildProjectName_),
                            QStringLiteral("Running makepkg..."), true);
        if (commandProgress_ != nullptr && !buildLogContents_.isEmpty()) {
            commandProgress_->appendOutput(buildLogContents_);
        }
    } else {
        commandProgress_->show();
        commandProgress_->raise();
        commandProgress_->activateWindow();
    }
}

void MainWindow::cancelRemoteBuild() {
    if (buildJobId_.isEmpty()) return;
    const auto config = library_.config();
    const auto jobId = buildJobId_;
    statusBar()->showMessage(QStringLiteral("Canceling build…"));
    QThreadPool::globalInstance()->start([config, jobId] {
        LibraryClient client(config);
        QString error;
        static_cast<void>(client.cancelJob(jobId, &error));
    });
}

void MainWindow::pollBuildJob() {
    if (buildJobId_.isEmpty() || buildPollInFlight_ || buildFinishInFlight_) return;
    buildPollInFlight_ = true;
    const auto config = library_.config();
    const auto jobId = buildJobId_;
    const auto after = buildLogAfter_;
    auto *watcher = new QFutureWatcher<BuildPollResult>(this);
    connect(watcher, &QFutureWatcher<BuildPollResult>::finished, this,
            [this, watcher, jobId] {
        const auto result = watcher->result();
        watcher->deleteLater();
        buildPollInFlight_ = false;
        if (jobId != buildJobId_ || !result.job) return;
        if (!result.logChunk.isEmpty()) {
            buildLogContents_.append(result.logChunk);
            if (commandProgress_ != nullptr) commandProgress_->appendOutput(result.logChunk);
        }
        buildLogAfter_ = result.nextOffset;
        if (result.job->status == QStringLiteral("succeeded") ||
            result.job->status == QStringLiteral("failed") ||
            result.job->status == QStringLiteral("interrupted")) {
            if (buildPollTimer_ != nullptr) buildPollTimer_->stop();
            finishBuildJob();
        }
    });
    watcher->setFuture(QtConcurrent::run([config, jobId, after] {
        LibraryClient client(config);
        BuildPollResult result;
        result.job = client.getJob(jobId, &result.error);
        result.nextOffset = after;
        if (result.job) {
            result.logChunk = client.jobLog(jobId, after, &result.nextOffset, nullptr);
        }
        return result;
    }));
}

void MainWindow::finishBuildJob() {
    if (buildFinishInFlight_ || buildJobId_.isEmpty()) return;
    buildFinishInFlight_ = true;
    const auto jobId = buildJobId_;
    const auto projectId = buildProjectId_;
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<BuildFinishResult>(this);
    connect(watcher, &QFutureWatcher<BuildFinishResult>::finished, this,
            [this, watcher, jobId, projectId] {
        auto result = watcher->result();
        watcher->deleteLater();
        buildFinishInFlight_ = false;
        if (jobId != buildJobId_) return;
        buildJobId_.clear();
        buildProjectId_.clear();
        buildReleaseId_.clear();
        buildProjectName_.clear();
        activeBuildJobs_.remove(jobId);
        updatePreparationIndicators();
        syncActivityTimer();
        if (!projectId.isEmpty() && result.project && result.project->id == projectId) {
            project_ = std::move(*result.project);
            projectCache_.insert(project_->id, *project_);
        }
        const bool succeeded = result.job && result.job->status == QStringLiteral("succeeded");
        const bool canceled = result.job && result.job->status == QStringLiteral("interrupted");
        populateOverview();
        populateBuild();
        populateHistory();
        updateDeleteButton();
        updateDashboardActions();
        const auto shouldInstall = succeeded && !canceled && installAfterSuccessfulBuild_;
        installAfterSuccessfulBuild_ = false;
        finishCommandProgress(succeeded && !canceled,
                              canceled ? QStringLiteral("Build canceled.")
                              : succeeded ? QStringLiteral("Build succeeded.")
                                          : QStringLiteral("Build failed."));
        statusBar()->showMessage(canceled ? QStringLiteral("Build canceled")
                                 : succeeded ? QStringLiteral("Build succeeded")
                                             : QStringLiteral("Build failed"), 8000);
        if (shouldInstall) QTimer::singleShot(0, this, [this] { startInstall(); });
    });
    watcher->setFuture(QtConcurrent::run([config, jobId, projectId] {
        LibraryClient client(config);
        BuildFinishResult result;
        result.job = client.getJob(jobId, &result.error);
        if (!projectId.isEmpty()) result.project = client.load(projectId, &result.error);
        return result;
    }));
}

void MainWindow::startInstall() {
    if (!project_ || currentRelease() == nullptr || packageOperationInProgress()) return;
    if (!currentRelease()->lifecycleScript.contents.isEmpty() &&
        (!currentRelease()->lifecycleScript.validationPassed || currentRelease()->lifecycleScript.requiresAcknowledgement())) {
        QMessageBox::warning(this, QStringLiteral("Installation blocked"),
                             QStringLiteral("The package contains a generated Arch lifecycle script that pacman will run as root. "
                                            "Open Scripts, review it, and acknowledge the exact content before installation."));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ConfigScripts);
        return;
    }
    if (!releaseHasRetainedPackage(*currentRelease())) {
        QMessageBox::warning(this, QStringLiteral("Installation unavailable"),
                             QStringLiteral("No retained Arch package artifact exists for this release. Build it first."));
        return;
    }
    const auto cachedPackage = retainedPackagePath(library_, *currentRelease());
    const auto packageDescription = cachedPackage.isEmpty()
        ? QStringLiteral("Retained package artifact for release %1 (downloaded from the PacSmith library if needed)")
              .arg(currentRelease()->debian.version)
        : cachedPackage;
    const auto lifecycleNotice = currentRelease()->lifecycleScript.contents.isEmpty()
                                     ? QString{}
                                     : QStringLiteral("\n\nPacman will also run the acknowledged lifecycle functions in %1 as root.")
                                           .arg(currentRelease()->lifecycleScript.fileName);
    if (QMessageBox::question(this, QStringLiteral("Install Arch package"),
                              QStringLiteral("Authorize pacman to install this package? PacSmith will run only pacman -U for the path below and pass --noconfirm after this explicit confirmation. Polkit may request your password.\n\n%1%2")
                                  .arg(packageDescription, lifecycleNotice)) !=
        QMessageBox::Yes) return;
    preparePackageInstallation(currentRelease()->id, QStringLiteral("install"), true);
}

bool MainWindow::packageOperationInProgress() const {
    return installPreparationInFlight_ || packageOperationFinishInFlight_ ||
           installService_.isRunning();
}

void MainWindow::preparePackageInstallation(const QString &releaseId,
                                            const QString &operation,
                                            const bool synchronizeLifecycle) {
    if (!project_ || packageOperationInProgress()) return;
    const auto *release = project_->release(releaseId);
    if (release == nullptr) return;
    installPreparationInFlight_ = true;
    projectList_->setEnabled(false);
    showCommandProgress(
        operation == QStringLiteral("rollback")
            ? QStringLiteral("Rolling back %1").arg(project_->displayName)
            : QStringLiteral("Installing %1").arg(project_->displayName),
        QStringLiteral("Preparing the retained package…"), false);
    statusBar()->showMessage(QStringLiteral("Preparing package installation…"));
    updateDashboardActions();
    populateBuild();
    updateDeleteButton();

    const auto config = library_.config();
    const auto projectSnapshot = *project_;
    auto *watcher = new QFutureWatcher<InstallPreparationResult>(this);
    connect(watcher, &QFutureWatcher<InstallPreparationResult>::finished, this,
            [this, watcher, operation] {
        auto result = watcher->result();
        watcher->deleteLater();
        installPreparationInFlight_ = false;
        if (result.project && project_ && result.project->id == project_->id) {
            project_ = std::move(*result.project);
            projectCache_.insert(project_->id, *project_);
        }
        if (!result.error.isEmpty()) {
            projectList_->setEnabled(!projectListRefreshInFlight_ && !projectDeleteInFlight_);
            finishCommandProgress(false,
                                  QStringLiteral("Package preparation failed: %1").arg(result.error));
            statusBar()->showMessage(QStringLiteral("Package preparation failed"), 10000);
            populateOverview();
            populateBuild();
            updateDashboardActions();
            updateDeleteButton();
            return;
        }
        if (result.lifecycleChanged) {
            projectList_->setEnabled(!projectListRefreshInFlight_ && !projectDeleteInFlight_);
            finishCommandProgress(false, QStringLiteral("Installation blocked because the lifecycle file changed."));
            populateScripts();
            populateOverview();
            populateBuild();
            updateDashboardActions();
            updateDeleteButton();
            QMessageBox::warning(this, QStringLiteral("Installation blocked"),
                                 QStringLiteral("The lifecycle file changed after the last build. Re-review it and rebuild the package."));
            return;
        }
        if (installService_.isRunning()) {
            projectList_->setEnabled(!projectListRefreshInFlight_ && !projectDeleteInFlight_);
            finishCommandProgress(false, QStringLiteral("Another package operation started first."));
            updateDashboardActions();
            updateDeleteButton();
            return;
        }
        pendingPackageOperation_ = operation;
        const auto status = operation == QStringLiteral("rollback")
            ? QStringLiteral("Waiting for polkit authorization to roll back…")
            : QStringLiteral("Waiting for polkit authorization to install…");
        statusBar()->showMessage(status);
        if (commandProgress_ != nullptr) {
            commandProgress_->setStatus(status);
            commandProgress_->appendOutput(
                QStringLiteral("Requesting narrowly scoped privilege elevation for non-interactive pacman -U…\n%1\n")
                    .arg(result.packagePath));
        }
        installService_.start(
            std::filesystem::path(result.packagePath.toUtf8().constData()),
            InstallPrivilegeMode::Polkit);
        populateBuild();
        updateDashboardActions();
        updateDeleteButton();
    });
    watcher->setFuture(QtConcurrent::run(
        [config, projectSnapshot, releaseId, synchronizeLifecycle] mutable {
            LibraryClient client(config);
            InstallPreparationResult result;
            result.releaseId = releaseId;
            auto project = projectSnapshot;
            auto *candidate = project.release(releaseId);
            if (candidate == nullptr) {
                result.error = QStringLiteral("The selected release is no longer available");
                return result;
            }
            if (synchronizeLifecycle &&
                !client.synchronizeLifecycle(project, *candidate, &result.lifecycleChanged,
                                             &result.error)) {
                return result;
            }
            candidate = project.release(releaseId);
            if (candidate == nullptr) {
                result.error = QStringLiteral("The selected release is no longer available");
                return result;
            }
            if (!result.lifecycleChanged) {
                result.packagePath = acquireRetainedPackagePath(client, *candidate, &result.error);
            }
            result.project = std::move(project);
            return result;
        }));
}

void MainWindow::editSelectedRelease() {
    if (!project_) return;
    const auto id = selectedDashboardReleaseId();
    const auto *release = project_->release(id);
    if (release == nullptr || release->state == ReleaseState::Discovered) return;
    showReleaseWorkbench(id);
}

void MainWindow::prepareSelectedRelease() {
    if (!project_) return;
    const auto id = selectedDashboardReleaseId();
    if (id == preparingReleaseId_) {
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
    beginReleasePreparation(id, true);
}

void MainWindow::beginReleasePreparation(const QString &releaseId,
                                         const bool askForConfirmation) {
    if (!project_ || !preparingReleaseId_.isEmpty() || importThread_ != nullptr) return;
    const auto *release = project_->release(releaseId);
    if (release == nullptr || release->state != ReleaseState::Discovered) return;
    const auto integrity = release->sourceSha256.isEmpty()
        ? QStringLiteral("No publisher digest was provided; PacSmith will record the locally computed SHA256 and show the source as unsigned.")
        : QStringLiteral("PacSmith will require publisher SHA256 %1.").arg(release->sourceSha256);
    if (askForConfirmation && QMessageBox::question(
            this, QStringLiteral("Prepare vendor release artifact"),
            QStringLiteral("Download release %1 from %2, inspect it as untrusted data, and create its package setup?\n\n%3")
                .arg(release->debian.version, acquisitionKindName(release->acquisition.kind), integrity)) !=
        QMessageBox::Yes) return;

    preparingProjectId_ = project_->id;
    preparingReleaseId_ = release->id;
    preparationPhase_ = QStringLiteral("Downloading");
    preparationBytesReceived_ = 0;
    preparationBytesTotal_ = -1;
    preparationSpinnerFrame_ = 0;
    lastPreparationPublish_.invalidate();
    downloadProgress_ = new QProgressDialog(
        QStringLiteral("The PacSmith daemon is downloading and inspecting the vendor artifact…\n"
                       "You may hide this window; preparation will continue on the server."),
        QStringLiteral("Hide"), 0, 0, this);
    downloadProgress_->setWindowTitle(
        QStringLiteral("Downloading %1 %2").arg(project_->displayName, release->debian.version));
    downloadProgress_->setWindowModality(Qt::NonModal);
    downloadProgress_->setMinimumDuration(0);
    downloadProgress_->setAutoClose(false);
    downloadProgress_->setAutoReset(false);
    downloadProgress_->setMinimumWidth(460);
    downloadProgress_->show();
    preparationSpinnerTimer_->start();
    populateOverview();
    publishPreparationActivity();
    updatePreparationIndicators();
    updateUpdateCheckIndicators();
    const auto projectId = project_->id;
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<UpdatePreparationJobTask>(this);
    connect(watcher, &QFutureWatcher<UpdatePreparationJobTask>::finished, this,
            [this, watcher, projectId] {
        const auto task = watcher->result();
        watcher->deleteLater();
        const bool succeeded = task.job && task.job->status == QStringLiteral("succeeded");
        const auto error = !task.error.isEmpty() ? task.error
            : task.job && !task.job->error.isEmpty() ? task.job->error : QString{};
        resetPreparationState();
        refreshProjectList(projectId);
        if (succeeded) {
            statusBar()->showMessage(QStringLiteral("Release prepared by pacsmithd"), 8000);
        } else {
            QMessageBox::critical(this, QStringLiteral("Could not prepare release"),
                                  error.isEmpty() ? QStringLiteral("The daemon rejected release preparation")
                                                  : error);
        }
    });
    watcher->setFuture(QtConcurrent::run([config, releaseId] {
        UpdatePreparationJobTask task;
        LibraryClient client(config);
        const auto started = client.startUpdatePreparation(releaseId, &task.error);
        if (!started) return task;
        task.job = client.waitForJob(started->id, &task.error);
        return task;
    }));
}

void MainWindow::deleteSelectedRelease() {
    if (!project_ || !ensureCurrentProjectWritable()) return;
    const auto id = selectedDashboardReleaseId();
    const auto *release = project_->release(id);
    if (release == nullptr || id == project_->installedReleaseId) return;
    if (QMessageBox::warning(
            this, QStringLiteral("Delete retained release"),
            QStringLiteral("Permanently delete release %1, including its vendor artifact, settings, PKGBUILD, and built artifacts?")
                .arg(release->debian.version),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    const auto projectId = project_->id;
    const auto version = release->debian.version;
    const auto projectSnapshot = *project_;
    setProjectListBusy(true, QStringLiteral("Deleting release…"));
    statusBar()->showMessage(QStringLiteral("Deleting release %1…").arg(version));
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<ReleaseDeletionResult>(this);
    connect(watcher, &QFutureWatcher<ReleaseDeletionResult>::finished, this,
            [this, watcher, projectId, version] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!result.succeeded) {
            setProjectListBusy(false);
            QMessageBox::critical(
                this, QStringLiteral("Could not delete release"),
                result.error.isEmpty() ? QStringLiteral("The library rejected the deletion")
                                       : result.error);
            return;
        }
        refreshProjectList(projectId, [this, version](const bool succeeded) {
            if (succeeded && project_) {
                refreshCurrentProject();
                syncTrayUpdateCensus();
            }
            statusBar()->showMessage(QStringLiteral("Deleted release %1").arg(version), 8000);
        });
    });
    watcher->setFuture(QtConcurrent::run([config, projectSnapshot, id] {
        LibraryClient client(config);
        auto mutableProject = projectSnapshot;
        ReleaseDeletionResult result;
        result.succeeded = client.deleteRelease(mutableProject, id, &result.error);
        return result;
    }));
}

void MainWindow::rollbackSelectedRelease() {
    if (!project_ || packageOperationInProgress()) return;
    const auto id = selectedDashboardReleaseId();
    const auto *release = project_->release(id);
    if (release == nullptr || id == project_->installedReleaseId) return;
    if (!releaseHasRetainedPackage(*release)) {
        QMessageBox::warning(this, QStringLiteral("Rollback unavailable"),
                             QStringLiteral("That release no longer has a retained Arch package artifact."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Roll back package"),
                              QStringLiteral("Authorize pacman to install retained release %1 non-interactively? PacSmith will download it from the configured library first if it is not cached locally. Polkit may request your password.")
                                  .arg(release->debian.version)) != QMessageBox::Yes) return;
    currentReleaseId_ = id;
    preparePackageInstallation(id, QStringLiteral("rollback"), false);
}

void MainWindow::installSelectedRelease() {
    if (!project_ || packageOperationInProgress()) return;
    const auto id = selectedDashboardReleaseId();
    const auto *release = project_->release(id);
    if (release == nullptr || !releaseHasRetainedPackage(*release)) return;
    currentReleaseId_ = id;
    startInstall();
}

void MainWindow::startUninstall() {
    if (!project_ || project_->installedVersion.isEmpty() || packageOperationInProgress()) return;
    if (QMessageBox::question(this, QStringLiteral("Uninstall package"),
                              QStringLiteral("Authorize pacman to remove %1 non-interactively? The PacSmith project and retained releases will remain. Polkit may request your password.")
                                  .arg(project_->archPackageName)) != QMessageBox::Yes) return;
    pendingPackageOperation_ = QStringLiteral("uninstall");
    if (project_->installedRelease() != nullptr) currentReleaseId_ = project_->installedReleaseId;
    projectList_->setEnabled(false);
    showCommandProgress(QStringLiteral("Uninstalling %1").arg(project_->displayName),
                        QStringLiteral("Waiting for polkit authorization…"), false);
    if (commandProgress_ != nullptr) {
        commandProgress_->appendOutput(
            QStringLiteral("Removing pacman package %1\n").arg(project_->archPackageName));
    }
    installService_.startUninstall(project_->archPackageName,
                                   InstallPrivilegeMode::Polkit);
    populateOverview();
}


} // namespace pacsmith::gui
