#include "gui/main_window/common.hpp"

namespace pacsmith::gui {

void MainWindow::populateSourceOverview() {
    if (!project_ || currentRelease() == nullptr || sourceTypeHeadline_ == nullptr) return;
    const auto &release = *currentRelease();
    sourceTypeHeadline_->setText(
        QStringLiteral("<h2>%1</h2>").arg(sourcePackageTypeTitle(release.sourceType).toHtmlEscaped()));
    applySourcePackageTypeHelp(sourceTypeExplanation_, release.sourceType);
    const auto location = !release.acquisition.originalUrl.isEmpty()
        ? release.acquisition.originalUrl
        : !release.sourceUrl.isEmpty() ? release.sourceUrl : release.originalSourceFilename;
    QString acquisition = QStringLiteral("<b>Acquired from:</b> %1")
                              .arg(acquisitionKindTitle(release.acquisition.kind).toHtmlEscaped());
    if (!location.isEmpty()) {
        acquisition += QStringLiteral("<br>%1").arg(location.toHtmlEscaped());
    }
    if (release.acquisition.kind == AcquisitionKind::GitHubRelease &&
        !release.acquisition.githubOwner.isEmpty()) {
        acquisition += QStringLiteral("<br>%1/%2")
                           .arg(release.acquisition.githubOwner.toHtmlEscaped(),
                                release.acquisition.githubRepository.toHtmlEscaped());
        if (!release.acquisition.githubTag.isEmpty()) {
            acquisition += QStringLiteral(" · %1").arg(release.acquisition.githubTag.toHtmlEscaped());
        }
        if (!release.acquisition.githubAssetName.isEmpty()) {
            acquisition += QStringLiteral("<br>Asset: %1")
                               .arg(release.acquisition.githubAssetName.toHtmlEscaped());
        }
    }
    sourceAcquisitionDetail_->setText(acquisition);
    QString identity = QStringLiteral("<b>File:</b> %1")
                           .arg(release.originalSourceFilename.isEmpty()
                                    ? QStringLiteral("(none recorded)")
                                    : release.originalSourceFilename.toHtmlEscaped());
    if (!release.sourceSha256.isEmpty()) {
        identity += QStringLiteral("<br><b>SHA256:</b> %1").arg(release.sourceSha256.toHtmlEscaped());
    }
    if (!release.acquisition.publisherDigest.isEmpty()) {
        identity += QStringLiteral("<br><b>Publisher digest:</b> %1%2")
                        .arg(release.acquisition.publisherDigest.toHtmlEscaped(),
                             release.acquisition.publisherVerified
                                 ? QStringLiteral(" (verified)")
                                 : QStringLiteral(" (recorded, not verified)"));
    } else if (release.acquisition.kind != AcquisitionKind::LocalFile) {
        identity += QStringLiteral("<br>No publisher checksum; the local SHA256 is recorded and the source remains unsigned.");
    }
    sourceIdentityDetail_->setText(identity);
    const auto pendingScripts = pendingScriptFindings(release);
    const auto pendingPayload = pendingPayloadReviews(release);
    QStringList inventory;
    inventory.append(QStringLiteral("%1 payload file(s)").arg(release.payload.size()));
    inventory.append(QStringLiteral("%1 vendor script(s)").arg(release.maintainerScripts.size()));
    inventory.append(QStringLiteral("%1 dependency group(s)").arg(release.dependencies.size()));
    if (pendingScripts > 0) {
        inventory.append(QStringLiteral("%1 script responsibility item(s) need configuration")
                             .arg(pendingScripts));
    }
    if (pendingPayload > 0) {
        inventory.append(QStringLiteral("%1 payload path(s) need a keep/exclude decision")
                             .arg(pendingPayload));
    }
    if (release.installMapping.appRun.requiresReview()) {
        inventory.append(QStringLiteral("AppRun needs review"));
    }
    sourceInventoryDetail_->setText(
        QStringLiteral("<b>Inspected contents:</b> %1").arg(inventory.join(QStringLiteral(" · "))));
}

void MainWindow::populatePackage() {
    if (!project_) return;
    const bool archive = currentRelease()->sourceType == SourcePackageType::Archive;
    const bool appImage = currentRelease()->sourceType == SourcePackageType::AppImage;
    const bool elf = currentRelease()->sourceType == SourcePackageType::ElfBinary;
    installMappingWidget_->setVisible(archive || elf);
    archiveLayout_->setEnabled(archive);
    archiveLayout_->setCurrentIndex(
        currentRelease()->installMapping.archiveLayout == ArchiveLayout::OptBundle ? 0 : 1);
    const bool opt = (archive && archiveLayout_->currentIndex() == 0) || appImage;
    installOptDirectory_->setEnabled(opt);
    installBinarySource_->setEnabled(opt);
    installBinaryDestination_->setEnabled(opt || elf);
    installOptDirectory_->setText(currentRelease()->installMapping.optDirectory);
    installCommonPrefix_->setText(currentRelease()->installMapping.commonPrefix);
    installCommonPrefix_->setVisible(archive);
    installStripPrefix_->setVisible(archive);
    installStripPrefix_->setChecked(currentRelease()->installMapping.stripCommonPrefix);
    installStripPrefix_->setEnabled(archive && opt &&
                                    !currentRelease()->installMapping.commonPrefix.isEmpty());
    installBinarySource_->setText(currentRelease()->installMapping.binarySourcePath);
    installBinaryDestination_->setText(currentRelease()->installMapping.binaryDestination);
    if (installCommandsHint_ != nullptr) installCommandsHint_->setVisible(archive);
    if (installMappingLayout_ != nullptr) {
        installMappingLayout_->setRowVisible(installBinarySource_, false);
        installMappingLayout_->setRowVisible(installBinaryDestination_, elf);
    }
    const auto &metadata = currentRelease()->debian;
    metadataView_->setPlainText(
        QStringLiteral("Artifact type: %1\nAcquisition: %2\nPackage: %3\nVersion: %4\nArchitecture: %5\nMaintainer: %6\nHomepage: %7\n\nDescription:\n%8\n\nDepends: %9\nPre-Depends: %10\nRecommends: %11\nSuggests: %12\nConflicts: %13\nProvides: %14")
            .arg(sourcePackageTypeTitle(currentRelease()->sourceType),
                 acquisitionKindTitle(currentRelease()->acquisition.kind), metadata.package,
                 metadata.version, metadata.architecture, metadata.maintainer,
                 metadata.homepage, metadata.description, metadata.depends,
                 metadata.preDepends, metadata.recommends, metadata.suggests,
                 metadata.conflicts, metadata.provides));
    QStringList raw;
    for (auto iterator = metadata.rawFields.cbegin(); iterator != metadata.rawFields.cend(); ++iterator) {
        QString value = iterator.value();
        value.replace(QLatin1Char('\n'), QStringLiteral("\n "));
        raw.append(iterator.key() + QStringLiteral(": ") + value);
    }
    rawMetadataView_->setPlainText(raw.join(QLatin1Char('\n')));
}

void MainWindow::populateInstallPlan() {
    if (!project_ || currentRelease() == nullptr || appImageInstallPlan_ == nullptr) return;
    const auto &release = *currentRelease();
    const auto pkgbuild = currentPkgbuildText();
    const auto plan = PkgbuildInstallPlan::parse(pkgbuild, release);
    if (installPlanNotice_ != nullptr) {
        QString summary = release.pkgbuildManuallyModified
            ? QStringLiteral("Filesystem parsed from the Custom PKGBUILD.")
            : QStringLiteral("Filesystem parsed from the Guided PKGBUILD.");
        if (!plan.warnings.isEmpty()) {
            summary += QLatin1Char(' ') + plan.warnings.join(QLatin1Char(' '));
        }
        setSettingsSectionHelp(
            installPlanNotice_, summary,
            QStringLiteral("Expand a directory to see each installed path, its source, and its purpose."));
    }
    appImageInstallPlan_->clear();
    QHash<QString, QTreeWidgetItem *> nodes;
    const auto excludedColor = QColor(150, 150, 150);
    for (const auto &entry : plan.entries) {
        addInstallPlanEntry(appImageInstallPlan_, &nodes, entry.path, entry.source, entry.purpose,
                            entry.excluded ? excludedColor : QColor{});
    }
    for (int row = 0; row < appImageInstallPlan_->topLevelItemCount(); ++row) {
        auto *item = appImageInstallPlan_->topLevelItem(row);
        decorateInstallPlanTree(item);
        item->setExpanded(true);
    }
    appImageInstallPlan_->sortItems(0, Qt::AscendingOrder);
}

void MainWindow::saveInstallMapping() {
    if (!project_ || currentRelease() == nullptr) return;
    auto &release = *currentRelease();
    if (release.sourceType != SourcePackageType::Archive &&
        release.sourceType != SourcePackageType::AppImage &&
        release.sourceType != SourcePackageType::ElfBinary) return;
    const auto destination = installBinaryDestination_->text().trimmed();
    static const QRegularExpression commandPath(
        QStringLiteral("^/usr/bin/[A-Za-z0-9@._+\\-]+$"));
    if (release.sourceType == SourcePackageType::ElfBinary && !destination.isEmpty() &&
        !commandPath.match(destination).hasMatch()) {
        QMessageBox::warning(this, QStringLiteral("Unsafe command destination"),
                             QStringLiteral("The command destination must be a simple absolute path below /usr/bin."));
        return;
    }
    if ((release.sourceType == SourcePackageType::Archive && archiveLayout_->currentIndex() == 0) ||
        release.sourceType == SourcePackageType::AppImage) {
        static const QRegularExpression optName(QStringLiteral("^[A-Za-z0-9@._+\\-]+$"));
        const auto opt = installOptDirectory_->text().trimmed();
        if (!optName.match(opt).hasMatch()) {
            QMessageBox::warning(this, QStringLiteral("Unsafe /opt directory"),
                                 QStringLiteral("Use a single directory name containing letters, digits, '.', '_', '+', '@', or '-'."));
            return;
        }
        release.installMapping.optDirectory = opt;
        if (release.sourceType == SourcePackageType::Archive) {
            release.installMapping.stripCommonPrefix = installStripPrefix_->isChecked();
        }
    }
    release.installMapping.archiveLayout = archiveLayout_->currentIndex() == 0
        ? ArchiveLayout::OptBundle : ArchiveLayout::PreserveRoot;
    if (release.sourceType == SourcePackageType::ElfBinary) {
        release.installMapping.binaryDestination = destination;
        if (!release.installMapping.launchers.isEmpty() && !destination.isEmpty()) {
            release.installMapping.launchers.first().destination = destination;
            release.installMapping.launchers.first().commandName = QFileInfo(destination).fileName();
        }
    }
    refreshGeneratedPkgbuildAfterModelChange();
    populatePackage();
    statusBar()->showMessage(QStringLiteral("Install mapping saved and generated PKGBUILD refreshed"), 6000);
}

void MainWindow::populateDependencies() {
    if (!project_) return;
    dependenciesTable_->horizontalHeaderItem(0)->setText(
        currentRelease()->sourceType == SourcePackageType::Debian
            ? QStringLiteral("Debian dependency")
            : currentRelease()->sourceType == SourcePackageType::Rpm
                ? QStringLiteral("RPM requirement") : QStringLiteral("Source dependency"));
    populating_ = true;
    QStringList packagesToValidate;
    dependenciesTable_->setRowCount(static_cast<int>(currentRelease()->dependencies.size()));
    for (int row = 0; row < static_cast<int>(currentRelease()->dependencies.size()); ++row) {
        const auto &dependency = currentRelease()->dependencies.at(row);
        auto *debianItem = new QTableWidgetItem(dependency.rawExpression);
        debianItem->setFlags(debianItem->flags() & ~Qt::ItemIsEditable);
        auto *archItem = new QTableWidgetItem(dependency.archPackage);
        const bool requiresRepositoryPackage =
            !dependency.ignored && !dependency.bundled && !dependency.provided &&
            dependency.status != MappingStatus::Ignored &&
            dependency.status != MappingStatus::Bundled &&
            dependency.status != MappingStatus::Provided &&
            !dependency.archPackage.isEmpty();
        if (requiresRepositoryPackage && repositoryPackages_.contains(dependency.archPackage)) {
            repositoryDependencyAvailability_.insert(dependency.archPackage, true);
        } else if (requiresRepositoryPackage && repositoryCatalogLoaded_ &&
                   !repositoryDependencyAvailability_.contains(dependency.archPackage) &&
                   !repositoryDependencyChecksPending_.contains(dependency.archPackage)) {
            packagesToValidate.append(dependency.archPackage);
        }
        const bool availabilityKnown = repositoryDependencyAvailability_.contains(
            dependency.archPackage);
        const bool unavailable = requiresRepositoryPackage && availabilityKnown &&
                                 !repositoryDependencyAvailability_.value(dependency.archPackage);
        const bool unresolved = dependency.status == MappingStatus::Unresolved;
        const auto visibleStatus = unavailable
            ? QStringLiteral("Unavailable")
            : requiresRepositoryPackage && !availabilityKnown
                ? QStringLiteral("Checking repository…")
                : mappingStatusName(dependency.status);
        auto *statusItem = new QTableWidgetItem(visibleStatus);
        statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);
        const auto sourceText = dependency.mappingSource.isEmpty()
                                    ? QStringLiteral("—")
                                    : QStringLiteral("%1 (%2%)").arg(dependency.mappingSource)
                                          .arg(static_cast<int>(dependency.confidence * 100.0));
        auto *sourceItem = new QTableWidgetItem(sourceText);
        sourceItem->setFlags(sourceItem->flags() & ~Qt::ItemIsEditable);
        const bool aiGenerated = currentRelease()->fieldProvenance
                                     .value(QStringLiteral("dependency.%1.archPackage").arg(row))
                                     .origin == ValueOrigin::Ai ||
                                 currentRelease()->fieldProvenance
                                     .value(QStringLiteral("dependency.%1.treatment").arg(row))
                                     .origin == ValueOrigin::Ai;
        if (unavailable) {
            const auto explanation = QStringLiteral(
                "'%1' is not present in any configured pacman sync repository. Choose a suggested package name or leave this dependency unresolved.")
                                         .arg(dependency.archPackage);
            for (auto *item : {debianItem, archItem, statusItem, sourceItem}) {
                item->setBackground(QColor(125, 35, 35));
                item->setForeground(Qt::white);
                item->setToolTip(explanation);
            }
        } else if (unresolved) {
            const auto explanation = QStringLiteral(
                "No Arch package or explicit treatment has resolved this dependency. Human review is required.");
            for (auto *item : {debianItem, archItem, statusItem, sourceItem}) {
                item->setBackground(QColor(112, 82, 12));
                item->setForeground(Qt::white);
                item->setToolTip(explanation);
            }
        } else if (aiGenerated) {
            for (auto *item : {debianItem, archItem, statusItem, sourceItem}) {
                item->setBackground(QColor(35, 90, 55));
                item->setForeground(Qt::white);
            }
        }
        dependenciesTable_->setItem(row, 0, debianItem);
        dependenciesTable_->setItem(row, 1, archItem);
        dependenciesTable_->setItem(row, 2, statusItem);
        dependenciesTable_->setItem(row, 3, sourceItem);
        auto *treatment = new QComboBox(dependenciesTable_);
        treatment->addItems({QStringLiteral("Require Arch package"), QStringLiteral("Provided by this package"),
                             QStringLiteral("Bundled inside this package"), QStringLiteral("Ignore")});
        if (dependency.provided || dependency.status == MappingStatus::Provided) treatment->setCurrentIndex(1);
        else if (dependency.bundled || dependency.status == MappingStatus::Bundled) treatment->setCurrentIndex(2);
        else if (dependency.ignored || dependency.status == MappingStatus::Ignored) treatment->setCurrentIndex(3);
        if (unavailable) {
            treatment->setStyleSheet(QStringLiteral("QComboBox { background: #7d2323; color: white; }"));
            treatment->setToolTip(statusItem->toolTip());
        } else if (unresolved) {
            treatment->setStyleSheet(QStringLiteral("QComboBox { background: #70520c; color: white; }"));
            treatment->setToolTip(statusItem->toolTip());
        } else if (aiGenerated) {
            treatment->setStyleSheet(QStringLiteral("QComboBox { background: #235a37; color: white; }"));
        }
        connect(treatment, &QComboBox::currentIndexChanged, this,
                [this, row](const int index) { dependencyDispositionChanged(row, index); });
        dependenciesTable_->setCellWidget(row, 4, treatment);
    }
    populating_ = false;
    scheduleRepositoryPackageValidation(packagesToValidate);
}

