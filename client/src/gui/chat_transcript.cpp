#include "gui/chat_transcript.hpp"
#include "gui/chat_image_input.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QVBoxLayout>

namespace pacsmith::gui {
namespace {
bool running(const QJsonObject &entry) {
    const auto status = entry.value(QStringLiteral("status")).toString();
    return status == QStringLiteral("pending") || status == QStringLiteral("in_progress");
}
class ActivityIcon final : public QWidget {
public:
    ActivityIcon(bool busy, bool error, int *angle, QWidget *parent)
        : QWidget(parent), busy_(busy), error_(error), angle_(angle) {
        setFixedSize(18, 18);
        setObjectName(busy ? QStringLiteral("toolSpinner") : QStringLiteral("toolIcon"));
        setAccessibleName(busy ? QStringLiteral("Tool running") : error ? QStringLiteral("Tool failed") : QStringLiteral("Tool activity"));
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto color = error_ ? QColor(QStringLiteral("#DC5466")) : palette().color(QPalette::Text);
        painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
        if (busy_) {
            painter.drawArc(QRectF(2, 2, 14, 14), -*angle_ * 16, 260 * 16);
            return;
        }
        painter.drawEllipse(QRectF(1.5, 1.5, 15, 15));
        painter.translate(9, 9);
        painter.rotate(-40);
        painter.scale(0.75, 0.75);
        QPainterPath wrench;
        wrench.moveTo(-1.2, 6); wrench.lineTo(-1.2, -1);
        wrench.cubicTo(-4.5, -2, -4, -5, -2, -6.5);
        wrench.lineTo(-2, -3.5); wrench.lineTo(2, -3.5); wrench.lineTo(2, -6.5);
        wrench.cubicTo(4, -5, 4.5, -2, 1.2, -1); wrench.lineTo(1.2, 6); wrench.closeSubpath();
        painter.setPen(Qt::NoPen); painter.setBrush(color); painter.drawPath(wrench);
    }
private:
    bool busy_, error_;
    int *angle_;
};
class ImageThumbnail final : public QPushButton {
public:
    ImageThumbnail(const QImage &image, QWidget *parent) : QPushButton(parent), image_(image) {
        setObjectName(QStringLiteral("chatImage"));
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setFixedHeight(qMin(image.height(), 200));
        setCursor(Qt::PointingHandCursor);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const auto size = image_.size().scaled(contentsRect().size(), Qt::KeepAspectRatio);
        painter.drawImage(QRect(QPoint(0, (height() - size.height()) / 2), size), image_);
        if (hasFocus()) {
            painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
            painter.drawRect(rect().adjusted(1, 1, -1, -1));
        }
    }
private:
    QImage image_;
};
std::optional<ChatImage> storedImage(const QString &directory, const QJsonObject &record, const QSize &size) {
    const auto name = record.value(QStringLiteral("file")).toString();
    if (name.isEmpty() || QFileInfo(name).fileName() != name) return std::nullopt;
    QFile file(QDir(directory).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    QString error;
    return readChatImage(file.read(10 * 1024 * 1024 + 1), record.value(QStringLiteral("name")).toString(), &error, size);
}
QLabel *textLabel(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}
}

ChatTranscript::ChatTranscript(QWidget *parent) : QScrollArea(parent) {
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    contents_ = new QWidget(this);
    rows_ = new QVBoxLayout(contents_);
    rows_->setContentsMargins(8, 12, 8, 12);
    rows_->setSpacing(8);
    rows_->setAlignment(Qt::AlignTop);
    setWidget(contents_);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        followingOutput_ = verticalScrollBar()->maximum() - value < 24;
    });
    // Wrapped text and images can change the range after the initial layout pass.
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int maximum) {
        if (followingOutput_) verticalScrollBar()->setValue(maximum);
    });
    animation_.setInterval(80);
    connect(&animation_, &QTimer::timeout, this, [this] {
        angle_ = (angle_ + 24) % 360;
        for (auto *icon : contents_->findChildren<QWidget *>(QStringLiteral("toolSpinner"))) icon->update();
    });
}

