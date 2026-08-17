#pragma once

#include <QDialog>
#include <QElapsedTimer>

class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTimer;
class QToolButton;
class QWidget;

namespace pacsmith::gui {

class CommandProgressDialog final : public QDialog {
    Q_OBJECT
public:
    explicit CommandProgressDialog(QWidget *parent = nullptr);

    void setStatus(const QString &status);
    void appendOutput(const QString &text);
    void setCancelable(bool cancelable);
    void markFinished(bool success, const QString &summary);
    [[nodiscard]] bool isFinished() const noexcept { return finished_; }

signals:
    void cancelRequested();

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    void applyDetailsVisibility(bool shown);

    QLabel *status_{nullptr};
    QLabel *elapsed_{nullptr};
    QProgressBar *progress_{nullptr};
    QToolButton *detailsToggle_{nullptr};
    QWidget *details_{nullptr};
    QPlainTextEdit *output_{nullptr};
    QPushButton *cancelButton_{nullptr};
    QPushButton *closeButton_{nullptr};
    QTimer *timer_{nullptr};
    QElapsedTimer elapsedTimer_;
    bool finished_{false};
    bool cancelable_{false};
};

} // namespace pacsmith::gui
