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

void setLinkedLabel(QLabel *label, const QString &html) {
    label->setTextFormat(Qt::RichText);
    label->setOpenExternalLinks(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setText(html);
}

void setPlainLabel(QLabel *label, const QString &text) {
    label->setTextFormat(Qt::PlainText);
    label->setOpenExternalLinks(false);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setText(text);
}

QString htmlLink(const QString &url, const QString &text) {
    return QStringLiteral("<a href=\"%1\">%2</a>").arg(url.toHtmlEscaped(), text.toHtmlEscaped());
}

bool hostSelected(const QStringList &hosts, const QString &value) { return hosts.contains(value, Qt::CaseInsensitive); }

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
        if (!iface.flags().testFlag(QNetworkInterface::IsUp) || !iface.flags().testFlag(QNetworkInterface::IsRunning)) {
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
        addItem(QStringLiteral("%1 (%2)").arg(iface.name(), addresses.join(QStringLiteral(", "))), iface.name(),
                !allSelected && addressSelected);
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

void wireExclusiveListenHosts(QListWidget *list) {
    QObject::connect(list, &QListWidget::itemChanged, list, [list](QListWidgetItem *changed) {
        if (changed == nullptr) return;
        QSignalBlocker blocker(list);
        if (changed->data(Qt::UserRole).toString() == QStringLiteral("0.0.0.0")) {
            if (changed->checkState() == Qt::Checked) {
                for (int row = 0; row < list->count(); ++row) {
                    auto *item = list->item(row);
                    if (item != nullptr && item != changed) item->setCheckState(Qt::Unchecked);
                }
            }
            return;
        }
        if (changed->checkState() == Qt::Checked && list->count() > 0) {
            list->item(0)->setCheckState(Qt::Unchecked);
        }
    });
}

void configureRepositoryForm(QFormLayout *layout) {
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->setHorizontalSpacing(18);
    layout->setVerticalSpacing(10);
}

void setListeningStatus(QLabel *label, const bool enabled, const QStringList &bound) {
    const auto darkPalette = label->palette().color(QPalette::Window).lightness() < 128;
    if (enabled && !bound.isEmpty()) {
        label->setText(QStringLiteral("●  Listening on %1").arg(bound.join(QStringLiteral(", "))));
        label->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
                                 .arg(darkPalette ? QStringLiteral("#72d392") : QStringLiteral("#247a3d")));
    } else if (enabled) {
        label->setText(QStringLiteral("●  Waiting for the server to bind"));
        label->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
                                 .arg(darkPalette ? QStringLiteral("#e0ba68") : QStringLiteral("#8a5a00")));
    } else {
        label->setText(QStringLiteral("○  Not listening"));
        label->setStyleSheet(
            QStringLiteral("color: %1;").arg(label->palette().color(QPalette::PlaceholderText).name()));
    }
}

} // namespace

