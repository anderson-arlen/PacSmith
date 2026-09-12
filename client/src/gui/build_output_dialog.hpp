#pragma once

#include "core/library_client.hpp"
#include "gui/command_progress_dialog.hpp"
#include <QTimer>

namespace pacsmith::gui {

class BuildOutputDialog final : public CommandProgressDialog {
    Q_OBJECT
public:
    BuildOutputDialog(ConnectionConfig connection, QString jobId, QWidget *parent = nullptr);

private:
    void poll();
    void cancelBuild();
    ConnectionConfig connection_;
    QString jobId_;
    QTimer pollTimer_;
    qint64 offset_{0};
    bool polling_{false};
    bool cancelPending_{false};
};

} // namespace pacsmith::gui
