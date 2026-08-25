#include "core_tests.hpp"

#include "core/harness_launcher.hpp"
#include "core/http_transport.hpp"
#include "core/agent_skill.hpp"
#include "core/domain_validation.hpp"
#include "core/release_review.hpp"
#include "core/payload_review.hpp"
#include "mcp/permission_policy.hpp"
#include "mcp/server.hpp"

#include <QtTest>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

void CoreTests::describesDomainMcpToolsAndPermissions() {
    const auto catalog = pacsmith::mcp::Server::toolCatalog();
    QVERIFY(catalog.size() >= 25);
    QSet<QString> names;
    for (const auto &value : catalog) {
        const auto entry = value.toObject();
        const auto name = entry.value(QStringLiteral("name")).toString();
        QVERIFY(!name.isEmpty());
        QVERIFY(!names.contains(name));
        names.insert(name);
        const auto annotations = entry.value(QStringLiteral("annotations")).toObject();
        for (const auto &field : {"readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint"}) {
            QVERIFY2(annotations.value(QString::fromLatin1(field)).isBool(), qPrintable(name));
        }
        if (!annotations.value(QStringLiteral("readOnlyHint")).toBool()) {
            const auto properties = entry.value(QStringLiteral("inputSchema")).toObject()
                                        .value(QStringLiteral("properties")).toObject();
            QVERIFY2(!properties.contains(QStringLiteral("release_id")), qPrintable(name));
            QVERIFY2(!properties.contains(QStringLiteral("project")), qPrintable(name));
            QVERIFY2(!properties.contains(QStringLiteral("existing_project")), qPrintable(name));
        }
        QVERIFY(!name.contains(QStringLiteral("internal")));
        QVERIFY(!name.contains(QStringLiteral("arbitrary")));
        QVERIFY(!name.contains(QStringLiteral("json")));
    }
    QVERIFY(names.contains(QStringLiteral("get_dependencies")));
    QVERIFY(names.contains(QStringLiteral("get_package_metadata")));
    QVERIFY(names.contains(QStringLiteral("get_release_issues")));
    QVERIFY(names.contains(QStringLiteral("set_dependency_mapping")));
    QVERIFY(names.contains(QStringLiteral("set_package_metadata")));
    QVERIFY(names.contains(QStringLiteral("add_runtime_dependency")));
    QVERIFY(names.contains(QStringLiteral("remove_runtime_dependency")));
    QVERIFY(names.contains(QStringLiteral("write_pkgbuild")));
    QVERIFY(names.contains(QStringLiteral("promote_repository_package")));
    QVERIFY(names.contains(QStringLiteral("check_updates")));
    QVERIFY(names.contains(QStringLiteral("prepare_release")));
    QVERIFY(names.contains(QStringLiteral("import_github_release")));
    QVERIFY(names.contains(QStringLiteral("import_direct_url")));
    QVERIFY(names.contains(QStringLiteral("list_harness_profiles")));
    QVERIFY(names.contains(QStringLiteral("upsert_harness_profile")));
    QVERIFY(names.contains(QStringLiteral("remove_harness_profile")));
    QVERIFY(names.contains(QStringLiteral("set_default_harness_profile")));
    for (const auto &name : {QStringLiteral("get_library_settings"),
                             QStringLiteral("set_library_settings"),
                             QStringLiteral("get_client_preferences"),
                             QStringLiteral("set_client_preferences"),
                             QStringLiteral("get_repository_configuration"),
                             QStringLiteral("set_repository_configuration"),
                             QStringLiteral("initialize_repository_signing"),
                             QStringLiteral("set_remote_listening"),
                             QStringLiteral("list_remote_clients"),
                             QStringLiteral("approve_remote_registration"),
                             QStringLiteral("get_github_credential_status"),
                             QStringLiteral("set_github_credential"),
                             QStringLiteral("get_build_job"),
                             QStringLiteral("get_build_job_log"),
                             QStringLiteral("cancel_build_job"),
                             QStringLiteral("get_payload_file_inspection"),
                             QStringLiteral("download_artifact"),
                             QStringLiteral("remove_launcher"),
                             QStringLiteral("delete_desktop_entry"),
                             QStringLiteral("set_release_icon")}) {
        QVERIFY2(names.contains(name), qPrintable(name));
    }

    const auto findTool = [&](const QString &name) {
        for (const auto &value : catalog) {
            if (value.toObject().value(QStringLiteral("name")).toString() == name) {
                return value.toObject();
            }
        }
        return QJsonObject{};
    };
    const auto updateAnnotations =
        findTool(QStringLiteral("check_updates")).value(QStringLiteral("annotations")).toObject();
    QCOMPARE(updateAnnotations.value(QStringLiteral("readOnlyHint")).toBool(), false);
    QCOMPARE(updateAnnotations.value(QStringLiteral("destructiveHint")).toBool(), false);
    QCOMPARE(updateAnnotations.value(QStringLiteral("idempotentHint")).toBool(), false);
    QCOMPARE(updateAnnotations.value(QStringLiteral("openWorldHint")).toBool(), true);
    const auto issueAnnotations =
        findTool(QStringLiteral("get_release_issues")).value(QStringLiteral("annotations")).toObject();
    QCOMPARE(issueAnnotations.value(QStringLiteral("readOnlyHint")).toBool(), true);
    QCOMPARE(issueAnnotations.value(QStringLiteral("destructiveHint")).toBool(), false);
    const auto payloadInspection = findTool(QStringLiteral("get_payload_file_inspection"));
    QCOMPARE(payloadInspection.value(QStringLiteral("annotations")).toObject()
                 .value(QStringLiteral("readOnlyHint")).toBool(), true);
    const auto payloadInspectionSchema = payloadInspection.value(QStringLiteral("inputSchema")).toObject();
    QVERIFY(payloadInspectionSchema.value(QStringLiteral("properties")).toObject()
                .contains(QStringLiteral("path")));
    QVERIFY(payloadInspectionSchema.value(QStringLiteral("required")).toArray()
                .contains(QStringLiteral("path")));
    QVERIFY(findTool(QStringLiteral("download_artifact")).value(QStringLiteral("description"))
                .toString().contains(QStringLiteral("Do not use this to inspect")));
    for (const auto &toolName : {QStringLiteral("import_github_release"),
                                 QStringLiteral("import_direct_url")}) {
        const auto remoteImport = findTool(toolName);
        const auto schema = remoteImport.value(QStringLiteral("inputSchema")).toObject();
        const auto properties = schema.value(QStringLiteral("properties")).toObject();
        QVERIFY2(properties.contains(QStringLiteral("url")), qPrintable(toolName));
        QVERIFY2(properties.contains(QStringLiteral("existing_project_name")), qPrintable(toolName));
        QVERIFY2(schema.value(QStringLiteral("required")).toArray()
                     .contains(QStringLiteral("url")), qPrintable(toolName));
        QVERIFY2(remoteImport.value(QStringLiteral("description")).toString()
                     .contains(QStringLiteral("never download")), qPrintable(toolName));
        QCOMPARE(remoteImport.value(QStringLiteral("annotations")).toObject()
                     .value(QStringLiteral("openWorldHint")).toBool(), true);
    }
    QVERIFY(findTool(QStringLiteral("import_github_release"))
                .value(QStringLiteral("inputSchema")).toObject()
                .value(QStringLiteral("properties")).toObject()
                .contains(QStringLiteral("asset_regex")));
    const auto profileAnnotations =
        findTool(QStringLiteral("upsert_harness_profile")).value(QStringLiteral("annotations")).toObject();
    QCOMPARE(profileAnnotations.value(QStringLiteral("readOnlyHint")).toBool(), false);
    QCOMPARE(profileAnnotations.value(QStringLiteral("idempotentHint")).toBool(), true);
    const auto removeAnnotations =
        findTool(QStringLiteral("remove_harness_profile")).value(QStringLiteral("annotations")).toObject();
    QCOMPARE(removeAnnotations.value(QStringLiteral("destructiveHint")).toBool(), true);
    const auto repositorySchema = findTool(QStringLiteral("configure_project_repository"))
                                      .value(QStringLiteral("inputSchema")).toObject();
    QVERIFY(repositorySchema.value(QStringLiteral("properties")).toObject()
                .contains(QStringLiteral("project_name")));
    QVERIFY(!repositorySchema.value(QStringLiteral("properties")).toObject()
                 .contains(QStringLiteral("project")));
    QVERIFY(pacsmith::mcp::PermissionPolicy::confirmationMessage(
                QStringLiteral("configure_project_repository"),
                QStringLiteral("ChatGPT (chatgpt-bin)"))
                .contains(QStringLiteral("ChatGPT (chatgpt-bin)")));
    for (const auto &toolName : {QStringLiteral("prepare_release"),
                                 QStringLiteral("set_dependency_mapping"),
                                 QStringLiteral("start_build")}) {
        const auto properties = findTool(toolName).value(QStringLiteral("inputSchema")).toObject()
                                    .value(QStringLiteral("properties")).toObject();
        QVERIFY2(properties.contains(QStringLiteral("project_name")), qPrintable(toolName));
        QVERIFY2(properties.contains(QStringLiteral("release_name")), qPrintable(toolName));
    }

    QCOMPARE(pacsmith::mcp::PermissionPolicy::level(QStringLiteral("set_dependency_mapping")),
             pacsmith::mcp::PermissionLevel::Routine);
    QCOMPARE(pacsmith::mcp::PermissionPolicy::level(QStringLiteral("set_package_metadata")),
             pacsmith::mcp::PermissionLevel::Routine);
    QCOMPARE(pacsmith::mcp::PermissionPolicy::level(QStringLiteral("delete_project")),
             pacsmith::mcp::PermissionLevel::MandatoryConfirmation);
    QCOMPARE(pacsmith::mcp::PermissionPolicy::level(QStringLiteral("reanalyze_release")),
             pacsmith::mcp::PermissionLevel::MandatoryConfirmation);
    for (const auto &name : {QStringLiteral("set_library_settings"),
                             QStringLiteral("set_client_preferences"),
                             QStringLiteral("set_repository_configuration"),
                             QStringLiteral("initialize_repository_signing"),
                             QStringLiteral("upload_repository_root_key"),
                             QStringLiteral("set_remote_listening"),
                             QStringLiteral("approve_remote_registration"),
                             QStringLiteral("revoke_remote_client"),
                             QStringLiteral("set_github_credential"),
                             QStringLiteral("delete_github_credential")}) {
        QCOMPARE(pacsmith::mcp::PermissionPolicy::level(name),
                 pacsmith::mcp::PermissionLevel::MandatoryConfirmation);
        QString denied;
        QVERIFY(!pacsmith::mcp::PermissionPolicy::canProceedToConfirmation(name, false, &denied));
        QVERIFY(denied.contains(QStringLiteral("requires explicit PacSmith confirmation")));
    }
    QString error;
    QVERIFY(!pacsmith::mcp::PermissionPolicy::canProceedToConfirmation(
        QStringLiteral("delete_project"), false, &error));
    QVERIFY(error.contains(QStringLiteral("requires explicit PacSmith confirmation")));
    QVERIFY(pacsmith::mcp::PermissionPolicy::canProceedToConfirmation(
        QStringLiteral("delete_project"), true, &error));
}

