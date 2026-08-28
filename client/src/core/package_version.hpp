#pragma once

#include <QString>

namespace pacsmith {

class DebianVersion final {
public:
    [[nodiscard]] static int compare(const QString &left, const QString &right);
};

class RpmVersion final {
public:
    [[nodiscard]] static int compare(const QString &left, const QString &right);
};

} // namespace pacsmith
