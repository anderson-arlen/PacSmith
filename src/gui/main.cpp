#include "gui/main_window.hpp"
#include "core/app_settings.hpp"
#include "core/background_updates.hpp"
#include "core/project_store.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QMessageBox>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QStyle>
#include <QTimer>

#include <algorithm>
#include <memory>

#include <unistd.h>

namespace {

QIcon applicationIcon() {
    return QIcon(QStringLiteral(":/pacsmith/icons/pacsmith.png"));
}

QIcon trayStatusIcon(const QApplication &application, int availableUpdates) {
    QIcon source(QStringLiteral(":/pacsmith/icons/pacsmith-tray.png"));
    QPixmap mask = source.pixmap(QSize(32, 32));
    if (mask.isNull()) mask = application.style()->standardIcon(QStyle::SP_ComputerIcon).pixmap(32, 32);
    QPixmap pixmap(mask.size());
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawPixmap(0, 0, mask);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), application.palette().color(QPalette::WindowText));
    if (availableUpdates > 0) {
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setBrush(QColor(210, 50, 50));
        painter.setPen(Qt::white);
        painter.drawEllipse(QRect(17, 0, 15, 15));
        auto font = painter.font();
        font.setBold(true);
        font.setPixelSize(10);
        painter.setFont(font);
        painter.drawText(QRect(17, 0, 15, 15), Qt::AlignCenter,
                         availableUpdates > 9 ? QStringLiteral("9+")
                                              : QString::number(availableUpdates));
    }
    return QIcon(pixmap);
}

}

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("pacsmith-gui"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PACSMITH_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("PacSmith"));
    QGuiApplication::setDesktopFileName(QStringLiteral("pacsmith"));
    application.setWindowIcon(applicationIcon());

    if (geteuid() == 0) {
        QMessageBox::critical(nullptr, QStringLiteral("PacSmith"),
                              QStringLiteral("Do not run PacSmith's GUI as root. Analysis and builds are unprivileged; only an explicit package installation uses pkexec."));
        return 1;
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native Arch package workbench for vendor artifacts"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption importOption(QStringList{QStringLiteral("i"), QStringLiteral("import")},
                                    QStringLiteral("Import a vendor artifact or GitHub release URL"), QStringLiteral("source"));
    parser.addOption(importOption);
    QCommandLineOption trayOption(QStringLiteral("tray"),
                                  QStringLiteral("Run PacSmith's update-status tray helper"));
    parser.addOption(trayOption);
    parser.addPositionalArgument(QStringLiteral("source"),
                                 QStringLiteral("Artifact path or GitHub release URL"),
                                 QStringLiteral("[source]"));
    auto arguments = QCoreApplication::arguments();
    if (!arguments.isEmpty()) {
        const QString &last = arguments.constLast();
        if (last == QStringLiteral("--import") || last == QStringLiteral("-i")) {
            arguments.removeLast();
        }
    }
    parser.process(arguments);

    if (parser.isSet(trayOption)) {
        const auto settings = pacsmith::AppSettingsStore{}.load();
        if (!settings.updates.enabled || settings.updates.trayMode == pacsmith::TrayMode::Disabled ||
            !pacsmith::BackgroundUpdateManager::unitInstalled()) return 0;
        if (!QSystemTrayIcon::isSystemTrayAvailable()) return 1;
        QSystemTrayIcon tray;
        QMenu menu;
        auto *openAction = menu.addAction(QStringLiteral("Open PacSmith"));
        auto *checkAction = menu.addAction(QStringLiteral("Check for Updates Now"));
        menu.addSeparator();
        auto *quitAction = menu.addAction(QStringLiteral("Quit Tray Icon"));
        tray.setContextMenu(&menu);
        std::unique_ptr<pacsmith::gui::MainWindow> window;
        const auto openWindow = [&] {
            if (!window) {
                window = std::make_unique<pacsmith::gui::MainWindow>();
                window->resize(1180, 760);
            }
            window->show();
            window->raise();
            window->activateWindow();
        };
        QObject::connect(openAction, &QAction::triggered, &application, openWindow);
        QObject::connect(&tray, &QSystemTrayIcon::activated, &application,
                         [&](const QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) openWindow();
        });
        QObject::connect(checkAction, &QAction::triggered, &application, [&] {
            QString error;
            if (!pacsmith::BackgroundUpdateManager::runNow(&error)) tray.showMessage(QStringLiteral("PacSmith"), error);
        });
        QObject::connect(quitAction, &QAction::triggered, &application, &QApplication::quit);
        QTimer refreshTimer;
        refreshTimer.setInterval(5000);
        const auto refresh = [&] {
            const auto current = pacsmith::BackgroundUpdateStateStore::load();
            tray.setIcon(trayStatusIcon(application, current.availableUpdates));
            tray.setToolTip(current.checking ? QStringLiteral("PacSmith is checking for updates")
                : current.availableUpdates > 0 ? QStringLiteral("PacSmith: %1 update(s) available").arg(current.availableUpdates)
                                               : QStringLiteral("PacSmith: packages are current"));
            const bool visible = settings.updates.trayMode == pacsmith::TrayMode::Always ||
                                 current.checking || current.availableUpdates > 0;
            tray.setVisible(visible);
        };
        QObject::connect(&refreshTimer, &QTimer::timeout, &application, refresh);
        refreshTimer.start();
        refresh();
        return application.exec();
    }

    pacsmith::gui::MainWindow window;
    window.resize(1180, 760);
    window.show();
    QString importPath;
    if (parser.isSet(importOption)) importPath = parser.value(importOption);
    else if (!parser.positionalArguments().isEmpty()) importPath = parser.positionalArguments().first();
    if (!importPath.isEmpty()) {
        const QUrl url(importPath);
        window.importPackage(url.isValid() && url.scheme().startsWith(QStringLiteral("http"))
                                 ? importPath
                                 : QFileInfo(importPath).absoluteFilePath());
    } else {
        QTimer::singleShot(0, &window, [&window] {
            pacsmith::AppSettingsStore settingsStore;
            auto settings = settingsStore.load();
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
                        &window, QStringLiteral("Open vendor packages with PacSmith"),
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
                            &window, QStringLiteral("Could not set package associations"),
                            QStringLiteral("Install PacSmith for the current user first, then choose PacSmith from your desktop's Open With dialog."));
                    }
                }
            }
            if (!settings.selfTrackingPrompted) {
                settings.selfTrackingPrompted = true;
                changed = true;
                const pacsmith::ProjectStore store;
                const bool emptyWorkbench = store.list().isEmpty();
                const auto message = emptyWorkbench
                    ? QStringLiteral("Set PacSmith up as the first project in this workbench? PacSmith will inspect the official x86_64 release artifact from GitHub and prepare a normal pacman package. It will remain marked Not installed until that package is actually installed through pacman.")
                    : QStringLiteral("Track PacSmith's own official GitHub releases as a project too? It will remain marked Not installed unless a PacSmith-built package is actually installed through pacman.");
                if (QMessageBox::question(&window, QStringLiteral("Track PacSmith updates"), message) ==
                    QMessageBox::Yes) {
                    if (changed) {
                        QString error;
                        if (!settingsStore.save(settings, &error)) {
                            QMessageBox::warning(&window, QStringLiteral("Could not save onboarding settings"), error);
                        }
                        changed = false;
                    }
                    window.importPackage(QStringLiteral("https://github.com/anderson-arlen/pacsmith"));
                }
            }
            if (changed) {
                QString error;
                if (!settingsStore.save(settings, &error)) {
                    QMessageBox::warning(&window, QStringLiteral("Could not save onboarding settings"), error);
                }
            }
        });
    }
    return application.exec();
}
