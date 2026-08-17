#pragma once

#include "core/apt_update_service.hpp"
#include "core/ai_service.hpp"
#include "core/ai_model_catalog_service.hpp"
#include "core/app_settings.hpp"
#include "core/chatgpt_auth.hpp"
#include "core/credential_store.hpp"
#include "core/deb_download_service.hpp"
#include "core/github_update_service.hpp"
#include "core/process_services.hpp"
#include "core/project_store.hpp"
#include "core/repository_key_download_service.hpp"
#include "core/rpm_update_service.hpp"

#include <QHash>
#include <QMainWindow>

#include <optional>

class QComboBox;
class QCheckBox;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QFormLayout;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QProgressDialog;
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

class AiProgressDialog;
class CommandProgressDialog;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void importPackage(const QString &path);

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
        ConfigLayout,
        ConfigDependencies,
        ConfigScripts,
        ConfigCommands,
        ConfigAppRun,
        ConfigDesktopEntries,
        ConfigIcon,
        ConfigUpdates,
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
    void refreshProjectList(const QString &selectId = {});
    void loadProject(const QString &id);
    void refreshCurrentProject();
    void populateCurrentWorkbenchPage();
    void updateDeleteButton();
    void populateOverview();
    void updateDashboardActions();
    void placeUpdatesEditor();
    void handleProjectPrimaryAction();
    void editPackageConfiguration();
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
    void validateAndApplyAiResolution(const AiResolution &resolution);
    void finishAiResolution(const AiResolution &resolution);
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
    void populateUpdates();
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
    bool saveUpdateConfiguration();
    void startUpdateCheck();
    void applyUpdateCheckResult(const UpdateCheckResult &result, const QString &sourceName);
    void startBuild();
    void startInstall();
    void startUninstall();
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
    void resetPreparationState();
    void beginGitHubImport(const QUrl &url);
    void continueGitHubImport(const QString &owner, const QString &repository,
                              const QString &assetRegex, bool includePrereleases,
                              const QString &requestedTag = {});
    void downloadGitHubAsset(const PackageRelease &probe,
                             const UpdateCheckResult &result);
    void startGitHubChooserAi(const PackageRelease &probe, const QStringList &assets,
                              const QString &preferredAsset, QLineEdit *editor,
                              QLabel *status, QPushButton *button, QWidget *dialog);
    [[nodiscard]] QString selectedDashboardReleaseId() const;
    void showSettings();
    void startReanalysis();
    void startAiResolution();
    void startGithubRegexAi();
    void applyGithubRegexAi(const AiResolution &resolution);
    void applyAiResolution(const AiResolution &resolution);
    bool unlockAgeCredentials();
    void importSigningKey();
    void downloadSigningKey();
    [[nodiscard]] PackageRelease *currentRelease();
    [[nodiscard]] const PackageRelease *currentRelease() const;
    [[nodiscard]] PackageRelease *activeTrackingRelease();
    [[nodiscard]] const PackageRelease *activeTrackingRelease() const;
    [[nodiscard]] PackageRelease *updateEditorRelease();

    ProjectStore store_;
    AppSettingsStore settingsStore_;
    AiSettings aiSettings_;
    CredentialStore credentialStore_;
    std::optional<Project> project_;
    QHash<QString, Project> projectCache_;
    QString currentReleaseId_;
    QString updateCheckReleaseId_;
    bool updateCheckFromWorkbench_{false};
    BuildService buildService_;
    InstallService installService_;
    DebDownloadService debDownloadService_;
    RepositoryKeyDownloadService signingKeyDownloadService_;
    AptUpdateService aptUpdateService_;
    RpmUpdateService rpmUpdateService_;
    GitHubUpdateService githubUpdateService_;
    AiAnalysisService aiService_;
    AiModelCatalogService aiModelCatalogService_;
    ChatGptLoginService chatGptLoginService_;
    QString agePassword_;
    bool populating_{false};
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
    AiProgressDialog *aiProgress_{nullptr};
    CommandProgressDialog *commandProgress_{nullptr};
    bool aiProgressCanceled_{false};
    bool githubRegexAiPending_{false};
    QString githubRegexAiReleaseId_;
    QStringList githubRegexAiAssets_;
    QString githubRegexAiPreferredAsset_;
    QStringList repositoryPackageNames_;
    QSet<QString> repositoryPackages_;
    QHash<QString, bool> repositoryDependencyAvailability_;
    QSet<QString> repositoryDependencyChecksPending_;
    bool repositoryCatalogLoaded_{false};
    bool repositoryImportRunning_{false};

    QWidget *projectSidebar_{nullptr};
    QListWidget *projectList_{nullptr};
    QPushButton *deleteProjectButton_{nullptr};
    QLabel *projectTitle_{nullptr};
    QLabel *projectSubtitle_{nullptr};
    QStackedWidget *rightStack_{nullptr};
    QTabWidget *projectTabs_{nullptr};
    QWidget *dashboardUpdatesHost_{nullptr};
    QWidget *configUpdatesHost_{nullptr};
    QWidget *updatesEditor_{nullptr};
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
    QTableWidget *dependenciesTable_{nullptr};
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
    QPushButton *resolveWithAiButton_{nullptr};
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
    QLabel *pkgbuildState_{nullptr};
    QLabel *pkgbuildPreviewNotice_{nullptr};
    QComboBox *updateStrategy_{nullptr};
    QLineEdit *updateUrl_{nullptr};
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
    QPushButton *githubRegexAiButton_{nullptr};
    QCheckBox *githubPrereleases_{nullptr};
    QListWidget *updateCandidates_{nullptr};
    QLabel *updateNotice_{nullptr};
    QLabel *updateOwnerLabel_{nullptr};
    QLabel *updateCheckStatus_{nullptr};
    QPushButton *updateSaveButton_{nullptr};
    QPushButton *updateCheckButton_{nullptr};
    QLabel *buildChecklist_{nullptr};
    QLabel *builtPackage_{nullptr};
    QPushButton *buildButton_{nullptr};
    QPushButton *installButton_{nullptr};
    QPushButton *pkgbuildBuildButton_{nullptr};
    QPushButton *resultPkgbuildBuildButton_{nullptr};
    QListWidget *historyList_{nullptr};
    QString pendingPackageOperation_;
    QString pendingDownloadedImport_;
    ImportOptions pendingImportOptions_;
    QHash<int, SectionLocation> sectionLocations_;
};

} // namespace pacsmith::gui