void ChatTranscript::renderRow(int index) {
    const auto entry = entries_.at(index).toObject();
    auto *row = new QWidget(contents_);
    const auto kind = entry.value(QStringLiteral("kind")).toString();
    row->setObjectName(kind == QStringLiteral("tool") ? QStringLiteral("toolCallRow") : QStringLiteral("chatEntry"));
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    const bool user = kind == QStringLiteral("user");
    const bool bubble = user || kind == QStringLiteral("assistant");
    if (bubble) {
        auto *card = new QWidget(row);
        card->setObjectName(user ? QStringLiteral("userBubble") : QStringLiteral("agentBubble"));
        auto *body = new QVBoxLayout(card);
        body->setContentsMargins(16, 12, 16, 12);
        if (!entry.value(QStringLiteral("text")).toString().isEmpty())
            body->addWidget(textLabel(entry.value(QStringLiteral("text")).toString(), card));
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const auto background = user ? (dark ? "#075B6B" : "#D5F1F8") : (dark ? "#30383C" : "#E6E9ED");
        const auto foreground = dark ? "#F0F4F6" : "#172C38";
        card->setStyleSheet(QStringLiteral("QWidget#%1 { background: %2; border-radius: 14px; } QLabel { color: %3; }")
            .arg(card->objectName(), QString::fromLatin1(background), QString::fromLatin1(foreground)));
        card->setMaximumWidth(720);
        for (const auto &value : entry.value(QStringLiteral("images")).toArray()) {
            const auto record = value.toObject();
            const auto image = storedImage(imageDirectory_, record, QSize(480, 320));
            if (!image) { body->addWidget(textLabel(QStringLiteral("Image unavailable: %1").arg(record.value(QStringLiteral("name")).toString()), card)); continue; }
            auto *thumbnail = new ImageThumbnail(image->preview, card);
            thumbnail->setAccessibleName(QStringLiteral("Open image: %1").arg(image->name));
            thumbnail->setToolTip(image->name);
            body->addWidget(thumbnail);
            connect(thumbnail, &QPushButton::clicked, this, [this, record] {
                const auto full = storedImage(imageDirectory_, record, QSize(1600, 1200));
                if (!full) return;
                auto *dialog = new QDialog(this);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->setObjectName(QStringLiteral("chatImagePreview"));
                dialog->setWindowTitle(full->name);
                dialog->resize(800, 600);
                auto *dialogLayout = new QVBoxLayout(dialog);
                auto *scroll = new QScrollArea(dialog);
                auto *imageLabel = new QLabel(scroll);
                imageLabel->setPixmap(QPixmap::fromImage(full->preview));
                scroll->setWidget(imageLabel);
                dialogLayout->addWidget(scroll);
                auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
                connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
                dialogLayout->addWidget(buttons);
                dialog->show();
            });
        }
        if (user) layout->addStretch(1);
        layout->addWidget(card, 3);
        if (!user) layout->addStretch(1);
    } else {
        const bool failed = kind == QStringLiteral("error") || entry.value(QStringLiteral("status")).toString() == QStringLiteral("failed");
        layout->addWidget(new ActivityIcon(false, failed, &angle_, row), 0, Qt::AlignTop);
        auto *body = new QWidget(row);
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(0, 0, 0, 0);
        bodyLayout->setSpacing(4);
        auto *label = textLabel(entry.value(QStringLiteral("text")).toString(), body);
        auto font = label->font(); font.setPointSizeF(qMax(8.0, font.pointSizeF() - 1)); label->setFont(font);
        label->setToolTip(entry.value(QStringLiteral("status")).toString());
        bodyLayout->addWidget(label);
        const auto details = entry.value(QStringLiteral("details")).toString();
        if (!details.isEmpty()) {
            auto *toggle = new QPushButton(QStringLiteral("Show details"), body);
            toggle->setFlat(true); toggle->setCheckable(true);
            auto *output = textLabel(details, body); output->hide();
            bodyLayout->addWidget(toggle, 0, Qt::AlignLeft); bodyLayout->addWidget(output);
            connect(toggle, &QPushButton::toggled, output, &QWidget::setVisible);
        }
        layout->addWidget(body, 1);
        if (running(entry)) layout->addWidget(new ActivityIcon(true, false, &angle_, row), 0, Qt::AlignTop);
    }
    if (index < rows_->count()) {
        auto *old = rows_->takeAt(index);
        old->widget()->hide(); old->widget()->deleteLater(); delete old;
    }
    rows_->insertWidget(index, row);
}

