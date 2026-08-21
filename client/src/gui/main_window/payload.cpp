#include "gui/main_window/common.hpp"

namespace pacsmith::gui {
namespace {

constexpr qint64 kInlinePayloadPreviewLimit = 256 * 1024;

bool payloadPreviewNeedsExtract(const PayloadEntry &entry) {
    return entry.type == QStringLiteral("file") && entry.contentSha256.isEmpty() &&
           entry.textPreview.isEmpty() && entry.size > 0 &&
           entry.size <= kInlinePayloadPreviewLimit;
}

QString optInstallDirectory(const PackageRelease &release) {
    return release.installMapping.optDirectory.isEmpty()
               ? release.archPackageName
               : release.installMapping.optDirectory;
}

const PayloadEntry *payloadByPath(const PackageRelease &release, const QString &path) {
    const auto iterator = std::find_if(release.payload.cbegin(), release.payload.cend(),
                                       [&path](const auto &entry) { return entry.path == path; });
    return iterator == release.payload.cend() ? nullptr : &*iterator;
}

QString binaryAppRunExplanation(const PackageRelease &release) {
    const auto &appRun = release.installMapping.appRun;
    const auto *entry = payloadByPath(release, QStringLiteral("AppRun"));
    auto sha = appRun.originalContentsSha256;
    if (sha.isEmpty() && entry != nullptr) sha = entry->contentSha256;
    auto text = QStringLiteral(
        "This AppRun is a compiled binary or symlink, not a #! script.\n"
        "PacSmith cannot show or edit it as text. It stays in the extracted AppDir as /opt/%1/AppRun; "
        "the PATH command on Commands is a separate host wrapper that execs this file.")
                    .arg(optInstallDirectory(release));
    if (!appRun.reviewReason.isEmpty()) {
        text += QLatin1Char('\n') + appRun.reviewReason;
    }
    if (!sha.isEmpty()) {
        text += QStringLiteral("\n\nSHA256: %1").arg(sha);
    }
    return text;
}

} // namespace

void MainWindow::populatePayload() {
    if (!project_) return;
    const bool appImage = currentRelease()->sourceType == SourcePackageType::AppImage;
    if (appImage) {
        const auto optDirectory = currentRelease()->installMapping.optDirectory.isEmpty()
                                      ? currentRelease()->archPackageName
                                      : currentRelease()->installMapping.optDirectory;
        setSettingsSectionHelp(
            payloadIntroduction_,
            QStringLiteral("Read-only AppDir contents. Every entry stays under /opt."),
            QStringLiteral("Every entry remains inside /opt/%1; PacSmith does not relocate, prune, or expose individual bundle files. Select a file to inspect it.")
                .arg(optDirectory));
    } else {
        setSettingsSectionHelp(
            payloadIntroduction_,
            QStringLiteral("Filesystem from the imported artifact. Most files need no action."),
            QStringLiteral("Keep or exclude records a recipe rule for the resulting package; it does not modify the source archive. For highlighted system files, inspect the explanation and explicitly keep or exclude them. Symbolic links that point outside the package are shown in red and stay excluded until you keep them. Decisions are content-specific and changed files require review again."));
    }
    payloadTree_->headerItem()->setText(3, appImage ? QStringLiteral("Bundle status")
                                                    : QStringLiteral("Review"));
    keepPayloadButton_->setVisible(!appImage);
    excludePayloadButton_->setVisible(!appImage);
    clearPayloadDecisionButton_->setVisible(!appImage);
    const auto selectedPath = payloadTree_->currentItem()
                                  ? payloadTree_->currentItem()->data(0, Qt::UserRole).toString()
                                  : QString{};
    QTreeWidgetItem *firstPendingFile = nullptr;
    {
        QSignalBlocker blocker(payloadTree_);
        payloadTree_->setUpdatesEnabled(false);
        payloadTree_->clear();
    QHash<QString, QTreeWidgetItem *> nodes;
    nodes.insert(QString{}, payloadTree_->invisibleRootItem());
    for (const auto &entry : currentRelease()->payload) {
        const auto parts = entry.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString currentPath;
        QTreeWidgetItem *parent = payloadTree_->invisibleRootItem();
        for (int index = 0; index < parts.size(); ++index) {
            if (!currentPath.isEmpty()) currentPath += QLatin1Char('/');
            currentPath += parts.at(index);
            auto *item = nodes.value(currentPath, nullptr);
            if (item == nullptr) {
                item = new QTreeWidgetItem(parent, QStringList{parts.at(index)});
                item->setToolTip(0, QLatin1Char('/') + currentPath);
                nodes.insert(currentPath, item);
            }
            parent = item;
            if (index == parts.size() - 1) {
                item->setData(0, Qt::UserRole, entry.path);
                item->setText(1, entry.type);
                item->setText(2, entry.type == QStringLiteral("file") ? QString::number(entry.size) : QString{});
                const auto appliedRule = std::find_if(
                    currentRelease()->payloadRules.cbegin(),
                    currentRelease()->payloadRules.cend(),
                    [&entry](const auto &rule) {
                        return payloadRuleCovers(rule, entry.path);
                    });
                if (!appImage &&
                    (entry.requiresReview || appliedRule != currentRelease()->payloadRules.cend())) {
                    const auto review = PayloadReview::state(*currentRelease(), entry);
                    const auto provenancePath = appliedRule != currentRelease()->payloadRules.cend()
                        ? appliedRule->path : entry.path;
                    const auto provenance = currentRelease()->fieldProvenance
                                                .value(QStringLiteral("payload.%1.treatment").arg(provenancePath));
                    const bool aiGenerated = provenance.origin == ValueOrigin::Ai;
                    if (review.needsReview) {
                        const bool currentlyExcluded = review.disposition == PayloadDisposition::ExcludedByDefault ||
                                                       review.disposition == PayloadDisposition::Excluded;
                        const bool unsafeLink = unsafePackageSymlink(entry);
                        item->setText(3, currentlyExcluded
                                                 ? QStringLiteral("Currently excluded — choose keep or exclude")
                                                 : QStringLiteral("Currently kept — choose keep or exclude"));
                        if (unsafeLink) {
                            item->setText(3, currentlyExcluded
                                                 ? QStringLiteral("Unsafe symlink, excluded — choose keep or exclude")
                                                 : QStringLiteral("Unsafe symlink — choose keep or exclude"));
                            item->setData(0, Qt::UserRole + 1, true);
                        }
                        const auto color = unsafeLink ? payloadUnsafeRed : payloadReviewAmber;
                        for (int column = 0; column < 4; ++column) {
                            item->setForeground(column, color);
                        }
                    } else if (review.disposition == PayloadDisposition::Excluded) {
                        item->setText(3, aiGenerated && provenance.userApproved
                                                    ? QStringLiteral("Excluded — AI-proposed, user approved")
                                                : aiGenerated ? QStringLiteral("Excluded — AI-generated")
                                                    : QStringLiteral("Excluded — acknowledged"));
                    } else {
                        item->setText(3, aiGenerated && provenance.userApproved
                                                    ? QStringLiteral("Kept — AI-proposed, user approved")
                                                : aiGenerated ? QStringLiteral("Kept — AI-generated")
                                                    : QStringLiteral("Kept — acknowledged"));
                    }
                    if (aiGenerated && !review.needsReview) {
                        for (int column = 0; column < 4; ++column) {
                            item->setBackground(column, QColor(35, 90, 55));
                            item->setForeground(column, Qt::white);
                        }
                    }
                }
                if (appImage && entry.path == QStringLiteral("AppRun")) {
                    item->setText(3, QStringLiteral("Vendor entry point"));
                }
                if (!entry.symlinkTarget.isEmpty()) item->setText(1, QStringLiteral("symlink → %1").arg(entry.symlinkTarget));
            }
        }
    }
    std::function<std::pair<bool, bool>(QTreeWidgetItem *)> markPendingDescendants =
        [&](QTreeWidgetItem *item) {
            bool pending = item->text(3).contains(QStringLiteral("choose keep"), Qt::CaseInsensitive);
            bool unsafe = item->data(0, Qt::UserRole + 1).toBool();
            for (int child = 0; child < item->childCount(); ++child) {
                const auto childState = markPendingDescendants(item->child(child));
                pending = childState.first || pending;
                unsafe = childState.second || unsafe;
            }
            if (pending && item->text(3).isEmpty()) {
                item->setText(3, unsafe ? QStringLiteral("Contains unsafe symlink(s) needing a decision")
                                        : QStringLiteral("Contains item(s) needing a decision"));
                const auto color = unsafe ? payloadUnsafeRed : payloadReviewAmber;
                for (int column = 0; column < 4; ++column) item->setForeground(column, color);
            }
            return std::pair<bool, bool>{pending, unsafe};
        };
    for (int top = 0; top < payloadTree_->topLevelItemCount(); ++top) {
        static_cast<void>(markPendingDescendants(payloadTree_->topLevelItem(top)));
    }
    payloadTree_->sortItems(0, Qt::AscendingOrder);
    firstPendingFile = nullptr;
    std::function<void(QTreeWidgetItem *)> expandPending = [&](QTreeWidgetItem *item) {
        const auto reviewText = item->text(3);
        const bool pending = reviewText.contains(QStringLiteral("choose keep"), Qt::CaseInsensitive) ||
                             reviewText.contains(QStringLiteral("needing a decision"), Qt::CaseInsensitive);
        for (int child = 0; child < item->childCount(); ++child) expandPending(item->child(child));
        if (!pending) return;
        item->setExpanded(true);
        if (firstPendingFile == nullptr &&
            !item->data(0, Qt::UserRole).toString().isEmpty() &&
            reviewText.contains(QStringLiteral("choose keep"), Qt::CaseInsensitive)) {
            firstPendingFile = item;
        }
    };
    for (int top = 0; top < payloadTree_->topLevelItemCount(); ++top) {
        expandPending(payloadTree_->topLevelItem(top));
    }
        payloadTree_->setUpdatesEnabled(true);
    }
    if (!selectedPath.isEmpty()) {
        QTreeWidgetItemIterator iterator(payloadTree_);
        while (*iterator != nullptr) {
            if ((*iterator)->data(0, Qt::UserRole).toString() == selectedPath) {
                payloadTree_->setCurrentItem(*iterator);
                payloadTree_->scrollToItem(*iterator, QAbstractItemView::PositionAtCenter);
                break;
            }
            ++iterator;
        }
    } else if (firstPendingFile != nullptr) {
        payloadTree_->setCurrentItem(firstPendingFile);
        payloadTree_->scrollToItem(firstPendingFile, QAbstractItemView::PositionAtCenter);
    }
    if (payloadTree_->currentItem() == nullptr) {
        payloadStatus_->setText(appImage
            ? QStringLiteral("Select a bundle file to inspect it.")
            : QStringLiteral("Select a highlighted file to review it."));
        payloadPreview_->clear();
        keepPayloadButton_->setEnabled(false);
        excludePayloadButton_->setEnabled(false);
        clearPayloadDecisionButton_->setEnabled(false);
    }
}

void MainWindow::updateSelectedPayload() {
    if (!project_ || payloadTree_->currentItem() == nullptr) return;
    const auto path = payloadTree_->currentItem()->data(0, Qt::UserRole).toString();
    const auto iterator = std::find_if(currentRelease()->payload.cbegin(), currentRelease()->payload.cend(),
                                       [&path](const auto &entry) { return entry.path == path; });
    if (iterator == currentRelease()->payload.cend()) {
        payloadStatus_->setText(QStringLiteral("This is a directory grouping. Select a file below it for details."));
        payloadPreview_->clear();
        keepPayloadButton_->setEnabled(false);
        excludePayloadButton_->setEnabled(false);
        clearPayloadDecisionButton_->setEnabled(false);
        return;
    }
    const auto &entry = *iterator;
    const bool appImage = currentRelease()->sourceType == SourcePackageType::AppImage;
    const auto review = PayloadReview::state(*currentRelease(), entry);
    const auto appliedRule = std::find_if(
        currentRelease()->payloadRules.cbegin(), currentRelease()->payloadRules.cend(),
        [&entry](const auto &rule) { return payloadRuleCovers(rule, entry.path); });
    const bool hasDecision = appliedRule != currentRelease()->payloadRules.cend();
    QString decision;
    if (appImage) {
        const auto opt = currentRelease()->installMapping.optDirectory.isEmpty()
            ? currentRelease()->archPackageName
            : currentRelease()->installMapping.optDirectory;
        decision = QStringLiteral("Read-only bundle content; installed as /opt/%1/%2.")
                       .arg(opt, entry.path);
    } else if (review.needsReview) decision = QStringLiteral("Choose whether this belongs in the Arch package.");
    else if (review.disposition == PayloadDisposition::Excluded) decision = QStringLiteral("Decision: exclude this file from the generated Arch package.");
    else if (hasDecision) decision = QStringLiteral("Decision: keep this file in the generated Arch package.");
    else decision = QStringLiteral("No decision is required for this item.");
    const auto scope = review.decisionPath != entry.path
                           ? QStringLiteral("<br><b>Scope:</b> this choice applies to /%1 and everything under it.")
                                 .arg(review.decisionPath.toHtmlEscaped())
                           : QString{};
    const auto reviewReason = entry.reviewReason.isEmpty() && hasDecision
        ? QStringLiteral("Explicit content-specific packaging decision")
        : entry.reviewReason;
    payloadStatus_->setText(QStringLiteral("<b>/%1</b><br>%2<br>%3%4")
                                .arg(entry.path.toHtmlEscaped(), reviewReason.toHtmlEscaped(), decision, scope));

    const auto &appRun = currentRelease()->installMapping.appRun;
    if (appImage && path == QStringLiteral("AppRun") && appRun.present && appRun.script &&
        !appRun.contents.isEmpty()) {
        payloadPreview_->setPlainText(appRun.contents);
    } else if (!entry.textPreview.isEmpty()) {
        payloadPreview_->setPlainText(entry.textPreview +
                                      (entry.previewTruncated ? QStringLiteral("\n\n[Preview truncated at 1 MiB]") : QString{}));
    } else if (appImage && path == QStringLiteral("AppRun") && appRun.present && !appRun.script) {
        payloadPreview_->setPlainText(binaryAppRunExplanation(*currentRelease()));
        if (payloadPreviewNeedsExtract(entry) && appRun.originalContentsSha256.isEmpty()) {
            loadSelectedPayloadPreview(path);
        }
    } else if (payloadPreviewNeedsExtract(entry)) {
        payloadPreview_->setPlainText(QStringLiteral("Loading file content from the saved vendor artifact…"));
        loadSelectedPayloadPreview(path);
    } else if (entry.type == QStringLiteral("file") && entry.contentSha256.isEmpty()) {
        payloadPreview_->setPlainText(
            QStringLiteral("Binary or large file; preview is omitted so the window stays responsive.\n"
                           "Keep or exclude it without hashing the archive member."));
    } else if (entry.type == QStringLiteral("file")) {
        payloadPreview_->setPlainText(QStringLiteral("Binary or non-UTF-8 file; text preview is unavailable.\nSHA256: %1")
                                          .arg(entry.contentSha256));
    } else if (!entry.symlinkTarget.isEmpty()) {
        payloadPreview_->setPlainText(QStringLiteral("Symbolic link target: %1").arg(entry.symlinkTarget));
    } else {
        payloadPreview_->clear();
    }

    const bool fingerprintReady = !payloadPreviewNeedsExtract(entry);
    const bool actionable = !appImage && (entry.requiresReview || hasDecision) && fingerprintReady &&
                            !payloadInspectionRunning_;
    keepPayloadButton_->setEnabled(actionable);
    excludePayloadButton_->setEnabled(actionable);
    const auto rule = std::find_if(currentRelease()->payloadRules.cbegin(), currentRelease()->payloadRules.cend(),
                                   [&review](const auto &candidate) {
                                       return candidate.path == review.decisionPath && candidate.userDecision;
                                   });
    clearPayloadDecisionButton_->setEnabled(rule != currentRelease()->payloadRules.cend() && !payloadInspectionRunning_);
}

void MainWindow::setSelectedPayloadDecision(const bool exclude) {
    if (!project_ || payloadTree_->currentItem() == nullptr) return;
    const auto path = payloadTree_->currentItem()->data(0, Qt::UserRole).toString();
    const auto entry = std::find_if(currentRelease()->payload.cbegin(), currentRelease()->payload.cend(),
                                    [&path](const auto &candidate) { return candidate.path == path; });
    if (entry == currentRelease()->payload.cend()) return;
    if (payloadPreviewNeedsExtract(*entry)) return;
    const auto existingRule = std::find_if(
        currentRelease()->payloadRules.cbegin(), currentRelease()->payloadRules.cend(),
        [&entry](const auto &rule) { return payloadRuleCovers(rule, entry->path); });
    if (!entry->requiresReview && existingRule == currentRelease()->payloadRules.cend()) return;
    const auto review = PayloadReview::state(*currentRelease(), *entry);
    const auto decisionPath = review.decisionPath.isEmpty() ? path : review.decisionPath;
    PayloadReview::decide(*currentRelease(), decisionPath, exclude);
    currentRelease()->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("payload-review"),
                              QStringLiteral("%1 /%2 after content-specific review")
                                  .arg(exclude ? QStringLiteral("Excluded") : QStringLiteral("Kept"), decisionPath)});
    refreshGeneratedPkgbuildAfterModelChange();
    populatePayload();
    populateOverview();
    populateBuild();
    populateHistory();
    statusBar()->showMessage(exclude ? QStringLiteral("Payload file excluded from generated package")
                                     : QStringLiteral("Payload file kept and acknowledged"), 7000);
}

