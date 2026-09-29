#include "core/library_cache.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

namespace pacsmith::library_cache {
namespace {

bool isId(const QString &name) {
    return name.size() == 36 && !QUuid(name).isNull();
}

void removeDirectory(const QString &path) {
    if (!QDir(path).removeRecursively()) qWarning() << "Could not remove library cache" << path;
}

} // namespace

QString root() {
    const auto data = qEnvironmentVariable("XDG_DATA_HOME");
    const auto home = (!data.isEmpty() && QDir::isAbsolutePath(data))
                          ? data : QDir::home().filePath(QStringLiteral(".local/share"));
    return QDir(home).filePath(QStringLiteral("pacsmith/client/cache"));
}

void removeRelease(const QString &projectId, const QString &releaseId) {
    if (!isId(projectId) || !isId(releaseId)) return;
    if (QFileInfo(QDir(root()).filePath(projectId)).isSymLink()) return;
    removeDirectory(QDir(root()).filePath(projectId + QLatin1Char('/') + releaseId));
}

void prune(const QList<Project> &projects, const QSet<QString> &deletedArtifacts) {
    QSet<QString> artifacts;
    QSet<QString> releases;
    for (const auto &project : projects) {
        for (const auto &release : project.releases) {
            artifacts.insert(release.sourceArtifactId);
            artifacts.insert(release.iconArtifactId);
            releases.insert(project.id + QLatin1Char('/') + release.id);
        }
    }
    const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-1);
    const QDir cache(root());
    for (const auto &entry : cache.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry.isSymLink()) continue;
        const auto name = entry.fileName();
        if (entry.isFile()) {
            const auto id = name.left(36);
            if (!isId(id) || name.size() < 38 || name.at(36) != QLatin1Char('-') ||
                artifacts.contains(id)) continue;
            // A refresh can overlap a download for a newly imported release.
            if (!deletedArtifacts.contains(id) && entry.lastModified() >= cutoff) continue;
            if (!QFile::remove(entry.filePath())) qWarning() << "Could not remove library cache" << entry.filePath();
        } else if (isId(name)) {
            const QDir project(entry.filePath());
            for (const auto &release : project.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                if (release.isSymLink() || !isId(release.fileName()) || release.lastModified() >= cutoff) continue;
                if (!releases.contains(name + QLatin1Char('/') + release.fileName())) {
                    removeDirectory(release.filePath());
                }
            }
            cache.rmdir(name);
        }
    }
}

} // namespace pacsmith::library_cache
