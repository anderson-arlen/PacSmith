#pragma once

#include <QString>

#include <filesystem>
#include <optional>

namespace pacsmith {

struct PayloadInspection {
    QString contentSha256;
    QString textPreview;
    bool previewTruncated{false};
    bool binary{false};
};

class PayloadInspector final {
public:
    [[nodiscard]] static std::optional<PayloadInspection> inspectFile(
        const std::filesystem::path &debPath, const QString &payloadPath, QString *error = nullptr);
    [[nodiscard]] static std::optional<QByteArray> readFileBytes(
        const std::filesystem::path &sourcePath, const QString &payloadPath,
        qsizetype maximumBytes, QString *error = nullptr);
};

} // namespace pacsmith
