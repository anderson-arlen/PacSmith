#pragma once

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QDateTimeEdit>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QSet>
#include <QTextDocument>
#include <QTimer>

#include <functional>

namespace pacsmith::gui {

class AutoSave final : public QObject {
public:
    AutoSave(QWidget *scope, std::function<void()> save)
        : QObject(scope), scope_(scope), save_(std::move(save)) {
        timer_.setSingleShot(true);
        timer_.setInterval(650);
        connect(&timer_, &QTimer::timeout, this, &AutoSave::flush);
        qApp->installEventFilter(this);
    }

    void watch(QLineEdit *editor) {
        editors_.insert(editor);
        connect(editor, &QLineEdit::textEdited, this, [this] { dirty_ = true; });
        connect(editor, &QLineEdit::editingFinished, this, &AutoSave::flush);
    }

    void watch(QPlainTextEdit *editor) {
        editors_.insert(editor);
        connect(editor, &QPlainTextEdit::textChanged, this, [this, editor] {
            if (saving_ || editor->isReadOnly() || !editor->document()->isModified()) return;
            // setPlainText emits textChanged before clearing the modified flag,
            // but it clears undo/redo. Loading a page must never write it back.
            if (!editor->document()->isUndoAvailable() && !editor->document()->isRedoAvailable()) return;
            dirty_ = true;
            timer_.start();
        });
    }

    void watch(QComboBox *editor) {
        connect(editor, &QComboBox::activated, this, [this] { changed(); });
    }

    void watch(QCheckBox *editor) {
        connect(editor, &QCheckBox::clicked, this, [this] { changed(); });
    }

    void watch(QSpinBox *editor) {
        editor->setKeyboardTracking(false);
        connect(editor, &QSpinBox::valueChanged, this, [this, editor] {
            if (editor->hasFocus()) changed();
        });
    }

    void watch(QDateTimeEdit *editor) {
        editor->setKeyboardTracking(false);
        connect(editor, &QDateTimeEdit::dateTimeChanged, this, [this, editor] {
            if (editor->hasFocus()) changed();
        });
    }

    void schedule() {
        if (saving_) return;
        dirty_ = true;
        timer_.start(0);
    }

    void flush() {
        if (!dirty_ || saving_) return;
        timer_.stop();
        dirty_ = false;
        const QScopedValueRollback guard(saving_, true);
        save_();
    }

    static void flushAll(QWidget *scope) {
        for (auto *child : scope->findChildren<QObject *>()) {
            if (auto *save = dynamic_cast<AutoSave *>(child)) save->flush();
        }
    }

protected:
    bool eventFilter(QObject *target, QEvent *event) override {
        if (!dirty_ || saving_) return false;
        const auto *widget = qobject_cast<QWidget *>(target);
        if (widget == nullptr) return false;
        // Flush before a navigation click changes the selected project or list row.
        if ((event->type() == QEvent::MouseButtonPress &&
             widget != QApplication::focusWidget() &&
             (QApplication::focusWidget() == nullptr ||
              !QApplication::focusWidget()->isAncestorOf(widget))) ||
            (event->type() == QEvent::FocusOut && editors_.contains(target)) ||
            (event->type() == QEvent::Close && widget == scope_->window())) {
            flush();
        }
        return false;
    }

private:
    void changed() {
        if (saving_) return;
        dirty_ = true;
        flush();
    }

    QWidget *scope_;
    QSet<QObject *> editors_;
    std::function<void()> save_;
    QTimer timer_;
    bool dirty_{false};
    bool saving_{false};
};

} // namespace pacsmith::gui