void MainWindow::clearSelectedPayloadDecision() {
    if (!project_ || payloadTree_->currentItem() == nullptr) return;
    const auto path = payloadTree_->currentItem()->data(0, Qt::UserRole).toString();
    const auto entry = std::find_if(currentRelease()->payload.cbegin(), currentRelease()->payload.cend(),
                                    [&path](const auto &candidate) { return candidate.path == path; });
    if (entry == currentRelease()->payload.cend()) return;
    const auto review = PayloadReview::state(*currentRelease(), *entry);
    PayloadReview::clearDecision(*currentRelease(), review.decisionPath.isEmpty() ? path : review.decisionPath);
    refreshGeneratedPkgbuildAfterModelChange();
    populatePayload();
    populateOverview();
    populateBuild();
}

void MainWindow::loadSelectedPayloadPreview(const QString &path) {
    if (!project_ || payloadInspectionRunning_) return;
    payloadInspectionRunning_ = true;
    keepPayloadButton_->setEnabled(false);
    excludePayloadButton_->setEnabled(false);
    clearPayloadDecisionButton_->setEnabled(false);
    const auto projectId = project_->id;
    const auto debPath = library_.sourcePath(*currentRelease());
    using InspectionTaskResult = std::pair<std::optional<PayloadInspection>, QString>;
    auto *watcher = new QFutureWatcher<InspectionTaskResult>(this);
    connect(watcher, &QFutureWatcher<InspectionTaskResult>::finished, this,
            [this, watcher, projectId, path] {
                const auto [inspection, error] = watcher->result();
                watcher->deleteLater();
                payloadInspectionRunning_ = false;
                if (!project_ || project_->id != projectId) return;
                auto entry = std::find_if(currentRelease()->payload.begin(), currentRelease()->payload.end(),
                                          [&path](const auto &candidate) { return candidate.path == path; });
                if (!inspection || entry == currentRelease()->payload.end()) {
                    payloadPreview_->setPlainText(QStringLiteral("Could not load this payload file: %1").arg(error));
                    keepPayloadButton_->setEnabled(false);
                    excludePayloadButton_->setEnabled(false);
                    clearPayloadDecisionButton_->setEnabled(false);
                    return;
                }
                entry->contentSha256 = inspection->contentSha256;
                entry->textPreview = inspection->textPreview;
                entry->previewTruncated = inspection->previewTruncated;
                PayloadReview::adoptFilledContentHash(*currentRelease(), path);
                projectCache_.insert(project_->id, *project_);
                updateSelectedPayload();
            });
    watcher->setFuture(QtConcurrent::run([debPath, path]() -> InspectionTaskResult {
        QString error;
        auto result = PayloadInspector::inspectFile(debPath, path, &error);
        return {std::move(result), std::move(error)};
    }));
}

