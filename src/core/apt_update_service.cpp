#include "core/apt_update_service.hpp"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <algorithm>

namespace pacsmith {
namespace {

bool safeRepositoryPath(const QString &value, const bool allowTrailingSlash) {
    if (value.isEmpty() || value.startsWith(QLatin1Char('/')) || value.contains(QStringLiteral(".."))) return false;
    static const QRegularExpression expression(QStringLiteral(R"(^[A-Za-z0-9._+/-]+$)"));
    if (!expression.match(value).hasMatch()) return false;
    return allowTrailingSlash || !value.endsWith(QLatin1Char('/'));
}

QUrl directoryUrl(QUrl url) {
    auto path = url.path();
    if (!path.endsWith(QLatin1Char('/'))) path += QLatin1Char('/');
    url.setPath(path);
    return url;
}

bool verifySignatureFiles(const QStringList &arguments, const QString &keyring,
                          const QStringList &allowedFingerprints, QString &error) {
    const auto gpgv = QStandardPaths::findExecutable(QStringLiteral("gpgv"));
    if (gpgv.isEmpty()) {
        error = QStringLiteral("gpgv is required to verify APT repository signatures");
        return false;
    }
    if (!QFileInfo(keyring).isFile() || allowedFingerprints.isEmpty()) {
        error = QStringLiteral("APT signature verification requires a keyring and pinned fingerprint");
        return false;
    }
    QProcess process;
    process.setProgram(gpgv);
    QStringList processArguments{QStringLiteral("--status-fd"), QStringLiteral("1"),
                                 QStringLiteral("--keyring"), keyring};
    processArguments.append(arguments);
    process.setArguments(processArguments);
    process.start();
    if (!process.waitForStarted(5000) || !process.waitForFinished(15000) ||
        process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = QStringLiteral("APT repository signature verification failed: %1")
                    .arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
        return false;
    }
    const auto status = QString::fromUtf8(process.readAllStandardOutput());
    static const QRegularExpression validSignature(
        QStringLiteral(R"((?:^|\n)\[GNUPG:\] VALIDSIG ([0-9A-Fa-f]{40,64})\s)"));
    const auto match = validSignature.match(status);
    if (!match.hasMatch()) {
        error = QStringLiteral("gpgv did not report a valid APT repository signature");
        return false;
    }
    const auto signatureFingerprint = match.captured(1);
    const auto allowed = std::any_of(allowedFingerprints.cbegin(), allowedFingerprints.cend(),
                                     [&](const auto &fingerprint) {
                                         return fingerprint.compare(signatureFingerprint,
                                                                    Qt::CaseInsensitive) == 0;
                                     });
    if (!allowed) {
        error = QStringLiteral("APT repository signature did not match the pinned signing-key fingerprint");
        return false;
    }
    return true;
}

} // namespace

bool AptSignatureVerifier::verifyInRelease(const QByteArray &data, const QString &keyring,
                                           const QStringList &allowedFingerprints,
                                           QString &error) {
    QTemporaryFile inRelease;
    if (!inRelease.open() || inRelease.write(data) != data.size() || !inRelease.flush()) {
        error = QStringLiteral("Could not create temporary InRelease file for signature verification");
        return false;
    }
    return verifySignatureFiles({inRelease.fileName()}, keyring, allowedFingerprints, error);
}

bool AptSignatureVerifier::verifyDetachedRelease(const QByteArray &release,
                                                 const QByteArray &signature,
                                                 const QString &keyring,
                                                 const QStringList &allowedFingerprints,
                                                 QString &error) {
    QTemporaryFile releaseFile;
    QTemporaryFile signatureFile;
    if (!releaseFile.open() || releaseFile.write(release) != release.size() || !releaseFile.flush() ||
        !signatureFile.open() || signatureFile.write(signature) != signature.size() ||
        !signatureFile.flush()) {
        error = QStringLiteral("Could not create temporary files for detached APT signature verification");
        return false;
    }
    return verifySignatureFiles({signatureFile.fileName(), releaseFile.fileName()}, keyring,
                                allowedFingerprints, error);
}

AptUpdateService::AptUpdateService(QObject *parent) : QObject(parent), network_(this) {}

bool AptUpdateService::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void AptUpdateService::start(const PackageRelease &project, const std::filesystem::path &projectDirectory) {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    project_ = project;
    projectDirectory_ = projectDirectory;
    cancelled_ = false;
    signatureVerified_ = false;
    const auto &configuration = project_.update;
    if (configuration.aptSigningKeyring.isEmpty() || configuration.trustedSigningFingerprint.isEmpty()) {
        fail(QStringLiteral("APT update checks require a trusted signing key and pinned fingerprint"));
        return;
    }
    repositoryBase_ = directoryUrl(QUrl(configuration.url));
    if (!repositoryBase_.isValid() || repositoryBase_.host().isEmpty() ||
        (repositoryBase_.scheme() != QStringLiteral("https") && repositoryBase_.scheme() != QStringLiteral("http")) ||
        !repositoryBase_.userInfo().isEmpty() || repositoryBase_.hasQuery() || repositoryBase_.hasFragment()) {
        fail(QStringLiteral("APT repository URL must be an HTTP(S) base URL without credentials, query, or fragment"));
        return;
    }
    if (!safeRepositoryPath(configuration.aptSuite, true) ||
        !safeRepositoryPath(configuration.aptArchitecture, false) ||
        configuration.aptPackageName.isEmpty()) {
        fail(QStringLiteral("APT suite, package, and architecture must be configured with safe values"));
        return;
    }
    flatRepository_ = configuration.aptSuite.endsWith(QLatin1Char('/'));
    if (!flatRepository_ && !safeRepositoryPath(configuration.aptComponent, false)) {
        fail(QStringLiteral("APT component must be configured for a non-flat repository"));
        return;
    }

    const auto relativeRoot = flatRepository_ ? configuration.aptSuite
                                              : QStringLiteral("dists/%1/").arg(configuration.aptSuite);
    releaseRoot_ = repositoryBase_.resolved(QUrl(relativeRoot));
    emit progressChanged(QStringLiteral("Downloading signed APT release metadata…"));
    fetch(releaseRoot_.resolved(QUrl(QStringLiteral("InRelease"))), RequestKind::InRelease, 16 * 1024 * 1024);
}

void AptUpdateService::cancel() {
    cancelled_ = true;
    if (reply_ != nullptr) reply_->abort();
}

void AptUpdateService::fetch(const QUrl &url, const RequestKind kind, const qsizetype maximumBytes) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    request.setRawHeader("User-Agent", "PacSmith/0.1");
    response_.clear();
    responseTooLarge_ = false;
    maximumBytes_ = maximumBytes;
    reply_ = network_.get(request);
    auto *currentReply = reply_;
    connect(currentReply, &QNetworkReply::readyRead, this, [this, currentReply] {
        const auto chunk = currentReply->readAll();
        if (chunk.size() > maximumBytes_ - response_.size()) {
            responseTooLarge_ = true;
            currentReply->abort();
            return;
        }
        response_.append(chunk);
    });
    connect(currentReply, &QNetworkReply::finished, this,
            [this, currentReply, kind] { requestFinished(currentReply, kind); });
}

