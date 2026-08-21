#pragma once

#include "core/http_transport.hpp"

#include <QString>

namespace pacsmith {

[[nodiscard]] bool startLocalLibraryDaemon(QString *error = nullptr);
[[nodiscard]] bool stopLocalLibraryDaemon(QString *error = nullptr);
[[nodiscard]] bool applyLibraryRuntime(const ConnectionConfig &config, QString *error = nullptr);
[[nodiscard]] bool waitForLocalSocket(const QString &socketPath, int timeoutMs = 15000,
                                      QString *error = nullptr);

} // namespace pacsmith
