#include "gui/main_window/common.hpp"
#include "gui/future_button_guard.hpp"
#include "gui/appearance.hpp"

namespace pacsmith::gui {
namespace {

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

struct SettingsRefreshResult {
    std::optional<LibrarySettings> library;
    std::optional<RepoSettings> repo;
    std::optional<CredentialStatus> githubCredential;
    std::optional<ServerInfo> server;
    QList<Registration> registrations;
    QList<RemoteClient> clients;
    QString libraryError;
    QString repositoryError;
    QString credentialError;
    QString administrationError;
};

struct ClientsRefreshResult {
    QList<Registration> registrations;
    QList<RemoteClient> clients;
    QString error;
};

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

void populateAppearanceModes(QComboBox *combo, const AppearanceMode selected) {
    combo->addItem(QStringLiteral("Auto"), QStringLiteral("auto"));
    combo->addItem(QStringLiteral("Light"), QStringLiteral("light"));
    combo->addItem(QStringLiteral("Dark"), QStringLiteral("dark"));
    combo->setCurrentIndex(combo->findData(appearanceModeName(selected)));
}

AppearanceMode selectedAppearanceMode(const QComboBox *combo) {
    return appearanceModeFromName(combo->currentData().toString());
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
    reloadClientSettings();
    std::optional<LibrarySettings> library;
    std::optional<RepoSettings> repo;
    const QString repoLoadError;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("PacSmith Settings"));
    dialog.setMinimumSize(760, 640);
    dialog.resize(900, 760);
    auto *rootLayout = new QVBoxLayout(&dialog);
    auto *settingsSyncNotice = new QLabel(&dialog);
    settingsSyncNotice->setWordWrap(true);
    settingsSyncNotice->setText(QStringLiteral("Loading library settings…"));
    settingsSyncNotice->setVisible(true);
    rootLayout->addWidget(settingsSyncNotice);
    auto *settingsTabs = new QTabWidget(&dialog);
    settingsTabs->setEnabled(false);

