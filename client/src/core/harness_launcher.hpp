#pragma once

#include "core/app_settings.hpp"

#include <QString>

namespace pacsmith {

struct HarnessLaunchResult {
    bool started{false};
    bool promptNeedsClipboard{false};
    QString error;
};

class HarnessLauncher final {
public:
    [[nodiscard]] static HarnessLaunchResult launch(const HarnessProfile &profile,
                                                    const QString &prompt);
    [[nodiscard]] static QStringList expandedArguments(const HarnessProfile &profile,
                                                       const QString &prompt,
                                                       bool *promptInserted = nullptr);
    [[nodiscard]] static QString projectPrompt(const QString &projectId,
                                               const QString &releaseId = {});
    [[nodiscard]] static QString dependencyPrompt(const QString &projectId,
                                                  const QString &releaseId,
                                                  const QString &dependency);
    [[nodiscard]] static QString buildFailurePrompt(const QString &projectId,
                                                    const QString &releaseId);
    [[nodiscard]] static QString appImagePrompt(const QString &projectId,
                                               const QString &releaseId);
    [[nodiscard]] static QString customPkgbuildPrompt(const QString &projectId,
                                                     const QString &releaseId);
    [[nodiscard]] static QString automaticUpdatePrompt(const QString &projectId,
                                                       const QString &releaseId,
                                                       bool customPkgbuild);
};

} // namespace pacsmith
