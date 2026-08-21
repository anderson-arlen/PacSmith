#include "gui/main_window/common.hpp"

namespace pacsmith::gui {

void MainWindow::startReanalysis() {
    if (!project_ || currentRelease() == nullptr || importThread_ != nullptr ||
        buildInProgress() || installService_.isRunning() ||
        debDownloadService_->isRunning() || aiInProgress()) {
        return;
    }
    const auto projectId = project_->id;
    const auto releaseId = currentRelease()->id;
    const auto version = currentRelease()->debian.version;
    if (!std::filesystem::is_regular_file(library_.sourcePath(*currentRelease()))) {
        QMessageBox::critical(
            this, QStringLiteral("Cannot reanalyze artifact"),
            QStringLiteral("The immutable stored artifact for this release is missing."));
        return;
    }

    QMessageBox confirmation(
        QMessageBox::Warning, QStringLiteral("Reset and reanalyze release?"),
        QStringLiteral("Reset the package setup for %1?").arg(version),
        QMessageBox::NoButton, this);
    confirmation.setInformativeText(QStringLiteral(
        "PacSmith will verify and reread the stored artifact, then discard this release's dependency overrides, "
        "AI decisions, script and payload acknowledgements, lifecycle script, command mappings, desktop entries, "
        "icon selection, and manual PKGBUILD edits.\n\n"
        "The source artifact, update configuration, installed-package record, prior build history, and package "
        "artifacts are retained. This setup reset cannot be undone."));
    auto *resetButton = confirmation.addButton(
        QStringLiteral("Reset & Reanalyze"), QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != resetButton) return;

    importProgress_ = new QProgressDialog(
        QStringLiteral("Verifying the stored artifact…"), QString{}, 0, 0, this);
    importProgress_->setWindowTitle(QStringLiteral("Resetting Package Setup"));
    importProgress_->setWindowModality(Qt::WindowModal);
    importProgress_->setCancelButton(nullptr);
    importProgress_->setMinimumDuration(0);
    importProgress_->setAutoClose(false);
    importProgress_->setAutoReset(false);
    importProgress_->setMinimumWidth(460);
    importProgress_->show();

    auto *thread = new QThread(this);
    auto *worker = new ReanalyzeWorker(
        library_.projectsRoot(), projectId, releaseId);
    importThread_ = thread;
    projectList_->setEnabled(false);
    resolveWithAiButton_->setEnabled(false);
    reanalyzeButton_->setEnabled(false);
    updateDeleteButton();
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &ReanalyzeWorker::run);
    connect(worker, &ReanalyzeWorker::progressChanged, this,
            [this](const QString &description) {
                if (importProgress_ != nullptr) importProgress_->setLabelText(description);
                statusBar()->showMessage(description);
            });
    connect(worker, &ReanalyzeWorker::completed, this,
            [this, projectId, releaseId](const QString &completedProjectId,
                                         const QString &completedReleaseId,
                                         const QString &error) {
                if (importProgress_ != nullptr) {
                    importProgress_->close();
                    importProgress_->deleteLater();
                    importProgress_ = nullptr;
                }
                projectList_->setEnabled(true);
                if (completedProjectId.isEmpty()) {
                    statusBar()->clearMessage();
                    QMessageBox::critical(this, QStringLiteral("Reanalysis failed"), error);
                    updateDeleteButton();
                    return;
                }
                refreshProjectList(projectId);
                loadProject(projectId);
                currentReleaseId_ = completedReleaseId.isEmpty()
                    ? releaseId : completedReleaseId;
                const auto *release = currentRelease();
                const auto commands = release == nullptr
                    ? 0 : release->installMapping.launchers.size();
                const auto desktopEntries = release == nullptr
                    ? 0 : release->installMapping.desktopEntries.size();
                const auto iconDetected = release != nullptr &&
                    release->installMapping.icon.sourceKind != IconSourceKind::None;
                showReleaseWorkbenchAtFirstAttention(currentReleaseId_);
                statusBar()->showMessage(
                    QStringLiteral("Artifact reanalyzed from a blank package setup"), 12000);
                QMessageBox::information(
                    this, QStringLiteral("Artifact reanalyzed"),
                    QStringLiteral(
                        "PacSmith rebuilt this release's setup from the stored artifact.\n\n"
                        "Detected commands: %1\nDesktop entries: %2\nIcon: %3\n\n"
                        "The first section that still needs attention is now open.")
                        .arg(commands)
                        .arg(desktopEntries)
                        .arg(iconDetected ? QStringLiteral("detected")
                                          : QStringLiteral("not detected")));
                updateDeleteButton();
            });
    connect(worker, &ReanalyzeWorker::completed, thread, &QThread::quit);
    connect(worker, &ReanalyzeWorker::completed, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread] {
        if (importProgress_ != nullptr) {
            importProgress_->close();
            importProgress_->deleteLater();
            importProgress_ = nullptr;
        }
        if (importThread_ == thread) importThread_ = nullptr;
        projectList_->setEnabled(true);
        updateDeleteButton();
        thread->deleteLater();
    });
    thread->start();
}

void MainWindow::startAiResolution() {
    if (!project_ || currentRelease() == nullptr || aiInProgress()) return;
    if (aiSettings_.provider == AiProviderKind::None) {
        QMessageBox::information(this, QStringLiteral("Configure AI"),
                                 QStringLiteral("Choose an AI provider with the Settings button first."));
        showSettings();
        return;
    }
    if (aiSettings_.model.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("AI model required"),
                             QStringLiteral("Set a provider model ID with the Settings button."));
        return;
    }
    aiProgressCanceled_ = false;
    aiProgress_ = new AiProgressDialog(aiSettings_, this);
    auto *progressDialog = aiProgress_;
    aiProgress_->show();
    connect(aiProgress_, &QDialog::rejected, this, [this, progressDialog] {
        if (aiProgress_ != progressDialog) return;
        if (aiInProgress()) {
            aiProgressCanceled_ = true;
            cancelRemoteAi();
            return;
        }
        progressDialog->deleteLater();
        aiProgress_ = nullptr;
        projectList_->setEnabled(true);
        updateDeleteButton();
    });
    projectList_->setEnabled(false);
    resolveWithAiButton_->setEnabled(false);
    QString error;
    const auto job = library_.startAiReview(currentRelease()->id, &error);
    if (!job || job->id.isEmpty()) {
        if (aiProgress_ != nullptr) {
            aiProgress_->showFailure(error.isEmpty()
                                         ? QStringLiteral("AI review could not start.")
                                         : error,
                                     {});
        } else {
            QMessageBox::critical(this, QStringLiteral("AI review could not start"), error);
        }
        projectList_->setEnabled(true);
        updateDeleteButton();
        return;
    }
    beginAiJob(*job);
    updateDeleteButton();
}

