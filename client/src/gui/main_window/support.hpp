#pragma once

#include "core/background_updates.hpp"
#include "core/model.hpp"
#include "core/library_client.hpp"

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStyle>
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

constexpr int projectSubtitleRole = Qt::UserRole + 1;
constexpr int projectVisualStateRole = Qt::UserRole + 2;
constexpr int projectCheckingRole = Qt::UserRole + 3;
constexpr int projectActivityRole = Qt::UserRole + 4;
constexpr int projectRepositoryEnabledRole = Qt::UserRole + 5;
constexpr int projectRepositoryBusyRole = Qt::UserRole + 6;
constexpr int sectionBaseLabelRole = Qt::UserRole + 1;

enum class ProjectVisualState { NotInstalled, Current, UpdateAvailable, Warning, Attention, Preparing };

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
QString repositoryArchitecture(bool apt);
QList<RepositoryKeySeed> knownRepositoryKeys(const QHash<QString, Project> &projects,
                                             const LibraryClient &library);
std::optional<RepositorySourceChoice> chooseRepositorySource(
    QWidget *parent, bool rpm, const QList<RepositoryKeySeed> &knownKeys);
QString suggestedAssetRegex(const QString &asset);
bool isGitHubSidecarAsset(const QString &asset);
int githubArtifactPreference(const QString &asset);
std::optional<GitHubRuleChoice> chooseGitHubAssetRule(
    QWidget *parent, const QStringList &assets, bool includePrereleases);
QString formatLocalDateTime(const QDateTime &value);
QWidget *emptyPageHost(QWidget *parent);
QString sourcePackageTypeTitle(SourcePackageType type);
QString acquisitionKindTitle(AcquisitionKind kind);
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
QString projectDirectory(const LibraryClient &library, const Project &project);
QIcon projectIcon(const LibraryClient &library, const Project &project);
QString retainedPackagePath(const LibraryClient &library, const PackageRelease &release);
QString acquireRetainedPackagePath(const LibraryClient &library,
                                   const PackageRelease &release,
                                   QString *error = nullptr);
const BuildRecord *latestSuccessfulBuild(const PackageRelease &release);
bool releaseHasExistingBuild(const PackageRelease &release);
bool releaseHasRetainedPackage(const PackageRelease &release);
void applyPrimaryActionStyle(QPushButton *button);
QString builtPackageSummaryHtml(const LibraryClient &library, const PackageRelease &release,
                                bool building, bool installing);
qsizetype pendingPayloadReviews(const PackageRelease &project);
QStringList pendingPayloadReviewPaths(const PackageRelease &project, qsizetype limit = 12);
bool unsafePackageSymlink(const PayloadEntry &entry);
bool pkgbuildReferencesLifecycle(const QString &pkgbuild, const QString &fileName);
QString pkgbuildLifecycleReference(const QString &pkgbuild);
bool findingRequiresArchAction(const PackageRelease &project, const ScriptFinding &finding);
qsizetype unresolvedResponsibilitiesForScript(const PackageRelease &project,
                                              const QString &scriptName);
qsizetype pendingScriptFindings(const PackageRelease &project);

} // namespace pacsmith::gui
