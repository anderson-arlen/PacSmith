#pragma once
#include <QJsonObject>
#include <QString>

namespace pacsmith {

class AcpToolPermissions {
public:
    AcpToolPermissions(QString directory, const QJsonObject &identity);
    [[nodiscard]] QString remembered(const QJsonObject &params) const;
    bool remember(const QJsonObject &params, const QString &optionId, QString *error) const;
    [[nodiscard]] QString description(const QJsonObject &params, const QJsonObject &option) const;
    bool clear(QString *error) const;
private:
    [[nodiscard]] QJsonObject grant(const QString &tool, const QString &session) const;
    [[nodiscard]] QString path(const QJsonObject &grant) const;
    QString directory_;
};

} // namespace pacsmith