void MainWindow::startGithubRegexAi() {
    auto *tracker = updateEditorRelease();
    if (!project_ || tracker == nullptr || aiInProgress()) return;
    if (aiSettings_.provider == AiProviderKind::None || aiSettings_.model.trimmed().isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("Configure AI"),
            QStringLiteral("Choose an AI provider and model with the Settings button before generating a GitHub asset rule."));
        showSettings();
        return;
    }
    const auto owner = githubOwner_->text().trimmed();
    const auto repository = githubRepository_->text().trimmed();
    if (owner.isEmpty() || repository.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("GitHub repository required"),
                             QStringLiteral("Enter the GitHub owner and repository first."));
        return;
    }
    PackageRelease probe = *tracker;
    probe.update.strategy = UpdateStrategy::GitHubRelease;
    probe.update.githubOwner = owner;
    probe.update.githubRepository = repository;
    probe.update.githubAssetRegex = QStringLiteral(".*");
    probe.update.githubIncludePrereleases = githubPrereleases_->isChecked();
    probe.update.githubEtag.clear();

    QString githubToken = sessionCredential(QStringLiteral("github.token"));

    auto *service = networkServiceOnThread<GitHubUpdateService>(networkIoThread_);
    auto *progress = new QProgressDialog(QStringLiteral("Loading the latest GitHub release assets…"),
                                         QStringLiteral("Cancel"), 0, 0, this);
    progress->setWindowTitle(QStringLiteral("Generate GitHub Asset Rule"));
    progress->setWindowModality(Qt::WindowModal);
    progress->show();
    connect(progress, &QProgressDialog::canceled, service, &GitHubUpdateService::cancel);
    connect(service, &GitHubUpdateService::progressChanged, progress,
            &QProgressDialog::setLabelText);
    connect(service, &GitHubUpdateService::finished, this,
            [this, service, progress, probe](const UpdateCheckResult &result) mutable {
        progress->close();
        progress->deleteLater();
        service->deleteLater();
        QStringList assets;
        for (const auto &asset : result.availableAssets) {
            if (isGitHubSidecarAsset(asset)) continue;
            if (!assets.contains(asset)) assets.append(asset);
        }
        if (assets.isEmpty()) {
            QMessageBox::critical(
                this, QStringLiteral("No supported GitHub assets"),
                result.message.isEmpty()
                    ? QStringLiteral("The latest release has no supported prebuilt Linux assets for PacSmith to evaluate.")
                    : result.message);
            return;
        }
        QStringList choices{QStringLiteral("Let AI choose the best supported artifact")};
        choices.append(assets);
        bool accepted = false;
        const auto choice = QInputDialog::getItem(
            this, QStringLiteral("Preferred GitHub artifact"),
            QStringLiteral("Optionally select the artifact family PacSmith should track:"),
            choices, 0, false, &accepted);
        if (!accepted) return;
        const auto preferred = choice == choices.first() ? QString{} : choice;

        githubRegexAiPending_ = true;
        githubRegexAiReleaseId_ = probe.id;
        githubRegexAiAssets_ = assets;
        githubRegexAiPreferredAsset_ = preferred;
        aiProgressCanceled_ = false;
        aiProgress_ = new AiProgressDialog(aiSettings_, this);
        auto *progressDialog = aiProgress_;
        aiProgress_->setWindowTitle(QStringLiteral("Generating GitHub Asset Rule"));
        aiProgress_->show();
        connect(aiProgress_, &QDialog::rejected, this, [this, progressDialog] {
            if (aiProgress_ != progressDialog) return;
            if (aiInProgress()) {
                aiProgressCanceled_ = true;
                cancelRemoteAi();
                return;
            }
            progressDialog->deleteLater();
            aiProgress_ = nullptr;
            projectList_->setEnabled(true);
            updateDeleteButton();
        });
        projectList_->setEnabled(false);
        githubRegexAiButton_->setEnabled(false);
        QString error;
        const auto job = library_.startGitHubAssetAi(probe.update.githubOwner,
                                                     probe.update.githubRepository, assets,
                                                     preferred, &error);
        if (!job || job->id.isEmpty()) {
            githubRegexAiPending_ = false;
            githubRegexAiReleaseId_.clear();
            githubRegexAiAssets_.clear();
            githubRegexAiPreferredAsset_.clear();
            if (aiProgress_ != nullptr) {
                aiProgress_->showFailure(error.isEmpty()
                                             ? QStringLiteral("GitHub asset review could not start.")
                                             : error,
                                         {});
            }
            projectList_->setEnabled(true);
            updateDeleteButton();
            return;
        }
        beginAiJob(*job);
        updateDeleteButton();
    });
    onServiceThread(*service, [service, probe, githubToken]() mutable {
        service->start(probe, githubToken);
        githubToken.fill(QChar::Null);
    });
    githubToken.fill(QChar::Null);
}

void MainWindow::applyGithubRegexAi(const AiResolution &resolution) {
    const auto releaseId = std::exchange(githubRegexAiReleaseId_, QString{});
    const auto assets = std::exchange(githubRegexAiAssets_, QStringList{});
    const auto preferred = std::exchange(githubRegexAiPreferredAsset_, QString{});
    githubRegexAiPending_ = false;
    const auto *tracker = updateEditorRelease();
    if (tracker == nullptr || tracker->id != releaseId) {
        QMessageBox::warning(this, QStringLiteral("Active release changed"),
                             QStringLiteral("The generated rule was not applied because the active update release changed."));
        populateUpdates();
        return;
    }
    const auto rule = std::find_if(resolution.changes.cbegin(), resolution.changes.cend(),
                                   [](const auto &change) {
                                       return change.field == QStringLiteral("update.githubAssetRegex");
                                   });
    if (rule == resolution.changes.cend() || resolution.changes.size() != 1) {
        QMessageBox::warning(this, QStringLiteral("AI returned an invalid asset rule"),
                             QStringLiteral("Expected exactly one update.githubAssetRegex change; nothing was applied."));
        populateUpdates();
        return;
    }
    const auto text = rule->value.trimmed();
    const QRegularExpression expression(text);
    QStringList matches;
    if (expression.isValid() && !text.isEmpty() && text.size() <= 512) {
        for (const auto &asset : assets) {
            const auto match = expression.match(asset);
            if (match.hasMatch() && match.capturedLength() == asset.size()) matches.append(asset);
        }
    }
    const bool preferredMatched = preferred.isEmpty() ||
                                  (matches.size() == 1 && matches.first() == preferred);
    if (!expression.isValid() || text.isEmpty() || text.size() > 512 ||
        matches.size() != 1 || !preferredMatched) {
        QMessageBox::warning(
            this, QStringLiteral("AI asset rule rejected"),
            QStringLiteral("PacSmith requires a valid expression of at most 512 characters that full-matches exactly one available asset%1. The proposed expression matched %2 asset(s) and was not applied.\n\n/%3/")
                .arg(preferred.isEmpty() ? QString{} : QStringLiteral("—the selected preferred artifact"))
                .arg(matches.size())
                .arg(text));
        populateUpdates();
        return;
    }
    githubAssetRegex_->setText(text);
    githubAssetRegex_->setFocus();
    updateNotice_->setText(
        QStringLiteral("AI selected %1. Review the expression, then choose Save Update Configuration; it has not been persisted yet.\n%2")
            .arg(matches.first(), rule->rationale));
    statusBar()->showMessage(QStringLiteral("Generated GitHub asset rule applied to the editor; save to persist it"), 10000);
    githubRegexAiButton_->setEnabled(true);
}

