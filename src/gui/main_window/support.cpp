#include "gui/main_window/support.hpp"
#include "gui/main_window/help_widgets.hpp"

#include "core/path_safety.hpp"
#include "core/payload_review.hpp"
#include "core/repository_key_download_service.hpp"
#include "core/repository_trust.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QDate>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSizePolicy>
#include <QSysInfo>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

namespace pacsmith::gui {

QString downloadActivityText(const QString &phase, const qint64 received, const qint64 total) {
    if (phase.isEmpty() || phase == QStringLiteral("Downloading")) {
        if (total > 0) {
            return QStringLiteral("Downloading update · %1 / %2 MiB")
                .arg(received / (1024 * 1024))
                .arg(total / (1024 * 1024));
        }
        if (received > 0) {
            return QStringLiteral("Downloading update · %1 MiB").arg(received / (1024 * 1024));
        }
        return QStringLiteral("Downloading update…");
    }
    return phase;
}

QString finishedUpdateCheckStatus(const BackgroundUpdateState &state) {
    const auto &message = state.message;
    if (!message.isEmpty() &&
        !message.startsWith(QStringLiteral("Checking")) &&
        !message.startsWith(QStringLiteral("Downloading"))) {
        return message;
    }
    if (state.availableUpdates > 0) {
        return QStringLiteral("%1 update(s) available").arg(state.availableUpdates);
    }
    return QStringLiteral("Update check finished");
}

QString downloadStatusText(const QString &name, const QString &phase, const qint64 received,
                           const qint64 total) {
    const auto label = name.isEmpty() ? QStringLiteral("update") : name;
    if (phase.isEmpty() || phase == QStringLiteral("Downloading")) {
        if (total > 0) {
            return QStringLiteral("Downloading %1 update… %2 / %3 MiB")
                .arg(label)
                .arg(received / (1024 * 1024))
                .arg(total / (1024 * 1024));
        }
        if (received > 0) {
            return QStringLiteral("Downloading %1 update… %2 MiB")
                .arg(label)
                .arg(received / (1024 * 1024));
        }
        return QStringLiteral("Downloading %1 update…").arg(label);
    }
    return QStringLiteral("Preparing %1: %2").arg(label, phase);
}
bool requiresRepositoryPackage(const DependencyMapping &dependency) {
    return !dependency.ignored && !dependency.bundled && !dependency.provided &&
           dependency.status != MappingStatus::Ignored &&
           dependency.status != MappingStatus::Bundled &&
           dependency.status != MappingStatus::Provided &&
           !dependency.archPackage.isEmpty();
}

bool repositoryPackageUnavailable(const DependencyMapping &dependency,
                                  const QHash<QString, bool> &availability) {
    return requiresRepositoryPackage(dependency) &&
           availability.contains(dependency.archPackage) &&
           !availability.value(dependency.archPackage);
}

bool payloadRuleCovers(const PayloadRule &rule, const QString &path) {
    return path == rule.path || path.startsWith(rule.path + QLatin1Char('/'));
}

QList<AiDependencyCandidate> requiredAiDependencyCandidates(
    const PackageRelease &release, const AiResolution &resolution) {
    QHash<int, QString> packages;
    QHash<int, bool> required;
    QSet<int> aiOwned;
    for (int index = 0; index < release.dependencies.size(); ++index) {
        const auto &dependency = release.dependencies.at(index);
        packages.insert(index, dependency.archPackage);
        required.insert(index, requiresRepositoryPackage(dependency));
        if (dependency.mappingSource.startsWith(QStringLiteral("AI:"),
                                                Qt::CaseInsensitive)) {
            aiOwned.insert(index);
        }
    }
    static const QRegularExpression pattern(
        QStringLiteral(R"(^dependency\.(\d+)\.(archPackage|treatment)$)"));
    for (const auto &change : resolution.changes) {
        const auto match = pattern.match(change.field);
        if (!match.hasMatch()) continue;
        const auto index = match.captured(1).toInt();
        if (index < 0 || index >= release.dependencies.size()) continue;
        aiOwned.insert(index);
        if (match.captured(2) == QStringLiteral("archPackage")) {
            packages.insert(index, change.value.trimmed());
            required.insert(index, true);
            continue;
        }
        auto treatment = change.value.trimmed().toLower();
        if (treatment == QStringLiteral("require")) treatment = QStringLiteral("required");
        required.insert(index, treatment == QStringLiteral("required"));
        if (treatment == QStringLiteral("unresolved")) packages.insert(index, QString{});
    }
    QList<AiDependencyCandidate> result;
    for (const auto index : aiOwned) {
        const auto package = packages.value(index).trimmed();
        if (required.value(index) && !package.isEmpty()) result.append({index, package});
    }
    std::ranges::sort(result, {}, &AiDependencyCandidate::index);
    return result;
}
QString repositoryArchitecture(const bool apt) {
    const auto current = QSysInfo::currentCpuArchitecture().toLower();
    if (current == QStringLiteral("x86_64") || current == QStringLiteral("amd64")) {
        return apt ? QStringLiteral("amd64") : QStringLiteral("x86_64");
    }
    if (current == QStringLiteral("arm64") || current == QStringLiteral("aarch64")) {
        return apt ? QStringLiteral("arm64") : QStringLiteral("aarch64");
    }
    return current;
}

QList<RepositoryKeySeed> knownRepositoryKeys(const QHash<QString, Project> &projects,
                                             const ProjectStore &store) {
    QList<RepositoryKeySeed> result;
    QSet<QString> seenHashes;
    for (const auto &project : projects) {
        for (const auto &release : project.releases) {
            for (const auto &key : release.update.signingKeys) {
                if (!key.trusted || key.relativePath.isEmpty() ||
                    seenHashes.contains(key.sha256)) continue;
                const auto path = store.releasePath(release) /
                                  std::filesystem::path(key.relativePath.toUtf8().constData());
                const auto encoded = path.u8string();
                QFile file(QString::fromUtf8(
                    reinterpret_cast<const char *>(encoded.data()),
                    static_cast<qsizetype>(encoded.size())));
                if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
                    file.size() > 4 * 1024 * 1024) continue;
                const auto contents = file.readAll();
                QString inspectionError;
                const auto inspection = RepositoryTrust::inspectKey(contents, &inspectionError);
                if (!inspection || inspection->sha256 != key.sha256) continue;
                RepositoryKeySeed seed;
                seed.label = QStringLiteral("%1 · %2")
                                 .arg(project.displayName,
                                      key.fingerprints.isEmpty()
                                          ? key.sha256.left(16)
                                          : key.fingerprints.first());
                seed.contents = contents;
                seed.source = QStringLiteral("reused from PacSmith project %1 (%2)")
                                  .arg(project.id, key.sourcePath);
                result.append(std::move(seed));
                seenHashes.insert(key.sha256);
            }
        }
    }
    return result;
}