void MainWindow::loadRepositoryPackageCatalog() {
    repositoryCatalogLoaded_ = false;
    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher] {
        repositoryPackageNames_ = watcher->result();
        watcher->deleteLater();
        repositoryPackages_ = QSet<QString>(repositoryPackageNames_.cbegin(),
                                            repositoryPackageNames_.cend());
        for (const auto &package : repositoryPackageNames_) {
            repositoryDependencyAvailability_.insert(package, true);
        }
        repositoryCatalogLoaded_ = !repositoryPackageNames_.isEmpty();
        if (dependenciesTable_ != nullptr) {
            dependenciesTable_->setProperty("pacsmithRepositoryPackages",
                                            repositoryPackageNames_);
        }
        if (project_ && currentRelease() != nullptr && stageTabs_ != nullptr &&
            rightStack_->currentIndex() == 1) {
            if (isSectionActive(EditorSection::ConfigDependencies)) populateDependencies();
            else if (isSectionActive(EditorSection::ResultBuild)) populateBuild();
        } else if (project_ && rightStack_->currentIndex() == 0) {
            populateOverview();
        }
        if (!repositoryCatalogLoaded_) {
            statusBar()->showMessage(
                QStringLiteral("Could not read configured pacman repositories; dependency availability checks are unavailable"),
                10000);
        }
    });
    watcher->setFuture(QtConcurrent::run([] {
        return SystemInformationBroker::repositoryPackageNames();
    }));
}