void MainWindow::validateAndApplyAiResolution(const AiResolution &resolution) {
    if (!project_ || currentRelease() == nullptr) {
        finishAiResolution(resolution);
        return;
    }
    const auto candidates = requiredAiDependencyCandidates(*currentRelease(), resolution);
    if (candidates.isEmpty()) {
        finishAiResolution(resolution);
        return;
    }
    if (aiProgress_ != nullptr) {
        aiProgress_->setStatus(QStringLiteral("Verifying proposed Arch packages…"));
        aiProgress_->appendActivity(
            QStringLiteral("Checking %1 required package mapping(s) against configured pacman repositories.")
                .arg(candidates.size()));
    }
    auto *watcher = new QFutureWatcher<QJsonArray>(this);
    connect(watcher, &QFutureWatcher<QJsonArray>::finished, this,
            [this, watcher, resolution] {
        const auto results = watcher->result();
        watcher->deleteLater();
        if (std::exchange(aiProgressCanceled_, false)) {
            if (aiProgress_ != nullptr) {
                aiProgress_->hide();
                aiProgress_->deleteLater();
                aiProgress_ = nullptr;
            }
            projectList_->setEnabled(true);
            updateDeleteButton();
            return;
        }
        QSet<int> unavailableIndexes;
        QStringList unavailablePackages;
        for (const auto &value : results) {
            const auto result = value.toObject();
            if (result.value(QStringLiteral("available")).toBool()) continue;
            unavailableIndexes.insert(result.value(QStringLiteral("dependencyIndex")).toInt(-1));
            unavailablePackages.append(result.value(QStringLiteral("argument")).toString());
        }
        unavailableIndexes.remove(-1);
        unavailablePackages.removeDuplicates();
        if (unavailableIndexes.isEmpty()) {
            finishAiResolution(resolution);
            return;
        }
        AiResolution safeResolution = resolution;
        safeResolution.changes.erase(
            std::remove_if(safeResolution.changes.begin(), safeResolution.changes.end(),
                           [&unavailableIndexes](const AiFieldChange &change) {
                static const QRegularExpression dependencyPattern(
                    QStringLiteral(R"(^dependency\.(\d+)\.(?:archPackage|treatment)$)"));
                const auto match = dependencyPattern.match(change.field);
                return match.hasMatch() && unavailableIndexes.contains(match.captured(1).toInt());
            }),
            safeResolution.changes.end());
        for (const auto index : unavailableIndexes) {
            safeResolution.changes.append(
                {QStringLiteral("dependency.%1.treatment").arg(index),
                 QStringLiteral("unresolved"),
                 QStringLiteral("PacSmith could not find the AI-proposed package in any configured pacman repository; the mapping was cleared without sending another AI request.")});
        }
        safeResolution.rationale += QStringLiteral(
            " PacSmith deterministically cleared mappings that were unavailable after repository validation; AI review is single-request.");
        if (aiProgress_ != nullptr) {
            aiProgress_->appendActivity(
                QStringLiteral("Rejected unavailable repository mapping(s): %1. Leaving those dependencies unresolved; no follow-up AI request was sent.")
                    .arg(unavailablePackages.join(QStringLiteral(", "))));
        }
        finishAiResolution(safeResolution);
    });
    watcher->setFuture(QtConcurrent::run([candidates] {
        QJsonArray results;
        for (const auto &candidate : candidates) {
            auto result = SystemInformationBroker::execute(
                {QStringLiteral("pacsmith-repository-validation-%1").arg(candidate.index),
                 QStringLiteral("repository-package"), candidate.package,
                 QStringLiteral("Verify that the proposed required Arch dependency can be installed from a configured pacman repository")});
            result.insert(QStringLiteral("dependencyIndex"), candidate.index);
            result.insert(QStringLiteral("source"), QStringLiteral("pacsmith-automatic-repository-validation"));
            results.append(result);
        }
        return results;
    }));
}

void MainWindow::finishAiResolution(const AiResolution &resolution) {
    if (aiProgress_ != nullptr) aiProgress_->releaseModality();
    projectList_->setEnabled(true);
    updateDeleteButton();
    applyAiResolution(resolution);
}

bool MainWindow::aiInProgress() const {
    return !aiJobId_.isEmpty();
}

void MainWindow::beginAiJob(const JobStatus &job) {
    aiJobId_ = job.id;
    aiLogAfter_ = 0;
    if (aiPollTimer_ == nullptr) {
        aiPollTimer_ = new QTimer(this);
        connect(aiPollTimer_, &QTimer::timeout, this, &MainWindow::pollAiJob);
    }
    aiPollTimer_->start(100);
    if (aiProgress_ != nullptr) {
        aiProgress_->setStatus(QStringLiteral("Library daemon is running the AI review…"));
        aiProgress_->appendActivity(
            QStringLiteral("The provider call runs on pacsmithd; package binaries are not sent."));
    }
}

void MainWindow::cancelRemoteAi() {
    if (aiJobId_.isEmpty()) return;
    QString error;
    static_cast<void>(library_.cancelJob(aiJobId_, &error));
}

void MainWindow::pollAiJob() {
    if (aiJobId_.isEmpty()) return;
    QString error;
    const auto job = library_.getJob(aiJobId_, &error);
    if (!job) return;
    qint64 next = aiLogAfter_;
    const auto chunk = library_.jobLog(aiJobId_, aiLogAfter_, &next, nullptr);
    if (!chunk.isEmpty() && aiProgress_ != nullptr) {
        for (const auto &line : chunk.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            aiProgress_->appendActivity(line);
            aiProgress_->setStatus(line);
        }
    }
    aiLogAfter_ = next;
    if (job->status == QStringLiteral("succeeded") || job->status == QStringLiteral("failed") ||
        job->status == QStringLiteral("interrupted")) {
        if (aiPollTimer_ != nullptr) aiPollTimer_->stop();
        finishAiJob();
    }
}

void MainWindow::finishAiJob() {
    const auto jobId = aiJobId_;
    aiJobId_.clear();
    QString error;
    const auto job = library_.getJob(jobId, &error);
    const auto canceled = std::exchange(aiProgressCanceled_, false) ||
                          (job && job->status == QStringLiteral("interrupted"));
    if (canceled) {
        githubRegexAiPending_ = false;
        githubRegexAiReleaseId_.clear();
        githubRegexAiAssets_.clear();
        githubRegexAiPreferredAsset_.clear();
        if (aiProgress_ != nullptr) {
            aiProgress_->hide();
            aiProgress_->deleteLater();
            aiProgress_ = nullptr;
        }
        projectList_->setEnabled(true);
        resolveWithAiButton_->setEnabled(
            project_ && currentRelease() != nullptr &&
            currentRelease()->state != ReleaseState::Discovered);
        populateUpdates();
        updateDeleteButton();
        return;
    }
    AiResolution resolution;
    if (job) resolution = aiResolutionFromJson(job->result);
    if (resolution.provider.isEmpty()) resolution.provider = aiProviderName(aiSettings_.provider);
    if (resolution.model.isEmpty()) resolution.model = aiSettings_.model;
    if (!resolution.success && resolution.error.isEmpty() && job) {
        resolution.error = job->error.isEmpty()
                               ? QStringLiteral("AI review failed")
                               : job->error;
    }
    if (!resolution.success) {
        githubRegexAiPending_ = false;
        githubRegexAiReleaseId_.clear();
        githubRegexAiAssets_.clear();
        githubRegexAiPreferredAsset_.clear();
        projectList_->setEnabled(true);
        resolveWithAiButton_->setEnabled(
            project_ && currentRelease() != nullptr &&
            currentRelease()->state != ReleaseState::Discovered);
        updateDeleteButton();
        if (aiProgress_ != nullptr) {
            aiProgress_->showFailure(resolution.error, resolution.errorDetails);
        } else {
            showAiErrorDialog(this, resolution);
        }
        populateUpdates();
        return;
    }
    if (githubRegexAiPending_) {
        if (aiProgress_ != nullptr) {
            aiProgress_->hide();
            aiProgress_->deleteLater();
            aiProgress_ = nullptr;
        }
        projectList_->setEnabled(true);
        updateDeleteButton();
        applyGithubRegexAi(resolution);
    } else {
        if (aiProgress_ != nullptr) {
            const auto json = QJsonDocument(job ? job->result : QJsonObject{}).toJson(QJsonDocument::Indented);
            aiProgress_->setCompletedResponse(QString::fromUtf8(json));
            aiProgress_->appendActivity(QStringLiteral("Provider response received; applying accepted recommendations."));
        }
        validateAndApplyAiResolution(resolution);
    }
}

