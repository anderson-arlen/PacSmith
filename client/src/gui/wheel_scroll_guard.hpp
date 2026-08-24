#pragma once

#include <QObject>

namespace pacsmith::gui {

class WheelScrollGuard final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

} // namespace pacsmith::gui
