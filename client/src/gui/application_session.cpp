#include "gui/application_session.hpp"

#include "core/app_settings.hpp"
#include "core/background_updates.hpp"
#include "core/credential_store.hpp"
#include "core/library_client.hpp"
#include "core/library_events.hpp"
#include "gui/appearance.hpp"
#include "gui/main_window/main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QIcon>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QStyle>
#include <QtConcurrent>

#include <algorithm>

namespace pacsmith::gui {
namespace {

QIcon applicationIcon() {
    return QIcon(QStringLiteral(":/pacsmith/icons/pacsmith.png"));
}

QIcon trayStatusIcon(const int availableUpdates, const QColor &foreground,
                     const int activityFrame = -1) {
    auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
    QIcon source(QStringLiteral(":/pacsmith/icons/pacsmith-tray.png"));
    QPixmap mask = source.pixmap(QSize(32, 32));
    if (mask.isNull() && application != nullptr) {
        mask = application->style()->standardIcon(QStyle::SP_ComputerIcon).pixmap(32, 32);
    }
    return renderTrayStatusIcon(mask, availableUpdates, foreground, activityFrame);
}

QString pacsmithCliPath() {
    auto program = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("pacsmith"));
    if (!QFileInfo::exists(program)) program = QStandardPaths::findExecutable(QStringLiteral("pacsmith"));
    return program;
}

}

ApplicationSession::ApplicationSession(AppSettingsStore &settingsStore, CredentialStore &credentials,
                                       QObject *parent)
    : QObject(parent), settingsStore_(settingsStore), credentials_(credentials) {
    connect(&server_, &GuiInstanceServer::activated, this, &ApplicationSession::showWorkbench);
    connect(&server_, &GuiInstanceServer::checkRequested, this, [this] {
        runBackgroundCheck(CheckKind::Manual);
    });
    connect(&server_, &GuiInstanceServer::trayRequested, this, &ApplicationSession::refreshTray);
    connect(&server_, &GuiInstanceServer::projectsReloadRequested, this, [this] {
        if (window_ != nullptr) window_->reloadVisibleProjects(false);
        refreshTray();
    });
    trayRefresh_.setInterval(5000);
    connect(&trayRefresh_, &QTimer::timeout, this, &ApplicationSession::refreshTray);
    trayAnimation_.setInterval(160);
    connect(&trayAnimation_, &QTimer::timeout, this, [this] {
        trayActivityFrame_ = (trayActivityFrame_ + 1) % 8;
        refreshTray();
    });
    checkTimer_.setSingleShot(true);
    connect(&checkTimer_, &QTimer::timeout, this, [this] { runBackgroundCheck(CheckKind::Scheduled); });
}

ApplicationSession::~ApplicationSession() = default;

bool ApplicationSession::listen() { return server_.listen(); }

bool ApplicationSession::trayWanted() const {
    const auto settings = settingsStore_.load().updates;
    return settings.keepInTray || settings.startMinimized || startHidden_;
}

void ApplicationSession::start(const bool startHidden, const QString &importPath) {
    startHidden_ = startHidden;
    trayRefresh_.start();
    if (trayWanted()) ensureTray();
    libraryEventStream_ = new LibraryEventStream(ConnectionConfig::load(), this);
    connect(libraryEventStream_, &LibraryEventStream::eventReceived,
            this, &ApplicationSession::handleServerEvent);
    libraryEventStream_->start();
    auto *activeJobs = new QFutureWatcher<QList<JobStatus>>(this);
    connect(activeJobs, &QFutureWatcher<QList<JobStatus>>::finished, this, [this, activeJobs] {
        const auto jobs = activeJobs->result();
        activeJobs->deleteLater();
        for (const auto &job : jobs) {
            ServerEvent event;
            event.jobId = job.id;
            event.jobKind = job.kind;
            event.jobStatus = job.status;
            event.projectId = job.projectId;
            event.projectName = job.projectName;
            event.packageName = job.packageName;
            event.releaseId = job.releaseId;
            handleServerEvent(event);
        }
    });
    const auto connection = ConnectionConfig::load();
    activeJobs->setFuture(QtConcurrent::run([connection] {
        return LibraryClient(connection).activeJobs(QStringLiteral("build"));
    }));
    if (!startHidden || !importPath.isEmpty()) showWorkbench(importPath);
    QTimer::singleShot(0, this, [this] {
        runBackgroundCheck(CheckKind::IfOverdue);
        scheduleNextCheck();
    });
}