std::optional<RepositorySourceChoice> chooseRepositorySource(QWidget *parent,
                                                             const bool rpm,
                                                             const QList<RepositoryKeySeed> &knownKeys) {
    QDialog dialog(parent);
    dialog.setWindowTitle(rpm ? QStringLiteral("New Project from RPM Repository")
                              : QStringLiteral("New Project from APT Repository"));
    dialog.resize(760, rpm ? 460 : 540);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(pageIntroduction(
        rpm
            ? QStringLiteral("PacSmith will verify the signed RPM repository and import the newest matching package.")
            : QStringLiteral("PacSmith will verify the signed APT repository and import the newest matching package."),
        &dialog,
        rpm
            ? QStringLiteral("PacSmith will download and let you review the repository signing key, verify signed RPM metadata, select the newest matching package, verify its checksum, and import it as a persistent project.")
            : QStringLiteral("PacSmith will download and let you review the repository signing key, verify signed APT metadata, select the newest matching package, verify its SHA256, and import it as a persistent project.")));
    auto *form = new QFormLayout;
    auto *repository = new QLineEdit(&dialog);
    repository->setPlaceholderText(rpm
        ? QStringLiteral("https://vendor.example/rpm/x86_64")
        : QStringLiteral("https://vendor.example/debian"));
    auto *package = new QLineEdit(&dialog);
    package->setPlaceholderText(QStringLiteral("vendor-package-name"));
    auto *architecture = new QLineEdit(repositoryArchitecture(!rpm), &dialog);
    auto *suite = new QLineEdit(&dialog);
    suite->setPlaceholderText(QStringLiteral("stable, or ./ for a flat repository"));
    auto *component = new QLineEdit(&dialog);
    component->setPlaceholderText(QStringLiteral("main; blank for a flat repository"));
    auto *keyUrl = new QLineEdit(&dialog);
    keyUrl->setPlaceholderText(QStringLiteral("https://vendor.example/repository-signing-key.gpg"));
    auto *keySource = new QComboBox(&dialog);
    keySource->addItems({QStringLiteral("Vendor HTTPS URL"),
                         QStringLiteral("Local file or pasted key"),
                         QStringLiteral("Existing trusted PacSmith key")});
    auto *keyFileRow = new QWidget(&dialog);
    auto *keyFileLayout = new QHBoxLayout(keyFileRow);
    keyFileLayout->setContentsMargins(0, 0, 0, 0);
    auto *keyFile = new QLineEdit(keyFileRow);
    keyFile->setReadOnly(true);
    keyFile->setPlaceholderText(QStringLiteral("Choose a local OpenPGP key or paste its contents"));
    auto *browseKey = new QPushButton(QStringLiteral("Browse…"), keyFileRow);
    auto *pasteKey = new QPushButton(QStringLiteral("Paste…"), keyFileRow);
    keyFileLayout->addWidget(keyFile, 1);
    keyFileLayout->addWidget(browseKey);
    keyFileLayout->addWidget(pasteKey);
    auto *existingKey = new QComboBox(&dialog);
    existingKey->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    for (const auto &known : knownKeys) existingKey->addItem(known.label);
    auto *keyStatus = new QLabel(&dialog);
    keyStatus->setWordWrap(true);
    keyStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QByteArray selectedKeyContents;
    QString selectedKeySource;
    const auto inspectSelectedKey = [keyStatus, &selectedKeyContents](const QString &source) {
        QString error;
        const auto inspection = RepositoryTrust::inspectKey(selectedKeyContents, &error);
        if (!inspection) {
            keyStatus->setText(QStringLiteral("⚠ %1").arg(error));
            return false;
        }
        keyStatus->setText(QStringLiteral("Selected %1 · fingerprint %2 · SHA256 %3")
                               .arg(source, inspection->fingerprints.join(QStringLiteral(", ")),
                                    inspection->sha256));
        return true;
    };
    QObject::connect(browseKey, &QPushButton::clicked, &dialog,
                     [&dialog, keyFile, &selectedKeyContents, &selectedKeySource,
                      inspectSelectedKey] {
        const auto path = QFileDialog::getOpenFileName(
            &dialog, QStringLiteral("Select OpenPGP repository signing key"), {},
            QStringLiteral("OpenPGP keys (*.gpg *.pgp *.asc *.key);;All files (*)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 4 * 1024 * 1024) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not read signing key"),
                                  QStringLiteral("The selected key must be a readable file no larger than 4 MiB."));
            return;
        }
        selectedKeyContents = file.readAll();
        selectedKeySource = path;
        if (inspectSelectedKey(QFileInfo(path).fileName())) keyFile->setText(path);
    });
    QObject::connect(pasteKey, &QPushButton::clicked, &dialog,
                     [&dialog, keyFile, &selectedKeyContents, &selectedKeySource,
                      inspectSelectedKey] {
        bool accepted = false;
        const auto text = QInputDialog::getMultiLineText(
            &dialog, QStringLiteral("Paste OpenPGP public key"),
            QStringLiteral("Paste an armored key or Base64-encoded binary key:"), {}, &accepted);
        if (!accepted || text.trimmed().isEmpty()) return;
        selectedKeyContents = text.trimmed().toUtf8();
        if (!selectedKeyContents.startsWith("-----BEGIN PGP")) {
            const auto decoded = QByteArray::fromBase64(
                selectedKeyContents, QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded.isEmpty()) selectedKeyContents = decoded;
        }
        selectedKeySource = QStringLiteral("user-pasted key");
        if (inspectSelectedKey(QStringLiteral("pasted key"))) {
            keyFile->setText(QStringLiteral("Pasted OpenPGP key"));
        }
    });
    form->addRow(QStringLiteral("Repository URL"), repository);
    if (!rpm) {
        form->addRow(QStringLiteral("Suite"), suite);
        form->addRow(QStringLiteral("Component"), component);
    }
    form->addRow(QStringLiteral("Architecture"), architecture);
    form->addRow(QStringLiteral("Package name"), package);
    form->addRow(QStringLiteral("Signing key source"), keySource);
    form->addRow(QStringLiteral("Signing key URL"), keyUrl);
    form->addRow(QStringLiteral("Local / pasted key"), keyFileRow);
    form->addRow(QStringLiteral("Existing trusted key"), existingKey);
    form->addRow(QStringLiteral("Selected key"), keyStatus);
    layout->addLayout(form);
    auto *notice = new QLabel(
        QStringLiteral("PacSmith will show the selected key's fingerprint and require explicit trust before querying the repository. Reusing a key copies it into the new release; projects never depend on another project's files."),
        &dialog);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok,
                                         &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Fetch Key and Inspect"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    const auto updateKeySourceUi = [keySource, keyUrl, keyFileRow, existingKey,
                                    keyStatus, &knownKeys](const int index) {
        keyUrl->setEnabled(index == 0);
        keyFileRow->setEnabled(index == 1);
        existingKey->setEnabled(index == 2 && !knownKeys.isEmpty());
        if (index == 2 && knownKeys.isEmpty()) {
            keyStatus->setText(QStringLiteral("No trusted signing keys exist in current PacSmith projects."));
        }
    };
    QObject::connect(keySource, &QComboBox::currentIndexChanged, &dialog, updateKeySourceUi);
    QObject::connect(existingKey, &QComboBox::currentIndexChanged, &dialog,
                     [keyStatus, &knownKeys](const int index) {
        if (index < 0 || index >= knownKeys.size()) return;
        QString error;
        const auto inspection = RepositoryTrust::inspectKey(knownKeys.at(index).contents, &error);
        keyStatus->setText(inspection
            ? QStringLiteral("Selected existing key · fingerprint %1 · SHA256 %2")
                  .arg(inspection->fingerprints.join(QStringLiteral(", ")), inspection->sha256)
            : QStringLiteral("⚠ Stored key could not be inspected: %1").arg(error));
    });
    updateKeySourceUi(0);

    static const QRegularExpression packageName(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9+._-]*$"));
    static const QRegularExpression repositoryPart(
        QStringLiteral("^[A-Za-z0-9._+/-]+$"));
    while (dialog.exec() == QDialog::Accepted) {
        auto repositoryText = repository->text().trimmed();
        if (!repositoryText.contains(QStringLiteral("://"))) {
            repositoryText.prepend(QStringLiteral("https://"));
        }
        const QUrl repositoryUrl(repositoryText, QUrl::StrictMode);
        const QUrl signingUrl(keyUrl->text().trimmed(), QUrl::StrictMode);
        const auto packageText = package->text().trimmed();
        const auto architectureText = architecture->text().trimmed();
        const auto suiteText = suite->text().trimmed();
        const auto componentText = component->text().trimmed();
        QString validationError;
        if (!repositoryUrl.isValid() || repositoryUrl.scheme() != QStringLiteral("https") ||
            repositoryUrl.host().isEmpty() || !repositoryUrl.userInfo().isEmpty() ||
            repositoryUrl.hasQuery() || repositoryUrl.hasFragment()) {
            validationError = QStringLiteral("Enter an HTTPS repository base URL without credentials, a query, or a fragment.");
        } else if (!packageName.match(packageText).hasMatch()) {
            validationError = QStringLiteral("Enter the exact repository package name.");
        } else if (architectureText.isEmpty() || architectureText.contains(QStringLiteral("..")) ||
                   !repositoryPart.match(architectureText).hasMatch()) {
            validationError = QStringLiteral("Enter a safe repository architecture name.");
        } else if (keySource->currentIndex() == 0 &&
                   !isAcceptableRepositoryKeyUrl(signingUrl)) {
            validationError = QStringLiteral("Enter the vendor's HTTPS OpenPGP signing-key URL.");
        } else if (keySource->currentIndex() == 1 && selectedKeyContents.isEmpty()) {
            validationError = QStringLiteral("Choose a local key file or paste a public key.");
        } else if (keySource->currentIndex() == 2 &&
                   (existingKey->currentIndex() < 0 || knownKeys.isEmpty())) {
            validationError = QStringLiteral("Select an existing trusted PacSmith key.");
        } else if (!rpm &&
                   (suiteText.isEmpty() || suiteText.startsWith(QLatin1Char('/')) ||
                    suiteText.contains(QStringLiteral("..")) ||
                    !repositoryPart.match(suiteText).hasMatch())) {
            validationError = QStringLiteral("Enter an APT suite such as stable, or ./ for a flat repository.");
        } else if (!rpm && !suiteText.endsWith(QLatin1Char('/')) &&
                   (componentText.isEmpty() || componentText.contains(QStringLiteral("..")) ||
                    !repositoryPart.match(componentText).hasMatch())) {
            validationError = QStringLiteral("A non-flat APT repository requires a component such as main.");
        }
        if (!validationError.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Incomplete repository source"),
                                 validationError);
            continue;
        }
        RepositorySourceChoice result;
        result.update.strategy = rpm ? UpdateStrategy::RpmRepository
                                     : UpdateStrategy::AptRepository;
        result.update.url = repositoryUrl.toString();
        if (rpm) {
            result.update.rpmArchitecture = architectureText;
            result.update.rpmPackageName = packageText;
        } else {
            result.update.aptSuite = suiteText;
            result.update.aptComponent = componentText;
            result.update.aptArchitecture = architectureText;
            result.update.aptPackageName = packageText;
        }
        if (keySource->currentIndex() == 0) {
            result.signingKeyUrl = signingUrl;
        } else if (keySource->currentIndex() == 1) {
            result.signingKeyContents = selectedKeyContents;
            result.signingKeySource = selectedKeySource;
        } else {
            const auto &known = knownKeys.at(existingKey->currentIndex());
            result.signingKeyContents = known.contents;
            result.signingKeySource = known.source;
        }
        return result;
    }
    return std::nullopt;
}
QString suggestedAssetRegex(const QString &asset) {
    static const QRegularExpression version(
        QStringLiteral(
            R"((?<![A-Za-z0-9])v?[0-9]+(?:\.[0-9]+)+(?:[-_](?:alpha|beta|rc|pre|preview|dev|nightly)[0-9A-Za-z.]*)?)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = version.match(asset);
    if (!match.hasMatch()) return QRegularExpression::escape(asset);
    return QRegularExpression::escape(asset.left(match.capturedStart())) +
           QStringLiteral("v?[0-9][A-Za-z0-9._+-]*") +
           QRegularExpression::escape(asset.mid(match.capturedEnd()));
}

bool isGitHubSidecarAsset(const QString &asset) {
    const auto lower = asset.toLower();
    return lower.endsWith(QStringLiteral(".sig")) ||
           lower.endsWith(QStringLiteral(".asc")) ||
           lower.endsWith(QStringLiteral(".sha256")) ||
           lower.endsWith(QStringLiteral(".sha512")) ||
           lower.contains(QStringLiteral("checksums")) ||
           lower.contains(QStringLiteral("sha256sums")) ||
           lower == QStringLiteral("manifest.json");
}

int githubArtifactPreference(const QString &asset) {
    const auto lower = asset.toLower();
    if (isGitHubSidecarAsset(asset)) return 100;
    if (lower.contains(QStringLiteral(".pkg.tar."))) return 0;
    if (lower.endsWith(QStringLiteral(".deb"))) return 1;
    if (lower.endsWith(QStringLiteral(".rpm"))) return 2;
    if (lower.endsWith(QStringLiteral(".appimage"))) return 3;
    if (lower.endsWith(QStringLiteral(".tar.gz")) || lower.endsWith(QStringLiteral(".tgz")) ||
        lower.endsWith(QStringLiteral(".tar.xz")) || lower.endsWith(QStringLiteral(".tar.zst")) ||
        lower.endsWith(QStringLiteral(".zip")) || lower.endsWith(QStringLiteral(".7z"))) return 4;
    return 5;
}

std::optional<GitHubRuleChoice> chooseGitHubAssetRule(
    QWidget *parent, const QStringList &assets, const bool includePrereleases,
    const GitHubAiAssist &aiAssist) {
    if (assets.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("No release assets"),
                             QStringLiteral("The selected GitHub release has no downloadable assets."));
        return std::nullopt;
    }
    QStringList installableAssets;
    for (const auto &asset : assets) {
        if (!isGitHubSidecarAsset(asset)) installableAssets.append(asset);
    }
    if (installableAssets.isEmpty()) {
        QMessageBox::warning(
            parent, QStringLiteral("No installable release assets"),
            QStringLiteral("This release contains only signatures, checksums, or manifest files. PacSmith could not find a package artifact to import."));
        return std::nullopt;
    }
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Select GitHub Release Asset"));
    dialog.resize(720, 520);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Choose the prebuilt artifact family PacSmith should track in later releases."),
        &dialog,
        QStringLiteral("The regular expression is saved as this release's GitHub update rule; it must match exactly one asset in each release.")));
    auto *expressionRow = new QWidget(&dialog);
    auto *expressionLayout = new QHBoxLayout(expressionRow);
    expressionLayout->setContentsMargins(0, 0, 0, 0);
    auto *expression = new QLineEdit(&dialog);
    expression->setPlaceholderText(QStringLiteral("Example: chamber-.*-linux-amd64(?:\\.tar\\.gz)?"));
    auto *aiButton = new QPushButton(QStringLiteral("Generate with AI…"), expressionRow);
    aiButton->setEnabled(static_cast<bool>(aiAssist));
    aiButton->setToolTip(QStringLiteral("Ask the configured AI provider to choose an appropriate artifact family and generate a rule that tracks later versions"));
    expressionLayout->addWidget(expression, 1);
    expressionLayout->addWidget(aiButton);
    layout->addWidget(expressionRow);
    auto *prerelease = new QCheckBox(
        QStringLiteral("Track prereleases even after a stable release exists"), &dialog);
    prerelease->setChecked(includePrereleases);
    prerelease->setToolTip(QStringLiteral(
        "When disabled, PacSmith still uses prereleases automatically if no matching stable release exists, then switches to stable as soon as one is published."));
    layout->addWidget(prerelease);
    auto *status = new QLabel(&dialog);
    layout->addWidget(status);
    auto *list = new QListWidget(&dialog);
    for (const auto &asset : assets) {
        auto *item = new QListWidgetItem(asset, list);
        if (isGitHubSidecarAsset(asset)) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled & ~Qt::ItemIsSelectable);
            item->setForeground(dialog.palette().placeholderText());
            item->setToolTip(QStringLiteral("Signature, checksum, and manifest sidecars are verification data, not installable package sources."));
        }
    }
    layout->addWidget(list, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    auto *ok = buttons->button(QDialogButtonBox::Ok);
    const auto update = [=] {
        const QRegularExpression regex(expression->text());
        int matches = 0;
        for (int row = 0; row < list->count(); ++row) {
            auto *item = list->item(row);
            const auto match = regex.match(item->text());
            const bool selected = !isGitHubSidecarAsset(item->text()) &&
                                  regex.isValid() && match.hasMatch() &&
                                  match.capturedLength() == item->text().size();
            item->setBackground(selected ? QColor(30, 105, 55) : QColor{});
            if (selected) ++matches;
        }
        status->setText(!regex.isValid()
            ? QStringLiteral("Invalid expression: %1").arg(regex.errorString())
            : QStringLiteral("%1 matching asset(s); exactly one is required.").arg(matches));
        ok->setEnabled(regex.isValid() && !expression->text().isEmpty() && matches == 1);
    };
    QObject::connect(expression, &QLineEdit::textChanged, &dialog, update);
    QObject::connect(list, &QListWidget::itemClicked, &dialog,
                     [=](QListWidgetItem *item) {
        if (isGitHubSidecarAsset(item->text())) return;
        expression->setText(suggestedAssetRegex(item->text()));
    });
    QObject::connect(aiButton, &QPushButton::clicked, &dialog,
                     [list, expression, status, aiButton, &dialog, aiAssist,
                      installableAssets] {
        QString preferred;
        if (const auto *item = list->currentItem(); item != nullptr &&
            !isGitHubSidecarAsset(item->text())) {
            preferred = item->text();
        }
        aiAssist(installableAssets, preferred, expression, status, aiButton, &dialog);
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto architecture = QSysInfo::currentCpuArchitecture();
    QString preferredAsset;
    int preferredRank = 100;
    for (const auto &asset : assets) {
        const auto lower = asset.toLower();
        const bool matchingArchitecture = lower.contains(architecture.toLower()) ||
            (architecture == QStringLiteral("x86_64") &&
             (lower.contains(QStringLiteral("amd64")) || lower.contains(QStringLiteral("x64"))));
        const auto rank = githubArtifactPreference(asset);
        if (matchingArchitecture && rank < preferredRank) {
            preferredAsset = asset;
            preferredRank = rank;
        }
    }
    if (!preferredAsset.isEmpty()) {
        expression->setText(suggestedAssetRegex(preferredAsset));
        for (int row = 0; row < list->count(); ++row) {
            if (list->item(row)->text() == preferredAsset) {
                list->setCurrentRow(row);
                break;
            }
        }
    }
    if (expression->text().isEmpty()) {
        expression->setText(suggestedAssetRegex(installableAssets.first()));
    }
    update();
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return GitHubRuleChoice{expression->text(), prerelease->isChecked()};
}
QString formatLocalDateTime(const QDateTime &value) {
    if (!value.isValid()) return QStringLiteral("never");
    const auto local = value.toLocalTime();
    const auto locale = QLocale::system();
    const auto time = locale.toString(local.time(), QLocale::ShortFormat);
    if (local.date() == QDate::currentDate()) return QStringLiteral("today at %1").arg(time);
    if (local.date() == QDate::currentDate().addDays(-1)) {
        return QStringLiteral("yesterday at %1").arg(time);
    }
    return QStringLiteral("%1 at %2").arg(locale.toString(local.date(), QLocale::ShortFormat), time);
}
QWidget *emptyPageHost(QWidget *parent) {
    auto *host = new QWidget(parent);
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    return host;
}

QString sourcePackageTypeTitle(const SourcePackageType type) {
    switch (type) {
    case SourcePackageType::Debian: return QStringLiteral("Deb Package");
    case SourcePackageType::Rpm: return QStringLiteral("Rpm Package");
    case SourcePackageType::ArchPackage: return QStringLiteral("Arch Package");
    case SourcePackageType::Archive: return QStringLiteral("Archive");
    case SourcePackageType::AppImage: return QStringLiteral("AppImage");
    case SourcePackageType::ElfBinary: return QStringLiteral("Executable");
    case SourcePackageType::Unknown: return QStringLiteral("Not yet inspected");
    }
    return QStringLiteral("Not yet inspected");
}
QString acquisitionKindTitle(const AcquisitionKind kind) {
    switch (kind) {
    case AcquisitionKind::LocalFile: return QStringLiteral("Local file");
    case AcquisitionKind::DirectUrl: return QStringLiteral("Direct HTTPS download");
    case AcquisitionKind::AptRepository: return QStringLiteral("Signed APT repository");
    case AcquisitionKind::RpmRepository: return QStringLiteral("Signed RPM repository");
    case AcquisitionKind::GitHubRelease: return QStringLiteral("GitHub release");
    }
    return QStringLiteral("Local file");
}

QString reasoningEffortLabel(const AiReasoningEffort effort) {
    switch (effort) {
    case AiReasoningEffort::ProviderDefault: return QStringLiteral("Provider default");
    case AiReasoningEffort::None: return QStringLiteral("None");
    case AiReasoningEffort::Low: return QStringLiteral("Low");
    case AiReasoningEffort::Medium: return QStringLiteral("Medium");
    case AiReasoningEffort::High: return QStringLiteral("High");
    case AiReasoningEffort::XHigh: return QStringLiteral("Extra high");
    case AiReasoningEffort::Max: return QStringLiteral("Maximum");
    }
    return QStringLiteral("Provider default");
}

QList<AiReasoningEffort> supportedReasoningEfforts(const AiProviderKind provider,
                                                   const QString &model) {
    const auto id = model.trimmed().toLower();
    if (id.isEmpty()) return {};
    if (provider == AiProviderKind::Xai) {
        if (id.startsWith(QStringLiteral("grok-4.6")) ||
            id.startsWith(QStringLiteral("grok-4.20"))) {
            return {AiReasoningEffort::Low, AiReasoningEffort::Medium,
                    AiReasoningEffort::High, AiReasoningEffort::XHigh};
        }
        if (id.startsWith(QStringLiteral("grok-4.5"))) {
            return {AiReasoningEffort::Low, AiReasoningEffort::Medium,
                    AiReasoningEffort::High};
        }
        return {};
    }
    if (provider == AiProviderKind::OpenAi || provider == AiProviderKind::ChatGpt) {
        if (id.startsWith(QStringLiteral("gpt-5.6"))) {
            return {AiReasoningEffort::None, AiReasoningEffort::Low,
                    AiReasoningEffort::Medium, AiReasoningEffort::High,
                    AiReasoningEffort::XHigh, AiReasoningEffort::Max};
        }
        if (id.startsWith(QStringLiteral("gpt-5"))) {
            return {AiReasoningEffort::Low, AiReasoningEffort::Medium,
                    AiReasoningEffort::High, AiReasoningEffort::XHigh};
        }
        if (id.startsWith(QChar('o'))) {
            return {AiReasoningEffort::Low, AiReasoningEffort::Medium,
                    AiReasoningEffort::High};
        }
    }
    return {};
}

void makeReadOnlyCodeEditor(QPlainTextEdit *editor) {
    editor->setReadOnly(true);
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
}

void configureIdentityVariablesEditor(QPlainTextEdit *editor) {
    makeReadOnlyCodeEditor(editor);
    const auto lineHeight = QFontMetrics(editor->font()).lineSpacing();
    editor->setMinimumHeight(lineHeight * 6 + 8);
    editor->setMaximumHeight(lineHeight * 12 + 8);
    editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
}

QTreeWidgetItem *ensureInstallPlanNode(QTreeWidget *tree, QHash<QString, QTreeWidgetItem *> *nodes,
                                       const QString &absolutePath) {
    QString path = absolutePath;
    while (path.startsWith(QLatin1Char('/'))) path.remove(0, 1);
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (path.isEmpty()) return tree->invisibleRootItem();
    const auto parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString current;
    QTreeWidgetItem *parent = tree->invisibleRootItem();
    for (const auto &part : parts) {
        current += QLatin1Char('/') + part;
        auto *item = nodes->value(current, nullptr);
        if (item == nullptr) {
            const auto label = current.count(QLatin1Char('/')) == 1 ? current : part;
            item = new QTreeWidgetItem(parent, QStringList{label});
            item->setToolTip(0, current);
            item->setData(0, Qt::UserRole, current);
            nodes->insert(current, item);
        }
        parent = item;
    }
    return parent;
}

void addInstallPlanEntry(QTreeWidget *tree, QHash<QString, QTreeWidgetItem *> *nodes,
                         const QString &path, const QString &source, const QString &purpose,
                         const QColor &foreground) {
    auto *item = ensureInstallPlanNode(tree, nodes, path);
    if (item == nullptr || item == tree->invisibleRootItem()) return;
    item->setText(1, source);
    item->setText(2, purpose);
    item->setToolTip(1, source);
    item->setToolTip(2, purpose);
    if (foreground.isValid()) {
        for (int column = 0; column < 3; ++column) item->setForeground(column, foreground);
    }
}

int decorateInstallPlanTree(QTreeWidgetItem *item) {
    if (item->childCount() == 0) return item->text(1).isEmpty() ? 0 : 1;
    int files = 0;
    for (int row = 0; row < item->childCount(); ++row) {
        files += decorateInstallPlanTree(item->child(row));
    }
    if (item->text(2).isEmpty()) {
        item->setText(2, files == 1 ? QStringLiteral("1 file")
                                    : QStringLiteral("%1 files").arg(files));
    }
    item->sortChildren(0, Qt::AscendingOrder);
    return files;
}

void showDetailedMessageDialog(QWidget *parent, const QString &title, const QString &message,
                               const QString &diagnosticDetails,
                               const QStyle::StandardPixmap iconType,
                               const bool showDetailsInitially) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setSizeGripEnabled(true);
    dialog.setMinimumSize(520, 220);

    auto *layout = new QVBoxLayout(&dialog);
    auto *summaryLayout = new QHBoxLayout;
    auto *icon = new QLabel(&dialog);
    icon->setPixmap(dialog.style()->standardIcon(iconType).pixmap(48, 48));
    icon->setAlignment(Qt::AlignTop);
    summaryLayout->addWidget(icon);

    auto *summary = new QLabel(message, &dialog);
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summaryLayout->addWidget(summary, 1);
    layout->addLayout(summaryLayout);

    const auto hasDetails = !diagnosticDetails.isEmpty();
    auto *details = new QPlainTextEdit(&dialog);
    makeReadOnlyCodeEditor(details);
    details->setPlainText(diagnosticDetails);
    details->setVisible(hasDetails && showDetailsInitially);
    layout->addWidget(details, 1);

    auto *buttonLayout = new QHBoxLayout;
    auto *detailsButton = new QPushButton(showDetailsInitially ? QStringLiteral("Hide Details")
                                                               : QStringLiteral("Show Details"),
                                              &dialog);
    auto *copyButton = new QPushButton(QStringLiteral("Copy Details"), &dialog);
    detailsButton->setEnabled(hasDetails);
    copyButton->setEnabled(hasDetails);
    buttonLayout->addWidget(detailsButton);
    buttonLayout->addWidget(copyButton);
    buttonLayout->addStretch();
    auto *closeButton = new QPushButton(QStringLiteral("OK"), &dialog);
    closeButton->setDefault(true);
    buttonLayout->addWidget(closeButton);
    layout->addLayout(buttonLayout);

    QObject::connect(detailsButton, &QPushButton::clicked, &dialog,
                     [&dialog, details, detailsButton] {
        const auto show = !details->isVisible();
        details->setVisible(show);
        detailsButton->setText(show ? QStringLiteral("Hide Details")
                                    : QStringLiteral("Show Details"));
        if (show) dialog.resize(960, 680);
    });
    QObject::connect(copyButton, &QPushButton::clicked, &dialog,
                     [copyButton, diagnosticDetails] {
        QApplication::clipboard()->setText(diagnosticDetails);
        copyButton->setText(QStringLiteral("Copied"));
        QTimer::singleShot(1500, copyButton, [copyButton] {
            copyButton->setText(QStringLiteral("Copy Details"));
        });
    });
    QObject::connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.resize(showDetailsInitially ? QSize(960, 680) : QSize(640, 240));
    dialog.exec();
}

void showAiErrorDialog(QWidget *parent, const AiResolution &resolution) {
    showDetailedMessageDialog(parent, QStringLiteral("AI resolution failed"), resolution.error,
                              resolution.errorDetails, QStyle::SP_MessageBoxCritical);
}

QString projectDirectory(const ProjectStore &store, const Project &project) {
    return QString::fromUtf8(store.projectPath(project.id).string().c_str());
}

QIcon projectIcon(const ProjectStore &store, const Project &project) {
    const auto path = store.iconPath(project);
    if (!path.empty()) {
        QPixmap pixmap;
        if (pixmap.load(QString::fromUtf8(path.string().c_str()))) return QIcon(pixmap);
    }
    return QIcon::fromTheme(QStringLiteral("application-x-executable"));
}

QString retainedPackagePath(const ProjectStore &store, const PackageRelease &release) {
    for (auto build = release.builds.crbegin(); build != release.builds.crend(); ++build) {
        for (const auto &artifact : build->artifacts) {
            const auto path = store.releasePath(release) /
                std::filesystem::path(artifact.relativePath.toUtf8().constData());
            const auto value = QString::fromUtf8(path.string().c_str());
            if (QFileInfo::exists(value)) return value;
        }
    }
    for (const auto &path : release.producedPackages) {
        if (QFileInfo::exists(path)) return path;
    }
    return {};
}

const BuildRecord *latestSuccessfulBuild(const PackageRelease &release) {
    for (auto build = release.builds.crbegin(); build != release.builds.crend(); ++build) {
        if (build->status == BuildStatus::Succeeded) return &*build;
    }
    return nullptr;
}

bool releaseHasExistingBuild(const PackageRelease &release) {
    if (release.buildStatus == BuildStatus::Succeeded) return true;
    return std::any_of(release.builds.cbegin(), release.builds.cend(), [](const auto &build) {
        return build.status == BuildStatus::Succeeded;
    });
}

void applyPrimaryActionStyle(QPushButton *button) {
    button->setStyleSheet(
        QStringLiteral("QPushButton { font-weight: 600; padding: 8px 22px; min-height: 36px; }"));
    button->setDefault(true);
}

QString builtPackageSummaryHtml(const ProjectStore &store, const PackageRelease &release,
                                const bool building, const bool installing) {
    if (building) {
        return QStringLiteral(
            "<b>Building…</b><br>Progress and command output are in the dialog.");
    }
    if (installing) {
        return QStringLiteral(
            "<b>Installing…</b><br>Progress and command output are in the dialog.");
    }
    const auto packagePath = retainedPackagePath(store, release);
    if (packagePath.isEmpty()) {
        if (release.buildStatus == BuildStatus::Failed) {
            return QStringLiteral(
                "<b>No package on disk.</b><br>The last build failed. Rebuild to produce an Arch package.");
        }
        if (release.buildStatus == BuildStatus::Canceled) {
            return QStringLiteral(
                "<b>No package on disk.</b><br>The last build was canceled.");
        }
        if (releaseHasExistingBuild(release)) {
            return QStringLiteral(
                "<b>No package on disk.</b><br>A previous build succeeded, but the package file is no longer retained. Rebuild to recreate it.");
        }
        return QStringLiteral(
            "<b>Not built yet.</b><br>Build this release to produce an Arch package, then install it with pacman.");
    }
    const QFileInfo info(packagePath);
    qint64 size = info.size();
    auto builtAt = info.lastModified();
    QString packageName;
    QString packageVersion;
    QString architecture;
    if (const auto *build = latestSuccessfulBuild(release)) {
        if (build->finishedAt.isValid()) builtAt = build->finishedAt;
        if (!build->artifacts.isEmpty()) {
            const auto &artifact = build->artifacts.first();
            if (artifact.size > 0) size = artifact.size;
            if (artifact.createdAt.isValid()) builtAt = artifact.createdAt;
            packageName = artifact.packageName;
            packageVersion = artifact.packageVersion;
            architecture = artifact.architecture;
        }
    }
    QStringList lines{QStringLiteral("<b>Built package</b>")};
    if (!packageName.isEmpty()) {
        auto identity = packageName.toHtmlEscaped();
        if (!packageVersion.isEmpty()) identity += QLatin1Char(' ') + packageVersion.toHtmlEscaped();
        if (!architecture.isEmpty()) {
            identity += QStringLiteral(" (%1)").arg(architecture.toHtmlEscaped());
        }
        lines.append(identity);
    }
    lines.append(info.fileName().toHtmlEscaped());
    lines.append(QStringLiteral("Size: %1").arg(QLocale().formattedDataSize(size)));
    if (builtAt.isValid()) {
        lines.append(QStringLiteral("Built: %1").arg(
            QLocale().toString(builtAt.toLocalTime(), QLocale::ShortFormat)));
    }
    return lines.join(QStringLiteral("<br>"));
}

qsizetype pendingPayloadReviews(const PackageRelease &project) {
    if (project.sourceType == SourcePackageType::AppImage) return 0;
    return std::count_if(project.payload.cbegin(), project.payload.cend(), [&project](const auto &entry) {
        return entry.requiresReview && PayloadReview::state(project, entry).needsReview;
    });
}

bool unsafePackageSymlink(const PayloadEntry &entry) {
    return !entry.symlinkTarget.isEmpty() &&
           !PathSafety::safePackageSymlinkTarget(entry.path, entry.symlinkTarget);
}
bool pkgbuildReferencesLifecycle(const QString &pkgbuild, const QString &fileName) {
    if (fileName.isEmpty()) return false;
    const auto reference = pkgbuildLifecycleReference(pkgbuild);
    if (reference == fileName) return true;
    if (reference == QStringLiteral("$_PACSMITH_INSTALL") ||
        reference == QStringLiteral("${_PACSMITH_INSTALL}")) {
        return true;
    }
    const auto escaped = QRegularExpression::escape(fileName);
    const QRegularExpression assignment(
        QStringLiteral("(?m)^\\s*install\\s*=\\s*(?:'%1'|\"%1\"|%1)\\s*(?:#.*)?$")
            .arg(escaped));
    return assignment.match(pkgbuild).hasMatch();
}

QString pkgbuildLifecycleReference(const QString &pkgbuild) {
    const QRegularExpression assignment(
        QStringLiteral("(?m)(?:^|&&\\s*)install\\s*=\\s*(?:'([^']+)'|\"([^\"]+)\"|([^\\s#]+))"));
    const auto match = assignment.match(pkgbuild);
    if (!match.hasMatch()) return {};
    for (int capture = 1; capture <= 3; ++capture) {
        if (!match.captured(capture).isEmpty()) return match.captured(capture);
    }
    return {};
}

bool findingRequiresArchAction(const PackageRelease &project, const ScriptFinding &finding) {
    if (finding.disposition == ScriptDisposition::Unresolved) return true;
    return finding.disposition == ScriptDisposition::LifecycleRequired &&
           (!project.lifecycleScript.validationPassed ||
            !project.lifecycleScript.sourceFingerprints.contains(finding.evidenceFingerprint));
}

qsizetype unresolvedResponsibilitiesForScript(const PackageRelease &project, const QString &scriptName) {
    return std::count_if(project.scriptFindings.cbegin(), project.scriptFindings.cend(),
                         [&project, &scriptName](const auto &finding) {
        return finding.scriptName == scriptName && findingRequiresArchAction(project, finding);
    });
}

qsizetype pendingScriptFindings(const PackageRelease &project) {
    return std::count_if(project.scriptFindings.cbegin(), project.scriptFindings.cend(),
                         [&project](const auto &finding) {
        const auto script = std::find_if(project.maintainerScripts.cbegin(),
                                         project.maintainerScripts.cend(),
                                         [&](const auto &candidate) {
                                             return candidate.name == finding.scriptName;
                                         });
        if (script != project.maintainerScripts.cend() && !script->requiresReview()) return false;
        if (finding.disposition == ScriptDisposition::Unresolved) return true;
        if (finding.disposition != ScriptDisposition::LifecycleRequired) return false;
        return !project.lifecycleScript.validationPassed ||
               !project.lifecycleScript.sourceFingerprints.contains(finding.evidenceFingerprint);
    });
}

} // namespace pacsmith::gui