void AptUpdateService::requestFinished(QNetworkReply *reply, const RequestKind kind) {
    if (reply != reply_) return;
    const auto remaining = reply->readAll();
    if (remaining.size() > maximumBytes_ - response_.size()) responseTooLarge_ = true;
    else response_.append(remaining);
    const auto errorCode = reply->error();
    const auto errorText = reply->errorString();
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    reply_ = nullptr;
    if (cancelled_) {
        fail(QStringLiteral("APT update check cancelled"));
        return;
    }
    if (responseTooLarge_) {
        fail(QStringLiteral("APT repository response exceeded its safety limit"));
        return;
    }
    if (errorCode != QNetworkReply::NoError) {
        if (kind == RequestKind::InRelease && status == 404) {
            emit progressChanged(QStringLiteral("InRelease is unavailable; downloading Release metadata…"));
            fetch(releaseRoot_.resolved(QUrl(QStringLiteral("Release"))), RequestKind::Release, 16 * 1024 * 1024);
            return;
        }
        fail(QStringLiteral("APT repository request failed: %1").arg(errorText));
        return;
    }
    if (kind == RequestKind::Packages) {
        processPackages(response_);
    } else if (kind == RequestKind::Release) {
        releaseData_ = response_;
        emit progressChanged(QStringLiteral("Downloading detached APT Release signature…"));
        fetch(releaseRoot_.resolved(QUrl(QStringLiteral("Release.gpg"))),
              RequestKind::ReleaseSignature, 4 * 1024 * 1024);
    } else if (kind == RequestKind::ReleaseSignature) {
        QString verificationError;
        if (!verifyDetachedRelease(releaseData_, response_, verificationError)) {
            fail(verificationError);
            return;
        }
        signatureVerified_ = true;
        processRelease(releaseData_, false);
    } else {
        processRelease(response_, true);
    }
}

void AptUpdateService::processRelease(const QByteArray &data, const bool inRelease) {
    if (inRelease) {
        QString verificationError;
        if (!verifyInRelease(data, verificationError)) {
            fail(verificationError);
            return;
        }
        signatureVerified_ = true;
    }
    QString parseError;
    const auto entries = AptRepositoryMetadata::parseRelease(QByteArrayView(data), &parseError);
    packagesIndex_ = AptRepositoryMetadata::selectPackagesIndex(
                         entries, project_.update.aptComponent,
                         project_.update.aptArchitecture, flatRepository_)
                         .value_or(AptReleaseEntry{});
    if (packagesIndex_.path.isEmpty()) {
        fail(QStringLiteral("%1; no Packages index was published for %2/%3")
                 .arg(parseError.isEmpty() ? QStringLiteral("Release metadata parsed") : parseError,
                      project_.update.aptComponent, project_.update.aptArchitecture));
        return;
    }
    emit progressChanged(QStringLiteral("Downloading %1…").arg(packagesIndex_.path));
    fetch(releaseRoot_.resolved(QUrl(packagesIndex_.path)), RequestKind::Packages, 64 * 1024 * 1024);
}

