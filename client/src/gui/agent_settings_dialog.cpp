#include "gui/agent_settings_dialog.hpp"
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

AgentSettingsDialog::AgentSettingsDialog(const HarnessProfile &profile, const ConnectionConfig &connection,
                                       SaveDefaults save, QWidget *parent)
    : QDialog(parent), agent_(new AcpClient(this)), lockedMode_(isCodexAcp(profile)), save_(std::move(save)) {
    initialize(true);
    QString error;
    const auto environment = prepareAcpEnvironment(profile, acpDataDirectory(), QProcessEnvironment::systemEnvironment(), &error);
    if (!environment) { status_->setText(error); return; }
    agent_->start(profile, *environment, acpMcpServers(connection,
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("pacsmith"))));
}

AgentSettingsDialog::AgentSettingsDialog(AcpClient *agent, bool lockedMode, QWidget *parent)
    : QDialog(parent), agent_(agent), lockedMode_(lockedMode) {
    initialize(false);
}

void AgentSettingsDialog::initialize(bool defaults) {
    setWindowTitle(QStringLiteral("Agent settings"));
    setObjectName(QStringLiteral("agentSettings"));
    resize(520, 560);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 16);
    auto *heading = new QLabel(windowTitle(), this);
    auto font = heading->font(); font.setPointSizeF(font.pointSizeF() + 5); heading->setFont(font);
    layout->addWidget(heading);
    auto *notice = new QLabel(defaults
        ? QStringLiteral("Used for new chats and automatic reviews. Choices come from the selected agent. Changes save automatically.")
        : QStringLiteral("Used for the next turn in this conversation. Choices come from its agent."), this);
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
    connect(agent_, &AcpClient::failed, this, [this](const QString &message) { status_->setText(message); });
    connect(agent_, &AcpClient::configurationWarning, this, [this, warning](const QString &message) { status_->clear(); warning->setText(message); warning->show(); });
    connect(agent_, &AcpClient::configOptionApplied, this, [this, warning](const QString &, const QJsonValue &) {
        warning->hide();
        if (!save_) { status_->setText(QStringLiteral("Updated")); return; }
        QJsonObject values;
        for (const auto &entry : agent_->configOptions()) {
            const auto option = entry.toObject();
            values.insert(option.value(QStringLiteral("id")).toString(), option.value(QStringLiteral("currentValue")));
        }
        QString error;
        status_->setText(save_(values, &error) ? QStringLiteral("Defaults saved") : QStringLiteral("Could not save defaults: %1").arg(error));
    });
    if (save_) connect(agent_, &AcpClient::permissionRequested, this, [this](const QJsonValue &id, const QJsonObject &) { agent_->answerPermission(id); });
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
