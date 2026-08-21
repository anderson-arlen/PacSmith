#include "gui/main_window/common.hpp"

namespace pacsmith::gui {

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

void MainWindow::importGitHubUrl() {
    QInputDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New Project from GitHub"));
    dialog.setLabelText(QStringLiteral(
        "Enter a GitHub repository, release, or release-asset link. PacSmith will load the release assets and let you choose the package pattern.\n\n"
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
    if (repositoryImportRunning_ || debDownloadService_->isRunning() ||
        importThread_ != nullptr) {
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

        auto temporary = std::make_shared<QTemporaryDir>();
        if (!temporary->isValid()) {
            repositoryImportRunning_ = false;
            QMessageBox::critical(this, QStringLiteral("Could not query repository"),
                                  QStringLiteral("Could not create a temporary verification directory."));
            return;
        }
        QString keyError;
        const auto key = RepositoryTrust::importUserKey(
            std::filesystem::path(temporary->path().toUtf8().constData()), contents,
            requestedUrl.toString(), &keyError);
        if (!key || key->fingerprints.isEmpty()) {
            repositoryImportRunning_ = false;
            QMessageBox::critical(this, QStringLiteral("Could not prepare signing key"),
                                  keyError);
            return;
        }
        configuration.signingKeys = {*key};
        configuration.aptSigningKeyring = key->relativePath;
        configuration.trustedSigningFingerprint = key->fingerprints.first();
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

        PackageRelease probe;
        probe.debian.package = packageName;
        probe.debian.version = QStringLiteral("0");
        probe.debian.architecture = rpm ? configuration.rpmArchitecture
                                        : configuration.aptArchitecture;
        probe.update = configuration;
        auto *queryProgress = new QProgressDialog(
            QStringLiteral("Downloading and verifying signed repository metadata…"),
            QStringLiteral("Cancel"), 0, 0, this);
        queryProgress->setWindowTitle(rpm ? QStringLiteral("Querying RPM Repository")
                                          : QStringLiteral("Querying APT Repository"));
        queryProgress->setWindowModality(Qt::WindowModal);
        queryProgress->setMinimumDuration(0);
        queryProgress->setAutoClose(false);
        queryProgress->setMinimumWidth(520);
        queryProgress->show();

        const auto finishQuery =
            [this, queryProgress, temporary, configuration, contents, requestedUrl,
             packageName, rpm](const UpdateCheckResult &result, QObject *service) {
            queryProgress->disconnect();
            queryProgress->close();
            queryProgress->deleteLater();
            service->deleteLater();
            if (!result.success || !result.signatureVerified ||
                result.downloadUrl.isEmpty() || result.sha256.isEmpty()) {
                repositoryImportRunning_ = false;
                QMessageBox::critical(
                    this, QStringLiteral("Repository package could not be acquired"),
                    result.message.isEmpty()
                        ? QStringLiteral("The repository did not yield a signature-verified package artifact.")
                        : result.message);
                return;
            }
            auto importedConfiguration = configuration;
            importedConfiguration.detectedVersion = result.detectedVersion;
            importedConfiguration.detectedFilename = result.filename;
            importedConfiguration.detectedSha256 = result.sha256;
            importedConfiguration.detectedUrl = result.downloadUrl;
            importedConfiguration.lastChecked = QDateTime::currentDateTimeUtc();
            importedConfiguration.lastCheckMessage = result.message;
            importedConfiguration.signatureVerified = true;

            pendingImportOptions_ = {};
            pendingImportOptions_.version = result.detectedVersion;
            pendingImportOptions_.initialUpdate = importedConfiguration;
            pendingImportOptions_.trustedSigningKey = contents;
            pendingImportOptions_.trustedSigningKeySource = requestedUrl.toString();
            auto &acquisition = pendingImportOptions_.acquisition;
            acquisition.kind = rpm ? AcquisitionKind::RpmRepository
                                   : AcquisitionKind::AptRepository;
            acquisition.canonicalIdentity = rpm
                ? QStringLiteral("rpm:%1:%2:%3")
                      .arg(importedConfiguration.url.toLower(),
                           importedConfiguration.rpmArchitecture.toLower(),
                           importedConfiguration.rpmPackageName.toLower())
                : QStringLiteral("apt:%1:%2:%3:%4:%5")
                      .arg(importedConfiguration.url.toLower(),
                           importedConfiguration.aptSuite.toLower(),
                           importedConfiguration.aptComponent.toLower(),
                           importedConfiguration.aptArchitecture.toLower(),
                           importedConfiguration.aptPackageName.toLower());
            acquisition.originalUrl = result.downloadUrl;
            acquisition.publisherDigest = result.sha256;
            acquisition.publisherVerified = true;

            const auto filename = QFileInfo(QUrl(result.downloadUrl).path()).fileName().isEmpty()
                ? QFileInfo(result.filename).fileName()
                : QFileInfo(QUrl(result.downloadUrl).path()).fileName();
            const auto target = defaultDownloadPath(
                PkgbuildGenerator::sanitizePackageName(packageName),
                PkgbuildGenerator::sanitizePackageName(result.detectedVersion), filename);
            downloadProgress_ = new QProgressDialog(
                QStringLiteral("Downloading signature-verified %1…\nYou may hide this window; the download will continue.")
                    .arg(filename),
                QStringLiteral("Hide"), 0, 0, this);
            downloadProgress_->setWindowTitle(
                rpm ? QStringLiteral("Downloading RPM Repository Package")
                    : QStringLiteral("Downloading APT Repository Package"));
            downloadProgress_->setWindowModality(Qt::NonModal);
            downloadProgress_->setMinimumDuration(0);
            downloadProgress_->setAutoClose(false);
            downloadProgress_->setAutoReset(false);
            downloadProgress_->setMinimumWidth(500);
            downloadProgress_->show();
            downloadProgress_->raise();
            downloadProgress_->activateWindow();
            repositoryImportRunning_ = false;
            startListDownloadActivity(PkgbuildGenerator::sanitizePackageName(packageName));
            const auto url = QUrl(result.downloadUrl);
            const auto sha = result.sha256;
            const auto path = std::filesystem::path(target.toUtf8().constData());
            onServiceThread(*debDownloadService_, [this, url, sha, path] {
                debDownloadService_->start(url, sha, path);
            });
        };

        if (rpm) {
            auto *service = networkServiceOnThread<RpmUpdateService>(networkIoThread_);
            connect(queryProgress, &QProgressDialog::canceled, service,
                    &RpmUpdateService::cancel);
            connect(service, &RpmUpdateService::progressChanged, queryProgress,
                    &QProgressDialog::setLabelText);
            connect(service, &RpmUpdateService::finished, this,
                    [finishQuery, service](const UpdateCheckResult &result) mutable {
                finishQuery(result, service);
            });
            const auto path = std::filesystem::path(temporary->path().toUtf8().constData());
            onServiceThread(*service, [service, probe, path] { service->start(probe, path); });
        } else {
            auto *service = networkServiceOnThread<AptUpdateService>(networkIoThread_);
            connect(queryProgress, &QProgressDialog::canceled, service,
                    &AptUpdateService::cancel);
            connect(service, &AptUpdateService::progressChanged, queryProgress,
                    &QProgressDialog::setLabelText);
            connect(service, &AptUpdateService::finished, this,
                    [finishQuery, service](const UpdateCheckResult &result) mutable {
                finishQuery(result, service);
            });
            const auto path = std::filesystem::path(temporary->path().toUtf8().constData());
            onServiceThread(*service, [service, probe, path] { service->start(probe, path); });
        }
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
        if (debDownloadService_->isRunning()) return;
        const auto filename = QFileInfo(remote.path()).fileName().isEmpty()
            ? QStringLiteral("vendor-artifact") : QFileInfo(remote.path()).fileName();
        if (QMessageBox::question(
                this, QStringLiteral("Download vendor artifact"),
                QStringLiteral("Download %1 over HTTPS and inspect it as untrusted input? The server did not provide PacSmith with a trusted publisher checksum; PacSmith will compute and record the downloaded bytes' SHA256.")
                    .arg(remote.toDisplayString())) != QMessageBox::Yes) return;
        pendingImportOptions_ = {};
        pendingImportOptions_.acquisition.kind = AcquisitionKind::DirectUrl;
        pendingImportOptions_.acquisition.canonicalIdentity =
            remote.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
        pendingImportOptions_.acquisition.originalUrl = remote.toString();
        const auto target = defaultDownloadPath(
            PkgbuildGenerator::sanitizePackageName(QFileInfo(filename).completeBaseName()),
            QStringLiteral("direct"), filename);
        const auto targetPath = std::filesystem::path(target.toUtf8().constData());
        onServiceThread(*debDownloadService_, [this, remote, targetPath] {
            debDownloadService_->start(remote, {}, targetPath);
        });
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
        if (!pendingDownloadedImport_.isEmpty()) {
            static_cast<void>(QFile::remove(pendingDownloadedImport_));
            pendingDownloadedImport_.clear();
        }
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
        refreshProjectList(projectId);
        if (!project_ || project_->id != projectId) loadProject(projectId);
        if (project_ && project_->release(releaseId) != nullptr) {
            currentReleaseId_ = releaseId;
            refreshCurrentProject();
        }
        if (preparedUpdate) applyRetentionCleanup();
        const auto loaded = library_.load(projectId);
        statusBar()->showMessage(
            loaded ? QStringLiteral("Imported to %1").arg(projectDirectory(library_, *loaded))
                   : QStringLiteral("Package imported successfully"),
            10000);
        if (preparedUpdate) showReleaseWorkbenchAtFirstAttention(releaseId);
        QTimer::singleShot(0, this, [this, projectId, releaseId] {
            if (!project_ || project_->id != projectId || project_->release(releaseId) == nullptr) return;
            currentReleaseId_ = releaseId;
            const bool needsReview = pendingScriptFindings(*currentRelease()) > 0 ||
                                     pendingPayloadReviews(*currentRelease()) > 0 ||
                                     currentRelease()->installMapping.appRun.requiresReview() ||
                                     std::any_of(currentRelease()->dependencies.cbegin(), currentRelease()->dependencies.cend(),
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
            if (aiSettings_.provider == AiProviderKind::None) {
                showReleaseWorkbenchAtFirstAttention(releaseId);
                QMessageBox::information(
                    this, QStringLiteral("Package needs review"),
                    QStringLiteral("PacSmith found items that need an Arch-specific decision and opened the first section "
                                   "that needs your attention. Resolve the highlighted items, then continue to PKGBUILD and Build.\n\n"
                                   "You can also configure an AI provider with the Settings button to resolve supported items automatically."));
            } else if (aiSettings_.automaticallyResolveReviewItems) {
                startAiResolution();
            } else {
                const auto answer = QMessageBox::question(
                    this, QStringLiteral("Resolve review items with AI?"),
                    QStringLiteral("Local deterministic analysis is complete. Send the bounded package evidence bundle "
                                   "to the configured %1 provider now?\n\n"
                                   "When the review finishes, PacSmith will open the first remaining step, or Build if the package is ready.")
                        .arg(aiProviderName(aiSettings_.provider)));
                if (answer == QMessageBox::Yes) {
                    startAiResolution();
                } else {
                    showReleaseWorkbenchAtFirstAttention(releaseId);
                    statusBar()->showMessage(
                        QStringLiteral("AI review skipped. Resolve the highlighted items, then continue to Build."),
                        12000);
                }
            }
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
        thread->deleteLater();
    });
    thread->start();
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
    PackageRelease probe;
    probe.debian.version = QStringLiteral("0");
    probe.update.strategy = UpdateStrategy::GitHubRelease;
    probe.update.githubOwner = owner;
    probe.update.githubRepository = repository;
    probe.update.githubAssetRegex = initialRegex;
    probe.update.githubIncludePrereleases = false;
    auto *service = networkServiceOnThread<GitHubUpdateService>(networkIoThread_);
    auto *progress = new QProgressDialog(QStringLiteral("Loading GitHub release assets…"),
                                         QStringLiteral("Cancel"), 0, 0, this);
    progress->setWindowTitle(QStringLiteral("Import from GitHub"));
    progress->setWindowModality(Qt::WindowModal);
    progress->show();
    connect(progress, &QProgressDialog::canceled, service, &GitHubUpdateService::cancel);
    connect(service, &GitHubUpdateService::progressChanged, progress, &QProgressDialog::setLabelText);
    connect(service, &GitHubUpdateService::finished, this,
            [this, service, progress, probe, owner, repository, initialRegex,
             requestedTag](const UpdateCheckResult &result) mutable {
        progress->close();
        progress->deleteLater();
        service->deleteLater();
        if (!result.success && result.availableAssets.isEmpty()) {
            QMessageBox::critical(this, QStringLiteral("GitHub import failed"), result.message);
            return;
        }
        if (initialRegex != QStringLiteral(".*") && result.success) {
            downloadGitHubAsset(probe, result);
            return;
        }
        const auto rule = chooseGitHubAssetRule(
            this, result.availableAssets, false,
            [this, probe](const QStringList &assets, const QString &preferred,
                          QLineEdit *editor, QLabel *status, QPushButton *button,
                          QWidget *dialog) {
                startGitHubChooserAi(probe, assets, preferred, editor, status, button,
                                     dialog);
            });
        if (!rule) return;
        continueGitHubImport(owner, repository, rule->expression,
                             rule->includePrereleases, requestedTag);
    });
    QString token = sessionCredential(QStringLiteral("github.token"));
    onServiceThread(*service, [service, probe, token, requestedTag]() mutable {
        service->start(probe, token, requestedTag);
        token.fill(QChar::Null);
    });
    token.fill(QChar::Null);
}

void MainWindow::continueGitHubImport(const QString &owner, const QString &repository,
                                      const QString &assetRegex, const bool includePrereleases,
                                      const QString &requestedTag) {
    PackageRelease probe;
    probe.debian.version = QStringLiteral("0");
    probe.update.strategy = UpdateStrategy::GitHubRelease;
    probe.update.githubOwner = owner;
    probe.update.githubRepository = repository;
    probe.update.githubAssetRegex = assetRegex;
    probe.update.githubIncludePrereleases = includePrereleases;
    auto *service = networkServiceOnThread<GitHubUpdateService>(networkIoThread_);
    auto *progress = new QProgressDialog(QStringLiteral("Resolving selected GitHub asset…"),
                                         QStringLiteral("Cancel"), 0, 0, this);
    progress->setWindowTitle(QStringLiteral("Import from GitHub"));
    progress->setWindowModality(Qt::WindowModal);
    progress->show();
    connect(progress, &QProgressDialog::canceled, service, &GitHubUpdateService::cancel);
    connect(service, &GitHubUpdateService::finished, this,
            [this, service, progress, probe](const UpdateCheckResult &result) {
        progress->close();
        progress->deleteLater();
        service->deleteLater();
        if (!result.success || result.downloadUrl.isEmpty()) {
            QMessageBox::critical(this, QStringLiteral("GitHub import failed"), result.message);
            return;
        }
        downloadGitHubAsset(probe, result);
    });
    QString token = sessionCredential(QStringLiteral("github.token"));
    onServiceThread(*service, [service, probe, token, requestedTag]() mutable {
        service->start(probe, token, requestedTag);
        token.fill(QChar::Null);
    });
    token.fill(QChar::Null);
}

void MainWindow::startGitHubChooserAi(const PackageRelease &probe,
                                      const QStringList &assets,
                                      const QString &preferredAsset,
                                      QLineEdit *editor, QLabel *status,
                                      QPushButton *button, QWidget *dialog) {
    if (editor == nullptr || status == nullptr || button == nullptr || dialog == nullptr) return;
    if (aiSettings_.provider == AiProviderKind::None || aiSettings_.model.trimmed().isEmpty()) {
        QMessageBox::information(
            dialog, QStringLiteral("Configure AI"),
            QStringLiteral("Choose an AI provider and model with the Settings button, then use Generate with AI again."));
        showSettings();
        return;
    }

    QString error;
    const auto job = library_.startGitHubAssetAi(probe.update.githubOwner,
                                                 probe.update.githubRepository, assets,
                                                 preferredAsset, &error);
    if (!job || job->id.isEmpty()) {
        QMessageBox::critical(dialog, QStringLiteral("AI review could not start"), error);
        return;
    }

    auto *progress = new AiProgressDialog(aiSettings_, dialog);
    progress->setWindowTitle(QStringLiteral("Generating GitHub Asset Rule"));
    button->setEnabled(false);
    progress->show();
    progress->setStatus(QStringLiteral("Library daemon is running the AI review…"));
    progress->appendActivity(
        QStringLiteral("The provider call runs on pacsmithd; only asset names are sent."));

    const auto jobId = job->id;
    auto *canceled = new bool(false);
    auto *logAfter = new qint64(0);
    auto *timer = new QTimer(progress);
    connect(progress, &QDialog::rejected, progress, [this, jobId, canceled, button] {
        *canceled = true;
        QString cancelError;
        static_cast<void>(library_.cancelJob(jobId, &cancelError));
        button->setEnabled(true);
    });
    connect(timer, &QTimer::timeout, progress,
            [this, jobId, canceled, logAfter, timer, progress, editor, status, button, dialog,
             assets, preferredAsset] {
        QString pollError;
        const auto current = library_.getJob(jobId, &pollError);
        if (!current) return;
        qint64 next = *logAfter;
        const auto chunk = library_.jobLog(jobId, *logAfter, &next, nullptr);
        *logAfter = next;
        if (!chunk.isEmpty()) {
            for (const auto &line : chunk.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                progress->appendActivity(line);
                progress->setStatus(line);
            }
        }
        if (current->status != QStringLiteral("succeeded") &&
            current->status != QStringLiteral("failed") &&
            current->status != QStringLiteral("interrupted")) {
            return;
        }
        timer->stop();
        button->setEnabled(true);
        if (*canceled || current->status == QStringLiteral("interrupted")) {
            progress->close();
            progress->deleteLater();
            return;
        }
        auto resolution = aiResolutionFromJson(current->result);
        if (!resolution.success && resolution.error.isEmpty()) {
            resolution.error = current->error.isEmpty()
                                   ? QStringLiteral("AI review failed")
                                   : current->error;
        }
        if (!resolution.success) {
            progress->showFailure(resolution.error, resolution.errorDetails);
            return;
        }
        progress->close();
        progress->deleteLater();
        const auto rule = std::find_if(
            resolution.changes.cbegin(), resolution.changes.cend(),
            [](const auto &change) {
                return change.field == QStringLiteral("update.githubAssetRegex");
            });
        if (rule == resolution.changes.cend() || resolution.changes.size() != 1) {
            QMessageBox::warning(
                dialog, QStringLiteral("AI returned an invalid asset rule"),
                QStringLiteral("Expected exactly one update.githubAssetRegex change; nothing was applied."));
            return;
        }
        const auto text = rule->value.trimmed();
        const QRegularExpression expression(text);
        QStringList matches;
        if (expression.isValid() && !text.isEmpty() && text.size() <= 512) {
            for (const auto &asset : assets) {
                const auto match = expression.match(asset);
                if (match.hasMatch() && match.capturedLength() == asset.size()) {
                    matches.append(asset);
                }
            }
        }
        const bool preferredMatched = preferredAsset.isEmpty() ||
            (matches.size() == 1 && matches.first() == preferredAsset);
        if (!expression.isValid() || text.isEmpty() || text.size() > 512 ||
            matches.size() != 1 || !preferredMatched) {
            QMessageBox::warning(
                dialog, QStringLiteral("AI asset rule rejected"),
                QStringLiteral("The generated expression must full-match exactly one available asset%1. It matched %2 asset(s) and was not applied.\n\n/%3/")
                    .arg(preferredAsset.isEmpty()
                             ? QString{}
                             : QStringLiteral("—the selected artifact"))
                    .arg(matches.size())
                    .arg(text));
            return;
        }
        editor->setText(text);
        editor->setFocus();
        status->setText(
            QStringLiteral("AI selected %1. Review the persistent update rule before continuing.\n%2")
                .arg(matches.first(), rule->rationale));
    });
    connect(progress, &QObject::destroyed, progress, [canceled, logAfter] {
        delete canceled;
        delete logAfter;
    });
    timer->start(100);
}

void MainWindow::downloadGitHubAsset(const PackageRelease &probe,
                                     const UpdateCheckResult &result) {
    if (debDownloadService_->isRunning()) return;
    pendingImportOptions_ = {};
    pendingImportOptions_.version = result.detectedVersion;
    pendingImportOptions_.githubAssetRegex = probe.update.githubAssetRegex;
    pendingImportOptions_.githubIncludePrereleases = probe.update.githubIncludePrereleases;
    auto &acquisition = pendingImportOptions_.acquisition;
    acquisition.kind = AcquisitionKind::GitHubRelease;
    acquisition.canonicalIdentity = QStringLiteral("github:%1/%2")
        .arg(probe.update.githubOwner, probe.update.githubRepository);
    acquisition.originalUrl = result.downloadUrl;
    acquisition.githubOwner = probe.update.githubOwner;
    acquisition.githubRepository = probe.update.githubRepository;
    acquisition.githubReleaseId = result.releaseId;
    acquisition.githubPrerelease = result.prerelease;
    acquisition.githubTag = result.tag;
    acquisition.githubAssetId = result.assetId;
    acquisition.githubAssetName = result.filename;
    acquisition.publisherDigest = result.publisherDigest;
    const auto projectId = PkgbuildGenerator::sanitizePackageName(probe.update.githubRepository);
    const auto releaseId = QStringLiteral("%1-%2").arg(result.detectedVersion).arg(result.assetId);
    const auto target = defaultDownloadPath(projectId, releaseId, result.filename);

    downloadProgress_ = new QProgressDialog(
        QStringLiteral("Downloading %1…\nYou may hide this window; the download will continue.")
            .arg(result.filename),
        QStringLiteral("Hide"), 0, 0, this);
    downloadProgress_->setWindowTitle(QStringLiteral("Downloading GitHub Release"));
    downloadProgress_->setWindowModality(Qt::NonModal);
    downloadProgress_->setMinimumDuration(0);
    downloadProgress_->setAutoClose(false);
    downloadProgress_->setAutoReset(false);
    downloadProgress_->setMinimumWidth(460);
    downloadProgress_->show();
    downloadProgress_->raise();
    downloadProgress_->activateWindow();
    startListDownloadActivity(projectId);

    const auto url = QUrl(result.downloadUrl);
    const auto sha = result.sha256;
    const auto path = std::filesystem::path(target.toUtf8().constData());
    onServiceThread(*debDownloadService_, [this, url, sha, path] {
        debDownloadService_->start(url, sha, path);
    });
}


} // namespace pacsmith::gui
