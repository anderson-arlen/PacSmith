#include "gui/wheel_scroll_guard.hpp"

#include <QApplication>
#include <QComboBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTest>
#include <QWheelEvent>

namespace pacsmith::gui {
namespace {

void sendWheel(QWidget *target, const int verticalDelta) {
    const QPointF position(target->rect().center());
    const QPointF globalPosition(target->mapToGlobal(position.toPoint()));
    QWheelEvent event(position, globalPosition, {}, QPoint(0, verticalDelta), Qt::NoButton, Qt::NoModifier,
                      Qt::ScrollUpdate, false);
    QApplication::sendEvent(target, &event);
}

class WheelScrollGuardTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        guard_ = new WheelScrollGuard(this);
        qApp->installEventFilter(guard_);
    }

    void wheelOverValueControlsScrollsThePage() {
        QScrollArea area;
        area.resize(280, 180);
        auto *content = new QWidget;
        content->setMinimumSize(260, 800);
        auto *spinBox = new QSpinBox(content);
        spinBox->setRange(0, 10);
        spinBox->setValue(5);
        spinBox->move(20, 200);
        auto *comboBox = new QComboBox(content);
        comboBox->addItems({QStringLiteral("First"), QStringLiteral("Second"), QStringLiteral("Third")});
        comboBox->setCurrentIndex(1);
        comboBox->move(20, 500);
        area.setWidget(content);
        area.show();
        QApplication::processEvents();

        area.verticalScrollBar()->setValue(100);
        sendWheel(spinBox, -120);
        QCOMPARE(spinBox->value(), 5);
        QVERIFY(area.verticalScrollBar()->value() > 100);

        area.verticalScrollBar()->setValue(400);
        sendWheel(comboBox, -120);
        QCOMPARE(comboBox->currentIndex(), 1);
        QVERIFY(area.verticalScrollBar()->value() > 400);
    }

    void wheelCannotEditAValueWithoutAScrollArea() {
        QSpinBox spinBox;
        spinBox.setRange(0, 10);
        spinBox.setValue(5);
        spinBox.show();
        QApplication::processEvents();

        sendWheel(&spinBox, 120);
        QCOMPARE(spinBox.value(), 5);
    }

private:
    WheelScrollGuard *guard_{nullptr};
};

} // namespace
} // namespace pacsmith::gui

QTEST_MAIN(pacsmith::gui::WheelScrollGuardTest)

#include "wheel_scroll_guard_test.moc"
