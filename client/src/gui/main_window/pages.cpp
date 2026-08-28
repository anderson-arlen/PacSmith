#include "gui/main_window/common.hpp"

namespace pacsmith::gui {

QWidget *MainWindow::createProjectInfoPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Installed status and how this package was acquired."),
        page,
        QStringLiteral("Update settings are on the Update Monitoring tab. Acquisition details are recorded when PacSmith imports a vendor artifact and do not change when you edit package configuration.")));

    auto *summary = new QHBoxLayout;
    summary->setSpacing(20);
    overviewIcon_ = new QLabel(page);
    overviewIcon_->setFixedSize(96, 96);
    overviewIcon_->setAlignment(Qt::AlignCenter);

    auto *details = new QVBoxLayout;
    details->setSpacing(8);
    projectStateLabel_ = new QLabel(page);
    projectStateLabel_->setWordWrap(true);
    projectStateLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    projectRepositoryStateLabel_ = new QLabel(page);
    projectRepositoryStateLabel_->setWordWrap(true);
    projectRepositoryStateLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    activeTrackerLabel_ = new QLabel(page);
    activeTrackerLabel_->setWordWrap(true);
    activeTrackerLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    projectAcquisitionLabel_ = new QLabel(page);
    projectAcquisitionLabel_->setWordWrap(true);
    projectAcquisitionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(projectStateLabel_);
    details->addWidget(projectRepositoryStateLabel_);
    details->addWidget(activeTrackerLabel_);
    details->addWidget(projectAcquisitionLabel_);

    auto *actions = new QWidget(page);
    auto *actionLayout = new QVBoxLayout(actions);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(8);
    projectActionNotice_ = new QLabel(actions);
    projectActionNotice_->setWordWrap(true);
    projectActionNotice_->setVisible(false);
    projectActionButton_ = new QPushButton(actions);
    applyPrimaryActionStyle(projectActionButton_);
    projectActionButton_->setVisible(false);
    projectActionButton_->setMinimumWidth(240);
    editConfigurationButton_ = new QPushButton(QStringLiteral("Edit Package Configuration"), actions);
    editConfigurationButton_->setVisible(false);
    editConfigurationButton_->setMinimumWidth(240);
    actionLayout->addWidget(projectActionNotice_);
    actionLayout->addWidget(projectActionButton_);
    actionLayout->addWidget(editConfigurationButton_);

    summary->addWidget(overviewIcon_, 0, Qt::AlignTop);
    summary->addLayout(details, 1);
    summary->addWidget(actions, 0, Qt::AlignTop);
    layout->addLayout(summary);
    layout->addStretch(1);
    connect(projectActionButton_, &QPushButton::clicked, this, &MainWindow::handleProjectInfoAction);
    connect(editConfigurationButton_, &QPushButton::clicked, this,
            &MainWindow::editPackageConfiguration);
    return page;
}

QWidget *MainWindow::createOverviewPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Every imported vendor release, with actions for the selected row."),
        page,
        QStringLiteral("Each vendor release retains its acquisition evidence, update configuration, package recipe, builds, and install history. Select a release to download, install, roll back, or open package setup.")));
    releaseTable_ = new QTableWidget(page);
    releaseTable_->setColumnCount(8);
    releaseTable_->setHorizontalHeaderLabels({QStringLiteral("Vendor version"),
        QStringLiteral("Status"), QStringLiteral("Acquired from"), QStringLiteral("Update strategy"), QStringLiteral("Review"),
        QStringLiteral("Builds"), QStringLiteral("Arch package"), QStringLiteral("Installed")});
    releaseTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    releaseTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    releaseTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    releaseTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    releaseTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    releaseTable_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    releaseTable_->setMinimumHeight(180);
    layout->addWidget(releaseTable_, 1);
    overviewChecklist_ = new QLabel(page);
    overviewChecklist_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    overviewChecklist_->setWordWrap(true);
    editReleaseButton_ = new QPushButton(QStringLiteral("Edit / Set Up Release"), page);
    prepareReleaseButton_ = new QPushButton(QStringLiteral("Download & Prepare"), page);
    installReleaseButton_ = new QPushButton(QStringLiteral("Install Selected Release"), page);
    rollbackButton_ = new QPushButton(QStringLiteral("Roll Back to Release"), page);
    deleteReleaseButton_ = new QPushButton(QStringLiteral("Delete Release"), page);
    historyCheckUpdatesButton_ = new QPushButton(QStringLiteral("Check for Updates"), page);
    submitReleaseButton_ = new QPushButton(QStringLiteral("Submit New Release…"), page);
    submitReleaseButton_->setToolTip(
        QStringLiteral("Import a locally downloaded vendor artifact as a new release of this project"));
    historyCheckUpdatesButton_->setToolTip(
        QStringLiteral("Check the current update source for a newer vendor release"));
    historyCheckUpdatesButton_->setEnabled(false);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(historyCheckUpdatesButton_);
    buttons->addWidget(submitReleaseButton_);
    buttons->addWidget(editReleaseButton_);
    buttons->addWidget(prepareReleaseButton_);
    buttons->addWidget(installReleaseButton_);
    buttons->addWidget(rollbackButton_);
    buttons->addWidget(deleteReleaseButton_);
    buttons->addStretch();
    layout->addWidget(overviewChecklist_);
    layout->addLayout(buttons);
    layout->addWidget(new QLabel(QStringLiteral("Project history"), page));
    historyList_ = new QListWidget(page);
    historyList_->setMinimumHeight(120);
    layout->addWidget(historyList_, 1);
    connect(historyCheckUpdatesButton_, &QPushButton::clicked, this, &MainWindow::startUpdateCheck);
    connect(submitReleaseButton_, &QPushButton::clicked, this,
            &MainWindow::submitManualRelease);
    connect(editReleaseButton_, &QPushButton::clicked, this, &MainWindow::editSelectedRelease);
    connect(prepareReleaseButton_, &QPushButton::clicked, this, &MainWindow::prepareSelectedRelease);
    connect(installReleaseButton_, &QPushButton::clicked, this, &MainWindow::installSelectedRelease);
    connect(rollbackButton_, &QPushButton::clicked, this, &MainWindow::rollbackSelectedRelease);
    connect(deleteReleaseButton_, &QPushButton::clicked, this, &MainWindow::deleteSelectedRelease);
    connect(releaseTable_, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *) { editSelectedRelease(); });
    connect(releaseTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto id = selectedDashboardReleaseId();
        if (id.isEmpty() || !project_) return;
        const auto *selected = project_->release(id);
        const bool preparing = selected != nullptr && selected->id == preparingReleaseId_ &&
                               project_->id == preparingProjectId_;
        editReleaseButton_->setEnabled(selected != nullptr &&
                                       selected->state != ReleaseState::Discovered && !preparing);
        prepareReleaseButton_->setText(preparing ? QStringLiteral("Show Progress")
                                                 : QStringLiteral("Download & Prepare"));
        prepareReleaseButton_->setEnabled(preparing ||
            (selected != nullptr && selected->state == ReleaseState::Discovered &&
             !debDownloadService_->isRunning() && importThread_ == nullptr));
        const bool installed = selected != nullptr && selected->id == project_->installedReleaseId;
        installReleaseButton_->setEnabled(selected != nullptr && !installed &&
                                          releaseHasRetainedPackage(*selected) &&
                                          !packageOperationInProgress());
        rollbackButton_->setEnabled(selected != nullptr && !installed && !project_->installedVersion.isEmpty() &&
                                    (!selected->builds.isEmpty() || !selected->producedPackages.isEmpty()));
        deleteReleaseButton_->setEnabled(selected != nullptr && !installed && !preparing &&
                                         !packageOperationInProgress());
        if (preparing) {
            overviewChecklist_->setText(
                QStringLiteral("<b>Preparing release %1</b><br>%2. Download and inspection continue in the background even when the progress window is hidden.")
                    .arg(selected->debian.version.toHtmlEscaped(),
                         preparationPhase_.toHtmlEscaped()));
        } else if (selected != nullptr && selected->state == ReleaseState::Discovered) {
            const auto trust = selected->sourceSha256.isEmpty()
                ? QStringLiteral("No publisher checksum is available; the downloaded bytes will be locally hashed and remain marked unsigned.")
                : QStringLiteral("A publisher or signed-repository SHA256 is available and will be required during download.");
            overviewChecklist_->setText(QStringLiteral(
                "<b>Discovered release %1</b><br>%2 Download and inspect the artifact before its package settings can be reviewed or edited.")
                    .arg(selected->debian.version.toHtmlEscaped(), trust));
        } else if (selected != nullptr && currentReleaseId_ != id) {
            currentReleaseId_ = id;
            populateOverview();
        }
    });
    return page;
}

