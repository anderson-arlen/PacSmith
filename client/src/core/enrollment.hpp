#pragma once

#include "core/http_transport.hpp"

#include <QString>

#include <functional>
#include <optional>

namespace pacsmith {

struct EnrollmentResult {
    ConnectionConfig config;
    QString fingerprint;
    QString fingerprintSha256;
    QString registrationId;
};

[[nodiscard]] QString clientPkiDirectory();
[[nodiscard]] ConnectionConfig remoteConnection(const QString &host, int port);
[[nodiscard]] bool parseRemoteTarget(const QString &text, QString *host, int *port, QString *error);
[[nodiscard]] QString defaultEnrollmentName();

[[nodiscard]] std::optional<EnrollmentResult> enrollRemote(
    const QString &host, int port, const QString &friendlyName,
    const std::function<bool(const QString &fingerprint, const QString &sha256)> &confirm,
    const std::function<void(const QString &)> &progress,
    const std::function<bool()> &waitOneSecond, QString *error);

} // namespace pacsmith
