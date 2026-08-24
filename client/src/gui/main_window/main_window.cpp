#include "gui/main_window/common.hpp"
#include "core/daemon_control.hpp"
#include "gui/connection_dialog.hpp"

#include <QFontMetrics>
#include <QProgressBar>

namespace pacsmith::gui {

MainWindow::MainWindow(AppSettingsStore &settingsStore, CredentialStore &credentials,
                       QWidget *parent)
    : QMainWindow(parent), settingsStore_(settingsStore), aiSettings_(settingsStore_.load()),
      credentialStore_(credentials), buildService_(this),
      installService_(this), signingKeyDownloadService_(this),
      chatGptLoginService_(this) {
    setWindowTitle(QStringLiteral("PacSmith"));
    setAcceptDrops(true);
    qRegisterMetaType<UpdateCheckResult>();
    networkIoThread_.setObjectName(QStringLiteral("pacsmith-network-io"));
    debDownloadService_ = new DebDownloadService;
    aptUpdateService_ = new AptUpdateService;
    rpmUpdateService_ = new RpmUpdateService;
    githubUpdateService_ = new GitHubUpdateService;
    debDownloadService_->moveToThread(&networkIoThread_);
    aptUpdateService_->moveToThread(&networkIoThread_);
    rpmUpdateService_->moveToThread(&networkIoThread_);
    githubUpdateService_->moveToThread(&networkIoThread_);
    networkIoThread_.start();

    auto *githubAction = new QAction(QStringLiteral("GitHub Link…"), this);
    auto *packageFileAction = new QAction(QStringLiteral("Package File…"), this);
    auto *directUrlAction = new QAction(QStringLiteral("Direct Download URL…"), this);
    auto *aptRepositoryAction = new QAction(QStringLiteral("APT Repository…"), this);
    auto *rpmRepositoryAction = new QAction(QStringLiteral("RPM Repository…"), this);
    packageFileAction->setShortcut(QKeySequence::Open);
    connect(githubAction, &QAction::triggered, this, &MainWindow::importGitHubUrl);
    connect(packageFileAction, &QAction::triggered, this, &MainWindow::chooseImport);
    connect(directUrlAction, &QAction::triggered, this, &MainWindow::importDirectUrl);
    connect(aptRepositoryAction, &QAction::triggered, this,
            &MainWindow::importAptRepository);
    connect(rpmRepositoryAction, &QAction::triggered, this,
            &MainWindow::importRpmRepository);

    auto *splitter = new QSplitter(this);
    auto *leftPanel = new QWidget(splitter);
    projectSidebar_ = leftPanel;
    auto *leftLayout = new QVBoxLayout(leftPanel);
    auto *packagesHeader = new QHBoxLayout;
    auto *packagesLabel = new QLabel(QStringLiteral("<b>Packages</b>"), leftPanel);
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"), leftPanel);
    auto refreshIcon = QIcon::fromTheme(QStringLiteral("view-refresh"));
    if (refreshIcon.isNull()) refreshIcon = style()->standardIcon(QStyle::SP_BrowserReload);
    refreshButton->setIcon(refreshIcon);
    refreshButton->setToolTip(QStringLiteral("Reload the package list and the selected package from the library."));
    refreshButton->setShortcut(QKeySequence::Refresh);
    packagesHeader->addWidget(packagesLabel, 1);
    packagesHeader->addWidget(refreshButton, 0, Qt::AlignRight);
    projectList_ = new QListWidget(leftPanel);
    projectList_->setMinimumWidth(260);
    projectList_->setIconSize(QSize(44, 44));
    projectList_->setSpacing(2);
    projectList_->setItemDelegate(new ProjectListDelegate(projectList_));
    preparationSpinnerTimer_ = new QTimer(this);
    preparationSpinnerTimer_->setInterval(160);
    connect(preparationSpinnerTimer_, &QTimer::timeout, this, [this] {
        preparationSpinnerFrame_ = (preparationSpinnerFrame_ + 1) % 4;
        updatePreparationIndicators();
        updateUpdateCheckIndicators();
        syncActivityTimer();
    });
    deleteProjectButton_ = new QPushButton(QStringLiteral("Delete"), leftPanel);
    deleteProjectButton_->setEnabled(false);
    deleteProjectButton_->setToolTip(QStringLiteral("Delete the selected project"));
    auto *newButton = new QPushButton(QStringLiteral("New"), leftPanel);
    auto *sidebarNewMenu = new QMenu(newButton);
    sidebarNewMenu->addAction(githubAction);
    sidebarNewMenu->addAction(packageFileAction);
    sidebarNewMenu->addAction(directUrlAction);
    sidebarNewMenu->addSeparator();
    sidebarNewMenu->addAction(aptRepositoryAction);
    sidebarNewMenu->addAction(rpmRepositoryAction);
    newButton->setMenu(sidebarNewMenu);
    newButton->setToolTip(QStringLiteral("Create a project from a repository, GitHub release, package file, or direct download URL"));
    auto *settingsButton = new QPushButton(QStringLiteral("Settings"), leftPanel);
    settingsButton->setToolTip(QStringLiteral("Configure library connection, AI providers, update checks, remote listening, and credentials"));
    leftLayout->addLayout(packagesHeader);
    leftLayout->addWidget(projectList_, 1);
    auto *projectButtons = new QHBoxLayout;
    projectButtons->addWidget(newButton);
    projectButtons->addWidget(deleteProjectButton_);
    projectButtons->addWidget(settingsButton);
    leftLayout->addLayout(projectButtons);
    connect(deleteProjectButton_, &QPushButton::clicked, this, &MainWindow::deleteCurrentProject);
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::refreshLibraryView);
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::showSettings);

    auto *rightPanel = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightStack_ = new QStackedWidget(rightPanel);

    auto *dashboard = new QWidget(rightStack_);
    auto *dashboardLayout = new QVBoxLayout(dashboard);
    projectTitle_ = new QLabel(QStringLiteral("<h2>PacSmith</h2>"), dashboard);
    projectSubtitle_ = new QLabel(QStringLiteral("Import a vendor artifact or GitHub release to begin."), dashboard);
    projectSubtitle_->setWordWrap(true);
    projectPrimaryButton_ = new QPushButton(dashboard);
    applyPrimaryActionStyle(projectPrimaryButton_);
    projectPrimaryButton_->setVisible(false);
    uninstallButton_ = new QPushButton(QStringLiteral("Uninstall"), dashboard);
    uninstallButton_->setVisible(false);
    auto *dashboardHeader = new QHBoxLayout;
    auto *dashboardHeading = new QVBoxLayout;
    dashboardHeading->addWidget(projectTitle_);
    dashboardHeading->addWidget(projectSubtitle_);
    dashboardHeader->addLayout(dashboardHeading, 1);
    dashboardHeader->addWidget(projectPrimaryButton_, 0, Qt::AlignTop);
    dashboardHeader->addWidget(uninstallButton_, 0, Qt::AlignTop);
    connect(projectPrimaryButton_, &QPushButton::clicked, this,
            &MainWindow::handleProjectPrimaryAction);
    connect(uninstallButton_, &QPushButton::clicked, this, &MainWindow::startUninstall);
    projectTabs_ = new QTabWidget(dashboard);
    dashboardUpdatesHost_ = emptyPageHost(dashboard);
    updatesEditor_ = createUpdatesPage();
    dashboardUpdatesHost_->layout()->addWidget(updatesEditor_);
    dashboardRepositoryHost_ = emptyPageHost(dashboard);
    repositoryEditor_ = createRepositoryPage();
    dashboardRepositoryHost_->layout()->addWidget(repositoryEditor_);
    projectTabs_->addTab(createProjectInfoPage(), QStringLiteral("Project Info"));
    projectTabs_->addTab(createOverviewPage(), QStringLiteral("Versions"));
    projectTabs_->addTab(dashboardUpdatesHost_, QStringLiteral("Update Monitoring"));
    projectTabs_->addTab(dashboardRepositoryHost_, QStringLiteral("Repository"));
    connect(projectTabs_, &QTabWidget::currentChanged, this, [this] {
        if (rightStack_ == nullptr || rightStack_->currentIndex() != 0) return;
        if (projectTabs_->currentWidget() == dashboardUpdatesHost_) {
            placeUpdatesEditor();
            if (project_) populateUpdates();
        } else if (projectTabs_->currentWidget() == dashboardRepositoryHost_) {
            placeRepositoryEditor();
            if (project_) populateRepository();
        }
    });
    dashboardLayout->addLayout(dashboardHeader);
    dashboardBodyStack_ = new QStackedWidget(dashboard);
    dashboardLoadingPage_ = new QWidget(dashboardBodyStack_);
    auto *loadingLayout = new QVBoxLayout(dashboardLoadingPage_);
    loadingLayout->setAlignment(Qt::AlignCenter);
    auto *loadingSpinner = new QProgressBar(dashboardLoadingPage_);
    loadingSpinner->setRange(0, 0);
    loadingSpinner->setTextVisible(false);
    loadingSpinner->setFixedWidth(240);
    auto *loadingLabel = new QLabel(QStringLiteral("Loading package…"), dashboardLoadingPage_);
    loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLayout->addWidget(loadingSpinner, 0, Qt::AlignCenter);
    loadingLayout->addWidget(loadingLabel, 0, Qt::AlignCenter);
    dashboardBodyStack_->addWidget(projectTabs_);
    dashboardBodyStack_->addWidget(dashboardLoadingPage_);
    dashboardBodyStack_->setCurrentWidget(projectTabs_);
    dashboardLayout->addWidget(dashboardBodyStack_, 1);

    auto *workbench = new QWidget(rightStack_);
    auto *workbenchLayout = new QVBoxLayout(workbench);
    auto *workbenchHeader = new QHBoxLayout;
    auto *backButton = new QPushButton(QStringLiteral("← Back to Project"), workbench);
    auto *workbenchHeading = new QVBoxLayout;
    auto *titleRow = new QHBoxLayout;
    workbenchTitle_ = new QLabel(QStringLiteral("<h2>Package Setup</h2>"), workbench);
    sourceTypeBadge_ = new QLabel(workbench);
    sourceTypeBadge_->setObjectName(QStringLiteral("sourceTypeBadge"));
    sourceTypeBadge_->setAlignment(Qt::AlignCenter);
    sourceTypeBadge_->setStyleSheet(QStringLiteral(
        "QLabel#sourceTypeBadge { background: rgba(52, 152, 219, 36); border: 1px solid #2e86ab; "
        "border-radius: 4px; padding: 4px 10px; font-weight: 600; }"));
    titleRow->addWidget(workbenchTitle_, 0, Qt::AlignVCenter);
    titleRow->addWidget(sourceTypeBadge_, 0, Qt::AlignVCenter);
    titleRow->addStretch();
    workbenchSubtitle_ = new QLabel(
        QStringLiteral("Inspect the imported artifact, configure the Pacman package, then review the generated recipe."),
        workbench);
    workbenchSubtitle_->setWordWrap(true);
    workbenchHeading->addLayout(titleRow);
    workbenchHeading->addWidget(workbenchSubtitle_);
    workbenchHeader->addWidget(backButton, 0, Qt::AlignTop);
    workbenchHeader->addLayout(workbenchHeading, 1);
    reanalyzeButton_ = new QPushButton(QStringLiteral("Reanalyze Artifact…"), workbench);
    reanalyzeButton_->setToolTip(QStringLiteral(
        "Discard this release's package-setup decisions and rebuild them from the stored artifact"));
    workbenchHeader->addWidget(reanalyzeButton_, 0, Qt::AlignTop);
    resolveWithAiButton_ = new QPushButton(QStringLiteral("Resolve Review Items with AI"), workbench);
    workbenchHeader->addWidget(resolveWithAiButton_, 0, Qt::AlignTop);
    stageTabs_ = new QTabWidget(workbench);
    auto *sourceHost = createStageHost(&sourceNav_, &sourceStack_);
    auto *modeSwitch = new QWidget(workbench);
    modeSwitch->setObjectName(QStringLiteral("configModeSwitch"));
    auto *modeLayout = new QHBoxLayout(modeSwitch);
    modeLayout->setContentsMargins(0, 0, 0, 0);
    modeLayout->setSpacing(0);
    guidedModeButton_ = new QPushButton(QStringLiteral("Guided"), modeSwitch);
    customModeButton_ = new QPushButton(QStringLiteral("Custom"), modeSwitch);
    guidedModeButton_->setObjectName(QStringLiteral("configModeGuided"));
    customModeButton_->setObjectName(QStringLiteral("configModeCustom"));
    guidedModeButton_->setCheckable(true);
    customModeButton_->setCheckable(true);
    guidedModeButton_->setCursor(Qt::PointingHandCursor);
    customModeButton_->setCursor(Qt::PointingHandCursor);
    guidedModeButton_->setToolTip(QStringLiteral("Generate the PKGBUILD from these settings"));
    customModeButton_->setToolTip(QStringLiteral("Edit the PKGBUILD directly; Guided settings are ignored"));
    auto *modeGroup = new QButtonGroup(modeSwitch);
    modeGroup->setExclusive(true);
    modeGroup->addButton(guidedModeButton_);
    modeGroup->addButton(customModeButton_);
    guidedModeButton_->setChecked(true);
    modeLayout->addWidget(guidedModeButton_, 1);
    modeLayout->addWidget(customModeButton_, 1);
    modeSwitch->setStyleSheet(QStringLiteral(
        "QPushButton#configModeGuided, QPushButton#configModeCustom {"
        "  padding: 5px 0; border: 1px solid #5a7a90; background: transparent; }"
        "QPushButton#configModeGuided { border-top-left-radius: 4px; border-bottom-left-radius: 4px;"
        "  border-right: none; }"
        "QPushButton#configModeCustom { border-top-right-radius: 4px; border-bottom-right-radius: 4px; }"
        "QPushButton#configModeGuided:checked, QPushButton#configModeCustom:checked {"
        "  background: rgba(52, 152, 219, 70); font-weight: 600; }"));
    connect(guidedModeButton_, &QPushButton::toggled, this, [this](const bool checked) {
        if (checked) setConfigurationMode(false);
    });
    connect(customModeButton_, &QPushButton::toggled, this, [this](const bool checked) {
        if (checked) setConfigurationMode(true);
    });
    auto *configHost = createStageHost(&configNav_, &configStack_, modeSwitch);
    auto *resultHost = createStageHost(&resultNav_, &resultStack_);
    stageTabs_->addTab(sourceHost, QStringLiteral("Deb Package"));
    stageTabs_->addTab(configHost, QStringLiteral("Configuration"));
    stageTabs_->addTab(resultHost, QStringLiteral("Pacman Package"));
    stageTabs_->setTabToolTip(0, QStringLiteral("Imported vendor artifact: inspect metadata, original scripts, and payload"));
    stageTabs_->setTabToolTip(1, QStringLiteral("Guided settings generate the PKGBUILD, or switch to Custom to edit it directly"));
    stageTabs_->setTabToolTip(2, QStringLiteral("What the Pacman package will install, the PKGBUILD makepkg runs, and the built package"));
    {
        QSignalBlocker sourceBlock(sourceNav_);
        QSignalBlocker configBlock(configNav_);
        QSignalBlocker resultBlock(resultNav_);
        addWorkbenchPage(0, sourceNav_, sourceStack_, EditorSection::SourceOverview,
                         QStringLiteral("Overview"), createSourceOverviewPage());
        addWorkbenchPage(0, sourceNav_, sourceStack_, EditorSection::SourceMetadata,
                         QStringLiteral("Metadata"), createSourceMetadataPage());
        addWorkbenchPage(0, sourceNav_, sourceStack_, EditorSection::SourceScripts,
                         QStringLiteral("Vendor scripts"), createVendorScriptsPage());
        addWorkbenchPage(0, sourceNav_, sourceStack_, EditorSection::SourceContents,
                         QStringLiteral("Contents"), createPayloadPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigPkgbuild,
                         QStringLiteral("PKGBUILD"), createPkgbuildPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigLayout,
                         QStringLiteral("Install layout"), createInstallLayoutPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigDependencies,
                         QStringLiteral("Dependencies"), createDependenciesPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigScripts,
                         QStringLiteral("Scripts"), createConfigScriptsPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigCommands,
                         QStringLiteral("Commands"), createCommandsPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigAppRun,
                         QStringLiteral("AppRun"), createAppRunPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigDesktopEntries,
                         QStringLiteral("Desktop entries"), createDesktopEntriesPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigIcon,
                         QStringLiteral("Icon"), createIconPage());
        configUpdatesHost_ = emptyPageHost(workbench);
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigUpdates,
                         QStringLiteral("Updates"), configUpdatesHost_);
        configRepositoryHost_ = emptyPageHost(workbench);
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigRepository,
                         QStringLiteral("Repository"), configRepositoryHost_);
        addWorkbenchPage(2, resultNav_, resultStack_, EditorSection::ResultInstallPlan,
                         QStringLiteral("Install plan"), createInstallPlanPage());
        addWorkbenchPage(2, resultNav_, resultStack_, EditorSection::ResultPkgbuild,
                         QStringLiteral("PKGBUILD"), createResultPkgbuildPage());
        addWorkbenchPage(2, resultNav_, resultStack_, EditorSection::ResultBuild,
                         QStringLiteral("Build"), createBuildPage());
    }
    sourceNav_->setCurrentRow(0);
    configNav_->setCurrentRow(1);
    resultNav_->setCurrentRow(0);
    iconNetwork_ = new QNetworkAccessManager(this);
    workbenchLayout->addLayout(workbenchHeader);
    workbenchLayout->addWidget(stageTabs_, 1);
    connect(backButton, &QPushButton::clicked, this, &MainWindow::showProjectDashboard);
    connect(reanalyzeButton_, &QPushButton::clicked, this, &MainWindow::startReanalysis);
    connect(resolveWithAiButton_, &QPushButton::clicked, this, &MainWindow::startAiResolution);
    connect(stageTabs_, &QTabWidget::currentChanged, this, [this] {
        updateWorkbenchStageChrome();
        if (rightStack_ != nullptr && rightStack_->currentIndex() == 1) {
            populateCurrentWorkbenchPage();
        }
    });

    rightStack_->addWidget(dashboard);
    rightStack_->addWidget(workbench);
    rightStack_->setCurrentWidget(dashboard);
    rightLayout->addWidget(rightStack_, 1);
    splitter->addWidget(leftPanel);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    connect(projectList_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current) {
                if (current == nullptr) return;
                const auto id = current->data(Qt::UserRole).toString();
                if (!id.isEmpty()) loadProjectInteractively(id);
            });

    connect(&buildService_, &BuildService::outputAvailable, this, [this](const QString &text) {
        if (commandProgress_ != nullptr) commandProgress_->appendOutput(text);
    });
    connect(&buildService_, &BuildService::failedToStart, this, [this](const QString &message) {
        if (project_ && currentRelease()->buildStatus == BuildStatus::Building) {
            currentRelease()->buildStatus = BuildStatus::Failed;
            currentRelease()->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("build"),
                                      QStringLiteral("Build could not start")});
            persistCurrent();
        }
        finishCommandProgress(false, QStringLiteral("Build could not start: %1").arg(message));
        installAfterSuccessfulBuild_ = false;
        if (commandProgress_ == nullptr) {
            QMessageBox::critical(this, QStringLiteral("Build could not start"), message);
        }
        populateBuild();
        updateDashboardActions();
        updateDeleteButton();
    });
    connect(&buildService_, &BuildService::finished, this, [this](const ProcessResult &result) {
        if (!project_) return;
        currentRelease()->buildStatus = result.canceled ? BuildStatus::Canceled
                                                : result.succeeded() ? BuildStatus::Succeeded
                                                                     : BuildStatus::Failed;
        currentRelease()->lastBuildLog = result.output + result.errorOutput;
        if (!result.canceled) currentRelease()->producedPackages = result.producedPackages;
        currentRelease()->builds.append(buildRecordFromResult(
            QStringLiteral("build-%1").arg(result.startedAt.toMSecsSinceEpoch()),
            currentRelease()->buildStatus, currentRelease()->lastBuildLog,
            result.producedPackages, library_.releasePath(*currentRelease()),
            result.startedAt, result.finishedAt));
        if (result.succeeded()) currentRelease()->state = ReleaseState::Built;
        currentRelease()->history.append({result.finishedAt, QStringLiteral("build"),
                                  result.canceled ? QStringLiteral("Build canceled by user")
                                  : result.succeeded() ? QStringLiteral("Build succeeded")
                                                       : QStringLiteral("Build failed (exit %1)").arg(result.exitCode)});
        persistCurrent();
        populateOverview();
        populateBuild();
        populateHistory();
        updateDeleteButton();
        const auto shouldInstall = result.succeeded() && !result.canceled && installAfterSuccessfulBuild_;
        installAfterSuccessfulBuild_ = false;
        finishCommandProgress(!result.canceled && result.succeeded(),
                              result.canceled ? QStringLiteral("Build canceled.")
                              : result.succeeded() ? QStringLiteral("Build succeeded.")
                                                   : QStringLiteral("Build failed (exit %1).")
                                                         .arg(result.exitCode));
        statusBar()->showMessage(result.canceled ? QStringLiteral("Build canceled")
                                 : result.succeeded() ? QStringLiteral("Build succeeded")
                                                      : QStringLiteral("Build failed"), 8000);
        if (shouldInstall) QTimer::singleShot(0, this, [this] { startInstall(); });
    });
    connect(&installService_, &InstallService::outputAvailable, this, [this](const QString &text) {
        if (commandProgress_ != nullptr) commandProgress_->appendOutput(text);
    });
    connect(&installService_, &InstallService::progressChanged, this,
            [this](const QString &message) {
                statusBar()->showMessage(message);
                if (commandProgress_ != nullptr && !commandProgress_->isFinished()) {
                    commandProgress_->setStatus(message);
                }
            });
    connect(&installService_, &InstallService::failedToStart, this, [this](const QString &message) {
        projectList_->setEnabled(true);
        pendingPackageOperation_.clear();
        if (project_) {
            currentRelease()->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("install"),
                                      QStringLiteral("Installation session failed: %1").arg(message)});
            persistCurrent();
            populateHistory();
        }
        statusBar()->showMessage(QStringLiteral("Package operation failed"), 10000);
        finishCommandProgress(false, QStringLiteral("Package operation could not start: %1").arg(message));
        if (commandProgress_ == nullptr) {
            QMessageBox::critical(this, QStringLiteral("Installation could not start"), message);
        }
        populateBuild();
        updateDashboardActions();
        updateDeleteButton();
    });
    connect(&installService_, &InstallService::finished, this, [this](const ProcessResult &result) {
        projectList_->setEnabled(true);
        if (!project_) return;
        if (result.succeeded()) static_cast<void>(library_.reconcileInstalled(*project_, nullptr));
        const auto operation = pendingPackageOperation_.isEmpty()
            ? QStringLiteral("install") : pendingPackageOperation_;
        currentRelease()->history.append({result.finishedAt, operation,
                                  result.succeeded() ? QStringLiteral("Package operation succeeded")
                                                     : QStringLiteral("Package operation failed (exit %1)").arg(result.exitCode)});
        project_->history.append({result.finishedAt, operation,
                                  result.succeeded() ? QStringLiteral("Package operation succeeded")
                                                     : QStringLiteral("Package operation failed")});
        pendingPackageOperation_.clear();
        persistCurrent();
        const auto projectId = project_->id;
        refreshProjectList(projectId);
        if (!project_ || project_->id != projectId) loadProject(projectId);
        refreshCurrentProject();
        if (result.succeeded()) syncTrayUpdateCensus();
        const auto succeeded = result.succeeded();
        const auto summary = operation == QStringLiteral("uninstall")
            ? (succeeded ? QStringLiteral("Uninstall completed successfully.")
                         : QStringLiteral("Uninstall failed (exit %1).").arg(result.exitCode))
            : operation == QStringLiteral("rollback")
                ? (succeeded ? QStringLiteral("Rollback completed successfully.")
                             : QStringLiteral("Rollback failed (exit %1).").arg(result.exitCode))
                : (succeeded ? QStringLiteral("Installation completed successfully.")
                             : QStringLiteral("Installation failed (exit %1).").arg(result.exitCode));
        finishCommandProgress(succeeded, summary);
        statusBar()->showMessage(succeeded ? QStringLiteral("Installation succeeded")
                                           : QStringLiteral("Installation failed"), 10000);
        if (commandProgress_ == nullptr) {
            QMessageBox::information(this, QStringLiteral("Package operation"),
                                     succeeded ? QStringLiteral("Pacman completed successfully.")
                                               : QStringLiteral("Pacman did not complete successfully. Review the captured output in the progress dialog."));
        }
    });

    connect(debDownloadService_, &DebDownloadService::progress, this,
            [this](const qint64 received, const qint64 total) {
        preparationBytesReceived_ = received;
        preparationBytesTotal_ = total;
        if (downloadProgress_ != nullptr) {
            if (total > 0) {
                downloadProgress_->setRange(0, 1000);
                downloadProgress_->setValue(static_cast<int>(std::clamp<qint64>(
                    received * 1000 / total, 0, 1000)));
                downloadProgress_->setLabelText(
                    QStringLiteral("Downloading vendor artifact… %1 / %2 MiB\nYou may hide this window; the download will continue.")
                        .arg(received / (1024 * 1024)).arg(total / (1024 * 1024)));
            } else {
                downloadProgress_->setRange(0, 0);
                downloadProgress_->setLabelText(
                    QStringLiteral("Downloading vendor artifact… %1 MiB\nYou may hide this window; the download will continue.")
                        .arg(received / (1024 * 1024)));
            }
        }
        statusBar()->showMessage(total > 0
            ? QStringLiteral("Downloading vendor artifact: %1 / %2 MiB")
                  .arg(received / (1024 * 1024)).arg(total / (1024 * 1024))
            : QStringLiteral("Downloading vendor artifact: %1 MiB").arg(received / (1024 * 1024)));
        if (!lastPreparationPublish_.isValid() || lastPreparationPublish_.elapsed() >= 150) {
            publishPreparationActivity();
            lastPreparationPublish_.restart();
            updatePreparationIndicators();
            updateUpdateCheckIndicators();
        }
    });
    connect(debDownloadService_, &DebDownloadService::failed, this, [this](const QString &message) {
        const auto projectId = preparingProjectId_;
        resetPreparationState();
        statusBar()->showMessage(QStringLiteral("Vendor artifact download failed"), 8000);
        QMessageBox::critical(this, QStringLiteral("Could not prepare release"), message);
        if (!projectId.isEmpty()) refreshProjectList(projectId);
        populateOverview();
    });
    connect(debDownloadService_, &DebDownloadService::finished, this, [this](const QString &path) {
        preparationPhase_ = QStringLiteral("Inspecting");
        if (downloadProgress_ != nullptr) {
            downloadProgress_->close();
            downloadProgress_->deleteLater();
            downloadProgress_ = nullptr;
        }
        updatePreparationIndicators();
        publishPreparationActivity();
        statusBar()->showMessage(QStringLiteral("Vendor artifact downloaded; importing release"));
        pendingDownloadedImport_ = path;
        importPackage(path);
    });
    connect(&signingKeyDownloadService_, &RepositoryKeyDownloadService::progress, this,
            [this](const qint64 received, const qint64 total) {
        if (signingKeyProgress_ == nullptr) return;
        if (total > 0) {
            signingKeyProgress_->setRange(0, 1000);
            signingKeyProgress_->setValue(static_cast<int>(std::clamp<qint64>(
                received * 1000 / total, 0, 1000)));
        } else {
            signingKeyProgress_->setRange(0, 0);
        }
        signingKeyProgress_->setLabelText(
            QStringLiteral("Downloading repository signing key… %1 KiB")
                .arg(received / 1024));
    });
    connect(&signingKeyDownloadService_, &RepositoryKeyDownloadService::failed, this,
            [this](const QString &message) {
        if (signingKeyProgress_ != nullptr) {
            signingKeyProgress_->close();
            signingKeyProgress_->deleteLater();
            signingKeyProgress_ = nullptr;
        }
        projectList_->setEnabled(true);
        if (aptSigningKeyDownloadButton_ != nullptr) {
            aptSigningKeyDownloadButton_->setText(QStringLiteral("Fetch && Review…"));
            aptSigningKeyDownloadButton_->setEnabled(updateStrategy_->currentIndex() == 2 ||
                                                      updateStrategy_->currentIndex() == 3);
        }
        aptSigningKeyUrl_->setEnabled(updateStrategy_->currentIndex() == 2 ||
                                      updateStrategy_->currentIndex() == 3);
        signingKeyDownloadProjectId_.clear();
        signingKeyDownloadReleaseId_.clear();
        if (message == QStringLiteral("Signing-key download canceled")) {
            statusBar()->showMessage(message, 5000);
        } else {
            QMessageBox::critical(this, QStringLiteral("Could not download signing key"), message);
        }
    });
    connect(&signingKeyDownloadService_, &RepositoryKeyDownloadService::finished, this,
            [this](const QByteArray &contents, const QUrl &requestedUrl, const QUrl &resolvedUrl) {
        if (signingKeyProgress_ != nullptr) {
            signingKeyProgress_->close();
            signingKeyProgress_->deleteLater();
            signingKeyProgress_ = nullptr;
        }
        projectList_->setEnabled(true);
        aptSigningKeyDownloadButton_->setText(QStringLiteral("Fetch && Review…"));
        aptSigningKeyDownloadButton_->setEnabled(updateStrategy_->currentIndex() == 2 ||
                                                  updateStrategy_->currentIndex() == 3);
        aptSigningKeyUrl_->setEnabled(updateStrategy_->currentIndex() == 2 ||
                                      updateStrategy_->currentIndex() == 3);

        const auto targetProject = signingKeyDownloadProjectId_;
        const auto targetRelease = signingKeyDownloadReleaseId_;
        signingKeyDownloadProjectId_.clear();
        signingKeyDownloadReleaseId_.clear();
        if (!project_ || project_->id != targetProject || project_->release(targetRelease) == nullptr) {
            QMessageBox::warning(
                this, QStringLiteral("Signing key not imported"),
                QStringLiteral("The release that requested this signing key is no longer open. Fetch it again from that release's Updates page."));
            return;
        }

        QString inspectionError;
        const auto inspection = RepositoryTrust::inspectKey(contents, &inspectionError);
        if (!inspection) {
            QMessageBox::critical(this, QStringLiteral("Downloaded file is not an OpenPGP key"),
                                  inspectionError);
            return;
        }

        const auto resolvedNotice = requestedUrl == resolvedUrl
            ? QString{}
            : QStringLiteral("\nThe server redirected the download to %1.")
                  .arg(resolvedUrl.toDisplayString());
        QMessageBox confirmation(QMessageBox::Question,
                                 QStringLiteral("Trust downloaded signing key?"),
                                 QStringLiteral("PacSmith found OpenPGP fingerprint:\n%1")
                                     .arg(inspection->fingerprints.join(QStringLiteral("\n"))),
                                 QMessageBox::Yes | QMessageBox::Cancel, this);
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.setInformativeText(
            QStringLiteral("Confirm this fingerprint against the vendor's published documentation when possible. The HTTPS URL identifies where the bytes came from; the pinned fingerprint is what protects future APT checks.%1")
                .arg(resolvedNotice));
        confirmation.setDetailedText(
            QStringLiteral("Requested URL: %1\nResolved URL: %2\nSHA256: %3\nFingerprint(s):\n%4")
                .arg(requestedUrl.toString(), resolvedUrl.toString(), inspection->sha256,
                     inspection->fingerprints.join(QStringLiteral("\n"))));
        auto *trustButton = confirmation.button(QMessageBox::Yes);
        if (trustButton != nullptr) trustButton->setText(QStringLiteral("Trust and Pin Key"));
        if (confirmation.exec() != QMessageBox::Yes) {
            statusBar()->showMessage(QStringLiteral("Signing key downloaded but not trusted"), 6000);
            return;
        }

        auto *tracker = project_->release(targetRelease);
        QString importError;
        const auto key = RepositoryTrust::importUserKey(
            library_.releasePath(*tracker), contents, requestedUrl.toString(), &importError);
        if (!key) {
            QMessageBox::critical(this, QStringLiteral("Could not import signing key"), importError);
            return;
        }
        const auto duplicate = std::find_if(
            tracker->update.signingKeys.cbegin(), tracker->update.signingKeys.cend(),
            [&](const auto &candidate) { return candidate.sha256 == key->sha256; });
        if (duplicate == tracker->update.signingKeys.cend()) tracker->update.signingKeys.append(*key);
        tracker->update.aptSigningKeyring = key->relativePath;
        tracker->update.trustedSigningFingerprint = key->fingerprints.first();
        tracker->fieldProvenance.insert(QStringLiteral("update.aptSigningKeyring"), key->provenance);
        tracker->fieldProvenance.insert(QStringLiteral("update.trustedSigningFingerprint"), key->provenance);
        tracker->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("update-key"),
                                 QStringLiteral("Trusted repository key %1 downloaded from %2")
                                     .arg(key->fingerprints.first(), requestedUrl.toString())});
        if (persistCurrent()) {
            populateUpdates();
            statusBar()->showMessage(QStringLiteral("Repository signing key trusted and pinned"), 7000);
        }
    });
    connect(aptUpdateService_, &AptUpdateService::progressChanged, this, [this](const QString &message) {
        updateCheckStatus_->setText(message);
    });
    connect(aptUpdateService_, &AptUpdateService::finished, this, [this](const UpdateCheckResult &result) {
        applyUpdateCheckResult(result, QStringLiteral("APT"));
    });
    connect(rpmUpdateService_, &RpmUpdateService::progressChanged, this, [this](const QString &message) {
        updateCheckStatus_->setText(message);
    });
    connect(rpmUpdateService_, &RpmUpdateService::finished, this, [this](const UpdateCheckResult &result) {
        applyUpdateCheckResult(result, QStringLiteral("RPM"));
    });
    connect(githubUpdateService_, &GitHubUpdateService::progressChanged, this,
            [this](const QString &message) { updateCheckStatus_->setText(message); });
    connect(githubUpdateService_, &GitHubUpdateService::finished, this,
            [this](const UpdateCheckResult &result) {
        applyUpdateCheckResult(result, QStringLiteral("GitHub"));
    });

    loadRepositoryPackageCatalog();
    statusBar()->setSizeGripEnabled(false);
    auto *connectionSlot = new QWidget(this);
    auto *connectionSlotLayout = new QHBoxLayout(connectionSlot);
    constexpr int kConnectionPad = 8;
    connectionSlotLayout->setContentsMargins(0, kConnectionPad, kConnectionPad, kConnectionPad);
    connectionSlotLayout->setSpacing(0);
    connectionButton_ = new QPushButton(connectionSlot);
    connectionButton_->setCursor(Qt::PointingHandCursor);
    connectionButton_->setFocusPolicy(Qt::TabFocus);
    connectionButton_->setAutoDefault(false);
    connectionButton_->setDefault(false);
    connectionButton_->setIconSize(QSize(16, 16));
    connectionButton_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    connectionButton_->setToolTip(
        QStringLiteral("Library connection. Click to switch between this computer and a remote host."));
    connectionSlotLayout->addWidget(connectionButton_);
    statusBar()->addPermanentWidget(connectionSlot);
    connect(connectionButton_, &QPushButton::clicked, this, &MainWindow::showConnectionDialog);
    connectionStatusTimer_ = new QTimer(this);
    connectionStatusTimer_->setInterval(5000);
    connect(connectionStatusTimer_, &QTimer::timeout, this, &MainWindow::refreshConnectionStatus);
    connectionStatusTimer_->start();
    QString runtimeError;
    if (!applyLibraryRuntime(library_.config(), &runtimeError) && !runtimeError.isEmpty()) {
        statusBar()->showMessage(runtimeError, 10000);
    }
    QString libraryError;
    if (const auto library = library_.librarySettings(&libraryError)) {
        applyLibrarySettings(*library);
    } else if (!libraryError.isEmpty() && runtimeError.isEmpty()) {
        statusBar()->showMessage(libraryError, 8000);
    }
    refreshConnectionStatus();
    refreshProjectList();
    QTimer::singleShot(0, this, [this] { syncActivityTimer(); });
}

