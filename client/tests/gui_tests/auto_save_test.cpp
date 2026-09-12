#include "gui/auto_save.hpp"

#include <QPushButton>
#include <QTest>
#include <QVBoxLayout>

using pacsmith::gui::AutoSave;

class AutoSaveTest final : public QObject {
    Q_OBJECT
private slots:
    void textFieldsSaveOnFinishButNotPopulation() {
        QWidget page;
        QLineEdit editor(&page);
        int saves = 0;
        AutoSave save(&page, [&] { ++saves; });
        save.watch(&editor);
        editor.setText(QStringLiteral("Loaded"));
        save.flush();
        QCOMPARE(saves, 0);
        QTest::keyClicks(&editor, " edit");
        QCOMPARE(saves, 0);
        QTest::keyClick(&editor, Qt::Key_Return);
        QCOMPARE(saves, 1);
        save.flush();
        QCOMPARE(saves, 1);
    }

    void scriptsSaveAfterTypingAndKeepCursorAndUndo() {
        QWidget page;
        QPlainTextEdit editor(&page);
        int saves = 0;
        AutoSave save(&page, [&] {
            ++saves;
            editor.document()->setModified(false);
        });
        save.watch(&editor);
        editor.setPlainText(QStringLiteral("loaded"));
        save.flush();
        QCOMPARE(saves, 0);
        QTest::keyClicks(&editor, "draft");
        const auto position = editor.textCursor().position();
        QCOMPARE(saves, 0);
        QTRY_COMPARE(saves, 1);
        QCOMPARE(editor.textCursor().position(), position);
        QVERIFY(editor.document()->isUndoAvailable());
        editor.undo();
        QTRY_COMPARE(saves, 2);
    }

    void navigationSavesBeforeSelectionChanges() {
        QWidget page;
        auto *layout = new QVBoxLayout(&page);
        QPlainTextEdit editor;
        QPushButton navigate(QStringLiteral("Next release"));
        layout->addWidget(&editor);
        layout->addWidget(&navigate);
        int selected = 1;
        int savedSelection = 0;
        AutoSave save(&page, [&] { savedSelection = selected; });
        save.watch(&editor);
        connect(&navigate, &QPushButton::clicked, &page, [&] { selected = 2; });
        page.show();
        editor.setFocus();
        QApplication::processEvents();
        QTest::keyClicks(&editor, "draft");
        QTest::mouseClick(&navigate, Qt::LeftButton);
        QCOMPARE(savedSelection, 1);
        QCOMPARE(selected, 2);
    }

    void focusOutAndCloseFlushPendingText() {
        QWidget page;
        QPlainTextEdit editor(&page);
        int saves = 0;
        AutoSave save(&page, [&] { ++saves; });
        save.watch(&editor);
        QTest::keyClicks(&editor, "draft");
        QFocusEvent blur(QEvent::FocusOut);
        QApplication::sendEvent(&editor, &blur);
        QCOMPARE(saves, 1);
        QTest::keyClicks(&editor, "more");
        QCloseEvent close;
        QApplication::sendEvent(&page, &close);
        QCOMPARE(saves, 2);
    }

    void choicesSaveOnlyUserChanges() {
        QWidget page;
        QComboBox combo(&page);
        combo.addItems({QStringLiteral("A"), QStringLiteral("B")});
        QCheckBox check(&page);
        int saves = 0;
        AutoSave save(&page, [&] { ++saves; });
        save.watch(&combo);
        save.watch(&check);
        combo.setCurrentIndex(1);
        check.setChecked(true);
        QCOMPARE(saves, 0);
        check.click();
        QCOMPARE(saves, 1);
        QTest::keyClick(&combo, Qt::Key_Up);
        QCOMPARE(saves, 2);
    }
};

QTEST_MAIN(AutoSaveTest)
#include "auto_save_test.moc"
