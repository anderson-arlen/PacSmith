#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QTime>

namespace pacsmith {

enum class CredentialSource { Environment, Keyring, Age };
enum class AppearanceMode { Auto, Light, Dark };

struct AppearanceSettings {
    AppearanceMode interfaceTheme{AppearanceMode::Auto};
    AppearanceMode trayTheme{AppearanceMode::Auto};
};

struct BackgroundUpdateSettings {
    bool enabled{false};
    bool startAtLogin{false};
    bool startMinimized{false};
    bool keepInTray{false};
    bool daily{true};
    int weekDay{1};
    QTime localTime{2, 0};
    bool automaticallyPrepare{false};
    int retentionVersions{2};
};

struct HarnessProfile {
    QString name;
    QString executable;
    QStringList arguments;
    bool isDefault{false};
};

struct AppSettings {
    QMap<QString, CredentialSource> credentialSources;
    AppearanceSettings appearance;
    BackgroundUpdateSettings updates;
    QList<HarnessProfile> harnessProfiles;
    bool githubTokenConfigured{false};
    bool debAssociationPrompted{false};
    bool selfTrackingPrompted{false};

    [[nodiscard]] const HarnessProfile *defaultHarness() const;
};

class AppSettingsStore final {
public:
    AppSettingsStore();
    explicit AppSettingsStore(QString configDirectory);

    [[nodiscard]] static QString defaultConfigDirectory();
    [[nodiscard]] AppSettings load(QString *error = nullptr) const;
    [[nodiscard]] bool save(const AppSettings &settings, QString *error = nullptr) const;
    [[nodiscard]] bool upsertHarnessProfile(const HarnessProfile &profile,
                                            QString *error = nullptr) const;
    [[nodiscard]] bool removeHarnessProfile(const QString &name,
                                            QString *error = nullptr) const;
    [[nodiscard]] bool setDefaultHarnessProfile(const QString &name,
                                                QString *error = nullptr) const;
    [[nodiscard]] QString settingsPath() const;
    [[nodiscard]] QString ageSecretsPath() const;

private:
    QString directory_;
};

[[nodiscard]] QString credentialSourceName(CredentialSource source);
[[nodiscard]] CredentialSource credentialSourceFromName(const QString &name);
[[nodiscard]] QString appearanceModeName(AppearanceMode mode);
[[nodiscard]] AppearanceMode appearanceModeFromName(const QString &name);
[[nodiscard]] bool githubTokenUsesAge(const AppSettings &settings);

} // namespace pacsmith
