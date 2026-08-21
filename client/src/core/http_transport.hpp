#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>

namespace pacsmith {

struct ConnectionConfig {
    enum class Mode { Local, Remote };

    Mode mode{Mode::Local};
    QString socketPath;
    QUrl remoteUrl;
    QString serverCaPath;
    QString clientCertPath;
    QString clientKeyPath;
    bool enrollmentInsecure{false};

    [[nodiscard]] static ConnectionConfig localDefault();
    [[nodiscard]] static ConnectionConfig load(QString *error = nullptr);
    [[nodiscard]] static QString configPath();
    [[nodiscard]] bool save(QString *error = nullptr) const;
    [[nodiscard]] QString origin() const;
    [[nodiscard]] QString summary() const;
    [[nodiscard]] bool sameTarget(const ConnectionConfig &other) const;
};

struct HttpResponse {
    int status{0};
    QByteArray body;
    QString error;
    QMap<QString, QString> headers;
};

class HttpTransport final {
public:
    explicit HttpTransport(ConnectionConfig config);

    [[nodiscard]] HttpResponse request(const QString &method, const QString &path,
                                       const QMap<QString, QString> &headers = {},
                                       const QByteArray &body = {}) const;
    [[nodiscard]] HttpResponse upload(const QString &path, const QString &filename,
                                      const QString &kind, const QString &filePath) const;
    [[nodiscard]] bool downloadToFile(const QString &path, const QString &destination,
                                      QString *error = nullptr) const;

private:
    ConnectionConfig config_;
};

} // namespace pacsmith
