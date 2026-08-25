#pragma once

#include "core/apt_update_service.hpp"
#include "core/app_settings.hpp"
#include "core/credential_store.hpp"
#include "core/deb_download_service.hpp"
#include "core/direct_url_update_service.hpp"
#include "core/github_update_service.hpp"
#include "core/process_services.hpp"
#include "core/library_client.hpp"
#include "core/library_events.hpp"
#include "core/repository_key_download_service.hpp"
#include "core/rpm_update_service.hpp"

#include <QHash>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QSet>
#include <QThread>

#include <functional>
#include <optional>

class QComboBox;
class QCheckBox;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QFormLayout;
class QFrame;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QProgressDialog;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTabWidget;
class QTreeWidget;
class QThread;
class QTimer;
class QUrl;
class QWidget;

namespace pacsmith::gui {

class CommandProgressDialog;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(AppSettingsStore &settingsStore, CredentialStore &credentials,
               QWidget *parent = nullptr);
    ~MainWindow() override;
    void importPackage(const QString &path);
    void activateExistingSession(const QString &importPath = {});
    void setKeepRunningInTray(bool enabled);
    void reloadVisibleProjects(bool refreshOpenProject = true);
    void noteBackgroundCheckStarted();

signals:
    void clientSettingsReloaded();
    void serverTopicsChanged(const QStringList &topics);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    enum class EditorSection {
        SourceOverview,
        SourceMetadata,
        SourceScripts,
        SourceContents,
        ConfigPkgbuild,
        ConfigMetadata,
        ConfigLayout,
        ConfigDependencies,
        ConfigScripts,
        ConfigCommands,
        ConfigAppRun,
        ConfigDesktopEntries,
        ConfigIcon,
        ConfigUpdates,
        ConfigRepository,
        ResultInstallPlan,
        ResultPkgbuild,
        ResultBuild
    };

    struct SectionLocation {
        int stage{-1};
        int page{-1};
        QListWidget *nav{nullptr};
        QStackedWidget *stack{nullptr};
        QListWidgetItem *item{nullptr};
    };

    QWidget *createProjectInfoPage();
    QWidget *createOverviewPage();
    QWidget *createSourceOverviewPage();
    QWidget *createSourceMetadataPage();
    QWidget *createInstallLayoutPage();
    QWidget *createPackageMetadataPage();
    QWidget *createInstallPlanPage();
    QWidget *createDependenciesPage();
    QWidget *createVendorScriptsPage();
    QWidget *createConfigScriptsPage();
    QWidget *createPayloadPage();
    QWidget *createCommandsPage();
    QWidget *createAppRunPage();
    QWidget *createDesktopEntriesPage();
    QWidget *createIconPage();
    QWidget *createPkgbuildPage();
    QWidget *createResultPkgbuildPage();
    QWidget *createUpdatesPage();
    QWidget *createRepositoryPage();
    QWidget *createBuildPage();
    QWidget *createHistoryPage();
    QWidget *createStageHost(QListWidget **nav, QStackedWidget **stack,
                             QWidget *navHeader = nullptr);
    void addWorkbenchPage(int stage, QListWidget *nav, QStackedWidget *stack,
                          EditorSection section, const QString &label, QWidget *page);
    void setSectionVisible(EditorSection section, bool visible);
    void updateSectionReviewMarkers();
    void updateWorkbenchStageChrome();
    [[nodiscard]] EditorSection currentSection() const;
    [[nodiscard]] QString sectionTitle(EditorSection section) const;
    [[nodiscard]] bool isSectionActive(EditorSection section) const;

