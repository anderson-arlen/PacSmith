#include "gui/main_window/common.hpp"

namespace pacsmith::gui {
namespace {

QString aiCredentialName(const AiProviderKind kind) {
    switch (kind) {
    case AiProviderKind::ChatGpt:
        return QStringLiteral("chatgpt.session");
    case AiProviderKind::OpenAi:
        return QStringLiteral("openai.api_key");
    case AiProviderKind::Xai:
        return QStringLiteral("xai.api_key");
    case AiProviderKind::None:
        break;
    }
    return {};
}

QString secretBackendLabel(const QString &backend) {
    if (backend == QStringLiteral("secret-service")) return QStringLiteral("Desktop Secret Service");
    if (backend == QStringLiteral("file")) return QStringLiteral("Protected file on the library host");
    if (backend == QStringLiteral("env")) return QStringLiteral("Process environment on the library host");
    if (backend.isEmpty()) return QStringLiteral("Not initialized");
    return backend;
}

bool hostSelected(const QStringList &hosts, const QString &value) {
    return hosts.contains(value, Qt::CaseInsensitive);
}

void populateListenInterfaces(QListWidget *list, const QStringList &selected) {
    list->clear();
    const bool allSelected = hostSelected(selected, QStringLiteral("0.0.0.0")) ||
                             hostSelected(selected, QStringLiteral("*")) ||
                             hostSelected(selected, QStringLiteral("all"));
    auto addItem = [&](const QString &label, const QString &value, const bool checked) {
        auto *item = new QListWidgetItem(label, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setData(Qt::UserRole, value);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    };
    addItem(QStringLiteral("All IPv4 addresses (0.0.0.0)"), QStringLiteral("0.0.0.0"), allSelected);
    addItem(QStringLiteral("Loopback (127.0.0.1)"), QStringLiteral("127.0.0.1"),
            !allSelected && hostSelected(selected, QStringLiteral("127.0.0.1")));
    QSet<QString> listed;
    listed.insert(QStringLiteral("0.0.0.0"));
    listed.insert(QStringLiteral("127.0.0.1"));
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const auto &iface : interfaces) {
        if (!iface.isValid() || iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        if (!iface.flags().testFlag(QNetworkInterface::IsUp) ||
            !iface.flags().testFlag(QNetworkInterface::IsRunning)) {
            continue;
        }
        QStringList addresses;
        bool addressSelected = hostSelected(selected, iface.name());
        for (const auto &entry : iface.addressEntries()) {
            const auto ip = entry.ip();
            if (ip.isNull() || ip.isLinkLocal()) continue;
            addresses.append(ip.toString());
            addressSelected = addressSelected || hostSelected(selected, ip.toString());
        }
        if (addresses.isEmpty()) continue;
        listed.insert(iface.name());
        addItem(QStringLiteral("%1 (%2)").arg(iface.name(), addresses.join(QStringLiteral(", "))),
                iface.name(), !allSelected && addressSelected);
    }
    for (const auto &host : selected) {
        const auto value = host.trimmed();
        if (value.isEmpty() || listed.contains(value) || allSelected) continue;
        if (value == QStringLiteral("*") || value == QStringLiteral("all")) continue;
        addItem(QStringLiteral("%1 (offline)").arg(value), value, true);
        listed.insert(value);
    }
}

QStringList normalizedListenHosts(QStringList hosts) {
    for (auto &host : hosts) host = host.trimmed();
    hosts.removeAll(QString{});
    hosts.sort();
    hosts.removeDuplicates();
    return hosts;
}

bool sameListenTarget(const ListenSettings &left, const ListenSettings &right) {
    return left.enabled == right.enabled && left.port == right.port &&
           normalizedListenHosts(left.hosts) == normalizedListenHosts(right.hosts);
}

QStringList selectedListenHosts(const QListWidget *list) {
    QStringList hosts;
    if (list == nullptr) return {QStringLiteral("0.0.0.0")};
    for (int row = 0; row < list->count(); ++row) {
        const auto *item = list->item(row);
        if (item == nullptr || item->checkState() != Qt::Checked) continue;
        const auto value = item->data(Qt::UserRole).toString().trimmed();
        if (value == QStringLiteral("0.0.0.0")) return {QStringLiteral("0.0.0.0")};
        if (!value.isEmpty()) hosts.append(value);
    }
    return hosts;
}

} // namespace

void MainWindow::showSettings() {
    QString loadError;
    auto library = library_.librarySettings(&loadError);
    if (library) applyLibrarySettings(*library);
    else if (!loadError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Could not load library settings"), loadError);
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("PacSmith Settings"));
    dialog.setMinimumSize(720, 640);
    auto *rootLayout = new QVBoxLayout(&dialog);
    auto *settingsTabs = new QTabWidget(&dialog);

    auto *generalPage = new QWidget(settingsTabs);
    auto *generalLayout = new QVBoxLayout(generalPage);
    auto *sessionGroup = new QGroupBox(QStringLiteral("This machine"), generalPage);
    auto *sessionLayout = new QVBoxLayout(sessionGroup);
    sessionLayout->addWidget(settingsSectionHelp(
        sessionGroup,
        QStringLiteral("Tray and login behavior stay on this computer."),
        QStringLiteral("Closing the main window quits PacSmith unless it is kept running in the tray. "
                       "These options are not library settings; they only affect this GUI.")));
    auto *keepInTray = new QCheckBox(QStringLiteral("Keep PacSmith running in the tray"), sessionGroup);
    keepInTray->setChecked(aiSettings_.updates.keepInTray || aiSettings_.updates.startMinimized);
    auto *startAtLogin = new QCheckBox(QStringLiteral("Start PacSmith at login"), sessionGroup);
    startAtLogin->setChecked(aiSettings_.updates.startAtLogin);
    auto *startMinimized = new QCheckBox(QStringLiteral("Start minimized to the tray"), sessionGroup);
    startMinimized->setChecked(aiSettings_.updates.startMinimized);
    sessionLayout->addWidget(keepInTray);
    sessionLayout->addWidget(startAtLogin);
    sessionLayout->addWidget(startMinimized);
    generalLayout->addWidget(sessionGroup);
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        auto *trayNotice = new QLabel(
            QStringLiteral("No system tray is available, so PacSmith cannot stay running after the window closes."),
            generalPage);
        trayNotice->setWordWrap(true);
        keepInTray->setEnabled(false);
        keepInTray->setChecked(false);
        startMinimized->setEnabled(false);
        startMinimized->setChecked(false);
        generalLayout->addWidget(trayNotice);
    }

