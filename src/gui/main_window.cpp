#include "gui/main_window.hpp"

#include "core/pkgbuild_generator.hpp"
#include "core/pkgbuild_install_plan.hpp"
#include "core/lifecycle_validator.hpp"
#include "core/repository_trust.hpp"
#include "core/payload_inspector.hpp"
#include "core/payload_review.hpp"
#include "core/package_artifact.hpp"
#include "core/background_updates.hpp"
#include "core/github_update_service.hpp"
#include "core/path_safety.hpp"
#include "core/managed_package.hpp"
#include "gui/ai_progress_dialog.hpp"
#include "gui/age_unlock.hpp"
#include "gui/command_progress_dialog.hpp"
#include "gui/desktop_entry_highlighter.hpp"
#include "gui/gui_instance.hpp"
#include "gui/import_worker.hpp"
#include "gui/pkgbuild_highlighter.hpp"
#include "gui/reanalyze_worker.hpp"
#include "gui/shell_highlighter.hpp"

#include <QAbstractItemView>
#include <QAbstractButton>
#include <QAction>
#include <QButtonGroup>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QCompleter>
#include <QCursor>
#include <QDate>
#include <QCloseEvent>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPen>
#include <QProcess>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSaveFile>
#include <QPixmap>
#include <QPushButton>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSet>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QSysInfo>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimeEdit>
#include <QSpinBox>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

namespace pacsmith::gui {
namespace {

QLabel *pageIntroduction(const QString &text, QWidget *parent);

constexpr int projectSubtitleRole = Qt::UserRole + 1;
constexpr int projectVisualStateRole = Qt::UserRole + 2;
constexpr int projectCheckingRole = Qt::UserRole + 3;
constexpr int projectActivityRole = Qt::UserRole + 4;
constexpr int sectionBaseLabelRole = Qt::UserRole + 1;

enum class ProjectVisualState { NotInstalled, Current, UpdateAvailable, Attention, Preparing };

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

struct AiDependencyCandidate {
    int index{-1};
    QString package;
};

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

class PackageNameDelegate final : public QStyledItemDelegate {
public:
    explicit PackageNameDelegate(QTableWidget *table) : QStyledItemDelegate(table), table_(table) {}

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override {
        auto *editor = qobject_cast<QLineEdit *>(
            QStyledItemDelegate::createEditor(parent, option, index));
        if (editor == nullptr) return nullptr;
        auto *completer = new QCompleter(
            table_->property("pacsmithRepositoryPackages").toStringList(), editor);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setCompletionMode(QCompleter::PopupCompletion);
        completer->setFilterMode(Qt::MatchStartsWith);
        editor->setCompleter(completer);
        return editor;
    }

private:
    QTableWidget *table_;
};

class ProjectListDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(std::max(size.height(), 60));
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        const auto selected = option.state.testFlag(QStyle::State_Selected);
        const auto darkTheme = option.palette.color(QPalette::Base).lightness() < 128;
        const auto activity = index.data(projectActivityRole).toString();
        const auto state = activity.isEmpty()
            ? static_cast<ProjectVisualState>(index.data(projectVisualStateRole).toInt())
            : ProjectVisualState::Preparing;
        const auto row = option.rect.adjusted(2, 2, -2, -2);

        if (selected || !activity.isEmpty()) {
            QColor background;
            QColor border;
            switch (state) {
            case ProjectVisualState::Current:
                background = darkTheme ? QColor(24, 70, 42) : QColor(219, 244, 226);
                border = darkTheme ? QColor(70, 205, 108) : QColor(31, 145, 66);
                break;
            case ProjectVisualState::UpdateAvailable:
            case ProjectVisualState::Attention:
                background = darkTheme ? QColor(107, 75, 0) : QColor(255, 235, 184);
                border = darkTheme ? QColor(255, 190, 48) : QColor(180, 112, 0);
                break;
            case ProjectVisualState::Preparing:
                background = darkTheme ? QColor(27, 63, 86) : QColor(216, 238, 252);
                border = option.palette.link().color();
                break;
            case ProjectVisualState::NotInstalled:
                background = darkTheme ? QColor(53, 57, 61) : QColor(228, 231, 234);
                border = darkTheme ? QColor(139, 145, 151) : QColor(112, 119, 126);
                break;
            }
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setBrush(background);
            painter->setPen(QPen(border, 1.5));
            painter->drawRoundedRect(row, 5, 5);
        }

        const auto iconSize = 44;
        const QRect iconRect(row.left() + 7, row.center().y() - iconSize / 2,
                             iconSize, iconSize);
        const auto icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        icon.paint(painter, iconRect, Qt::AlignCenter,
                   option.state.testFlag(QStyle::State_Enabled) ? QIcon::Normal
                                                                : QIcon::Disabled);

        const auto textLeft = iconRect.right() + 10;
        const bool checking = index.data(projectCheckingRole).toBool() || !activity.isEmpty();
        const auto textWidth = std::max(0, row.right() - textLeft - 7 - (checking ? 24 : 0));
        const QRect nameRect(textLeft, row.top() + 8, textWidth, 21);
        const QRect subtitleRect(textLeft, row.top() + 31, textWidth, 19);
        auto nameFont = option.font;
        nameFont.setBold(true);
        painter->setFont(nameFont);
        painter->setPen(option.palette.text().color());
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          option.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                        Qt::ElideRight, textWidth));

        auto subtitleFont = option.font;
        subtitleFont.setPointSizeF(std::max(7.0, subtitleFont.pointSizeF() - 0.5));
        subtitleFont.setBold(state == ProjectVisualState::UpdateAvailable ||
                             state == ProjectVisualState::Preparing);
        painter->setFont(subtitleFont);
        QColor secondary;
        switch (state) {
        case ProjectVisualState::Current:
            secondary = darkTheme ? QColor(92, 214, 126) : QColor(24, 125, 55);
            break;
        case ProjectVisualState::UpdateAvailable:
        case ProjectVisualState::Attention:
            secondary = darkTheme ? QColor(255, 218, 128) : QColor(105, 61, 0);
            break;
        case ProjectVisualState::Preparing:
            secondary = option.palette.link().color();
            break;
        case ProjectVisualState::NotInstalled:
            secondary = selected
                ? (darkTheme ? QColor(205, 209, 213) : QColor(72, 78, 84))
                : option.palette.placeholderText().color();
            break;
        }
        painter->setPen(secondary);
        const QFontMetrics subtitleMetrics(subtitleFont);
        const auto subtitle = activity.isEmpty() ? index.data(projectSubtitleRole).toString()
                                                 : activity;
        painter->drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          subtitleMetrics.elidedText(subtitle, Qt::ElideRight, textWidth));
        if (checking) {
            int spinnerFrame = 0;
            if (option.widget != nullptr) {
                spinnerFrame = option.widget->property("pacsmithSpinnerFrame").toInt();
                if (spinnerFrame == 0 && option.widget->parentWidget() != nullptr) {
                    spinnerFrame = option.widget->parentWidget()->property("pacsmithSpinnerFrame").toInt();
                }
            }
            const auto spinnerSize = 16;
            const QRect spinnerRect(row.right() - spinnerSize - 6,
                                    row.center().y() - spinnerSize / 2, spinnerSize, spinnerSize);
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setBrush(Qt::NoBrush);
            auto spinnerPen = QPen(option.palette.link().color(), 2.25);
            spinnerPen.setCapStyle(Qt::RoundCap);
            painter->setPen(spinnerPen);
            painter->drawArc(spinnerRect, (spinnerFrame % 4) * 90 * 16, 270 * 16);
        }
        painter->restore();
    }
};

struct GitHubRuleChoice {
    QString expression;
    bool includePrereleases{false};
};

struct RepositorySourceChoice {
    UpdateConfiguration update;
    QUrl signingKeyUrl;
    QByteArray signingKeyContents;
    QString signingKeySource;
};

struct RepositoryKeySeed {
    QString label;
    QByteArray contents;
    QString source;
};

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
            ? QStringLiteral("PacSmith will download and let you review the repository signing key, verify signed RPM metadata, select the newest matching package, verify its checksum, and import it as a persistent project.")
            : QStringLiteral("PacSmith will download and let you review the repository signing key, verify signed APT metadata, select the newest matching package, verify its SHA256, and import it as a persistent project."),
        &dialog));
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

using GitHubAiAssist = std::function<void(const QStringList &, const QString &,
                                          QLineEdit *, QLabel *, QPushButton *, QWidget *)>;

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
        QStringLiteral("Select the prebuilt artifact family PacSmith should install and track in future releases. The regular expression is saved as this release's GitHub update rule; it must match exactly one asset in each release."),
        &dialog));
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

QLabel *pageIntroduction(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    return label;
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

QFrame *settingsStatusFrame(QWidget *parent) {
    auto *frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("settingsStatusPanel"));
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setStyleSheet(QStringLiteral(
        "QFrame#settingsStatusPanel { background-color: rgba(52, 152, 219, 28); "
        "border: 1px solid rgba(52, 152, 219, 150); border-radius: 6px; } "
        "QFrame#settingsStatusPanel QLabel { background: transparent; border: none; }"));
    return frame;
}

void setSettingsSectionHelp(QLabel *label, const QString &summary, const QString &details) {
    label->setProperty("helpDetails", details);
    auto text = summary.toHtmlEscaped();
    if (!details.isEmpty()) {
        text += QStringLiteral(" <a href=\"learn-more\">Learn more</a>");
    }
    label->setText(text);
}

