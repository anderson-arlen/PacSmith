#pragma once

#include "core/model.hpp"

#include <QString>

namespace pacsmith::DomainValidation {

[[nodiscard]] QString optDirectory(const QString &value);
[[nodiscard]] QString command(const QString &name, const QString &destination);
[[nodiscard]] QString appRun(const QString &contents);
[[nodiscard]] QString desktopEntry(const QString &contents, const QString &destination);
[[nodiscard]] QString packageRelation(const QString &value);
[[nodiscard]] QString archPackageName(const QString &value);
[[nodiscard]] QString updateConfiguration(const UpdateConfiguration &configuration);

} // namespace pacsmith::DomainValidation
