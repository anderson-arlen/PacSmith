#include "gui/main_window/common.hpp"

namespace pacsmith::gui {

namespace {

QStringList metadataList(const QString &text) {
    QStringList result;
    for (const auto &part : text.split(QRegularExpression(QStringLiteral("[,\\n]")),
                                      Qt::SkipEmptyParts)) {
        const auto value = part.trimmed();
        if (!value.isEmpty() && !result.contains(value)) result.append(value);
    }
    return result;
}

QString buildSpinnerLabel(const int frame) {
    static const QStringList frames{QStringLiteral("⠋"), QStringLiteral("⠙"),
                                    QStringLiteral("⠹"), QStringLiteral("⠸")};
    return QStringLiteral("%1 Cancel Build").arg(frames.at(frame % frames.size()));
}

struct ProjectSaveTaskResult {
    std::optional<Project> project;
    QString error;
};

} // namespace

void MainWindow::populatePackageMetadata() {
    if (!project_ || currentRelease() == nullptr || packageDescription_ == nullptr) return;
    QSignalBlocker displayBlock(packageDisplayName_);
    QSignalBlocker archNameBlock(packageArchName_);
    QSignalBlocker vendorBlock(packageVendorName_);
    QSignalBlocker descriptionBlock(packageDescription_);
    QSignalBlocker homepageBlock(packageHomepage_);
    QSignalBlocker licensesBlock(packageLicenses_);
    QSignalBlocker providesBlock(packageProvides_);
    QSignalBlocker conflictsBlock(packageConflicts_);
    const auto &metadata = currentRelease()->packageMetadata;
    packageDisplayName_->setText(project_->displayName);
    packageArchName_->setText(project_->archPackageName);
    packageVendorName_->setText(project_->vendorName);
    packageDescription_->setText(metadata.description);
    packageHomepage_->setText(metadata.homepage);
    packageLicenses_->setText(metadata.licenses.join(QStringLiteral(", ")));
    packageProvides_->setText(metadata.provides.join(QStringLiteral(", ")));
    packageConflicts_->setText(metadata.conflicts.join(QStringLiteral(", ")));
}

void MainWindow::savePackageMetadata() {
    if (!project_ || currentRelease() == nullptr || packageDescription_ == nullptr) return;
    const auto licenses = metadataList(packageLicenses_->text());
    const auto provides = metadataList(packageProvides_->text());
    const auto conflicts = metadataList(packageConflicts_->text());
    if (licenses.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("License required"),
                             QStringLiteral("Enter at least one SPDX license expression or custom:vendor."));
        return;
    }
    if (packageDisplayName_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Display name required"),
                             QStringLiteral("Enter a display name for this project."));
        return;
    }
    const auto packageNameError =
        DomainValidation::archPackageName(packageArchName_->text().trimmed());
    if (!packageNameError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Invalid Arch package name"),
                             packageNameError);
        return;
    }
    const QUrl homepage(packageHomepage_->text().trimmed(), QUrl::StrictMode);
    if (!packageHomepage_->text().trimmed().isEmpty() &&
        (!homepage.isValid() || (homepage.scheme() != QStringLiteral("https") &&
                                 homepage.scheme() != QStringLiteral("http")))) {
        QMessageBox::warning(this, QStringLiteral("Invalid homepage"),
                             QStringLiteral("Homepage must be an absolute HTTP or HTTPS URL."));
        return;
    }
    for (const auto &relation : provides + conflicts) {
        const auto error = DomainValidation::packageRelation(relation);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Invalid package relationship"),
                                 QStringLiteral("%1: %2").arg(relation, error));
            return;
        }
    }
    auto &metadata = currentRelease()->packageMetadata;
    project_->displayName = packageDisplayName_->text().trimmed();
    project_->archPackageName = packageArchName_->text().trimmed();
    project_->vendorName = packageVendorName_->text().trimmed();
    for (auto &release : project_->releases) {
        release.displayName = project_->displayName;
        release.archPackageName = project_->archPackageName;
        release.vendorName = project_->vendorName;
    }
    metadata.description = packageDescription_->text().trimmed();
    metadata.homepage = packageHomepage_->text().trimmed();
    metadata.licenses = licenses;
    metadata.provides = provides;
    metadata.conflicts = conflicts;
    refreshGeneratedPkgbuildAfterModelChange();
    populatePackageMetadata();
    statusBar()->showMessage(QStringLiteral("Package metadata saved"), 5000);
}

