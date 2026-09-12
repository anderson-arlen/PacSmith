#pragma once

#include <QStringList>
#include <QDateTime>
#include <QList>

namespace pacsmith {

struct AcpConversation { QString key; QString description; QDateTime updatedAt; };

class AcpConversations {
public:
    explicit AcpConversations(QString directory);
    [[nodiscard]] QString latest(const QString &scope, const QStringList &legacyKeys = {}) const;
    bool select(const QString &scope, const QString &key, QString *error = nullptr) const;
    [[nodiscard]] static QString freshKey();
    [[nodiscard]] QList<AcpConversation> recent(const QString &scope, int limit = 10) const;
    [[nodiscard]] QString description(const QString &key) const;
    bool describe(const QString &key, const QString &description, QString *error = nullptr) const;
    void cleanup(const QDateTime &now = QDateTime::currentDateTimeUtc()) const;
private:
    QString directory_;
};

} // namespace pacsmith