void MainWindow::scheduleRepositoryPackageValidation(const QStringList &packages) {
    auto pending = packages;
    pending.removeDuplicates();
    if (pending.isEmpty()) return;
    for (const auto &package : pending) repositoryDependencyChecksPending_.insert(package);
    auto *watcher = new QFutureWatcher<QHash<QString, bool>>(this);
    connect(watcher, &QFutureWatcher<QHash<QString, bool>>::finished, this,
            [this, watcher] {
        const auto results = watcher->result();
        watcher->deleteLater();
        for (auto iterator = results.cbegin(); iterator != results.cend(); ++iterator) {
            repositoryDependencyAvailability_.insert(iterator.key(), iterator.value());
            repositoryDependencyChecksPending_.remove(iterator.key());
        }
        if (project_ && currentRelease() != nullptr && rightStack_->currentIndex() == 1) {
            if (isSectionActive(EditorSection::ConfigDependencies)) populateDependencies();
            else if (isSectionActive(EditorSection::ResultBuild)) populateBuild();
        } else if (project_ && rightStack_->currentIndex() == 0) {
            populateOverview();
        }
    });
    watcher->setFuture(QtConcurrent::run([pending] {
        QHash<QString, bool> results;
        for (const auto &package : pending) {
            const auto result = SystemInformationBroker::execute(
                {QStringLiteral("pacsmith-dependency-editor"),
                 QStringLiteral("repository-package"), package,
                 QStringLiteral("Validate a required dependency mapping")});
            results.insert(package, result.value(QStringLiteral("available")).toBool());
        }
        return results;
    }));
}

