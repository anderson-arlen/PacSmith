#include "gui/main_window/common.hpp"

namespace pacsmith::gui {

std::optional<RepositorySourceChoice> choosePPAPackage(
    QWidget *parent, const QJsonObject &source) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Choose PPA Package"));
    dialog.resize(560, 240);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(
        QStringLiteral("%1\n%2\nArchitecture: %3")
            .arg(source.value(QStringLiteral("display_name")).toString(),
                 source.value(QStringLiteral("ppa")).toString(),
                 source.value(QStringLiteral("architecture")).toString()), &dialog);
    description->setTextFormat(Qt::PlainText);
    description->setWordWrap(true);
    layout->addWidget(description);
    const auto packages = source.value(QStringLiteral("packages")).toObject();
    auto *suite = new QComboBox(&dialog);
    suite->setObjectName(QStringLiteral("ppaSuite"));
    suite->addItems(packages.keys());
    auto *package = new QComboBox(&dialog);
    package->setObjectName(QStringLiteral("ppaPackage"));
    package->setEditable(true);
    package->setInsertPolicy(QComboBox::NoInsert);
    package->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    package->completer()->setFilterMode(Qt::MatchContains);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Ubuntu release"), suite);
    form->addRow(QStringLiteral("Package"), package);
    layout->addLayout(form);
    auto *help = new QLabel(
        QStringLiteral("Choose the Ubuntu release whose package you want to import. "
                       "PacSmith will review its dependencies for Arch compatibility."), &dialog);
    help->setWordWrap(true);
    layout->addWidget(help);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                        &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Review Signing Key"));
    layout->addWidget(buttons);
    const auto validate = [package, buttons] {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            package->findText(package->currentText(), Qt::MatchExactly) >= 0);
    };
    const auto updatePackages = [suite, package, packages, validate] {
        package->clear();
        for (const auto &name : packages.value(suite->currentText()).toArray()) {
            package->addItem(name.toString());
        }
        package->setCurrentIndex(-1);
        package->setEditText({});
        validate();
    };
    QObject::connect(suite, &QComboBox::currentTextChanged, &dialog, updatePackages);
    QObject::connect(package, &QComboBox::editTextChanged, &dialog, validate);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    updatePackages();
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    RepositorySourceChoice choice;
    choice.update.strategy = UpdateStrategy::AptRepository;
    choice.update.url = source.value(QStringLiteral("repository_url")).toString();
    choice.update.aptSuite = suite->currentText();
    choice.update.aptComponent = QStringLiteral("main");
    choice.update.aptArchitecture = source.value(QStringLiteral("architecture")).toString();
    choice.update.aptPackageName = package->currentText();
    choice.update.trustedSigningFingerprint =
        source.value(QStringLiteral("signing_fingerprint")).toString();
    choice.signingKeyUrl = QUrl(source.value(QStringLiteral("signing_key_url")).toString());
    return choice;
}

void MainWindow::importPPA() {
    if (repositoryImportRunning_ || serverImportRunning_ || importThread_ != nullptr) return;
    bool accepted = false;
    const auto ppa = QInputDialog::getText(
        this, QStringLiteral("New Project from Ubuntu PPA"),
        QStringLiteral("Enter the PPA name, for example ppa:ubuntugis/ubuntugis-unstable"),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || ppa.isEmpty()) return;
    repositoryImportRunning_ = true;
    auto *progress = new QProgressDialog(
        QStringLiteral("Looking up the PPA and its published packages on Launchpad…"),
        QString{}, 0, 0, this);
    progress->setWindowTitle(QStringLiteral("Ubuntu PPA"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setCancelButton(nullptr);
    progress->setMinimumDuration(0);
    progress->show();
    using Result = std::pair<std::optional<QJsonObject>, QString>;
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, progress] {
        const auto result = watcher->result();
        watcher->deleteLater();
        progress->close();
        progress->deleteLater();
        repositoryImportRunning_ = false;
        if (!result.first) {
            QMessageBox::critical(this, QStringLiteral("Could not find PPA"), result.second);
            return;
        }
        const auto choice = choosePPAPackage(this, *result.first);
        if (choice) beginRepositoryImport(choice->update, choice->signingKeyUrl);
    });
    const auto config = library_.config();
    const auto architecture = repositoryArchitecture(true);
    watcher->setFuture(QtConcurrent::run([config, ppa, architecture] {
        LibraryClient client(config);
        Result result;
        result.first = client.resolvePPA(ppa, architecture, &result.second);
        return result;
    }));
}

} // namespace pacsmith::gui
