#include <QFutureWatcher>
#include <QtConcurrent>
#include "core/server_ai.hpp"
#include "gui/acp_chat_widget.hpp"
#include "core/acp_conversations.hpp"
#include <QComboBox>
#include <QAbstractItemView>
#include <algorithm>
#include <QSignalBlocker>
#include "gui/chat_transcript.hpp"
#include "gui/agent_settings_dialog.hpp"
#include "gui/chat_icons.hpp"

#include <QApplication>
#include <QClipboard>
#include <QBuffer>
#include <QFileDialog>
#include <QDialog>
#include <QScrollArea>
#include <QMimeData>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QUuid>
#include <QVBoxLayout>
#include <utility>

namespace pacsmith::gui {
namespace {
void clearLayout(QLayout *layout) {
    while (auto *item = layout->takeAt(0)) {
        if (item->widget() != nullptr) item->widget()->deleteLater();
        delete item;
    }
}

}

AcpChatWidget::AcpChatWidget(HarnessProfile profile, ConnectionConfig connection, QString key, QString context, QWidget *parent, QString projectId)
    : QWidget(parent), profile_(std::move(profile)), connection_(std::move(connection)), context_(std::move(context)), projectId_(std::move(projectId)) {
    filePath_ = QDir(acpDataDirectory()).filePath(QStringLiteral("conversations/%1.json").arg(key));
    setObjectName(key);
    QDir().mkpath(QFileInfo(filePath_).absolutePath());
    auto *layout = new QVBoxLayout(this);
    auto *header = new QHBoxLayout;
    sessionTitle_ = new QComboBox(this);
    sessionTitle_->setObjectName(QStringLiteral("sessionTitle"));
    sessionTitle_->setAccessibleName(QStringLiteral("Recent AI sessions"));
    sessionTitle_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    sessionTitle_->setMinimumContentsLength(12);
    sessionTitle_->setMaxVisibleItems(10);
    header->addWidget(sessionTitle_, 1);
    connect(sessionTitle_, &QComboBox::activated, this, [this](int index) {
        const auto selectedKey = sessionTitle_->itemData(index).toString();
        if (selectedKey != objectName()) emit conversationSelected(selectedKey);
    });
    historyTimer_.setInterval(1000);
    connect(&historyTimer_, &QTimer::timeout, this, [this] { if (isVisible()) refreshSessions(); });
    historyTimer_.start();
    fresh_ = new QPushButton(QStringLiteral("New chat"), this);
    fresh_->setObjectName(QStringLiteral("newChat"));
    header->addWidget(fresh_);
    layout->addLayout(header);
    transcript_ = new ChatTranscript(this);
    transcript_->setObjectName(QStringLiteral("aiTranscript"));
    transcript_->setImageDirectory(QFileInfo(filePath_).dir().filePath(QStringLiteral("images")));
    layout->addWidget(transcript_, 1);
    status_ = new QLabel(QStringLiteral("Ready to connect"), this);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    permissions_ = new QWidget(this);
    permissionLayout_ = new QVBoxLayout(permissions_);
    permissionLayout_->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(permissions_);
    auto *imageRow = new QWidget(this);
    imagesLayout_ = new QHBoxLayout(imageRow);
    imagesLayout_->setContentsMargins(0, 0, 0, 0);
    auto *imageScroll = new QScrollArea(this);
    imageScroll->setWidgetResizable(true);
    imageScroll->setWidget(imageRow);
    imageScroll->setFixedHeight(155);
    imageScroll->hide();
    imagesScroll_ = imageScroll;
    layout->addWidget(imageScroll);
    auto *composer = new ChatImageInput(this);
    composer->send = [this] { submit(input_->toPlainText()); };
    composer->attach = [this](const QMimeData *source) {
        if (isBusy()) return;
        if (source->hasImage()) {
            const auto image = qvariant_cast<QImage>(source->imageData());
            QByteArray bytes;
            QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            attachImage(bytes, QStringLiteral("Pasted screenshot"));
        } else for (const auto &url : source->urls()) {
            if (!url.isLocalFile()) continue;
            QFile file(url.toLocalFile());
            if (file.open(QIODevice::ReadOnly)) attachImage(file.read(10 * 1024 * 1024 + 1), QFileInfo(file).fileName());
        }
    };
    input_ = composer;
    input_->setObjectName(QStringLiteral("aiComposer"));
    input_->setAccessibleName(QStringLiteral("Message"));
    setFocusProxy(input_);
    input_->setPlaceholderText(QStringLiteral("Ask a question about this screen…"));
    input_->setToolTip(QStringLiteral("Enter to send · Shift+Enter for a new line"));
    auto *composerRow = new QHBoxLayout;
    auto *accessories = new QVBoxLayout;
    accessories->setSpacing(4);
    attach_ = new QPushButton(this);
    attach_->setIcon(chatIcon(ChatIcon::Image));
    attach_->setAccessibleName(QStringLiteral("Attach image"));
    attach_->setToolTip(QStringLiteral("Attach image · Ctrl+V to paste"));
    attach_->setObjectName(QStringLiteral("attachImage"));
    settings_ = new QPushButton(this);
    settings_->setIcon(chatIcon(ChatIcon::Settings));
    settings_->setAccessibleName(QStringLiteral("Conversation settings"));
    settings_->setToolTip(QStringLiteral("Conversation settings"));
    settings_->setObjectName(QStringLiteral("conversationSettings"));
    for (auto *button : {settings_, attach_}) {
        button->setFlat(true);
        button->setIconSize(QSize(22, 22));
        button->setFixedSize(32, 32);
        accessories->addWidget(button);
    }
    composerRow->addLayout(accessories);
    connect(settings_, &QPushButton::clicked, this, [this] {
        if (!agent_.isReady()) start();
        AgentSettingsDialog dialog(&agent_, isCodexAcp(profile_), this);
        dialog.exec();
    });
    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("chatComposerFrame"));
    frame->setStyleSheet(QStringLiteral("QFrame#chatComposerFrame { background: palette(base); border: 1px solid palette(mid); border-radius: 6px; }"));
    frame->setFixedHeight(100);
    auto *editorLayout = new QVBoxLayout(frame);
    editorLayout->setContentsMargins(4, 4, 4, 4);
    editorLayout->setSpacing(0);
    input_->setFrameShape(QFrame::NoFrame);
    editorLayout->addWidget(input_, 1);
    action_ = new QPushButton(frame);
    action_->setObjectName(QStringLiteral("chatAction"));
    action_->setFixedSize(28, 28);
    action_->setIconSize(QSize(18, 18));
    action_->setStyleSheet(QStringLiteral("QPushButton { background: palette(highlight); border: none; border-radius: 14px; } QPushButton:hover { border: 2px solid palette(mid); } QPushButton:focus { border: 2px solid palette(text); }"));
    editorLayout->addWidget(action_, 0, Qt::AlignRight);
    composerRow->addWidget(frame, 1);
    layout->addLayout(composerRow);
    connect(attach_, &QPushButton::clicked, this, [this] {
        const auto paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Attach screenshots"), {}, QStringLiteral("Images (*.png *.jpg *.jpeg *.gif *.webp)"));
        for (const auto &path : paths) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) { status_->setText(file.errorString()); continue; }
            attachImage(file.read(10 * 1024 * 1024 + 1), QFileInfo(path).fileName());
        }
    });
    QFile file(filePath_);
    if (file.open(QIODevice::ReadOnly)) {
        const auto record = QJsonDocument::fromJson(file.readAll()).object();
        input_->setPlainText(record.value(QStringLiteral("draft")).toString());
        for (const auto &entry : record.value(QStringLiteral("draftImages")).toArray()) {
            const auto draft = entry.toObject();
            const auto stored = draft.value(QStringLiteral("file")).toString();
            if (images_.size() >= 4 || stored.isEmpty() || QFileInfo(stored).fileName() != stored) continue;
            QFile attachment(QFileInfo(filePath_).dir().filePath(QStringLiteral("images/") + stored));
            if (!attachment.open(QIODevice::ReadOnly)) continue;
            QString error;
            auto image = readChatImage(attachment.read(10 * 1024 * 1024 + 1), draft.value(QStringLiteral("name")).toString(), &error);
            if (image) { image->storageName = stored; images_.append(*image); }
        }
        renderAttachments();
    }
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(500);
    connect(&saveTimer_, &QTimer::timeout, this, &AcpChatWidget::save);
    connect(transcript_, &ChatTranscript::changed, this, [this] { saveTimer_.start(); });
    connect(input_, &QPlainTextEdit::textChanged, this, [this] { saveTimer_.start(); });
    connect(action_, &QPushButton::clicked, this, [this] {
        if (isBusy()) agent_.cancel();
        else submit(input_->toPlainText());
    });
    connect(fresh_, &QPushButton::clicked, this, [this] {
        if (isBusy()) return;
        agent_.close();
        save();
        const auto freshKey = AcpConversations::freshKey();
        filePath_ = QFileInfo(filePath_).dir().filePath(freshKey + QStringLiteral(".json"));
        setObjectName(freshKey);
        setConversationScope(scope_);
        sessionId_.clear();
        transcript_->clear();
        images_.clear();
        input_->clear();
        renderAttachments();
        save();
        emit conversationReset();
        agent_.startServer(connection_, objectName(), projectId_, false);
    });
    connect(&agent_, &AcpClient::busyChanged, this, [this] { controls(); });
    connect(&agent_, &AcpClient::ready, this, [this] {
        status_->setText(QStringLiteral("Connected"));
        if (turnActive_ && !queued_.isEmpty()) sendQueued();
        controls();
    });
    connect(&agent_, &AcpClient::sessionStarted, this, [this](const QString &id) { sessionId_ = id; save(); });
    connect(&agent_, &AcpClient::updated, this, [this](const QJsonObject &update, bool replaying) {
        // The local transcript already contains replayed history; appending it would duplicate turns.
        if (replaying) return;
        const auto kind = update.value(QStringLiteral("sessionUpdate")).toString();
        if (kind == QStringLiteral("agent_message_chunk") || kind == QStringLiteral("agent_thought_chunk")) {
            const auto content = update.value(QStringLiteral("content")).toObject();
            if (content.value(QStringLiteral("type")).toString() != QStringLiteral("text")) return;
            transcript_->message(kind == QStringLiteral("agent_message_chunk") ? QStringLiteral("assistant") : QStringLiteral("thought"), content.value(QStringLiteral("text")).toString(), true);
        } else if (kind == QStringLiteral("tool_call") || kind == QStringLiteral("tool_call_update")) {
            transcript_->toolCall(update);
        } else if (kind == QStringLiteral("plan")) {
            append(QStringLiteral("\n\nPlan\n"));
            for (const auto &entry : update.value(QStringLiteral("entries")).toArray()) {
                const auto item = entry.toObject();
                append(QStringLiteral("• %1 — %2\n").arg(item.value(QStringLiteral("content")).toString(), item.value(QStringLiteral("status")).toString()));
            }
        }
    });
    connect(&agent_, &AcpClient::permissionRemembered, this, [this](const QString &message) { append(message); });
    connect(&agent_, &AcpClient::permissionRequested, this, [this](const QJsonValue &id, const QJsonObject &params) {
        status_->setText(QStringLiteral("Waiting for your approval"));
        auto *card = new QWidget(permissions_);
        auto *cardLayout = new QVBoxLayout(card);
        const auto call = params.value(QStringLiteral("toolCall")).toObject();
        auto *title = new QLabel(call.value(QStringLiteral("title")).toString(QStringLiteral("Agent requests permission")), card);
        title->setTextFormat(Qt::PlainText);
        title->setWordWrap(true);
        cardLayout->addWidget(title);
        auto *details = new QPlainTextEdit(card);
        details->setReadOnly(true);
        details->setMaximumHeight(130);
        details->setPlainText(QString::fromUtf8(QJsonDocument(call).toJson(QJsonDocument::Indented)));
        cardLayout->addWidget(details);
        auto *choices = new QVBoxLayout;
        for (const auto &entry : params.value(QStringLiteral("options")).toArray()) {
            const auto option = entry.toObject();
            auto *button = new QPushButton(option.value(QStringLiteral("name")).toString(), card);
            button->setObjectName(QStringLiteral("permissionChoice"));
            button->setProperty("optionId", option.value(QStringLiteral("optionId")).toString());
            choices->addWidget(button);
            auto description = option.value(QStringLiteral("pacsmithDescription")).toString();
            if (description.isEmpty() && option.value(QStringLiteral("kind")).toString() == QStringLiteral("allow_always"))
                description = QStringLiteral("The agent controls how long this permission lasts.");
            if (!description.isEmpty()) {
                auto *scope = new QLabel(description, card);
                scope->setTextFormat(Qt::PlainText);
                scope->setWordWrap(true);
                choices->addWidget(scope);
            }
            connect(button, &QPushButton::clicked, this, [this, id, option, card] {
                card->setEnabled(false);
                append(QStringLiteral("\n\nPermission: %1\n").arg(option.value(QStringLiteral("name")).toString()));
                agent_.answerPermission(id, option.value(QStringLiteral("optionId")).toString());
                card->deleteLater();
                status_->setText(QStringLiteral("Agent working…"));
            });
        }
        cardLayout->addLayout(choices);
        permissionLayout_->addWidget(card);
        emit approvalRequired();
    });
    connect(&agent_, &AcpClient::permissionsCleared, this, [this] { clearLayout(permissionLayout_); });
    connect(&agent_, &AcpClient::configurationWarning, this, [this](const QString &message) { transcript_->message(QStringLiteral("notice"), message); });
    connect(&agent_, &AcpClient::turnFinished, this, [this](const QString &reason) {
        status_->setText(reason == QStringLiteral("end_turn") ? QStringLiteral("Ready") : QStringLiteral("Stopped"));
        transcript_->endTurn(reason == QStringLiteral("end_turn"));
        const bool active = std::exchange(turnActive_, false);
        controls(); save();
        if (active) emit completed(reason == QStringLiteral("end_turn") ? QString{} : QStringLiteral("Agent stopped: %1").arg(reason));
    });
    connect(&agent_, &AcpClient::failed, this, [this](const QString &message) {
        queued_.clear();
        if (input_->toPlainText().isEmpty()) input_->setPlainText(queuedQuestion_);
        status_->setText(message);
        transcript_->endTurn(false);
        transcript_->message(QStringLiteral("error"), message);
        const bool active = std::exchange(turnActive_, false);
        controls(); save();
        if (active) emit completed(message);
    });
    connect(&agent_, &AcpClient::promptReceived, this, [this](const QJsonArray &content) {
        if (!queued_.isEmpty()) {
            queued_.clear(); queuedQuestion_.clear(); images_.clear(); input_->clear(); renderAttachments();
        }
        transcript_->beginTurn();
        QString text; QJsonArray attachments;
        for (const auto &entry : content) {
            const auto item=entry.toObject();
            if(item.value(QStringLiteral("type")).toString()==QStringLiteral("text")) text+=item.value(QStringLiteral("text")).toString();
            else if(item.value(QStringLiteral("type")).toString()==QStringLiteral("image")) {
                const auto bytes=QByteArray::fromBase64(item.value(QStringLiteral("data")).toString().toLatin1());
                const auto name=QUuid::createUuid().toString(QUuid::WithoutBraces);
                QFile cachedImage(QFileInfo(filePath_).dir().filePath(QStringLiteral("images/")+name));
                QDir().mkpath(QFileInfo(cachedImage).absolutePath());
                if(cachedImage.open(QIODevice::WriteOnly)){cachedImage.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);cachedImage.write(bytes);attachments.append(QJsonObject{{QStringLiteral("file"),name},{QStringLiteral("name"),QStringLiteral("Screenshot")}});}
            }
        }
        transcript_->message(QStringLiteral("user"),text,false,attachments);
    });
    agent_.startServer(connection_, objectName(), projectId_, false);
    refreshSessions();
    controls();
}

