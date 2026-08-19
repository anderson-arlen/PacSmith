#pragma once

#include "core/app_settings.hpp"

#include <QMap>
#include <QProcessEnvironment>
#include <QString>

#include <optional>

namespace pacsmith {

class CredentialStore final {
public:
    explicit CredentialStore(QString ageSecretsPath);
    ~CredentialStore();

    CredentialStore(const CredentialStore &) = delete;
    CredentialStore &operator=(const CredentialStore &) = delete;

    [[nodiscard]] static bool keyringAvailable(QString *error = nullptr);
    [[nodiscard]] static bool ageAvailable();
    [[nodiscard]] bool hasAgeFile() const;
    [[nodiscard]] bool ageUnlocked() const noexcept;
    [[nodiscard]] bool createAge(const QString &password, QString *error = nullptr);
    [[nodiscard]] bool unlockAge(const QString &password, QString *error = nullptr);
    void lockAge();

    [[nodiscard]] bool store(const QString &provider, CredentialSource source,
                             const QString &secret, const QString &agePassword = {},
                             QString *error = nullptr);
    [[nodiscard]] bool remove(const QString &provider, CredentialSource source,
                              const QString &agePassword = {}, QString *error = nullptr);
    [[nodiscard]] std::optional<QString> load(const QString &provider, CredentialSource source,
                                              QString *error = nullptr) const;
    [[nodiscard]] QProcessEnvironment environmentWithGithubToken(CredentialSource source) const;

private:
    [[nodiscard]] bool saveAge(const QString &password, QString *error);
    [[nodiscard]] QString effectiveAgePassword(const QString &provided) const;
    void rememberAgePassword(const QString &password);

    QString ageSecretsPath_;
    QMap<QString, QString> ageSecrets_;
    QString rememberedAgePassword_;
    bool ageUnlocked_{false};
};

} // namespace pacsmith
