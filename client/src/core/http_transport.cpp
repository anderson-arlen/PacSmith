#include "core/http_transport.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <curl/curl.h>
#include <mutex>
#include <unistd.h>

namespace pacsmith {
namespace {

QString xdgDir(const char *envName, const QString &fallbackRelative) {
    const auto value = qEnvironmentVariable(envName);
    if (!value.isEmpty() && QDir::isAbsolutePath(value)) return value;
    return QDir::home().filePath(fallbackRelative);
}

QString clientConfigPath() {
    return QDir(xdgDir("XDG_CONFIG_HOME", QStringLiteral(".config"))).filePath(
        QStringLiteral("pacsmith/client/connection.json"));
}

size_t readCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *file = static_cast<QFile *>(userdata);
    const auto total = static_cast<qint64>(size * nmemb);
    const auto read = file->read(ptr, total);
    if (read < 0) return CURL_READFUNC_ABORT;
    return static_cast<size_t>(read);
}

size_t fileWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *file = static_cast<QFile *>(userdata);
    const auto total = static_cast<qint64>(size * nmemb);
    const auto written = file->write(ptr, total);
    if (written != total) return 0;
    return static_cast<size_t>(written);
}

size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<QByteArray *>(userdata);
    const auto total = size * nmemb;
    out->append(ptr, static_cast<int>(total));
    return total;
}

size_t headerCallback(char *buffer, size_t size, size_t nitems, void *userdata) {
    auto *headers = static_cast<QMap<QString, QString> *>(userdata);
    const auto total = size * nitems;
    const auto line = QString::fromUtf8(buffer, static_cast<int>(total)).trimmed();
    const auto colon = line.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        headers->insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
    }
    return total;
}

void applyConnection(CURL *curl, const ConnectionConfig &config) {
    if (config.mode == ConnectionConfig::Mode::Local) {
        curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH,
                         config.socketPath.toUtf8().constData());
        curl_easy_setopt(curl, CURLOPT_URL, "http://localhost/");
    } else {
        curl_easy_setopt(curl, CURLOPT_URL, config.remoteUrl.toString().toUtf8().constData());
        if (!config.serverCaPath.isEmpty() && QFileInfo::exists(config.serverCaPath)) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, config.serverCaPath.toUtf8().constData());
        }
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config.enrollmentInsecure ? 0L : 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config.enrollmentInsecure ? 0L : 2L);
        if (!config.clientCertPath.isEmpty() && QFileInfo::exists(config.clientCertPath)) {
            curl_easy_setopt(curl, CURLOPT_SSLCERT, config.clientCertPath.toUtf8().constData());
        }
        if (!config.clientKeyPath.isEmpty() && QFileInfo::exists(config.clientKeyPath)) {
            curl_easy_setopt(curl, CURLOPT_SSLKEY, config.clientKeyPath.toUtf8().constData());
        }
    }
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
}

QString joinOrigin(const ConnectionConfig &config, const QString &path) {
    const auto origin = config.origin();
    if (path.startsWith(QLatin1Char('/'))) return origin + path;
    return origin + QLatin1Char('/') + path;
}

} // namespace

ConnectionConfig ConnectionConfig::localDefault() {
    ConnectionConfig config;
    config.mode = Mode::Local;
    const auto runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const auto root = (!runtime.isEmpty() && QDir::isAbsolutePath(runtime))
                          ? runtime
                          : QStringLiteral("/run/user/%1").arg(::getuid());
    config.socketPath = QDir(root).filePath(QStringLiteral("pacsmith/pacsmith.sock"));
    return config;
}

ConnectionConfig ConnectionConfig::load(QString *error) {
    QFile file(clientConfigPath());
    if (!file.exists()) return localDefault();
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return localDefault();
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = parseError.errorString();
        return localDefault();
    }
    const auto object = document.object();
    ConnectionConfig config = localDefault();
    const auto mode = object.value(QStringLiteral("mode")).toString();
    if (mode == QStringLiteral("remote")) {
        config.mode = Mode::Remote;
        config.remoteUrl = QUrl(object.value(QStringLiteral("url")).toString());
        config.serverCaPath = object.value(QStringLiteral("serverCaPath")).toString();
        config.clientCertPath = object.value(QStringLiteral("clientCertPath")).toString();
        config.clientKeyPath = object.value(QStringLiteral("clientKeyPath")).toString();
    } else if (object.contains(QStringLiteral("socketPath"))) {
        config.socketPath = object.value(QStringLiteral("socketPath")).toString();
    }
    return config;
}

