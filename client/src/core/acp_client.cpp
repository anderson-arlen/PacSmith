#include <QDir>
#include <QSet>
#include "core/acp_client.hpp"

#include <QJsonDocument>


namespace pacsmith {
namespace {
QString permissionKey(const QJsonValue &id) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{id}).toJson(QJsonDocument::Compact));
}
bool acceptsChoice(const QJsonArray &choices, const QJsonValue &value) {
    for (const auto &entry : choices) {
        const auto choice = entry.toObject();
        if (choice.contains(QStringLiteral("options"))) {
            if (acceptsChoice(choice.value(QStringLiteral("options")).toArray(), value)) return true;
        } else if (choice.value(QStringLiteral("value")) == value) return true;
    }
    return false;
}
}

AcpClient::AcpClient(QObject *parent) : QObject(parent) {
    cancelTimer_.setSingleShot(true);
    cancelTimer_.setInterval(5000);
    connect(&cancelTimer_, &QTimer::timeout, this, [this] {
        close();
        emit turnFinished(QStringLiteral("cancelled"));
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        diagnostics_ = (diagnostics_ + process_.readAllStandardError()).right(16384);
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        buffer_ += process_.readAllStandardOutput();
        if (buffer_.size() > 16 * 1024 * 1024) {
            fail(QStringLiteral("ACP response exceeded the message size limit."));
            return;
        }
        while (buffer_.contains('\n')) {
            const auto end = buffer_.indexOf('\n');
            const auto line = buffer_.left(end).trimmed();
            buffer_.remove(0, end + 1);
            if (line.isEmpty()) continue;
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || (!document.isObject() && !document.isArray())) {
                fail(QStringLiteral("The agent did not return valid ACP JSON. Choose an ACP stdio executable, not a terminal or desktop launcher."));
                return;
            }
            if (document.isObject()) receive(document.object());
            else for (const auto &message : document.array()) receive(message.toObject());
            if (closing_) return;
        }
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!closing_ && error == QProcess::FailedToStart) fail(process_.errorString());
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (!closing_) fail(QStringLiteral("ACP agent exited (%1). %2").arg(code).arg(QString::fromUtf8(diagnostics_).trimmed()));
    });
}

AcpClient::~AcpClient() { close(); }

