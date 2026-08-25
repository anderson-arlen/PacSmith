#include "core/domain_validation.hpp"

#include <QRegularExpression>
#include <QUrl>

namespace pacsmith::DomainValidation {

QString optDirectory(const QString &value) {
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9@._+\\-]+$"));
    return safe.match(value.trimmed()).hasMatch()
               ? QString{}
               : QStringLiteral("Use a single /opt directory name containing letters, digits, '.', '_', '+', '@', or '-'.");
}

QString command(const QString &name, const QString &destination) {
    static const QRegularExpression safeName(QStringLiteral("^[A-Za-z0-9@._+\\-]+$"));
    static const QRegularExpression safeDestination(
        QStringLiteral("^/usr/bin/[A-Za-z0-9@._+\\-]+$"));
    if (!safeName.match(name.trimmed()).hasMatch()) {
        return QStringLiteral("Command names must contain only letters, digits, '.', '_', '+', '@', or '-'.");
    }
    if (!safeDestination.match(destination.trimmed()).hasMatch()) {
        return QStringLiteral("Command destinations must be a simple absolute path below /usr/bin.");
    }
    return {};
}

QString appRun(const QString &contents) {
    if (!contents.startsWith(QStringLiteral("#!")) || contents.contains(QChar(QChar::Null)) ||
        contents.size() > 256 * 1024) {
        return QStringLiteral("AppRun must remain a #! script without NUL bytes and be at most 256 KiB.");
    }
    return {};
}

QString desktopEntry(const QString &contents, const QString &destination) {
    QStringList errors;
    if (!contents.contains(QRegularExpression(QStringLiteral(R"(^\s*\[Desktop Entry\]\s*$)"),
                                               QRegularExpression::MultilineOption))) {
        errors.append(QStringLiteral("Missing [Desktop Entry] section"));
    }
    if (!contents.contains(QRegularExpression(QStringLiteral(R"(^Name(?:\[[^\]]+\])?=.+$)"),
                                               QRegularExpression::MultilineOption))) {
        errors.append(QStringLiteral("Missing Name="));
    }
    if (!contents.contains(QRegularExpression(QStringLiteral(R"(^Type=(?:Application|Link|Directory)$)"),
                                               QRegularExpression::MultilineOption))) {
        errors.append(QStringLiteral("Type must be Application, Link, or Directory"));
    }
    const auto exec = QRegularExpression(QStringLiteral(R"(^Exec=(.+)$)"),
                                         QRegularExpression::MultilineOption).match(contents);
    if (!exec.hasMatch() || exec.captured(1).contains(QLatin1Char('\n')) ||
        exec.captured(1).contains(QLatin1Char('`')) ||
        exec.captured(1).contains(QStringLiteral("$(")) ||
        exec.captured(1).contains(QLatin1Char(';'))) {
        errors.append(QStringLiteral("Exec must be present and must not contain shell syntax"));
    }
    static const QRegularExpression safeDestination(
        QStringLiteral("^/usr/share/applications/[A-Za-z0-9@._+\\-]+\\.desktop$"));
    if (!safeDestination.match(destination.trimmed()).hasMatch()) {
        errors.append(QStringLiteral("Destination must be a simple .desktop name under /usr/share/applications"));
    }
    return errors.join(QStringLiteral("; "));
}

QString packageRelation(const QString &value) {
    static const QRegularExpression valid(QStringLiteral(
        "^[a-z0-9@._+:-]+(?:[<>=]{1,2}[A-Za-z0-9@._+~:-]+)?$"));
    return valid.match(value.trimmed()).hasMatch()
               ? QString{}
               : QStringLiteral("Use an Arch package or virtual name, optionally followed by a version comparison.");
}

QString archPackageName(const QString &value) {
    static const QRegularExpression valid(QStringLiteral("^[a-z0-9@._+:-]+$"));
    return valid.match(value.trimmed()).hasMatch()
               ? QString{}
               : QStringLiteral("Arch package names may contain lowercase letters, digits, '@', '.', '_', '+', ':', or '-'.");
}

QString updateConfiguration(const UpdateConfiguration &configuration) {
    if (configuration.strategy == UpdateStrategy::Manual) return {};
    if (configuration.strategy == UpdateStrategy::GitHubRelease) {
        const QRegularExpression expression(configuration.githubAssetRegex.trimmed());
        if (configuration.githubOwner.trimmed().isEmpty() ||
            configuration.githubRepository.trimmed().isEmpty() ||
            configuration.githubAssetRegex.trimmed().isEmpty()) {
            return QStringLiteral("GitHub owner, repository, and an asset-name regular expression are required.");
        }
        if (!expression.isValid()) {
            return QStringLiteral("The asset-name regular expression is invalid: %1")
                .arg(expression.errorString());
        }
        return {};
    }
    const QUrl url(configuration.url.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != QStringLiteral("https") &&
                           url.scheme() != QStringLiteral("http")) || url.host().isEmpty()) {
        return QStringLiteral("The update source must be an absolute HTTP or HTTPS URL.");
    }
    if (configuration.strategy == UpdateStrategy::AptRepository &&
        (configuration.aptSuite.trimmed().isEmpty() ||
         configuration.aptComponent.trimmed().isEmpty() ||
         configuration.aptArchitecture.trimmed().isEmpty() ||
         configuration.aptPackageName.trimmed().isEmpty())) {
        return QStringLiteral("APT suite, component, architecture, and package are required.");
    }
    if (configuration.strategy == UpdateStrategy::RpmRepository &&
        (configuration.rpmArchitecture.trimmed().isEmpty() ||
         configuration.rpmPackageName.trimmed().isEmpty())) {
        return QStringLiteral("RPM architecture and package are required.");
    }
    return {};
}

} // namespace pacsmith::DomainValidation
