#include "gui/gui_instance.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

#include <unistd.h>

namespace pacsmith::gui {
namespace {

QByteArray encodeMessage(const QString &importPath) {
    const QJsonObject object = importPath.isEmpty()
        ? QJsonObject{{QStringLiteral("command"), QStringLiteral("activate")}}
        : QJsonObject{{QStringLiteral("command"), QStringLiteral("import")},
                      {QStringLiteral("path"), importPath}};
    auto encoded = QJsonDocument(object).toJson(QJsonDocument::Compact);
    encoded.append('\n');
    return encoded;
}

QString importPathFromMessage(const QByteArray &line) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return {};
    const auto object = document.object();
    if (object.value(QStringLiteral("command")).toString() == QStringLiteral("import")) {
        return object.value(QStringLiteral("path")).toString();
    }
    return {};
}

}

GuiInstanceServer::GuiInstanceServer(QObject *parent) : QObject(parent) {}

QString GuiInstanceServer::socketName() {
    return QStringLiteral("pacsmith-gui-%1").arg(getuid());
}

bool GuiInstanceServer::activateExisting(const QString &importPath) {
    QLocalSocket socket;
    socket.connectToServer(socketName());
    if (!socket.waitForConnected(250)) return false;
    const auto payload = encodeMessage(importPath);
    if (socket.write(payload) != payload.size() || !socket.waitForBytesWritten(1000)) return false;
    socket.waitForDisconnected(250);
    return true;
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
            emit activated(importPathFromMessage(socket->readLine().trimmed()));
        }
        socket->disconnectFromServer();
    });
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
}

} // namespace pacsmith::gui
