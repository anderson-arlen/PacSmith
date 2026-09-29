#include "core/server_ai.hpp"
#include "gui/application_session.hpp"
#include <QDialog>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace pacsmith::gui {
void ApplicationSession::refreshPermissions() {
    if (permissionsInFlight_)
        return;
    permissionsInFlight_ = true;
    const auto connection = ConnectionConfig::load();
    const auto origin = connection.origin() + connection.socketPath + connection.clientCertPath;
    if (permissionOrigin_ != origin) {
        for (auto dialog : permissionDialogs_)
            if (dialog)
                delete dialog;
        permissionDialogs_.clear();
        permissionOrigin_ = origin;
    }
    auto *watcher = new QFutureWatcher<std::optional<QJsonObject>>(this);
    connect(
        watcher, &QFutureWatcher<std::optional<QJsonObject>>::finished, this,
        [this, watcher, connection, origin] {
            const auto result = watcher->result();
            watcher->deleteLater();
            permissionsInFlight_ = false;
            const auto current = ConnectionConfig::load();
            if (current.origin() + current.socketPath + current.clientCertPath != origin || !result)
                return;
            QSet<QString> pending;
            for (const auto &entry : result->value(QStringLiteral("permissions")).toArray()) {
                const auto request = entry.toObject();
                const auto id = request.value(QStringLiteral("id")).toString();
                pending.insert(id);
                if (permissionDialogs_.contains(id))
                    continue;
                auto *dialog = new QDialog;
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->setWindowTitle(QStringLiteral("PacSmith — AI permission"));
                dialog->resize(560, 420);
                permissionDialogs_.insert(id, dialog);
                auto *layout = new QVBoxLayout(dialog);
                auto *server = new QLabel(connection.summary(), dialog);
                server->setTextFormat(Qt::PlainText);
                layout->addWidget(server);
                const auto params = request.value(QStringLiteral("params")).toObject();
                const auto call = params.value(QStringLiteral("toolCall")).toObject();
                auto *title =
                    new QLabel(call.value(QStringLiteral("title"))
                                   .toString(QStringLiteral("The server agent requests permission")),
                               dialog);
                title->setTextFormat(Qt::PlainText);
                title->setWordWrap(true);
                layout->addWidget(title);
                auto *details = new QPlainTextEdit(dialog);
                details->setReadOnly(true);
                details->setPlainText(QString::fromUtf8(QJsonDocument(call).toJson(QJsonDocument::Indented)));
                layout->addWidget(details);
                auto *status = new QLabel(
                    QStringLiteral("This request can be answered from any connected client."), dialog);
                status->setTextFormat(Qt::PlainText);
                status->setWordWrap(true);
                layout->addWidget(status);
                for (const auto &choice : params.value(QStringLiteral("options")).toArray()) {
                    const auto option = choice.toObject();
                    auto *button = new QPushButton(option.value(QStringLiteral("name")).toString(), dialog);
                    layout->addWidget(button);
                    connect(button, &QPushButton::clicked, dialog,
                            [this, connection, id, option, dialog, status] {
                                for (auto *choiceButton : dialog->findChildren<QPushButton *>())
                                    choiceButton->setEnabled(false);
                                auto *answer = new QFutureWatcher<QString>(dialog);
                                connect(answer, &QFutureWatcher<QString>::finished, dialog,
                                        [this, answer, dialog, status] {
                                            const auto error = answer->result();
                                            answer->deleteLater();
                                            if (error.isEmpty())
                                                dialog->close();
                                            else {
                                                status->setText(error);
                                                for (auto *choiceButton :
                                                     dialog->findChildren<QPushButton *>())
                                                    choiceButton->setEnabled(true);
                                            }
                                            refreshPermissions();
                                        });
                                answer->setFuture(QtConcurrent::run([connection, id, option] {
                                    QString error;
                                    static_cast<void>(
                                        ServerAi(connection)
                                            .request(QStringLiteral("POST"),
                                                     QStringLiteral("/permissions/%1/response").arg(id),
                                                     {{QStringLiteral("option_id"),
                                                       option.value(QStringLiteral("optionId"))}},
                                                     &error));
                                    return error;
                                }));
                            });
                }
                dialog->show();
            }
            for (auto it = permissionDialogs_.begin(); it != permissionDialogs_.end();) {
                if (!pending.contains(it.key())) {
                    if (it.value())
                        it.value()->close();
                    it = permissionDialogs_.erase(it);
                } else
                    ++it;
            }
        });
    watcher->setFuture(QtConcurrent::run([connection] {
        return ServerAi(connection).request(QStringLiteral("GET"), QStringLiteral("/permissions"));
    }));
}
} // namespace pacsmith::gui
