#include "gui/main_window/common.hpp"

namespace pacsmith::gui {

std::optional<RepositorySourceChoice> choosePPAPackage(
    QWidget *parent, const QJsonObject &source) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Choose Ubuntu Release"));
    dialog.resize(560, 240);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(
        QStringLiteral("%1\n%2\nPackage: %3\nArchitecture: %4")
            .arg(source.value(QStringLiteral("display_name")).toString(),
                 source.value(QStringLiteral("ppa")).toString(),
                 source.value(QStringLiteral("package")).toString(),
                 source.value(QStringLiteral("architecture")).toString()), &dialog);
    description->setTextFormat(Qt::PlainText);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *suite = new QComboBox(&dialog);
    suite->setObjectName(QStringLiteral("ppaSuite"));
    for (const auto &name : source.value(QStringLiteral("suites")).toArray()) {
        suite->addItem(name.toString());
    }
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Ubuntu release"), suite);
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
    buttons->button(QDialogButtonBox::Ok)->setEnabled(suite->count() > 0);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    RepositorySourceChoice choice;
    choice.update.strategy = UpdateStrategy::AptRepository;
    choice.update.url = source.value(QStringLiteral("repository_url")).toString();
    choice.update.aptSuite = suite->currentText();
    choice.update.aptComponent = QStringLiteral("main");
    choice.update.aptArchitecture = source.value(QStringLiteral("architecture")).toString();
    choice.update.aptPackageName = source.value(QStringLiteral("package")).toString();
    choice.update.trustedSigningFingerprint =
        source.value(QStringLiteral("signing_fingerprint")).toString();
    choice.signingKeyUrl = QUrl(source.value(QStringLiteral("signing_key_url")).toString());
    return choice;
}

void MainWindow::importPPA() {
    if (repositoryImportRunning_ || serverImportRunning_ || importThread_ != nullptr) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New Project from Ubuntu PPA"));
    dialog.resize(560, 160);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *ppaInput = new QLineEdit(&dialog);
    ppaInput->setPlaceholderText(QStringLiteral("ppa:ubuntugis/ubuntugis-unstable"));
    auto *packageInput = new QLineEdit(&dialog);
    packageInput->setPlaceholderText(QStringLiteral("grass-core"));
    form->addRow(QStringLiteral("PPA"), ppaInput);
    form->addRow(QStringLiteral("Package"), packageInput);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Find Ubuntu Releases"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);
    const auto validate = [ppaInput, packageInput, buttons] {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
            !ppaInput->text().trimmed().isEmpty() && !packageInput->text().trimmed().isEmpty());
    };
    connect(ppaInput, &QLineEdit::textChanged, &dialog, validate);
    connect(packageInput, &QLineEdit::textChanged, &dialog, validate);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto ppa = ppaInput->text().trimmed();
    const auto package = packageInput->text().trimmed();
    repositoryImportRunning_ = true;
    auto *progress = new QProgressDialog(
        QStringLiteral("Looking up this package's published Ubuntu releases on Launchpad…"),
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
    watcher->setFuture(QtConcurrent::run([config, ppa, package, architecture] {
        LibraryClient client(config);
        Result result;
        result.first = client.resolvePPA(ppa, package, architecture, &result.second);
        return result;
    }));
}

} // namespace pacsmith::gui