void CoreTests::reportsStructuredReleaseReviewIssues() {
    pacsmith::PackageRelease release;
    release.debian.version = QStringLiteral("1.0");
    pacsmith::DependencyMapping dependency;
    dependency.rawExpression = QStringLiteral("vendor-runtime");
    release.dependencies.append(dependency);
    pacsmith::PayloadEntry payload;
    payload.path = QStringLiteral("etc/vendor.conf");
    payload.type = QStringLiteral("file");
    payload.size = 12;
    payload.requiresReview = true;
    payload.reviewReason = QStringLiteral("Configuration policy needs review");
    release.payload.append(payload);
    pacsmith::MaintainerScript script;
    script.name = QStringLiteral("postinst");
    script.contents = QStringLiteral("#!/bin/sh\nsystemctl enable vendor\n");
    release.maintainerScripts.append(script);
    pacsmith::ScriptFinding finding;
    finding.scriptName = QStringLiteral("postinst");
    finding.kind = QStringLiteral("service-enable");
    finding.summary = QStringLiteral("Vendor script enables a service");
    finding.evidence = QStringLiteral("systemctl enable vendor");
    finding.evidenceFingerprint = QStringLiteral("finding-fingerprint");
    finding.disposition = pacsmith::ScriptDisposition::Unresolved;
    release.scriptFindings.append(finding);

    auto issues = pacsmith::releaseReviewIssues(release);
    const auto hasCode = [&](const QString &code) {
        return std::any_of(issues.cbegin(), issues.cend(), [&](const auto &issue) {
            return issue.code == code;
        });
    };
    QVERIFY(hasCode(QStringLiteral("dependency-unresolved")));
    QVERIFY(hasCode(QStringLiteral("payload-decision-required")));
    QVERIFY(hasCode(QStringLiteral("vendor-script-finding-unresolved")));

    release.dependencies.first().status = pacsmith::MappingStatus::Resolved;
    release.dependencies.first().archPackage = QStringLiteral("vendor-runtime");
    pacsmith::PayloadReview::decide(release, QStringLiteral("etc/vendor.conf"), false);
    release.maintainerScripts.first().acknowledge();
    QVERIFY(pacsmith::releaseReviewIssues(release).isEmpty());
}

