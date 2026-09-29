#pragma once
#include "core/app_settings.hpp"
#include "core/http_transport.hpp"
#include <QJsonArray>

namespace pacsmith {
class ServerAi final {
  public:
    explicit ServerAi(ConnectionConfig connection) : connection_(std::move(connection)) {
        if (connection_.mode == ConnectionConfig::Mode::Local && connection_.socketPath.isEmpty())
            connection_ = ConnectionConfig::load();
    }
    [[nodiscard]] std::optional<QJsonObject> request(const QString &method, const QString &path,
                                                     const QJsonObject &body = {},
                                                     QString *error = nullptr) const;
    [[nodiscard]] std::optional<HarnessProfile> harness(QString *error = nullptr) const;
    [[nodiscard]] bool setHarness(const std::optional<HarnessProfile> &profile,
                                  QString *error = nullptr) const;
    [[nodiscard]] QJsonArray conversations(const QString &project, QString *error = nullptr) const;
    [[nodiscard]] static QJsonObject profileJson(const HarnessProfile &profile);
    [[nodiscard]] static HarnessProfile profileFromJson(const QJsonObject &object);

  private:
    ConnectionConfig connection_;
};
} // namespace pacsmith