QLabel *settingsSectionHelp(QWidget *parent, const QString &summary, const QString &details) {
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setOpenExternalLinks(false);
    auto font = label->font();
    if (font.pointSize() >= 10) font.setPointSize(font.pointSize() - 1);
    label->setFont(font);
    auto palette = label->palette();
    const auto link = palette.color(QPalette::Link);
    const auto muted = palette.color(QPalette::PlaceholderText);
    palette.setColor(QPalette::WindowText, muted.isValid() ? muted : palette.color(QPalette::Mid));
    palette.setColor(QPalette::Link, link);
    palette.setColor(QPalette::LinkVisited, link);
    label->setPalette(palette);
    setSettingsSectionHelp(label, summary, details);
    QObject::connect(label, &QLabel::linkActivated, label, [label](const QString &) {
        const auto more = label->property("helpDetails").toString();
        if (more.isEmpty()) return;
        QMessageBox::information(label->window() != nullptr ? label->window() : static_cast<QWidget *>(label),
                                 QStringLiteral("More information"), more);
    });
    return label;
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

QString sourcePackageTypeExplanation(const SourcePackageType type) {
    switch (type) {
    case SourcePackageType::Debian:
        return QStringLiteral("Official Debian/Ubuntu binary package. PacSmith inspects control metadata, maintainer scripts, and the data archive without executing anything.");
    case SourcePackageType::Rpm:
        return QStringLiteral("Official RPM binary package. PacSmith reads the header and cpio payload statically and never runs RPM scriptlets.");
    case SourcePackageType::ArchPackage:
        return QStringLiteral("Existing Arch package. PacSmith reads .PKGINFO and the payload, then rebuilds a new package with PacSmith identity metadata.");
    case SourcePackageType::Archive:
        return QStringLiteral("tar, zip, or 7z application bundle. PacSmith maps the inspected layout into an Arch package, usually under /opt.");
    case SourcePackageType::AppImage:
        return QStringLiteral("Type 2 AppImage. PacSmith decomposes the AppDir with unsquashfs; it does not run the image, FUSE-mount it, or keep the embedded updater.");
    case SourcePackageType::ElfBinary:
        return QStringLiteral("Standalone Linux executable identified from its ELF header. PacSmith installs it to an explicit /usr/bin path and never runs it during analysis.");
    case SourcePackageType::Unknown:
        return QStringLiteral("This release has been recorded but its artifact has not been downloaded and inspected yet.");
    }
    return {};
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
                         const QColor &foreground = {}) {
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
                               const bool showDetailsInitially = false) {
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

const QColor payloadReviewAmber(Qt::darkYellow);
const QColor payloadUnsafeRed(0xe5, 0x53, 0x4b);

bool pkgbuildReferencesLifecycle(const QString &pkgbuild, const QString &fileName) {
    if (fileName.isEmpty()) return false;
    const auto escaped = QRegularExpression::escape(fileName);
    const QRegularExpression assignment(
        QStringLiteral("(?m)^\\s*install\\s*=\\s*(?:'%1'|\"%1\"|%1)\\s*(?:#.*)?$")
            .arg(escaped));
    return assignment.match(pkgbuild).hasMatch();
}

QString pkgbuildLifecycleReference(const QString &pkgbuild) {
    const QRegularExpression assignment(
        QStringLiteral("(?m)^\\s*install\\s*=\\s*(?:'([^']+)'|\"([^\"]+)\"|([^\\s#]+))"));
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

} // namespace

MainWindow::MainWindow(AppSettingsStore &settingsStore, CredentialStore &credentials,
                       QWidget *parent)
    : QMainWindow(parent), settingsStore_(settingsStore), aiSettings_(settingsStore_.load()),
      credentialStore_(credentials), buildService_(this),
      installService_(this), debDownloadService_(this), signingKeyDownloadService_(this),
      aptUpdateService_(this), rpmUpdateService_(this),
      githubUpdateService_(this), aiService_(this),
      aiModelCatalogService_(this), chatGptLoginService_(this) {
    setWindowTitle(QStringLiteral("PacSmith"));
    setAcceptDrops(true);

    auto *githubAction = new QAction(QStringLiteral("GitHub Link…"), this);
    auto *packageFileAction = new QAction(QStringLiteral("Package File…"), this);
    auto *directUrlAction = new QAction(QStringLiteral("Direct Download URL…"), this);
    auto *aptRepositoryAction = new QAction(QStringLiteral("APT Repository…"), this);
    auto *rpmRepositoryAction = new QAction(QStringLiteral("RPM Repository…"), this);
    packageFileAction->setShortcut(QKeySequence::Open);
    connect(githubAction, &QAction::triggered, this, &MainWindow::importGitHubUrl);
    connect(packageFileAction, &QAction::triggered, this, &MainWindow::chooseImport);
    connect(directUrlAction, &QAction::triggered, this, &MainWindow::importDirectUrl);
    connect(aptRepositoryAction, &QAction::triggered, this,
            &MainWindow::importAptRepository);
    connect(rpmRepositoryAction, &QAction::triggered, this,
            &MainWindow::importRpmRepository);

    auto *splitter = new QSplitter(this);
    auto *leftPanel = new QWidget(splitter);
    projectSidebar_ = leftPanel;
    auto *leftLayout = new QVBoxLayout(leftPanel);
    auto *packagesLabel = new QLabel(QStringLiteral("<b>Packages</b>"), leftPanel);
    projectList_ = new QListWidget(leftPanel);
    projectList_->setMinimumWidth(260);
    projectList_->setIconSize(QSize(44, 44));
    projectList_->setSpacing(2);
    projectList_->setItemDelegate(new ProjectListDelegate(projectList_));
    preparationSpinnerTimer_ = new QTimer(this);
    preparationSpinnerTimer_->setInterval(160);
    connect(preparationSpinnerTimer_, &QTimer::timeout, this, [this] {
        preparationSpinnerFrame_ = (preparationSpinnerFrame_ + 1) % 4;
        updatePreparationIndicators();
        updateUpdateCheckIndicators();
        syncActivityTimer();
    });
    deleteProjectButton_ = new QPushButton(QStringLiteral("Delete"), leftPanel);
    deleteProjectButton_->setEnabled(false);
    deleteProjectButton_->setToolTip(QStringLiteral("Delete the selected project"));
    auto *newButton = new QPushButton(QStringLiteral("New"), leftPanel);
    auto *sidebarNewMenu = new QMenu(newButton);
    sidebarNewMenu->addAction(githubAction);
    sidebarNewMenu->addAction(packageFileAction);
    sidebarNewMenu->addAction(directUrlAction);
    sidebarNewMenu->addSeparator();
    sidebarNewMenu->addAction(aptRepositoryAction);
    sidebarNewMenu->addAction(rpmRepositoryAction);
    newButton->setMenu(sidebarNewMenu);
    newButton->setToolTip(QStringLiteral("Create a project from a repository, GitHub release, package file, or direct download URL"));
    auto *settingsButton = new QPushButton(QStringLiteral("Settings"), leftPanel);
    settingsButton->setToolTip(QStringLiteral("Configure AI providers, update checks, retention, and credentials"));
    leftLayout->addWidget(packagesLabel);
    leftLayout->addWidget(projectList_, 1);
    auto *projectButtons = new QHBoxLayout;
    projectButtons->addWidget(newButton);
    projectButtons->addWidget(deleteProjectButton_);
    projectButtons->addWidget(settingsButton);
    leftLayout->addLayout(projectButtons);
    connect(deleteProjectButton_, &QPushButton::clicked, this, &MainWindow::deleteCurrentProject);
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::showSettings);

    auto *rightPanel = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightStack_ = new QStackedWidget(rightPanel);

    auto *dashboard = new QWidget(rightStack_);
    auto *dashboardLayout = new QVBoxLayout(dashboard);
    projectTitle_ = new QLabel(QStringLiteral("<h2>PacSmith</h2>"), dashboard);
    projectSubtitle_ = new QLabel(QStringLiteral("Import a vendor artifact or GitHub release to begin."), dashboard);
    projectSubtitle_->setWordWrap(true);
    projectPrimaryButton_ = new QPushButton(dashboard);
    applyPrimaryActionStyle(projectPrimaryButton_);
    projectPrimaryButton_->setVisible(false);
    uninstallButton_ = new QPushButton(QStringLiteral("Uninstall"), dashboard);
    uninstallButton_->setVisible(false);
    auto *dashboardHeader = new QHBoxLayout;
    auto *dashboardHeading = new QVBoxLayout;
    dashboardHeading->addWidget(projectTitle_);
    dashboardHeading->addWidget(projectSubtitle_);
    dashboardHeader->addLayout(dashboardHeading, 1);
    dashboardHeader->addWidget(projectPrimaryButton_, 0, Qt::AlignTop);
    dashboardHeader->addWidget(uninstallButton_, 0, Qt::AlignTop);
    connect(projectPrimaryButton_, &QPushButton::clicked, this,
            &MainWindow::handleProjectPrimaryAction);
    connect(uninstallButton_, &QPushButton::clicked, this, &MainWindow::startUninstall);
    projectTabs_ = new QTabWidget(dashboard);
    dashboardUpdatesHost_ = emptyPageHost(dashboard);
    updatesEditor_ = createUpdatesPage();
    dashboardUpdatesHost_->layout()->addWidget(updatesEditor_);
    projectTabs_->addTab(createProjectInfoPage(), QStringLiteral("Project Info"));
    projectTabs_->addTab(dashboardUpdatesHost_, QStringLiteral("Updates"));
    projectTabs_->addTab(createOverviewPage(), QStringLiteral("Version History"));
    connect(projectTabs_, &QTabWidget::currentChanged, this, [this] {
        if (rightStack_ == nullptr || rightStack_->currentIndex() != 0) return;
        if (projectTabs_->currentWidget() == dashboardUpdatesHost_) {
            placeUpdatesEditor();
            if (project_) populateUpdates();
        }
    });
    dashboardLayout->addLayout(dashboardHeader);
    dashboardLayout->addWidget(projectTabs_, 1);

    auto *workbench = new QWidget(rightStack_);
    auto *workbenchLayout = new QVBoxLayout(workbench);
    auto *workbenchHeader = new QHBoxLayout;
    auto *backButton = new QPushButton(QStringLiteral("← Back to Project"), workbench);
    auto *workbenchHeading = new QVBoxLayout;
    auto *titleRow = new QHBoxLayout;
    workbenchTitle_ = new QLabel(QStringLiteral("<h2>Package Setup</h2>"), workbench);
    sourceTypeBadge_ = new QLabel(workbench);
    sourceTypeBadge_->setObjectName(QStringLiteral("sourceTypeBadge"));
    sourceTypeBadge_->setAlignment(Qt::AlignCenter);
    sourceTypeBadge_->setStyleSheet(QStringLiteral(
        "QLabel#sourceTypeBadge { background: rgba(52, 152, 219, 36); border: 1px solid #2e86ab; "
        "border-radius: 4px; padding: 4px 10px; font-weight: 600; }"));
    titleRow->addWidget(workbenchTitle_, 0, Qt::AlignVCenter);
    titleRow->addWidget(sourceTypeBadge_, 0, Qt::AlignVCenter);
    titleRow->addStretch();
    workbenchSubtitle_ = new QLabel(
        QStringLiteral("Inspect the imported artifact, configure the Pacman package, then review the generated recipe."),
        workbench);
    workbenchSubtitle_->setWordWrap(true);
    workbenchHeading->addLayout(titleRow);
    workbenchHeading->addWidget(workbenchSubtitle_);
    workbenchHeader->addWidget(backButton, 0, Qt::AlignTop);
    workbenchHeader->addLayout(workbenchHeading, 1);
    reanalyzeButton_ = new QPushButton(QStringLiteral("Reanalyze Artifact…"), workbench);
    reanalyzeButton_->setToolTip(QStringLiteral(
        "Discard this release's package-setup decisions and rebuild them from the stored artifact"));
    workbenchHeader->addWidget(reanalyzeButton_, 0, Qt::AlignTop);
    resolveWithAiButton_ = new QPushButton(QStringLiteral("Resolve Review Items with AI"), workbench);
    workbenchHeader->addWidget(resolveWithAiButton_, 0, Qt::AlignTop);
    stageTabs_ = new QTabWidget(workbench);
    auto *sourceHost = createStageHost(&sourceNav_, &sourceStack_);
    auto *modeSwitch = new QWidget(workbench);
    modeSwitch->setObjectName(QStringLiteral("configModeSwitch"));
    auto *modeLayout = new QHBoxLayout(modeSwitch);
    modeLayout->setContentsMargins(0, 0, 0, 0);
    modeLayout->setSpacing(0);
    guidedModeButton_ = new QPushButton(QStringLiteral("Guided"), modeSwitch);
    customModeButton_ = new QPushButton(QStringLiteral("Custom"), modeSwitch);
    guidedModeButton_->setObjectName(QStringLiteral("configModeGuided"));
    customModeButton_->setObjectName(QStringLiteral("configModeCustom"));
    guidedModeButton_->setCheckable(true);
    customModeButton_->setCheckable(true);
    guidedModeButton_->setCursor(Qt::PointingHandCursor);
    customModeButton_->setCursor(Qt::PointingHandCursor);
    guidedModeButton_->setToolTip(QStringLiteral("Generate the PKGBUILD from these settings"));
    customModeButton_->setToolTip(QStringLiteral("Edit the PKGBUILD directly; Guided settings are ignored"));
    auto *modeGroup = new QButtonGroup(modeSwitch);
    modeGroup->setExclusive(true);
    modeGroup->addButton(guidedModeButton_);
    modeGroup->addButton(customModeButton_);
    guidedModeButton_->setChecked(true);
    modeLayout->addWidget(guidedModeButton_, 1);
    modeLayout->addWidget(customModeButton_, 1);
    modeSwitch->setStyleSheet(QStringLiteral(
        "QPushButton#configModeGuided, QPushButton#configModeCustom {"
        "  padding: 5px 0; border: 1px solid #5a7a90; background: transparent; }"
        "QPushButton#configModeGuided { border-top-left-radius: 4px; border-bottom-left-radius: 4px;"
        "  border-right: none; }"
        "QPushButton#configModeCustom { border-top-right-radius: 4px; border-bottom-right-radius: 4px; }"
        "QPushButton#configModeGuided:checked, QPushButton#configModeCustom:checked {"
        "  background: rgba(52, 152, 219, 70); font-weight: 600; }"));
    connect(guidedModeButton_, &QPushButton::clicked, this, [this] { setConfigurationMode(false); });
    connect(customModeButton_, &QPushButton::clicked, this, [this] { setConfigurationMode(true); });
    auto *configHost = createStageHost(&configNav_, &configStack_, modeSwitch);
    auto *resultHost = createStageHost(&resultNav_, &resultStack_);
    stageTabs_->addTab(sourceHost, QStringLiteral("Deb Package"));
    stageTabs_->addTab(configHost, QStringLiteral("Configuration"));
    stageTabs_->addTab(resultHost, QStringLiteral("Pacman Package"));
    stageTabs_->setTabToolTip(0, QStringLiteral("Imported vendor artifact: inspect metadata, original scripts, and payload"));
    stageTabs_->setTabToolTip(1, QStringLiteral("Guided settings generate the PKGBUILD, or switch to Custom to edit it directly"));
    stageTabs_->setTabToolTip(2, QStringLiteral("What the Pacman package will install, the PKGBUILD makepkg runs, and the built package"));
    {
        QSignalBlocker sourceBlock(sourceNav_);
        QSignalBlocker configBlock(configNav_);
        QSignalBlocker resultBlock(resultNav_);
        addWorkbenchPage(0, sourceNav_, sourceStack_, EditorSection::SourceOverview,
                         QStringLiteral("Overview"), createSourceOverviewPage());
        addWorkbenchPage(0, sourceNav_, sourceStack_, EditorSection::SourceMetadata,
                         QStringLiteral("Metadata"), createSourceMetadataPage());
        addWorkbenchPage(0, sourceNav_, sourceStack_, EditorSection::SourceScripts,
                         QStringLiteral("Vendor scripts"), createVendorScriptsPage());
        addWorkbenchPage(0, sourceNav_, sourceStack_, EditorSection::SourceContents,
                         QStringLiteral("Contents"), createPayloadPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigPkgbuild,
                         QStringLiteral("PKGBUILD"), createPkgbuildPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigLayout,
                         QStringLiteral("Install layout"), createInstallLayoutPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigDependencies,
                         QStringLiteral("Dependencies"), createDependenciesPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigScripts,
                         QStringLiteral("Scripts"), createConfigScriptsPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigCommands,
                         QStringLiteral("Commands"), createCommandsPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigAppRun,
                         QStringLiteral("AppRun"), createAppRunPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigDesktopEntries,
                         QStringLiteral("Desktop entries"), createDesktopEntriesPage());
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigIcon,
                         QStringLiteral("Icon"), createIconPage());
        configUpdatesHost_ = emptyPageHost(workbench);
        addWorkbenchPage(1, configNav_, configStack_, EditorSection::ConfigUpdates,
                         QStringLiteral("Updates"), configUpdatesHost_);
        addWorkbenchPage(2, resultNav_, resultStack_, EditorSection::ResultInstallPlan,
                         QStringLiteral("Install plan"), createInstallPlanPage());
        addWorkbenchPage(2, resultNav_, resultStack_, EditorSection::ResultPkgbuild,
                         QStringLiteral("PKGBUILD"), createResultPkgbuildPage());
        addWorkbenchPage(2, resultNav_, resultStack_, EditorSection::ResultBuild,
                         QStringLiteral("Build"), createBuildPage());
    }
    sourceNav_->setCurrentRow(0);
    configNav_->setCurrentRow(1);
    resultNav_->setCurrentRow(0);
    iconNetwork_ = new QNetworkAccessManager(this);
    workbenchLayout->addLayout(workbenchHeader);
    workbenchLayout->addWidget(stageTabs_, 1);
    connect(backButton, &QPushButton::clicked, this, &MainWindow::showProjectDashboard);
    connect(reanalyzeButton_, &QPushButton::clicked, this, &MainWindow::startReanalysis);
    connect(resolveWithAiButton_, &QPushButton::clicked, this, &MainWindow::startAiResolution);
    connect(stageTabs_, &QTabWidget::currentChanged, this, [this] {
        updateWorkbenchStageChrome();
        if (rightStack_ != nullptr && rightStack_->currentIndex() == 1) {
            populateCurrentWorkbenchPage();
        }
    });

    rightStack_->addWidget(dashboard);
    rightStack_->addWidget(workbench);
    rightStack_->setCurrentWidget(dashboard);
    rightLayout->addWidget(rightStack_, 1);
    splitter->addWidget(leftPanel);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    connect(projectList_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current) {
                if (current != nullptr) loadProject(current->data(Qt::UserRole).toString());
            });

    connect(&buildService_, &BuildService::outputAvailable, this, [this](const QString &text) {
        if (commandProgress_ != nullptr) commandProgress_->appendOutput(text);
    });
    connect(&buildService_, &BuildService::failedToStart, this, [this](const QString &message) {
        if (project_ && currentRelease()->buildStatus == BuildStatus::Building) {
            currentRelease()->buildStatus = BuildStatus::Failed;
            currentRelease()->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("build"),
                                      QStringLiteral("Build could not start")});
            persistCurrent();
        }
        finishCommandProgress(false, QStringLiteral("Build could not start: %1").arg(message));
        if (commandProgress_ == nullptr) {
            QMessageBox::critical(this, QStringLiteral("Build could not start"), message);
        }
        populateBuild();
        updateDeleteButton();
    });
    connect(&buildService_, &BuildService::finished, this, [this](const ProcessResult &result) {
        if (!project_) return;
        currentRelease()->buildStatus = result.canceled ? BuildStatus::Canceled
                                                : result.succeeded() ? BuildStatus::Succeeded
                                                                     : BuildStatus::Failed;
        currentRelease()->lastBuildLog = result.output + result.errorOutput;
        if (!result.canceled) currentRelease()->producedPackages = result.producedPackages;
        currentRelease()->builds.append(buildRecordFromResult(
            QStringLiteral("build-%1").arg(result.startedAt.toMSecsSinceEpoch()),
            currentRelease()->buildStatus, currentRelease()->lastBuildLog,
            result.producedPackages, store_.releasePath(*currentRelease()),
            result.startedAt, result.finishedAt));
        if (result.succeeded()) currentRelease()->state = ReleaseState::Built;
        currentRelease()->history.append({result.finishedAt, QStringLiteral("build"),
                                  result.canceled ? QStringLiteral("Build canceled by user")
                                  : result.succeeded() ? QStringLiteral("Build succeeded")
                                                       : QStringLiteral("Build failed (exit %1)").arg(result.exitCode)});
        persistCurrent();
        populateOverview();
        populateBuild();
        populateHistory();
        updateDeleteButton();
        finishCommandProgress(!result.canceled && result.succeeded(),
                              result.canceled ? QStringLiteral("Build canceled.")
                              : result.succeeded() ? QStringLiteral("Build succeeded.")
                                                   : QStringLiteral("Build failed (exit %1).")
                                                         .arg(result.exitCode));
        statusBar()->showMessage(result.canceled ? QStringLiteral("Build canceled")
                                 : result.succeeded() ? QStringLiteral("Build succeeded")
                                                      : QStringLiteral("Build failed"), 8000);
    });
    connect(&installService_, &InstallService::outputAvailable, this, [this](const QString &text) {
        if (commandProgress_ != nullptr) commandProgress_->appendOutput(text);
    });
    connect(&installService_, &InstallService::progressChanged, this,
            [this](const QString &message) {
                statusBar()->showMessage(message);
                if (commandProgress_ != nullptr && !commandProgress_->isFinished()) {
                    commandProgress_->setStatus(message);
                }
            });
    connect(&installService_, &InstallService::failedToStart, this, [this](const QString &message) {
        projectList_->setEnabled(true);
        pendingPackageOperation_.clear();
        if (project_) {
            currentRelease()->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("install"),
                                      QStringLiteral("Installation session failed: %1").arg(message)});
            persistCurrent();
            populateHistory();
        }
        statusBar()->showMessage(QStringLiteral("Package operation failed"), 10000);
        finishCommandProgress(false, QStringLiteral("Package operation could not start: %1").arg(message));
        if (commandProgress_ == nullptr) {
            QMessageBox::critical(this, QStringLiteral("Installation could not start"), message);
        }
        populateBuild();
        updateDashboardActions();
        updateDeleteButton();
    });
    connect(&installService_, &InstallService::finished, this, [this](const ProcessResult &result) {
        projectList_->setEnabled(true);
        if (!project_) return;
        if (result.succeeded()) static_cast<void>(store_.reconcileInstalled(*project_, nullptr));
        const auto operation = pendingPackageOperation_.isEmpty()
            ? QStringLiteral("install") : pendingPackageOperation_;
        currentRelease()->history.append({result.finishedAt, operation,
                                  result.succeeded() ? QStringLiteral("Package operation succeeded")
                                                     : QStringLiteral("Package operation failed (exit %1)").arg(result.exitCode)});
        project_->history.append({result.finishedAt, operation,
                                  result.succeeded() ? QStringLiteral("Package operation succeeded")
                                                     : QStringLiteral("Package operation failed")});
        pendingPackageOperation_.clear();
        persistCurrent();
        const auto projectId = project_->id;
        refreshProjectList(projectId);
        if (!project_ || project_->id != projectId) loadProject(projectId);
        refreshCurrentProject();
        const auto succeeded = result.succeeded();
        const auto summary = operation == QStringLiteral("uninstall")
            ? (succeeded ? QStringLiteral("Uninstall completed successfully.")
                         : QStringLiteral("Uninstall failed (exit %1).").arg(result.exitCode))
            : operation == QStringLiteral("rollback")
                ? (succeeded ? QStringLiteral("Rollback completed successfully.")
                             : QStringLiteral("Rollback failed (exit %1).").arg(result.exitCode))
                : (succeeded ? QStringLiteral("Installation completed successfully.")
                             : QStringLiteral("Installation failed (exit %1).").arg(result.exitCode));
        finishCommandProgress(succeeded, summary);
        statusBar()->showMessage(succeeded ? QStringLiteral("Installation succeeded")
                                           : QStringLiteral("Installation failed"), 10000);
        if (commandProgress_ == nullptr) {
            QMessageBox::information(this, QStringLiteral("Package operation"),
                                     succeeded ? QStringLiteral("Pacman completed successfully.")
                                               : QStringLiteral("Pacman did not complete successfully. Review the captured output in the progress dialog."));
        }
    });

    connect(&debDownloadService_, &DebDownloadService::progress, this,
            [this](const qint64 received, const qint64 total) {
        preparationBytesReceived_ = received;
        preparationBytesTotal_ = total;
        if (downloadProgress_ != nullptr) {
            if (total > 0) {
                downloadProgress_->setRange(0, 1000);
                downloadProgress_->setValue(static_cast<int>(std::clamp<qint64>(
                    received * 1000 / total, 0, 1000)));
                downloadProgress_->setLabelText(
                    QStringLiteral("Downloading vendor artifact… %1 / %2 MiB\nYou may hide this window; the download will continue.")
                        .arg(received / (1024 * 1024)).arg(total / (1024 * 1024)));
            } else {
                downloadProgress_->setRange(0, 0);
                downloadProgress_->setLabelText(
                    QStringLiteral("Downloading vendor artifact… %1 MiB\nYou may hide this window; the download will continue.")
                        .arg(received / (1024 * 1024)));
            }
        }
        statusBar()->showMessage(total > 0
            ? QStringLiteral("Downloading vendor artifact: %1 / %2 MiB")
                  .arg(received / (1024 * 1024)).arg(total / (1024 * 1024))
            : QStringLiteral("Downloading vendor artifact: %1 MiB").arg(received / (1024 * 1024)));
        if (!lastPreparationPublish_.isValid() || lastPreparationPublish_.elapsed() >= 150) {
            publishPreparationActivity();
            lastPreparationPublish_.restart();
        }
        updatePreparationIndicators();
        updateUpdateCheckIndicators();
    });
    connect(&debDownloadService_, &DebDownloadService::failed, this, [this](const QString &message) {
        const auto projectId = preparingProjectId_;
        resetPreparationState();
        statusBar()->showMessage(QStringLiteral("Vendor artifact download failed"), 8000);
        QMessageBox::critical(this, QStringLiteral("Could not prepare release"), message);
        if (!projectId.isEmpty()) refreshProjectList(projectId);
        populateOverview();
    });
    connect(&debDownloadService_, &DebDownloadService::finished, this, [this](const QString &path) {
        preparationPhase_ = QStringLiteral("Inspecting");
        if (downloadProgress_ != nullptr) {
            downloadProgress_->close();
            downloadProgress_->deleteLater();
            downloadProgress_ = nullptr;
        }
        updatePreparationIndicators();
        publishPreparationActivity();
        statusBar()->showMessage(QStringLiteral("Vendor artifact downloaded; importing release"));
        pendingDownloadedImport_ = path;
        importPackage(path);
    });
    connect(&signingKeyDownloadService_, &RepositoryKeyDownloadService::progress, this,
            [this](const qint64 received, const qint64 total) {
        if (signingKeyProgress_ == nullptr) return;
        if (total > 0) {
            signingKeyProgress_->setRange(0, 1000);
            signingKeyProgress_->setValue(static_cast<int>(std::clamp<qint64>(
                received * 1000 / total, 0, 1000)));
        } else {
            signingKeyProgress_->setRange(0, 0);
        }
        signingKeyProgress_->setLabelText(
            QStringLiteral("Downloading repository signing key… %1 KiB")
                .arg(received / 1024));
    });
    connect(&signingKeyDownloadService_, &RepositoryKeyDownloadService::failed, this,
            [this](const QString &message) {
        if (signingKeyProgress_ != nullptr) {
            signingKeyProgress_->close();
            signingKeyProgress_->deleteLater();
            signingKeyProgress_ = nullptr;
        }
        projectList_->setEnabled(true);
        if (aptSigningKeyDownloadButton_ != nullptr) {
            aptSigningKeyDownloadButton_->setText(QStringLiteral("Fetch && Review…"));
            aptSigningKeyDownloadButton_->setEnabled(updateStrategy_->currentIndex() == 2 ||
                                                      updateStrategy_->currentIndex() == 3);
        }
        aptSigningKeyUrl_->setEnabled(updateStrategy_->currentIndex() == 2 ||
                                      updateStrategy_->currentIndex() == 3);
        signingKeyDownloadProjectId_.clear();
        signingKeyDownloadReleaseId_.clear();
        if (message == QStringLiteral("Signing-key download canceled")) {
            statusBar()->showMessage(message, 5000);
        } else {
            QMessageBox::critical(this, QStringLiteral("Could not download signing key"), message);
        }
    });
    connect(&signingKeyDownloadService_, &RepositoryKeyDownloadService::finished, this,
            [this](const QByteArray &contents, const QUrl &requestedUrl, const QUrl &resolvedUrl) {
        if (signingKeyProgress_ != nullptr) {
            signingKeyProgress_->close();
            signingKeyProgress_->deleteLater();
            signingKeyProgress_ = nullptr;
        }
        projectList_->setEnabled(true);
        aptSigningKeyDownloadButton_->setText(QStringLiteral("Fetch && Review…"));
        aptSigningKeyDownloadButton_->setEnabled(updateStrategy_->currentIndex() == 2 ||
                                                  updateStrategy_->currentIndex() == 3);
        aptSigningKeyUrl_->setEnabled(updateStrategy_->currentIndex() == 2 ||
                                      updateStrategy_->currentIndex() == 3);

        const auto targetProject = signingKeyDownloadProjectId_;
        const auto targetRelease = signingKeyDownloadReleaseId_;
        signingKeyDownloadProjectId_.clear();
        signingKeyDownloadReleaseId_.clear();
        if (!project_ || project_->id != targetProject || project_->release(targetRelease) == nullptr) {
            QMessageBox::warning(
                this, QStringLiteral("Signing key not imported"),
                QStringLiteral("The release that requested this signing key is no longer open. Fetch it again from that release's Updates page."));
            return;
        }

        QString inspectionError;
        const auto inspection = RepositoryTrust::inspectKey(contents, &inspectionError);
        if (!inspection) {
            QMessageBox::critical(this, QStringLiteral("Downloaded file is not an OpenPGP key"),
                                  inspectionError);
            return;
        }

        const auto resolvedNotice = requestedUrl == resolvedUrl
            ? QString{}
            : QStringLiteral("\nThe server redirected the download to %1.")
                  .arg(resolvedUrl.toDisplayString());
        QMessageBox confirmation(QMessageBox::Question,
                                 QStringLiteral("Trust downloaded signing key?"),
                                 QStringLiteral("PacSmith found OpenPGP fingerprint:\n%1")
                                     .arg(inspection->fingerprints.join(QStringLiteral("\n"))),
                                 QMessageBox::Yes | QMessageBox::Cancel, this);
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.setInformativeText(
            QStringLiteral("Confirm this fingerprint against the vendor's published documentation when possible. The HTTPS URL identifies where the bytes came from; the pinned fingerprint is what protects future APT checks.%1")
                .arg(resolvedNotice));
        confirmation.setDetailedText(
            QStringLiteral("Requested URL: %1\nResolved URL: %2\nSHA256: %3\nFingerprint(s):\n%4")
                .arg(requestedUrl.toString(), resolvedUrl.toString(), inspection->sha256,
                     inspection->fingerprints.join(QStringLiteral("\n"))));
        auto *trustButton = confirmation.button(QMessageBox::Yes);
        if (trustButton != nullptr) trustButton->setText(QStringLiteral("Trust and Pin Key"));
        if (confirmation.exec() != QMessageBox::Yes) {
            statusBar()->showMessage(QStringLiteral("Signing key downloaded but not trusted"), 6000);
            return;
        }

        auto *tracker = project_->release(targetRelease);
        QString importError;
        const auto key = RepositoryTrust::importUserKey(
            store_.releasePath(*tracker), contents, requestedUrl.toString(), &importError);
        if (!key) {
            QMessageBox::critical(this, QStringLiteral("Could not import signing key"), importError);
            return;
        }
        const auto duplicate = std::find_if(
            tracker->update.signingKeys.cbegin(), tracker->update.signingKeys.cend(),
            [&](const auto &candidate) { return candidate.sha256 == key->sha256; });
        if (duplicate == tracker->update.signingKeys.cend()) tracker->update.signingKeys.append(*key);
        tracker->update.aptSigningKeyring = key->relativePath;
        tracker->update.trustedSigningFingerprint = key->fingerprints.first();
        tracker->fieldProvenance.insert(QStringLiteral("update.aptSigningKeyring"), key->provenance);
        tracker->fieldProvenance.insert(QStringLiteral("update.trustedSigningFingerprint"), key->provenance);
        tracker->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("update-key"),
                                 QStringLiteral("Trusted repository key %1 downloaded from %2")
                                     .arg(key->fingerprints.first(), requestedUrl.toString())});
        if (persistCurrent()) {
            populateUpdates();
            statusBar()->showMessage(QStringLiteral("Repository signing key trusted and pinned"), 7000);
        }
    });
    connect(&aptUpdateService_, &AptUpdateService::progressChanged, this, [this](const QString &message) {
        updateCheckStatus_->setText(message);
    });
    connect(&aptUpdateService_, &AptUpdateService::finished, this, [this](const UpdateCheckResult &result) {
        applyUpdateCheckResult(result, QStringLiteral("APT"));
    });
    connect(&rpmUpdateService_, &RpmUpdateService::progressChanged, this, [this](const QString &message) {
        updateCheckStatus_->setText(message);
    });
    connect(&rpmUpdateService_, &RpmUpdateService::finished, this, [this](const UpdateCheckResult &result) {
        applyUpdateCheckResult(result, QStringLiteral("RPM"));
    });
    connect(&githubUpdateService_, &GitHubUpdateService::progressChanged, this,
            [this](const QString &message) { updateCheckStatus_->setText(message); });
    connect(&githubUpdateService_, &GitHubUpdateService::finished, this,
            [this](const UpdateCheckResult &result) {
        applyUpdateCheckResult(result, QStringLiteral("GitHub"));
    });

    connect(&aiService_, &AiAnalysisService::progressChanged, this, [this](const QString &message) {
        if (aiProgress_ != nullptr) aiProgress_->setStatus(message);
    });
    connect(&aiService_, &AiAnalysisService::activityChanged, this, [this](const QString &message) {
        if (aiProgress_ != nullptr) aiProgress_->appendActivity(message);
    });
    connect(&aiService_, &AiAnalysisService::responseProgress, this,
            [this](const qint64 bytesReceived, const qint64 outputCharacters) {
        if (aiProgress_ != nullptr) {
            aiProgress_->setResponseProgress(bytesReceived, outputCharacters);
        }
    });
    connect(&aiService_, &AiAnalysisService::requestAvailable, this,
            [this](const int round, const QByteArray &body) {
        if (aiProgress_ != nullptr) aiProgress_->setRequest(round, body);
    });
    connect(&aiService_, &AiAnalysisService::responseDelta, this,
            [this](const int round, const QString &text) {
        if (aiProgress_ != nullptr) aiProgress_->appendResponseDelta(round, text);
    });
    connect(&aiService_, &AiAnalysisService::credentialUpdated, this,
            [this](const QString &serialized) {
        const auto source = aiSettings_.credentialSources.value(
            QStringLiteral("chatgpt"), CredentialSource::Keyring);
        if (source == CredentialSource::Age && !credentialStore_.ageUnlocked()) {
            statusBar()->showMessage(
                QStringLiteral("ChatGPT session refreshed for this run, but encrypted credentials are locked"),
                10000);
            return;
        }
        QString error;
        if (!credentialStore_.store(QStringLiteral("chatgpt"), source, serialized,
                                    QString{}, &error)) {
            statusBar()->showMessage(
                QStringLiteral("Could not persist the refreshed ChatGPT session: %1").arg(error), 10000);
        }
    });
    connect(&aiService_, &AiAnalysisService::finished, this, [this](const AiResolution &resolution) {
        const auto canceled = std::exchange(aiProgressCanceled_, false);
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
            validateAndApplyAiResolution(resolution);
        }
    });

    loadRepositoryPackageCatalog();
    refreshProjectList();
    QTimer::singleShot(0, this, [this] {
        const auto providerName = aiProviderName(aiSettings_.provider);
        const auto defaultSource = aiSettings_.provider == AiProviderKind::ChatGpt
                                       ? CredentialSource::Keyring
                                       : CredentialSource::Environment;
        const auto source = aiSettings_.credentialSources.value(providerName, defaultSource);
        if ((aiSettings_.provider == AiProviderKind::ChatGpt ||
             aiSettings_.provider == AiProviderKind::OpenAi ||
             aiSettings_.provider == AiProviderKind::Xai) &&
            source == CredentialSource::Age && credentialStore_.hasAgeFile()) {
            static_cast<void>(unlockAgeCredentials());
        }
        syncActivityTimer();
    });
}

QWidget *MainWindow::createProjectInfoPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Project status and immutable acquisition details. Update settings are on the Updates tab."),
        page));
    auto *summary = new QHBoxLayout;
    overviewIcon_ = new QLabel(page);
    overviewIcon_->setFixedSize(96, 96);
    overviewIcon_->setAlignment(Qt::AlignCenter);
    auto *details = new QVBoxLayout;
    projectStateLabel_ = new QLabel(page);
    projectStateLabel_->setWordWrap(true);
    projectStateLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    activeTrackerLabel_ = new QLabel(page);
    activeTrackerLabel_->setWordWrap(true);
    activeTrackerLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    projectAcquisitionLabel_ = new QLabel(page);
    projectAcquisitionLabel_->setWordWrap(true);
    projectAcquisitionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(projectStateLabel_);
    details->addWidget(activeTrackerLabel_);
    details->addWidget(projectAcquisitionLabel_);
    details->addStretch();
    summary->addWidget(overviewIcon_, 0, Qt::AlignTop);
    summary->addLayout(details, 1);
    layout->addLayout(summary);

    auto *editRow = new QHBoxLayout;
    editRow->addStretch();
    editConfigurationButton_ = new QPushButton(QStringLiteral("Edit Package Configuration"), page);
    editConfigurationButton_->setVisible(false);
    editRow->addWidget(editConfigurationButton_);
    editRow->addStretch();
    layout->addLayout(editRow);
    layout->addStretch();
    connect(editConfigurationButton_, &QPushButton::clicked, this,
            &MainWindow::editPackageConfiguration);
    return page;
}