QWidget *MainWindow::createSourceOverviewPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("The imported vendor artifact. Inspection never executes it."),
        page,
        QStringLiteral("This page is evidence only. Analysis never executes package content, and nothing here changes the generated Arch package.")));
    sourceTypeHeadline_ = new QLabel(page);
    sourceTypeHeadline_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sourceTypeHeadline_->setWordWrap(true);
    sourceTypeExplanation_ = settingsSectionHelp(page, {}, {});
    sourceAcquisitionDetail_ = new QLabel(page);
    sourceAcquisitionDetail_->setWordWrap(true);
    sourceAcquisitionDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sourceIdentityDetail_ = new QLabel(page);
    sourceIdentityDetail_->setWordWrap(true);
    sourceIdentityDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sourceInventoryDetail_ = new QLabel(page);
    sourceInventoryDetail_->setWordWrap(true);
    sourceInventoryDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(sourceTypeHeadline_);
    layout->addWidget(sourceTypeExplanation_);
    layout->addSpacing(8);
    layout->addWidget(sourceAcquisitionDetail_);
    layout->addWidget(sourceIdentityDetail_);
    layout->addWidget(sourceInventoryDetail_);
    layout->addStretch();
    return page;
}

QWidget *MainWindow::createSourceMetadataPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Vendor metadata from the imported artifact."),
        page,
        QStringLiteral("These fields describe the source package. Arch package name, dependencies, and install paths are configured separately.")));
    auto *splitter = new QSplitter(Qt::Vertical, page);
    metadataView_ = new QPlainTextEdit(splitter);
    rawMetadataView_ = new QPlainTextEdit(splitter);
    makeReadOnlyCodeEditor(metadataView_);
    makeReadOnlyCodeEditor(rawMetadataView_);
    metadataView_->setPlaceholderText(QStringLiteral("Parsed vendor metadata"));
    rawMetadataView_->setPlaceholderText(QStringLiteral("Raw vendor fields"));
    splitter->addWidget(metadataView_);
    splitter->addWidget(rawMetadataView_);
    layout->addWidget(splitter, 1);
    return page;
}

QWidget *MainWindow::createInstallLayoutPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    auto *mappingGroup = new QGroupBox(QStringLiteral("Archive / binary install mapping"), page);
    installMappingWidget_ = mappingGroup;
    auto *mappingLayout = new QFormLayout(mappingGroup);
    installMappingLayout_ = mappingLayout;
    auto *mappingExplanation = pageIntroduction(
        QStringLiteral("How the vendor archive or executable is laid out in the Arch package."),
        mappingGroup,
        QStringLiteral("These settings do not modify the imported artifact."));
    mappingLayout->addRow(mappingExplanation);
    archiveLayout_ = new QComboBox(mappingGroup);
    archiveLayout_->addItems({QStringLiteral("Install under /opt"),
                              QStringLiteral("Preserve recognized filesystem root")});
    installOptDirectory_ = new QLineEdit(mappingGroup);
    installOptDirectory_->setPlaceholderText(QStringLiteral("application-name"));
    installCommonPrefix_ = new QLineEdit(mappingGroup);
    installCommonPrefix_->setReadOnly(true);
    installCommonPrefix_->setPlaceholderText(QStringLiteral("No single archive root detected"));
    installStripPrefix_ = new QCheckBox(
        QStringLiteral("Strip this common top-level directory while installing"), mappingGroup);
    installBinarySource_ = new QLineEdit(mappingGroup);
    installBinarySource_->setPlaceholderText(QStringLiteral("relative/path/to/executable"));
    installBinaryDestination_ = new QLineEdit(mappingGroup);
    installBinaryDestination_->setPlaceholderText(QStringLiteral("/usr/bin/application"));
    auto *saveMapping = new QPushButton(QStringLiteral("Save Install Mapping"), mappingGroup);
    mappingLayout->addRow(QStringLiteral("Archive layout"), archiveLayout_);
    mappingLayout->addRow(QStringLiteral("/opt directory"), installOptDirectory_);
    mappingLayout->addRow(QStringLiteral("Detected common root"), installCommonPrefix_);
    mappingLayout->addRow(QString{}, installStripPrefix_);
    installCommandsHint_ = settingsSectionHelp(
        mappingGroup,
        QStringLiteral("PATH commands for this archive are configured on Commands."),
        QStringLiteral("Choose a payload file there instead of typing a raw path here."));
    mappingLayout->addRow(installCommandsHint_);
    mappingLayout->addRow(QStringLiteral("Executable inside archive"), installBinarySource_);
    mappingLayout->addRow(QStringLiteral("Command destination"), installBinaryDestination_);
    mappingLayout->addRow(QString{}, saveMapping);
    layout->addWidget(mappingGroup);
    layout->addStretch();
    connect(saveMapping, &QPushButton::clicked, this, &MainWindow::saveInstallMapping);
    connect(archiveLayout_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        if (currentRelease() == nullptr ||
            (currentRelease()->sourceType != SourcePackageType::Archive &&
             currentRelease()->sourceType != SourcePackageType::AppImage)) return;
        const bool opt = index == 0;
        installOptDirectory_->setEnabled(opt);
        installBinarySource_->setEnabled(opt);
        installBinaryDestination_->setEnabled(opt);
    });
    return page;
}

QWidget *MainWindow::createInstallPlanPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    installPlanNotice_ = pageIntroduction({}, page);
    layout->addWidget(installPlanNotice_);
    auto *appImagePlan = new QGroupBox(QStringLiteral("Installed filesystem layout"), page);
    appImageInstallPlanWidget_ = appImagePlan;
    auto *appImagePlanLayout = new QVBoxLayout(appImagePlan);
    appImageInstallPlan_ = new QTreeWidget(appImagePlan);
    appImageInstallPlan_->setColumnCount(3);
    appImageInstallPlan_->setHeaderLabels(
        {QStringLiteral("Installed path"), QStringLiteral("Source"),
         QStringLiteral("Purpose")});
    appImageInstallPlan_->header()->setSectionResizeMode(QHeaderView::Stretch);
    appImageInstallPlan_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    appImageInstallPlan_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    appImageInstallPlan_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    appImageInstallPlan_->setItemsExpandable(true);
    appImageInstallPlan_->setExpandsOnDoubleClick(true);
    appImageInstallPlan_->setAnimated(true);
    appImageInstallPlan_->setUniformRowHeights(true);
    appImagePlanLayout->addWidget(appImageInstallPlan_);
    layout->addWidget(appImagePlan, 1);
    return page;
}

QWidget *MainWindow::createDependenciesPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Map vendor dependencies onto Arch packages and how they are treated."),
        page,
        QStringLiteral("“Require Arch package” adds the mapped package to depends. PacSmith checks required names against your configured pacman repositories: unresolved mappings that need your review are amber, unavailable package names are red, and typing in the Arch package column suggests repository package names. Green indicates an AI-assisted decision that is resolved. Use “Bundled with application” or “Provided by this package” only when the imported payload actually contains that dependency; those choices remove it from depends.")));
    dependenciesTable_ = new QTableWidget(page);
    dependenciesTable_->setColumnCount(5);
    dependenciesTable_->setHorizontalHeaderLabels({QStringLiteral("Debian dependency"), QStringLiteral("Arch package"),
                                                    QStringLiteral("Status"), QStringLiteral("Source / confidence"),
                                                    QStringLiteral("Treatment")});
    dependenciesTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    dependenciesTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    dependenciesTable_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    dependenciesTable_->verticalHeader()->setVisible(false);
    dependenciesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    dependenciesTable_->setItemDelegateForColumn(1, new PackageNameDelegate(dependenciesTable_));
    connect(dependenciesTable_, &QTableWidget::cellChanged, this, &MainWindow::dependencyEdited);
    layout->addWidget(dependenciesTable_, 1);

    auto *additional = new QGroupBox(QStringLiteral("Additional Arch runtime dependencies"), page);
    auto *additionalLayout = new QVBoxLayout(additional);
    auto *additionalHelp = new QLabel(QStringLiteral(
        "Add an official Arch package only when static inspection or upstream documentation shows that the installed application requires it and the vendor payload does not bundle it."),
        additional);
    additionalHelp->setWordWrap(true);
    additionalLayout->addWidget(additionalHelp);
    additionalDependencies_ = new QListWidget(additional);
    additionalDependencies_->setSelectionMode(QAbstractItemView::SingleSelection);
    additionalLayout->addWidget(additionalDependencies_);
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add dependency…"), additional);
    auto *remove = new QPushButton(QStringLiteral("Remove selected"), additional);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    additionalLayout->addLayout(buttons);
    connect(add, &QPushButton::clicked, this, &MainWindow::addAdditionalDependency);
    connect(remove, &QPushButton::clicked, this, &MainWindow::removeAdditionalDependency);
    layout->addWidget(additional);
    return page;
}