void MainWindow::applyAiResolution(const AiResolution &resolution) {
    if (!project_ || currentRelease() == nullptr) return;
    const auto projectId = project_->id;
    const auto releaseId = currentRelease()->id;
    QSet<QString> approved;
    const auto conflicts = AiResolutionApplier::manualConflicts(*currentRelease(), resolution);
    if (!conflicts.isEmpty()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Replace manually edited values?"),
            QStringLiteral("AI proposed replacements for these user-owned fields:\n\n%1\n\nApply those replacements?")
                .arg(conflicts.join(QLatin1Char('\n'))),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) approved = QSet<QString>(conflicts.cbegin(), conflicts.cend());
    }
    const auto explicitApprovals =
        AiResolutionApplier::explicitApprovalRequired(*currentRelease(), resolution);
    for (const auto &field : explicitApprovals) {
        const auto proposal = std::find_if(
            resolution.changes.cbegin(), resolution.changes.cend(),
            [&field](const auto &change) { return change.field == field; });
        if (proposal == resolution.changes.cend()) continue;
        auto path = field;
        path.remove(0, QStringLiteral("payload.").size());
        path.chop(QStringLiteral(".treatment").size());
        auto treatment = proposal->value.trimmed().toLower();
        if (treatment == QStringLiteral("include") ||
            treatment == QStringLiteral("included")) {
            treatment = QStringLiteral("keep");
        }
        const auto fingerprint = PayloadReview::fingerprint(*currentRelease(), path);
        QMessageBox approval(
            QMessageBox::Warning, QStringLiteral("Approve unclassified payload change?"),
            QStringLiteral("The AI proposes to <b>%1</b> an existing payload path that PacSmith did not pre-classify as requiring a decision:<br><br><code>%2</code>")
                .arg(treatment.toHtmlEscaped(), path.toHtmlEscaped()),
            QMessageBox::NoButton, this);
        approval.setTextFormat(Qt::RichText);
        approval.setInformativeText(
            QStringLiteral("<p>This is a packaging judgment, not an archive-safety failure. Approving binds the decision to the current file or directory contents; changed content in a later release must be reviewed again.</p><p><b>AI rationale:</b> %1</p>")
                .arg(proposal->rationale.isEmpty()
                         ? QStringLiteral("No rationale supplied")
                         : proposal->rationale.toHtmlEscaped()));
        approval.setDetailedText(
            QStringLiteral("Field: %1\nTreatment: %2\nPayload fingerprint: %3")
                .arg(field, treatment, fingerprint));
        auto *approve = approval.addButton(QStringLiteral("Approve Exact Content"),
                                           QMessageBox::AcceptRole);
        approval.addButton(QStringLiteral("Keep Blocked"), QMessageBox::RejectRole);
        approval.exec();
        if (approval.clickedButton() == approve) approved.insert(field);
    }
    const auto applied = AiResolutionApplier::apply(*currentRelease(), resolution, approved);
    if (currentRelease()->update.strategy == UpdateStrategy::Manual && !currentRelease()->update.url.isEmpty() &&
        !currentRelease()->update.aptSuite.isEmpty()) {
        currentRelease()->update.strategy = UpdateStrategy::AptRepository;
    }
    currentRelease()->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("ai-resolution"),
                              QStringLiteral("Applied %1 change(s) from %2/%3")
                                  .arg(resolution.changes.size()).arg(resolution.provider, resolution.model)});
    if (!currentRelease()->lifecycleScript.contents.isEmpty()) {
        QString error;
        if (!library_.saveLifecycle(*project_, *currentRelease(), &error)) {
            QMessageBox::critical(this, QStringLiteral("Could not save generated lifecycle script"), error);
            return;
        }
        projectCache_.insert(project_->id, *project_);
    }
    refreshGeneratedPkgbuildAfterModelChange();

    {
        QSignalBlocker blocker(projectList_);
        refreshProjectList(projectId);
    }
    if (!project_ || project_->id != projectId) loadProject(projectId);
    if (!project_ || project_->release(releaseId) == nullptr) {
        QMessageBox::critical(
            this, QStringLiteral("AI review completed"),
            QStringLiteral("The AI result was saved, but PacSmith could not reopen the reviewed release."));
        return;
    }
    currentReleaseId_ = releaseId;

    const auto attention = firstReviewSection(*currentRelease());
    const auto remainingPayload = pendingPayloadReviews(*currentRelease());
    const auto remainingPaths = pendingPayloadReviewPaths(*currentRelease());
    const bool readyToBuild = !attention.has_value();
    const auto stepName = sectionTitle(attention.value_or(EditorSection::ResultBuild));
    const auto appliedSummary = applied.changed
        ? QStringLiteral("PacSmith applied the accepted AI recommendations.")
        : QStringLiteral("The AI review completed without an accepted configuration change.");
    QString nextStep;
    if (readyToBuild) {
        nextStep = QStringLiteral("No review step remains. PacSmith opened Build; review the readiness checklist, then build the package.");
    } else if (remainingPayload > 0) {
        nextStep = QStringLiteral("PacSmith opened Contents. %1 payload path(s) still need an explicit keep or exclude:\n%2%3")
                       .arg(remainingPayload)
                       .arg(remainingPaths.join(QLatin1Char('\n')))
                       .arg(remainingPayload > remainingPaths.size()
                                ? QStringLiteral("\n…")
                                : QString{});
    } else {
        nextStep = QStringLiteral("PacSmith opened %1, the first section that still needs your attention. "
                                  "Resolve the highlighted items, then continue through PKGBUILD to Build.")
                       .arg(stepName);
    }
    QString details;
    if (!applied.errors.isEmpty()) {
        details = QStringLiteral("Blocked %1 proposal(s); no blocked proposal was applied.\n\n%2")
                      .arg(applied.errors.size())
                      .arg(applied.errors.join(QStringLiteral("\n\n----------------------------------------\n\n")));
    }
    if (aiProgress_ != nullptr) {
        aiProgress_->showCompletion(QStringLiteral("%1\n\n%2").arg(appliedSummary, nextStep), details);
    } else if (!applied.errors.isEmpty()) {
        showDetailedMessageDialog(
            this, QStringLiteral("AI review complete with blocked proposals"),
            QStringLiteral("%1\n\nPacSmith blocked %2 proposal(s); no blocked proposal was applied.\n\n%3")
                .arg(appliedSummary).arg(applied.errors.size()).arg(nextStep),
            applied.errors.join(QStringLiteral("\n\n----------------------------------------\n\n")),
            QStyle::SP_MessageBoxWarning, true);
    } else {
        QMessageBox::information(this, QStringLiteral("AI review complete"),
                                 QStringLiteral("%1\n\n%2").arg(appliedSummary, nextStep));
    }
    statusBar()->showMessage(
        readyToBuild
            ? QStringLiteral("AI review complete — next: review readiness and build")
            : QStringLiteral("AI review complete — next: resolve the highlighted items on %1").arg(stepName),
        15000);
    QTimer::singleShot(0, this, [this, projectId, releaseId] {
        if (!project_ || project_->id != projectId || project_->release(releaseId) == nullptr) return;
        currentReleaseId_ = releaseId;
        showReleaseWorkbenchAtFirstAttention(releaseId);
    });
}

