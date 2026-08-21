#include "core/terminal_install_service.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

namespace pacsmith {
namespace {

constexpr auto sessionTitle = "PacSmith Package Installation";

QString randomHexToken() {
    QByteArray bytes(32, '\0');
    auto *generator = QRandomGenerator::system();
    for (auto &byte : bytes) {
        byte = static_cast<char>(generator->bounded(256));
    }
    return QString::fromLatin1(bytes.toHex());
}

QString resolveExecutable(const QString &program) {
    if (program.isEmpty()) return {};
    const QFileInfo info(program);
    if (info.isAbsolute()) return info.isExecutable() && info.isFile() ? info.absoluteFilePath() : QString{};
    return QStandardPaths::findExecutable(program);
}

QStringList splitConfiguredTerminal(const QString &value) {
    auto command = QProcess::splitCommand(value.trimmed());
    command.removeAll(QString{});
    return command;
}

QString desktopSetting(const QString &program, const QStringList &arguments) {
    const auto executable = QStandardPaths::findExecutable(program);
    if (executable.isEmpty()) return {};
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(1000) || !process.waitForFinished(2000) || process.exitCode() != 0) return {};
    auto value = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (value.size() >= 2 && value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))) {
        value = value.mid(1, value.size() - 2);
    }
    return value;
}

bool validPackage(const QFileInfo &package) {
    return package.isAbsolute() && package.exists() && package.isFile() &&
           package.fileName().contains(QStringLiteral(".pkg.tar.")) &&
           !package.fileName().endsWith(QStringLiteral(".sig"));
}

} // namespace

QByteArray InstallSessionProtocol::encode(const InstallSessionEvent &event) {
    QJsonObject object{{QStringLiteral("type"), event.type},
                       {QStringLiteral("token"), event.token}};
    if (!event.text.isEmpty()) object.insert(QStringLiteral("text"), event.text);
    if (event.type == QStringLiteral("finished")) {
        object.insert(QStringLiteral("exitCode"), event.exitCode);
        object.insert(QStringLiteral("exitStatus"),
                      event.exitStatus == QProcess::NormalExit ? QStringLiteral("normal")
                                                               : QStringLiteral("crashed"));
        object.insert(QStringLiteral("canceled"), event.canceled);
    }
    auto encoded = QJsonDocument(object).toJson(QJsonDocument::Compact);
    encoded.append('\n');
    return encoded;
}

std::optional<InstallSessionEvent> InstallSessionProtocol::decode(const QByteArrayView line,
                                                                   QString *error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(line.toByteArray(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) *error = QStringLiteral("Invalid install-session message: %1").arg(parseError.errorString());
        return std::nullopt;
    }
    const auto object = document.object();
    InstallSessionEvent event;
    event.type = object.value(QStringLiteral("type")).toString();
    event.token = object.value(QStringLiteral("token")).toString();
    event.text = object.value(QStringLiteral("text")).toString();
    if (event.type != QStringLiteral("started") && event.type != QStringLiteral("output") &&
        event.type != QStringLiteral("finished")) {
        if (error != nullptr) *error = QStringLiteral("Unknown install-session event: %1").arg(event.type);
        return std::nullopt;
    }
    static const QRegularExpression tokenFormat(QStringLiteral("^[0-9a-f]{64}$"));
    if (!tokenFormat.match(event.token).hasMatch()) {
        if (error != nullptr) *error = QStringLiteral("Install-session message has an invalid token");
        return std::nullopt;
    }
    if (event.type == QStringLiteral("finished")) {
        if (!object.value(QStringLiteral("exitCode")).isDouble() ||
            !object.value(QStringLiteral("exitStatus")).isString() ||
            !object.value(QStringLiteral("canceled")).isBool()) {
            if (error != nullptr) *error = QStringLiteral("Install-session completion is incomplete");
            return std::nullopt;
        }
        event.exitCode = object.value(QStringLiteral("exitCode")).toInt(-1);
        const auto status = object.value(QStringLiteral("exitStatus")).toString();
        if (status != QStringLiteral("normal") && status != QStringLiteral("crashed")) {
            if (error != nullptr) *error = QStringLiteral("Install-session completion has an invalid exit status");
            return std::nullopt;
        }
        event.exitStatus = status == QStringLiteral("normal") ? QProcess::NormalExit : QProcess::CrashExit;
        event.canceled = object.value(QStringLiteral("canceled")).toBool();
    }
    return event;
}