    void chooseImport();
    void importGitHubUrl();
    void importDirectUrl();
    void importAptRepository();
    void importRpmRepository();
    void beginRepositoryImport(UpdateConfiguration configuration,
                               const QUrl &signingKeyUrl,
                               const QByteArray &signingKeyContents = {},
                               const QString &signingKeySource = {});
    void showProjectDashboard();
    void showReleaseWorkbench(const QString &releaseId);
    void showReleaseWorkbenchAtFirstAttention(const QString &releaseId);
    void deleteCurrentProject();
    void syncTrayUpdateCensus();
    void refreshLibraryView();
    void refreshProjectList(const QString &selectId = {},
                            std::function<void(bool)> completed = {});
    void setProjectListBusy(bool busy, const QString &message = {});
    void applyProjectList(QList<Project> projects, const QString &selectId,
                          const QString &error = {}, bool preserveCurrent = false);
    void loadProject(const QString &id);
    void loadProjectInteractively(const QString &id);
    void applyLoadedProject(Project project);
    void showDashboardLoading(const QString &displayName);
    void startBackgroundProjectLoad(const QString &id, quint64 generation);
    void prefetchProjectIcons();
    void prefetchSigningKeys(const Project &project);
    void refreshCurrentProject();
    void handleServerEvent(const ServerEvent &event);
    void runEventRefresh();
    void applyEventProjects(QList<Project> projects, const QString &error,
                            const QSet<QString> &topics);
    void reloadExternalProject();
    void showExternalChange(bool deleted);
    [[nodiscard]] bool hasUnsavedProjectDraft() const;
    [[nodiscard]] bool ensureCurrentProjectWritable();
    void populateCurrentWorkbenchPage();
    void updateDeleteButton();
    void populateOverview();
    void updateDashboardActions();
    void updateProjectInfoActions();
    void placeUpdatesEditor();
    void placeRepositoryEditor();
    bool saveProjectRepository();
    void promoteProjectRepository();
    void applyProjectRepository(const ProjectRepository &status);
    void handleProjectPrimaryAction();
    void handleProjectInfoAction();
    void editPackageConfiguration();
    [[nodiscard]] const PackageRelease *dashboardActionRelease() const;
    [[nodiscard]] const PackageRelease *configurationEditRelease() const;
    [[nodiscard]] std::optional<EditorSection> firstReviewSection(const PackageRelease &release) const;
    void syncUpdateCheckButtons();
    void populateSourceOverview();
    void populatePackage();
    void populateInstallPlan();
    void restoreGeneratedPkgbuild();
    void setConfigurationMode(bool custom);
    void updateConfigurationModeChrome();
    [[nodiscard]] QString currentPkgbuildText() const;
    void saveInstallMapping();
    void populateDependencies();
    void loadRepositoryPackageCatalog();
    void scheduleRepositoryPackageValidation(const QStringList &packages);
    void populateScripts();
    void updateSelectedScript();
    void acknowledgeSelectedScript();
    void beginLifecycleEdit();
    void saveLifecycleEdit();
    void cancelLifecycleEdit();
    void acknowledgeLifecycleScript();
    void discardLifecycleScript();
    void populatePayload();
    void populateCommands();
    void commandEdited(int row, int column);
    void addCommandFromPayload();
    void assignPayloadToSelectedCommand();
    void removeSelectedCommand();
    void syncInstallMappingFromLaunchers();
    void populateAppRunEditor();
    void saveAppRun();
    void keepOriginalAppRun();
    void restoreOriginalAppRun();
    [[nodiscard]] QStringList payloadFileChoices() const;
    [[nodiscard]] QString choosePayloadFile(const QString &title, const QString &selected = {});
    void populateDesktopEntries();
    void updateSelectedDesktopEntry();
    void saveSelectedDesktopEntry();
    void addDesktopEntry();
    void duplicateDesktopEntry();
    void deleteDesktopEntry();
    void populateIcon();
    void selectPayloadIcon();
    void importLocalIcon();
    void fetchRemoteIcon();
    void applyIconBytes(const QByteArray &contents, const QString &suffix,
                        IconSourceKind sourceKind, const QString &sourcePath,
                        const QString &sourceUrl = {});
    void updateSelectedPayload();
    void setSelectedPayloadDecision(bool exclude);
    void clearSelectedPayloadDecision();
    void loadSelectedPayloadPreview(const QString &path);
    void populatePkgbuild();
    void populatePackageMetadata();
    void savePackageMetadata();
    void addAdditionalDependency();
    void removeAdditionalDependency();
    void populateUpdates();
    void populateRepository();
    void populateBuild();
    void populateHistory();
    void configureEditorProfile();
    [[nodiscard]] int sectionIndex(EditorSection section) const;
    void selectSection(EditorSection section);
    bool persistCurrent();
    bool savePkgbuild();
    void refreshGeneratedPkgbuildAfterModelChange();
    void dependencyEdited(int row, int column);
    void dependencyDispositionChanged(int row, int index);
    void scriptFindingDispositionChanged(int row, int index);
    bool saveUpdateConfiguration();
    void startUpdateCheck();
    void applyUpdateCheckResult(const UpdateCheckResult &result, const QString &sourceName);
    void applyRetentionCleanup();
    void startBuild(bool installWhenSuccessful = false);
    void pollBuildJob();
    void finishBuildJob();
    void cancelRemoteBuild();
    [[nodiscard]] bool buildInProgress() const;
    [[nodiscard]] bool packageOperationInProgress() const;
    void startInstall();
    void startUninstall();
    void preparePackageInstallation(const QString &releaseId,
                                    const QString &operation,
                                    bool synchronizeLifecycle);
    void showCommandProgress(const QString &title, const QString &status, bool cancelable);
    void finishCommandProgress(bool success, const QString &summary);
    void installSelectedRelease();
    void rollbackSelectedRelease();
    void editSelectedRelease();
    void deleteSelectedRelease();
    void prepareSelectedRelease();
    void beginReleasePreparation(const QString &releaseId, bool askForConfirmation);
    void selectDashboardRelease(const QString &releaseId);
    void updatePreparationIndicators();
    void updateUpdateCheckIndicators();
    void syncActivityTimer();
    [[nodiscard]] bool updateCheckInProgress() const;
    [[nodiscard]] bool listActivityInProgress() const;
    [[nodiscard]] bool canShowUpdateCheckStatus() const;
    void publishUpdateCheckActivity(bool running, const QString &projectId = {},
                                    const QString &projectName = {});
    void publishPreparationActivity();
    void clearPublishedPreparationActivity(const QString &projectId);
    void startListDownloadActivity(const QString &projectId, const QString &releaseId = {});
    [[nodiscard]] QString displayNameForProject(const QString &projectId) const;
    void resetPreparationState();
    void beginGitHubImport(const QUrl &url);
    void continueGitHubImport(const QString &owner, const QString &repository,
                              const QString &assetRegex, bool includePrereleases,
                              const QString &requestedTag = {});
    void downloadGitHubAsset(const PackageRelease &probe,
                             const UpdateCheckResult &result);
    [[nodiscard]] QString selectedDashboardReleaseId() const;
    void showSettings();
    void showConnectionDialog();
    void refreshConnectionStatus();
    void applyLibrarySettings(const LibrarySettings &settings);
    void reloadClientSettings();
    [[nodiscard]] QString sessionCredential(const QString &name) const;
    void rememberSessionCredential(const QString &name, const QString &value);
    void startReanalysis();
    void askExternalHarness();
    bool unlockAgeCredentials();
    void importSigningKey();
    void downloadSigningKey();
    [[nodiscard]] PackageRelease *currentRelease();
    [[nodiscard]] const PackageRelease *currentRelease() const;
    [[nodiscard]] PackageRelease *activeTrackingRelease();
    [[nodiscard]] const PackageRelease *activeTrackingRelease() const;
    [[nodiscard]] PackageRelease *updateEditorRelease();

