#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace pacsmith {

class PathSafety final {
public:
    [[nodiscard]] static std::optional<QString> normalizedArchivePath(const QString &path);
    [[nodiscard]] static bool safeSymlinkTarget(const QString &entryPath, const QString &target);
    // DEB/RPM/archive payloads may use absolute FHS links such as
    // etc/cron.daily/app -> /opt/vendor/cron/app. Relative links still must not
    // escape the archive; absolute targets may only resolve under conventional
    // package install roots.
    [[nodiscard]] static bool safePackageSymlinkTarget(const QString &entryPath,
                                                       const QString &target);
    [[nodiscard]] static bool safeAppImageSymlinkTarget(const QString &entryPath,
                                                        const QString &target);
    // Empty when the link is a normal package layout. Otherwise a review reason
    // so the project can be created and the user can keep or exclude the link.
    [[nodiscard]] static QString symlinkReviewReason(const QString &entryPath,
                                                     const QString &target);
    [[nodiscard]] static bool isDebianSpecificPath(const QString &path);
    [[nodiscard]] static bool isForeignPackageManagerPath(const QString &path);
    [[nodiscard]] static QString reviewReason(const QString &path);
    [[nodiscard]] static QStringList urlsFromText(const QString &text);
};

} // namespace pacsmith