void MainWindow::applyRetentionCleanup() {
    if (!project_ || !aiSettings_.updates.automaticallyPrepare) return;
    QString error;
    const auto result = library_.cleanup(
        *project_,
        {aiSettings_.updates.retainedPackageVersions,
         aiSettings_.updates.retainedCompleteReleases, true},
        &error);
    if (result.removedReleases.isEmpty() && result.removedArtifacts.isEmpty()) return;
    const auto projectId = project_->id;
    const auto releaseId = currentReleaseId_;
    refreshProjectList(projectId);
    if (!project_ || project_->id != projectId) loadProject(projectId);
    if (project_ && project_->release(releaseId) == nullptr) {
        if (const auto *newest = project_->newestRelease()) currentReleaseId_ = newest->id;
        else currentReleaseId_.clear();
    }
    refreshCurrentProject();
}

void MainWindow::startUpdateCheck() {
    if (!project_ || aptUpdateService_->isRunning() || rpmUpdateService_->isRunning() ||
        githubUpdateService_->isRunning() ||
        debDownloadService_->isRunning() || importThread_ != nullptr) return;
    if (!saveUpdateConfiguration()) return;
    auto *tracker = updateEditorRelease();
    if (tracker == nullptr) return;
    const auto strategy = tracker->update.strategy;
    if (strategy != UpdateStrategy::AptRepository && strategy != UpdateStrategy::RpmRepository &&
        strategy != UpdateStrategy::GitHubRelease) {
        QMessageBox::information(this, QStringLiteral("Update check"),
                                 QStringLiteral("Select an APT repository, RPM repository, or GitHub releases to run an automatic metadata check."));
        return;
    }
    const auto backgroundState = BackgroundUpdateStateStore::load();
    if (backgroundState.checking && !aptUpdateService_->isRunning() &&
        !rpmUpdateService_->isRunning() && !githubUpdateService_->isRunning()) {
        statusBar()->showMessage(QStringLiteral("An update check is already running"), 6000);
        return;
    }
    updateCheckButton_->setEnabled(false);
    if (historyCheckUpdatesButton_ != nullptr) historyCheckUpdatesButton_->setEnabled(false);
    projectList_->setEnabled(false);
    updateCheckReleaseId_ = tracker->id;
    updateCheckFromWorkbench_ = rightStack_ != nullptr && rightStack_->currentIndex() == 1;
    if (strategy == UpdateStrategy::AptRepository) {
        updateCheckStatus_->setText(QStringLiteral("Starting APT repository check…"));
        const auto release = *tracker;
        const auto path = library_.releasePath(*tracker);
        onServiceThread(*aptUpdateService_, [this, release, path] {
            aptUpdateService_->start(release, path);
        });
    } else if (strategy == UpdateStrategy::RpmRepository) {
        updateCheckStatus_->setText(QStringLiteral("Starting RPM repository check…"));
        const auto release = *tracker;
        const auto path = library_.releasePath(*tracker);
        onServiceThread(*rpmUpdateService_, [this, release, path] {
            rpmUpdateService_->start(release, path);
        });
    } else {
        QString token = sessionCredential(QStringLiteral("github.token"));
        updateCheckStatus_->setText(QStringLiteral("Starting GitHub release check…"));
        const auto release = *tracker;
        onServiceThread(*githubUpdateService_, [this, release, token]() mutable {
            githubUpdateService_->start(release, token);
            token.fill(QChar::Null);
        });
        token.fill(QChar::Null);
    }
    const auto projectName = project_->displayName.isEmpty() ? project_->archPackageName
                                                             : project_->displayName;
    publishUpdateCheckActivity(true, project_->id, projectName);
    updateDeleteButton();
}

void MainWindow::applyUpdateCheckResult(const UpdateCheckResult &result,
                                        const QString &sourceName) {
    projectList_->setEnabled(true);
    updateDeleteButton();
    publishUpdateCheckActivity(false, project_ ? project_->id : QString{});
    if (!project_) return;
    const auto checkedReleaseId = updateCheckReleaseId_;
    const bool remainInWorkbench = updateCheckFromWorkbench_;
    auto *tracker = project_->release(checkedReleaseId);
    updateCheckReleaseId_.clear();
    updateCheckFromWorkbench_ = false;
    if (tracker == nullptr) {
        statusBar()->showMessage(QStringLiteral("The release used for the update check is no longer available"), 8000);
        populateUpdates();
        return;
    }
    auto &update = tracker->update;
    update.lastChecked = QDateTime::currentDateTimeUtc();
    update.lastCheckMessage = result.message;
    update.signatureVerified = result.signatureVerified;
    if (!result.etag.isEmpty()) update.githubEtag = result.etag;
    if (result.success && !result.detectedVersion.isEmpty()) {
        update.detectedVersion = result.detectedVersion;
        update.detectedFilename = result.filename;
        update.detectedSha256 = result.sha256;
        update.detectedUrl = result.downloadUrl;
        update.githubReleaseId = result.releaseId;
        update.githubAssetId = result.assetId;
        update.githubTag = result.tag;
        update.githubPublisherDigest = result.publisherDigest;
    }
    tracker->history.append(
        {update.lastChecked, QStringLiteral("update-check"), result.message});
    QString discoveryError;
    QString discoveredReleaseId;
    ReleaseState discoveredState = ReleaseState::Discovered;
    if (result.success && result.updateAvailable) {
        const auto trackerSnapshot = *tracker;
        if (const auto *discovered = library_.recordDiscoveredRelease(
                *project_, trackerSnapshot, result.detectedVersion, result.filename,
                result.sha256, result.downloadUrl, &discoveryError, result.releaseId,
                result.assetId, result.tag, result.publisherDigest, result.prerelease);
            discovered != nullptr) {
            discoveredReleaseId = discovered->id;
            discoveredState = discovered->state;
        }
    } else {
        persistCurrent();
    }
    if (!discoveryError.isEmpty()) {
        statusBar()->showMessage(discoveryError, 8000);
    }
    const auto projectId = project_->id;
    refreshProjectList(projectId);
    if (!project_ || project_->id != projectId) loadProject(projectId);
    applyRetentionCleanup();
    syncTrayUpdateCensus();
    if (remainInWorkbench && project_.has_value() &&
        project_->release(checkedReleaseId) != nullptr) {
        showReleaseWorkbench(checkedReleaseId);
        selectSection(EditorSection::ConfigUpdates);
        populateUpdates();
    } else {
        populateUpdates();
        populateOverview();
        populateHistory();
    }

    if (result.success && result.updateAvailable && !discoveredReleaseId.isEmpty()) {
        if (!remainInWorkbench) selectDashboardRelease(discoveredReleaseId);
        statusBar()->showMessage(
            QStringLiteral("%1 %2 is available").arg(project_->displayName, result.detectedVersion),
            12000);
        if (discoveredState != ReleaseState::Discovered) return;
        if (aiSettings_.updates.automaticallyPrepare) {
            beginReleasePreparation(discoveredReleaseId, false);
            return;
        }

        QMessageBox available(QMessageBox::Warning, QStringLiteral("Update available"),
            QStringLiteral("%1 %2 is available. Download and inspect the vendor artifact now?\n\n"
                           "You can choose Later; PacSmith will keep the update alert in the package list and the release in Versions.")
                .arg(project_->displayName, result.detectedVersion),
            QMessageBox::NoButton, this);
        auto *download = available.addButton(QStringLiteral("Download & Inspect"),
                                             QMessageBox::AcceptRole);
        available.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
        available.exec();
        if (available.clickedButton() == download) {
            beginReleasePreparation(discoveredReleaseId, false);
        }
        return;
    }

    statusBar()->showMessage(
        result.success ? QStringLiteral("%1 update check completed; package is current").arg(sourceName)
                       : QStringLiteral("%1 update check failed").arg(sourceName),
        8000);
}

