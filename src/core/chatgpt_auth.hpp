#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QUrl>

#include <optional>

class QNetworkReply;
class QTcpSocket;

namespace pacsmith {

struct ChatGptCredentials {
    QString accessToken;
    QString refreshToken;
    qint64 expiresAtMs{0};
    QString accountId;
    QString email;
    QString planType;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool needsRefresh(qint64 nowMs) const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] QString serialize() const;

    [[nodiscard]] static std::optional<ChatGptCredentials> fromJson(
        const QJsonObject &object, QString *error = nullptr);
    [[nodiscard]] static std::optional<ChatGptCredentials> fromSerialized(
        const QString &serialized, QString *error = nullptr);
};

[[nodiscard]] QNetworkRequest chatGptTokenRequest();
[[nodiscard]] QByteArray chatGptRefreshRequestBody(const QString &refreshToken);
[[nodiscard]] std::optional<ChatGptCredentials> parseChatGptTokenResponse(
    const QByteArray &body, const QString &previousRefreshToken = {},
    QString *error = nullptr);

class ChatGptLoginService final : public QObject {
    Q_OBJECT
public:
    explicit ChatGptLoginService(QObject *parent = nullptr);

    [[nodiscard]] bool isRunning() const noexcept;
    void start();
    void cancel();

signals:
    void authorizationUrlReady(const QUrl &url);
    void progressChanged(const QString &message);
    void succeeded(const QString &serializedCredentials);
    void failed(const QString &message);

private:
    void acceptConnection();
    void processSocket(QTcpSocket *socket);
    void exchangeAuthorizationCode(const QString &code);
    void finishWithError(const QString &message);
    void clearFlowState();

    QTcpServer callbackServer_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QString verifier_;
    QString state_;
    QString redirectUri_;
    bool running_{false};
};

} // namespace pacsmith
