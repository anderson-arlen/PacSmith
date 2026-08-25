#pragma once

#include <QString>
#include <QStringList>

namespace pacsmith {

class SystemPackageQuery final {
public:
    [[nodiscard]] static QStringList repositoryPackageNames(QString *error = nullptr);
    [[nodiscard]] static bool repositoryPackageAvailable(const QString &packageName);
};

} // namespace pacsmith
