#include "core/acp_tool_permissions.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

namespace pacsmith {
namespace {
QString fingerprint(const QJsonObject &object) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
QString localTool(const QJsonObject &params) {
    const auto call = params.value(QStringLiteral("toolCall")).toObject();
    const auto input = call.value(QStringLiteral("rawInput")).toObject();
    const auto name = input.value(QStringLiteral("tool")).toString();
    static const QRegularExpression valid(QStringLiteral("\\A[a-z][a-z0-9_]*\\z"));
    if (!call.value(QStringLiteral("_meta")).toObject().value(QStringLiteral("is_mcp_tool_call")).toBool() ||
        input.value(QStringLiteral("server")).toString() != QStringLiteral("pacsmith_session") || !valid.match(name).hasMatch()) return {};
    return name;
}
}
AcpToolPermissions::AcpToolPermissions(QString directory, const QJsonObject &identity)
    : directory_(QDir(directory).filePath(fingerprint(identity))) {}
QJsonObject AcpToolPermissions::grant(const QString &tool, const QString &session) const {
    return {{QStringLiteral("version"), 1}, {QStringLiteral("server"), QStringLiteral("pacsmith_session")},
            {QStringLiteral("tool"), tool}, {QStringLiteral("session"), session}};
}
QString AcpToolPermissions::path(const QJsonObject &grant) const {
    return QDir(directory_).filePath(fingerprint(grant) + QStringLiteral(".json"));
}
QString AcpToolPermissions::remembered(const QJsonObject &params) const {
    const auto tool = localTool(params);
    if (tool.isEmpty()) return {};
    bool allowed = false;
    const auto session = params.value(QStringLiteral("sessionId")).toString();
    for (const auto &scope : {QString{}, session}) {
        const auto record = grant(tool, scope);
        QFile file(path(record));
        if (file.open(QIODevice::ReadOnly) && file.readAll() == QJsonDocument(record).toJson(QJsonDocument::Compact)) allowed = true;
    }
    if (!allowed) return {};
    for (const auto &entry : params.value(QStringLiteral("options")).toArray()) {
        const auto option = entry.toObject();
        if (option.value(QStringLiteral("kind")).toString() == QStringLiteral("allow_once"))
            return option.value(QStringLiteral("optionId")).toString();
    }
    return {};
}
QString AcpToolPermissions::description(const QJsonObject &params, const QJsonObject &option) const {
    if (localTool(params).isEmpty() || option.value(QStringLiteral("kind")).toString() != QStringLiteral("allow_always")) return {};
    const auto id = option.value(QStringLiteral("optionId")).toString();
    if (id == QStringLiteral("allow_session")) return QStringLiteral("Remember this tool for this conversation, including after resuming it.");
    if (id == QStringLiteral("allow_always")) return QStringLiteral("Remember this tool for future conversations with this agent and library, including after restarting PacSmith.");
    return {};
}
bool AcpToolPermissions::remember(const QJsonObject &params, const QString &optionId, QString *error) const {
    // Only the Codex adapter's explicit MCP scopes have known durable semantics.
    // Shell commands, display titles, other servers and unknown scopes never become grants.
    for (const auto &entry : params.value(QStringLiteral("options")).toArray()) {
        const auto option = entry.toObject();
        if (option.value(QStringLiteral("optionId")).toString() != optionId || description(params, option).isEmpty()) continue;
        const auto session = params.value(QStringLiteral("sessionId")).toString();
        if (optionId == QStringLiteral("allow_session") && session.isEmpty()) return true;
        const auto record = grant(localTool(params), optionId == QStringLiteral("allow_session") ? session : QString{});
        QDir().mkpath(directory_);
        QSaveFile file(path(record));
        const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
        if (file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) &&
            file.write(bytes) == bytes.size() && file.commit()) return true;
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}
bool AcpToolPermissions::clear(QString *error) const {
    for (const auto &file : QDir(directory_).entryList({QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks)) {
        QFile grantFile(QDir(directory_).filePath(file));
        if (!grantFile.remove()) {
            if (error) *error = grantFile.errorString();
            return false;
        }
    }
    return true;
}
} // namespace pacsmith
