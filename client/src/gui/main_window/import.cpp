#include "gui/main_window/common.hpp"
#include "core/release_review.hpp"

namespace pacsmith::gui {
namespace {

struct AutomaticBuildRepositoryReadiness {
    bool ready{false};
    QString message;
};

struct GitHubResolveTask {
    UpdateCheckResult result;
    QString error;
};

struct GitHubImportTask {
    std::optional<GitHubImportResult> result;
    QString error;
};

struct ArtifactImportTask {
    std::optional<ImportResult> result;
    QString error;
};

} // namespace

void MainWindow::chooseImport() {
    const auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("New Project from Package File"), {},
        QStringLiteral(
            "All PacSmith package sources — DEB, RPM, AppImage, Arch package, tar/ZIP/7z archive (*.deb *.rpm *.AppImage *.appimage *.pkg.tar.zst *.pkg.tar.xz *.pkg.tar.gz *.tar *.tar.gz *.tgz *.tar.xz *.tar.zst *.tar.bz2 *.tbz2 *.tar.lz4 *.zip *.7z);;"
            "Debian packages (*.deb);;"
            "RPM packages (*.rpm);;"
            "Type 2 AppImages (*.AppImage *.appimage);;"
            "Arch packages (*.pkg.tar.zst *.pkg.tar.xz *.pkg.tar.gz);;"
            "Archives (*.tar *.tar.gz *.tgz *.tar.xz *.tar.zst *.tar.bz2 *.tbz2 *.tar.lz4 *.zip *.7z);;"
            "Standalone Linux executables and other files (*)"));
    if (!path.isEmpty()) importPackage(path);
}