void MainWindow::populateCommands() {
    if (!project_ || commandsTable_ == nullptr) return;
    QSignalBlocker blocker(commandsTable_);
    populating_ = true;
    const auto &launchers = currentRelease()->installMapping.launchers;
    commandsTable_->setRowCount(static_cast<int>(launchers.size()));
    for (int row = 0; row < launchers.size(); ++row) {
        const auto &launcher = launchers.at(row);
        auto *enabled = new QTableWidgetItem;
        enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        enabled->setCheckState(launcher.enabled ? Qt::Checked : Qt::Unchecked);
        commandsTable_->setItem(row, 0, enabled);
        auto *source = new QTableWidgetItem(launcher.sourcePath);
        source->setFlags(source->flags() & ~Qt::ItemIsEditable);
        commandsTable_->setItem(row, 1, source);
        commandsTable_->setItem(row, 2, new QTableWidgetItem(launcher.commandName));
        commandsTable_->setItem(row, 3, new QTableWidgetItem(launcher.destination));
        auto *method = new QTableWidgetItem(
            launcher.kind == LauncherKind::Wrapper ? QStringLiteral("Wrapper")
                                                   : QStringLiteral("Symlink"));
        method->setFlags(method->flags() & ~Qt::ItemIsEditable);
        commandsTable_->setItem(row, 4, method);
        auto *status = new QTableWidgetItem(
            launcher.missing ? QStringLiteral("Source missing — review")
                             : launcher.enabled ? QStringLiteral("Included")
                                                : QStringLiteral("Not exposed"));
        status->setFlags(status->flags() & ~Qt::ItemIsEditable);
        if (launcher.missing) {
            status->setForeground(QColor(Qt::darkYellow));
            source->setForeground(QColor(Qt::darkYellow));
        }
        commandsTable_->setItem(row, 5, status);
    }
    populating_ = false;
}