void AptUpdateService::processPackages(const QByteArray &data) {
    if (data.size() != packagesIndex_.size) {
        fail(QStringLiteral("Packages index size does not match signed Release metadata"));
        return;
    }
    const auto actualHash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    if (actualHash != packagesIndex_.sha256) {
        fail(QStringLiteral("Packages index SHA256 does not match Release metadata"));
        return;
    }
    emit progressChanged(QStringLiteral("Inspecting repository package versions…"));
    QString parseError;
    const auto uncompressed = AptRepositoryMetadata::decompressIndex(data, &parseError);
    if (!uncompressed) {
        fail(QStringLiteral("Could not decompress Packages index: %1").arg(parseError));
        return;
    }
    const auto record = AptRepositoryMetadata::latestPackage(
        QByteArrayView(*uncompressed), project_.update.aptPackageName,
        project_.update.aptArchitecture, &parseError);
    if (!record) {
        fail(parseError);
        return;
    }
    UpdateCheckResult result;
    result.supported = true;
    result.success = true;
    result.updateAvailable = DebianVersion::compare(record->version, project_.debian.version) > 0;
    result.detectedVersion = record->version;
    result.filename = record->filename;
    result.sha256 = record->sha256;
    result.downloadUrl = repositoryBase_.resolved(QUrl(record->filename)).toString();
    result.signatureVerified = signatureVerified_;
    result.message = QStringLiteral("%1: %2 %3 (%4; Packages index hash verified%5)")
                         .arg(result.updateAvailable ? QStringLiteral("Update available")
                                                     : QStringLiteral("No newer version"),
                              record->package, record->version, record->architecture,
                              signatureVerified_ ? QStringLiteral(", repository signature verified")
                                                 : QStringLiteral(", repository signature not verified"));
    complete(result);
}

bool AptUpdateService::verifyInRelease(const QByteArray &data, QString &error) const {
    const auto keyring = resolvedKeyring(error);
    return !keyring.isEmpty() && AptSignatureVerifier::verifyInRelease(
                                     data, keyring, allowedSigningFingerprints(), error);
}

bool AptUpdateService::verifyDetachedRelease(const QByteArray &release, const QByteArray &signature,
                                             QString &error) const {
    const auto keyring = resolvedKeyring(error);
    return !keyring.isEmpty() && AptSignatureVerifier::verifyDetachedRelease(
                                     release, signature, keyring,
                                     allowedSigningFingerprints(), error);
}

QString AptUpdateService::resolvedKeyring(QString &error) const {
    const QFileInfo configured(project_.update.aptSigningKeyring);
    const auto candidate = configured.isAbsolute()
                               ? std::filesystem::path(configured.absoluteFilePath().toUtf8().constData())
                               : projectDirectory_ /
                                     std::filesystem::path(project_.update.aptSigningKeyring.toUtf8().constData());
    std::error_code filesystemError;
    const auto canonical = std::filesystem::weakly_canonical(candidate, filesystemError);
    if (filesystemError || !std::filesystem::is_regular_file(canonical)) {
        error = QStringLiteral("Configured APT signing keyring is not an existing regular file");
        return {};
    }
    if (!configured.isAbsolute()) {
        const auto root = std::filesystem::weakly_canonical(projectDirectory_, filesystemError);
        const auto relative = std::filesystem::relative(canonical, root, filesystemError);
        if (filesystemError || relative.empty() || relative.native().starts_with("..")) {
            error = QStringLiteral("Project-relative APT signing keyring escapes the project directory");
            return {};
        }
    }
    return QString::fromUtf8(canonical.string().c_str());
}

QStringList AptUpdateService::allowedSigningFingerprints() const {
    QStringList allowedFingerprints{project_.update.trustedSigningFingerprint};
    const auto selectedKey = std::find_if(project_.update.signingKeys.cbegin(), project_.update.signingKeys.cend(),
                                          [&](const auto &key) {
                                              return key.relativePath == project_.update.aptSigningKeyring && key.trusted;
                                          });
    if (selectedKey != project_.update.signingKeys.cend()) allowedFingerprints.append(selectedKey->fingerprints);
    allowedFingerprints.removeAll(QString{});
    allowedFingerprints.removeDuplicates();
    return allowedFingerprints;
}

void AptUpdateService::complete(const UpdateCheckResult &result) {
    running_.store(false, std::memory_order_release);
    emit finished(result);
}

void AptUpdateService::fail(const QString &message) {
    UpdateCheckResult result;
    result.supported = true;
    result.message = message;
    complete(result);
}

} // namespace pacsmith
