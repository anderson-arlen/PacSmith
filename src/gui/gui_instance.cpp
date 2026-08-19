#include "gui/gui_instance.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

#include <unistd.h>

namespace pacsmith::gui {
namespace {

QByteArray encodeMessage(const QString &command, const QString &importPath) {
    QJsonObject object{{QStringLiteral("command"), command}};
    if (!importPath.isEmpty()) object.insert(QStringLiteral("path"), importPath);
    auto encoded = QJsonDocument(object).toJson(QJsonDocument::Compact);
    encoded.append('\n');
    return encoded;
}

}

GuiInstanceServer::GuiInstanceServer(QObject *parent) : QObject(parent) {}

QString GuiInstanceServer::socketName() {
    return QStringLiteral("pacsmith-gui-%1").arg(getuid());
}

bool GuiInstanceServer::sendCommand(const QString &command, const QString &importPath) {
    QLocalSocket socket;
    socket.connectToServer(socketName());
    if (!socket.waitForConnected(250)) return false;
    const auto payload = encodeMessage(command, importPath);
    if (socket.write(payload) != payload.size() || !socket.waitForBytesWritten(1000)) return false;
    socket.waitForDisconnected(250);
    return true;
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