void AcpClient::start(const HarnessProfile &profile, const AcpEnvironment &environment,
                      const QJsonArray &mcpServers, const QString &sessionId) {
    close();
    closing_ = false;
    cancelled_ = false;
    modeConfigured_ = false;
    codex_ = isCodexAcp(profile);
    toolPermissions_.reset();
    if (codex_) {
        QJsonObject endpoint;
        for (const auto &serverValue : mcpServers) {
            const auto server = serverValue.toObject();
            if (server.value(QStringLiteral("name")).toString() != QStringLiteral("pacsmith_session")) continue;
            for (const auto &entry : server.value(QStringLiteral("env")).toArray()) {
                const auto variable = entry.toObject();
                const auto name = variable.value(QStringLiteral("name")).toString();
                if (name.startsWith(QStringLiteral("PACSMITH_MCP_"))) endpoint.insert(name, variable.value(QStringLiteral("value")));
            }
        }
        QJsonArray arguments;
        for (const auto &argument : profile.arguments) arguments.append(argument);
        QJsonObject variables;
        for (auto it = profile.environment.begin(); it != profile.environment.end(); ++it) variables.insert(it.key(), it.value());
        if (!endpoint.isEmpty()) toolPermissions_ = std::make_unique<AcpToolPermissions>(
            QDir(environment.dataDirectory).filePath(QStringLiteral("permissions")),
            QJsonObject{{QStringLiteral("endpoint"), endpoint}, {QStringLiteral("executable"), profile.executable},
                        {QStringLiteral("arguments"), arguments}, {QStringLiteral("environment"), variables}});
    }
    sessionId_ = sessionId;
    defaults_ = sessionId.isEmpty() ? profile.configDefaults : QJsonObject{};
    configOptions_ = {};
    workspace_ = environment.workspace;
    servers_ = mcpServers;
    buffer_.clear();
    diagnostics_.clear();
    toolCalls_.clear();
    setBusy(true);
    process_.setProcessEnvironment(environment.process);
    process_.setWorkingDirectory(workspace_);
    process_.setProgram(profile.executable);
    process_.setArguments(profile.arguments);
    startedConnection_ = connect(&process_, &QProcess::started, this, [this] {
        disconnect(startedConnection_);
        request(QStringLiteral("initialize"), {
            {QStringLiteral("protocolVersion"), 1},
            {QStringLiteral("clientCapabilities"), QJsonObject{
                {QStringLiteral("session"), QJsonObject{{QStringLiteral("configOptions"),
                    QJsonObject{{QStringLiteral("boolean"), QJsonObject{}}}}}}}},
            {QStringLiteral("clientInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("pacsmith")},
                {QStringLiteral("version"), QStringLiteral(PACSMITH_VERSION)}}}}, [this](const QJsonObject &result) {
            if (result.value(QStringLiteral("protocolVersion")).toInt() != 1) {
                fail(QStringLiteral("This agent does not support ACP protocol version 1."));
                return;
            }
            supportsImages_ = result.value(QStringLiteral("agentCapabilities")).toObject()
                .value(QStringLiteral("promptCapabilities")).toObject().value(QStringLiteral("image")).toBool();
            supportsLoad_ = result.value(QStringLiteral("agentCapabilities")).toObject()
                .value(QStringLiteral("loadSession")).toBool();
            emit authenticationAvailable(result.value(QStringLiteral("authMethods")).toArray());
            createSession();
        });
    });
    process_.start();
}

void AcpClient::createSession() {
    const bool resume = !sessionId_.isEmpty();
    if (resume && !supportsLoad_) {
        fail(QStringLiteral("This agent cannot resume sessions. Choose New chat to continue in a fresh session."));
        return;
    }
    replaying_ = resume;
    QJsonObject params{{QStringLiteral("cwd"), workspace_}, {QStringLiteral("mcpServers"), servers_}};
    if (resume) params.insert(QStringLiteral("sessionId"), sessionId_);
    request(resume ? QStringLiteral("session/load") : QStringLiteral("session/new"), params,
        [this, resume](const QJsonObject &result) {
            replaying_ = false;
            if (!resume) sessionId_ = result.value(QStringLiteral("sessionId")).toString();
            if (sessionId_.isEmpty()) {
                fail(QStringLiteral("The ACP agent returned no session ID."));
                return;
            }
            emit sessionStarted(sessionId_);
            configureSession(result);
        });
}