void MainWindow::populateScripts() {
    if (!project_) return;
    const auto unresolvedResponsibilities =
        std::count_if(currentRelease()->scriptFindings.cbegin(), currentRelease()->scriptFindings.cend(),
                      [this](const auto &finding) {
            return findingRequiresArchAction(*currentRelease(), finding);
        });
    const auto &lifecycle = currentRelease()->lifecycleScript;
    if (lifecycleEditing_) {
        scriptsActionNotice_->setText(
            QStringLiteral("Editing a user-authored Arch lifecycle script. Save validates the draft and binds it to the currently lifecycle-required responsibilities. A valid saved script must still be approved before installation."));
        scriptsActionNotice_->setStyleSheet(QStringLiteral(
            "background: rgba(52,152,219,24); border: 1px solid #347fa8; border-radius: 5px;"));
    } else if (!lifecycle.contents.isEmpty() && lifecycle.validationPassed &&
        lifecycle.requiresAcknowledgement()) {
        scriptsActionNotice_->setText(
            QStringLiteral("Action required before installation: review the exact .install script below, then approve it. That is the script pacman will run as root. Vendor scripts on the imported package tab are reference-only."));
        scriptsActionNotice_->setStyleSheet(QStringLiteral(
            "background: rgba(229,185,61,28); border: 1px solid #b89624; border-radius: 5px;"));
    } else if (unresolvedResponsibilities > 0) {
        scriptsActionNotice_->setText(
            QStringLiteral("⚠ %1 extracted script responsibility item(s) still need an Arch-specific resolution. Vendor script text is on the imported package tab → Vendor scripts; reading it does not create an Arch action.")
                .arg(unresolvedResponsibilities));
        scriptsActionNotice_->setStyleSheet(QStringLiteral(
            "background: rgba(229,185,61,28); border: 1px solid #b89624; border-radius: 5px;"));
    } else {
        scriptsActionNotice_->setText(
            lifecycle.contents.isEmpty()
                ? QStringLiteral("✓ No action required. All extracted responsibilities are handled, and no privileged Arch lifecycle script is needed. Vendor scripts remain on the imported package tab for reference.")
                : QStringLiteral("✓ All extracted responsibilities are handled and the exact generated Arch lifecycle script has been approved. Vendor scripts remain on the imported package tab for reference."));
        scriptsActionNotice_->setStyleSheet(QStringLiteral(
            "background: rgba(85,204,119,24); border: 1px solid #3f8f58; border-radius: 5px;"));
    }
    scriptFindingsTable_->setRowCount(static_cast<int>(currentRelease()->scriptFindings.size()));
    for (int row = 0; row < static_cast<int>(currentRelease()->scriptFindings.size()); ++row) {
        const auto &finding = currentRelease()->scriptFindings.at(row);
        auto *scriptItem = new QTableWidgetItem(finding.scriptName);
        auto *summaryItem = new QTableWidgetItem(finding.summary);
        auto *statusItem = new QTableWidgetItem(scriptDispositionName(finding.disposition));
        const auto provenanceName = valueOriginName(finding.provenance.origin);
        auto *provenanceItem = new QTableWidgetItem(
            finding.provenance.origin == ValueOrigin::Ai
                ? QStringLiteral("AI · %1/%2").arg(finding.provenance.provider, finding.provenance.model)
                : provenanceName);
        if (finding.provenance.origin == ValueOrigin::Ai) {
            for (auto *item : {scriptItem, summaryItem, statusItem, provenanceItem}) {
                item->setBackground(QColor(35, 90, 55));
                item->setForeground(Qt::white);
            }
        }
        const bool findingNeedsReview = finding.disposition == ScriptDisposition::Unresolved ||
            (finding.disposition == ScriptDisposition::LifecycleRequired &&
             (!currentRelease()->lifecycleScript.validationPassed ||
              !currentRelease()->lifecycleScript.sourceFingerprints.contains(finding.evidenceFingerprint)));
        const auto sourceScript = std::find_if(currentRelease()->maintainerScripts.cbegin(),
                                               currentRelease()->maintainerScripts.cend(),
                                               [&](const auto &candidate) {
                                                   return candidate.name == finding.scriptName;
                                               });
        const bool acknowledgedFallback = sourceScript != currentRelease()->maintainerScripts.cend() &&
                                          !sourceScript->requiresReview();
        if (findingNeedsReview && !acknowledgedFallback) {
            statusItem->setBackground(QColor(120, 92, 0));
            statusItem->setForeground(Qt::white);
        } else if (findingNeedsReview) {
            statusItem->setToolTip(QStringLiteral(
                "No automatic Arch action was created; the user acknowledged the exact original script as reviewed."));
        }
        scriptFindingsTable_->setItem(row, 0, scriptItem);
        scriptFindingsTable_->setItem(row, 1, summaryItem);
        scriptFindingsTable_->setItem(row, 2, statusItem);
        scriptFindingsTable_->setItem(row, 3, provenanceItem);
    }
    scriptsList_->clear();
    for (const auto &script : currentRelease()->maintainerScripts) {
        const auto unresolved = unresolvedResponsibilitiesForScript(*currentRelease(), script.name);
        QString label;
        if (unresolved == 0) {
            label = QStringLiteral("✓ %1    Responsibilities resolved").arg(script.name);
        } else if (!script.requiresReview()) {
            label = QStringLiteral("✓ %1    Original accepted by user").arg(script.name);
        } else {
            label = QStringLiteral("⚠ %1    %2 item(s) need resolution").arg(script.name).arg(unresolved);
        }
        auto *item = new QListWidgetItem(label, scriptsList_);
        if (unresolved > 0 && script.requiresReview()) item->setForeground(QColor(Qt::darkYellow));
        item->setToolTip(script.requiresReview()
                             ? QStringLiteral("Original package-script source has not been marked as read; this is optional once its responsibilities are resolved.")
                             : QStringLiteral("Acknowledgment is bound to this exact original-script content."));
    }
    if (scriptsList_->count() > 0) scriptsList_->setCurrentRow(0);
    else {
        scriptView_->setPlainText(QStringLiteral("No imported package lifecycle scripts were detected."));
        scriptStatus_->clear();
        acknowledgeScriptButton_->setEnabled(false);
    }
    if (lifecycleEditing_) {
        lifecycleView_->setReadOnly(false);
        lifecycleStatus_->setText(
            QStringLiteral("Editing draft · Allowed functions: pre_install, post_install, pre_upgrade, post_upgrade, pre_remove, and post_remove. Network access, package-manager recursion, privilege elevation, dynamic evaluation, and command substitution are blocked."));
        editLifecycleButton_->setEnabled(false);
        saveLifecycleButton_->setVisible(true);
        saveLifecycleButton_->setEnabled(true);
        cancelLifecycleButton_->setVisible(true);
        cancelLifecycleButton_->setEnabled(true);
        acknowledgeLifecycleButton_->setEnabled(false);
        discardLifecycleButton_->setEnabled(false);
        return;
    }

    lifecycleView_->setReadOnly(true);
    editLifecycleButton_->setEnabled(true);
    editLifecycleButton_->setText(lifecycle.contents.isEmpty()
                                      ? QStringLiteral("Create Lifecycle Script")
                                      : QStringLiteral("Edit Lifecycle Script"));
    saveLifecycleButton_->setVisible(false);
    cancelLifecycleButton_->setVisible(false);
    const auto lifecycleOrigin = lifecycle.provenance.origin == ValueOrigin::Ai
                                     ? QStringLiteral("<span style='color:#55cc77'>AI-generated</span>")
                                 : lifecycle.provenance.origin == ValueOrigin::User
                                     ? QStringLiteral("User-authored")
                                     : QStringLiteral("Generated");
    lifecycleView_->setPlainText(lifecycle.contents.isEmpty()
                                     ? QStringLiteral("No Arch lifecycle script is configured. PacSmith will rely on normal package files and Arch hooks.")
                                     : lifecycle.contents);
    if (lifecycle.contents.isEmpty()) {
        const auto lifecycleNeeded = std::count_if(
            currentRelease()->scriptFindings.cbegin(), currentRelease()->scriptFindings.cend(),
            [](const auto &finding) {
                return finding.disposition == ScriptDisposition::LifecycleRequired;
            });
        lifecycleStatus_->setText(
            lifecycleNeeded > 0
                ? QStringLiteral("⚠ No Arch lifecycle script is configured, but %1 responsibility item(s) are marked lifecycle-required. Create one here or resolve those findings another way.")
                      .arg(lifecycleNeeded)
                : QStringLiteral("✓ No privileged package lifecycle script is needed. The generated PKGBUILD therefore has no install= entry."));
        acknowledgeLifecycleButton_->setEnabled(false);
        discardLifecycleButton_->setEnabled(false);
    } else if (!lifecycle.validationPassed) {
        lifecycleStatus_->setText(QStringLiteral("%1 · ⚠ content is blocked by validation: %2")
                                      .arg(lifecycleOrigin, lifecycle.validationMessage.toHtmlEscaped()));
        acknowledgeLifecycleButton_->setEnabled(false);
        discardLifecycleButton_->setEnabled(true);
    } else if (lifecycle.requiresAcknowledgement()) {
        const auto integration = currentRelease()->pkgbuildManuallyModified
                                     ? QStringLiteral("⚠ The PKGBUILD is user-owned; add install='%1' or install=\"${_PACSMITH_INSTALL}\" manually or restore the generated PKGBUILD.")
                                           .arg(lifecycle.fileName)
                                     : QStringLiteral("✓ The generated PKGBUILD sets install=\"${_PACSMITH_INSTALL}\" (%1).")
                                           .arg(lifecycle.fileName);
        lifecycleStatus_->setText(QStringLiteral("%1 · "
                                                 "<span style='color:#e5b93d'>⚠ user approval required before installation</span><br>"
                                                 "This is the only script pacman will execute. Vendor maintainer scripts are never this file.<br>%2<br>%3")
                                      .arg(lifecycleOrigin, lifecycle.validationMessage.toHtmlEscaped(), integration));
        acknowledgeLifecycleButton_->setEnabled(true);
        discardLifecycleButton_->setEnabled(true);
    } else {
        const auto integration = currentRelease()->pkgbuildManuallyModified
                                     ? QStringLiteral("⚠ The PKGBUILD is user-owned; verify it contains install='%1' or install=\"${_PACSMITH_INSTALL}\".")
                                           .arg(lifecycle.fileName)
                                     : QStringLiteral("✓ The generated PKGBUILD sets install=\"${_PACSMITH_INSTALL}\" (%1).")
                                           .arg(lifecycle.fileName);
        lifecycleStatus_->setText(QStringLiteral("%1 · ✓ exact privileged content approved<br>%2<br>%3")
                                      .arg(lifecycleOrigin, lifecycle.validationMessage.toHtmlEscaped(), integration));
        acknowledgeLifecycleButton_->setEnabled(false);
        discardLifecycleButton_->setEnabled(true);
    }
}

