#pragma once

#include <QString>

namespace pacsmith::mcp {

enum class PermissionLevel { Routine, MandatoryConfirmation };

class PermissionPolicy final {
public:
    [[nodiscard]] static PermissionLevel level(const QString &toolName);
    [[nodiscard]] static QString confirmationMessage(const QString &toolName,
                                                     const QString &target);
    [[nodiscard]] static bool canProceedToConfirmation(const QString &toolName,
                                                       bool elicitationSupported,
                                                       QString *error = nullptr);
};

} // namespace pacsmith::mcp
