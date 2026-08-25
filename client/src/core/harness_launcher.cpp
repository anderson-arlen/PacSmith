#include "core/harness_launcher.hpp"

#include <QProcess>

namespace pacsmith {
namespace {

QString basePrompt(const QString &projectId, const QString &releaseId) {
    auto prompt = QStringLiteral("Help me review PacSmith project %1").arg(projectId);
    if (!releaseId.isEmpty()) prompt += QStringLiteral(", release %1").arg(releaseId);
    prompt += QStringLiteral(
        ".\nUse the PacSmith MCP tools to inspect the actual current project/release state before "
        "making conclusions. Use get_payload for the package inventory and "
        "get_payload_file_inspection for detailed file or ELF evidence; do not download or unpack "
        "the source artifact for inspection. If PacSmith MCP tools are unavailable, do not use PacSmith CLI "
        "project commands, sockets, D-Bus, or daemon access as a substitute. Ask me whether to "
        "install the PacSmith integration, then after approval use this harness's native plugin or "
        "MCP installer with the bundle reported by `pacsmith plugin path`. Resume only after the "
        "PacSmith MCP tools are visible.");
    return prompt;
}

}

HarnessLaunchResult HarnessLauncher::launch(const HarnessProfile &profile, const QString &prompt) {
    HarnessLaunchResult result;
    if (profile.executable.trimmed().isEmpty()) {
        result.error = QStringLiteral("The harness executable is empty");
        return result;
    }
    bool insertedPrompt = false;
    const auto arguments = expandedArguments(profile, prompt, &insertedPrompt);
    result.promptNeedsClipboard = !insertedPrompt;
    result.started = QProcess::startDetached(profile.executable, arguments);
    if (!result.started) result.error = QStringLiteral("Could not start %1").arg(profile.executable);
    return result;
}

QStringList HarnessLauncher::expandedArguments(const HarnessProfile &profile,
                                               const QString &prompt,
                                               bool *promptInserted) {
    QStringList arguments;
    bool inserted = false;
    for (auto argument : profile.arguments) {
        if (argument.contains(QStringLiteral("{prompt}"))) {
            argument.replace(QStringLiteral("{prompt}"), prompt);
            inserted = true;
        }
        arguments.append(argument);
    }
    if (promptInserted != nullptr) *promptInserted = inserted;
    return arguments;
}

QString HarnessLauncher::projectPrompt(const QString &projectId, const QString &releaseId) {
    return basePrompt(projectId, releaseId);
}

QString HarnessLauncher::dependencyPrompt(const QString &projectId, const QString &releaseId,
                                          const QString &dependency) {
    return basePrompt(projectId, releaseId) +
           QStringLiteral("\nI am currently looking at dependency %1.").arg(dependency);
}

QString HarnessLauncher::buildFailurePrompt(const QString &projectId, const QString &releaseId) {
    return basePrompt(projectId, releaseId) + QStringLiteral(
        "\nThe build failed. Inspect the build records and logs through PacSmith MCP, then help me "
        "diagnose the failure. Ask me for additional runtime observations or files as useful.");
}

QString HarnessLauncher::appImagePrompt(const QString &projectId, const QString &releaseId) {
    return basePrompt(projectId, releaseId) + QStringLiteral(
        "\nThe original AppImage works but the extracted PacSmith package does not behave correctly. "
        "Inspect AppRun, launchers, payload evidence, and current recipe state through MCP. Ask me for "
        "runtime logs, screenshots, or other observations as useful.");
}

QString HarnessLauncher::customPkgbuildPrompt(const QString &projectId, const QString &releaseId) {
    return basePrompt(projectId, releaseId) + QStringLiteral(
        "\nI am looking at this release's Custom PKGBUILD. Preserve pacsmith.vars and applicable "
        "_PACSMITH_* variables, edit only this release's copied recipe, and keep automatic updates compatible.");
}

} // namespace pacsmith
