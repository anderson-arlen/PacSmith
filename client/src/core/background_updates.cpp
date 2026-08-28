#include "core/background_updates.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QSaveFile>
#include <QTime>
#include <QTimeZone>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <unistd.h>

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

quint64 processStartTicks(const qint64 processId) {
    QFile file(QStringLiteral("/proc/%1/stat").arg(processId));
    if (!file.open(QIODevice::ReadOnly)) return 0;
    const auto stat = file.readAll();
    const auto commandEnd = stat.lastIndexOf(") ");
    if (commandEnd < 0) return 0;
    const auto fields = stat.mid(commandEnd + 2).split(' ');
    if (fields.size() <= 19) return 0;
    bool ok = false;
    const auto ticks = fields.at(19).toULongLong(&ok);
    return ok ? ticks : 0;
}

bool processOwnsActivity(const BackgroundUpdateState &state) {
    if (state.activityProcessId <= 0 || state.activityProcessStartTicks == 0) return false;
    if (::kill(static_cast<pid_t>(state.activityProcessId), 0) != 0 && errno != EPERM) return false;
    return processStartTicks(state.activityProcessId) == state.activityProcessStartTicks;
}

void discardOrphanedActivity(BackgroundUpdateState &state) {
    const bool hasActivity = state.checking || !state.preparingProjectId.isEmpty();
    if (!hasActivity || processOwnsActivity(state)) return;
    state.checking = false;
    BackgroundUpdateStateStore::clearActivityOwner(state);
    state.checkingProjectId.clear();
    state.checkingProjectName.clear();
    state.preparingProjectId.clear();
    state.preparingProjectName.clear();
    state.preparationPhase.clear();
    state.preparationBytesReceived = 0;
    state.preparationBytesTotal = -1;
    state.message = state.availableUpdates > 0
        ? QStringLiteral("%1 update(s) available").arg(state.availableUpdates)
        : state.failedChecks > 0 ? QStringLiteral("Update checks completed with failures")
                                 : QStringLiteral("All eligible project trackers are current");
}

} // namespace

void BackgroundUpdateStateStore::claimActivity(BackgroundUpdateState &state) {
    state.activityProcessId = static_cast<qint64>(::getpid());
    state.activityProcessStartTicks = processStartTicks(state.activityProcessId);
}

void BackgroundUpdateStateStore::clearActivityOwner(BackgroundUpdateState &state) {
    state.activityProcessId = 0;
    state.activityProcessStartTicks = 0;
}

void applyAvailableUpdateCensus(BackgroundUpdateState &state, const QList<Project> &projects) {
    state.projectsWithUpdates.clear();
    for (const auto &project : projects) {
        if (project.hasAvailableUpdate()) state.projectsWithUpdates.append(project.id);
    }
    state.availableUpdates = static_cast<int>(state.projectsWithUpdates.size());
}

int availableUpdateCount(const QList<Project> &projects) {
    BackgroundUpdateState state;
    applyAvailableUpdateCensus(state, projects);
    return state.availableUpdates;
}

QString runningGuiSocketName() {
    return QStringLiteral("pacsmith-gui-%1").arg(::getuid());
}

