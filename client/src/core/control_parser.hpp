#pragma once

#include "core/model.hpp"

#include <QList>
#include <QMap>
#include <QByteArrayView>

namespace pacsmith {

class ControlParser final {
public:
    [[nodiscard]] static QList<QMap<QString, QString>> parseParagraphs(QByteArrayView data);
    [[nodiscard]] static DebianMetadata parsePackage(QByteArrayView data);
};

} // namespace pacsmith
