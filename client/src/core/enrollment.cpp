#include "core/enrollment.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QThread>
#include <QUrl>

#include <curl/curl.h>

namespace pacsmith {
namespace {

QString xdgDir(const char *envName, const QString &fallbackRelative) {
    const auto value = qEnvironmentVariable(envName);
    if (!value.isEmpty() && QDir::isAbsolutePath(value)) return value;
    return QDir::home().filePath(fallbackRelative);
}

size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<QByteArray *>(userdata);
    const auto total = size * nmemb;
    out->append(ptr, static_cast<int>(total));
    return total;
}

QString opensslError(QProcess &process, const QString &fallback) {
    const auto text = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return text.isEmpty() ? fallback : text;
}

bool runOpenSsl(const QStringList &arguments, const QByteArray &input, QByteArray *output,
                QString *error) {
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QStringLiteral("openssl"), arguments);
    if (!process.waitForStarted(5000)) {
        if (error != nullptr) {
            *error = QStringLiteral("openssl is required to enroll a remote client");
        }
        return false;
    }
    if (!input.isEmpty()) process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(15000) || process.exitCode() != 0) {
        if (error != nullptr) *error = opensslError(process, QStringLiteral("openssl failed"));
        return false;
    }
    if (output != nullptr) *output = process.readAllStandardOutput();
    return true;
}

QString groupedFingerprint(const QByteArray &spkiDer) {
    const auto hex = QCryptographicHash::hash(spkiDer, QCryptographicHash::Sha256).toHex().toUpper();
    QString grouped;
    for (int index = 0; index < 20 && index < hex.size(); index += 4) {
        if (!grouped.isEmpty()) grouped += QLatin1Char(' ');
        grouped += QString::fromLatin1(hex.mid(index, 4));
    }
    return grouped;
}

bool fingerprintFromPem(const QByteArray &pem, QString *abbrev, QString *full, QString *error) {
    QByteArray pubkey;
    if (!runOpenSsl({QStringLiteral("x509"), QStringLiteral("-pubkey"), QStringLiteral("-noout")},
                    pem, &pubkey, error)) {
        return false;
    }
    QByteArray der;
    if (!runOpenSsl({QStringLiteral("pkey"), QStringLiteral("-pubin"), QStringLiteral("-outform"),
                     QStringLiteral("DER")},
                    pubkey, &der, error)) {
        return false;
    }
    const auto hex = QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex();
    if (abbrev != nullptr) *abbrev = groupedFingerprint(der);
    if (full != nullptr) *full = QString::fromLatin1(hex);
    return true;
}

