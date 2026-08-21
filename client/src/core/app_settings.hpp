#pragma once

#include <QMap>
#include <QString>
#include <QTime>

namespace pacsmith {

enum class AiProviderKind { None, ChatGpt, OpenAi, Xai };
enum class CredentialSource { Environment, Keyring, Age };
enum class AiReasoningEffort { ProviderDefault, None, Low, Medium, High, XHigh, Max };
enum class AiExecutionMode { Standard, Fast };

struct BackgroundUpdateSettings {
    bool enabled{false};
    bool startAtLogin{false};
    bool startMinimized{false};
    bool keepInTray{false};
    bool daily{true};
    // Qt weekday: Monday=1 through Sunday=7. Ignored for a daily schedule.
    int weekDay{1};
    QTime localTime{2, 0};
    bool automaticallyPrepare{false};
    int retainedPackageVersions{2};
    int retainedCompleteReleases{3};
};

struct AiSettings {
    AiProviderKind provider{AiProviderKind::None};
    QString model;
    AiReasoningEffort reasoningEffort{AiReasoningEffort::ProviderDefault};
    AiExecutionMode executionMode{AiExecutionMode::Standard};
    bool automaticallyResolveReviewItems{false};
    QMap<QString, CredentialSource> credentialSources;
    BackgroundUpdateSettings updates;
    bool githubTokenConfigured{false};
    bool debAssociationPrompted{false};
    bool selfTrackingPrompted{false};
};

class AppSettingsStore final {
public:
    AppSettingsStore();
    explicit AppSettingsStore(QString configDirectory);

    [[nodiscard]] static QString defaultConfigDirectory();
    [[nodiscard]] AiSettings load(QString *error = nullptr) const;
    [[nodiscard]] bool save(const AiSettings &settings, QString *error = nullptr) const;
    [[nodiscard]] QString ageSecretsPath() const;

private:
    QString directory_;
};

[[nodiscard]] QString aiProviderName(AiProviderKind provider);
[[nodiscard]] AiProviderKind aiProviderFromName(const QString &name);
[[nodiscard]] QString credentialSourceName(CredentialSource source);
[[nodiscard]] CredentialSource credentialSourceFromName(const QString &name);
[[nodiscard]] QString aiReasoningEffortName(AiReasoningEffort effort);
[[nodiscard]] AiReasoningEffort aiReasoningEffortFromName(const QString &name);
[[nodiscard]] QString aiExecutionModeName(AiExecutionMode mode);
[[nodiscard]] AiExecutionMode aiExecutionModeFromName(const QString &name);
[[nodiscard]] bool githubTokenUsesAge(const AiSettings &settings);

} // namespace pacsmith
