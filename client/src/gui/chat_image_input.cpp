#include "gui/chat_image_input.hpp"
#include <QBuffer>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>

namespace pacsmith::gui {
QJsonObject ChatImage::content() const {
    return {{QStringLiteral("type"), QStringLiteral("image")}, {QStringLiteral("mimeType"), mimeType},
        {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())}};
}
std::optional<ChatImage> readChatImage(const QByteArray &bytes, const QString &name, QString *error, const QSize &previewSize) {
    if (bytes.size() > 10 * 1024 * 1024) { *error = QStringLiteral("Each screenshot must be at most 10 MB."); return std::nullopt; }
    QBuffer buffer;
    buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto format = reader.format().toLower();
    if (format != "png" && format != "jpeg" && format != "jpg" && format != "gif" && format != "webp") {
        *error = QStringLiteral("Choose a PNG, JPEG, GIF, or WebP image."); return std::nullopt;
    }
    const auto size = reader.size();
    if (!size.isValid() || static_cast<qint64>(size.width()) * size.height() > 64 * 1024 * 1024) {
        *error = QStringLiteral("This image is too large to preview."); return std::nullopt;
    }
    reader.setScaledSize(size.boundedTo(size.scaled(previewSize, Qt::KeepAspectRatio)));
    const auto preview = reader.read();
    if (preview.isNull()) { *error = QStringLiteral("Could not read the image."); return std::nullopt; }
    return ChatImage{name, bytes, QStringLiteral("image/") + QString::fromLatin1(format == "jpg" ? QByteArray("jpeg") : format), preview};
}
void ChatImageInput::keyPressEvent(QKeyEvent *event) {
    const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        modifiers == Qt::NoModifier && !composing_) {
        if (!event->isAutoRepeat() && send) send();
        event->accept();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}
void ChatImageInput::inputMethodEvent(QInputMethodEvent *event) {
    // Enter must remain available to confirm text being composed with an input method.
    composing_ = !event->preeditString().isEmpty();
    QPlainTextEdit::inputMethodEvent(event);
}
bool ChatImageInput::canInsertFromMimeData(const QMimeData *source) const {
    return source->hasImage() || source->hasUrls() || QPlainTextEdit::canInsertFromMimeData(source);
}
void ChatImageInput::insertFromMimeData(const QMimeData *source) {
    if (attach && (source->hasImage() || source->hasUrls())) attach(source);
    else QPlainTextEdit::insertFromMimeData(source);
}
}
