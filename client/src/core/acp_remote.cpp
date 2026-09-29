#include "core/acp_client.hpp"
#include "core/server_ai.hpp"
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QtConcurrent>

namespace pacsmith {
namespace {
struct Reply {
    std::optional<QJsonObject> body;
    QString error;
};
} // namespace
void AcpClient::startServer(const ConnectionConfig &requestedConnection, const QString &key,
                            const QString &project, bool initialize) {
    close();
    const auto connection =
        requestedConnection.mode == ConnectionConfig::Mode::Local && requestedConnection.socketPath.isEmpty()
            ? ConnectionConfig::load()
            : requestedConnection;
    serverConnection_ = connection;
    serverKey_ = key;
    sessionId_.clear();
    configOptions_ = {};
    disconnect(&serverTimer_, nullptr, this, nullptr);
    serverTimer_.setInterval(500);
    connect(&serverTimer_, &QTimer::timeout, this, &AcpClient::pollServer);
    setBusy(true);
    const auto generation = serverGeneration_;
    auto *watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this, [this, watcher, generation, initialize] {
        const auto reply = watcher->result();
        watcher->deleteLater();
        if (generation != serverGeneration_)
            return;
        if (!reply.body) {
            setBusy(false);
            emit failed(reply.error);
            return;
        }
        serverTimer_.start();
        if (initialize)
            serverAction(QStringLiteral("initialize"));
        else
            pollServer();
    });
    watcher->setFuture(QtConcurrent::run([connection, key, project] {
        Reply reply;
        reply.body = ServerAi(connection)
                         .request(QStringLiteral("POST"), QStringLiteral("/conversations"),
                                  {{QStringLiteral("id"), key}, {QStringLiteral("project_id"), project}},
                                  &reply.error);
        return reply;
    }));
}
void AcpClient::serverAction(const QString &action, const QJsonObject &body) {
    if (!serverConnection_ || serverActionInFlight_)
        return;
    serverActionInFlight_ = true;
    setBusy(true);
    const auto generation = serverGeneration_;
    const auto connection = *serverConnection_;
    const auto key = serverKey_;
    auto *watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this, [this, watcher, generation] {
        const auto reply = watcher->result();
        watcher->deleteLater();
        if (generation != serverGeneration_)
            return;
        serverActionInFlight_ = false;
        if (!reply.body) {
            setBusy(false);
            emit failed(reply.error);
        }
        pollServer();
    });
    watcher->setFuture(QtConcurrent::run([connection, key, action, body] {
        Reply reply;
        reply.body =
            ServerAi(connection)
                .request(QStringLiteral("POST"), QStringLiteral("/conversations/%1/%2").arg(key, action),
                         body, &reply.error);
        return reply;
    }));
}
void AcpClient::pollServer() {
    if (!serverConnection_ || serverPollInFlight_ || serverActionInFlight_)
        return;
    serverPollInFlight_ = true;
    const auto generation = serverGeneration_;
    const auto connection = *serverConnection_;
    const auto key = serverKey_;
    const auto after = serverAfter_;
    auto *watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this, [this, watcher, generation] {
        const auto reply = watcher->result();
        watcher->deleteLater();
        if (generation != serverGeneration_)
            return;
        serverPollInFlight_ = false;
        if (reply.body)
            applyServerSnapshot(*reply.body);
        else
            emit configurationWarning(reply.error);
    });
    watcher->setFuture(QtConcurrent::run([connection, key, after] {
        Reply reply;
        reply.body =
            ServerAi(connection)
                .request(QStringLiteral("GET"),
                         QStringLiteral("/conversations/%1?after=%2").arg(key).arg(after), {}, &reply.error);
        return reply;
    }));
}
void AcpClient::applyServerSnapshot(const QJsonObject &snapshot) {
    const auto state = snapshot.value(QStringLiteral("status")).toString();
    const auto id = snapshot.value(QStringLiteral("session_id")).toString();
    if (id != sessionId_) {
        sessionId_ = id;
        emit sessionStarted(id);
    }
    const auto options = snapshot.value(QStringLiteral("config_options")).toArray();
    if (options != configOptions_) {
        configOptions_ = options;
        emit configOptionsChanged(options);
    }
    const bool active = state == QStringLiteral("starting") || state == QStringLiteral("running") ||
                        state == QStringLiteral("waiting");
    setBusy(active || serverActionInFlight_);
    for (const auto &entry : snapshot.value(QStringLiteral("events")).toArray()) {
        const auto event = entry.toObject();
        const auto sequence = event.value(QStringLiteral("id")).toInteger();
        if (sequence <= serverAfter_)
            continue;
        serverAfter_ = sequence;
        const auto kind = event.value(QStringLiteral("kind")).toString();
        const auto body = event.value(QStringLiteral("body")).toObject();
        if (kind == QStringLiteral("update"))
            emit updated(body, false);
        else if (kind == QStringLiteral("prompt")) {
            auto content = body.value(QStringLiteral("content")).toArray();
            if (body.value(QStringLiteral("display_text")).isString()) {
                QJsonArray display;
                display.append(
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                {QStringLiteral("text"), body.value(QStringLiteral("display_text"))}});
                for (const auto &item : content)
                    if (item.toObject().value(QStringLiteral("type")).toString() != QStringLiteral("text"))
                        display.append(item);
                content = display;
            }
            emit promptReceived(content);
        } else if (kind == QStringLiteral("finished"))
            emit turnFinished(body.value(QStringLiteral("stopReason")).toString());
        else if (kind == QStringLiteral("notice"))
            emit permissionRemembered(body.value(QStringLiteral("message")).toString());
        else if (kind == QStringLiteral("error"))
            emit failed(body.value(QStringLiteral("message")).toString());
        else if (kind == QStringLiteral("configured"))
            emit configOptionApplied(body.value(QStringLiteral("configId")).toString(),
                                     body.value(QStringLiteral("value")));
        else if (kind == QStringLiteral("authentication"))
            emit authenticationAvailable(body.value(QStringLiteral("methods")).toArray());
        else if (kind == QStringLiteral("permission_resolved"))
            emit permissionRemembered(QStringLiteral("Permission resolved by %1: %2")
                                          .arg(body.value(QStringLiteral("answered_by")).toString(),
                                               body.value(QStringLiteral("option_id")).toString()));
    }
    const auto pending = snapshot.value(QStringLiteral("permissions")).toArray();
    const auto signature = QString::fromUtf8(QJsonDocument(pending).toJson(QJsonDocument::Compact));
    if (signature != serverPermissions_) {
        serverPermissions_ = signature;
        emit permissionsCleared();
        for (const auto &p : pending) {
            const auto permission = p.toObject();
            emit permissionRequested(permission.value(QStringLiteral("id")),
                                     permission.value(QStringLiteral("params")).toObject());
        }
    }
    if (!ready_ && !active && !id.isEmpty()) {
        ready_ = true;
        emit ready();
    }
}
} // namespace pacsmith