QWidget *MainWindow::createOverviewPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Each vendor release retains its acquisition evidence, update configuration, package recipe, builds, and install history. Select a release for operational actions or open its package setup."), page));
    releaseTable_ = new QTableWidget(page);
    releaseTable_->setColumnCount(8);
    releaseTable_->setHorizontalHeaderLabels({QStringLiteral("Vendor version"),
        QStringLiteral("Status"), QStringLiteral("Acquired from"), QStringLiteral("Update strategy"), QStringLiteral("Review"),
        QStringLiteral("Builds"), QStringLiteral("Arch package"), QStringLiteral("Installed")});
    releaseTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    releaseTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    releaseTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    releaseTable_->horizontalHeader()->setStretchLastSection(true);
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
    historyCheckUpdatesButton_->setToolTip(
        QStringLiteral("Check the current update source for a newer vendor release"));
    historyCheckUpdatesButton_->setEnabled(false);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(historyCheckUpdatesButton_);
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
             !debDownloadService_.isRunning() && importThread_ == nullptr));
        const bool installed = selected != nullptr && selected->id == project_->installedReleaseId;
        installReleaseButton_->setEnabled(selected != nullptr && !installed &&
                                          !retainedPackagePath(store_, *selected).isEmpty() &&
                                          !installService_.isRunning());
        rollbackButton_->setEnabled(selected != nullptr && !installed && !project_->installedVersion.isEmpty() &&
                                    (!selected->builds.isEmpty() || !selected->producedPackages.isEmpty()));
        deleteReleaseButton_->setEnabled(selected != nullptr && !installed && !preparing &&
                                         !installService_.isRunning());
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
        QStringLiteral("This is the vendor artifact PacSmith imported. It is evidence only: analysis never executes package content, and this page does not change the generated Arch package."),
        page));
    sourceTypeHeadline_ = new QLabel(page);
    sourceTypeHeadline_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sourceTypeHeadline_->setWordWrap(true);
    sourceTypeExplanation_ = new QLabel(page);
    sourceTypeExplanation_->setWordWrap(true);
    sourceTypeExplanation_->setTextInteractionFlags(Qt::TextSelectableByMouse);
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
        QStringLiteral("Vendor metadata taken from the imported artifact. These fields describe the source package; Arch package name, dependencies, and install paths are configured separately."),
        page));
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
        QStringLiteral("These settings control how the vendor archive or executable is laid out in the generated Arch package. They do not modify the imported artifact."),
        mappingGroup);
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
    installCommandsHint_ = new QLabel(
        QStringLiteral("PATH commands for this archive are configured on Commands. Choose a payload file there instead of typing a raw path here."),
        mappingGroup);
    installCommandsHint_->setWordWrap(true);
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
    appImageInstallPlan_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    appImageInstallPlan_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    appImageInstallPlan_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
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
        QStringLiteral("“Require Arch package” adds the mapped package to depends. PacSmith checks required names against your configured pacman repositories: unresolved mappings that need your review are amber, unavailable package names are red, and typing in the Arch package column suggests repository package names. Green indicates an AI-assisted decision that is resolved. Use “Bundled with application” or “Provided by this package” only when the imported payload actually contains that dependency; those choices remove it from depends."), page));
    dependenciesTable_ = new QTableWidget(page);
    dependenciesTable_->setColumnCount(5);
    dependenciesTable_->setHorizontalHeaderLabels({QStringLiteral("Debian dependency"), QStringLiteral("Arch package"),
                                                    QStringLiteral("Status"), QStringLiteral("Source / confidence"),
                                                    QStringLiteral("Treatment")});
    dependenciesTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    dependenciesTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    dependenciesTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    dependenciesTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    dependenciesTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    dependenciesTable_->verticalHeader()->setVisible(false);
    dependenciesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    dependenciesTable_->setItemDelegateForColumn(1, new PackageNameDelegate(dependenciesTable_));
    connect(dependenciesTable_, &QTableWidget::cellChanged, this, &MainWindow::dependencyEdited);
    layout->addWidget(dependenciesTable_, 1);
    return page;
}

QWidget *MainWindow::createVendorScriptsPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("Original maintainer scripts from the vendor package. PacSmith never executes them. Use them as reference while deciding Arch-specific handling under Configuration."),
        page));
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
        QStringLiteral("Vendor maintainer scripts are the source. Each row maps a responsibility onto Arch handling; the .install file below is the destination pacman may run as root. Original script text stays on the imported package tab."),
        page));
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
    scriptFindingsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    scriptFindingsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    scriptFindingsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    scriptFindingsTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
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
        QStringLiteral("Vendor filesystem from the imported artifact. Most files need no action. Keep or exclude records a recipe rule for the resulting package; it does not modify the source archive. Changed upstream content restores review."), page);
    layout->addWidget(payloadIntroduction_);
    auto *splitter = new QSplitter(Qt::Vertical, page);
    payloadTree_ = new QTreeWidget(page);
    payloadTree_->setHeaderLabels({QStringLiteral("Path"), QStringLiteral("Type"), QStringLiteral("Size"),
                                   QStringLiteral("Review")});
    payloadTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    payloadTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    payloadTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    payloadTree_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
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
        QStringLiteral("Choose which inspected executables the generated Arch package should expose on PATH. Rename a command to change the /usr/bin name. Source paths name exact files in the vendor artifact; they are not edited there. Use Add from payload to map any archive file onto /usr/bin."),
        page));
    commandsTable_ = new QTableWidget(page);
    commandsTable_->setColumnCount(6);
    commandsTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Expose"), QStringLiteral("Payload executable"),
         QStringLiteral("Command"), QStringLiteral("Destination"),
         QStringLiteral("Method"), QStringLiteral("Status")});
    commandsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    commandsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    commandsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    commandsTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    commandsTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    commandsTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
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
        QStringLiteral("AppRun is the AppImage entry point. It stays inside the extracted AppDir under /opt with the rest of the image; the PATH command on Commands is a separate wrapper. Vendor AppRun scripts often assume they are still a mounted AppImage and can hang or mis-launch once extracted. Review this script, keep the original, edit it, or resolve it with AI."),
        page));
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
        QStringLiteral("Desktop entries are host integration owned by the local recipe. PacSmith derives genuine application launchers from vendor metadata, preserves the vendor payload unchanged, and carries user-edited or additional entries forward exactly."),
        page));
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
        QStringLiteral("Select an icon detected in the inspected artifact, import a local image, or fetch an official HTTPS image for review. PacSmith stores the chosen bytes in the release and pins their SHA256."),
        page));
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
        QStringLiteral("Custom mode: this PKGBUILD is what makepkg executes. Guided configuration is ignored until you switch back. Saving keeps this file user-owned."),
        page));
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
    connect(pkgbuildBuildButton_, &QPushButton::clicked, this, &MainWindow::startBuild);
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
        QStringLiteral("Read-only PKGBUILD makepkg will run. Change Guided settings to regenerate it, or switch Configuration to Custom to edit it."),
        page));
    pkgbuildPreviewNotice_ = new QLabel(page);
    pkgbuildPreviewNotice_->setWordWrap(true);
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
    layout->addWidget(pkgbuildPreview_, 1);
    layout->addLayout(buttons);
    connect(validateButton, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(this, QStringLiteral("PKGBUILD validation"),
                                 PkgbuildGenerator::validate(currentPkgbuildText()));
    });
    connect(editButton, &QPushButton::clicked, this, [this] { setConfigurationMode(true); });
    connect(resultPkgbuildBuildButton_, &QPushButton::clicked, this, &MainWindow::startBuild);
    return page;
}

QWidget *MainWindow::createUpdatesPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(pageIntroduction(
        QStringLiteral("How PacSmith will look for the next vendor release. On the project dashboard this edits the current tracking release (installed, or newest analyzed if nothing is installed). In Configuration it edits the open release. This is not part of the Pacman package, and it is separate from the immutable acquisition recorded for the imported artifact."), page));
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
    updateUrl_ = new QLineEdit(page);
    updateUrl_->setPlaceholderText(QStringLiteral("https://vendor.example/download/package.deb"));
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
    auto *githubRegexRow = new QWidget(page);
    auto *githubRegexLayout = new QHBoxLayout(githubRegexRow);
    githubRegexLayout->setContentsMargins(0, 0, 0, 0);
    githubRegexAiButton_ = new QPushButton(QStringLiteral("Generate with AI…"), githubRegexRow);
    githubRegexLayout->addWidget(githubAssetRegex_, 1);
    githubRegexLayout->addWidget(githubRegexAiButton_);
    githubPrereleases_ = new QCheckBox(
        QStringLiteral("Track prereleases even after a stable release exists"), page);
    githubPrereleases_->setToolTip(QStringLiteral(
        "The default policy prefers stable releases, falls back to prereleases when no matching stable release exists, and moves to stable when one is published."));
    form->addRow(QStringLiteral("Strategy"), updateStrategy_);
    form->addRow(QStringLiteral("URL / repository"), updateUrl_);
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
    form->addRow(QStringLiteral("Asset-name regex"), githubRegexRow);
    form->addRow(QString{}, githubPrereleases_);
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
    connect(githubRegexAiButton_, &QPushButton::clicked, this, &MainWindow::startGithubRegexAi);
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
        const auto key = RepositoryTrust::importUserKey(store_.releasePath(*tracker), contents,
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
    const auto updateStrategyUi = [this, form, keyringRow, keyUrlRow, githubRegexRow](const int index) {
        const bool hasRelease = updateEditorRelease() != nullptr;
        const bool apt = index == 2;
        const bool rpm = index == 3;
        const bool repository = apt || rpm;
        const bool github = index == 4;
        updateUrl_->setEnabled(hasRelease && index != 0);
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
        form->setRowVisible(githubRegexRow, github);
        form->setRowVisible(githubPrereleases_, github);
        githubOwner_->setEnabled(hasRelease && github);
        githubRepository_->setEnabled(hasRelease && github);
        githubRegexRow->setEnabled(hasRelease && github);
        githubPrereleases_->setEnabled(hasRelease && github);
        githubRegexAiButton_->setEnabled(hasRelease && github &&
                                         aiSettings_.provider != AiProviderKind::None &&
                                         !aiService_.isRunning());
        updateSaveButton_->setEnabled(hasRelease);
        syncUpdateCheckButtons();
        updateNotice_->setText(index == 0 ? QStringLiteral("Manual updates: PacSmith will not query the network.")
                              : index == 1 ? QStringLiteral("Direct URL saved; automatic version discovery is not implemented yet.")
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
        QStringLiteral("Build the Arch package with makepkg, then install it with pacman. Progress and command output open in a dialog."),
        page));
    buildChecklist_ = new QLabel(page);
    buildChecklist_->setWordWrap(true);
    buildButton_ = new QPushButton(QStringLiteral("Build"), page);
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
    layout->addWidget(buildButton_, 0, Qt::AlignLeft);
    layout->addLayout(packageRow);
    layout->addStretch(1);
    connect(buildButton_, &QPushButton::clicked, this, [this] {
        if (buildService_.isRunning()) {
            buildButton_->setText(QStringLiteral("Canceling…"));
            buildButton_->setEnabled(false);
            buildService_.cancel();
        } else {
            startBuild();
        }
    });
    connect(installButton_, &QPushButton::clicked, this, &MainWindow::startInstall);
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
        this, false, knownRepositoryKeys(projectCache_, store_));
    if (choice) beginRepositoryImport(choice->update, choice->signingKeyUrl,
                                      choice->signingKeyContents,
                                      choice->signingKeySource);
}

void MainWindow::importRpmRepository() {
    const auto choice = chooseRepositorySource(
        this, true, knownRepositoryKeys(projectCache_, store_));
    if (choice) beginRepositoryImport(choice->update, choice->signingKeyUrl,
                                      choice->signingKeyContents,
                                      choice->signingKeySource);
}