void MainWindow::populateAppRunEditor() {
    if (appRunEditor_ == nullptr || currentRelease() == nullptr) return;
    const auto &appRun = currentRelease()->installMapping.appRun;
    const auto disableEditor = [this] {
        appRunEditor_->setEnabled(false);
        saveAppRunButton_->setEnabled(false);
        if (keepOriginalAppRunButton_ != nullptr) keepOriginalAppRunButton_->setEnabled(false);
        if (restoreAppRunButton_ != nullptr) restoreAppRunButton_->setVisible(false);
    };
    if (!appRun.present) {
        appRunEditor_->clear();
        disableEditor();
        if (appRunReviewBanner_ != nullptr) appRunReviewBanner_->setVisible(false);
        appRunStatus_->setText(QStringLiteral("Reanalyze to inspect AppRun."));
        updateSectionReviewMarkers();
        return;
    }
    if (!appRun.script) {
        const auto explanation = binaryAppRunExplanation(*currentRelease());
        if (appRunEditor_->toPlainText() != explanation) {
            appRunEditor_->setPlainText(explanation);
            appRunEditor_->document()->setModified(false);
        }
        disableEditor();
        if (appRunReviewBanner_ != nullptr) appRunReviewBanner_->setVisible(false);
        appRunStatus_->setText(QStringLiteral("Binary or symlink AppRun; not editable as text."));
        updateSectionReviewMarkers();
        return;
    }
    appRunEditor_->setEnabled(true);
    saveAppRunButton_->setEnabled(true);
    if (appRunEditor_->toPlainText() != appRun.contents) {
        appRunEditor_->setPlainText(appRun.contents);
        appRunEditor_->document()->setModified(false);
    }
    const bool needsReview = appRun.requiresReview();
    const bool edited = appRun.userModified ||
        appRunEditor_->toPlainText() != appRun.originalContents;
    if (appRunReviewBanner_ != nullptr) {
        appRunReviewBanner_->setVisible(needsReview);
        const auto reason = appRun.reviewReason.isEmpty()
            ? QStringLiteral("Review this AppRun script before packaging.")
            : appRun.reviewReason;
        appRunReviewLabel_->setText(QStringLiteral("⚠ Needs review — %1").arg(reason));
    }
    if (keepOriginalAppRunButton_ != nullptr) {
        keepOriginalAppRunButton_->setEnabled(needsReview && !appRunEditor_->document()->isModified());
    }
    restoreAppRunButton_->setVisible(edited && !appRun.originalContents.isEmpty());
    if (needsReview) {
        appRunStatus_->clear();
    } else if (appRun.userModified) {
        appRunStatus_->setText(QStringLiteral("Edited"));
    } else {
        appRunStatus_->setText(QStringLiteral("Original kept"));
    }
    updateSectionReviewMarkers();
}

void MainWindow::saveAppRun() {
    if (!project_ || currentRelease() == nullptr || appRunEditor_ == nullptr) return;
    auto &appRun = currentRelease()->installMapping.appRun;
    if (!appRun.present || !appRun.script) return;
    const auto contents = appRunEditor_->toPlainText();
    if (!contents.startsWith(QStringLiteral("#!")) || contents.contains(QChar(QChar::Null)) ||
        contents.size() > 256 * 1024) {
        appRunStatus_->setText(QStringLiteral("⚠ AppRun must remain a #! script of at most 256 KiB."));
        return;
    }
    appRun.contents = contents;
    appRun.userModified = contents != appRun.originalContents;
    appRun.acknowledge();
    appRun.provenance.origin = ValueOrigin::User;
    appRun.provenance.userApproved = true;
    appRun.provenance.timestamp = QDateTime::currentDateTimeUtc();
    refreshGeneratedPkgbuildAfterModelChange();
    populateAppRunEditor();
    appRunStatus_->setText(appRun.userModified ? QStringLiteral("✓ Saved")
                                               : QStringLiteral("✓ Original kept"));
}

void MainWindow::keepOriginalAppRun() {
    if (!project_ || currentRelease() == nullptr) return;
    auto &appRun = currentRelease()->installMapping.appRun;
    if (!appRun.present || !appRun.script || !appRun.requiresReview()) return;
    appRun.contents = appRun.originalContents.isEmpty() ? appRun.contents : appRun.originalContents;
    appRun.userModified = false;
    appRun.acknowledge();
    appRun.provenance.origin = ValueOrigin::User;
    appRun.provenance.userApproved = true;
    appRun.provenance.timestamp = QDateTime::currentDateTimeUtc();
    refreshGeneratedPkgbuildAfterModelChange();
    populateAppRunEditor();
    appRunStatus_->setText(QStringLiteral("✓ Original kept"));
}

