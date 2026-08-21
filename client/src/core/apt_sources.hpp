#pragma once

#include "core/model.hpp"

#include <QByteArrayView>
#include <QList>
#include <QString>

namespace pacsmith {

class AptSourcesParser final {
public:
    [[nodiscard]] static QList<AptRepositoryCandidate> parse(QByteArrayView data,
                                                              const QString &sourcePath = {});
};

} // namespace pacsmith
