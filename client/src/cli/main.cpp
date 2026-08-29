#include "core/app_settings.hpp"
#include "core/background_updates.hpp"
#include "core/managed_package.hpp"
#include "core/payload_inspector.hpp"
#include "core/payload_review.hpp"
#include "core/package_artifact.hpp"
#include "core/pkgbuild_generator.hpp"
#include "core/process_services.hpp"
#include "core/library_client.hpp"
#include "core/daemon_control.hpp"
#include "core/repository_key_download_service.hpp"
#include "core/repository_trust.hpp"
#include "core/terminal_install_service.hpp"
#include "cli/agent_integration.hpp"
#include "cli/admin.hpp"
#include "mcp/server.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <filesystem>
#include <unistd.h>

namespace {

void printUsage(QTextStream &stream) {
    stream << "PacSmith - vendor artifact to local Arch package workbench\n\n"
              "Usage:\n"
              "  pacsmith add <artifact>\n"
              "  pacsmith add <github-url> [--asset-regex <regex>] [--prerelease]\n"
              "  pacsmith add apt <repo-url> <suite> <component|-> <architecture> <package> <key-url> [--trust-fingerprint <fingerprint>]\n"
              "  pacsmith add rpm <repo-url> <architecture> <package> <key-url> [--trust-fingerprint <fingerprint>]\n"
              "  pacsmith list\n"
              "  pacsmith versions <project>\n"
              "  pacsmith info <project>\n"
              "  pacsmith dependencies <project>\n"
              "  pacsmith scripts <project> [--acknowledge <script>]\n"
              "  pacsmith lifecycle <project> [--acknowledge <sha256>|--discard]\n"
              "  pacsmith payload <project> [--show <path>]\n"
              "  pacsmith pkgbuild <project>\n"
              "  pacsmith custom-file <project> list|read <name>|write <name> <path>|delete <name>\n"
              "  pacsmith build <project>\n"
              "  pacsmith install [--polkit] <project>\n"
              "  pacsmith rollback <project> <release-id|version> [--polkit]\n"
              "  pacsmith uninstall [--polkit] <project>\n"
              "  pacsmith check <project>|--all\n"
              "  pacsmith mcp\n"
              "  pacsmith skill path|install [--force]|uninstall\n"
              "  pacsmith plugin path\n"
              "  pacsmith connect status|local|remote <host>[:port]\n"
              "  pacsmith server status\n"
              "  pacsmith server info\n"
              "  pacsmith server listen\n"
              "  pacsmith server listen on [--port N] [--interface ADDR]...\n"
              "  pacsmith server listen off\n"
              "  pacsmith clients list|pending|approve <id>|reject <id>|revoke <id>\n"
              "  pacsmith gui\n";
}

std::optional<pacsmith::Project> requireProject(const pacsmith::LibraryClient &library, const QString &name,
                                                QTextStream &errorStream) {
    QString error;
    auto project = library.load(name, &error);
    if (!project) errorStream << "error: " << error << '\n';
    return project;
}

QString scriptFriendly(const QString &value) {
    QString result = value;
    result.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    result.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return result;
}

bool askYesNo(QTextStream &errorStream, const QString &prompt) {
    if (::isatty(STDIN_FILENO) == 0) return false;
    errorStream << prompt << " [y/N] " << Qt::flush;
    QTextStream input(stdin);
    const auto answer = input.readLine().trimmed().toLower();
    return answer == QStringLiteral("y") || answer == QStringLiteral("yes");
}

int runRepositoryAdd(const QStringList &arguments, pacsmith::LibraryClient &library,
                     QTextStream &out, QTextStream &errorStream) {
    const bool rpm = arguments.value(2) == QStringLiteral("rpm");
    const int required = rpm ? 7 : 9;
    if (arguments.size() != required && arguments.size() != required + 2) {
        errorStream << (rpm
            ? "error: add rpm requires <repo-url> <architecture> <package> <key-url> [--trust-fingerprint <fingerprint>]\n"
            : "error: add apt requires <repo-url> <suite> <component|-> <architecture> <package> <key-url> [--trust-fingerprint <fingerprint>]\n");
        return 1;
    }
    QString trustedFingerprint;
    if (arguments.size() == required + 2) {
        if (arguments.at(required) != QStringLiteral("--trust-fingerprint")) {
            errorStream << "error: expected --trust-fingerprint as the final option\n";
            return 1;
        }
        trustedFingerprint = arguments.at(required + 1).trimmed().toUpper();
    }

    pacsmith::UpdateConfiguration update;
    update.strategy = rpm ? pacsmith::UpdateStrategy::RpmRepository
                          : pacsmith::UpdateStrategy::AptRepository;
    update.url = arguments.at(3);
    QString packageName;
    QString keyText;
    if (rpm) {
        update.rpmArchitecture = arguments.at(4);
        update.rpmPackageName = arguments.at(5);
        packageName = update.rpmPackageName;
        keyText = arguments.at(6);
    } else {
        update.aptSuite = arguments.at(4);
        update.aptComponent = arguments.at(5) == QStringLiteral("-")
            ? QString{} : arguments.at(5);
        update.aptArchitecture = arguments.at(6);
        update.aptPackageName = arguments.at(7);
        packageName = update.aptPackageName;
        keyText = arguments.at(8);
    }
    const QUrl repositoryUrl(update.url, QUrl::StrictMode);
    if (!repositoryUrl.isValid() || repositoryUrl.scheme() != QStringLiteral("https") ||
        repositoryUrl.host().isEmpty() || !repositoryUrl.userInfo().isEmpty() ||
        repositoryUrl.hasQuery() || repositoryUrl.hasFragment()) {
        errorStream << "error: repository URL must be an HTTPS base URL without credentials, query, or fragment\n";
        return 1;
    }
    const QUrl keyUrl(keyText, QUrl::StrictMode);
    if (!pacsmith::isAcceptableRepositoryKeyUrl(keyUrl)) {
        errorStream << "error: signing-key URL must be a valid HTTPS URL without credentials or a fragment\n";
        return 1;
    }

    QString keyError;
    const auto downloadedKey = pacsmith::downloadRepositorySigningKey(keyUrl, &keyError);
    if (!downloadedKey) {
        errorStream << "error: " << keyError << '\n';
        return 1;
    }
    const auto &keyContents = downloadedKey->contents;
    QString inspectionError;
    const auto inspection = pacsmith::RepositoryTrust::inspectKey(keyContents,
                                                                  &inspectionError);
    if (!inspection) {
        errorStream << "error: " << inspectionError << '\n';
        return 1;
    }
    errorStream << "repository signing-key SHA256: " << inspection->sha256 << '\n';
    for (const auto &fingerprint : inspection->fingerprints) {
        errorStream << "OpenPGP fingerprint: " << fingerprint << '\n';
    }
    const auto fingerprintMatches = [&](const QString &fingerprint) {
        return std::any_of(inspection->fingerprints.cbegin(),
                           inspection->fingerprints.cend(), [&](const auto &candidate) {
            return candidate.compare(fingerprint, Qt::CaseInsensitive) == 0;
        });
    };
    if (!trustedFingerprint.isEmpty() && !fingerprintMatches(trustedFingerprint)) {
        errorStream << "error: downloaded key does not contain the fingerprint supplied with --trust-fingerprint\n";
        return 1;
    }
    if (trustedFingerprint.isEmpty() &&
        !askYesNo(errorStream,
                  QStringLiteral("Trust this key and query %1?").arg(update.url))) {
        errorStream << "error: repository key was not trusted; non-interactive use requires --trust-fingerprint\n";
        return 1;
    }

    const auto pinned = trustedFingerprint.isEmpty()
        ? inspection->fingerprints.first() : trustedFingerprint;
    if (rpm) {
        update.rpmCandidates.append(
            {update.url, update.rpmArchitecture, {keyUrl.toString()},
             QStringLiteral("command-line repository import")});
    } else {
        update.aptCandidates.append(
            {update.url, update.aptSuite,
             update.aptComponent.isEmpty() ? QStringList{} : QStringList{update.aptComponent},
             {update.aptArchitecture}, {}, QStringLiteral("command-line repository import")});
    }

    QString importError;
    const auto imported = library.importRepository(
        update, keyContents, keyUrl.toString(), pinned, &importError);
    if (!imported) {
        errorStream << "error: " << importError << '\n';
        return 1;
    }
    out << imported->project.id << '\t' << imported->releaseId << '\n';
    return 0;
}

int runCheck(pacsmith::LibraryClient &library, pacsmith::Project project, QTextStream &out,
             QTextStream &errorStream, pacsmith::BackgroundUpdateState *backgroundState = nullptr) {
    Q_UNUSED(backgroundState)
    const auto *tracker = project.activeTrackingRelease();
    if (tracker == nullptr) {
        out << project.id << "\tpaused\tproject has no analyzed release to track\n";
        return 0;
    }
    QString error;
    const auto started = library.startUpdateCheck(tracker->id, false, &error);
    if (!started) {
        errorStream << "error: " << error << '\n';
        return 1;
    }
    const auto job = library.waitForJob(started->id, &error);
    const auto log = library.jobLog(started->id, nullptr);
    if (!log.isEmpty()) errorStream << log;
    if (!job || job->status != QStringLiteral("succeeded")) {
        errorStream << "error: " << (!error.isEmpty() ? error : job ? job->error
                                                                : QStringLiteral("update check failed")) << '\n';
        return 1;
    }
    int failed = 0;
    for (const auto &value : job->result.value(QStringLiteral("checks")).toArray()) {
        const auto checked = value.toObject();
        const auto projectId = checked.value(QStringLiteral("project_id")).toString();
        const auto version = checked.value(QStringLiteral("detected_version")).toString();
        if (checked.value(QStringLiteral("prepared")).toBool()) {
            out << projectId << "\tprepared\t" << version << '\n';
        }
        if (checked.value(QStringLiteral("built")).toBool()) {
            out << projectId << "\tbuilt\t" << version << '\n';
        }
        const auto status = checked.value(QStringLiteral("status")).toString();
        out << projectId << '\t' << status << '\t'
            << checked.value(QStringLiteral("message")).toString() << '\n';
        if (status == QStringLiteral("error")) ++failed;
    }
    return failed == 0 ? 0 : 1;
}
int runInstallSession(QCoreApplication &application, const QStringList &arguments,
                      QTextStream &out, QTextStream &errorStream) {
    if (arguments.size() != 8 || arguments.at(2) != QStringLiteral("--socket") ||
        arguments.at(4) != QStringLiteral("--token") ||
        arguments.at(6) != QStringLiteral("--package")) {
        errorStream << "error: invalid internal installation session\n";
        return 2;
    }
    const auto socketName = arguments.at(3);
    const auto token = arguments.at(5);
    const auto operation = arguments.at(6);
    const QFileInfo package(arguments.at(7));
    static const QRegularExpression validToken(QStringLiteral("^[0-9a-f]{64}$"));
    if (socketName.isEmpty() || socketName.size() > 200 ||
        !validToken.match(token).hasMatch() ||
        (operation != QStringLiteral("--package") && operation != QStringLiteral("--remove"))) {
        errorStream << "error: invalid internal installation session parameters\n";
        return 2;
    }
    static const QRegularExpression safePackageName(QStringLiteral("^[a-z0-9][a-z0-9@._+\\-]*$"));
    if ((operation == QStringLiteral("--package") &&
         (!package.isAbsolute() || !package.exists() || !package.isFile() ||
          !package.fileName().contains(QStringLiteral(".pkg.tar.")) ||
          package.fileName().endsWith(QStringLiteral(".sig")))) ||
        (operation == QStringLiteral("--remove") &&
         !safePackageName.match(arguments.at(7)).hasMatch())) {
        errorStream << "error: invalid internal package operation parameters\n";
        return 2;
    }

    QLocalSocket socket;
    socket.connectToServer(socketName, QIODevice::ReadWrite);
    if (!socket.waitForConnected(10000)) {
        errorStream << "error: could not connect to PacSmith: " << socket.errorString() << '\n';
        return 2;
    }
    bool channelAvailable = true;
    const auto sendEvent = [&socket, &channelAvailable](const pacsmith::InstallSessionEvent &event) {
        if (!channelAvailable || socket.state() != QLocalSocket::ConnectedState) return false;
        const auto message = pacsmith::InstallSessionProtocol::encode(event);
        if (socket.write(message) != message.size() || !socket.waitForBytesWritten(5000)) {
            channelAvailable = false;
            return false;
        }
        return true;
    };
    if (!sendEvent({QStringLiteral("started"), token, {}})) {
        errorStream << "error: could not authenticate the PacSmith installation session\n";
        return 2;
    }

    pacsmith::InstallService service;
    int exitCode = 1;
    bool completed = false;
    bool eventLoopRunning = false;
    const auto finishSession = [&](const pacsmith::ProcessResult &result) {
        if (completed) return;
        completed = true;
        exitCode = result.succeeded() ? 0 : 1;
        static_cast<void>(sendEvent({QStringLiteral("finished"), token, {}, result.exitCode,
                                     result.exitStatus, result.canceled}));
        if (socket.state() == QLocalSocket::ConnectedState) {
            socket.disconnectFromServer();
            if (socket.state() != QLocalSocket::UnconnectedState) socket.waitForDisconnected(2000);
        }
        out << '\n'
            << (result.succeeded() ? "PacSmith: package installation completed successfully.\n"
                                   : "PacSmith: package installation did not complete successfully.\n");
        if (!channelAvailable) {
            out << "PacSmith: the GUI connection was lost; reopen the project to refresh its status.\n";
        }
        out << "Press Enter to close this terminal." << Qt::flush;
        if (::isatty(STDIN_FILENO) != 0) {
            QTextStream input(stdin);
            static_cast<void>(input.readLine());
        } else {
            out << '\n';
        }
        if (eventLoopRunning) application.quit();
    };
    QObject::connect(&service, &pacsmith::InstallService::outputAvailable, &application,
                     [&](const QString &text) {
                         out << text << Qt::flush;
                         static_cast<void>(sendEvent({QStringLiteral("output"), token, text}));
                     });
    QObject::connect(&service, &pacsmith::InstallService::failedToStart, &application,
                     [&](const QString &message) {
                         errorStream << "error: " << message << '\n' << Qt::flush;
                         static_cast<void>(sendEvent({QStringLiteral("output"), token,
                                                      QStringLiteral("error: %1\n").arg(message)}));
                         pacsmith::ProcessResult result;
                         result.exitStatus = QProcess::CrashExit;
                         result.finishedAt = QDateTime::currentDateTimeUtc();
                         finishSession(result);
                     });
    QObject::connect(&service, &pacsmith::InstallService::finished, &application,
                     finishSession);
    if (operation == QStringLiteral("--remove")) service.startUninstall(arguments.at(7));
    else service.start(std::filesystem::path(package.absoluteFilePath().toUtf8().constData()));
    if (!completed && service.isRunning()) {
        eventLoopRunning = true;
        application.exec();
    }
    return exitCode;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("pacsmith"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PACSMITH_VERSION));
    QTextStream out(stdout);
    QTextStream errorStream(stderr);
    out.setEncoding(QStringConverter::Utf8);
    errorStream.setEncoding(QStringConverter::Utf8);