QWidget *MainWindow::createPackageMetadataPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Maintain the metadata published in the generated Arch package."), page,
        QStringLiteral("Description, homepage, and license describe this package. Provides/conflicts are optional compatibility relationships; do not infer them merely from a -bin suffix. These values are ordinary Guided controls and are also available to external agents through MCP.")));
    auto *form = new QFormLayout;
    packageDisplayName_ = new QLineEdit(page);
    packageArchName_ = new QLineEdit(page);
    packageVendorName_ = new QLineEdit(page);
    packageDescription_ = new QLineEdit(page);
    packageHomepage_ = new QLineEdit(page);
    packageLicenses_ = new QLineEdit(page);
    packageLicenses_->setPlaceholderText(QStringLiteral("SPDX expression, or custom:vendor"));
    packageProvides_ = new QLineEdit(page);
    packageProvides_->setPlaceholderText(QStringLiteral("Comma-separated package or virtual names"));
    packageConflicts_ = new QLineEdit(page);
    packageConflicts_->setPlaceholderText(QStringLiteral("Comma-separated package names"));
    form->addRow(QStringLiteral("Display name"), packageDisplayName_);
    form->addRow(QStringLiteral("Arch package name"), packageArchName_);
    form->addRow(QStringLiteral("Vendor / maintainer"), packageVendorName_);
    form->addRow(QStringLiteral("Description"), packageDescription_);
    form->addRow(QStringLiteral("Homepage"), packageHomepage_);
    form->addRow(QStringLiteral("License(s)"), packageLicenses_);
    form->addRow(QStringLiteral("Provides"), packageProvides_);
    form->addRow(QStringLiteral("Conflicts"), packageConflicts_);
    layout->addLayout(form);
    auto *save = new QPushButton(QStringLiteral("Save package metadata"), page);
    connect(save, &QPushButton::clicked, this, &MainWindow::savePackageMetadata);
    layout->addWidget(save, 0, Qt::AlignLeft);
    layout->addStretch(1);
    return page;
}

QWidget *MainWindow::createVendorScriptsPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Original vendor maintainer scripts. PacSmith never executes them."),
        page,
        QStringLiteral("Reference only. Arch handling is decided on Configuration → Scripts in Guided mode, or in the PKGBUILD in Custom mode.")));
    auto *splitter = new QSplitter(page);
    scriptsList_ = new QListWidget(splitter);
    scriptView_ = new QPlainTextEdit(splitter);
    makeReadOnlyCodeEditor(scriptView_);
    new PkgbuildHighlighter(scriptView_->document());
    scriptsList_->setMinimumWidth(210);
    splitter->addWidget(scriptsList_);
    splitter->addWidget(scriptView_);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    auto *reviewRow = new QHBoxLayout;
    scriptStatus_ = new QLabel(page);
    acknowledgeScriptButton_ = new QPushButton(QStringLiteral("Mark Original Source as Read (Optional)"), page);
    reviewRow->addWidget(scriptStatus_, 1);
    reviewRow->addWidget(acknowledgeScriptButton_);
    layout->addLayout(reviewRow);
    connect(scriptsList_, &QListWidget::currentRowChanged, this, [this] { updateSelectedScript(); });
    connect(acknowledgeScriptButton_, &QPushButton::clicked, this, &MainWindow::acknowledgeSelectedScript);
    return page;
}

QWidget *MainWindow::createConfigScriptsPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Map vendor script responsibilities onto Arch lifecycle handling."),
        page,
        QStringLiteral("Each row is an extracted responsibility. Set Arch handling here; the .install file below is what pacman may run as root.")));
    scriptsActionNotice_ = new QLabel(page);
    scriptsActionNotice_->setWordWrap(true);
    scriptsActionNotice_->setFrameStyle(QFrame::StyledPanel);
    scriptsActionNotice_->setContentsMargins(10, 8, 10, 8);
    layout->addWidget(scriptsActionNotice_);
    auto *splitter = new QSplitter(Qt::Vertical, page);
    auto *mapping = new QWidget(splitter);
    auto *mappingLayout = new QVBoxLayout(mapping);
    mappingLayout->setContentsMargins(0, 0, 0, 0);
    scriptFindingsTable_ = new QTableWidget(mapping);
    scriptFindingsTable_->setColumnCount(4);
    scriptFindingsTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Source script"), QStringLiteral("Responsibility"),
         QStringLiteral("Arch handling"), QStringLiteral("Provenance")});
    scriptFindingsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    scriptFindingsTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scriptFindingsTable_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    scriptFindingsTable_->verticalHeader()->setVisible(false);
    scriptFindingsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    scriptFindingsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    mappingLayout->addWidget(scriptFindingsTable_, 1);
    auto *jumpRow = new QHBoxLayout;
    auto *viewSource = new QPushButton(QStringLiteral("View vendor scripts"), mapping);
    jumpRow->addWidget(viewSource);
    jumpRow->addStretch();
    mappingLayout->addLayout(jumpRow);

    auto *lifecycleGroup = new QGroupBox(QStringLiteral("Arch lifecycle (.install)"), splitter);
    auto *lifecycleLayout = new QVBoxLayout(lifecycleGroup);
    lifecycleStatus_ = new QLabel(lifecycleGroup);
    lifecycleStatus_->setWordWrap(true);
    lifecycleView_ = new QPlainTextEdit(lifecycleGroup);
    makeReadOnlyCodeEditor(lifecycleView_);
    new PkgbuildHighlighter(lifecycleView_->document());
    lifecycleView_->setMinimumHeight(140);
    editLifecycleButton_ = new QPushButton(QStringLiteral("Create Lifecycle Script"), lifecycleGroup);
    saveLifecycleButton_ = new QPushButton(QStringLiteral("Save Script"), lifecycleGroup);
    cancelLifecycleButton_ = new QPushButton(QStringLiteral("Cancel Edit"), lifecycleGroup);
    acknowledgeLifecycleButton_ = new QPushButton(QStringLiteral("Approve Exact Arch Script"), lifecycleGroup);
    discardLifecycleButton_ = new QPushButton(QStringLiteral("Remove Lifecycle Script"), lifecycleGroup);
    auto *lifecycleButtons = new QHBoxLayout;
    lifecycleButtons->addWidget(editLifecycleButton_);
    lifecycleButtons->addWidget(saveLifecycleButton_);
    lifecycleButtons->addWidget(cancelLifecycleButton_);
    lifecycleButtons->addStretch();
    lifecycleButtons->addWidget(discardLifecycleButton_);
    lifecycleButtons->addWidget(acknowledgeLifecycleButton_);
    lifecycleLayout->addWidget(lifecycleStatus_);
    lifecycleLayout->addWidget(lifecycleView_, 1);
    lifecycleLayout->addLayout(lifecycleButtons);
    splitter->addWidget(mapping);
    splitter->addWidget(lifecycleGroup);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    connect(viewSource, &QPushButton::clicked, this, [this] {
        selectSection(EditorSection::SourceScripts);
    });
    connect(scriptFindingsTable_, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item) {
        if (item == nullptr || scriptsList_ == nullptr) return;
        const auto *nameItem = scriptFindingsTable_->item(item->row(), 0);
        if (nameItem == nullptr) return;
        selectSection(EditorSection::SourceScripts);
        const auto name = nameItem->text();
        for (int row = 0; row < scriptsList_->count(); ++row) {
            const auto *scriptItem = scriptsList_->item(row);
            if (scriptItem != nullptr && scriptItem->text().contains(name)) {
                scriptsList_->setCurrentRow(row);
                break;
            }
        }
    });
    connect(editLifecycleButton_, &QPushButton::clicked, this, &MainWindow::beginLifecycleEdit);
    connect(saveLifecycleButton_, &QPushButton::clicked, this, &MainWindow::saveLifecycleEdit);
    connect(cancelLifecycleButton_, &QPushButton::clicked, this, &MainWindow::cancelLifecycleEdit);
    connect(acknowledgeLifecycleButton_, &QPushButton::clicked, this, &MainWindow::acknowledgeLifecycleScript);
    connect(discardLifecycleButton_, &QPushButton::clicked, this, &MainWindow::discardLifecycleScript);
    return page;
}