void AcpClient::configureSession(const QJsonObject &result) {
    const auto options = result.value(QStringLiteral("configOptions")).toArray();
    if (codex_) {
        bool supported = false;
        bool selected = false;
        for (const auto &entry : options) {
            const auto option = entry.toObject();
            if (option.value(QStringLiteral("id")).toString() != QStringLiteral("mode")) continue;
            selected = option.value(QStringLiteral("currentValue")).toString() == QStringLiteral("read-only");
            supported = acceptsChoice(option.value(QStringLiteral("options")).toArray(), QStringLiteral("read-only"));
        }
        if (!supported) {
            fail(QStringLiteral("This Codex ACP adapter must support Ask for approval mode (read-only). Update the adapter before continuing."));
            return;
        }
        if (!selected && modeConfigured_) {
            fail(QStringLiteral("The agent did not accept Ask for approval mode."));
            return;
        }
        if (!selected) {
            modeConfigured_ = true;
            request(QStringLiteral("session/set_config_option"), {
                {QStringLiteral("sessionId"), sessionId_}, {QStringLiteral("configId"), QStringLiteral("mode")},
                {QStringLiteral("value"), QStringLiteral("read-only")}},
                [this](const QJsonObject &configured) { configureSession(configured); });
            return;
        }
    }
    for (const auto &entry : options) {
        const auto option = entry.toObject();
        const auto id = option.value(QStringLiteral("id")).toString();
        if (!defaults_.contains(id)) continue;
        const auto value = defaults_.value(id);
        if (codex_ && id == QStringLiteral("mode")) { defaults_.remove(id); continue; }
        const bool valid = option.value(QStringLiteral("type")).toString() == QStringLiteral("boolean")
            ? value.isBool() : value.isString() && acceptsChoice(option.value(QStringLiteral("options")).toArray(), value);
        // Changing another option (such as the model) can make this saved choice available.
        if (!valid) continue;
        defaults_.remove(id);
        if (option.value(QStringLiteral("currentValue")) == value) continue;
        QJsonObject params{{QStringLiteral("sessionId"), sessionId_}, {QStringLiteral("configId"), id}, {QStringLiteral("value"), value}};
        if (value.isBool()) params.insert(QStringLiteral("type"), QStringLiteral("boolean"));
        request(QStringLiteral("session/set_config_option"), params,
                [this](const QJsonObject &configured) { configureSession(configured); },
                [this, result, id](const QString &error) {
                    emit configurationWarning(QStringLiteral("Could not apply the saved default for %1: %2").arg(id, error));
                    configureSession(result);
                });
        return;
    }
    if (!defaults_.isEmpty()) {
        emit configurationWarning(QStringLiteral("Some saved defaults are no longer offered by this agent: %1").arg(defaults_.keys().join(QStringLiteral(", "))));
        defaults_ = {};
    }
    configOptions_ = options;
    emit configOptionsChanged(configOptions_);
    ready_ = true;
    setBusy(false);
    emit ready();
}

bool AcpClient::prompt(const QString &text, const QJsonArray &images) {
    if (!ready_ || busy_ || (text.trimmed().isEmpty() && images.isEmpty())) return false;
    if (!images.isEmpty() && !supportsImages_) {
        emit failed(QStringLiteral("This ACP agent does not support images. Choose an agent with image support; your message and attachments are still in the composer."));
        return false;
    }
    QJsonArray content;
    if (!text.isEmpty()) content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}});
    for (const auto &image : images) content.append(image);
    cancelled_ = false;
    toolCalls_.clear();
    setBusy(true);
    request(QStringLiteral("session/prompt"), {
        {QStringLiteral("sessionId"), sessionId_},
        {QStringLiteral("prompt"), content}}, [this](const QJsonObject &result) {
        cancelTimer_.stop();
        const auto ids = permissions_.keys();
        for (const auto &key : ids) answerPermission(permissions_.value(key).value(QStringLiteral("id")));
        setBusy(false);
        emit turnFinished(cancelled_ ? QStringLiteral("cancelled") : result.value(QStringLiteral("stopReason")).toString());
    });
    return true;
}

void AcpClient::authenticate(const QString &methodId) {
    if (busy_ || process_.state() != QProcess::Running) return;
    setBusy(true);
    request(QStringLiteral("authenticate"), {{QStringLiteral("methodId"), methodId}},
        [this](const QJsonObject &) { createSession(); });
}

void AcpClient::setConfigOption(const QString &id, const QJsonValue &value) {
    if (!ready_ || busy_ || (codex_ && id == QStringLiteral("mode") && value.toString() != QStringLiteral("read-only"))) return;
    setBusy(true);
    QJsonObject params{{QStringLiteral("sessionId"), sessionId_}, {QStringLiteral("configId"), id}, {QStringLiteral("value"), value}};
    if (value.isBool()) params.insert(QStringLiteral("type"), QStringLiteral("boolean"));
    request(QStringLiteral("session/set_config_option"), params, [this, id, value](const QJsonObject &result) {
        configOptions_ = result.value(QStringLiteral("configOptions")).toArray();
        emit configOptionsChanged(configOptions_);
        setBusy(false);
        emit configOptionApplied(id, value);
    }, [this](const QString &error) {
        setBusy(false);
        emit configOptionsChanged(configOptions_);
        emit configurationWarning(error);
    });
}