    const auto currentConnection = library_.config();
    const bool localAdmin = currentConnection.mode == ConnectionConfig::Mode::Local;
    QString infoError;
    const auto info = localAdmin ? library_.serverInfo(&infoError) : std::optional<ServerInfo>{};
    auto *secretsGroup = new QGroupBox(QStringLiteral("Library secrets"), generalPage);
    auto *secretsForm = new QFormLayout(secretsGroup);
    secretsForm->addRow(settingsSectionHelp(
        secretsGroup,
        QStringLiteral("GitHub tokens and AI credentials are stored by pacsmithd."),
        QStringLiteral("The daemon chose its secret backend on first start. This client never reads stored secret values back.")));
    auto *backendLabel = new QLabel(
        localAdmin ? secretBackendLabel(info ? info->secretBackend : QString{})
                   : QStringLiteral("Stored on the remote library host"),
        secretsGroup);
    backendLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    secretsForm->addRow(QStringLiteral("Backend"), backendLabel);
    generalLayout->addWidget(secretsGroup);

    auto *githubGroup = new QGroupBox(QStringLiteral("GitHub"), generalPage);
    auto *githubForm = new QFormLayout(githubGroup);
    auto *githubToken = new QLineEdit(githubGroup);
    githubToken->setEchoMode(QLineEdit::Password);
    githubToken->setPlaceholderText(aiSettings_.githubTokenConfigured
                                        ? QStringLiteral("Configured on the library daemon; leave blank to keep it")
                                        : QStringLiteral("Optional personal access token"));
    githubForm->addRow(settingsSectionHelp(
        githubGroup,
        QStringLiteral("Optional. Used when adding GitHub packages and checking for updates."),
        QStringLiteral("A token is optional for public repositories but raises GitHub API rate limits. "
                       "The value is stored on the library daemon.")));
    githubForm->addRow(QStringLiteral("Personal access token"), githubToken);
    generalLayout->addWidget(githubGroup);
    generalLayout->addStretch(1);
    settingsTabs->addTab(generalPage, QStringLiteral("General"));

    auto *aiPage = new QWidget(settingsTabs);
    auto *aiLayout = new QVBoxLayout(aiPage);
    auto *aiGroup = new QGroupBox(QStringLiteral("AI Advisor"), aiPage);
    auto *aiGroupLayout = new QVBoxLayout(aiGroup);
    auto *aiSectionHelp = settingsSectionHelp(
        aiGroup,
        QStringLiteral("AI is optional. Local inspection always runs first."),
        QStringLiteral("Provider, model, and credentials belong to the library daemon so every client uses the same advisor."));
    aiGroupLayout->addWidget(aiSectionHelp);
    auto *form = new QFormLayout;
    auto *provider = new QComboBox(aiPage);
    provider->addItems({QStringLiteral("None"), QStringLiteral("ChatGPT subscription"),
                        QStringLiteral("OpenAI API"),
                        QStringLiteral("xAI / Grok API")});
    provider->setCurrentIndex(static_cast<int>(aiSettings_.provider));
    auto *model = new QComboBox(aiPage);
    model->setEditable(true);
    model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    model->setMinimumContentsLength(36);
    model->lineEdit()->setPlaceholderText(QStringLiteral("Provider model ID"));
    model->setEditText(aiSettings_.model);
    auto *reasoningEffort = new QComboBox(aiPage);
    auto *executionMode = new QComboBox(aiPage);
    executionMode->addItem(QStringLiteral("Standard"), static_cast<int>(AiExecutionMode::Standard));
    executionMode->addItem(QStringLiteral("Fast (priority)"), static_cast<int>(AiExecutionMode::Fast));
    executionMode->setCurrentIndex(executionMode->findData(static_cast<int>(aiSettings_.executionMode)));
    auto *automatic = new QCheckBox(QStringLiteral("Automatically resolve items flagged for review with AI"), aiPage);
    automatic->setChecked(aiSettings_.automaticallyResolveReviewItems);
    auto *apiKey = new QLineEdit(aiPage);
    apiKey->setEchoMode(QLineEdit::Password);
    apiKey->setPlaceholderText(QStringLiteral("Leave blank to keep an existing stored key"));
    auto *chatGptSignIn = new QPushButton(QStringLiteral("Sign in with ChatGPT…"), aiPage);
    auto *loadModels = new QPushButton(QStringLiteral("Load Available Models"), aiPage);
    form->addRow(QStringLiteral("Provider"), provider);
    form->addRow(QStringLiteral("API key"), apiKey);
    form->addRow(QString{}, chatGptSignIn);
    form->addRow(QStringLiteral("Model"), model);
    form->addRow(QString{}, loadModels);
    form->addRow(QStringLiteral("Reasoning effort"), reasoningEffort);
    form->addRow(QStringLiteral("Execution speed"), executionMode);
    form->addRow(QString{}, automatic);
    aiGroupLayout->addLayout(form);
    aiLayout->addWidget(aiGroup);
    auto *aiStatusPanel = settingsStatusFrame(aiPage);
    auto *aiStatusLayout = new QVBoxLayout(aiStatusPanel);
    aiStatusLayout->setContentsMargins(12, 8, 12, 9);
    auto *aiCredentialStatus = new QLabel(aiStatusPanel);
    aiCredentialStatus->setWordWrap(true);
    aiCredentialStatus->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    aiStatusLayout->addWidget(new QLabel(QStringLiteral("●  STATUS"), aiStatusPanel));
    aiStatusLayout->addWidget(aiCredentialStatus);
    aiLayout->addWidget(aiStatusPanel);
    settingsTabs->addTab(aiPage, QStringLiteral("AI Advisor"));

