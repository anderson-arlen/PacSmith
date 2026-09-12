#include "core/app_settings.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <utility>

namespace pacsmith {

const HarnessProfile *AppSettings::configuredHarness() const {
    return harness ? &*harness : nullptr;
}

AppSettingsStore::AppSettingsStore() : directory_(defaultConfigDirectory()) {}

AppSettingsStore::AppSettingsStore(QString configDirectory) : directory_(std::move(configDirectory)) {}

QString AppSettingsStore::defaultConfigDirectory() {
    const auto xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (!xdg.isEmpty() && QDir::isAbsolutePath(xdg)) return QDir(xdg).filePath(QStringLiteral("pacsmith"));
    return QDir::home().filePath(QStringLiteral(".config/pacsmith"));
}

AppSettings AppSettingsStore::load(QString *error) const {
    AppSettings result;
    QFile file(QDir(directory_).filePath(QStringLiteral("settings.json")));
    if (!file.exists()) return result;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return result;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = parseError.errorString();
        return result;
    }
    const auto object = document.object();
    const auto appearance = object.value(QStringLiteral("appearance")).toObject();
    result.appearance.interfaceTheme =
        appearanceModeFromName(appearance.value(QStringLiteral("interfaceTheme")).toString());
    result.appearance.trayTheme =
        appearanceModeFromName(appearance.value(QStringLiteral("trayTheme")).toString());
    const auto updates = object.value(QStringLiteral("updates")).toObject();
    result.updates.enabled = updates.value(QStringLiteral("enabled")).toBool(false);
    result.updates.startAtLogin = updates.value(QStringLiteral("startAtLogin")).toBool(false);
    result.updates.startMinimized = updates.value(QStringLiteral("startMinimized")).toBool(false);
    result.updates.keepInTray = updates.value(QStringLiteral("keepInTray")).toBool(result.updates.startMinimized);
    result.updates.daily = updates.value(QStringLiteral("daily")).toBool(true);
    result.updates.weekDay = std::clamp(updates.value(QStringLiteral("weekDay")).toInt(1), 1, 7);
    const auto time = QTime::fromString(updates.value(QStringLiteral("localTime")).toString(), QStringLiteral("HH:mm"));
    if (time.isValid()) result.updates.localTime = time;
    result.updates.automaticallyPrepare = updates.value(QStringLiteral("automaticallyPrepare")).toBool(false);
    result.updates.retentionVersions =
        std::max(-1, updates.value(QStringLiteral("retentionVersions")).toInt(2));
    result.githubTokenConfigured = object.value(QStringLiteral("githubTokenConfigured")).toBool(false);
    auto candidates = object.value(QStringLiteral("harnessProfiles")).toArray();
    if (object.contains(QStringLiteral("harness"))) candidates = {object.value(QStringLiteral("harness"))};
    bool selectedDefault = false;
    for (const auto &value : candidates) {
        const auto profileObject = value.toObject();
        if (profileObject.value(QStringLiteral("protocol")).toString() != QStringLiteral("acp")) continue;
        HarnessProfile profile;
        profile.name = profileObject.value(QStringLiteral("name")).toString().trimmed();
        profile.executable = profileObject.value(QStringLiteral("executable")).toString().trimmed();
        for (const auto &argument : profileObject.value(QStringLiteral("arguments")).toArray()) {
            profile.arguments.append(argument.toString());
        }

        profile.registryId = profileObject.value(QStringLiteral("registryId")).toString();
        profile.registryVersion = profileObject.value(QStringLiteral("registryVersion")).toString();
        profile.configDefaults = profileObject.value(QStringLiteral("configDefaults")).toObject();
        const auto environment = profileObject.value(QStringLiteral("environment")).toObject();
        for (auto it = environment.begin(); it != environment.end(); ++it) profile.environment.insert(it.key(), it.value().toString());
        if (!profile.name.isEmpty() && !profile.executable.isEmpty() &&
            (!result.harness || (!selectedDefault && profileObject.value(QStringLiteral("default")).toBool()))) {
            result.harness = profile;
            selectedDefault = profileObject.value(QStringLiteral("default")).toBool();
        }
    }
    const auto onboarding = object.value(QStringLiteral("onboarding")).toObject();
    result.debAssociationPrompted = onboarding.value(QStringLiteral("debAssociationPrompted")).toBool(false);
    result.selfTrackingPrompted = onboarding.value(QStringLiteral("selfTrackingPrompted")).toBool(false);
    return result;
}

