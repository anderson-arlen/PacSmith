#pragma once

#include "gui/gui_instance.hpp"

#include <QMenu>
#include <QHash>
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>
#include <QTimer>

#include <memory>

namespace pacsmith {

class AppSettingsStore;
class LibraryEventStream;
struct ServerEvent;

}

namespace pacsmith::gui {

class MainWindow;

class ApplicationSession final : public QObject {
    Q_OBJECT
public:
    explicit ApplicationSession(AppSettingsStore &settingsStore, QObject *parent = nullptr);
    ~ApplicationSession() override;

    [[nodiscard]] bool listen();
    void start(bool startHidden, const QString &importPath);
    void showWorkbench(const QString &importPath = {});
    void ensureTray();
    void runBackgroundCheck();

private:
    [[nodiscard]] bool trayWanted() const;
    void setupTray();
    void refreshTray();
    void refreshUpdateCensus();
    void handleServerEvent(const pacsmith::ServerEvent &event);
    void quitSession();
    void maybeOnboard();

    AppSettingsStore &settingsStore_;
    GuiInstanceServer server_;
    std::unique_ptr<QSystemTrayIcon> tray_;
    std::unique_ptr<QMenu> trayMenu_;
    std::unique_ptr<MainWindow> window_;
    QTimer trayRefresh_;
    QTimer trayAnimation_;
    bool updateRequestInFlight_{false};
    int lastTrayBadge_{-1};
    QRgb lastTrayColor_{0};
    bool lastTrayChecking_{false};
    bool lastTrayPreparing_{false};
    int lastTrayActivityFrame_{-1};
    int trayActivityFrame_{0};
    QHash<QString, QString> activeBuildJobs_;
    QHash<QString, QString> activeUpdateJobs_;
    QHash<QString, QString> activePreparationJobs_;
    LibraryEventStream *libraryEventStream_{nullptr};
    bool updateCensusInFlight_{false};
    bool startHidden_{false};
    bool onboardingStarted_{false};
};

} // namespace pacsmith::gui
