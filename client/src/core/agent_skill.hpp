#pragma once

#include <QString>

namespace pacsmith {

class AgentSkill final {
public:
    [[nodiscard]] static QString userDirectory(const QString &homeDirectory = {});
    [[nodiscard]] static bool isSkillDirectory(const QString &directory);
    [[nodiscard]] static bool isPluginDirectory(const QString &directory);
    [[nodiscard]] static bool install(const QString &sourceDirectory,
                                      const QString &targetDirectory,
                                      bool replaceUnmanaged,
                                      QString *error = nullptr);
    [[nodiscard]] static bool uninstall(const QString &targetDirectory,
                                        QString *error = nullptr);
};

} // namespace pacsmith