void MainWindow::showCommandProgress(const QString &title, const QString &status,
                                     const bool cancelable) {
    if (commandProgress_ != nullptr) {
        auto *previous = commandProgress_;
        commandProgress_ = nullptr;
        previous->hide();
        previous->deleteLater();
    }
    auto *dialog = new CommandProgressDialog(this);
    commandProgress_ = dialog;
    dialog->setWindowTitle(title);
    dialog->setStatus(status);
    dialog->setCancelable(cancelable);
    connect(dialog, &CommandProgressDialog::cancelRequested, this, [this, dialog] {
        if (commandProgress_ != dialog || !buildInProgress()) return;
        cancelRemoteBuild();
        dialog->setStatus(QStringLiteral("Canceling build…"));
        dialog->setCancelable(false);
        dialog->appendOutput(QStringLiteral("\n[PacSmith] Cancel requested.\n"));
    });
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (commandProgress_ == dialog) commandProgress_ = nullptr;
        dialog->deleteLater();
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::finishCommandProgress(const bool success, const QString &summary) {
    if (commandProgress_ == nullptr) return;
    commandProgress_->markFinished(success, summary);
}

void MainWindow::startBuild(const bool installWhenSuccessful) {
    installAfterSuccessfulBuild_ = false;
    if (!project_ || currentRelease() == nullptr || buildInProgress()) return;
    bool lifecycleChanged = false;
    QString lifecycleError;
    if (!library_.synchronizeLifecycle(*project_, *currentRelease(), &lifecycleChanged, &lifecycleError)) {
        QMessageBox::critical(this, QStringLiteral("Could not inspect lifecycle script"), lifecycleError);
        return;
    }
    if (lifecycleChanged) {
        populateScripts();
        populateOverview();
        populateBuild();
        QMessageBox::information(
            this, QStringLiteral("Lifecycle file synchronized"),
            QStringLiteral("PacSmith restored a missing project-local .install file from its exact recorded content, or detected a direct edit. Prior build results were cleared. Review Configuration → Scripts, then build again."));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ConfigScripts);
        return;
    }
    if (!currentRelease()->lifecycleScript.contents.isEmpty() &&
        !currentRelease()->lifecycleScript.validationPassed) {
        QMessageBox::warning(
            this, QStringLiteral("Lifecycle script is not buildable"),
            QStringLiteral("The PKGBUILD expects '%1', but its recorded lifecycle content is missing or failed validation. Open Configuration → Scripts and repair or remove the lifecycle script before building.\n\n%2")
                .arg(currentRelease()->lifecycleScript.fileName,
                     currentRelease()->lifecycleScript.validationMessage));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ConfigScripts);
        return;
    }
    QStringList unavailablePackages;
    QStringList uncheckedPackages;
    if (repositoryCatalogLoaded_) {
        for (const auto &dependency : currentRelease()->dependencies) {
            const bool required = !dependency.ignored && !dependency.bundled &&
                                  !dependency.provided &&
                                  dependency.status != MappingStatus::Ignored &&
                                  dependency.status != MappingStatus::Bundled &&
                                  dependency.status != MappingStatus::Provided;
            if (required && !dependency.archPackage.isEmpty()) {
                if (!repositoryDependencyAvailability_.contains(dependency.archPackage)) {
                    uncheckedPackages.append(dependency.archPackage);
                } else if (!repositoryDependencyAvailability_.value(dependency.archPackage)) {
                    unavailablePackages.append(dependency.archPackage);
                }
            }
        }
    }
    uncheckedPackages.removeDuplicates();
    if (!uncheckedPackages.isEmpty()) {
        scheduleRepositoryPackageValidation(uncheckedPackages);
        QMessageBox::information(
            this, QStringLiteral("Checking Arch dependencies"),
            QStringLiteral("PacSmith is still checking these dependency names against the configured pacman repositories:\n\n%1\n\nThe build has not started. Try again after the Dependencies page finishes checking them.")
                .arg(uncheckedPackages.join(QLatin1Char('\n'))));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ConfigDependencies);
        return;
    }
    unavailablePackages.removeDuplicates();
    if (!unavailablePackages.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("Unavailable Arch dependencies"),
            QStringLiteral("PacSmith will not start makepkg because these required package names are absent from every configured pacman repository:\n\n%1\n\nOpen Dependencies and choose an available package from the suggestions, mark the dependency with an evidence-backed treatment, or leave it unresolved for explicit review.")
                .arg(unavailablePackages.join(QLatin1Char('\n'))));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ConfigDependencies);
        return;
    }
    const auto unresolved = std::count_if(currentRelease()->dependencies.cbegin(), currentRelease()->dependencies.cend(),
                                          [](const auto &dependency) {
                                              return dependency.status == MappingStatus::Unresolved;
                                          });
    if (unresolved > 0 && QMessageBox::warning(this, QStringLiteral("Unresolved dependencies"),
                                               QStringLiteral("%1 dependency group(s) are unresolved. Build anyway?").arg(unresolved),
                                               QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    if (!savePkgbuild()) return;
    const auto pkgbuildText = currentPkgbuildText();
    auto lifecycleReference = pkgbuildLifecycleReference(pkgbuildText);
    if (lifecycleReference == QStringLiteral("$_PACSMITH_INSTALL") ||
        lifecycleReference == QStringLiteral("${_PACSMITH_INSTALL}")) {
        lifecycleReference = currentRelease()->lifecycleScript.fileName;
    }
    if (!lifecycleReference.isEmpty() &&
        (lifecycleReference.contains(QLatin1Char('/')) ||
         (!lifecycleReference.contains(QLatin1Char('$')) &&
          currentRelease()->lifecycleScript.fileName != lifecycleReference &&
          currentRelease()->lifecycleScript.contents.isEmpty()))) {
        QMessageBox::warning(
            this, QStringLiteral("PKGBUILD lifecycle file is unavailable"),
            QStringLiteral("The PKGBUILD references install='%1', but PacSmith cannot verify a regular project-local file with that literal name. Repair the lifecycle file on Configuration → Scripts or correct the Custom PKGBUILD before building.")
                .arg(lifecycleReference));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(currentRelease()->lifecycleScript.contents.isEmpty()
                          ? (currentRelease()->pkgbuildManuallyModified
                                 ? EditorSection::ConfigPkgbuild : EditorSection::ResultPkgbuild)
                          : EditorSection::ConfigScripts);
        return;
    }
    if (!currentRelease()->lifecycleScript.contents.isEmpty() &&
        currentRelease()->lifecycleScript.validationPassed &&
        !pkgbuildReferencesLifecycle(pkgbuildText,
                                     currentRelease()->lifecycleScript.fileName)) {
        QMessageBox::warning(
            this, QStringLiteral("PKGBUILD omits lifecycle script"),
            QStringLiteral("The validated lifecycle script '%1' is not referenced by the PKGBUILD. "
                           "Add install='%1' or install=\"${_PACSMITH_INSTALL}\" in Custom mode or switch back to Guided before building.")
                .arg(currentRelease()->lifecycleScript.fileName));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(currentRelease()->pkgbuildManuallyModified
                          ? EditorSection::ConfigPkgbuild : EditorSection::ConfigScripts);
        return;
    }
    currentRelease()->buildStatus = BuildStatus::Building;
    persistCurrent();
    populateOverview();
    if (!installWhenSuccessful) {
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ResultBuild);
    }
    installAfterSuccessfulBuild_ = installWhenSuccessful;
    showCommandProgress(QStringLiteral("Building %1").arg(project_->displayName),
                        QStringLiteral("Running makepkg…"), true);
    QString error;
    const auto job = library_.startBuild(currentRelease()->id, &error);
    if (!job || job->id.isEmpty()) {
        currentRelease()->buildStatus = BuildStatus::Failed;
        finishCommandProgress(false, QStringLiteral("Build could not start: %1").arg(error));
        populateBuild();
        updateDashboardActions();
        updateDeleteButton();
        return;
    }
    buildJobId_ = job->id;
    buildLogAfter_ = 0;
    if (buildPollTimer_ == nullptr) {
        buildPollTimer_ = new QTimer(this);
        connect(buildPollTimer_, &QTimer::timeout, this, &MainWindow::pollBuildJob);
    }
    buildPollTimer_->start(100);
    populateBuild();
    updateDashboardActions();
    updateDeleteButton();
}

