#pragma once

#include "gui/gui_instance.hpp"

#include <QMenu>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QSystemTrayIcon>
#include <QTimer>

#include <memory>

namespace pacsmith {

class AppSettingsStore;
class CredentialStore;
class LibraryEventStream;
struct ServerEvent;

}

namespace pacsmith::gui {

class MainWindow;

class ApplicationSession final : public QObject {
    Q_OBJECT
public:
    ApplicationSession(AppSettingsStore &settingsStore, CredentialStore &credentials,
                       QObject *parent = nullptr);
    ~ApplicationSession() override;

    [[nodiscard]] bool listen();
    void start(bool startHidden, const QString &importPath);
    void showWorkbench(const QString &importPath = {});
    void ensureTray();
    void runBackgroundCheck(bool onlyIfOverdue = false);

private:
    enum class CheckKind { Manual, IfOverdue, Scheduled };

    [[nodiscard]] bool trayWanted() const;
    void setupTray();
    void refreshTray();
    void scheduleNextCheck();
    void runBackgroundCheck(CheckKind kind);
    void handleServerEvent(const pacsmith::ServerEvent &event);
    void quitSession();
    void maybeOnboard();

    AppSettingsStore &settingsStore_;
    CredentialStore &credentials_;
    GuiInstanceServer server_;
    std::unique_ptr<QSystemTrayIcon> tray_;
    std::unique_ptr<QMenu> trayMenu_;
    std::unique_ptr<MainWindow> window_;
    QTimer trayRefresh_;
    QTimer trayAnimation_;
    QTimer checkTimer_;
    QProcess *checkProcess_{nullptr};
    int lastTrayBadge_{-1};
    QRgb lastTrayColor_{0};
    bool lastTrayChecking_{false};
    bool lastTrayPreparing_{false};
    int lastTrayActivityFrame_{-1};
    int trayActivityFrame_{0};
    QHash<QString, QString> activeBuildJobs_;
    LibraryEventStream *libraryEventStream_{nullptr};
    bool startHidden_{false};
    bool onboardingStarted_{false};
};

} // namespace pacsmith::gui
