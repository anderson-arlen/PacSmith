#include "core/path_safety.hpp"

#include <QDir>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace pacsmith {
namespace {

bool absoluteTargetInRoots(const QString &target, const QStringList &roots) {
    if (target.isEmpty() || !target.startsWith(QLatin1Char('/')) ||
        target.contains(QChar::Null)) {
        return false;
    }
    const auto normalized = QDir::cleanPath(QDir::fromNativeSeparators(target));
    if (!normalized.startsWith(QLatin1Char('/'))) return false;
    return std::any_of(roots.cbegin(), roots.cend(), [&normalized](const QString &root) {
        return normalized == root || normalized.startsWith(root + QLatin1Char('/'));
    });
}

} // namespace

std::optional<QString> PathSafety::normalizedArchivePath(const QString &path) {
    if (path.contains(QChar::Null) || path.startsWith(QLatin1Char('/')) ||
        QRegularExpression(QStringLiteral(R"(^[A-Za-z]:[\\/])")).match(path).hasMatch()) {
        return std::nullopt;
    }
    QString normalized = QDir::fromNativeSeparators(path);
    while (normalized.startsWith(QStringLiteral("./"))) {
        normalized.remove(0, 2);
    }
    const auto parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList safeParts;
    for (const auto &part : parts) {
        if (part == QStringLiteral("..")) {
            return std::nullopt;
        }
        if (part != QStringLiteral(".")) {
            safeParts.append(part);
        }
    }
    if (safeParts.isEmpty()) {
        return QString{};
    }
    return safeParts.join(QLatin1Char('/'));
}

bool PathSafety::safeSymlinkTarget(const QString &entryPath, const QString &target) {
    if (target.isEmpty() || target.startsWith(QLatin1Char('/')) || target.contains(QChar::Null)) {
        return false;
    }
    const auto entry = normalizedArchivePath(entryPath);
    if (!entry) return false;
    QStringList resolved = entry->section(QLatin1Char('/'), 0, -2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const auto &part : QDir::fromNativeSeparators(target).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QStringLiteral("..")) {
            if (resolved.isEmpty()) return false;
            resolved.removeLast();
        } else if (part != QStringLiteral(".")) {
            resolved.append(part);
        }
    }
    return true;
}

bool PathSafety::safePackageSymlinkTarget(const QString &entryPath, const QString &target) {
    if (safeSymlinkTarget(entryPath, target)) return true;
    static const QStringList packageRoots{
        QStringLiteral("/opt"), QStringLiteral("/usr"),
        QStringLiteral("/bin"), QStringLiteral("/sbin"),
        QStringLiteral("/lib"), QStringLiteral("/lib64")};
    return absoluteTargetInRoots(target, packageRoots);
}

bool PathSafety::safeAppImageSymlinkTarget(const QString &entryPath,
                                           const QString &target) {
    if (safeSymlinkTarget(entryPath, target)) return true;
    static const QStringList runtimeRoots{
        QStringLiteral("/bin"), QStringLiteral("/sbin"),
        QStringLiteral("/lib"), QStringLiteral("/lib64"),
        QStringLiteral("/usr/bin"), QStringLiteral("/usr/sbin"),
        QStringLiteral("/usr/lib"), QStringLiteral("/usr/lib64"),
        QStringLiteral("/usr/libexec")};
    return absoluteTargetInRoots(target, runtimeRoots);
}

QString PathSafety::symlinkReviewReason(const QString &entryPath, const QString &target) {
    if (safePackageSymlinkTarget(entryPath, target)) return {};
    return QStringLiteral(
        "Symbolic link target '%1' leaves the package tree or points outside "
        "conventional package install roots. It is excluded from the Arch package "
        "until you keep it.")
        .arg(target);
}

bool PathSafety::isDebianSpecificPath(const QString &path) {
    const auto normalized = path.startsWith(QLatin1Char('/')) ? path.mid(1) : path;
    return normalized == QStringLiteral("etc/apt") ||
           normalized.startsWith(QStringLiteral("etc/apt/")) ||
           normalized == QStringLiteral("usr/share/keyrings") ||
           normalized.startsWith(QStringLiteral("usr/share/keyrings/")) ||
           normalized == QStringLiteral("usr/share/lintian") ||
           normalized.startsWith(QStringLiteral("usr/share/lintian/"));
}

