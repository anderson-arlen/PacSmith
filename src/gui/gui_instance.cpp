#include "gui/gui_instance.hpp"

#include "core/background_updates.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

namespace pacsmith::gui {

GuiInstanceServer::GuiInstanceServer(QObject *parent) : QObject(parent) {}

QString GuiInstanceServer::socketName() {
    return runningGuiSocketName();
}

bool GuiInstanceServer::sendCommand(const QString &command, const QString &importPath) {
    return notifyRunningGui(command, importPath);
}

bool GuiInstanceServer::activateExisting(const QString &importPath) {
    return importPath.isEmpty() ? sendCommand(QStringLiteral("activate"))
                                : sendCommand(QStringLiteral("import"), importPath);
}

bool GuiInstanceServer::requestCheck() {
    return sendCommand(QStringLiteral("check"));
}

bool GuiInstanceServer::requestTray() {
    return sendCommand(QStringLiteral("tray"));
}

bool GuiInstanceServer::requestProjectsReload() {
    return sendCommand(QStringLiteral("projects"));
}

bool GuiInstanceServer::listen() {
    const auto name = socketName();
    // Caller already tried to connect; any leftover socket is stale.
    QLocalServer::removeServer(name);
    if (!server_.listen(name)) return false;
    connect(&server_, &QLocalServer::newConnection, this, &GuiInstanceServer::acceptConnection);
    return true;
}

void GuiInstanceServer::acceptConnection() {
    auto *socket = server_.nextPendingConnection();
    if (socket == nullptr) return;
    connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
        while (socket->canReadLine()) {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(socket->readLine().trimmed(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) continue;
            const auto object = document.object();
            const auto command = object.value(QStringLiteral("command")).toString();
            if (command == QStringLiteral("check")) emit checkRequested();
            else if (command == QStringLiteral("tray")) emit trayRequested();
            else if (command == QStringLiteral("projects")) emit projectsReloadRequested();
            else if (command == QStringLiteral("import")) {
                emit activated(object.value(QStringLiteral("path")).toString());
            } else {
                emit activated({});
            }
        }
        socket->disconnectFromServer();
    });
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
}

} // namespace pacsmith::gui
