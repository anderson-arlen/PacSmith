#pragma once

#include "core/library_client.hpp"

#include <QJsonArray>
#include <QJsonObject>

namespace pacsmith::mcp {

class Server final {
public:
    explicit Server(LibraryClient client = LibraryClient{});
    int run();
    [[nodiscard]] static QJsonArray toolCatalog();
    [[nodiscard]] const ConnectionConfig &connectionConfig() const noexcept;

private:
    QJsonObject handleRequest(const QJsonObject &request);
    QJsonObject callTool(const QJsonValue &id, const QJsonObject &params);
    bool writeMessage(const QJsonObject &message);
    std::optional<QJsonObject> readMessage();

    LibraryClient library_;
    bool initialized_{false};
    QString protocolVersion_{QStringLiteral("2025-11-25")};
};

} // namespace pacsmith::mcp