bool writeFile(const QString &path, const QByteArray &contents, QString *error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    file.write(contents);
    if (!file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QString apiMessage(const HttpResponse &response, const QString &fallback) {
    if (!response.error.isEmpty()) return response.error;
    const auto message = QJsonDocument::fromJson(response.body)
                             .object()
                             .value(QStringLiteral("error"))
                             .toObject()
                             .value(QStringLiteral("message"))
                             .toString()
                             .trimmed();
    if (!message.isEmpty()) return message;
    return QStringLiteral("%1 (HTTP %2)").arg(fallback).arg(response.status);
}

bool writeReqConfig(QTemporaryFile *file, QString *error) {
    if (file == nullptr || !file->open()) {
        if (error != nullptr) *error = QStringLiteral("could not write an OpenSSL config");
        return false;
    }
    file->write("[req]\n"
                "distinguished_name = dn\n"
                "prompt = no\n"
                "utf8 = yes\n"
                "[dn]\n");
    file->flush();
    file->close();
    return true;
}

bool ensureClientKey(const QString &keyPath, const QString &csrPath, const QString &name,
                     QString *error) {
    QDir().mkpath(QFileInfo(keyPath).absolutePath());
    auto cn = name.trimmed();
    cn.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._ -]")), QStringLiteral("-"));
    cn = cn.trimmed();
    if (cn.isEmpty()) cn = QStringLiteral("PacSmith-client");
    QTemporaryFile config(QDir::temp().filePath(QStringLiteral("pacsmith-openssl-XXXXXX.cnf")));
    config.setAutoRemove(true);
    if (!writeReqConfig(&config, error)) return false;
    QStringList arguments{QStringLiteral("req"),
                          QStringLiteral("-new"),
                          QStringLiteral("-config"),
                          config.fileName(),
                          QStringLiteral("-nodes"),
                          QStringLiteral("-out"),
                          csrPath,
                          QStringLiteral("-subj"),
                          QStringLiteral("/O=PacSmith/CN=") + cn,
                          QStringLiteral("-sha256")};
    if (QFileInfo::exists(keyPath)) {
        arguments << QStringLiteral("-key") << keyPath;
    } else {
        arguments << QStringLiteral("-newkey") << QStringLiteral("ec") << QStringLiteral("-pkeyopt")
                  << QStringLiteral("ec_paramgen_curve:prime256v1") << QStringLiteral("-keyout")
                  << keyPath;
    }
    QByteArray ignored;
    if (!runOpenSsl(arguments, {}, &ignored, error)) return false;
    QFile::setPermissions(keyPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

QByteArray extractPemCerts(curl_certinfo *info) {
    QByteArray last;
    if (info == nullptr) return {};
    for (int index = 0; index < info->num_of_certs; ++index) {
        for (auto *item = info->certinfo[index]; item != nullptr; item = item->next) {
            const auto line = QByteArray(item->data);
            const auto prefix = QByteArray("Cert:");
            if (line.startsWith(prefix)) last = line.mid(prefix.size()).trimmed();
        }
    }
    return last;
}

bool probeServerCa(const QUrl &url, QByteArray *caPem, QString *error) {
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        if (error != nullptr) *error = QStringLiteral("could not initialize HTTP client");
        return false;
    }
    QByteArray body;
    const auto target = url.toString(QUrl::FullyEncoded).toUtf8();
    curl_easy_setopt(curl, CURLOPT_URL, target.constData());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CERTINFO, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const auto code = curl_easy_perform(curl);
    curl_certinfo *info = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CERTINFO, &info);
    const auto pem = extractPemCerts(info);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        if (error != nullptr) *error = QString::fromUtf8(curl_easy_strerror(code));
        return false;
    }
    if (pem.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("the library host did not present a PacSmith server certificate");
        }
        return false;
    }
    if (caPem != nullptr) *caPem = pem;
    return true;
}

} // namespace

QString clientPkiDirectory() {
    const auto data = xdgDir("XDG_DATA_HOME", QStringLiteral(".local/share"));
    return QDir(data).filePath(QStringLiteral("pacsmith/client/pki"));
}

ConnectionConfig remoteConnection(const QString &host, int port) {
    ConnectionConfig config;
    config.mode = ConnectionConfig::Mode::Remote;
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(host);
    url.setPort(port > 0 ? port : 8443);
    config.remoteUrl = url;
    const auto root = clientPkiDirectory();
    config.serverCaPath = QDir(root).filePath(QStringLiteral("server-ca.pem"));
    config.clientCertPath = QDir(root).filePath(QStringLiteral("client.crt"));
    config.clientKeyPath = QDir(root).filePath(QStringLiteral("client.key"));
    return config;
}

bool parseRemoteTarget(const QString &text, QString *host, int *port, QString *error) {
    auto spec = text.trimmed();
    if (spec.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("host is required");
        return false;
    }
    if (!spec.contains(QStringLiteral("://"))) spec.prepend(QStringLiteral("https://"));
    QUrl url(spec);
    if (!url.isValid() || url.host().isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("enter a host or host:port");
        return false;
    }
    if (host != nullptr) *host = url.host();
    if (port != nullptr) *port = url.port(8443);
    return true;
}

QString defaultEnrollmentName() {
    const auto host = QHostInfo::localHostName().trimmed();
    return host.isEmpty() ? QStringLiteral("PacSmith client") : host;
}