AcpChatWidget::~AcpChatWidget() { agent_.close(); save(); }
bool AcpChatWidget::isBusy() const { return turnActive_ || agent_.isBusy(); }
void AcpChatWidget::controls() {
    action_->setIcon(chatIcon(isBusy() ? ChatIcon::Stop : ChatIcon::Send));
    action_->setAccessibleName(isBusy() ? QStringLiteral("Stop agent") : QStringLiteral("Send message"));
    action_->setToolTip(action_->accessibleName());
    attach_->setEnabled(!isBusy());
    input_->setReadOnly(isBusy());
    fresh_->setEnabled(!isBusy());
    settings_->setEnabled(!isBusy());
}
void AcpChatWidget::append(const QString &text) {
    transcript_->message(QStringLiteral("notice"), text.trimmed());
}
void AcpChatWidget::save() {
    saveTimer_.stop();
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath())) { status_->setText(QStringLiteral("Could not create conversation storage.")); return; }
    QJsonArray drafts;
    for (const auto &image : images_) drafts.append(QJsonObject{{QStringLiteral("file"), image.storageName}, {QStringLiteral("name"), image.name}});
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("draft"), input_->toPlainText()}, {QStringLiteral("draftImages"), drafts}}).toJson(QJsonDocument::Compact);
    QFile previous(filePath_);
    if (previous.open(QIODevice::ReadOnly) && previous.readAll() == bytes) return;
    previous.close();
    QSaveFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || file.write(bytes) != bytes.size() || !file.commit()) {
        status_->setText(QStringLiteral("Conversation could not be saved: %1").arg(file.errorString()));
    }
}
void AcpChatWidget::start() {
    status_->setText(QStringLiteral("Connecting to the server agent…"));
    if (agent_.serverOwned()) agent_.initializeServer();
    else agent_.startServer(connection_, objectName(), projectId_);
}
bool AcpChatWidget::submit(const QString &prompt) {
    if (isBusy() || (prompt.trimmed().isEmpty() && images_.isEmpty())) return false;
    if (!refreshContext()) return false;
    if (scope_.isEmpty()) setConversationScope(objectName());
    queuedQuestion_ = prompt;
    queued_ = context_.isEmpty() ? prompt : context_ + QStringLiteral("\n\nUser request:\n") + prompt;
    const AcpConversations conversations(QFileInfo(filePath_).absolutePath());
    if (conversations.description(objectName()).isEmpty())
        queued_.prepend(QStringLiteral("As one of your first actions, call the injected PacSmith set_session_description tool with a short, specific title (at most 120 characters) describing this request in its package/screen context. Then carry out the request. Do not ask the user to name the session.\n\n"));
    turnActive_ = true;
    if (agent_.isReady()) sendQueued();
    else start();
    controls();
    return true;
}
void AcpChatWidget::sendQueued() {
    QJsonArray content;
    QJsonArray attachments;
    for (const auto &image : images_) {
        content.append(image.content());
        attachments.append(QJsonObject{{QStringLiteral("file"), image.storageName}, {QStringLiteral("name"), image.name}});
    }
    if (!agent_.prompt(queued_, content, queuedQuestion_.isNull() ? QStringLiteral("") : queuedQuestion_)) return;
    status_->setText(QStringLiteral("Agent working…"));
}
bool AcpChatWidget::attachImage(const QByteArray &bytes, const QString &name) {
    if (isBusy()) return false;
    if (images_.size() >= 4) { status_->setText(QStringLiteral("Attach up to four images per message.")); return false; }
    QString error;
    auto image = readChatImage(bytes, name, &error);
    if (!image) { status_->setText(error); return false; }
    const auto directory = QFileInfo(filePath_).dir().filePath(QStringLiteral("images"));
    image->storageName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSaveFile file(QDir(directory).filePath(image->storageName));
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || file.write(bytes) != bytes.size() || !file.commit()) {
        status_->setText(QStringLiteral("Could not save the screenshot: %1").arg(file.errorString()));
        return false;
    }
    images_.append(*image);
    renderAttachments();
    input_->setFocus(Qt::OtherFocusReason);
    save();
    return true;
}
void AcpChatWidget::renderAttachments() {
    clearLayout(imagesLayout_);
    imagesScroll_->setVisible(!images_.isEmpty());
    for (qsizetype index = 0; index < images_.size(); ++index) {
        auto *card = new QWidget(this);
        auto *layout = new QVBoxLayout(card);
        auto *preview = new QLabel(card);
        preview->setObjectName(QStringLiteral("draftImage"));
        preview->setPixmap(QPixmap::fromImage(images_.at(index).preview));
        preview->setToolTip(images_.at(index).name);
        layout->addWidget(preview);
        auto *remove = new QPushButton(QStringLiteral("Remove"), card);
        remove->setEnabled(!isBusy());
        connect(remove, &QPushButton::clicked, this, [this, index] {
            if (!isBusy() && index < images_.size()) { images_.removeAt(index); renderAttachments(); save(); }
        });
        layout->addWidget(remove);
        imagesLayout_->addWidget(card);
    }
}
void AcpChatWidget::setContextProvider(std::function<std::optional<ChatScreenContext>()> provider) {
    contextProvider_ = std::move(provider);
}
void AcpChatWidget::focusComposer() {
    window()->activateWindow();
    input_->setFocus(Qt::ShortcutFocusReason);
}
void AcpChatWidget::setConversationScope(const QString &scope) {
    scope_ = scope.isEmpty() ? objectName() : scope;
    refreshSessions();
}
void AcpChatWidget::refreshSessions() {
    if (sessionTitle_->view()->isVisible()) return;
    if(historyInFlight_)return;
    historyInFlight_=true;
    auto *watcher=new QFutureWatcher<QJsonArray>(this);
    connect(watcher,&QFutureWatcher<QJsonArray>::finished,this,[this,watcher] {
        const auto sessions=watcher->result();watcher->deleteLater();historyInFlight_=false;
        if(sessionTitle_->view()->isVisible())return;
        const QSignalBlocker blocker(sessionTitle_);sessionTitle_->clear();
        for(const auto &entry:sessions) {
            const auto row=entry.toObject();auto title=row.value(QStringLiteral("title")).toString();
            sessionTitle_->addItem(title.isEmpty()?QStringLiteral("New conversation"):title,row.value(QStringLiteral("id")).toString());
        }
        if(sessionTitle_->findData(objectName())<0)sessionTitle_->addItem(QStringLiteral("New conversation"),objectName());
        sessionTitle_->setCurrentIndex(sessionTitle_->findData(objectName()));sessionTitle_->setToolTip(sessionTitle_->currentText());
    });
    const auto connection=connection_;const auto project=projectId_;
    watcher->setFuture(QtConcurrent::run([connection,project] { return ServerAi(connection).conversations(project); }));
}

bool AcpChatWidget::refreshContext() {
    if (!contextProvider_) return true;
    const auto current = contextProvider_();
    if (!current) {
        status_->setText(QStringLiteral("The selected package or library changed. Reopen Ask AI to use its conversation."));
        return false;
    }
    context_ = current->prompt;
    return true;
}
}
