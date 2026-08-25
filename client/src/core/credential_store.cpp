#include "core/credential_store.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef PACSMITH_HAS_LIBSECRET
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")
#endif

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <utility>

namespace pacsmith {
namespace {

#ifdef PACSMITH_HAS_LIBSECRET
const SecretSchema credentialSchema{
    "org.pacsmith.Credentials", SECRET_SCHEMA_NONE,
    {{"provider", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
    0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
#endif

bool writeAll(const int descriptor, const QByteArray &data) {
    qsizetype written = 0;
    while (written < data.size()) {
        const auto count = ::write(descriptor, data.constData() + written,
                                   static_cast<size_t>(data.size() - written));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        written += static_cast<qsizetype>(count);
    }
    return true;
}

bool runAge(const bool encrypt, const QString &encryptedPath, const QByteArray &input,
            const QString &password, QByteArray &output, QString &error) {
    if (password.isEmpty()) {
        error = QStringLiteral("The age password cannot be empty");
        return false;
    }
    if (input.size() > 64 * 1024) {
        error = QStringLiteral("PacSmith's encrypted credential payload exceeds 64 KiB");
        return false;
    }
    int inputPipe[2]{-1, -1};
    int outputPipe[2]{-1, -1};
    if (::pipe(inputPipe) != 0 || ::pipe(outputPipe) != 0) {
        error = QStringLiteral("Could not create private pipes for age: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    int terminal = -1;
    const auto child = ::forkpty(&terminal, nullptr, nullptr, nullptr);
    if (child < 0) {
        error = QStringLiteral("Could not start age: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        ::close(inputPipe[0]); ::close(inputPipe[1]); ::close(outputPipe[0]); ::close(outputPipe[1]);
        return false;
    }
    if (child == 0) {
        if (encrypt) ::dup2(inputPipe[0], STDIN_FILENO);
        ::dup2(outputPipe[1], STDOUT_FILENO);
        ::close(inputPipe[0]); ::close(inputPipe[1]); ::close(outputPipe[0]); ::close(outputPipe[1]);
        if (encrypt) {
            ::execl("/usr/bin/age", "age", "--encrypt", "--passphrase", static_cast<char *>(nullptr));
        } else {
            const auto encoded = encryptedPath.toLocal8Bit();
            ::execl("/usr/bin/age", "age", "--decrypt", encoded.constData(), static_cast<char *>(nullptr));
        }
        _exit(127);
    }

    ::close(inputPipe[0]);
    ::close(outputPipe[1]);
    bool inputOpen = true;
    bool inputSent = !encrypt;
    if (!encrypt) {
        ::close(inputPipe[1]);
        inputOpen = false;
    }

    QByteArray terminalOutput;
    const auto passwordLine = password.toUtf8() + '\n';
    int passwordsSent = 0;
    const int requiredPasswords = encrypt ? 2 : 1;
    bool outputOpen = true;
    bool terminalOpen = true;
    int status = 0;
    bool exited = false;
    QElapsedTimer timer;
    timer.start();
    while ((!exited || outputOpen || terminalOpen) && timer.elapsed() < 30000) {
        pollfd descriptors[2]{{terminal, POLLIN | POLLHUP, 0}, {outputPipe[0], POLLIN | POLLHUP, 0}};
        static_cast<void>(::poll(descriptors, 2, 100));
        char buffer[4096];
        if (terminalOpen && (descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
            const auto count = ::read(terminal, buffer, sizeof(buffer));
            if (count > 0) {
                terminalOutput.append(buffer, static_cast<qsizetype>(count));
                const auto prompts = terminalOutput.count("passphrase") + terminalOutput.count("Passphrase");
                while (passwordsSent < requiredPasswords && prompts > passwordsSent) {
                    if (!writeAll(terminal, passwordLine)) break;
                    ++passwordsSent;
                }
                if (encrypt && passwordsSent == requiredPasswords && !inputSent) {
                    if (!writeAll(inputPipe[1], input)) {
                        error = QStringLiteral("Could not send credentials to age");
                    }
                    ::close(inputPipe[1]);
                    inputOpen = false;
                    inputSent = true;
                }
            } else if (count == 0 || (count < 0 && errno == EIO)) {
                terminalOpen = false;
            }
        }
        if (outputOpen && (descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
            const auto count = ::read(outputPipe[0], buffer, sizeof(buffer));
            if (count > 0) output.append(buffer, static_cast<qsizetype>(count));
            else if (count == 0) outputOpen = false;
        }
        if (!exited) {
            const auto wait = ::waitpid(child, &status, WNOHANG);
            exited = wait == child;
        }
    }
    if (inputOpen) ::close(inputPipe[1]);
    if (!exited) {
        ::kill(child, SIGKILL);
        static_cast<void>(::waitpid(child, &status, 0));
        error = QStringLiteral("age timed out");
    }
    ::close(terminal);
    ::close(outputPipe[0]);
    auto mutablePassword = passwordLine;
    mutablePassword.fill('\0');
    if (!error.isEmpty()) return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        auto message = QString::fromUtf8(terminalOutput).trimmed();
        message.replace(QRegularExpression(QStringLiteral("(?i)passphrase.*")), QStringLiteral("credential operation failed"));
        error = message.isEmpty() ? QStringLiteral("age rejected the credential password") : message;
        return false;
    }
    return true;
}

} // namespace

CredentialStore::CredentialStore(QString ageSecretsPath) : ageSecretsPath_(std::move(ageSecretsPath)) {}

CredentialStore::~CredentialStore() { lockAge(); }

bool CredentialStore::keyringAvailable(QString *error) {
#ifdef PACSMITH_HAS_LIBSECRET
    GError *failure = nullptr;
    auto *service = secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &failure);
    if (service != nullptr) {
        g_object_unref(service);
        return true;
    }
    if (error != nullptr) {
        *error = failure != nullptr ? QString::fromUtf8(failure->message)
                                   : QStringLiteral("No Secret Service is available");
    }
    if (failure != nullptr) g_error_free(failure);
#else
    if (error != nullptr) *error = QStringLiteral("PacSmith was built without libsecret support");
#endif
    return false;
}

bool CredentialStore::ageAvailable() {
    return QFileInfo(QStringLiteral("/usr/bin/age")).isExecutable();
}

bool CredentialStore::hasAgeFile() const { return QFileInfo::exists(ageSecretsPath_); }
bool CredentialStore::ageUnlocked() const noexcept { return ageUnlocked_; }

bool CredentialStore::createAge(const QString &password, QString *error) {
    if (hasAgeFile()) {
        if (error != nullptr) *error = QStringLiteral("The encrypted credential store already exists");
        return false;
    }
    if (password.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("The age password cannot be empty");
        return false;
    }
    ageSecrets_.clear();
    ageUnlocked_ = true;
    if (saveAge(password, error)) {
        rememberAgePassword(password);
        return true;
    }
    ageUnlocked_ = false;
    return false;
}

bool CredentialStore::unlockAge(const QString &password, QString *error) {
    if (!hasAgeFile()) {
        if (error != nullptr) {
            *error = QStringLiteral("No encrypted credential store exists yet; create it first");
        }
        return false;
    }
    QByteArray plaintext;
    QString failure;
    if (!runAge(false, ageSecretsPath_, {}, password, plaintext, failure)) {
        if (error != nullptr) *error = failure;
        plaintext.fill('\0');
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(plaintext, &parseError);
    plaintext.fill('\0');
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("The decrypted credential file is not valid PacSmith JSON");
        return false;
    }
    ageSecrets_.clear();
    const auto object = document.object();
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        ageSecrets_.insert(iterator.key(), iterator.value().toString());
    }
    ageUnlocked_ = true;
    rememberAgePassword(password);
    return true;
}

void CredentialStore::lockAge() {
    for (auto iterator = ageSecrets_.begin(); iterator != ageSecrets_.end(); ++iterator) {
        iterator.value().fill(QChar::Null);
    }
    ageSecrets_.clear();
    rememberedAgePassword_.fill(QChar::Null);
    rememberedAgePassword_.clear();
    ageUnlocked_ = false;
}

QString CredentialStore::effectiveAgePassword(const QString &provided) const {
    return provided.isEmpty() ? rememberedAgePassword_ : provided;
}

void CredentialStore::rememberAgePassword(const QString &password) {
    rememberedAgePassword_.fill(QChar::Null);
    rememberedAgePassword_ = password;
}

bool CredentialStore::saveAge(const QString &password, QString *error) {
    QJsonObject object;
    for (auto iterator = ageSecrets_.cbegin(); iterator != ageSecrets_.cend(); ++iterator) {
        object.insert(iterator.key(), iterator.value());
    }
    auto plaintext = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray encrypted;
    QString failure;
    const auto succeeded = runAge(true, {}, plaintext, password, encrypted, failure);
    plaintext.fill('\0');
    if (!succeeded) {
        if (error != nullptr) *error = failure;
        return false;
    }
    if (!QDir{}.mkpath(QFileInfo(ageSecretsPath_).absolutePath())) {
        if (error != nullptr) *error = QStringLiteral("Could not create the credential directory");
        return false;
    }
    QSaveFile file(ageSecretsPath_);
    if (!file.open(QIODevice::WriteOnly) || file.write(encrypted) != encrypted.size() || !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    QFile::setPermissions(ageSecretsPath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool CredentialStore::store(const QString &provider, const CredentialSource source,
                            const QString &secret, const QString &agePassword, QString *error) {
    if (provider.isEmpty() || secret.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Provider and credential are required");
        return false;
    }
    if (source == CredentialSource::Environment) {
        if (error != nullptr) *error = QStringLiteral("Environment credentials cannot be written by PacSmith");
        return false;
    }
    if (source == CredentialSource::Age) {
        const auto password = effectiveAgePassword(agePassword);
        if (!ageUnlocked_) {
            const auto ready = hasAgeFile() ? unlockAge(password, error)
                                            : createAge(password, error);
            if (!ready) return false;
        }
        ageSecrets_.insert(provider, secret);
        return saveAge(effectiveAgePassword(password), error);
    }
#ifdef PACSMITH_HAS_LIBSECRET
    GError *failure = nullptr;
    const auto label = QStringLiteral("PacSmith %1 credential").arg(provider);
    const auto succeeded = secret_password_store_sync(
        &credentialSchema, SECRET_COLLECTION_DEFAULT, label.toUtf8().constData(),
        secret.toUtf8().constData(), nullptr, &failure, "provider", provider.toUtf8().constData(), nullptr);
    if (!succeeded && error != nullptr) {
        *error = failure != nullptr ? QString::fromUtf8(failure->message)
                                   : QStringLiteral("Secret Service rejected the credential");
    }
    if (failure != nullptr) g_error_free(failure);
    return succeeded;
#else
    if (error != nullptr) *error = QStringLiteral("PacSmith was built without libsecret support");
    return false;
#endif
}

bool CredentialStore::remove(const QString &provider, const CredentialSource source,
                             const QString &agePassword, QString *error) {
    if (provider.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Provider is required");
        return false;
    }
    if (source == CredentialSource::Environment) {
        if (error != nullptr) *error = QStringLiteral("Environment credentials are not owned by PacSmith");
        return false;
    }
    if (source == CredentialSource::Age) {
        const auto password = effectiveAgePassword(agePassword);
        if (!ageUnlocked_ && !unlockAge(password, error)) return false;
        auto removed = ageSecrets_.take(provider);
        removed.fill(QChar::Null);
        return saveAge(effectiveAgePassword(password), error);
    }
#ifdef PACSMITH_HAS_LIBSECRET
    GError *failure = nullptr;
    const auto succeeded = secret_password_clear_sync(
        &credentialSchema, nullptr, &failure, "provider", provider.toUtf8().constData(), nullptr);
    if (!succeeded && error != nullptr) {
        *error = failure != nullptr ? QString::fromUtf8(failure->message)
                                   : QStringLiteral("Secret Service could not remove the credential");
    }
    if (failure != nullptr) g_error_free(failure);
    return succeeded;
#else
    if (error != nullptr) *error = QStringLiteral("PacSmith was built without libsecret support");
    return false;
#endif
}

std::optional<QString> CredentialStore::load(const QString &provider, const CredentialSource source,
                                             QString *error) const {
    if (source == CredentialSource::Environment) {
        if (provider != QStringLiteral("github")) {
            if (error != nullptr) *error = QStringLiteral("Only the GitHub token has an environment source");
            return std::nullopt;
        }
        const auto variable = "PACSMITH_GITHUB_TOKEN";
        const auto value = qEnvironmentVariable(variable);
        if (value.isEmpty() && error != nullptr) *error = QStringLiteral("%1 is not set").arg(QString::fromLatin1(variable));
        return value.isEmpty() ? std::nullopt : std::optional<QString>{value};
    }
    if (source == CredentialSource::Age) {
        if (!ageUnlocked_) {
            if (provider == QStringLiteral("github")) {
                const auto injected = qEnvironmentVariable("PACSMITH_GITHUB_TOKEN");
                if (!injected.isEmpty()) return injected;
            }
            if (error != nullptr) *error = QStringLiteral("Age-encrypted credentials are locked");
            return std::nullopt;
        }
        const auto iterator = ageSecrets_.constFind(provider);
        if (iterator == ageSecrets_.cend() || iterator.value().isEmpty()) {
            if (error != nullptr) *error = QStringLiteral("No age-encrypted credential is stored for %1").arg(provider);
            return std::nullopt;
        }
        return iterator.value();
    }
#ifdef PACSMITH_HAS_LIBSECRET
    GError *failure = nullptr;
    auto *password = secret_password_lookup_sync(&credentialSchema, nullptr, &failure,
                                                 "provider", provider.toUtf8().constData(), nullptr);
    if (password == nullptr) {
        if (error != nullptr) {
            *error = failure != nullptr ? QString::fromUtf8(failure->message)
                                       : QStringLiteral("No keyring credential is stored for %1").arg(provider);
        }
        if (failure != nullptr) g_error_free(failure);
        return std::nullopt;
    }
    const auto result = QString::fromUtf8(password);
    secret_password_free(password);
    if (failure != nullptr) g_error_free(failure);
    return result;
#else
    if (error != nullptr) *error = QStringLiteral("PacSmith was built without libsecret support");
    return std::nullopt;
#endif
}

QProcessEnvironment CredentialStore::environmentWithGithubToken(const CredentialSource source) const {
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto token = load(QStringLiteral("github"), source, nullptr);
    if (token && !token->isEmpty()) {
        environment.insert(QStringLiteral("PACSMITH_GITHUB_TOKEN"), *token);
    }
    return environment;
}

} // namespace pacsmith