void CoreTests::validatesGuiAndMcpDomainEditsConsistently() {
    QVERIFY(pacsmith::DomainValidation::optDirectory(QStringLiteral("safe-name_1")).isEmpty());
    QVERIFY(!pacsmith::DomainValidation::optDirectory(QStringLiteral("../escape")).isEmpty());
    QVERIFY(pacsmith::DomainValidation::command(QStringLiteral("app"), QStringLiteral("/usr/bin/app")).isEmpty());
    QVERIFY(!pacsmith::DomainValidation::command(QStringLiteral("app;bad"), QStringLiteral("/tmp/app")).isEmpty());
    QVERIFY(pacsmith::DomainValidation::archPackageName(QStringLiteral("letos-bin")).isEmpty());
    QVERIFY(!pacsmith::DomainValidation::archPackageName(QStringLiteral("Letos bin")).isEmpty());
    QVERIFY(pacsmith::DomainValidation::packageRelation(QStringLiteral("libnotify>=0.8")).isEmpty());
    QVERIFY(!pacsmith::DomainValidation::packageRelation(QStringLiteral("libnotify;bad")).isEmpty());
    QVERIFY(pacsmith::DomainValidation::appRun(QStringLiteral("#!/bin/sh\nexec app\n")).isEmpty());
    QVERIFY(!pacsmith::DomainValidation::appRun(QStringLiteral("exec app\n")).isEmpty());
    const auto desktop = QStringLiteral("[Desktop Entry]\nName=App\nType=Application\nExec=/usr/bin/app\n");
    QVERIFY(pacsmith::DomainValidation::desktopEntry(desktop, QStringLiteral("/usr/share/applications/app.desktop")).isEmpty());
    QVERIFY(!pacsmith::DomainValidation::desktopEntry(desktop + QStringLiteral("Exec=$(bad)\n"), QStringLiteral("/tmp/app.desktop")).isEmpty());
    pacsmith::UpdateConfiguration github;
    github.strategy = pacsmith::UpdateStrategy::GitHubRelease;
    github.githubOwner = QStringLiteral("vendor");
    github.githubRepository = QStringLiteral("app");
    github.githubAssetRegex = QStringLiteral("app-.*\\.deb");
    QVERIFY(pacsmith::DomainValidation::updateConfiguration(github).isEmpty());
    github.githubAssetRegex = QStringLiteral("[");
    QVERIFY(!pacsmith::DomainValidation::updateConfiguration(github).isEmpty());
}