    auto *generalPage = new QWidget(settingsTabs);
    auto *generalLayout = new QVBoxLayout(generalPage);
    auto *appearanceGroup = new QGroupBox(QStringLiteral("Appearance"), generalPage);
    auto *appearanceForm = new QFormLayout(appearanceGroup);
    auto *interfaceTheme = new QComboBox(appearanceGroup);
    auto *trayTheme = new QComboBox(appearanceGroup);
    populateAppearanceModes(interfaceTheme, appSettings_.appearance.interfaceTheme);
    populateAppearanceModes(trayTheme, appSettings_.appearance.trayTheme);
    appearanceForm->addRow(QStringLiteral("Interface theme"), interfaceTheme);
    appearanceForm->addRow(QStringLiteral("System tray theme"), trayTheme);
    generalLayout->addWidget(appearanceGroup);
    auto *sessionGroup = new QGroupBox(QStringLiteral("This machine"), generalPage);
    auto *sessionLayout = new QVBoxLayout(sessionGroup);
    sessionLayout->addWidget(
        settingsSectionHelp(sessionGroup, QStringLiteral("Tray and login behavior stay on this computer."),
                            QStringLiteral("Closing the main window quits PacSmith unless it is kept "
                                           "running in the tray. "
                                           "These options are not library settings; they only affect "
                                           "this GUI.")));
    auto *keepInTray = new QCheckBox(QStringLiteral("Keep PacSmith running in the tray"), sessionGroup);
    keepInTray->setChecked(appSettings_.updates.keepInTray || appSettings_.updates.startMinimized);
    auto *startAtLogin = new QCheckBox(QStringLiteral("Start PacSmith at login"), sessionGroup);
    startAtLogin->setChecked(appSettings_.updates.startAtLogin);
    auto *startMinimized = new QCheckBox(QStringLiteral("Start minimized to the tray"), sessionGroup);
    startMinimized->setChecked(appSettings_.updates.startMinimized);
    bool applyingClientFields = false;
    bool sessionDirty = false;
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
    std::optional<ServerInfo> info;
    QString infoError;
    auto *secretsGroup = new QGroupBox(QStringLiteral("Library secrets"), generalPage);
    auto *secretsForm = new QFormLayout(secretsGroup);
    secretsForm->addRow(settingsSectionHelp(secretsGroup,
                                            QStringLiteral("GitHub tokens are stored by pacsmithd."),
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
    githubToken->setPlaceholderText(appSettings_.githubTokenConfigured
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

    auto *harnessPage = new QWidget(settingsTabs);
    auto *harnessLayout = new QVBoxLayout(harnessPage);
    auto *harnessGroup = new QGroupBox(QStringLiteral("External AI harnesses"), harnessPage);
    auto *harnessGroupLayout = new QVBoxLayout(harnessGroup);
    harnessGroupLayout->addWidget(settingsSectionHelp(
        harnessGroup, QStringLiteral("PacSmith launches your AI harness; it does not provide an AI model or chat."),
        QStringLiteral("`pacsmith plugin path` reports the portable Agent Plugin containing both "
                       "the Skill and MCP declaration. Install and approve it through the harness's "
                       "own plugin controls. `pacsmith skill install` remains available for harnesses "
                       "that only discover shared Agent Skills. Arguments are passed directly "
                       "without a shell. Put {prompt} in an argument to receive PacSmith context. "
                       "For a terminal harness, use a terminal emulator as the executable and put "
                       "its execute arguments followed by the harness command in Arguments.")));
    auto *harnessForm = new QFormLayout;
    auto *harnessSelector = new QComboBox(harnessGroup);
    auto *harnessName = new QLineEdit(harnessGroup);
    auto *harnessExecutable = new QLineEdit(harnessGroup);
    harnessExecutable->setPlaceholderText(QStringLiteral("Executable name or absolute path"));
    auto *harnessArguments = new QPlainTextEdit(harnessGroup);
    harnessArguments->setPlaceholderText(QStringLiteral("One argument per line, for example:\n--prompt\n{prompt}"));
    harnessArguments->setMaximumHeight(150);
    auto *defaultHarness = new QCheckBox(QStringLiteral("Use this profile by default"), harnessGroup);
    auto *profileButtons = new QWidget(harnessGroup);
    auto *profileButtonsLayout = new QHBoxLayout(profileButtons);
    profileButtonsLayout->setContentsMargins(0, 0, 0, 0);
    auto *addHarness = new QPushButton(QStringLiteral("Add profile"), profileButtons);
    auto *removeHarness = new QPushButton(QStringLiteral("Remove profile"), profileButtons);
    profileButtonsLayout->addWidget(addHarness);
    profileButtonsLayout->addWidget(removeHarness);
    profileButtonsLayout->addStretch();
    harnessForm->addRow(QStringLiteral("Profile"), harnessSelector);
    harnessForm->addRow(QStringLiteral("Name"), harnessName);
    harnessForm->addRow(QStringLiteral("Executable"), harnessExecutable);
    harnessForm->addRow(QStringLiteral("Arguments"), harnessArguments);
    harnessForm->addRow(QString{}, defaultHarness);
    harnessForm->addRow(QString{}, profileButtons);
    harnessGroupLayout->addLayout(harnessForm);
    harnessLayout->addWidget(harnessGroup);
    auto *harnessNotice = new QLabel(
        QStringLiteral("If {prompt} is omitted, PacSmith copies the contextual prompt to the clipboard before launching."),
        harnessPage);
    harnessNotice->setWordWrap(true);
    harnessLayout->addWidget(harnessNotice);
    auto *externalHarnessNotice = new QLabel(harnessPage);
    externalHarnessNotice->setWordWrap(true);
    externalHarnessNotice->setVisible(false);
    auto *reloadExternalHarnesses = new QPushButton(QStringLiteral("Reload external changes"), harnessPage);
    reloadExternalHarnesses->setVisible(false);
    harnessLayout->addWidget(externalHarnessNotice);
    harnessLayout->addWidget(reloadExternalHarnesses, 0, Qt::AlignLeft);
    harnessLayout->addStretch();
    settingsTabs->addTab(harnessPage, QStringLiteral("AI Harnesses"));

    QList<HarnessProfile> harnessProfiles = appSettings_.harnessProfiles;
    for (const auto &profile : harnessProfiles) harnessSelector->addItem(profile.name);
    int activeHarness = harnessProfiles.isEmpty() ? -1 : 0;
    bool applyingHarnessFields = false;
    bool harnessDirty = false;
    auto setHarnessFieldsEnabled = [&] {
        const bool enabled = activeHarness >= 0 && activeHarness < harnessProfiles.size();
        harnessName->setEnabled(enabled);
        harnessExecutable->setEnabled(enabled);
        harnessArguments->setEnabled(enabled);
        defaultHarness->setEnabled(enabled);
        removeHarness->setEnabled(enabled);
    };
    auto loadHarness = [&] {
        const QScopedValueRollback applying(applyingHarnessFields, true);
        if (activeHarness < 0 || activeHarness >= harnessProfiles.size()) {
            harnessName->clear();
            harnessExecutable->clear();
            harnessArguments->clear();
            defaultHarness->setChecked(false);
            setHarnessFieldsEnabled();
            return;
        }
        const auto &profile = harnessProfiles.at(activeHarness);
        harnessName->setText(profile.name);
        harnessExecutable->setText(profile.executable);
        harnessArguments->setPlainText(profile.arguments.join(QLatin1Char('\n')));
        defaultHarness->setChecked(profile.isDefault);
        setHarnessFieldsEnabled();
    };
    auto commitHarness = [&] {
        if (activeHarness < 0 || activeHarness >= harnessProfiles.size()) return;
        auto &profile = harnessProfiles[activeHarness];
        profile.name = harnessName->text().trimmed();
        profile.executable = harnessExecutable->text().trimmed();
        profile.arguments = harnessArguments->toPlainText().isEmpty()
                                ? QStringList{}
                                : harnessArguments->toPlainText().split(QLatin1Char('\n'));
        profile.isDefault = defaultHarness->isChecked();
        harnessSelector->setItemText(activeHarness,
                                     profile.name.isEmpty() ? QStringLiteral("Unnamed profile") : profile.name);
    };
    auto replaceHarnessProfiles = [&] {
        const auto selectedName = activeHarness >= 0 && activeHarness < harnessProfiles.size()
            ? harnessProfiles.at(activeHarness).name : QString{};
        const QScopedValueRollback applying(applyingHarnessFields, true);
        harnessProfiles = appSettings_.harnessProfiles;
        harnessSelector->clear();
        int selected = -1;
        for (qsizetype index = 0; index < harnessProfiles.size(); ++index) {
            const auto &profile = harnessProfiles.at(index);
            harnessSelector->addItem(profile.name);
            if (profile.name.compare(selectedName, Qt::CaseInsensitive) == 0) {
                selected = static_cast<int>(index);
            }
            if (selected < 0 && profile.isDefault) selected = static_cast<int>(index);
        }
        activeHarness = selected >= 0 ? selected : harnessProfiles.isEmpty() ? -1 : 0;
        harnessSelector->setCurrentIndex(activeHarness);
        loadHarness();
        harnessDirty = false;
        reloadExternalHarnesses->setVisible(false);
    };
    QObject::connect(harnessSelector, &QComboBox::currentIndexChanged, &dialog, [&](const int index) {
        if (applyingHarnessFields) return;
        commitHarness();
        activeHarness = index;
        loadHarness();
    });
    QObject::connect(addHarness, &QPushButton::clicked, &dialog, [&] {
        harnessDirty = true;
        commitHarness();
        HarnessProfile profile;
        profile.name = QStringLiteral("New harness");
        profile.isDefault = harnessProfiles.isEmpty();
        harnessProfiles.append(profile);
        harnessSelector->addItem(profile.name);
        harnessSelector->setCurrentIndex(static_cast<int>(harnessProfiles.size()) - 1);
    });
    QObject::connect(removeHarness, &QPushButton::clicked, &dialog, [&] {
        harnessDirty = true;
        if (activeHarness < 0 || activeHarness >= harnessProfiles.size()) return;
        harnessProfiles.removeAt(activeHarness);
        harnessSelector->removeItem(activeHarness);
        activeHarness = harnessSelector->currentIndex();
        loadHarness();
    });
    QObject::connect(defaultHarness, &QCheckBox::toggled, &dialog, [&](const bool checked) {
        if (!applyingHarnessFields) harnessDirty = true;
        if (!checked || activeHarness < 0 || activeHarness >= harnessProfiles.size()) return;
        for (auto &profile : harnessProfiles) profile.isDefault = false;
        harnessProfiles[activeHarness].isDefault = true;
    });
    QObject::connect(harnessName, &QLineEdit::textEdited, &dialog,
                     [&](const QString &) { if (!applyingHarnessFields) harnessDirty = true; });
    QObject::connect(harnessExecutable, &QLineEdit::textEdited, &dialog,
                     [&](const QString &) { if (!applyingHarnessFields) harnessDirty = true; });
    QObject::connect(harnessArguments, &QPlainTextEdit::textChanged, &dialog,
                     [&] { if (!applyingHarnessFields) harnessDirty = true; });
    QObject::connect(reloadExternalHarnesses, &QPushButton::clicked, &dialog, [&] {
        replaceHarnessProfiles();
        externalHarnessNotice->setText(QStringLiteral("✓ External harness profiles loaded."));
        externalHarnessNotice->setVisible(true);
    });
    if (activeHarness >= 0) harnessSelector->setCurrentIndex(activeHarness);
    loadHarness();

    auto *updatesPage = new QWidget(settingsTabs);
    auto *updatesLayout = new QVBoxLayout(updatesPage);
    auto *scheduleGroup = new QGroupBox(QStringLiteral("Library update checks"), updatesPage);
    auto *updatesForm = new QFormLayout(scheduleGroup);
    updatesForm->addRow(
        settingsSectionHelp(scheduleGroup, QStringLiteral("The schedule is stored on the library daemon."),
                            QStringLiteral("Each project release still owns its own update source. This "
                                           "schedule is shared by every client of this library. When automatic "
                                           "handling is enabled, PacSmith carries forward the reviewed package "
                                           "configuration and builds only when dependencies, lifecycle behavior, "
                                           "and all structured review checks remain unchanged. Successful builds "
                                           "are published to the project's unstable repository channel. PacSmith "
                                           "never installs an update automatically.")));
    auto *backgroundEnabled = new QCheckBox(QStringLiteral("Check for updates periodically"), scheduleGroup);
    backgroundEnabled->setChecked(appSettings_.updates.enabled);
    auto *schedule = new QComboBox(scheduleGroup);
    schedule->addItems({QStringLiteral("Every day"), QStringLiteral("Selected weekday")});
    schedule->setCurrentIndex(appSettings_.updates.daily ? 0 : 1);
    auto *weekday = new QComboBox(scheduleGroup);
    weekday->addItems({QStringLiteral("Monday"), QStringLiteral("Tuesday"), QStringLiteral("Wednesday"),
                       QStringLiteral("Thursday"), QStringLiteral("Friday"), QStringLiteral("Saturday"),
                       QStringLiteral("Sunday")});
    weekday->setCurrentIndex(std::clamp(appSettings_.updates.weekDay, 1, 7) - 1);
    auto *checkTime = new QTimeEdit(appSettings_.updates.localTime, scheduleGroup);
    checkTime->setDisplayFormat(QStringLiteral("HH:mm"));
    updatesForm->addRow(QString{}, backgroundEnabled);
    updatesForm->addRow(QStringLiteral("Frequency"), schedule);
    updatesForm->addRow(QStringLiteral("Weekday"), weekday);
    updatesForm->addRow(QStringLiteral("Local time"), checkTime);
    auto *automaticPrepare = new QCheckBox(
        QStringLiteral("Download and prepare new updates automatically; each project decides whether to build"),
        scheduleGroup);
    automaticPrepare->setChecked(appSettings_.updates.automaticallyPrepare);
    updatesForm->addRow(QString{}, automaticPrepare);
    updatesLayout->addWidget(scheduleGroup);

    auto *retentionGroup = new QGroupBox(QStringLiteral("Retention"), updatesPage);
    auto *retentionForm = new QFormLayout(retentionGroup);
    retentionForm->addRow(settingsSectionHelp(
        retentionGroup,
        QStringLiteral("Keep a rollback tail behind each package's oldest distribution-channel version."),
        QStringLiteral("When Stable has a published version, completed releases older than Stable are outdated. "
                       "Otherwise, completed releases older than Unstable are outdated. Versions from Stable "
                       "through Unstable remain protected, even when repository HTTP serving is off. PacSmith "
                       "removes each excess outdated version's source artifact and built packages together.")));
    auto *retentionVersions = new QSpinBox(retentionGroup);
    retentionVersions->setRange(-1, 1000);
    retentionVersions->setSuffix(QStringLiteral(" versions"));
    retentionVersions->setSpecialValueText(QStringLiteral("Forever"));
    retentionVersions->setValue(appSettings_.updates.retentionVersions);
    retentionForm->addRow(QStringLiteral("Keep outdated versions per package"), retentionVersions);
    updatesLayout->addWidget(retentionGroup);

    auto *checkNow = new QPushButton(QStringLiteral("Check Now"), updatesPage);
    auto *serviceStatus = new QLabel(updatesPage);
    serviceStatus->setWordWrap(true);
    updatesLayout->addWidget(checkNow, 0, Qt::AlignLeft);
    updatesLayout->addWidget(serviceStatus);
    updatesLayout->addStretch();
    settingsTabs->addTab(updatesPage, QStringLiteral("Updates"));

    auto *buildsPage = new QWidget(settingsTabs);
    auto *buildsLayout = new QVBoxLayout(buildsPage);
    auto *buildsGroup = new QGroupBox(QStringLiteral("Package builds"), buildsPage);
    auto *buildsForm = new QFormLayout(buildsGroup);
    buildsForm->addRow(settingsSectionHelp(
        buildsGroup, QStringLiteral("Compile parallelism is stored on the library daemon."),
        QStringLiteral("The limit applies to builds started after it is saved. More jobs can shorten "
                       "source builds, but they also increase CPU and memory use on the library host.")));
    auto *buildParallelism = new QSpinBox(buildsGroup);
    buildParallelism->setRange(1, 1);
    buildParallelism->setValue(1);
    buildsForm->addRow(QStringLiteral("Parallel compile jobs"), buildParallelism);
    auto *availableBuildCores = new QLabel(QStringLiteral("Loading..."), buildsGroup);
    buildsForm->addRow(QStringLiteral("Available on library host"), availableBuildCores);
    buildsLayout->addWidget(buildsGroup);
    buildsLayout->addStretch();
    settingsTabs->addTab(buildsPage, QStringLiteral("Builds"));

    bool applyingLibraryFields = false;
    bool libraryFieldsDirty = false;
    const auto markLibraryDirty = [&] {
        if (!applyingLibraryFields) libraryFieldsDirty = true;
    };
    QObject::connect(backgroundEnabled, &QCheckBox::toggled, &dialog,
                     [&](bool) { markLibraryDirty(); });
    QObject::connect(schedule, &QComboBox::currentIndexChanged, &dialog,
                     [&](int) { markLibraryDirty(); });
    QObject::connect(weekday, &QComboBox::currentIndexChanged, &dialog,
                     [&](int) { markLibraryDirty(); });
    QObject::connect(checkTime, &QTimeEdit::timeChanged, &dialog,
                     [&](const QTime &) { markLibraryDirty(); });
    QObject::connect(automaticPrepare, &QCheckBox::toggled, &dialog,
                     [&](bool) { markLibraryDirty(); });
    QObject::connect(retentionVersions, &QSpinBox::valueChanged, &dialog,
                     [&](int) { markLibraryDirty(); });
    QObject::connect(buildParallelism, &QSpinBox::valueChanged, &dialog,
                     [&](int) { markLibraryDirty(); });

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
                                           "QLabel#repositoryRecoveryNotice {"
                                           "  background-color: rgba(205, 145, 35, 28);"
                                           "  border: 1px solid rgba(205, 145, 35, 150);"
                                           "  border-radius: 7px; padding: 10px 12px;"
                                           "}"
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
    auto *repoRecoveryNotice = new QLabel(repoPage);
    repoRecoveryNotice->setObjectName(QStringLiteral("repositoryRecoveryNotice"));
    repoRecoveryNotice->setWordWrap(true);
    repoRecoveryNotice->setTextInteractionFlags(Qt::TextSelectableByMouse);
    repoRecoveryNotice->setVisible(false);
    repoLayout->addWidget(repoRecoveryNotice);

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
    auto *repoStableEnabled = new QCheckBox(QStringLiteral("Add a Stable channel"), repoPolicyGroup);
    auto *repoSoakHelp = settingsSectionHelp(
        repoPolicyGroup,
        QStringLiteral("Stable adds a second repository channel. Each project can then use manual promotion or "
                       "automatically promote after this default soak period."),
        QStringLiteral("A newer upstream release does not reset an older version's soak. A project can override "
                       "this duration. Stable is never automatically downgraded."));
    auto *repoSoakLabel = new QLabel(QStringLiteral("Default soak duration"), repoPolicyGroup);
    auto *repoSoakDays = new QSpinBox(repoPolicyGroup);
    repoSoakDays->setRange(0, 3650);
    repoSoakDays->setSuffix(QStringLiteral(" days"));
    repoSoakDays->setSpecialValueText(QStringLiteral("Immediate"));
    auto *repoPrefixEnabled = new QCheckBox(QStringLiteral("Prefix published package names"), repoPolicyGroup);
    auto *repoPrefixEdit = new QLineEdit(repoPolicyGroup);
    repoPrefixEdit->setPlaceholderText(QStringLiteral("pacsmith-"));
    repoPolicyForm->addRow(QString{}, repoPrefixEnabled);
    repoPolicyForm->addRow(QStringLiteral("Package-name prefix"), repoPrefixEdit);
    repoPolicyForm->addRow(QString{}, repoStableEnabled);
    repoPolicyForm->addRow(repoSoakHelp);
    repoPolicyForm->addRow(repoSoakLabel, repoSoakDays);
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
                            QStringLiteral("Copy a bootstrap script for the repository channel. "
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
    repoBootstrapChannel->addItem(QStringLiteral("Unstable"), QStringLiteral("unstable"));
    repoBootstrapChannel->addItem(QStringLiteral("Stable"), QStringLiteral("stable"));
    auto *repoBootstrapOnlyChannel = new QLabel(QStringLiteral("Unstable"), repoBootstrapGroup);
    auto *copyBootstrap = new QPushButton(QStringLiteral("Copy bootstrap script"), repoBootstrapGroup);
    auto *repoBootstrapRow = new QWidget(repoBootstrapGroup);
    auto *repoBootstrapRowLayout = new QHBoxLayout(repoBootstrapRow);
    repoBootstrapRowLayout->setContentsMargins(0, 0, 0, 0);
    repoBootstrapRowLayout->setSpacing(8);
    repoBootstrapRowLayout->addWidget(repoBootstrapOnlyChannel, 1);
    repoBootstrapRowLayout->addWidget(repoBootstrapChannel, 1);
    repoBootstrapRowLayout->addWidget(copyBootstrap);
    auto *repoBootstrapChannelLabel = new QLabel(QStringLiteral("Channel"), repoBootstrapGroup);
    repoBootstrapForm->addRow(repoBootstrapChannelLabel, repoBootstrapRow);
    repoBootstrapLayout->addLayout(repoBootstrapForm);
    repoLayout->addWidget(repoBootstrapGroup);
    repoLayout->addStretch(1);
    repoScroll->setWidget(repoPage);

    auto updateRepoCertVisibility = [repoTrustMode, repoCertPane] {
        repoCertPane->setVisible(repoTrustMode->currentData().toString() == QStringLiteral("root-certified"));
    };
    QObject::connect(repoTrustMode, &QComboBox::currentIndexChanged, &dialog,
                     [updateRepoCertVisibility](int) { updateRepoCertVisibility(); });

    bool applyingRepoFields = false;
    bool repoFieldsDirty = false;
    auto applyRepoUi = [=, &applyingRepoFields](const RepoSettings &settings) {
        const QScopedValueRollback applying(applyingRepoFields, true);
        const auto keepRootCertified = repoTrustMode->currentData().toString() == QStringLiteral("root-certified") &&
                                       settings.trustMode != QStringLiteral("root-certified");
        repoRecoveryNotice->setText(settings.recoveryMessage);
        repoRecoveryNotice->setVisible(!settings.recoveryMessage.isEmpty());
        repoEnabled->setChecked(settings.enabled);
        repoListenPort->setValue(settings.listenPort);
        {
            QSignalBlocker blocker(repoListenInterfaces);
            populateListenInterfaces(repoListenInterfaces, settings.listenHosts);
        }
        setListeningStatus(repoBound, settings.enabled, settings.bound);
        repoAdvertisedUrl->setText(settings.advertisedUrl);
        repoStableEnabled->setChecked(settings.stableEnabled);
        repoSoakHelp->setVisible(settings.stableEnabled);
        repoSoakLabel->setVisible(settings.stableEnabled);
        repoSoakDays->setVisible(settings.stableEnabled);
        repoBootstrapOnlyChannel->setVisible(!settings.stableEnabled);
        repoBootstrapChannel->setVisible(settings.stableEnabled);
        if (!settings.stableEnabled) repoBootstrapChannel->setCurrentIndex(0);
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
            setLinkedLabel(repoKeyringStatus, QStringLiteral("Published on %1 as pacsmith-keyring version %2<br>")
                                                      .arg(settings.stableEnabled
                                                               ? QStringLiteral("Stable and Unstable")
                                                               : QStringLiteral("Unstable"))
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
    const auto markRepoDirty = [&] {
        if (!applyingRepoFields) repoFieldsDirty = true;
    };
    QObject::connect(repoEnabled, &QCheckBox::toggled, &dialog,
                     [&](bool) { markRepoDirty(); });
    QObject::connect(repoListenPort, &QSpinBox::valueChanged, &dialog,
                     [&](int) { markRepoDirty(); });
    QObject::connect(repoListenInterfaces, &QListWidget::itemChanged, &dialog,
                     [&](QListWidgetItem *) { markRepoDirty(); });
    QObject::connect(repoAdvertisedUrl, &QLineEdit::textEdited, &dialog,
                     [&](const QString &) { markRepoDirty(); });
    QObject::connect(repoStableEnabled, &QCheckBox::toggled, &dialog,
                     [&, repoSoakHelp, repoSoakLabel, repoSoakDays,
                      repoBootstrapOnlyChannel, repoBootstrapChannel](bool enabled) {
        repoSoakHelp->setVisible(enabled);
        repoSoakLabel->setVisible(enabled);
        repoSoakDays->setVisible(enabled);
        repoBootstrapOnlyChannel->setVisible(!enabled);
        repoBootstrapChannel->setVisible(enabled);
        if (!enabled) repoBootstrapChannel->setCurrentIndex(0);
        markRepoDirty();
    });
    QObject::connect(repoSoakDays, &QSpinBox::valueChanged, &dialog,
                     [&](int) { markRepoDirty(); });
    QObject::connect(repoPrefixEnabled, &QCheckBox::toggled, &dialog,
                     [&](bool) { markRepoDirty(); });
    QObject::connect(repoPrefixEdit, &QLineEdit::textEdited, &dialog,
                     [&](const QString &) { markRepoDirty(); });
    QObject::connect(repoTrustMode, &QComboBox::currentIndexChanged, &dialog,
                     [&](int) { markRepoDirty(); });

    auto collectRepoSettings = [&, repoEnabled, repoListenPort, repoListenInterfaces, repoAdvertisedUrl,
                                repoStableEnabled, repoSoakDays, repoPrefixEnabled, repoPrefixEdit, repoTrustMode]() {
        RepoSettings next;
        next.revision = repo ? repo->revision : 1;
        next.enabled = repoEnabled->isChecked();
        next.listenHosts = selectedListenHosts(repoListenInterfaces);
        next.listenPort = repoListenPort->value();
        next.advertisedUrl = repoAdvertisedUrl->text().trimmed();
        next.stableEnabled = repoStableEnabled->isChecked();
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
        repoFieldsDirty = false;
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
    std::function<void(const QList<Registration> &, const QList<RemoteClient> &, const QString &)>
        applyClientsTables;
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
        pendingTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        pendingTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        pendingTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        pendingTable->verticalHeader()->setVisible(false);
        pendingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        pendingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        auto *clientsLabel = new QLabel(QStringLiteral("<b>Enrolled clients</b>"), clientsPage);
        clientsTable = new QTableWidget(0, 4, clientsPage);
        clientsTable->setHorizontalHeaderLabels(
            {QStringLiteral("Name"), QStringLiteral("Status"), QStringLiteral("Certificate"), QStringLiteral("")});
        clientsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        clientsTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        clientsTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
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
        applyClientsTables = [&](const QList<Registration> &pending,
                                 const QList<RemoteClient> &enrolled,
                                 const QString &error) {
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
            clientsError->setText(error);
            clientsError->setVisible(!error.isEmpty());
        };
        refreshClientsTables = [&, refreshClients] {
            clientsError->setText(QStringLiteral("Loading library clients…"));
            clientsError->setVisible(true);
            const auto config = library_.config();
            auto future = QtConcurrent::run([config] {
                LibraryClient client(config);
                ClientsRefreshResult result;
                result.registrations = client.pendingRegistrations(&result.error);
                QString clientError;
                result.clients = client.clients(&clientError);
                if (result.error.isEmpty()) result.error = clientError;
                return result;
            });
            watchFutureWithDisabledButton(
                refreshClients, &dialog, std::move(future), [&](const ClientsRefreshResult &result) {
                applyClientsTables(result.registrations, result.clients, result.error);
            });
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
    QObject::connect(keepInTray, &QCheckBox::toggled, &dialog, [&](bool) {
        if (!applyingClientFields) sessionDirty = true;
        refreshSessionControls();
    });
    QObject::connect(startAtLogin, &QCheckBox::toggled, &dialog, [&](bool) {
        if (!applyingClientFields) sessionDirty = true;
        refreshSessionControls();
    });
    QObject::connect(startMinimized, &QCheckBox::toggled, &dialog, [&](const bool checked) {
        if (!applyingClientFields) sessionDirty = true;
        if (checked) keepInTray->setChecked(true);
        refreshSessionControls();
    });
    QObject::connect(interfaceTheme, &QComboBox::currentIndexChanged, &dialog, [&](int) {
        if (!applyingClientFields) sessionDirty = true;
    });
    QObject::connect(trayTheme, &QComboBox::currentIndexChanged, &dialog, [&](int) {
        if (!applyingClientFields) sessionDirty = true;
    });
    refreshSessionControls();

    QObject::connect(this, &MainWindow::clientSettingsReloaded, &dialog, [&] {
        if (!sessionDirty) {
            const QScopedValueRollback applying(applyingClientFields, true);
            keepInTray->setChecked(appSettings_.updates.keepInTray ||
                                   appSettings_.updates.startMinimized);
            startAtLogin->setChecked(appSettings_.updates.startAtLogin);
            startMinimized->setChecked(appSettings_.updates.startMinimized);
            interfaceTheme->setCurrentIndex(
                interfaceTheme->findData(appearanceModeName(appSettings_.appearance.interfaceTheme)));
            trayTheme->setCurrentIndex(
                trayTheme->findData(appearanceModeName(appSettings_.appearance.trayTheme)));
            refreshSessionControls();
        }
        if (!harnessDirty) {
            replaceHarnessProfiles();
            externalHarnessNotice->setText(
                QStringLiteral("✓ Harness profiles updated by another PacSmith client."));
            externalHarnessNotice->setVisible(true);
            return;
        }
        externalHarnessNotice->setText(
            QStringLiteral("⚠ Harness profiles changed externally. Reload them or save your current edits."));
        externalHarnessNotice->setVisible(true);
        reloadExternalHarnesses->setVisible(true);
    });

    auto refreshScheduleControls = [&] {
        const bool periodic = backgroundEnabled->isChecked();
        schedule->setEnabled(periodic);
        weekday->setEnabled(periodic && schedule->currentIndex() == 1);
        checkTime->setEnabled(periodic);
    };
    QObject::connect(schedule, &QComboBox::currentIndexChanged, &dialog, [&](int) { refreshScheduleControls(); });
    QObject::connect(backgroundEnabled, &QCheckBox::toggled, &dialog, [&](bool) { refreshScheduleControls(); });
    QObject::connect(checkNow, &QPushButton::clicked, &dialog, [&] {
        if (GuiInstanceServer::requestCheck()) {
            checkNow->setEnabled(false);
            serviceStatus->setText(QStringLiteral("Requesting update check from the library daemon…"));
        } else {
            serviceStatus->setText(QStringLiteral("⚠ Could not reach the running PacSmith session to start a check."));
        }
    });
    QObject::connect(this, &MainWindow::updateCheckActivityChanged, &dialog,
                     [checkNow, serviceStatus](const QString &message, const bool active,
                                               const bool failed) {
        checkNow->setEnabled(!active);
        serviceStatus->setText(failed ? QStringLiteral("⚠ %1").arg(message)
                                      : active ? QStringLiteral("↻ %1").arg(message)
                                               : QStringLiteral("✓ %1").arg(message));
    });
    refreshScheduleControls();

    auto *serverSettingsRefresh = new QTimer(&dialog);
    serverSettingsRefresh->setSingleShot(true);
    serverSettingsRefresh->setInterval(75);
    bool settingsRefreshRunning = false;
    bool settingsRefreshAgain = false;
    bool settingsInitialLoad = true;
    QObject::connect(this, &MainWindow::serverTopicsChanged, &dialog,
                     [serverSettingsRefresh](const QStringList &topics) {
        if (topics.contains(QStringLiteral("all")) ||
            topics.contains(QStringLiteral("settings")) ||
            topics.contains(QStringLiteral("repository")) ||
            topics.contains(QStringLiteral("credentials")) ||
            topics.contains(QStringLiteral("administration"))) {
            serverSettingsRefresh->start();
        }
    });
    QObject::connect(serverSettingsRefresh, &QTimer::timeout, &dialog, [&] {
        if (settingsRefreshRunning) {
            settingsRefreshAgain = true;
            return;
        }
        settingsRefreshRunning = true;
        auto *watcher = new QFutureWatcher<SettingsRefreshResult>(&dialog);
        QObject::connect(watcher, &QFutureWatcher<SettingsRefreshResult>::finished,
                         &dialog, [&, watcher] {
        const auto latest = watcher->result();
        watcher->deleteLater();
        settingsRefreshRunning = false;
        if (latest.server) {
            info = latest.server;
            infoError = latest.administrationError;
        }
        bool updated = false;
        bool conflicted = false;
        if (latest.library &&
            (settingsInitialLoad || latest.library->revision != librarySettingsRevision_)) {
            if (libraryFieldsDirty) {
                conflicted = true;
            } else {
                const QScopedValueRollback applying(applyingLibraryFields, true);
                library = latest.library;
                applyLibrarySettings(*latest.library);
                backgroundEnabled->setChecked(latest.library->updatesEnabled);
                schedule->setCurrentIndex(latest.library->updatesDaily ? 0 : 1);
                weekday->setCurrentIndex(std::clamp(latest.library->weekDay, 1, 7) - 1);
                checkTime->setTime(latest.library->localTime);
                automaticPrepare->setChecked(latest.library->automaticallyPrepare);
                retentionVersions->setValue(latest.library->retentionVersions);
                buildParallelism->setRange(1, latest.library->availableBuildCores);
                buildParallelism->setValue(latest.library->buildParallelism);
                availableBuildCores->setText(
                    latest.library->availableBuildCores == 1
                        ? QStringLiteral("1 logical core")
                        : QStringLiteral("%1 logical cores")
                              .arg(latest.library->availableBuildCores));
                refreshScheduleControls();
                updated = true;
            }
        }
        if (latest.repo && (!repo || latest.repo->revision != repo->revision)) {
            if (repoFieldsDirty) {
                conflicted = true;
            } else {
                repo = latest.repo;
                applyRepoUi(*latest.repo);
                updated = true;
            }
        }
        if (latest.githubCredential &&
            latest.githubCredential->configured != appSettings_.githubTokenConfigured) {
            appSettings_.githubTokenConfigured = latest.githubCredential->configured;
            githubToken->setPlaceholderText(
                latest.githubCredential->configured
                    ? QStringLiteral("Configured on the library daemon; leave blank to keep it")
                    : QStringLiteral("Optional personal access token"));
            updated = true;
        }
        if (latest.server && listenEnabled != nullptr && listenPort != nullptr &&
            listenInterfaces != nullptr &&
            !sameListenTarget(latest.server->listen, lastAppliedListen)) {
            ListenSettings edited;
            edited.enabled = listenEnabled->isChecked();
            edited.port = listenPort->value();
            edited.hosts = selectedListenHosts(listenInterfaces);
            if (!sameListenTarget(edited, lastAppliedListen)) {
                conflicted = true;
            } else {
                lastAppliedListen = latest.server->listen;
                listenEnabled->setChecked(lastAppliedListen.enabled);
                listenPort->setValue(lastAppliedListen.port);
                populateListenInterfaces(listenInterfaces, lastAppliedListen.hosts);
                updateListenVisibility(lastAppliedListen);
                updated = true;
            }
        }
        if (latest.server && backendLabel != nullptr) {
            backendLabel->setText(secretBackendLabel(latest.server->secretBackend));
        }
        if (applyClientsTables && latest.server) {
            applyClientsTables(latest.registrations, latest.clients,
                               latest.administrationError);
        }
        if (conflicted) {
            settingsSyncNotice->setText(
                QStringLiteral("⚠ Library settings changed through another client while this dialog has unsaved edits. Cancel and reopen before making further changes here."));
            settingsSyncNotice->setVisible(true);
        } else if (updated) {
            settingsSyncNotice->setText(
                QStringLiteral("✓ Settings updated by another PacSmith client."));
            settingsSyncNotice->setVisible(true);
        } else if (settingsInitialLoad) {
            const auto loadError = !latest.libraryError.isEmpty()
                ? latest.libraryError
                : !latest.repositoryError.isEmpty() ? latest.repositoryError
                                                    : latest.credentialError;
            settingsSyncNotice->setText(loadError.isEmpty()
                                            ? QString{}
                                            : QStringLiteral("Could not load all library settings: %1")
                                                  .arg(loadError));
            settingsSyncNotice->setVisible(!loadError.isEmpty());
        }
        if (settingsInitialLoad) settingsTabs->setEnabled(true);
        settingsInitialLoad = false;
        if (settingsRefreshAgain) {
            settingsRefreshAgain = false;
            serverSettingsRefresh->start();
        }
        });
        const auto config = library_.config();
        watcher->setFuture(QtConcurrent::run([config, localAdmin] {
            LibraryClient client(config);
            SettingsRefreshResult result;
            result.library = client.librarySettings(&result.libraryError);
            result.repo = client.repoSettings(&result.repositoryError);
            result.githubCredential = client.credentialStatus(
                QStringLiteral("github.token"), &result.credentialError);
            if (localAdmin) {
                QString error;
                result.server = client.serverInfo(&error);
                result.registrations = client.pendingRegistrations(&error);
                QString clientError;
                result.clients = client.clients(&clientError);
                result.administrationError = error.isEmpty() ? clientError : error;
            }
            return result;
        }));
    });

    serverSettingsRefresh->start();

    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, this] {
        if (!applyListenSettings()) return;
        commitHarness();
        for (const auto &profile : harnessProfiles) {
            if (profile.name.isEmpty() || profile.executable.isEmpty()) {
                QMessageBox::warning(&dialog, QStringLiteral("Incomplete harness profile"),
                                     QStringLiteral("Every external harness profile needs a name and executable."));
                return;
            }
        }
        bool foundDefault = false;
        for (auto &profile : harnessProfiles) {
            if (!profile.isDefault) continue;
            if (foundDefault) profile.isDefault = false;
            else foundDefault = true;
        }
        if (!foundDefault && !harnessProfiles.isEmpty()) harnessProfiles.first().isDefault = true;
        if (!githubToken->text().isEmpty()) {
            QString error;
            if (!library_.setCredential(QStringLiteral("github.token"), githubToken->text(), &error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not store GitHub token"), error);
                return;
            }
            appSettings_.githubTokenConfigured = true;
        }

        LibrarySettings next;
        next.revision = librarySettingsRevision_;
        next.updatesEnabled = backgroundEnabled->isChecked();
        next.updatesDaily = schedule->currentIndex() == 0;
        next.weekDay = weekday->currentIndex() + 1;
        next.localTime = checkTime->time();
        next.automaticallyPrepare = automaticPrepare->isChecked();
        next.retentionVersions = retentionVersions->value();
        next.buildParallelism = buildParallelism->value();
        QString error;
        auto saved = library_.saveLibrarySettings(next, &error);
        if (!saved) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not save library settings"), error);
            return;
        }
        applyLibrarySettings(*saved);

        if (!saveCollectedRepo(collectRepoSettings())) return;

        appSettings_.updates.startAtLogin = startAtLogin->isChecked();
        appSettings_.updates.startMinimized = startMinimized->isChecked();
        appSettings_.updates.keepInTray = keepInTray->isChecked();
        appSettings_.appearance.interfaceTheme = selectedAppearanceMode(interfaceTheme);
        appSettings_.appearance.trayTheme = selectedAppearanceMode(trayTheme);
        appSettings_.harnessProfiles = harnessProfiles;
        if (!settingsStore_.save(appSettings_, &error) ||
            !BackgroundUpdateManager::apply(appSettings_.updates, QCoreApplication::applicationFilePath(), &error)) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not save this machine's session settings"), error);
            return;
        }
        const bool runInTray = appSettings_.updates.keepInTray && QSystemTrayIcon::isSystemTrayAvailable();
        applyInterfaceTheme(appSettings_.appearance.interfaceTheme);
        setKeepRunningInTray(runInTray);
        QApplication::setQuitOnLastWindowClosed(!runInTray);
        static_cast<void>(GuiInstanceServer::requestTray());
        dialog.accept();
    });
    reloadClientSettings();
    if (!harnessDirty) replaceHarnessProfiles();
    dialog.exec();
}

} // namespace pacsmith::gui
