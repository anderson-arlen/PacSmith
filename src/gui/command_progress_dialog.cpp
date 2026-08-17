#include "gui/command_progress_dialog.hpp"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace pacsmith::gui {
namespace {

QString elapsedText(const qint64 milliseconds) {
    const auto totalSeconds = milliseconds / 1000;
    const auto minutes = totalSeconds / 60;
    const auto seconds = totalSeconds % 60;
    return minutes > 0 ? QStringLiteral("Elapsed: %1m %2s").arg(minutes).arg(seconds, 2, 10, QChar('0'))
                       : QStringLiteral("Elapsed: %1s").arg(seconds);
}

} // namespace

CommandProgressDialog::CommandProgressDialog(QWidget *parent) : QDialog(parent) {
    setWindowModality(Qt::WindowModal);
    setSizeGripEnabled(true);
    setMinimumWidth(560);
    setMinimumHeight(220);

    auto *layout = new QVBoxLayout(this);
    status_ = new QLabel(QStringLiteral("Working…"), this);
    status_->setWordWrap(true);
    auto statusFont = status_->font();
    statusFont.setBold(true);
    status_->setFont(statusFont);
    layout->addWidget(status_);

    progress_ = new QProgressBar(this);
    progress_->setRange(0, 0);
    progress_->setTextVisible(false);
    layout->addWidget(progress_);

    elapsed_ = new QLabel(QStringLiteral("Elapsed: 0s"), this);
    layout->addWidget(elapsed_);

    detailsToggle_ = new QToolButton(this);
    detailsToggle_->setText(QStringLiteral("Show Details"));
    detailsToggle_->setCheckable(true);
    detailsToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    detailsToggle_->setArrowType(Qt::RightArrow);
    layout->addWidget(detailsToggle_, 0, Qt::AlignLeft);

    details_ = new QWidget(this);
    auto *detailsLayout = new QVBoxLayout(details_);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    output_ = new QPlainTextEdit(details_);
    output_->setReadOnly(true);
    output_->setLineWrapMode(QPlainTextEdit::NoWrap);
    output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output_->setPlaceholderText(QStringLiteral("Command output will appear here."));
    detailsLayout->addWidget(output_, 1);
    details_->setVisible(false);
    layout->addWidget(details_, 1);

    auto *buttons = new QDialogButtonBox(this);
    cancelButton_ = buttons->addButton(QDialogButtonBox::Cancel);
    closeButton_ = buttons->addButton(QDialogButtonBox::Close);
    closeButton_->setVisible(false);
    closeButton_->setDefault(true);
    layout->addWidget(buttons);

    connect(detailsToggle_, &QToolButton::toggled, this, [this](const bool shown) {
        applyDetailsVisibility(shown);
    });
    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        if (finished_ || !cancelable_) return;
        emit cancelRequested();
    });
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::accept);

    elapsedTimer_.start();
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, [this] {
        elapsed_->setText(elapsedText(elapsedTimer_.elapsed()));
    });
    timer_->start();
}

void CommandProgressDialog::setStatus(const QString &status) {
    status_->setText(status);
}

void CommandProgressDialog::appendOutput(const QString &text) {
    if (text.isEmpty()) return;
    output_->moveCursor(QTextCursor::End);
    output_->insertPlainText(text);
    output_->moveCursor(QTextCursor::End);
}

void CommandProgressDialog::setCancelable(const bool cancelable) {
    cancelable_ = cancelable;
    cancelButton_->setVisible(!finished_ && cancelable_);
    cancelButton_->setEnabled(!finished_ && cancelable_);
}

void CommandProgressDialog::markFinished(const bool success, const QString &summary) {
    finished_ = true;
    cancelable_ = false;
    timer_->stop();
    elapsed_->setText(elapsedText(elapsedTimer_.elapsed()));
    progress_->setRange(0, 1);
    progress_->setValue(success ? 1 : 0);
    status_->setText(summary);
    cancelButton_->setVisible(false);
    closeButton_->setVisible(true);
    closeButton_->setEnabled(true);
    closeButton_->setFocus();
}

void CommandProgressDialog::closeEvent(QCloseEvent *event) {
    if (finished_) {
        event->accept();
        return;
    }
    if (cancelable_) emit cancelRequested();
    event->ignore();
}

void CommandProgressDialog::reject() {
    if (finished_) {
        QDialog::reject();
        return;
    }
    if (cancelable_) emit cancelRequested();
}

void CommandProgressDialog::applyDetailsVisibility(const bool shown) {
    details_->setVisible(shown);
    detailsToggle_->setText(shown ? QStringLiteral("Hide Details")
                                  : QStringLiteral("Show Details"));
    detailsToggle_->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
    resize(width(), shown ? 520 : 240);
}

} // namespace pacsmith::gui
