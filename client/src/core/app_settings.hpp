#pragma once

#include <optional>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QTime>

namespace pacsmith {

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
    QString registryId{};
    QString registryVersion{};
    QMap<QString, QString> environment{};
    QJsonObject configDefaults{};
};

struct AppSettings {
    AppearanceSettings appearance;
    BackgroundUpdateSettings updates;
    std::optional<HarnessProfile> harness;
    bool githubTokenConfigured{false};
    bool debAssociationPrompted{false};
    bool selfTrackingPrompted{false};

    [[nodiscard]] const HarnessProfile *configuredHarness() const;
};

class AppSettingsStore final {
public:
    AppSettingsStore();
    explicit AppSettingsStore(QString configDirectory);

    [[nodiscard]] static QString defaultConfigDirectory();
    [[nodiscard]] AppSettings load(QString *error = nullptr) const;
    [[nodiscard]] bool save(const AppSettings &settings, QString *error = nullptr) const;
    [[nodiscard]] bool setHarness(const HarnessProfile &harness, QString *error = nullptr) const;
    [[nodiscard]] bool clearHarness(QString *error = nullptr) const;
    [[nodiscard]] QString settingsPath() const;

private:
    QString directory_;
};

[[nodiscard]] QString appearanceModeName(AppearanceMode mode);
[[nodiscard]] AppearanceMode appearanceModeFromName(const QString &name);

} // namespace pacsmith
