#pragma once

#include <QString>
#include <QStringList>

namespace pacsmith {

struct PackageRelease;

struct LifecycleValidation {
    bool passed{false};
    QStringList problems;
    bool originalArchScript{false};

    [[nodiscard]] QString message() const;
};

class LifecycleValidator final {
public:
    [[nodiscard]] static LifecycleValidation validate(const QString &contents,
                                                       const PackageRelease *release = nullptr);
};

} // namespace pacsmith