QWidget *MainWindow::createPayloadPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    payloadIntroduction_ = pageIntroduction(
        QStringLiteral("Filesystem from the imported artifact. Most files need no action."),
        page,
        QStringLiteral("Keep or exclude records a recipe rule for the resulting package; it does not modify the source archive. Changed upstream content restores review."));
    layout->addWidget(payloadIntroduction_);
    auto *splitter = new QSplitter(Qt::Vertical, page);
    payloadTree_ = new QTreeWidget(page);
    payloadTree_->setHeaderLabels({QStringLiteral("Path"), QStringLiteral("Type"), QStringLiteral("Size"),
                                   QStringLiteral("Review")});
    payloadTree_->header()->setSectionResizeMode(QHeaderView::Stretch);
    payloadTree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    payloadTree_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    auto *details = new QWidget(splitter);
    auto *detailsLayout = new QVBoxLayout(details);
    payloadStatus_ = new QLabel(details);
    payloadStatus_->setWordWrap(true);
    payloadPreview_ = new QPlainTextEdit(details);
    makeReadOnlyCodeEditor(payloadPreview_);
    payloadPreview_->setPlaceholderText(QStringLiteral("Select a payload file to inspect it."));
    keepPayloadButton_ = new QPushButton(QStringLiteral("Keep in Package && Acknowledge"), details);
    excludePayloadButton_ = new QPushButton(QStringLiteral("Exclude from Package"), details);
    clearPayloadDecisionButton_ = new QPushButton(QStringLiteral("Clear Decision"), details);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(keepPayloadButton_);
    buttons->addWidget(excludePayloadButton_);
    buttons->addWidget(clearPayloadDecisionButton_);
    buttons->addStretch();
    detailsLayout->addWidget(payloadStatus_);
    detailsLayout->addWidget(payloadPreview_, 1);
    detailsLayout->addLayout(buttons);
    splitter->addWidget(payloadTree_);
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);
    connect(payloadTree_, &QTreeWidget::currentItemChanged, this, [this] { updateSelectedPayload(); });
    connect(keepPayloadButton_, &QPushButton::clicked, this, [this] { setSelectedPayloadDecision(false); });
    connect(excludePayloadButton_, &QPushButton::clicked, this, [this] { setSelectedPayloadDecision(true); });
    connect(clearPayloadDecisionButton_, &QPushButton::clicked, this, &MainWindow::clearSelectedPayloadDecision);
    return page;
}

QWidget *MainWindow::createCommandsPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Choose which executables the Arch package should expose on PATH."),
        page,
        QStringLiteral("Rename a command to change the /usr/bin name. Source paths name exact files in the vendor artifact; they are not edited there. Use Add from payload to map any archive file onto /usr/bin.")));
    commandsTable_ = new QTableWidget(page);
    commandsTable_->setColumnCount(6);
    commandsTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Expose"), QStringLiteral("Payload executable"),
         QStringLiteral("Command"), QStringLiteral("Destination"),
         QStringLiteral("Method"), QStringLiteral("Status")});
    commandsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    commandsTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    commandsTable_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    commandsTable_->verticalHeader()->setVisible(false);
    commandsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    commandsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(commandsTable_, 1);
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add from payload…"), page);
    auto *assign = new QPushButton(QStringLiteral("Assign payload…"), page);
    auto *remove = new QPushButton(QStringLiteral("Remove"), page);
    buttons->addWidget(add);
    buttons->addWidget(assign);
    buttons->addWidget(remove);
    buttons->addStretch();
    layout->addLayout(buttons);
    connect(commandsTable_, &QTableWidget::cellChanged,
            this, &MainWindow::commandEdited);
    connect(add, &QPushButton::clicked, this, &MainWindow::addCommandFromPayload);
    connect(assign, &QPushButton::clicked, this, &MainWindow::assignPayloadToSelectedCommand);
    connect(remove, &QPushButton::clicked, this, &MainWindow::removeSelectedCommand);
    return page;
}

QWidget *MainWindow::createAppRunPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("AppRun is the AppImage entry point. Review text scripts before building."),
        page,
        QStringLiteral("It stays inside the extracted AppDir under /opt with the rest of the image; the PATH command on Commands is a separate wrapper. A #! script can be reviewed or edited here. Compiled ELF or symlink AppRuns cannot be shown as text and are installed unchanged.")));
    appRunReviewBanner_ = new QFrame(page);
    appRunReviewBanner_->setObjectName(QStringLiteral("appRunReviewBanner"));
    appRunReviewBanner_->setStyleSheet(QStringLiteral(
        "QFrame#appRunReviewBanner { background-color: rgba(229,185,61,28); "
        "border: 1px solid #b89624; border-radius: 5px; }"
        "QFrame#appRunReviewBanner QLabel { background: transparent; border: none; color: #e5b93d; }"));
    auto *bannerLayout = new QHBoxLayout(appRunReviewBanner_);
    bannerLayout->setContentsMargins(12, 8, 12, 8);
    appRunReviewLabel_ = new QLabel(appRunReviewBanner_);
    appRunReviewLabel_->setWordWrap(true);
    keepOriginalAppRunButton_ = new QPushButton(QStringLiteral("Keep original"), appRunReviewBanner_);
    bannerLayout->addWidget(appRunReviewLabel_, 1);
    bannerLayout->addWidget(keepOriginalAppRunButton_, 0, Qt::AlignTop);
    layout->addWidget(appRunReviewBanner_);
    appRunEditor_ = new QPlainTextEdit(page);
    appRunEditor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    appRunEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    new ShellHighlighter(appRunEditor_->document());
    appRunStatus_ = new QLabel(page);
    appRunStatus_->setWordWrap(true);
    restoreAppRunButton_ = new QPushButton(QStringLiteral("Restore original"), page);
    saveAppRunButton_ = new QPushButton(QStringLiteral("Save"), page);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(appRunStatus_, 1);
    buttons->addWidget(restoreAppRunButton_);
    buttons->addWidget(saveAppRunButton_);
    layout->addWidget(appRunEditor_, 1);
    layout->addLayout(buttons);
    connect(saveAppRunButton_, &QPushButton::clicked, this, &MainWindow::saveAppRun);
    connect(keepOriginalAppRunButton_, &QPushButton::clicked, this, &MainWindow::keepOriginalAppRun);
    connect(restoreAppRunButton_, &QPushButton::clicked, this, &MainWindow::restoreOriginalAppRun);
    connect(appRunEditor_->document(), &QTextDocument::modificationChanged, this, [this](const bool modified) {
        if (currentRelease() == nullptr) return;
        const auto &appRun = currentRelease()->installMapping.appRun;
        if (keepOriginalAppRunButton_ != nullptr) {
            keepOriginalAppRunButton_->setEnabled(appRun.requiresReview() && !modified);
        }
        if (restoreAppRunButton_ != nullptr) {
            restoreAppRunButton_->setVisible(
                !appRun.originalContents.isEmpty() &&
                (appRun.userModified || modified ||
                 appRunEditor_->toPlainText() != appRun.originalContents));
        }
    });
    return page;
}

QWidget *MainWindow::createDesktopEntriesPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Desktop launchers for the generated Arch package."),
        page,
        QStringLiteral("PacSmith derives genuine application launchers from vendor metadata, preserves the vendor payload unchanged, and carries user-edited or additional entries forward exactly.")));
    auto *splitter = new QSplitter(page);
    auto *left = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(left);
    desktopEntriesList_ = new QListWidget(left);
    auto *entryButtons = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("New"), left);
    auto *duplicate = new QPushButton(QStringLiteral("Duplicate"), left);
    deleteDesktopEntryButton_ = new QPushButton(QStringLiteral("Delete"), left);
    entryButtons->addWidget(add);
    entryButtons->addWidget(duplicate);
    entryButtons->addWidget(deleteDesktopEntryButton_);
    leftLayout->addWidget(desktopEntriesList_, 1);
    leftLayout->addLayout(entryButtons);

    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    desktopEntryEnabled_ = new QCheckBox(QStringLiteral("Include this desktop entry"), right);
    auto *destinationRow = new QHBoxLayout;
    destinationRow->addWidget(new QLabel(QStringLiteral("Install as"), right));
    desktopEntryDestination_ = new QLineEdit(right);
    desktopEntryDestination_->setPlaceholderText(
        QStringLiteral("/usr/share/applications/application.desktop"));
    destinationRow->addWidget(desktopEntryDestination_, 1);
    desktopEntryEditor_ = new QPlainTextEdit(right);
    desktopEntryEditor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    desktopEntryEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    new DesktopEntryHighlighter(desktopEntryEditor_->document());
    desktopEntryStatus_ = new QLabel(right);
    desktopEntryStatus_->setWordWrap(true);
    saveDesktopEntryButton_ = new QPushButton(QStringLiteral("Validate && Save"), right);
    rightLayout->addWidget(desktopEntryEnabled_);
    rightLayout->addLayout(destinationRow);
    rightLayout->addWidget(desktopEntryEditor_, 1);
    auto *saveRow = new QHBoxLayout;
    saveRow->addWidget(desktopEntryStatus_, 1);
    saveRow->addWidget(saveDesktopEntryButton_);
    rightLayout->addLayout(saveRow);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    connect(desktopEntriesList_, &QListWidget::currentRowChanged,
            this, [this] { updateSelectedDesktopEntry(); });
    connect(saveDesktopEntryButton_, &QPushButton::clicked,
            this, &MainWindow::saveSelectedDesktopEntry);
    connect(add, &QPushButton::clicked, this, &MainWindow::addDesktopEntry);
    connect(duplicate, &QPushButton::clicked, this, &MainWindow::duplicateDesktopEntry);
    connect(deleteDesktopEntryButton_, &QPushButton::clicked,
            this, &MainWindow::deleteDesktopEntry);
    return page;
}