void MainWindow::beginRepositoryImport(UpdateConfiguration configuration,
                                       const QUrl &signingKeyUrl,
                                       const QByteArray &signingKeyContents,
                                       const QString &signingKeySource) {
    if (repositoryImportRunning_ || debDownloadService_.isRunning() ||
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
            debDownloadService_.start(
                QUrl(result.downloadUrl), result.sha256,
                std::filesystem::path(target.toUtf8().constData()));
        };

        if (rpm) {
            auto *service = new RpmUpdateService(this);
            connect(queryProgress, &QProgressDialog::canceled, service,
                    &RpmUpdateService::cancel);
            connect(service, &RpmUpdateService::progressChanged, queryProgress,
                    &QProgressDialog::setLabelText);
            connect(service, &RpmUpdateService::finished, this,
                    [finishQuery, service](const UpdateCheckResult &result) mutable {
                finishQuery(result, service);
            });
            service->start(probe,
                           std::filesystem::path(temporary->path().toUtf8().constData()));
        } else {
            auto *service = new AptUpdateService(this);
            connect(queryProgress, &QProgressDialog::canceled, service,
                    &AptUpdateService::cancel);
            connect(service, &AptUpdateService::progressChanged, queryProgress,
                    &QProgressDialog::setLabelText);
            connect(service, &AptUpdateService::finished, this,
                    [finishQuery, service](const UpdateCheckResult &result) mutable {
                finishQuery(result, service);
            });
            service->start(probe,
                           std::filesystem::path(temporary->path().toUtf8().constData()));
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

QWidget *MainWindow::createStageHost(QListWidget **nav, QStackedWidget **stack, QWidget *navHeader) {
    auto *host = new QWidget(this);
    auto *layout = new QHBoxLayout(host);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(8);
    auto *list = new QListWidget(host);
    list->setObjectName(QStringLiteral("workbenchStageNav"));
    list->setFixedWidth(188);
    list->setSpacing(1);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setStyleSheet(QStringLiteral(
        "QListWidget#workbenchStageNav { border: none; background: transparent; outline: none; }"
        "QListWidget#workbenchStageNav::item { padding: 8px 10px; }"
        "QListWidget#workbenchStageNav::item:selected { background: rgba(52, 152, 219, 40); }"));
    auto *pages = new QStackedWidget(host);
    if (navHeader != nullptr) {
        auto *navColumn = new QWidget(host);
        navColumn->setFixedWidth(188);
        auto *navLayout = new QVBoxLayout(navColumn);
        navLayout->setContentsMargins(0, 0, 0, 0);
        navLayout->setSpacing(6);
        navHeader->setParent(navColumn);
        navLayout->addWidget(navHeader);
        list->setFixedWidth(188);
        navLayout->addWidget(list, 1);
        layout->addWidget(navColumn);
    } else {
        layout->addWidget(list);
    }
    layout->addWidget(pages, 1);
    connect(list, &QListWidget::currentRowChanged, this, [this, pages](const int row) {
        if (row >= 0) pages->setCurrentIndex(row);
        if (rightStack_ != nullptr && rightStack_->currentIndex() == 1) {
            populateCurrentWorkbenchPage();
        }
    });
    *nav = list;
    *stack = pages;
    return host;
}

void MainWindow::addWorkbenchPage(const int stage, QListWidget *nav, QStackedWidget *stack,
                                  const EditorSection section, const QString &label, QWidget *page) {
    auto *item = new QListWidgetItem(label, nav);
    item->setData(Qt::UserRole, static_cast<int>(section));
    item->setData(sectionBaseLabelRole, label);
    stack->addWidget(page);
    SectionLocation location;
    location.stage = stage;
    location.page = nav->count() - 1;
    location.nav = nav;
    location.stack = stack;
    location.item = item;
    sectionLocations_.insert(static_cast<int>(section), location);
}

void MainWindow::setSectionVisible(const EditorSection section, const bool visible) {
    const auto location = sectionLocations_.value(static_cast<int>(section));
    if (location.item != nullptr) location.item->setHidden(!visible);
}

void MainWindow::updateSectionReviewMarkers() {
    if (currentRelease() == nullptr) return;
    const auto &release = *currentRelease();
    const auto mark = [this](const EditorSection section, const bool needsReview) {
        const auto location = sectionLocations_.value(static_cast<int>(section));
        if (location.item == nullptr) return;
        const auto base = location.item->data(sectionBaseLabelRole).toString();
        if (base.isEmpty()) return;
        location.item->setText(needsReview ? base + QStringLiteral("  ⚠") : base);
        if (needsReview) {
            location.item->setForeground(QColor(0xe5, 0xb9, 0x3d));
        } else {
            location.item->setData(Qt::ForegroundRole, QVariant());
        }
    };
    const bool commandReview = std::any_of(
        release.installMapping.launchers.cbegin(), release.installMapping.launchers.cend(),
        [](const auto &launcher) { return launcher.enabled && launcher.missing; }) ||
        (release.sourceType == SourcePackageType::Archive &&
         std::any_of(release.installMapping.desktopEntries.cbegin(),
                     release.installMapping.desktopEntries.cend(),
                     [&](const auto &desktop) {
                         if (!desktop.enabled) return false;
                         const auto command = desktopEntryCommand(desktop.contents);
                         if (command.isEmpty()) return false;
                         return std::none_of(
                             release.installMapping.launchers.cbegin(),
                             release.installMapping.launchers.cend(),
                             [&](const auto &launcher) {
                                 return launcher.enabled && !launcher.missing &&
                                        launcher.commandName.compare(
                                            command, Qt::CaseInsensitive) == 0;
                             });
                     }));
    const bool desktopReview = std::any_of(
        release.installMapping.desktopEntries.cbegin(),
        release.installMapping.desktopEntries.cend(),
        [](const auto &desktop) { return desktop.enabled && desktop.missing; });
    const bool dependencyReview = std::any_of(
        release.dependencies.cbegin(), release.dependencies.cend(),
        [this](const auto &dependency) {
            return dependency.status == MappingStatus::Unresolved ||
                   repositoryPackageUnavailable(dependency, repositoryDependencyAvailability_);
        });
    const bool lifecycleReview = !release.lifecycleScript.contents.isEmpty() &&
        (!release.lifecycleScript.validationPassed ||
         release.lifecycleScript.requiresAcknowledgement());
    mark(EditorSection::SourceContents, pendingPayloadReviews(release) > 0);
    mark(EditorSection::SourceScripts, pendingScriptFindings(release) > 0);
    mark(EditorSection::ConfigDependencies, dependencyReview);
    mark(EditorSection::ConfigScripts, pendingScriptFindings(release) > 0 || lifecycleReview);
    mark(EditorSection::ConfigCommands, commandReview);
    mark(EditorSection::ConfigAppRun, release.installMapping.appRun.requiresReview());
    mark(EditorSection::ConfigDesktopEntries, desktopReview);
    mark(EditorSection::ConfigIcon, release.installMapping.icon.missing);
}

void MainWindow::updateWorkbenchStageChrome() {
    if (stageTabs_ == nullptr) return;
    const auto stage = stageTabs_->currentIndex();
    if (currentRelease() != nullptr) {
        const auto sourceTitle = sourcePackageTypeTitle(currentRelease()->sourceType);
        if (sourceTypeBadge_ != nullptr) {
            sourceTypeBadge_->setText(sourceTitle);
            sourceTypeBadge_->setVisible(true);
        }
        stageTabs_->setTabText(0, sourceTitle);
        stageTabs_->setTabText(2, QStringLiteral("Pacman Package"));
        stageTabs_->setTabToolTip(0, QStringLiteral("Imported %1: inspect metadata, original scripts, and payload")
                                       .arg(sourceTitle.toLower()));
        stageTabs_->setTabToolTip(1, currentRelease()->pkgbuildManuallyModified
            ? QStringLiteral("Custom PKGBUILD for this release. Guided configuration is ignored.")
            : QStringLiteral("Guided settings generate the PKGBUILD for this release."));
    }
    updateConfigurationModeChrome();
    if (workbenchSubtitle_ == nullptr || currentRelease() == nullptr) return;
    const auto version = currentRelease()->debian.version;
    const auto sourceTitle = sourcePackageTypeTitle(currentRelease()->sourceType);
    if (stage == 0) {
        workbenchSubtitle_->setText(
            QStringLiteral("%1 for release %2 — inspect only. Nothing here is executed or installed.")
                .arg(sourceTitle, version));
    } else if (stage == 1) {
        workbenchSubtitle_->setText(
            currentRelease()->pkgbuildManuallyModified
                ? QStringLiteral("Custom configuration for release %1. Edit the PKGBUILD directly; Guided pages are hidden.")
                      .arg(version)
                : QStringLiteral("Guided configuration for release %1. These settings generate the PKGBUILD on Pacman Package.")
                      .arg(version));
    } else {
        workbenchSubtitle_->setText(
            currentRelease()->pkgbuildManuallyModified
                ? QStringLiteral("Pacman package for release %1. The Custom PKGBUILD is what makepkg will run.")
                      .arg(version)
                : QStringLiteral("Pacman package for release %1: install layout, generated PKGBUILD, and build.")
                      .arg(version));
    }
}

void MainWindow::updateConfigurationModeChrome() {
    if (guidedModeButton_ == nullptr || customModeButton_ == nullptr) return;
    const bool custom = currentRelease() != nullptr && currentRelease()->pkgbuildManuallyModified;
    QSignalBlocker guidedBlock(guidedModeButton_);
    QSignalBlocker customBlock(customModeButton_);
    guidedModeButton_->setChecked(!custom);
    customModeButton_->setChecked(custom);
}

void MainWindow::setConfigurationMode(const bool custom) {
    if (!project_ || currentRelease() == nullptr) {
        updateConfigurationModeChrome();
        return;
    }
    const bool already = currentRelease()->pkgbuildManuallyModified == custom;
    if (pkgbuildEditor_ != nullptr && pkgbuildEditor_->document()->isModified()) {
        QString error;
        if (!store_.saveCustomPkgbuild(*project_, *currentRelease(), pkgbuildEditor_->toPlainText(), &error)) {
            QMessageBox::critical(this, QStringLiteral("Could not save Custom PKGBUILD"), error);
            updateConfigurationModeChrome();
            return;
        }
        pkgbuildEditor_->document()->setModified(false);
        projectCache_.insert(project_->id, *project_);
    }
    if (currentRelease()->pkgbuildManuallyModified != custom) {
        QString error;
        const bool ok = custom
            ? store_.activateCustomPkgbuild(*project_, *currentRelease(), &error)
            : store_.activateGuidedPkgbuild(*project_, *currentRelease(), &error);
        if (!ok) {
            QMessageBox::critical(this, QStringLiteral("Could not switch PKGBUILD mode"), error);
            updateConfigurationModeChrome();
            return;
        }
        projectCache_.insert(project_->id, *project_);
    }
    configureEditorProfile();
    updateConfigurationModeChrome();
    if (already) {
        populatePkgbuild();
        return;
    }
    if (custom) {
        selectSection(EditorSection::ConfigPkgbuild);
    } else if (configNav_ != nullptr) {
        for (int row = 0; row < configNav_->count(); ++row) {
            const auto *item = configNav_->item(row);
            if (item != nullptr && !item->isHidden() &&
                static_cast<EditorSection>(item->data(Qt::UserRole).toInt()) != EditorSection::ConfigPkgbuild) {
                selectSection(static_cast<EditorSection>(item->data(Qt::UserRole).toInt()));
                break;
            }
        }
    }
    populatePkgbuild();
}

MainWindow::EditorSection MainWindow::currentSection() const {
    if (stageTabs_ == nullptr) return EditorSection::SourceOverview;
    const auto stage = stageTabs_->currentIndex();
    QListWidget *nav = stage == 1 ? configNav_ : stage == 2 ? resultNav_ : sourceNav_;
    if (nav == nullptr || nav->currentItem() == nullptr) return EditorSection::SourceOverview;
    return static_cast<EditorSection>(nav->currentItem()->data(Qt::UserRole).toInt());
}

QString MainWindow::sectionTitle(const EditorSection section) const {
    const auto location = sectionLocations_.value(static_cast<int>(section));
    if (location.item != nullptr) return location.item->text();
    return QStringLiteral("Package setup");
}

bool MainWindow::isSectionActive(const EditorSection section) const {
    if (stageTabs_ == nullptr || rightStack_ == nullptr || rightStack_->currentIndex() != 1) {
        return false;
    }
    const auto location = sectionLocations_.value(static_cast<int>(section));
    return location.nav != nullptr && stageTabs_->currentIndex() == location.stage &&
           location.nav->currentRow() == location.page &&
           (location.item == nullptr || !location.item->isHidden());
}

void MainWindow::placeUpdatesEditor() {
    if (updatesEditor_ == nullptr) return;
    QWidget *host = (rightStack_ != nullptr && rightStack_->currentIndex() == 1)
                        ? configUpdatesHost_
                        : dashboardUpdatesHost_;
    if (host == nullptr || updatesEditor_->parentWidget() == host) return;
    if (auto *layout = host->layout()) layout->addWidget(updatesEditor_);
}

void MainWindow::syncUpdateCheckButtons() {
    const bool busy = aptUpdateService_.isRunning() || rpmUpdateService_.isRunning() ||
                      githubUpdateService_.isRunning() || debDownloadService_.isRunning() ||
                      importThread_ != nullptr;
    bool allowed = false;
    if (!busy && project_ && updateEditorRelease() != nullptr && updateStrategy_ != nullptr) {
        const auto index = updateStrategy_->currentIndex();
        allowed = index == 2 || index == 3 || index == 4;
    }
    if (updateCheckButton_ != nullptr) updateCheckButton_->setEnabled(allowed);
    if (historyCheckUpdatesButton_ != nullptr) historyCheckUpdatesButton_->setEnabled(allowed);
}

void MainWindow::updateDashboardActions() {
    const bool installed = project_ && !project_->installedVersion.isEmpty();
    if (uninstallButton_ != nullptr) {
        uninstallButton_->setVisible(installed);
        uninstallButton_->setEnabled(installed && !installService_.isRunning());
    }
    const auto *tracker = project_ ? project_->activeTrackingRelease() : nullptr;
    if (editConfigurationButton_ != nullptr) {
        editConfigurationButton_->setVisible(tracker != nullptr &&
                                             tracker->state != ReleaseState::Discovered);
    }
    if (projectPrimaryButton_ == nullptr) {
        syncUpdateCheckButtons();
        return;
    }
    if (tracker == nullptr) {
        projectPrimaryButton_->setVisible(false);
        syncUpdateCheckButtons();
        return;
    }
    const bool preparing = tracker->id == preparingReleaseId_ && project_->id == preparingProjectId_;
    const bool hasPackage = !retainedPackagePath(store_, *tracker).isEmpty();
    const bool installedHere = tracker->id == project_->installedReleaseId;
    if (preparing) {
        projectPrimaryButton_->setVisible(true);
        projectPrimaryButton_->setText(QStringLiteral("Show Progress"));
        projectPrimaryButton_->setEnabled(true);
    } else if (hasPackage && !installedHere) {
        projectPrimaryButton_->setVisible(true);
        projectPrimaryButton_->setText(project_->installedVersion.isEmpty()
                                           ? QStringLiteral("Install")
                                           : QStringLiteral("Install Update"));
        projectPrimaryButton_->setEnabled(!installService_.isRunning());
    } else if (tracker->state == ReleaseState::Discovered) {
        projectPrimaryButton_->setVisible(true);
        projectPrimaryButton_->setText(QStringLiteral("Download & Prepare"));
        projectPrimaryButton_->setEnabled(!debDownloadService_.isRunning() &&
                                          importThread_ == nullptr);
    } else {
        projectPrimaryButton_->setVisible(false);
    }
    syncUpdateCheckButtons();
}

void MainWindow::handleProjectPrimaryAction() {
    if (!project_) return;
    auto *tracker = project_->activeTrackingRelease();
    if (tracker == nullptr) return;
    const bool preparing = tracker->id == preparingReleaseId_ && project_->id == preparingProjectId_;
    if (preparing) {
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
    const bool hasPackage = !retainedPackagePath(store_, *tracker).isEmpty();
    const bool installedHere = tracker->id == project_->installedReleaseId;
    if (hasPackage && !installedHere) {
        currentReleaseId_ = tracker->id;
        startInstall();
        return;
    }
    if (tracker->state == ReleaseState::Discovered) {
        beginReleasePreparation(tracker->id, true);
    }
}

void MainWindow::editPackageConfiguration() {
    if (!project_) return;
    const auto *tracker = project_->activeTrackingRelease();
    if (tracker == nullptr || tracker->state == ReleaseState::Discovered) return;
    showReleaseWorkbenchAtFirstAttention(tracker->id);
}

void MainWindow::showProjectDashboard() {
    if (rightStack_ == nullptr) return;
    if (projectSidebar_ != nullptr) projectSidebar_->show();
    rightStack_->setCurrentIndex(0);
    placeUpdatesEditor();
    if (projectTabs_ != nullptr) projectTabs_->setCurrentIndex(0);
    if (project_) refreshCurrentProject();
}

void MainWindow::showReleaseWorkbench(const QString &releaseId) {
    if (!project_) return;
    const auto *release = project_->release(releaseId);
    if (release == nullptr || release->state == ReleaseState::Discovered) return;
    currentReleaseId_ = releaseId;
    if (projectSidebar_ != nullptr) projectSidebar_->hide();
    rightStack_->setCurrentIndex(1);
    placeUpdatesEditor();
    configureEditorProfile();
    selectSection(EditorSection::SourceOverview);
    refreshCurrentProject();
    workbenchTitle_->setText(
        QStringLiteral("<h2>Set Up %1 %2</h2>")
            .arg(project_->displayName.toHtmlEscaped(), release->debian.version.toHtmlEscaped()));
    updateWorkbenchStageChrome();
}

int MainWindow::sectionIndex(const EditorSection section) const {
    return sectionLocations_.value(static_cast<int>(section)).page;
}

void MainWindow::selectSection(const EditorSection section) {
    auto location = sectionLocations_.value(static_cast<int>(section));
    if (location.item != nullptr && location.item->isHidden()) {
        location = sectionLocations_.value(static_cast<int>(EditorSection::SourceOverview));
    }
    if (location.nav == nullptr || location.stack == nullptr || location.item == nullptr ||
        location.item->isHidden() || stageTabs_ == nullptr) {
        return;
    }
    QSignalBlocker stageBlocker(stageTabs_);
    QSignalBlocker navBlocker(location.nav);
    stageTabs_->setCurrentIndex(location.stage);
    location.nav->setCurrentRow(location.page);
    location.stack->setCurrentIndex(location.page);
    updateWorkbenchStageChrome();
    if (rightStack_ != nullptr && rightStack_->currentIndex() == 1) {
        populateCurrentWorkbenchPage();
    }
}

void MainWindow::configureEditorProfile() {
    if (stageTabs_ == nullptr || currentRelease() == nullptr) return;
    const auto type = currentRelease()->sourceType;
    if (sourceTypeBadge_ != nullptr) {
        sourceTypeBadge_->setText(sourcePackageTypeTitle(type));
        sourceTypeBadge_->setVisible(true);
    }
    const bool standalone = type == SourcePackageType::ElfBinary;
    const bool packageContainer = type == SourcePackageType::Debian ||
                                  type == SourcePackageType::Rpm ||
                                  type == SourcePackageType::ArchPackage;
    const bool custom = currentRelease()->pkgbuildManuallyModified;
    const bool hasScripts = packageContainer ||
                            !currentRelease()->maintainerScripts.isEmpty() ||
                            !currentRelease()->scriptFindings.isEmpty();
    const bool hasLifecycle = hasScripts || !currentRelease()->lifecycleScript.contents.isEmpty();
    setSectionVisible(EditorSection::SourceOverview, true);
    setSectionVisible(EditorSection::SourceMetadata, true);
    setSectionVisible(EditorSection::SourceScripts, hasScripts);
    setSectionVisible(EditorSection::SourceContents, !standalone);
    setSectionVisible(EditorSection::ConfigPkgbuild, custom);
    setSectionVisible(EditorSection::ConfigLayout,
                      !custom && (type == SourcePackageType::Archive || standalone));
    setSectionVisible(EditorSection::ConfigDependencies,
                      !custom && (packageContainer || !currentRelease()->dependencies.isEmpty()));
    setSectionVisible(EditorSection::ConfigScripts, hasScripts || hasLifecycle);
    setSectionVisible(EditorSection::ConfigCommands,
                      !custom && (!currentRelease()->installMapping.launchers.isEmpty() ||
                          type == SourcePackageType::Archive || standalone ||
                          type == SourcePackageType::Debian ||
                          type == SourcePackageType::Rpm));
    setSectionVisible(EditorSection::ConfigAppRun,
                      !custom && type == SourcePackageType::AppImage);
    setSectionVisible(EditorSection::ConfigDesktopEntries, !custom);
    setSectionVisible(EditorSection::ConfigIcon, !custom);
    setSectionVisible(EditorSection::ConfigUpdates, true);
    setSectionVisible(EditorSection::ResultInstallPlan, true);
    setSectionVisible(EditorSection::ResultPkgbuild, true);
    setSectionVisible(EditorSection::ResultBuild, true);
    updateSectionReviewMarkers();

    const auto showStageIfNeeded = [this](QListWidget *nav, const int stage) {
        if (nav == nullptr || stageTabs_ == nullptr) return;
        bool any = false;
        for (int row = 0; row < nav->count(); ++row) {
            const auto *item = nav->item(row);
            if (item != nullptr && !item->isHidden()) any = true;
        }
        stageTabs_->setTabVisible(stage, any);
        if (any && (nav->currentItem() == nullptr || nav->currentItem()->isHidden())) {
            for (int row = 0; row < nav->count(); ++row) {
                if (nav->item(row) != nullptr && !nav->item(row)->isHidden()) {
                    nav->setCurrentRow(row);
                    break;
                }
            }
        }
    };
    showStageIfNeeded(sourceNav_, 0);
    showStageIfNeeded(configNav_, 1);
    showStageIfNeeded(resultNav_, 2);
    updateWorkbenchStageChrome();
}

void MainWindow::showReleaseWorkbenchAtFirstAttention(const QString &releaseId) {
    if (!project_) return;
    const auto *release = project_->release(releaseId);
    if (release == nullptr || release->state == ReleaseState::Discovered) return;
    showReleaseWorkbench(releaseId);

    auto attention = EditorSection::ResultBuild;
    const bool packageMappingNeedsAttention =
        ((release->sourceType == SourcePackageType::Archive ||
          release->sourceType == SourcePackageType::AppImage) &&
         release->installMapping.archiveLayout == ArchiveLayout::OptBundle &&
         release->installMapping.optDirectory.trimmed().isEmpty()) ||
        (release->sourceType == SourcePackageType::ElfBinary &&
         release->installMapping.binaryDestination.trimmed().isEmpty());
    const bool dependencyReview = std::any_of(
        release->dependencies.cbegin(), release->dependencies.cend(),
        [this](const auto &dependency) {
            return dependency.status == MappingStatus::Unresolved ||
                   repositoryPackageUnavailable(dependency,
                                                repositoryDependencyAvailability_);
        });
    const bool lifecycleReview = !release->lifecycleScript.contents.isEmpty() &&
        (!release->lifecycleScript.validationPassed ||
         release->lifecycleScript.requiresAcknowledgement());
    const bool commandReview = std::any_of(
        release->installMapping.launchers.cbegin(), release->installMapping.launchers.cend(),
        [](const auto &launcher) { return launcher.enabled && launcher.missing; }) ||
        (release->sourceType == SourcePackageType::Archive &&
         std::any_of(release->installMapping.desktopEntries.cbegin(),
                     release->installMapping.desktopEntries.cend(),
                     [&](const auto &desktop) {
                         if (!desktop.enabled) return false;
                         const auto command = desktopEntryCommand(desktop.contents);
                         if (command.isEmpty()) return false;
                         return std::none_of(
                             release->installMapping.launchers.cbegin(),
                             release->installMapping.launchers.cend(),
                             [&](const auto &launcher) {
                                 return launcher.enabled && !launcher.missing &&
                                        launcher.commandName.compare(
                                            command, Qt::CaseInsensitive) == 0;
                             });
                     }));
    const bool desktopReview = std::any_of(
        release->installMapping.desktopEntries.cbegin(),
        release->installMapping.desktopEntries.cend(),
        [](const auto &desktop) { return desktop.enabled && desktop.missing; });
    const bool custom = release->pkgbuildManuallyModified;
    if (packageMappingNeedsAttention && !custom) {
        attention = release->sourceType == SourcePackageType::AppImage
                        ? EditorSection::ResultInstallPlan
                        : EditorSection::ConfigLayout;
    }
    else if (dependencyReview && !custom) attention = EditorSection::ConfigDependencies;
    else if (pendingScriptFindings(*release) > 0) attention = EditorSection::ConfigScripts;
    else if (lifecycleReview) attention = EditorSection::ConfigScripts;
    else if (pendingPayloadReviews(*release) > 0) attention = EditorSection::SourceContents;
    else if (release->installMapping.appRun.requiresReview() && !custom) {
        attention = EditorSection::ConfigAppRun;
    }
    else if (commandReview && !custom) attention = EditorSection::ConfigCommands;
    else if (desktopReview && !custom) attention = EditorSection::ConfigDesktopEntries;
    else if (release->installMapping.icon.missing && !custom) attention = EditorSection::ConfigIcon;
    else if (custom) attention = EditorSection::ConfigPkgbuild;
    selectSection(attention);
}

void MainWindow::selectDashboardRelease(const QString &releaseId) {
    if (releaseTable_ == nullptr) return;
    if (projectSidebar_ != nullptr) projectSidebar_->show();
    rightStack_->setCurrentIndex(0);
    placeUpdatesEditor();
    if (projectTabs_ != nullptr) {
        QWidget *historyPage = releaseTable_;
        while (historyPage != nullptr && projectTabs_->indexOf(historyPage) < 0) {
            historyPage = historyPage->parentWidget();
        }
        if (historyPage != nullptr) projectTabs_->setCurrentWidget(historyPage);
    }
    for (int row = 0; row < releaseTable_->rowCount(); ++row) {
        const auto *item = releaseTable_->item(row, 0);
        if (item != nullptr && item->data(Qt::UserRole).toString() == releaseId) {
            releaseTable_->selectRow(row);
            releaseTable_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            break;
        }
    }
}

void MainWindow::updatePreparationIndicators() {
    static const QStringList frames{QStringLiteral("⠋"), QStringLiteral("⠙"),
                                    QStringLiteral("⠹"), QStringLiteral("⠸")};
    const auto frame = frames.at(preparationSpinnerFrame_ % frames.size());
    QString preparingId = preparingProjectId_;
    QString phase = preparationPhase_;
    qint64 received = preparationBytesReceived_;
    qint64 total = preparationBytesTotal_;
    if (preparingId.isEmpty()) {
        const auto updateState = BackgroundUpdateStateStore::load();
        preparingId = updateState.preparingProjectId;
        phase = updateState.preparationPhase;
        received = updateState.preparationBytesReceived;
        total = updateState.preparationBytesTotal;
    }
    const auto activityText = preparingId.isEmpty()
        ? QString{}
        : downloadActivityText(phase, received, total);
    for (int row = 0; projectList_ != nullptr && row < projectList_->count(); ++row) {
        auto *item = projectList_->item(row);
        const auto projectId = item->data(Qt::UserRole).toString();
        const auto currentActivity = item->data(projectActivityRole).toString();
        if (projectId == preparingId && !preparingId.isEmpty()) {
            if (currentActivity != activityText) item->setData(projectActivityRole, activityText);
            projectList_->viewport()->update(projectList_->visualItemRect(item));
        } else if (!currentActivity.isEmpty()) {
            item->setData(projectActivityRole, QString{});
            projectList_->viewport()->update(projectList_->visualItemRect(item));
        }
    }
    if (!project_ || project_->id != preparingProjectId_ || releaseTable_ == nullptr) return;
    for (int row = 0; row < releaseTable_->rowCount(); ++row) {
        auto *versionItem = releaseTable_->item(row, 0);
        if (versionItem == nullptr ||
            versionItem->data(Qt::UserRole).toString() != preparingReleaseId_) continue;
        const auto tablePhase = preparationPhase_.isEmpty() ? QStringLiteral("Downloading")
                                                            : preparationPhase_;
        if (auto *status = releaseTable_->item(row, 1); status != nullptr) {
            status->setText(QStringLiteral("%1 %2").arg(frame, tablePhase));
        }
        if (auto *review = releaseTable_->item(row, 4); review != nullptr) {
            review->setText(preparationBytesTotal_ > 0 && tablePhase == QStringLiteral("Downloading")
                ? QStringLiteral("%1 / %2 MiB")
                      .arg(preparationBytesReceived_ / (1024 * 1024))
                      .arg(preparationBytesTotal_ / (1024 * 1024))
                : QStringLiteral("Processing…"));
        }
        break;
    }
}

void MainWindow::updateUpdateCheckIndicators() {
    if (projectList_ == nullptr) return;
    const auto updateState = BackgroundUpdateStateStore::load();
    QString checkingId = updateState.checkingProjectId;
    QString checkingName = updateState.checkingProjectName;
    const bool inProcess = aptUpdateService_.isRunning() || rpmUpdateService_.isRunning() ||
                           githubUpdateService_.isRunning();
    if (checkingId.isEmpty() && inProcess && project_) {
        checkingId = project_->id;
        checkingName = project_->displayName.isEmpty() ? project_->archPackageName
                                                       : project_->displayName;
    }
    const bool checkBusy = updateState.checking || inProcess;
    const bool checking = checkBusy && !checkingId.isEmpty();
    QString preparingId = preparingProjectId_;
    QString preparingName;
    QString phase = preparationPhase_;
    qint64 received = preparationBytesReceived_;
    qint64 total = preparationBytesTotal_;
    if (preparingId.isEmpty()) {
        preparingId = updateState.preparingProjectId;
        preparingName = updateState.preparingProjectName;
        phase = updateState.preparationPhase;
        received = updateState.preparationBytesReceived;
        total = updateState.preparationBytesTotal;
    } else {
        preparingName = displayNameForProject(preparingId);
    }
    const bool prepareBusy = !preparingId.isEmpty();
    projectList_->setProperty("pacsmithSpinnerFrame", preparationSpinnerFrame_);
    projectList_->viewport()->setProperty("pacsmithSpinnerFrame", preparationSpinnerFrame_);
    for (int row = 0; row < projectList_->count(); ++row) {
        auto *item = projectList_->item(row);
        const auto projectId = item->data(Qt::UserRole).toString();
        const bool itemPreparing = prepareBusy && projectId == preparingId;
        const bool itemChecking = checking && projectId == checkingId && !itemPreparing;
        const bool itemBusy = itemPreparing || itemChecking;
        if (item->data(projectCheckingRole).toBool() != itemBusy) {
            item->setData(projectCheckingRole, itemBusy);
        }
        if (itemBusy) projectList_->viewport()->update(projectList_->visualItemRect(item));
    }
    if (prepareBusy && canShowUpdateCheckStatus()) {
        updateCheckStatusActive_ = true;
        statusBar()->showMessage(downloadStatusText(preparingName, phase, received, total));
    } else if (checkBusy && canShowUpdateCheckStatus()) {
        updateCheckStatusActive_ = true;
        statusBar()->showMessage(checkingName.isEmpty()
                                     ? QStringLiteral("Checking for updates…")
                                     : QStringLiteral("Checking %1 for updates…").arg(checkingName));
    } else if (!checkBusy && !prepareBusy && updateCheckStatusActive_) {
        updateCheckStatusActive_ = false;
        if (canShowUpdateCheckStatus()) {
            statusBar()->showMessage(updateState.message.isEmpty()
                                         ? QStringLiteral("Update check finished")
                                         : updateState.message,
                                     8000);
        }
    }
    if (checkBusy || prepareBusy) projectList_->viewport()->update();
}

void MainWindow::syncActivityTimer() {
    if (preparationSpinnerTimer_ == nullptr) return;
    const bool busy = !preparingProjectId_.isEmpty() || listActivityInProgress();
    if (busy) {
        if (!preparationSpinnerTimer_->isActive()) preparationSpinnerTimer_->start();
        return;
    }
    if (preparationSpinnerTimer_->isActive()) preparationSpinnerTimer_->stop();
}

bool MainWindow::updateCheckInProgress() const {
    if (aptUpdateService_.isRunning() || rpmUpdateService_.isRunning() ||
        githubUpdateService_.isRunning()) {
        return true;
    }
    return BackgroundUpdateStateStore::load().checking;
}

bool MainWindow::listActivityInProgress() const {
    if (!preparingProjectId_.isEmpty()) return true;
    if (aptUpdateService_.isRunning() || rpmUpdateService_.isRunning() ||
        githubUpdateService_.isRunning()) {
        return true;
    }
    const auto updateState = BackgroundUpdateStateStore::load();
    return updateState.checking || !updateState.preparingProjectId.isEmpty();
}

bool MainWindow::canShowUpdateCheckStatus() const {
    return importThread_ == nullptr && importProgress_ == nullptr &&
           !buildService_.isRunning() && !installService_.isRunning();
}

void MainWindow::publishUpdateCheckActivity(const bool running, const QString &projectId,
                                            const QString &projectName) {
    auto updateState = BackgroundUpdateStateStore::load();
    if (running) {
        updateState.checking = true;
        updateState.checkingProjectId = projectId;
        updateState.checkingProjectName = projectName;
        updateState.message = projectName.isEmpty()
            ? QStringLiteral("Checking for updates")
            : QStringLiteral("Checking %1 for updates").arg(projectName);
        static_cast<void>(BackgroundUpdateStateStore::save(updateState));
        syncActivityTimer();
        updateUpdateCheckIndicators();
        return;
    }
    if (!projectId.isEmpty() && !updateState.checkingProjectId.isEmpty() &&
        updateState.checkingProjectId != projectId) {
        return;
    }
    updateState.checking = false;
    updateState.checkingProjectId.clear();
    updateState.checkingProjectName.clear();
    static_cast<void>(BackgroundUpdateStateStore::save(updateState));
    updateUpdateCheckIndicators();
    syncActivityTimer();
}

QString MainWindow::displayNameForProject(const QString &projectId) const {
    if (project_ && project_->id == projectId) {
        return project_->displayName.isEmpty() ? project_->archPackageName : project_->displayName;
    }
    const auto cached = projectCache_.constFind(projectId);
    if (cached != projectCache_.cend()) {
        return cached->displayName.isEmpty() ? cached->archPackageName : cached->displayName;
    }
    return projectId;
}

void MainWindow::publishPreparationActivity() {
    if (preparingProjectId_.isEmpty()) return;
    auto updateState = BackgroundUpdateStateStore::load();
    updateState.preparingProjectId = preparingProjectId_;
    updateState.preparingProjectName = displayNameForProject(preparingProjectId_);
    updateState.preparationPhase = preparationPhase_;
    updateState.preparationBytesReceived = preparationBytesReceived_;
    updateState.preparationBytesTotal = preparationBytesTotal_;
    updateState.message = downloadStatusText(updateState.preparingProjectName, preparationPhase_,
                                             preparationBytesReceived_, preparationBytesTotal_);
    static_cast<void>(BackgroundUpdateStateStore::save(updateState));
}

void MainWindow::clearPublishedPreparationActivity(const QString &projectId) {
    auto updateState = BackgroundUpdateStateStore::load();
    if (!projectId.isEmpty() && !updateState.preparingProjectId.isEmpty() &&
        updateState.preparingProjectId != projectId) {
        return;
    }
    if (updateState.preparingProjectId.isEmpty() && updateState.preparingProjectName.isEmpty()) {
        return;
    }
    updateState.preparingProjectId.clear();
    updateState.preparingProjectName.clear();
    updateState.preparationPhase.clear();
    updateState.preparationBytesReceived = 0;
    updateState.preparationBytesTotal = -1;
    static_cast<void>(BackgroundUpdateStateStore::save(updateState));
}

void MainWindow::startListDownloadActivity(const QString &projectId, const QString &releaseId) {
    if (projectId.isEmpty()) return;
    const bool known = (project_ && project_->id == projectId) || projectCache_.contains(projectId);
    if (!known && !store_.load(projectId, nullptr)) return;
    preparingProjectId_ = projectId;
    preparingReleaseId_ = releaseId;
    preparationPhase_ = QStringLiteral("Downloading");
    preparationBytesReceived_ = 0;
    preparationBytesTotal_ = -1;
    lastPreparationPublish_.invalidate();
    publishPreparationActivity();
    syncActivityTimer();
    updatePreparationIndicators();
    updateUpdateCheckIndicators();
}

void MainWindow::noteBackgroundCheckStarted() {
    syncActivityTimer();
    updatePreparationIndicators();
    updateUpdateCheckIndicators();
}

void MainWindow::resetPreparationState() {
    if (downloadProgress_ != nullptr) {
        downloadProgress_->close();
        downloadProgress_->deleteLater();
        downloadProgress_ = nullptr;
    }
    const auto projectId = preparingProjectId_;
    if (!projectId.isEmpty()) clearPublishedPreparationActivity(projectId);
    preparingProjectId_.clear();
    preparingReleaseId_.clear();
    preparationPhase_.clear();
    preparationBytesReceived_ = 0;
    preparationBytesTotal_ = -1;
    lastPreparationPublish_.invalidate();
    if (!listActivityInProgress()) preparationSpinnerFrame_ = 0;
    syncActivityTimer();
    updatePreparationIndicators();
    updateUpdateCheckIndicators();
}

void MainWindow::activateExistingSession(const QString &importPath) {
    if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
    if (auto *handle = windowHandle()) handle->requestActivate();
    if (!importPath.isEmpty()) importPackage(importPath);
    syncActivityTimer();
    updateUpdateCheckIndicators();
}

void MainWindow::setKeepRunningInTray(const bool enabled) {
    keepRunningInTray_ = enabled;
}

void MainWindow::reloadVisibleProjects() {
    const auto projectId = project_ ? project_->id : QString{};
    refreshProjectList(projectId);
    if (!projectId.isEmpty() && (!project_ || project_->id != projectId)) loadProject(projectId);
    if (project_) refreshCurrentProject();
    syncActivityTimer();
}

void MainWindow::importPackage(const QString &path) {
    const QUrl remote(path);
    if (remote.isValid() && remote.scheme() == QStringLiteral("https") &&
        remote.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) == 0) {
        beginGitHubImport(remote);
        return;
    }
    if (remote.isValid() && remote.scheme() == QStringLiteral("https")) {
        if (debDownloadService_.isRunning()) return;
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
        debDownloadService_.start(remote, {},
                                  std::filesystem::path(target.toUtf8().constData()));
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
    auto *worker = new ImportWorker(store_.projectsRoot(), QFileInfo(path).absoluteFilePath(),
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
        const auto loaded = store_.load(projectId);
        statusBar()->showMessage(
            loaded ? QStringLiteral("Imported to %1").arg(projectDirectory(store_, *loaded))
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
    auto *service = new GitHubUpdateService(this);
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
    QString token;
    const auto credentialSource = aiSettings_.credentialSources.value(
        QStringLiteral("github"), CredentialSource::Keyring);
    if (credentialSource != CredentialSource::Age) {
        token = credentialStore_.load(QStringLiteral("github"), credentialSource, nullptr).value_or(QString{});
    }
    service->start(probe, token, requestedTag);
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
    auto *service = new GitHubUpdateService(this);
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
    QString token;
    const auto credentialSource = aiSettings_.credentialSources.value(
        QStringLiteral("github"), CredentialSource::Keyring);
    if (credentialSource != CredentialSource::Age) {
        token = credentialStore_.load(QStringLiteral("github"), credentialSource, nullptr).value_or(QString{});
    }
    service->start(probe, token, requestedTag);
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

    const auto providerName = aiProviderName(aiSettings_.provider);
    const auto defaultSource = aiSettings_.provider == AiProviderKind::ChatGpt
                                   ? CredentialSource::Keyring
                                   : CredentialSource::Environment;
    const auto source = aiSettings_.credentialSources.value(providerName, defaultSource);
    if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() &&
        !unlockAgeCredentials()) return;
    QString credentialError;
    auto credential = credentialStore_.load(providerName, source, &credentialError);
    if (!credential) {
        QMessageBox::critical(dialog, QStringLiteral("AI credential unavailable"), credentialError);
        return;
    }

    auto *service = new AiAnalysisService(dialog);
    auto *progress = new AiProgressDialog(aiSettings_, dialog);
    progress->setWindowTitle(QStringLiteral("Generating GitHub Asset Rule"));
    button->setEnabled(false);
    progress->show();
    connect(service, &AiAnalysisService::progressChanged, progress,
            &AiProgressDialog::setStatus);
    connect(service, &AiAnalysisService::activityChanged, progress,
            &AiProgressDialog::appendActivity);
    connect(service, &AiAnalysisService::responseProgress, progress,
            &AiProgressDialog::setResponseProgress);
    connect(service, &AiAnalysisService::requestAvailable, progress,
            &AiProgressDialog::setRequest);
    connect(service, &AiAnalysisService::responseDelta, progress,
            &AiProgressDialog::appendResponseDelta);
    connect(service, &AiAnalysisService::credentialUpdated, dialog,
            [this, providerName, source](const QString &serialized) {
        QString error;
        if (!credentialStore_.store(providerName, source, serialized,
                                    QString{},
                                    &error)) {
            statusBar()->showMessage(
                QStringLiteral("Could not persist refreshed AI credentials: %1").arg(error),
                10000);
        }
    });
    connect(progress, &QDialog::rejected, service, [service, button] {
        service->setProperty("pacsmith-canceled", true);
        service->cancel();
        button->setEnabled(true);
    });
    connect(service, &AiAnalysisService::finished, dialog,
            [editor, status, button, dialog, service, progress, assets,
             preferredAsset](const AiResolution &resolution) {
        progress->close();
        progress->deleteLater();
        button->setEnabled(true);
        const bool canceled = service->property("pacsmith-canceled").toBool();
        service->deleteLater();
        if (canceled) return;
        if (!resolution.success) {
            showAiErrorDialog(dialog, resolution);
            return;
        }
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
    service->startGitHubAssetRule(probe, assets, preferredAsset, aiSettings_, *credential);
    credential->fill(QChar::Null);
}

void MainWindow::downloadGitHubAsset(const PackageRelease &probe,
                                     const UpdateCheckResult &result) {
    if (debDownloadService_.isRunning()) return;
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

    debDownloadService_.start(QUrl(result.downloadUrl), result.sha256,
                              std::filesystem::path(target.toUtf8().constData()));
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (importThread_ != nullptr && importThread_->isRunning()) {
        QMessageBox::information(this, QStringLiteral("Import in progress"),
                                 QStringLiteral("Wait for the current package import to finish before closing PacSmith."));
        event->ignore();
        return;
    }
    if (installService_.isRunning()) {
        QMessageBox::information(this, QStringLiteral("Installation in progress"),
                                 QStringLiteral("Wait for the current pacman operation to finish before closing PacSmith."));
        event->ignore();
        return;
    }
    if (keepRunningInTray_) {
        event->ignore();
        hide();
        return;
    }
    if (aiService_.isRunning()) aiService_.cancel();
    if (aptUpdateService_.isRunning()) aptUpdateService_.cancel();
    if (rpmUpdateService_.isRunning()) rpmUpdateService_.cancel();
    if (githubUpdateService_.isRunning()) githubUpdateService_.cancel();
    QMainWindow::closeEvent(event);
}

void MainWindow::refreshProjectList(const QString &selectId) {
    const auto previous = selectId.isEmpty() && project_ ? project_->id : selectId;
    projectList_->clear();
    QString error;
    const auto projects = store_.list(&error);
    projectCache_.clear();
    projectCache_.reserve(projects.size());
    for (const auto &project : projects) projectCache_.insert(project.id, project);
    QSet<QString> projectIds;
    const auto updateState = BackgroundUpdateStateStore::load();
    for (const auto &project : projects) {
        projectIds.insert(project.id);
        const auto *release = project.activeTrackingRelease();
        if (release == nullptr) release = project.newestRelease();
        auto visualState = ProjectVisualState::NotInstalled;
        QString subtitle = QStringLiteral("Not installed");
        QString statusDescription = QStringLiteral("Not installed");
        if (!project.installedVersion.isEmpty()) {
            if (project.installedRelease() == nullptr || project.externallyInstalled) {
                visualState = ProjectVisualState::Attention;
                subtitle = QStringLiteral("⚠ Installed %1 · tracking needs attention")
                               .arg(project.installedVersion);
                statusDescription = QStringLiteral("Installed package cannot be matched to a retained PacSmith release");
            } else if (project.hasAvailableUpdate()) {
                visualState = ProjectVisualState::UpdateAvailable;
                auto availableVersion = project.newestRelease() == nullptr
                    ? QString{} : project.newestRelease()->debian.version;
                const auto detected = project.installedRelease()->update.detectedVersion;
                if (!detected.isEmpty() &&
                    (availableVersion.isEmpty() ||
                     comparePackageVersions(
                         project.installedRelease()->update.strategy == UpdateStrategy::RpmRepository
                             ? SourcePackageType::Rpm : project.installedRelease()->sourceType,
                         detected, availableVersion) > 0)) {
                    availableVersion = detected;
                }
                subtitle = availableVersion.isEmpty()
                    ? QStringLiteral("⚠ Update available")
                    : QStringLiteral("⚠ Update %1 available").arg(availableVersion);
                statusDescription = QStringLiteral("Update available");
            } else {
                visualState = ProjectVisualState::Current;
                subtitle = QStringLiteral("✓ Installed %1 · up to date")
                               .arg(project.installedVersion);
                statusDescription = QStringLiteral("Installed and up to date");
            }
        }
        const bool checking = project.id == updateState.checkingProjectId && updateState.checking &&
                              project.id != preparingProjectId_ &&
                              project.id != updateState.preparingProjectId;
        const bool preparing = project.id == preparingProjectId_ ||
                               project.id == updateState.preparingProjectId;
        auto *item = new QListWidgetItem(project.displayName, projectList_);
        item->setIcon(projectIcon(store_, project));
        item->setData(Qt::UserRole, project.id);
        item->setData(projectSubtitleRole, subtitle);
        item->setData(projectVisualStateRole, static_cast<int>(visualState));
        item->setData(projectCheckingRole, checking || preparing);
        item->setData(projectActivityRole, QString{});
        item->setSizeHint(QSize(0, 60));
        item->setToolTip(QStringLiteral("%1 · %2%3")
            .arg(project.archPackageName, statusDescription,
                 release == nullptr ? QString{} : QStringLiteral(" · tracked release %1").arg(release->debian.version)));
        if (project.id == previous) projectList_->setCurrentItem(item);
    }
    QString managedError;
    for (const auto &managed : ManagedPackageRegistry::installed(&managedError)) {
        if (projectIds.contains(managed.projectId())) continue;
        auto *item = new QListWidgetItem(managed.packageName, projectList_);
        item->setData(projectSubtitleRole, QStringLiteral("⚠ PacSmith project files are missing"));
        item->setData(projectVisualStateRole,
                      static_cast<int>(ProjectVisualState::Attention));
        item->setSizeHint(QSize(0, 60));
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable & ~Qt::ItemIsEnabled);
        item->setToolTip(
            QStringLiteral("Pacman xdata identifies this as PacSmith-managed release %1, but its local project directory is missing. Source: %2")
                .arg(managed.releaseId(), managed.sourceIdentity()));
    }
    if (projectList_->currentItem() == nullptr && projectList_->count() > 0) projectList_->setCurrentRow(0);
    if (!error.isEmpty()) statusBar()->showMessage(error, 8000);
    if (!managedError.isEmpty()) statusBar()->showMessage(managedError, 8000);
    if (!preparingProjectId_.isEmpty() || !updateState.preparingProjectId.isEmpty()) {
        updatePreparationIndicators();
    }
    syncActivityTimer();
    if (listActivityInProgress()) updateUpdateCheckIndicators();
    if (projects.isEmpty()) {
        project_.reset();
        projectCache_.clear();
        stageTabs_->setEnabled(false);
        projectTabs_->setEnabled(false);
        if (projectSidebar_ != nullptr) projectSidebar_->show();
        rightStack_->setCurrentIndex(0);
        deleteProjectButton_->setEnabled(false);
        projectTitle_->setText(QStringLiteral("<h2>PacSmith</h2>"));
        projectSubtitle_->setText(QStringLiteral("Import a vendor artifact or GitHub release to begin."));
        if (projectPrimaryButton_ != nullptr) projectPrimaryButton_->setVisible(false);
        if (uninstallButton_ != nullptr) uninstallButton_->setVisible(false);
        if (editConfigurationButton_ != nullptr) editConfigurationButton_->setVisible(false);
    }
}

void MainWindow::loadProject(const QString &id) {
    lifecycleEditing_ = false;
    const auto cached = projectCache_.constFind(id);
    if (cached != projectCache_.cend()) {
        project_ = cached.value();
    } else {
        QString error;
        auto loaded = store_.load(id, &error);
        if (!loaded) {
            QMessageBox::critical(this, QStringLiteral("Could not load project"), error);
            return;
        }
        project_ = std::move(*loaded);
        projectCache_.insert(project_->id, *project_);
    }
    const auto *initialRelease = project_->activeTrackingRelease();
    if (initialRelease == nullptr) initialRelease = project_->newestRelease();
    currentReleaseId_ = initialRelease == nullptr ? QString{} : initialRelease->id;
    stageTabs_->setEnabled(true);
    projectTabs_->setEnabled(true);
    showProjectDashboard();
}

PackageRelease *MainWindow::currentRelease() {
    return project_ ? project_->release(currentReleaseId_) : nullptr;
}

const PackageRelease *MainWindow::currentRelease() const {
    return project_ ? project_->release(currentReleaseId_) : nullptr;
}

PackageRelease *MainWindow::activeTrackingRelease() {
    return project_ ? project_->activeTrackingRelease() : nullptr;
}

const PackageRelease *MainWindow::activeTrackingRelease() const {
    return project_ ? project_->activeTrackingRelease() : nullptr;
}

PackageRelease *MainWindow::updateEditorRelease() {
    if (!project_) return nullptr;
    if (rightStack_ != nullptr && rightStack_->currentIndex() == 1 && currentRelease() != nullptr) {
        return currentRelease();
    }
    return activeTrackingRelease();
}

void MainWindow::refreshCurrentProject() {
    if (!project_ || currentRelease() == nullptr) return;
    projectTitle_->setText(QStringLiteral("<h2>%1</h2>").arg(project_->displayName.toHtmlEscaped()));
    QStringList subtitleParts;
    if (!project_->installedVersion.isEmpty()) {
        subtitleParts.append(QStringLiteral("Installed %1").arg(project_->installedVersion));
    }
    subtitleParts.append(QStringLiteral("%1 %2")
                             .arg(sourcePackageTypeTitle(currentRelease()->sourceType),
                                  currentRelease()->debian.version));
    subtitleParts.append(QStringLiteral("Arch package %1").arg(project_->archPackageName));
    projectSubtitle_->setText(subtitleParts.join(QStringLiteral(" · ")));
    workbenchTitle_->setText(
        QStringLiteral("<h2>Set Up %1 %2</h2>")
            .arg(project_->displayName.toHtmlEscaped(), currentRelease()->debian.version.toHtmlEscaped()));
    configureEditorProfile();
    updateDeleteButton();
    if (rightStack_ != nullptr && rightStack_->currentIndex() == 1) {
        populateCurrentWorkbenchPage();
        return;
    }
    populateOverview();
    // Keep the hidden editor synchronized with the active tracking release so a
    // dashboard update check can never save a historical release's values into it.
    populateUpdates();
}

void MainWindow::populateCurrentWorkbenchPage() {
    if (!project_ || currentRelease() == nullptr || stageTabs_ == nullptr) return;
    switch (currentSection()) {
    case EditorSection::SourceOverview:
        populateSourceOverview();
        break;
    case EditorSection::SourceMetadata:
    case EditorSection::ConfigLayout:
        populatePackage();
        break;
    case EditorSection::ResultInstallPlan:
        populateInstallPlan();
        break;
    case EditorSection::ConfigDependencies:
        populateDependencies();
        break;
    case EditorSection::SourceScripts:
    case EditorSection::ConfigScripts:
        populateScripts();
        break;
    case EditorSection::SourceContents:
        populatePayload();
        break;
    case EditorSection::ConfigCommands:
        populateCommands();
        break;
    case EditorSection::ConfigAppRun:
        populateAppRunEditor();
        break;
    case EditorSection::ConfigDesktopEntries:
        populateDesktopEntries();
        break;
    case EditorSection::ConfigIcon:
        populateIcon();
        break;
    case EditorSection::ConfigUpdates:
        placeUpdatesEditor();
        populateUpdates();
        break;
    case EditorSection::ConfigPkgbuild:
    case EditorSection::ResultPkgbuild:
        populatePkgbuild();
        break;
    case EditorSection::ResultBuild:
        populateBuild();
        break;
    }
}

void MainWindow::updateDeleteButton() {
    if (!project_) {
        deleteProjectButton_->setEnabled(false);
        deleteProjectButton_->setToolTip({});
        if (reanalyzeButton_ != nullptr) reanalyzeButton_->setEnabled(false);
        return;
    }
    const bool busy = buildService_.isRunning() || installService_.isRunning() ||
                      aptUpdateService_.isRunning() || rpmUpdateService_.isRunning() ||
                      githubUpdateService_.isRunning() ||
                      aiService_.isRunning() || debDownloadService_.isRunning() ||
                      importThread_ != nullptr;
    if (reanalyzeButton_ != nullptr) {
        reanalyzeButton_->setEnabled(currentRelease() != nullptr &&
                                     currentRelease()->state != ReleaseState::Discovered &&
                                     currentRelease()->state != ReleaseState::Preparing && !busy);
    }
    deleteProjectButton_->setEnabled(!project_->ownsInstalledPackage() && !busy);
    if (project_->ownsInstalledPackage()) {
        deleteProjectButton_->setToolTip(
            QStringLiteral("Uninstall %1 with pacman before deleting this project").arg(project_->archPackageName));
    } else if (!project_->installedVersion.isEmpty()) {
        deleteProjectButton_->setToolTip(
            QStringLiteral("Delete this extra project. Installed %1 is managed by another PacSmith project or an external install")
                .arg(project_->archPackageName));
    } else if (busy) {
        deleteProjectButton_->setToolTip(QStringLiteral("Wait for the current operation to finish"));
    } else {
        deleteProjectButton_->setToolTip(QStringLiteral("Permanently delete this local PacSmith project"));
    }
}

void MainWindow::deleteCurrentProject() {
    if (!project_) return;
    if (project_->ownsInstalledPackage()) {
        QMessageBox::warning(this, QStringLiteral("Project cannot be deleted"),
                             QStringLiteral("%1 is installed. Uninstall it with pacman before deleting its PacSmith project.")
                                 .arg(project_->archPackageName));
        return;
    }
    if (buildService_.isRunning() || installService_.isRunning() ||
        aptUpdateService_.isRunning() || rpmUpdateService_.isRunning() ||
        githubUpdateService_.isRunning() ||
        aiService_.isRunning() || debDownloadService_.isRunning() ||
        importThread_ != nullptr) {
        QMessageBox::warning(this, QStringLiteral("Project is busy"),
                             QStringLiteral("Wait for the current operation to finish before deleting the project."));
        return;
    }
    QMessageBox confirmation(QMessageBox::Warning, QStringLiteral("Delete project"),
                             QStringLiteral("Delete “%1”?").arg(project_->displayName),
                             QMessageBox::NoButton, this);
    confirmation.setInformativeText(project_->installedVersion.isEmpty()
        ? QStringLiteral("This permanently removes the local vendor artifact, PKGBUILD, mappings, patches, build output, and history. This cannot be undone.")
        : QStringLiteral("This permanently removes the local vendor artifact, PKGBUILD, mappings, patches, build output, and history. The installed %1 package will be left in place. This cannot be undone.")
              .arg(project_->archPackageName));
    auto *deleteButton = confirmation.addButton(QStringLiteral("Delete Project"), QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != deleteButton) return;

    const auto deletedName = project_->displayName;
    QString error;
    if (!store_.deleteProject(*project_, &error)) {
        QMessageBox::critical(this, QStringLiteral("Could not delete project"), error);
        return;
    }
    project_.reset();
    refreshProjectList();
    statusBar()->showMessage(QStringLiteral("Deleted project %1").arg(deletedName), 8000);
}

void MainWindow::populateOverview() {
    if (!project_) return;
    const auto icon = projectIcon(store_, *project_);
    const auto pixmap = icon.pixmap(96, 96);
    overviewIcon_->setPixmap(pixmap);
    overviewIcon_->setVisible(!pixmap.isNull());
    const auto *latest = project_->newestRelease();
    const auto latestText = latest == nullptr ? QStringLiteral("unknown")
                                               : latest->debian.version.toHtmlEscaped();
    projectStateLabel_->setText(project_->installedVersion.isEmpty()
        ? QStringLiteral("<b>Not installed.</b> Latest known vendor release: %1. Retained releases remain available for building or installation.")
              .arg(latestText)
        : project_->externallyInstalled
            ? QStringLiteral("<b>Externally installed:</b> %1. Latest known vendor release: %2. This pacman version does not match a retained PacSmith build; automatic update tracking is paused.")
                  .arg(project_->installedVersion.toHtmlEscaped(), latestText)
            : project_->hasAvailableUpdate()
                ? QStringLiteral("<b>Update available.</b> Installed: %1 from release %2. Latest known vendor release: %3.")
                      .arg(project_->installedVersion.toHtmlEscaped(),
                           project_->installedRelease() == nullptr
                               ? QStringLiteral("unknown")
                               : project_->installedRelease()->debian.version.toHtmlEscaped(),
                           latestText)
                : QStringLiteral("<b>Installed and up to date:</b> %1 from release %2. Latest known vendor release: %3.")
                  .arg(project_->installedVersion.toHtmlEscaped(),
                       project_->installedRelease() == nullptr
                           ? QStringLiteral("unknown")
                           : project_->installedRelease()->debian.version.toHtmlEscaped(),
                       latestText));
    const auto *tracker = project_->activeTrackingRelease();
    if (tracker == nullptr) {
        activeTrackerLabel_->setText(QStringLiteral("<b>Update monitoring:</b> paused — %1")
            .arg(project_->externallyInstalled || !project_->installedVersion.isEmpty()
                     ? QStringLiteral("installed package is not a retained PacSmith release")
                     : QStringLiteral("no analyzed release is available")));
        projectAcquisitionLabel_->setText(QStringLiteral("<b>Acquisition:</b> no active release"));
    } else if (tracker->update.strategy == UpdateStrategy::Manual) {
        activeTrackerLabel_->setText(QStringLiteral("<b>Active update configuration:</b> release %1 · Manual (no automatic checks)%2")
                                         .arg(tracker->debian.version.toHtmlEscaped(),
                                              project_->installedRelease() == tracker
                                                  ? QStringLiteral(" · installed")
                                                  : QStringLiteral(" · newest analyzed fallback")));
    } else {
        activeTrackerLabel_->setText(QStringLiteral("<b>Active update configuration:</b> release %1 · %2 · %3%4")
            .arg(tracker->debian.version.toHtmlEscaped(),
                 tracker->update.strategy == UpdateStrategy::AptRepository
                     ? QStringLiteral("signed APT repository")
                 : tracker->update.strategy == UpdateStrategy::RpmRepository
                     ? QStringLiteral("signed RPM repository")
                 : tracker->update.strategy == UpdateStrategy::GitHubRelease
                     ? QStringLiteral("GitHub releases") : QStringLiteral("direct URL"),
                 tracker->update.url.toHtmlEscaped(),
                 project_->installedRelease() == tracker
                     ? QStringLiteral(" · installed")
                     : QStringLiteral(" · newest analyzed fallback")));
    }
    if (tracker != nullptr) {
        const auto acquisitionLocation = !tracker->acquisition.originalUrl.isEmpty()
            ? tracker->acquisition.originalUrl
            : !tracker->sourceUrl.isEmpty() ? tracker->sourceUrl : tracker->originalSourceFilename;
        projectAcquisitionLabel_->setText(
            QStringLiteral("<b>Acquisition for release %1:</b> %2 · %3 · %4<br><b>Recorded SHA256:</b> %5")
                .arg(tracker->debian.version.toHtmlEscaped(),
                     acquisitionKindName(tracker->acquisition.kind).toHtmlEscaped(),
                     sourcePackageTypeName(tracker->sourceType).toHtmlEscaped(),
                     acquisitionLocation.toHtmlEscaped(), tracker->sourceSha256.toHtmlEscaped()));
    }

    const auto selectedBefore = selectedDashboardReleaseId().isEmpty()
        ? currentReleaseId_ : selectedDashboardReleaseId();
    QSignalBlocker tableBlocker(releaseTable_);
    QList<const PackageRelease *> ordered;
    for (const auto &release : project_->releases) ordered.append(&release);
    std::sort(ordered.begin(), ordered.end(), [](const auto *left, const auto *right) {
        return compareReleaseVersions(*left, *right) > 0;
    });
    releaseTable_->setRowCount(static_cast<int>(ordered.size()));
    int selectedRow = -1;
    for (int row = 0; row < ordered.size(); ++row) {
        const auto &release = *ordered.at(row);
        const bool preparing = project_->id == preparingProjectId_ &&
                               release.id == preparingReleaseId_;
        const auto unresolvedForRelease = std::count_if(
            release.dependencies.cbegin(), release.dependencies.cend(),
            [this](const auto &dependency) {
                return dependency.status == MappingStatus::Unresolved ||
                       repositoryPackageUnavailable(dependency,
                                                    repositoryDependencyAvailability_);
            });
        const auto reviewCount = release.state == ReleaseState::Discovered ? 0
            : unresolvedForRelease + pendingScriptFindings(release) + pendingPayloadReviews(release) +
                  (release.installMapping.appRun.requiresReview() ? 1 : 0) +
                  ((!release.lifecycleScript.contents.isEmpty() &&
                    (!release.lifecycleScript.validationPassed || release.lifecycleScript.requiresAcknowledgement())) ? 1 : 0);
        QString packageVersion;
        for (auto build = release.builds.crbegin(); build != release.builds.crend() && packageVersion.isEmpty(); ++build) {
            if (!build->artifacts.isEmpty()) packageVersion = build->artifacts.first().packageVersion;
        }
        const QStringList values{
            release.debian.version,
            preparing ? QStringLiteral("Preparing") : releaseStateName(release.state),
            release.state == ReleaseState::Discovered
                ? QStringLiteral("%1%2")
                      .arg(acquisitionKindName(release.acquisition.kind),
                           release.sourceSha256.isEmpty() ? QStringLiteral(" · unsigned")
                                                          : QStringLiteral(" · checksum"))
                : release.originalSourceFilename,
            release.update.strategy == UpdateStrategy::Manual ? QStringLiteral("Manual")
                : release.update.strategy == UpdateStrategy::DirectUrl ? QStringLiteral("Direct URL")
                : release.update.strategy == UpdateStrategy::AptRepository ? QStringLiteral("APT")
                : release.update.strategy == UpdateStrategy::RpmRepository ? QStringLiteral("RPM")
                                                                           : QStringLiteral("GitHub"),
            preparing ? QStringLiteral("Processing…")
            : release.state == ReleaseState::Discovered ? QStringLiteral("Not prepared")
                : reviewCount == 0 ? QStringLiteral("Ready")
                                   : QStringLiteral("%1 item(s)").arg(reviewCount),
            QString::number(release.builds.size()),
            packageVersion.isEmpty() ? QStringLiteral("—") : packageVersion,
            release.id == project_->installedReleaseId ? QStringLiteral("Yes") : QStringLiteral("—")};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setData(Qt::UserRole, release.id);
            releaseTable_->setItem(row, column, item);
        }
        if (release.id == selectedBefore) selectedRow = row;
    }
    releaseTable_->resizeColumnsToContents();
    if (selectedRow < 0 && !ordered.isEmpty()) selectedRow = 0;
    if (selectedRow >= 0) releaseTable_->selectRow(selectedRow);

    const auto unresolved = std::count_if(currentRelease()->dependencies.cbegin(), currentRelease()->dependencies.cend(),
                                          [this](const auto &dependency) {
                                              return dependency.status == MappingStatus::Unresolved ||
                                                     repositoryPackageUnavailable(
                                                         dependency,
                                                         repositoryDependencyAvailability_);
                                          });
    const auto scriptReviews = pendingScriptFindings(*currentRelease());
    const auto payloadReviews = pendingPayloadReviews(*currentRelease());
    QStringList lines{QStringLiteral("✓ Source analyzed and SHA256 recorded"),
                      currentRelease()->sourceType == SourcePackageType::Debian
                          ? QStringLiteral("✓ Debian metadata imported")
                      : currentRelease()->sourceType == SourcePackageType::Rpm
                          ? QStringLiteral("✓ RPM metadata imported")
                          : QStringLiteral("✓ Artifact metadata imported"),
                      unresolved == 0 ? QStringLiteral("✓ Dependencies resolved, available, or explicitly treated")
                                      : QStringLiteral("⚠ %1 dependency group(s) need attention").arg(unresolved),
                      currentRelease()->maintainerScripts.isEmpty()
                          ? QStringLiteral("✓ No maintainer scripts detected")
                          : scriptReviews == 0
                                ? QStringLiteral("✓ Maintainer-script responsibilities resolved")
                                : QStringLiteral("⚠ %1 script responsibility item(s) require resolution").arg(scriptReviews),
                      payloadReviews == 0
                          ? QStringLiteral("✓ Flagged payload files have explicit decisions")
                          : QStringLiteral("⚠ %1 payload file(s) need a keep/exclude decision").arg(payloadReviews),
                      currentRelease()->lifecycleScript.contents.isEmpty()
                          ? QStringLiteral("✓ No generated privileged lifecycle script")
                      : !currentRelease()->lifecycleScript.validationPassed
                          ? QStringLiteral("⚠ Generated lifecycle script failed validation")
                      : currentRelease()->lifecycleScript.requiresAcknowledgement()
                          ? QStringLiteral("⚠ AI-generated lifecycle script requires exact-content acknowledgement")
                          : QStringLiteral("✓ Generated lifecycle script acknowledged"),
                      QStringLiteral("✓ PKGBUILD present"),
                      currentRelease()->update.strategy == UpdateStrategy::Manual
                          ? QStringLiteral("○ Automatic update source not configured")
                      : currentRelease()->update.strategy == UpdateStrategy::DirectUrl
                          ? QStringLiteral("○ Direct URL configured; version discovery is not implemented")
                      : currentRelease()->update.strategy == UpdateStrategy::GitHubRelease
                          ? currentRelease()->update.lastChecked.isValid()
                              ? QStringLiteral("✓ GitHub releases checked: %1")
                                .arg(currentRelease()->update.detectedVersion.isEmpty()
                                         ? QStringLiteral("no version recorded")
                                         : currentRelease()->update.detectedVersion)
                              : QStringLiteral("○ GitHub release tracking configured; not checked yet")
                      : currentRelease()->update.lastChecked.isValid()
                          ? QStringLiteral("✓ APT repository checked: %1")
                                .arg(currentRelease()->update.detectedVersion.isEmpty()
                                         ? QStringLiteral("no version recorded")
                                         : currentRelease()->update.detectedVersion)
                          : currentRelease()->update.aptSigningKeyring.isEmpty() ||
                                    currentRelease()->update.trustedSigningFingerprint.isEmpty()
                              ? QStringLiteral("⚠ APT repository needs a trusted signing key")
                              : QStringLiteral("○ APT repository configured with pinned key; not checked yet")};
    overviewChecklist_->setText(QStringLiteral("<b>Selected release %1</b><br>%2")
                                    .arg(currentRelease()->debian.version.toHtmlEscaped(),
                                         lines.join(QStringLiteral("<br>"))));
    resolveWithAiButton_->setEnabled(!aiService_.isRunning() &&
                                     currentRelease()->state != ReleaseState::Discovered);
    updateDashboardActions();
    tableBlocker.unblock();
    if (selectedRow >= 0) emit releaseTable_->itemSelectionChanged();
    if (!preparingReleaseId_.isEmpty()) updatePreparationIndicators();
}

void MainWindow::populateSourceOverview() {
    if (!project_ || currentRelease() == nullptr || sourceTypeHeadline_ == nullptr) return;
    const auto &release = *currentRelease();
    sourceTypeHeadline_->setText(
        QStringLiteral("<h2>%1</h2>").arg(sourcePackageTypeTitle(release.sourceType).toHtmlEscaped()));
    sourceTypeExplanation_->setText(sourcePackageTypeExplanation(release.sourceType));
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
        QString notice = release.pkgbuildManuallyModified
            ? QStringLiteral("Filesystem parsed from the Custom PKGBUILD. Expand a directory to see each file.")
            : QStringLiteral("Filesystem parsed from the Guided PKGBUILD. Expand a directory to see each file.");
        if (!plan.warnings.isEmpty()) {
            notice += QLatin1Char('\n') + plan.warnings.join(QLatin1Char('\n'));
        }
        installPlanNotice_->setText(notice);
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
                                     ? QStringLiteral("⚠ The PKGBUILD is user-owned; add install='%1' manually or restore the generated PKGBUILD.")
                                           .arg(lifecycle.fileName)
                                     : QStringLiteral("✓ The generated PKGBUILD contains install='%1'.")
                                           .arg(lifecycle.fileName);
        lifecycleStatus_->setText(QStringLiteral("%1 · "
                                                 "<span style='color:#e5b93d'>⚠ user approval required before installation</span><br>"
                                                 "This is the only script pacman will execute. Vendor maintainer scripts are never this file.<br>%2<br>%3")
                                      .arg(lifecycleOrigin, lifecycle.validationMessage.toHtmlEscaped(), integration));
        acknowledgeLifecycleButton_->setEnabled(true);
        discardLifecycleButton_->setEnabled(true);
    } else {
        const auto integration = currentRelease()->pkgbuildManuallyModified
                                     ? QStringLiteral("⚠ The PKGBUILD is user-owned; verify it contains install='%1'.")
                                           .arg(lifecycle.fileName)
                                     : QStringLiteral("✓ The generated PKGBUILD contains install='%1'.")
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
            QStringLiteral("The script validated, but Configuration is in Custom mode. Add install='%1' to the PKGBUILD or switch back to Guided. Then review and approve the exact script before installation.")
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

void MainWindow::populatePayload() {
    if (!project_) return;
    const bool appImage = currentRelease()->sourceType == SourcePackageType::AppImage;
    payloadIntroduction_->setText(appImage
        ? QStringLiteral("Read-only AppDir contents. Every entry remains inside /opt/%1; PacSmith does not relocate, prune, or expose individual bundle files. Select a file to inspect it.")
              .arg(currentRelease()->installMapping.optDirectory.isEmpty()
                       ? currentRelease()->archPackageName
                       : currentRelease()->installMapping.optDirectory)
        : QStringLiteral("This is the filesystem payload in the vendor artifact. Most files need no action. For highlighted system files, inspect the explanation/content and explicitly keep or exclude them. Symbolic links that point outside the package are shown in red and stay excluded until you keep them. Decisions are content-specific and changed files require review again."));
    payloadTree_->headerItem()->setText(3, appImage ? QStringLiteral("Bundle status")
                                                    : QStringLiteral("Review"));
    keepPayloadButton_->setVisible(!appImage);
    excludePayloadButton_->setVisible(!appImage);
    clearPayloadDecisionButton_->setVisible(!appImage);
    const auto selectedPath = payloadTree_->currentItem()
                                  ? payloadTree_->currentItem()->data(0, Qt::UserRole).toString()
                                  : QString{};
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
    if (!selectedPath.isEmpty()) {
        QTreeWidgetItemIterator iterator(payloadTree_);
        while (*iterator != nullptr) {
            if ((*iterator)->data(0, Qt::UserRole).toString() == selectedPath) {
                payloadTree_->setCurrentItem(*iterator);
                break;
            }
            ++iterator;
        }
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

    if (!entry.textPreview.isEmpty()) {
        payloadPreview_->setPlainText(entry.textPreview +
                                      (entry.previewTruncated ? QStringLiteral("\n\n[Preview truncated at 1 MiB]") : QString{}));
    } else if (entry.type == QStringLiteral("file") && entry.contentSha256.isEmpty()) {
        payloadPreview_->setPlainText(QStringLiteral("Loading file content from the saved vendor artifact…"));
        loadSelectedPayloadPreview(path);
    } else if (entry.type == QStringLiteral("file")) {
        payloadPreview_->setPlainText(QStringLiteral("Binary or non-UTF-8 file; text preview is unavailable.\nSHA256: %1")
                                          .arg(entry.contentSha256));
    } else if (!entry.symlinkTarget.isEmpty()) {
        payloadPreview_->setPlainText(QStringLiteral("Symbolic link target: %1").arg(entry.symlinkTarget));
    } else {
        payloadPreview_->clear();
    }

    const bool fingerprintReady = entry.type != QStringLiteral("file") || !entry.contentSha256.isEmpty();
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
    if (entry == currentRelease()->payload.cend() ||
        (entry->type == QStringLiteral("file") && entry->contentSha256.isEmpty())) return;
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
    const auto debPath = store_.sourcePath(*currentRelease());
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
                persistCurrent();
                populatePayload();
                QTreeWidgetItemIterator iterator(payloadTree_);
                while (*iterator != nullptr) {
                    if ((*iterator)->data(0, Qt::UserRole).toString() == path) {
                        payloadTree_->setCurrentItem(*iterator);
                        break;
                    }
                    ++iterator;
                }
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
        appRunEditor_->clear();
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
    const auto selected = payloadIconCandidates_->findData(icon.sourcePath);
    if (selected >= 0) payloadIconCandidates_->setCurrentIndex(selected);
    const auto path = store_.releasePath(*currentRelease()) /
                      std::filesystem::path(icon.projectPath.toUtf8().constData());
    QPixmap pixmap(QString::fromUtf8(path.string().c_str()));
    if (!pixmap.isNull()) {
        iconPreview_->setPixmap(pixmap.scaled(iconPreview_->size() - QSize(16, 16),
                                               Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation));
    } else {
        iconPreview_->setPixmap({});
        iconPreview_->setText(QStringLiteral("No icon selected"));
    }
    iconStatus_->setText(
        icon.projectPath.isEmpty()
            ? QStringLiteral("No project icon is configured. The package can still be built, but desktop integration may look incomplete.")
            : QStringLiteral("Source: %1\nStored as: %2\nSHA256: %3")
                  .arg(icon.sourcePath.isEmpty() ? icon.sourceUrl : icon.sourcePath,
                       icon.projectPath, icon.sha256));
}

void MainWindow::selectPayloadIcon() {
    if (!project_ || payloadIconCandidates_->currentIndex() < 0) return;
    const auto path = payloadIconCandidates_->currentData().toString();
    QString error;
    const auto contents = PayloadInspector::readFileBytes(
        store_.sourcePath(*currentRelease()), path, 4 * 1024 * 1024, &error);
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
    const auto absolute = store_.releasePath(*currentRelease()) /
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
    refreshGeneratedPkgbuildAfterModelChange();
    populateIcon();
    if (auto *item = projectList_->currentItem()) {
        item->setIcon(QIcon(QString::fromUtf8(absolute.string().c_str())));
    }
}

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

void MainWindow::showSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("PacSmith Settings"));
    dialog.setMinimumSize(680, 620);
    auto *rootLayout = new QVBoxLayout(&dialog);
    auto *settingsTabs = new QTabWidget(&dialog);

    auto *generalPage = new QWidget(settingsTabs);
    auto *generalLayout = new QVBoxLayout(generalPage);
    auto *sessionGroup = new QGroupBox(QStringLiteral("Session"), generalPage);
    auto *sessionLayout = new QVBoxLayout(sessionGroup);
    sessionLayout->addWidget(settingsSectionHelp(
        sessionGroup,
        QStringLiteral("Closing the window quits PacSmith unless it stays in the tray."),
        QStringLiteral("Closing the main window quits PacSmith unless it is kept running in the tray. "
                       "Open it again from the tray icon, or choose Quit there to exit completely.\n\n"
                       "Start at login can also start PacSmith minimized to the tray.")));
    auto *keepInTray = new QCheckBox(QStringLiteral("Keep PacSmith running in the tray"), sessionGroup);
    keepInTray->setChecked(aiSettings_.updates.keepInTray || aiSettings_.updates.startMinimized);
    auto *startAtLogin = new QCheckBox(QStringLiteral("Start PacSmith at login"), sessionGroup);
    startAtLogin->setChecked(aiSettings_.updates.startAtLogin);
    auto *startMinimized = new QCheckBox(QStringLiteral("Start minimized to the tray"), sessionGroup);
    startMinimized->setChecked(aiSettings_.updates.startMinimized);
    sessionLayout->addWidget(keepInTray);
    sessionLayout->addWidget(startAtLogin);
    sessionLayout->addWidget(startMinimized);
    generalLayout->addWidget(sessionGroup);
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        auto *trayNotice = new QLabel(
            QStringLiteral("No system tray is available, so PacSmith cannot stay running after the window closes."),
            generalPage);
        trayNotice->setWordWrap(true);
        keepInTray->setEnabled(false);
        keepInTray->setChecked(false);
        startMinimized->setEnabled(false);
        startMinimized->setChecked(false);
        generalLayout->addWidget(trayNotice);
    }

    auto *credentialSource = new QComboBox(generalPage);
    credentialSource->addItems({QStringLiteral("Process environment (read-only)"),
                                QStringLiteral("Desktop keyring"),
                                QStringLiteral("Age-encrypted file")});
    credentialSource->setPlaceholderText(QStringLiteral("Choose how PacSmith stores secrets…"));
    credentialSource->setCurrentIndex(-1);
    const auto configuredSource = [&]() -> std::optional<CredentialSource> {
        if (aiSettings_.provider != AiProviderKind::None) {
            const auto providerSource = aiSettings_.credentialSources.constFind(
                aiProviderName(aiSettings_.provider));
            if (providerSource != aiSettings_.credentialSources.cend()) return providerSource.value();
        }
        const auto githubSource = aiSettings_.credentialSources.constFind(QStringLiteral("github"));
        if (githubSource != aiSettings_.credentialSources.cend()) return githubSource.value();
        if (!aiSettings_.credentialSources.isEmpty()) return aiSettings_.credentialSources.first();
        return std::nullopt;
    }();
    if (configuredSource) credentialSource->setCurrentIndex(static_cast<int>(*configuredSource));
    auto *ageStoreAction = new QPushButton(generalPage);
    auto *storeNotice = new QLabel(generalPage);
    storeNotice->setWordWrap(true);
    auto *secretsGroup = new QGroupBox(QStringLiteral("Secret store"), generalPage);
    auto *secretsForm = new QFormLayout(secretsGroup);
    secretsForm->addRow(settingsSectionHelp(
        secretsGroup,
        QStringLiteral("Choose a store before PacSmith will accept a GitHub token or AI credentials."),
        QStringLiteral("The same store is used for GitHub tokens and AI credentials.\n\n"
                       "Process environment is read-only and must be set before PacSmith starts. Desktop launches "
                       "usually do not inherit variables set in a terminal.\n\n"
                       "Desktop keyring stores secrets in your session keyring.\n\n"
                       "Age-encrypted file stores secrets in a password-protected file.")));
    secretsForm->addRow(QStringLiteral("Storage"), credentialSource);
    secretsForm->addRow(QString{}, ageStoreAction);
    secretsForm->addRow(storeNotice);
    generalLayout->addWidget(secretsGroup);

    auto *githubGroup = new QGroupBox(QStringLiteral("GitHub"), generalPage);
    auto *githubForm = new QFormLayout(githubGroup);
    auto *githubToken = new QLineEdit(githubGroup);
    githubToken->setEchoMode(QLineEdit::Password);
    githubToken->setPlaceholderText(QStringLiteral("Optional; leave blank to keep a saved token"));
    auto *githubLockNotice = new QLabel(githubGroup);
    githubLockNotice->setWordWrap(true);
    githubForm->addRow(settingsSectionHelp(
        githubGroup,
        QStringLiteral("Optional. Used when adding GitHub packages and checking for updates."),
        QStringLiteral("A token is optional for public repositories but raises GitHub API rate limits. "
                       "PacSmith uses it when adding packages from GitHub URLs and when checking for updates.\n\n"
                       "Leave the field blank to keep a saved token. Environment storage reads "
                       "PACSMITH_GITHUB_TOKEN when PacSmith starts and never displays its value.")));
    githubForm->addRow(QStringLiteral("Personal access token"), githubToken);
    githubForm->addRow(githubLockNotice);
    generalLayout->addWidget(githubGroup);
    generalLayout->addStretch(1);
    settingsTabs->addTab(generalPage, QStringLiteral("General"));

    auto *aiPage = new QWidget(settingsTabs);
    auto *layout = new QVBoxLayout(aiPage);
    settingsTabs->addTab(aiPage, QStringLiteral("AI Advisor"));
    rootLayout->addWidget(settingsTabs, 1);
    auto *aiGroup = new QGroupBox(QStringLiteral("AI Advisor"), aiPage);
    auto *aiGroupLayout = new QVBoxLayout(aiGroup);
    auto *aiSectionHelp = settingsSectionHelp(
        aiGroup,
        QStringLiteral("AI is optional. Local inspection always runs first."),
        QStringLiteral("PacSmith always performs local deterministic inspection first and sends only a bounded "
                       "package-evidence bundle. Package binaries and unrelated files are never sent."));
    aiGroupLayout->addWidget(aiSectionHelp);
    auto *form = new QFormLayout;
    auto *provider = new QComboBox(aiPage);
    provider->addItems({QStringLiteral("None"), QStringLiteral("ChatGPT subscription"),
                        QStringLiteral("OpenAI API"),
                        QStringLiteral("xAI / Grok API")});
    provider->setCurrentIndex(static_cast<int>(aiSettings_.provider));
    if (!configuredSource && aiSettings_.provider != AiProviderKind::None) {
        provider->setCurrentIndex(static_cast<int>(AiProviderKind::None));
    }
    auto *model = new QComboBox(aiPage);
    model->setEditable(true);
    model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    model->setMinimumContentsLength(36);
    model->lineEdit()->setPlaceholderText(QStringLiteral("Provider model ID"));
    model->setEditText(aiSettings_.model);
    auto *reasoningEffort = new QComboBox(aiPage);
    auto *executionMode = new QComboBox(aiPage);
    executionMode->addItem(QStringLiteral("Standard"),
                           static_cast<int>(AiExecutionMode::Standard));
    executionMode->addItem(QStringLiteral("Fast (priority)"),
                           static_cast<int>(AiExecutionMode::Fast));
    executionMode->setCurrentIndex(
        executionMode->findData(static_cast<int>(aiSettings_.executionMode)));
    auto *automatic = new QCheckBox(QStringLiteral("Automatically resolve items flagged for review with AI"), aiPage);
    automatic->setChecked(aiSettings_.automaticallyResolveReviewItems);
    auto *secretsHint = new QLabel(aiPage);
    secretsHint->setWordWrap(true);
    auto *apiKey = new QLineEdit(aiPage);
    apiKey->setEchoMode(QLineEdit::Password);
    apiKey->setPlaceholderText(QStringLiteral("Leave blank to keep an existing stored key"));
    auto *aiEnvironmentNotice = new QLabel(aiPage);
    aiEnvironmentNotice->setWordWrap(true);
    aiEnvironmentNotice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    aiEnvironmentNotice->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *chatGptSignIn = new QPushButton(QStringLiteral("Sign in with ChatGPT…"), aiPage);
    auto *loadModels = new QPushButton(QStringLiteral("Load Available Models"), aiPage);
    form->addRow(secretsHint);
    form->addRow(QStringLiteral("Provider"), provider);
    form->addRow(QStringLiteral("API key"), apiKey);
    form->addRow(QString{}, chatGptSignIn);
    form->addRow(aiEnvironmentNotice);
    form->addRow(QStringLiteral("Model"), model);
    form->addRow(QString{}, loadModels);
    form->addRow(QStringLiteral("Reasoning effort"), reasoningEffort);
    form->addRow(QStringLiteral("Execution speed"), executionMode);
    form->addRow(QString{}, automatic);
    aiGroupLayout->addLayout(form);
    layout->addWidget(aiGroup);
    layout->addStretch(1);
    auto *aiStatusPanel = settingsStatusFrame(aiPage);
    auto *aiStatusLayout = new QVBoxLayout(aiStatusPanel);
    aiStatusLayout->setContentsMargins(12, 8, 12, 9);
    aiStatusLayout->setSpacing(4);
    auto *aiStatusTitle = new QLabel(QStringLiteral("●  STATUS"), aiStatusPanel);
    aiStatusTitle->setStyleSheet(QStringLiteral("color: #3498db; font-weight: 600;"));
    auto *aiCredentialStatus = new QLabel(aiStatusPanel);
    aiCredentialStatus->setWordWrap(true);
    aiCredentialStatus->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    aiCredentialStatus->setMinimumHeight(42);
    aiCredentialStatus->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    aiCredentialStatus->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    aiStatusLayout->addWidget(aiStatusTitle);
    aiStatusLayout->addWidget(aiCredentialStatus);
    layout->addWidget(aiStatusPanel);

    auto *updatesPage = new QWidget(settingsTabs);
    auto *updatesLayout = new QVBoxLayout(updatesPage);
    auto *scheduleGroup = new QGroupBox(QStringLiteral("Updates"), updatesPage);
    auto *updatesForm = new QFormLayout(scheduleGroup);
    updatesForm->addRow(settingsSectionHelp(
        scheduleGroup,
        QStringLiteral("Checks run while PacSmith is open, including in the tray."),
        QStringLiteral("Periodic checks run while PacSmith is open, including while it is kept in the tray. "
                       "Each project release owns its own update source. The installed known release is the "
                       "active tracker; before installation, the newest analyzed release is. Checks pause when "
                       "an external installed version cannot be matched safely.")));
    auto *backgroundEnabled = new QCheckBox(QStringLiteral("Check for updates periodically"), scheduleGroup);
    backgroundEnabled->setChecked(aiSettings_.updates.enabled);
    auto *schedule = new QComboBox(scheduleGroup);
    schedule->addItems({QStringLiteral("Every day"), QStringLiteral("Selected weekday")});
    schedule->setCurrentIndex(aiSettings_.updates.daily ? 0 : 1);
    auto *weekday = new QComboBox(scheduleGroup);
    weekday->addItems({QStringLiteral("Monday"), QStringLiteral("Tuesday"),
                       QStringLiteral("Wednesday"), QStringLiteral("Thursday"),
                       QStringLiteral("Friday"), QStringLiteral("Saturday"),
                       QStringLiteral("Sunday")});
    weekday->setCurrentIndex(std::clamp(aiSettings_.updates.weekDay, 1, 7) - 1);
    auto *checkTime = new QTimeEdit(aiSettings_.updates.localTime, scheduleGroup);
    checkTime->setDisplayFormat(QStringLiteral("HH:mm"));
    updatesForm->addRow(QString{}, backgroundEnabled);
    updatesForm->addRow(QStringLiteral("Frequency"), schedule);
    updatesForm->addRow(QStringLiteral("Weekday"), weekday);
    updatesForm->addRow(QStringLiteral("Local time"), checkTime);
    updatesLayout->addWidget(scheduleGroup);

    auto *behaviorGroup = new QGroupBox(QStringLiteral("When updates are found"), updatesPage);
    auto *behaviorForm = new QFormLayout(behaviorGroup);
    behaviorForm->addRow(settingsSectionHelp(
        behaviorGroup,
        QStringLiteral("Newly discovered vendor artifacts can be downloaded and prepared automatically."),
        QStringLiteral("APT artifacts require their signed repository SHA256. GitHub artifacts use the publisher "
                       "digest when available and otherwise remain visibly unsigned. Every artifact is re-inspected; "
                       "unchanged fingerprinted decisions may carry forward.")));
    auto *automaticPrepare = new QCheckBox(
        QStringLiteral("Download and prepare newly discovered vendor artifacts automatically"), behaviorGroup);
    automaticPrepare->setChecked(aiSettings_.updates.automaticallyPrepare);
    behaviorForm->addRow(QString{}, automaticPrepare);
    updatesLayout->addWidget(behaviorGroup);

    auto *checkStatusPanel = settingsStatusFrame(updatesPage);
    auto *checkStatusLayout = new QVBoxLayout(checkStatusPanel);
    checkStatusLayout->setContentsMargins(12, 8, 12, 9);
    checkStatusLayout->setSpacing(4);
    auto *checkStatusTitle = new QLabel(QStringLiteral("●  LAST CHECK"), checkStatusPanel);
    checkStatusTitle->setStyleSheet(QStringLiteral("color: #3498db; font-weight: 600;"));
    auto *lastCheckStatus = new QLabel(checkStatusPanel);
    lastCheckStatus->setWordWrap(true);
    lastCheckStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lastCheckStatus->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    checkStatusLayout->addWidget(checkStatusTitle);
    checkStatusLayout->addWidget(lastCheckStatus);
    updatesLayout->addWidget(checkStatusPanel);

    auto *serviceStatus = new QLabel(updatesPage);
    serviceStatus->setWordWrap(true);
    serviceStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *updateButtons = new QHBoxLayout;
    auto *checkNow = new QPushButton(QStringLiteral("Check Now"), updatesPage);
    updateButtons->addWidget(checkNow);
    updateButtons->addStretch();
    updatesLayout->addLayout(updateButtons);
    updatesLayout->addWidget(serviceStatus);
    updatesLayout->addStretch();
    settingsTabs->addTab(updatesPage, QStringLiteral("Updates"));

    auto *cleanupPage = new QWidget(settingsTabs);
    auto *cleanupLayout = new QVBoxLayout(cleanupPage);
    auto *cleanupGroup = new QGroupBox(QStringLiteral("Cleanup"), cleanupPage);
    auto *cleanupForm = new QFormLayout(cleanupGroup);
    cleanupForm->addRow(settingsSectionHelp(
        cleanupGroup,
        QStringLiteral("Older artifacts and complete releases are removed after update checks."),
        QStringLiteral("These counts apply to versions older than the currently installed PacSmith release. "
                       "Rolling back moves that retention anchor.\n\n"
                       "Complete-release retention cannot be lower than artifact retention. When automatic "
                       "download is enabled, unbuilt intermediate updates between the installed or last-built "
                       "version and the latest download are dropped entirely.")));
    auto *retainedPackages = new QSpinBox(cleanupPage);
    retainedPackages->setRange(-1, 50);
    retainedPackages->setSpecialValueText(QStringLiteral("Unlimited"));
    retainedPackages->setValue(aiSettings_.updates.retainedPackageVersions);
    auto *retainedReleases = new QSpinBox(cleanupPage);
    retainedReleases->setRange(-1, 50);
    retainedReleases->setSpecialValueText(QStringLiteral("Unlimited"));
    retainedReleases->setValue(aiSettings_.updates.retainedCompleteReleases);
    cleanupForm->addRow(QStringLiteral("Old package artifacts"), retainedPackages);
    cleanupForm->addRow(QStringLiteral("Old complete releases"), retainedReleases);
    cleanupLayout->addWidget(cleanupGroup);
    cleanupLayout->addStretch();
    settingsTabs->addTab(cleanupPage, QStringLiteral("Cleanup"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto *saveButton = buttons->button(QDialogButtonBox::Save);
    rootLayout->addWidget(buttons);

    auto currentUpdateSettings = [&] {
        BackgroundUpdateSettings settings = aiSettings_.updates;
        settings.enabled = backgroundEnabled->isChecked();
        settings.startAtLogin = startAtLogin->isChecked();
        settings.startMinimized = startMinimized->isChecked();
        settings.keepInTray = keepInTray->isChecked();
        settings.daily = schedule->currentIndex() == 0;
        settings.weekDay = weekday->currentIndex() + 1;
        settings.localTime = checkTime->time();
        settings.automaticallyPrepare = automaticPrepare->isChecked();
        settings.retainedPackageVersions = retainedPackages->value();
        settings.retainedCompleteReleases = retainedPackages->value() < 0 ||
                                            retainedReleases->value() < 0
            ? -1 : std::max(retainedReleases->value(), retainedPackages->value());
        return settings;
    };
    auto refreshScheduleControls = [&] {
        const bool periodic = backgroundEnabled->isChecked();
        schedule->setEnabled(periodic);
        weekday->setEnabled(periodic && schedule->currentIndex() == 1);
        checkTime->setEnabled(periodic);
    };
    auto refreshCheckStatus = [&] {
        const auto state = BackgroundUpdateStateStore::load();
        const auto pending = currentUpdateSettings();
        QStringList lines;
        if (state.checking) {
            lines.append(QStringLiteral("Update check in progress…"));
        } else if (!state.lastRun.isValid()) {
            lines.append(QStringLiteral("Last check: never"));
        } else {
            lines.append(QStringLiteral("Last check: %1").arg(formatLocalDateTime(state.lastRun)));
            if (!state.message.isEmpty()) lines.append(state.message);
            if (pending.enabled && BackgroundUpdateManager::isOverdue(pending, state.lastRun)) {
                lines.append(QStringLiteral("This check is overdue."));
            }
        }
        if (pending.enabled) {
            lines.append(QStringLiteral("Next scheduled check: %1")
                             .arg(formatLocalDateTime(
                                 BackgroundUpdateManager::nextScheduledOccurrence(pending))));
        } else {
            lines.append(QStringLiteral("Automatic checks are disabled."));
        }
        lastCheckStatus->setText(lines.join(QLatin1Char('\n')));
    };
    auto persistBackgroundSettings = [&, this](QString *error) {
        aiSettings_.updates = currentUpdateSettings();
        if (credentialSource->currentIndex() >= 0) {
            const auto source = static_cast<CredentialSource>(credentialSource->currentIndex());
            aiSettings_.credentialSources.insert(QStringLiteral("github"), source);
            if (source == CredentialSource::Environment) {
                aiSettings_.githubTokenConfigured = false;
            } else if (!githubToken->text().isEmpty()) {
                if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() &&
                    !unlockAgeCredentials()) {
                    if (error != nullptr) *error = QStringLiteral("The age credential store is locked");
                    return false;
                }
                if (!credentialStore_.store(QStringLiteral("github"), source,
                                            githubToken->text(), {}, error)) {
                    return false;
                }
                aiSettings_.githubTokenConfigured = true;
            }
        }
        return settingsStore_.save(aiSettings_, error) &&
               BackgroundUpdateManager::apply(aiSettings_.updates,
                                              QCoreApplication::applicationFilePath(), error);
    };
    auto refreshSessionControls = [&] {
        const bool trayOk = QSystemTrayIcon::isSystemTrayAvailable();
        keepInTray->setEnabled(trayOk);
        if (!trayOk) {
            keepInTray->setChecked(false);
            startMinimized->setChecked(false);
        } else if (!keepInTray->isChecked()) {
            startMinimized->setChecked(false);
        }
        startMinimized->setEnabled(trayOk && keepInTray->isChecked() &&
                                   startAtLogin->isChecked());
    };
    connect(keepInTray, &QCheckBox::toggled, &dialog, [&](bool) { refreshSessionControls(); });
    connect(startAtLogin, &QCheckBox::toggled, &dialog, [&](bool) { refreshSessionControls(); });
    connect(startMinimized, &QCheckBox::toggled, &dialog, [&](const bool checked) {
        if (checked) keepInTray->setChecked(true);
        refreshSessionControls();
    });
    refreshSessionControls();
    connect(schedule, &QComboBox::currentIndexChanged, &dialog,
            [&](const int) {
                refreshScheduleControls();
                refreshCheckStatus();
            });
    connect(backgroundEnabled, &QCheckBox::toggled, &dialog, [&](bool) {
        refreshScheduleControls();
        refreshCheckStatus();
    });
    connect(weekday, &QComboBox::currentIndexChanged, &dialog, [&](int) { refreshCheckStatus(); });
    connect(checkTime, &QTimeEdit::timeChanged, &dialog, [&](const QTime &) { refreshCheckStatus(); });
    connect(retainedPackages, &QSpinBox::valueChanged, &dialog, [retainedReleases](const int value) {
        if (value < 0) {
            retainedReleases->setValue(-1);
        } else if (retainedReleases->value() >= 0 && retainedReleases->value() < value) {
            retainedReleases->setValue(value);
        }
    });
    connect(checkNow, &QPushButton::clicked, &dialog, [&] {
        if (GuiInstanceServer::requestCheck()) {
            serviceStatus->setText(QStringLiteral("✓ Update check started."));
        } else {
            serviceStatus->setText(QStringLiteral("⚠ Could not reach the running PacSmith session to start a check."));
        }
        refreshCheckStatus();
    });
    auto *checkStatusTimer = new QTimer(&dialog);
    checkStatusTimer->setInterval(2000);
    connect(checkStatusTimer, &QTimer::timeout, &dialog, refreshCheckStatus);
    checkStatusTimer->start();
    refreshScheduleControls();
    refreshCheckStatus();

    QHash<int, QString> modelSelections;
    modelSelections.insert(provider->currentIndex(), aiSettings_.model);
    int previousProvider = provider->currentIndex();
    std::optional<ChatGptCredentials> chatGptSession;
    QString chatGptSerialized;

    auto selectedCredentialSource = [&] {
        return static_cast<CredentialSource>(credentialSource->currentIndex());
    };
    auto loadChatGptSession = [&] {
        chatGptSession.reset();
        chatGptSerialized.clear();
        if (credentialSource->currentIndex() < 0) return;
        const auto source = selectedCredentialSource();
        if (source == CredentialSource::Environment) return;
        if (source == CredentialSource::Age && !credentialStore_.ageUnlocked()) return;
        QString error;
        const auto saved = credentialStore_.load(QStringLiteral("chatgpt"), source, &error);
        if (!saved) return;
        chatGptSerialized = *saved;
        chatGptSession = ChatGptCredentials::fromSerialized(chatGptSerialized, &error);
    };

    auto chatGptIdentity = [&] {
        if (!chatGptSession) return QStringLiteral("Not signed in");
        auto identity = chatGptSession->email.isEmpty()
                            ? QStringLiteral("ChatGPT account %1").arg(chatGptSession->accountId)
                            : chatGptSession->email;
        if (!chatGptSession->planType.isEmpty()) {
            identity += QStringLiteral(" · %1 plan").arg(chatGptSession->planType);
        }
        return QStringLiteral("✓ Signed in as %1").arg(identity);
    };

    auto setCredentialStatus = [&](const QString &text) {
        aiCredentialStatus->setText(text);
    };

    AiReasoningEffort desiredReasoningEffort = aiSettings_.reasoningEffort;
    auto refreshReasoningEfforts = [&] {
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        const auto supported = supportedReasoningEfforts(kind, model->currentText());
        reasoningEffort->blockSignals(true);
        reasoningEffort->clear();
        reasoningEffort->addItem(reasoningEffortLabel(AiReasoningEffort::ProviderDefault),
                                 static_cast<int>(AiReasoningEffort::ProviderDefault));
        for (const auto effort : supported) {
            reasoningEffort->addItem(reasoningEffortLabel(effort), static_cast<int>(effort));
        }
        const auto desiredIndex = reasoningEffort->findData(
            static_cast<int>(desiredReasoningEffort));
        reasoningEffort->setCurrentIndex(desiredIndex >= 0 ? desiredIndex : 0);
        reasoningEffort->setToolTip(
            supported.isEmpty()
                ? QStringLiteral("PacSmith has no verified reasoning-effort controls for this model. The provider default will be used.")
                : QStringLiteral("Controls the model's reasoning.effort request option. Higher effort can increase latency and usage."));
        reasoningEffort->blockSignals(false);
    };

    auto updateControls = [&, this] {
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        const bool api = kind == AiProviderKind::OpenAi || kind == AiProviderKind::Xai;
        const bool subscription = kind == AiProviderKind::ChatGpt;
        const bool storageSelected = credentialSource->currentIndex() >= 0;
        const bool operationRunning = aiModelCatalogService_.isRunning() ||
                                      chatGptLoginService_.isRunning();

        provider->setEnabled(false);
        credentialSource->setEnabled(!operationRunning);
        apiKey->setVisible(api);
        if (auto *label = form->labelForField(apiKey); label != nullptr) label->setVisible(api);
        form->setRowVisible(secretsHint, !storageSelected);
        form->setRowVisible(apiKey, api);
        form->setRowVisible(chatGptSignIn, subscription);
        form->setRowVisible(aiEnvironmentNotice, false);
        secretsForm->setRowVisible(storeNotice, false);
        secretsForm->setRowVisible(ageStoreAction, false);
        githubForm->setRowVisible(githubLockNotice, false);
        ageStoreAction->setVisible(false);
        chatGptSignIn->setVisible(subscription);
        secretsHint->setVisible(!storageSelected);

        const auto aiHelpSummary = QStringLiteral("AI is optional. Local inspection always runs first.");
        auto aiHelpDetails = QStringLiteral(
            "PacSmith always performs local deterministic inspection first and sends only a bounded "
            "package-evidence bundle. Package binaries and unrelated files are never sent.\n\n"
            "Standard uses the provider's normal processing. Fast requests priority processing. "
            "API providers may charge premium per-token rates. ChatGPT availability and usage "
            "limits depend on the subscription.");
        setSettingsSectionHelp(aiSectionHelp, aiHelpSummary, aiHelpDetails);

        if (!storageSelected) {
            apiKey->setEnabled(false);
            ageStoreAction->setEnabled(false);
            chatGptSignIn->setEnabled(false);
            githubToken->setEnabled(false);
            model->setEnabled(false);
            loadModels->setEnabled(false);
            reasoningEffort->setEnabled(false);
            executionMode->setEnabled(false);
            automatic->setEnabled(false);
            saveButton->setEnabled(!operationRunning);
            secretsHint->setText(
                QStringLiteral("Choose how PacSmith stores secrets on the General tab before enabling AI."));
            githubLockNotice->setText(
                QStringLiteral("Choose a secret store above first."));
            githubForm->setRowVisible(githubLockNotice, true);
            setCredentialStatus(
                QStringLiteral("Choose a secret store on the General tab before enabling AI."));
            return;
        }

        const auto source = selectedCredentialSource();
        const bool sourceCompatible = !(subscription && source == CredentialSource::Environment);
        bool storageAvailable = true;
        QString storageError;
        if (source == CredentialSource::Keyring) {
            storageAvailable = CredentialStore::keyringAvailable(&storageError);
        } else if (source == CredentialSource::Age) {
            storageAvailable = CredentialStore::ageAvailable();
            if (!storageAvailable) storageError = QStringLiteral("/usr/bin/age is not installed");
        }
        const bool storagePrepared = storageAvailable &&
                                     (source != CredentialSource::Age ||
                                      credentialStore_.ageUnlocked());
        provider->setEnabled(storagePrepared && !operationRunning);
        if (source == CredentialSource::Age && !credentialStore_.ageUnlocked()) {
            ageStoreAction->setVisible(true);
            secretsForm->setRowVisible(ageStoreAction, true);
            ageStoreAction->setText(
                credentialStore_.hasAgeFile() ? QStringLiteral("Unlock Credential Store…")
                                              : QStringLiteral("Create Encrypted Store…"));
            ageStoreAction->setEnabled(storageAvailable && !operationRunning);
        }
        if (!storageAvailable) {
            storeNotice->setText(QStringLiteral("⚠ Credential storage unavailable: %1").arg(storageError));
            secretsForm->setRowVisible(storeNotice, true);
        }

        if (subscription) loadChatGptSession();
        bool apiCredentialAvailable = false;
        QString apiCredentialError;
        if (api && storagePrepared) {
            if (!apiKey->text().isEmpty()) {
                apiCredentialAvailable = true;
            } else if (source != CredentialSource::Age || credentialStore_.ageUnlocked()) {
                const auto stored = credentialStore_.load(aiProviderName(kind), source,
                                                          &apiCredentialError);
                apiCredentialAvailable = stored.has_value();
            }
        }
        const auto githubEnvironment = source == CredentialSource::Environment;
        githubToken->setEnabled(!githubEnvironment && storagePrepared && !operationRunning);
        githubToken->setPlaceholderText(
            githubEnvironment
                ? QStringLiteral("Read from PACSMITH_GITHUB_TOKEN when PacSmith starts; value is never displayed")
                : QStringLiteral("Optional; leave blank to keep a saved token"));
        if (!storagePrepared) {
            githubLockNotice->setText(QStringLiteral("Unlock or create the secret store first."));
            githubForm->setRowVisible(githubLockNotice, true);
        }
        if (source == CredentialSource::Environment && api) {
            const auto variable = kind == AiProviderKind::Xai ? QStringLiteral("XAI_API_KEY")
                                                               : QStringLiteral("OPENAI_API_KEY");
            apiKey->setPlaceholderText(
                QStringLiteral("Read from %1 when PacSmith starts; value is never displayed")
                    .arg(variable));
            apiKey->setToolTip(
                QStringLiteral("This is a read-only process credential. Set %1 before launching "
                               "pacsmith-gui. PacSmith intentionally does not reveal its value.")
                    .arg(variable));
            aiEnvironmentNotice->setVisible(true);
            form->setRowVisible(aiEnvironmentNotice, true);
            aiEnvironmentNotice->setText(
                apiCredentialAvailable
                    ? QStringLiteral("\u2713 %1 is available in PacSmith's process environment.")
                          .arg(variable)
                    : QStringLiteral("\u26a0 %1 was not found in PacSmith's process environment.")
                          .arg(variable));
        } else {
            apiKey->setPlaceholderText(QStringLiteral("Leave blank to keep an existing stored key"));
            apiKey->setToolTip({});
        }
        const bool credentialReady = subscription ? chatGptSession.has_value()
                                                  : api ? apiCredentialAvailable
                                                        : kind == AiProviderKind::None;
        const bool modelReady = kind != AiProviderKind::None && credentialReady &&
                                !model->currentText().trimmed().isEmpty();

        apiKey->setEnabled(api && source != CredentialSource::Environment && storagePrepared &&
                           !operationRunning);
        automatic->setEnabled(storagePrepared && !operationRunning);
        model->setEnabled(kind != AiProviderKind::None && credentialReady && !operationRunning);
        loadModels->setEnabled((api || subscription) && credentialReady && !operationRunning);
        reasoningEffort->setEnabled(modelReady && reasoningEffort->count() > 1 &&
                                    !operationRunning);
        executionMode->setEnabled(modelReady && !operationRunning);
        saveButton->setEnabled(
            !operationRunning && storagePrepared &&
            (kind == AiProviderKind::None ||
             (credentialReady && !model->currentText().trimmed().isEmpty())));
        chatGptSignIn->setText(chatGptSession ? QStringLiteral("Sign out of ChatGPT…")
                                              : QStringLiteral("Sign in with ChatGPT…"));
        chatGptSignIn->setEnabled(subscription && sourceCompatible && storagePrepared &&
                                  !operationRunning);

        if (subscription) {
            aiHelpDetails += QStringLiteral(
                "\n\nPacSmith opens OpenAI's browser sign-in and never receives your password. "
                "The OAuth session is stored only in PacSmith's selected credential store; "
                "PacSmith does not read Codex, OpenClaw, or any other application's files.");
        } else if (api) {
            aiHelpDetails += QStringLiteral(
                "\n\nEnter or provide the selected API provider's credential before model selection is unlocked.");
            if (source == CredentialSource::Environment) {
                const auto variable = kind == AiProviderKind::Xai ? QStringLiteral("XAI_API_KEY")
                                                                   : QStringLiteral("OPENAI_API_KEY");
                aiHelpDetails += QStringLiteral(
                    "\n\n%1 is read from the process environment when PacSmith starts. "
                    "Desktop launches usually do not inherit variables set in a terminal.")
                                    .arg(variable);
            }
        }
        setSettingsSectionHelp(aiSectionHelp, aiHelpSummary, aiHelpDetails);

        if (!storageAvailable) {
            setCredentialStatus(
                QStringLiteral("⚠ Credential storage unavailable: %1").arg(storageError));
        } else if (!storagePrepared) {
            setCredentialStatus(
                credentialStore_.hasAgeFile()
                    ? QStringLiteral("Unlock PacSmith's encrypted credential store on the General tab before entering secrets.")
                    : QStringLiteral("Create PacSmith's encrypted credential store on the General tab before entering secrets."));
        } else if (kind == AiProviderKind::None) {
            setCredentialStatus(QStringLiteral("Secret store is ready. Choose an AI provider or save."));
        } else if (!sourceCompatible) {
            setCredentialStatus(
                QStringLiteral("⚠ ChatGPT browser login cannot use environment-variable storage. Choose Desktop keyring or Age-encrypted file."));
        } else if (credentialReady && subscription) {
            setCredentialStatus(chatGptIdentity() +
                                      QStringLiteral(". Model selection is unlocked."));
        } else if (credentialReady) {
            setCredentialStatus(
                QStringLiteral("✓ Provider credential is available. Model selection is unlocked."));
        } else if (subscription) {
            setCredentialStatus(
                QStringLiteral("Sign in with ChatGPT before selecting a model."));
        } else if (source == CredentialSource::Environment) {
            setCredentialStatus(
                QStringLiteral("Waiting for %1 before model selection can be unlocked.")
                    .arg(kind == AiProviderKind::Xai ? QStringLiteral("XAI_API_KEY")
                                                     : QStringLiteral("OPENAI_API_KEY")));
        } else {
            setCredentialStatus(
                apiCredentialError.isEmpty()
                    ? QStringLiteral("Enter an API key before selecting a model.")
                    : QStringLiteral("Enter an API key before selecting a model. %1")
                          .arg(apiCredentialError));
        }
    };
    connect(model, &QComboBox::currentTextChanged, &dialog, [&](const QString &) {
        modelSelections.insert(provider->currentIndex(), model->currentText().trimmed());
        refreshReasoningEfforts();
        updateControls();
    });
    connect(reasoningEffort, &QComboBox::currentIndexChanged, &dialog, [&](const int index) {
        if (index >= 0) {
            desiredReasoningEffort = static_cast<AiReasoningEffort>(
                reasoningEffort->itemData(index).toInt());
        }
    });
    connect(executionMode, &QComboBox::currentIndexChanged, &dialog,
            [&](const int) { updateControls(); });
    connect(provider, &QComboBox::currentIndexChanged, &dialog, [&](const int index) {
        modelSelections.insert(previousProvider, model->currentText().trimmed());
        previousProvider = index;
        model->clear();
        model->setEditText(modelSelections.value(index));
        refreshReasoningEfforts();
        updateControls();
    });
    connect(credentialSource, &QComboBox::currentIndexChanged, &dialog, updateControls);
    connect(apiKey, &QLineEdit::textChanged, &dialog,
            [&](const QString &) { updateControls(); });
    connect(ageStoreAction, &QPushButton::clicked, &dialog, [&, this] {
        if (!unlockAgeCredentials()) return;
        loadChatGptSession();
        updateControls();
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        if (chatGptSession) {
            setCredentialStatus(chatGptIdentity() + QStringLiteral(". Model selection is unlocked."));
        } else if (kind == AiProviderKind::ChatGpt) {
            setCredentialStatus(
                QStringLiteral("✓ Encrypted credential store is ready. Sign in with ChatGPT next."));
        } else if (kind == AiProviderKind::OpenAi || kind == AiProviderKind::Xai) {
            setCredentialStatus(
                QStringLiteral("✓ Encrypted credential store is ready. Enter the API key next."));
        } else {
            setCredentialStatus(
                QStringLiteral("✓ Encrypted credential store is ready. Choose an AI provider next."));
        }
    });
    connect(chatGptSignIn, &QPushButton::clicked, &dialog, [&, this] {
        if (chatGptSession) {
            if (QMessageBox::question(&dialog, QStringLiteral("Sign out of ChatGPT"),
                                      QStringLiteral("Remove PacSmith's saved ChatGPT session?")) !=
                QMessageBox::Yes) {
                return;
            }
            const auto source = selectedCredentialSource();
            if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() &&
                !unlockAgeCredentials()) {
                return;
            }
            QString error;
            if (!credentialStore_.remove(QStringLiteral("chatgpt"), source,
                                         QString{},
                                         &error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not sign out"), error);
                return;
            }
            chatGptSession.reset();
            chatGptSerialized.clear();
            updateControls();
            return;
        }

        const auto source = selectedCredentialSource();
        if (source == CredentialSource::Environment) {
            setCredentialStatus(
                QStringLiteral("ChatGPT sessions must be stored in PacSmith's keyring or age file"));
            return;
        }
        if (source == CredentialSource::Age && !credentialStore_.ageUnlocked()) {
            if (!unlockAgeCredentials()) return;
            loadChatGptSession();
            updateControls();
            if (chatGptSession) {
                setCredentialStatus(
                    chatGptIdentity() + QStringLiteral(". Loading available models…"));
                aiModelCatalogService_.fetch(AiProviderKind::ChatGpt, chatGptSerialized);
                return;
            }
        }
        setCredentialStatus(QStringLiteral("Starting secure ChatGPT browser sign-in…"));
        chatGptSignIn->setEnabled(false);
        chatGptLoginService_.start();
    });
    connect(&chatGptLoginService_, &ChatGptLoginService::authorizationUrlReady, &dialog,
            [&, this](const QUrl &url) {
        if (!QDesktopServices::openUrl(url)) {
            setCredentialStatus(
                QStringLiteral("Open this OpenAI sign-in URL in your browser: %1").arg(url.toString()));
            aiCredentialStatus->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
        }
    });
    connect(&chatGptLoginService_, &ChatGptLoginService::progressChanged, &dialog,
            [&](const QString &message) {
        updateControls();
        setCredentialStatus(message);
    });
    connect(&chatGptLoginService_, &ChatGptLoginService::failed, &dialog,
            [&, this](const QString &message) {
        updateControls();
        setCredentialStatus(QStringLiteral("⚠ %1").arg(message));
    });
    connect(&chatGptLoginService_, &ChatGptLoginService::succeeded, &dialog,
            [&, this](const QString &serialized) {
        const auto source = selectedCredentialSource();
        QString error;
        if (!credentialStore_.store(QStringLiteral("chatgpt"), source, serialized,
                                    QString{}, &error)) {
            setCredentialStatus(
                QStringLiteral("⚠ Signed in, but PacSmith could not save the session: %1").arg(error));
            return;
        }
        chatGptSerialized = serialized;
        chatGptSession = ChatGptCredentials::fromSerialized(serialized, &error);
        if (!chatGptSession) {
            setCredentialStatus(QStringLiteral("⚠ %1").arg(error));
            return;
        }
        updateControls();
        aiModelCatalogService_.fetch(AiProviderKind::ChatGpt, chatGptSerialized);
    });
    connect(loadModels, &QPushButton::clicked, &dialog, [&, this] {
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        if (kind == AiProviderKind::None) return;
        const auto name = aiProviderName(kind);
        const auto source = static_cast<CredentialSource>(credentialSource->currentIndex());
        QString credential;
        if (kind == AiProviderKind::ChatGpt && chatGptSession) {
            credential = chatGptSerialized;
        } else if (source != CredentialSource::Environment && !apiKey->text().isEmpty()) {
            credential = apiKey->text();
        } else {
            if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() &&
                !unlockAgeCredentials()) {
                return;
            }
            QString error;
            const auto existing = credentialStore_.load(name, source, &error);
            if (!existing) {
                setCredentialStatus(QStringLiteral("⚠ %1").arg(error));
                return;
            }
            credential = *existing;
        }
        aiModelCatalogService_.fetch(kind, credential);
        credential.fill(QChar::Null);
    });
    connect(&aiModelCatalogService_, &AiModelCatalogService::progressChanged, &dialog,
            [&](const QString &message) {
                updateControls();
                setCredentialStatus(message);
            });
    connect(&aiModelCatalogService_, &AiModelCatalogService::credentialUpdated, &dialog,
            [&, this](const QString &serialized) {
        const auto source = selectedCredentialSource();
        QString error;
        if (credentialStore_.store(QStringLiteral("chatgpt"), source, serialized,
                                   QString{}, &error)) {
            chatGptSerialized = serialized;
            chatGptSession = ChatGptCredentials::fromSerialized(serialized);
        } else {
            setCredentialStatus(
                QStringLiteral("⚠ Could not save the refreshed ChatGPT session: %1").arg(error));
        }
    });
    connect(&aiModelCatalogService_, &AiModelCatalogService::finished, &dialog,
            [&](const QStringList &models) {
                const auto desired = model->currentText().trimmed();
                model->clear();
                model->addItems(models);
                if (!desired.isEmpty()) {
                    const auto index = model->findText(desired);
                    if (index >= 0) model->setCurrentIndex(index);
                    else model->setEditText(desired);
                } else if (!models.isEmpty()) {
                    model->setCurrentIndex(0);
                }
                updateControls();
                setCredentialStatus(
                    QStringLiteral("✓ Loaded %1 model(s) directly from %2")
                        .arg(models.size()).arg(aiProviderName(
                            static_cast<AiProviderKind>(provider->currentIndex()))));
            });
    connect(&aiModelCatalogService_, &AiModelCatalogService::failed, &dialog,
            [&](const QString &message) {
                updateControls();
                setCredentialStatus(QStringLiteral("⚠ %1").arg(message));
            });
    connect(&dialog, &QDialog::finished, &aiModelCatalogService_, &AiModelCatalogService::cancel);
    connect(&dialog, &QDialog::finished, &chatGptLoginService_, &ChatGptLoginService::cancel);
    refreshReasoningEfforts();
    updateControls();

    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto kind = static_cast<AiProviderKind>(provider->currentIndex());
        const auto name = aiProviderName(kind);
        if (credentialSource->currentIndex() < 0) {
            aiSettings_.provider = AiProviderKind::None;
            aiSettings_.model.clear();
            aiSettings_.reasoningEffort = AiReasoningEffort::ProviderDefault;
            aiSettings_.executionMode = AiExecutionMode::Standard;
            aiSettings_.automaticallyResolveReviewItems = automatic->isChecked();
            QString error;
            if (!persistBackgroundSettings(&error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not save settings"), error);
                return;
            }
            const bool runInTray = aiSettings_.updates.keepInTray &&
                                   QSystemTrayIcon::isSystemTrayAvailable();
            setKeepRunningInTray(runInTray);
            QApplication::setQuitOnLastWindowClosed(!runInTray);
            static_cast<void>(GuiInstanceServer::requestTray());
            dialog.accept();
            return;
        }
        const auto source = static_cast<CredentialSource>(credentialSource->currentIndex());
        if ((kind == AiProviderKind::OpenAi || kind == AiProviderKind::Xai) &&
            source != CredentialSource::Environment && !apiKey->text().isEmpty()) {
            if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() &&
                !unlockAgeCredentials()) {
                return;
            }
            QString error;
            if (!credentialStore_.store(name, source, apiKey->text(), {}, &error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not store API key"), error);
                return;
            }
        }
        if (kind == AiProviderKind::ChatGpt && !chatGptSession) {
            QMessageBox::warning(&dialog, QStringLiteral("ChatGPT sign-in required"),
                                 QStringLiteral("Sign in to ChatGPT before selecting the subscription provider."));
            return;
        }
        aiSettings_.provider = kind;
        aiSettings_.model = kind == AiProviderKind::None ? QString{} : model->currentText().trimmed();
        aiSettings_.reasoningEffort = kind == AiProviderKind::None
                                          ? AiReasoningEffort::ProviderDefault
                                          : static_cast<AiReasoningEffort>(
                                                reasoningEffort->currentData().toInt());
        aiSettings_.executionMode = kind == AiProviderKind::None
                                        ? AiExecutionMode::Standard
                                        : static_cast<AiExecutionMode>(
                                              executionMode->currentData().toInt());
        aiSettings_.automaticallyResolveReviewItems = automatic->isChecked();
        if (kind == AiProviderKind::ChatGpt || kind == AiProviderKind::OpenAi ||
            kind == AiProviderKind::Xai) {
            aiSettings_.credentialSources.insert(name, source);
        }
        QString error;
        if (!persistBackgroundSettings(&error)) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not save settings"), error);
            return;
        }
        const bool runInTray = aiSettings_.updates.keepInTray &&
                               QSystemTrayIcon::isSystemTrayAvailable();
        setKeepRunningInTray(runInTray);
        QApplication::setQuitOnLastWindowClosed(!runInTray);
        static_cast<void>(GuiInstanceServer::requestTray());
        dialog.accept();
    });
    dialog.exec();
}

void MainWindow::startReanalysis() {
    if (!project_ || currentRelease() == nullptr || importThread_ != nullptr ||
        buildService_.isRunning() || installService_.isRunning() ||
        debDownloadService_.isRunning() || aiService_.isRunning()) {
        return;
    }
    const auto projectId = project_->id;
    const auto releaseId = currentRelease()->id;
    const auto version = currentRelease()->debian.version;
    if (!std::filesystem::is_regular_file(store_.sourcePath(*currentRelease()))) {
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
        store_.projectsRoot(), projectId, releaseId);
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
    if (!project_ || aiService_.isRunning()) return;
    if (aiSettings_.provider == AiProviderKind::None) {
        QMessageBox::information(this, QStringLiteral("Configure AI"),
                                 QStringLiteral("Choose an AI provider with the Settings button first."));
        showSettings();
        return;
    }
    QString credential;
    if (aiSettings_.provider == AiProviderKind::ChatGpt ||
        aiSettings_.provider == AiProviderKind::OpenAi || aiSettings_.provider == AiProviderKind::Xai) {
        const auto name = aiProviderName(aiSettings_.provider);
        const auto defaultSource = aiSettings_.provider == AiProviderKind::ChatGpt
                                       ? CredentialSource::Keyring
                                       : CredentialSource::Environment;
        const auto source = aiSettings_.credentialSources.value(name, defaultSource);
        if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() && !unlockAgeCredentials()) return;
        QString error;
        const auto loaded = credentialStore_.load(name, source, &error);
        if (!loaded) {
            QMessageBox::critical(this, QStringLiteral("AI credential unavailable"), error);
            return;
        }
        credential = *loaded;
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
        if (aiService_.isRunning()) {
            aiProgressCanceled_ = true;
            aiService_.cancel();
            return;
        }
        progressDialog->deleteLater();
        aiProgress_ = nullptr;
        projectList_->setEnabled(true);
        updateDeleteButton();
    });
    projectList_->setEnabled(false);
    resolveWithAiButton_->setEnabled(false);
    aiService_.start(*currentRelease(), aiSettings_, credential);
    credential.fill(QChar::Null);
    updateDeleteButton();
}