void MainWindow::addAdditionalDependency() {
    if (!project_ || currentRelease() == nullptr) return;
    bool accepted = false;
    const auto value = QInputDialog::getText(
        this, QStringLiteral("Add runtime dependency"),
        QStringLiteral("Official Arch package name (optionally with a version constraint):"),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || value.isEmpty()) return;
    const auto validationError = DomainValidation::packageRelation(value);
    if (!validationError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Invalid dependency"), validationError);
        return;
    }
    auto &dependencies = currentRelease()->packageMetadata.additionalDependencies;
    if (!dependencies.contains(value)) dependencies.append(value);
    refreshGeneratedPkgbuildAfterModelChange();
    populateDependencies();
    populateOverview();
    populateBuild();
}

void MainWindow::removeAdditionalDependency() {
    if (!project_ || currentRelease() == nullptr || additionalDependencies_ == nullptr ||
        additionalDependencies_->currentItem() == nullptr) return;
    currentRelease()->packageMetadata.additionalDependencies.removeAll(
        additionalDependencies_->currentItem()->text());
    refreshGeneratedPkgbuildAfterModelChange();
    populateDependencies();
    populateOverview();
    populateBuild();
}

void MainWindow::populatePkgbuild() {
    if (!project_ || currentRelease() == nullptr) return;
    QString error;
    const auto contents = library_.readPkgbuild(*currentRelease(), &error);
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
    if (pkgbuildState_ != nullptr &&
        (pkgbuildEditor_ == nullptr || !pkgbuildEditor_->document()->isModified())) {
        pkgbuildState_->setText(state);
    }
    if (pkgbuildPreviewNotice_ != nullptr) {
        pkgbuildPreviewNotice_->setText(state);
    }
    const auto vars = library_.readIdentityVariables(*currentRelease(), nullptr)
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
    const auto contents = library_.readPkgbuild(*currentRelease(), &error);
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
        updateStrategy_, autoBuildPolicy_, updateUrl_, directUrlFullCheckInterval_,
        aptSuite_, aptComponent_, aptArchitecture_, aptPackageName_,
        rpmArchitecture_, rpmPackageName_,
        aptSigningKeyUrl_, aptSigningKeyDownloadButton_, aptSigningKeyring_, aptSigningKey_,
        githubOwner_, githubRepository_, githubAssetRegex_,
        githubPrereleases_, updateCandidates_, updateSaveButton_};
    if (tracker == nullptr) {
        updateOwnerLabel_->setText(
            QStringLiteral("No analyzed release currently owns the project update view. Prepare an artifact first."));
        updateStrategy_->setCurrentIndex(0);
        for (auto *control : controls) control->setEnabled(false);
        updateUrl_->clear();
        directUrlFullCheckInterval_->setCurrentIndex(0);
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
        setUpdateCheckStatus(QStringLiteral("No update checks are available."));
        updateMonitoringAttention();
        syncUpdateCheckButtons();
        populating_ = false;
        return;
    }
    for (auto *control : controls) control->setEnabled(true);
    const auto autoBuildIndex = autoBuildPolicy_->findData(
        autoBuildPolicyName(project_->autoBuildPolicy));
    autoBuildPolicy_->setCurrentIndex(autoBuildIndex < 0 ? 1 : autoBuildIndex);
    if (auto *model = qobject_cast<QStandardItemModel *>(autoBuildPolicy_->model())) {
        if (auto *reviewFree = model->item(1)) {
            const bool custom = tracker->pkgbuildManuallyModified;
            reviewFree->setEnabled(!custom);
            reviewFree->setToolTip(custom
                ? QStringLiteral("Custom PKGBUILDs require manual or AI review because upstream compatibility cannot be determined mechanically.")
                : QString{});
        }
    }
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
    const auto directIntervalIndex = directUrlFullCheckInterval_->findData(
        tracker->update.directUrlFullCheckIntervalHours);
    directUrlFullCheckInterval_->setCurrentIndex(directIntervalIndex >= 0
                                                     ? directIntervalIndex : 0);
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
    directUrlFullCheckInterval_->setEnabled(updateStrategy_->currentIndex() == 1);
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
    syncUpdateCheckButtons();
    const auto githubPolicy = tracker->update.githubIncludePrereleases
        ? QStringLiteral(" Preview tracking is enabled, so newer prereleases may be selected even when a stable release exists.")
        : QStringLiteral(" Stable releases are preferred. If none match, PacSmith follows prereleases until a matching stable release is published, then remains on stable.");
    const bool directValidatorsAvailable = !tracker->update.directUrlEtag.isEmpty() ||
        !tracker->update.directUrlLastModified.isEmpty() ||
        (!tracker->update.directUrlVendorValidatorName.isEmpty() &&
         !tracker->update.directUrlVendorValidator.isEmpty());
    const auto directInterval = tracker->update.directUrlFullCheckIntervalHours <= 0
        ? QStringLiteral("manual only")
        : directUrlFullCheckInterval_->currentText().toLower();
    updateNotice_->setText(updateStrategy_->currentIndex() == 0
                               ? QStringLiteral("Manual updates: PacSmith will not query the network.")
                           : updateStrategy_->currentIndex() == 1
                               ? directValidatorsAvailable
                                   ? QStringLiteral("✓ Direct URL checks use the stored remote validator before downloading. SHA256 remains authoritative when the validator changes.")
                                   : tracker->update.lastChecked.isValid()
                                       ? QStringLiteral("⚠ This server exposes no usable cheap change validator. PacSmith must download and hash the complete artifact; automatic full-content checks are %1.")
                                             .arg(directInterval)
                                       : QStringLiteral("PacSmith will probe for ETag, Last-Modified, and supported object-version headers before deciding whether a download is needed.")
                           : github
                               ? QStringLiteral("GitHub releases are not signature-verified by GitHub. PacSmith records any publisher digest exposed by the API, downloads over HTTPS, and always computes its own SHA256 before import.%1")
                                     .arg(githubPolicy)
                           : tracker->update.trustedSigningFingerprint.isEmpty()
                                   ? QStringLiteral("⚠ Repository checking is blocked until a trusted signing key is selected.")
                                   : QStringLiteral("✓ Repository metadata must verify against the pinned signing-key fingerprint before update results are trusted."));
    const bool trackerAutomaticPaused = tracker->state != ReleaseState::Built &&
        tracker->update.lastAutomaticStatus == QStringLiteral("paused");
    const PackageRelease *healthRelease = project_->updateHealthRelease();
    const bool healthCheckFailed = healthRelease != nullptr &&
                                   healthRelease->update.lastCheckFailed;
    const bool healthAutomaticPaused = healthRelease != nullptr &&
        healthRelease->state != ReleaseState::Built &&
        healthRelease->update.lastAutomaticStatus == QStringLiteral("paused");
    QStringList statusLines;
    if (healthRelease != tracker && (healthCheckFailed || healthAutomaticPaused)) {
        const auto version = healthRelease->debian.version;
        if (healthCheckFailed) {
            statusLines.append(QStringLiteral("⚠ Last update check failed for release %1")
                                   .arg(version));
            statusLines.append(healthRelease->update.lastCheckMessage);
        }
        if (healthAutomaticPaused) {
            statusLines.append(QStringLiteral("⚠ Automatic handling paused for release %1")
                                   .arg(version));
            if (!healthRelease->update.lastAutomaticMessage.isEmpty()) {
                statusLines.append(healthRelease->update.lastAutomaticMessage);
            }
        }
        statusLines.append(QString{});
        statusLines.append(QStringLiteral("Monitoring source release %1:")
                               .arg(tracker->debian.version));
    }
    if (tracker->update.lastChecked.isValid()) {
        if (tracker->update.lastCheckFailed) {
            statusLines.append(QStringLiteral("⚠ Last update check failed at %1")
                                   .arg(tracker->update.lastChecked.toLocalTime().toString(Qt::ISODate)));
            statusLines.append(tracker->update.lastCheckMessage);
        } else {
            const auto signature = tracker->update.strategy == UpdateStrategy::DirectUrl
                ? directValidatorsAvailable
                    ? QStringLiteral("Remote validator available; downloaded bytes are verified by SHA256.")
                    : QStringLiteral("No remote validator available; checks require full-content SHA256 comparison.")
                : tracker->update.signatureVerified
                    ? QStringLiteral("Repository signature verified.")
                    : QStringLiteral("Repository signature not verified.");
            statusLines.append(QStringLiteral("Last checked %1")
                                   .arg(tracker->update.lastChecked.toLocalTime().toString(Qt::ISODate)));
            statusLines.append(tracker->update.lastCheckMessage);
            statusLines.append(signature);
        }
        if (!tracker->update.lastAutomaticStatus.isEmpty()) {
            const auto automaticLabel = trackerAutomaticPaused
                ? QStringLiteral("⚠ Automatic handling paused")
                : QStringLiteral("Automatic handling: %1")
                      .arg(tracker->update.lastAutomaticStatus);
            statusLines.append(automaticLabel +
                (tracker->update.lastAutomaticMessage.isEmpty()
                     ? QString{}
                     : QStringLiteral(" — %1").arg(tracker->update.lastAutomaticMessage)));
        }
    } else {
        statusLines.append(QStringLiteral("Not checked yet."));
    }
    if (healthCheckFailed || tracker->update.lastCheckFailed) {
        statusLines.append(QStringLiteral(
            "This is the retained result of the last check. A successful recheck clears this warning."));
    }
    setUpdateCheckStatus(statusLines.join(QLatin1Char('\n')),
                         healthCheckFailed || tracker->update.lastCheckFailed,
                         healthAutomaticPaused || trackerAutomaticPaused);
    updateMonitoringAttention();
    populating_ = false;
}

