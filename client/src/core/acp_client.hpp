#pragma once

#include "core/acp_environment.hpp"
#include "core/acp_tool_permissions.hpp"
#include <memory>

#include <QHash>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <functional>

namespace pacsmith {

class AcpClient final : public QObject {
    Q_OBJECT
public:
    explicit AcpClient(QObject *parent = nullptr);
    ~AcpClient() override;
    void start(const HarnessProfile &profile, const AcpEnvironment &environment,
               const QJsonArray &mcpServers, const QString &sessionId = {});
    bool prompt(const QString &text, const QJsonArray &images = {});
    void cancel();
    void close();
    void authenticate(const QString &methodId);
    void setConfigOption(const QString &id, const QJsonValue &value);
    bool clearRememberedPermissions(QString *error = nullptr);
    void answerPermission(const QJsonValue &requestId, const QString &optionId = {});
    [[nodiscard]] bool isReady() const { return ready_; }
    [[nodiscard]] bool isBusy() const { return busy_; }
    [[nodiscard]] QString sessionId() const { return sessionId_; }
    [[nodiscard]] QJsonArray configOptions() const { return configOptions_; }

signals:
    void ready();
    void sessionStarted(const QString &id);
    void updated(const QJsonObject &update, bool replaying);
    void permissionRequested(const QJsonValue &id, const QJsonObject &params);
    void permissionsCleared();
    void permissionRemembered(const QString &message);
    void configOptionsChanged(const QJsonArray &options);
    void configOptionApplied(const QString &id, const QJsonValue &value);
    void configurationWarning(const QString &message);
    void authenticationAvailable(const QJsonArray &methods);
    void turnFinished(const QString &reason);
    void failed(const QString &message);
    void busyChanged(bool busy);

private:
    using Callback = std::function<void(const QJsonObject &)>;
    using ErrorCallback = std::function<void(const QString &)>;
    struct Pending { QString method; Callback callback; QTimer *timer; ErrorCallback error; };
    void request(const QString &method, const QJsonObject &params, Callback callback, ErrorCallback error = {});
    void send(const QJsonObject &message);
    void receive(const QJsonObject &message);
    void createSession();
    void configureSession(const QJsonObject &result);
    void fail(const QString &message);
    void setBusy(bool busy);

    std::unique_ptr<AcpToolPermissions> toolPermissions_;
    QProcess process_;
    QMetaObject::Connection startedConnection_;
    bool modeConfigured_{false};
    QByteArray buffer_;
    QByteArray diagnostics_;
    QHash<qint64, Pending> pending_;
    QHash<QString, QJsonObject> permissions_;
    QHash<QString, QJsonObject> toolCalls_;
    QTimer cancelTimer_;
    qint64 nextId_{1};
    QString sessionId_;
    QString workspace_;
    QJsonArray servers_;
    bool codex_{false};
    bool ready_{false};
    bool busy_{false};
    bool closing_{false};
    bool replaying_{false};
    bool cancelled_{false};
    bool supportsLoad_{false};
    bool supportsImages_{false};
    QJsonObject defaults_;
    QJsonArray configOptions_;
};

} // namespace pacsmith
