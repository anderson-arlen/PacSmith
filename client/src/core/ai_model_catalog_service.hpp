#pragma once

#include "core/app_settings.hpp"
#include "core/chatgpt_auth.hpp"

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QStringList>

class QNetworkReply;

namespace pacsmith {

class AiModelCatalogService final : public QObject {
    Q_OBJECT
public:
    explicit AiModelCatalogService(QObject *parent = nullptr);

    [[nodiscard]] bool isRunning() const noexcept;
    void fetch(AiProviderKind provider, const QString &credential);
    void cancel();

    [[nodiscard]] static QStringList parseModelIds(const QByteArray &response,
                                                   QString *error = nullptr);
    [[nodiscard]] static QStringList parseChatGptModelIds(const QByteArray &response,
                                                          QString *error = nullptr);

signals:
    void progressChanged(const QString &message);
    void credentialUpdated(const QString &serializedCredentials);
    void finished(const QStringList &modelIds);
    void failed(const QString &message);

private:
    void refreshChatGptCredentials(const ChatGptCredentials &credentials);
    void fetchModels(AiProviderKind provider, const QString &bearer,
                     const QString &accountId = {});

    QNetworkAccessManager network_;
    QNetworkReply *reply_{nullptr};
};

} // namespace pacsmith
