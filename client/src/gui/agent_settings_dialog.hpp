#pragma once

#include "core/acp_client.hpp"
#include "core/library_client.hpp"
#include <QDialog>

class QVBoxLayout;
class QLabel;

namespace pacsmith::gui {

class AgentSettingsDialog final : public QDialog {
public:
    using SaveDefaults = std::function<bool(const QJsonObject &, QString *)>;
    AgentSettingsDialog(const HarnessProfile &profile, const ConnectionConfig &connection,
                        SaveDefaults save, QWidget *parent = nullptr);
    AgentSettingsDialog(AcpClient *agent, bool lockedMode, QWidget *parent = nullptr);
private:
    void initialize(bool defaults);
    void render(const QJsonArray &options);
    AcpClient *agent_;
    bool lockedMode_;
    SaveDefaults save_;
    QWidget *fields_;
    QVBoxLayout *fieldsLayout_;
    QLabel *status_;
};

} // namespace pacsmith::gui