void MainWindow::showConnectionDialog() {
    if (runConnectionDialog(this)) return;
    refreshConnectionStatus();
}

namespace {

QIcon connectionStatusIcon(QWidget *widget, bool reachable) {
    auto icon = QIcon::fromTheme(reachable ? QStringLiteral("network-wired")
                                           : QStringLiteral("network-offline"));
    if (icon.isNull()) icon = QIcon::fromTheme(QStringLiteral("network-workgroup"));
    if (icon.isNull()) icon = QIcon::fromTheme(QStringLiteral("network-server"));
    if (icon.isNull()) icon = widget->style()->standardIcon(QStyle::SP_DriveNetIcon);
    return icon;
}

QString remoteConnectionCaption(const ConnectionConfig &config, const QFontMetrics &metrics) {
    const auto host = config.remoteUrl.host();
    const auto port = config.remoteUrl.port(8443);
    if (host.isEmpty()) return QStringLiteral("Remote");
    const auto suffix = QStringLiteral(":%1").arg(port);
    const auto full = host + suffix;
    constexpr int kMaxWidth = 168;
    if (metrics.horizontalAdvance(full) <= kMaxWidth) return full;
    const auto hostWidth = kMaxWidth - metrics.horizontalAdvance(suffix);
    if (hostWidth < metrics.averageCharWidth() * 6) {
        return metrics.elidedText(full, Qt::ElideMiddle, kMaxWidth);
    }
    return metrics.elidedText(host, Qt::ElideRight, hostWidth) + suffix;
}

} // namespace