bool MainWindow::buildInProgress() const {
    return !buildJobId_.isEmpty();
}

void MainWindow::cancelRemoteBuild() {
    if (buildJobId_.isEmpty()) return;
    QString error;
    static_cast<void>(library_.cancelJob(buildJobId_, &error));
}

void MainWindow::pollBuildJob() {
    if (buildJobId_.isEmpty()) return;
    QString error;
    const auto job = library_.getJob(buildJobId_, &error);
    if (!job) return;
    qint64 next = buildLogAfter_;
    const auto chunk = library_.jobLog(buildJobId_, buildLogAfter_, &next, nullptr);
    if (!chunk.isEmpty() && commandProgress_ != nullptr) {
        commandProgress_->appendOutput(chunk);
    }
    buildLogAfter_ = next;
    if (job->status == QStringLiteral("succeeded") || job->status == QStringLiteral("failed") ||
        job->status == QStringLiteral("interrupted")) {
        if (buildPollTimer_ != nullptr) buildPollTimer_->stop();
        finishBuildJob();
    }
}

void MainWindow::finishBuildJob() {
    const auto jobId = buildJobId_;
    QString error;
    const auto job = library_.getJob(jobId, &error);
    buildJobId_.clear();
    if (!project_) return;
    auto reloaded = library_.load(project_->id, &error);
    if (reloaded) {
        project_ = *reloaded;
        projectCache_.insert(project_->id, *project_);
    }
    const bool succeeded = job && job->status == QStringLiteral("succeeded");
    const bool canceled = job && job->status == QStringLiteral("interrupted");
    populateOverview();
    populateBuild();
    populateHistory();
    updateDeleteButton();
    updateDashboardActions();
    const auto shouldInstall = succeeded && !canceled && installAfterSuccessfulBuild_;
    installAfterSuccessfulBuild_ = false;
    finishCommandProgress(succeeded && !canceled,
                          canceled ? QStringLiteral("Build canceled.")
                          : succeeded ? QStringLiteral("Build succeeded.")
                                      : QStringLiteral("Build failed."));
    statusBar()->showMessage(canceled ? QStringLiteral("Build canceled")
                             : succeeded ? QStringLiteral("Build succeeded")
                                         : QStringLiteral("Build failed"), 8000);
    if (shouldInstall) QTimer::singleShot(0, this, [this] { startInstall(); });
}

void MainWindow::startInstall() {
    if (!project_ || installService_.isRunning()) return;
    bool lifecycleChanged = false;
    QString lifecycleError;
    if (!library_.synchronizeLifecycle(*project_, *currentRelease(), &lifecycleChanged, &lifecycleError)) {
        QMessageBox::critical(this, QStringLiteral("Could not inspect lifecycle script"), lifecycleError);
        return;
    }
    if (lifecycleChanged) {
        populateScripts();
        populateOverview();
        populateBuild();
        QMessageBox::warning(this, QStringLiteral("Installation blocked"),
                             QStringLiteral("The lifecycle file changed after the last build. Re-review it and rebuild the package."));
        return;
    }
    if (!currentRelease()->lifecycleScript.contents.isEmpty() &&
        (!currentRelease()->lifecycleScript.validationPassed || currentRelease()->lifecycleScript.requiresAcknowledgement())) {
        QMessageBox::warning(this, QStringLiteral("Installation blocked"),
                             QStringLiteral("The package contains a generated Arch lifecycle script that pacman will run as root. "
                                            "Open Scripts, review it, and acknowledge the exact content before installation."));
        if (projectSidebar_ != nullptr) projectSidebar_->hide();
        rightStack_->setCurrentIndex(1);
        selectSection(EditorSection::ConfigScripts);
        return;
    }
    const auto package = retainedPackagePath(library_, *currentRelease());
    if (package.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Installation unavailable"),
                             QStringLiteral("No retained Arch package artifact exists for this release. Build it first."));
        return;
    }
    const auto lifecycleNotice = currentRelease()->lifecycleScript.contents.isEmpty()
                                     ? QString{}
                                     : QStringLiteral("\n\nPacman will also run the acknowledged lifecycle functions in %1 as root.")
                                           .arg(currentRelease()->lifecycleScript.fileName);
    if (QMessageBox::question(this, QStringLiteral("Install Arch package"),
                              QStringLiteral("Authorize pacman to install this package? PacSmith will run only pacman -U for the path below and pass --noconfirm after this explicit confirmation. Polkit may request your password.\n\n%1%2")
                                  .arg(package, lifecycleNotice)) !=
        QMessageBox::Yes) return;
    installButton_->setEnabled(false);
    if (projectPrimaryButton_ != nullptr) projectPrimaryButton_->setEnabled(false);
    if (projectActionButton_ != nullptr) projectActionButton_->setEnabled(false);
    projectList_->setEnabled(false);
    pendingPackageOperation_ = QStringLiteral("install");
    showCommandProgress(QStringLiteral("Installing %1").arg(project_->displayName),
                        QStringLiteral("Waiting for polkit authorization…"), false);
    if (commandProgress_ != nullptr) {
        commandProgress_->appendOutput(
            QStringLiteral("Requesting narrowly scoped privilege elevation for non-interactive pacman -U…\n%1\n")
                .arg(package));
    }
    installService_.start(std::filesystem::path(package.toUtf8().constData()), true);
    populateBuild();
    updateDeleteButton();
}