QWidget *MainWindow::createIconPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Choose an icon from the artifact, a local file, or an official HTTPS URL."),
        page,
        QStringLiteral("PacSmith stores the chosen bytes in the release and pins their SHA256.")));
    auto *top = new QHBoxLayout;
    iconPreview_ = new QLabel(page);
    iconPreview_->setFixedSize(160, 160);
    iconPreview_->setAlignment(Qt::AlignCenter);
    iconPreview_->setFrameStyle(QFrame::StyledPanel);
    top->addWidget(iconPreview_);
    auto *formWidget = new QWidget(page);
    auto *form = new QFormLayout(formWidget);
    payloadIconCandidates_ = new QComboBox(formWidget);
    auto *selectPayload = new QPushButton(QStringLiteral("Use Selected Payload Icon"), formWidget);
    auto *payloadRow = new QHBoxLayout;
    payloadRow->addWidget(payloadIconCandidates_, 1);
    payloadRow->addWidget(selectPayload);
    auto *payloadContainer = new QWidget(formWidget);
    payloadContainer->setLayout(payloadRow);
    auto *browse = new QPushButton(QStringLiteral("Choose Local Image…"), formWidget);
    iconUrl_ = new QLineEdit(formWidget);
    iconUrl_->setPlaceholderText(QStringLiteral("https://vendor.example/application.svg"));
    auto *fetch = new QPushButton(QStringLiteral("Fetch && Review…"), formWidget);
    auto *urlRow = new QHBoxLayout;
    urlRow->addWidget(iconUrl_, 1);
    urlRow->addWidget(fetch);
    auto *urlContainer = new QWidget(formWidget);
    urlContainer->setLayout(urlRow);
    form->addRow(QStringLiteral("Artifact icon"), payloadContainer);
    form->addRow(QStringLiteral("Local file"), browse);
    form->addRow(QStringLiteral("Official HTTPS URL"), urlContainer);
    top->addWidget(formWidget, 1);
    layout->addLayout(top);
    iconStatus_ = new QLabel(page);
    iconStatus_->setWordWrap(true);
    iconStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(iconStatus_);
    layout->addStretch(1);
    connect(selectPayload, &QPushButton::clicked, this, &MainWindow::selectPayloadIcon);
    connect(browse, &QPushButton::clicked, this, &MainWindow::importLocalIcon);
    connect(fetch, &QPushButton::clicked, this, &MainWindow::fetchRemoteIcon);
    return page;
}

QWidget *MainWindow::createPkgbuildPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("This PKGBUILD is what makepkg executes in Custom mode."),
        page,
        QStringLiteral("Guided configuration is ignored until you switch back. Saving keeps this file user-owned.")));
    auto *varsNotice = new QLabel(
        QStringLiteral("PacSmith rewrites pacsmith.vars for each vendor artifact. Use $_PACSMITH_* instead of versioned filenames."),
        page);
    varsNotice->setWordWrap(true);
    pkgbuildVarsPreview_ = new QPlainTextEdit(page);
    configureIdentityVariablesEditor(pkgbuildVarsPreview_);
    new PkgbuildHighlighter(pkgbuildVarsPreview_->document());
    pkgbuildState_ = new QLabel(page);
    pkgbuildEditor_ = new QPlainTextEdit(page);
    pkgbuildEditor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    pkgbuildEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    new PkgbuildHighlighter(pkgbuildEditor_->document());
    auto *saveButton = new QPushButton(QStringLiteral("Save"), page);
    auto *reloadButton = new QPushButton(QStringLiteral("Reload"), page);
    auto *validateButton = new QPushButton(QStringLiteral("Validate"), page);
    auto *guidedButton = new QPushButton(QStringLiteral("Return to Guided"), page);
    pkgbuildBuildButton_ = new QPushButton(QStringLiteral("Build"), page);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(saveButton);
    buttons->addWidget(reloadButton);
    buttons->addWidget(validateButton);
    buttons->addWidget(guidedButton);
    buttons->addWidget(pkgbuildBuildButton_);
    buttons->addStretch();
    layout->addWidget(varsNotice);
    layout->addWidget(pkgbuildVarsPreview_);
    layout->addWidget(pkgbuildState_);
    layout->addWidget(pkgbuildEditor_, 1);
    layout->addLayout(buttons);
    connect(saveButton, &QPushButton::clicked, this, &MainWindow::savePkgbuild);
    connect(reloadButton, &QPushButton::clicked, this, &MainWindow::populatePkgbuild);
    connect(validateButton, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(this, QStringLiteral("PKGBUILD validation"),
                                 PkgbuildGenerator::validate(pkgbuildEditor_->toPlainText()));
    });
    connect(guidedButton, &QPushButton::clicked, this, [this] { setConfigurationMode(false); });
    connect(pkgbuildBuildButton_, &QPushButton::clicked, this, [this] { startBuild(); });
    connect(pkgbuildEditor_->document(), &QTextDocument::modificationChanged, this, [this](const bool modified) {
        if (!project_ || pkgbuildState_ == nullptr) return;
        pkgbuildState_->setText(modified ? QStringLiteral("● Unsaved editor changes")
                                         : QStringLiteral("⚠ Custom PKGBUILD. Guided configuration is ignored until you switch back."));
    });
    return page;
}

QWidget *MainWindow::createResultPkgbuildPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Read-only PKGBUILD that makepkg will run."),
        page,
        QStringLiteral("Change Guided settings to regenerate it, or switch Configuration to Custom to edit it.")));
    pkgbuildPreviewNotice_ = new QLabel(page);
    pkgbuildPreviewNotice_->setWordWrap(true);
    auto *varsNotice = new QLabel(
        QStringLiteral("PacSmith rewrites pacsmith.vars for each vendor artifact. Use $_PACSMITH_* instead of versioned filenames."),
        page);
    varsNotice->setWordWrap(true);
    resultPkgbuildVarsPreview_ = new QPlainTextEdit(page);
    configureIdentityVariablesEditor(resultPkgbuildVarsPreview_);
    new PkgbuildHighlighter(resultPkgbuildVarsPreview_->document());
    pkgbuildPreview_ = new QPlainTextEdit(page);
    makeReadOnlyCodeEditor(pkgbuildPreview_);
    new PkgbuildHighlighter(pkgbuildPreview_->document());
    auto *validateButton = new QPushButton(QStringLiteral("Validate"), page);
    auto *editButton = new QPushButton(QStringLiteral("Edit in Custom"), page);
    resultPkgbuildBuildButton_ = new QPushButton(QStringLiteral("Build"), page);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(validateButton);
    buttons->addWidget(editButton);
    buttons->addWidget(resultPkgbuildBuildButton_);
    buttons->addStretch();
    layout->addWidget(pkgbuildPreviewNotice_);
    layout->addWidget(varsNotice);
    layout->addWidget(resultPkgbuildVarsPreview_);
    layout->addWidget(pkgbuildPreview_, 1);
    layout->addLayout(buttons);
    connect(validateButton, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(this, QStringLiteral("PKGBUILD validation"),
                                 PkgbuildGenerator::validate(currentPkgbuildText()));
    });
    connect(editButton, &QPushButton::clicked, this, [this] { setConfigurationMode(true); });
    connect(resultPkgbuildBuildButton_, &QPushButton::clicked, this, [this] { startBuild(); });
    return page;
}

