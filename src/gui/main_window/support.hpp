#pragma once

#include "core/ai_service.hpp"
#include "core/background_updates.hpp"
#include "core/model.hpp"
#include "core/project_store/project_store.hpp"

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QThread>
#include <QUrl>

#include <functional>
#include <optional>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

namespace pacsmith::gui {

template <typename Service, typename Function>
void onServiceThread(Service &service, Function &&function) {
    QMetaObject::invokeMethod(&service, std::forward<Function>(function));
}

template <typename Service>
void cancelOnServiceThread(Service &service) {
    if (service.thread() == QThread::currentThread()) {
        service.cancel();
        return;
    }
    QMetaObject::invokeMethod(&service, [&service] { service.cancel(); },
                              Qt::BlockingQueuedConnection);
}

template <typename Service>
void shutdownNetworkService(Service *&service) {
    if (service == nullptr) return;
    cancelOnServiceThread(*service);
    service->deleteLater();
    service = nullptr;
}

template <typename Service>
Service *networkServiceOnThread(QThread &thread) {
    auto *service = new Service;
    service->moveToThread(&thread);
    return service;
}

constexpr int projectSubtitleRole = Qt::UserRole + 1;
constexpr int projectVisualStateRole = Qt::UserRole + 2;
constexpr int projectCheckingRole = Qt::UserRole + 3;
constexpr int projectActivityRole = Qt::UserRole + 4;
constexpr int sectionBaseLabelRole = Qt::UserRole + 1;

enum class ProjectVisualState { NotInstalled, Current, UpdateAvailable, Attention, Preparing };

struct AiDependencyCandidate {
    int index{-1};
    QString package;
};

struct GitHubRuleChoice {
    QString expression;
    bool includePrereleases{false};
};

struct RepositorySourceChoice {
    UpdateConfiguration update;
    QUrl signingKeyUrl;
    QByteArray signingKeyContents;
    QString signingKeySource;
};

struct RepositoryKeySeed {
    QString label;
    QByteArray contents;
    QString source;
};

using GitHubAiAssist = std::function<void(const QStringList &, const QString &,
                                          QLineEdit *, QLabel *, QPushButton *, QWidget *)>;

inline const QColor payloadReviewAmber(Qt::darkYellow);
inline const QColor payloadUnsafeRed(0xe5, 0x53, 0x4b);

QString downloadActivityText(const QString &phase, qint64 received, qint64 total);
QString finishedUpdateCheckStatus(const BackgroundUpdateState &state);
QString downloadStatusText(const QString &name, const QString &phase, qint64 received,
                           qint64 total);
bool requiresRepositoryPackage(const DependencyMapping &dependency);
bool repositoryPackageUnavailable(const DependencyMapping &dependency,
                                  const QHash<QString, bool> &availability);
bool payloadRuleCovers(const PayloadRule &rule, const QString &path);
QList<AiDependencyCandidate> requiredAiDependencyCandidates(
    const PackageRelease &release, const AiResolution &resolution);
QString repositoryArchitecture(bool apt);
QList<RepositoryKeySeed> knownRepositoryKeys(const QHash<QString, Project> &projects,
                                             const ProjectStore &store);
std::optional<RepositorySourceChoice> chooseRepositorySource(
    QWidget *parent, bool rpm, const QList<RepositoryKeySeed> &knownKeys);
QString suggestedAssetRegex(const QString &asset);
bool isGitHubSidecarAsset(const QString &asset);
int githubArtifactPreference(const QString &asset);
std::optional<GitHubRuleChoice> chooseGitHubAssetRule(
    QWidget *parent, const QStringList &assets, bool includePrereleases,
    const GitHubAiAssist &aiAssist);
QString formatLocalDateTime(const QDateTime &value);
QWidget *emptyPageHost(QWidget *parent);
QString sourcePackageTypeTitle(SourcePackageType type);
QString acquisitionKindTitle(AcquisitionKind kind);
QString reasoningEffortLabel(AiReasoningEffort effort);
QList<AiReasoningEffort> supportedReasoningEfforts(AiProviderKind provider, const QString &model);
void makeReadOnlyCodeEditor(QPlainTextEdit *editor);
void configureIdentityVariablesEditor(QPlainTextEdit *editor);
QTreeWidgetItem *ensureInstallPlanNode(QTreeWidget *tree, QHash<QString, QTreeWidgetItem *> *nodes,
                                       const QString &absolutePath);
void addInstallPlanEntry(QTreeWidget *tree, QHash<QString, QTreeWidgetItem *> *nodes,
                         const QString &path, const QString &source, const QString &purpose,
                         const QColor &foreground = {});
int decorateInstallPlanTree(QTreeWidgetItem *item);
void showDetailedMessageDialog(QWidget *parent, const QString &title, const QString &message,
                               const QString &diagnosticDetails,
                               QStyle::StandardPixmap iconType,
                               bool showDetailsInitially = false);
void showAiErrorDialog(QWidget *parent, const AiResolution &resolution);
QString projectDirectory(const ProjectStore &store, const Project &project);
QIcon projectIcon(const ProjectStore &store, const Project &project);
QString retainedPackagePath(const ProjectStore &store, const PackageRelease &release);
const BuildRecord *latestSuccessfulBuild(const PackageRelease &release);
bool releaseHasExistingBuild(const PackageRelease &release);
void applyPrimaryActionStyle(QPushButton *button);
QString builtPackageSummaryHtml(const ProjectStore &store, const PackageRelease &release,
                                bool building, bool installing);
qsizetype pendingPayloadReviews(const PackageRelease &project);
bool unsafePackageSymlink(const PayloadEntry &entry);
bool pkgbuildReferencesLifecycle(const QString &pkgbuild, const QString &fileName);
QString pkgbuildLifecycleReference(const QString &pkgbuild);
bool findingRequiresArchAction(const PackageRelease &project, const ScriptFinding &finding);
qsizetype unresolvedResponsibilitiesForScript(const PackageRelease &project,
                                              const QString &scriptName);
qsizetype pendingScriptFindings(const PackageRelease &project);

} // namespace pacsmith::gui
