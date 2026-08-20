#include "gui/main_window/common.hpp"

namespace pacsmith::gui {

void MainWindow::populatePkgbuild() {
    if (!project_ || currentRelease() == nullptr) return;
    QString error;
    const auto contents = store_.readPkgbuild(*currentRelease(), &error);
    if (!contents) {
        QMessageBox::critical(this, QStringLiteral("Could not read PKGBUILD"), error);
        return;
    }
    const bool keepDraft = pkgbuildEditor_ != nullptr &&
                           pkgbuildEditor_->document()->isModified() &&
                           isSectionActive(EditorSection::ConfigPkgbuild);
    if (pkgbuildEditor_ != nullptr && !keepDraft) {
        pkgbuildEditor_->setPlainText(*contents);
        pkgbuildEditor_->document()->setModified(false);
    }
    if (pkgbuildPreview_ != nullptr) {
        pkgbuildPreview_->setPlainText(*contents);
    }
    auto state = currentRelease()->pkgbuildManuallyModified
        ? QStringLiteral("⚠ Custom PKGBUILD. Guided configuration is ignored until you switch back.")
        : QStringLiteral("✓ Generated from Guided configuration");
    if (!currentRelease()->previousManualPkgbuild.isEmpty()) {
        state += QStringLiteral("\nThe previous release had a Custom PKGBUILD. PacSmith generated this release fresh and kept the prior text at files/previous-manual-PKGBUILD for reference; it was not merged automatically.");
    }
    if (pkgbuildState_ != nullptr &&
        (pkgbuildEditor_ == nullptr || !pkgbuildEditor_->document()->isModified())) {
        pkgbuildState_->setText(state);
    }
    if (pkgbuildPreviewNotice_ != nullptr) {
        pkgbuildPreviewNotice_->setText(state);
    }
    const auto vars = store_.readIdentityVariables(*currentRelease(), nullptr)
                          .value_or(PkgbuildGenerator::identityVariables(*currentRelease()));
    if (pkgbuildVarsPreview_ != nullptr) pkgbuildVarsPreview_->setPlainText(vars);
    if (resultPkgbuildVarsPreview_ != nullptr) resultPkgbuildVarsPreview_->setPlainText(vars);
    if (isSectionActive(EditorSection::ResultBuild) || buildButton_ != nullptr) populateBuild();
}

QString MainWindow::currentPkgbuildText() const {
    if (pkgbuildEditor_ != nullptr && pkgbuildEditor_->document()->isModified()) {
        return pkgbuildEditor_->toPlainText();
    }
    if (!project_ || currentRelease() == nullptr) {
        return pkgbuildEditor_ != nullptr ? pkgbuildEditor_->toPlainText() : QString{};
    }
    QString error;
    const auto contents = store_.readPkgbuild(*currentRelease(), &error);
    if (contents) return *contents;
    if (pkgbuildPreview_ != nullptr && !pkgbuildPreview_->toPlainText().isEmpty()) {
        return pkgbuildPreview_->toPlainText();
    }
    return pkgbuildEditor_ != nullptr ? pkgbuildEditor_->toPlainText() : QString{};
}

void MainWindow::populateUpdates() {
    if (!project_) return;
    auto *tracker = updateEditorRelease();
    populating_ = true;
    const auto controls = QList<QWidget *>{
        updateStrategy_, updateUrl_, aptSuite_, aptComponent_, aptArchitecture_, aptPackageName_,
        rpmArchitecture_, rpmPackageName_,
        aptSigningKeyUrl_, aptSigningKeyDownloadButton_, aptSigningKeyring_, aptSigningKey_,
        githubOwner_, githubRepository_, githubAssetRegex_,
        githubPrereleases_, githubRegexAiButton_, updateCandidates_, updateSaveButton_};
    if (tracker == nullptr) {
        updateOwnerLabel_->setText(
            QStringLiteral("No release currently owns the project update view. Prepare an artifact first, or reconcile the installed package with a retained PacSmith release."));
        updateStrategy_->setCurrentIndex(0);
        for (auto *control : controls) control->setEnabled(false);
        updateUrl_->clear();
        aptSuite_->clear();
        aptComponent_->clear();
        aptArchitecture_->clear();
        aptPackageName_->clear();
        rpmArchitecture_->clear();
        rpmPackageName_->clear();
        aptSigningKeyUrl_->clear();
        aptSigningKeyring_->clear();
        aptSigningKey_->clear();
        aptSigningFingerprint_->clear();
        githubOwner_->clear();
        githubRepository_->clear();
        githubAssetRegex_->clear();
        githubPrereleases_->setChecked(false);
        updateCandidates_->clear();
        updateNotice_->setText(QStringLiteral("Update configuration is release-owned; there is no project-level source record."));
        updateCheckStatus_->setText(QStringLiteral("No update checks are available."));
        syncUpdateCheckButtons();
        populating_ = false;
        return;
    }
    for (auto *control : controls) control->setEnabled(true);
    const bool installedOwner = project_->installedRelease() == tracker;
    const bool newestOwner = project_->newestRelease() == tracker;
    const auto ownership = installedOwner
        ? QStringLiteral("currently installed")
        : newestOwner ? QStringLiteral("newest analyzed release")
                      : QStringLiteral("retained historical release");
    updateOwnerLabel_->setText(
        QStringLiteral("<b>Editing release %1</b> · %2<br>The acquisition record for this release remains immutable; these settings only determine how its successor is discovered.")
            .arg(tracker->debian.version.toHtmlEscaped(), ownership));
    updateStrategy_->setCurrentIndex(tracker->update.strategy == UpdateStrategy::Manual ? 0
                                      : tracker->update.strategy == UpdateStrategy::DirectUrl ? 1
                                      : tracker->update.strategy == UpdateStrategy::AptRepository ? 2
                                      : tracker->update.strategy == UpdateStrategy::RpmRepository ? 3 : 4);
    updateUrl_->setText(tracker->update.url);
    aptSuite_->setText(tracker->update.aptSuite);
    aptComponent_->setText(tracker->update.aptComponent);
    aptArchitecture_->setText(tracker->update.aptArchitecture);
    aptPackageName_->setText(tracker->update.aptPackageName);
    rpmArchitecture_->setText(tracker->update.rpmArchitecture);
    rpmPackageName_->setText(tracker->update.rpmPackageName);
    githubOwner_->setText(tracker->update.githubOwner);
    githubRepository_->setText(tracker->update.githubRepository);
    githubAssetRegex_->setText(tracker->update.githubAssetRegex);
    githubPrereleases_->setChecked(tracker->update.githubIncludePrereleases);
    aptSigningKeyring_->setText(tracker->update.aptSigningKeyring);
    const auto markAiField = [tracker](QWidget *widget, const QString &field) {
        widget->setStyleSheet(tracker->fieldProvenance.value(field).origin == ValueOrigin::Ai
                                  ? QStringLiteral("background: #235a37; color: white;")
                                  : QString{});
    };
    markAiField(updateUrl_, QStringLiteral("update.url"));
    markAiField(aptSuite_, QStringLiteral("update.aptSuite"));
    markAiField(aptComponent_, QStringLiteral("update.aptComponent"));
    markAiField(aptArchitecture_, QStringLiteral("update.aptArchitecture"));
    markAiField(aptPackageName_, QStringLiteral("update.aptPackageName"));
    markAiField(rpmArchitecture_, QStringLiteral("update.rpmArchitecture"));
    markAiField(rpmPackageName_, QStringLiteral("update.rpmPackageName"));
    markAiField(githubOwner_, QStringLiteral("update.githubOwner"));
    markAiField(githubRepository_, QStringLiteral("update.githubRepository"));
    markAiField(githubAssetRegex_, QStringLiteral("update.githubAssetRegex"));
    markAiField(aptSigningKeyring_, QStringLiteral("update.signingKeySha256"));
    aptSigningKey_->clear();
    int selectedKey = -1;
    for (int index = 0; index < tracker->update.signingKeys.size(); ++index) {
        const auto &key = tracker->update.signingKeys.at(index);
        const auto fingerprint = key.fingerprints.isEmpty() ? QStringLiteral("unknown fingerprint")
                                                            : key.fingerprints.first();
        aptSigningKey_->addItem(QStringLiteral("%1 · %2 · %3")
                                    .arg(fingerprint, key.sourcePath,
                                         key.provenance.origin == ValueOrigin::Ai ? QStringLiteral("AI")
                                         : key.provenance.origin == ValueOrigin::User ? QStringLiteral("User")
                                                                                      : QStringLiteral("Detected")));
        if (key.relativePath == tracker->update.aptSigningKeyring) selectedKey = index;
    }
    if (selectedKey >= 0) aptSigningKey_->setCurrentIndex(selectedKey);
    if (selectedKey >= 0) {
        const QUrl sourceUrl(tracker->update.signingKeys.at(selectedKey).sourcePath);
        aptSigningKeyUrl_->setText(isAcceptableRepositoryKeyUrl(sourceUrl)
                                       ? sourceUrl.toString() : QString{});
    } else {
        aptSigningKeyUrl_->clear();
    }
    markAiField(aptSigningKey_, QStringLiteral("update.signingKeySha256"));
    aptSigningFingerprint_->setText(tracker->update.trustedSigningFingerprint.isEmpty()
                                        ? QStringLiteral("⚠ No trusted fingerprint configured")
                                        : tracker->update.trustedSigningFingerprint);
    updateCandidates_->clear();
    for (int index = 0; index < tracker->update.aptCandidates.size(); ++index) {
        const auto &candidate = tracker->update.aptCandidates.at(index);
        auto *item = new QListWidgetItem(candidate.displayText(), updateCandidates_);
        item->setData(Qt::UserRole, index);
        item->setData(Qt::UserRole + 1, QStringLiteral("apt"));
        item->setToolTip(QStringLiteral("Detected in %1%2")
                             .arg(candidate.sourcePath,
                                  candidate.signedBy.isEmpty()
                                      ? QString{}
                                      : QStringLiteral(" · Signed-By: %1").arg(candidate.signedBy)));
    }
    for (int index = 0; index < tracker->update.rpmCandidates.size(); ++index) {
        const auto &candidate = tracker->update.rpmCandidates.at(index);
        auto *item = new QListWidgetItem(candidate.displayText(), updateCandidates_);
        item->setData(Qt::UserRole, index);
        item->setData(Qt::UserRole + 1, QStringLiteral("rpm"));
        item->setToolTip(QStringLiteral("Detected in %1%2")
                             .arg(candidate.sourcePath,
                                  candidate.keyUrls.isEmpty()
                                      ? QString{}
                                      : QStringLiteral(" · Key URL: %1").arg(candidate.keyUrls.first())));
    }
    for (const auto &candidateUrl : tracker->update.detectedCandidates) {
        const auto structured = std::any_of(tracker->update.aptCandidates.cbegin(),
                                            tracker->update.aptCandidates.cend(),
                                            [&candidateUrl](const auto &candidate) {
                                                return candidate.uri == candidateUrl;
                                            }) ||
                                std::any_of(tracker->update.rpmCandidates.cbegin(),
                                            tracker->update.rpmCandidates.cend(),
                                            [&candidateUrl](const auto &candidate) {
                                                return candidate.baseUrl == candidateUrl;
                                            });
        if (!structured) {
            auto *item = new QListWidgetItem(candidateUrl, updateCandidates_);
            item->setData(Qt::UserRole, -1);
            item->setData(Qt::UserRole + 1, QStringLiteral("url"));
        }
    }
    updateUrl_->setEnabled(updateStrategy_->currentIndex() != 0);
    const bool apt = updateStrategy_->currentIndex() == 2;
    const bool rpm = updateStrategy_->currentIndex() == 3;
    const bool repository = apt || rpm;
    const bool github = updateStrategy_->currentIndex() == 4;
    aptSuite_->setEnabled(apt);
    aptComponent_->setEnabled(apt);
    aptArchitecture_->setEnabled(apt);
    aptPackageName_->setEnabled(apt);
    rpmArchitecture_->setEnabled(rpm);
    rpmPackageName_->setEnabled(rpm);
    aptSigningKeyUrl_->setEnabled(repository && !signingKeyDownloadService_.isRunning());
    aptSigningKeyDownloadButton_->setEnabled(repository && !signingKeyDownloadService_.isRunning());
    aptSigningKeyring_->setEnabled(repository);
    aptSigningKey_->setEnabled(repository);
    githubRegexAiButton_->setEnabled(github && aiSettings_.provider != AiProviderKind::None &&
                                     !aiService_.isRunning());
    syncUpdateCheckButtons();
    const auto githubPolicy = tracker->update.githubIncludePrereleases
        ? QStringLiteral(" Preview tracking is enabled, so newer prereleases may be selected even when a stable release exists.")
        : QStringLiteral(" Stable releases are preferred. If none match, PacSmith follows prereleases until a matching stable release is published, then remains on stable.");
    updateNotice_->setText(updateStrategy_->currentIndex() == 0
                               ? QStringLiteral("Manual updates: PacSmith will not query the network.")
                           : updateStrategy_->currentIndex() == 1
                               ? QStringLiteral("Direct URL saved; automatic version discovery is not implemented yet.")
                           : github
                               ? QStringLiteral("GitHub releases are not signature-verified by GitHub. PacSmith records any publisher digest exposed by the API, downloads over HTTPS, and always computes its own SHA256 before import.%1")
                                     .arg(githubPolicy)
                           : tracker->update.trustedSigningFingerprint.isEmpty()
                                   ? QStringLiteral("⚠ Repository checking is blocked until a trusted signing key is selected.")
                                   : QStringLiteral("✓ Repository metadata must verify against the pinned signing-key fingerprint before update results are trusted."));
    if (tracker->update.lastChecked.isValid()) {
        const auto signature = tracker->update.signatureVerified
                                   ? QStringLiteral("Repository signature verified.")
                                   : QStringLiteral("Repository signature not verified.");
        updateCheckStatus_->setText(QStringLiteral("Last checked %1\n%2\n%3")
                                        .arg(tracker->update.lastChecked.toLocalTime().toString(Qt::ISODate),
                                             tracker->update.lastCheckMessage, signature));
    } else {
        updateCheckStatus_->setText(QStringLiteral("Not checked yet."));
    }
    populating_ = false;
}

void MainWindow::populateBuild() {
    if (!project_) return;
    const auto unresolved = std::count_if(currentRelease()->dependencies.cbegin(), currentRelease()->dependencies.cend(),
                                          [](const auto &dependency) {
                                              return dependency.status == MappingStatus::Unresolved;
                                          });
    int unavailable = 0;
    if (repositoryCatalogLoaded_) {
        unavailable = static_cast<int>(std::count_if(
            currentRelease()->dependencies.cbegin(), currentRelease()->dependencies.cend(),
            [this](const auto &dependency) {
                const bool required = !dependency.ignored && !dependency.bundled &&
                                      !dependency.provided &&
                                      dependency.status != MappingStatus::Ignored &&
                                      dependency.status != MappingStatus::Bundled &&
                                      dependency.status != MappingStatus::Provided;
                return required && !dependency.archPackage.isEmpty() &&
                       repositoryDependencyAvailability_.contains(dependency.archPackage) &&
                       !repositoryDependencyAvailability_.value(dependency.archPackage);
            }));
    }
    const auto scriptReviews = pendingScriptFindings(*currentRelease());
    const auto payloadReviews = pendingPayloadReviews(*currentRelease());
    const auto lifecycleState = currentRelease()->lifecycleScript.contents.isEmpty()
                                    ? QStringLiteral("✓ No privileged lifecycle script")
                                : !currentRelease()->lifecycleScript.validationPassed
                                    ? QStringLiteral("⚠ Lifecycle script failed validation")
                                : currentRelease()->lifecycleScript.requiresAcknowledgement()
                                    ? QStringLiteral("⚠ AI-generated privileged script requires acknowledgement before install")
                                    : QStringLiteral("✓ Privileged lifecycle script acknowledged");
    buildChecklist_->setText(QStringLiteral("✓ Source available<br>✓ SHA256 known<br>✓ PKGBUILD present<br>%1<br>%2<br>%3<br>%4")
                                 .arg(unavailable > 0
                                          ? QStringLiteral("✗ %1 required Arch package name(s) are unavailable").arg(unavailable)
                                      : unresolved == 0 ? QStringLiteral("✓ Dependencies resolved and available or explicitly treated")
                                                        : QStringLiteral("⚠ %1 unresolved dependency group(s)").arg(unresolved),
                                      currentRelease()->maintainerScripts.isEmpty()
                                          ? QStringLiteral("✓ No maintainer scripts")
                                      : scriptReviews == 0
                                          ? QStringLiteral("✓ Maintainer-script responsibilities resolved")
                                          : QStringLiteral("⚠ %1 script responsibility item(s) require resolution").arg(scriptReviews),
                                      payloadReviews == 0
                                          ? QStringLiteral("✓ Flagged payload files reviewed")
                                          : QStringLiteral("⚠ %1 payload file(s) need a keep/exclude decision")
                                                .arg(payloadReviews), lifecycleState));
    const bool building = buildService_.isRunning();
    const bool installing = installService_.isRunning();
    const bool hasExistingBuild = releaseHasExistingBuild(*currentRelease());
    buildButton_->setText(building ? QStringLiteral("Cancel Build")
                                   : hasExistingBuild ? QStringLiteral("Rebuild")
                                                      : QStringLiteral("Build"));
    buildButton_->setEnabled(!installing && (building || unavailable == 0));
    buildButton_->setToolTip(!building && unavailable > 0
                                 ? QStringLiteral("Correct unavailable package names on Dependencies before building")
                                 : QString{});
    const bool updateInstall = !project_->installedVersion.isEmpty() &&
                               currentRelease()->id != project_->installedReleaseId;
    installButton_->setText(installing ? QStringLiteral("Installing…")
                                       : updateInstall ? QStringLiteral("Install Update")
                                                       : QStringLiteral("Install"));
    builtPackage_->setText(builtPackageSummaryHtml(store_, *currentRelease(), building, installing));
    const bool lifecycleReady = currentRelease()->lifecycleScript.contents.isEmpty() ||
                                (currentRelease()->lifecycleScript.validationPassed &&
                                 !currentRelease()->lifecycleScript.requiresAcknowledgement());
    const bool installable = !retainedPackagePath(store_, *currentRelease()).isEmpty();
    installButton_->setEnabled(!building && !installing && installable && lifecycleReady);
    installButton_->setToolTip(!lifecycleReady
                                   ? QStringLiteral("Review and acknowledge the exact privileged lifecycle script first")
                                   : !installable
                                         ? QStringLiteral("Build the package before installing")
                                         : QString{});
    const auto pkgbuildLabel = hasExistingBuild ? QStringLiteral("Rebuild") : QStringLiteral("Build");
    if (pkgbuildBuildButton_ != nullptr) {
        pkgbuildBuildButton_->setText(pkgbuildLabel);
        pkgbuildBuildButton_->setEnabled(!building && !installing && unavailable == 0);
    }
    if (resultPkgbuildBuildButton_ != nullptr) {
        resultPkgbuildBuildButton_->setText(pkgbuildLabel);
        resultPkgbuildBuildButton_->setEnabled(!building && !installing && unavailable == 0);
    }
}

void MainWindow::populateHistory() {
    if (!project_) return;
    historyList_->clear();
    for (auto iterator = project_->history.crbegin(); iterator != project_->history.crend(); ++iterator) {
        historyList_->addItem(QStringLiteral("%1  ·  %2  ·  %3")
                                  .arg(iterator->timestamp.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                       iterator->event, iterator->detail));
    }
}

bool MainWindow::persistCurrent() {
    if (!project_) return false;
    QString error;
    if (!store_.save(*project_, &error)) {
        QMessageBox::critical(this, QStringLiteral("Could not save project"), error);
        return false;
    }
    projectCache_.insert(project_->id, *project_);
    return true;
}

bool MainWindow::savePkgbuild() {
    if (!project_ || currentRelease() == nullptr || pkgbuildEditor_ == nullptr) return false;
    if (!pkgbuildEditor_->document()->isModified()) return true;
    QString error;
    if (!store_.saveCustomPkgbuild(*project_, *currentRelease(), pkgbuildEditor_->toPlainText(), &error)) {
        QMessageBox::critical(this, QStringLiteral("Could not save PKGBUILD"), error);
        return false;
    }
    projectCache_.insert(project_->id, *project_);
    pkgbuildEditor_->document()->setModified(false);
    configureEditorProfile();
    populatePkgbuild();
    populateBuild();
    populateHistory();
    statusBar()->showMessage(QStringLiteral("PKGBUILD saved"), 4000);
    return true;
}

void MainWindow::restoreGeneratedPkgbuild() {
    setConfigurationMode(false);
}

void MainWindow::refreshGeneratedPkgbuildAfterModelChange() {
    if (!project_ || currentRelease() == nullptr) return;
    const bool custom = currentRelease()->pkgbuildManuallyModified;
    currentRelease()->generatedPkgbuild = PkgbuildGenerator::generate(*currentRelease());
    currentRelease()->generatedPkgbuildSha256 = sha256Hex(currentRelease()->generatedPkgbuild.toUtf8());
    if (custom) {
        persistCurrent();
    } else {
        QString error;
        if (!store_.activateGuidedPkgbuild(*project_, *currentRelease(), &error)) {
            QMessageBox::critical(this, QStringLiteral("Could not update generated PKGBUILD"), error);
            return;
        }
        projectCache_.insert(project_->id, *project_);
        populatePkgbuild();
    }
    updateConfigurationModeChrome();
}

void MainWindow::dependencyEdited(const int row, const int column) {
    if (populating_ || !project_ || column != 1 || row < 0 || row >= currentRelease()->dependencies.size()) return;
    auto &dependency = currentRelease()->dependencies[row];
    dependency.archPackage = dependenciesTable_->item(row, 1)->text().trimmed();
    dependency.status = dependency.archPackage.isEmpty() ? MappingStatus::Unresolved : MappingStatus::Resolved;
    dependency.mappingSource = QStringLiteral("user override");
    dependency.confidence = 1.0;
    dependency.userOverride = true;
    dependency.ignored = false;
    dependency.bundled = false;
    dependency.provided = false;
    refreshGeneratedPkgbuildAfterModelChange();
    populateDependencies();
    populateOverview();
    populateBuild();
}

void MainWindow::dependencyDispositionChanged(const int row, const int index) {
    if (populating_ || !project_ || row < 0 || row >= currentRelease()->dependencies.size()) return;
    auto &dependency = currentRelease()->dependencies[row];
    dependency.userOverride = true;
    dependency.mappingSource = QStringLiteral("user override");
    dependency.confidence = 1.0;
    dependency.ignored = index == 3;
    dependency.bundled = index == 2;
    dependency.provided = index == 1;
    if (index == 3) dependency.status = MappingStatus::Ignored;
    else if (index == 2) dependency.status = MappingStatus::Bundled;
    else if (index == 1) dependency.status = MappingStatus::Provided;
    else dependency.status = dependency.archPackage.isEmpty() ? MappingStatus::Unresolved : MappingStatus::Resolved;
    refreshGeneratedPkgbuildAfterModelChange();
    populateDependencies();
    populateOverview();
    populateBuild();
}

bool MainWindow::saveUpdateConfiguration() {
    auto *tracker = updateEditorRelease();
    if (!project_ || populating_ || tracker == nullptr) return false;
    const auto strategy = updateStrategy_->currentIndex() == 0 ? UpdateStrategy::Manual
                        : updateStrategy_->currentIndex() == 1 ? UpdateStrategy::DirectUrl
                        : updateStrategy_->currentIndex() == 2 ? UpdateStrategy::AptRepository
                        : updateStrategy_->currentIndex() == 3 ? UpdateStrategy::RpmRepository
                                                               : UpdateStrategy::GitHubRelease;
    if (strategy == UpdateStrategy::GitHubRelease) {
        const QRegularExpression expression(githubAssetRegex_->text().trimmed());
        if (githubOwner_->text().trimmed().isEmpty() || githubRepository_->text().trimmed().isEmpty() ||
            githubAssetRegex_->text().trimmed().isEmpty() || !expression.isValid()) {
            QMessageBox::warning(this, QStringLiteral("Incomplete GitHub update source"),
                                 expression.isValid()
                                     ? QStringLiteral("GitHub owner, repository, and an asset-name regular expression are required.")
                                     : QStringLiteral("The asset-name regular expression is invalid: %1")
                                           .arg(expression.errorString()));
            return false;
        }
    }
    tracker->update.strategy = strategy;
    tracker->update.url = updateUrl_->text().trimmed();
    tracker->update.aptSuite = aptSuite_->text().trimmed();
    tracker->update.aptComponent = aptComponent_->text().trimmed();
    tracker->update.aptArchitecture = aptArchitecture_->text().trimmed();
    tracker->update.aptPackageName = aptPackageName_->text().trimmed();
    tracker->update.rpmArchitecture = rpmArchitecture_->text().trimmed();
    tracker->update.rpmPackageName = rpmPackageName_->text().trimmed();
    tracker->update.githubOwner = githubOwner_->text().trimmed();
    tracker->update.githubRepository = githubRepository_->text().trimmed();
    tracker->update.githubAssetRegex = githubAssetRegex_->text().trimmed();
    tracker->update.githubIncludePrereleases = githubPrereleases_->isChecked();
    if (tracker->update.strategy == UpdateStrategy::GitHubRelease) {
        tracker->update.url = QStringLiteral("https://github.com/%1/%2/releases")
            .arg(tracker->update.githubOwner, tracker->update.githubRepository);
    }
    tracker->update.aptSigningKeyring = aptSigningKeyring_->text().trimmed();
    if (aptSigningKey_->currentIndex() >= 0 &&
        aptSigningKey_->currentIndex() < tracker->update.signingKeys.size()) {
        const auto &key = tracker->update.signingKeys.at(aptSigningKey_->currentIndex());
        tracker->update.aptSigningKeyring = key.relativePath;
        tracker->update.trustedSigningFingerprint = key.fingerprints.value(0);
    }
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &field : {QStringLiteral("update.url"), QStringLiteral("update.aptSuite"),
                              QStringLiteral("update.aptComponent"), QStringLiteral("update.aptArchitecture"),
                              QStringLiteral("update.aptPackageName"), QStringLiteral("update.aptSigningKeyring"),
                              QStringLiteral("update.rpmArchitecture"), QStringLiteral("update.rpmPackageName"),
                              QStringLiteral("update.trustedSigningFingerprint"), QStringLiteral("update.githubOwner"),
                              QStringLiteral("update.githubRepository"), QStringLiteral("update.githubAssetRegex")}) {
        tracker->fieldProvenance.insert(field,
            FieldProvenance{ValueOrigin::User, {}, {}, tracker->sourceSha256,
                            QStringLiteral("Edited in the release's Updates tab"), now, true});
    }
    if (persistCurrent()) {
        populateUpdates();
        populateOverview();
        statusBar()->showMessage(
            QStringLiteral("Update configuration for release %1 saved").arg(tracker->debian.version), 5000);
        return true;
    }
    return false;
}