void AcpClient::cancel() {
    if (!busy_) return;
    cancelled_ = true;
    const auto ids = permissions_.keys();
    for (const auto &key : ids) answerPermission(permissions_.value(key).value(QStringLiteral("id")));
    if (ready_ && !sessionId_.isEmpty()) {
        send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("method"), QStringLiteral("session/cancel")},
              {QStringLiteral("params"), QJsonObject{{QStringLiteral("sessionId"), sessionId_}}}});
        cancelTimer_.start();
    } else {
        close();
        emit turnFinished(QStringLiteral("cancelled"));
    }
}

bool AcpClient::clearRememberedPermissions(QString *error) {
    return !toolPermissions_ || toolPermissions_->clear(error);
}

void AcpClient::answerPermission(const QJsonValue &requestId, const QString &optionId) {
    const auto key = permissionKey(requestId);
    if (!permissions_.contains(key)) return;
    const auto offered = permissions_.take(key);
    bool valid = false;
    for (const auto &entry : offered.value(QStringLiteral("params")).toObject().value(QStringLiteral("options")).toArray()) {
        if (!optionId.isEmpty() && entry.toObject().value(QStringLiteral("optionId")).toString() == optionId) valid = true;
    }
    if (valid && !cancelled_ && toolPermissions_) {
        QString error;
        if (!toolPermissions_->remember(offered.value(QStringLiteral("params")).toObject(), optionId, &error))
            emit configurationWarning(QStringLiteral("This approval was sent to the agent, but could not be remembered by PacSmith: %1").arg(error));
    }
    QJsonObject outcome{{QStringLiteral("outcome"), valid && !cancelled_ ? QStringLiteral("selected") : QStringLiteral("cancelled")}};
    if (valid && !cancelled_) outcome.insert(QStringLiteral("optionId"), optionId);
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), requestId},
          {QStringLiteral("result"), QJsonObject{{QStringLiteral("outcome"), outcome}}}});
    if (permissions_.isEmpty()) emit permissionsCleared();
}

void AcpClient::request(const QString &method, const QJsonObject &params, Callback callback, ErrorCallback error) {
    const auto id = nextId_++;
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id] {
        if (pending_.contains(id)) fail(QStringLiteral("ACP request timed out: %1").arg(pending_.value(id).method));
    });
    pending_.insert(id, Pending{method, std::move(callback), timer, std::move(error)});
    if (method != QStringLiteral("session/prompt")) timer->start(60000);
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id},
          {QStringLiteral("method"), method}, {QStringLiteral("params"), params}});
}