    const auto arguments = application.arguments();
    if (arguments.size() < 2 || arguments.at(1) == QStringLiteral("help") ||
        arguments.at(1) == QStringLiteral("--help") || arguments.at(1) == QStringLiteral("-h")) {
        printUsage(arguments.size() < 2 ? errorStream : out);
        return arguments.size() < 2 ? 1 : 0;
    }

    const auto command = arguments.at(1);
    if (command == QStringLiteral("_install-session")) {
        return runInstallSession(application, arguments, out, errorStream);
    }
    if (command == QStringLiteral("connect")) {
        return runConnectCommand(arguments, out, errorStream);
    }
    if (command == QStringLiteral("skill") || command == QStringLiteral("plugin")) {
        return pacsmith::cli::runAgentIntegrationCommand(arguments, out, errorStream);
    }
    if (command == QStringLiteral("mcp") && arguments.size() >= 3 &&
        (arguments.at(2) == QStringLiteral("--help") || arguments.at(2) == QStringLiteral("-h"))) {
        out << "Run PacSmith's stdio Model Context Protocol server.\n\n"
               "Usage: pacsmith mcp\n\n"
               "The server uses the same configured local Unix-socket or remote HTTPS/mTLS "
               "PacSmith connection as this CLI. JSON-RPC is written only to stdout; diagnostics "
               "use stderr. Sensitive system, trust, credential, publication, automation, and "
               "destructive tools are marked for the MCP host's permission policy. "
               "`pacsmith plugin path` locates "
               "the portable Skill plus MCP bundle for harness installation.\n";
        return 0;
    }
    if (command == QStringLiteral("gui")) {
        QString program = QCoreApplication::applicationDirPath() + QStringLiteral("/pacsmith-gui");
        if (!QFileInfo::exists(program)) program = QStringLiteral("pacsmith-gui");
        return QProcess::startDetached(program, {}) ? 0 : 1;
    }
    pacsmith::LibraryClient library;
    QString runtimeError;
    if (!pacsmith::applyLibraryRuntime(library.config(), &runtimeError)) {
        errorStream << "error: " << runtimeError << '\n';
        return 1;
    }
    if (command == QStringLiteral("mcp")) {
        if (arguments.size() != 2) {
            errorStream << "error: use 'pacsmith mcp' or 'pacsmith mcp --help'\n";
            return 1;
        }
        return pacsmith::mcp::Server(std::move(library)).run();
    }
    if (command == QStringLiteral("server")) {
        return runServerCommand(arguments, library, out, errorStream);
    }
    if (command == QStringLiteral("clients")) {
        return runClientsCommand(arguments, library, out, errorStream);
    }
    if (command == QStringLiteral("add")) {
        if (arguments.size() < 3) {
            errorStream << "error: add requires a supported artifact path or GitHub URL\n";
            return 1;
        }
        if (arguments.at(2) == QStringLiteral("apt") ||
            arguments.at(2) == QStringLiteral("rpm")) {
            return runRepositoryAdd(arguments, library, out, errorStream);
        }
        const QUrl remote(arguments.at(2));
        const bool github = remote.isValid() && remote.scheme() == QStringLiteral("https") &&
                            remote.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) == 0;
        if (github) {
            const auto parts = remote.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (parts.size() < 2) {
                errorStream << "error: expected a GitHub repository, release, or asset URL\n";
                return 1;
            }
            QString assetRegex;
            bool prerelease = false;
            for (int index = 3; index < arguments.size(); ++index) {
                if (arguments.at(index) == QStringLiteral("--prerelease")) {
                    prerelease = true;
                } else if (arguments.at(index) == QStringLiteral("--asset-regex") &&
                           index + 1 < arguments.size()) {
                    assetRegex = arguments.at(++index);
                } else {
                    errorStream << "error: unknown or incomplete add option: " << arguments.at(index) << '\n';
                    return 1;
                }
            }
            if (parts.size() >= 6 && parts.at(2) == QStringLiteral("releases") &&
                parts.at(3) == QStringLiteral("download")) {
                if (assetRegex.isEmpty()) {
                    assetRegex = QRegularExpression::escape(parts.mid(5).join(QLatin1Char('/')));
                }
            } else if (parts.size() >= 5 && parts.at(2) == QStringLiteral("releases") &&
                       parts.at(3) == QStringLiteral("tag")) {
            }
            auto repositoryName = parts.at(1);
            if (repositoryName.endsWith(QStringLiteral(".git"))) repositoryName.chop(4);
            if (assetRegex.isEmpty() &&
                parts.at(0).compare(QStringLiteral("anderson-arlen"), Qt::CaseInsensitive) == 0 &&
                repositoryName.compare(QStringLiteral("pacsmith"), Qt::CaseInsensitive) == 0) {
                assetRegex = QStringLiteral(
                    R"(pacsmith-[0-9][A-Za-z0-9._+-]*-[0-9]+-x86_64\.pkg\.tar\.zst)");
            }
            const QRegularExpression expression(assetRegex);
            if (assetRegex.isEmpty() || !expression.isValid()) {
                errorStream << "error: GitHub imports require --asset-regex <regex> matching exactly one release artifact"
                            << (expression.isValid() ? QString{} : QStringLiteral(": %1").arg(expression.errorString()))
                            << '\n';
                return 1;
            }
            QString importError;
            const auto imported = library.importGitHub(remote, assetRegex, prerelease, {},
                                                       &importError);
            if (!imported) {
                errorStream << "error: " << importError << '\n';
                return 1;
            }
            out << QString::fromUtf8(library.releasePath(imported->imported.project.id,
                                                         imported->imported.releaseId)
                                         .string().c_str()) << '\n';
            return 0;
        }
        if (remote.isValid() && remote.scheme() == QStringLiteral("https")) {
            if (arguments.size() != 3) {
                errorStream << "error: direct URL imports do not accept GitHub asset options\n";
                return 1;
            }
            QString importError;
            const auto imported = library.importRemoteUrl(remote, {}, {}, {}, &importError);
            if (!imported) {
                errorStream << "error: " << importError << '\n';
                return 1;
            }
            out << imported->project.id << '\t' << imported->releaseId << '\t'
                << imported->jobId << '\n';
            return 0;
        }
        if (arguments.size() != 3) {
            errorStream << "error: local artifact imports do not accept additional options\n";
            return 1;
        }
        QString error;
        const auto absolute = QFileInfo(arguments.at(2)).absoluteFilePath();
        const auto project = library.importSource(
            std::filesystem::path(absolute.toUtf8().constData()), {}, &error);
        if (!project) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        out << QString::fromUtf8(library.releasePath(project->project.id, project->releaseId).string().c_str()) << '\n';
        return 0;
    }
    if (command == QStringLiteral("list")) {
        QString error;
        const auto projects = library.list(&error);
        for (const auto &project : projects) {
            const auto *release = project.newestRelease();
            out << project.id << '\t' << project.archPackageName << '\t'
                << (release == nullptr ? QString{} : release->debian.version) << '\t'
                << (release == nullptr ? QStringLiteral("no-releases")
                                       : pacsmith::buildStatusName(release->buildStatus)) << '\n';
        }
        if (!error.isEmpty()) errorStream << "warning: " << error << '\n';
        return 0;
    }
    if (command == QStringLiteral("check") && arguments.size() == 3 && arguments.at(2) == QStringLiteral("--all")) {
        QString error;
        const auto started = library.startUpdateCheck({}, false, &error);
        if (!started) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        const auto job = library.waitForJob(started->id, &error);
        const auto log = library.jobLog(started->id, nullptr);
        if (!log.isEmpty()) errorStream << log;
        if (!job || job->status != QStringLiteral("succeeded")) {
            errorStream << "error: " << (!error.isEmpty() ? error : job ? job->error
                                                                    : QStringLiteral("update checks failed")) << '\n';
            return 1;
        }
        for (const auto &value : job->result.value(QStringLiteral("checks")).toArray()) {
            const auto checked = value.toObject();
            const auto projectId = checked.value(QStringLiteral("project_id")).toString();
            const auto version = checked.value(QStringLiteral("detected_version")).toString();
            if (checked.value(QStringLiteral("prepared")).toBool()) {
                out << projectId << "\tprepared\t" << version << '\n';
            }
            if (checked.value(QStringLiteral("built")).toBool()) {
                out << projectId << "\tbuilt\t" << version << '\n';
            }
            out << projectId << '\t' << checked.value(QStringLiteral("status")).toString()
                << '\t' << checked.value(QStringLiteral("message")).toString() << '\n';
        }
        return job->result.value(QStringLiteral("failed")).toInt() == 0 ? 0 : 1;
    }
    if (arguments.size() < 3) {
        errorStream << "error: " << command << " requires a project ID or name\n";
        return 1;
    }
    auto projectArgument = arguments.at(2);
    QStringList packageAuthorizationOptions;
    if (command == QStringLiteral("install") || command == QStringLiteral("uninstall")) {
        if (arguments.at(2) == QStringLiteral("--polkit")) {
            if (arguments.size() < 4) {
                errorStream << "error: " << command << " requires a project ID or name\n";
                return 1;
            }
            projectArgument = arguments.at(3);
            packageAuthorizationOptions.append(QStringLiteral("--polkit"));
            packageAuthorizationOptions.append(arguments.sliced(4));
        } else {
            packageAuthorizationOptions = arguments.sliced(3);
        }
    }
    auto project = requireProject(library, projectArgument, errorStream);
    if (!project) return 1;
    auto *release = project->newestRelease();
    if (release == nullptr) {
        errorStream << "error: project has no releases\n";
        return 1;
    }

    if (command == QStringLiteral("custom-file")) {
        if (arguments.size() < 4) {
            errorStream << "error: custom-file requires list, read, write, or delete\n";
            return 1;
        }
        const auto action = arguments.at(3);
        if (action == QStringLiteral("list") && arguments.size() == 4) {
            for (auto iterator = release->customFiles.cbegin(); iterator != release->customFiles.cend(); ++iterator) {
                out << iterator.key() << '\n';
            }
            return 0;
        }
        if ((action == QStringLiteral("read") || action == QStringLiteral("delete")) &&
            arguments.size() == 5) {
            const auto name = arguments.at(4);
            QString error;
            if (action == QStringLiteral("read")) {
                const auto contents = library.readFile(release->id, name, &error);
                if (!contents) {
                    errorStream << "error: " << error << '\n';
                    return 1;
                }
                out << *contents;
                return 0;
            }
            if (!library.deleteFile(release->id, name, release->revision, &error)) {
                errorStream << "error: " << error << '\n';
                return 1;
            }
            return 0;
        }
        if (action == QStringLiteral("write") && arguments.size() == 6) {
            QFile input(arguments.at(5));
            if (!input.open(QIODevice::ReadOnly)) {
                errorStream << "error: could not read " << arguments.at(5) << ": " << input.errorString() << '\n';
                return 1;
            }
            QString error;
            if (!library.writeFile(release->id, arguments.at(4), QString::fromUtf8(input.readAll()),
                                   release->revision, &error)) {
                errorStream << "error: " << error << '\n';
                return 1;
            }
            return 0;
        }
        errorStream << "error: usage: pacsmith custom-file <project> "
                       "list|read <name>|write <name> <path>|delete <name>\n";
        return 1;
    }

    if (command == QStringLiteral("info")) {
        out << "id\t" << project->id << '\n'
            << "name\t" << scriptFriendly(project->displayName) << '\n'
            << "arch-package\t" << project->archPackageName << '\n'
            << "artifact-type\t" << pacsmith::sourcePackageTypeName(release->sourceType) << '\n'
            << "acquisition\t" << pacsmith::acquisitionKindName(release->acquisition.kind) << '\n'
            << "source-package\t" << release->debian.package << '\n'
            << "version\t" << release->debian.version << '\n'
            << "architecture\t" << release->debian.architecture << '\n'
            << "maintainer\t" << scriptFriendly(release->debian.maintainer) << '\n'
            << "homepage\t" << release->debian.homepage << '\n'
            << "source\t" << release->originalSourceFilename << '\n'
            << "sha256\t" << release->sourceSha256 << '\n'
            << "scripts\t" << release->maintainerScripts.size() << '\n'
            << "payload-entries\t" << release->payload.size() << '\n'
            << "pkgbuild-modified\t" << (release->pkgbuildManuallyModified ? "yes" : "no") << '\n';
        return 0;
    }
    if (command == QStringLiteral("versions")) {
        for (const auto &candidate : project->releases) {
            out << candidate.id << '\t' << candidate.debian.version << '\t'
                << pacsmith::releaseStateName(candidate.state) << '\t'
                << (candidate.id == project->installedReleaseId ? "installed" : "-") << '\t'
                << candidate.builds.size() << " build(s)\n";
        }
        if (project->externallyInstalled) {
            out << "external\t" << project->installedVersion << "\texternally-installed\tinstalled\t0 build(s)\n";
        }
        return 0;
    }
    if (command == QStringLiteral("dependencies")) {
        for (const auto &dependency : release->dependencies) {
            out << dependency.rawExpression << '\t' << dependency.archPackage << '\t'
                << pacsmith::mappingStatusName(dependency.status) << '\t' << dependency.mappingSource << '\n';
        }
        return 0;
    }
    if (command == QStringLiteral("scripts")) {
        if (arguments.size() == 5 && arguments.at(3) == QStringLiteral("--acknowledge")) {
            const auto scriptName = arguments.at(4);
            const auto iterator = std::find_if(release->maintainerScripts.begin(), release->maintainerScripts.end(),
                                               [&scriptName](const auto &script) { return script.name == scriptName; });
            if (iterator == release->maintainerScripts.end()) {
                errorStream << "error: maintainer script not found: " << scriptName << '\n';
                return 1;
            }
            iterator->acknowledge();
            QString saveError;
            if (!library.save(*project, &saveError)) {
                errorStream << "error: " << saveError << '\n';
                return 1;
            }
            out << project->id << '\t' << iterator->name << "\tacknowledged\n";
            return 0;
        }
        for (const auto &finding : release->scriptFindings) {
            out << "RESPONSIBILITY\t" << finding.scriptName << '\t' << finding.kind << '\t'
                << pacsmith::scriptDispositionName(finding.disposition) << '\t'
                << pacsmith::valueOriginName(finding.provenance.origin) << '\t'
                << scriptFriendly(finding.summary) << '\n';
        }
        for (const auto &script : release->maintainerScripts) {
            out << "===== " << script.name << (script.requiresReview() ? " (REVIEW REQUIRED)" : " (ACKNOWLEDGED)")
                << " =====\n" << script.contents;
            if (!script.contents.endsWith(QLatin1Char('\n'))) out << '\n';
        }
        return 0;
    }
    if (command == QStringLiteral("lifecycle")) {
        if (arguments.size() == 4 && arguments.at(3) == QStringLiteral("--discard")) {
            QString saveError;
            if (!library.removeLifecycle(*project, *release, &saveError)) {
                errorStream << "error: " << saveError << '\n';
                return 1;
            }
            release->generatedPkgbuild = pacsmith::PkgbuildGenerator::generate(*release);
            release->generatedPkgbuildSha256 = pacsmith::sha256Hex(release->generatedPkgbuild.toUtf8());
            const auto saved = !release->pkgbuildManuallyModified
                                   ? library.savePkgbuild(*project, *release, release->generatedPkgbuild, &saveError)
                                   : library.save(*project, &saveError);
            if (!saved) {
                errorStream << "error: " << saveError << '\n';
                return 1;
            }
            out << project->id << "\tlifecycle-discarded\n";
            return 0;
        }
        if (arguments.size() == 5 && arguments.at(3) == QStringLiteral("--acknowledge")) {
            if (release->lifecycleScript.contents.isEmpty()) {
                errorStream << "error: this project has no Arch lifecycle script\n";
                return 1;
            }
            const auto fingerprint = release->lifecycleScript.contentFingerprint();
            if (arguments.at(4).compare(fingerprint, Qt::CaseInsensitive) != 0) {
                errorStream << "error: fingerprint does not match the exact current lifecycle content\n"
                            << "current: " << fingerprint << '\n';
                return 1;
            }
            if (!release->lifecycleScript.validationPassed) {
                errorStream << "error: lifecycle script failed validation: "
                            << release->lifecycleScript.validationMessage << '\n';
                return 1;
            }
            release->lifecycleScript.acknowledge();
            QString saveError;
            if (!library.save(*project, &saveError)) {
                errorStream << "error: " << saveError << '\n';
                return 1;
            }
            out << project->id << "\tlifecycle\tacknowledged\t" << fingerprint << '\n';
            return 0;
        }
        if (release->lifecycleScript.contents.isEmpty()) {
            out << "No Arch lifecycle script is configured.\n";
            return 0;
        }
        out << "file\t" << release->lifecycleScript.fileName << '\n'
            << "fingerprint\t" << release->lifecycleScript.contentFingerprint() << '\n'
            << "validation\t" << (release->lifecycleScript.validationPassed ? "passed" : "failed") << '\n'
            << "acknowledgement\t"
            << (release->lifecycleScript.requiresAcknowledgement() ? "required" : "acknowledged") << '\n'
            << "provenance\t" << pacsmith::valueOriginName(release->lifecycleScript.provenance.origin) << '\n'
            << "===== CONTENT =====\n" << release->lifecycleScript.contents;
        if (!release->lifecycleScript.contents.endsWith(QLatin1Char('\n'))) out << '\n';
        return 0;
    }
    if (command == QStringLiteral("payload")) {
        if (arguments.size() == 5 && arguments.at(3) == QStringLiteral("--show")) {
            const auto path = arguments.at(4);
            const auto entry = std::find_if(release->payload.cbegin(), release->payload.cend(),
                                            [&path](const auto &candidate) { return candidate.path == path; });
            if (entry == release->payload.cend()) {
                errorStream << "error: payload path not found: " << path << '\n';
                return 1;
            }
            QString inspectionError;
            const auto inspection = pacsmith::PayloadInspector::inspectFile(
                library.sourcePath(*release), path, &inspectionError);
            if (!inspection) {
                errorStream << "error: " << inspectionError << '\n';
                return 1;
            }
            out << "path\t" << path << '\n'
                << "sha256\t" << inspection->contentSha256 << '\n';
            if (inspection->binary) out << "[binary or non-UTF-8 content]\n";
            else out << inspection->textPreview;
            if (!inspection->textPreview.endsWith(QLatin1Char('\n'))) out << '\n';
            if (inspection->previewTruncated) out << "[preview truncated at 1 MiB]\n";
            return 0;
        }
        for (const auto &entry : release->payload) {
            out << entry.type << '\t' << entry.size << '\t' << entry.path;
            if (!entry.symlinkTarget.isEmpty()) out << " -> " << entry.symlinkTarget;
            if (entry.requiresReview) {
                const auto review = pacsmith::PayloadReview::state(*release, entry);
                if (review.needsReview) out << "\tREVIEW: choose keep or exclude — " << entry.reviewReason;
                else if (review.disposition == pacsmith::PayloadDisposition::Excluded) out << "\tEXCLUDED (acknowledged)";
                else out << "\tKEPT (acknowledged)";
            }
            out << '\n';
        }
        return 0;
    }
    if (command == QStringLiteral("pkgbuild")) {
        QString error;
        const auto contents = library.readPkgbuild(*release, &error);
        if (!contents) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        out << *contents;
        return 0;
    }
    if (command == QStringLiteral("check")) return runCheck(library, *project, out, errorStream);

    if (command == QStringLiteral("build")) {
        QString error;
        const auto started = library.startBuild(release->id, &error);
        if (!started) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        const auto job = library.waitForJob(started->id, &error);
        if (!job) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        const auto log = library.jobLog(job->id);
        if (!log.isEmpty()) out << log << Qt::flush;
        if (job->status != QStringLiteral("succeeded")) {
            errorStream << "error: " << (job->error.isEmpty() ? QStringLiteral("build failed") : job->error) << '\n';
            return 1;
        }
        return 0;
    }
    if (command == QStringLiteral("install")) {
        QString optionError;
        const auto privilegeMode = pacsmith::parseInstallPrivilegeOptions(
            packageAuthorizationOptions, &optionError);
        if (!privilegeMode) {
            errorStream << "error: " << optionError
                        << "\nusage: pacsmith install [--polkit] <project>\n";
            return 1;
        }
        if (!release->lifecycleScript.contents.isEmpty() &&
            (!release->lifecycleScript.validationPassed ||
             release->lifecycleScript.requiresAcknowledgement())) {
            errorStream << "error: installation is blocked until the exact validated Arch lifecycle script is acknowledged\n"
                        << "review: pacsmith lifecycle " << project->id << '\n'
                        << "acknowledge: pacsmith lifecycle " << project->id << " --acknowledge "
                        << release->lifecycleScript.contentFingerprint() << '\n';
            return 1;
        }
        QString error;
        QString packagePath;
        if (!release->builtArtifactIds.isEmpty()) {
            packagePath = library.cacheArtifact(release->builtArtifactIds.first(),
                                                QStringLiteral("package.pkg.tar.zst"), &error);
        } else if (!release->producedPackages.isEmpty()) packagePath = release->producedPackages.first();
        if (packagePath.isEmpty()) {
            errorStream << "error: no built package is recorded; run pacsmith build first\n";
            return 1;
        }
        pacsmith::InstallService service;
        int exitCode = 1;
        QObject::connect(&service, &pacsmith::InstallService::outputAvailable, &application,
                         [&out](const QString &text) { out << text << Qt::flush; });
        QObject::connect(&service, &pacsmith::InstallService::failedToStart, &application,
                         [&library, &project, releaseId = release->id,
                          &errorStream](const QString &message) {
                             errorStream << "error: " << message << '\n';
                             QString historyError;
                             if (!library.recordPackageOperation(*project, releaseId,
                                                                 QStringLiteral("install"),
                                                                 -1, false, message,
                                                                 &historyError)) {
                                 errorStream << "warning: " << historyError << '\n';
                             }
                             QCoreApplication::exit(1);
                         });
        QObject::connect(&service, &pacsmith::InstallService::finished, &application,
                         [&library, &project, releaseId = release->id,
                          &errorStream, &exitCode](const pacsmith::ProcessResult &result) {
                             QString historyError;
                             if (!library.recordPackageOperation(*project, releaseId,
                                                                 QStringLiteral("install"),
                                                                 result.exitCode, result.canceled,
                                                                 result.errorOutput,
                                                                 &historyError)) {
                                 errorStream << "warning: " << historyError << '\n';
                             }
                             if (result.succeeded()) static_cast<void>(library.reconcileInstalled(*project, nullptr));
                             if (result.succeeded()) {
                                 static_cast<void>(
                                     pacsmith::BackgroundUpdateStateStore::syncAvailableUpdates(library.list()));
                             }
                             exitCode = result.succeeded() ? 0 : 1;
                             QCoreApplication::exit(exitCode);
        });
        service.start(std::filesystem::path(packagePath.toUtf8().constData()), *privilegeMode);
        if (!service.isRunning()) return exitCode;
        application.exec();
        return exitCode;
    }
    if (command == QStringLiteral("rollback")) {
        if (arguments.size() < 4) {
            errorStream << "error: rollback requires a release ID or version\n";
            return 1;
        }
        QString optionError;
        const auto privilegeMode = pacsmith::parseInstallPrivilegeOptions(
            arguments.sliced(4), &optionError);
        if (!privilegeMode) {
            errorStream << "error: " << optionError
                        << "\nusage: pacsmith rollback <project> <release-id|version> [--polkit]\n";
            return 1;
        }
        const auto selected = std::find_if(project->releases.begin(), project->releases.end(),
                                           [&](const auto &candidate) {
                                               return candidate.id == arguments.at(3) ||
                                                      candidate.debian.version == arguments.at(3);
                                           });
        if (selected == project->releases.end()) {
            errorStream << "error: retained release not found: " << arguments.at(3) << '\n';
            return 1;
        }
        QString packagePath;
        for (auto build = selected->builds.crbegin(); build != selected->builds.crend() && packagePath.isEmpty(); ++build) {
            for (const auto &artifact : build->artifacts) {
                const auto candidate = library.releasePath(*selected) /
                    std::filesystem::path(artifact.relativePath.toUtf8().constData());
                if (QFileInfo::exists(QString::fromUtf8(candidate.string().c_str()))) {
                    packagePath = QString::fromUtf8(candidate.string().c_str());
                    break;
                }
            }
        }
        if (packagePath.isEmpty()) {
            for (const auto &candidate : selected->producedPackages) {
                if (QFileInfo::exists(candidate)) { packagePath = candidate; break; }
            }
        }
        if (packagePath.isEmpty()) {
            errorStream << "error: this release has no retained Arch package artifact\n";
            return 1;
        }
        pacsmith::InstallService service;
        int exitCode = 1;
        QObject::connect(&service, &pacsmith::InstallService::outputAvailable, &application,
                         [&out](const QString &text) { out << text << Qt::flush; });
        QObject::connect(&service, &pacsmith::InstallService::failedToStart, &application,
                         [&library, &project, releaseId = selected->id,
                          &errorStream](const QString &message) {
                             errorStream << "error: " << message << '\n';
                             QString historyError;
                             if (!library.recordPackageOperation(*project, releaseId,
                                                                 QStringLiteral("rollback"),
                                                                 -1, false, message,
                                                                 &historyError)) {
                                 errorStream << "warning: " << historyError << '\n';
                             }
                             QCoreApplication::exit(1);
                         });
        QObject::connect(&service, &pacsmith::InstallService::finished, &application,
                         [&library, &project, releaseId = selected->id,
                          &errorStream, &exitCode](const pacsmith::ProcessResult &result) {
                             QString historyError;
                             if (!library.recordPackageOperation(*project, releaseId,
                                                                 QStringLiteral("rollback"),
                                                                 result.exitCode, result.canceled,
                                                                 result.errorOutput,
                                                                 &historyError)) {
                                 errorStream << "warning: " << historyError << '\n';
                             }
                             if (result.succeeded()) static_cast<void>(library.reconcileInstalled(*project, nullptr));
                             if (result.succeeded()) {
                                 static_cast<void>(
                                     pacsmith::BackgroundUpdateStateStore::syncAvailableUpdates(library.list()));
                             }
                             exitCode = result.succeeded() ? 0 : 1;
                             QCoreApplication::exit(exitCode);
                         });
        service.start(std::filesystem::path(packagePath.toUtf8().constData()), *privilegeMode);
        if (service.isRunning()) application.exec();
        return exitCode;
    }
    if (command == QStringLiteral("uninstall")) {
        QString optionError;
        const auto privilegeMode = pacsmith::parseInstallPrivilegeOptions(
            packageAuthorizationOptions, &optionError);
        if (!privilegeMode) {
            errorStream << "error: " << optionError
                        << "\nusage: pacsmith uninstall [--polkit] <project>\n";
            return 1;
        }
        if (project->installedVersion.isEmpty()) {
            out << project->id << "\tnot-installed\n";
            return 0;
        }
        pacsmith::InstallService service;
        int exitCode = 1;
        QObject::connect(&service, &pacsmith::InstallService::outputAvailable, &application,
                         [&out](const QString &text) { out << text << Qt::flush; });
        QObject::connect(&service, &pacsmith::InstallService::failedToStart, &application,
                         [&library, &project, &errorStream](const QString &message) {
                             errorStream << "error: " << message << '\n';
                             QString historyError;
                             if (!library.recordPackageOperation(*project, {},
                                                                 QStringLiteral("uninstall"),
                                                                 -1, false, message,
                                                                 &historyError)) {
                                 errorStream << "warning: " << historyError << '\n';
                             }
                             QCoreApplication::exit(1);
                         });
        QObject::connect(&service, &pacsmith::InstallService::finished, &application,
                         [&library, &project, &errorStream, &exitCode](const pacsmith::ProcessResult &result) {
                             QString historyError;
                             if (!library.recordPackageOperation(*project, {},
                                                                 QStringLiteral("uninstall"),
                                                                 result.exitCode, result.canceled,
                                                                 result.errorOutput,
                                                                 &historyError)) {
                                 errorStream << "warning: " << historyError << '\n';
                             }
                             if (result.succeeded()) static_cast<void>(library.reconcileInstalled(*project, nullptr));
                             if (result.succeeded()) {
                                 static_cast<void>(
                                     pacsmith::BackgroundUpdateStateStore::syncAvailableUpdates(library.list()));
                             }
                             exitCode = result.succeeded() ? 0 : 1;
                             QCoreApplication::exit(exitCode);
                         });
        service.startUninstall(project->archPackageName, *privilegeMode);
        if (service.isRunning()) application.exec();
        return exitCode;
    }

    errorStream << "error: unknown command: " << command << '\n';
    printUsage(errorStream);
    return 1;
}