void MainWindow::startGithubRegexAi() {
    auto *tracker = updateEditorRelease();
    if (!project_ || tracker == nullptr || aiService_.isRunning()) return;
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

    QString githubToken;
    const auto githubCredentialSource = aiSettings_.credentialSources.value(
        QStringLiteral("github"), CredentialSource::Environment);
    if (githubCredentialSource == CredentialSource::Age && !credentialStore_.ageUnlocked() &&
        !unlockAgeCredentials()) return;
    githubToken = credentialStore_.load(QStringLiteral("github"), githubCredentialSource, nullptr)
                      .value_or(QString{});

    auto *service = new GitHubUpdateService(this);
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

        const auto providerName = aiProviderName(aiSettings_.provider);
        const auto defaultSource = aiSettings_.provider == AiProviderKind::ChatGpt
                                       ? CredentialSource::Keyring
                                       : CredentialSource::Environment;
        const auto source = aiSettings_.credentialSources.value(providerName, defaultSource);
        if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() &&
            !unlockAgeCredentials()) return;
        QString credentialError;
        auto credential = credentialStore_.load(providerName, source, &credentialError);
        if (!credential) {
            QMessageBox::critical(this, QStringLiteral("AI credential unavailable"), credentialError);
            return;
        }
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
            if (aiService_.isRunning()) {
                aiProgressCanceled_ = true;
                aiService_.cancel();
                return;
            }
            progressDialog->deleteLater();
            aiProgress_ = nullptr;
            projectList_->setEnabled(true);
            updateDeleteButton();
        });
        projectList_->setEnabled(false);
        githubRegexAiButton_->setEnabled(false);
        aiService_.startGitHubAssetRule(probe, assets, preferred, aiSettings_, *credential);
        credential->fill(QChar::Null);
        updateDeleteButton();
    });
    service->start(probe, githubToken);
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
    if (aiProgress_ != nullptr) {
        aiProgress_->hide();
        aiProgress_->deleteLater();
        aiProgress_ = nullptr;
    }
    projectList_->setEnabled(true);
    updateDeleteButton();
    applyAiResolution(resolution);
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
    if (!currentRelease()->lifecycleScript.contents.isEmpty()) {
        QString error;
        if (!store_.saveLifecycle(*project_, *currentRelease(), &error)) {
            QMessageBox::critical(this, QStringLiteral("Could not save generated lifecycle script"), error);
            return;
        }
    }
    if (currentRelease()->update.strategy == UpdateStrategy::Manual && !currentRelease()->update.url.isEmpty() &&
        !currentRelease()->update.aptSuite.isEmpty()) {
        currentRelease()->update.strategy = UpdateStrategy::AptRepository;
    }
    currentRelease()->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("ai-resolution"),
                              QStringLiteral("Applied %1 change(s) from %2/%3")
                                  .arg(resolution.changes.size()).arg(resolution.provider, resolution.model)});
    refreshGeneratedPkgbuildAfterModelChange();
    if (!persistCurrent()) return;

    // Refreshing the sidebar reloads the project and normally returns to the dashboard.
    // Restore the reviewed release and deliberately advance to its first remaining step.
    refreshProjectList(projectId);
    if (!project_ || project_->id != projectId) loadProject(projectId);
    if (!project_ || project_->release(releaseId) == nullptr) {
        QMessageBox::critical(
            this, QStringLiteral("AI review completed"),
            QStringLiteral("The AI result was saved, but PacSmith could not reopen the reviewed release."));
        return;
    }
    currentReleaseId_ = releaseId;
    showReleaseWorkbenchAtFirstAttention(releaseId);

    QString stepName = sectionTitle(currentSection());
    const bool readyToBuild = currentSection() == EditorSection::ResultBuild;
    const auto appliedSummary = applied.changed
        ? QStringLiteral("PacSmith applied the accepted AI recommendations.")
        : QStringLiteral("The AI review completed without an accepted configuration change.");
    const auto nextStep = readyToBuild
        ? QStringLiteral("No review step remains. PacSmith opened Build; review the readiness checklist, then build the package.")
        : QStringLiteral("PacSmith opened %1, the first section that still needs your attention. "
                         "Resolve the highlighted items, then continue through PKGBUILD to Build.")
              .arg(stepName);
    if (!applied.errors.isEmpty()) {
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
}

