#include "gui/main_window/common.hpp"

namespace pacsmith::gui {

void MainWindow::showSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("PacSmith Settings"));
    dialog.setMinimumSize(680, 620);
    auto *rootLayout = new QVBoxLayout(&dialog);
    auto *settingsTabs = new QTabWidget(&dialog);

    auto *generalPage = new QWidget(settingsTabs);
    auto *generalLayout = new QVBoxLayout(generalPage);
    auto *sessionGroup = new QGroupBox(QStringLiteral("Session"), generalPage);
    auto *sessionLayout = new QVBoxLayout(sessionGroup);
    sessionLayout->addWidget(settingsSectionHelp(
        sessionGroup,
        QStringLiteral("Closing the window quits PacSmith unless it stays in the tray."),
        QStringLiteral("Closing the main window quits PacSmith unless it is kept running in the tray. "
                       "Open it again from the tray icon, or choose Quit there to exit completely.\n\n"
                       "Start at login can also start PacSmith minimized to the tray.")));
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

    auto *credentialSource = new QComboBox(generalPage);
    credentialSource->addItems({QStringLiteral("Process environment (read-only)"),
                                QStringLiteral("Desktop keyring"),
                                QStringLiteral("Age-encrypted file")});
    credentialSource->setPlaceholderText(QStringLiteral("Choose how PacSmith stores secrets…"));
    credentialSource->setCurrentIndex(-1);
    const auto configuredSource = [&]() -> std::optional<CredentialSource> {
        if (aiSettings_.provider != AiProviderKind::None) {
            const auto providerSource = aiSettings_.credentialSources.constFind(
                aiProviderName(aiSettings_.provider));
            if (providerSource != aiSettings_.credentialSources.cend()) return providerSource.value();
        }
        const auto githubSource = aiSettings_.credentialSources.constFind(QStringLiteral("github"));
        if (githubSource != aiSettings_.credentialSources.cend()) return githubSource.value();
        if (!aiSettings_.credentialSources.isEmpty()) return aiSettings_.credentialSources.first();
        return std::nullopt;
    }();
    if (configuredSource) credentialSource->setCurrentIndex(static_cast<int>(*configuredSource));
    auto *ageStoreAction = new QPushButton(generalPage);
    auto *storeNotice = new QLabel(generalPage);
    storeNotice->setWordWrap(true);
    auto *secretsGroup = new QGroupBox(QStringLiteral("Secret store"), generalPage);
    auto *secretsForm = new QFormLayout(secretsGroup);
    secretsForm->addRow(settingsSectionHelp(
        secretsGroup,
        QStringLiteral("Choose a store before PacSmith will accept a GitHub token or AI credentials."),
        QStringLiteral("The same store is used for GitHub tokens and AI credentials.\n\n"
                       "Process environment is read-only and must be set before PacSmith starts. Desktop launches "
                       "usually do not inherit variables set in a terminal.\n\n"
                       "Desktop keyring stores secrets in your session keyring.\n\n"
                       "Age-encrypted file stores secrets in a password-protected file.")));
    secretsForm->addRow(QStringLiteral("Storage"), credentialSource);
    secretsForm->addRow(QString{}, ageStoreAction);
    secretsForm->addRow(storeNotice);
    generalLayout->addWidget(secretsGroup);

    auto *githubGroup = new QGroupBox(QStringLiteral("GitHub"), generalPage);
    auto *githubForm = new QFormLayout(githubGroup);
    auto *githubToken = new QLineEdit(githubGroup);
    githubToken->setEchoMode(QLineEdit::Password);
    githubToken->setPlaceholderText(QStringLiteral("Optional; leave blank to keep a saved token"));
    auto *githubLockNotice = new QLabel(githubGroup);
    githubLockNotice->setWordWrap(true);
    githubForm->addRow(settingsSectionHelp(
        githubGroup,
        QStringLiteral("Optional. Used when adding GitHub packages and checking for updates."),
        QStringLiteral("A token is optional for public repositories but raises GitHub API rate limits. "
                       "PacSmith uses it when adding packages from GitHub URLs and when checking for updates.\n\n"
                       "Leave the field blank to keep a saved token. Environment storage reads "
                       "PACSMITH_GITHUB_TOKEN when PacSmith starts and never displays its value.")));
    githubForm->addRow(QStringLiteral("Personal access token"), githubToken);
    githubForm->addRow(githubLockNotice);
    generalLayout->addWidget(githubGroup);
    generalLayout->addStretch(1);
    settingsTabs->addTab(generalPage, QStringLiteral("General"));

    auto *aiPage = new QWidget(settingsTabs);
    auto *layout = new QVBoxLayout(aiPage);
    settingsTabs->addTab(aiPage, QStringLiteral("AI Advisor"));
    rootLayout->addWidget(settingsTabs, 1);
    auto *aiGroup = new QGroupBox(QStringLiteral("AI Advisor"), aiPage);
    auto *aiGroupLayout = new QVBoxLayout(aiGroup);
    auto *aiSectionHelp = settingsSectionHelp(
        aiGroup,
        QStringLiteral("AI is optional. Local inspection always runs first."),
        QStringLiteral("PacSmith always performs local deterministic inspection first and sends only a bounded "
                       "package-evidence bundle. Package binaries and unrelated files are never sent."));
    aiGroupLayout->addWidget(aiSectionHelp);
    auto *form = new QFormLayout;
    auto *provider = new QComboBox(aiPage);
    provider->addItems({QStringLiteral("None"), QStringLiteral("ChatGPT subscription"),
                        QStringLiteral("OpenAI API"),
                        QStringLiteral("xAI / Grok API")});
    provider->setCurrentIndex(static_cast<int>(aiSettings_.provider));
    if (!configuredSource && aiSettings_.provider != AiProviderKind::None) {
        provider->setCurrentIndex(static_cast<int>(AiProviderKind::None));
    }
    auto *model = new QComboBox(aiPage);
    model->setEditable(true);
    model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    model->setMinimumContentsLength(36);
    model->lineEdit()->setPlaceholderText(QStringLiteral("Provider model ID"));
    model->setEditText(aiSettings_.model);
    auto *reasoningEffort = new QComboBox(aiPage);
    auto *executionMode = new QComboBox(aiPage);
    executionMode->addItem(QStringLiteral("Standard"),
                           static_cast<int>(AiExecutionMode::Standard));
    executionMode->addItem(QStringLiteral("Fast (priority)"),
                           static_cast<int>(AiExecutionMode::Fast));
    executionMode->setCurrentIndex(
        executionMode->findData(static_cast<int>(aiSettings_.executionMode)));
    auto *automatic = new QCheckBox(QStringLiteral("Automatically resolve items flagged for review with AI"), aiPage);
    automatic->setChecked(aiSettings_.automaticallyResolveReviewItems);
    auto *secretsHint = new QLabel(aiPage);
    secretsHint->setWordWrap(true);
    auto *apiKey = new QLineEdit(aiPage);
    apiKey->setEchoMode(QLineEdit::Password);
    apiKey->setPlaceholderText(QStringLiteral("Leave blank to keep an existing stored key"));
    auto *aiEnvironmentNotice = new QLabel(aiPage);
    aiEnvironmentNotice->setWordWrap(true);
    aiEnvironmentNotice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    aiEnvironmentNotice->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *chatGptSignIn = new QPushButton(QStringLiteral("Sign in with ChatGPT…"), aiPage);
    auto *loadModels = new QPushButton(QStringLiteral("Load Available Models"), aiPage);
    form->addRow(secretsHint);
    form->addRow(QStringLiteral("Provider"), provider);
    form->addRow(QStringLiteral("API key"), apiKey);
    form->addRow(QString{}, chatGptSignIn);
    form->addRow(aiEnvironmentNotice);
    form->addRow(QStringLiteral("Model"), model);
    form->addRow(QString{}, loadModels);
    form->addRow(QStringLiteral("Reasoning effort"), reasoningEffort);
    form->addRow(QStringLiteral("Execution speed"), executionMode);
    form->addRow(QString{}, automatic);
    aiGroupLayout->addLayout(form);
    layout->addWidget(aiGroup);
    layout->addStretch(1);
    auto *aiStatusPanel = settingsStatusFrame(aiPage);
    auto *aiStatusLayout = new QVBoxLayout(aiStatusPanel);
    aiStatusLayout->setContentsMargins(12, 8, 12, 9);
    aiStatusLayout->setSpacing(4);
    auto *aiStatusTitle = new QLabel(QStringLiteral("●  STATUS"), aiStatusPanel);
    aiStatusTitle->setStyleSheet(QStringLiteral("color: #3498db; font-weight: 600;"));
    auto *aiCredentialStatus = new QLabel(aiStatusPanel);
    aiCredentialStatus->setWordWrap(true);
    aiCredentialStatus->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    aiCredentialStatus->setMinimumHeight(42);
    aiCredentialStatus->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    aiCredentialStatus->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    aiStatusLayout->addWidget(aiStatusTitle);
    aiStatusLayout->addWidget(aiCredentialStatus);
    layout->addWidget(aiStatusPanel);

    auto *updatesPage = new QWidget(settingsTabs);
    auto *updatesLayout = new QVBoxLayout(updatesPage);
    auto *scheduleGroup = new QGroupBox(QStringLiteral("Updates"), updatesPage);
    auto *updatesForm = new QFormLayout(scheduleGroup);
    updatesForm->addRow(settingsSectionHelp(
        scheduleGroup,
        QStringLiteral("Checks run while PacSmith is open, including in the tray."),
        QStringLiteral("Periodic checks run while PacSmith is open, including while it is kept in the tray. "
                       "Each project release owns its own update source. The installed known release is the "
                       "active tracker; before installation, the newest analyzed release is. Checks pause when "
                       "an external installed version cannot be matched safely.")));
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
    behaviorForm->addRow(settingsSectionHelp(
        behaviorGroup,
        QStringLiteral("Newly discovered vendor artifacts can be downloaded and prepared automatically."),
        QStringLiteral("APT artifacts require their signed repository SHA256. GitHub artifacts use the publisher "
                       "digest when available and otherwise remain visibly unsigned. Every artifact is re-inspected; "
                       "unchanged fingerprinted decisions may carry forward.")));
    auto *automaticPrepare = new QCheckBox(
        QStringLiteral("Download and prepare newly discovered vendor artifacts automatically"), behaviorGroup);
    automaticPrepare->setChecked(aiSettings_.updates.automaticallyPrepare);
    behaviorForm->addRow(QString{}, automaticPrepare);
    updatesLayout->addWidget(behaviorGroup);

    auto *checkStatusPanel = settingsStatusFrame(updatesPage);
    auto *checkStatusLayout = new QVBoxLayout(checkStatusPanel);
    checkStatusLayout->setContentsMargins(12, 8, 12, 9);
    checkStatusLayout->setSpacing(4);
    auto *checkStatusTitle = new QLabel(QStringLiteral("●  LAST CHECK"), checkStatusPanel);
    checkStatusTitle->setStyleSheet(QStringLiteral("color: #3498db; font-weight: 600;"));
    auto *lastCheckStatus = new QLabel(checkStatusPanel);
    lastCheckStatus->setWordWrap(true);
    lastCheckStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lastCheckStatus->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    checkStatusLayout->addWidget(checkStatusTitle);
    checkStatusLayout->addWidget(lastCheckStatus);
    updatesLayout->addWidget(checkStatusPanel);

    auto *serviceStatus = new QLabel(updatesPage);
    serviceStatus->setWordWrap(true);
    serviceStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *updateButtons = new QHBoxLayout;
    auto *checkNow = new QPushButton(QStringLiteral("Check Now"), updatesPage);
    updateButtons->addWidget(checkNow);
    updateButtons->addStretch();
    updatesLayout->addLayout(updateButtons);
    updatesLayout->addWidget(serviceStatus);
    updatesLayout->addStretch();
    settingsTabs->addTab(updatesPage, QStringLiteral("Updates"));

    auto *cleanupPage = new QWidget(settingsTabs);
    auto *cleanupLayout = new QVBoxLayout(cleanupPage);
    auto *cleanupGroup = new QGroupBox(QStringLiteral("Cleanup"), cleanupPage);
    auto *cleanupForm = new QFormLayout(cleanupGroup);
    cleanupForm->addRow(settingsSectionHelp(
        cleanupGroup,
        QStringLiteral("Older artifacts and complete releases are removed after update checks."),
        QStringLiteral("These counts apply to versions older than the currently installed PacSmith release. "
                       "Rolling back moves that retention anchor.\n\n"
                       "Complete-release retention cannot be lower than artifact retention. When automatic "
                       "download is enabled, unbuilt intermediate updates between the installed or last-built "
                       "version and the latest download are dropped entirely.")));
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

    auto *aboutPage = new QWidget(settingsTabs);
    auto *aboutLayout = new QVBoxLayout(aboutPage);
    auto *hero = new QLabel(aboutPage);
    QPixmap heroPixmap(QStringLiteral(":/pacsmith/icons/pacsmith-hero.png"));
    const auto heroSide = qRound(192.0 * dialog.devicePixelRatioF());
    auto heroScaled = heroPixmap.scaled(heroSide, heroSide, Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation);
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
    licenseView->setTabChangesFocus(true);
    QFile licenseFile(QStringLiteral(":/pacsmith/LICENSE"));
    if (licenseFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        licenseView->setPlainText(QString::fromUtf8(licenseFile.readAll()).trimmed());
    } else {
        licenseView->setPlainText(QStringLiteral(
            "MIT License\n\n"
            "Copyright (c) 2026 Arlen Anderson and contributors"));
    }
    aboutLayout->addWidget(hero);
    aboutLayout->addSpacing(12);
    aboutLayout->addWidget(aboutName);
    aboutLayout->addWidget(aboutVersion);
    aboutLayout->addSpacing(8);
    aboutLayout->addWidget(aboutSummary);
    aboutLayout->addSpacing(8);
    aboutLayout->addWidget(aboutLink);
    aboutLayout->addSpacing(16);
    aboutLayout->addWidget(licenseView, 1);
    settingsTabs->addTab(aboutPage, QStringLiteral("About"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto *saveButton = buttons->button(QDialogButtonBox::Save);
    rootLayout->addWidget(buttons);

    auto currentUpdateSettings = [&] {
        BackgroundUpdateSettings settings = aiSettings_.updates;
        settings.enabled = backgroundEnabled->isChecked();
        settings.startAtLogin = startAtLogin->isChecked();
        settings.startMinimized = startMinimized->isChecked();
        settings.keepInTray = keepInTray->isChecked();
        settings.daily = schedule->currentIndex() == 0;
        settings.weekDay = weekday->currentIndex() + 1;
        settings.localTime = checkTime->time();
        settings.automaticallyPrepare = automaticPrepare->isChecked();
        settings.retainedPackageVersions = retainedPackages->value();
        settings.retainedCompleteReleases = retainedPackages->value() < 0 ||
                                            retainedReleases->value() < 0
            ? -1 : std::max(retainedReleases->value(), retainedPackages->value());
        return settings;
    };
    auto refreshScheduleControls = [&] {
        const bool periodic = backgroundEnabled->isChecked();
        schedule->setEnabled(periodic);
        weekday->setEnabled(periodic && schedule->currentIndex() == 1);
        checkTime->setEnabled(periodic);
    };
    auto refreshCheckStatus = [&] {
        const auto state = BackgroundUpdateStateStore::load();
        const auto pending = currentUpdateSettings();
        QStringList lines;
        if (state.checking) {
            lines.append(QStringLiteral("Update check in progress…"));
        } else if (!state.lastRun.isValid()) {
            lines.append(QStringLiteral("Last check: never"));
        } else {
            lines.append(QStringLiteral("Last check: %1").arg(formatLocalDateTime(state.lastRun)));
            if (!state.message.isEmpty()) lines.append(state.message);
            if (pending.enabled && BackgroundUpdateManager::isOverdue(pending, state.lastRun)) {
                lines.append(QStringLiteral("This check is overdue."));
            }
        }
        if (pending.enabled) {
            lines.append(QStringLiteral("Next scheduled check: %1")
                             .arg(formatLocalDateTime(
                                 BackgroundUpdateManager::nextScheduledOccurrence(pending))));
        } else {
            lines.append(QStringLiteral("Automatic checks are disabled."));
        }
        lastCheckStatus->setText(lines.join(QLatin1Char('\n')));
    };
    auto persistBackgroundSettings = [&, this](QString *error) {
        aiSettings_.updates = currentUpdateSettings();
        if (credentialSource->currentIndex() >= 0) {
            const auto source = static_cast<CredentialSource>(credentialSource->currentIndex());
            aiSettings_.credentialSources.insert(QStringLiteral("github"), source);
            if (source == CredentialSource::Environment) {
                aiSettings_.githubTokenConfigured = false;
            } else if (!githubToken->text().isEmpty()) {
                if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() &&
                    !unlockAgeCredentials()) {
                    if (error != nullptr) *error = QStringLiteral("The age credential store is locked");
                    return false;
                }
                if (!credentialStore_.store(QStringLiteral("github"), source,
                                            githubToken->text(), {}, error)) {
                    return false;
                }
                aiSettings_.githubTokenConfigured = true;
            }
        }
        return settingsStore_.save(aiSettings_, error) &&
               BackgroundUpdateManager::apply(aiSettings_.updates,
                                              QCoreApplication::applicationFilePath(), error);
    };
    auto refreshSessionControls = [&] {
        const bool trayOk = QSystemTrayIcon::isSystemTrayAvailable();
        keepInTray->setEnabled(trayOk);
        if (!trayOk) {
            keepInTray->setChecked(false);
            startMinimized->setChecked(false);
        } else if (!keepInTray->isChecked()) {
            startMinimized->setChecked(false);
        }
        startMinimized->setEnabled(trayOk && keepInTray->isChecked() &&
                                   startAtLogin->isChecked());
    };
    connect(keepInTray, &QCheckBox::toggled, &dialog, [&](bool) { refreshSessionControls(); });
    connect(startAtLogin, &QCheckBox::toggled, &dialog, [&](bool) { refreshSessionControls(); });
    connect(startMinimized, &QCheckBox::toggled, &dialog, [&](const bool checked) {
        if (checked) keepInTray->setChecked(true);
        refreshSessionControls();
    });
    refreshSessionControls();
    connect(schedule, &QComboBox::currentIndexChanged, &dialog,
            [&](const int) {
                refreshScheduleControls();
                refreshCheckStatus();
            });
    connect(backgroundEnabled, &QCheckBox::toggled, &dialog, [&](bool) {
        refreshScheduleControls();
        refreshCheckStatus();
    });
    connect(weekday, &QComboBox::currentIndexChanged, &dialog, [&](int) { refreshCheckStatus(); });
    connect(checkTime, &QTimeEdit::timeChanged, &dialog, [&](const QTime &) { refreshCheckStatus(); });
    connect(retainedPackages, &QSpinBox::valueChanged, &dialog, [retainedReleases](const int value) {
        if (value < 0) {
            retainedReleases->setValue(-1);
        } else if (retainedReleases->value() >= 0 && retainedReleases->value() < value) {
            retainedReleases->setValue(value);
        }
    });
    connect(checkNow, &QPushButton::clicked, &dialog, [&] {
        if (GuiInstanceServer::requestCheck()) {
            serviceStatus->setText(QStringLiteral("✓ Update check started."));
        } else {
            serviceStatus->setText(QStringLiteral("⚠ Could not reach the running PacSmith session to start a check."));
        }
        refreshCheckStatus();
    });
    auto *checkStatusTimer = new QTimer(&dialog);
    checkStatusTimer->setInterval(2000);
    connect(checkStatusTimer, &QTimer::timeout, &dialog, refreshCheckStatus);
    checkStatusTimer->start();
    refreshScheduleControls();
    refreshCheckStatus();

    QHash<int, QString> modelSelections;
    modelSelections.insert(provider->currentIndex(), aiSettings_.model);
    int previousProvider = provider->currentIndex();
    std::optional<ChatGptCredentials> chatGptSession;
    QString chatGptSerialized;

    auto selectedCredentialSource = [&] {
        return static_cast<CredentialSource>(credentialSource->currentIndex());
    };
    auto loadChatGptSession = [&] {
        chatGptSession.reset();
        chatGptSerialized.clear();
        if (credentialSource->currentIndex() < 0) return;
        const auto source = selectedCredentialSource();
        if (source == CredentialSource::Environment) return;
        if (source == CredentialSource::Age && !credentialStore_.ageUnlocked()) return;
        QString error;
        const auto saved = credentialStore_.load(QStringLiteral("chatgpt"), source, &error);
        if (!saved) return;
        chatGptSerialized = *saved;
        chatGptSession = ChatGptCredentials::fromSerialized(chatGptSerialized, &error);
    };

    auto chatGptIdentity = [&] {
        if (!chatGptSession) return QStringLiteral("Not signed in");
        auto identity = chatGptSession->email.isEmpty()
                            ? QStringLiteral("ChatGPT account %1").arg(chatGptSession->accountId)
                            : chatGptSession->email;
        if (!chatGptSession->planType.isEmpty()) {
            identity += QStringLiteral(" · %1 plan").arg(chatGptSession->planType);
        }
        return QStringLiteral("✓ Signed in as %1").arg(identity);
    };

    auto setCredentialStatus = [&](const QString &text) {
        aiCredentialStatus->setText(text);
    };

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
        const auto desiredIndex = reasoningEffort->findData(
            static_cast<int>(desiredReasoningEffort));
        reasoningEffort->setCurrentIndex(desiredIndex >= 0 ? desiredIndex : 0);
        reasoningEffort->setToolTip(
            supported.isEmpty()
                ? QStringLiteral("PacSmith has no verified reasoning-effort controls for this model. The provider default will be used.")
                : QStringLiteral("Controls the model's reasoning.effort request option. Higher effort can increase latency and usage."));
        reasoningEffort->blockSignals(false);
    };

    auto updateControls = [&, this] {
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        const bool api = kind == AiProviderKind::OpenAi || kind == AiProviderKind::Xai;
        const bool subscription = kind == AiProviderKind::ChatGpt;
        const bool storageSelected = credentialSource->currentIndex() >= 0;
        const bool operationRunning = aiModelCatalogService_.isRunning() ||
                                      chatGptLoginService_.isRunning();

        provider->setEnabled(false);
        credentialSource->setEnabled(!operationRunning);
        apiKey->setVisible(api);
        if (auto *label = form->labelForField(apiKey); label != nullptr) label->setVisible(api);
        form->setRowVisible(secretsHint, !storageSelected);
        form->setRowVisible(apiKey, api);
        form->setRowVisible(chatGptSignIn, subscription);
        form->setRowVisible(aiEnvironmentNotice, false);
        secretsForm->setRowVisible(storeNotice, false);
        secretsForm->setRowVisible(ageStoreAction, false);
        githubForm->setRowVisible(githubLockNotice, false);
        ageStoreAction->setVisible(false);
        chatGptSignIn->setVisible(subscription);
        secretsHint->setVisible(!storageSelected);

        const auto aiHelpSummary = QStringLiteral("AI is optional. Local inspection always runs first.");
        auto aiHelpDetails = QStringLiteral(
            "PacSmith always performs local deterministic inspection first and sends only a bounded "
            "package-evidence bundle. Package binaries and unrelated files are never sent.\n\n"
            "Standard uses the provider's normal processing. Fast requests priority processing. "
            "API providers may charge premium per-token rates. ChatGPT availability and usage "
            "limits depend on the subscription.");
        setSettingsSectionHelp(aiSectionHelp, aiHelpSummary, aiHelpDetails);

        if (!storageSelected) {
            apiKey->setEnabled(false);
            ageStoreAction->setEnabled(false);
            chatGptSignIn->setEnabled(false);
            githubToken->setEnabled(false);
            model->setEnabled(false);
            loadModels->setEnabled(false);
            reasoningEffort->setEnabled(false);
            executionMode->setEnabled(false);
            automatic->setEnabled(false);
            saveButton->setEnabled(!operationRunning);
            secretsHint->setText(
                QStringLiteral("Choose how PacSmith stores secrets on the General tab before enabling AI."));
            githubLockNotice->setText(
                QStringLiteral("Choose a secret store above first."));
            githubForm->setRowVisible(githubLockNotice, true);
            setCredentialStatus(
                QStringLiteral("Choose a secret store on the General tab before enabling AI."));
            return;
        }

        const auto source = selectedCredentialSource();
        const bool sourceCompatible = !(subscription && source == CredentialSource::Environment);
        bool storageAvailable = true;
        QString storageError;
        if (source == CredentialSource::Keyring) {
            storageAvailable = CredentialStore::keyringAvailable(&storageError);
        } else if (source == CredentialSource::Age) {
            storageAvailable = CredentialStore::ageAvailable();
            if (!storageAvailable) storageError = QStringLiteral("/usr/bin/age is not installed");
        }
        const bool storagePrepared = storageAvailable &&
                                     (source != CredentialSource::Age ||
                                      credentialStore_.ageUnlocked());
        provider->setEnabled(storagePrepared && !operationRunning);
        if (source == CredentialSource::Age && !credentialStore_.ageUnlocked()) {
            ageStoreAction->setVisible(true);
            secretsForm->setRowVisible(ageStoreAction, true);
            ageStoreAction->setText(
                credentialStore_.hasAgeFile() ? QStringLiteral("Unlock Credential Store…")
                                              : QStringLiteral("Create Encrypted Store…"));
            ageStoreAction->setEnabled(storageAvailable && !operationRunning);
        }
        if (!storageAvailable) {
            storeNotice->setText(QStringLiteral("⚠ Credential storage unavailable: %1").arg(storageError));
            secretsForm->setRowVisible(storeNotice, true);
        }

        if (subscription) loadChatGptSession();
        bool apiCredentialAvailable = false;
        QString apiCredentialError;
        if (api && storagePrepared) {
            if (!apiKey->text().isEmpty()) {
                apiCredentialAvailable = true;
            } else if (source != CredentialSource::Age || credentialStore_.ageUnlocked()) {
                const auto stored = credentialStore_.load(aiProviderName(kind), source,
                                                          &apiCredentialError);
                apiCredentialAvailable = stored.has_value();
            }
        }
        const auto githubEnvironment = source == CredentialSource::Environment;
        githubToken->setEnabled(!githubEnvironment && storagePrepared && !operationRunning);
        githubToken->setPlaceholderText(
            githubEnvironment
                ? QStringLiteral("Read from PACSMITH_GITHUB_TOKEN when PacSmith starts; value is never displayed")
                : QStringLiteral("Optional; leave blank to keep a saved token"));
        if (!storagePrepared) {
            githubLockNotice->setText(QStringLiteral("Unlock or create the secret store first."));
            githubForm->setRowVisible(githubLockNotice, true);
        }
        if (source == CredentialSource::Environment && api) {
            const auto variable = kind == AiProviderKind::Xai ? QStringLiteral("XAI_API_KEY")
                                                               : QStringLiteral("OPENAI_API_KEY");
            apiKey->setPlaceholderText(
                QStringLiteral("Read from %1 when PacSmith starts; value is never displayed")
                    .arg(variable));
            apiKey->setToolTip(
                QStringLiteral("This is a read-only process credential. Set %1 before launching "
                               "pacsmith-gui. PacSmith intentionally does not reveal its value.")
                    .arg(variable));
            aiEnvironmentNotice->setVisible(true);
            form->setRowVisible(aiEnvironmentNotice, true);
            aiEnvironmentNotice->setText(
                apiCredentialAvailable
                    ? QStringLiteral("\u2713 %1 is available in PacSmith's process environment.")
                          .arg(variable)
                    : QStringLiteral("\u26a0 %1 was not found in PacSmith's process environment.")
                          .arg(variable));
        } else {
            apiKey->setPlaceholderText(QStringLiteral("Leave blank to keep an existing stored key"));
            apiKey->setToolTip({});
        }
        const bool credentialReady = subscription ? chatGptSession.has_value()
                                                  : api ? apiCredentialAvailable
                                                        : kind == AiProviderKind::None;
        const bool modelReady = kind != AiProviderKind::None && credentialReady &&
                                !model->currentText().trimmed().isEmpty();

        apiKey->setEnabled(api && source != CredentialSource::Environment && storagePrepared &&
                           !operationRunning);
        automatic->setEnabled(storagePrepared && !operationRunning);
        model->setEnabled(kind != AiProviderKind::None && credentialReady && !operationRunning);
        loadModels->setEnabled((api || subscription) && credentialReady && !operationRunning);
        reasoningEffort->setEnabled(modelReady && reasoningEffort->count() > 1 &&
                                    !operationRunning);
        executionMode->setEnabled(modelReady && !operationRunning);
        saveButton->setEnabled(
            !operationRunning && storagePrepared &&
            (kind == AiProviderKind::None ||
             (credentialReady && !model->currentText().trimmed().isEmpty())));
        chatGptSignIn->setText(chatGptSession ? QStringLiteral("Sign out of ChatGPT…")
                                              : QStringLiteral("Sign in with ChatGPT…"));
        chatGptSignIn->setEnabled(subscription && sourceCompatible && storagePrepared &&
                                  !operationRunning);

        if (subscription) {
            aiHelpDetails += QStringLiteral(
                "\n\nPacSmith opens OpenAI's browser sign-in and never receives your password. "
                "The OAuth session is stored only in PacSmith's selected credential store; "
                "PacSmith does not read Codex, OpenClaw, or any other application's files.");
        } else if (api) {
            aiHelpDetails += QStringLiteral(
                "\n\nEnter or provide the selected API provider's credential before model selection is unlocked.");
            if (source == CredentialSource::Environment) {
                const auto variable = kind == AiProviderKind::Xai ? QStringLiteral("XAI_API_KEY")
                                                                   : QStringLiteral("OPENAI_API_KEY");
                aiHelpDetails += QStringLiteral(
                    "\n\n%1 is read from the process environment when PacSmith starts. "
                    "Desktop launches usually do not inherit variables set in a terminal.")
                                    .arg(variable);
            }
        }
        setSettingsSectionHelp(aiSectionHelp, aiHelpSummary, aiHelpDetails);

        if (!storageAvailable) {
            setCredentialStatus(
                QStringLiteral("⚠ Credential storage unavailable: %1").arg(storageError));
        } else if (!storagePrepared) {
            setCredentialStatus(
                credentialStore_.hasAgeFile()
                    ? QStringLiteral("Unlock PacSmith's encrypted credential store on the General tab before entering secrets.")
                    : QStringLiteral("Create PacSmith's encrypted credential store on the General tab before entering secrets."));
        } else if (kind == AiProviderKind::None) {
            setCredentialStatus(QStringLiteral("Secret store is ready. Choose an AI provider or save."));
        } else if (!sourceCompatible) {
            setCredentialStatus(
                QStringLiteral("⚠ ChatGPT browser login cannot use environment-variable storage. Choose Desktop keyring or Age-encrypted file."));
        } else if (credentialReady && subscription) {
            setCredentialStatus(chatGptIdentity() +
                                      QStringLiteral(". Model selection is unlocked."));
        } else if (credentialReady) {
            setCredentialStatus(
                QStringLiteral("✓ Provider credential is available. Model selection is unlocked."));
        } else if (subscription) {
            setCredentialStatus(
                QStringLiteral("Sign in with ChatGPT before selecting a model."));
        } else if (source == CredentialSource::Environment) {
            setCredentialStatus(
                QStringLiteral("Waiting for %1 before model selection can be unlocked.")
                    .arg(kind == AiProviderKind::Xai ? QStringLiteral("XAI_API_KEY")
                                                     : QStringLiteral("OPENAI_API_KEY")));
        } else {
            setCredentialStatus(
                apiCredentialError.isEmpty()
                    ? QStringLiteral("Enter an API key before selecting a model.")
                    : QStringLiteral("Enter an API key before selecting a model. %1")
                          .arg(apiCredentialError));
        }
    };
    connect(model, &QComboBox::currentTextChanged, &dialog, [&](const QString &) {
        modelSelections.insert(provider->currentIndex(), model->currentText().trimmed());
        refreshReasoningEfforts();
        updateControls();
    });
    connect(reasoningEffort, &QComboBox::currentIndexChanged, &dialog, [&](const int index) {
        if (index >= 0) {
            desiredReasoningEffort = static_cast<AiReasoningEffort>(
                reasoningEffort->itemData(index).toInt());
        }
    });
    connect(executionMode, &QComboBox::currentIndexChanged, &dialog,
            [&](const int) { updateControls(); });
    connect(provider, &QComboBox::currentIndexChanged, &dialog, [&](const int index) {
        modelSelections.insert(previousProvider, model->currentText().trimmed());
        previousProvider = index;
        model->clear();
        model->setEditText(modelSelections.value(index));
        refreshReasoningEfforts();
        updateControls();
    });
    connect(credentialSource, &QComboBox::currentIndexChanged, &dialog, updateControls);
    connect(apiKey, &QLineEdit::textChanged, &dialog,
            [&](const QString &) { updateControls(); });
    connect(ageStoreAction, &QPushButton::clicked, &dialog, [&, this] {
        if (!unlockAgeCredentials()) return;
        loadChatGptSession();
        updateControls();
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        if (chatGptSession) {
            setCredentialStatus(chatGptIdentity() + QStringLiteral(". Model selection is unlocked."));
        } else if (kind == AiProviderKind::ChatGpt) {
            setCredentialStatus(
                QStringLiteral("✓ Encrypted credential store is ready. Sign in with ChatGPT next."));
        } else if (kind == AiProviderKind::OpenAi || kind == AiProviderKind::Xai) {
            setCredentialStatus(
                QStringLiteral("✓ Encrypted credential store is ready. Enter the API key next."));
        } else {
            setCredentialStatus(
                QStringLiteral("✓ Encrypted credential store is ready. Choose an AI provider next."));
        }
    });
    connect(chatGptSignIn, &QPushButton::clicked, &dialog, [&, this] {
        if (chatGptSession) {
            if (QMessageBox::question(&dialog, QStringLiteral("Sign out of ChatGPT"),
                                      QStringLiteral("Remove PacSmith's saved ChatGPT session?")) !=
                QMessageBox::Yes) {
                return;
            }
            const auto source = selectedCredentialSource();
            if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() &&
                !unlockAgeCredentials()) {
                return;
            }
            QString error;
            if (!credentialStore_.remove(QStringLiteral("chatgpt"), source,
                                         QString{},
                                         &error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not sign out"), error);
                return;
            }
            chatGptSession.reset();
            chatGptSerialized.clear();
            updateControls();
            return;
        }

        const auto source = selectedCredentialSource();
        if (source == CredentialSource::Environment) {
            setCredentialStatus(
                QStringLiteral("ChatGPT sessions must be stored in PacSmith's keyring or age file"));
            return;
        }
        if (source == CredentialSource::Age && !credentialStore_.ageUnlocked()) {
            if (!unlockAgeCredentials()) return;
            loadChatGptSession();
            updateControls();
            if (chatGptSession) {
                setCredentialStatus(
                    chatGptIdentity() + QStringLiteral(". Loading available models…"));
                aiModelCatalogService_.fetch(AiProviderKind::ChatGpt, chatGptSerialized);
                return;
            }
        }
        setCredentialStatus(QStringLiteral("Starting secure ChatGPT browser sign-in…"));
        chatGptSignIn->setEnabled(false);
        chatGptLoginService_.start();
    });
    connect(&chatGptLoginService_, &ChatGptLoginService::authorizationUrlReady, &dialog,
            [&, this](const QUrl &url) {
        if (!QDesktopServices::openUrl(url)) {
            setCredentialStatus(
                QStringLiteral("Open this OpenAI sign-in URL in your browser: %1").arg(url.toString()));
            aiCredentialStatus->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
        }
    });
    connect(&chatGptLoginService_, &ChatGptLoginService::progressChanged, &dialog,
            [&](const QString &message) {
        updateControls();
        setCredentialStatus(message);
    });
    connect(&chatGptLoginService_, &ChatGptLoginService::failed, &dialog,
            [&, this](const QString &message) {
        updateControls();
        setCredentialStatus(QStringLiteral("⚠ %1").arg(message));
    });
    connect(&chatGptLoginService_, &ChatGptLoginService::succeeded, &dialog,
            [&, this](const QString &serialized) {
        const auto source = selectedCredentialSource();
        QString error;
        if (!credentialStore_.store(QStringLiteral("chatgpt"), source, serialized,
                                    QString{}, &error)) {
            setCredentialStatus(
                QStringLiteral("⚠ Signed in, but PacSmith could not save the session: %1").arg(error));
            return;
        }
        chatGptSerialized = serialized;
        chatGptSession = ChatGptCredentials::fromSerialized(serialized, &error);
        if (!chatGptSession) {
            setCredentialStatus(QStringLiteral("⚠ %1").arg(error));
            return;
        }
        updateControls();
        aiModelCatalogService_.fetch(AiProviderKind::ChatGpt, chatGptSerialized);
    });
    connect(loadModels, &QPushButton::clicked, &dialog, [&, this] {
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        if (kind == AiProviderKind::None) return;
        const auto name = aiProviderName(kind);
        const auto source = static_cast<CredentialSource>(credentialSource->currentIndex());
        QString credential;
        if (kind == AiProviderKind::ChatGpt && chatGptSession) {
            credential = chatGptSerialized;
        } else if (source != CredentialSource::Environment && !apiKey->text().isEmpty()) {
            credential = apiKey->text();
        } else {
            if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() &&
                !unlockAgeCredentials()) {
                return;
            }
            QString error;
            const auto existing = credentialStore_.load(name, source, &error);
            if (!existing) {
                setCredentialStatus(QStringLiteral("⚠ %1").arg(error));
                return;
            }
            credential = *existing;
        }
        aiModelCatalogService_.fetch(kind, credential);
        credential.fill(QChar::Null);
    });
    connect(&aiModelCatalogService_, &AiModelCatalogService::progressChanged, &dialog,
            [&](const QString &message) {
                updateControls();
                setCredentialStatus(message);
            });
    connect(&aiModelCatalogService_, &AiModelCatalogService::credentialUpdated, &dialog,
            [&, this](const QString &serialized) {
        const auto source = selectedCredentialSource();
        QString error;
        if (credentialStore_.store(QStringLiteral("chatgpt"), source, serialized,
                                   QString{}, &error)) {
            chatGptSerialized = serialized;
            chatGptSession = ChatGptCredentials::fromSerialized(serialized);
        } else {
            setCredentialStatus(
                QStringLiteral("⚠ Could not save the refreshed ChatGPT session: %1").arg(error));
        }
    });
    connect(&aiModelCatalogService_, &AiModelCatalogService::finished, &dialog,
            [&](const QStringList &models) {
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
                setCredentialStatus(
                    QStringLiteral("✓ Loaded %1 model(s) directly from %2")
                        .arg(models.size()).arg(aiProviderName(
                            static_cast<AiProviderKind>(provider->currentIndex()))));
            });
    connect(&aiModelCatalogService_, &AiModelCatalogService::failed, &dialog,
            [&](const QString &message) {
                updateControls();
                setCredentialStatus(QStringLiteral("⚠ %1").arg(message));
            });
    connect(&dialog, &QDialog::finished, &aiModelCatalogService_, &AiModelCatalogService::cancel);
    connect(&dialog, &QDialog::finished, &chatGptLoginService_, &ChatGptLoginService::cancel);
    refreshReasoningEfforts();
    updateControls();

    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        const auto name = aiProviderName(kind);
        if (credentialSource->currentIndex() < 0) {
            aiSettings_.provider = AiProviderKind::None;
            aiSettings_.model.clear();
            aiSettings_.reasoningEffort = AiReasoningEffort::ProviderDefault;
            aiSettings_.executionMode = AiExecutionMode::Standard;
            aiSettings_.automaticallyResolveReviewItems = automatic->isChecked();
            QString error;
            if (!persistBackgroundSettings(&error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not save settings"), error);
                return;
            }
            const bool runInTray = aiSettings_.updates.keepInTray &&
                                   QSystemTrayIcon::isSystemTrayAvailable();
            setKeepRunningInTray(runInTray);
            QApplication::setQuitOnLastWindowClosed(!runInTray);
            static_cast<void>(GuiInstanceServer::requestTray());
            dialog.accept();
            return;
        }
        const auto source = static_cast<CredentialSource>(credentialSource->currentIndex());
        if ((kind == AiProviderKind::OpenAi || kind == AiProviderKind::Xai) &&
            source != CredentialSource::Environment && !apiKey->text().isEmpty()) {
            if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() &&
                !unlockAgeCredentials()) {
                return;
            }
            QString error;
            if (!credentialStore_.store(name, source, apiKey->text(), {}, &error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not store API key"), error);
                return;
            }
        }
        if (kind == AiProviderKind::ChatGpt && !chatGptSession) {
            QMessageBox::warning(&dialog, QStringLiteral("ChatGPT sign-in required"),
                                 QStringLiteral("Sign in to ChatGPT before selecting the subscription provider."));
            return;
        }
        aiSettings_.provider = kind;
        aiSettings_.model = kind == AiProviderKind::None ? QString{} : model->currentText().trimmed();
        aiSettings_.reasoningEffort = kind == AiProviderKind::None
                                          ? AiReasoningEffort::ProviderDefault
                                          : static_cast<AiReasoningEffort>(
                                                reasoningEffort->currentData().toInt());
        aiSettings_.executionMode = kind == AiProviderKind::None
                                        ? AiExecutionMode::Standard
                                        : static_cast<AiExecutionMode>(
                                              executionMode->currentData().toInt());
        aiSettings_.automaticallyResolveReviewItems = automatic->isChecked();
        if (kind == AiProviderKind::ChatGpt || kind == AiProviderKind::OpenAi ||
            kind == AiProviderKind::Xai) {
            aiSettings_.credentialSources.insert(name, source);
        }
        QString error;
        if (!persistBackgroundSettings(&error)) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not save settings"), error);
            return;
        }
        const bool runInTray = aiSettings_.updates.keepInTray &&
                               QSystemTrayIcon::isSystemTrayAvailable();
        setKeepRunningInTray(runInTray);
        QApplication::setQuitOnLastWindowClosed(!runInTray);
        static_cast<void>(GuiInstanceServer::requestTray());
        dialog.accept();
    });
    dialog.exec();
}


} // namespace pacsmith::gui