    auto *updatesPage = new QWidget(settingsTabs);
    auto *updatesLayout = new QVBoxLayout(updatesPage);
    auto *scheduleGroup = new QGroupBox(QStringLiteral("Library update checks"), updatesPage);
    auto *updatesForm = new QFormLayout(scheduleGroup);
    updatesForm->addRow(settingsSectionHelp(
        scheduleGroup,
        QStringLiteral("The schedule is stored on the library daemon."),
        QStringLiteral("Each project release still owns its own update source. This schedule is shared by every client of this library.")));
    auto *backgroundEnabled = new QCheckBox(QStringLiteral("Check for updates periodically"), scheduleGroup);
    backgroundEnabled->setChecked(aiSettings_.updates.enabled);
    auto *schedule = new QComboBox(scheduleGroup);
    schedule->addItems({QStringLiteral("Every day"), QStringLiteral("Selected weekday")});
    schedule->setCurrentIndex(aiSettings_.updates.daily ? 0 : 1);
    auto *weekday = new QComboBox(scheduleGroup);
    weekday->addItems({QStringLiteral("Monday"), QStringLiteral("Tuesday"),
                       QStringLiteral("Wednesday"), QStringLiteral("Thursday"),
                       QStringLiteral("Friday"), QStringLiteral("Saturday"),
                       QStringLiteral("Sunday")});
    weekday->setCurrentIndex(std::clamp(aiSettings_.updates.weekDay, 1, 7) - 1);
    auto *checkTime = new QTimeEdit(aiSettings_.updates.localTime, scheduleGroup);
    checkTime->setDisplayFormat(QStringLiteral("HH:mm"));
    updatesForm->addRow(QString{}, backgroundEnabled);
    updatesForm->addRow(QStringLiteral("Frequency"), schedule);
    updatesForm->addRow(QStringLiteral("Weekday"), weekday);
    updatesForm->addRow(QStringLiteral("Local time"), checkTime);
    updatesLayout->addWidget(scheduleGroup);
    auto *behaviorGroup = new QGroupBox(QStringLiteral("When updates are found"), updatesPage);
    auto *behaviorForm = new QFormLayout(behaviorGroup);
    auto *automaticPrepare = new QCheckBox(
        QStringLiteral("Download and prepare newly discovered vendor artifacts automatically"),
        behaviorGroup);
    automaticPrepare->setChecked(aiSettings_.updates.automaticallyPrepare);
    behaviorForm->addRow(QString{}, automaticPrepare);
    updatesLayout->addWidget(behaviorGroup);
    auto *checkNow = new QPushButton(QStringLiteral("Check Now"), updatesPage);
    auto *serviceStatus = new QLabel(updatesPage);
    serviceStatus->setWordWrap(true);
    updatesLayout->addWidget(checkNow, 0, Qt::AlignLeft);
    updatesLayout->addWidget(serviceStatus);
    updatesLayout->addStretch();
    settingsTabs->addTab(updatesPage, QStringLiteral("Updates"));

    auto *cleanupPage = new QWidget(settingsTabs);
    auto *cleanupLayout = new QVBoxLayout(cleanupPage);
    auto *cleanupGroup = new QGroupBox(QStringLiteral("Cleanup"), cleanupPage);
    auto *cleanupForm = new QFormLayout(cleanupGroup);
    cleanupForm->addRow(settingsSectionHelp(
        cleanupGroup,
        QStringLiteral("Retention is a library policy on the daemon."),
        QStringLiteral("These counts apply to versions older than the currently installed PacSmith release. "
                       "Complete-release retention cannot be lower than artifact retention.")));
    auto *retainedPackages = new QSpinBox(cleanupPage);
    retainedPackages->setRange(-1, 50);
    retainedPackages->setSpecialValueText(QStringLiteral("Unlimited"));
    retainedPackages->setValue(aiSettings_.updates.retainedPackageVersions);
    auto *retainedReleases = new QSpinBox(cleanupPage);
    retainedReleases->setRange(-1, 50);
    retainedReleases->setSpecialValueText(QStringLiteral("Unlimited"));
    retainedReleases->setValue(aiSettings_.updates.retainedCompleteReleases);
    cleanupForm->addRow(QStringLiteral("Old package artifacts"), retainedPackages);
    cleanupForm->addRow(QStringLiteral("Old complete releases"), retainedReleases);
    cleanupLayout->addWidget(cleanupGroup);
    cleanupLayout->addStretch();
    settingsTabs->addTab(cleanupPage, QStringLiteral("Cleanup"));

