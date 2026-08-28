#include "gui/application_session.hpp"

#include "core/app_settings.hpp"
#include "core/background_updates.hpp"
#include "core/library_client.hpp"
#include "core/library_events.hpp"
#include "gui/appearance.hpp"
#include "gui/main_window/main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QFutureWatcher>
#include <QIcon>
#include <QJsonArray>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QStyle>
#include <QtConcurrent>

#include <algorithm>
#include <optional>

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

struct UpdateCensusResult {
    QList<Project> projects;
    QString error;
};

struct UpdateRequestResult {
    std::optional<JobStatus> job;
    QString error;
};

QString updateCompletionMessage(const JobStatus &job) {
    const auto checks = job.result.value(QStringLiteral("checks")).toArray();
    int available = 0;
    int built = 0;
    QStringList failures;
    QStringList paused;
    for (const auto &value : checks) {
        const auto check = value.toObject();
        if (check.value(QStringLiteral("update_available")).toBool()) ++available;
        if (check.value(QStringLiteral("built")).toBool()) ++built;
        auto name = check.value(QStringLiteral("project_name")).toString();
        if (name.isEmpty()) name = check.value(QStringLiteral("package_name")).toString();
        if (name.isEmpty()) name = check.value(QStringLiteral("project_id")).toString();
        if (name.isEmpty()) name = QStringLiteral("Unknown package");
        if (check.value(QStringLiteral("status")).toString() != QStringLiteral("error")) continue;
        auto message = check.value(QStringLiteral("message")).toString().trimmed();
        if (message.isEmpty()) {
            message = QStringLiteral("update check failed without an error message");
        }
        failures.append(QStringLiteral("• %1 — %2").arg(name, message));
    }
    for (const auto &value : checks) {
        const auto check = value.toObject();
        if (check.value(QStringLiteral("automatic_status")).toString() !=
            QStringLiteral("paused")) continue;
        auto name = check.value(QStringLiteral("project_name")).toString();
        if (name.isEmpty()) name = check.value(QStringLiteral("package_name")).toString();
        if (name.isEmpty()) name = check.value(QStringLiteral("project_id")).toString();
        if (name.isEmpty()) name = QStringLiteral("Unknown package");
        auto message = check.value(QStringLiteral("automatic_message")).toString().trimmed();
        if (message.isEmpty()) message = QStringLiteral("automatic handling paused without a reason");
        paused.append(QStringLiteral("• %1 — %2").arg(name, message));
    }
    if (!failures.isEmpty() || !paused.isEmpty()) {
        auto summary = QStringLiteral("Update check finished: %1 update(s) found; %2 built automatically")
                           .arg(available).arg(built);
        if (!failures.isEmpty()) {
            summary += QStringLiteral("; %1 check(s) failed.\nFailed checks:\n%2")
                           .arg(failures.size()).arg(failures.join(QLatin1Char('\n')));
        } else {
            summary += QLatin1Char('.');
        }
        if (!paused.isEmpty()) {
            summary += QStringLiteral("\nAutomatic handling paused:\n%1")
                           .arg(paused.join(QLatin1Char('\n')));
        }
        return summary;
    }
    if (available > 0) {
        return QStringLiteral("Update check finished: %1 update(s) found; %2 built automatically.")
            .arg(available).arg(built);
    }
    return built > 0
        ? QStringLiteral("Update check finished: all vendor versions are current; %1 prepared update(s) built automatically.")
              .arg(built)
        : QStringLiteral("Update check finished: all eligible packages are current.");
}

}

ApplicationSession::ApplicationSession(AppSettingsStore &settingsStore, QObject *parent)
    : QObject(parent), settingsStore_(settingsStore) {
    connect(&server_, &GuiInstanceServer::activated, this, &ApplicationSession::showWorkbench);
    connect(&server_, &GuiInstanceServer::checkRequested, this, [this] {
        runBackgroundCheck();
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
        LibraryClient client(connection);
        auto jobs = client.activeJobs(QStringLiteral("build"));
        jobs.append(client.activeJobs(QStringLiteral("update_check")));
        jobs.append(client.activeJobs(QStringLiteral("update_prepare")));
        return jobs;
    }));
    if (!startHidden || !importPath.isEmpty()) showWorkbench(importPath);
}

void ApplicationSession::handleServerEvent(const ServerEvent &event) {
    if (event.jobId.isEmpty()) return;
    const bool finished = event.jobStatus == QStringLiteral("succeeded") ||
                          event.jobStatus == QStringLiteral("failed") ||
                          event.jobStatus == QStringLiteral("interrupted");
    const bool active = event.jobStatus == QStringLiteral("queued") ||
                        event.jobStatus == QStringLiteral("running");
    if (event.jobKind == QStringLiteral("build")) {
        if (finished) {
            activeBuildJobs_.remove(event.jobId);
        } else if (active) {
            auto name = !event.projectName.isEmpty() ? event.projectName : event.packageName;
            if (name.isEmpty()) name = QStringLiteral("package");
            activeBuildJobs_.insert(event.jobId, name);
        }
    } else if (event.jobKind == QStringLiteral("update_check")) {
        if (finished) {
            activeUpdateJobs_.remove(event.jobId);
            refreshUpdateCensus();
        } else if (active) {
            activeUpdateJobs_.insert(event.jobId, jobStatusMessage(event));
        }
    } else if (event.jobKind == QStringLiteral("update_prepare")) {
        if (finished) {
            activePreparationJobs_.remove(event.jobId);
            refreshUpdateCensus();
        } else if (active) {
            auto name = !event.projectName.isEmpty() ? event.projectName : event.packageName;
            activePreparationJobs_.insert(event.jobId, name);
        }
    } else {
        return;
    }
    if (activeBuildJobs_.isEmpty() && activeUpdateJobs_.isEmpty() &&
        activePreparationJobs_.isEmpty() && !updateRequestInFlight_) {
        trayAnimation_.stop();
    } else if (!trayAnimation_.isActive()) {
        trayAnimation_.start();
    }
    refreshTray();
}