void ApplicationSession::handleServerEvent(const ServerEvent &event) {
    if (event.jobKind != QStringLiteral("build") || event.jobId.isEmpty()) return;
    const bool finished = event.jobStatus == QStringLiteral("succeeded") ||
                          event.jobStatus == QStringLiteral("failed") ||
                          event.jobStatus == QStringLiteral("interrupted");
    if (finished) {
        activeBuildJobs_.remove(event.jobId);
    } else if (event.jobStatus == QStringLiteral("queued") ||
               event.jobStatus == QStringLiteral("running")) {
        auto name = !event.projectName.isEmpty() ? event.projectName : event.packageName;
        if (name.isEmpty()) name = QStringLiteral("package");
        activeBuildJobs_.insert(event.jobId, name);
    }
    if (activeBuildJobs_.isEmpty()) trayAnimation_.stop();
    else if (!trayAnimation_.isActive()) trayAnimation_.start();
    refreshTray();
}

void ApplicationSession::showWorkbench(const QString &importPath) {
    const bool created = window_ == nullptr;
    if (created) {
        window_ = std::make_unique<MainWindow>(settingsStore_, credentials_);
        window_->resize(1180, 760);
        window_->setWindowIcon(applicationIcon());
    }
    window_->setKeepRunningInTray(trayWanted());
    window_->activateExistingSession(importPath);
    if (created && importPath.isEmpty()) maybeOnboard();
}

void ApplicationSession::ensureTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    if (tray_ == nullptr) setupTray();
    if (window_ != nullptr) window_->setKeepRunningInTray(true);
    QApplication::setQuitOnLastWindowClosed(false);
    refreshTray();
    if (!trayRefresh_.isActive()) trayRefresh_.start();
}

void ApplicationSession::setupTray() {
    trayMenu_ = std::make_unique<QMenu>();
    auto *openAction = trayMenu_->addAction(QStringLiteral("Open PacSmith"));
    auto *checkAction = trayMenu_->addAction(QStringLiteral("Check for Updates Now"));
    trayMenu_->addSeparator();
    auto *exitAction = trayMenu_->addAction(QStringLiteral("Quit"));
    connect(openAction, &QAction::triggered, this, [this] { showWorkbench(); });
    connect(checkAction, &QAction::triggered, this, [this] { runBackgroundCheck(CheckKind::Manual); });
    connect(exitAction, &QAction::triggered, this, &ApplicationSession::quitSession);

    tray_ = std::make_unique<QSystemTrayIcon>();
    tray_->setContextMenu(trayMenu_.get());
    connect(tray_.get(), &QSystemTrayIcon::activated, this,
            [this](const QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick ||
            reason == QSystemTrayIcon::MiddleClick) {
            // StatusNotifier left-click is Activate with no usable screen position
            // (Qt also reports empty tray geometry). A widget menu cannot be placed,
            // so open the window. Right-click still uses the native DBus menu.
            showWorkbench();
        }
    });
}

void ApplicationSession::refreshTray() {
    const auto settings = settingsStore_.load();
    const bool trayAvailable = QSystemTrayIcon::isSystemTrayAvailable();
    const bool wantTray = trayWanted() && trayAvailable;
    if (window_ != nullptr) window_->setKeepRunningInTray(wantTray);
    QApplication::setQuitOnLastWindowClosed(!wantTray);
    if (settings.updates.enabled) {
        runBackgroundCheck(CheckKind::IfOverdue);
        if (!checkTimer_.isActive()) scheduleNextCheck();
    } else {
        checkTimer_.stop();
    }
    if (!wantTray) {
        if (tray_ != nullptr) tray_->hide();
        return;
    }
    if (tray_ == nullptr) {
        if (!trayAvailable) return;
        setupTray();
    }
    const auto current = BackgroundUpdateStateStore::load();
    // Use the persisted census. Reloading every project here would parse
    // release.json and shell out to pacman -Q on the GUI thread every 5s.
    const auto availableUpdates = current.availableUpdates;
    const auto checking = current.checking;
    const auto preparing = !current.preparingProjectId.isEmpty();
    const auto foreground = trayIconColor(settings.appearance.trayTheme);
    const auto activityFrame = activeBuildJobs_.isEmpty() ? -1 : trayActivityFrame_;
    if (lastTrayBadge_ != availableUpdates || lastTrayChecking_ != checking ||
        lastTrayPreparing_ != preparing || lastTrayColor_ != foreground.rgba() ||
        lastTrayActivityFrame_ != activityFrame) {
        lastTrayBadge_ = availableUpdates;
        lastTrayChecking_ = checking;
        lastTrayPreparing_ = preparing;
        lastTrayColor_ = foreground.rgba();
        lastTrayActivityFrame_ = activityFrame;
        tray_->setIcon(trayStatusIcon(availableUpdates, foreground, activityFrame));
    }
    if (!activeBuildJobs_.isEmpty()) {
        auto names = activeBuildJobs_.values();
        names.removeDuplicates();
        names.sort(Qt::CaseInsensitive);
        tray_->setToolTip(QStringLiteral("PacSmith is building %1").arg(names.join(QStringLiteral(", "))));
    } else tray_->setToolTip(preparing
        ? (current.preparingProjectName.isEmpty()
               ? QStringLiteral("PacSmith is downloading an update")
               : QStringLiteral("PacSmith is downloading an update for %1")
                     .arg(current.preparingProjectName))
        : checking ? QStringLiteral("PacSmith is checking for updates")
        : availableUpdates > 0 ? QStringLiteral("PacSmith: %1 update(s) available").arg(availableUpdates)
                               : QStringLiteral("PacSmith: packages are current"));
    tray_->setVisible(true);
    if (window_ != nullptr && (checking || preparing)) {
        window_->noteBackgroundCheckStarted();
    }
}