void MainWindow::restoreOriginalAppRun() {
    if (!project_ || currentRelease() == nullptr) return;
    auto &appRun = currentRelease()->installMapping.appRun;
    if (appRun.originalContents.isEmpty()) return;
    appRun.contents = appRun.originalContents;
    appRun.userModified = false;
    appRun.acknowledgedFingerprint.clear();
    appRun.provenance.origin = ValueOrigin::User;
    appRun.provenance.userApproved = true;
    appRun.provenance.timestamp = QDateTime::currentDateTimeUtc();
    refreshGeneratedPkgbuildAfterModelChange();
    if (appRunEditor_ != nullptr) {
        appRunEditor_->setPlainText(appRun.contents);
        appRunEditor_->document()->setModified(false);
    }
    populateAppRunEditor();
}

void MainWindow::commandEdited(const int row, const int column) {
    if (populating_ || !project_ || row < 0 ||
        row >= currentRelease()->installMapping.launchers.size()) return;
    auto &launcher = currentRelease()->installMapping.launchers[row];
    const auto command = commandsTable_->item(row, 2)->text().trimmed();
    const auto destination = commandsTable_->item(row, 3)->text().trimmed();
    static const QRegularExpression commandPattern(QStringLiteral("^[A-Za-z0-9@._+\\-]+$"));
    static const QRegularExpression destinationPattern(
        QStringLiteral("^/usr/bin/[A-Za-z0-9@._+\\-]+$"));
    if ((column == 2 && !commandPattern.match(command).hasMatch()) ||
        (column == 3 && !destinationPattern.match(destination).hasMatch())) {
        statusBar()->showMessage(
            QStringLiteral("Command names must be simple names and destinations must be below /usr/bin"),
            8000);
        populateCommands();
        return;
    }
    launcher.enabled = commandsTable_->item(row, 0)->checkState() == Qt::Checked;
    launcher.commandName = command;
    launcher.destination = destination.isEmpty()
        ? QStringLiteral("/usr/bin/%1").arg(command) : destination;
    launcher.provenance.origin = ValueOrigin::User;
    launcher.provenance.userApproved = true;
    launcher.provenance.timestamp = QDateTime::currentDateTimeUtc();
    syncInstallMappingFromLaunchers();
    refreshGeneratedPkgbuildAfterModelChange();
    populateCommands();
}

QStringList MainWindow::payloadFileChoices() const {
    QStringList executable;
    QStringList other;
    if (currentRelease() == nullptr) return {};
    for (const auto &entry : currentRelease()->payload) {
        if (entry.type != QStringLiteral("file")) continue;
        if (entry.executable) executable.append(entry.path);
        else other.append(entry.path);
    }
    executable.sort();
    other.sort();
    return executable + other;
}

QString MainWindow::choosePayloadFile(const QString &title, const QString &selected) {
    const auto choices = payloadFileChoices();
    if (choices.isEmpty()) {
        QMessageBox::information(this, title,
                                 QStringLiteral("This artifact has no inspected files to map."));
        return {};
    }
    int current = 0;
    if (!selected.isEmpty()) {
        const auto index = choices.indexOf(selected);
        if (index >= 0) current = static_cast<int>(index);
    }
    bool accepted = false;
    const auto path = QInputDialog::getItem(
        this, title,
        QStringLiteral("Choose a file from the inspected vendor payload:"),
        choices, current, false, &accepted);
    if (!accepted) return {};
    return path;
}

void MainWindow::syncInstallMappingFromLaunchers() {
    if (currentRelease() == nullptr) return;
    auto &mapping = currentRelease()->installMapping;
    mapping.binarySourcePath.clear();
    mapping.binaryDestination.clear();
    mapping.executableLinks.clear();
    for (const auto &launcher : mapping.launchers) {
        if (!launcher.enabled || launcher.missing || launcher.sourcePath.isEmpty() ||
            launcher.commandName.isEmpty()) {
            continue;
        }
        mapping.binarySourcePath = launcher.sourcePath;
        mapping.binaryDestination = launcher.destination;
        mapping.executableLinks.append(launcher.commandName);
        break;
    }
}

void MainWindow::addCommandFromPayload() {
    if (!project_ || currentRelease() == nullptr) return;
    const auto path = choosePayloadFile(QStringLiteral("Add command from payload"));
    if (path.isEmpty()) return;
    const auto already = std::any_of(
        currentRelease()->installMapping.launchers.cbegin(),
        currentRelease()->installMapping.launchers.cend(),
        [&](const auto &launcher) { return launcher.sourcePath == path; });
    if (already) {
        QMessageBox::information(this, QStringLiteral("Command already listed"),
                                 QStringLiteral("That payload file is already listed as a command."));
        return;
    }
    LauncherMapping launcher;
    launcher.enabled = true;
    launcher.sourcePath = path;
    launcher.commandName = QFileInfo(path).fileName().toLower();
    launcher.destination = QStringLiteral("/usr/bin/%1").arg(launcher.commandName);
    launcher.provenance.origin = ValueOrigin::User;
    launcher.provenance.userApproved = true;
    launcher.provenance.timestamp = QDateTime::currentDateTimeUtc();
    launcher.provenance.rationale = QStringLiteral("User selected an inspected payload file");
    currentRelease()->installMapping.launchers.append(std::move(launcher));
    syncInstallMappingFromLaunchers();
    refreshGeneratedPkgbuildAfterModelChange();
    populateCommands();
    statusBar()->showMessage(QStringLiteral("Command added from payload"), 6000);
}

void MainWindow::assignPayloadToSelectedCommand() {
    if (!project_ || currentRelease() == nullptr || commandsTable_ == nullptr) return;
    const auto row = commandsTable_->currentRow();
    if (row < 0 || row >= currentRelease()->installMapping.launchers.size()) {
        QMessageBox::information(this, QStringLiteral("Assign payload"),
                                 QStringLiteral("Select a command row first."));
        return;
    }
    auto &launcher = currentRelease()->installMapping.launchers[row];
    const auto path = choosePayloadFile(QStringLiteral("Assign payload file"), launcher.sourcePath);
    if (path.isEmpty()) return;
    launcher.sourcePath = path;
    launcher.missing = false;
    if (launcher.commandName.isEmpty()) {
        launcher.commandName = QFileInfo(path).fileName().toLower();
        launcher.destination = QStringLiteral("/usr/bin/%1").arg(launcher.commandName);
    }
    launcher.provenance.origin = ValueOrigin::User;
    launcher.provenance.userApproved = true;
    launcher.provenance.timestamp = QDateTime::currentDateTimeUtc();
    syncInstallMappingFromLaunchers();
    refreshGeneratedPkgbuildAfterModelChange();
    populateCommands();
    statusBar()->showMessage(QStringLiteral("Command source assigned from payload"), 6000);
}

