#include "gui/future_button_guard.hpp"
#include "gui/appearance.hpp"
#include "gui/main_window/project_list_delegate.hpp"
#include "gui/project_history_view.hpp"
#include "gui/wheel_scroll_guard.hpp"

#include "core/model.hpp"

#include <QApplication>
#include <QComboBox>
#include <QPromise>
#include <QPushButton>
#include <QListWidget>
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

    void futureCompletionReenablesButtonAfterPointerVariableExpires() {
        QPushButton button;
        QPromise<int> promise;
        promise.start();
        bool finished = false;

        {
            auto *shortLivedPointer = &button;
            watchFutureWithDisabledButton(
                shortLivedPointer, this, promise.future(), [&](const int result) {
                    QCOMPARE(result, 42);
                    finished = true;
                });
        }

        QVERIFY(!button.isEnabled());
        promise.addResult(42);
        promise.finish();
        QTRY_VERIFY(finished);
        QVERIFY(button.isEnabled());
    }

    void futureCompletionToleratesDestroyedButton() {
        QPromise<int> promise;
        promise.start();
        bool finished = false;
        auto *button = new QPushButton;
        watchFutureWithDisabledButton(button, this, promise.future(), [&](const int) {
            finished = true;
        });
        delete button;

        promise.addResult(1);
        promise.finish();
        QTRY_VERIFY(finished);
    }

    void projectHistoryRendersNewestFirst() {
        Project project;
        project.history.append({QDateTime::fromString(QStringLiteral("2026-08-28T12:00:00Z"),
                                                      Qt::ISODate),
                                QStringLiteral("import"), QStringLiteral("Imported release")});
        project.history.append({QDateTime::fromString(QStringLiteral("2026-08-28T13:00:00Z"),
                                                      Qt::ISODate),
                                QStringLiteral("update-check"),
                                QStringLiteral("No newer version")});
        QListWidget list;

        populateProjectHistory(&list, &project);

        QCOMPARE(list.count(), 2);
        QVERIFY(list.item(0)->text().contains(QStringLiteral("update-check")));
        QVERIFY(list.item(0)->text().contains(QStringLiteral("No newer version")));
        QVERIFY(list.item(1)->text().contains(QStringLiteral("import")));
    }

    void projectListDelegateRendersStatusBadge() {
        applyInterfaceTheme(AppearanceMode::Dark);
        QListWidget list;
        list.resize(280, 70);
        list.setIconSize(QSize(44, 44));
        list.setItemDelegate(new ProjectListDelegate(&list));
        auto *item = new QListWidgetItem(QStringLiteral("Visual Studio Code"), &list);
        QPixmap projectIcon(44, 44);
        projectIcon.fill(QColor(55, 95, 160));
        item->setIcon(QIcon(projectIcon));
        item->setData(projectSubtitleRole, QStringLiteral("code-bin"));
        item->setData(projectVisualStateRole, static_cast<int>(ProjectVisualState::Current));
        item->setData(projectRepositoryEnabledRole, true);
        item->setSizeHint(QSize(0, 60));
        list.show();
        QApplication::processEvents();

        QImage rendered(list.size(), QImage::Format_ARGB32_Premultiplied);
        rendered.fill(Qt::transparent);
        QPainter painter(&rendered);
        list.render(&painter);
        painter.end();
        const auto screenshot = qEnvironmentVariable("PACSMITH_GUI_TEST_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(rendered.save(screenshot));

        int greenPixels = 0;
        int readableSubtitlePixels = 0;
        for (int y = 0; y < rendered.height(); ++y) {
            for (int x = 0; x < rendered.width(); ++x) {
                const auto color = rendered.pixelColor(x, y);
                if (color.green() > 120 && color.green() > color.red() * 2 &&
                    color.green() * 10 > color.blue() * 13) {
                    ++greenPixels;
                }
                if (x >= 64 && x <= 180 && y >= 32 && y <= 53 &&
                    color.lightness() >= 128) {
                    ++readableSubtitlePixels;
                }
            }
        }
        QVERIFY2(greenPixels > 20, "the up-to-date badge was not visibly rendered");
        QVERIFY2(readableSubtitlePixels > 10, "the project subtitle was not readable in dark mode");
        applyInterfaceTheme(AppearanceMode::Auto);
    }

    void trayAppearanceKeepsContrastingGlyphAndBottomBadge() {
        applyInterfaceTheme(AppearanceMode::Dark);
        QApplication::processEvents();
        const auto darkPalette = QApplication::palette();
        QVERIFY(darkPalette.color(QPalette::Window).lightness() < 128);
        QVERIFY(darkPalette.color(QPalette::PlaceholderText).lightness() >= 128);
        QVERIFY(darkPalette.color(QPalette::Disabled, QPalette::Text).lightness() >= 128);
        applyInterfaceTheme(AppearanceMode::Light);
        QApplication::processEvents();
        const auto lightPalette = QApplication::palette();
        QVERIFY(lightPalette.color(QPalette::Window).lightness() >= 128);
        QVERIFY(lightPalette.color(QPalette::PlaceholderText).lightness() < 128);
        QVERIFY(lightPalette.color(QPalette::Disabled, QPalette::Text).lightness() < 128);
        applyInterfaceTheme(AppearanceMode::Auto);

        QCOMPARE(trayIconColor(AppearanceMode::Light), QColor(Qt::black));
        QCOMPARE(trayIconColor(AppearanceMode::Dark), QColor(Qt::white));

        QPixmap mask(32, 32);
        mask.fill(Qt::transparent);
        QPainter maskPainter(&mask);
        maskPainter.fillRect(QRect(8, 8, 16, 16), Qt::black);
        maskPainter.end();

        const auto icon = renderTrayStatusIcon(mask, 3, Qt::white);
        const auto rendered = icon.pixmap(32, 32).toImage();
        QVERIFY(rendered.pixelColor(2, 2).alpha() == 0);
        QCOMPARE(rendered.pixelColor(12, 12), QColor(Qt::white));

        const auto active = renderTrayStatusIcon(mask, 0, Qt::white, 2).pixmap(32, 32).toImage();
        int activityPixels = 0;
        for (int y = 16; y < active.height(); ++y) {
            for (int x = 16; x < active.width(); ++x) {
                const auto color = active.pixelColor(x, y);
                if (color.blue() > 180 && color.green() > 120 && color.red() < 140) {
                    ++activityPixels;
                }
            }
        }
        QVERIFY2(activityPixels > 4, "the tray build activity spinner was not rendered");

        int topRightRed = 0;
        int bottomRightRed = 0;
        for (int y = 0; y < rendered.height(); ++y) {
            for (int x = 16; x < rendered.width(); ++x) {
                const auto color = rendered.pixelColor(x, y);
                if (color.red() < 150 || color.red() < color.green() * 2) continue;
                if (y < 16) ++topRightRed;
                else ++bottomRightRed;
            }
        }
        QCOMPARE(topRightRed, 0);
        QVERIFY(bottomRightRed > 20);
    }

private:
    WheelScrollGuard *guard_{nullptr};
};

} // namespace
} // namespace pacsmith::gui

QTEST_MAIN(pacsmith::gui::WheelScrollGuardTest)

#include "wheel_scroll_guard_test.moc"