    LibraryClient library_;
    AppSettingsStore &settingsStore_;
    AppSettings appSettings_;
    qint64 librarySettingsRevision_{1};
    QFileSystemWatcher *clientSettingsWatcher_{nullptr};
    QTimer *clientSettingsReloadTimer_{nullptr};
    LibraryEventStream *libraryEventStream_{nullptr};
    QTimer *eventRefreshTimer_{nullptr};
    QSet<QString> pendingEventTopics_;
    bool eventRefreshInFlight_{false};
    bool eventRefreshAgain_{false};
    bool applyingServerRefresh_{false};
    bool projectStale_{false};
    bool pendingExternalDeletion_{false};
    std::optional<Project> pendingExternalProject_;
    QHash<QString, QString> sessionCredentials_;
    CredentialStore &credentialStore_;
    std::optional<Project> project_;
    QHash<QString, Project> projectCache_;
    QSet<QString> hydratedProjectIds_;
    quint64 projectLoadGeneration_{0};
    quint64 projectListRefreshGeneration_{0};
    QString loadingProjectId_;
    bool projectListRefreshInFlight_{false};
    bool projectDeleteInFlight_{false};
    QString currentReleaseId_;
    QString updateCheckReleaseId_;
    bool updateCheckFromWorkbench_{false};
    BuildService buildService_;
    InstallService installService_;
    bool installPreparationInFlight_{false};
    bool packageOperationFinishInFlight_{false};
    QString buildJobId_;
    qint64 buildLogAfter_{0};
    QTimer *buildPollTimer_{nullptr};
    bool buildPollInFlight_{false};
    bool buildFinishInFlight_{false};
    QThread networkIoThread_;
    DebDownloadService *debDownloadService_{nullptr};
    DirectUrlUpdateService *directUrlUpdateService_{nullptr};
    RepositoryKeyDownloadService signingKeyDownloadService_;
    AptUpdateService *aptUpdateService_{nullptr};
    RpmUpdateService *rpmUpdateService_{nullptr};
    GitHubUpdateService *githubUpdateService_{nullptr};
    bool keepRunningInTray_{false};
    bool populating_{false};
    bool switchingConfigurationMode_{false};
    QThread *importThread_{nullptr};
    QProgressDialog *importProgress_{nullptr};
    QProgressDialog *downloadProgress_{nullptr};
    QProgressDialog *signingKeyProgress_{nullptr};
    QString signingKeyDownloadProjectId_;
    QString signingKeyDownloadReleaseId_;
    QTimer *preparationSpinnerTimer_{nullptr};
    QString preparingProjectId_;
    QString preparingReleaseId_;
    QString preparationPhase_;
    qint64 preparationBytesReceived_{0};
    qint64 preparationBytesTotal_{-1};
    int preparationSpinnerFrame_{0};
    bool updateCheckStatusActive_{false};
    QElapsedTimer lastPreparationPublish_;
    CommandProgressDialog *commandProgress_{nullptr};
    QStringList repositoryPackageNames_;
    QSet<QString> repositoryPackages_;
    QHash<QString, bool> repositoryDependencyAvailability_;
    QSet<QString> repositoryDependencyChecksPending_;
    bool repositoryCatalogLoaded_{false};
    bool repositoryImportRunning_{false};
    quint64 repositoryLoadGeneration_{0};
    bool repositoryOperationInFlight_{false};