void MainWindow::removeSelectedCommand() {
    if (!project_ || currentRelease() == nullptr || commandsTable_ == nullptr) return;
    const auto row = commandsTable_->currentRow();
    if (row < 0 || row >= currentRelease()->installMapping.launchers.size()) {
        QMessageBox::information(this, QStringLiteral("Remove command"),
                                 QStringLiteral("Select a command row first."));
        return;
    }
    currentRelease()->installMapping.launchers.removeAt(row);
    syncInstallMappingFromLaunchers();
    refreshGeneratedPkgbuildAfterModelChange();
    populateCommands();
    statusBar()->showMessage(QStringLiteral("Command removed"), 6000);
}

void MainWindow::populateDesktopEntries() {
    if (!project_ || currentRelease() == nullptr || desktopEntriesList_ == nullptr) return;
    const auto selected = desktopEntriesList_->currentRow();
    {
        // Rebuilding the list must not fire selection handlers against a
        // partially populated model. Explicitly load the selected recipe once
        // the blocker is gone; setCurrentRow() cannot emit while blocked.
        QSignalBlocker blocker(desktopEntriesList_);
        desktopEntriesList_->clear();
        for (const auto &desktop : currentRelease()->installMapping.desktopEntries) {
            auto *item = new QListWidgetItem(
                QStringLiteral("%1%2%3")
                    .arg(desktop.enabled ? QStringLiteral("✓ ") : QStringLiteral("○ "),
                         desktop.id.isEmpty() ? QFileInfo(desktop.destination).completeBaseName()
                                              : desktop.id,
                         desktop.missing ? QStringLiteral("  ⚠ missing source")
                                         : desktop.userModified ? QStringLiteral("  • edited")
                                                                : QString{}),
                desktopEntriesList_);
            if (desktop.missing) item->setForeground(QColor(Qt::darkYellow));
        }
        if (desktopEntriesList_->count() > 0) {
            desktopEntriesList_->setCurrentRow(std::clamp(
                selected, 0, desktopEntriesList_->count() - 1));
        }
    }
    if (desktopEntriesList_->count() > 0) {
        updateSelectedDesktopEntry();
    } else {
        desktopEntryEditor_->clear();
        desktopEntryEditor_->setEnabled(false);
        desktopEntryEnabled_->setEnabled(false);
        desktopEntryDestination_->setEnabled(false);
        saveDesktopEntryButton_->setEnabled(false);
        deleteDesktopEntryButton_->setEnabled(false);
        desktopEntryStatus_->setText(
            QStringLiteral("No desktop entry was detected. Choose New to create one."));
    }
}

void MainWindow::updateSelectedDesktopEntry() {
    const auto row = desktopEntriesList_->currentRow();
    if (!project_ || row < 0 ||
        row >= currentRelease()->installMapping.desktopEntries.size()) {
        desktopEntryEditor_->clear();
        return;
    }
    const auto &desktop = currentRelease()->installMapping.desktopEntries.at(row);
    desktopEntryEditor_->setEnabled(true);
    desktopEntryEnabled_->setEnabled(true);
    desktopEntryDestination_->setEnabled(true);
    saveDesktopEntryButton_->setEnabled(true);
    deleteDesktopEntryButton_->setEnabled(true);
    desktopEntryEnabled_->setChecked(desktop.enabled);
    desktopEntryDestination_->setText(desktop.destination);
    desktopEntryEditor_->setPlainText(desktop.contents);
    desktopEntryEditor_->document()->setModified(false);
    desktopEntryStatus_->setText(
        desktop.missing
            ? QStringLiteral("⚠ The inherited payload path is absent in this release. Select a replacement or convert this to a custom entry before building.")
            : desktop.userModified
                ? QStringLiteral("User-owned content; future releases will preserve it exactly.")
                : QStringLiteral("Detected vendor content; it will refresh only while left untouched."));
}

void MainWindow::saveSelectedDesktopEntry() {
    const auto row = desktopEntriesList_->currentRow();
    if (!project_ || row < 0 ||
        row >= currentRelease()->installMapping.desktopEntries.size()) return;
    const auto contents = desktopEntryEditor_->toPlainText();
    const auto destination = desktopEntryDestination_->text().trimmed();
    QStringList errors;
    if (!contents.contains(QRegularExpression(QStringLiteral(R"(^\s*\[Desktop Entry\]\s*$)"),
                                               QRegularExpression::MultilineOption))) {
        errors.append(QStringLiteral("Missing [Desktop Entry] section"));
    }
    if (!contents.contains(QRegularExpression(QStringLiteral(R"(^Name(?:\[[^\]]+\])?=.+$)"),
                                               QRegularExpression::MultilineOption))) {
        errors.append(QStringLiteral("Missing Name="));
    }
    if (!contents.contains(QRegularExpression(QStringLiteral(R"(^Type=(?:Application|Link|Directory)$)"),
                                               QRegularExpression::MultilineOption))) {
        errors.append(QStringLiteral("Type must be Application, Link, or Directory"));
    }
    const auto exec = QRegularExpression(QStringLiteral(R"(^Exec=(.+)$)"),
                                         QRegularExpression::MultilineOption).match(contents);
    if (!exec.hasMatch() || exec.captured(1).contains(QLatin1Char('\n')) ||
        exec.captured(1).contains(QLatin1Char('`')) ||
        exec.captured(1).contains(QStringLiteral("$(")) ||
        exec.captured(1).contains(QLatin1Char(';'))) {
        errors.append(QStringLiteral("Exec must be present and must not contain shell syntax"));
    }
    static const QRegularExpression destinationPattern(
        QStringLiteral("^/usr/share/applications/[A-Za-z0-9@._+\\-]+\\.desktop$"));
    if (!destinationPattern.match(destination).hasMatch()) {
        errors.append(QStringLiteral("Destination must be a simple .desktop name under /usr/share/applications"));
    }
    if (!errors.isEmpty()) {
        desktopEntryStatus_->setText(QStringLiteral("⚠ %1").arg(errors.join(QStringLiteral("; "))));
        return;
    }
    auto &desktop = currentRelease()->installMapping.desktopEntries[row];
    desktop.enabled = desktopEntryEnabled_->isChecked();
    desktop.destination = destination;
    desktop.contents = contents;
    desktop.userModified = desktop.generated ||
        sha256Hex(contents.toUtf8()) != desktop.originalContentsSha256;
    desktop.missing = false;
    desktop.provenance.origin = ValueOrigin::User;
    desktop.provenance.userApproved = true;
    desktop.provenance.timestamp = QDateTime::currentDateTimeUtc();
    refreshGeneratedPkgbuildAfterModelChange();
    populateDesktopEntries();
    desktopEntriesList_->setCurrentRow(row);
    desktopEntryStatus_->setText(QStringLiteral("✓ Desktop entry validated and saved"));
}

