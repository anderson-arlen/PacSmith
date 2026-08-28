#include "gui/main_window/common.hpp"

#include <optional>

namespace pacsmith::gui {
namespace {

struct RepositoryTaskResult {
    std::optional<ProjectRepository> status;
    QString error;
};

QString repoVersionText(const bool present, const RepoPackageRef &ref) {
    if (!present) return QStringLiteral("—");
    return ref.version.isEmpty() ? QStringLiteral("%1-%2").arg(ref.pkgver, ref.pkgrel)
                                 : ref.version;
}

QString remainingSoakText(const QString &eligibleAt) {
    const auto eligible = QDateTime::fromString(eligibleAt, Qt::ISODate);
    if (!eligible.isValid()) return QStringLiteral("remaining time unavailable");
    qint64 seconds = QDateTime::currentDateTimeUtc().secsTo(eligible.toUTC());
    if (seconds <= 0) return QStringLiteral("eligible now");
    const auto days = seconds / 86400;
    seconds %= 86400;
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    if (days > 0) return QStringLiteral("%1d %2h remaining").arg(days).arg(hours);
    if (hours > 0) return QStringLiteral("%1h %2m remaining").arg(hours).arg(minutes);
    return QStringLiteral("%1m remaining").arg(std::max<qint64>(1, minutes));
}

QString soakDurationText(const qint64 seconds) {
    if (seconds <= 0) return QStringLiteral("Immediate");
    if (seconds % 86400 == 0) {
        return QStringLiteral("%1 days").arg(seconds / 86400);
    }
    if (seconds % 3600 == 0) {
        return QStringLiteral("%1 hours").arg(seconds / 3600);
    }
    return QStringLiteral("%1 seconds").arg(seconds);
}

} // namespace

void MainWindow::applyProjectRepository(const ProjectRepository &status) {
    if (project_) {
        project_->repository = status;
        if (status.revision > 0) project_->revision = status.revision;
        projectCache_.insert(project_->id, *project_);
    }
    if (repoPublishCheck_ == nullptr) return;
    const QSignalBlocker publishBlocker(repoPublishCheck_);
    const QSignalBlocker automaticBlocker(repoAutomaticSoakCheck_);
    const QSignalBlocker soakOverrideBlocker(repoSoakOverrideCheck_);
    const QSignalBlocker soakDaysBlocker(repoSoakDays_);
    const QSignalBlocker overrideBlocker(repoOverrideEdit_);
    populating_ = true;
    repoPublishCheck_->setChecked(status.publish);
    repoStablePolicy_->setVisible(status.stableChannelEnabled);
    repoAutomaticSoakCheck_->setChecked(status.stableChannelEnabled && status.automaticSoak);
    repoSoakOverrideCheck_->setChecked(status.soakSecondsOverride >= 0);
    const auto displayedSoakSeconds = status.soakSecondsOverride >= 0
        ? status.soakSecondsOverride : status.librarySoakSeconds;
    repoSoakDays_->setValue(static_cast<int>((displayedSoakSeconds + 86399) / 86400));
    repoSoakDefaultLabel_->setText(
        QStringLiteral("Library default: %1. Projects inherit later default changes unless an override is enabled here.")
            .arg(soakDurationText(status.librarySoakSeconds)));
    const bool editable = project_.has_value() && !repositoryOperationInFlight_;
    repoPublishCheck_->setEnabled(editable);
    repoAutomaticSoakCheck_->setEnabled(editable && status.publish && status.stableChannelEnabled);
    repoSoakOverrideCheck_->setEnabled(editable && status.publish && status.stableChannelEnabled &&
                                       status.automaticSoak);
    repoSoakDays_->setEnabled(repoSoakOverrideCheck_->isEnabled() &&
                              repoSoakOverrideCheck_->isChecked());
    repoOverrideEdit_->setEnabled(editable);
    repoOriginalName_->setText(status.originalPackageName.isEmpty()
                                   ? QStringLiteral("—")
                                   : status.originalPackageName);
    repoPrefixDefault_->setText(status.prefixDefault.isEmpty()
                                    ? QStringLiteral("—")
                                    : status.prefixDefault);
    repoOverrideEdit_->setText(status.packageNameOverride);
    repoEffectiveName_->setText(status.effectivePackageName.isEmpty()
                                    ? QStringLiteral("—")
                                    : status.effectivePackageName);
    repoPublishedName_->setText(status.publishedPackageName.isEmpty()
                                    ? QStringLiteral("Not published yet")
                                    : status.publishedPackageName);
    repoNameWarning_->setVisible(status.pkgnameChangeWarning || status.reserved);
    if (status.reserved) {
        repoNameWarning_->setText(
            QStringLiteral("%1 is reserved by PacSmith and cannot be used for a user project.")
                .arg(status.effectivePackageName));
    } else if (status.pkgnameChangeWarning) {
        repoNameWarning_->setText(
            QStringLiteral("Changing the published package name is a migration. Machines that already installed %1 will keep that name until they are updated.")
                .arg(status.publishedPackageName));
    }
    repoChannelTable_->setColumnHidden(3, !status.stableChannelEnabled);
    repoChannelTable_->setRowCount(status.stableChannelEnabled ? 2 : 1);
    QString unstablePromotion = !status.publish
        ? QStringLiteral("Publication disabled")
        : status.stableChannelEnabled
            ? status.automaticSoak ? QStringLiteral("Waiting to begin soaking")
                                   : QStringLiteral("Manual promotion")
            : QStringLiteral("Only channel");
    if (status.stableChannelEnabled && status.automaticSoak && status.hasUnstable) {
        for (const auto &soak : status.soaks) {
            if (soak.releaseId != status.unstable.releaseId &&
                soak.artifactId != status.unstable.artifactId) {
                continue;
            }
            unstablePromotion = soak.status == QStringLiteral("soaking")
                ? QStringLiteral("Soaking · %1").arg(remainingSoakText(soak.eligibleAt))
                : soak.status == QStringLiteral("eligible")
                    ? QStringLiteral("Eligible for automatic promotion")
                    : QStringLiteral("Promotion %1").arg(soak.status);
            break;
        }
    }
    const QList<QStringList> rows{
        {QStringLiteral("unstable"), repoVersionText(status.hasUnstable, status.unstable),
         status.hasUnstable ? status.unstable.arch : QStringLiteral("—"), unstablePromotion},
        {QStringLiteral("stable"), repoVersionText(status.hasStable, status.stable),
         status.hasStable ? status.stable.arch : QStringLiteral("—"),
         status.hasStable ? QStringLiteral("Current") : QStringLiteral("Awaiting promotion")},
    };
    for (int row = 0; row < repoChannelTable_->rowCount(); ++row) {
        for (int column = 0; column < rows.at(row).size(); ++column) {
            repoChannelTable_->setItem(row, column,
                                       new QTableWidgetItem(rows.at(row).at(column)));
        }
    }
    repoPromoteButton_->setVisible(status.stableChannelEnabled);
    repoPromoteButton_->setEnabled(status.publish && status.stableChannelEnabled && status.hasUnstable);
    repoSaveButton_->setEnabled(editable);
    repoStatusLabel_->clear();
    populating_ = false;
}

void MainWindow::populateRepository() {
    if (repoPublishCheck_ == nullptr) return;
    if (!project_) {
        applyProjectRepository({});
        if (repoStatusLabel_ != nullptr) {
            repoStatusLabel_->setText(QStringLiteral("Open a project to configure repository publication."));
        }
        if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(false);
        if (repoPromoteButton_ != nullptr) repoPromoteButton_->setEnabled(false);
        return;
    }
    if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(!repositoryOperationInFlight_);
    if (applyingServerRefresh_) {
        applyProjectRepository(project_->repository);
        return;
    }
    const auto generation = ++repositoryLoadGeneration_;
    const auto projectId = project_->id;
    const auto fallback = project_->repository;
    if (repoStatusLabel_ != nullptr) repoStatusLabel_->setText(QStringLiteral("Loading repository status…"));
    repoPublishCheck_->setEnabled(false);
    repoAutomaticSoakCheck_->setEnabled(false);
    repoSoakOverrideCheck_->setEnabled(false);
    repoSoakDays_->setEnabled(false);
    repoOverrideEdit_->setEnabled(false);
    if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(false);
    if (repoPromoteButton_ != nullptr) repoPromoteButton_->setEnabled(false);
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<RepositoryTaskResult>(this);
    connect(watcher, &QFutureWatcher<RepositoryTaskResult>::finished, this,
            [this, watcher, generation, projectId, fallback] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != repositoryLoadGeneration_ || !project_ || project_->id != projectId) return;
        if (!result.status) {
            applyProjectRepository(fallback);
            if (repoStatusLabel_ != nullptr) {
                repoStatusLabel_->setText(result.error.isEmpty()
                                              ? QStringLiteral("Could not load repository status.")
                                              : result.error);
            }
            return;
        }
        applyProjectRepository(*result.status);
    });
    watcher->setFuture(QtConcurrent::run([config, projectId] {
        LibraryClient client(config);
        RepositoryTaskResult result;
        result.status = client.projectRepo(projectId, &result.error);
        return result;
    }));
}