void MainWindow::setUpdateCheckStatus(const QString &message, const bool failed,
                                      const bool warning) {
    if (updateCheckStatus_ == nullptr) return;
    updateCheckStatus_->setText(message);
    if (!failed && !warning) {
        updateCheckStatus_->setStyleSheet({});
        return;
    }
    const QColor accent = failed ? QColor(QStringLiteral("#ef5350"))
                                 : QColor(QStringLiteral("#f2b84b"));
    QColor background = accent;
    background.setAlpha(36);
    updateCheckStatus_->setStyleSheet(
        QStringLiteral("QLabel { background-color: rgba(%1, %2, %3, %4); "
                       "border: 1px solid %5; border-radius: 6px; padding: 10px; }")
            .arg(background.red()).arg(background.green()).arg(background.blue())
            .arg(background.alpha()).arg(accent.name()));
}

void MainWindow::updateMonitoringAttention() {
    if (projectTabs_ == nullptr) return;
    int tabIndex = -1;
    for (int index = 0; index < projectTabs_->count(); ++index) {
        auto *page = projectTabs_->widget(index);
        if (page != nullptr && page->isAncestorOf(dashboardUpdatesHost_)) {
            tabIndex = index;
            break;
        }
    }
    if (tabIndex < 0) return;
    const PackageRelease *release = project_ ? project_->updateHealthRelease() : nullptr;
    const bool failed = release != nullptr && release->update.lastCheckFailed;
    const bool warning = release != nullptr && release->state != ReleaseState::Built &&
                         release->update.lastAutomaticStatus == QStringLiteral("paused");
    projectTabs_->setTabIcon(tabIndex,
        failed ? style()->standardIcon(QStyle::SP_MessageBoxCritical)
               : warning ? style()->standardIcon(QStyle::SP_MessageBoxWarning) : QIcon{});
    if (failed || warning) {
        projectTabs_->tabBar()->setTabTextColor(
            tabIndex, failed ? QColor(QStringLiteral("#ef5350"))
                             : QColor(QStringLiteral("#f2b84b")));
    } else {
        projectTabs_->tabBar()->setTabTextColor(tabIndex, QColor{});
    }
}

