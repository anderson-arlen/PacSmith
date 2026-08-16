#pragma once

#include <QString>
#include <QStringList>

namespace pacsmith {

struct LifecycleValidation {
    bool passed{false};
    QStringList problems;

    [[nodiscard]] QString message() const;
};

class LifecycleValidator final {
public:
    [[nodiscard]] static LifecycleValidation validate(const QString &contents);
};

} // namespace pacsmith