void AcpClient::send(const QJsonObject &message) {
    process_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void AcpClient::receive(const QJsonObject &message) {
    const auto id = message.value(QStringLiteral("id"));
    const auto method = message.value(QStringLiteral("method")).toString();
    if (!method.isEmpty()) {
        auto params = message.value(QStringLiteral("params")).toObject();
        if (method == QStringLiteral("session/update") && id.isUndefined()) {
            if (params.value(QStringLiteral("sessionId")).toString() != sessionId_ || sessionId_.isEmpty()) return;
            const auto update = params.value(QStringLiteral("update")).toObject();
            const auto kind = update.value(QStringLiteral("sessionUpdate")).toString();
            const auto toolId = update.value(QStringLiteral("toolCallId")).toString();
            if ((kind == QStringLiteral("tool_call") || kind == QStringLiteral("tool_call_update")) && !toolId.isEmpty()) {
                auto &tool = toolCalls_[toolId];
                for (auto it = update.begin(); it != update.end(); ++it) tool.insert(it.key(), it.value());
            }
            if (kind == QStringLiteral("config_option_update")) {
                configOptions_ = update.value(QStringLiteral("configOptions")).toArray();
                emit configOptionsChanged(configOptions_);
            }
            emit updated(update, replaying_);
        } else if (!id.isUndefined() && method == QStringLiteral("session/request_permission")) {
            auto call = params.value(QStringLiteral("toolCall")).toObject();
            auto merged = toolCalls_.value(call.value(QStringLiteral("toolCallId")).toString());
            for (auto it = call.begin(); it != call.end(); ++it) merged.insert(it.key(), it.value());
            params.insert(QStringLiteral("toolCall"), merged);
            permissions_.insert(permissionKey(id), QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("params"), params}});
            if (cancelled_ || replaying_ || !busy_ || params.value(QStringLiteral("sessionId")).toString() != sessionId_ || sessionId_.isEmpty()) answerPermission(id);
            else {
                QSet<QString> optionIds;
                bool validOptions = !params.value(QStringLiteral("options")).toArray().isEmpty();
                auto options = params.value(QStringLiteral("options")).toArray();
                for (qsizetype index = 0; index < options.size(); ++index) {
                    auto option = options.at(index).toObject();
                    const auto optionId = option.value(QStringLiteral("optionId")).toString();
                    const auto kind = option.value(QStringLiteral("kind")).toString();
                    if (optionId.isEmpty() || optionIds.contains(optionId) ||
                        !QStringList{QStringLiteral("allow_once"), QStringLiteral("allow_always"), QStringLiteral("reject_once"), QStringLiteral("reject_always")}.contains(kind)) validOptions = false;
                    optionIds.insert(optionId);
                    if (toolPermissions_) option.insert(QStringLiteral("pacsmithDescription"), toolPermissions_->description(params, option));
                    options[index] = option;
                }
                if (!validOptions) { answerPermission(id); return; }
                if (toolPermissions_) {
                    const auto remembered = toolPermissions_->remembered(params);
                    if (!remembered.isEmpty()) {
                        emit permissionRemembered(QStringLiteral("Allowed by saved permission: %1").arg(merged.value(QStringLiteral("title")).toString()));
                        answerPermission(id, remembered);
                        return;
                    }
                }
                params.insert(QStringLiteral("options"), options);
                emit permissionRequested(id, params);
            }
        } else if (!id.isUndefined()) {
            send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id},
                {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32601},
                    {QStringLiteral("message"), QStringLiteral("Client capability not supported")}}}});
        }
        return;
    }
    if (!id.isDouble() || !pending_.contains(id.toInteger())) return;
    auto pending = pending_.take(id.toInteger());
    pending.timer->stop();
    pending.timer->deleteLater();
    if (message.contains(QStringLiteral("error"))) {
        if (pending.error) {
            pending.error(message.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(QStringLiteral("ACP request failed")));
            return;
        }
        const auto ids = permissions_.keys();
        for (const auto &key : ids) answerPermission(permissions_.value(key).value(QStringLiteral("id")));
        replaying_ = false;
        cancelTimer_.stop();
        setBusy(false);
        emit failed(message.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(QStringLiteral("ACP request failed")));
        return;
    }
    pending.callback(message.value(QStringLiteral("result")).toObject());
}

void AcpClient::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged(busy);
}

void AcpClient::close() {
    closing_ = true;
    disconnect(startedConnection_);
    cancelTimer_.stop();
    const auto ids = permissions_.keys();
    for (const auto &key : ids) answerPermission(permissions_.value(key).value(QStringLiteral("id")));
    for (const auto &pending : std::as_const(pending_)) delete pending.timer;
    pending_.clear();
    ready_ = false;
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1000);
    }
    setBusy(false);
}

void AcpClient::fail(const QString &message) {
    close();
    emit failed(message);
}

} // namespace pacsmith