void MainWindow::editSelectedRelease() {
    if (!project_) return;
    const auto id = selectedDashboardReleaseId();
    const auto *release = project_->release(id);
    if (release == nullptr || release->state == ReleaseState::Discovered) return;
    showReleaseWorkbench(id);
}

void MainWindow::prepareSelectedRelease() {
    if (!project_) return;
    const auto id = selectedDashboardReleaseId();
    if (id == preparingReleaseId_) {
        if (downloadProgress_ != nullptr) {
            downloadProgress_->show();
            downloadProgress_->raise();
            downloadProgress_->activateWindow();
        } else if (importProgress_ != nullptr) {
            importProgress_->show();
            importProgress_->raise();
            importProgress_->activateWindow();
        }
        return;
    }
    beginReleasePreparation(id, true);
}

void MainWindow::beginReleasePreparation(const QString &releaseId,
                                         const bool askForConfirmation) {
    if (!project_ || debDownloadService_->isRunning() || importThread_ != nullptr) return;
    const auto *release = project_->release(releaseId);
    if (release == nullptr || release->state != ReleaseState::Discovered) return;
    const auto integrity = release->sourceSha256.isEmpty()
        ? QStringLiteral("No publisher digest was provided; PacSmith will record the locally computed SHA256 and show the source as unsigned.")
        : QStringLiteral("PacSmith will require publisher SHA256 %1.").arg(release->sourceSha256);
    if (askForConfirmation && QMessageBox::question(
            this, QStringLiteral("Prepare vendor release artifact"),
            QStringLiteral("Download release %1 from %2, inspect it as untrusted data, and create its package setup?\n\n%3")
                .arg(release->debian.version, acquisitionKindName(release->acquisition.kind), integrity)) !=
        QMessageBox::Yes) return;

    preparingProjectId_ = project_->id;
    preparingReleaseId_ = release->id;
    preparationPhase_ = QStringLiteral("Downloading");
    preparationBytesReceived_ = 0;
    preparationBytesTotal_ = -1;
    preparationSpinnerFrame_ = 0;
    lastPreparationPublish_.invalidate();
    const auto target = defaultDownloadPath(project_->id, release->id, release->originalSourceFilename);
    pendingImportOptions_ = {};
    pendingImportOptions_.version = release->debian.version;
    pendingImportOptions_.acquisition = release->acquisition;
    pendingImportOptions_.githubAssetRegex = release->update.githubAssetRegex;
    pendingImportOptions_.githubIncludePrereleases = release->update.githubIncludePrereleases;
    pendingImportOptions_.existingProjectId = project_->id;

    downloadProgress_ = new QProgressDialog(
        QStringLiteral("Downloading vendor artifact…\nYou may hide this window; the download will continue."),
        QStringLiteral("Hide"), 0, 0, this);
    downloadProgress_->setWindowTitle(
        QStringLiteral("Downloading %1 %2").arg(project_->displayName, release->debian.version));
    downloadProgress_->setWindowModality(Qt::NonModal);
    downloadProgress_->setMinimumDuration(0);
    downloadProgress_->setAutoClose(false);
    downloadProgress_->setAutoReset(false);
    downloadProgress_->setMinimumWidth(460);
    downloadProgress_->show();
    preparationSpinnerTimer_->start();
    populateOverview();
    publishPreparationActivity();
    updatePreparationIndicators();
    updateUpdateCheckIndicators();
    const auto url = QUrl(release->sourceUrl);
    const auto sha = release->sourceSha256;
    const auto path = std::filesystem::path(target.toUtf8().constData());
    onServiceThread(*debDownloadService_, [this, url, sha, path] {
        debDownloadService_->start(url, sha, path);
    });
}

void MainWindow::deleteSelectedRelease() {
    if (!project_) return;
    const auto id = selectedDashboardReleaseId();
    const auto *release = project_->release(id);
    if (release == nullptr || id == project_->installedReleaseId) return;
    if (QMessageBox::warning(
            this, QStringLiteral("Delete retained release"),
            QStringLiteral("Permanently delete release %1, including its vendor artifact, settings, PKGBUILD, and built artifacts?")
                .arg(release->debian.version),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    QString error;
    if (!library_.deleteRelease(*project_, id, &error)) {
        QMessageBox::critical(this, QStringLiteral("Could not delete release"), error);
        return;
    }
    if (currentReleaseId_ == id) {
        const auto *fallback = project_->activeTrackingRelease();
        if (fallback == nullptr) fallback = project_->newestRelease();
        currentReleaseId_ = fallback == nullptr ? QString{} : fallback->id;
    }
    refreshProjectList(project_->id);
    refreshCurrentProject();
    syncTrayUpdateCensus();
}

void MainWindow::rollbackSelectedRelease() {
    if (!project_ || installService_.isRunning()) return;
    const auto id = selectedDashboardReleaseId();
    const auto *release = project_->release(id);
    if (release == nullptr || id == project_->installedReleaseId) return;
    const auto package = retainedPackagePath(library_, *release);
    if (package.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Rollback unavailable"),
                             QStringLiteral("That release no longer has a retained Arch package artifact."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Roll back package"),
                              QStringLiteral("Authorize pacman to install retained release %1 non-interactively? Polkit may request your password.\n\n%2")
                                  .arg(release->debian.version, package)) != QMessageBox::Yes) return;
    currentReleaseId_ = id;
    pendingPackageOperation_ = QStringLiteral("rollback");
    projectList_->setEnabled(false);
    showCommandProgress(QStringLiteral("Rolling back %1").arg(project_->displayName),
                        QStringLiteral("Waiting for polkit authorization…"), false);
    if (commandProgress_ != nullptr) {
        commandProgress_->appendOutput(
            QStringLiteral("Installing retained package:\n%1\n").arg(package));
    }
    installService_.start(std::filesystem::path(package.toUtf8().constData()), true);
    populateOverview();
}

void MainWindow::installSelectedRelease() {
    if (!project_ || installService_.isRunning()) return;
    const auto id = selectedDashboardReleaseId();
    const auto *release = project_->release(id);
    if (release == nullptr || retainedPackagePath(library_, *release).isEmpty()) return;
    currentReleaseId_ = id;
    startInstall();
}

void MainWindow::startUninstall() {
    if (!project_ || project_->installedVersion.isEmpty() || installService_.isRunning()) return;
    if (QMessageBox::question(this, QStringLiteral("Uninstall package"),
                              QStringLiteral("Authorize pacman to remove %1 non-interactively? The PacSmith project and retained releases will remain. Polkit may request your password.")
                                  .arg(project_->archPackageName)) != QMessageBox::Yes) return;
    pendingPackageOperation_ = QStringLiteral("uninstall");
    if (project_->installedRelease() != nullptr) currentReleaseId_ = project_->installedReleaseId;
    projectList_->setEnabled(false);
    showCommandProgress(QStringLiteral("Uninstalling %1").arg(project_->displayName),
                        QStringLiteral("Waiting for polkit authorization…"), false);
    if (commandProgress_ != nullptr) {
        commandProgress_->appendOutput(
            QStringLiteral("Removing pacman package %1\n").arg(project_->archPackageName));
    }
    installService_.startUninstall(project_->archPackageName, true);
    populateOverview();
}


} // namespace pacsmith::gui