void MainWindow::populateBuild() {
    if (!project_) return;
    if (compileCachePolicy_ != nullptr) {
        QSignalBlocker blocker(compileCachePolicy_);
        const auto index = compileCachePolicy_->findData(
            compileCachePolicyName(project_->compileCachePolicy));
        compileCachePolicy_->setCurrentIndex(index < 0 ? 0 : index);
        const bool custom = currentRelease()->pkgbuildManuallyModified;
        compileCachePolicy_->setEnabled(custom);
        compileCachePolicy_->setToolTip(custom
            ? QStringLiteral("Custom source builds share one compiler cache across versions of this project.")
            : QStringLiteral("Compiler caching is managed only for containerized Custom PKGBUILDs."));
    }
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
                                    ? QStringLiteral("⚠ Privileged script requires exact-content acknowledgement before install")
                                    : QStringLiteral("✓ Privileged lifecycle script acknowledged");
    const auto iconState = currentRelease()->installMapping.icon.isConfigured()
                               ? QStringLiteral("✓ Application icon selected")
                               : QStringLiteral("⚠ No application icon selected — the package can still be built");
    buildChecklist_->setText(QStringLiteral("✓ Source available<br>✓ SHA256 known<br>✓ PKGBUILD present<br>%1<br>%2<br>%3<br>%4<br>%5")
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
                                                .arg(payloadReviews),
                                      lifecycleState, iconState));
    const bool building = releaseBuildInProgress(currentRelease()->id);
    const bool anotherBuild = buildInProgress() && !building;
    const bool installing = packageOperationInProgress();
    const bool hasExistingBuild = releaseHasExistingBuild(*currentRelease());
    buildButton_->setText(building ? buildSpinnerLabel(preparationSpinnerFrame_)
                                   : hasExistingBuild ? QStringLiteral("Rebuild")
                                                      : QStringLiteral("Build"));
    buildButton_->setEnabled(!installing && !anotherBuild && (building || unavailable == 0));
    buildButton_->setToolTip(!building && unavailable > 0
                                 ? QStringLiteral("Correct unavailable package names on Dependencies before building")
                                 : QString{});
    if (viewBuildOutputButton_ != nullptr) {
        viewBuildOutputButton_->setVisible(building);
        viewBuildOutputButton_->setEnabled(building);
    }
    const bool updateInstall = !project_->installedVersion.isEmpty() &&
                               currentRelease()->id != project_->installedReleaseId;
    installButton_->setText(installing ? QStringLiteral("Installing…")
                                       : updateInstall ? QStringLiteral("Install Update")
                                                       : QStringLiteral("Install"));
    builtPackage_->setText(builtPackageSummaryHtml(library_, *currentRelease(), building, installing));
    const bool lifecycleReady = currentRelease()->lifecycleScript.contents.isEmpty() ||
                                (currentRelease()->lifecycleScript.validationPassed &&
                                 !currentRelease()->lifecycleScript.requiresAcknowledgement());
    const bool installable = releaseHasRetainedPackage(*currentRelease());
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
    populateProjectHistory(historyList_, project_ ? &*project_ : nullptr);
}

