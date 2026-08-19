#pragma once

#include <QObject>
#include <QLocalServer>
#include <QString>

namespace pacsmith::gui {

class GuiInstanceServer final : public QObject {
    Q_OBJECT
public:
    explicit GuiInstanceServer(QObject *parent = nullptr);

    [[nodiscard]] static QString socketName();
    [[nodiscard]] static bool activateExisting(const QString &importPath = {});
    [[nodiscard]] static bool requestCheck();
    [[nodiscard]] static bool requestTray();
    [[nodiscard]] bool listen();

signals:
    void activated(const QString &importPath);
    void checkRequested();
    void trayRequested();

private:
    void acceptConnection();
    [[nodiscard]] static bool sendCommand(const QString &command, const QString &importPath = {});

    QLocalServer server_;
};

} // namespace pacsmith::gui
