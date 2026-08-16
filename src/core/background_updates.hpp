#pragma once

#include "core/app_settings.hpp"

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace pacsmith {

struct BackgroundUpdateState {
    bool checking{false};
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
    [[nodiscard]] static QString timerUnitPath();
    [[nodiscard]] static QString scheduleDropInPath();
    [[nodiscard]] static bool unitInstalled();
    [[nodiscard]] static bool isEnabled(QString *error = nullptr);
    [[nodiscard]] static bool apply(const BackgroundUpdateSettings &settings,
                                    QString *error = nullptr);
    [[nodiscard]] static bool runNow(QString *error = nullptr);
};

} // namespace pacsmith