void ChatTranscript::followOutput() {
    QTimer::singleShot(0, this, [this] {
        if (followingOutput_) verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    });
    emit changed();
}

void ChatTranscript::message(const QString &kind, const QString &text, bool continuation, const QJsonArray &images) {
    if (text.isEmpty() && images.isEmpty()) return;
    if (continuation && !entries_.isEmpty() && entries_.last().toObject().value(QStringLiteral("kind")).toString() == kind) {
        auto entry = entries_.last().toObject();
        entry.insert(QStringLiteral("text"), entry.value(QStringLiteral("text")).toString() + text);
        entries_.replace(entries_.size() - 1, entry);
    } else {
        QJsonObject entry{{QStringLiteral("kind"), kind}, {QStringLiteral("text"), text}};
        if (!images.isEmpty()) entry.insert(QStringLiteral("images"), images);
        entries_.append(entry);
    }
    renderRow(static_cast<int>(entries_.size() - 1));
    followOutput();
}

void ChatTranscript::toolCall(const QJsonObject &update) {
    const auto id = update.value(QStringLiteral("toolCallId")).toString();
    if (id.isEmpty()) return;
    const int index = tools_.value(id, static_cast<int>(entries_.size()));
    auto entry = index < entries_.size() ? entries_.at(index).toObject()
        : QJsonObject{{QStringLiteral("kind"), QStringLiteral("tool")}, {QStringLiteral("toolCallId"), id},
                      {QStringLiteral("text"), QStringLiteral("Tool call")}, {QStringLiteral("status"), QStringLiteral("pending")}};
    if (update.contains(QStringLiteral("title"))) entry.insert(QStringLiteral("text"), update.value(QStringLiteral("title")));
    if (update.contains(QStringLiteral("status"))) entry.insert(QStringLiteral("status"), update.value(QStringLiteral("status")));
    if (update.contains(QStringLiteral("content"))) {
        QStringList lines;
        for (const auto &item : update.value(QStringLiteral("content")).toArray()) {
            const auto content = item.toObject().value(QStringLiteral("content")).toObject();
            if (content.value(QStringLiteral("type")).toString() == QStringLiteral("text")) lines.append(content.value(QStringLiteral("text")).toString());
        }
        entry.insert(QStringLiteral("details"), lines.join(QLatin1Char('\n')));
    }
    if (index == entries_.size()) { entries_.append(entry); tools_.insert(id, index); }
    else entries_.replace(index, entry);
    renderRow(index);
    bool active = false;
    for (const auto &item : entries_) active |= running(item.toObject());
    if (active && !animation_.isActive()) animation_.start();
    else if (!active) animation_.stop();
    followOutput();
}

