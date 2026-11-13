#pragma once

#include "core/acp_client.hpp"
#include "core/library_client.hpp"
#include <QDialog>

class QVBoxLayout;
class QLabel;

namespace pacsmith::gui {

class AgentSettingsDialog final : public QDialog {
public:
    explicit AgentSettingsDialog(const ConnectionConfig &connection, QWidget *parent = nullptr);
private:
    void initialize();
    void render(const QJsonArray &options);
    AcpClient *agent_;
    bool lockedMode_;
    ConnectionConfig connection_;
    std::optional<HarnessProfile> profile_;
    QWidget *fields_;
    QVBoxLayout *fieldsLayout_;
    QLabel *status_;
};

} // namespace pacsmith::gui