void CoreTests::preservesConfiguredRemoteConnectionForMcp() {
    auto config = pacsmith::ConnectionConfig::localDefault();
    config.mode = pacsmith::ConnectionConfig::Mode::Remote;
    config.remoteUrl = QUrl(QStringLiteral("https://library.example.invalid:9443"));
    pacsmith::mcp::Server server{pacsmith::LibraryClient(config)};
    QCOMPARE(server.connectionConfig().mode, pacsmith::ConnectionConfig::Mode::Remote);
    QCOMPARE(server.connectionConfig().remoteUrl,
             QUrl(QStringLiteral("https://library.example.invalid:9443")));
}

void CoreTests::buildsHarnessPromptsAndArgumentsSafely() {
    const auto hostilePrompt = QStringLiteral("review; $(touch /tmp/must-not-run) `false`");
    pacsmith::HarnessProfile profile{
        QStringLiteral("Harness"), QStringLiteral("harness"),
        {QStringLiteral("--new"), QStringLiteral("--prompt={prompt}"), QStringLiteral("literal argument")}, true};
    bool inserted = false;
    const auto arguments = pacsmith::HarnessLauncher::expandedArguments(profile, hostilePrompt, &inserted);
    QVERIFY(inserted);
    QCOMPARE(arguments.size(), 3);
    QCOMPARE(arguments.at(1), QStringLiteral("--prompt=") + hostilePrompt);
    QCOMPARE(arguments.at(2), QStringLiteral("literal argument"));

    const auto prompt = pacsmith::HarnessLauncher::dependencyPrompt(
        QStringLiteral("project-id"), QStringLiteral("release-id"), QStringLiteral("libfoo >= 2"));
    QVERIFY(prompt.contains(QStringLiteral("project-id")));
    QVERIFY(prompt.contains(QStringLiteral("release-id")));
    QVERIFY(prompt.contains(QStringLiteral("libfoo >= 2")));
    QVERIFY(prompt.contains(QStringLiteral("PacSmith MCP tools are unavailable")));
    QVERIFY(prompt.contains(QStringLiteral("do not use PacSmith CLI")));
    QVERIFY(prompt.contains(QStringLiteral("Ask me whether to install")));
    QVERIFY(prompt.contains(QStringLiteral("pacsmith plugin path")));
    QVERIFY(prompt.contains(QStringLiteral("Resume only after")));
}