void MainWindow::refreshConnectionStatus() {
    if (connectionButton_ == nullptr) return;
    const auto &config = library_.config();
    const bool remote = config.mode == ConnectionConfig::Mode::Remote;
    QString error;
    const bool ok = library_.reachable(&error);
    const auto target = config.summary();
    connectionButton_->setIcon(connectionStatusIcon(this, ok));
    connectionButton_->setText(remote ? remoteConnectionCaption(config, connectionButton_->fontMetrics())
                                      : QStringLiteral("Local"));
    if (ok) {
        connectionButton_->setToolTip(
            QStringLiteral("Connected to %1. Click to change library connection.").arg(target));
    } else {
        connectionButton_->setToolTip(
            error.isEmpty()
                ? QStringLiteral("Not connected to %1. Click to change library connection.").arg(target)
                : error);
    }
}

QWidget *MainWindow::createStageHost(QListWidget **nav, QStackedWidget **stack, QWidget *navHeader) {
    auto *host = new QWidget(this);
    auto *layout = new QHBoxLayout(host);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(8);
    auto *list = new QListWidget(host);
    list->setObjectName(QStringLiteral("workbenchStageNav"));
    list->setFixedWidth(188);
    list->setSpacing(1);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setStyleSheet(QStringLiteral(
        "QListWidget#workbenchStageNav { border: none; background: transparent; outline: none; }"
        "QListWidget#workbenchStageNav::item { padding: 8px 10px; }"
        "QListWidget#workbenchStageNav::item:selected { background: rgba(52, 152, 219, 40); }"));
    auto *pages = new QStackedWidget(host);
    if (navHeader != nullptr) {
        auto *navColumn = new QWidget(host);
        navColumn->setFixedWidth(188);
        auto *navLayout = new QVBoxLayout(navColumn);
        navLayout->setContentsMargins(0, 0, 0, 0);
        navLayout->setSpacing(6);
        navHeader->setParent(navColumn);
        navLayout->addWidget(navHeader);
        list->setFixedWidth(188);
        navLayout->addWidget(list, 1);
        layout->addWidget(navColumn);
    } else {
        layout->addWidget(list);
    }
    layout->addWidget(pages, 1);
    connect(list, &QListWidget::currentRowChanged, this, [this, pages](const int row) {
        if (row >= 0) pages->setCurrentIndex(row);
        if (rightStack_ != nullptr && rightStack_->currentIndex() == 1) {
            populateCurrentWorkbenchPage();
        }
    });
    *nav = list;
    *stack = pages;
    return host;
}