    std::function<bool()> applyListenSettings = [] { return true; };
    std::function<void()> refreshClientsTables;
    std::function<void(const ListenSettings &)> updateListenVisibility = [](const ListenSettings &) {};
    ListenSettings lastAppliedListen = info ? info->listen : ListenSettings{};
    QCheckBox *listenEnabled = nullptr;
    QSpinBox *listenPort = nullptr;
    QListWidget *listenInterfaces = nullptr;
    QLabel *listenBound = nullptr;
    QLabel *fingerprint = nullptr;
    QLabel *listenOffNotice = nullptr;
    QLabel *pendingLabel = nullptr;
    QTableWidget *pendingTable = nullptr;
    QTableWidget *clientsTable = nullptr;
    QLabel *clientsError = nullptr;

    if (localAdmin) {
        auto *clientsPage = new QWidget(settingsTabs);
        auto *clientsLayout = new QVBoxLayout(clientsPage);
        clientsLayout->addWidget(settingsSectionHelp(
            clientsPage,
            QStringLiteral("Remote HTTPS listening is off until you enable it here."),
            QStringLiteral("Choose whether this library host accepts remote clients, which interfaces, and which port. "
                           "Registration approval appears only while listening is enabled. PKI administration stays on this computer.")));
        auto *listenGroup = new QGroupBox(QStringLiteral("Remote listening"), clientsPage);
        auto *listenForm = new QFormLayout(listenGroup);
        listenEnabled = new QCheckBox(QStringLiteral("Accept remote clients over HTTPS / mTLS"), listenGroup);
        listenEnabled->setChecked(info && info->listen.enabled);
        listenPort = new QSpinBox(listenGroup);
        listenPort->setRange(1, 65535);
        listenPort->setValue(info ? info->listen.port : 8443);
        listenInterfaces = new QListWidget(listenGroup);
        listenInterfaces->setMinimumHeight(120);
        populateListenInterfaces(listenInterfaces, info ? info->listen.hosts
                                                        : QStringList{QStringLiteral("0.0.0.0")});
        listenBound = new QLabel(listenGroup);
        listenBound->setWordWrap(true);
        listenBound->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *applyListen = new QPushButton(QStringLiteral("Apply listen settings"), listenGroup);
        listenForm->addRow(QString{}, listenEnabled);
        listenForm->addRow(QStringLiteral("Port"), listenPort);
        listenForm->addRow(QStringLiteral("Interfaces"), listenInterfaces);
        listenForm->addRow(QStringLiteral("Bound"), listenBound);
        listenForm->addRow(QString{}, applyListen);
        clientsLayout->addWidget(listenGroup);

        fingerprint = new QLabel(clientsPage);
        fingerprint->setWordWrap(true);
        fingerprint->setTextInteractionFlags(Qt::TextSelectableByMouse);
        listenOffNotice = new QLabel(
            QStringLiteral("Turn on remote listening to enroll other computers. "
                           "Pending registration approval is hidden until this host is listening."),
            clientsPage);
        listenOffNotice->setWordWrap(true);
        pendingLabel = new QLabel(QStringLiteral("<b>Pending registrations</b>"), clientsPage);
        pendingTable = new QTableWidget(0, 3, clientsPage);
        pendingTable->setHorizontalHeaderLabels(
            {QStringLiteral("Name"), QStringLiteral("Registration"), QStringLiteral("")});
        pendingTable->horizontalHeader()->setStretchLastSection(true);
        pendingTable->verticalHeader()->setVisible(false);
        pendingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        pendingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        auto *clientsLabel = new QLabel(QStringLiteral("<b>Enrolled clients</b>"), clientsPage);
        clientsTable = new QTableWidget(0, 4, clientsPage);
        clientsTable->setHorizontalHeaderLabels(
            {QStringLiteral("Name"), QStringLiteral("Status"), QStringLiteral("Certificate"), QStringLiteral("")});
        clientsTable->horizontalHeader()->setStretchLastSection(true);
        clientsTable->verticalHeader()->setVisible(false);
        clientsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        clientsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        clientsError = new QLabel(clientsPage);
        clientsError->setWordWrap(true);
        auto *refreshClients = new QPushButton(QStringLiteral("Refresh"), clientsPage);
        clientsLayout->addWidget(fingerprint);
        clientsLayout->addWidget(listenOffNotice);
        clientsLayout->addWidget(pendingLabel);
        clientsLayout->addWidget(pendingTable, 1);
        clientsLayout->addWidget(clientsLabel);
        clientsLayout->addWidget(clientsTable, 1);
        clientsLayout->addWidget(clientsError);
        clientsLayout->addWidget(refreshClients, 0, Qt::AlignLeft);
        settingsTabs->addTab(clientsPage, QStringLiteral("Library"));

        updateListenVisibility = [&](const ListenSettings &listen) {
            const bool listening = listen.enabled;
            if (fingerprint == nullptr || listenOffNotice == nullptr || pendingLabel == nullptr ||
                pendingTable == nullptr || listenBound == nullptr) {
                return;
            }
            fingerprint->setVisible(listening);
            fingerprint->setText(info
                                     ? QStringLiteral("Library fingerprint: %1\n%2")
                                           .arg(info->fingerprint, info->fingerprintSha256)
                                     : QStringLiteral("Could not read server identity over the local Unix socket.\n%1")
                                           .arg(infoError));
            listenOffNotice->setVisible(!listening);
            pendingLabel->setVisible(listening);
            pendingTable->setVisible(listening);
            listenBound->setText(listen.bound.isEmpty()
                                     ? (listening ? QStringLiteral("Not bound yet")
                                                  : QStringLiteral("Not listening"))
                                     : listen.bound.join(QStringLiteral(", ")));
        };

        applyListenSettings = [&]() -> bool {
            ListenSettings settings;
            settings.enabled = listenEnabled->isChecked();
            settings.port = listenPort->value();
            settings.hosts = selectedListenHosts(listenInterfaces);
            if (settings.enabled && settings.hosts.isEmpty()) {
                QMessageBox::warning(&dialog, QStringLiteral("Listen interfaces"),
                                     QStringLiteral("Select at least one interface, or All IPv4 addresses."));
                return false;
            }
            if (settings.hosts.isEmpty()) settings.hosts.append(QStringLiteral("0.0.0.0"));
            if (sameListenTarget(settings, lastAppliedListen)) {
                updateListenVisibility(lastAppliedListen);
                return true;
            }
            QString error;
            const auto saved = library_.saveListen(settings, &error);
            if (!saved) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not update listen settings"), error);
                return false;
            }
            lastAppliedListen = *saved;
            updateListenVisibility(*saved);
            if (refreshClientsTables) refreshClientsTables();
            return true;
        };
        QObject::connect(listenInterfaces, &QListWidget::itemChanged, &dialog, [&](QListWidgetItem *changed) {
            if (changed == nullptr || listenInterfaces == nullptr) return;
            QSignalBlocker blocker(listenInterfaces);
            if (changed->data(Qt::UserRole).toString() == QStringLiteral("0.0.0.0")) {
                if (changed->checkState() == Qt::Checked) {
                    for (int row = 0; row < listenInterfaces->count(); ++row) {
                        auto *item = listenInterfaces->item(row);
                        if (item != nullptr && item != changed) item->setCheckState(Qt::Unchecked);
                    }
                }
                return;
            }
            if (changed->checkState() == Qt::Checked && listenInterfaces->count() > 0) {
                listenInterfaces->item(0)->setCheckState(Qt::Unchecked);
            }
        });
        QObject::connect(applyListen, &QPushButton::clicked, &dialog, [&] {
            static_cast<void>(applyListenSettings());
        });
        refreshClientsTables = [&] {
            QString error;
            const auto pending = library_.pendingRegistrations(&error);
            pendingTable->setRowCount(0);
            for (const auto &reg : pending) {
                const auto row = pendingTable->rowCount();
                pendingTable->insertRow(row);
                pendingTable->setItem(row, 0, new QTableWidgetItem(reg.name));
                pendingTable->setItem(row, 1, new QTableWidgetItem(reg.id));
                auto *actions = new QWidget(pendingTable);
                auto *layout = new QHBoxLayout(actions);
                layout->setContentsMargins(0, 0, 0, 0);
                auto *approve = new QPushButton(QStringLiteral("Approve"), actions);
                auto *reject = new QPushButton(QStringLiteral("Reject"), actions);
                layout->addWidget(approve);
                layout->addWidget(reject);
                pendingTable->setCellWidget(row, 2, actions);
                QObject::connect(approve, &QPushButton::clicked, &dialog, [&, id = reg.id] {
                    QString actionError;
                    if (!library_.approveRegistration(id, &actionError)) {
                        QMessageBox::critical(&dialog, QStringLiteral("Could not approve client"), actionError);
                        return;
                    }
                    refreshClientsTables();
                });
                QObject::connect(reject, &QPushButton::clicked, &dialog, [&, id = reg.id] {
                    QString actionError;
                    if (!library_.rejectRegistration(id, &actionError)) {
                        QMessageBox::critical(&dialog, QStringLiteral("Could not reject registration"), actionError);
                        return;
                    }
                    refreshClientsTables();
                });
            }
            QString clientError;
            const auto enrolled = library_.clients(&clientError);
            if (error.isEmpty()) error = clientError;
            clientsTable->setRowCount(0);
            for (const auto &client : enrolled) {
                const auto row = clientsTable->rowCount();
                clientsTable->insertRow(row);
                clientsTable->setItem(row, 0, new QTableWidgetItem(client.name));
                clientsTable->setItem(row, 1, new QTableWidgetItem(
                    client.revoked ? QStringLiteral("Revoked") : QStringLiteral("Active")));
                clientsTable->setItem(row, 2, new QTableWidgetItem(client.certSha256));
                if (!client.revoked) {
                    auto *revoke = new QPushButton(QStringLiteral("Revoke"), clientsTable);
                    clientsTable->setCellWidget(row, 3, revoke);
                    QObject::connect(revoke, &QPushButton::clicked, &dialog, [&, id = client.id, name = client.name] {
                        if (QMessageBox::question(&dialog, QStringLiteral("Revoke client"),
                                                  QStringLiteral("Revoke %1? It will lose library access immediately.")
                                                      .arg(name)) != QMessageBox::Yes) {
                            return;
                        }
                        QString actionError;
                        if (!library_.revokeClient(id, &actionError)) {
                            QMessageBox::critical(&dialog, QStringLiteral("Could not revoke client"), actionError);
                            return;
                        }
                        refreshClientsTables();
                    });
                }
            }
            pendingTable->resizeColumnsToContents();
            clientsTable->resizeColumnsToContents();
            clientsError->setText(error);
            clientsError->setVisible(!error.isEmpty());
        };
        QObject::connect(refreshClients, &QPushButton::clicked, &dialog, refreshClientsTables);
        updateListenVisibility(info ? info->listen : ListenSettings{});
        refreshClientsTables();
    }