bool notifyRunningGui(const QString &command, const QString &importPath) {
    QLocalSocket socket;
    socket.connectToServer(runningGuiSocketName());
    if (!socket.waitForConnected(250)) return false;
    QJsonObject object{{QStringLiteral("command"), command}};
    if (!importPath.isEmpty()) object.insert(QStringLiteral("path"), importPath);
    auto encoded = QJsonDocument(object).toJson(QJsonDocument::Compact);
    encoded.append('\n');
    if (socket.write(encoded) != encoded.size() || !socket.waitForBytesWritten(1000)) return false;
    socket.waitForDisconnected(250);
    return true;
}

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
    result.activityProcessId = object.value(QStringLiteral("activityProcessId")).toInteger();
    result.activityProcessStartTicks =
        object.value(QStringLiteral("activityProcessStartTicks")).toVariant().toULongLong();
    result.checkingProjectId = object.value(QStringLiteral("checkingProjectId")).toString();
    result.checkingProjectName = object.value(QStringLiteral("checkingProjectName")).toString();
    result.preparingProjectId = object.value(QStringLiteral("preparingProjectId")).toString();
    result.preparingProjectName = object.value(QStringLiteral("preparingProjectName")).toString();
    result.preparationPhase = object.value(QStringLiteral("preparationPhase")).toString();
    result.preparationBytesReceived =
        object.value(QStringLiteral("preparationBytesReceived")).toInteger();
    result.preparationBytesTotal =
        object.value(QStringLiteral("preparationBytesTotal")).toInteger(-1);
    result.availableUpdates = object.value(QStringLiteral("availableUpdates")).toInt();
    result.failedChecks = object.value(QStringLiteral("failedChecks")).toInt();
    result.lastRun = QDateTime::fromString(object.value(QStringLiteral("lastRun")).toString(), Qt::ISODateWithMs);
    for (const auto &entry : object.value(QStringLiteral("projectsWithUpdates")).toArray()) {
        result.projectsWithUpdates.append(entry.toString());
    }
    result.message = object.value(QStringLiteral("message")).toString();
    discardOrphanedActivity(result);
    return result;
}

