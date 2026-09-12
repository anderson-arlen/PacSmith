#pragma once
#include "core/app_settings.hpp"
#include <QDateTime>
#include <QJsonObject>
#include <functional>
#include <optional>

namespace pacsmith {
struct AcpRegistryAgent {
    QString id;
    QString name;
    QString version;
    QString description;
    QJsonObject distribution;
};
struct AcpRegistrySnapshot {
    QList<AcpRegistryAgent> agents;
    QDateTime fetchedAt;
    bool fromCache{false};
    QString notice;
};
using RegistryFetcher = std::function<QByteArray(QString *)>;
std::optional<AcpRegistrySnapshot> parseAcpRegistry(const QByteArray &payload, QString *error);
std::optional<AcpRegistrySnapshot> loadAcpRegistry(const QString &cacheDirectory, bool refresh,
    QString *error, RegistryFetcher fetch = {});
std::optional<HarnessProfile> acpRegistryProfile(const AcpRegistryAgent &agent, QString *error,
    const QString &npx, const QString &uvx);
}
