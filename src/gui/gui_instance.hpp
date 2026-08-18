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
    [[nodiscard]] bool listen();

signals:
    void activated(const QString &importPath);

private:
    void acceptConnection();

    QLocalServer server_;
};

} // namespace pacsmith::gui