void MainWindow::updateSelectedScript() {
    const auto row = scriptsList_->currentRow();
    if (!project_ || row < 0 || row >= currentRelease()->maintainerScripts.size()) {
        scriptView_->clear();
        scriptStatus_->clear();
        acknowledgeScriptButton_->setEnabled(false);
        return;
    }
    const auto &script = currentRelease()->maintainerScripts.at(row);
    scriptView_->setPlainText(script.contents);
    const auto unresolved = unresolvedResponsibilitiesForScript(*currentRelease(), script.name);
    if (unresolved == 0) {
        scriptStatus_->setText(
            script.requiresReview()
                ? QStringLiteral("Reference only · Responsibilities are resolved. This imported script will never execute; marking its original source as read is optional.")
                : QStringLiteral("Reference only · Responsibilities are resolved and this exact original source was marked as read."));
    } else if (script.requiresReview()) {
        scriptStatus_->setText(
            QStringLiteral("⚠ %1 responsibility item(s) still need an Arch-specific resolution. Marking the original source as read is an explicit fallback, not a translation of this script.")
                .arg(unresolved));
    } else {
        scriptStatus_->setText(
            QStringLiteral("✓ Exact original source acknowledged by the user as a fallback; %1 responsibility item(s) have no generated Arch action.")
                .arg(unresolved));
    }
    acknowledgeScriptButton_->setEnabled(script.requiresReview());
    acknowledgeScriptButton_->setText(unresolved == 0
                                          ? QStringLiteral("Mark Original Source as Read (Optional)")
                                          : QStringLiteral("Accept Original Source Without Arch Action"));
}

