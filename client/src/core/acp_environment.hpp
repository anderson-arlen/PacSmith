#pragma once

#include "core/app_settings.hpp"
#include "core/http_transport.hpp"

#include <QJsonArray>
#include <QProcessEnvironment>
#include <optional>

namespace pacsmith {

struct AcpEnvironment {
    QString dataDirectory;
    QString workspace;
    QProcessEnvironment process;
};

[[nodiscard]] bool isCodexAcp(const HarnessProfile &profile);
[[nodiscard]] QString acpDataDirectory();
[[nodiscard]] std::optional<AcpEnvironment> prepareAcpEnvironment(
    const HarnessProfile &profile, const QString &dataDirectory,
    const QProcessEnvironment &parent, QString *error);
[[nodiscard]] QJsonArray acpMcpServers(const ConnectionConfig &connection,
                                     const QString &cliExecutable, const QString &conversationKey = {});
[[nodiscard]] QString acpConversationKey(const ConnectionConfig &connection,
    const HarnessProfile &profile, const QString &projectId, const QString &releaseId);

} // namespace pacsmith
