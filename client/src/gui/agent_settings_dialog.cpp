#include <QUuid>
#include "gui/agent_settings_dialog.hpp"
#include "core/server_ai.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace pacsmith::gui {
namespace {
void addChoices(QComboBox *box, const QJsonArray &choices, const QString &group = {}) {
    for (const auto &entry : choices) {
        const auto choice = entry.toObject();
        if (choice.contains(QStringLiteral("options"))) {
            addChoices(box, choice.value(QStringLiteral("options")).toArray(), choice.value(QStringLiteral("name")).toString());
        } else {
            const auto name = choice.value(QStringLiteral("name")).toString();
            box->addItem(group.isEmpty() ? name : group + QStringLiteral(" — ") + name,
                         choice.value(QStringLiteral("value")).toVariant());
        }
    }
}
}

AgentSettingsDialog::AgentSettingsDialog(const ConnectionConfig &connection, QWidget *parent)
    : QDialog(parent), agent_(new AcpClient(this)), lockedMode_(false), connection_(connection) {
    QString error;
    profile_ = ServerAi(connection_).harness(&error);
    lockedMode_ = profile_ && isCodexAcp(*profile_);
    initialize();
    if (profile_)
        agent_->startServer(connection_, QUuid::createUuid().toString(QUuid::WithoutBraces));
    else
        status_->setText(error.isEmpty() ? QStringLiteral("Choose an AI agent in Settings first.") : error);
}

void AgentSettingsDialog::initialize() {
    setWindowTitle(QStringLiteral("Agent settings"));
    setObjectName(QStringLiteral("agentSettings"));
    resize(520, 560);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 16);
    auto *heading = new QLabel(windowTitle(), this);
    auto font = heading->font(); font.setPointSizeF(font.pointSizeF() + 5); heading->setFont(font);
    layout->addWidget(heading);
    auto *notice = new QLabel(QStringLiteral("Shared by all chats and automatic reviews using this agent. Changes save automatically and apply to the next turn, including in existing chats."), this);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    fields_ = new QWidget(scroll);
    fieldsLayout_ = new QVBoxLayout(fields_);
    fieldsLayout_->setContentsMargins(0, 12, 0, 12);
    fieldsLayout_->setSpacing(10);
    scroll->setWidget(fields_);
    layout->addWidget(scroll, 1);
    auto *warning = new QLabel(this);
    warning->setWordWrap(true);
    warning->setTextFormat(Qt::PlainText);
    warning->hide();
    layout->addWidget(warning);
    status_ = new QLabel(QStringLiteral("Connecting to the agent…"), this);
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    if (lockedMode_) {
        auto *reset = new QPushButton(QStringLiteral("Reset remembered tool permissions"), this);
        reset->setToolTip(QStringLiteral("Ask again for tools previously allowed in this agent and library. Permissions managed internally by the provider are separate."));
        layout->addWidget(reset);
        connect(reset, &QPushButton::clicked, this, [this] {
            QString error;
            status_->setText(agent_->clearRememberedPermissions(&error)
                ? QStringLiteral("Remembered tool permissions cleared.") : error);
        });
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Done"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(agent_, &AcpClient::configOptionsChanged, this, &AgentSettingsDialog::render);
    connect(agent_, &AcpClient::ready, this, [this] { render(agent_->configOptions()); status_->clear(); });
    connect(agent_, &AcpClient::busyChanged, this, [this, buttons](bool busy) {
        fields_->setEnabled(!busy && agent_->isReady());
        buttons->setEnabled(!busy);
        if (busy) status_->setText(agent_->isReady() ? QStringLiteral("Applying setting…") : QStringLiteral("Connecting to the agent…"));
    });
    auto *authentication = new QWidget(this);
    auto *authLayout = new QVBoxLayout(authentication);
    authLayout->setContentsMargins(0, 0, 0, 0);
    layout->insertWidget(layout->count() - 1, authentication);
    connect(agent_, &AcpClient::authenticationAvailable, this, [this, authLayout, authentication](const QJsonArray &methods) {
        while (auto *item=authLayout->takeAt(0)) { if(item->widget())item->widget()->deleteLater();delete item; }
        for(const auto &entry:methods) {
            const auto method=entry.toObject();
            auto *button=new QPushButton(QStringLiteral("Authenticate: %1").arg(method.value(QStringLiteral("name")).toString()),authentication);
            authLayout->addWidget(button);
            connect(button,&QPushButton::clicked,this,[this,method] { agent_->authenticate(method.value(QStringLiteral("id")).toString()); });
        }
    });
    connect(agent_, &AcpClient::failed, this, [this](const QString &message) { status_->setText(message); });
    connect(agent_, &AcpClient::configurationWarning, this, [this, warning](const QString &message) { status_->clear(); warning->setText(message); warning->show(); });
    connect(agent_, &AcpClient::configOptionApplied, this, [this, warning](const QString &, const QJsonValue &) {
        warning->hide();
        QJsonObject values;
        for (const auto &entry : agent_->configOptions()) {
            const auto option = entry.toObject();
            values.insert(option.value(QStringLiteral("id")).toString(), option.value(QStringLiteral("currentValue")));
        }
        QString error;
        if (profile_ && ServerAi(connection_).setConfigDefaults(*profile_, values, &error)) {
            profile_->configDefaults = values;
            status_->setText(QStringLiteral("Settings saved"));
        } else {
            status_->setText(QStringLiteral("Could not save settings: %1").arg(error));
        }
    });
    render(agent_->configOptions());
    if (agent_->isReady()) status_->clear();
}

