#pragma once

#include <QList>
#include <QHash>
#include <QMap>
#include <QString>

#include <optional>

namespace pacsmith {

struct ManagedPackageInfo {
    QString packageName;
    QString packageVersion;
    QMap<QString, QString> xdata;

    [[nodiscard]] QString projectId() const;
    [[nodiscard]] QString releaseId() const;
    [[nodiscard]] QString sourceIdentity() const;
};

class ManagedPackageRegistry final {
public:
    [[nodiscard]] static QHash<QString, ManagedPackageInfo> snapshot(QString *error = nullptr);
    [[nodiscard]] static QList<ManagedPackageInfo> installed(QString *error = nullptr);
    [[nodiscard]] static std::optional<ManagedPackageInfo> find(const QString &packageName,
                                                                QString *error = nullptr);
};

} // namespace pacsmith