QWidget *MainWindow::createUpdatesPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("How PacSmith looks for the next vendor release."),
        page,
        QStringLiteral("On the project dashboard this edits the current tracking release (the installed PacSmith release when identified, otherwise the newest analyzed release). In Configuration it edits the open release. This is not part of the Pacman package, and it is separate from the immutable acquisition recorded for the imported artifact.")));
    updateOwnerLabel_ = new QLabel(page);
    updateOwnerLabel_->setWordWrap(true);
    updateOwnerLabel_->setFrameStyle(QFrame::StyledPanel);
    updateOwnerLabel_->setContentsMargins(10, 8, 10, 8);
    layout->addWidget(updateOwnerLabel_);
    auto *form = new QFormLayout;
    updateStrategy_ = new QComboBox(page);
    updateStrategy_->addItems({QStringLiteral("Manual"), QStringLiteral("Direct URL"),
                               QStringLiteral("APT repository"), QStringLiteral("RPM repository"),
                               QStringLiteral("GitHub releases")});
    autoBuildPolicy_ = new QComboBox(page);
    autoBuildPolicy_->addItem(QStringLiteral("Never"), QStringLiteral("never"));
    autoBuildPolicy_->addItem(QStringLiteral("When nothing is flagged for review"),
                              QStringLiteral("review_free"));
    autoBuildPolicy_->addItem(QStringLiteral("Resolve review items with AI automatically"),
                              QStringLiteral("ai"));
    updateUrl_ = new QLineEdit(page);
    updateUrl_->setPlaceholderText(QStringLiteral("https://vendor.example/download/package.deb"));
    directUrlFullCheckInterval_ = new QComboBox(page);
    directUrlFullCheckInterval_->addItem(QStringLiteral("Daily"), 24);
    directUrlFullCheckInterval_->addItem(QStringLiteral("Weekly"), 24 * 7);
    directUrlFullCheckInterval_->addItem(QStringLiteral("Monthly"), 24 * 30);
    directUrlFullCheckInterval_->addItem(QStringLiteral("Manual only"), 0);
    directUrlFullCheckInterval_->setToolTip(QStringLiteral(
        "Used only when the server exposes no ETag, Last-Modified value, or supported object-version header. Manual checks always run immediately."));
    aptSuite_ = new QLineEdit(page);
    aptSuite_->setPlaceholderText(QStringLiteral("stable, or ./ for a flat repository"));
    aptSuite_->setToolTip(QStringLiteral(
        "Use ./ for a flat repository whose Packages index is directly below dists-independent paths, such as Typora."));
    aptComponent_ = new QLineEdit(page);
    aptComponent_->setPlaceholderText(QStringLiteral("main; leave blank for a flat repository"));
    aptComponent_->setToolTip(QStringLiteral("Leave this blank when the APT suite ends in /."));
    aptArchitecture_ = new QLineEdit(page);
    aptArchitecture_->setPlaceholderText(QStringLiteral("amd64"));
    aptPackageName_ = new QLineEdit(page);
    rpmArchitecture_ = new QLineEdit(page);
    rpmArchitecture_->setPlaceholderText(QStringLiteral("x86_64"));
    rpmPackageName_ = new QLineEdit(page);
    aptSigningKeyring_ = new QLineEdit(page);
    aptSigningKeyring_->setPlaceholderText(QStringLiteral("Project-local keyring selected below"));
    aptSigningKeyring_->setReadOnly(true);
    aptSigningKey_ = new QComboBox(page);
    aptSigningKey_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    aptSigningKeyUrl_ = new QLineEdit(page);
    aptSigningKeyUrl_->setPlaceholderText(
        QStringLiteral("https://vendor.example/repository-signing-key.gpg"));
    aptSigningKeyUrl_->setClearButtonEnabled(true);
    auto *keyUrlRow = new QWidget(page);
    auto *keyUrlLayout = new QHBoxLayout(keyUrlRow);
    keyUrlLayout->setContentsMargins(0, 0, 0, 0);
    aptSigningKeyDownloadButton_ = new QPushButton(QStringLiteral("Fetch && Review…"), keyUrlRow);
    aptSigningKeyDownloadButton_->setToolTip(QStringLiteral(
        "Download an OpenPGP public key over HTTPS, inspect its fingerprint and SHA256, then ask before trusting it"));
    keyUrlLayout->addWidget(aptSigningKeyUrl_, 1);
    keyUrlLayout->addWidget(aptSigningKeyDownloadButton_);
    auto *keyringRow = new QWidget(page);
    auto *keyringLayout = new QHBoxLayout(keyringRow);
    keyringLayout->setContentsMargins(0, 0, 0, 0);
    auto *keyringBrowse = new QPushButton(QStringLiteral("Import…"), keyringRow);
    auto *keyringPaste = new QPushButton(QStringLiteral("Paste…"), keyringRow);
    keyringLayout->addWidget(aptSigningKeyring_, 1);
    keyringLayout->addWidget(keyringBrowse);
    keyringLayout->addWidget(keyringPaste);
    githubOwner_ = new QLineEdit(page);
    githubOwner_->setPlaceholderText(QStringLiteral("owner or organization"));
    githubRepository_ = new QLineEdit(page);
    githubRepository_->setPlaceholderText(QStringLiteral("repository"));
    githubAssetRegex_ = new QLineEdit(page);
    githubAssetRegex_->setPlaceholderText(
        QStringLiteral("Exactly one full asset name must match, e.g. app-.*-linux-amd64\\.tar\\.gz"));
    githubPrereleases_ = new QCheckBox(
        QStringLiteral("Track prereleases even after a stable release exists"), page);
    githubPrereleases_->setToolTip(QStringLiteral(
        "The default policy prefers stable releases, falls back to prereleases when no matching stable release exists, and moves to stable when one is published."));
    form->addRow(QStringLiteral("Strategy"), updateStrategy_);
    form->addRow(QStringLiteral("Auto-build updates when"), autoBuildPolicy_);
    form->addRow(QStringLiteral("URL / repository"), updateUrl_);
    form->addRow(QStringLiteral("Full-content checks"), directUrlFullCheckInterval_);
    form->addRow(QStringLiteral("APT suite"), aptSuite_);
    form->addRow(QStringLiteral("APT component"), aptComponent_);
    form->addRow(QStringLiteral("APT architecture"), aptArchitecture_);
    form->addRow(QStringLiteral("APT package"), aptPackageName_);
    form->addRow(QStringLiteral("RPM architecture"), rpmArchitecture_);
    form->addRow(QStringLiteral("RPM package"), rpmPackageName_);
    form->addRow(QStringLiteral("Signing key URL"), keyUrlRow);
    form->addRow(QStringLiteral("Trusted signing key"), aptSigningKey_);
    form->addRow(QStringLiteral("Keyring file"), keyringRow);
    aptSigningFingerprint_ = new QLabel(page);
    aptSigningFingerprint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    aptSigningFingerprint_->setWordWrap(true);
    form->addRow(QStringLiteral("Pinned fingerprint"), aptSigningFingerprint_);
    form->addRow(QStringLiteral("GitHub owner"), githubOwner_);
    form->addRow(QStringLiteral("GitHub repository"), githubRepository_);
    form->addRow(QStringLiteral("Asset-name regex"), githubAssetRegex_);
    form->addRow(QString{}, githubPrereleases_);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    updateNotice_ = new QLabel(page);
    updateNotice_->setWordWrap(true);
    updateCandidates_ = new QListWidget(page);
    updateSaveButton_ = new QPushButton(QStringLiteral("Save Update Configuration"), page);
    updateCheckButton_ = new QPushButton(QStringLiteral("Check for Updates"), page);
    updateCheckStatus_ = new QLabel(page);
    updateCheckStatus_->setWordWrap(true);
    layout->addLayout(form);
    layout->addWidget(updateNotice_);
    layout->addWidget(new QLabel(QStringLiteral("Detected repository/update candidates"), page));
    layout->addWidget(updateCandidates_, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(updateSaveButton_);
    buttons->addWidget(updateCheckButton_);
    buttons->addStretch();
    layout->addLayout(buttons);
    layout->addWidget(updateCheckStatus_);
    connect(updateSaveButton_, &QPushButton::clicked, this, &MainWindow::saveUpdateConfiguration);
    connect(updateCheckButton_, &QPushButton::clicked, this, &MainWindow::startUpdateCheck);
    connect(keyringBrowse, &QPushButton::clicked, this, &MainWindow::importSigningKey);
    connect(aptSigningKeyDownloadButton_, &QPushButton::clicked,
            this, &MainWindow::downloadSigningKey);
    connect(keyringPaste, &QPushButton::clicked, this, [this] {
        auto *tracker = updateEditorRelease();
        if (!project_ || tracker == nullptr) return;
        bool accepted = false;
        const auto text = QInputDialog::getMultiLineText(
            this, QStringLiteral("Paste OpenPGP public key"),
            QStringLiteral("Paste an armored public key or Base64-encoded binary public key:"), {}, &accepted);
        if (!accepted || text.trimmed().isEmpty()) return;
        QByteArray contents = text.trimmed().toUtf8();
        if (!contents.startsWith("-----BEGIN PGP")) {
            const auto decoded = QByteArray::fromBase64(contents, QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded.isEmpty()) contents = decoded;
        }
        QString error;
        const auto key = RepositoryTrust::importUserKey(library_.releasePath(*tracker), contents,
                                                        QStringLiteral("user-pasted key"), &error);
        if (!key) {
            QMessageBox::critical(this, QStringLiteral("Could not import signing key"), error);
            return;
        }
        const auto duplicate = std::find_if(tracker->update.signingKeys.cbegin(),
                                            tracker->update.signingKeys.cend(),
                                            [&](const auto &candidate) {
                                                return candidate.sha256 == key->sha256;
                                            });
        if (duplicate == tracker->update.signingKeys.cend()) {
            tracker->update.signingKeys.append(*key);
        }
        tracker->update.aptSigningKeyring = key->relativePath;
        tracker->update.trustedSigningFingerprint = key->fingerprints.first();
        tracker->fieldProvenance.insert(QStringLiteral("update.aptSigningKeyring"), key->provenance);
        tracker->fieldProvenance.insert(QStringLiteral("update.trustedSigningFingerprint"), key->provenance);
        persistCurrent();
        populateUpdates();
    });
    connect(aptSigningKey_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        auto *tracker = updateEditorRelease();
        if (populating_ || !project_ || tracker == nullptr || index < 0 ||
            index >= tracker->update.signingKeys.size()) return;
        const auto &key = tracker->update.signingKeys.at(index);
        aptSigningKeyring_->setText(key.relativePath);
        aptSigningFingerprint_->setText(key.fingerprints.join(QStringLiteral("\n")));
        const QUrl sourceUrl(key.sourcePath);
        aptSigningKeyUrl_->setText(isAcceptableRepositoryKeyUrl(sourceUrl)
                                       ? sourceUrl.toString() : QString{});
    });
    const auto updateStrategyUi = [this, form, keyringRow, keyUrlRow](const int index) {
        const bool hasRelease = updateEditorRelease() != nullptr;
        const bool direct = index == 1;
        const bool apt = index == 2;
        const bool rpm = index == 3;
        const bool repository = apt || rpm;
        const bool github = index == 4;
        updateUrl_->setEnabled(hasRelease && index != 0);
        form->setRowVisible(directUrlFullCheckInterval_, direct);
        directUrlFullCheckInterval_->setEnabled(hasRelease && direct);
        form->setRowVisible(aptSuite_, apt);
        form->setRowVisible(aptComponent_, apt);
        form->setRowVisible(aptArchitecture_, apt);
        form->setRowVisible(aptPackageName_, apt);
        form->setRowVisible(rpmArchitecture_, rpm);
        form->setRowVisible(rpmPackageName_, rpm);
        form->setRowVisible(keyUrlRow, repository);
        form->setRowVisible(aptSigningKey_, repository);
        form->setRowVisible(keyringRow, repository);
        form->setRowVisible(aptSigningFingerprint_, repository);
        aptSuite_->setEnabled(hasRelease && apt);
        aptComponent_->setEnabled(hasRelease && apt);
        aptArchitecture_->setEnabled(hasRelease && apt);
        aptPackageName_->setEnabled(hasRelease && apt);
        rpmArchitecture_->setEnabled(hasRelease && rpm);
        rpmPackageName_->setEnabled(hasRelease && rpm);
        const bool keyDownloadAvailable = hasRelease && repository &&
                                          !signingKeyDownloadService_.isRunning();
        keyUrlRow->setEnabled(keyDownloadAvailable);
        aptSigningKeyUrl_->setEnabled(keyDownloadAvailable);
        aptSigningKeyDownloadButton_->setEnabled(keyDownloadAvailable);
        aptSigningKey_->setEnabled(hasRelease && repository);
        keyringRow->setEnabled(hasRelease && repository);
        form->setRowVisible(githubOwner_, github);
        form->setRowVisible(githubRepository_, github);
        form->setRowVisible(githubAssetRegex_, github);
        form->setRowVisible(githubPrereleases_, github);
        githubOwner_->setEnabled(hasRelease && github);
        githubRepository_->setEnabled(hasRelease && github);
        githubAssetRegex_->setEnabled(hasRelease && github);
        githubPrereleases_->setEnabled(hasRelease && github);
        updateSaveButton_->setEnabled(hasRelease);
        syncUpdateCheckButtons();
        updateNotice_->setText(index == 0 ? QStringLiteral("Manual updates: PacSmith will not query the network.")
                              : index == 1 ? QStringLiteral("Direct URL checks use HTTP validators first. Servers without usable validators require a scheduled full download and SHA256 comparison.")
                              : apt ? QStringLiteral("APT checks compare verified Packages metadata with the active release.")
                              : rpm ? QStringLiteral("RPM checks verify repomd.xml, its primary metadata checksum, and the selected package SHA256 before accepting an update.")
                                    : QStringLiteral("GitHub checks ignore drafts, use stable releases by default, and require exactly one matching asset."));
    };
    connect(updateStrategy_, &QComboBox::currentIndexChanged, this, updateStrategyUi);
    updateStrategyUi(updateStrategy_->currentIndex());
    connect(updateCandidates_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
                auto *tracker = updateEditorRelease();
                if (!project_ || tracker == nullptr) return;
                const auto candidateIndex = item->data(Qt::UserRole).toInt();
                const auto kind = item->data(Qt::UserRole + 1).toString();
                if (kind == QStringLiteral("rpm") && candidateIndex >= 0 &&
                    candidateIndex < tracker->update.rpmCandidates.size()) {
                    const auto &candidate = tracker->update.rpmCandidates.at(candidateIndex);
                    updateStrategy_->setCurrentIndex(3);
                    updateUrl_->setText(candidate.baseUrl);
                    rpmArchitecture_->setText(candidate.architecture);
                    rpmPackageName_->setText(tracker->debian.package);
                    if (!candidate.keyUrls.isEmpty()) aptSigningKeyUrl_->setText(candidate.keyUrls.first());
                    return;
                }
                if (kind != QStringLiteral("apt") || candidateIndex < 0 ||
                    candidateIndex >= tracker->update.aptCandidates.size()) {
                    updateUrl_->setText(item->text());
                    return;
                }
                const auto &candidate = tracker->update.aptCandidates.at(candidateIndex);
                updateStrategy_->setCurrentIndex(2);
                updateUrl_->setText(candidate.uri);
                aptSuite_->setText(candidate.suite);
                if (!candidate.components.isEmpty()) aptComponent_->setText(candidate.components.first());
                if (!candidate.architectures.isEmpty()) aptArchitecture_->setText(candidate.architectures.first());
            });
    return page;
}