void MainWindow::addWorkbenchPage(const int stage, QListWidget *nav, QStackedWidget *stack,
                                  const EditorSection section, const QString &label, QWidget *page) {
    auto *item = new QListWidgetItem(label, nav);
    item->setData(Qt::UserRole, static_cast<int>(section));
    item->setData(sectionBaseLabelRole, label);
    stack->addWidget(page);
    SectionLocation location;
    location.stage = stage;
    location.page = nav->count() - 1;
    location.nav = nav;
    location.stack = stack;
    location.item = item;
    sectionLocations_.insert(static_cast<int>(section), location);
}

void MainWindow::setSectionVisible(const EditorSection section, const bool visible) {
    const auto location = sectionLocations_.value(static_cast<int>(section));
    if (location.item != nullptr) location.item->setHidden(!visible);
}

void MainWindow::updateSectionReviewMarkers() {
    if (currentRelease() == nullptr) return;
    const auto &release = *currentRelease();
    const auto mark = [this](const EditorSection section, const bool needsReview) {
        const auto location = sectionLocations_.value(static_cast<int>(section));
        if (location.item == nullptr) return;
        const auto base = location.item->data(sectionBaseLabelRole).toString();
        if (base.isEmpty()) return;
        location.item->setText(needsReview ? base + QStringLiteral("  ⚠") : base);
        if (needsReview) {
            location.item->setForeground(QColor(0xe5, 0xb9, 0x3d));
        } else {
            location.item->setData(Qt::ForegroundRole, QVariant());
        }
    };
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
    const bool dependencyReview = std::any_of(
        release.dependencies.cbegin(), release.dependencies.cend(),
        [this](const auto &dependency) {
            return dependency.status == MappingStatus::Unresolved ||
                   repositoryPackageUnavailable(dependency, repositoryDependencyAvailability_);
        });
    const bool lifecycleReview = !release.lifecycleScript.contents.isEmpty() &&
        (!release.lifecycleScript.validationPassed ||
         release.lifecycleScript.requiresAcknowledgement());
    mark(EditorSection::SourceContents, pendingPayloadReviews(release) > 0);
    mark(EditorSection::SourceScripts, false);
    mark(EditorSection::ConfigDependencies, dependencyReview);
    mark(EditorSection::ConfigScripts, pendingScriptFindings(release) > 0 || lifecycleReview);
    mark(EditorSection::ConfigCommands, commandReview);
    mark(EditorSection::ConfigAppRun, release.installMapping.appRun.requiresReview());
    mark(EditorSection::ConfigDesktopEntries, desktopReview);
    mark(EditorSection::ConfigIcon, !release.installMapping.icon.isConfigured());
}