void ApplicationSession::scheduleNextCheck() {
    const auto settings = settingsStore_.load();
    if (!settings.updates.enabled) {
        checkTimer_.stop();
        return;
    }
    const auto next = BackgroundUpdateManager::nextScheduledOccurrence(settings.updates);
    auto milliseconds = QDateTime::currentDateTime().msecsTo(next);
    if (milliseconds < 1000) milliseconds = 1000;
    if (milliseconds > 24 * 60 * 60 * 1000LL) milliseconds = 24 * 60 * 60 * 1000LL;
    checkTimer_.start(static_cast<int>(milliseconds));
}

void ApplicationSession::runBackgroundCheck(const bool onlyIfOverdue) {
    runBackgroundCheck(onlyIfOverdue ? CheckKind::IfOverdue : CheckKind::Manual);
}

void ApplicationSession::runBackgroundCheck(const CheckKind kind) {
    const auto settings = settingsStore_.load();
    if (kind != CheckKind::Manual && !settings.updates.enabled) return;
    if (checkProcess_ != nullptr) return;
    const auto updateState = BackgroundUpdateStateStore::load();
    if (updateState.checking) return;
    if (kind == CheckKind::IfOverdue &&
        !BackgroundUpdateManager::isOverdue(settings.updates, updateState.lastRun)) {
        return;
    }
    const auto program = pacsmithCliPath();
    if (program.isEmpty()) {
        if (tray_ != nullptr) {
            tray_->showMessage(QStringLiteral("PacSmith"),
                               QStringLiteral("Could not find the pacsmith command to check for updates."));
        }
        return;
    }
    checkProcess_ = new QProcess(this);
    checkProcess_->setProgram(program);
    checkProcess_->setArguments({QStringLiteral("check"), QStringLiteral("--all")});
    const auto source = settings.credentialSources.value(QStringLiteral("github"),
                                                         CredentialSource::Environment);
    checkProcess_->setProcessEnvironment(credentials_.environmentWithGithubToken(source));
    auto pendingState = BackgroundUpdateStateStore::load();
    pendingState.checking = true;
    BackgroundUpdateStateStore::claimActivity(pendingState);
    pendingState.message = QStringLiteral("Checking for updates");
    static_cast<void>(BackgroundUpdateStateStore::save(pendingState));
    connect(checkProcess_, &QProcess::finished, this, [this] {
        if (checkProcess_ != nullptr) {
            checkProcess_->deleteLater();
            checkProcess_ = nullptr;
        }
        auto finishedState = BackgroundUpdateStateStore::load();
        if (finishedState.checking || !finishedState.preparingProjectId.isEmpty()) {
            finishedState.checking = false;
            BackgroundUpdateStateStore::clearActivityOwner(finishedState);
            finishedState.checkingProjectId.clear();
            finishedState.checkingProjectName.clear();
            finishedState.preparingProjectId.clear();
            finishedState.preparingProjectName.clear();
            finishedState.preparationPhase.clear();
            finishedState.preparationBytesReceived = 0;
            finishedState.preparationBytesTotal = -1;
            static_cast<void>(BackgroundUpdateStateStore::save(finishedState));
        }
        if (window_ != nullptr) window_->reloadVisibleProjects();
        scheduleNextCheck();
        refreshTray();
    });
    connect(checkProcess_, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        if (checkProcess_ == nullptr) return;
        checkProcess_->deleteLater();
        checkProcess_ = nullptr;
        auto failedState = BackgroundUpdateStateStore::load();
        failedState.checking = false;
        BackgroundUpdateStateStore::clearActivityOwner(failedState);
        failedState.checkingProjectId.clear();
        failedState.checkingProjectName.clear();
        static_cast<void>(BackgroundUpdateStateStore::save(failedState));
        if (tray_ != nullptr) {
            tray_->showMessage(QStringLiteral("PacSmith"),
                               QStringLiteral("Could not start an update check."));
        }
        if (window_ != nullptr) window_->reloadVisibleProjects();
    });
    checkProcess_->start();
    if (window_ != nullptr) window_->noteBackgroundCheckStarted();
}

