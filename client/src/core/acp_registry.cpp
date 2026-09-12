#include "core/acp_registry.hpp"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <algorithm>

namespace pacsmith {
namespace {
constexpr qint64 maxRegistryBytes = 4 * 1024 * 1024;
QByteArray fetchRegistry(QString *error) {
    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(QStringLiteral("https://cdn.agentclientprotocol.com/registry/v1/latest/registry.json")));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(15000);
    auto *reply = network.get(request);
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [reply] {
        if (reply->bytesAvailable() > maxRegistryBytes) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    deadline.start(15000);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        *error = QStringLiteral("Could not load ACP registry: %1").arg(reply->errorString());
        return {};
    }
    return reply->readAll();
}
}
std::optional<AcpRegistrySnapshot> parseAcpRegistry(const QByteArray &payload, QString *error) {
    const auto root = QJsonDocument::fromJson(payload).object();
    if (payload.size() > maxRegistryBytes || !root.value(QStringLiteral("agents")).isArray()) {
        *error = QStringLiteral("Invalid ACP registry response.");
        return std::nullopt;
    }
    AcpRegistrySnapshot result;
    QSet<QString> ids;
    for (const auto &entry : root.value(QStringLiteral("agents")).toArray()) {
        const auto item = entry.toObject();
        AcpRegistryAgent agent{item.value(QStringLiteral("id")).toString(), item.value(QStringLiteral("name")).toString(),
            item.value(QStringLiteral("version")).toString(), item.value(QStringLiteral("description")).toString(),
            item.value(QStringLiteral("distribution")).toObject()};
        if (agent.id.trimmed().isEmpty() || agent.name.trimmed().isEmpty() || agent.version.trimmed().isEmpty() ||
            agent.distribution.isEmpty() || ids.contains(agent.id)) {
            *error = QStringLiteral("Invalid agent entry in the ACP registry.");
            return std::nullopt;
        }
        ids.insert(agent.id);
        result.agents.append(agent);
    }
    std::sort(result.agents.begin(), result.agents.end(), [](const auto &left, const auto &right) {
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    return result;
}
std::optional<AcpRegistrySnapshot> loadAcpRegistry(const QString &cacheDirectory, bool refresh,
    QString *error, RegistryFetcher fetch) {
    const auto path = QDir(cacheDirectory).filePath(QStringLiteral("registry.json"));
    QFile cache(path);
    QByteArray cached;
    if (cache.open(QIODevice::ReadOnly)) cached = cache.read(maxRegistryBytes + 1);
    QString cacheError;
    auto previous = parseAcpRegistry(cached, &cacheError);
    if (previous) {
        previous->fetchedAt = QFileInfo(path).lastModified();
        previous->fromCache = true;
        if (!refresh && previous->fetchedAt.secsTo(QDateTime::currentDateTimeUtc()) < 3 * 60 * 60) return previous;
    }
    QString fetchError;
    const auto payload = (fetch ? fetch : fetchRegistry)(&fetchError);
    auto result = fetchError.isEmpty() ? parseAcpRegistry(payload, &fetchError) : std::nullopt;
    if (!result) {
        if (previous) { previous->notice = QStringLiteral("Offline · using cached registry. %1").arg(fetchError); return previous; }
        *error = fetchError;
        return std::nullopt;
    }
    result->fetchedAt = QDateTime::currentDateTimeUtc();
    QSaveFile saved(path);
    if (!QDir().mkpath(cacheDirectory) || !saved.open(QIODevice::WriteOnly) || saved.write(payload) != payload.size() || !saved.commit()) {
        result->notice = QStringLiteral("Registry loaded, but the offline cache could not be saved.");
    }
    return result;
}
std::optional<HarnessProfile> acpRegistryProfile(const AcpRegistryAgent &agent, QString *error,
    const QString &npx, const QString &uvx) {
    QString type;
    if (agent.distribution.value(QStringLiteral("npx")).isObject()) type = QStringLiteral("npx");
    else if (agent.distribution.value(QStringLiteral("uvx")).isObject()) type = QStringLiteral("uvx");
    else { *error = QStringLiteral("This agent has no npm or uvx distribution. Use an installed ACP executable in a custom profile."); return std::nullopt; }
    const auto spec = agent.distribution.value(type).toObject();
    const auto package = spec.value(QStringLiteral("package")).toString();
    if (package.isEmpty() || package.startsWith(QLatin1Char('-')) || package.contains(QChar::Null) || package.contains(QLatin1Char('\n'))) {
        *error = QStringLiteral("The registry has an invalid package name."); return std::nullopt;
    }
    HarnessProfile profile;
    profile.name = agent.name;
    profile.executable = type == QStringLiteral("npx") ? npx : uvx;
    if (profile.executable.isEmpty()) {
        *error = QStringLiteral("%1 requires %2. Install %2 and reopen the registry.").arg(agent.name, type);
        return std::nullopt;
    }
    profile.arguments = type == QStringLiteral("npx") ? QStringList{QStringLiteral("-y"), package} : QStringList{package};
    for (const auto &argument : spec.value(QStringLiteral("args")).toArray()) {
        if (!argument.isString() || argument.toString().contains(QChar::Null)) {
            *error = QStringLiteral("The registry has invalid agent arguments."); return std::nullopt;
        }
        profile.arguments.append(argument.toString());
    }
    const auto environment = spec.value(QStringLiteral("env")).toObject();
    for (auto it = environment.begin(); it != environment.end(); ++it) {
        if (!it.value().isString() || it.key().contains(QLatin1Char('=')) || it.key().contains(QChar::Null) || it.value().toString().contains(QChar::Null)) {
            *error = QStringLiteral("The registry has an invalid agent environment."); return std::nullopt;
        }
        profile.environment.insert(it.key(), it.value().toString());
    }
    profile.registryId = agent.id;
    profile.registryVersion = agent.version;
    return profile;
}
}