    QWidget *projectSidebar_{nullptr};
    QFrame *externalChangeBanner_{nullptr};
    QLabel *externalChangeLabel_{nullptr};
    QPushButton *externalReloadButton_{nullptr};
    QListWidget *projectList_{nullptr};
    QLabel *projectListBusyLabel_{nullptr};
    QProgressBar *projectListProgress_{nullptr};
    QPushButton *refreshProjectListButton_{nullptr};
    QPushButton *deleteProjectButton_{nullptr};
    QPushButton *connectionButton_{nullptr};
    QTimer *connectionStatusTimer_{nullptr};
    bool connectionStatusInFlight_{false};
    QLabel *projectTitle_{nullptr};
    QLabel *projectSubtitle_{nullptr};
    QStackedWidget *rightStack_{nullptr};
    QStackedWidget *dashboardBodyStack_{nullptr};
    QWidget *dashboardLoadingPage_{nullptr};
    QTabWidget *projectTabs_{nullptr};
    QWidget *dashboardUpdatesHost_{nullptr};
    QWidget *configUpdatesHost_{nullptr};
    QWidget *updatesEditor_{nullptr};
    QWidget *dashboardRepositoryHost_{nullptr};
    QWidget *configRepositoryHost_{nullptr};
    QWidget *repositoryEditor_{nullptr};
    QLabel *workbenchTitle_{nullptr};
    QLabel *workbenchSubtitle_{nullptr};
    QLabel *sourceTypeBadge_{nullptr};
    QPushButton *guidedModeButton_{nullptr};
    QPushButton *customModeButton_{nullptr};
    QTabWidget *stageTabs_{nullptr};
    QListWidget *sourceNav_{nullptr};
    QStackedWidget *sourceStack_{nullptr};
    QListWidget *configNav_{nullptr};
    QStackedWidget *configStack_{nullptr};
    QListWidget *resultNav_{nullptr};
    QStackedWidget *resultStack_{nullptr};