void MainWindow::addDesktopEntry() {
    if (!project_) return;
    DesktopEntryConfiguration desktop;
    desktop.id = currentRelease()->archPackageName;
    auto id = desktop.id;
    int suffix = 2;
    const auto exists = [&](const QString &candidate) {
        return std::any_of(currentRelease()->installMapping.desktopEntries.cbegin(),
                           currentRelease()->installMapping.desktopEntries.cend(),
                           [&](const auto &entry) { return entry.id == candidate; });
    };
    while (exists(id)) id = desktop.id + QStringLiteral("-%1").arg(suffix++);
    desktop.id = id;
    desktop.generated = true;
    desktop.userModified = true;
    desktop.destination = QStringLiteral("/usr/share/applications/%1.desktop").arg(id);
    const auto command = currentRelease()->installMapping.launchers.isEmpty()
        ? currentRelease()->archPackageName
        : currentRelease()->installMapping.launchers.first().commandName;
    desktop.contents = QStringLiteral(
        "[Desktop Entry]\nType=Application\nName=%1\nExec=%2 %%U\nIcon=%3\nTerminal=false\nCategories=Utility;\n")
        .arg(currentRelease()->displayName, command,
             currentRelease()->installMapping.icon.iconName.isEmpty()
                 ? currentRelease()->archPackageName
                 : currentRelease()->installMapping.icon.iconName);
    desktop.originalContentsSha256.clear();
    desktop.provenance.origin = ValueOrigin::User;
    currentRelease()->installMapping.desktopEntries.append(desktop);
    populateDesktopEntries();
    desktopEntriesList_->setCurrentRow(
        static_cast<int>(currentRelease()->installMapping.desktopEntries.size() - 1));
}

void MainWindow::duplicateDesktopEntry() {
    const auto row = desktopEntriesList_->currentRow();
    if (!project_ || row < 0 ||
        row >= currentRelease()->installMapping.desktopEntries.size()) return;
    auto copy = currentRelease()->installMapping.desktopEntries.at(row);
    copy.id += QStringLiteral("-copy");
    copy.sourcePath.clear();
    copy.destination = QStringLiteral("/usr/share/applications/%1.desktop").arg(copy.id);
    copy.generated = true;
    copy.userModified = true;
    copy.originalContentsSha256.clear();
    copy.provenance.origin = ValueOrigin::User;
    currentRelease()->installMapping.desktopEntries.append(copy);
    populateDesktopEntries();
    desktopEntriesList_->setCurrentRow(
        static_cast<int>(currentRelease()->installMapping.desktopEntries.size() - 1));
}

void MainWindow::deleteDesktopEntry() {
    const auto row = desktopEntriesList_->currentRow();
    if (!project_ || row < 0 ||
        row >= currentRelease()->installMapping.desktopEntries.size()) return;
    currentRelease()->installMapping.desktopEntries.removeAt(row);
    refreshGeneratedPkgbuildAfterModelChange();
    populateDesktopEntries();
}

void MainWindow::populateIcon() {
    if (!project_ || iconPreview_ == nullptr) return;
    QSignalBlocker blocker(payloadIconCandidates_);
    payloadIconCandidates_->clear();
    payloadIconCandidates_->addItem(QStringLiteral("Select a payload icon…"), QString{});
    for (const auto &entry : currentRelease()->payload) {
        if (entry.type != QStringLiteral("file") || entry.size <= 0 ||
            entry.size > 4 * 1024 * 1024) continue;
        const auto suffix = QFileInfo(entry.path).suffix().toLower();
        if (suffix == QStringLiteral("png") || suffix == QStringLiteral("svg") ||
            suffix == QStringLiteral("xpm")) {
            payloadIconCandidates_->addItem(entry.path, entry.path);
        }
    }
    const auto &icon = currentRelease()->installMapping.icon;
    if (icon.isConfigured() && icon.sourceKind == IconSourceKind::Payload) {
        const auto selected = payloadIconCandidates_->findData(icon.sourcePath);
        if (selected >= 0) payloadIconCandidates_->setCurrentIndex(selected);
    }
    QPixmap pixmap;
    if (icon.isConfigured() && !icon.projectPath.isEmpty()) {
        const auto path = library_.releasePath(*currentRelease()) /
                          std::filesystem::path(icon.projectPath.toUtf8().constData());
        pixmap.load(QString::fromUtf8(path.string().c_str()));
    }
    if (icon.isConfigured() && pixmap.isNull()) {
        const auto artifact = library_.iconPath(*currentRelease());
        if (!artifact.empty()) {
            pixmap.load(QString::fromUtf8(artifact.string().c_str()));
        }
    }
    if (!pixmap.isNull()) {
        iconPreview_->clear();
        iconPreview_->setPixmap(pixmap.scaled(iconPreview_->size() - QSize(16, 16),
                                               Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation));
    } else {
        iconPreview_->setPixmap({});
        iconPreview_->setText(QStringLiteral("No icon selected"));
    }
    iconStatus_->setText(
        icon.isConfigured()
            ? QStringLiteral("Source: %1\nStored as: %2\nSHA256: %3")
                  .arg(icon.sourcePath.isEmpty() ? icon.sourceUrl : icon.sourcePath,
                       icon.projectPath, icon.sha256)
            : QStringLiteral("No project icon is configured. The package can still be built, but desktop integration may look incomplete."));
    updateSectionReviewMarkers();
}

void MainWindow::selectPayloadIcon() {
    if (!project_ || payloadIconCandidates_->currentIndex() < 0) return;
    const auto path = payloadIconCandidates_->currentData().toString();
    if (path.isEmpty()) return;
    QString error;
    const auto contents = PayloadInspector::readFileBytes(
        library_.sourcePath(*currentRelease()), path, 4 * 1024 * 1024, &error);
    if (!contents) {
        QMessageBox::warning(this, QStringLiteral("Could not read payload icon"), error);
        return;
    }
    applyIconBytes(*contents, QFileInfo(path).suffix().toLower(),
                   IconSourceKind::Payload, path);
}

void MainWindow::importLocalIcon() {
    if (!project_) return;
    const auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose application icon"), {},
        QStringLiteral("Application icons (*.png *.svg *.xpm);;PNG (*.png);;SVG (*.svg);;XPM (*.xpm)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
        QMessageBox::warning(this, QStringLiteral("Could not import icon"),
                             file.isOpen() ? QStringLiteral("The icon exceeds 4 MiB")
                                           : file.errorString());
        return;
    }
    applyIconBytes(file.readAll(), QFileInfo(path).suffix().toLower(),
                   IconSourceKind::LocalFile, path);
}

