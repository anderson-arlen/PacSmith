#include "cli/admin.hpp"

#include "core/daemon_control.hpp"
#include "core/enrollment.hpp"
#include "core/library_client.hpp"

#include <QFileInfo>
#include <QString>

#include <unistd.h>

namespace {

constexpr auto kLocalAdminHint =
    "PKI and listen administration require a local Unix-socket connection. "
    "Use `pacsmith connect local` on the library host.\n";

constexpr auto kListenOffHint =
    "remote listening is off; enable it with `pacsmith server listen on` before "
    "approving clients\n";

bool requireLocalAdmin(const pacsmith::LibraryClient &library, QTextStream &errorStream) {
    if (library.config().mode == pacsmith::ConnectionConfig::Mode::Remote) {
        errorStream << "error: " << kLocalAdminHint;
        return false;
    }
    return true;
}

void printListen(QTextStream &out, const pacsmith::ListenSettings &listen) {
    out << "enabled\t" << (listen.enabled ? "true" : "false") << '\n'
        << "port\t" << listen.port << '\n'
        << "hosts\t" << listen.hosts.join(QLatin1Char(',')) << '\n'
        << "bound\t" << listen.bound.join(QLatin1Char(',')) << '\n';
}

int printServerStatus(pacsmith::LibraryClient &library, QTextStream &out) {
    for (const auto &row : library.statusRows()) {
        out << row.first << '\t' << row.second << '\n';
    }
    return 0;
}

int printServerInfo(pacsmith::LibraryClient &library, QTextStream &out, QTextStream &errorStream) {
    QString error;
    const auto info = library.serverInfo(&error);
    if (!info) {
        errorStream << "error: " << error << '\n';
        return 1;
    }
    out << "fingerprint\t" << info->fingerprint << '\n'
        << "fingerprint_sha256\t" << info->fingerprintSha256 << '\n'
        << "secret_backend\t" << info->secretBackend << '\n'
        << "pki_ready\t" << (info->pkiReady ? "true" : "false") << '\n';
    printListen(out, info->listen);
    return 0;
}

int applyListen(pacsmith::LibraryClient &library, pacsmith::ListenSettings settings,
                QTextStream &out, QTextStream &errorStream) {
    QString error;
    const auto saved = library.saveListen(settings, &error);
    if (!saved) {
        errorStream << "error: " << error << '\n';
        return 1;
    }
    printListen(out, *saved);
    return 0;
}

int runListenOn(const QStringList &arguments, pacsmith::LibraryClient &library, QTextStream &out,
                QTextStream &errorStream) {
    QString error;
    const auto info = library.serverInfo(&error);
    if (!info) {
        errorStream << "error: " << error << '\n';
        return 1;
    }
    auto settings = info->listen;
    settings.enabled = true;
    QStringList hosts;
    bool sawInterface = false;
    for (int index = 4; index < arguments.size(); ++index) {
        const auto flag = arguments.at(index);
        if ((flag == QStringLiteral("--port") || flag == QStringLiteral("-p")) &&
            index + 1 < arguments.size()) {
            bool ok = false;
            const auto port = arguments.at(++index).toInt(&ok);
            if (!ok || port < 1 || port > 65535) {
                errorStream << "error: listen port must be between 1 and 65535\n";
                return 1;
            }
            settings.port = port;
        } else if ((flag == QStringLiteral("--interface") || flag == QStringLiteral("-i")) &&
                   index + 1 < arguments.size()) {
            sawInterface = true;
            hosts.append(arguments.at(++index));
        } else {
            errorStream << "error: usage: pacsmith server listen on [--port N] [--interface ADDR]...\n";
            return 1;
        }
    }
    if (sawInterface) settings.hosts = hosts;
    return applyListen(library, settings, out, errorStream);
}

} // namespace