void MainWindow::submitManualRelease() {
    if (!project_ || importThread_ != nullptr || serverImportRunning_) return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Submit New Release"));
    dialog.resize(680, dialog.sizeHint().height());
    auto *layout = new QVBoxLayout(&dialog);
    auto *introduction = new QLabel(
        QStringLiteral("Import a vendor artifact as a new release of <b>%1</b>. "
                       "PacSmith will verify the optional publisher checksum, inspect the "
                       "artifact, and carry forward the previous package configuration.")
            .arg(project_->displayName.toHtmlEscaped()),
        &dialog);
    introduction->setWordWrap(true);
    layout->addWidget(introduction);

    auto *form = new QFormLayout;
    auto *sourceChoice = new QWidget(&dialog);
    auto *sourceChoiceLayout = new QHBoxLayout(sourceChoice);
    sourceChoiceLayout->setContentsMargins(0, 0, 0, 0);
    auto *localSource = new QRadioButton(QStringLiteral("Local file"), sourceChoice);
    auto *urlSource = new QRadioButton(QStringLiteral("Direct HTTPS URL"), sourceChoice);
    localSource->setChecked(true);
    sourceChoiceLayout->addWidget(localSource);
    sourceChoiceLayout->addWidget(urlSource);
    sourceChoiceLayout->addStretch();
    auto *artifactRow = new QWidget(&dialog);
    auto *artifactLayout = new QHBoxLayout(artifactRow);
    artifactLayout->setContentsMargins(0, 0, 0, 0);
    auto *artifactPath = new QLineEdit(artifactRow);
    artifactPath->setPlaceholderText(QStringLiteral("Locally downloaded vendor artifact"));
    auto *browse = new QPushButton(QStringLiteral("Browse…"), artifactRow);
    artifactLayout->addWidget(artifactPath, 1);
    artifactLayout->addWidget(browse);
    auto *artifactUrl = new QLineEdit(&dialog);
    artifactUrl->setPlaceholderText(
        QStringLiteral("https://vendor.example/releases/application.tar.gz"));
    artifactUrl->setEnabled(false);
    auto *version = new QLineEdit(&dialog);
    version->setPlaceholderText(
        QStringLiteral("Optional when the artifact or filename contains the version"));
    auto *expectedSha256 = new QLineEdit(&dialog);
    expectedSha256->setPlaceholderText(
        QStringLiteral("Optional 64-character checksum published by the vendor"));
    form->addRow(QStringLiteral("Source"), sourceChoice);
    form->addRow(QStringLiteral("Local artifact"), artifactRow);
    form->addRow(QStringLiteral("Artifact URL"), artifactUrl);
    form->addRow(QStringLiteral("Vendor version"), version);
    form->addRow(QStringLiteral("Publisher SHA256"), expectedSha256);
    layout->addLayout(form);

    auto *versionHelp = new QLabel(
        QStringLiteral("Enter the vendor version when it cannot be inferred, such as an "
                       "archive whose filename contains only a release codename."),
        &dialog);
    versionHelp->setWordWrap(true);
    layout->addWidget(versionHelp);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok,
                                         &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Import && Inspect"));
    layout->addWidget(buttons);

    connect(localSource, &QRadioButton::toggled, artifactRow,
            &QWidget::setEnabled);
    connect(urlSource, &QRadioButton::toggled, artifactUrl,
            &QWidget::setEnabled);

    connect(browse, &QPushButton::clicked, &dialog, [this, artifactPath] {
        const auto path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select Vendor Artifact"), {},
            QStringLiteral(
                "PacSmith package sources (*.deb *.rpm *.AppImage *.appimage *.pkg.tar.zst "
                "*.pkg.tar.xz *.pkg.tar.gz *.tar *.tar.gz *.tgz *.tar.xz *.tar.zst "
                "*.tar.bz2 *.tbz2 *.tar.lz4 *.zip *.7z);;All files (*)"));
        if (!path.isEmpty()) artifactPath->setText(path);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            [&dialog, localSource, artifactPath, artifactUrl, expectedSha256] {
        if (localSource->isChecked()) {
            const QFileInfo artifact(artifactPath->text().trimmed());
            if (!artifact.isAbsolute() || !artifact.isFile()) {
                QMessageBox::warning(&dialog, QStringLiteral("Artifact required"),
                                     QStringLiteral("Select a local vendor artifact file."));
                return;
            }
        } else {
            const QUrl url(artifactUrl->text().trimmed(), QUrl::StrictMode);
            if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
                url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment()) {
                QMessageBox::warning(
                    &dialog, QStringLiteral("Invalid artifact URL"),
                    QStringLiteral("Enter a complete HTTPS artifact URL without credentials or a fragment."));
                return;
            }
        }
        const auto checksum = expectedSha256->text().trimmed();
        static const QRegularExpression sha256(QStringLiteral("^[0-9A-Fa-f]{64}$"));
        if (!checksum.isEmpty() && !sha256.match(checksum).hasMatch()) {
            QMessageBox::warning(
                &dialog, QStringLiteral("Invalid SHA256"),
                QStringLiteral("The publisher SHA256 must contain exactly 64 hexadecimal characters."));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted || !project_) return;

    pendingImportOptions_ = {};
    pendingImportOptions_.existingProjectId = project_->id;
    pendingImportOptions_.version = version->text().trimmed();
    pendingImportOptions_.expectedSha256 = expectedSha256->text().trimmed().toLower();
    if (localSource->isChecked()) {
        pendingImportOptions_.acquisition.kind = AcquisitionKind::LocalFile;
        pendingImportOptions_.acquisition.canonicalIdentity = project_->sourceIdentity;
        importPackage(artifactPath->text().trimmed());
    } else {
        const QUrl url(artifactUrl->text().trimmed(), QUrl::StrictMode);
        pendingImportOptions_.acquisition.kind = AcquisitionKind::DirectUrl;
        pendingImportOptions_.acquisition.canonicalIdentity =
            url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
        pendingImportOptions_.acquisition.originalUrl = url.toString();
        importPackage(url.toString());
    }
}

void MainWindow::importGitHubUrl() {
    QInputDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New Project from GitHub"));
    dialog.setLabelText(QStringLiteral(
        "Enter a GitHub repository, release, or release-asset link. PacSmith will load uploaded assets and generated source archives, then let you choose the artifact pattern.\n\n"
        "Examples:\n"
        "https://github.com/segmentio/chamber\n"
        "https://github.com/owner/project/releases/tag/v1.2.3"));
    dialog.setOkButtonText(QStringLiteral("Continue"));
    dialog.setTextEchoMode(QLineEdit::Normal);
    dialog.resize(680, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return;

    auto entered = dialog.textValue().trimmed();
    if (!entered.contains(QStringLiteral("://"))) entered.prepend(QStringLiteral("https://"));
    QUrl url(entered, QUrl::StrictMode);
    if (url.host().compare(QStringLiteral("www.github.com"), Qt::CaseInsensitive) == 0) {
        url.setHost(QStringLiteral("github.com"));
    }
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        url.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(
            this, QStringLiteral("Invalid GitHub link"),
            QStringLiteral("Enter an HTTPS link on github.com for a repository, release, or release asset."));
        return;
    }
    importPackage(url.toString());
}

void MainWindow::importDirectUrl() {
    QInputDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New Project from Direct Download URL"));
    dialog.setLabelText(QStringLiteral(
        "Enter the direct HTTPS download URL for a DEB, RPM, Type 2 AppImage, Arch package, archive, or standalone Linux executable.\n\n"
        "Example:\n"
        "https://vendor.example/download/application.tar.gz"));
    dialog.setOkButtonText(QStringLiteral("Download and Inspect"));
    dialog.setTextEchoMode(QLineEdit::Normal);
    dialog.resize(680, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return;

    auto entered = dialog.textValue().trimmed();
    if (!entered.contains(QStringLiteral("://"))) entered.prepend(QStringLiteral("https://"));
    const QUrl url(entered, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Invalid download URL"),
                             QStringLiteral("Enter a complete HTTPS artifact URL."));
        return;
    }
    importPackage(url.toString());
}

void MainWindow::importAptRepository() {
    const auto choice = chooseRepositorySource(
        this, false, knownRepositoryKeys(projectCache_, library_));
    if (choice) beginRepositoryImport(choice->update, choice->signingKeyUrl,
                                      choice->signingKeyContents,
                                      choice->signingKeySource);
}

void MainWindow::importRpmRepository() {
    const auto choice = chooseRepositorySource(
        this, true, knownRepositoryKeys(projectCache_, library_));
    if (choice) beginRepositoryImport(choice->update, choice->signingKeyUrl,
                                      choice->signingKeyContents,
                                      choice->signingKeySource);
}

void MainWindow::beginRepositoryImport(UpdateConfiguration configuration,
                                       const QUrl &signingKeyUrl,
                                       const QByteArray &signingKeyContents,
                                       const QString &signingKeySource) {
    if (repositoryImportRunning_ || serverImportRunning_ || importThread_ != nullptr) {
        statusBar()->showMessage(QStringLiteral("Another package acquisition is already in progress"),
                                 5000);
        return;
    }
    repositoryImportRunning_ = true;
    const bool rpm = configuration.strategy == UpdateStrategy::RpmRepository;
    const auto packageName = rpm ? configuration.rpmPackageName
                                 : configuration.aptPackageName;
    auto *keyService = new RepositoryKeyDownloadService(this);
    auto *keyProgress = new QProgressDialog(
        signingKeyContents.isEmpty()
            ? QStringLiteral("Downloading the repository signing key for review…")
            : QStringLiteral("Preparing the supplied repository signing key for review…"),
        QStringLiteral("Cancel"), 0, 0, this);
    keyProgress->setWindowTitle(rpm ? QStringLiteral("Import from RPM Repository")
                                    : QStringLiteral("Import from APT Repository"));
    keyProgress->setWindowModality(Qt::WindowModal);
    keyProgress->setMinimumDuration(0);
    keyProgress->setAutoClose(false);
    keyProgress->setMinimumWidth(500);
    connect(keyProgress, &QProgressDialog::canceled, keyService,
            &RepositoryKeyDownloadService::cancel);
    connect(keyService, &RepositoryKeyDownloadService::progress, this,
            [keyProgress](const qint64 received, const qint64 total) {
        if (total > 0) {
            keyProgress->setRange(0, 1000);
            keyProgress->setValue(static_cast<int>(std::clamp<qint64>(
                received * 1000 / total, 0, 1000)));
        } else {
            keyProgress->setRange(0, 0);
        }
        keyProgress->setLabelText(
            QStringLiteral("Downloading repository signing key… %1 KiB")
                .arg(received / 1024));
    });
    connect(keyService, &RepositoryKeyDownloadService::failed, this,
            [this, keyService, keyProgress](const QString &message) {
        keyProgress->disconnect();
        keyProgress->close();
        keyProgress->deleteLater();
        keyService->deleteLater();
        repositoryImportRunning_ = false;
        if (message != QStringLiteral("Signing-key download canceled")) {
            QMessageBox::critical(this, QStringLiteral("Could not load signing key"),
                                  message);
        }
    });
    connect(keyService, &RepositoryKeyDownloadService::finished, this,
            [this, keyService, keyProgress, configuration, packageName, rpm](
                const QByteArray &contents, const QUrl &requestedUrl,
                const QUrl &resolvedUrl) mutable {
        keyProgress->disconnect();
        keyProgress->close();
        keyProgress->deleteLater();
        keyService->deleteLater();

        QString inspectionError;
        const auto inspection = RepositoryTrust::inspectKey(contents, &inspectionError);
        if (!inspection) {
            repositoryImportRunning_ = false;
            QMessageBox::critical(this, QStringLiteral("Signing key is not usable"),
                                  inspectionError);
            return;
        }
        QMessageBox review(QMessageBox::Question,
                           QStringLiteral("Trust repository signing key?"),
                           QStringLiteral("PacSmith inspected the selected OpenPGP key. Verify the fingerprint against a separate official source before trusting it.\n\nFingerprint(s):\n%1")
                               .arg(inspection->fingerprints.join(QLatin1Char('\n'))),
                           QMessageBox::NoButton, this);
        auto *trustButton = review.addButton(QStringLiteral("Trust and Query Repository"),
                                             QMessageBox::AcceptRole);
        review.addButton(QMessageBox::Cancel);
        review.setDetailedText(
            QStringLiteral("Selected source: %1\nResolved source: %2\nKey SHA256: %3\n\nPacSmith will pin the selected fingerprint and store a normalized copy of this key only inside the new release directory.")
                .arg(requestedUrl.toString(), resolvedUrl.toString(), inspection->sha256));
        review.exec();
        if (review.clickedButton() != trustButton) {
            repositoryImportRunning_ = false;
            return;
        }

        configuration.trustedSigningFingerprint = inspection->fingerprints.first();
        if (rpm) {
            configuration.rpmCandidates.append(
                {configuration.url, configuration.rpmArchitecture,
                 {requestedUrl.toString()}, QStringLiteral("user-configured repository")});
        } else {
            configuration.aptCandidates.append(
                {configuration.url, configuration.aptSuite,
                 configuration.aptComponent.isEmpty()
                     ? QStringList{} : QStringList{configuration.aptComponent},
                 {configuration.aptArchitecture}, {},
                 QStringLiteral("user-configured repository")});
        }

        auto *queryProgress = new QProgressDialog(
            QStringLiteral("The PacSmith daemon is verifying the repository and inspecting its package…"),
            QString{}, 0, 0, this);
        queryProgress->setWindowTitle(rpm ? QStringLiteral("Querying RPM Repository")
                                          : QStringLiteral("Querying APT Repository"));
        queryProgress->setWindowModality(Qt::WindowModal);
        queryProgress->setMinimumDuration(0);
        queryProgress->setAutoClose(false);
        queryProgress->setCancelButton(nullptr);
        queryProgress->setMinimumWidth(520);
        queryProgress->show();
        auto *watcher = new QFutureWatcher<ArtifactImportTask>(this);
        connect(watcher, &QFutureWatcher<ArtifactImportTask>::finished, this,
                [this, watcher, queryProgress, rpm] {
            const auto task = watcher->result();
            watcher->deleteLater();
            queryProgress->close();
            queryProgress->deleteLater();
            repositoryImportRunning_ = false;
            if (!task.result) {
                QMessageBox::critical(this,
                    QStringLiteral("Repository package could not be acquired"), task.error);
                return;
            }
            finishServerArtifactImport(
                *task.result, false,
                rpm ? QStringLiteral("RPM repository package verified and inspected by pacsmithd")
                    : QStringLiteral("APT repository package verified and inspected by pacsmithd"));
        });
        const auto config = library_.config();
        const auto fingerprint = configuration.trustedSigningFingerprint;
        const auto source = requestedUrl.toString();
        watcher->setFuture(QtConcurrent::run(
            [config, configuration, contents, source, fingerprint] {
                ArtifactImportTask task;
                LibraryClient client(config);
                task.result = client.importRepository(configuration, contents, source,
                                                      fingerprint, &task.error);
                return task;
            }));
    });
    keyProgress->show();
    if (signingKeyContents.isEmpty()) {
        keyService->start(signingKeyUrl);
    } else {
        const auto source = signingKeySource.isEmpty()
            ? QUrl(QStringLiteral("pacsmith:user-supplied-key"))
            : QUrl(signingKeySource);
        keyService->provide(signingKeyContents, source);
    }
}

void MainWindow::importPackage(const QString &path) {
    const QUrl remote(path);
    if (remote.isValid() && remote.scheme() == QStringLiteral("https") &&
        remote.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) == 0) {
        beginGitHubImport(remote);
        return;
    }
    if (remote.isValid() && remote.scheme() == QStringLiteral("https")) {
        if (serverImportRunning_) return;
        const auto filename = QFileInfo(remote.path()).fileName().isEmpty()
            ? QStringLiteral("vendor-artifact") : QFileInfo(remote.path()).fileName();
        const auto expectedSha256 = pendingImportOptions_.expectedSha256;
        const auto trustDescription = expectedSha256.isEmpty()
            ? QStringLiteral("PacSmith will compute and record the downloaded bytes' SHA256, but no publisher checksum was supplied.")
            : QStringLiteral("PacSmith will require the download to match the supplied publisher SHA256 before inspection.");
        if (QMessageBox::question(
                this, QStringLiteral("Download vendor artifact"),
                QStringLiteral("Download %1 over HTTPS and inspect it as untrusted input? %2")
                    .arg(remote.toDisplayString(), trustDescription)) != QMessageBox::Yes) {
            pendingImportOptions_ = {};
            return;
        }
        const auto existingProjectId = pendingImportOptions_.existingProjectId;
        const auto version = pendingImportOptions_.version;
        pendingImportOptions_ = {};
        serverImportRunning_ = true;
        auto *progress = new QProgressDialog(
            QStringLiteral("The PacSmith daemon is downloading and inspecting the vendor artifact…"),
            QString{}, 0, 0, this);
        progress->setWindowTitle(QStringLiteral("Importing Direct Download"));
        progress->setWindowModality(Qt::WindowModal);
        progress->setCancelButton(nullptr);
        progress->setMinimumDuration(0);
        progress->show();
        auto *watcher = new QFutureWatcher<ArtifactImportTask>(this);
        connect(watcher, &QFutureWatcher<ArtifactImportTask>::finished, this,
                [this, watcher, progress, submittedManualRelease = !existingProjectId.isEmpty()] {
            const auto task = watcher->result();
            watcher->deleteLater();
            progress->close();
            progress->deleteLater();
            serverImportRunning_ = false;
            if (!task.result) {
                QMessageBox::critical(this, QStringLiteral("Direct download import failed"),
                                      task.error);
                return;
            }
            finishServerArtifactImport(*task.result, submittedManualRelease,
                                       QStringLiteral("Vendor artifact downloaded and inspected by pacsmithd"));
        });
        const auto config = library_.config();
        watcher->setFuture(QtConcurrent::run(
            [config, remote, existingProjectId, version, expectedSha256] {
                ArtifactImportTask task;
                LibraryClient client(config);
                task.result = client.importRemoteUrl(remote, existingProjectId, version,
                                                     expectedSha256, &task.error);
                return task;
            }));
        return;
    }
    if (importThread_ != nullptr) {
        statusBar()->showMessage(QStringLiteral("A package import is already in progress"), 5000);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Analyzing %1…").arg(QFileInfo(path).fileName()));
    const bool releasePreparation = !preparingReleaseId_.isEmpty();
    importProgress_ = new QProgressDialog(
        QStringLiteral("Preparing import…"),
        releasePreparation ? QStringLiteral("Hide") : QString{}, 0, 0, this);
    importProgress_->setWindowTitle(QStringLiteral("Importing Artifact"));
    importProgress_->setWindowModality(releasePreparation ? Qt::NonModal : Qt::WindowModal);
    if (!releasePreparation) importProgress_->setCancelButton(nullptr);
    importProgress_->setMinimumDuration(0);
    importProgress_->setAutoClose(false);
    importProgress_->setAutoReset(false);
    importProgress_->setMinimumWidth(420);
    importProgress_->show();

    auto *thread = new QThread(this);
    auto *worker = new ImportWorker(library_.projectsRoot(), QFileInfo(path).absoluteFilePath(),
                                    pendingImportOptions_);
    importThread_ = thread;
    updateDeleteButton();
    updateDashboardActions();
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &ImportWorker::run);
    connect(worker, &ImportWorker::progressChanged, this, [this](const QString &description) {
        if (!preparingReleaseId_.isEmpty()) {
            preparationPhase_ = description;
            updatePreparationIndicators();
        }
        if (importProgress_ != nullptr) importProgress_->setLabelText(description);
        statusBar()->showMessage(description);
    });
    connect(worker, &ImportWorker::completed, this,
            [this](const QString &projectId, const QString &releaseId, const QString &error) {
        const auto preparationProjectId = preparingProjectId_;
        const bool preparedUpdate = !preparingReleaseId_.isEmpty();
        const bool submittedManualRelease =
            !pendingImportOptions_.existingProjectId.isEmpty();
        const auto preparationSourceReleaseId = preparationSourceReleaseId_;
        const bool automaticPreparationBuild = automaticPreparationBuild_;
        pendingImportOptions_ = {};
        if (importProgress_ != nullptr) {
            importProgress_->close();
            importProgress_->deleteLater();
            importProgress_ = nullptr;
        }
        if (projectId.isEmpty()) {
            resetPreparationState();
            if (!preparationProjectId.isEmpty()) refreshProjectList(preparationProjectId);
            statusBar()->clearMessage();
            QMessageBox::critical(this, QStringLiteral("Import failed"), error);
            return;
        }
        resetPreparationState();
        refreshProjectList(projectId, [this, projectId, releaseId, preparedUpdate,
                                       submittedManualRelease,
                                       preparationSourceReleaseId,
                                       automaticPreparationBuild](const bool succeeded) {
            if (!succeeded || !project_ || project_->id != projectId ||
                project_->release(releaseId) == nullptr) {
                statusBar()->showMessage(
                    QStringLiteral("Package imported, but its refreshed state could not be loaded"), 10000);
                return;
            }
            currentReleaseId_ = releaseId;
            refreshCurrentProject();
            QString automaticBuildPauseMessage;
            if (preparedUpdate && automaticPreparationBuild) {
                const auto *previous = project_->release(preparationSourceReleaseId);
                const auto *prepared = project_->release(releaseId);
                QStringList blockers;
                if (previous == nullptr || prepared == nullptr) {
                    blockers.append(QStringLiteral(
                        "PacSmith could not identify the previous package configuration."));
                } else {
                    blockers = automaticUpdateBuildBlockers(*previous, *prepared);
                }
                if (!project_->repository.publish) {
                    blockers.append(QStringLiteral(
                        "Repository publishing is not enabled for this project."));
                }
                if (blockers.isEmpty()) {
                    statusBar()->showMessage(QStringLiteral(
                        "Update prepared with no review changes; checking repository readiness…"));
                    const auto config = library_.config();
                    auto *watcher = new QFutureWatcher<AutomaticBuildRepositoryReadiness>(this);
                    connect(watcher, &QFutureWatcher<AutomaticBuildRepositoryReadiness>::finished,
                            this, [this, watcher, projectId, releaseId] {
                        const auto readiness = watcher->result();
                        watcher->deleteLater();
                        if (!project_ || project_->id != projectId ||
                            project_->release(releaseId) == nullptr) return;
                        currentReleaseId_ = releaseId;
                        if (!readiness.ready) {
                            showReleaseWorkbenchAtFirstAttention(releaseId);
                            statusBar()->showMessage(
                                QStringLiteral("Update prepared but automatic build paused: %1")
                                    .arg(readiness.message),
                                12000);
                            return;
                        }
                        statusBar()->showMessage(
                            QStringLiteral("Building package %1…").arg(project_->displayName));
                        startBuild(false, true);
                    });
                    watcher->setFuture(QtConcurrent::run([config] {
                        LibraryClient client(config);
                        QString repoError;
                        const auto repo = client.repoSettings(&repoError);
                        if (!repo) {
                            return AutomaticBuildRepositoryReadiness{
                                false, repoError.isEmpty()
                                           ? QStringLiteral("repository settings are unavailable")
                                           : repoError};
                        }
                        if (!repo->enabled) {
                            return AutomaticBuildRepositoryReadiness{
                                false, QStringLiteral("the PacSmith package repository is not enabled")};
                        }
                        if (!repo->signingInitialized) {
                            return AutomaticBuildRepositoryReadiness{
                                false, QStringLiteral("repository signing is not initialized")};
                        }
                        return AutomaticBuildRepositoryReadiness{true, {}};
                    }));
                    return;
                }
                automaticBuildPauseMessage = QStringLiteral(
                    "Update prepared but needs attention: %1").arg(blockers.constFirst());
            }
            if (preparedUpdate || submittedManualRelease) applyRetentionCleanup();
            if (automaticBuildPauseMessage.isEmpty()) {
                statusBar()->showMessage(
                    QStringLiteral("Imported to %1").arg(projectDirectory(library_, *project_)), 10000);
            }
            if (preparedUpdate) showReleaseWorkbenchAtFirstAttention(releaseId);
            QTimer::singleShot(0, this, [this, projectId, releaseId,
                                         automaticBuildPauseMessage] {
                if (!project_ || project_->id != projectId ||
                    project_->release(releaseId) == nullptr) return;
                currentReleaseId_ = releaseId;
                if (!automaticBuildPauseMessage.isEmpty()) {
                    showReleaseWorkbenchAtFirstAttention(releaseId);
                    statusBar()->showMessage(automaticBuildPauseMessage, 12000);
                    return;
                }
                const bool needsReview = pendingScriptFindings(*currentRelease()) > 0 ||
                                         pendingPayloadReviews(*currentRelease()) > 0 ||
                                         currentRelease()->installMapping.appRun.requiresReview() ||
                                         std::any_of(currentRelease()->dependencies.cbegin(),
                                                     currentRelease()->dependencies.cend(),
                                                     [](const auto &dependency) {
                                                         return dependency.status == MappingStatus::Unresolved;
                                                     });
                if (!needsReview) {
                    showReleaseWorkbenchAtFirstAttention(releaseId);
                    statusBar()->showMessage(
                        QStringLiteral("Import complete. Review the readiness checklist, then build the package."),
                        12000);
                    return;
                }
                showReleaseWorkbenchAtFirstAttention(releaseId);
                QMessageBox::information(
                    this, QStringLiteral("Package needs review"),
                    QStringLiteral("PacSmith found items that need an Arch-specific decision and opened the first section "
                                   "that needs your attention. Resolve the highlighted items, then continue to PKGBUILD and Build.\n\n"
                                   "Use Ask AI to launch your configured external harness with this project and release context."));
            });
        });
    });
    connect(worker, &ImportWorker::completed, thread, &QThread::quit);
    connect(worker, &ImportWorker::completed, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (importProgress_ != nullptr) {
            importProgress_->close();
            importProgress_->deleteLater();
            importProgress_ = nullptr;
        }
        if (importThread_ == thread) importThread_ = nullptr;
        updateDeleteButton();
        updateDashboardActions();
        thread->deleteLater();
    });
    thread->start();
}