bool BackgroundUpdateStateStore::save(const BackgroundUpdateState &state, QString *error) {
    if (!QDir{}.mkpath(stateDirectory())) {
        if (error != nullptr) *error = QStringLiteral("Could not create PacSmith state directory");
        return false;
    }
    QJsonArray projects;
    for (const auto &project : state.projectsWithUpdates) projects.append(project);
    const QJsonObject object{{QStringLiteral("formatVersion"), 2},
                             {QStringLiteral("checking"), state.checking},
                             {QStringLiteral("activityProcessId"), state.activityProcessId},
                             {QStringLiteral("activityProcessStartTicks"),
                              QString::number(state.activityProcessStartTicks)},
                             {QStringLiteral("checkingProjectId"), state.checkingProjectId},
                             {QStringLiteral("checkingProjectName"), state.checkingProjectName},
                             {QStringLiteral("preparingProjectId"), state.preparingProjectId},
                             {QStringLiteral("preparingProjectName"), state.preparingProjectName},
                             {QStringLiteral("preparationPhase"), state.preparationPhase},
                             {QStringLiteral("preparationBytesReceived"),
                              state.preparationBytesReceived},
                             {QStringLiteral("preparationBytesTotal"), state.preparationBytesTotal},
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

bool BackgroundUpdateStateStore::syncAvailableUpdates(const QList<Project> &projects, QString *error) {
    auto state = load(error);
    applyAvailableUpdateCensus(state, projects);
    if (!state.checking && state.preparingProjectId.isEmpty()) {
        state.message = state.availableUpdates > 0
            ? QStringLiteral("%1 update(s) available").arg(state.availableUpdates)
            : state.failedChecks > 0 ? QStringLiteral("Update checks completed with failures")
                                     : QStringLiteral("All eligible project trackers are current");
    }
    return save(state, error);
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

QDateTime BackgroundUpdateManager::lastScheduledOccurrence(const BackgroundUpdateSettings &settings,
                                                           const QDateTime &now) {
    const auto time = settings.localTime.isValid() ? settings.localTime : QTime(2, 0);
    const auto timeZone = now.timeZone().isValid() ? now.timeZone() : QTimeZone::systemTimeZone();
    const auto localNow = now.toTimeZone(timeZone);
    if (settings.daily) {
        QDateTime candidate(localNow.date(), time, timeZone);
        if (candidate > localNow) candidate = candidate.addDays(-1);
        return candidate;
    }
    const int targetDay = std::clamp(settings.weekDay, 1, 7);
    int delta = localNow.date().dayOfWeek() - targetDay;
    if (delta < 0) delta += 7;
    QDateTime candidate(localNow.date().addDays(-delta), time, timeZone);
    if (candidate > localNow) candidate = candidate.addDays(-7);
    return candidate;
}

QDateTime BackgroundUpdateManager::nextScheduledOccurrence(const BackgroundUpdateSettings &settings,
                                                           const QDateTime &now) {
    const auto last = lastScheduledOccurrence(settings, now);
    return settings.daily ? last.addDays(1) : last.addDays(7);
}

bool BackgroundUpdateManager::isOverdue(const BackgroundUpdateSettings &settings,
                                        const QDateTime &lastRun, const QDateTime &now) {
    if (!settings.enabled) return false;
    if (!lastRun.isValid()) return true;
    return lastRun.toUTC() < lastScheduledOccurrence(settings, now).toUTC();
}

QString BackgroundUpdateManager::autostartPath() {
    const auto config = qEnvironmentVariable("XDG_CONFIG_HOME");
    const auto base = !config.isEmpty() && QDir::isAbsolutePath(config)
        ? config : QDir::home().filePath(QStringLiteral(".config"));
    return QDir(base).filePath(QStringLiteral("autostart/pacsmith.desktop"));
}

namespace {

QString quotedDesktopExec(const QString &path) {
    if (path.isEmpty()) return {};
    const bool needsQuotes = path.contains(QLatin1Char(' ')) || path.contains(QLatin1Char('\t')) ||
                             path.contains(QLatin1Char('"')) || path.contains(QLatin1Char('\\'));
    if (!needsQuotes) return path;
    auto escaped = path;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(escaped);
}

QString legacyScheduleDropInPath() {
    const auto config = qEnvironmentVariable("XDG_CONFIG_HOME");
    const auto base = !config.isEmpty() && QDir::isAbsolutePath(config)
        ? config : QDir::home().filePath(QStringLiteral(".config"));
    return QDir(base).filePath(QStringLiteral("systemd/user/pacsmith-update.timer.d/schedule.conf"));
}

void disableLegacySystemdUnits() {
    static bool completed = false;
    if (completed) return;
    completed = true;
    if (!QFileInfo::exists(QStringLiteral("/usr/bin/systemctl"))) return;
    static_cast<void>(runSystemctl({QStringLiteral("disable"), QStringLiteral("--now"),
                                    QStringLiteral("pacsmith-update.timer")}, nullptr));
    static_cast<void>(runSystemctl({QStringLiteral("disable"), QStringLiteral("--now"),
                                    QStringLiteral("pacsmith-tray.service")}, nullptr));
    static_cast<void>(runSystemctl({QStringLiteral("stop"), QStringLiteral("pacsmith-update.service")},
                                   nullptr));
    QFile::remove(legacyScheduleDropInPath());
}

}

bool BackgroundUpdateManager::apply(const BackgroundUpdateSettings &settings,
                                    const QString &executablePath, QString *error) {
    disableLegacySystemdUnits();
    const auto path = autostartPath();
    if (!settings.startAtLogin) {
        if (QFile::exists(path) && !QFile::remove(path) && error != nullptr) {
            *error = QStringLiteral("Could not remove PacSmith's login autostart entry");
            return false;
        }
        return true;
    }
    if (executablePath.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("PacSmith's executable path is unknown");
        return false;
    }
    if (!QDir{}.mkpath(QFileInfo(path).absolutePath())) {
        if (error != nullptr) *error = QStringLiteral("Could not create the autostart directory");
        return false;
    }
    auto exec = quotedDesktopExec(executablePath);
    if (settings.startMinimized) exec += QStringLiteral(" --tray");
    const auto contents = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=PacSmith\n"
        "Comment=Start PacSmith with the current session\n"
        "Exec=%1\n"
        "Icon=pacsmith\n"
        "Terminal=false\n"
        "Hidden=false\n"
        "X-GNOME-Autostart-enabled=true\n")
                              .arg(exec)
                              .toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace pacsmith