void MainWindow::acknowledgeSelectedScript() {
    const auto row = scriptsList_->currentRow();
    if (!project_ || row < 0 || row >= currentRelease()->maintainerScripts.size()) return;
    auto &script = currentRelease()->maintainerScripts[row];
    const auto unresolvedScriptResponsibilities =
        unresolvedResponsibilitiesForScript(*currentRelease(), script.name);
    if (unresolvedScriptResponsibilities > 0 &&
        QMessageBox::warning(
            this, QStringLiteral("Accept unresolved imported-script responsibilities"),
            QStringLiteral("This original package script will still never be executed or translated. "
                           "Marking it as read tells PacSmith that you deliberately accept %1 unresolved "
                           "responsibility item(s) without an Arch-specific action. The decision applies only "
                           "to this exact script content and resets if it changes. Continue?")
                .arg(unresolvedScriptResponsibilities),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    script.acknowledge();
    currentRelease()->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("script-review"),
                              QStringLiteral("Acknowledged maintainer script %1 (%2)")
                                  .arg(script.name, script.acknowledgedFingerprint)});
    if (!persistCurrent()) return;
    populateScripts();
    scriptsList_->setCurrentRow(row);
    populateOverview();
    populateBuild();
    populateHistory();

    const auto unresolved = std::count_if(currentRelease()->dependencies.cbegin(), currentRelease()->dependencies.cend(),
                                          [](const auto &dependency) {
                                              return dependency.status == MappingStatus::Unresolved;
                                          });
    const auto scriptReviews = pendingScriptFindings(*currentRelease());
    if (auto *item = projectList_->currentItem()) {
        item->setText(project_->displayName + (unresolved > 0 || scriptReviews > 0 ? QStringLiteral("  ⚠") : QString{}));
    }
    statusBar()->showMessage(QStringLiteral("Acknowledged %1; changed content will require review again").arg(script.name),
                             7000);
}