void MainWindow::finishServerArtifactImport(const ImportResult &result,
                                            const bool applyRetention,
                                            const QString &successMessage) {
    const auto projectId = result.project.id;
    const auto releaseId = result.releaseId;
    refreshProjectList(projectId, [this, projectId, releaseId, applyRetention,
                                   successMessage](const bool succeeded) {
        if (!succeeded || !project_ || project_->id != projectId ||
            project_->release(releaseId) == nullptr) {
            statusBar()->showMessage(
                QStringLiteral("Package imported, but its refreshed state could not be loaded"),
                10000);
            return;
        }
        currentReleaseId_ = releaseId;
        refreshCurrentProject();
        if (applyRetention) applyRetentionCleanup();
        showReleaseWorkbenchAtFirstAttention(releaseId);
        statusBar()->showMessage(successMessage, 10000);
    });
}

void MainWindow::beginGitHubImport(const QUrl &url) {
    const auto parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        QMessageBox::critical(this, QStringLiteral("Invalid GitHub URL"),
                              QStringLiteral("Expected a repository, release, or release-asset URL."));
        return;
    }
    auto owner = parts.at(0);
    auto repository = parts.at(1);
    if (repository.endsWith(QStringLiteral(".git"))) repository.chop(4);
    QString initialRegex = QStringLiteral(".*");
    if (owner.compare(QStringLiteral("anderson-arlen"), Qt::CaseInsensitive) == 0 &&
        repository.compare(QStringLiteral("pacsmith"), Qt::CaseInsensitive) == 0) {
        initialRegex = QStringLiteral(
            R"(pacsmith-[0-9][A-Za-z0-9._+-]*-[0-9]+-x86_64\.pkg\.tar\.zst)");
    }
    QString requestedTag;
    if (parts.size() >= 6 && parts.at(2) == QStringLiteral("releases") &&
        parts.at(3) == QStringLiteral("download")) {
        requestedTag = parts.at(4);
        initialRegex = QRegularExpression::escape(parts.mid(5).join(QLatin1Char('/')));
    } else if (parts.size() >= 5 && parts.at(2) == QStringLiteral("releases") &&
               parts.at(3) == QStringLiteral("tag")) {
        requestedTag = parts.mid(4).join(QLatin1Char('/'));
    }
    auto *progress = new QProgressDialog(QStringLiteral("Loading GitHub release artifacts…"),
                                         QString{}, 0, 0, this);
    progress->setWindowTitle(QStringLiteral("Import from GitHub"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setCancelButton(nullptr);
    progress->show();
    auto *watcher = new QFutureWatcher<GitHubResolveTask>(this);
    connect(watcher, &QFutureWatcher<GitHubResolveTask>::finished, this,
            [this, watcher, progress, owner, repository, initialRegex,
             requestedTag]() mutable {
        const auto task = watcher->result();
        watcher->deleteLater();
        progress->close();
        progress->deleteLater();
        if (!task.error.isEmpty() && task.result.availableAssets.isEmpty()) {
            QMessageBox::critical(this, QStringLiteral("GitHub import failed"), task.error);
            return;
        }
        if (initialRegex != QStringLiteral(".*") && task.result.success) {
            continueGitHubImport(owner, repository, initialRegex, false, requestedTag);
            return;
        }
        const auto rule = chooseGitHubAssetRule(this, task.result.availableAssets, false);
        if (!rule) return;
        continueGitHubImport(owner, repository, rule->expression,
                             rule->includePrereleases, requestedTag);
    });
    const auto config = library_.config();
    watcher->setFuture(QtConcurrent::run([config, url, initialRegex]() {
        GitHubResolveTask task;
        LibraryClient client(config);
        task.result = client.resolveGitHub(url, initialRegex, false, nullptr, &task.error);
        return task;
    }));
}

void MainWindow::continueGitHubImport(const QString &owner, const QString &repository,
                                      const QString &assetRegex, const bool includePrereleases,
                                      const QString &requestedTag) {
    QUrl url(QStringLiteral("https://github.com/%1/%2").arg(owner, repository));
    if (!requestedTag.isEmpty()) {
        url.setPath(QStringLiteral("/%1/%2/releases/tag/%3")
                        .arg(owner, repository, requestedTag));
    }
    auto *progress = new QProgressDialog(QStringLiteral("Downloading and inspecting GitHub release…"),
                                         QString{}, 0, 0, this);
    progress->setWindowTitle(QStringLiteral("Import from GitHub"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setCancelButton(nullptr);
    progress->show();
    auto *watcher = new QFutureWatcher<GitHubImportTask>(this);
    connect(watcher, &QFutureWatcher<GitHubImportTask>::finished, this,
            [this, watcher, progress]( ) {
        const auto task = watcher->result();
        watcher->deleteLater();
        progress->close();
        progress->deleteLater();
        if (!task.result) {
            QMessageBox::critical(this, QStringLiteral("GitHub import failed"), task.error);
            return;
        }
        const auto projectId = task.result->imported.project.id;
        const auto releaseId = task.result->imported.releaseId;
        refreshProjectList(projectId, [this, projectId, releaseId](const bool succeeded) {
            if (!succeeded || !project_ || project_->id != projectId) {
                statusBar()->showMessage(
                    QStringLiteral("Package imported, but its refreshed state could not be loaded"),
                    10000);
                return;
            }
            currentReleaseId_ = releaseId;
            refreshCurrentProject();
            showReleaseWorkbenchAtFirstAttention(releaseId);
            statusBar()->showMessage(
                QStringLiteral("GitHub release downloaded and inspected by pacsmithd"), 10000);
        });
    });
    const auto config = library_.config();
    watcher->setFuture(QtConcurrent::run(
        [config, url, assetRegex, includePrereleases]() {
        GitHubImportTask task;
        LibraryClient client(config);
        task.result = client.importGitHub(url, assetRegex, includePrereleases, {}, &task.error);
        return task;
    }));
}


} // namespace pacsmith::gui