QWidget *MainWindow::createBuildPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Build the Arch package, then install it with pacman."),
        page,
        QStringLiteral("Guided packages use the library host directly. Custom PKGBUILDs use rootless Podman with their declared dependencies and project compiler cache. Progress and command output open in a dialog.")));
    buildChecklist_ = new QLabel(page);
    buildChecklist_->setWordWrap(true);
    compileCachePolicy_ = new QComboBox(page);
    compileCachePolicy_->addItem(QStringLiteral("Reuse project compiler cache"),
                                 QStringLiteral("reuse"));
    compileCachePolicy_->addItem(QStringLiteral("Clear project cache after success"),
                                 QStringLiteral("clear_after_success"));
    compileCachePolicy_->addItem(QStringLiteral("Disable compiler cache"),
                                 QStringLiteral("disabled"));
    compileCachePolicy_->setToolTip(QStringLiteral(
        "Custom source builds share one compiler cache across versions of this project."));
    buildButton_ = new QPushButton(QStringLiteral("Build"), page);
    viewBuildOutputButton_ = new QPushButton(QStringLiteral("View Build Output"), page);
    viewBuildOutputButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    viewBuildOutputButton_->setVisible(false);
    installButton_ = new QPushButton(QStringLiteral("Install"), page);
    applyPrimaryActionStyle(installButton_);
    builtPackage_ = new QLabel(page);
    builtPackage_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    builtPackage_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    auto *packageRow = new QHBoxLayout;
    packageRow->setSpacing(16);
    packageRow->addWidget(builtPackage_, 0, Qt::AlignTop);
    packageRow->addWidget(installButton_, 0, Qt::AlignTop);
    packageRow->addStretch(1);
    layout->addWidget(buildChecklist_);
    auto *cacheForm = new QFormLayout;
    cacheForm->addRow(QStringLiteral("Compiler cache"), compileCachePolicy_);
    layout->addLayout(cacheForm);
    auto *buildActions = new QHBoxLayout;
    buildActions->addWidget(buildButton_);
    buildActions->addWidget(viewBuildOutputButton_);
    buildActions->addStretch(1);
    layout->addLayout(buildActions);
    layout->addLayout(packageRow);
    layout->addStretch(1);
    connect(buildButton_, &QPushButton::clicked, this, [this] {
        if (currentRelease() != nullptr && releaseBuildInProgress(currentRelease()->id)) {
            buildButton_->setText(QStringLiteral("Canceling…"));
            buildButton_->setEnabled(false);
            cancelRemoteBuild();
        } else {
            startBuild();
        }
    });
    connect(installButton_, &QPushButton::clicked, this, &MainWindow::startInstall);
    connect(viewBuildOutputButton_, &QPushButton::clicked,
            this, &MainWindow::showBuildOutput);
    connect(compileCachePolicy_, &QComboBox::currentIndexChanged, this, [this] {
        if (populating_ || !project_) return;
        project_->compileCachePolicy = compileCachePolicyFromName(
            compileCachePolicy_->currentData().toString());
        if (persistCurrent()) {
            statusBar()->showMessage(QStringLiteral("Compiler cache policy saved"), 4000);
        }
    });
    return page;
}

