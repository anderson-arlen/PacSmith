#pragma once

#include "core/model.hpp"

#include <QList>
#include <QSet>
#include <QString>

namespace pacsmith::library_cache {

QString root();
void prune(const QList<Project> &projects, const QSet<QString> &deletedArtifacts = {});
void removeRelease(const QString &projectId, const QString &releaseId);

} // namespace pacsmith::library_cache
