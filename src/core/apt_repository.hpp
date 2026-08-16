#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>

#include <optional>

namespace pacsmith {

struct AptReleaseEntry {
    QString path;
    QString sha256;
    qint64 size{0};
};

struct AptPackageRecord {
    QString package;
    QString version;
    QString architecture;
    QString filename;
    QString sha256;
    qint64 size{0};
};

class DebianVersion final {
public:
    [[nodiscard]] static int compare(const QString &left, const QString &right);
};

class AptRepositoryMetadata final {
public:
    [[nodiscard]] static QList<AptReleaseEntry> parseRelease(QByteArrayView data, QString *error = nullptr);
    [[nodiscard]] static std::optional<AptReleaseEntry> selectPackagesIndex(
        const QList<AptReleaseEntry> &entries, const QString &component,
        const QString &architecture, bool flatRepository);
    [[nodiscard]] static std::optional<QByteArray> decompressIndex(
        const QByteArray &data, QString *error = nullptr);
    [[nodiscard]] static std::optional<AptPackageRecord> latestPackage(
        QByteArrayView packages, const QString &packageName,
        const QString &architecture, QString *error = nullptr);
};

} // namespace pacsmith
