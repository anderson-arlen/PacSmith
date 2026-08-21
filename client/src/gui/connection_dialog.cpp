#include "gui/connection_dialog.hpp"

#include "core/daemon_control.hpp"
#include "core/enrollment.hpp"
#include "core/library_client.hpp"
#include "gui/main_window/help_widgets.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPair>
#include <QProcess>
#include <QProgressDialog>
#include <QRadioButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace pacsmith::gui {
namespace {

QString statusFieldLabel(const QString &key) {
    if (key == QLatin1String("mode")) return QStringLiteral("Mode");
    if (key == QLatin1String("socket")) return QStringLiteral("Socket");
    if (key == QLatin1String("url")) return QStringLiteral("URL");
    if (key == QLatin1String("enrolled")) return QStringLiteral("Enrolled");
    if (key == QLatin1String("reachable")) return QStringLiteral("Reachable");
    if (key == QLatin1String("health")) return QStringLiteral("Health");
    if (key == QLatin1String("database")) return QStringLiteral("Database");
    if (key == QLatin1String("api_version")) return QStringLiteral("API");
    if (key == QLatin1String("server_version")) return QStringLiteral("Server");
    if (key == QLatin1String("fingerprint")) return QStringLiteral("Fingerprint");
    if (key == QLatin1String("fingerprint_sha256")) return QStringLiteral("SHA-256");
    if (key == QLatin1String("secret_backend")) return QStringLiteral("Secrets");
    if (key == QLatin1String("pki_ready")) return QStringLiteral("PKI");
    if (key == QLatin1String("enabled")) return QStringLiteral("HTTPS listen");
    if (key == QLatin1String("port")) return QStringLiteral("Listen port");
    if (key == QLatin1String("hosts")) return QStringLiteral("Interfaces");
    if (key == QLatin1String("bound")) return QStringLiteral("Bound");
    if (key == QLatin1String("error")) return QStringLiteral("Error");
    if (key == QLatin1String("server")) return QStringLiteral("Server");
    return key;
}

QString statusFieldValue(const QString &key, const QString &value) {
    if (key == QLatin1String("mode")) {
        if (value == QLatin1String("local")) return QStringLiteral("Local");
        if (value == QLatin1String("remote")) return QStringLiteral("Remote");
    }
    if (key == QLatin1String("reachable")) {
        if (value == QLatin1String("true")) return QStringLiteral("Yes");
        if (value == QLatin1String("false")) return QStringLiteral("No");
    }
    if (key == QLatin1String("enrolled")) {
        if (value == QLatin1String("true")) return QStringLiteral("Yes");
        if (value == QLatin1String("false")) return QStringLiteral("No");
    }
    if (key == QLatin1String("pki_ready")) {
        if (value == QLatin1String("true")) return QStringLiteral("Ready");
        if (value == QLatin1String("false")) return QStringLiteral("Not ready");
    }
    if (key == QLatin1String("enabled")) {
        if (value == QLatin1String("true")) return QStringLiteral("On");
        if (value == QLatin1String("false")) return QStringLiteral("Off");
    }
    if (key == QLatin1String("secret_backend")) {
        if (value == QLatin1String("secret-service")) return QStringLiteral("Desktop Secret Service");
        if (value == QLatin1String("file")) return QStringLiteral("Protected file on the library host");
        if (value == QLatin1String("env")) return QStringLiteral("Process environment on the library host");
        if (value.isEmpty()) return QStringLiteral("Not initialized");
    }
    if ((key == QLatin1String("bound") || key == QLatin1String("hosts")) && value.isEmpty()) {
        return QStringLiteral("—");
    }
    return value;
}

void fillStatusForm(QFormLayout *form, const QList<QPair<QString, QString>> &rows) {
    while (form->rowCount() > 0) form->removeRow(0);
    for (const auto &row : rows) {
        auto *value = new QLabel(statusFieldValue(row.first, row.second), form->parentWidget());
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        form->addRow(statusFieldLabel(row.first), value);
    }
}

} // namespace

void restartPacsmithGui() {
    QStringList args = QCoreApplication::arguments();
    if (!args.isEmpty()) args.removeFirst();
    QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
    QApplication::quit();
}

