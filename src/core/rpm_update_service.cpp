#include "core/rpm_update_service.hpp"

#include "core/apt_repository.hpp"

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

QUrl directoryUrl(QUrl url) {
    auto path = url.path();
    if (!path.endsWith(QLatin1Char('/'))) path += QLatin1Char('/');
    url.setPath(path);
    return url;
}

QCryptographicHash::Algorithm checksumAlgorithm(const QString &type) {
    return type == QStringLiteral("sha512") ? QCryptographicHash::Sha512
                                             : QCryptographicHash::Sha256;
}

bool verifyDetached(const QByteArray &contents, const QByteArray &signature,
                    const QString &keyring, const QStringList &allowedFingerprints,
                    QString &error) {
    const auto gpgv = QStandardPaths::findExecutable(QStringLiteral("gpgv"));
    if (gpgv.isEmpty()) {
        error = QStringLiteral("gpgv is required to verify RPM repository metadata signatures");
        return false;
    }
    if (!QFileInfo(keyring).isFile() || allowedFingerprints.isEmpty()) {
        error = QStringLiteral("RPM metadata verification requires a keyring and pinned fingerprint");
        return false;
    }
    QTemporaryFile contentsFile;
    QTemporaryFile signatureFile;
    if (!contentsFile.open() || contentsFile.write(contents) != contents.size() ||
        !contentsFile.flush() || !signatureFile.open() ||
        signatureFile.write(signature) != signature.size() || !signatureFile.flush()) {
        error = QStringLiteral("Could not create temporary files for RPM metadata signature verification");
        return false;
    }
    QProcess process;
    process.setProgram(gpgv);
    process.setArguments({QStringLiteral("--status-fd"), QStringLiteral("1"),
                          QStringLiteral("--keyring"), keyring,
                          signatureFile.fileName(), contentsFile.fileName()});
    process.start();
    if (!process.waitForStarted(5000) || !process.waitForFinished(15000) ||
        process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = QStringLiteral("RPM repository metadata signature verification failed: %1")
                    .arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
        return false;
    }
    const auto status = QString::fromUtf8(process.readAllStandardOutput());
    static const QRegularExpression validSignature(
        QStringLiteral(R"((?:^|\n)\[GNUPG:\] VALIDSIG ([0-9A-Fa-f]{40,64})\s)"));
    const auto match = validSignature.match(status);
    if (!match.hasMatch()) {
        error = QStringLiteral("gpgv did not report a valid RPM metadata signature");
        return false;
    }
    const auto actual = match.captured(1);
    const auto accepted = std::any_of(allowedFingerprints.cbegin(), allowedFingerprints.cend(),
                                      [&](const auto &allowed) {
                                          return allowed.compare(actual, Qt::CaseInsensitive) == 0;
                                      });
    if (!accepted) {
        error = QStringLiteral("RPM metadata signature did not match the pinned signing-key fingerprint");
    }
    return accepted;
}

} // namespace

RpmUpdateService::RpmUpdateService(QObject *parent) : QObject(parent), network_(this) {}

bool RpmUpdateService::isRunning() const noexcept { return reply_ != nullptr; }

void RpmUpdateService::start(const PackageRelease &release,
                             const std::filesystem::path &releaseDirectory) {
    if (isRunning()) return;
    release_ = release;
    releaseDirectory_ = releaseDirectory;
    cancelled_ = false;
    if (release_.update.aptSigningKeyring.isEmpty() ||
        release_.update.trustedSigningFingerprint.isEmpty()) {
        fail(QStringLiteral("RPM update checks require a trusted signing key and pinned fingerprint"));
        return;
    }
    if (release_.update.rpmPackageName.isEmpty() || release_.update.rpmArchitecture.isEmpty()) {
        fail(QStringLiteral("RPM package name and architecture must be configured"));
        return;
    }
    repositoryBase_ = directoryUrl(QUrl(release_.update.url, QUrl::StrictMode));
    if (!repositoryBase_.isValid() || repositoryBase_.host().isEmpty() ||
        (repositoryBase_.scheme() != QStringLiteral("https") &&
         repositoryBase_.scheme() != QStringLiteral("http")) ||
        !repositoryBase_.userInfo().isEmpty() || repositoryBase_.hasQuery() ||
        repositoryBase_.hasFragment()) {
        fail(QStringLiteral("RPM repository URL must be an HTTP(S) base URL without credentials, query, or fragment"));
        return;
    }
    emit progressChanged(QStringLiteral("Downloading signed RPM repository metadata…"));
    fetch(repositoryBase_.resolved(QUrl(QStringLiteral("repodata/repomd.xml"))),
          RequestKind::Repomd, 16 * 1024 * 1024);
}

void RpmUpdateService::cancel() {
    cancelled_ = true;
    if (reply_ != nullptr) reply_->abort();
}

void RpmUpdateService::fetch(const QUrl &url, const RequestKind kind,
                             const qsizetype maximumBytes) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    request.setRawHeader("User-Agent", "PacSmith/0.1");
    response_.clear();
    responseTooLarge_ = false;
    maximumBytes_ = maximumBytes;
    reply_ = network_.get(request);
    auto *current = reply_;
    connect(current, &QNetworkReply::readyRead, this, [this, current] {
        const auto chunk = current->readAll();
        if (chunk.size() > maximumBytes_ - response_.size()) {
            responseTooLarge_ = true;
            current->abort();
            return;
        }
        response_.append(chunk);
    });
    connect(current, &QNetworkReply::finished, this,
            [this, current, kind] { requestFinished(current, kind); });
}