bool AppSettingsStore::save(const AppSettings &settings, QString *error) const {
    if (!QDir{}.mkpath(directory_)) {
        if (error != nullptr) *error = QStringLiteral("Could not create PacSmith's configuration directory");
        return false;
    }
    const QJsonObject appearance{
        {QStringLiteral("interfaceTheme"), appearanceModeName(settings.appearance.interfaceTheme)},
        {QStringLiteral("trayTheme"), appearanceModeName(settings.appearance.trayTheme)}};
    const QJsonObject updates{{QStringLiteral("enabled"), settings.updates.enabled},
                              {QStringLiteral("startAtLogin"), settings.updates.startAtLogin},
                              {QStringLiteral("startMinimized"), settings.updates.startMinimized},
                              {QStringLiteral("keepInTray"), settings.updates.keepInTray},
                              {QStringLiteral("daily"), settings.updates.daily},
                              {QStringLiteral("weekDay"), settings.updates.weekDay},
                              {QStringLiteral("localTime"), settings.updates.localTime.toString(QStringLiteral("HH:mm"))},
                              {QStringLiteral("automaticallyPrepare"), settings.updates.automaticallyPrepare},
                              {QStringLiteral("retentionVersions"), settings.updates.retentionVersions}};
    QJsonValue harness;
    if (settings.harness) {
        const auto &profile = *settings.harness;
        QJsonArray arguments;
        for (const auto &argument : profile.arguments) arguments.append(argument);
        QJsonObject environment;
        for (auto it = profile.environment.begin(); it != profile.environment.end(); ++it) environment.insert(it.key(), it.value());
        harness = QJsonObject{{QStringLiteral("name"), profile.name},
                                    {QStringLiteral("executable"), profile.executable},
                                    {QStringLiteral("arguments"), arguments},
                                    {QStringLiteral("protocol"), QStringLiteral("acp")},
                                    {QStringLiteral("registryId"), profile.registryId},
                                    {QStringLiteral("registryVersion"), profile.registryVersion},
                                    {QStringLiteral("configDefaults"), profile.configDefaults},
                                    {QStringLiteral("environment"), environment}};
    }
    const QJsonObject onboarding{{QStringLiteral("debAssociationPrompted"), settings.debAssociationPrompted},
                                 {QStringLiteral("selfTrackingPrompted"), settings.selfTrackingPrompted}};
    const QJsonObject object{{QStringLiteral("formatVersion"), 8},
                             {QStringLiteral("githubTokenConfigured"), settings.githubTokenConfigured},
                             {QStringLiteral("appearance"), appearance},
                             {QStringLiteral("updates"), updates},
                             {QStringLiteral("harness"), harness},
                             {QStringLiteral("onboarding"), onboarding}};
    QSaveFile file(QDir(directory_).filePath(QStringLiteral("settings.json")));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    const auto contents = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    return true;
}

bool AppSettingsStore::setHarness(const HarnessProfile &profile, QString *error) const {
    HarnessProfile normalized = profile;
    normalized.name = normalized.name.trimmed();
    normalized.executable = normalized.executable.trimmed();
    if (normalized.name.isEmpty() || normalized.executable.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("AI harness name and executable are required");
        return false;
    }
    if (normalized.name.contains(QChar::Null) || normalized.executable.contains(QChar::Null) ||
        std::any_of(normalized.arguments.cbegin(), normalized.arguments.cend(),
                    [](const auto &argument) { return argument.contains(QChar::Null); })) {
        if (error != nullptr) *error = QStringLiteral("AI harness values cannot contain NUL characters");
        return false;
    }

    auto settings = load(error);
    if (error != nullptr && !error->isEmpty()) return false;
    settings.harness = normalized;
    return save(settings, error);
}

bool AppSettingsStore::clearHarness(QString *error) const {
    auto settings = load(error);
    if (error != nullptr && !error->isEmpty()) return false;
    settings.harness.reset();
    return save(settings, error);
}

QString AppSettingsStore::settingsPath() const {
    return QDir(directory_).filePath(QStringLiteral("settings.json"));
}

QString appearanceModeName(const AppearanceMode mode) {
    switch (mode) {
    case AppearanceMode::Light: return QStringLiteral("light");
    case AppearanceMode::Dark: return QStringLiteral("dark");
    case AppearanceMode::Auto: return QStringLiteral("auto");
    }
    return QStringLiteral("auto");
}

AppearanceMode appearanceModeFromName(const QString &name) {
    if (name == QStringLiteral("light")) return AppearanceMode::Light;
    if (name == QStringLiteral("dark")) return AppearanceMode::Dark;
    return AppearanceMode::Auto;
}

} // namespace pacsmith
