#pragma once

#include "core/app_settings.hpp"

#include <QByteArray>
#include <QDialog>
#include <QElapsedTimer>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QTimer;
class QToolButton;
class QWidget;

namespace pacsmith::gui {

class AiProgressDialog final : public QDialog {
public:
    explicit AiProgressDialog(const AiSettings &settings, QWidget *parent = nullptr);

    void setStatus(const QString &status);
    void appendActivity(const QString &activity);
    void setResponseProgress(qint64 bytesReceived, qint64 outputCharacters);
    void setRequest(int round, const QByteArray &requestBody);
    void appendResponseDelta(int round, const QString &text);
    void setCompletedResponse(const QString &text);
    void showFailure(const QString &summary, const QString &diagnosticDetails);
    void showCompletion(const QString &summary, const QString &details = {});
    void releaseModality();

private:
    void copyText(const QString &text);
    QLabel *status_{nullptr};
    QLabel *elapsed_{nullptr};
    QLabel *transfer_{nullptr};
    QPlainTextEdit *activity_{nullptr};
    QPlainTextEdit *requests_{nullptr};
    QPlainTextEdit *responses_{nullptr};
    QWidget *details_{nullptr};
    QToolButton *detailsToggle_{nullptr};
    int responseRound_{0};
    QProgressBar *progress_{nullptr};
    QPushButton *closeButton_{nullptr};
    QTimer *timer_{nullptr};
    QElapsedTimer elapsedTimer_;
};

} // namespace pacsmith::gui