void ApplicationSession::showWorkbench(const QString &importPath) {
    const bool created = window_ == nullptr;
    if (created) {
        window_ = std::make_unique<MainWindow>(settingsStore_);
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
    connect(checkAction, &QAction::triggered, this, &ApplicationSession::runBackgroundCheck);
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
    const auto checking = current.checking || updateRequestInFlight_ || !activeUpdateJobs_.isEmpty();
    const auto preparing = !current.preparingProjectId.isEmpty() ||
                           !activePreparationJobs_.isEmpty();
    const auto foreground = trayIconColor(settings.appearance.trayTheme);
    const auto activityFrame = activeBuildJobs_.isEmpty() && !checking && !preparing
        ? -1 : trayActivityFrame_;
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
    } else {
        const auto updateMessages = activeUpdateJobs_.values();
        const auto checkingMessage = !updateMessages.isEmpty() && !updateMessages.constFirst().isEmpty()
            ? updateMessages.constFirst() : current.message;
        const auto serverPreparationNames = activePreparationJobs_.values();
        const auto preparingName = !current.preparingProjectName.isEmpty()
            ? current.preparingProjectName
            : serverPreparationNames.isEmpty() ? QString{} : serverPreparationNames.constFirst();
        tray_->setToolTip(preparing
        ? (preparingName.isEmpty()
               ? QStringLiteral("PacSmith is downloading an update")
               : QStringLiteral("PacSmith is downloading an update for %1")
                     .arg(preparingName))
        : checking ? (checkingMessage.isEmpty() ? QStringLiteral("PacSmith is checking for updates")
                                                : checkingMessage)
        : availableUpdates > 0 ? QStringLiteral("PacSmith: %1 update(s) available").arg(availableUpdates)
                               : QStringLiteral("PacSmith: packages are current"));
    }
    tray_->setVisible(true);
    if (window_ != nullptr && (checking || preparing)) {
        window_->noteBackgroundCheckStarted();
    }
}

void ApplicationSession::refreshUpdateCensus() {
    if (updateCensusInFlight_) return;
    updateCensusInFlight_ = true;
    auto *watcher = new QFutureWatcher<UpdateCensusResult>(this);
    connect(watcher, &QFutureWatcher<UpdateCensusResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        updateCensusInFlight_ = false;
        if (!result.error.isEmpty()) return;
        static_cast<void>(BackgroundUpdateStateStore::syncAvailableUpdates(result.projects));
        refreshTray();
    });
    const auto connection = ConnectionConfig::load();
    watcher->setFuture(QtConcurrent::run([connection] {
        UpdateCensusResult result;
        result.projects = LibraryClient(connection).list(&result.error);
        return result;
    }));
}

void ApplicationSession::runBackgroundCheck() {
    if (updateRequestInFlight_ || !activeUpdateJobs_.isEmpty()) return;
    updateRequestInFlight_ = true;
    if (!trayAnimation_.isActive()) trayAnimation_.start();
    if (tray_ != nullptr) {
        tray_->showMessage(QStringLiteral("PacSmith"),
                           QStringLiteral("Update check requested from the library daemon."));
    }
    refreshTray();
    auto *watcher = new QFutureWatcher<UpdateRequestResult>(this);
    connect(watcher, &QFutureWatcher<UpdateRequestResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        updateRequestInFlight_ = false;
        const bool succeeded = result.job && result.job->status == QStringLiteral("succeeded");
        if (!succeeded && tray_ != nullptr) {
            auto message = !result.error.isEmpty() ? result.error
                : result.job && !result.job->error.isEmpty() ? result.job->error
                                                            : QStringLiteral("Update check failed");
            tray_->showMessage(QStringLiteral("PacSmith"), message);
        } else if (succeeded && tray_ != nullptr) {
            tray_->showMessage(QStringLiteral("PacSmith"), updateCompletionMessage(*result.job));
        }
        if (succeeded) refreshUpdateCensus();
        if (window_ != nullptr) window_->reloadVisibleProjects();
        if (activeBuildJobs_.isEmpty() && activeUpdateJobs_.isEmpty() &&
            activePreparationJobs_.isEmpty()) {
            trayAnimation_.stop();
        }
        refreshTray();
    });
    const auto connection = ConnectionConfig::load();
    watcher->setFuture(QtConcurrent::run([connection] {
        UpdateRequestResult result;
        LibraryClient client(connection);
        const auto started = client.startUpdateCheck({}, false, &result.error);
        if (!started) return result;
        result.job = client.waitForJob(started->id, &result.error);
        return result;
    }));
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
