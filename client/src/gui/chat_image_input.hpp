#pragma once
#include <QPlainTextEdit>
#include <QJsonObject>
#include <functional>
#include <optional>
#include <QImage>

class QMimeData;
namespace pacsmith::gui {
struct ChatImage {
    QString name;
    QByteArray bytes;
    QString mimeType;
    QImage preview;
    QString storageName{};
    QJsonObject content() const;
};
std::optional<ChatImage> readChatImage(const QByteArray &bytes, const QString &name, QString *error, const QSize &previewSize = QSize(160, 100));
class ChatImageInput final : public QPlainTextEdit {
public:
    explicit ChatImageInput(QWidget *parent = nullptr) : QPlainTextEdit(parent) {}
    std::function<void(const QMimeData *)> attach;
    std::function<void()> send;
protected:
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
private:
    bool composing_{false};
};
}
