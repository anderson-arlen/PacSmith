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

const HarnessProfile *AppSettings::defaultHarness() const {
    for (const auto &profile : harnessProfiles) {
        if (profile.isDefault) return &profile;
    }
    return harnessProfiles.isEmpty() ? nullptr : &harnessProfiles.first();
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
    const auto credentials = object.value(QStringLiteral("credentialSources")).toObject();
    for (auto iterator = credentials.constBegin(); iterator != credentials.constEnd(); ++iterator) {
        result.credentialSources.insert(iterator.key(), credentialSourceFromName(iterator.value().toString()));
    }
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
    result.updates.retainedPackageVersions = std::max(-1, updates.value(QStringLiteral("retainedPackageVersions")).toInt(2));
    result.updates.retainedCompleteReleases = std::max(-1, updates.value(QStringLiteral("retainedCompleteReleases")).toInt(3));
    result.githubTokenConfigured = object.value(QStringLiteral("githubTokenConfigured")).toBool(false) ||
                                   (!object.contains(QStringLiteral("githubTokenConfigured")) &&
                                    result.credentialSources.contains(QStringLiteral("github")));
    for (const auto &value : object.value(QStringLiteral("harnessProfiles")).toArray()) {
        const auto profileObject = value.toObject();
        HarnessProfile profile;
        profile.name = profileObject.value(QStringLiteral("name")).toString().trimmed();
        profile.executable = profileObject.value(QStringLiteral("executable")).toString().trimmed();
        for (const auto &argument : profileObject.value(QStringLiteral("arguments")).toArray()) {
            profile.arguments.append(argument.toString());
        }
        profile.isDefault = profileObject.value(QStringLiteral("default")).toBool(false);
        if (!profile.name.isEmpty() && !profile.executable.isEmpty()) result.harnessProfiles.append(profile);
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
    QJsonObject credentials;
    for (auto iterator = settings.credentialSources.cbegin(); iterator != settings.credentialSources.cend(); ++iterator) {
        credentials.insert(iterator.key(), credentialSourceName(iterator.value()));
    }
    const auto completeRetention = settings.updates.retainedPackageVersions < 0 ||
                                   settings.updates.retainedCompleteReleases < 0
        ? -1
        : std::max(settings.updates.retainedCompleteReleases, settings.updates.retainedPackageVersions);
    const QJsonObject updates{{QStringLiteral("enabled"), settings.updates.enabled},
                              {QStringLiteral("startAtLogin"), settings.updates.startAtLogin},
                              {QStringLiteral("startMinimized"), settings.updates.startMinimized},
                              {QStringLiteral("keepInTray"), settings.updates.keepInTray},
                              {QStringLiteral("daily"), settings.updates.daily},
                              {QStringLiteral("weekDay"), settings.updates.weekDay},
                              {QStringLiteral("localTime"), settings.updates.localTime.toString(QStringLiteral("HH:mm"))},
                              {QStringLiteral("automaticallyPrepare"), settings.updates.automaticallyPrepare},
                              {QStringLiteral("retainedPackageVersions"), settings.updates.retainedPackageVersions},
                              {QStringLiteral("retainedCompleteReleases"), completeRetention}};
    QJsonArray profiles;
    for (const auto &profile : settings.harnessProfiles) {
        QJsonArray arguments;
        for (const auto &argument : profile.arguments) arguments.append(argument);
        profiles.append(QJsonObject{{QStringLiteral("name"), profile.name},
                                    {QStringLiteral("executable"), profile.executable},
                                    {QStringLiteral("arguments"), arguments},
                                    {QStringLiteral("default"), profile.isDefault}});
    }
    const QJsonObject onboarding{{QStringLiteral("debAssociationPrompted"), settings.debAssociationPrompted},
                                 {QStringLiteral("selfTrackingPrompted"), settings.selfTrackingPrompted}};
    const QJsonObject object{{QStringLiteral("formatVersion"), 5},
                             {QStringLiteral("credentialSources"), credentials},
                             {QStringLiteral("githubTokenConfigured"), settings.githubTokenConfigured},
                             {QStringLiteral("updates"), updates},
                             {QStringLiteral("harnessProfiles"), profiles},
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

bool AppSettingsStore::upsertHarnessProfile(const HarnessProfile &profile, QString *error) const {
    HarnessProfile normalized = profile;
    normalized.name = normalized.name.trimmed();
    normalized.executable = normalized.executable.trimmed();
    if (normalized.name.isEmpty() || normalized.executable.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Harness profile name and executable are required");
        return false;
    }
    if (normalized.name.contains(QChar::Null) || normalized.executable.contains(QChar::Null) ||
        std::any_of(normalized.arguments.cbegin(), normalized.arguments.cend(),
                    [](const auto &argument) { return argument.contains(QChar::Null); })) {
        if (error != nullptr) *error = QStringLiteral("Harness profile values cannot contain NUL characters");
        return false;
    }

    auto settings = load(error);
    if (error != nullptr && !error->isEmpty()) return false;
    auto existing = std::find_if(settings.harnessProfiles.begin(), settings.harnessProfiles.end(),
                                 [&](const auto &candidate) {
                                     return candidate.name.compare(normalized.name, Qt::CaseInsensitive) == 0;
                                 });
    if (normalized.isDefault) {
        for (auto &candidate : settings.harnessProfiles) candidate.isDefault = false;
    } else if (existing != settings.harnessProfiles.end()) {
        normalized.isDefault = existing->isDefault;
    }
    if (existing == settings.harnessProfiles.end()) settings.harnessProfiles.append(normalized);
    else *existing = normalized;
    const auto hasDefault = std::any_of(settings.harnessProfiles.cbegin(),
                                        settings.harnessProfiles.cend(),
                                        [](const auto &candidate) { return candidate.isDefault; });
    if (!hasDefault && !settings.harnessProfiles.isEmpty()) {
        settings.harnessProfiles.first().isDefault = true;
    }
    return save(settings, error);
}

bool AppSettingsStore::removeHarnessProfile(const QString &name, QString *error) const {
    auto settings = load(error);
    if (error != nullptr && !error->isEmpty()) return false;
    const auto normalized = name.trimmed();
    const auto removed = settings.harnessProfiles.removeIf([&](const auto &candidate) {
        return candidate.name.compare(normalized, Qt::CaseInsensitive) == 0;
    });
    if (removed == 0) {
        if (error != nullptr) *error = QStringLiteral("Harness profile not found");
        return false;
    }
    const auto hasDefault = std::any_of(settings.harnessProfiles.cbegin(),
                                        settings.harnessProfiles.cend(),
                                        [](const auto &candidate) { return candidate.isDefault; });
    if (!hasDefault && !settings.harnessProfiles.isEmpty()) {
        settings.harnessProfiles.first().isDefault = true;
    }
    return save(settings, error);
}

bool AppSettingsStore::setDefaultHarnessProfile(const QString &name, QString *error) const {
    auto settings = load(error);
    if (error != nullptr && !error->isEmpty()) return false;
    const auto normalized = name.trimmed();
    auto selected = std::find_if(settings.harnessProfiles.begin(), settings.harnessProfiles.end(),
                                 [&](const auto &candidate) {
                                     return candidate.name.compare(normalized, Qt::CaseInsensitive) == 0;
                                 });
    if (selected == settings.harnessProfiles.end()) {
        if (error != nullptr) *error = QStringLiteral("Harness profile not found");
        return false;
    }
    for (auto &candidate : settings.harnessProfiles) candidate.isDefault = false;
    selected->isDefault = true;
    return save(settings, error);
}

QString AppSettingsStore::ageSecretsPath() const {
    return QDir(directory_).filePath(QStringLiteral("secrets.age"));
}

QString AppSettingsStore::settingsPath() const {
    return QDir(directory_).filePath(QStringLiteral("settings.json"));
}

QString credentialSourceName(const CredentialSource source) {
    switch (source) {
    case CredentialSource::Keyring: return QStringLiteral("keyring");
    case CredentialSource::Age: return QStringLiteral("age");
    case CredentialSource::Environment: return QStringLiteral("environment");
    }
    return QStringLiteral("environment");
}

CredentialSource credentialSourceFromName(const QString &name) {
    if (name == QStringLiteral("keyring")) return CredentialSource::Keyring;
    if (name == QStringLiteral("age")) return CredentialSource::Age;
    return CredentialSource::Environment;
}

bool githubTokenUsesAge(const AppSettings &settings) {
    return settings.githubTokenConfigured &&
           settings.credentialSources.value(QStringLiteral("github"), CredentialSource::Environment) ==
               CredentialSource::Age;
}

} // namespace pacsmith
