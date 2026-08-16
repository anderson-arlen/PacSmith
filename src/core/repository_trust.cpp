#include "core/repository_trust.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace pacsmith {
namespace {

QString fromPath(const std::filesystem::path &path) {
    return QString::fromUtf8(path.string().c_str());
}

bool writeFile(const std::filesystem::path &path, const QByteArray &contents, QString *error) {
    QSaveFile file(fromPath(path));
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    return true;
}

std::optional<RepositorySigningKey> storeKey(const std::filesystem::path &projectDirectory,
                                             const QByteArray &contents, const QString &source,
                                             const QString &sourceFingerprint, const bool trusted,
                                             const ValueOrigin origin, QString *error) {
    if (contents.isEmpty() || contents.size() > 4 * 1024 * 1024) {
        if (error != nullptr) *error = QStringLiteral("Signing key is empty or exceeds the 4 MiB limit");
        return std::nullopt;
    }
    const auto sha = QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
    const auto relative = QStringLiteral("files/keys/vendor-%1.gpg").arg(sha.left(16));
    const auto destination = projectDirectory / std::filesystem::path(relative.toUtf8().constData());
    std::error_code filesystemError;
    std::filesystem::create_directories(destination.parent_path(), filesystemError);
    if (filesystemError) {
        if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
        return std::nullopt;
    }

    if (contents.startsWith("-----BEGIN PGP PUBLIC KEY BLOCK-----")) {
        const auto gpg = QStandardPaths::findExecutable(QStringLiteral("gpg"));
        if (gpg.isEmpty()) {
            if (error != nullptr) *error = QStringLiteral("gpg is required to normalize an armored signing key");
            return std::nullopt;
        }
        QTemporaryFile input;
        if (!input.open() || input.write(contents) != contents.size() || !input.flush()) {
            if (error != nullptr) *error = QStringLiteral("Could not prepare the armored signing key");
            return std::nullopt;
        }
        QProcess process;
        process.setProgram(gpg);
        process.setArguments({QStringLiteral("--batch"), QStringLiteral("--yes"), QStringLiteral("--dearmor"),
                              QStringLiteral("--output"), fromPath(destination), input.fileName()});
        process.start();
        if (!process.waitForStarted(5000) || !process.waitForFinished(15000) || process.exitCode() != 0) {
            if (error != nullptr) {
                *error = QStringLiteral("Could not normalize signing key: %1")
                             .arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
            }
            return std::nullopt;
        }
    } else if (!writeFile(destination, contents, error)) {
        return std::nullopt;
    }

    auto keyFingerprints = RepositoryTrust::fingerprints(destination, error);
    if (keyFingerprints.isEmpty()) {
        QFile::remove(fromPath(destination));
        if (error != nullptr && error->isEmpty()) *error = QStringLiteral("No OpenPGP public key was found");
        return std::nullopt;
    }
    FieldProvenance provenance{origin, {}, {}, sourceFingerprint,
                               origin == ValueOrigin::User
                                   ? QStringLiteral("Signing key was explicitly imported and trusted by the user.")
                                   : QStringLiteral("Signing key was embedded in the imported vendor package."),
                               QDateTime::currentDateTimeUtc(), origin == ValueOrigin::User};
    return RepositorySigningKey{relative, sha, keyFingerprints, source, sourceFingerprint,
                                trusted, provenance};
}

} // namespace

QStringList RepositoryTrust::fingerprints(const std::filesystem::path &keyring, QString *error) {
    const auto gpg = QStandardPaths::findExecutable(QStringLiteral("gpg"));
    if (gpg.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("gpg was not found");
        return {};
    }
    QProcess process;
    process.setProgram(gpg);
    process.setArguments({QStringLiteral("--batch"), QStringLiteral("--with-colons"),
                          QStringLiteral("--show-keys"), fromPath(keyring)});
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.start();
    if (!process.waitForStarted(5000) || !process.waitForFinished(15000) || process.exitCode() != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not inspect signing key: %1")
                         .arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
        }
        return {};
    }
    QStringList result;
    static const QRegularExpression valid(QStringLiteral("^[0-9A-Fa-f]{40,64}$"));
    for (const auto &line : QString::fromUtf8(process.readAllStandardOutput()).split(QLatin1Char('\n'))) {
        const auto fields = line.split(QLatin1Char(':'));
        if (fields.size() > 9 && fields.at(0) == QStringLiteral("fpr") && valid.match(fields.at(9)).hasMatch()) {
            const auto value = fields.at(9).toUpper();
            if (!result.contains(value)) result.append(value);
        }
    }
    return result;
}

std::optional<RepositoryKeyInspection> RepositoryTrust::inspectKey(
    const QByteArray &contents, QString *error) {
    if (contents.isEmpty() || contents.size() > 4 * 1024 * 1024) {
        if (error != nullptr) *error = QStringLiteral("Signing key is empty or exceeds the 4 MiB limit");
        return std::nullopt;
    }
    QTemporaryFile temporary;
    if (!temporary.open() || temporary.write(contents) != contents.size() || !temporary.flush()) {
        if (error != nullptr) *error = QStringLiteral("Could not prepare the signing key for inspection");
        return std::nullopt;
    }
    const auto keyFingerprints = fingerprints(
        std::filesystem::path(temporary.fileName().toUtf8().constData()), error);
    if (keyFingerprints.isEmpty()) {
        if (error != nullptr && error->isEmpty()) *error = QStringLiteral("No OpenPGP public key was found");
        return std::nullopt;
    }
    return RepositoryKeyInspection{
        QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()),
        keyFingerprints};
}

std::optional<RepositorySigningKey> RepositoryTrust::storeVendorKey(
    const std::filesystem::path &projectDirectory, const ExtractedSigningKey &candidate, QString *error) {
    return storeKey(projectDirectory, candidate.contents, candidate.sourcePath,
                    candidate.sourceFingerprint, true, ValueOrigin::Deterministic, error);
}

std::optional<RepositorySigningKey> RepositoryTrust::importUserKey(
    const std::filesystem::path &projectDirectory, const QByteArray &contents,
    const QString &sourceDescription, QString *error) {
    const auto fingerprint = QString::fromLatin1(
        QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
    return storeKey(projectDirectory, contents, sourceDescription, fingerprint,
                    true, ValueOrigin::User, error);
}

} // namespace pacsmith
