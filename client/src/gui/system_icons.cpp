#include "gui/system_icons.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QStyle>

namespace pacsmith::gui {
namespace {

bool installedTheme(const QString &name) {
    if (name.isEmpty() || name == QStringLiteral("hicolor") || name.contains(QLatin1Char('/'))) return false;
    for (const auto &root : QIcon::themeSearchPaths()) {
        if (QFileInfo::exists(QDir(root).filePath(name + QStringLiteral("/index.theme")))) return true;
    }
    return false;
}

QString configuredTheme() {
    const QList<QPair<QString, QString>> settings{
        {QStringLiteral("qt6ct/qt6ct.conf"), QStringLiteral("Appearance/icon_theme")},
        {QStringLiteral("kdeglobals"), QStringLiteral("Icons/Theme")},
        {QStringLiteral("gtk-4.0/settings.ini"), QStringLiteral("Settings/gtk-icon-theme-name")},
        {QStringLiteral("gtk-3.0/settings.ini"), QStringLiteral("Settings/gtk-icon-theme-name")},
    };
    for (const auto &root : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) {
        for (const auto &[file, key] : settings) {
            const QSettings config(QDir(root).filePath(file), QSettings::IniFormat);
            const auto name = config.value(key).toString().trimmed();
            if (installedTheme(name)) return name;
        }
    }
    return {};
}

} // namespace

void initializeSystemIconTheme() {
    // Minimal window managers may not provide Qt's platform theme integration, even with a GTK theme configured.
    const auto current = QIcon::themeName();
    if (!current.isEmpty() && current != QStringLiteral("hicolor")) return;
    auto paths = QIcon::themeSearchPaths();
    paths.prepend(QDir::home().filePath(QStringLiteral(".icons")));
    for (const auto &root : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        paths.append(QDir(root).filePath(QStringLiteral("icons")));
    }
    paths.removeDuplicates();
    QIcon::setThemeSearchPaths(paths);
    auto theme = configuredTheme();
    if (theme.isEmpty()) {
        for (const auto &candidate : {QStringLiteral("breeze"), QStringLiteral("AdwaitaLegacy"),
                                      QStringLiteral("Adwaita"), QStringLiteral("oxygen")}) {
            if (installedTheme(candidate)) { theme = candidate; break; }
        }
    }
    if (!theme.isEmpty()) QIcon::setThemeName(theme);
}

QIcon systemIcon(const QString &name) {
    initializeSystemIconTheme();
    return QIcon::fromTheme(name,
        QIcon::fromTheme(QStringLiteral("application-x-executable"),
                         QApplication::style()->standardIcon(QStyle::SP_FileIcon)));
}

QList<SystemIconChoice> standardSystemIcons() {
    initializeSystemIconTheme();
    const QList<QPair<QString, QString>> names{
        {QStringLiteral("Executable"), QStringLiteral("application-x-executable")},
        {QStringLiteral("Terminal"), QStringLiteral("utilities-terminal")},
        {QStringLiteral("Utilities"), QStringLiteral("applications-utilities")},
        {QStringLiteral("Development"), QStringLiteral("applications-development")},
        {QStringLiteral("System tools"), QStringLiteral("applications-system")},
        {QStringLiteral("Settings"), QStringLiteral("preferences-system")},
        {QStringLiteral("Internet"), QStringLiteral("applications-internet")},
        {QStringLiteral("Web browser"), QStringLiteral("web-browser")},
        {QStringLiteral("Graphics"), QStringLiteral("applications-graphics")},
        {QStringLiteral("Multimedia"), QStringLiteral("applications-multimedia")},
        {QStringLiteral("Office"), QStringLiteral("applications-office")},
        {QStringLiteral("Games"), QStringLiteral("applications-games")},
        {QStringLiteral("Education"), QStringLiteral("applications-education")},
        {QStringLiteral("Science"), QStringLiteral("applications-science")},
        {QStringLiteral("Text editor"), QStringLiteral("accessories-text-editor")},
        {QStringLiteral("File manager"), QStringLiteral("system-file-manager")},
        {QStringLiteral("Archive"), QStringLiteral("package-x-generic")},
        {QStringLiteral("Font"), QStringLiteral("preferences-desktop-font")},
    };
    QList<SystemIconChoice> choices;
    for (const auto &[label, name] : names) {
        if (!QIcon::hasThemeIcon(name)) continue;
        const auto icon = QIcon::fromTheme(name);
        if (icon.pixmap(24, 24).isNull()) continue;
        choices.append({label, name, icon});
    }
    return choices;
}

QList<SystemIconChoice> availableSystemIcons() {
    initializeSystemIconTheme();
    QSet<QString> names;
    QSet<QString> visited;
    QStringList themes{QIcon::themeName(), QIcon::fallbackThemeName(), QStringLiteral("hicolor")};
    const auto collect = [&](const QString &directory) {
        for (const auto &file : QDir(directory).entryInfoList(
                 {QStringLiteral("*.png"), QStringLiteral("*.svg"), QStringLiteral("*.svgz"), QStringLiteral("*.xpm")},
                 QDir::Files)) {
            names.insert(file.completeBaseName());
        }
    };
    for (qsizetype index = 0; index < themes.size(); ++index) {
        const auto theme = themes[index];
        if (theme.isEmpty() || theme.contains(QLatin1Char('/')) || visited.contains(theme)) continue;
        visited.insert(theme);
        for (const auto &root : QIcon::themeSearchPaths()) {
            const QDir directory(QDir(root).filePath(theme));
            if (!QFileInfo::exists(directory.filePath(QStringLiteral("index.theme")))) continue;
            QSettings settings(directory.filePath(QStringLiteral("index.theme")), QSettings::IniFormat);
            settings.beginGroup(QStringLiteral("Icon Theme"));
            themes.append(settings.value(QStringLiteral("Inherits")).toStringList());
            auto subdirectories = settings.value(QStringLiteral("Directories")).toStringList();
            subdirectories.append(settings.value(QStringLiteral("ScaledDirectories")).toStringList());
            subdirectories.removeDuplicates();
            for (const auto &subdirectory : subdirectories) collect(directory.filePath(subdirectory));
        }
    }
    for (const auto &directory : QIcon::fallbackSearchPaths()) collect(directory);
    auto sorted = names.values();
    sorted.sort(Qt::CaseInsensitive);
    QList<SystemIconChoice> choices;
    for (const auto &name : sorted) {
        if (QIcon::hasThemeIcon(name)) choices.append({name, name, QIcon::fromTheme(name)});
    }
    return choices;
}

} // namespace pacsmith::gui