QWidget *MainWindow::createRepositoryPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Project-wide repository settings for every retained and future version."),
        page,
        QStringLiteral("Enabling publication adds successful builds to Unstable. The system-wide repository settings determine whether Stable exists; when it does, this project can use manual or automatic promotion. These settings and the published package name belong to the project, not the selected release. Changing a package name after publication is a migration: installed machines keep the old name until they are updated.")));

    repoPublishCheck_ = new QCheckBox(QStringLiteral("Publish this project's builds to the repository"), page);
    layout->addWidget(repoPublishCheck_);
    repoStablePolicy_ = new QWidget(page);
    auto *stablePolicyLayout = new QVBoxLayout(repoStablePolicy_);
    stablePolicyLayout->setContentsMargins(0, 0, 0, 0);
    repoAutomaticSoakCheck_ = new QCheckBox(
        QStringLiteral("Automatically promote unstable builds to stable after the soak period"), page);
    stablePolicyLayout->addWidget(repoAutomaticSoakCheck_);
    auto *soakRow = new QHBoxLayout;
    repoSoakOverrideCheck_ = new QCheckBox(
        QStringLiteral("Use a project-specific soak duration"), page);
    repoSoakDays_ = new QSpinBox(page);
    repoSoakDays_->setRange(0, 3650);
    repoSoakDays_->setSuffix(QStringLiteral(" days"));
    repoSoakDays_->setSpecialValueText(QStringLiteral("Immediate"));
    soakRow->addWidget(repoSoakOverrideCheck_);
    soakRow->addWidget(repoSoakDays_);
    soakRow->addStretch();
    stablePolicyLayout->addLayout(soakRow);
    repoSoakDefaultLabel_ = new QLabel(page);
    repoSoakDefaultLabel_->setWordWrap(true);
    stablePolicyLayout->addWidget(repoSoakDefaultLabel_);
    layout->addWidget(repoStablePolicy_);

    auto *names = new QFormLayout;
    repoOriginalName_ = new QLabel(page);
    repoOriginalName_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    repoPrefixDefault_ = new QLabel(page);
    repoPrefixDefault_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    repoOverrideEdit_ = new QLineEdit(page);
    repoOverrideEdit_->setPlaceholderText(QStringLiteral("Leave blank to use the repository default"));
    repoEffectiveName_ = new QLabel(page);
    repoEffectiveName_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    repoPublishedName_ = new QLabel(page);
    repoPublishedName_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    names->addRow(QStringLiteral("Original package name"), repoOriginalName_);
    names->addRow(QStringLiteral("Repository default"), repoPrefixDefault_);
    names->addRow(QStringLiteral("Package-name override"), repoOverrideEdit_);
    names->addRow(QStringLiteral("Effective published name"), repoEffectiveName_);
    names->addRow(QStringLiteral("Already published as"), repoPublishedName_);
    layout->addLayout(names);

    repoNameWarning_ = new QLabel(page);
    repoNameWarning_->setWordWrap(true);
    repoNameWarning_->setObjectName(QStringLiteral("repoNameWarning"));
    repoNameWarning_->setVisible(false);
    layout->addWidget(repoNameWarning_);

    layout->addWidget(new QLabel(QStringLiteral("Repository channels"), page));
    repoChannelTable_ = new QTableWidget(page);
    repoChannelTable_->setColumnCount(4);
    repoChannelTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Channel"), QStringLiteral("Version"), QStringLiteral("Architecture"),
         QStringLiteral("Promotion status")});
    repoChannelTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    repoChannelTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    repoChannelTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    repoChannelTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    repoChannelTable_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    repoChannelTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    repoChannelTable_->verticalHeader()->setVisible(false);
    repoChannelTable_->setMinimumHeight(120);
    layout->addWidget(repoChannelTable_, 1);

    repoStatusLabel_ = new QLabel(page);
    repoStatusLabel_->setWordWrap(true);
    layout->addWidget(repoStatusLabel_);

    auto *buttons = new QHBoxLayout;
    repoSaveButton_ = new QPushButton(QStringLiteral("Apply Project Repository Settings"), page);
    repoPromoteButton_ = new QPushButton(QStringLiteral("Promote to Stable"), page);
    repoPromoteButton_->setToolTip(
        QStringLiteral("Promote the newest package that would advance stable, bypassing remaining soak time. Stable is never automatically downgraded."));
    buttons->addWidget(repoSaveButton_);
    buttons->addWidget(repoPromoteButton_);
    buttons->addStretch();
    layout->addLayout(buttons);

    connect(repoSaveButton_, &QPushButton::clicked, this, &MainWindow::saveProjectRepository);
    connect(repoPromoteButton_, &QPushButton::clicked, this, &MainWindow::promoteProjectRepository);
    connect(repoOverrideEdit_, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (populating_ || !project_ || repoEffectiveName_ == nullptr) return;
        const auto override = text.trimmed();
        auto effective = override.isEmpty() ? project_->repository.prefixDefault : override;
        if (effective.isEmpty()) effective = project_->archPackageName;
        repoEffectiveName_->setText(effective);
        const auto published = project_->repository.publishedPackageName;
        const bool warn = !published.isEmpty() && published != effective;
        if (repoNameWarning_ != nullptr) {
            repoNameWarning_->setVisible(warn);
            if (warn) {
                repoNameWarning_->setText(
                    QStringLiteral("Changing the published package name is a migration. Machines that already installed %1 will keep that name until they are updated.")
                        .arg(published));
            }
        }
    });
    const auto updateChannelControls = [this] {
        if (repoStablePolicy_ == nullptr || repoAutomaticSoakCheck_ == nullptr ||
            repoSoakOverrideCheck_ == nullptr || repoSoakDays_ == nullptr) return;
        const bool publishing = repoPublishCheck_->isChecked();
        const bool stableAvailable = project_ && project_->repository.stableChannelEnabled;
        repoStablePolicy_->setVisible(stableAvailable);
        repoAutomaticSoakCheck_->setEnabled(publishing && stableAvailable);
        if (!repoAutomaticSoakCheck_->isEnabled()) {
            const QSignalBlocker blocker(repoAutomaticSoakCheck_);
            repoAutomaticSoakCheck_->setChecked(false);
        }
        const bool automatic = repoAutomaticSoakCheck_->isEnabled() &&
                               repoAutomaticSoakCheck_->isChecked();
        repoSoakOverrideCheck_->setEnabled(automatic);
        repoSoakDays_->setEnabled(automatic && repoSoakOverrideCheck_->isChecked());
        if (repoStatusLabel_ != nullptr && !populating_) {
            repoStatusLabel_->setText(
                QStringLiteral("Project repository settings have unsaved changes."));
        }
    };
    connect(repoPublishCheck_, &QCheckBox::toggled, this,
            [updateChannelControls](const bool) { updateChannelControls(); });
    connect(repoAutomaticSoakCheck_, &QCheckBox::toggled, this,
            [updateChannelControls](const bool) { updateChannelControls(); });
    connect(repoSoakOverrideCheck_, &QCheckBox::toggled, this,
            [updateChannelControls](const bool) { updateChannelControls(); });
    return page;
}

QWidget *MainWindow::createHistoryPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(QStringLiteral("Project creation, imports, builds, and installations."), page));
    historyList_ = new QListWidget(page);
    layout->addWidget(historyList_, 1);
    return page;
}


} // namespace pacsmith::gui