void CoreTests::validatesPortableAgentPluginBundle() {
    const QDir plugin(QStringLiteral(PACSMITH_SOURCE_PLUGIN_DIR));
    QVERIFY(pacsmith::AgentSkill::isPluginDirectory(plugin.absolutePath()));

    QFile manifest(plugin.filePath(QStringLiteral("plugin.json")));
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const auto manifestDocument = QJsonDocument::fromJson(manifest.readAll());
    QVERIFY(manifestDocument.isObject());
    const auto manifestObject = manifestDocument.object();
    QCOMPARE(manifestObject.value(QStringLiteral("$schema")).toString(),
             QStringLiteral("https://agent-plugins.org/schemas/1.0.0/plugin.schema.json"));
    QCOMPARE(manifestObject.value(QStringLiteral("name")).toString(), QStringLiteral("pacsmith"));
    QCOMPARE(manifestObject.value(QStringLiteral("version")).toString(),
             QStringLiteral(PACSMITH_VERSION));

    QFile mcpConfig(plugin.filePath(QStringLiteral("mcp.json")));
    QVERIFY(mcpConfig.open(QIODevice::ReadOnly));
    const auto mcpDocument = QJsonDocument::fromJson(mcpConfig.readAll());
    QVERIFY(mcpDocument.isObject());
    const auto mcpObject = mcpDocument.object();
    QCOMPARE(mcpObject.value(QStringLiteral("$schema")).toString(),
             QStringLiteral("https://agent-plugins.org/schemas/1.0.0/mcp.schema.json"));
    const auto servers = mcpObject.value(QStringLiteral("mcpServers")).toObject();
    QCOMPARE(servers.size(), 1);
    const auto pacsmithServer = servers.value(QStringLiteral("pacsmith")).toObject();
    QCOMPARE(pacsmithServer.value(QStringLiteral("type")).toString(), QStringLiteral("stdio"));
    QCOMPARE(pacsmithServer.value(QStringLiteral("command")).toString(), QStringLiteral("pacsmith"));
    const auto arguments = pacsmithServer.value(QStringLiteral("args")).toArray();
    QCOMPARE(arguments.size(), 1);
    QCOMPARE(arguments.first().toString(), QStringLiteral("mcp"));

    QFile skill(plugin.filePath(QStringLiteral("skills/pacsmith/SKILL.md")));
    QVERIFY(skill.open(QIODevice::ReadOnly));
    const auto instructions = QString::fromUtf8(skill.readAll());
    QVERIFY(instructions.contains(QStringLiteral("Would you like me to install it?")));
    QVERIFY(instructions.contains(QStringLiteral("Never substitute PacSmith CLI")));
    QVERIFY(instructions.contains(QStringLiteral("Unix-socket access")));
    QVERIFY(instructions.contains(QStringLiteral("remote HTTPS/mTLS")));
    QVERIFY(instructions.contains(QStringLiteral("check_updates")));
    QVERIFY(instructions.contains(QStringLiteral("upsert_harness_profile")));
    QVERIFY(instructions.contains(QStringLiteral("Never substitute `pacsmith check`")));
    QVERIFY(instructions.contains(QStringLiteral("import_github_release")));
    QVERIFY(instructions.contains(QStringLiteral("import_direct_url")));
    QVERIFY(instructions.contains(QStringLiteral("pacsmith install <project_name>")));
    QVERIFY(instructions.contains(QStringLiteral(
        "must always use `pacsmith install --polkit <project_name>` first")));
    QVERIFY(instructions.contains(QStringLiteral(
        "a tool-provided pseudo-TTY does not mean the user can enter a sudo password")));
    QVERIFY(instructions.contains(QStringLiteral("never invoke sudo or pacman directly")));
}