void RpmUpdateService::requestFinished(QNetworkReply *reply, const RequestKind kind) {
    if (reply != reply_) return;
    const auto remaining = reply->readAll();
    if (remaining.size() > maximumBytes_ - response_.size()) responseTooLarge_ = true;
    else response_.append(remaining);
    const auto networkError = reply->error();
    const auto errorText = reply->errorString();
    reply->deleteLater();
    reply_ = nullptr;
    if (cancelled_) {
        fail(QStringLiteral("RPM update check cancelled"));
        return;
    }
    if (responseTooLarge_) {
        fail(QStringLiteral("RPM repository response exceeded its safety limit"));
        return;
    }
    if (networkError != QNetworkReply::NoError) {
        fail(QStringLiteral("RPM repository request failed: %1").arg(errorText));
        return;
    }
    if (kind == RequestKind::Repomd) {
        repomd_ = response_;
        emit progressChanged(QStringLiteral("Downloading detached repomd.xml signature…"));
        fetch(repositoryBase_.resolved(QUrl(QStringLiteral("repodata/repomd.xml.asc"))),
              RequestKind::Signature, 4 * 1024 * 1024);
        return;
    }
    if (kind == RequestKind::Signature) {
        QString verificationError;
        if (!verifyRepomdSignature(response_, verificationError)) {
            fail(verificationError);
            return;
        }
        QString parseError;
        const auto parsed = RpmRepositoryMetadata::parseRepomd(QByteArrayView(repomd_), &parseError);
        if (!parsed) {
            fail(parseError);
            return;
        }
        primary_ = *parsed;
        emit progressChanged(QStringLiteral("Downloading verified RPM primary metadata…"));
        fetch(repositoryBase_.resolved(QUrl(primary_.path)), RequestKind::Primary,
              128 * 1024 * 1024);
        return;
    }
    processPrimary(response_);
}

bool RpmUpdateService::verifyRepomdSignature(const QByteArray &signature,
                                             QString &error) const {
    const auto keyring = resolvedKeyring(error);
    return !keyring.isEmpty() && verifyDetached(repomd_, signature, keyring,
                                                allowedSigningFingerprints(), error);
}

QString RpmUpdateService::resolvedKeyring(QString &error) const {
    const QFileInfo configured(release_.update.aptSigningKeyring);
    const auto candidate = configured.isAbsolute()
        ? std::filesystem::path(configured.absoluteFilePath().toUtf8().constData())
        : releaseDirectory_ /
              std::filesystem::path(release_.update.aptSigningKeyring.toUtf8().constData());
    std::error_code filesystemError;
    const auto canonical = std::filesystem::weakly_canonical(candidate, filesystemError);
    if (filesystemError || !std::filesystem::is_regular_file(canonical)) {
        error = QStringLiteral("Configured RPM signing keyring is not an existing regular file");
        return {};
    }
    if (!configured.isAbsolute()) {
        const auto root = std::filesystem::weakly_canonical(releaseDirectory_, filesystemError);
        const auto relative = std::filesystem::relative(canonical, root, filesystemError);
        if (filesystemError || relative.empty() || relative.native().starts_with("..")) {
            error = QStringLiteral("Project-relative RPM signing keyring escapes the release directory");
            return {};
        }
    }
    return QString::fromUtf8(canonical.string().c_str());
}

QStringList RpmUpdateService::allowedSigningFingerprints() const {
    QStringList result{release_.update.trustedSigningFingerprint};
    const auto selected = std::find_if(
        release_.update.signingKeys.cbegin(), release_.update.signingKeys.cend(),
        [&](const auto &key) {
            return key.relativePath == release_.update.aptSigningKeyring && key.trusted;
        });
    if (selected != release_.update.signingKeys.cend()) result.append(selected->fingerprints);
    result.removeAll(QString{});
    result.removeDuplicates();
    return result;
}

void RpmUpdateService::processPrimary(const QByteArray &compressed) {
    const auto actual = QString::fromLatin1(
        QCryptographicHash::hash(compressed, checksumAlgorithm(primary_.checksumType)).toHex());
    if (actual.compare(primary_.checksum, Qt::CaseInsensitive) != 0) {
        fail(QStringLiteral("RPM primary metadata checksum does not match signed repomd.xml"));
        return;
    }
    QString decompressionError;
    const auto xml = AptRepositoryMetadata::decompressIndex(compressed, &decompressionError);
    if (!xml) {
        fail(QStringLiteral("Could not decompress RPM primary metadata: %1").arg(decompressionError));
        return;
    }
    QString parseError;
    const auto package = RpmRepositoryMetadata::latestPackage(
        QByteArrayView(*xml), release_.update.rpmPackageName,
        release_.update.rpmArchitecture, &parseError);
    if (!package) {
        fail(parseError);
        return;
    }
    UpdateCheckResult result;
    result.supported = true;
    result.success = true;
    result.updateAvailable = RpmVersion::compare(package->evr(), release_.debian.version) > 0;
    result.detectedVersion = package->evr();
    result.filename = package->filename;
    result.sha256 = package->checksum;
    result.downloadUrl = repositoryBase_.resolved(QUrl(package->filename)).toString();
    result.signatureVerified = true;
    result.message = QStringLiteral("%1: %2 %3 (%4; repomd signature and metadata checksums verified)")
                         .arg(result.updateAvailable ? QStringLiteral("Update available")
                                                     : QStringLiteral("No newer version"),
                              package->name, package->evr(), package->architecture);
    emit finished(result);
}

void RpmUpdateService::fail(const QString &message) {
    UpdateCheckResult result;
    result.supported = true;
    result.message = message;
    emit finished(result);
}

} // namespace pacsmith