void ChatTranscript::beginTurn() { tools_.clear(); }
void ChatTranscript::endTurn(bool succeeded) {
    animation_.stop();
    for (int index = 0; index < entries_.size(); ++index) {
        auto entry = entries_.at(index).toObject();
        if (!running(entry)) continue;
        entry.insert(QStringLiteral("status"), succeeded ? QStringLiteral("completed") : QStringLiteral("interrupted"));
        entries_.replace(index, entry); renderRow(index);
    }
    emit changed();
}
void ChatTranscript::clear() {
    followingOutput_ = true;
    animation_.stop(); tools_.clear(); entries_ = {};
    while (auto *item = rows_->takeAt(0)) { delete item->widget(); delete item; }
    emit changed();
}
void ChatTranscript::restore(const QJsonArray &entries) {
    clear(); entries_ = entries;
    for (int index = 0; index < entries_.size(); ++index) renderRow(index);
    endTurn(false);
}
void ChatTranscript::restoreLegacyImages(const QJsonArray &images) {
    qsizetype next = 0;
    for (int index = 0; index < entries_.size() && next < images.size(); ++index) {
        auto entry = entries_.at(index).toObject();
        if (entry.value(QStringLiteral("kind")).toString() != QStringLiteral("user")) continue;
        const auto text = entry.value(QStringLiteral("text")).toString();
        QStringList names;
        QJsonArray attachments;
        for (auto end = next; end < images.size() && end < next + 4; ++end) {
            names.append(images.at(end).toObject().value(QStringLiteral("name")).toString());
            attachments.append(images.at(end));
            const auto marker = QStringLiteral("[Images: %1]").arg(names.join(QStringLiteral(", ")));
            if (!text.endsWith(marker)) continue;
            entry.insert(QStringLiteral("text"), text.left(text.size() - marker.size()).trimmed());
            entry.insert(QStringLiteral("images"), attachments);
            entries_.replace(index, entry);
            renderRow(index);
            next = end + 1;
            break;
        }
    }
    // Older text transcripts may no longer retain the message marker, but the images still belong in the chat.
    while (next < images.size()) {
        QJsonArray attachments;
        for (int count = 0; count < 4 && next < images.size(); ++count) attachments.append(images.at(next++));
        message(QStringLiteral("user"), QStringLiteral("Earlier attachments"), false, attachments);
    }
}
void ChatTranscript::restoreLegacy(const QString &text) {
    clear();
    QString role = QStringLiteral("notice");
    int serial = 0;
    QString pending;
    for (auto part : text.split(QStringLiteral("\n\n"), Qt::SkipEmptyParts)) {
        part = part.trimmed();
        if (part.isEmpty() || part == QStringLiteral("[end_turn]")) continue;
        for (const auto &pair : {qMakePair(QStringLiteral("You\n"), QStringLiteral("user")), qMakePair(QStringLiteral("Agent\n"), QStringLiteral("assistant")), qMakePair(QStringLiteral("Thinking\n"), QStringLiteral("thought"))}) {
            if (part.startsWith(pair.first)) { role = pair.second; part.remove(0, pair.first.size()); break; }
        }
        if (part.startsWith(QStringLiteral("Tool:")) || part.startsWith(QChar::ObjectReplacementCharacter)) {
            part.remove(0, part.startsWith(QStringLiteral("Tool:")) ? 5 : 1);
            const auto lines = part.trimmed().split(QLatin1Char('\n'));
            const auto separator = lines.first().lastIndexOf(QStringLiteral(" · "));
            const auto title = separator >= 0 ? lines.first().left(separator) : lines.first();
            const auto status = separator >= 0 ? lines.first().mid(separator + 3) : QStringLiteral("completed");
            const bool completion = title.startsWith(QStringLiteral("exec-")) && status == QStringLiteral("completed") && !pending.isEmpty();
            const auto id = completion ? pending : QStringLiteral("legacy-%1").arg(++serial);
            QJsonObject update{{QStringLiteral("toolCallId"), id}, {QStringLiteral("status"), status}};
            if (!completion) update.insert(QStringLiteral("title"), title);
            if (lines.size() > 1) update.insert(QStringLiteral("content"), QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("content")}, {QStringLiteral("content"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), lines.mid(1).join(QLatin1Char('\n'))}}}}});
            toolCall(update); pending = status == QStringLiteral("in_progress") ? id : QString{};
            role = QStringLiteral("notice");
        } else message(role, part);
    }
    endTurn(false);
}
QString ChatTranscript::toPlainText() const {
    QStringList lines;
    for (const auto &item : entries_) {
        const auto entry = item.toObject();
        lines.append(entry.value(QStringLiteral("text")).toString());
        if (!entry.value(QStringLiteral("details")).toString().isEmpty()) lines.append(entry.value(QStringLiteral("details")).toString());
    }
    return lines.join(QStringLiteral("\n\n"));
}
void ChatTranscript::changeEvent(QEvent *event) {
    QScrollArea::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && rows_ != nullptr) {
        for (int index = 0; index < entries_.size(); ++index) renderRow(index);
    }
}
} // namespace pacsmith::gui
