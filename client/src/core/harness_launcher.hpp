#pragma once

#include <QString>

namespace pacsmith {

class HarnessLauncher final {
public:
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
