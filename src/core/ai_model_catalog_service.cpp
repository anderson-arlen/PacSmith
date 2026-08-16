#include "core/ai_model_catalog_service.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>

namespace pacsmith {

AiModelCatalogService::AiModelCatalogService(QObject *parent) : QObject(parent), network_(this) {}

bool AiModelCatalogService::isRunning() const noexcept { return reply_ != nullptr; }

void AiModelCatalogService::fetch(const AiProviderKind provider, const QString &credential) {
    if (isRunning()) return;
    if (provider == AiProviderKind::ChatGpt) {
        QString error;
        const auto credentials = ChatGptCredentials::fromSerialized(credential, &error);
        if (!credentials) {
            emit failed(error);
            return;
        }
        if (credentials->needsRefresh(QDateTime::currentMSecsSinceEpoch())) {
            refreshChatGptCredentials(*credentials);
        } else {
            fetchModels(provider, credentials->accessToken, credentials->accountId);
        }
        return;
    }
    if ((provider != AiProviderKind::OpenAi && provider != AiProviderKind::Xai) || credential.isEmpty()) {
        emit failed(QStringLiteral("The selected provider requires a PacSmith-configured API credential"));
        return;
    }
    fetchModels(provider, credential);
}

void AiModelCatalogService::refreshChatGptCredentials(const ChatGptCredentials &credentials) {
    emit progressChanged(QStringLiteral("Refreshing PacSmith's ChatGPT session…"));
    reply_ = network_.post(chatGptTokenRequest(), chatGptRefreshRequestBody(credentials.refreshToken));
    auto *current = reply_;
    connect(current, &QNetworkReply::finished, this, [this, current, credentials] {
        if (current != reply_) return;
        const auto response = current->readAll();
        const auto networkError = current->error();
        const auto networkMessage = current->errorString();
        current->deleteLater();
        reply_ = nullptr;
        if (networkError != QNetworkReply::NoError) {
            emit failed(QStringLiteral("Could not refresh the ChatGPT session: %1")
                            .arg(networkMessage));
            return;
        }
        QString error;
        const auto refreshed = parseChatGptTokenResponse(response, credentials.refreshToken, &error);
        if (!refreshed) {
            emit failed(error);
            return;
        }
        const auto serialized = refreshed->serialize();
        emit credentialUpdated(serialized);
        fetchModels(AiProviderKind::ChatGpt, refreshed->accessToken, refreshed->accountId);
    });
}

void AiModelCatalogService::fetchModels(const AiProviderKind provider, const QString &bearer,
                                        const QString &accountId) {
    if (bearer.isEmpty()) {
        emit failed(QStringLiteral("The selected provider credential is empty"));
        return;
    }

    QUrl endpoint;
    if (provider == AiProviderKind::ChatGpt) {
        endpoint = QUrl(QStringLiteral(
            "https://chatgpt.com/backend-api/codex/models?client_version=0.147.0"));
    } else if (provider == AiProviderKind::Xai) {
        endpoint = QUrl(QStringLiteral("https://api.x.ai/v1/models"));
    } else {
        endpoint = QUrl(QStringLiteral("https://api.openai.com/v1/models"));
    }
    QNetworkRequest request(endpoint);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    auto authorization = QByteArrayLiteral("Bearer ") + bearer.toUtf8();
    request.setRawHeader("Authorization", authorization);
    authorization.fill('\0');
    if (provider == AiProviderKind::ChatGpt) {
        request.setRawHeader("ChatGPT-Account-ID", accountId.toUtf8());
        request.setRawHeader("originator", "pacsmith");
        request.setRawHeader("User-Agent", "pacsmith/0.1.0");
        request.setRawHeader("Accept", "application/json");
    }
    request.setTransferTimeout(30000);

    emit progressChanged(QStringLiteral("Loading available models directly from %1…")
                             .arg(aiProviderName(provider)));
    reply_ = network_.get(request);
    auto *current = reply_;
    connect(current, &QNetworkReply::finished, this, [this, current, provider] {
        if (current != reply_) return;
        const auto response = current->readAll();
        const auto networkError = current->error();
        const auto networkMessage = current->errorString();
        current->deleteLater();
        reply_ = nullptr;
        if (networkError != QNetworkReply::NoError) {
            auto detail = networkMessage;
            const auto document = QJsonDocument::fromJson(response);
            const auto providerMessage = document.object()
                                             .value(QStringLiteral("error")).toObject()
                                             .value(QStringLiteral("message")).toString();
            if (!providerMessage.isEmpty()) detail = providerMessage;
            emit failed(QStringLiteral("Could not load provider models: %1").arg(detail));
            return;
        }
        QString error;
        const auto models = provider == AiProviderKind::ChatGpt
                                ? parseChatGptModelIds(response, &error)
                                : parseModelIds(response, &error);
        if (!error.isEmpty()) {
            emit failed(error);
            return;
        }
        emit finished(models);
    });
}

void AiModelCatalogService::cancel() {
    if (reply_ == nullptr) return;
    reply_->abort();
}

QStringList AiModelCatalogService::parseModelIds(const QByteArray &response, QString *error) {
    if (error != nullptr) error->clear();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("The provider returned an invalid model catalog");
        return {};
    }
    const auto data = document.object().value(QStringLiteral("data"));
    if (!data.isArray()) {
        if (error != nullptr) *error = QStringLiteral("The provider response did not contain a model list");
        return {};
    }
    QStringList result;
    for (const auto &entry : data.toArray()) {
        const auto id = entry.toObject().value(QStringLiteral("id")).toString().trimmed();
        if (!id.isEmpty() && !result.contains(id)) result.append(id);
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.localeAwareCompare(right) < 0;
    });
    if (result.isEmpty() && error != nullptr) {
        *error = QStringLiteral("The provider returned an empty model catalog");
    }
    return result;
}

QStringList AiModelCatalogService::parseChatGptModelIds(const QByteArray &response, QString *error) {
    if (error != nullptr) error->clear();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("ChatGPT returned an invalid model catalog");
        return {};
    }
    const auto entries = document.object().value(QStringLiteral("models"));
    if (!entries.isArray()) {
        if (error != nullptr) *error = QStringLiteral("ChatGPT's response did not contain a model list");
        return {};
    }
    QStringList result;
    for (const auto &entry : entries.toArray()) {
        const auto object = entry.toObject();
        const auto visibility = object.value(QStringLiteral("visibility")).toString();
        if (!visibility.isEmpty() && visibility != QStringLiteral("list")) continue;
        const auto hasPickerFlag = object.contains(QStringLiteral("show_in_picker")) ||
                                   object.contains(QStringLiteral("showInPicker"));
        const auto showInPicker = object.contains(QStringLiteral("show_in_picker"))
                                      ? object.value(QStringLiteral("show_in_picker")).toBool()
                                      : object.value(QStringLiteral("showInPicker")).toBool();
        if (hasPickerFlag && !showInPicker) {
            continue;
        }
        auto id = object.value(QStringLiteral("slug")).toString().trimmed();
        if (id.isEmpty()) id = object.value(QStringLiteral("id")).toString().trimmed();
        if (!id.isEmpty() && !result.contains(id)) result.append(id);
    }
    if (result.isEmpty() && error != nullptr) {
        *error = QStringLiteral("Your ChatGPT account returned no selectable models");
    }
    return result;
}

} // namespace pacsmith