void MainWindow::downloadSigningKey() {
    auto *tracker = updateEditorRelease();
    if (!project_ || tracker == nullptr || signingKeyDownloadService_.isRunning()) return;

    const QUrl url(aptSigningKeyUrl_->text().trimmed(), QUrl::StrictMode);
    if (!isAcceptableRepositoryKeyUrl(url)) {
        QMessageBox::warning(
            this, QStringLiteral("Invalid signing-key URL"),
            QStringLiteral("Enter a complete HTTPS URL for the vendor's OpenPGP public key. Embedded credentials and URL fragments are not accepted."));
        aptSigningKeyUrl_->setFocus();
        aptSigningKeyUrl_->selectAll();
        return;
    }

    signingKeyDownloadProjectId_ = project_->id;
    signingKeyDownloadReleaseId_ = tracker->id;
    projectList_->setEnabled(false);
    aptSigningKeyDownloadButton_->setText(QStringLiteral("Downloading…"));
    aptSigningKeyDownloadButton_->setEnabled(false);
    aptSigningKeyUrl_->setEnabled(false);
    signingKeyProgress_ = new QProgressDialog(
        QStringLiteral("Downloading repository signing key…"),
        QStringLiteral("Cancel"), 0, 0, this);
    signingKeyProgress_->setWindowTitle(QStringLiteral("Fetch Repository Signing Key"));
    signingKeyProgress_->setWindowModality(Qt::WindowModal);
    signingKeyProgress_->setMinimumDuration(0);
    signingKeyProgress_->setAutoClose(false);
    signingKeyProgress_->setAutoReset(false);
    connect(signingKeyProgress_, &QProgressDialog::canceled,
            &signingKeyDownloadService_, &RepositoryKeyDownloadService::cancel);
    signingKeyProgress_->show();
    signingKeyDownloadService_.start(url);
}

