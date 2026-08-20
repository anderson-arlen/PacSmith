#pragma once

#include "core/model.hpp"

#include <QString>
#include <utility>

namespace pacsmith {

class PkgbuildGenerator final {
public:
    [[nodiscard]] static QString generate(const PackageRelease &release);
    [[nodiscard]] static QString identityVariables(const PackageRelease &release);
    [[nodiscard]] static QString installedPayloadPath(const PackageRelease &release,
                                                      const QString &payloadPath);
    [[nodiscard]] static QString sanitizePackageName(const QString &name);
    [[nodiscard]] static QString translateVersion(const QString &debianVersion);
    [[nodiscard]] static std::pair<QString, QString> splitEpochAndVersion(const QString &debianVersion);
    [[nodiscard]] static QString translateArchitecture(const QString &debianArchitecture);
    [[nodiscard]] static QString shellQuote(const QString &value);
    [[nodiscard]] static QString validate(const QString &contents);
};

} // namespace pacsmith