bool runConnectionDialog(QWidget *parent) {
    const auto current = ConnectionConfig::load();
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Library connection"));
    dialog.setMinimumWidth(520);
    auto *root = new QVBoxLayout(&dialog);
    root->setSpacing(12);
    auto *intro = settingsSectionHelp(
        &dialog,
        QStringLiteral("PacSmith can work with your packages locally or even through a remote host. "
                       "Which mode would you like to use on this machine?"),
        QStringLiteral(
            "PacSmith keeps your package library in a background service named pacsmithd. "
            "The GUI and CLI are clients, and they talk to one library at a time.\n\n"
            "Local mode starts pacsmithd on this computer as your systemd user service and connects "
            "over a Unix socket that only your account on this machine can use. That is the default, "
            "and it is the only way to approve remote clients or change whether this library accepts "
            "network connections.\n\n"
            "Remote mode connects to pacsmithd on another computer over HTTPS with mutual TLS. This "
            "machine does not run a local daemon in that mode. The library host must already have "
            "HTTPS listening enabled. During enrollment you confirm the host fingerprint, this client "
            "generates its own key, and an administrator on that host approves the request. PacSmith "
            "pins that host's certificate authority instead of trusting the operating system store.\n\n"
            "Switching to remote stops the local daemon on this machine. Switching back to local "
            "starts it again. PacSmith is meant for private networks; exposing a library host on the "
            "public Internet is an administrator choice, not the default."));
    intro->setWordWrap(true);
    intro->setMinimumWidth(460);
    intro->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    intro->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto *localRadio = new QRadioButton(QStringLiteral("This computer"), &dialog);
    auto *remoteRadio = new QRadioButton(QStringLiteral("Remote library host"), &dialog);
    localRadio->setChecked(current.mode != ConnectionConfig::Mode::Remote);
    remoteRadio->setChecked(current.mode == ConnectionConfig::Mode::Remote);
    auto *remoteSettings = new QWidget(&dialog);
    auto *remoteForm = new QFormLayout(remoteSettings);
    remoteForm->setContentsMargins(22, 0, 0, 0);
    remoteForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto *hostEdit = new QLineEdit(remoteSettings);
    hostEdit->setPlaceholderText(QStringLiteral("hostname or IP"));
    auto *portEdit = new QSpinBox(remoteSettings);
    portEdit->setRange(1, 65535);
    portEdit->setValue(8443);
    auto *clientName = new QLineEdit(remoteSettings);
    clientName->setMaxLength(80);
    clientName->setText(defaultEnrollmentName());
    if (current.mode == ConnectionConfig::Mode::Remote) {
        hostEdit->setText(current.remoteUrl.host());
        portEdit->setValue(current.remoteUrl.port(8443));
    }
    remoteForm->addRow(QStringLiteral("Address"), hostEdit);
    remoteForm->addRow(QStringLiteral("Port"), portEdit);
    remoteForm->addRow(QStringLiteral("Client name"), clientName);
    auto *statusGroup = new QGroupBox(QStringLiteral("Connection details"), &dialog);
    auto *statusForm = new QFormLayout(statusGroup);
    statusForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto refreshControls = [&] {
        const bool remote = remoteRadio->isChecked();
        remoteSettings->setVisible(remote);
        if (!remote) {
            fillStatusForm(statusForm, LibraryClient(ConnectionConfig::localDefault()).statusRows());
        } else if (current.mode == ConnectionConfig::Mode::Remote) {
            fillStatusForm(statusForm, LibraryClient(current).statusRows());
        } else {
            QList<QPair<QString, QString>> rows{
                {QStringLiteral("mode"), QStringLiteral("remote")},
            };
            const auto host = hostEdit->text().trimmed();
            if (!host.isEmpty()) {
                rows.append({QStringLiteral("url"),
                             QStringLiteral("https://%1:%2").arg(host).arg(portEdit->value())});
            }
            rows.append({QStringLiteral("reachable"), QStringLiteral("false")});
            rows.append({QStringLiteral("error"),
                         QStringLiteral("Not connected. Save to enroll with this host.")});
            fillStatusForm(statusForm, rows);
        }
        dialog.adjustSize();
    };
    QObject::connect(localRadio, &QRadioButton::toggled, &dialog, [&](bool) { refreshControls(); });
    QObject::connect(remoteRadio, &QRadioButton::toggled, &dialog, [&](bool) { refreshControls(); });
    root->addWidget(intro);
    root->addWidget(localRadio);
    root->addWidget(remoteRadio);
    root->addWidget(remoteSettings);
    root->addWidget(statusGroup);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    root->addWidget(buttons);
    refreshControls();
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const bool wantRemote = remoteRadio->isChecked();
        QString host;
        int port = portEdit->value();
        if (wantRemote) {
            auto spec = hostEdit->text().trimmed();
            if (spec.isEmpty()) {
                QMessageBox::warning(&dialog, QStringLiteral("Remote library host"),
                                     QStringLiteral("Enter the library host address."));
                return;
            }
            QString parseError;
            if (spec.contains(QLatin1Char(':')) && !spec.startsWith(QLatin1Char('['))) {
                if (!parseRemoteTarget(spec, &host, &port, &parseError)) {
                    QMessageBox::warning(&dialog, QStringLiteral("Remote library host"), parseError);
                    return;
                }
            } else if (!parseRemoteTarget(spec, &host, &port, &parseError)) {
                QMessageBox::warning(&dialog, QStringLiteral("Remote library host"), parseError);
                return;
            } else {
                port = portEdit->value();
            }
        }
        ConnectionConfig next = ConnectionConfig::localDefault();
        if (wantRemote) next = remoteConnection(host, port);
        const bool targetChanged = !current.sameTarget(next);
        QString error;
        if (!wantRemote) {
            if (!applyLibraryRuntime(next, &error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not start pacsmithd"), error);
                return;
            }
            if (!next.save(&error)) {
                QMessageBox::critical(&dialog, QStringLiteral("Could not save connection"), error);
                return;
            }
        } else {
            const bool reuse = current.sameTarget(next) && QFileInfo::exists(next.clientCertPath) &&
                               QFileInfo::exists(next.serverCaPath);
            if (reuse) {
                if (!next.save(&error)) {
                    QMessageBox::critical(&dialog, QStringLiteral("Could not save connection"), error);
                    return;
                }
            } else {
                QProgressDialog progress(QStringLiteral("Enrolling with the library host…"),
                                         QStringLiteral("Cancel"), 0, 0, &dialog);
                progress.setWindowModality(Qt::WindowModal);
                progress.show();
                const auto enrolled = enrollRemote(
                    host, port, clientName->text(),
                    [&](const QString &fingerprint, const QString &sha256) {
                        return QMessageBox::question(
                                   &dialog, QStringLiteral("Trust this library host?"),
                                   QStringLiteral("This library host identifies as:\n\n%1\nSHA-256: %2\n\n"
                                                  "Compare this with Settings → Library or `pacsmith server info` "
                                                  "on that computer.")
                                       .arg(fingerprint, sha256)) == QMessageBox::Yes;
                    },
                    [&](const QString &message) {
                        progress.setLabelText(message);
                        QApplication::processEvents();
                    },
                    [&] {
                        if (progress.wasCanceled()) return false;
                        QEventLoop loop;
                        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
                        loop.exec();
                        return !progress.wasCanceled();
                    },
                    &error);
                if (!enrolled) {
                    if (error != QStringLiteral("enrollment canceled")) {
                        QMessageBox::critical(&dialog, QStringLiteral("Could not enroll"), error);
                    }
                    return;
                }
                next = enrolled->config;
            }
        }
        if (wantRemote && !applyLibraryRuntime(next, &error)) {
            QMessageBox::warning(&dialog, QStringLiteral("Local daemon"), error);
        }
        dialog.done(targetChanged ? QDialog::Accepted + 1 : QDialog::Accepted);
    });
    const auto result = dialog.exec();
    if (result == QDialog::Accepted + 1) {
        QMessageBox::information(
            parent, QStringLiteral("Restart required"),
            QStringLiteral("PacSmith will restart to use the new library connection."));
        restartPacsmithGui();
        return true;
    }
    return false;
}

} // namespace pacsmith::gui
