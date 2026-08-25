#include "cli/agent_integration.hpp"

#include "core/agent_skill.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>

namespace pacsmith::cli {
namespace {

bool runningFromBuildTree() {
    const QDir buildRoot(QStringLiteral(PACSMITH_BUILD_DIR));
    const auto executable = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const auto relativeExecutable = buildRoot.relativeFilePath(executable);
    return relativeExecutable != QStringLiteral("..") &&
           !relativeExecutable.startsWith(QStringLiteral("../"));
}

QString packagedSkillDirectory() {
    return QDir(packagedPluginDirectory()).filePath(QStringLiteral("skills/pacsmith"));
}

int runSkillCommand(const QStringList &arguments, QTextStream &out, QTextStream &errorStream) {
    const auto packaged = packagedSkillDirectory();
    const auto userSkill = AgentSkill::userDirectory();
    if (arguments.size() == 3 && arguments.at(2) == QStringLiteral("path")) {
        out << (AgentSkill::isSkillDirectory(userSkill) ? userSkill : packaged) << '\n';
        return 0;
    }
    if ((arguments.size() == 3 || arguments.size() == 4) &&
        arguments.at(2) == QStringLiteral("install")) {
        const bool force = arguments.size() == 4 && arguments.at(3) == QStringLiteral("--force");
        if (arguments.size() == 4 && !force) {
            errorStream << "error: the only supported install option is --force\n";
            return 1;
        }
        QString error;
        if (!AgentSkill::install(packaged, userSkill, force, &error)) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        out << userSkill << '\n';
        return 0;
    }
    if (arguments.size() == 3 && arguments.at(2) == QStringLiteral("uninstall")) {
        QString error;
        if (!AgentSkill::uninstall(userSkill, &error)) {
            errorStream << "error: " << error << '\n';
            return 1;
        }
        out << userSkill << '\n';
        return 0;
    }
    if (arguments.size() == 3 &&
        (arguments.at(2) == QStringLiteral("--help") ||
         arguments.at(2) == QStringLiteral("-h"))) {
        out << "Install PacSmith's Agent Skill into the shared user-level Agent Skills "
               "directory. The complete Skill plus MCP bundle is reported by `pacsmith plugin "
               "path`.\n\n"
               "Usage:\n"
               "  pacsmith skill install [--force]\n"
               "  pacsmith skill path\n"
               "  pacsmith skill uninstall\n\n"
               "The install destination is ~/.agents/skills/pacsmith. --force replaces an "
               "existing directory that PacSmith did not install.\n";
        return 0;
    }
    errorStream << "error: use 'pacsmith skill install [--force]', "
                   "'pacsmith skill path', or 'pacsmith skill uninstall'\n";
    return 1;
}

int runPluginCommand(const QStringList &arguments, QTextStream &out, QTextStream &errorStream) {
    if (arguments.size() == 3 && arguments.at(2) == QStringLiteral("path")) {
        const auto directory = packagedPluginDirectory();
        if (!AgentSkill::isPluginDirectory(directory)) {
            errorStream << "error: the packaged PacSmith Agent Plugin is missing or invalid\n";
            return 1;
        }
        out << directory << '\n';
        return 0;
    }
    if (arguments.size() == 3 &&
        (arguments.at(2) == QStringLiteral("--help") ||
         arguments.at(2) == QStringLiteral("-h"))) {
        out << "Locate PacSmith's portable Agent Plugins 1.0 bundle.\n\n"
               "Usage:\n"
               "  pacsmith plugin path\n\n"
               "The returned directory contains plugin.json, mcp.json, and the PacSmith Skill. "
               "Give that directory to the harness's native plugin installer after the user "
               "approves installation.\n";
        return 0;
    }
    errorStream << "error: use 'pacsmith plugin path' or 'pacsmith plugin --help'\n";
    return 1;
}

} // namespace

QString packagedPluginDirectory() {
    const auto source = QStringLiteral(PACSMITH_SOURCE_PLUGIN_DIR);
    const auto installed = QStringLiteral(PACSMITH_PLUGIN_DIR);
    if (runningFromBuildTree() && AgentSkill::isPluginDirectory(source)) return source;
    return AgentSkill::isPluginDirectory(installed) ? installed : source;
}

int runAgentIntegrationCommand(const QStringList &arguments, QTextStream &out,
                               QTextStream &errorStream) {
    if (arguments.size() < 2) return 1;
    if (arguments.at(1) == QStringLiteral("skill")) {
        return runSkillCommand(arguments, out, errorStream);
    }
    return runPluginCommand(arguments, out, errorStream);
}

} // namespace pacsmith::cli