    QLabel *sourceTypeHeadline_{nullptr};
    QLabel *sourceTypeExplanation_{nullptr};
    QLabel *sourceAcquisitionDetail_{nullptr};
    QLabel *sourceIdentityDetail_{nullptr};
    QLabel *sourceInventoryDetail_{nullptr};
    QLabel *overviewIcon_{nullptr};
    QLabel *overviewChecklist_{nullptr};
    QLabel *projectStateLabel_{nullptr};
    QLabel *activeTrackerLabel_{nullptr};
    QLabel *projectAcquisitionLabel_{nullptr};
    QLabel *projectActionNotice_{nullptr};
    QPushButton *projectActionButton_{nullptr};
    QTableWidget *releaseTable_{nullptr};
    QPushButton *editReleaseButton_{nullptr};
    QPushButton *prepareReleaseButton_{nullptr};
    QPushButton *installReleaseButton_{nullptr};
    QPushButton *rollbackButton_{nullptr};
    QPushButton *uninstallButton_{nullptr};
    QPushButton *projectPrimaryButton_{nullptr};
    QPushButton *historyCheckUpdatesButton_{nullptr};
    QPushButton *editConfigurationButton_{nullptr};
    QPushButton *deleteReleaseButton_{nullptr};
    QPlainTextEdit *metadataView_{nullptr};
    QPlainTextEdit *rawMetadataView_{nullptr};
    QWidget *installMappingWidget_{nullptr};
    QWidget *appImageInstallPlanWidget_{nullptr};
    QLabel *installPlanNotice_{nullptr};
    QTreeWidget *appImageInstallPlan_{nullptr};
    QComboBox *archiveLayout_{nullptr};
    QLineEdit *installOptDirectory_{nullptr};
    QFormLayout *installMappingLayout_{nullptr};
    QLabel *installCommandsHint_{nullptr};
    QLineEdit *installBinarySource_{nullptr};
    QLineEdit *installBinaryDestination_{nullptr};
    QLineEdit *installCommonPrefix_{nullptr};
    QCheckBox *installStripPrefix_{nullptr};
    QLineEdit *packageDisplayName_{nullptr};
    QLineEdit *packageArchName_{nullptr};
    QLineEdit *packageVendorName_{nullptr};
    QLineEdit *packageDescription_{nullptr};
    QLineEdit *packageHomepage_{nullptr};
    QLineEdit *packageLicenses_{nullptr};
    QLineEdit *packageProvides_{nullptr};
    QLineEdit *packageConflicts_{nullptr};
    QTableWidget *dependenciesTable_{nullptr};
    QListWidget *additionalDependencies_{nullptr};
    QListWidget *scriptsList_{nullptr};
    QPlainTextEdit *scriptView_{nullptr};
    QLabel *scriptsActionNotice_{nullptr};
    QLabel *scriptStatus_{nullptr};
    QPushButton *acknowledgeScriptButton_{nullptr};
    QTableWidget *scriptFindingsTable_{nullptr};
    QPlainTextEdit *lifecycleView_{nullptr};
    QLabel *lifecycleStatus_{nullptr};
    QPushButton *editLifecycleButton_{nullptr};
    QPushButton *saveLifecycleButton_{nullptr};
    QPushButton *cancelLifecycleButton_{nullptr};
    QPushButton *acknowledgeLifecycleButton_{nullptr};
    QPushButton *discardLifecycleButton_{nullptr};
    bool lifecycleEditing_{false};
    QPushButton *askAiButton_{nullptr};
    QPushButton *reanalyzeButton_{nullptr};
    QTreeWidget *payloadTree_{nullptr};
    QLabel *payloadIntroduction_{nullptr};
    QLabel *payloadStatus_{nullptr};
    QPlainTextEdit *payloadPreview_{nullptr};
    QPushButton *keepPayloadButton_{nullptr};
    QPushButton *excludePayloadButton_{nullptr};
    QPushButton *clearPayloadDecisionButton_{nullptr};
    bool payloadInspectionRunning_{false};
    QTableWidget *commandsTable_{nullptr};
    QPlainTextEdit *appRunEditor_{nullptr};
    QFrame *appRunReviewBanner_{nullptr};
    QLabel *appRunReviewLabel_{nullptr};
    QLabel *appRunStatus_{nullptr};
    QPushButton *saveAppRunButton_{nullptr};
    QPushButton *keepOriginalAppRunButton_{nullptr};
    QPushButton *restoreAppRunButton_{nullptr};
    QListWidget *desktopEntriesList_{nullptr};
    QCheckBox *desktopEntryEnabled_{nullptr};
    QLineEdit *desktopEntryDestination_{nullptr};
    QPlainTextEdit *desktopEntryEditor_{nullptr};
    QLabel *desktopEntryStatus_{nullptr};
    QPushButton *saveDesktopEntryButton_{nullptr};
    QPushButton *deleteDesktopEntryButton_{nullptr};
    QLabel *iconPreview_{nullptr};
    QComboBox *payloadIconCandidates_{nullptr};
    QLineEdit *iconUrl_{nullptr};
    QLabel *iconStatus_{nullptr};
    QNetworkAccessManager *iconNetwork_{nullptr};
    QNetworkReply *iconReply_{nullptr};
    QPlainTextEdit *pkgbuildEditor_{nullptr};
    QPlainTextEdit *pkgbuildPreview_{nullptr};
    QPlainTextEdit *pkgbuildVarsPreview_{nullptr};
    QPlainTextEdit *resultPkgbuildVarsPreview_{nullptr};
    QLabel *pkgbuildState_{nullptr};
    QLabel *pkgbuildPreviewNotice_{nullptr};
    QComboBox *updateStrategy_{nullptr};
    QLineEdit *updateUrl_{nullptr};
    QComboBox *directUrlFullCheckInterval_{nullptr};
    QLineEdit *aptSuite_{nullptr};
    QLineEdit *aptComponent_{nullptr};
    QLineEdit *aptArchitecture_{nullptr};
    QLineEdit *aptPackageName_{nullptr};
    QLineEdit *rpmArchitecture_{nullptr};
    QLineEdit *rpmPackageName_{nullptr};
    QLineEdit *aptSigningKeyring_{nullptr};
    QLineEdit *aptSigningKeyUrl_{nullptr};
    QPushButton *aptSigningKeyDownloadButton_{nullptr};
    QComboBox *aptSigningKey_{nullptr};
    QLabel *aptSigningFingerprint_{nullptr};
    QLineEdit *githubOwner_{nullptr};
    QLineEdit *githubRepository_{nullptr};
    QLineEdit *githubAssetRegex_{nullptr};
    QCheckBox *githubPrereleases_{nullptr};
    QListWidget *updateCandidates_{nullptr};
    QLabel *updateNotice_{nullptr};
    QLabel *updateOwnerLabel_{nullptr};
    QLabel *updateCheckStatus_{nullptr};
    QPushButton *updateSaveButton_{nullptr};
    QPushButton *updateCheckButton_{nullptr};
    QCheckBox *repoPublishCheck_{nullptr};
    QLabel *repoOriginalName_{nullptr};
    QLabel *repoPrefixDefault_{nullptr};
    QLineEdit *repoOverrideEdit_{nullptr};
    QLabel *repoEffectiveName_{nullptr};
    QLabel *repoPublishedName_{nullptr};
    QLabel *repoNameWarning_{nullptr};
    QLabel *repoUnstableLabel_{nullptr};
    QLabel *repoStableLabel_{nullptr};
    QTableWidget *repoSoakTable_{nullptr};
    QPushButton *repoSaveButton_{nullptr};
    QPushButton *repoPromoteButton_{nullptr};
    QLabel *repoStatusLabel_{nullptr};
    QLabel *buildChecklist_{nullptr};
    QLabel *builtPackage_{nullptr};
    QPushButton *buildButton_{nullptr};
    QPushButton *installButton_{nullptr};
    QPushButton *pkgbuildBuildButton_{nullptr};
    QPushButton *resultPkgbuildBuildButton_{nullptr};
    QListWidget *historyList_{nullptr};
    QString pendingPackageOperation_;
    bool installAfterSuccessfulBuild_{false};
    QString pendingDownloadedImport_;
    ImportOptions pendingImportOptions_;
    QHash<int, SectionLocation> sectionLocations_;
};

} // namespace pacsmith::gui