void MainWindow::importSigningKey() {
    auto *tracker = updateEditorRelease();
    if (!project_ || tracker == nullptr) return;
    const auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import OpenPGP signing key"), {},
        QStringLiteral("OpenPGP keys (*.gpg *.pgp *.asc);;All files (*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, QStringLiteral("Could not import signing key"), file.errorString());
        return;
    }
    const auto contents = file.read(4 * 1024 * 1024 + 1);
    QString error;
    const auto key = RepositoryTrust::importUserKey(store_.releasePath(*tracker), contents,
                                                    path, &error);
    if (!key) {
        QMessageBox::critical(this, QStringLiteral("Could not import signing key"), error);
        return;
    }
    const auto duplicate = std::find_if(tracker->update.signingKeys.cbegin(), tracker->update.signingKeys.cend(),
                                        [&](const auto &candidate) { return candidate.sha256 == key->sha256; });
    if (duplicate == tracker->update.signingKeys.cend()) tracker->update.signingKeys.append(*key);
    tracker->update.aptSigningKeyring = key->relativePath;
    tracker->update.trustedSigningFingerprint = key->fingerprints.first();
    tracker->fieldProvenance.insert(QStringLiteral("update.aptSigningKeyring"), key->provenance);
    tracker->fieldProvenance.insert(QStringLiteral("update.trustedSigningFingerprint"), key->provenance);
    if (persistCurrent()) populateUpdates();
}

bool MainWindow::unlockAgeCredentials() {
    return promptUnlockAge(credentialStore_, settingsStore_.ageSecretsPath(), this);
}


} // namespace pacsmith::gui