void MainWindow::beginLifecycleEdit() {
    if (!project_ || lifecycleEditing_) return;
    lifecycleEditing_ = true;
    lifecycleView_->setReadOnly(false);
    if (currentRelease()->lifecycleScript.contents.isEmpty()) lifecycleView_->clear();
    else lifecycleView_->setPlainText(currentRelease()->lifecycleScript.contents);
    lifecycleView_->document()->setModified(false);
    populateScripts();
    lifecycleView_->setFocus();
}

void MainWindow::saveLifecycleEdit() {
    if (!project_ || !lifecycleEditing_) return;
    const auto contents = lifecycleView_->toPlainText();
    if (contents.trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Lifecycle script is empty"),
                             QStringLiteral("Enter at least one Arch lifecycle function, or cancel editing. "
                                            "Use Discard Generated Script to remove an existing script."));
        return;
    }

    const auto previous = currentRelease()->lifecycleScript;
    auto &lifecycle = currentRelease()->lifecycleScript;
    lifecycle.fileName = project_->archPackageName + QStringLiteral(".install");
    lifecycle.contents = contents;
    lifecycle.acknowledgedFingerprint.clear();
    lifecycle.sourceFingerprints.clear();
    for (const auto &finding : currentRelease()->scriptFindings) {
        if (finding.disposition == ScriptDisposition::LifecycleRequired) {
            lifecycle.sourceFingerprints.append(finding.evidenceFingerprint);
        }
    }
    lifecycle.sourceFingerprints.removeDuplicates();
    const auto validation = LifecycleValidator::validate(contents);
    lifecycle.validationPassed = validation.passed;
    lifecycle.validationMessage = validation.message();
    lifecycle.provenance = {
        ValueOrigin::User, {}, {}, sha256Hex(contents.toUtf8()),
        QStringLiteral("User-authored Arch lifecycle script saved in PacSmith."),
        QDateTime::currentDateTimeUtc(), true};

    QString error;
    if (!store_.saveLifecycle(*project_, *currentRelease(), &error)) {
        lifecycle = previous;
        QMessageBox::critical(this, QStringLiteral("Could not save lifecycle script"), error);
        return;
    }
    currentRelease()->history.append(
        {QDateTime::currentDateTimeUtc(), QStringLiteral("lifecycle-script"),
         QStringLiteral("Saved user-authored Arch lifecycle script %1 (%2)")
             .arg(lifecycle.fileName,
                  validation.passed ? QStringLiteral("validated") : QStringLiteral("validation blocked"))});
    const auto lifecycleFileName = lifecycle.fileName;
    lifecycleEditing_ = false;
    lifecycleView_->document()->setModified(false);
    refreshGeneratedPkgbuildAfterModelChange();
    persistCurrent();
    populateScripts();
    populateOverview();
    populateBuild();
    populateHistory();

    if (!validation.passed) {
        showDetailedMessageDialog(
            this, QStringLiteral("Lifecycle script saved but blocked"),
            QStringLiteral("The draft was saved for further editing, but PacSmith will not add it to the generated PKGBUILD or permit installation until validation passes."),
            validation.message(), QStyle::SP_MessageBoxWarning, true);
    } else if (currentRelease()->pkgbuildManuallyModified) {
        QMessageBox::warning(
            this, QStringLiteral("Lifecycle script saved; PKGBUILD needs attention"),
            QStringLiteral("The script validated, but Configuration is in Custom mode. Add install='%1' or install=\"${_PACSMITH_INSTALL}\" to the PKGBUILD or switch back to Guided. Then review and approve the exact script before installation.")
                .arg(lifecycleFileName));
    } else {
        statusBar()->showMessage(
            QStringLiteral("Lifecycle script saved and referenced by the generated PKGBUILD; exact-content approval is still required"),
            10000);
    }
    refreshProjectList(project_->id);
}

