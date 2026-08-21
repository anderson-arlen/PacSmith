#include "gui/application_session.hpp"
#include "gui/gui_instance.hpp"
#include "core/app_settings.hpp"
#include "core/credential_store.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QSystemTrayIcon>

#include <string_view>

#include <unistd.h>

namespace {

QIcon applicationIcon() {
    return QIcon(QStringLiteral(":/pacsmith/icons/pacsmith.png"));
}

bool argvHasOption(const int argc, char *argv[], const std::string_view option) {
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && argv[index] == option) return true;
    }
    return false;
}

QString pacsmithCliPath() {
    auto program = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("pacsmith"));
    if (!QFileInfo::exists(program)) program = QStandardPaths::findExecutable(QStringLiteral("pacsmith"));
    return program;
}

int runStandaloneCheck() {
    if (pacsmith::gui::GuiInstanceServer::requestCheck()) return 0;
    const auto program = pacsmithCliPath();
    if (program.isEmpty()) return 1;
    return QProcess::execute(program, {QStringLiteral("check"), QStringLiteral("--all")}) == 0 ? 0 : 1;
}

}

int main(int argc, char *argv[]) {
    if (argvHasOption(argc, argv, "--check")) {
        QCoreApplication application(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("pacsmith-gui"));
        QCoreApplication::setApplicationVersion(QStringLiteral(PACSMITH_VERSION));
        QCoreApplication::setOrganizationName(QStringLiteral("PacSmith"));
        return runStandaloneCheck();
    }

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
                                  QStringLiteral("Run PacSmith in the background with an update-status tray icon"));
    parser.addOption(trayOption);
    QCommandLineOption checkOption(QStringLiteral("check"),
                                   QStringLiteral("Ask the running PacSmith session to check for updates, or run a CLI check if none is running"));
    parser.addOption(checkOption);
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

    const bool startHidden = parser.isSet(trayOption);
    QString importPath;
    if (parser.isSet(importOption)) importPath = parser.value(importOption);
    else if (!parser.positionalArguments().isEmpty()) importPath = parser.positionalArguments().first();

    if (startHidden) {
        if (pacsmith::gui::GuiInstanceServer::requestTray()) return 0;
    } else if (pacsmith::gui::GuiInstanceServer::activateExisting(importPath)) {
        return 0;
    }

    pacsmith::AppSettingsStore settingsStore;
    const bool hideWindow = startHidden && QSystemTrayIcon::isSystemTrayAvailable();
    if (startHidden && !hideWindow) {
        QMessageBox::warning(nullptr, QStringLiteral("PacSmith"),
                             QStringLiteral("No system tray is available, so PacSmith will open in a window."));
    }

    pacsmith::CredentialStore credentials(settingsStore.ageSecretsPath());
    pacsmith::gui::ApplicationSession session(settingsStore, credentials);
    static_cast<void>(session.listen());
    session.start(hideWindow, importPath);
    return application.exec();
}