bool ConnectionConfig::save(QString *error) const {
    const auto path = clientConfigPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    QJsonObject object{{QStringLiteral("mode"), mode == Mode::Remote
                                                    ? QStringLiteral("remote")
                                                    : QStringLiteral("local")}};
    if (mode == Mode::Remote) {
        object.insert(QStringLiteral("url"), remoteUrl.toString());
        object.insert(QStringLiteral("serverCaPath"), serverCaPath);
        object.insert(QStringLiteral("clientCertPath"), clientCertPath);
        object.insert(QStringLiteral("clientKeyPath"), clientKeyPath);
    } else {
        object.insert(QStringLiteral("socketPath"), socketPath);
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit();
}

QString ConnectionConfig::origin() const {
    if (mode == Mode::Remote) {
        auto url = remoteUrl;
        url.setPath(QString{});
        url.setQuery(QString{});
        url.setFragment(QString{});
        auto text = url.toString();
        if (text.endsWith(QLatin1Char('/'))) text.chop(1);
        return text;
    }
    return QStringLiteral("http://localhost");
}

QString ConnectionConfig::configPath() { return clientConfigPath(); }

QString ConnectionConfig::summary() const {
    if (mode == Mode::Remote) {
        const auto host = remoteUrl.host();
        const auto port = remoteUrl.port(8443);
        if (host.isEmpty()) return QStringLiteral("remote library");
        return QStringLiteral("%1:%2").arg(host).arg(port);
    }
    return QStringLiteral("this computer");
}

bool ConnectionConfig::sameTarget(const ConnectionConfig &other) const {
    if (mode != other.mode) return false;
    if (mode == Mode::Local) return true;
    return remoteUrl.host().compare(other.remoteUrl.host(), Qt::CaseInsensitive) == 0 &&
           remoteUrl.port(8443) == other.remoteUrl.port(8443);
}

HttpTransport::HttpTransport(ConnectionConfig config) : config_(std::move(config)) {
    static std::once_flag curlOnce;
    std::call_once(curlOnce, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

HttpResponse HttpTransport::request(const QString &method, const QString &path,
                                    const QMap<QString, QString> &headers,
                                    const QByteArray &body) const {
    HttpResponse response;
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        response.error = QStringLiteral("could not initialize HTTP client");
        return response;
    }
    applyConnection(curl, config_);
    const auto url = joinOrigin(config_, path);
    curl_easy_setopt(curl, CURLOPT_URL, url.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    curl_slist *headerList = nullptr;
    for (auto iterator = headers.cbegin(); iterator != headers.cend(); ++iterator) {
        const auto line = iterator.key() + QStringLiteral(": ") + iterator.value();
        headerList = curl_slist_append(headerList, line.toUtf8().constData());
    }
    if (!body.isEmpty() || method == QStringLiteral("POST") || method == QStringLiteral("PUT") ||
        method == QStringLiteral("PATCH")) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.constData());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    if (headerList != nullptr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    const auto code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        response.error = QString::fromUtf8(curl_easy_strerror(code));
        if (config_.mode == ConnectionConfig::Mode::Local &&
            (code == CURLE_COULDNT_CONNECT || code == CURLE_FAILED_INIT)) {
            response.error = QStringLiteral(
                "could not connect to pacsmithd at %1. Choose local management from the connection "
                "control, or run `pacsmith connect local`. On a headless host also run `loginctl enable-linger`")
                                 .arg(config_.socketPath);
        }
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        response.status = static_cast<int>(status);
    }
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return response;
}

HttpResponse HttpTransport::upload(const QString &path, const QString &filename,
                                   const QString &kind, const QString &filePath) const {
    HttpResponse response;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        response.error = file.errorString();
        return response;
    }
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        response.error = QStringLiteral("could not initialize HTTP client");
        return response;
    }
    applyConnection(curl, config_);
    const auto url = joinOrigin(config_, path);
    curl_easy_setopt(curl, CURLOPT_URL, url.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, readCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &file);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(file.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    const auto filenameHeader = QStringLiteral("Pacsmith-Filename: ") + filename;
    const auto kindHeader = QStringLiteral("Pacsmith-Kind: ") + kind;
    curl_slist *headerList = nullptr;
    headerList = curl_slist_append(headerList, "Content-Type: application/octet-stream");
    headerList = curl_slist_append(headerList, filenameHeader.toUtf8().constData());
    headerList = curl_slist_append(headerList, kindHeader.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    const auto code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        response.error = QString::fromUtf8(curl_easy_strerror(code));
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        response.status = static_cast<int>(status);
    }
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return response;
}

bool HttpTransport::downloadToFile(const QString &path, const QString &destination,
                                   QString *error) const {
    QDir().mkpath(QFileInfo(destination).absolutePath());
    QFile file(destination);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        if (error != nullptr) *error = QStringLiteral("could not initialize HTTP client");
        return false;
    }
    applyConnection(curl, config_);
    const auto url = joinOrigin(config_, path);
    curl_easy_setopt(curl, CURLOPT_URL, url.toUtf8().constData());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fileWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    const auto code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    file.close();
    if (code != CURLE_OK) {
        if (error != nullptr) *error = QString::fromUtf8(curl_easy_strerror(code));
        QFile::remove(destination);
        return false;
    }
    if (status != 200) {
        if (error != nullptr) *error = QStringLiteral("download failed with HTTP %1").arg(status);
        QFile::remove(destination);
        return false;
    }
    return true;
}

} // namespace pacsmith