std::optional<EnrollmentResult> enrollRemote(
    const QString &host, int port, const QString &friendlyName,
    const std::function<bool(const QString &fingerprint, const QString &sha256)> &confirm,
    const std::function<void(const QString &)> &progress,
    const std::function<bool()> &waitOneSecond, QString *error) {
    auto config = remoteConnection(host, port);
    const auto csrPath = QDir(clientPkiDirectory()).filePath(QStringLiteral("client.csr"));
    const auto name = [&] {
        auto value =
            friendlyName.trimmed().isEmpty() ? defaultEnrollmentName() : friendlyName.trimmed();
        if (value.size() > 80) value.truncate(80);
        return value;
    }();
    if (progress) progress(QStringLiteral("Preparing this computer's client certificate…"));
    if (!ensureClientKey(config.clientKeyPath, csrPath, name, error)) return std::nullopt;
    QUrl probe = config.remoteUrl;
    probe.setPath(QStringLiteral("/api/v1/version"));
    if (progress) progress(QStringLiteral("Contacting the library host to read its identity…"));
    QByteArray caPem;
    if (!probeServerCa(probe, &caPem, error)) return std::nullopt;
    QString fingerprint;
    QString sha256;
    if (!fingerprintFromPem(caPem, &fingerprint, &sha256, error)) return std::nullopt;
    if (confirm && !confirm(fingerprint, sha256)) {
        if (error != nullptr) *error = QStringLiteral("enrollment canceled");
        return std::nullopt;
    }
    if (!writeFile(config.serverCaPath, caPem, error)) return std::nullopt;
    const auto csr = QString::fromUtf8(readFile(csrPath));
    if (csr.trimmed().isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("could not read the generated certificate request");
        return std::nullopt;
    }
    if (progress) progress(QStringLiteral("Submitting a registration request…"));
    auto bootstrap = config;
    bootstrap.clientCertPath.clear();
    bootstrap.clientKeyPath.clear();
    HttpTransport transport(bootstrap);
    const auto accepted = transport.request(
        QStringLiteral("POST"), QStringLiteral("/api/v1/registrations"),
        {{QStringLiteral("Content-Type"), QStringLiteral("application/json")}},
        QJsonDocument(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("csr"), csr}})
            .toJson(QJsonDocument::Compact));
    if (!accepted.error.isEmpty() || accepted.status != 202) {
        if (error != nullptr) {
            *error = apiMessage(accepted, QStringLiteral("the library host rejected registration"));
        }
        return std::nullopt;
    }
    const auto registrationId =
        QJsonDocument::fromJson(accepted.body).object().value(QStringLiteral("id")).toString();
    if (registrationId.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("the library host did not return a registration id");
        return std::nullopt;
    }
    if (progress) {
        progress(QStringLiteral("Waiting for approval of registration %1…").arg(registrationId));
    }
    for (int attempt = 0; attempt < 600; ++attempt) {
        const auto current = transport.request(QStringLiteral("GET"),
                                               QStringLiteral("/api/v1/registrations/") +
                                                   registrationId);
        if (!current.error.isEmpty() || current.status != 200) {
            if (error != nullptr) {
                *error = apiMessage(current, QStringLiteral("could not poll registration"));
            }
            return std::nullopt;
        }
        const auto object = QJsonDocument::fromJson(current.body).object();
        const auto status = object.value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("approved")) {
            const auto cert = object.value(QStringLiteral("cert_pem")).toString().toUtf8();
            if (cert.trimmed().isEmpty()) {
                if (error != nullptr) *error = QStringLiteral("approved registration had no certificate");
                return std::nullopt;
            }
            if (!writeFile(config.clientCertPath, cert, error)) return std::nullopt;
            if (!config.save(error)) return std::nullopt;
            EnrollmentResult result;
            result.config = config;
            result.fingerprint = fingerprint;
            result.fingerprintSha256 = sha256;
            result.registrationId = registrationId;
            return result;
        }
        if (status == QStringLiteral("rejected") || status == QStringLiteral("expired")) {
            if (error != nullptr) *error = QStringLiteral("registration %1").arg(status);
            return std::nullopt;
        }
        const bool keepWaiting = waitOneSecond ? waitOneSecond() : (QThread::msleep(1000), true);
        if (!keepWaiting) {
            if (error != nullptr) *error = QStringLiteral("enrollment canceled");
            return std::nullopt;
        }
    }
    if (error != nullptr) *error = QStringLiteral("timed out waiting for registration approval");
    return std::nullopt;
}

} // namespace pacsmith