bool PathSafety::isForeignPackageManagerPath(const QString &path) {
    const auto normalized = path.startsWith(QLatin1Char('/')) ? path.mid(1) : path;
    return isDebianSpecificPath(normalized) ||
           normalized == QStringLiteral("etc/yum.repos.d") ||
           normalized.startsWith(QStringLiteral("etc/yum.repos.d/")) ||
           normalized == QStringLiteral("etc/dnf") ||
           normalized.startsWith(QStringLiteral("etc/dnf/")) ||
           normalized == QStringLiteral("etc/zypp") ||
           normalized.startsWith(QStringLiteral("etc/zypp/")) ||
           normalized == QStringLiteral("etc/pki/rpm-gpg") ||
           normalized.startsWith(QStringLiteral("etc/pki/rpm-gpg/")) ||
           normalized == QStringLiteral("etc/rpm") ||
           normalized.startsWith(QStringLiteral("etc/rpm/")) ||
           normalized == QStringLiteral("usr/lib/sysimage/rpm") ||
           normalized.startsWith(QStringLiteral("usr/lib/sysimage/rpm/")) ||
           normalized == QStringLiteral("var/lib/rpm") ||
           normalized.startsWith(QStringLiteral("var/lib/rpm/"));
}

QString PathSafety::reviewReason(const QString &path) {
    const auto normalized = path.startsWith(QLatin1Char('/')) ? path.mid(1) : path;
    if (normalized == QStringLiteral("etc/apparmor.d") ||
        normalized.startsWith(QStringLiteral("etc/apparmor.d/"))) {
        return QStringLiteral("AppArmor policy. Arch supports AppArmor but does not enable it by default. Recommended: keep the vendor profile for compatibility if AppArmor is enabled; it is inert when AppArmor is disabled. Exclude it only if you intentionally want no vendor policy under /etc");
    }
    if (normalized.startsWith(QStringLiteral("etc/apt/")) || normalized == QStringLiteral("etc/apt")) {
        return QStringLiteral("Debian/APT configuration is excluded by default");
    }
    if (normalized == QStringLiteral("usr/share/keyrings") ||
        normalized.startsWith(QStringLiteral("usr/share/keyrings/"))) {
        return QStringLiteral("Repository keyring may be Debian/APT-specific and is excluded by default");
    }
    if (normalized == QStringLiteral("usr/share/lintian") ||
        normalized.startsWith(QStringLiteral("usr/share/lintian/"))) {
        return QStringLiteral("Debian Lintian package-checker metadata has no function in an Arch package and is excluded by default");
    }
    if (normalized == QStringLiteral("etc/yum.repos.d") ||
        normalized.startsWith(QStringLiteral("etc/yum.repos.d/")) ||
        normalized == QStringLiteral("etc/dnf") ||
        normalized.startsWith(QStringLiteral("etc/dnf/")) ||
        normalized == QStringLiteral("etc/zypp") ||
        normalized.startsWith(QStringLiteral("etc/zypp/"))) {
        return QStringLiteral("RPM repository configuration is used only as update-source evidence and is excluded from the Arch package by default");
    }
    if (normalized == QStringLiteral("etc/pki/rpm-gpg") ||
        normalized.startsWith(QStringLiteral("etc/pki/rpm-gpg/"))) {
        return QStringLiteral("RPM repository signing key is used only as update-source evidence and is excluded from the Arch package by default");
    }
    if (normalized == QStringLiteral("etc/rpm") ||
        normalized.startsWith(QStringLiteral("etc/rpm/")) ||
        normalized == QStringLiteral("usr/lib/sysimage/rpm") ||
        normalized.startsWith(QStringLiteral("usr/lib/sysimage/rpm/")) ||
        normalized == QStringLiteral("var/lib/rpm") ||
        normalized.startsWith(QStringLiteral("var/lib/rpm/"))) {
        return QStringLiteral("RPM package-manager state or configuration is incompatible with pacman and is excluded by default");
    }
    if (normalized.startsWith(QStringLiteral("usr/lib/systemd/"))) {
        return QStringLiteral("Systemd unit should be reviewed for Arch compatibility");
    }
    if (normalized == QStringLiteral("etc") || normalized.startsWith(QStringLiteral("etc/"))) {
        return QStringLiteral("System configuration should be reviewed");
    }
    return {};
}

QStringList PathSafety::urlsFromText(const QString &text) {
    static const QRegularExpression urlExpression(QStringLiteral(R"(https?://[^\s'\"<>\)]+)"),
                                                   QRegularExpression::CaseInsensitiveOption);
    QSet<QString> unique;
    auto iterator = urlExpression.globalMatch(text);
    while (iterator.hasNext()) {
        QString candidate = iterator.next().captured(0);
        while (candidate.endsWith(QLatin1Char(';')) || candidate.endsWith(QLatin1Char(','))) {
            candidate.chop(1);
        }
        const QUrl url(candidate);
        if (url.isValid() && !url.host().isEmpty()) {
            unique.insert(candidate);
        }
    }
    QStringList result(unique.cbegin(), unique.cend());
    result.sort();
    return result;
}

} // namespace pacsmith
