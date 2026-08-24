#include "gui/wheel_scroll_guard.hpp"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QScrollBar>
#include <QTabBar>
#include <QWheelEvent>
#include <QWidget>

namespace pacsmith::gui {
namespace {

QWidget *wheelValueControl(QWidget *widget) {
    for (auto *candidate = widget; candidate != nullptr; candidate = candidate->parentWidget()) {
        if (auto *combo = qobject_cast<QComboBox *>(candidate)) {
            if (combo->view()->isVisible()) return nullptr;
            return combo;
        }
        if (qobject_cast<QAbstractSpinBox *>(candidate) != nullptr) return candidate;
        if (qobject_cast<QTabBar *>(candidate) != nullptr) return candidate;
        if (qobject_cast<QAbstractSlider *>(candidate) != nullptr &&
            qobject_cast<QScrollBar *>(candidate) == nullptr) {
            return candidate;
        }
    }
    return nullptr;
}

QAbstractScrollArea *enclosingScrollArea(QWidget *widget) {
    for (auto *ancestor = widget->parentWidget(); ancestor != nullptr; ancestor = ancestor->parentWidget()) {
        if (auto *scrollArea = qobject_cast<QAbstractScrollArea *>(ancestor)) return scrollArea;
    }
    return nullptr;
}

} // namespace

bool WheelScrollGuard::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() != QEvent::Wheel) return false;

    auto *target = qobject_cast<QWidget *>(watched);
    auto *control = target != nullptr ? wheelValueControl(target) : nullptr;
    if (control == nullptr) return false;

    auto *wheel = static_cast<QWheelEvent *>(event);
    if (auto *scrollArea = enclosingScrollArea(control)) {
        const auto viewportPosition = scrollArea->viewport()->mapFromGlobal(wheel->globalPosition().toPoint());
        QWheelEvent forwarded(QPointF(viewportPosition), wheel->globalPosition(), wheel->pixelDelta(),
                              wheel->angleDelta(), wheel->buttons(), wheel->modifiers(), wheel->phase(),
                              wheel->inverted(), wheel->source(), wheel->pointingDevice());
        QApplication::sendEvent(scrollArea->viewport(), &forwarded);
    }
    event->accept();
    return true;
}

} // namespace pacsmith::gui