void MainWindow::updateWorkbenchStageChrome() {
    if (stageTabs_ == nullptr) return;
    const auto stage = stageTabs_->currentIndex();
    if (currentRelease() != nullptr) {
        const auto sourceTitle = sourcePackageTypeTitle(currentRelease()->sourceType);
        if (sourceTypeBadge_ != nullptr) {
            sourceTypeBadge_->setText(sourceTitle);
            sourceTypeBadge_->setVisible(true);
        }
        stageTabs_->setTabText(0, sourceTitle);
        stageTabs_->setTabText(2, QStringLiteral("Pacman Package"));
        stageTabs_->setTabToolTip(0, QStringLiteral("Imported %1: inspect metadata, original scripts, and payload")
                                       .arg(sourceTitle.toLower()));
        stageTabs_->setTabToolTip(1, currentRelease()->pkgbuildManuallyModified
            ? QStringLiteral("Custom PKGBUILD for this release. Guided configuration is ignored.")
            : QStringLiteral("Guided settings generate the PKGBUILD for this release."));
    }
    updateConfigurationModeChrome();
    if (workbenchSubtitle_ == nullptr || currentRelease() == nullptr) return;
    const auto version = currentRelease()->debian.version;
    const auto sourceTitle = sourcePackageTypeTitle(currentRelease()->sourceType);
    if (stage == 0) {
        workbenchSubtitle_->setText(
            QStringLiteral("%1 for release %2 — inspect only. Nothing here is executed or installed.")
                .arg(sourceTitle, version));
    } else if (stage == 1) {
        workbenchSubtitle_->setText(
            currentRelease()->pkgbuildManuallyModified
                ? QStringLiteral("Custom configuration for release %1. Edit the PKGBUILD directly; Guided pages are hidden.")
                      .arg(version)
                : QStringLiteral("Guided configuration for release %1. These settings generate the PKGBUILD on Pacman Package.")
                      .arg(version));
    } else {
        workbenchSubtitle_->setText(
            currentRelease()->pkgbuildManuallyModified
                ? QStringLiteral("Pacman package for release %1. The Custom PKGBUILD is what makepkg will run.")
                      .arg(version)
                : QStringLiteral("Pacman package for release %1: install layout, generated PKGBUILD, and build.")
                      .arg(version));
    }
}