std::optional<TerminalCommand> TerminalLauncher::commandFor(
    const QString &terminalProgram, const QStringList &terminalArguments,
    const QString &helperProgram, const QStringList &helperArguments, QString *error) {
    const auto executable = resolveExecutable(terminalProgram);
    if (executable.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("Terminal executable was not found: %1").arg(terminalProgram);
        return std::nullopt;
    }
    const auto helper = resolveExecutable(helperProgram);
    if (helper.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("PacSmith CLI executable was not found: %1").arg(helperProgram);
        return std::nullopt;
    }

    QStringList arguments = terminalArguments;
    const auto name = QFileInfo(executable).fileName().toLower();
    const QString title = QString::fromLatin1(sessionTitle);
    if (name == QStringLiteral("xdg-terminal-exec")) {
        arguments.append(QStringLiteral("--"));
    } else if (name == QStringLiteral("konsole")) {
        arguments.append({QStringLiteral("--separate"), QStringLiteral("-e")});
    } else if (name == QStringLiteral("gnome-terminal")) {
        arguments.append({QStringLiteral("--wait"), QStringLiteral("--title=%1").arg(title),
                          QStringLiteral("--")});
    } else if (name == QStringLiteral("xfce4-terminal")) {
        arguments.append({QStringLiteral("--disable-server"), QStringLiteral("--title=%1").arg(title),
                          QStringLiteral("--execute")});
    } else if (name == QStringLiteral("mate-terminal")) {
        arguments.append({QStringLiteral("--disable-factory"), QStringLiteral("--title=%1").arg(title),
                          QStringLiteral("--execute")});
    } else if (name == QStringLiteral("kitty")) {
        arguments.append({QStringLiteral("--detach=no"), QStringLiteral("--title"), title});
    } else if (name == QStringLiteral("alacritty")) {
        arguments.append({QStringLiteral("--title"), title, QStringLiteral("--command")});
    } else if (name == QStringLiteral("foot")) {
        arguments.append(QStringLiteral("--title=%1").arg(title));
    } else if (name == QStringLiteral("xterm")) {
        arguments.append({QStringLiteral("-T"), title, QStringLiteral("-e")});
    } else {
        if (error != nullptr) *error = QStringLiteral("Unsupported terminal emulator: %1").arg(name);
        return std::nullopt;
    }
    arguments.append(helper);
    arguments.append(helperArguments);
    return TerminalCommand{executable, arguments, name};
}

std::optional<TerminalCommand> TerminalLauncher::resolve(
    const QString &helperProgram, const QStringList &helperArguments,
    const QProcessEnvironment &environment, QString *error) {
    struct Candidate {
        QString program;
        QStringList arguments;
    };
    QList<Candidate> candidates;
    const auto addCandidate = [&candidates](const QString &configured) {
        const auto parts = splitConfiguredTerminal(configured);
        if (parts.isEmpty()) return;
        const auto duplicate = std::any_of(candidates.cbegin(), candidates.cend(), [&](const auto &candidate) {
            return candidate.program == parts.first() && candidate.arguments == parts.sliced(1);
        });
        if (!duplicate) candidates.append({parts.first(), parts.sliced(1)});
    };

    addCandidate(QStringLiteral("xdg-terminal-exec"));
    addCandidate(environment.value(QStringLiteral("TERMINAL")));
    const auto desktop = environment.value(QStringLiteral("XDG_CURRENT_DESKTOP")).toUpper();
    if (desktop.contains(QStringLiteral("KDE"))) {
        addCandidate(desktopSetting(QStringLiteral("kreadconfig6"),
                                    {QStringLiteral("--file"), QStringLiteral("kdeglobals"),
                                     QStringLiteral("--group"), QStringLiteral("General"),
                                     QStringLiteral("--key"), QStringLiteral("TerminalApplication")}));
        addCandidate(QStringLiteral("konsole"));
    } else if (desktop.contains(QStringLiteral("GNOME"))) {
        addCandidate(desktopSetting(QStringLiteral("gsettings"),
                                    {QStringLiteral("get"),
                                     QStringLiteral("org.gnome.desktop.default-applications.terminal"),
                                     QStringLiteral("exec")}));
        addCandidate(QStringLiteral("gnome-terminal"));
    } else if (desktop.contains(QStringLiteral("XFCE"))) {
        addCandidate(QStringLiteral("xfce4-terminal"));
    } else if (desktop.contains(QStringLiteral("MATE"))) {
        addCandidate(QStringLiteral("mate-terminal"));
    }
    for (const auto &fallback : {QStringLiteral("konsole"), QStringLiteral("gnome-terminal"),
                                 QStringLiteral("xfce4-terminal"), QStringLiteral("mate-terminal"),
                                 QStringLiteral("kitty"), QStringLiteral("alacritty"),
                                 QStringLiteral("foot"), QStringLiteral("xterm")}) {
        addCandidate(fallback);
    }

    QString lastError;
    for (const auto &candidate : candidates) {
        auto command = commandFor(candidate.program, candidate.arguments, helperProgram,
                                  helperArguments, &lastError);
        if (command) return command;
    }
    if (error != nullptr) {
        *error = QStringLiteral("No supported terminal emulator was found. Install or configure one of: "
                                "Konsole, GNOME Terminal, Kitty, Alacritty, Foot, Xfce Terminal, "
                                "MATE Terminal, or xterm.");
        if (!lastError.isEmpty()) *error += QStringLiteral(" Last candidate: %1").arg(lastError);
    }
    return std::nullopt;
}

