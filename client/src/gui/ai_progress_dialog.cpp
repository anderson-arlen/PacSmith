#include "gui/ai_progress_dialog.hpp"

#include <QDialogButtonBox>
#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QLabel>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTextCursor>
#include <QVBoxLayout>

namespace pacsmith::gui {
namespace {

QString settingName(const QString &name, const QString &fallback) {
    return name.isEmpty() ? fallback : name;
}

QString byteCount(const qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KiB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MiB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0),
                                        0, 'f', 1);
}

QString elapsedText(const qint64 milliseconds) {
    const auto totalSeconds = milliseconds / 1000;
    const auto minutes = totalSeconds / 60;
    const auto seconds = totalSeconds % 60;
    return minutes > 0 ? QStringLiteral("Elapsed: %1m %2s").arg(minutes).arg(seconds, 2, 10, QChar('0'))
                       : QStringLiteral("Elapsed: %1s").arg(seconds);
}

} // namespace

AiProgressDialog::AiProgressDialog(const AiSettings &settings, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Resolving Package Review Items"));
    setWindowModality(Qt::WindowModal);
    setSizeGripEnabled(true);
    setMinimumWidth(620);

    auto *layout = new QVBoxLayout(this);
    auto *configuration = new QLabel(
        QStringLiteral("Provider: %1   •   Model: %2   •   Reasoning: %3   •   Speed: %4")
            .arg(aiProviderName(settings.provider), settings.model,
                 settingName(aiReasoningEffortName(settings.reasoningEffort),
                             QStringLiteral("provider default")),
                 settings.executionMode == AiExecutionMode::Fast ? QStringLiteral("fast / priority")
                                                                 : QStringLiteral("standard")),
        this);
    configuration->setTextInteractionFlags(Qt::TextSelectableByMouse);
    configuration->setWordWrap(true);
    layout->addWidget(configuration);

    status_ = new QLabel(QStringLiteral("Preparing AI analysis…"), this);
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
    transfer_ = new QLabel(QStringLiteral("No response data received yet"), this);
    layout->addWidget(elapsed_);
    layout->addWidget(transfer_);

    detailsToggle_ = new QToolButton(this);
    detailsToggle_->setText(QStringLiteral("Show Details"));
    detailsToggle_->setCheckable(true);
    detailsToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    detailsToggle_->setArrowType(Qt::RightArrow);
    layout->addWidget(detailsToggle_, 0, Qt::AlignLeft);

    details_ = new QWidget(this);
    auto *detailsLayout = new QVBoxLayout(details_);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    auto *detailsTabs = new QTabWidget(details_);
    activity_ = new QPlainTextEdit(detailsTabs);
    activity_->setReadOnly(true);
    activity_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    activity_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    activity_->setPlaceholderText(QStringLiteral("Request activity will appear here."));
    requests_ = new QPlainTextEdit(detailsTabs);
    requests_->setReadOnly(true);
    requests_->setLineWrapMode(QPlainTextEdit::NoWrap);
    requests_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    responses_ = new QPlainTextEdit(detailsTabs);
    responses_->setReadOnly(true);
    responses_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    responses_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    detailsTabs->addTab(activity_, QStringLiteral("Activity"));
    detailsTabs->addTab(requests_, QStringLiteral("Request"));
    detailsTabs->addTab(responses_, QStringLiteral("Live Response"));
    detailsLayout->addWidget(detailsTabs, 1);
    auto *copyRow = new QHBoxLayout;
    auto *copyRequest = new QPushButton(QStringLiteral("Copy Request"), details_);
    auto *copyResponse = new QPushButton(QStringLiteral("Copy Response"), details_);
    auto *copyAll = new QPushButton(QStringLiteral("Copy Full Transcript"), details_);
    copyRow->addWidget(copyRequest);
    copyRow->addWidget(copyResponse);
    copyRow->addWidget(copyAll);
    copyRow->addStretch();
    detailsLayout->addLayout(copyRow);
    details_->setVisible(false);
    layout->addWidget(details_, 1);
    connect(detailsToggle_, &QToolButton::toggled, this, [this](const bool shown) {
        details_->setVisible(shown);
        detailsToggle_->setText(shown ? QStringLiteral("Hide Details")
                                      : QStringLiteral("Show Details"));
        detailsToggle_->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
        resize(width(), shown ? 650 : 260);
    });
    connect(copyRequest, &QPushButton::clicked, this, [this] { copyText(requests_->toPlainText()); });
    connect(copyResponse, &QPushButton::clicked, this, [this] { copyText(responses_->toPlainText()); });
    connect(copyAll, &QPushButton::clicked, this, [this] {
        copyText(QStringLiteral("Activity\n========\n") + activity_->toPlainText() +
                 QStringLiteral("\n\nRequest\n=======\n") + requests_->toPlainText() +
                 QStringLiteral("\n\nResponse\n========\n") + responses_->toPlainText());
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    closeButton_ = buttons->button(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    elapsedTimer_.start();
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, [this] {
        elapsed_->setText(elapsedText(elapsedTimer_.elapsed()));
    });
    timer_->start();
    resize(720, 260);
}

void AiProgressDialog::setRequest(const int round, const QByteArray &requestBody) {
    const auto document = QJsonDocument::fromJson(requestBody);
    const auto display = document.isNull()
        ? QString::fromUtf8(requestBody)
        : QString::fromUtf8(document.toJson(QJsonDocument::Indented));
    if (!requests_->toPlainText().isEmpty()) requests_->appendPlainText(QString{});
    requests_->appendPlainText(QStringLiteral("Request %1\n=========\n%2").arg(round).arg(display));
}

void AiProgressDialog::appendResponseDelta(const int round, const QString &text) {
    if (responseRound_ != round) {
        responseRound_ = round;
        if (!responses_->toPlainText().isEmpty()) responses_->appendPlainText(QString{});
        responses_->appendPlainText(QStringLiteral("Response %1\n==========").arg(round));
    }
    responses_->moveCursor(QTextCursor::End);
    responses_->insertPlainText(text);
    responses_->moveCursor(QTextCursor::End);
}

void AiProgressDialog::setStatus(const QString &status) { status_->setText(status); }

void AiProgressDialog::appendActivity(const QString &activity) {
    const auto prefix = QStringLiteral("[%1] ").arg(elapsedText(elapsedTimer_.elapsed()).mid(9));
    activity_->appendPlainText(prefix + activity);
}

void AiProgressDialog::setResponseProgress(const qint64 bytesReceived,
                                           const qint64 outputCharacters) {
    transfer_->setText(outputCharacters > 0
                           ? QStringLiteral("Received %1; %2 structured-output characters")
                                 .arg(byteCount(bytesReceived))
                                 .arg(outputCharacters)
                           : QStringLiteral("Received %1; the model is still working")
                                 .arg(byteCount(bytesReceived)));
}

void AiProgressDialog::setCompletedResponse(const QString &text) {
    responses_->setPlainText(text);
    responses_->moveCursor(QTextCursor::Start);
}

void AiProgressDialog::releaseModality() {
    const bool visible = isVisible();
    if (visible) hide();
    setWindowModality(Qt::NonModal);
    setModal(false);
    if (visible) {
        show();
        raise();
        activateWindow();
    }
}

void AiProgressDialog::copyText(const QString &text) {
    const auto bytes = text.toUtf8();
    auto startCopier = [&](const QString &program, const QStringList &args) {
        if (QStandardPaths::findExecutable(program).isEmpty()) return false;
        auto *proc = new QProcess(qApp);
        QObject::connect(proc, &QProcess::started, proc, [proc, bytes] {
            proc->write(bytes);
            proc->closeWriteChannel();
        });
        QObject::connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
        QObject::connect(proc, &QProcess::errorOccurred, proc, [proc](QProcess::ProcessError) {
            proc->deleteLater();
        });
        proc->start(program, args);
        return true;
    };
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == QLatin1String("wayland") &&
        startCopier(QStringLiteral("wl-copy"), {})) {
        return;
    }
    if (startCopier(QStringLiteral("xclip"),
                    {QStringLiteral("-selection"), QStringLiteral("clipboard")})) {
        return;
    }
    auto *mime = new QMimeData;
    mime->setText(text);
    QTimer::singleShot(0, qApp, [mime] {
        QGuiApplication::clipboard()->setMimeData(mime, QClipboard::Clipboard);
    });
}

void AiProgressDialog::showFailure(const QString &summary,
                                   const QString &diagnosticDetails) {
    timer_->stop();
    progress_->setRange(0, 1);
    progress_->setValue(1);
    status_->setText(QStringLiteral("⚠ %1").arg(
        summary.trimmed().isEmpty() ? QStringLiteral("The AI request failed") : summary));
    status_->setStyleSheet(QStringLiteral("color: #ef6c6c;"));
    transfer_->setText(QStringLiteral("The request failed. Inspect the transcript below or copy it for troubleshooting."));
    appendActivity(QStringLiteral("Request failed: %1").arg(summary));
    if (!diagnosticDetails.trimmed().isEmpty()) {
        responses_->moveCursor(QTextCursor::End);
        responses_->appendPlainText(
            QStringLiteral("\n\nFailure diagnostics\n===================\n%1")
                .arg(diagnosticDetails));
    }
    detailsToggle_->setChecked(true);
    if (closeButton_ != nullptr) closeButton_->setText(QStringLiteral("Close"));
    setWindowTitle(QStringLiteral("AI Resolution Failed"));
    releaseModality();
    show();
    raise();
    activateWindow();
}

void AiProgressDialog::showCompletion(const QString &summary, const QString &details) {
    timer_->stop();
    progress_->setRange(0, 1);
    progress_->setValue(1);
    status_->setText(summary.trimmed().isEmpty() ? QStringLiteral("AI review complete") : summary);
    status_->setStyleSheet(QString{});
    transfer_->setText(QStringLiteral("Review finished. The transcript stays in this dialog so you can copy it."));
    appendActivity(summary.trimmed().isEmpty() ? QStringLiteral("AI review complete") : summary);
    if (!details.trimmed().isEmpty()) {
        activity_->appendPlainText(QString{});
        activity_->appendPlainText(details);
    }
    detailsToggle_->setChecked(true);
    if (closeButton_ != nullptr) closeButton_->setText(QStringLiteral("Close"));
    setWindowTitle(QStringLiteral("AI review complete"));
    releaseModality();
    show();
    raise();
    activateWindow();
}

} // namespace pacsmith::gui