void MainWindow::updateConfigurationModeChrome() {
    if (guidedModeButton_ == nullptr || customModeButton_ == nullptr) return;
    const bool custom = currentRelease() != nullptr && currentRelease()->pkgbuildManuallyModified;
    QSignalBlocker guidedBlock(guidedModeButton_);
    QSignalBlocker customBlock(customModeButton_);
    guidedModeButton_->setChecked(!custom);
    customModeButton_->setChecked(custom);
}

void MainWindow::setConfigurationMode(const bool custom) {
    if (switchingConfigurationMode_) return;
    switchingConfigurationMode_ = true;
    const struct SwitchingGuard {
        bool *flag;
        ~SwitchingGuard() { *flag = false; }
    } switchingGuard{&switchingConfigurationMode_};
    if (!project_ || currentRelease() == nullptr) {
        updateConfigurationModeChrome();
        return;
    }
    if (currentRelease()->pkgbuildManuallyModified == custom) {
        updateConfigurationModeChrome();
        return;
    }
    if (pkgbuildEditor_ != nullptr && pkgbuildEditor_->document()->isModified()) {
        QString error;
        if (!library_.saveCustomPkgbuild(*project_, *currentRelease(), pkgbuildEditor_->toPlainText(), &error)) {
            QMessageBox::critical(this, QStringLiteral("Could not save Custom PKGBUILD"), error);
            updateConfigurationModeChrome();
            return;
        }
        pkgbuildEditor_->document()->setModified(false);
        projectCache_.insert(project_->id, *project_);
        if (currentRelease() == nullptr) {
            updateConfigurationModeChrome();
            return;
        }
    }
    if (currentRelease()->pkgbuildManuallyModified != custom) {
        QString error;
        const bool ok = custom
            ? library_.activateCustomPkgbuild(*project_, *currentRelease(), &error)
            : library_.activateGuidedPkgbuild(*project_, *currentRelease(), &error);
        if (!ok) {
            QMessageBox::critical(this, QStringLiteral("Could not switch PKGBUILD mode"), error);
            updateConfigurationModeChrome();
            return;
        }
        projectCache_.insert(project_->id, *project_);
    }
    configureEditorProfile();
    updateConfigurationModeChrome();
    if (custom) {
        selectSection(EditorSection::ConfigPkgbuild);
    } else if (configNav_ != nullptr) {
        for (int row = 0; row < configNav_->count(); ++row) {
            const auto *item = configNav_->item(row);
            if (item != nullptr && !item->isHidden() &&
                static_cast<EditorSection>(item->data(Qt::UserRole).toInt()) != EditorSection::ConfigPkgbuild) {
                selectSection(static_cast<EditorSection>(item->data(Qt::UserRole).toInt()));
                break;
            }
        }
    }
    populatePkgbuild();
}