TerminalInstallService::TerminalInstallService(QObject *parent) : QObject(parent) {
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_, &QLocalServer::newConnection, this, &TerminalInstallService::acceptConnection);
    connectionTimer_.setSingleShot(true);
    connectionTimer_.setInterval(15000);
    connect(&connectionTimer_, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("The terminal opened, but the PacSmith installation helper did not connect within 15 seconds."));
    });
}

bool TerminalInstallService::isRunning() const noexcept {
    return active_;
}

void TerminalInstallService::start(const std::filesystem::path &packagePath,
                                   const QString &pacsmithExecutable) {
    if (active_) {
        emit failedToStart(QStringLiteral("An installation is already running"));
        return;
    }
    const QFileInfo package(QString::fromUtf8(packagePath.string().c_str()));
    if (!validPackage(package)) {
        emit failedToStart(QStringLiteral("Not a valid absolute Arch package path: %1").arg(package.filePath()));
        return;
    }
    startSession({QStringLiteral("--package"), package.absoluteFilePath()}, pacsmithExecutable,
                 QStringLiteral("package installation"));
}

void TerminalInstallService::startUninstall(const QString &packageName,
                                            const QString &pacsmithExecutable) {
    static const QRegularExpression safePackageName(QStringLiteral("^[a-z0-9][a-z0-9@._+\\-]*$"));
    if (!safePackageName.match(packageName).hasMatch()) {
        emit failedToStart(QStringLiteral("Invalid Arch package name: %1").arg(packageName));
        return;
    }
    startSession({QStringLiteral("--remove"), packageName}, pacsmithExecutable,
                 QStringLiteral("package removal"));
}

void TerminalInstallService::startSession(const QStringList &operationArguments,
                                          const QString &pacsmithExecutable,
                                          const QString &description) {
    if (active_) {
        emit failedToStart(QStringLiteral("A package operation is already running"));
        return;
    }
    resetSession();
    token_ = randomHexToken();
    result_ = {};
    result_.startedAt = QDateTime::currentDateTimeUtc();
    QString serverName;
    for (int attempt = 0; attempt < 4 && !server_.isListening(); ++attempt) {
        serverName = QStringLiteral("pacsmith-install-%1-%2")
                         .arg(QCoreApplication::applicationPid())
                         .arg(randomHexToken().left(16));
        server_.listen(serverName);
    }
    if (!server_.isListening()) {
        emit failedToStart(QStringLiteral("Could not create the private installation session: %1")
                               .arg(server_.errorString()));
        return;
    }

    const QStringList helperArguments{QStringLiteral("_install-session"),
                                      QStringLiteral("--socket"), serverName,
                                      QStringLiteral("--token"), token_};
    auto completeArguments = helperArguments;
    completeArguments.append(operationArguments);
    QString terminalError;
    const auto terminal = TerminalLauncher::resolve(pacsmithExecutable, completeArguments,
                                                     QProcessEnvironment::systemEnvironment(),
                                                     &terminalError);
    if (!terminal) {
        server_.close();
        emit failedToStart(terminalError);
        return;
    }

    active_ = true;
    qint64 terminalPid = 0;
    if (!QProcess::startDetached(terminal->program, terminal->arguments, QString{}, &terminalPid)) {
        fail(QStringLiteral("Could not launch %1 for %2").arg(terminal->displayName, description));
        return;
    }
    connectionTimer_.start();
    emit progressChanged(QStringLiteral("Opened %1; waiting for the PacSmith installation session…")
                             .arg(terminal->displayName));
}