bool MainWindow::saveProjectRepository() {
    if (!project_ || repositoryOperationInFlight_ || !ensureCurrentProjectWritable()) return false;
    const auto published = project_->repository.publishedPackageName;
    const auto effective = repoEffectiveName_ != nullptr ? repoEffectiveName_->text().trimmed()
                                                        : project_->repository.effectivePackageName;
    if (!published.isEmpty() && effective != published) {
        if (QMessageBox::warning(this, QStringLiteral("Change published package name?"),
                                 QStringLiteral("This project is already published as %1. Changing the effective package name is a migration, not a cosmetic rename. Installed machines will keep the old name until they are updated.\n\nContinue?")
                                     .arg(published),
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) != QMessageBox::Yes) {
            return false;
        }
    }
    const auto projectId = project_->id;
    const auto publish = repoPublishCheck_ != nullptr && repoPublishCheck_->isChecked();
    const auto automaticSoak = publish && project_->repository.stableChannelEnabled &&
                               repoAutomaticSoakCheck_ != nullptr &&
                               repoAutomaticSoakCheck_->isChecked();
    qint64 soakSecondsOverride = -1;
    if (repoSoakOverrideCheck_ != nullptr && repoSoakOverrideCheck_->isChecked() &&
        repoSoakDays_ != nullptr) {
        const auto currentOverride = project_->repository.soakSecondsOverride;
        const auto currentDays = currentOverride >= 0 ? (currentOverride + 86399) / 86400 : -1;
        soakSecondsOverride = currentOverride >= 0 && repoSoakDays_->value() == currentDays
            ? currentOverride : static_cast<qint64>(repoSoakDays_->value()) * 86400;
    }
    const auto overrideName = repoOverrideEdit_ != nullptr
        ? repoOverrideEdit_->text().trimmed() : QString{};
    const auto revision = project_->revision;
    repositoryOperationInFlight_ = true;
    repoPublishCheck_->setEnabled(false);
    repoAutomaticSoakCheck_->setEnabled(false);
    repoSoakOverrideCheck_->setEnabled(false);
    repoSoakDays_->setEnabled(false);
    repoOverrideEdit_->setEnabled(false);
    if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(false);
    if (repoPromoteButton_ != nullptr) repoPromoteButton_->setEnabled(false);
    if (repoStatusLabel_ != nullptr) repoStatusLabel_->setText(QStringLiteral("Saving repository settings…"));
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<RepositoryTaskResult>(this);
    connect(watcher, &QFutureWatcher<RepositoryTaskResult>::finished, this,
            [this, watcher, projectId] {
        const auto result = watcher->result();
        watcher->deleteLater();
        repositoryOperationInFlight_ = false;
        if (!project_ || project_->id != projectId) return;
        if (!result.status) {
            if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(true);
            repoPublishCheck_->setEnabled(true);
            repoAutomaticSoakCheck_->setEnabled(repoPublishCheck_->isChecked() &&
                                                project_->repository.stableChannelEnabled);
            repoSoakOverrideCheck_->setEnabled(repoAutomaticSoakCheck_->isEnabled() &&
                                               repoAutomaticSoakCheck_->isChecked());
            repoSoakDays_->setEnabled(repoSoakOverrideCheck_->isEnabled() &&
                                      repoSoakOverrideCheck_->isChecked());
            repoOverrideEdit_->setEnabled(true);
            QMessageBox::critical(this, QStringLiteral("Could not save repository settings"), result.error);
            return;
        }
        applyProjectRepository(*result.status);
    });
    watcher->setFuture(QtConcurrent::run([config, projectId, publish, automaticSoak,
                                          soakSecondsOverride, overrideName, revision] {
        LibraryClient client(config);
        RepositoryTaskResult result;
        result.status = client.saveProjectRepo(projectId, publish, automaticSoak, soakSecondsOverride,
                                               overrideName, revision,
                                               &result.error);
        return result;
    }));
    return true;
}

