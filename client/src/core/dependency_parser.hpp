#pragma once

#include "core/model.hpp"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>

namespace pacsmith {

class DependencyParser final {
public:
    [[nodiscard]] static QList<DependencyMapping> parse(const QString &declarations);
    [[nodiscard]] static bool applyVerifiedMappings(QList<DependencyMapping> &dependencies,
                                                    const QMap<QString, QString> &mappings);
    [[nodiscard]] static QMap<QString, QString> loadVerifiedMappings();
};

} // namespace pacsmith
