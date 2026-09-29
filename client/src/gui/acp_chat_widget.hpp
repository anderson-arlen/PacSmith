#pragma once

#include "core/acp_client.hpp"
#include "gui/chat_image_input.hpp"
#include <QWidget>
#include <QLockFile>
#include <memory>
class QComboBox;

class QLabel;
class QPlainTextEdit;

class QPushButton;
class QVBoxLayout;
class QHBoxLayout;

namespace pacsmith::gui {

class ChatTranscript;

struct ChatScreenContext { QString summary; QString prompt; };

class AcpChatWidget final : public QWidget {
    Q_OBJECT
public:
    AcpChatWidget(HarnessProfile profile, ConnectionConfig connection, QString key, QString context,
                  QWidget *parent = nullptr, QString projectId = {});
    ~AcpChatWidget() override;
    bool submit(const QString &prompt);
    void setContextProvider(std::function<std::optional<ChatScreenContext>()> provider);
    void focusComposer();
    void setConversationScope(const QString &scope);
    void refreshSessions();
    auto contextProvider() const { return contextProvider_; }
    void setDefaultsProvider(std::function<QJsonObject()> provider) { defaultsProvider_ = std::move(provider); }
    bool attachImage(const QByteArray &bytes, const QString &name);
    [[nodiscard]] bool isBusy() const;
signals:
    void completed(const QString &error);
    void conversationReset();
    void approvalRequired();
    void conversationSelected(const QString &key);
private:
    void start();
    void append(const QString &text);
    void save();
    void controls();
    bool refreshContext();
    void renderAttachments();
    void sendQueued();
    AcpClient agent_;
    HarnessProfile profile_;
    std::function<QJsonObject()> defaultsProvider_;
    ConnectionConfig connection_;
    QString context_;
    QString projectId_;
    std::function<std::optional<ChatScreenContext>()> contextProvider_;
    QString filePath_;
    QString sessionId_;
    QString queued_;
    QString queuedQuestion_;
    QList<ChatImage> images_;
    QHBoxLayout *imagesLayout_;
    QWidget *imagesScroll_;
    QPushButton *attach_;
    bool turnActive_{false};
    ChatTranscript *transcript_;
    QPlainTextEdit *input_;
    QLabel *status_;
    QComboBox *sessionTitle_;
    QString scope_;
    std::unique_ptr<QLockFile> lease_;
    QTimer historyTimer_;
    bool historyInFlight_{false};
    QPushButton *action_;
    QPushButton *fresh_;
    QWidget *permissions_;
    QVBoxLayout *permissionLayout_;
    QPushButton *settings_;
    QTimer saveTimer_;
};
}
