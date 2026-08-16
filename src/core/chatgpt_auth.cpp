#include "core/chatgpt_auth.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUrlQuery>

#include <limits>

namespace pacsmith {
namespace {

constexpr auto clientId = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr auto authorizationEndpoint = "https://auth.openai.com/oauth/authorize";
constexpr auto tokenEndpoint = "https://auth.openai.com/oauth/token";
constexpr auto callbackPath = "/auth/callback";
constexpr quint16 callbackPort = 1455;

QString randomVerifier(const qsizetype length) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    static constexpr auto alphabetSize = sizeof(alphabet) - 1U;
    QString value;
    value.reserve(length);
    auto *random = QRandomGenerator::system();
    for (qsizetype index = 0; index < length; ++index) {
        const auto offset = random->bounded(static_cast<quint32>(alphabetSize));
        value.append(QLatin1Char(alphabet[offset]));
    }
    return value;
}

QString randomState() {
    QByteArray bytes;
    bytes.resize(24);
    auto *random = QRandomGenerator::system();
    for (qsizetype index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>(random->bounded(256U));
    }
    return QString::fromLatin1(bytes.toHex());
}

QString pkceChallenge(const QString &verifier) {
    const auto digest = QCryptographicHash::hash(verifier.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(
        digest.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QJsonObject decodeJwtPayload(const QString &token) {
    const auto parts = token.split(QLatin1Char('.'));
    if (parts.size() != 3) return {};
    const auto decoded = QByteArray::fromBase64(
        parts.at(1).toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    const auto document = QJsonDocument::fromJson(decoded);
    return document.isObject() ? document.object() : QJsonObject{};
}

void populateIdentity(ChatGptCredentials &credentials) {
    const auto payload = decodeJwtPayload(credentials.accessToken);
    const auto auth = payload.value(QStringLiteral("https://api.openai.com/auth")).toObject();
    const auto profile = payload.value(QStringLiteral("https://api.openai.com/profile")).toObject();
    if (credentials.accountId.isEmpty()) {
        credentials.accountId = auth.value(QStringLiteral("chatgpt_account_id")).toString();
    }
    credentials.email = profile.value(QStringLiteral("email")).toString(credentials.email);
    credentials.planType = auth.value(QStringLiteral("chatgpt_plan_type")).toString(credentials.planType);
}

QByteArray formEncoded(const QList<QPair<QString, QString>> &fields) {
    QUrlQuery query;
    for (const auto &[name, value] : fields) query.addQueryItem(name, value);
    return query.query(QUrl::FullyEncoded).toUtf8();
}

void sendBrowserResponse(QTcpSocket *socket, const int status, const QByteArray &title,
                         const QByteArray &message) {
    const auto html = QByteArrayLiteral(
                          "<!doctype html><html><head><meta charset='utf-8'><title>") +
                      title + QByteArrayLiteral(
                                  "</title></head><body style='font-family:sans-serif;max-width:42rem;"
                                  "margin:4rem auto'><h1>") +
                      title + QByteArrayLiteral("</h1><p>") + message +
                      QByteArrayLiteral("</p><p>You can close this tab.</p></body></html>");
    const auto reason = status == 200 ? QByteArrayLiteral("OK") : QByteArrayLiteral("Bad Request");
    const auto response = QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) + ' ' + reason +
                          QByteArrayLiteral("\r\nConnection: close\r\nContent-Type: text/html; charset=utf-8\r\n") +
                          QByteArrayLiteral("Content-Length: ") + QByteArray::number(html.size()) +
                          QByteArrayLiteral("\r\n\r\n") + html;
    static_cast<void>(socket->write(response));
    socket->disconnectFromHost();
}

} // namespace

bool ChatGptCredentials::isValid() const {
    return !accessToken.isEmpty() && !refreshToken.isEmpty() && expiresAtMs > 0 &&
           !accountId.isEmpty();
}

bool ChatGptCredentials::needsRefresh(const qint64 nowMs) const {
    return expiresAtMs <= nowMs + 60'000;
}

QJsonObject ChatGptCredentials::toJson() const {
    return {{QStringLiteral("formatVersion"), 1},
            {QStringLiteral("accessToken"), accessToken},
            {QStringLiteral("refreshToken"), refreshToken},
            {QStringLiteral("expiresAtMs"), expiresAtMs},
            {QStringLiteral("accountId"), accountId},
            {QStringLiteral("email"), email},
            {QStringLiteral("planType"), planType}};
}

QString ChatGptCredentials::serialize() const {
    return QString::fromUtf8(QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
}

std::optional<ChatGptCredentials> ChatGptCredentials::fromJson(const QJsonObject &object,
                                                               QString *error) {
    ChatGptCredentials credentials;
    credentials.accessToken = object.value(QStringLiteral("accessToken")).toString();
    credentials.refreshToken = object.value(QStringLiteral("refreshToken")).toString();
    credentials.expiresAtMs = object.value(QStringLiteral("expiresAtMs")).toInteger();
    credentials.accountId = object.value(QStringLiteral("accountId")).toString();
    credentials.email = object.value(QStringLiteral("email")).toString();
    credentials.planType = object.value(QStringLiteral("planType")).toString();
    populateIdentity(credentials);
    if (!credentials.isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("PacSmith's saved ChatGPT session is incomplete; sign in again");
        }
        return std::nullopt;
    }
    return credentials;
}

std::optional<ChatGptCredentials> ChatGptCredentials::fromSerialized(
    const QString &serialized, QString *error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(serialized.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("PacSmith's saved ChatGPT session is invalid");
        return std::nullopt;
    }
    return fromJson(document.object(), error);
}

QNetworkRequest chatGptTokenRequest() {
    QNetworkRequest request(QUrl(QString::fromLatin1(tokenEndpoint)));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    request.setTransferTimeout(30'000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    return request;
}

QByteArray chatGptRefreshRequestBody(const QString &refreshToken) {
    return formEncoded({{QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
                        {QStringLiteral("refresh_token"), refreshToken},
                        {QStringLiteral("client_id"), QString::fromLatin1(clientId)}});
}

std::optional<ChatGptCredentials> parseChatGptTokenResponse(
    const QByteArray &body, const QString &previousRefreshToken, QString *error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("OpenAI returned an invalid OAuth token response");
        return std::nullopt;
    }
    const auto object = document.object();
    ChatGptCredentials credentials;
    credentials.accessToken = object.value(QStringLiteral("access_token")).toString();
    credentials.refreshToken = object.value(QStringLiteral("refresh_token")).toString();
    if (credentials.refreshToken.isEmpty()) credentials.refreshToken = previousRefreshToken;
    const auto expiresIn = object.value(QStringLiteral("expires_in")).toInteger();
    if (expiresIn > 0 && expiresIn <= std::numeric_limits<qint64>::max() / 1000) {
        credentials.expiresAtMs = QDateTime::currentMSecsSinceEpoch() + expiresIn * 1000;
    }
    populateIdentity(credentials);
    if (!credentials.isValid()) {
        if (error != nullptr) {
            *error = QStringLiteral("OpenAI's OAuth response did not contain a usable ChatGPT session");
        }
        return std::nullopt;
    }
    return credentials;
}

ChatGptLoginService::ChatGptLoginService(QObject *parent)
    : QObject(parent), callbackServer_(this), network_(this) {
    connect(&callbackServer_, &QTcpServer::newConnection, this,
            &ChatGptLoginService::acceptConnection);
}

bool ChatGptLoginService::isRunning() const noexcept { return running_; }

void ChatGptLoginService::start() {
    if (running_) return;
    clearFlowState();
    verifier_ = randomVerifier(64);
    state_ = randomState();
    redirectUri_ = QStringLiteral("http://localhost:%1%2").arg(callbackPort).arg(
        QString::fromLatin1(callbackPath));
    if (!callbackServer_.listen(QHostAddress::LocalHost, callbackPort)) {
        finishWithError(QStringLiteral("Could not listen for the browser sign-in callback on "
                                       "localhost:%1: %2")
                            .arg(callbackPort)
                            .arg(callbackServer_.errorString()));
        return;
    }
    running_ = true;

    QUrl url(QString::fromLatin1(authorizationEndpoint));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(clientId));
    query.addQueryItem(QStringLiteral("redirect_uri"), redirectUri_);
    query.addQueryItem(QStringLiteral("scope"),
                       QStringLiteral("openid profile email offline_access"));
    query.addQueryItem(QStringLiteral("code_challenge"), pkceChallenge(verifier_));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("state"), state_);
    query.addQueryItem(QStringLiteral("id_token_add_organizations"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("codex_cli_simplified_flow"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("originator"), QStringLiteral("pacsmith"));
    url.setQuery(query);
    emit progressChanged(QStringLiteral("Waiting for ChatGPT sign-in in your browser…"));
    emit authorizationUrlReady(url);
}

void ChatGptLoginService::cancel() {
    if (!running_ && reply_ == nullptr) return;
    if (reply_ != nullptr) {
        auto *current = reply_.data();
        reply_ = nullptr;
        current->abort();
        current->deleteLater();
    }
    clearFlowState();
    emit progressChanged(QStringLiteral("ChatGPT sign-in cancelled"));
}

void ChatGptLoginService::acceptConnection() {
    while (callbackServer_.hasPendingConnections()) {
        auto *socket = callbackServer_.nextPendingConnection();
        if (socket == nullptr) continue;
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket] { processSocket(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        processSocket(socket);
    }
}

void ChatGptLoginService::processSocket(QTcpSocket *socket) {
    if (socket == nullptr || !socket->canReadLine()) return;
    if (!socket->peerAddress().isLoopback()) {
        sendBrowserResponse(socket, 400, QByteArrayLiteral("Sign-in rejected"),
                            QByteArrayLiteral("The callback did not originate on this computer."));
        return;
    }
    const auto requestLine = socket->readLine(16 * 1024).trimmed();
    const auto parts = requestLine.split(' ');
    if (parts.size() < 2 || parts.at(0) != QByteArrayLiteral("GET")) {
        sendBrowserResponse(socket, 400, QByteArrayLiteral("Sign-in failed"),
                            QByteArrayLiteral("Invalid callback request."));
        return;
    }
    const QUrl callback(QStringLiteral("http://localhost") + QString::fromUtf8(parts.at(1)));
    const QUrlQuery query(callback);
    if (callback.path() != QString::fromLatin1(callbackPath) ||
        query.queryItemValue(QStringLiteral("state")) != state_) {
        sendBrowserResponse(socket, 400, QByteArrayLiteral("Sign-in failed"),
                            QByteArrayLiteral("The callback state did not match PacSmith's request."));
        finishWithError(QStringLiteral("ChatGPT sign-in callback validation failed"));
        return;
    }
    const auto providerError = query.queryItemValue(QStringLiteral("error"));
    const auto code = query.queryItemValue(QStringLiteral("code"));
    if (!providerError.isEmpty() || code.isEmpty()) {
        sendBrowserResponse(socket, 400, QByteArrayLiteral("Sign-in failed"),
                            QByteArrayLiteral("OpenAI did not return an authorization code."));
        finishWithError(providerError.isEmpty()
                            ? QStringLiteral("OpenAI did not return an authorization code")
                            : QStringLiteral("OpenAI rejected sign-in: %1").arg(providerError));
        return;
    }
    sendBrowserResponse(socket, 200, QByteArrayLiteral("PacSmith sign-in complete"),
                        QByteArrayLiteral("PacSmith received the authorization response."));
    callbackServer_.close();
    exchangeAuthorizationCode(code);
}

void ChatGptLoginService::exchangeAuthorizationCode(const QString &code) {
    emit progressChanged(QStringLiteral("Completing ChatGPT sign-in…"));
    const auto body = formEncoded({
        {QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
        {QStringLiteral("client_id"), QString::fromLatin1(clientId)},
        {QStringLiteral("code"), code},
        {QStringLiteral("code_verifier"), verifier_},
        {QStringLiteral("redirect_uri"), redirectUri_},
    });
    reply_ = network_.post(chatGptTokenRequest(), body);
    auto *current = reply_.data();
    connect(current, &QNetworkReply::finished, this, [this, current] {
        if (current != reply_) return;
        const auto response = current->readAll();
        const auto networkError = current->error();
        const auto status = current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkMessage = current->errorString();
        current->deleteLater();
        reply_ = nullptr;
        if (networkError != QNetworkReply::NoError) {
            finishWithError(QStringLiteral("ChatGPT token exchange failed (HTTP %1): %2")
                                .arg(status)
                                .arg(networkMessage));
            return;
        }
        QString error;
        const auto credentials = parseChatGptTokenResponse(response, {}, &error);
        if (!credentials) {
            finishWithError(error);
            return;
        }
        const auto serialized = credentials->serialize();
        clearFlowState();
        emit progressChanged(QStringLiteral("Signed in to ChatGPT"));
        emit succeeded(serialized);
    });
}

void ChatGptLoginService::finishWithError(const QString &message) {
    clearFlowState();
    emit failed(message);
}

void ChatGptLoginService::clearFlowState() {
    callbackServer_.close();
    running_ = false;
    verifier_.fill(QChar::Null);
    verifier_.clear();
    state_.clear();
    redirectUri_.clear();
}

} // namespace pacsmith
