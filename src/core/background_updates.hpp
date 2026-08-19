#pragma once

#include "core/app_settings.hpp"
#include "core/model.hpp"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

namespace pacsmith {

struct BackgroundUpdateState {
    bool checking{false};
    QString checkingProjectId;
    QString checkingProjectName;
    QString preparingProjectId;
    QString preparingProjectName;
    QString preparationPhase;
    qint64 preparationBytesReceived{0};
    qint64 preparationBytesTotal{-1};
    int availableUpdates{0};
    int failedChecks{0};
    QDateTime lastRun;
    QStringList projectsWithUpdates;
    QString message;
};

// Count projects whose retained or detected vendor version is newer than the
// currently installed PacSmith release. Used for the tray badge so installs do
// not leave the last check's snapshot stale until the next scheduled run.
void applyAvailableUpdateCensus(BackgroundUpdateState &state, const QList<Project> &projects);
[[nodiscard]] int availableUpdateCount(const QList<Project> &projects);

class BackgroundUpdateStateStore final {
public:
    [[nodiscard]] static QString defaultPath();
    [[nodiscard]] static BackgroundUpdateState load(QString *error = nullptr);
    [[nodiscard]] static bool save(const BackgroundUpdateState &state, QString *error = nullptr);
    [[nodiscard]] static bool syncAvailableUpdates(const QList<Project> &projects,
                                                   QString *error = nullptr);
};

class BackgroundUpdateManager final {
public:
    [[nodiscard]] static QString calendar(const BackgroundUpdateSettings &settings);
    [[nodiscard]] static QDateTime lastScheduledOccurrence(
        const BackgroundUpdateSettings &settings,
        const QDateTime &now = QDateTime::currentDateTime());
    [[nodiscard]] static QDateTime nextScheduledOccurrence(
        const BackgroundUpdateSettings &settings,
        const QDateTime &now = QDateTime::currentDateTime());
    [[nodiscard]] static bool isOverdue(const BackgroundUpdateSettings &settings,
                                        const QDateTime &lastRun,
                                        const QDateTime &now = QDateTime::currentDateTime());
    [[nodiscard]] static QString autostartPath();
    [[nodiscard]] static bool apply(const BackgroundUpdateSettings &settings,
                                    const QString &executablePath,
                                    QString *error = nullptr);
};

} // namespace pacsmith