    auto *aboutPage = new QWidget(settingsTabs);
    auto *aboutLayout = new QVBoxLayout(aboutPage);
    auto *hero = new QLabel(aboutPage);
    QPixmap heroPixmap(QStringLiteral(":/pacsmith/icons/pacsmith-hero.png"));
    const auto heroSide = qRound(192.0 * dialog.devicePixelRatioF());
    auto heroScaled = heroPixmap.scaled(heroSide, heroSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    heroScaled.setDevicePixelRatio(dialog.devicePixelRatioF());
    hero->setPixmap(heroScaled);
    hero->setAlignment(Qt::AlignCenter);
    auto *aboutName = new QLabel(QStringLiteral("PacSmith"), aboutPage);
    auto nameFont = aboutName->font();
    if (nameFont.pointSize() > 0) nameFont.setPointSize(nameFont.pointSize() + 6);
    else nameFont.setPixelSize(nameFont.pixelSize() + 8);
    nameFont.setBold(true);
    aboutName->setFont(nameFont);
    aboutName->setAlignment(Qt::AlignCenter);
    auto *aboutVersion = new QLabel(
        QStringLiteral("Version %1").arg(QCoreApplication::applicationVersion()), aboutPage);
    aboutVersion->setAlignment(Qt::AlignCenter);
    auto *aboutSummary = new QLabel(
        QStringLiteral("Convert vendor Linux packages into pacman packages you maintain yourself."),
        aboutPage);
    aboutSummary->setWordWrap(true);
    aboutSummary->setAlignment(Qt::AlignCenter);
    auto *aboutLink = new QLabel(
        QStringLiteral("<a href=\"https://github.com/anderson-arlen/pacsmith\">github.com/anderson-arlen/pacsmith</a>"),
        aboutPage);
    aboutLink->setTextFormat(Qt::RichText);
    aboutLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
    aboutLink->setOpenExternalLinks(true);
    aboutLink->setAlignment(Qt::AlignCenter);
    auto *licenseView = new QPlainTextEdit(aboutPage);
    licenseView->setReadOnly(true);
    licenseView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    QFile licenseFile(QStringLiteral(":/pacsmith/LICENSE"));
    if (licenseFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        licenseView->setPlainText(QString::fromUtf8(licenseFile.readAll()).trimmed());
    } else {
        licenseView->setPlainText(QStringLiteral("MIT License\n\nCopyright (c) 2026 Arlen Anderson and contributors"));
    }
    aboutLayout->addWidget(hero);
    aboutLayout->addWidget(aboutName);
    aboutLayout->addWidget(aboutVersion);
    aboutLayout->addWidget(aboutSummary);
    aboutLayout->addWidget(aboutLink);
    aboutLayout->addWidget(licenseView, 1);
    settingsTabs->addTab(aboutPage, QStringLiteral("About"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    rootLayout->addWidget(settingsTabs, 1);
    rootLayout->addWidget(buttons);

    auto refreshSessionControls = [&] {
        const bool trayOk = QSystemTrayIcon::isSystemTrayAvailable();
        keepInTray->setEnabled(trayOk);
        if (!trayOk) {
            keepInTray->setChecked(false);
            startMinimized->setChecked(false);
        } else if (!keepInTray->isChecked()) {
            startMinimized->setChecked(false);
        }
        startMinimized->setEnabled(trayOk && keepInTray->isChecked() && startAtLogin->isChecked());
    };
    QObject::connect(keepInTray, &QCheckBox::toggled, &dialog, [&](bool) { refreshSessionControls(); });
    QObject::connect(startAtLogin, &QCheckBox::toggled, &dialog, [&](bool) { refreshSessionControls(); });
    QObject::connect(startMinimized, &QCheckBox::toggled, &dialog, [&](const bool checked) {
        if (checked) keepInTray->setChecked(true);
        refreshSessionControls();
    });
    refreshSessionControls();

    auto refreshScheduleControls = [&] {
        const bool periodic = backgroundEnabled->isChecked();
        schedule->setEnabled(periodic);
        weekday->setEnabled(periodic && schedule->currentIndex() == 1);
        checkTime->setEnabled(periodic);
    };
    QObject::connect(schedule, &QComboBox::currentIndexChanged, &dialog,
                     [&](int) { refreshScheduleControls(); });
    QObject::connect(backgroundEnabled, &QCheckBox::toggled, &dialog,
                     [&](bool) { refreshScheduleControls(); });
    QObject::connect(retainedPackages, &QSpinBox::valueChanged, &dialog, [retainedReleases](const int value) {
        if (value < 0) retainedReleases->setValue(-1);
        else if (retainedReleases->value() >= 0 && retainedReleases->value() < value) {
            retainedReleases->setValue(value);
        }
    });
    QObject::connect(checkNow, &QPushButton::clicked, &dialog, [&] {
        if (GuiInstanceServer::requestCheck()) {
            serviceStatus->setText(QStringLiteral("✓ Update check started."));
        } else {
            serviceStatus->setText(QStringLiteral("⚠ Could not reach the running PacSmith session to start a check."));
        }
    });
    refreshScheduleControls();

    QHash<int, QString> modelSelections;
    modelSelections.insert(provider->currentIndex(), aiSettings_.model);
    int previousProvider = provider->currentIndex();
    std::optional<ChatGptCredentials> chatGptSession;
    QString chatGptSerialized = sessionCredential(QStringLiteral("chatgpt.session"));
    if (!chatGptSerialized.isEmpty()) {
        chatGptSession = ChatGptCredentials::fromSerialized(chatGptSerialized, nullptr);
    }
    bool modelsLoading = false;

    AiReasoningEffort desiredReasoningEffort = aiSettings_.reasoningEffort;
    auto refreshReasoningEfforts = [&] {
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        const auto supported = supportedReasoningEfforts(kind, model->currentText());
        reasoningEffort->blockSignals(true);
        reasoningEffort->clear();
        reasoningEffort->addItem(reasoningEffortLabel(AiReasoningEffort::ProviderDefault),
                                 static_cast<int>(AiReasoningEffort::ProviderDefault));
        for (const auto effort : supported) {
            reasoningEffort->addItem(reasoningEffortLabel(effort), static_cast<int>(effort));
        }
        const auto desiredIndex = reasoningEffort->findData(static_cast<int>(desiredReasoningEffort));
        reasoningEffort->setCurrentIndex(desiredIndex >= 0 ? desiredIndex : 0);
        reasoningEffort->blockSignals(false);
    };

    auto setCredentialStatus = [&](const QString &text) { aiCredentialStatus->setText(text); };

    auto updateControls = [&] {
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        const bool api = kind == AiProviderKind::OpenAi || kind == AiProviderKind::Xai;
        const bool subscription = kind == AiProviderKind::ChatGpt;
        const bool operationRunning = modelsLoading || chatGptLoginService_.isRunning();
        form->setRowVisible(apiKey, api);
        form->setRowVisible(chatGptSignIn, subscription);
        const bool chatgptReady = chatGptSession.has_value() ||
                                  (library && library->chatgptConfigured);
        const bool apiReady = api && (!apiKey->text().isEmpty() ||
                                      (kind == AiProviderKind::OpenAi && library && library->openaiConfigured) ||
                                      (kind == AiProviderKind::Xai && library && library->xaiConfigured));
        const bool credentialReady = kind == AiProviderKind::None || (subscription ? chatgptReady : apiReady);
        apiKey->setEnabled(api && !operationRunning);
        chatGptSignIn->setEnabled(subscription && !operationRunning);
        chatGptSignIn->setText(chatgptReady ? QStringLiteral("Sign out of ChatGPT…")
                                            : QStringLiteral("Sign in with ChatGPT…"));
        model->setEnabled(kind != AiProviderKind::None && credentialReady && !operationRunning);
        loadModels->setEnabled((api || subscription) && credentialReady && !operationRunning);
        reasoningEffort->setEnabled(credentialReady && reasoningEffort->count() > 1 && !operationRunning);
        executionMode->setEnabled(kind != AiProviderKind::None && credentialReady && !operationRunning);
        automatic->setEnabled(!operationRunning);
        provider->setEnabled(!operationRunning);
        if (kind == AiProviderKind::None) {
            setCredentialStatus(QStringLiteral("AI is optional. Credentials are stored on the library daemon, which makes the provider calls."));
        } else if (subscription && chatgptReady) {
            setCredentialStatus(QStringLiteral("✓ ChatGPT session is stored on the library daemon."));
        } else if (subscription) {
            setCredentialStatus(QStringLiteral("Sign in with ChatGPT. The session is stored on the library daemon."));
        } else if (api && credentialReady) {
            setCredentialStatus(QStringLiteral("✓ Provider credential is stored on the library daemon."));
        } else {
            setCredentialStatus(QStringLiteral("Enter an API key. It is stored on the library daemon and cannot be read back later."));
        }
    };

    QObject::connect(model, &QComboBox::currentTextChanged, &dialog, [&](const QString &) {
        modelSelections.insert(provider->currentIndex(), model->currentText().trimmed());
        refreshReasoningEfforts();
        updateControls();
    });
    QObject::connect(reasoningEffort, &QComboBox::currentIndexChanged, &dialog, [&](const int index) {
        if (index >= 0) {
            desiredReasoningEffort = static_cast<AiReasoningEffort>(reasoningEffort->itemData(index).toInt());
        }
    });
    QObject::connect(provider, &QComboBox::currentIndexChanged, &dialog, [&](const int index) {
        modelSelections.insert(previousProvider, model->currentText().trimmed());
        previousProvider = index;
        model->clear();
        model->setEditText(modelSelections.value(index));
        refreshReasoningEfforts();
        updateControls();
    });
    QObject::connect(apiKey, &QLineEdit::textChanged, &dialog, [&](const QString &) { updateControls(); });
    QObject::connect(chatGptSignIn, &QPushButton::clicked, &dialog, [&, this] {
        const bool signedIn = chatGptSession.has_value() || (library && library->chatgptConfigured);
        if (signedIn) {
            if (QMessageBox::question(&dialog, QStringLiteral("Sign out of ChatGPT"),
                                      QStringLiteral("Remove PacSmith's saved ChatGPT session from the library daemon?")) !=
                QMessageBox::Yes) {
                return;
            }
            QString error;
            if (!library_.deleteCredential(QStringLiteral("chatgpt.session"), &error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not sign out"), error);
                return;
            }
            rememberSessionCredential(QStringLiteral("chatgpt.session"), {});
            chatGptSession.reset();
            chatGptSerialized.clear();
            if (library) library->chatgptConfigured = false;
            updateControls();
            return;
        }
        setCredentialStatus(QStringLiteral("Starting secure ChatGPT browser sign-in…"));
        chatGptLoginService_.start();
    });
    QObject::connect(&chatGptLoginService_, &ChatGptLoginService::authorizationUrlReady, &dialog,
                     [&](const QUrl &url) {
        if (!QDesktopServices::openUrl(url)) {
            setCredentialStatus(QStringLiteral("Open this OpenAI sign-in URL in your browser: %1")
                                    .arg(url.toString()));
        }
    });
    QObject::connect(&chatGptLoginService_, &ChatGptLoginService::progressChanged, &dialog,
                     [&](const QString &message) {
        updateControls();
        setCredentialStatus(message);
    });
    QObject::connect(&chatGptLoginService_, &ChatGptLoginService::failed, &dialog,
                     [&](const QString &message) {
        updateControls();
        setCredentialStatus(QStringLiteral("⚠ %1").arg(message));
    });
    QObject::connect(&chatGptLoginService_, &ChatGptLoginService::succeeded, &dialog,
                     [&, this](const QString &serialized) {
        QString error;
        if (!library_.setCredential(QStringLiteral("chatgpt.session"), serialized, &error)) {
            setCredentialStatus(QStringLiteral("⚠ Signed in, but the daemon could not store the session: %1").arg(error));
            return;
        }
        rememberSessionCredential(QStringLiteral("chatgpt.session"), serialized);
        chatGptSerialized = serialized;
        chatGptSession = ChatGptCredentials::fromSerialized(serialized, &error);
        if (library) library->chatgptConfigured = true;
        updateControls();
        loadModels->click();
    });
    auto applyModels = [&](const QStringList &models) {
        const auto desired = model->currentText().trimmed();
        model->clear();
        model->addItems(models);
        if (!desired.isEmpty()) {
            const auto index = model->findText(desired);
            if (index >= 0) model->setCurrentIndex(index);
            else model->setEditText(desired);
        } else if (!models.isEmpty()) {
            model->setCurrentIndex(0);
        }
        updateControls();
        setCredentialStatus(QStringLiteral("✓ Loaded %1 model(s)").arg(models.size()));
    };
    QObject::connect(loadModels, &QPushButton::clicked, &dialog, [&, this] {
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        if (kind == AiProviderKind::OpenAi || kind == AiProviderKind::Xai) {
            if (!apiKey->text().isEmpty()) {
                QString error;
                if (!library_.setCredential(aiCredentialName(kind), apiKey->text(), &error)) {
                    setCredentialStatus(QStringLiteral("⚠ Could not store the API key: %1").arg(error));
                    return;
                }
                rememberSessionCredential(aiCredentialName(kind), apiKey->text());
                if (library) {
                    if (kind == AiProviderKind::OpenAi) library->openaiConfigured = true;
                    else library->xaiConfigured = true;
                }
            }
        }
        modelsLoading = true;
        updateControls();
        setCredentialStatus(QStringLiteral("Loading available models from the library daemon…"));
        QString error;
        const auto models = library_.listAiModels(aiProviderName(kind), &error);
        modelsLoading = false;
        if (!models) {
            updateControls();
            setCredentialStatus(QStringLiteral("⚠ %1").arg(error));
            return;
        }
        applyModels(*models);
    });
    QObject::connect(&dialog, &QDialog::finished, &chatGptLoginService_, &ChatGptLoginService::cancel);
    refreshReasoningEfforts();
    updateControls();

    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, this] {
        if (!applyListenSettings()) return;
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        if (kind == AiProviderKind::ChatGpt && !(chatGptSession || (library && library->chatgptConfigured))) {
            QMessageBox::warning(&dialog, QStringLiteral("ChatGPT sign-in required"),
                                 QStringLiteral("Sign in to ChatGPT before selecting the subscription provider."));
            return;
        }
        if ((kind == AiProviderKind::OpenAi || kind == AiProviderKind::Xai) && !apiKey->text().isEmpty()) {
            const auto name = aiCredentialName(kind);
            QString error;
            if (!library_.setCredential(name, apiKey->text(), &error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not store API key"), error);
                return;
            }
            rememberSessionCredential(name, apiKey->text());
        }
        if (!githubToken->text().isEmpty()) {
            QString error;
            if (!library_.setCredential(QStringLiteral("github.token"), githubToken->text(), &error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not store GitHub token"), error);
                return;
            }
            rememberSessionCredential(QStringLiteral("github.token"), githubToken->text());
            aiSettings_.githubTokenConfigured = true;
        }

        LibrarySettings next;
        next.revision = librarySettingsRevision_;
        next.provider = kind;
        next.model = kind == AiProviderKind::None ? QString{} : model->currentText().trimmed();
        next.reasoningEffort = kind == AiProviderKind::None
                                   ? AiReasoningEffort::ProviderDefault
                                   : static_cast<AiReasoningEffort>(reasoningEffort->currentData().toInt());
        next.executionMode = kind == AiProviderKind::None
                                 ? AiExecutionMode::Standard
                                 : static_cast<AiExecutionMode>(executionMode->currentData().toInt());
        next.automaticallyResolve = automatic->isChecked();
        next.updatesEnabled = backgroundEnabled->isChecked();
        next.updatesDaily = schedule->currentIndex() == 0;
        next.weekDay = weekday->currentIndex() + 1;
        next.localTime = checkTime->time();
        next.automaticallyPrepare = automaticPrepare->isChecked();
        next.retainedPackageVersions = retainedPackages->value();
        next.retainedCompleteReleases = retainedPackages->value() < 0 || retainedReleases->value() < 0
                                            ? -1
                                            : std::max(retainedReleases->value(), retainedPackages->value());
        QString error;
        auto saved = library_.saveLibrarySettings(next, &error);
        if (!saved) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not save library settings"), error);
            return;
        }
        applyLibrarySettings(*saved);

        aiSettings_.updates.startAtLogin = startAtLogin->isChecked();
        aiSettings_.updates.startMinimized = startMinimized->isChecked();
        aiSettings_.updates.keepInTray = keepInTray->isChecked();
        if (!settingsStore_.save(aiSettings_, &error) ||
            !BackgroundUpdateManager::apply(aiSettings_.updates, QCoreApplication::applicationFilePath(),
                                            &error)) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not save this machine's session settings"),
                                  error);
            return;
        }
        const bool runInTray = aiSettings_.updates.keepInTray && QSystemTrayIcon::isSystemTrayAvailable();
        setKeepRunningInTray(runInTray);
        QApplication::setQuitOnLastWindowClosed(!runInTray);
        static_cast<void>(GuiInstanceServer::requestTray());
        dialog.accept();
    });
    dialog.exec();
}

} // namespace pacsmith::gui
