#pragma once

#include "core/model.hpp"

#include <QList>
#include <QString>
#include <QStringList>

namespace pacsmith {

struct InstallPlanEntry {
    QString path;
    QString source;
    QString purpose;
    bool excluded{false};
};

struct InstallPlan {
    QList<InstallPlanEntry> entries;
    QStringList warnings;
};

class PkgbuildInstallPlan final {
public:
    [[nodiscard]] static InstallPlan parse(const QString &pkgbuild, const PackageRelease &release);
};

} // namespace pacsmith