void MainWindow::applyRetentionCleanup() {
    if (!project_ || !aiSettings_.updates.automaticallyPrepare) return;
    QString error;
    const auto result = store_.cleanup(
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
    if (!project_ || aptUpdateService_.isRunning() || rpmUpdateService_.isRunning() ||
        githubUpdateService_.isRunning() ||
        debDownloadService_.isRunning() || importThread_ != nullptr) return;
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
    if (backgroundState.checking && !aptUpdateService_.isRunning() &&
        !rpmUpdateService_.isRunning() && !githubUpdateService_.isRunning()) {
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
        aptUpdateService_.start(*tracker, store_.releasePath(*tracker));
    } else if (strategy == UpdateStrategy::RpmRepository) {
        updateCheckStatus_->setText(QStringLiteral("Starting RPM repository check…"));
        rpmUpdateService_.start(*tracker, store_.releasePath(*tracker));
    } else {
        QString token;
        const auto source = aiSettings_.credentialSources.value(
            QStringLiteral("github"), CredentialSource::Environment);
        if (source == CredentialSource::Age && !credentialStore_.ageUnlocked() && !unlockAgeCredentials()) {
            projectList_->setEnabled(true);
            updateCheckReleaseId_.clear();
            updateCheckFromWorkbench_ = false;
            populateUpdates();
            return;
        }
        QString credentialError;
        const auto loaded = credentialStore_.load(QStringLiteral("github"), source, &credentialError);
        if (loaded) token = *loaded;
        updateCheckStatus_->setText(QStringLiteral("Starting GitHub release check…"));
        githubUpdateService_.start(*tracker, token);
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
        if (const auto *discovered = store_.recordDiscoveredRelease(
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
        if (!remainInWorkbench && discoveredState != ReleaseState::Discovered) {
            showReleaseWorkbenchAtFirstAttention(discoveredReleaseId);
            return;
        }
        if (aiSettings_.updates.automaticallyPrepare) {
            beginReleasePreparation(discoveredReleaseId, false);
            return;
        }

        QMessageBox available(QMessageBox::Warning, QStringLiteral("Update available"),
            QStringLiteral("%1 %2 is available. Download and inspect the vendor artifact now?\n\n"
                           "You can choose Later; PacSmith will keep the update alert in the package list and the release in Version History.")
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
        if (commandProgress_ != dialog || !buildService_.isRunning()) return;
        buildService_.cancel();
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

void MainWindow::startBuild() {
    if (!project_ || buildService_.isRunning()) return;
    bool lifecycleChanged = false;
    QString lifecycleError;
    if (!store_.synchronizeLifecycle(*project_, *currentRelease(), &lifecycleChanged, &lifecycleError)) {
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
    const auto lifecycleReference = pkgbuildLifecycleReference(pkgbuildText);
    if (!lifecycleReference.isEmpty() &&
        (lifecycleReference.contains(QLatin1Char('/')) ||
         (!lifecycleReference.contains(QLatin1Char('$')) &&
          !QFileInfo(QString::fromUtf8(
              (store_.releasePath(*currentRelease()) /
               std::filesystem::path(lifecycleReference.toUtf8().constData())).string().c_str()))
               .isFile()))) {
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
                           "Add install='%1' in Custom mode or switch back to Guided before building.")
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
    if (projectSidebar_ != nullptr) projectSidebar_->hide();
    rightStack_->setCurrentIndex(1);
    selectSection(EditorSection::ResultBuild);
    showCommandProgress(QStringLiteral("Building %1").arg(project_->displayName),
                        QStringLiteral("Running makepkg…"), true);
    buildService_.start(store_.releasePath(*currentRelease()));
    populateBuild();
    updateDeleteButton();
}

void MainWindow::startInstall() {
    if (!project_ || installService_.isRunning()) return;
    bool lifecycleChanged = false;
    QString lifecycleError;
    if (!store_.synchronizeLifecycle(*project_, *currentRelease(), &lifecycleChanged, &lifecycleError)) {
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
    const auto package = retainedPackagePath(store_, *currentRelease());
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

QString MainWindow::selectedDashboardReleaseId() const {
    if (releaseTable_ == nullptr || releaseTable_->currentRow() < 0) return {};
    const auto *item = releaseTable_->item(releaseTable_->currentRow(), 0);
    return item == nullptr ? QString{} : item->data(Qt::UserRole).toString();
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
    if (!project_ || debDownloadService_.isRunning() || importThread_ != nullptr) return;
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
    debDownloadService_.start(QUrl(release->sourceUrl), release->sourceSha256,
                              std::filesystem::path(target.toUtf8().constData()));
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
    if (!store_.deleteRelease(*project_, id, &error)) {
        QMessageBox::critical(this, QStringLiteral("Could not delete release"), error);
        return;
    }
    if (currentReleaseId_ == id) {
        const auto *fallback = project_->activeTrackingRelease();
        if (fallback == nullptr) fallback = project_->newestRelease();
        currentReleaseId_ = fallback == nullptr ? QString{} : fallback->id;
    }
    refreshCurrentProject();
}

void MainWindow::rollbackSelectedRelease() {
    if (!project_ || installService_.isRunning()) return;
    const auto id = selectedDashboardReleaseId();
    const auto *release = project_->release(id);
    if (release == nullptr || id == project_->installedReleaseId) return;
    const auto package = retainedPackagePath(store_, *release);
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
    if (release == nullptr || retainedPackagePath(store_, *release).isEmpty()) return;
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

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) {
        for (const auto &url : event->mimeData()->urls()) {
            if (url.isLocalFile() || url.scheme() == QStringLiteral("https")) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    if (event->mimeData()->hasText()) {
        const QUrl url(event->mimeData()->text().trimmed());
        if (url.scheme() == QStringLiteral("https")) {
            event->acceptProposedAction();
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event) {
    for (const auto &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            event->acceptProposedAction();
            importPackage(url.toLocalFile());
            return;
        }
        if (url.scheme() == QStringLiteral("https")) {
            event->acceptProposedAction();
            importPackage(url.toString());
            return;
        }
    }
    if (event->mimeData()->hasText()) {
        const QUrl url(event->mimeData()->text().trimmed());
        if (url.scheme() == QStringLiteral("https")) {
            event->acceptProposedAction();
            importPackage(url.toString());
        }
    }
}

} // namespace pacsmith::gui