void MainWindow::promoteProjectRepository() {
    if (!project_ || repositoryOperationInFlight_ || !ensureCurrentProjectWritable()) return;
    if (QMessageBox::question(this, QStringLiteral("Promote to Stable"),
                              QStringLiteral("Promote the newest package that advances stable, bypassing remaining soak time? Stable is never downgraded.")) !=
        QMessageBox::Yes) {
        return;
    }
    const auto projectId = project_->id;
    repositoryOperationInFlight_ = true;
    if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(false);
    if (repoPromoteButton_ != nullptr) repoPromoteButton_->setEnabled(false);
    if (repoStatusLabel_ != nullptr) repoStatusLabel_->setText(QStringLiteral("Promoting package to stable…"));
    const auto config = library_.config();
    auto *watcher = new QFutureWatcher<RepositoryTaskResult>(this);
    connect(watcher, &QFutureWatcher<RepositoryTaskResult>::finished, this,
            [this, watcher, projectId] {
        const auto result = watcher->result();
        watcher->deleteLater();
        repositoryOperationInFlight_ = false;
        if (!project_ || project_->id != projectId) return;
        if (!result.status) {
            if (repoSaveButton_ != nullptr) repoSaveButton_->setEnabled(true);
            QMessageBox::critical(this, QStringLiteral("Could not promote to stable"), result.error);
            return;
        }
        applyProjectRepository(*result.status);
        refreshCurrentProject();
    });
    watcher->setFuture(QtConcurrent::run([config, projectId] {
        LibraryClient client(config);
        RepositoryTaskResult result;
        result.status = client.promoteProjectRepo(projectId, &result.error);
        return result;
    }));
}


} // namespace pacsmith::gui