void AgentSettingsDialog::render(const QJsonArray &options) {
    while (auto *item = fieldsLayout_->takeAt(0)) {
        if (item->widget()) { item->widget()->hide(); item->widget()->deleteLater(); }
        delete item;
    }
    if (options.isEmpty()) fieldsLayout_->addWidget(new QLabel(agent_->isReady()
        ? QStringLiteral("This agent does not advertise supported settings.") : QStringLiteral("Loading agent settings…"), fields_));
    for (const auto &entry : options) {
        const auto option = entry.toObject();
        const auto id = option.value(QStringLiteral("id")).toString();
        if (lockedMode_ && id == QStringLiteral("mode")) continue;
        const auto type = option.value(QStringLiteral("type")).toString();
        if (id.isEmpty() || (type != QStringLiteral("boolean") && type != QStringLiteral("select"))) continue;
        const auto name = option.value(QStringLiteral("name")).toString(id);
        if (type == QStringLiteral("boolean")) {
            auto *check = new QCheckBox(name, fields_);
            check->setObjectName(id);
            check->setChecked(option.value(QStringLiteral("currentValue")).toBool());
            fieldsLayout_->addWidget(check);
            connect(check, &QCheckBox::clicked, this, [this, id](bool value) { agent_->setConfigOption(id, value); });
        } else {
            auto *label = new QLabel(name, fields_);
            label->setTextFormat(Qt::PlainText);
            fieldsLayout_->addWidget(label);
            auto *box = new QComboBox(fields_);
            box->setObjectName(id);
            addChoices(box, option.value(QStringLiteral("options")).toArray());
            box->setCurrentIndex(box->findData(option.value(QStringLiteral("currentValue")).toVariant()));
            fieldsLayout_->addWidget(box);
            connect(box, &QComboBox::activated, this, [this, id, box] { agent_->setConfigOption(id, QJsonValue::fromVariant(box->currentData())); });
        }
        const auto description = option.value(QStringLiteral("description")).toString();
        if (!description.isEmpty()) {
            auto *label = new QLabel(description, fields_);
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
            fieldsLayout_->addWidget(label);
        }
        fieldsLayout_->addSpacing(8);
    }
    fieldsLayout_->addStretch();
    fields_->setEnabled(agent_->isReady() && !agent_->isBusy());
}

} // namespace pacsmith::gui