void MainWindow::showSettings() {
    QString loadError;
    auto library = library_.librarySettings(&loadError);
    if (library) applyLibrarySettings(*library);
    else if (!loadError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Could not load library settings"), loadError);
    }
    QString repoLoadError;
    auto repo = library_.repoSettings(&repoLoadError);

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("PacSmith Settings"));
    dialog.setMinimumSize(760, 640);
    dialog.resize(900, 760);
    auto *rootLayout = new QVBoxLayout(&dialog);
    auto *settingsTabs = new QTabWidget(&dialog);

    auto *generalPage = new QWidget(settingsTabs);
    auto *generalLayout = new QVBoxLayout(generalPage);
    auto *sessionGroup = new QGroupBox(QStringLiteral("This machine"), generalPage);
    auto *sessionLayout = new QVBoxLayout(sessionGroup);
    sessionLayout->addWidget(
        settingsSectionHelp(sessionGroup, QStringLiteral("Tray and login behavior stay on this computer."),
                            QStringLiteral("Closing the main window quits PacSmith unless it is kept "
                                           "running in the tray. "
                                           "These options are not library settings; they only affect "
                                           "this GUI.")));
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
        auto *trayNotice = new QLabel(QStringLiteral("No system tray is available, so PacSmith cannot stay "
                                                     "running after the window closes."),
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
    secretsForm->addRow(settingsSectionHelp(secretsGroup,
                                            QStringLiteral("GitHub tokens and AI credentials are stored by pacsmithd."),
                                            QStringLiteral("The daemon chose its secret backend on first start. This "
                                                           "client never reads stored secret values back.")));
    auto *backendLabel = new QLabel(localAdmin ? secretBackendLabel(info ? info->secretBackend : QString{})
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
    githubForm->addRow(settingsSectionHelp(githubGroup,
                                           QStringLiteral("Optional. Used when adding GitHub packages and checking "
                                                          "for updates."),
                                           QStringLiteral("A token is optional for public repositories but raises "
                                                          "GitHub API rate limits. "
                                                          "The value is stored on the library daemon.")));
    githubForm->addRow(QStringLiteral("Personal access token"), githubToken);
    generalLayout->addWidget(githubGroup);
    generalLayout->addStretch(1);
    settingsTabs->addTab(generalPage, QStringLiteral("General"));

    auto *aiPage = new QWidget(settingsTabs);
    auto *aiLayout = new QVBoxLayout(aiPage);
    auto *aiGroup = new QGroupBox(QStringLiteral("AI Advisor"), aiPage);
    auto *aiGroupLayout = new QVBoxLayout(aiGroup);
    auto *aiSectionHelp =
        settingsSectionHelp(aiGroup, QStringLiteral("AI is optional. Local inspection always runs first."),
                            QStringLiteral("Provider, model, and credentials belong to the library "
                                           "daemon so every client uses the same advisor."));
    aiGroupLayout->addWidget(aiSectionHelp);
    auto *form = new QFormLayout;
    auto *provider = new QComboBox(aiPage);
    provider->addItems({QStringLiteral("None"), QStringLiteral("ChatGPT subscription"), QStringLiteral("OpenAI API"),
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
    updatesForm->addRow(
        settingsSectionHelp(scheduleGroup, QStringLiteral("The schedule is stored on the library daemon."),
                            QStringLiteral("Each project release still owns its own update source. This "
                                           "schedule is shared by every client of this library.")));
    auto *backgroundEnabled = new QCheckBox(QStringLiteral("Check for updates periodically"), scheduleGroup);
    backgroundEnabled->setChecked(aiSettings_.updates.enabled);
    auto *schedule = new QComboBox(scheduleGroup);
    schedule->addItems({QStringLiteral("Every day"), QStringLiteral("Selected weekday")});
    schedule->setCurrentIndex(aiSettings_.updates.daily ? 0 : 1);
    auto *weekday = new QComboBox(scheduleGroup);
    weekday->addItems({QStringLiteral("Monday"), QStringLiteral("Tuesday"), QStringLiteral("Wednesday"),
                       QStringLiteral("Thursday"), QStringLiteral("Friday"), QStringLiteral("Saturday"),
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
    auto *automaticPrepare = new QCheckBox(QStringLiteral("Download and prepare newly discovered "
                                                          "vendor artifacts automatically"),
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
    cleanupForm->addRow(settingsSectionHelp(cleanupGroup,
                                            QStringLiteral("Retention is a library policy on the daemon."),
                                            QStringLiteral("These counts apply to versions older than the currently "
                                                           "installed PacSmith release. "
                                                           "Complete-release retention cannot be lower than artifact "
                                                           "retention.")));
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

    auto *repoScroll = new QScrollArea(settingsTabs);
    repoScroll->setWidgetResizable(true);
    repoScroll->setFrameShape(QFrame::NoFrame);
    repoScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *repoPage = new QWidget(repoScroll);
    repoPage->setObjectName(QStringLiteral("repositorySettingsPage"));
    repoPage->setStyleSheet(QStringLiteral("QWidget#repositorySettingsPage { background-color: palette(window); }"
                                           "QWidget#repositorySettingsPage QGroupBox {"
                                           "  background-color: rgba(127, 127, 127, 10);"
                                           "  border: 1px solid rgba(127, 127, 127, 75);"
                                           "  border-radius: 8px; margin-top: 14px; padding: 14px 10px 10px 10px;"
                                           "}"
                                           "QWidget#repositorySettingsPage QGroupBox::title {"
                                           "  subcontrol-origin: margin; subcontrol-position: top left; left: 12px;"
                                           "  padding: 0 6px; background-color: palette(window); font-weight: 600;"
                                           "}"
                                           "QWidget#repositorySettingsPage QLineEdit,"
                                           "QWidget#repositorySettingsPage QComboBox,"
                                           "QWidget#repositorySettingsPage QSpinBox { min-height: 30px; }"
                                           "QWidget#repositorySettingsPage QPushButton { min-height: 30px; padding: "
                                           "0 12px; }"
                                           "QWidget#repositorySettingsPage QListWidget { border-radius: 5px; }"
                                           "QLabel#repositoryPageTitle { font-size: 18px; font-weight: 600; }"
                                           "QWidget#repositoryFingerprintField {"
                                           "  background-color: rgba(127, 127, 127, 18); border-radius: 5px; "
                                           "padding: 4px;"
                                           "}"
                                           "QFrame#repositoryCertificationPane {"
                                           "  border-top: 1px solid rgba(127, 127, 127, 65); margin-top: 6px;"
                                           "}"
                                           "QFrame#repositoryCertificationPane QLabel { border: none; }"));
    auto *repoLayout = new QVBoxLayout(repoPage);
    repoLayout->setContentsMargins(20, 18, 20, 20);
    repoLayout->setSpacing(14);
    auto *repoTitle = new QLabel(QStringLiteral("Package repository"), repoPage);
    repoTitle->setObjectName(QStringLiteral("repositoryPageTitle"));
    repoLayout->addWidget(repoTitle);
    repoLayout->addWidget(
        settingsSectionHelp(repoPage,
                            QStringLiteral("Publish signed packages for ordinary pacman clients. No "
                                           "PacSmith enrollment is required."),
                            QStringLiteral("The pacman repository protocol does not use client enrollment or "
                                           "authentication. Package and repository authenticity comes from "
                                           "OpenPGP signatures. To restrict who can reach the repository, expose "
                                           "its listener only on a trusted private network such as a LAN, Tailscale, "
                                           "or WireGuard network; do not publish it directly to the public Internet. "
                                           "Deliver bootstrap scripts through a channel you already trust.")));
    if (!repo && !repoLoadError.isEmpty()) {
        auto *repoLoadNotice = new QLabel(repoLoadError, repoPage);
        repoLoadNotice->setWordWrap(true);
        repoLayout->addWidget(repoLoadNotice);
    }

    auto *repoListenGroup = new QGroupBox(QStringLiteral("Network"), repoPage);
    auto *repoListenForm = new QFormLayout(repoListenGroup);
    configureRepositoryForm(repoListenForm);
    auto *repoEnabled = new QCheckBox(QStringLiteral("Serve the pacman repository over HTTP"), repoListenGroup);
    auto *repoListenPort = new QSpinBox(repoListenGroup);
    repoListenPort->setRange(1, 65535);
    auto *repoListenInterfaces = new QListWidget(repoListenGroup);
    repoListenInterfaces->setMinimumHeight(120);
    auto *repoBound = new QLabel(repoListenGroup);
    repoBound->setObjectName(QStringLiteral("repositoryBoundStatus"));
    repoBound->setWordWrap(true);
    repoBound->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *applyRepoListen = new QPushButton(QStringLiteral("Apply network changes"), repoListenGroup);
    repoListenForm->addRow(QString{}, repoEnabled);
    repoListenForm->addRow(QStringLiteral("Port"), repoListenPort);
    repoListenForm->addRow(QStringLiteral("Interfaces"), repoListenInterfaces);
    repoListenForm->addRow(QStringLiteral("Status"), repoBound);
    repoListenForm->addRow(QString{}, applyRepoListen);
    repoLayout->addWidget(repoListenGroup);

    auto *repoPolicyGroup = new QGroupBox(QStringLiteral("Publication"), repoPage);
    auto *repoPolicyForm = new QFormLayout(repoPolicyGroup);
    configureRepositoryForm(repoPolicyForm);
    auto *repoSoakDays = new QSpinBox(repoPolicyGroup);
    repoSoakDays->setRange(0, 3650);
    repoSoakDays->setSuffix(QStringLiteral(" days"));
    repoSoakDays->setSpecialValueText(QStringLiteral("Immediate"));
    auto *repoPrefixEnabled = new QCheckBox(QStringLiteral("Prefix published package names"), repoPolicyGroup);
    auto *repoPrefixEdit = new QLineEdit(repoPolicyGroup);
    repoPrefixEdit->setPlaceholderText(QStringLiteral("pacsmith-"));
    repoPolicyForm->addRow(
        settingsSectionHelp(repoPolicyGroup,
                            QStringLiteral("Each upstream version has its own soak timer. Rebuilding the same "
                                           "upstream version resets only that version's timer."),
                            QStringLiteral("A newer upstream release does not reset an older version's soak. "
                                           "When a candidate finishes soaking it is promoted only if it would "
                                           "advance stable; stable is never automatically downgraded. Prefixing "
                                           "is optional. PacSmith never publishes two projects under the same "
                                           "effective package name.")));
    repoPolicyForm->addRow(QStringLiteral("Default stable soak"), repoSoakDays);
    repoPolicyForm->addRow(QString{}, repoPrefixEnabled);
    repoPolicyForm->addRow(QStringLiteral("Package-name prefix"), repoPrefixEdit);
    repoLayout->addWidget(repoPolicyGroup);
    QObject::connect(repoPrefixEnabled, &QCheckBox::toggled, repoPrefixEdit, &QLineEdit::setEnabled);

    auto *repoSignGroup = new QGroupBox(QStringLiteral("Signing and trust"), repoPage);
    auto *repoSignLayout = new QVBoxLayout(repoSignGroup);
    repoSignLayout->addWidget(
        settingsSectionHelp(repoSignGroup,
                            QStringLiteral("PacSmith generates a dedicated repository signing key on "
                                           "the server. The private key never leaves pacsmithd."),
                            QStringLiteral("This key is independent of PacSmith's X.509 Server CA "
                                           "and Client CA. Direct trust is the default: consuming "
                                           "machines trust the PacSmith signing key itself. "
                                           "Root-certified trust adds an administrator-owned "
                                           "certification without replacing the operational key.")));
    auto *repoFingerprint = new QLabel(repoSignGroup);
    repoFingerprint->setWordWrap(true);
    repoFingerprint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto fingerprintFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (fingerprintFont.pointSize() > 0 && fingerprintFont.pointSize() < 10) {
        fingerprintFont.setPointSize(10);
    }
    repoFingerprint->setFont(fingerprintFont);
    auto *repoFingerprintRow = new QWidget(repoSignGroup);
    repoFingerprintRow->setObjectName(QStringLiteral("repositoryFingerprintField"));
    auto *repoFingerprintLayout = new QHBoxLayout(repoFingerprintRow);
    repoFingerprintLayout->setContentsMargins(0, 0, 0, 0);
    auto *copyRepoFingerprint = new QPushButton(QStringLiteral("Copy"), repoFingerprintRow);
    copyRepoFingerprint->setEnabled(false);
    repoFingerprintLayout->addWidget(repoFingerprint, 1);
    repoFingerprintLayout->addWidget(copyRepoFingerprint);
    auto *repoKeyringStatus = new QLabel(repoSignGroup);
    repoKeyringStatus->setWordWrap(true);
    repoKeyringStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *repoTrustMode = new QComboBox(repoSignGroup);
    repoTrustMode->addItem(QStringLiteral("Direct trust"), QStringLiteral("direct"));
    repoTrustMode->addItem(QStringLiteral("Root-certified trust"), QStringLiteral("root-certified"));
    auto *repoSignButtons = new QHBoxLayout;
    auto *repoInitSigning = new QPushButton(QStringLiteral("Initialize signing"), repoSignGroup);
    auto *repoDownloadPubkey = new QPushButton(QStringLiteral("Download PacSmith public key…"), repoSignGroup);
    repoSignButtons->addWidget(repoInitSigning);
    repoSignButtons->addWidget(repoDownloadPubkey);
    repoSignButtons->addStretch();
    auto *repoSignForm = new QFormLayout;
    configureRepositoryForm(repoSignForm);
    repoSignForm->addRow(QStringLiteral("Signing key fingerprint"), repoFingerprintRow);
    repoSignForm->addRow(QStringLiteral("Keyring package"), repoKeyringStatus);
    repoSignForm->addRow(QStringLiteral("Trust model"), repoTrustMode);
    repoSignLayout->addLayout(repoSignForm);
    repoSignLayout->addLayout(repoSignButtons);

    auto *repoCertPane = new QFrame(repoSignGroup);
    repoCertPane->setObjectName(QStringLiteral("repositoryCertificationPane"));
    auto *repoCertLayout = new QVBoxLayout(repoCertPane);
    repoCertLayout->setContentsMargins(0, 16, 0, 0);
    repoCertLayout->setSpacing(10);
    auto *repoCertHelp =
        settingsSectionHelp(repoCertPane,
                            QStringLiteral("Upload the root public key, download PacSmith's public key, certify "
                                           "it offline, then upload the certified certificate."),
                            repo && !repo->certificationHelp.isEmpty()
                                ? repo->certificationHelp
                                : QStringLiteral("OpenPGP certification is how one key vouches for another. "
                                                 "PacSmith never asks for the root private key and cannot "
                                                 "expose its own private signing key."),
                            repo ? repo->certificationCommands : QString{});
    repoCertLayout->addWidget(repoCertHelp);
    auto *repoCertStatusPanel = settingsStatusFrame(repoCertPane);
    repoCertStatusPanel->setObjectName(QStringLiteral("repositoryCertificationStatus"));
    auto *repoCertStatusLayout = new QVBoxLayout(repoCertStatusPanel);
    repoCertStatusLayout->setContentsMargins(14, 11, 14, 12);
    repoCertStatusLayout->setSpacing(6);
    auto *repoCertStatusHeading = new QLabel(repoCertStatusPanel);
    auto certHeadingFont = repoCertStatusHeading->font();
    certHeadingFont.setBold(true);
    repoCertStatusHeading->setFont(certHeadingFont);
    auto *repoCertStatus = new QLabel(repoCertStatusPanel);
    repoCertStatus->setWordWrap(true);
    repoCertStatusLayout->addWidget(repoCertStatusHeading);
    repoCertStatusLayout->addWidget(repoCertStatus);
    repoCertLayout->addWidget(repoCertStatusPanel);
    auto *repoCertButtons = new QHBoxLayout;
    auto *repoUploadRoot = new QPushButton(QStringLiteral("Upload root key…"), repoCertPane);
    auto *repoUploadCertified = new QPushButton(QStringLiteral("Upload certified key…"), repoCertPane);
    repoCertButtons->addWidget(repoUploadRoot);
    repoCertButtons->addWidget(repoUploadCertified);
    repoCertButtons->addStretch();
    repoCertLayout->addLayout(repoCertButtons);
    repoSignLayout->addWidget(repoCertPane);
    repoLayout->addWidget(repoSignGroup);

    auto *repoBootstrapGroup = new QGroupBox(QStringLiteral("Client setup"), repoPage);
    auto *repoBootstrapLayout = new QVBoxLayout(repoBootstrapGroup);
    repoBootstrapLayout->addWidget(
        settingsSectionHelp(repoBootstrapGroup,
                            QStringLiteral("Copy a bootstrap script for the selected channel. "
                                           "Deliver it through a channel you already trust."),
                            QStringLiteral("The advertised URL is written into the script as pacman's Server. "
                                           "Use the address consuming machines will actually fetch from, which "
                                           "may be a reverse-proxied HTTPS URL rather than the listen address. "
                                           "If it is left empty, the script falls back to a listen address. Use "
                                           "configuration management, a provisioned OS image, or this "
                                           "authenticated management interface. The script verifies expected "
                                           "fingerprints and fails closed on mismatch.")));
    auto *repoAdvertisedUrl = new QLineEdit(repoBootstrapGroup);
    repoAdvertisedUrl->setPlaceholderText(QStringLiteral("https://packages.example.com"));
    auto *repoBootstrapForm = new QFormLayout;
    configureRepositoryForm(repoBootstrapForm);
    repoBootstrapForm->addRow(QStringLiteral("Advertised URL"), repoAdvertisedUrl);
    auto *repoBootstrapChannel = new QComboBox(repoBootstrapGroup);
    repoBootstrapChannel->addItem(QStringLiteral("Stable"), QStringLiteral("stable"));
    repoBootstrapChannel->addItem(QStringLiteral("Unstable"), QStringLiteral("unstable"));
    auto *copyBootstrap = new QPushButton(QStringLiteral("Copy bootstrap script"), repoBootstrapGroup);
    auto *repoBootstrapRow = new QWidget(repoBootstrapGroup);
    auto *repoBootstrapRowLayout = new QHBoxLayout(repoBootstrapRow);
    repoBootstrapRowLayout->setContentsMargins(0, 0, 0, 0);
    repoBootstrapRowLayout->setSpacing(8);
    repoBootstrapRowLayout->addWidget(repoBootstrapChannel, 1);
    repoBootstrapRowLayout->addWidget(copyBootstrap);
    repoBootstrapForm->addRow(QStringLiteral("Channel"), repoBootstrapRow);
    repoBootstrapLayout->addLayout(repoBootstrapForm);
    repoLayout->addWidget(repoBootstrapGroup);
    repoLayout->addStretch(1);
    repoScroll->setWidget(repoPage);

    auto updateRepoCertVisibility = [repoTrustMode, repoCertPane] {
        repoCertPane->setVisible(repoTrustMode->currentData().toString() == QStringLiteral("root-certified"));
    };
    QObject::connect(repoTrustMode, &QComboBox::currentIndexChanged, &dialog,
                     [updateRepoCertVisibility](int) { updateRepoCertVisibility(); });

    auto applyRepoUi = [=](const RepoSettings &settings) {
        const auto keepRootCertified = repoTrustMode->currentData().toString() == QStringLiteral("root-certified") &&
                                       settings.trustMode != QStringLiteral("root-certified");
        repoEnabled->setChecked(settings.enabled);
        repoListenPort->setValue(settings.listenPort);
        {
            QSignalBlocker blocker(repoListenInterfaces);
            populateListenInterfaces(repoListenInterfaces, settings.listenHosts);
        }
        setListeningStatus(repoBound, settings.enabled, settings.bound);
        repoAdvertisedUrl->setText(settings.advertisedUrl);
        repoSoakDays->setValue(static_cast<int>(settings.soakSeconds / 86400));
        repoPrefixEnabled->setChecked(!settings.packageNamePrefix.isEmpty());
        repoPrefixEdit->setText(settings.packageNamePrefix.isEmpty() ? QStringLiteral("pacsmith-")
                                                                     : settings.packageNamePrefix);
        repoPrefixEdit->setEnabled(repoPrefixEnabled->isChecked());
        if (!keepRootCertified) {
            const auto trustIndex = repoTrustMode->findData(settings.trustMode);
            repoTrustMode->setCurrentIndex(trustIndex >= 0 ? trustIndex : 0);
        }
        if (settings.signingInitialized && !settings.fingerprintSpaced.isEmpty()) {
            repoFingerprint->setText(settings.fingerprintSpaced);
        } else if (settings.signingInitialized && !settings.fingerprint.isEmpty()) {
            repoFingerprint->setText(settings.fingerprint);
        } else {
            repoFingerprint->setText(QStringLiteral("Not initialized"));
        }
        if (settings.keyringVersion > 0 && !settings.keyringUrl.isEmpty()) {
            const auto package = settings.keyringPackage.isEmpty()
                                     ? QStringLiteral("pacsmith-keyring version %1").arg(settings.keyringVersion)
                                     : settings.keyringPackage;
            setLinkedLabel(repoKeyringStatus, QStringLiteral("Published on stable and unstable as "
                                                             "pacsmith-keyring version %1<br>")
                                                      .arg(settings.keyringVersion) +
                                                  htmlLink(settings.keyringUrl, package));
        } else if (settings.keyringVersion > 0) {
            setPlainLabel(repoKeyringStatus,
                          QStringLiteral("pacsmith-keyring version %1").arg(settings.keyringVersion));
        } else {
            setPlainLabel(repoKeyringStatus, QStringLiteral("Not generated until signing is initialized"));
        }
        repoInitSigning->setEnabled(!settings.signingInitialized);
        repoDownloadPubkey->setEnabled(settings.signingInitialized);
        copyRepoFingerprint->setEnabled(settings.signingInitialized && !settings.fingerprint.isEmpty());
        copyRepoFingerprint->setProperty("fingerprint", settings.fingerprint);
        repoUploadRoot->setEnabled(settings.signingInitialized);
        const auto rootFingerprint =
            !settings.rootFingerprintSpaced.isEmpty() ? settings.rootFingerprintSpaced : settings.rootFingerprint;
        repoUploadCertified->setEnabled(settings.signingInitialized && !rootFingerprint.isEmpty());
        repoUploadCertified->setToolTip(rootFingerprint.isEmpty()
                                            ? QStringLiteral("Upload the root public key first")
                                            : QStringLiteral("Upload PacSmith's public key after certifying it "
                                                             "with the root key"));
        if (settings.certified) {
            repoCertStatusHeading->setText(QStringLiteral("✓  Root certification verified"));
            repoCertStatusPanel->setStyleSheet(
                QStringLiteral("QFrame#repositoryCertificationStatus {"
                               "  background-color: rgba(46, 160, 86, 28);"
                               "  border: 1px solid rgba(72, 190, 112, 150); border-radius: 7px;"
                               "}"
                               "QFrame#repositoryCertificationStatus QLabel { background: "
                               "transparent; border: none; }"));
            QString body = QStringLiteral("PacSmith accepted this host's signing key "
                                          "as certified by the uploaded root.");
            if (!rootFingerprint.isEmpty()) {
                body += QStringLiteral("<br><br><b>Root key</b><br>%1").arg(rootFingerprint.toHtmlEscaped());
            }
            if (settings.keyringVersion > 0 && !settings.keyringUrl.isEmpty()) {
                const auto package = settings.keyringPackage.isEmpty()
                                         ? QStringLiteral("pacsmith-keyring version %1").arg(settings.keyringVersion)
                                         : settings.keyringPackage;
                body += QStringLiteral("<br><br><b>Keyring package</b><br>");
                body += htmlLink(settings.keyringUrl, package);
            } else if (settings.keyringVersion > 0) {
                body += QStringLiteral("<br><br><b>Keyring package</b><br>Version %1 is published.")
                            .arg(settings.keyringVersion);
            } else {
                body += QStringLiteral("<br><br><b>Keyring package</b><br>Not published yet");
            }
            setLinkedLabel(repoCertStatus, body);
        } else {
            repoCertStatusHeading->setText(QStringLiteral("●  Root certification incomplete"));
            repoCertStatusPanel->setStyleSheet(
                QStringLiteral("QFrame#repositoryCertificationStatus {"
                               "  background-color: rgba(205, 145, 35, 24);"
                               "  border: 1px solid rgba(205, 145, 35, 135); border-radius: 7px;"
                               "}"
                               "QFrame#repositoryCertificationStatus QLabel { background: "
                               "transparent; border: none; }"));
            if (rootFingerprint.isEmpty()) {
                setPlainLabel(repoCertStatus, QStringLiteral("Upload the root public key to begin certification."));
            } else {
                setPlainLabel(repoCertStatus, QStringLiteral("Root key uploaded: %1\n\nCertify PacSmith's public "
                                                             "key offline, then upload the certified key.")
                                                  .arg(rootFingerprint));
            }
        }
        if (!settings.certificationHelp.isEmpty()) {
            setSettingsSectionHelp(repoCertHelp,
                                   QStringLiteral("Upload the root public key, download PacSmith's public key, "
                                                  "certify it offline, then upload the certified certificate."),
                                   settings.certificationHelp, settings.certificationCommands);
        }
        updateRepoCertVisibility();
    };
    if (repo) applyRepoUi(*repo);
    else applyRepoUi(RepoSettings{});
    wireExclusiveListenHosts(repoListenInterfaces);

    auto collectRepoSettings = [&, repoEnabled, repoListenPort, repoListenInterfaces, repoAdvertisedUrl, repoSoakDays,
                                repoPrefixEnabled, repoPrefixEdit, repoTrustMode]() {
        RepoSettings next;
        next.revision = repo ? repo->revision : 1;
        next.enabled = repoEnabled->isChecked();
        next.listenHosts = selectedListenHosts(repoListenInterfaces);
        next.listenPort = repoListenPort->value();
        next.advertisedUrl = repoAdvertisedUrl->text().trimmed();
        next.soakSeconds = static_cast<qint64>(repoSoakDays->value()) * 86400;
        next.packageNamePrefix = repoPrefixEnabled->isChecked() ? repoPrefixEdit->text().trimmed() : QString{};
        next.trustMode = repoTrustMode->currentData().toString();
        next.certified = repo && repo->certified;
        return next;
    };
    auto saveCollectedRepo = [&, applyRepoUi](RepoSettings next) -> bool {
        if (next.enabled && next.listenHosts.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Listen interfaces"),
                                 QStringLiteral("Select at least one interface, or All IPv4 addresses."));
            return false;
        }
        if (next.listenHosts.isEmpty()) next.listenHosts.append(QStringLiteral("127.0.0.1"));
        QString error;
        auto saved = library_.saveRepoSettings(next, &error);
        if (!saved) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not save repository settings"), error);
            return false;
        }
        repo = saved;
        applyRepoUi(*saved);
        return true;
    };
    auto readPublicKeyFile = [&dialog](const QString &title) -> std::optional<QString> {
        const auto path = QFileDialog::getOpenFileName(
            &dialog, title, QString{}, QStringLiteral("OpenPGP public keys (*.asc *.gpg *.pgp *.pub);;All files (*)"));
        if (path.isEmpty()) return std::nullopt;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not read key file"), file.errorString());
            return std::nullopt;
        }
        return QString::fromUtf8(file.readAll());
    };

    QObject::connect(applyRepoListen, &QPushButton::clicked, &dialog,
                     [&, saveCollectedRepo] { static_cast<void>(saveCollectedRepo(collectRepoSettings())); });
    QObject::connect(repoInitSigning, &QPushButton::clicked, &dialog, [&, applyRepoUi] {
        QString error;
        auto saved = library_.initRepoSigning(&error);
        if (!saved) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not initialize repository signing"), error);
            return;
        }
        repo = saved;
        applyRepoUi(*saved);
    });
    QObject::connect(repoDownloadPubkey, &QPushButton::clicked, &dialog, [&, this] {
        const auto path = QFileDialog::getSaveFileName(
            &dialog, QStringLiteral("Download PacSmith public key"), QStringLiteral("pacsmith.asc"),
            QStringLiteral("OpenPGP public keys (*.asc *.gpg);;All files (*)"));
        if (path.isEmpty()) return;
        QString error;
        if (!library_.downloadRepoPublicKey(path, &error)) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not download PacSmith public key"), error);
            return;
        }
        QMessageBox::information(&dialog, QStringLiteral("PacSmith public key saved"),
                                 QStringLiteral("Certify this public key offline if you use root-certified "
                                                "trust. Never upload a private key."));
    });
    QObject::connect(repoUploadRoot, &QPushButton::clicked, &dialog, [&, applyRepoUi, readPublicKeyFile] {
        const auto key = readPublicKeyFile(QStringLiteral("Upload root public key"));
        if (!key) return;
        QString error;
        auto saved = library_.uploadRepoRootKey(*key, &error);
        if (!saved) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not store root public key"), error);
            return;
        }
        repo = saved;
        applyRepoUi(*saved);
    });
    QObject::connect(repoUploadCertified, &QPushButton::clicked, &dialog, [&, applyRepoUi, readPublicKeyFile] {
        const auto key = readPublicKeyFile(QStringLiteral("Upload certified PacSmith public key"));
        if (!key) return;
        QString error;
        auto saved = library_.uploadRepoCertifiedKey(*key, &error);
        if (!saved) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not store certified PacSmith key"), error);
            return;
        }
        repo = saved;
        applyRepoUi(*saved);
    });
    QObject::connect(copyRepoFingerprint, &QPushButton::clicked, &dialog, [copyRepoFingerprint] {
        const auto fingerprint = copyRepoFingerprint->property("fingerprint").toString();
        if (!fingerprint.isEmpty()) QApplication::clipboard()->setText(fingerprint);
    });
    QObject::connect(copyBootstrap, &QPushButton::clicked, &dialog, [&] {
        if (!saveCollectedRepo(collectRepoSettings())) return;
        QString error;
        const auto script = library_.repoBootstrapScript(repoBootstrapChannel->currentData().toString(), &error);
        if (!script) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not load bootstrap script"), error);
            return;
        }
        QApplication::clipboard()->setText(*script);
    });

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
        clientsLayout->addWidget(
            settingsSectionHelp(clientsPage, QStringLiteral("Remote HTTPS listening is off until you enable it here."),
                                QStringLiteral("Choose whether this library host accepts remote "
                                               "clients, which interfaces, and which port. "
                                               "Registration approval appears only while listening is "
                                               "enabled. PKI administration stays on this computer.")));
        auto *listenGroup = new QGroupBox(QStringLiteral("Remote listening"), clientsPage);
        auto *listenForm = new QFormLayout(listenGroup);
        listenEnabled = new QCheckBox(QStringLiteral("Accept remote clients over HTTPS / mTLS"), listenGroup);
        listenEnabled->setChecked(info && info->listen.enabled);
        listenPort = new QSpinBox(listenGroup);
        listenPort->setRange(1, 65535);
        listenPort->setValue(info ? info->listen.port : 8443);
        listenInterfaces = new QListWidget(listenGroup);
        listenInterfaces->setMinimumHeight(120);
        populateListenInterfaces(listenInterfaces, info ? info->listen.hosts : QStringList{QStringLiteral("0.0.0.0")});
        listenBound = new QLabel(listenGroup);
        listenBound->setObjectName(QStringLiteral("libraryListeningStatus"));
        listenBound->setWordWrap(true);
        listenBound->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *applyListen = new QPushButton(QStringLiteral("Apply listen settings"), listenGroup);
        listenForm->addRow(QString{}, listenEnabled);
        listenForm->addRow(QStringLiteral("Port"), listenPort);
        listenForm->addRow(QStringLiteral("Interfaces"), listenInterfaces);
        listenForm->addRow(QStringLiteral("Status"), listenBound);
        listenForm->addRow(QString{}, applyListen);
        clientsLayout->addWidget(listenGroup);

        fingerprint = new QLabel(clientsPage);
        fingerprint->setWordWrap(true);
        fingerprint->setTextInteractionFlags(Qt::TextSelectableByMouse);
        listenOffNotice = new QLabel(QStringLiteral("Turn on remote listening to enroll other computers. "
                                                    "Pending registration approval is hidden until this "
                                                    "host is listening."),
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
            fingerprint->setText(
                info ? QStringLiteral("Library fingerprint: %1\n%2").arg(info->fingerprint, info->fingerprintSha256)
                     : QStringLiteral("Could not read server identity over the local "
                                      "Unix socket.\n%1")
                           .arg(infoError));
            listenOffNotice->setVisible(!listening);
            pendingLabel->setVisible(listening);
            pendingTable->setVisible(listening);
            setListeningStatus(listenBound, listen.enabled, listen.bound);
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
        wireExclusiveListenHosts(listenInterfaces);
        QObject::connect(applyListen, &QPushButton::clicked, &dialog,
                         [&] { static_cast<void>(applyListenSettings()); });
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
                clientsTable->setItem(
                    row, 1,
                    new QTableWidgetItem(client.revoked ? QStringLiteral("Revoked") : QStringLiteral("Active")));
                clientsTable->setItem(row, 2, new QTableWidgetItem(client.certSha256));
                if (!client.revoked) {
                    auto *revoke = new QPushButton(QStringLiteral("Revoke"), clientsTable);
                    clientsTable->setCellWidget(row, 3, revoke);
                    QObject::connect(revoke, &QPushButton::clicked, &dialog, [&, id = client.id, name = client.name] {
                        if (QMessageBox::question(&dialog, QStringLiteral("Revoke client"),
                                                  QStringLiteral("Revoke %1? It will lose library access "
                                                                 "immediately.")
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

    settingsTabs->addTab(repoScroll, QStringLiteral("Repository"));

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
    auto *aboutVersion =
        new QLabel(QStringLiteral("Version %1").arg(QCoreApplication::applicationVersion()), aboutPage);
    aboutVersion->setAlignment(Qt::AlignCenter);
    auto *aboutSummary = new QLabel(QStringLiteral("Convert vendor Linux packages into pacman "
                                                   "packages you maintain yourself."),
                                    aboutPage);
    aboutSummary->setWordWrap(true);
    aboutSummary->setAlignment(Qt::AlignCenter);
    auto *aboutLink = new QLabel(QStringLiteral("<a "
                                                "href=\"https://github.com/anderson-arlen/"
                                                "pacsmith\">github.com/anderson-arlen/pacsmith</a>"),
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
    QObject::connect(schedule, &QComboBox::currentIndexChanged, &dialog, [&](int) { refreshScheduleControls(); });
    QObject::connect(backgroundEnabled, &QCheckBox::toggled, &dialog, [&](bool) { refreshScheduleControls(); });
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
        const bool chatgptReady = chatGptSession.has_value() || (library && library->chatgptConfigured);
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
            setCredentialStatus(QStringLiteral("AI is optional. Credentials are stored on the "
                                               "library daemon, which makes the provider calls."));
        } else if (subscription && chatgptReady) {
            setCredentialStatus(QStringLiteral("✓ ChatGPT session is stored on the library daemon."));
        } else if (subscription) {
            setCredentialStatus(QStringLiteral("Sign in with ChatGPT. The session is "
                                               "stored on the library daemon."));
        } else if (api && credentialReady) {
            setCredentialStatus(QStringLiteral("✓ Provider credential is stored on the library daemon."));
        } else {
            setCredentialStatus(QStringLiteral("Enter an API key. It is stored on the library daemon "
                                               "and cannot be read back later."));
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
                                      QStringLiteral("Remove PacSmith's saved ChatGPT session from the "
                                                     "library daemon?")) != QMessageBox::Yes) {
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
    QObject::connect(&chatGptLoginService_, &ChatGptLoginService::authorizationUrlReady, &dialog, [&](const QUrl &url) {
        if (!QDesktopServices::openUrl(url)) {
            setCredentialStatus(QStringLiteral("Open this OpenAI sign-in URL in your browser: %1").arg(url.toString()));
        }
    });
    QObject::connect(&chatGptLoginService_, &ChatGptLoginService::progressChanged, &dialog,
                     [&](const QString &message) {
                         updateControls();
                         setCredentialStatus(message);
                     });
    QObject::connect(&chatGptLoginService_, &ChatGptLoginService::failed, &dialog, [&](const QString &message) {
        updateControls();
        setCredentialStatus(QStringLiteral("⚠ %1").arg(message));
    });
    QObject::connect(
        &chatGptLoginService_, &ChatGptLoginService::succeeded, &dialog, [&, this](const QString &serialized) {
            QString error;
            if (!library_.setCredential(QStringLiteral("chatgpt.session"), serialized, &error)) {
                setCredentialStatus(
                    QStringLiteral("⚠ Signed in, but the daemon could not store the session: %1").arg(error));
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
                                 QStringLiteral("Sign in to ChatGPT before selecting "
                                                "the subscription provider."));
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

        if (!saveCollectedRepo(collectRepoSettings())) return;

        aiSettings_.updates.startAtLogin = startAtLogin->isChecked();
        aiSettings_.updates.startMinimized = startMinimized->isChecked();
        aiSettings_.updates.keepInTray = keepInTray->isChecked();
        if (!settingsStore_.save(aiSettings_, &error) ||
            !BackgroundUpdateManager::apply(aiSettings_.updates, QCoreApplication::applicationFilePath(), &error)) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not save this machine's session settings"), error);
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
