#include "core/server_ai.hpp"
#include <QJsonDocument>
#include <QUrl>
namespace pacsmith {
std::optional<QJsonObject> ServerAi::request(const QString &method, const QString &path,
                                             const QJsonObject &body, QString *error) const {
    const auto response =
        HttpTransport(connection_)
            .request(method, QStringLiteral("/api/v1/ai") + path,
                     {{QStringLiteral("Content-Type"), QStringLiteral("application/json")}},
                     method == QStringLiteral("GET") ? QByteArray{}
                                                     : QJsonDocument(body).toJson(QJsonDocument::Compact));
    const auto object = QJsonDocument::fromJson(response.body).object();
    if (response.status < 200 || response.status >= 300 || !response.error.isEmpty()) {
        if (error)
            *error =
                !response.error.isEmpty()
                    ? response.error
                    : object.value(QStringLiteral("error"))
                          .toObject()
                          .value(QStringLiteral("message"))
                          .toString(QStringLiteral("Server AI request failed (%1)").arg(response.status));
        return std::nullopt;
    }
    return object;
}
QJsonObject ServerAi::profileJson(const HarnessProfile &p) {
    QJsonObject env;
    for (auto it = p.environment.begin(); it != p.environment.end(); ++it)
        env.insert(it.key(), it.value());
    return {{QStringLiteral("name"), p.name},
            {QStringLiteral("executable"), p.executable},
            {QStringLiteral("arguments"), QJsonArray::fromStringList(p.arguments)},
            {QStringLiteral("environment"), env},
            {QStringLiteral("registryId"), p.registryId},
            {QStringLiteral("registryVersion"), p.registryVersion},
            {QStringLiteral("protocol"), QStringLiteral("acp")},
            {QStringLiteral("configDefaults"), p.configDefaults}};
}
HarnessProfile ServerAi::profileFromJson(const QJsonObject &o) {
    HarnessProfile p;
    p.name = o.value(QStringLiteral("name")).toString();
    p.executable = o.value(QStringLiteral("executable")).toString();
    for (const auto &a : o.value(QStringLiteral("arguments")).toArray())
        p.arguments.append(a.toString());
    const auto env = o.value(QStringLiteral("environment")).toObject();
    for (auto it = env.begin(); it != env.end(); ++it)
        p.environment.insert(it.key(), it.value().toString());
    p.registryId = o.value(QStringLiteral("registryId")).toString();
    p.registryVersion = o.value(QStringLiteral("registryVersion")).toString();
    p.configDefaults = o.value(QStringLiteral("configDefaults")).toObject();
    return p;
}
std::optional<HarnessProfile> ServerAi::harness(QString *error) const {
    const auto result = request(QStringLiteral("GET"), QStringLiteral("/settings"), {}, error);
    if (!result || !result->value(QStringLiteral("harness")).isObject())
        return std::nullopt;
    return profileFromJson(result->value(QStringLiteral("harness")).toObject());
}
bool ServerAi::setHarness(const std::optional<HarnessProfile> &profile, QString *error) const {
    const auto current = request(QStringLiteral("GET"), QStringLiteral("/settings"), {}, error);
    if (!current)
        return false;
    return request(QStringLiteral("PUT"), QStringLiteral("/settings"),
                   {{QStringLiteral("revision"), current->value(QStringLiteral("revision"))},
                    {QStringLiteral("harness"),
                     profile ? QJsonValue(profileJson(*profile)) : QJsonValue(QJsonValue::Null)}},
                   error)
        .has_value();
}
QJsonArray ServerAi::conversations(const QString &project, QString *error) const {
    const auto result = request(QStringLiteral("GET"),
                                QStringLiteral("/conversations?project_id=") +
                                    QString::fromLatin1(QUrl::toPercentEncoding(project)),
                                {}, error);
    return result ? result->value(QStringLiteral("conversations")).toArray() : QJsonArray{};
}
} // namespace pacsmith