void MainWindow::cancelLifecycleEdit() {
    if (!lifecycleEditing_) return;
    lifecycleEditing_ = false;
    lifecycleView_->document()->setModified(false);
    populateScripts();
}

void MainWindow::acknowledgeLifecycleScript() {
    if (!project_ || lifecycleEditing_ || currentRelease()->lifecycleScript.contents.isEmpty() ||
        !currentRelease()->lifecycleScript.validationPassed) return;
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Approve generated privileged script"),
        QStringLiteral("Pacman will run this exact Arch lifecycle script as root during package transactions. "
                       "Approve it only after reviewing the complete content. Any change will reset this approval."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    currentRelease()->lifecycleScript.acknowledge();
    currentRelease()->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("lifecycle-review"),
                              QStringLiteral("Acknowledged Arch lifecycle script %1")
                                  .arg(currentRelease()->lifecycleScript.acknowledgedFingerprint)});
    if (!persistCurrent()) return;
    populateScripts();
    populateOverview();
    populateBuild();
    populateHistory();
}

void MainWindow::discardLifecycleScript() {
    if (!project_ || currentRelease()->lifecycleScript.contents.isEmpty()) return;
    if (QMessageBox::warning(
            this, QStringLiteral("Remove lifecycle script"),
            QStringLiteral("This removes the project .install file and prevents its lifecycle actions from running. "
                           "Only continue if those actions are unnecessary or handled another way."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!store_.removeLifecycle(*project_, *currentRelease(), &error)) {
        QMessageBox::critical(this, QStringLiteral("Could not discard lifecycle script"), error);
        return;
    }
    currentRelease()->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("lifecycle-script"),
                              QStringLiteral("Discarded generated Arch lifecycle script")});
    refreshGeneratedPkgbuildAfterModelChange();
    populateScripts();
    populateOverview();
    populateBuild();
    populateHistory();
}


} // namespace pacsmith::gui
