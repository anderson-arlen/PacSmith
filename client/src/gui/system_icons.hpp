#pragma once

#include <QIcon>
#include <QList>
#include <QString>

class QWidget;

namespace pacsmith::gui {

struct SystemIconChoice {
    QString label;
    QString name;
    QIcon icon;
};

void initializeSystemIconTheme();
[[nodiscard]] QIcon systemIcon(const QString &name);
[[nodiscard]] QList<SystemIconChoice> standardSystemIcons();
[[nodiscard]] QList<SystemIconChoice> availableSystemIcons();
[[nodiscard]] QString chooseSystemIcon(QWidget *parent);

} // namespace pacsmith::gui
