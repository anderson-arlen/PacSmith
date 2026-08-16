#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace pacsmith {

class PathSafety final {
public:
    [[nodiscard]] static std::optional<QString> normalizedArchivePath(const QString &path);
    [[nodiscard]] static bool safeSymlinkTarget(const QString &entryPath, const QString &target);
    [[nodiscard]] static bool safeAppImageSymlinkTarget(const QString &entryPath,
                                                        const QString &target);
    [[nodiscard]] static bool isDebianSpecificPath(const QString &path);
    [[nodiscard]] static bool isForeignPackageManagerPath(const QString &path);
    [[nodiscard]] static QString reviewReason(const QString &path);
    [[nodiscard]] static QStringList urlsFromText(const QString &text);
};

} // namespace pacsmith