int runConnectCommand(const QStringList &arguments, QTextStream &out, QTextStream &errorStream) {
    if (arguments.size() < 3) {
        errorStream << "error: usage: pacsmith connect status|local|remote <host>[:port]\n";
        return 1;
    }
    const auto action = arguments.at(2);
    if (action == QStringLiteral("status")) {
        const auto config = pacsmith::ConnectionConfig::load();
        if (config.mode == pacsmith::ConnectionConfig::Mode::Remote) {
            out << "mode\tremote\nurl\t" << config.remoteUrl.toString() << '\n';
        } else {
            out << "mode\tlocal\nsocket\t" << config.socketPath << '\n';
        }
        return 0;
    }
    if (action == QStringLiteral("local")) {
        const auto config = pacsmith::ConnectionConfig::localDefault();
        QString error;
        if (!config.save(&error)) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        if (!pacsmith::applyLibraryRuntime(config, &error)) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        out << "mode\tlocal\nsocket\t" << config.socketPath << '\n';
        return 0;
    }
    if (action != QStringLiteral("remote") || arguments.size() < 4) {
        errorStream << "error: usage: pacsmith connect remote <host>[:port] [--name <friendly>]\n";
        return 1;
    }
    QString friendly = pacsmith::defaultEnrollmentName();
    QString target = arguments.at(3);
    for (int index = 4; index < arguments.size(); ++index) {
        if ((arguments.at(index) == QStringLiteral("--name") ||
             arguments.at(index) == QStringLiteral("-n")) &&
            index + 1 < arguments.size()) {
            friendly = arguments.at(++index);
        } else {
            errorStream << "error: usage: pacsmith connect remote <host>[:port] [--name <friendly>]\n";
            return 1;
        }
    }
    QString host;
    int port = 8443;
    QString error;
    if (!pacsmith::parseRemoteTarget(target, &host, &port, &error)) {
        errorStream << "error: " << error << '\n';
        return 1;
    }
    const auto next = pacsmith::remoteConnection(host, port);
    const auto current = pacsmith::ConnectionConfig::load();
    if (current.mode == pacsmith::ConnectionConfig::Mode::Remote &&
        current.remoteUrl == next.remoteUrl && QFileInfo::exists(next.clientCertPath) &&
        QFileInfo::exists(next.serverCaPath)) {
        if (!next.save(&error)) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        if (!pacsmith::applyLibraryRuntime(next, &error)) {
            errorStream << "warning: " << error << '\n';
        }
        out << "mode\tremote\nurl\t" << next.remoteUrl.toString() << '\n';
        return 0;
    }
    const auto enrolled = pacsmith::enrollRemote(
        host, port, friendly,
        [&](const QString &fingerprint, const QString &sha256) {
            errorStream << "Library fingerprint:\t" << fingerprint << '\n'
                        << "fingerprint_sha256:\t" << sha256 << '\n'
                        << "Compare this with `pacsmith server info` on the library host.\n";
            if (::isatty(STDIN_FILENO) == 0) {
                errorStream << "error: confirmation requires an interactive terminal\n";
                return false;
            }
            errorStream << "Trust this library host? [y/N] " << Qt::flush;
            QTextStream input(stdin);
            const auto answer = input.readLine().trimmed().toLower();
            return answer == QStringLiteral("y") || answer == QStringLiteral("yes");
        },
        [&](const QString &message) { errorStream << message << '\n'; }, {}, &error);
    if (!enrolled) {
        errorStream << "error: " << error << '\n';
        return 1;
    }
    if (!pacsmith::applyLibraryRuntime(enrolled->config, &error)) {
        errorStream << "warning: " << error << '\n';
    }
    out << "mode\tremote\nurl\t" << enrolled->config.remoteUrl.toString() << '\n'
        << "registration\t" << enrolled->registrationId << '\n';
    return 0;
}

int runServerCommand(const QStringList &arguments, pacsmith::LibraryClient &library,
                     QTextStream &out, QTextStream &errorStream) {
    if (arguments.size() == 3 && arguments.at(2) == QStringLiteral("status")) {
        return printServerStatus(library, out);
    }
    if (!requireLocalAdmin(library, errorStream)) return 1;
    if (arguments.size() == 3 && arguments.at(2) == QStringLiteral("info")) {
        return printServerInfo(library, out, errorStream);
    }
    if (arguments.size() < 3 || arguments.at(2) != QStringLiteral("listen")) {
        errorStream << "error: usage: pacsmith server status|info|listen [on|off]\n";
        return 1;
    }
    if (arguments.size() == 3) {
        QString error;
        const auto info = library.serverInfo(&error);
        if (!info) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        printListen(out, info->listen);
        return 0;
    }
    if (arguments.at(3) == QStringLiteral("off")) {
        QString error;
        const auto info = library.serverInfo(&error);
        if (!info) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        auto settings = info->listen;
        settings.enabled = false;
        return applyListen(library, settings, out, errorStream);
    }
    if (arguments.at(3) == QStringLiteral("on")) {
        return runListenOn(arguments, library, out, errorStream);
    }
    errorStream << "error: usage: pacsmith server listen on [--port N] [--interface ADDR]...\n"
                   "       pacsmith server listen off\n";
    return 1;
}

int runClientsCommand(const QStringList &arguments, pacsmith::LibraryClient &library,
                      QTextStream &out, QTextStream &errorStream) {
    if (!requireLocalAdmin(library, errorStream)) return 1;
    QString error;
    const auto listenNeeded = arguments.size() >= 3 &&
                              (arguments.at(2) == QStringLiteral("pending") ||
                               arguments.at(2) == QStringLiteral("approve") ||
                               arguments.at(2) == QStringLiteral("reject"));
    if (listenNeeded) {
        const auto info = library.serverInfo(&error);
        if (!info) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        if (!info->listen.enabled) {
            errorStream << "error: " << kListenOffHint;
            return 1;
        }
        error.clear();
    }
    if (arguments.size() == 3 && arguments.at(2) == QStringLiteral("list")) {
        for (const auto &client : library.clients(&error)) {
            out << client.id << '\t' << client.name << '\t'
                << (client.revoked ? QStringLiteral("revoked") : QStringLiteral("active")) << '\n';
        }
    } else if (arguments.size() == 3 && arguments.at(2) == QStringLiteral("pending")) {
        for (const auto &reg : library.pendingRegistrations(&error)) {
            out << reg.id << '\t' << reg.name << '\t' << reg.status << '\n';
        }
    } else if (arguments.size() == 4 && arguments.at(2) == QStringLiteral("approve")) {
        if (!library.approveRegistration(arguments.at(3), &error)) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
    } else if (arguments.size() == 4 && arguments.at(2) == QStringLiteral("reject")) {
        if (!library.rejectRegistration(arguments.at(3), &error)) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
    } else if (arguments.size() == 4 && arguments.at(2) == QStringLiteral("revoke")) {
        if (!library.revokeClient(arguments.at(3), &error)) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
    } else {
        errorStream << "error: usage: pacsmith clients list|pending|approve <id>|reject <id>|revoke <id>\n";
        return 1;
    }
    if (!error.isEmpty()) {
        errorStream << "error: " << error << '\n';
        return 1;
    }
    return 0;
}
