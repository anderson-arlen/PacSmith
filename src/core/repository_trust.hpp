#pragma once

#include "core/model.hpp"
#include "core/script_evidence.hpp"

#include <filesystem>
#include <optional>

namespace pacsmith {

struct RepositoryKeyInspection {
    QString sha256;
    QStringList fingerprints;
};

class RepositoryTrust final {
public:
    [[nodiscard]] static std::optional<RepositorySigningKey>
    storeVendorKey(const std::filesystem::path &projectDirectory,
                   const ExtractedSigningKey &candidate, QString *error = nullptr);
    [[nodiscard]] static std::optional<RepositorySigningKey>
    importUserKey(const std::filesystem::path &projectDirectory,
                  const QByteArray &contents, const QString &sourceDescription,
                  QString *error = nullptr);
    [[nodiscard]] static QStringList fingerprints(const std::filesystem::path &keyring,
                                                  QString *error = nullptr);
    [[nodiscard]] static std::optional<RepositoryKeyInspection>
    inspectKey(const QByteArray &contents, QString *error = nullptr);
};

} // namespace pacsmith
