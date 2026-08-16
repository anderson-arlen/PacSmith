#include "core/background_updates.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace pacsmith {
namespace {

QString stateDirectory() {
    const auto xdg = qEnvironmentVariable("XDG_STATE_HOME");
    if (!xdg.isEmpty() && QDir::isAbsolutePath(xdg)) return QDir(xdg).filePath(QStringLiteral("pacsmith"));
    return QDir::home().filePath(QStringLiteral(".local/state/pacsmith"));
}

bool runSystemctl(const QStringList &arguments, QString *error, QByteArray *standardOutput = nullptr) {
    QProcess process;
    process.setProgram(QStringLiteral("/usr/bin/systemctl"));
    process.setArguments(QStringList{QStringLiteral("--user")} + arguments);
    process.start();
    if (!process.waitForStarted(3000) || !process.waitForFinished(15000)) {
        if (error != nullptr) *error = process.errorString();
        return false;
    }
    if (standardOutput != nullptr) *standardOutput = process.readAllStandardOutput();
    if (process.exitCode() != 0) {
        if (error != nullptr) {
            *error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            if (error->isEmpty()) *error = QStringLiteral("systemctl --user failed with exit %1").arg(process.exitCode());
        }
        return false;
    }
    return true;
}

} // namespace

QString BackgroundUpdateStateStore::defaultPath() {
    return QDir(stateDirectory()).filePath(QStringLiteral("update-state.json"));
}

BackgroundUpdateState BackgroundUpdateStateStore::load(QString *error) {
    BackgroundUpdateState result;
    QFile file(defaultPath());
    if (!file.exists()) return result;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return result;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = parseError.errorString();
        return result;
    }
    const auto object = document.object();
    result.checking = object.value(QStringLiteral("checking")).toBool();
    result.availableUpdates = object.value(QStringLiteral("availableUpdates")).toInt();
    result.failedChecks = object.value(QStringLiteral("failedChecks")).toInt();
    result.lastRun = QDateTime::fromString(object.value(QStringLiteral("lastRun")).toString(), Qt::ISODateWithMs);
    for (const auto &entry : object.value(QStringLiteral("projectsWithUpdates")).toArray()) {
        result.projectsWithUpdates.append(entry.toString());
    }
    result.message = object.value(QStringLiteral("message")).toString();
    return result;
}

bool BackgroundUpdateStateStore::save(const BackgroundUpdateState &state, QString *error) {
    if (!QDir{}.mkpath(stateDirectory())) {
        if (error != nullptr) *error = QStringLiteral("Could not create PacSmith state directory");
        return false;
    }
    QJsonArray projects;
    for (const auto &project : state.projectsWithUpdates) projects.append(project);
    const QJsonObject object{{QStringLiteral("formatVersion"), 1},
                             {QStringLiteral("checking"), state.checking},
                             {QStringLiteral("availableUpdates"), state.availableUpdates},
                             {QStringLiteral("failedChecks"), state.failedChecks},
                             {QStringLiteral("lastRun"), state.lastRun.toString(Qt::ISODateWithMs)},
                             {QStringLiteral("projectsWithUpdates"), projects},
                             {QStringLiteral("message"), state.message}};
    QSaveFile file(defaultPath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    return true;
}

QString BackgroundUpdateManager::calendar(const BackgroundUpdateSettings &settings) {
    const auto time = settings.localTime.isValid() ? settings.localTime : QTime(2, 0);
    if (settings.daily) return QStringLiteral("*-*-* %1:00").arg(time.toString(QStringLiteral("HH:mm")));
    static const QStringList days{QStringLiteral("Mon"), QStringLiteral("Tue"),
                                  QStringLiteral("Wed"), QStringLiteral("Thu"),
                                  QStringLiteral("Fri"), QStringLiteral("Sat"),
                                  QStringLiteral("Sun")};
    const auto index = std::clamp(settings.weekDay, 1, 7) - 1;
    return QStringLiteral("%1 *-*-* %2:00").arg(days.at(index), time.toString(QStringLiteral("HH:mm")));
}

QString BackgroundUpdateManager::timerUnitPath() {
    const auto locations = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const auto &location : locations) {
        const auto candidate = QDir(location).filePath(QStringLiteral("systemd/user/pacsmith-update.timer"));
        if (QFile::exists(candidate)) return candidate;
    }
    return {};
}

QString BackgroundUpdateManager::scheduleDropInPath() {
    const auto config = qEnvironmentVariable("XDG_CONFIG_HOME");
    const auto base = !config.isEmpty() && QDir::isAbsolutePath(config)
        ? config : QDir::home().filePath(QStringLiteral(".config"));
    return QDir(base).filePath(QStringLiteral("systemd/user/pacsmith-update.timer.d/schedule.conf"));
}

bool BackgroundUpdateManager::unitInstalled() { return !timerUnitPath().isEmpty(); }

bool BackgroundUpdateManager::isEnabled(QString *error) {
    QByteArray output;
    return runSystemctl({QStringLiteral("is-enabled"), QStringLiteral("pacsmith-update.timer")}, error, &output) &&
           output.trimmed() == QByteArrayLiteral("enabled");
}

bool BackgroundUpdateManager::apply(const BackgroundUpdateSettings &settings, QString *error) {
    if (!unitInstalled()) {
        if (error != nullptr) *error = QStringLiteral("PacSmith's systemd user units are not installed. Run 'make install' first.");
        return false;
    }
    const auto dropIn = scheduleDropInPath();
    if (!QDir{}.mkpath(QFileInfo(dropIn).absolutePath())) {
        if (error != nullptr) *error = QStringLiteral("Could not create the systemd user drop-in directory");
        return false;
    }
    QSaveFile file(dropIn);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    const auto contents = QStringLiteral("[Timer]\nOnCalendar=\nOnCalendar=%1\nPersistent=true\nRandomizedDelaySec=0\n")
                              .arg(calendar(settings)).toUtf8();
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    if (!runSystemctl({QStringLiteral("daemon-reload")}, error)) return false;
    const auto timerReady = settings.enabled
        ? runSystemctl({QStringLiteral("enable"), QStringLiteral("--now"), QStringLiteral("pacsmith-update.timer")}, error)
        : runSystemctl({QStringLiteral("disable"), QStringLiteral("--now"), QStringLiteral("pacsmith-update.timer")}, error);
    if (!timerReady) return false;
    const auto showTray = settings.enabled && settings.trayMode != TrayMode::Disabled;
    return showTray
        ? runSystemctl({QStringLiteral("enable"), QStringLiteral("--now"), QStringLiteral("pacsmith-tray.service")}, error)
        : runSystemctl({QStringLiteral("disable"), QStringLiteral("--now"), QStringLiteral("pacsmith-tray.service")}, error);
}

bool BackgroundUpdateManager::runNow(QString *error) {
    if (!unitInstalled()) {
        if (error != nullptr) *error = QStringLiteral("PacSmith's systemd user service is not installed");
        return false;
    }
    return runSystemctl({QStringLiteral("start"), QStringLiteral("pacsmith-update.service")}, error);
}

} // namespace pacsmith