MainWindow::EditorSection MainWindow::currentSection() const {
    if (stageTabs_ == nullptr) return EditorSection::SourceOverview;
    const auto stage = stageTabs_->currentIndex();
    QListWidget *nav = stage == 1 ? configNav_ : stage == 2 ? resultNav_ : sourceNav_;
    if (nav == nullptr || nav->currentItem() == nullptr) return EditorSection::SourceOverview;
    return static_cast<EditorSection>(nav->currentItem()->data(Qt::UserRole).toInt());
}

QString MainWindow::sectionTitle(const EditorSection section) const {
    const auto location = sectionLocations_.value(static_cast<int>(section));
    if (location.item != nullptr) return location.item->text();
    return QStringLiteral("Package setup");
}

bool MainWindow::isSectionActive(const EditorSection section) const {
    if (stageTabs_ == nullptr || rightStack_ == nullptr || rightStack_->currentIndex() != 1) {
        return false;
    }
    const auto location = sectionLocations_.value(static_cast<int>(section));
    return location.nav != nullptr && stageTabs_->currentIndex() == location.stage &&
           location.nav->currentRow() == location.page &&
           (location.item == nullptr || !location.item->isHidden());
}

void MainWindow::placeUpdatesEditor() {
    if (updatesEditor_ == nullptr) return;
    QWidget *host = (rightStack_ != nullptr && rightStack_->currentIndex() == 1)
                        ? configUpdatesHost_
                        : dashboardUpdatesHost_;
    if (host == nullptr || updatesEditor_->parentWidget() == host) return;
    if (auto *layout = host->layout()) layout->addWidget(updatesEditor_);
}

void MainWindow::placeRepositoryEditor() {
    if (repositoryEditor_ == nullptr) return;
    QWidget *host = (rightStack_ != nullptr && rightStack_->currentIndex() == 1)
                        ? configRepositoryHost_
                        : dashboardRepositoryHost_;
    if (host == nullptr || repositoryEditor_->parentWidget() == host) return;
    if (auto *layout = host->layout()) layout->addWidget(repositoryEditor_);
}

void MainWindow::syncUpdateCheckButtons() {
    const bool busy = aptUpdateService_->isRunning() || rpmUpdateService_->isRunning() ||
                      githubUpdateService_->isRunning() || debDownloadService_->isRunning() ||
                      importThread_ != nullptr;
    bool allowed = false;
    if (!busy && project_ && updateEditorRelease() != nullptr && updateStrategy_ != nullptr) {
        const auto index = updateStrategy_->currentIndex();
        allowed = index == 2 || index == 3 || index == 4;
    }
    if (updateCheckButton_ != nullptr) updateCheckButton_->setEnabled(allowed);
    if (historyCheckUpdatesButton_ != nullptr) historyCheckUpdatesButton_->setEnabled(allowed);
}

int MainWindow::sectionIndex(const EditorSection section) const {
    return sectionLocations_.value(static_cast<int>(section)).page;
}

void MainWindow::selectSection(const EditorSection section) {
    auto location = sectionLocations_.value(static_cast<int>(section));
    if (location.item != nullptr && location.item->isHidden()) {
        location = sectionLocations_.value(static_cast<int>(EditorSection::SourceOverview));
    }
    if (location.nav == nullptr || location.stack == nullptr || location.item == nullptr ||
        location.item->isHidden() || stageTabs_ == nullptr) {
        return;
    }
    QSignalBlocker stageBlocker(stageTabs_);
    QSignalBlocker navBlocker(location.nav);
    stageTabs_->setCurrentIndex(location.stage);
    location.nav->setCurrentRow(location.page);
    location.stack->setCurrentIndex(location.page);
    updateWorkbenchStageChrome();
    if (rightStack_ != nullptr && rightStack_->currentIndex() == 1) {
        populateCurrentWorkbenchPage();
    }
}

