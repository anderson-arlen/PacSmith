#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QDateTime>

class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTimer;
class QToolButton;
class QWidget;

namespace pacsmith::gui {

class CommandProgressDialog : public QDialog {
    Q_OBJECT
public:
    explicit CommandProgressDialog(QWidget *parent = nullptr);

    void setStatus(const QString &status);
    void appendOutput(const QString &text);
    void setCancelable(bool cancelable, const QString &label = QStringLiteral("Cancel"));
    void markFinished(bool success, const QString &summary);
    void setJobTiming(const QDateTime &startedAt, const QDateTime &finishedAt = {});
    void showDetails();
    [[nodiscard]] bool isFinished() const noexcept { return finished_; }

signals:
    void cancelRequested();

private:
    void applyDetailsVisibility(bool shown);
    void updateElapsed();

    QLabel *status_{nullptr};
    QLabel *elapsed_{nullptr};
    QLabel *started_{nullptr};
    QProgressBar *progress_{nullptr};
    QToolButton *detailsToggle_{nullptr};
    QWidget *details_{nullptr};
    QPlainTextEdit *output_{nullptr};
    QPushButton *cancelButton_{nullptr};
    QPushButton *closeButton_{nullptr};
    QTimer *timer_{nullptr};
    QElapsedTimer elapsedTimer_;
    QDateTime startedAt_;
    QDateTime finishedAt_;
    bool jobTiming_{false};
    bool finished_{false};
    bool cancelable_{false};
};

} // namespace pacsmith::gui
