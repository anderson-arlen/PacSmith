#include "gui/main_window/support.hpp"

#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QScopeGuard>
#include <QTest>
#include <QTimer>

namespace pacsmith::gui {

class GitHubPickerTest final : public QObject {
    Q_OBJECT
private slots:
    void artifactColorsFollowTheme_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void artifactColorsFollowTheme() {
        QFETCH(bool, dark);
        const auto originalPalette = QApplication::palette();
        const auto restorePalette = qScopeGuard([&] { QApplication::setPalette(originalPalette); });
        auto palette = originalPalette;
        palette.setColor(QPalette::Base, dark ? QColor("#202020") : QColor("#ffffff"));
        palette.setColor(QPalette::Text, dark ? QColor("#ffffff") : QColor("#202020"));
        palette.setColor(QPalette::Highlight, dark ? QColor("#86c5ff") : QColor("#215b94"));
        palette.setColor(QPalette::HighlightedText, dark ? QColor("#101010") : QColor("#ffffff"));
        QApplication::setPalette(palette);
        bool inspected = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            const auto closeDialog = qScopeGuard([&] { dialog->reject(); });
            auto *list = dialog->findChild<QListWidget *>();
            auto *expression = dialog->findChild<QLineEdit *>();
            QVERIFY(list != nullptr);
            QVERIFY(expression != nullptr);
            QCOMPARE(list->count(), 3);
            expression->setText(QStringLiteral("first\\.deb"));
            QCOMPARE(list->item(0)->background(), list->palette().brush(QPalette::Highlight));
            QCOMPARE(list->item(0)->foreground(), list->palette().brush(QPalette::HighlightedText));
            for (int row = 1; row < list->count(); ++row) {
                QVERIFY(!list->item(row)->data(Qt::BackgroundRole).isValid());
                QVERIFY(!list->item(row)->data(Qt::ForegroundRole).isValid());
            }
            QVERIFY(!list->item(2)->flags().testFlag(Qt::ItemIsEnabled));
            expression->setText(QStringLiteral("second\\.rpm"));
            QVERIFY(!list->item(0)->data(Qt::BackgroundRole).isValid());
            QVERIFY(!list->item(0)->data(Qt::ForegroundRole).isValid());
            QCOMPARE(list->item(1)->background(), list->palette().brush(QPalette::Highlight));
            QCOMPARE(list->item(1)->foreground(), list->palette().brush(QPalette::HighlightedText));
            inspected = true;
        });
        const auto choice = chooseGitHubAssetRule(nullptr,
            {QStringLiteral("first.deb"), QStringLiteral("second.rpm"), QStringLiteral("SHA256SUMS")}, false);
        QVERIFY(!choice.has_value());
        QVERIFY(inspected);
    }
};

}

QTEST_MAIN(pacsmith::gui::GitHubPickerTest)
#include "github_picker_test.moc"
