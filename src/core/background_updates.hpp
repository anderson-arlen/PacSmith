#pragma once

#include "core/app_settings.hpp"

#include <QDateTime>
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

class BackgroundUpdateStateStore final {
public:
    [[nodiscard]] static QString defaultPath();
    [[nodiscard]] static BackgroundUpdateState load(QString *error = nullptr);
    [[nodiscard]] static bool save(const BackgroundUpdateState &state, QString *error = nullptr);
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
