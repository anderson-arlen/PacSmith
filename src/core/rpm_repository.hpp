#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>

namespace pacsmith {

struct RpmRepomdPrimary {
    QString path;
    QString checksumType;
    QString checksum;
};

struct RpmPackageRecord {
    QString name;
    QString architecture;
    QString epoch;
    QString version;
    QString release;
    QString filename;
    QString checksumType;
    QString checksum;

    [[nodiscard]] QString evr() const;
};

class RpmVersion final {
public:
    [[nodiscard]] static int compare(const QString &left, const QString &right);
};

class RpmRepositoryMetadata final {
public:
    [[nodiscard]] static std::optional<RpmRepomdPrimary> parseRepomd(
        QByteArrayView data, QString *error = nullptr);
    [[nodiscard]] static std::optional<RpmPackageRecord> latestPackage(
        QByteArrayView primaryXml, const QString &packageName,
        const QString &architecture, QString *error = nullptr);
};

} // namespace pacsmith
