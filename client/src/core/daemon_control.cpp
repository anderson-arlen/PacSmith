#include "core/daemon_control.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>

#include <unistd.h>

namespace pacsmith {
namespace {

QProcessEnvironment userServiceEnvironment() {
    auto environment = QProcessEnvironment::systemEnvironment();
    auto runtime = environment.value(QStringLiteral("XDG_RUNTIME_DIR"));
    if (!QDir::isAbsolutePath(runtime)) {
        runtime = QStringLiteral("/run/user/%1").arg(::getuid());
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
    }
    if (environment.value(QStringLiteral("DBUS_SESSION_BUS_ADDRESS")).isEmpty()) {
        const auto bus = QDir(runtime).filePath(QStringLiteral("bus"));
        if (QFileInfo::exists(bus)) {
            // MCP hosts often sanitize session variables while leaving the user's runtime
            // sockets accessible, so systemctl needs the canonical bus address restored.
            environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                               QStringLiteral("unix:path=%1").arg(bus));
        }
    }
    return environment;
}

QString daemonStartFailure(const QString &detail) {
    return QStringLiteral(
               "Could not start pacsmithd.service: %1\n"
               "Run 'systemctl --user start pacsmithd.service', then retry. If it still fails, "
               "inspect 'journalctl --user -u pacsmithd.service'.")
        .arg(detail);
}

bool runSystemctl(const QStringList &arguments, QString *error, QByteArray *standardOutput = nullptr,
                  QByteArray *standardError = nullptr) {
    if (!QFileInfo::exists(QStringLiteral("/usr/bin/systemctl"))) {
        if (error != nullptr) {
            *error = QStringLiteral("systemctl is required to manage pacsmithd.service");
        }
        return false;
    }
    QProcess process;
    process.setProgram(QStringLiteral("/usr/bin/systemctl"));
    process.setArguments(QStringList{QStringLiteral("--user")} + arguments);
    process.setProcessEnvironment(userServiceEnvironment());
    process.start();
    if (!process.waitForStarted(3000) || !process.waitForFinished(20000)) {
        if (error != nullptr) *error = process.errorString();
        return false;
    }
    if (standardOutput != nullptr) *standardOutput = process.readAllStandardOutput();
    if (standardError != nullptr) *standardError = process.readAllStandardError();
    if (process.exitCode() != 0) {
        if (error != nullptr) {
            auto text = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            if (text.isEmpty() && standardError != nullptr) {
                text = QString::fromLocal8Bit(*standardError).trimmed();
            }
            if (text.isEmpty()) {
                *error = QStringLiteral("systemctl --user failed with exit %1").arg(process.exitCode());
            } else {
                *error = text;
            }
        }
        return false;
    }
    return true;
}

QString findUnitFile() {
    const auto bin = QCoreApplication::applicationDirPath();
    QStringList candidates{
        QDir(bin).absoluteFilePath(QStringLiteral("server/pacsmithd.service")),
        QDir(bin).absoluteFilePath(QStringLiteral("../share/systemd/user/pacsmithd.service")),
        QStringLiteral("/usr/lib/systemd/user/pacsmithd.service"),
        QStringLiteral("/usr/local/lib/systemd/user/pacsmithd.service")};
    const auto data = qEnvironmentVariable("XDG_DATA_HOME");
    if (!data.isEmpty() && QDir::isAbsolutePath(data)) {
        candidates.prepend(QDir(data).filePath(QStringLiteral("systemd/user/pacsmithd.service")));
    } else {
        candidates.prepend(QDir::home().filePath(QStringLiteral(".local/share/systemd/user/pacsmithd.service")));
    }
    for (const auto &path : candidates) {
        const QFileInfo info(path);
        if (info.exists() && info.isFile()) return info.canonicalFilePath();
    }
    return {};
}

bool unitInstalled() {
    QByteArray output;
    return runSystemctl({QStringLiteral("cat"), QStringLiteral("pacsmithd.service")}, nullptr, &output) &&
           !output.isEmpty();
}

void terminateLeftoverDaemons() {
    if (!QFileInfo::exists(QStringLiteral("/usr/bin/pkill"))) return;
    QProcess process;
    process.setProgram(QStringLiteral("/usr/bin/pkill"));
    process.setArguments({QStringLiteral("-TERM"), QStringLiteral("-u"),
                          QString::number(::getuid()), QStringLiteral("-x"),
                          QStringLiteral("pacsmithd")});
    process.start();
    static_cast<void>(process.waitForFinished(3000));
}

} // namespace

bool waitForLocalSocket(const QString &socketPath, const int timeoutMs, QString *error) {
    const auto deadline = timeoutMs > 0 ? timeoutMs : 15000;
    const auto slice = 100;
    for (int waited = 0; waited < deadline; waited += slice) {
        if (QFileInfo::exists(socketPath)) return true;
        QThread::msleep(static_cast<unsigned long>(slice));
    }
    if (error != nullptr) {
        *error = QStringLiteral("pacsmithd did not create %1").arg(socketPath);
    }
    return false;
}

bool startLocalLibraryDaemon(QString *error) {
    const auto socket = ConnectionConfig::localDefault().socketPath;
    if (QFileInfo::exists(socket)) {
        static_cast<void>(runSystemctl({QStringLiteral("enable"), QStringLiteral("pacsmithd.service")},
                                       nullptr));
        return true;
    }
    static_cast<void>(runSystemctl({QStringLiteral("daemon-reload")}, nullptr));
    QString enableError;
    if (unitInstalled()) {
        if (!runSystemctl({QStringLiteral("enable"), QStringLiteral("--now"),
                           QStringLiteral("pacsmithd.service")},
                          &enableError)) {
            if (error != nullptr) *error = daemonStartFailure(enableError);
            return false;
        }
    } else {
        const auto unit = findUnitFile();
        if (unit.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral(
                    "pacsmithd.service is not installed. Install PacSmith, then try again.");
            }
            return false;
        }
        if (!runSystemctl({QStringLiteral("enable"), QStringLiteral("--now"), unit}, &enableError)) {
            if (error != nullptr) *error = daemonStartFailure(enableError);
            return false;
        }
        static_cast<void>(runSystemctl({QStringLiteral("daemon-reload")}, nullptr));
        static_cast<void>(runSystemctl({QStringLiteral("start"), QStringLiteral("pacsmithd.service")},
                                       nullptr));
    }
    return waitForLocalSocket(socket, 15000, error);
}

bool stopLocalLibraryDaemon(QString *error) {
    static_cast<void>(runSystemctl({QStringLiteral("disable"), QStringLiteral("--now"),
                                    QStringLiteral("pacsmithd.service")},
                                   nullptr));
    static_cast<void>(runSystemctl({QStringLiteral("stop"), QStringLiteral("pacsmithd.service")},
                                   nullptr));
    terminateLeftoverDaemons();
    if (error != nullptr) error->clear();
    return true;
}

bool applyLibraryRuntime(const ConnectionConfig &config, QString *error) {
    if (config.mode == ConnectionConfig::Mode::Remote) return stopLocalLibraryDaemon(error);
    return startLocalLibraryDaemon(error);
}

} // namespace pacsmith