bool MainWindow::persistCurrent() {
    if (!project_ || !ensureCurrentProjectWritable()) return false;
    QString error;
    if (!library_.save(*project_, &error)) {
        QMessageBox::critical(this, QStringLiteral("Could not save project"), error);
        return false;
    }
    projectCache_.insert(project_->id, *project_);
    return true;
}

bool MainWindow::savePkgbuild() {
    if (!project_ || currentRelease() == nullptr || pkgbuildEditor_ == nullptr ||
        !ensureCurrentProjectWritable()) return false;
    if (!pkgbuildEditor_->document()->isModified()) return true;
    QString error;
    if (!library_.saveCustomPkgbuild(*project_, *currentRelease(), pkgbuildEditor_->toPlainText(), &error)) {
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
        if (!library_.activateGuidedPkgbuild(*project_, *currentRelease(), &error)) {
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

void MainWindow::scriptFindingDispositionChanged(const int row, const int) {
    if (populating_ || !project_ || currentRelease() == nullptr || scriptFindingsTable_ == nullptr) return;
    if (row < 0 || row >= currentRelease()->scriptFindings.size()) return;
    auto *combo = qobject_cast<QComboBox *>(scriptFindingsTable_->cellWidget(row, 2));
    if (combo == nullptr) return;
    auto &finding = currentRelease()->scriptFindings[row];
    finding.disposition = static_cast<ScriptDisposition>(combo->currentData().toInt());
    finding.provenance = {ValueOrigin::User, {}, {}, {}, QStringLiteral("user override"),
                          QDateTime::currentDateTimeUtc(), true};
    auto &lifecycle = currentRelease()->lifecycleScript;
    lifecycle.sourceFingerprints.clear();
    for (const auto &item : currentRelease()->scriptFindings) {
        if (item.disposition == ScriptDisposition::LifecycleRequired &&
            !item.evidenceFingerprint.isEmpty()) {
            lifecycle.sourceFingerprints.append(item.evidenceFingerprint);
        }
    }
    if (!persistCurrent()) return;
    refreshGeneratedPkgbuildAfterModelChange();
    populateScripts();
    populateOverview();
    populateBuild();
}

void MainWindow::saveUpdateConfiguration() {
    saveUpdateConfigurationThen({});
}

void MainWindow::saveUpdateConfigurationThen(std::function<void(bool)> completed) {
    auto *tracker = updateEditorRelease();
    if (!project_ || populating_ || tracker == nullptr || updateConfigurationSaveInFlight_ ||
        !ensureCurrentProjectWritable()) {
        if (completed) completed(false);
        return;
    }
    const auto strategy = updateStrategy_->currentIndex() == 0 ? UpdateStrategy::Manual
                        : updateStrategy_->currentIndex() == 1 ? UpdateStrategy::DirectUrl
                        : updateStrategy_->currentIndex() == 2 ? UpdateStrategy::AptRepository
                        : updateStrategy_->currentIndex() == 3 ? UpdateStrategy::RpmRepository
                                                               : UpdateStrategy::GitHubRelease;
    const auto previousStrategy = tracker->update.strategy;
    const auto previousUrl = tracker->update.url;
    project_->autoBuildPolicy = autoBuildPolicyFromName(
        autoBuildPolicy_->currentData().toString());
    tracker->update.strategy = strategy;
    tracker->update.url = updateUrl_->text().trimmed();
    tracker->update.directUrlFullCheckIntervalHours =
        directUrlFullCheckInterval_->currentData().toInt();
    if (strategy == UpdateStrategy::DirectUrl &&
        (previousStrategy != UpdateStrategy::DirectUrl || previousUrl != tracker->update.url)) {
        tracker->update.directUrlEtag.clear();
        tracker->update.directUrlLastModified.clear();
        tracker->update.directUrlContentLength = -1;
        tracker->update.directUrlVendorValidatorName.clear();
        tracker->update.directUrlVendorValidator.clear();
        tracker->update.directUrlLastSha256.clear();
        tracker->update.directUrlLastFullCheck = {};
    }
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
    const auto validationError = DomainValidation::updateConfiguration(tracker->update);
    if (!validationError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Invalid update source"), validationError);
        if (completed) completed(false);
        return;
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
    const auto projectId = project_->id;
    const auto releaseId = tracker->id;
    const auto releaseVersion = tracker->debian.version;
    auto projectSnapshot = *project_;
    const auto config = library_.config();
    updateConfigurationSaveInFlight_ = true;
    updateConfigurationSaveProjectId_ = projectId;
    updateConfigurationSaveReleaseId_ = releaseId;
    if (updatesEditor_ != nullptr) updatesEditor_->setEnabled(false);
    if (updateSaveButton_ != nullptr) updateSaveButton_->setEnabled(false);
    syncUpdateCheckButtons();
    setUpdateCheckStatus(QStringLiteral("Saving update configuration…"));
    updateUpdateCheckIndicators();
    syncActivityTimer();

    auto *watcher = new QFutureWatcher<ProjectSaveTaskResult>(this);
    connect(watcher, &QFutureWatcher<ProjectSaveTaskResult>::finished, this,
            [this, watcher, projectId, releaseVersion,
             completed = std::move(completed)]() mutable {
        auto result = watcher->result();
        watcher->deleteLater();
        updateConfigurationSaveInFlight_ = false;
        updateConfigurationSaveProjectId_.clear();
        updateConfigurationSaveReleaseId_.clear();
        if (updatesEditor_ != nullptr) updatesEditor_->setEnabled(true);
        updateUpdateCheckIndicators();
        syncActivityTimer();
        if (eventRefreshAgain_ || !pendingEventTopics_.isEmpty() ||
            !pendingEventProjectIds_.isEmpty() || pendingFullEventRefresh_) {
            eventRefreshAgain_ = false;
            eventRefreshTimer_->start();
        }
        if (!result.project) {
            syncUpdateCheckButtons();
            if (project_ && project_->id == projectId) {
                setUpdateCheckStatus(result.error.isEmpty()
                                         ? QStringLiteral("Could not save update configuration")
                                         : result.error,
                                     true);
            }
            QMessageBox::critical(this, QStringLiteral("Could not save update configuration"),
                                  result.error);
            if (completed) completed(false);
            return;
        }
        projectCache_.insert(projectId, *result.project);
        if (project_ && project_->id == projectId) {
            project_->revision = result.project->revision;
            for (auto &localRelease : project_->releases) {
                if (const auto *savedRelease = result.project->release(localRelease.id)) {
                    localRelease.revision = savedRelease->revision;
                }
            }
            projectCache_.insert(projectId, *project_);
            populateUpdates();
            populateOverview();
        }
        statusBar()->showMessage(
            QStringLiteral("Update configuration for release %1 saved").arg(releaseVersion), 5000);
        if (completed) completed(true);
    });
    watcher->setFuture(QtConcurrent::run(
        [config, projectSnapshot = std::move(projectSnapshot)]() mutable {
            ProjectSaveTaskResult result;
            LibraryClient client(config);
            if (client.save(projectSnapshot, &result.error)) {
                result.project = std::move(projectSnapshot);
            }
            return result;
        }));
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
    const auto key = RepositoryTrust::importUserKey(library_.releasePath(*tracker), contents,
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


} // namespace pacsmith::gui