void CoreTests::installsPortableAgentSkillSafely() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto source = QDir(temporary.path()).filePath(QStringLiteral("source/pacsmith"));
    QVERIFY(QDir().mkpath(QDir(source).filePath(QStringLiteral("references"))));
    QFile manifest(QDir(source).filePath(QStringLiteral("SKILL.md")));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    QVERIFY(manifest.write("---\nname: pacsmith\ndescription: test\n---\nversion one\n") > 0);
    manifest.close();
    QFile reference(QDir(source).filePath(QStringLiteral("references/policy.md")));
    QVERIFY(reference.open(QIODevice::WriteOnly));
    QCOMPARE(reference.write("first\n"), 6);
    reference.close();

    const auto target = pacsmith::AgentSkill::userDirectory(temporary.path());
    QString error;
    QVERIFY2(pacsmith::AgentSkill::install(source, target, false, &error), qPrintable(error));
    QVERIFY(pacsmith::AgentSkill::isSkillDirectory(target));
    QVERIFY(QFileInfo::exists(QDir(target).filePath(QStringLiteral(".pacsmith-managed"))));
    QFile installedReference(QDir(target).filePath(QStringLiteral("references/policy.md")));
    QVERIFY(installedReference.open(QIODevice::ReadOnly));
    QCOMPARE(installedReference.readAll(), QByteArray("first\n"));
    installedReference.close();

    QVERIFY(reference.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(reference.write("second\n"), 7);
    reference.close();
    QVERIFY2(pacsmith::AgentSkill::install(source, target, false, &error), qPrintable(error));
    QVERIFY(installedReference.open(QIODevice::ReadOnly));
    QCOMPARE(installedReference.readAll(), QByteArray("second\n"));
    installedReference.close();

    const auto unmanaged = QDir(temporary.path()).filePath(QStringLiteral("unmanaged/pacsmith"));
    QVERIFY(QDir().mkpath(unmanaged));
    QFile unmanagedManifest(QDir(unmanaged).filePath(QStringLiteral("SKILL.md")));
    QVERIFY(unmanagedManifest.open(QIODevice::WriteOnly));
    QVERIFY(unmanagedManifest.write("user-owned\n") > 0);
    unmanagedManifest.close();
    QVERIFY(!pacsmith::AgentSkill::install(source, unmanaged, false, &error));
    QVERIFY(error.contains(QStringLiteral("--force")));
    QVERIFY2(pacsmith::AgentSkill::install(source, unmanaged, true, &error), qPrintable(error));
    QVERIFY2(pacsmith::AgentSkill::uninstall(unmanaged, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(unmanaged));

    const auto userOwned = QDir(temporary.path()).filePath(QStringLiteral("user-owned/pacsmith"));
    QVERIFY(QDir().mkpath(userOwned));
    QVERIFY(!pacsmith::AgentSkill::uninstall(userOwned, &error));
    QVERIFY(QFileInfo::exists(userOwned));
}
