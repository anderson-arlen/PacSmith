#include "core/acp_environment.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace pacsmith {

bool isCodexAcp(const HarnessProfile &profile) {
    static const QRegularExpression adapter(QStringLiteral(
        R"((^|[/\\])codex-acp(?:@[^/]+|-(?:x64|arm64)-(?:linux|darwin|windows))?(?:\.exe)?$)"));
    for (const auto &part : QStringList{profile.executable} + profile.arguments) {
        if (adapter.match(part).hasMatch()) return true;
    }
    return false;
}

QString acpDataDirectory() {
    const auto xdg = qEnvironmentVariable("XDG_DATA_HOME");
    const auto root = QDir::isAbsolutePath(xdg) ? xdg : QDir::home().filePath(QStringLiteral(".local/share"));
    return QDir(root).filePath(QStringLiteral("pacsmith/ai"));
}

std::optional<AcpEnvironment> prepareAcpEnvironment(
    const HarnessProfile &profile, const QString &dataDirectory,
    const QProcessEnvironment &parent, QString *error) {
    const auto fail = [error](const QString &message) -> std::optional<AcpEnvironment> {
        if (error != nullptr) *error = message;
        return std::nullopt;
    };
    if (profile.executable.trimmed().isEmpty()) {
        return fail(QStringLiteral("Choose an ACP agent in Settings → AI Harness."));
    }
    for (const auto &argument : profile.arguments) {
        if (argument.contains(QStringLiteral("{prompt}"))) {
            return fail(QStringLiteral("ACP sends messages over its protocol. Remove {prompt} and terminal-launch arguments from this profile."));
        }
        if (isCodexAcp(profile) && (argument.contains(QStringLiteral("sqlite_home")) ||
            argument.contains(QStringLiteral("log_dir")) || argument.contains(QStringLiteral("--profile")))) {
            return fail(QStringLiteral("Codex storage overrides are managed by PacSmith to keep conversations separate."));
        }
    }
    AcpEnvironment result;
    result.dataDirectory = QDir(dataDirectory).absolutePath();
    result.workspace = QDir(result.dataDirectory).filePath(QStringLiteral("workspace"));
    result.process = parent;
    for (auto it = profile.environment.begin(); it != profile.environment.end(); ++it) result.process.insert(it.key(), it.value());
    if (!QDir().mkpath(result.workspace)) return fail(QStringLiteral("Could not create the PacSmith AI workspace."));
    QFile::setPermissions(result.dataDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    // These overrides apply only to the child agent. No personal configuration,
    // transcript, session index, or memory is copied or resumed.
    const auto home = QDir(result.dataDirectory).filePath(QStringLiteral("codex-home"));
    if (QFileInfo(home).isSymLink() || !QDir().mkpath(home)) {
        return fail(QStringLiteral("The private Codex directory must be a writable directory, not a symbolic link."));
    }
    QFile::setPermissions(home, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const auto source = parent.value(QStringLiteral("CODEX_HOME"),
        QDir(parent.value(QStringLiteral("HOME"), QDir::homePath())).filePath(QStringLiteral(".codex")));
    if (QFileInfo(source).canonicalFilePath() == QFileInfo(home).canonicalFilePath()) {
        return fail(QStringLiteral("PacSmith AI storage must be separate from the interactive Codex home."));
    }
    const auto auth = QDir(source).absoluteFilePath(QStringLiteral("auth.json"));
    const auto sharedAuth = QDir(home).filePath(QStringLiteral("auth.json"));
    if (QFileInfo::exists(auth) && !QFileInfo::exists(sharedAuth) && !QFileInfo(sharedAuth).isSymLink() &&
        !QFile::link(auth, sharedAuth)) {
        return fail(QStringLiteral("Could not share the Codex login. Sign in with Codex and retry."));
    }
    const QJsonObject config{
        {QStringLiteral("sqlite_home"), home},
        {QStringLiteral("log_dir"), QDir(home).filePath(QStringLiteral("logs"))},
        {QStringLiteral("approval_policy"), QStringLiteral("on-request")},
        {QStringLiteral("approvals_reviewer"), QStringLiteral("user")}};
    result.process.insert(QStringLiteral("CODEX_HOME"), home);
    result.process.insert(QStringLiteral("CODEX_SQLITE_HOME"), home);
    result.process.insert(QStringLiteral("CODEX_CONFIG"), QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Compact)));
    result.process.insert(QStringLiteral("APP_SERVER_LOGS"), QDir(home).filePath(QStringLiteral("logs")));
    result.process.insert(QStringLiteral("INITIAL_AGENT_MODE"), QStringLiteral("read-only"));
    result.process.remove(QStringLiteral("CODEX_THREAD_ID"));
    result.process.remove(QStringLiteral("CODEX_SESSION_ID"));
    return result;
}

QJsonArray acpMcpServers(const ConnectionConfig &connection, const QString &cliExecutable, const QString &conversationKey) {
    QJsonArray environment;
    const auto add = [&environment](const QString &name, const QString &value) {
        environment.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("value"), value}});
    };
    add(QStringLiteral("PACSMITH_CONVERSATION_KEY"), conversationKey);
    add(QStringLiteral("PACSMITH_CONVERSATION_DIRECTORY"), QDir(acpDataDirectory()).filePath(QStringLiteral("conversations")));
    add(QStringLiteral("PACSMITH_MCP_MODE"), connection.mode == ConnectionConfig::Mode::Local
        ? QStringLiteral("local") : QStringLiteral("remote"));
    add(QStringLiteral("PACSMITH_MCP_SOCKET"), connection.socketPath);
    add(QStringLiteral("PACSMITH_MCP_URL"), connection.remoteUrl.toString());
    add(QStringLiteral("PACSMITH_MCP_CA"), connection.serverCaPath);
    add(QStringLiteral("PACSMITH_MCP_CERT"), connection.clientCertPath);
    add(QStringLiteral("PACSMITH_MCP_KEY"), connection.clientKeyPath);
    return {QJsonObject{{QStringLiteral("name"), QStringLiteral("pacsmith_session")},
        {QStringLiteral("command"), cliExecutable}, {QStringLiteral("args"), QJsonArray{QStringLiteral("mcp")}},
        {QStringLiteral("env"), environment}}};
}

QString acpConversationKey(const ConnectionConfig &connection, const HarnessProfile &profile,
                           const QString &projectId, const QString &releaseId) {
    QJsonArray arguments;
    for (const auto &argument : profile.arguments) arguments.append(argument);
    QJsonObject environment;
    for (auto it = profile.environment.begin(); it != profile.environment.end(); ++it) environment.insert(it.key(), it.value());
    const QJsonArray identity{connection.origin(), connection.socketPath, connection.clientCertPath,
                             profile.executable, arguments, environment, projectId, releaseId};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),
                                                      QCryptographicHash::Sha256).toHex());
}

} // namespace pacsmith