void MainWindow::configureEditorProfile() {
    if (stageTabs_ == nullptr || currentRelease() == nullptr) return;
    const auto type = currentRelease()->sourceType;
    if (sourceTypeBadge_ != nullptr) {
        sourceTypeBadge_->setText(sourcePackageTypeTitle(type));
        sourceTypeBadge_->setVisible(true);
    }
    const bool standalone = type == SourcePackageType::ElfBinary;
    const bool packageContainer = type == SourcePackageType::Debian ||
                                  type == SourcePackageType::Rpm ||
                                  type == SourcePackageType::ArchPackage;
    const bool custom = currentRelease()->pkgbuildManuallyModified;
    const bool hasScripts = packageContainer ||
                            !currentRelease()->maintainerScripts.isEmpty() ||
                            !currentRelease()->scriptFindings.isEmpty();
    const bool hasLifecycle = hasScripts || !currentRelease()->lifecycleScript.contents.isEmpty();
    setSectionVisible(EditorSection::SourceOverview, true);
    setSectionVisible(EditorSection::SourceMetadata, true);
    setSectionVisible(EditorSection::SourceScripts, hasScripts);
    setSectionVisible(EditorSection::SourceContents, !standalone);
    setSectionVisible(EditorSection::ConfigPkgbuild, custom);
    setSectionVisible(EditorSection::ConfigLayout,
                      !custom && (type == SourcePackageType::Archive || standalone));
    setSectionVisible(EditorSection::ConfigDependencies,
                      !custom && (packageContainer || !currentRelease()->dependencies.isEmpty()));
    setSectionVisible(EditorSection::ConfigScripts, !custom && (hasScripts || hasLifecycle));
    setSectionVisible(EditorSection::ConfigCommands,
                      !custom && (!currentRelease()->installMapping.launchers.isEmpty() ||
                          type == SourcePackageType::Archive || standalone ||
                          type == SourcePackageType::Debian ||
                          type == SourcePackageType::Rpm));
    setSectionVisible(EditorSection::ConfigAppRun,
                      !custom && type == SourcePackageType::AppImage);
    setSectionVisible(EditorSection::ConfigDesktopEntries, !custom);
    setSectionVisible(EditorSection::ConfigIcon, !custom);
    setSectionVisible(EditorSection::ConfigUpdates, true);
    setSectionVisible(EditorSection::ConfigRepository, true);
    setSectionVisible(EditorSection::ResultInstallPlan, true);
    setSectionVisible(EditorSection::ResultPkgbuild, true);
    setSectionVisible(EditorSection::ResultBuild, true);
    updateSectionReviewMarkers();

    const auto showStageIfNeeded = [this](QListWidget *nav, QStackedWidget *stack, const int stage) {
        if (nav == nullptr || stageTabs_ == nullptr) return;
        bool any = false;
        for (int row = 0; row < nav->count(); ++row) {
            const auto *item = nav->item(row);
            if (item != nullptr && !item->isHidden()) any = true;
        }
        stageTabs_->setTabVisible(stage, any);
        if (any && (nav->currentItem() == nullptr || nav->currentItem()->isHidden())) {
            for (int row = 0; row < nav->count(); ++row) {
                if (nav->item(row) != nullptr && !nav->item(row)->isHidden()) {
                    QSignalBlocker blocker(nav);
                    nav->setCurrentRow(row);
                    break;
                }
            }
        }
        if (stack != nullptr && nav->currentRow() >= 0) {
            stack->setCurrentIndex(nav->currentRow());
        }
    };
    showStageIfNeeded(sourceNav_, sourceStack_, 0);
    showStageIfNeeded(configNav_, configStack_, 1);
    showStageIfNeeded(resultNav_, resultStack_, 2);
    updateWorkbenchStageChrome();
}

void MainWindow::activateExistingSession(const QString &importPath) {
    if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
    if (auto *handle = windowHandle()) handle->requestActivate();
    if (!importPath.isEmpty()) importPackage(importPath);
    syncActivityTimer();
    updateUpdateCheckIndicators();
}

void MainWindow::setKeepRunningInTray(const bool enabled) {
    keepRunningInTray_ = enabled;
}

void MainWindow::applyLibrarySettings(const LibrarySettings &settings) {
    librarySettingsRevision_ = settings.revision;
    settings.applyTo(aiSettings_);
}

QString MainWindow::sessionCredential(const QString &name) const {
    return sessionCredentials_.value(name);
}

void MainWindow::rememberSessionCredential(const QString &name, const QString &value) {
    if (value.isEmpty()) sessionCredentials_.remove(name);
    else sessionCredentials_.insert(name, value);
}

MainWindow::~MainWindow() {
    ++projectLoadGeneration_;
    loadingProjectId_.clear();
    if (networkIoThread_.isRunning()) {
        shutdownNetworkService(debDownloadService_);
        shutdownNetworkService(aptUpdateService_);
        shutdownNetworkService(rpmUpdateService_);
        shutdownNetworkService(githubUpdateService_);
        networkIoThread_.quit();
        networkIoThread_.wait();
        return;
    }
    delete debDownloadService_;
    delete aptUpdateService_;
    delete rpmUpdateService_;
    delete githubUpdateService_;
    debDownloadService_ = nullptr;
    aptUpdateService_ = nullptr;
    rpmUpdateService_ = nullptr;
    githubUpdateService_ = nullptr;
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (importThread_ != nullptr && importThread_->isRunning()) {
        QMessageBox::information(this, QStringLiteral("Import in progress"),
                                 QStringLiteral("Wait for the current package import to finish before closing PacSmith."));
        event->ignore();
        return;
    }
    if (installService_.isRunning()) {
        QMessageBox::information(this, QStringLiteral("Installation in progress"),
                                 QStringLiteral("Wait for the current pacman operation to finish before closing PacSmith."));
        event->ignore();
        return;
    }
    if (keepRunningInTray_) {
        event->ignore();
        hide();
        return;
    }
    if (aiInProgress()) cancelRemoteAi();
    if (aptUpdateService_->isRunning()) cancelOnServiceThread(*aptUpdateService_);
    if (rpmUpdateService_->isRunning()) cancelOnServiceThread(*rpmUpdateService_);
    if (githubUpdateService_->isRunning()) cancelOnServiceThread(*githubUpdateService_);
    if (debDownloadService_->isRunning()) cancelOnServiceThread(*debDownloadService_);
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) {
        for (const auto &url : event->mimeData()->urls()) {
            if (url.isLocalFile() || url.scheme() == QStringLiteral("https")) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    if (event->mimeData()->hasText()) {
        const QUrl url(event->mimeData()->text().trimmed());
        if (url.scheme() == QStringLiteral("https")) {
            event->acceptProposedAction();
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event) {
    for (const auto &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            event->acceptProposedAction();
            importPackage(url.toLocalFile());
            return;
        }
        if (url.scheme() == QStringLiteral("https")) {
            event->acceptProposedAction();
            importPackage(url.toString());
            return;
        }
    }
    if (event->mimeData()->hasText()) {
        const QUrl url(event->mimeData()->text().trimmed());
        if (url.scheme() == QStringLiteral("https")) {
            event->acceptProposedAction();
            importPackage(url.toString());
        }
    }
}


} // namespace pacsmith::gui