void TerminalInstallService::acceptConnection() {
    while (server_.hasPendingConnections()) {
        auto *candidate = server_.nextPendingConnection();
        if (socket_ != nullptr) {
            candidate->abort();
            candidate->deleteLater();
            continue;
        }
        socket_ = candidate;
        connect(candidate, &QLocalSocket::readyRead, this, &TerminalInstallService::readMessages);
        connect(candidate, &QLocalSocket::disconnected, this, [this] {
            readMessages();
            if (active_) {
                fail(authenticated_
                         ? QStringLiteral("The installation terminal closed before pacman reported a result.")
                         : QStringLiteral("The installation helper disconnected before authenticating."));
            }
        });
    }
}

void TerminalInstallService::readMessages() {
    if (socket_ == nullptr) return;
    receiveBuffer_.append(socket_->readAll());
    constexpr qsizetype maximumBufferedMessage = 1024 * 1024;
    if (receiveBuffer_.size() > maximumBufferedMessage && !receiveBuffer_.contains('\n')) {
        fail(QStringLiteral("The installation helper sent an oversized protocol message."));
        return;
    }
    while (active_) {
        const auto newline = receiveBuffer_.indexOf('\n');
        if (newline < 0) break;
        if (newline > maximumBufferedMessage) {
            fail(QStringLiteral("The installation helper sent an oversized protocol message."));
            return;
        }
        const auto line = receiveBuffer_.left(newline);
        receiveBuffer_.remove(0, newline + 1);
        QString error;
        const auto event = InstallSessionProtocol::decode(QByteArrayView(line), &error);
        if (!event) {
            fail(error);
            return;
        }
        handleMessage(*event);
    }
}

void TerminalInstallService::handleMessage(const InstallSessionEvent &event) {
    if (event.token != token_) {
        fail(QStringLiteral("The installation helper failed session authentication."));
        return;
    }
    if (event.type == QStringLiteral("started")) {
        if (authenticated_) {
            fail(QStringLiteral("The installation helper sent a duplicate start event."));
            return;
        }
        authenticated_ = true;
        connectionTimer_.stop();
        emit progressChanged(QStringLiteral("Pacman is running in the external terminal…"));
        return;
    }
    if (!authenticated_) {
        fail(QStringLiteral("The installation helper sent data before authenticating."));
        return;
    }
    if (event.type == QStringLiteral("output")) {
        result_.output.append(event.text);
        emit outputAvailable(event.text);
        return;
    }
    result_.exitCode = event.exitCode;
    result_.exitStatus = event.exitStatus;
    result_.canceled = event.canceled;
    result_.finishedAt = QDateTime::currentDateTimeUtc();
    active_ = false;
    connectionTimer_.stop();
    server_.close();
    if (socket_ != nullptr) socket_->disconnectFromServer();
    emit finished(result_);
}

void TerminalInstallService::fail(const QString &message) {
    if (!active_) return;
    active_ = false;
    connectionTimer_.stop();
    server_.close();
    if (socket_ != nullptr) socket_->abort();
    emit failedToStart(message);
}

void TerminalInstallService::resetSession() {
    connectionTimer_.stop();
    server_.close();
    if (socket_ != nullptr) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
    }
    socket_.clear();
    receiveBuffer_.clear();
    token_.clear();
    authenticated_ = false;
    active_ = false;
}

} // namespace pacsmith