void ApplicationSession::quitSession() {
    if (window_ != nullptr) window_->setKeepRunningInTray(false);
    QApplication::setQuitOnLastWindowClosed(true);
    QCoreApplication::quit();
}

void ApplicationSession::maybeOnboard() {
    if (onboardingStarted_ || window_ == nullptr) return;
    onboardingStarted_ = true;
    auto *window = window_.get();
    QTimer::singleShot(0, window, [this, window] {
        auto settings = settingsStore_.load();
        bool changed = false;
        if (!settings.debAssociationPrompted) {
            settings.debAssociationPrompted = true;
            changed = true;
            const auto xdgMime = QStandardPaths::findExecutable(QStringLiteral("xdg-mime"));
            const auto currentDefault = [&xdgMime](const QString &mimeType) {
                if (xdgMime.isEmpty()) return QString{};
                QProcess query;
                query.start(xdgMime, {QStringLiteral("query"), QStringLiteral("default"),
                                      mimeType});
                if (query.waitForFinished(3000)) {
                    return QString::fromUtf8(query.readAllStandardOutput()).trimmed();
                }
                return QString{};
            };
            const QStringList packageMimeTypes{
                QStringLiteral("application/vnd.debian.binary-package"),
                QStringLiteral("application/x-rpm")};
            const auto needsAssociation = std::any_of(
                packageMimeTypes.cbegin(), packageMimeTypes.cend(),
                [&currentDefault](const auto &mimeType) {
                    return currentDefault(mimeType) != QStringLiteral("pacsmith.desktop");
                });
            if (needsAssociation &&
                QMessageBox::question(
                    window, QStringLiteral("Open vendor packages with PacSmith"),
                    QStringLiteral("Set PacSmith as your default application for DEB and RPM package files? This changes only your desktop user's MIME preferences.")) ==
                    QMessageBox::Yes) {
                bool failed = xdgMime.isEmpty();
                for (const auto &mimeType : packageMimeTypes) {
                    if (!failed && QProcess::execute(
                            xdgMime, {QStringLiteral("default"), QStringLiteral("pacsmith.desktop"),
                                      mimeType}) != 0) {
                        failed = true;
                    }
                }
                if (failed) {
                    QMessageBox::warning(
                        window, QStringLiteral("Could not set package associations"),
                        QStringLiteral("Install PacSmith for the current user first, then choose PacSmith from your desktop's Open With dialog."));
                }
            }
        }
        if (!settings.selfTrackingPrompted) {
            settings.selfTrackingPrompted = true;
            changed = true;
            const LibraryClient library;
            const bool emptyWorkbench = library.list().isEmpty();
            const auto message = emptyWorkbench
                ? QStringLiteral("Set PacSmith up as the first project in this workbench? PacSmith will inspect the official x86_64 release artifact from GitHub and prepare a normal pacman package. It will remain marked Not installed until that package is actually installed through pacman.")
                : QStringLiteral("Track PacSmith's own official GitHub releases as a project too? It will remain marked Not installed unless a PacSmith-built package is actually installed through pacman.");
            if (QMessageBox::question(window, QStringLiteral("Track PacSmith updates"), message) ==
                QMessageBox::Yes) {
                if (changed) {
                    QString error;
                    if (!settingsStore_.save(settings, &error)) {
                        QMessageBox::warning(window, QStringLiteral("Could not save onboarding settings"), error);
                    }
                    changed = false;
                }
                window->importPackage(QStringLiteral("https://github.com/anderson-arlen/pacsmith"));
            }
        }
        if (changed) {
            QString error;
            if (!settingsStore_.save(settings, &error)) {
                QMessageBox::warning(window, QStringLiteral("Could not save onboarding settings"), error);
            }
        }
    });
}

} // namespace pacsmith::gui
