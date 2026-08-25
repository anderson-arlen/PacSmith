#include "core/agent_skill.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

namespace pacsmith {
namespace {

constexpr auto managedMarker = ".pacsmith-managed";

bool copyDirectory(const QString &sourcePath, const QString &targetPath, QString *error) {
    const QDir source(sourcePath);
    if (!QDir().mkpath(targetPath)) {
        if (error != nullptr) *error = QStringLiteral("could not create %1").arg(targetPath);
        return false;
    }
    const auto entries = source.entryInfoList(
        QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System,
        QDir::Name | QDir::DirsFirst);
    for (const auto &entry : entries) {
        if (entry.isSymbolicLink()) {
            if (error != nullptr) {
                *error = QStringLiteral("skill contains unsupported symbolic link: %1")
                             .arg(entry.fileName());
            }
            return false;
        }
        const auto target = QDir(targetPath).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirectory(entry.absoluteFilePath(), target, error)) return false;
            continue;
        }
        if (!entry.isFile() || !QFile::copy(entry.absoluteFilePath(), target)) {
            if (error != nullptr) {
                *error = QStringLiteral("could not copy skill file %1").arg(entry.fileName());
            }
            return false;
        }
        static_cast<void>(QFile::setPermissions(target, entry.permissions()));
    }
    return true;
}

bool removeStagingDirectory(const QString &path) {
    const QFileInfo entry(path);
    if (!entry.exists() && !entry.isSymbolicLink()) return true;
    return entry.isDir() && !entry.isSymbolicLink() ? QDir(path).removeRecursively()
                                                    : QFile::remove(path);
}

bool validateTarget(const QFileInfo &target, QString *error) {
    if (!target.isAbsolute() || target.fileName() != QStringLiteral("pacsmith")) {
        if (error != nullptr) {
            *error = QStringLiteral("skill destination must be an absolute pacsmith directory");
        }
        return false;
    }
    if (target.isSymbolicLink()) {
        if (error != nullptr) {
            *error = QStringLiteral("refusing to modify a symbolic-link skill destination");
        }
        return false;
    }
    return true;
}

} // namespace

QString AgentSkill::userDirectory(const QString &homeDirectory) {
    const auto home = homeDirectory.isEmpty() ? QDir::homePath() : homeDirectory;
    return QDir(home).filePath(QStringLiteral(".agents/skills/pacsmith"));
}

bool AgentSkill::isSkillDirectory(const QString &directory) {
    const QFileInfo root(directory);
    const QFileInfo manifest(QDir(directory).filePath(QStringLiteral("SKILL.md")));
    return root.isDir() && !root.isSymbolicLink() && manifest.isFile() &&
           !manifest.isSymbolicLink();
}

bool AgentSkill::isPluginDirectory(const QString &directory) {
    const QFileInfo root(directory);
    const QFileInfo manifest(QDir(directory).filePath(QStringLiteral("plugin.json")));
    const QFileInfo mcpConfig(QDir(directory).filePath(QStringLiteral("mcp.json")));
    const auto skill = QDir(directory).filePath(QStringLiteral("skills/pacsmith"));
    return root.isDir() && !root.isSymbolicLink() && manifest.isFile() &&
           !manifest.isSymbolicLink() && mcpConfig.isFile() &&
           !mcpConfig.isSymbolicLink() && isSkillDirectory(skill);
}

bool AgentSkill::install(const QString &sourceDirectory, const QString &targetDirectory,
                         const bool replaceUnmanaged, QString *error) {
    if (!isSkillDirectory(sourceDirectory)) {
        if (error != nullptr) *error = QStringLiteral("PacSmith Skill source is missing or invalid");
        return false;
    }
    const QFileInfo targetInfo(targetDirectory);
    if (!validateTarget(targetInfo, error)) return false;

    const auto parentPath = targetInfo.dir().absolutePath();
    if (!QDir().mkpath(parentPath)) {
        if (error != nullptr) *error = QStringLiteral("could not create shared Agent Skills directory %1").arg(parentPath);
        return false;
    }
    const QFileInfo marker(QDir(targetDirectory).filePath(QString::fromLatin1(managedMarker)));
    const bool managed = marker.isFile() && !marker.isSymbolicLink();
    if (targetInfo.exists() && !managed && !replaceUnmanaged) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 already exists and was not installed by PacSmith; use --force to replace it")
                         .arg(targetDirectory);
        }
        return false;
    }

    const auto suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto staging = QDir(parentPath).filePath(QStringLiteral(".pacsmith-install-%1").arg(suffix));
    const auto backup = QDir(parentPath).filePath(QStringLiteral(".pacsmith-backup-%1").arg(suffix));
    if (!copyDirectory(sourceDirectory, staging, error)) {
        static_cast<void>(removeStagingDirectory(staging));
        return false;
    }
    QSaveFile markerFile(QDir(staging).filePath(QString::fromLatin1(managedMarker)));
    if (!markerFile.open(QIODevice::WriteOnly) ||
        markerFile.write("Managed by `pacsmith skill install`.\n") < 0 || !markerFile.commit()) {
        if (error != nullptr) *error = QStringLiteral("could not mark the staged PacSmith Skill installation");
        static_cast<void>(removeStagingDirectory(staging));
        return false;
    }

    if (targetInfo.exists() && !QDir().rename(targetDirectory, backup)) {
        if (error != nullptr) *error = QStringLiteral("could not preserve the existing PacSmith Skill installation");
        static_cast<void>(removeStagingDirectory(staging));
        return false;
    }
    if (!QDir().rename(staging, targetDirectory)) {
        if (QFileInfo::exists(backup)) static_cast<void>(QDir().rename(backup, targetDirectory));
        if (error != nullptr) *error = QStringLiteral("could not activate the PacSmith Skill installation");
        static_cast<void>(removeStagingDirectory(staging));
        return false;
    }
    if (!removeStagingDirectory(backup)) {
        if (error != nullptr) {
            *error = QStringLiteral("PacSmith Skill was installed, but its previous backup could not be removed: %1")
                         .arg(backup);
        }
        return false;
    }
    return true;
}

bool AgentSkill::uninstall(const QString &targetDirectory, QString *error) {
    const QFileInfo targetInfo(targetDirectory);
    if (!validateTarget(targetInfo, error)) return false;
    if (!targetInfo.exists()) return true;
    const QFileInfo marker(QDir(targetDirectory).filePath(QString::fromLatin1(managedMarker)));
    if (!marker.isFile() || marker.isSymbolicLink()) {
        if (error != nullptr) {
            *error = QStringLiteral("refusing to remove %1 because it is not managed by PacSmith")
                         .arg(targetDirectory);
        }
        return false;
    }
    if (!QDir(targetDirectory).removeRecursively()) {
        if (error != nullptr) *error = QStringLiteral("could not remove %1").arg(targetDirectory);
        return false;
    }
    return true;
}

} // namespace pacsmith
