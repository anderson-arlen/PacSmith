#include "core/app_settings.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <utility>

namespace pacsmith {

AppSettingsStore::AppSettingsStore() : directory_(defaultConfigDirectory()) {}

AppSettingsStore::AppSettingsStore(QString configDirectory) : directory_(std::move(configDirectory)) {}

QString AppSettingsStore::defaultConfigDirectory() {
    const auto xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (!xdg.isEmpty() && QDir::isAbsolutePath(xdg)) return QDir(xdg).filePath(QStringLiteral("pacsmith"));
    return QDir::home().filePath(QStringLiteral(".config/pacsmith"));
}

AiSettings AppSettingsStore::load(QString *error) const {
    AiSettings result;
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
    result.provider = aiProviderFromName(object.value(QStringLiteral("provider")).toString());
    result.model = object.value(QStringLiteral("model")).toString();
    result.reasoningEffort = aiReasoningEffortFromName(
        object.value(QStringLiteral("reasoningEffort")).toString());
    result.executionMode = aiExecutionModeFromName(
        object.value(QStringLiteral("executionMode")).toString());
    result.automaticallyResolveReviewItems =
        object.value(QStringLiteral("automaticallyResolveReviewItems")).toBool();
    const auto credentials = object.value(QStringLiteral("credentialSources")).toObject();
    for (auto iterator = credentials.constBegin(); iterator != credentials.constEnd(); ++iterator) {
        result.credentialSources.insert(iterator.key(), credentialSourceFromName(iterator.value().toString()));
    }
    const auto updates = object.value(QStringLiteral("updates")).toObject();
    result.updates.enabled = updates.value(QStringLiteral("enabled")).toBool(false);
    result.updates.startAtLogin = updates.value(QStringLiteral("startAtLogin")).toBool(false);
    result.updates.startMinimized = updates.value(QStringLiteral("startMinimized")).toBool(false);
    result.updates.keepInTray =
        updates.value(QStringLiteral("keepInTray")).toBool(result.updates.startMinimized);
    result.updates.daily = updates.value(QStringLiteral("daily")).toBool(true);
    result.updates.weekDay = std::clamp(updates.value(QStringLiteral("weekDay")).toInt(1), 1, 7);
    const auto time = QTime::fromString(updates.value(QStringLiteral("localTime")).toString(),
                                       QStringLiteral("HH:mm"));
    if (time.isValid()) result.updates.localTime = time;
    result.updates.automaticallyPrepare =
        updates.value(QStringLiteral("automaticallyPrepare")).toBool(false);
    result.updates.retainedPackageVersions =
        std::max(-1, updates.value(QStringLiteral("retainedPackageVersions")).toInt(2));
    result.updates.retainedCompleteReleases =
        std::max(-1, updates.value(QStringLiteral("retainedCompleteReleases")).toInt(3));
    if (object.contains(QStringLiteral("githubTokenConfigured"))) {
        result.githubTokenConfigured = object.value(QStringLiteral("githubTokenConfigured")).toBool();
    } else {
        result.githubTokenConfigured =
            result.credentialSources.value(QStringLiteral("github"), CredentialSource::Environment) ==
            CredentialSource::Age;
    }
    const auto onboarding = object.value(QStringLiteral("onboarding")).toObject();
    result.debAssociationPrompted = onboarding.value(QStringLiteral("debAssociationPrompted")).toBool(false);
    result.selfTrackingPrompted = onboarding.value(QStringLiteral("selfTrackingPrompted")).toBool(false);
    return result;
}

bool AppSettingsStore::save(const AiSettings &settings, QString *error) const {
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
        : std::max(settings.updates.retainedCompleteReleases,
                   settings.updates.retainedPackageVersions);
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
    const QJsonObject onboarding{{QStringLiteral("debAssociationPrompted"),
                                  settings.debAssociationPrompted},
                                 {QStringLiteral("selfTrackingPrompted"),
                                  settings.selfTrackingPrompted}};
    const QJsonObject object{{QStringLiteral("formatVersion"), 4},
                             {QStringLiteral("provider"), aiProviderName(settings.provider)},
                             {QStringLiteral("model"), settings.model},
                             {QStringLiteral("reasoningEffort"),
                              aiReasoningEffortName(settings.reasoningEffort)},
                             {QStringLiteral("executionMode"),
                              aiExecutionModeName(settings.executionMode)},
                             {QStringLiteral("automaticallyResolveReviewItems"),
                              settings.automaticallyResolveReviewItems},
                             {QStringLiteral("credentialSources"), credentials},
                             {QStringLiteral("githubTokenConfigured"), settings.githubTokenConfigured},
                             {QStringLiteral("updates"), updates},
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

QString AppSettingsStore::ageSecretsPath() const {
    return QDir(directory_).filePath(QStringLiteral("secrets.age"));
}

QString aiProviderName(const AiProviderKind provider) {
    switch (provider) {
    case AiProviderKind::ChatGpt: return QStringLiteral("chatgpt");
    case AiProviderKind::OpenAi: return QStringLiteral("openai");
    case AiProviderKind::Xai: return QStringLiteral("xai");
    case AiProviderKind::None: return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

AiProviderKind aiProviderFromName(const QString &name) {
    if (name == QStringLiteral("chatgpt")) return AiProviderKind::ChatGpt;
    if (name == QStringLiteral("openai")) return AiProviderKind::OpenAi;
    if (name == QStringLiteral("xai")) return AiProviderKind::Xai;
    return AiProviderKind::None;
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

QString aiReasoningEffortName(const AiReasoningEffort effort) {
    switch (effort) {
    case AiReasoningEffort::None: return QStringLiteral("none");
    case AiReasoningEffort::Low: return QStringLiteral("low");
    case AiReasoningEffort::Medium: return QStringLiteral("medium");
    case AiReasoningEffort::High: return QStringLiteral("high");
    case AiReasoningEffort::XHigh: return QStringLiteral("xhigh");
    case AiReasoningEffort::Max: return QStringLiteral("max");
    case AiReasoningEffort::ProviderDefault: return QString{};
    }
    return {};
}

AiReasoningEffort aiReasoningEffortFromName(const QString &name) {
    if (name == QStringLiteral("none")) return AiReasoningEffort::None;
    if (name == QStringLiteral("low")) return AiReasoningEffort::Low;
    if (name == QStringLiteral("medium")) return AiReasoningEffort::Medium;
    if (name == QStringLiteral("high")) return AiReasoningEffort::High;
    if (name == QStringLiteral("xhigh")) return AiReasoningEffort::XHigh;
    if (name == QStringLiteral("max")) return AiReasoningEffort::Max;
    return AiReasoningEffort::ProviderDefault;
}

QString aiExecutionModeName(const AiExecutionMode mode) {
    return mode == AiExecutionMode::Fast ? QStringLiteral("fast")
                                         : QStringLiteral("standard");
}

AiExecutionMode aiExecutionModeFromName(const QString &name) {
    return name == QStringLiteral("fast") ? AiExecutionMode::Fast
                                          : AiExecutionMode::Standard;
}

bool githubTokenUsesAge(const AiSettings &settings) {
    return settings.githubTokenConfigured &&
           settings.credentialSources.value(QStringLiteral("github"), CredentialSource::Environment) ==
               CredentialSource::Age;
}

} // namespace pacsmith