void MainWindow::fetchRemoteIcon() {
    if (!project_ || iconNetwork_ == nullptr || iconReply_ != nullptr) return;
    const QUrl url(iconUrl_->text().trimmed());
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()) {
        iconStatus_->setText(QStringLiteral("⚠ Enter a valid HTTPS icon URL."));
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    iconStatus_->setText(QStringLiteral("Fetching icon for review…"));
    iconReply_ = iconNetwork_->get(request);
    connect(iconReply_, &QNetworkReply::readyRead, this, [this] {
        if (iconReply_ != nullptr && iconReply_->bytesAvailable() > 4 * 1024 * 1024) {
            iconReply_->abort();
        }
    });
    connect(iconReply_, &QNetworkReply::finished, this, [this, url] {
        auto *reply = iconReply_;
        iconReply_ = nullptr;
        const auto contents = reply->readAll();
        const auto error = reply->error();
        const auto errorText = reply->errorString();
        reply->deleteLater();
        if (error != QNetworkReply::NoError || contents.size() > 4 * 1024 * 1024) {
            iconStatus_->setText(QStringLiteral("⚠ Icon download failed: %1").arg(errorText));
            return;
        }
        auto suffix = QFileInfo(url.path()).suffix().toLower();
        if (suffix.isEmpty() && contents.startsWith("\x89PNG")) suffix = QStringLiteral("png");
        if (suffix.isEmpty() && contents.left(4096).contains("<svg")) suffix = QStringLiteral("svg");
        if (QMessageBox::question(
                this, QStringLiteral("Trust downloaded icon"),
                QStringLiteral("Use %1 bytes downloaded from %2 as this release's pinned icon?\n\nSHA256: %3")
                    .arg(contents.size()).arg(url.toDisplayString(), sha256Hex(contents)),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) == QMessageBox::Yes) {
            applyIconBytes(contents, suffix, IconSourceKind::RemoteUrl, {},
                           url.toString());
        } else {
            iconStatus_->setText(QStringLiteral("Downloaded icon was not accepted."));
        }
    });
}

void MainWindow::applyIconBytes(const QByteArray &contents, const QString &suffixValue,
                                const IconSourceKind sourceKind, const QString &sourcePath,
                                const QString &sourceUrl) {
    if (!project_ || contents.isEmpty() || contents.size() > 4 * 1024 * 1024) return;
    const auto suffix = suffixValue.toLower();
    if (suffix != QStringLiteral("png") && suffix != QStringLiteral("svg") &&
        suffix != QStringLiteral("xpm")) {
        QMessageBox::warning(this, QStringLiteral("Unsupported icon"),
                             QStringLiteral("PacSmith accepts PNG, SVG, and XPM icons."));
        return;
    }
    if (suffix == QStringLiteral("svg")) {
        const auto prefix = contents.left(4096).toLower();
        if (!prefix.contains("<svg") || prefix.contains("<!entity") ||
            prefix.contains("<!doctype")) {
            QMessageBox::warning(this, QStringLiteral("Unsafe SVG"),
                                 QStringLiteral("The SVG header is invalid or declares external entities."));
            return;
        }
    } else {
        QPixmap validation;
        if (!validation.loadFromData(contents) || validation.width() > 4096 ||
            validation.height() > 4096) {
            QMessageBox::warning(this, QStringLiteral("Invalid icon"),
                                 QStringLiteral("The image could not be decoded safely or is larger than 4096×4096."));
            return;
        }
    }
    const auto relative = QStringLiteral("files/integration/icon.%1").arg(suffix);
    const auto absolute = library_.releasePath(*currentRelease()) /
                          std::filesystem::path(relative.toUtf8().constData());
    std::error_code filesystemError;
    std::filesystem::create_directories(absolute.parent_path(), filesystemError);
    if (filesystemError) {
        QMessageBox::critical(this, QStringLiteral("Could not store icon"),
                              QString::fromStdString(filesystemError.message()));
        return;
    }
    QSaveFile file(QString::fromUtf8(absolute.string().c_str()));
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() ||
        !file.commit()) {
        QMessageBox::critical(this, QStringLiteral("Could not store icon"), file.errorString());
        return;
    }
    QString uploadError;
    if (!library_.setReleaseIcon(*currentRelease(),
                                 QString::fromUtf8(absolute.string().c_str()), &uploadError)) {
        QMessageBox::critical(this, QStringLiteral("Could not store icon"), uploadError);
        return;
    }
    auto &icon = currentRelease()->installMapping.icon;
    icon.sourceKind = sourceKind;
    icon.sourcePath = sourcePath;
    icon.sourceUrl = sourceUrl;
    icon.projectPath = relative;
    icon.sha256 = sha256Hex(contents);
    icon.format = suffix;
    icon.iconName = currentRelease()->archPackageName;
    icon.missing = false;
    icon.provenance.origin = sourceKind == IconSourceKind::Payload
        ? ValueOrigin::Deterministic : ValueOrigin::User;
    icon.provenance.userApproved = sourceKind != IconSourceKind::Payload;
    icon.provenance.timestamp = QDateTime::currentDateTimeUtc();
    currentRelease()->iconPath = relative;
    currentRelease()->iconSourcePath = sourcePath.isEmpty() ? sourceUrl : sourcePath;
    currentRelease()->iconSha256 = icon.sha256;
    project_->iconPath = QStringLiteral("releases/%1/%2").arg(currentRelease()->id, relative);
    project_->iconSourcePath = currentRelease()->iconSourcePath;
    project_->iconSha256 = icon.sha256;
    const auto desktopIcon = icon.iconName;
    if (!desktopIcon.isEmpty() && !currentRelease()->installMapping.desktopEntries.isEmpty()) {
        QStringList currentIcons;
        for (const auto &desktop : currentRelease()->installMapping.desktopEntries) {
            if (!desktop.enabled) continue;
            const auto value = desktopEntryField(desktop.contents, QStringLiteral("Icon"));
            if (!value.isEmpty() && !currentIcons.contains(value)) currentIcons.append(value);
        }
        const bool needsUpdate = std::any_of(
            currentRelease()->installMapping.desktopEntries.cbegin(),
            currentRelease()->installMapping.desktopEntries.cend(),
            [&](const auto &desktop) {
                return desktop.enabled &&
                       desktopEntryField(desktop.contents, QStringLiteral("Icon")) != desktopIcon;
            });
        if (needsUpdate) {
            const auto current = currentIcons.isEmpty()
                ? QStringLiteral("no Icon= value")
                : currentIcons.join(QStringLiteral(", "));
            if (QMessageBox::question(
                    this, QStringLiteral("Update desktop entries"),
                    QStringLiteral("The icon is installed into the hicolor theme as %1, so desktop "
                                   "entries should use that name, not a file path.\n\n"
                                   "Set Icon=%1 on enabled desktop entries? They currently use %2.")
                        .arg(desktopIcon, current),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes) {
                applyDesktopIconName(currentRelease()->installMapping.desktopEntries,
                                     desktopIcon);
            }
        }
    }
    refreshGeneratedPkgbuildAfterModelChange();
    populateIcon();
    populateDesktopEntries();
    if (auto *item = projectList_->currentItem()) {
        item->setIcon(QIcon(QString::fromUtf8(absolute.string().c_str())));
    }
}


} // namespace pacsmith::gui
