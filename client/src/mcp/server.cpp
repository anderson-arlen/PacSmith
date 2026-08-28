#include "mcp/server.hpp"

#include "core/app_settings.hpp"
#include "core/background_updates.hpp"
#include "core/credential_store.hpp"
#include "core/domain_validation.hpp"
#include "core/payload_review.hpp"
#include "core/pkgbuild_generator.hpp"
#include "core/release_review.hpp"
#include "core/remote_import_service.hpp"
#include "core/repository_key_download_service.hpp"
#include "core/repository_trust.hpp"
#include "core/update_check_runner.hpp"
#include "mcp/permission_policy.hpp"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTextStream>
#include <QUrl>

#include <algorithm>

namespace pacsmith::mcp {
namespace {

QFile &inputFile() {
    static QFile file;
    if (!file.isOpen() && !file.open(stdin, QIODevice::ReadOnly)) {
        QTextStream(stderr) << "pacsmith mcp: could not open stdin\n";
    }
    return file;
}

QFile &outputFile() {
    static QFile file;
    if (!file.isOpen() && !file.open(stdout, QIODevice::WriteOnly)) {
        QTextStream(stderr) << "pacsmith mcp: could not open stdout\n";
    }
    return file;
}

QJsonObject rpcResult(const QJsonValue &id, const QJsonValue &result) {
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("result"), result}};
}

QJsonObject rpcError(const QJsonValue &id, const int code, const QString &message) {
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code},
                                                   {QStringLiteral("message"), message}}}};
}

QJsonObject toolResult(const QJsonValue &id, const QJsonValue &value) {
    const auto wrapper = QJsonObject{{QStringLiteral("result"), value}};
    const auto text = QString::fromUtf8(QJsonDocument(wrapper).toJson(QJsonDocument::Indented));
    return rpcResult(id, QJsonObject{
        {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                           {QStringLiteral("text"), text}}}},
        {QStringLiteral("structuredContent"), wrapper},
        {QStringLiteral("isError"), false},
    });
}

QJsonObject toolError(const QJsonValue &id, const QString &message) {
    return rpcResult(id, QJsonObject{
        {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                           {QStringLiteral("text"), message}}}},
        {QStringLiteral("isError"), true},
    });
}

QJsonObject stringProperty(const QString &description) {
    return {{QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("description"), description}};
}

QJsonObject booleanProperty(const QString &description) {
    return {{QStringLiteral("type"), QStringLiteral("boolean")},
            {QStringLiteral("description"), description}};
}

QJsonObject integerProperty(const QString &description) {
    return {{QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("description"), description}};
}

QJsonObject stringArrayProperty(const QString &description) {
    return {{QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("description"), description},
            {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}};
}

QJsonObject objectSchema(const QJsonObject &properties = {}, const QStringList &required = {}) {
    QJsonArray requiredJson;
    for (const auto &name : required) requiredJson.append(name);
    QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
    if (!requiredJson.isEmpty()) schema.insert(QStringLiteral("required"), requiredJson);
    return schema;
}

QJsonObject annotations(const bool readOnly, const bool destructive,
                        const bool idempotent, const bool openWorld) {
    return {{QStringLiteral("readOnlyHint"), readOnly},
            {QStringLiteral("destructiveHint"), destructive},
            {QStringLiteral("idempotentHint"), idempotent},
            {QStringLiteral("openWorldHint"), openWorld}};
}

QJsonObject tool(const QString &name, const QString &description, const QJsonObject &schema,
                 const QJsonObject &toolAnnotations) {
    return {{QStringLiteral("name"), name},
            {QStringLiteral("description"), description},
            {QStringLiteral("inputSchema"), schema},
            {QStringLiteral("annotations"), toolAnnotations}};
}

QJsonArray tools() {
    const auto project = stringProperty(QStringLiteral("PacSmith project UUID, Arch package name, or display name."));
    const auto release = stringProperty(QStringLiteral("Opaque PacSmith release UUID."));
    const auto projectName = stringProperty(QStringLiteral("Human-readable PacSmith display name or Arch package name; do not use an opaque UUID."));
    const auto releaseName = stringProperty(QStringLiteral("Human-readable upstream version or source filename shown by list_projects."));
    const auto read = annotations(true, false, true, false);
    const auto write = annotations(false, false, true, false);
    const auto action = annotations(false, false, false, false);
    const auto sensitive = annotations(false, true, false, false);
    const auto sensitiveWrite = annotations(false, true, true, false);
    QJsonArray catalog{
        tool(QStringLiteral("list_projects"),
             QStringLiteral("List or search PacSmith projects with release summaries. Uses the configured PacSmith server connection."),
             objectSchema({{QStringLiteral("query"), stringProperty(QStringLiteral("Optional case-insensitive name or identity filter."))}}), read),
        tool(QStringLiteral("get_project"), QStringLiteral("Get project metadata and release summaries."),
             objectSchema({{QStringLiteral("project"), project}}, {QStringLiteral("project")}), read),
        tool(QStringLiteral("get_release"), QStringLiteral("Get release identity, acquisition, state, and artifact references."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), read),
        tool(QStringLiteral("get_release_issues"),
             QStringLiteral("Get every remaining structured PacSmith review issue for a release. Call this before building and again before claiming maintenance is complete; a successful build does not clear unresolved review items."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), read),
        tool(QStringLiteral("get_dependencies"), QStringLiteral("Get inspected vendor dependencies and current Arch mappings/treatments."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), read),
        tool(QStringLiteral("get_package_metadata"),
             QStringLiteral("Get the editable Arch package description, homepage, licenses, compatibility relations, and explicit runtime dependencies."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), read),
        tool(QStringLiteral("get_payload"), QStringLiteral("Get inspected payload evidence and current keep/exclude dispositions."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), read),
        tool(QStringLiteral("get_payload_file_inspection"),
             QStringLiteral("Statically inspect one exact payload path inside PacSmith. Returns original mode, size, SHA256, MIME/magic, bounded text, and ELF identity, interpreter, dependencies, paths, build ID, stripped/hardening, and bounded program-header/section evidence without downloading, extracting, or executing the vendor artifact."),
             objectSchema({{QStringLiteral("release_id"), release},
                           {QStringLiteral("path"), stringProperty(QStringLiteral("Exact path returned by get_payload."))}},
                          {QStringLiteral("release_id"), QStringLiteral("path")}), read),
        tool(QStringLiteral("get_lifecycle"), QStringLiteral("Get vendor maintainer scripts, structured lifecycle findings, and the current Arch lifecycle script."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), read),
        tool(QStringLiteral("get_install_configuration"), QStringLiteral("Get Guided layout, launchers, AppRun, desktop entries, and icon configuration."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), read),
        tool(QStringLiteral("get_update_configuration"), QStringLiteral("Get acquisition provenance, deterministic update-source configuration, and last discovery evidence."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), read),
        tool(QStringLiteral("get_recipe"), QStringLiteral("Get recipe mode, PKGBUILD, regenerated pacsmith.vars, lifecycle filename, and custom support-file names."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), read),
        tool(QStringLiteral("read_custom_support_file"), QStringLiteral("Read a text support file owned by this Custom PKGBUILD release."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("name"), stringProperty(QStringLiteral("Safe basename of the support file."))}},
                          {QStringLiteral("release_id"), QStringLiteral("name")}), read),
        tool(QStringLiteral("get_build_results"), QStringLiteral("Get build status, retained build records/logs, and package artifact IDs."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), read),
        tool(QStringLiteral("get_repository_state"), QStringLiteral("Get this project's normal PacSmith repository publication and soak state."),
             objectSchema({{QStringLiteral("project"), project}}, {QStringLiteral("project")}), read),
        tool(QStringLiteral("check_updates"),
             QStringLiteral("Run PacSmith's normal deterministic update check for one project or every project. This records discovery evidence and follows the user's automatic-preparation policy; it never invokes AI."),
             objectSchema({{QStringLiteral("project"), stringProperty(QStringLiteral("Optional PacSmith project UUID, package name, or display name. Omit to check all projects."))}}),
             annotations(false, false, false, true)),
        tool(QStringLiteral("prepare_release"),
             QStringLiteral("Download, verify, and inspect an already discovered release through PacSmith's normal deterministic preparation path."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}),
             annotations(false, false, false, true)),
        tool(QStringLiteral("get_connection_status"),
             QStringLiteral("Describe the exact local Unix-socket or remote authenticated PacSmith connection used by this MCP server."),
             objectSchema(), read),
        tool(QStringLiteral("get_library_settings"),
             QStringLiteral("Read deterministic update scheduling, automatic update handling, retention, and build parallelism settings from pacsmithd."),
             objectSchema(), read),
        tool(QStringLiteral("set_library_settings"),
             QStringLiteral("Change pacsmithd update scheduling, automatic update handling, retention, or build parallelism settings. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("updates_enabled"), booleanProperty(QStringLiteral("Run scheduled deterministic update checks."))},
                           {QStringLiteral("daily"), booleanProperty(QStringLiteral("Run daily rather than weekly."))},
                           {QStringLiteral("weekday"), integerProperty(QStringLiteral("ISO weekday 1 through 7."))},
                           {QStringLiteral("hour"), integerProperty(QStringLiteral("Local hour 0 through 23."))},
                           {QStringLiteral("minute"), integerProperty(QStringLiteral("Minute 0 through 59."))},
                           {QStringLiteral("automatically_prepare"), booleanProperty(QStringLiteral("Acquire, inspect, and build discovered updates automatically when copy-forward review finds no changes requiring attention; successful published-project builds enter unstable."))},
                           {QStringLiteral("retention_versions"), integerProperty(QStringLiteral("Number of completed versions to keep behind each package's oldest active distribution pointer, or -1 to keep them forever. Stable is the boundary when populated; otherwise Unstable is. Source and built-package artifacts are pruned together."))},
                           {QStringLiteral("build_parallelism"), integerProperty(QStringLiteral("Maximum compile jobs for subsequent package builds, from 1 through available_build_cores."))}}), sensitiveWrite),
        tool(QStringLiteral("get_client_preferences"),
             QStringLiteral("Read this PacSmith client's tray, login-start, and start-minimized preferences."),
             objectSchema(), read),
        tool(QStringLiteral("set_client_preferences"),
             QStringLiteral("Change this PacSmith client's tray and login-autostart preferences. PacSmith elicits consent because this may create or remove a desktop-session autostart entry."),
             objectSchema({{QStringLiteral("keep_in_tray"), booleanProperty(QStringLiteral("Keep the GUI running in the system tray."))},
                           {QStringLiteral("start_at_login"), booleanProperty(QStringLiteral("Start the GUI when the desktop session starts."))},
                           {QStringLiteral("start_minimized"), booleanProperty(QStringLiteral("Start the login-launched GUI in the tray."))}}), sensitiveWrite),
        tool(QStringLiteral("get_repository_configuration"),
             QStringLiteral("Read pacsmithd repository listener, signing, trust-mode, keyring, and retention/soak configuration."),
             objectSchema(), read),
        tool(QStringLiteral("set_repository_configuration"),
             QStringLiteral("Change global PacSmith repository channels, listener, publication defaults, or trust configuration. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("enabled"), booleanProperty(QStringLiteral("Serve the PacSmith pacman repository."))},
                           {QStringLiteral("listen_hosts"), stringArrayProperty(QStringLiteral("Exact listener host/address values."))},
                           {QStringLiteral("listen_port"), integerProperty(QStringLiteral("TCP port 1 through 65535."))},
                           {QStringLiteral("advertised_url"), stringProperty(QStringLiteral("Public base URL advertised to clients."))},
                           {QStringLiteral("stable_enabled"), booleanProperty(QStringLiteral("Add the system-wide Stable channel alongside Unstable."))},
                           {QStringLiteral("soak_seconds"), integerProperty(QStringLiteral("Default promotion delay used only when Stable is enabled and a project selects automatic promotion."))},
                           {QStringLiteral("package_name_prefix"), stringProperty(QStringLiteral("Optional repository package-name prefix."))},
                           {QStringLiteral("trust_mode"), stringProperty(QStringLiteral("direct or root-certified."))}}), sensitiveWrite),
        tool(QStringLiteral("initialize_repository_signing"),
             QStringLiteral("Initialize PacSmith repository signing keys. PacSmith always elicits explicit human consent."),
             objectSchema(), sensitive),
        tool(QStringLiteral("download_repository_public_key"),
             QStringLiteral("Download the repository public key through the configured PacSmith API to a new local file."),
             objectSchema({{QStringLiteral("destination"), stringProperty(QStringLiteral("Absolute path that must not already exist."))}},
                          {QStringLiteral("destination")}), annotations(false, false, false, false)),
        tool(QStringLiteral("upload_repository_root_key"),
             QStringLiteral("Install an armored root public key for certified repository trust. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("public_key"), stringProperty(QStringLiteral("Complete armored OpenPGP public key."))}},
                          {QStringLiteral("public_key")}), sensitiveWrite),
        tool(QStringLiteral("upload_repository_certified_key"),
             QStringLiteral("Install the root-certified PacSmith signing public key. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("public_key"), stringProperty(QStringLiteral("Complete armored certified OpenPGP public key."))}},
                          {QStringLiteral("public_key")}), sensitiveWrite),
        tool(QStringLiteral("get_repository_bootstrap_script"),
             QStringLiteral("Read the generated repository bootstrap script as unexecuted text for review."),
             objectSchema({{QStringLiteral("channel"), stringProperty(QStringLiteral("stable or unstable."))}}), read),
        tool(QStringLiteral("get_server_status"),
             QStringLiteral("Read local-admin server identity and remote-listener status through the configured connection. Remote clients are expected to receive a normal authorization error."),
             objectSchema(), read),
        tool(QStringLiteral("set_remote_listening"),
             QStringLiteral("Enable, disable, or configure pacsmithd's authenticated remote listener. PacSmith always elicits explicit human consent and the server enforces local-admin access."),
             objectSchema({{QStringLiteral("enabled"), booleanProperty(QStringLiteral("Accept authenticated remote clients."))},
                           {QStringLiteral("hosts"), stringArrayProperty(QStringLiteral("Listener host/address values."))},
                           {QStringLiteral("port"), integerProperty(QStringLiteral("TCP port 1 through 65535."))}},
                          {QStringLiteral("enabled")}), sensitiveWrite),
        tool(QStringLiteral("list_remote_clients"), QStringLiteral("List enrolled remote PacSmith clients; local-admin access is enforced by pacsmithd."), objectSchema(), read),
        tool(QStringLiteral("list_pending_registrations"), QStringLiteral("List pending remote-client enrollment requests; local-admin access is enforced by pacsmithd."), objectSchema(), read),
        tool(QStringLiteral("approve_remote_registration"), QStringLiteral("Approve a pending remote-client enrollment. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("registration_id"), stringProperty(QStringLiteral("Pending registration UUID."))}}, {QStringLiteral("registration_id")}), sensitive),
        tool(QStringLiteral("reject_remote_registration"), QStringLiteral("Reject a pending remote-client enrollment. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("registration_id"), stringProperty(QStringLiteral("Pending registration UUID."))}}, {QStringLiteral("registration_id")}), sensitive),
        tool(QStringLiteral("revoke_remote_client"), QStringLiteral("Revoke an enrolled remote client's trust. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("client_id"), stringProperty(QStringLiteral("Enrolled client UUID."))}}, {QStringLiteral("client_id")}), sensitive),
        tool(QStringLiteral("get_github_credential_status"), QStringLiteral("Read whether pacsmithd has a GitHub token and which secret backend stores it; the secret is never returned."), objectSchema(), read),
        tool(QStringLiteral("set_github_credential"), QStringLiteral("Store or replace pacsmithd's GitHub token. PacSmith always elicits explicit human consent and never echoes the secret."),
             objectSchema({{QStringLiteral("token"), stringProperty(QStringLiteral("GitHub access token."))}}, {QStringLiteral("token")}), sensitiveWrite),
        tool(QStringLiteral("delete_github_credential"), QStringLiteral("Delete pacsmithd's stored GitHub token. PacSmith always elicits explicit human consent."), objectSchema(), sensitiveWrite),
        tool(QStringLiteral("list_harness_profiles"),
             QStringLiteral("List the generic external AI harness launch profiles configured for this PacSmith client."),
             objectSchema(), read),
        tool(QStringLiteral("upsert_harness_profile"),
             QStringLiteral("Create or replace a generic external AI harness launch profile using a structured executable and argument array. Use {prompt} inside an argument to pass PacSmith's contextual prompt without a shell."),
             objectSchema({{QStringLiteral("name"), stringProperty(QStringLiteral("User-visible profile name."))},
                           {QStringLiteral("executable"), stringProperty(QStringLiteral("Executable name or absolute path; no shell command string."))},
                           {QStringLiteral("arguments"), stringArrayProperty(QStringLiteral("Exact argv entries. An entry may contain {prompt}."))},
                           {QStringLiteral("default"), booleanProperty(QStringLiteral("Make this the default launch profile."))}},
                          {QStringLiteral("name"), QStringLiteral("executable"), QStringLiteral("arguments")}), write),
        tool(QStringLiteral("remove_harness_profile"),
             QStringLiteral("Remove a generic external AI harness launch profile from the same client settings edited by PacSmith's GUI."),
             objectSchema({{QStringLiteral("name"), stringProperty(QStringLiteral("Exact profile name."))}},
                          {QStringLiteral("name")}), annotations(false, true, true, false)),
        tool(QStringLiteral("set_default_harness_profile"),
             QStringLiteral("Select which configured generic external AI harness profile PacSmith launches by default."),
             objectSchema({{QStringLiteral("name"), stringProperty(QStringLiteral("Exact profile name."))}},
                          {QStringLiteral("name")}), write),
        tool(QStringLiteral("import_artifact"), QStringLiteral("Create or update a project by uploading and inspecting a local first-party vendor artifact through the normal PacSmith HTTP API."),
             objectSchema({{QStringLiteral("path"), stringProperty(QStringLiteral("Absolute local path to a vendor artifact."))},
                           {QStringLiteral("existing_project"), project},
                           {QStringLiteral("canonical_identity"), stringProperty(QStringLiteral("Stable source identity such as vendor:product."))}},
                          {QStringLiteral("path")}), annotations(false, false, false, false)),
        tool(QStringLiteral("import_github_release"),
             QStringLiteral("Create or update a project directly from a first-party GitHub repository, release, or release-asset URL. PacSmith resolves the release, selects exactly one uploaded asset or generated source archive, downloads and verifies it, and performs normal inspection; never download the artifact with curl or another external tool."),
             objectSchema({{QStringLiteral("url"), stringProperty(QStringLiteral("HTTPS github.com repository, release, or release-asset URL."))},
                            {QStringLiteral("asset_regex"), stringProperty(QStringLiteral("Optional persistent regular expression that must full-match exactly one uploaded asset or generated source archive. An exact release-asset URL supplies this automatically."))},
                           {QStringLiteral("include_prereleases"), booleanProperty(QStringLiteral("Allow prerelease selection when a specific tag is not present in the URL."))},
                           {QStringLiteral("existing_project"), project}},
                          {QStringLiteral("url")}), annotations(false, false, false, true)),
        tool(QStringLiteral("import_direct_url"),
             QStringLiteral("Create or update a project directly from a first-party HTTPS artifact URL. PacSmith owns the download, SHA256 calculation, upload, and inspection; never download the artifact with curl or another external tool."),
             objectSchema({{QStringLiteral("url"), stringProperty(QStringLiteral("Direct HTTPS URL for a vendor package or archive."))},
                           {QStringLiteral("existing_project"), project}},
                          {QStringLiteral("url")}), annotations(false, false, false, true)),
        tool(QStringLiteral("update_project_metadata"), QStringLiteral("Edit ordinary project display/package/vendor metadata."),
             objectSchema({{QStringLiteral("project"), project}, {QStringLiteral("display_name"), stringProperty(QStringLiteral("Display name."))},
                           {QStringLiteral("arch_package_name"), stringProperty(QStringLiteral("Arch package name."))},
                           {QStringLiteral("vendor_name"), stringProperty(QStringLiteral("Vendor/maintainer name."))}},
                          {QStringLiteral("project")}), write),
        tool(QStringLiteral("set_dependency_mapping"), QStringLiteral("Set a dependency's ordinary Guided Arch mapping/treatment: resolved, ignored, bundled, provided, or unresolved."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("dependency"), stringProperty(QStringLiteral("Exact raw vendor dependency expression."))},
                           {QStringLiteral("status"), stringProperty(QStringLiteral("resolved, ignored, bundled, provided, or unresolved."))},
                           {QStringLiteral("arch_package"), stringProperty(QStringLiteral("Required Arch repository package when status is resolved."))}},
                          {QStringLiteral("release_id"), QStringLiteral("dependency"), QStringLiteral("status")}), write),
        tool(QStringLiteral("set_package_metadata"),
             QStringLiteral("Edit ordinary Arch package metadata used by Guided recipes and pacsmith.vars. Provides/conflicts are optional compatibility relationships and must not be inferred merely from a -bin suffix."),
             objectSchema({{QStringLiteral("release_id"), release},
                           {QStringLiteral("description"), stringProperty(QStringLiteral("Short Arch package description."))},
                           {QStringLiteral("homepage"), stringProperty(QStringLiteral("Absolute HTTP or HTTPS upstream homepage."))},
                           {QStringLiteral("licenses"), stringArrayProperty(QStringLiteral("Arch/SPDX license expressions."))},
                           {QStringLiteral("provides"), stringArrayProperty(QStringLiteral("Intentional package or virtual-package compatibility names."))},
                           {QStringLiteral("conflicts"), stringArrayProperty(QStringLiteral("Packages that cannot coexist with this package."))}},
                          {QStringLiteral("release_id")}), write),
        tool(QStringLiteral("add_runtime_dependency"),
             QStringLiteral("Add an evidence-backed explicit Arch runtime dependency to a Guided recipe when the vendor artifact does not declare it."),
             objectSchema({{QStringLiteral("release_id"), release},
                           {QStringLiteral("arch_package"), stringProperty(QStringLiteral("Official Arch package name, optionally with a version comparison."))}},
                          {QStringLiteral("release_id"), QStringLiteral("arch_package")}), write),
        tool(QStringLiteral("remove_runtime_dependency"),
             QStringLiteral("Remove an explicit Arch runtime dependency previously added to the Guided recipe."),
             objectSchema({{QStringLiteral("release_id"), release},
                           {QStringLiteral("arch_package"), stringProperty(QStringLiteral("Exact configured dependency relation."))}},
                          {QStringLiteral("release_id"), QStringLiteral("arch_package")}), write),
        tool(QStringLiteral("set_payload_disposition"), QStringLiteral("Set an inspected payload path to keep, exclude, or clear its explicit Guided decision."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("path"), stringProperty(QStringLiteral("Exact inspected payload path."))},
                           {QStringLiteral("disposition"), stringProperty(QStringLiteral("keep, exclude, or clear."))}},
                          {QStringLiteral("release_id"), QStringLiteral("path"), QStringLiteral("disposition")}), write),
        tool(QStringLiteral("set_install_layout"), QStringLiteral("Set the supported Guided archive layout and /opt directory."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("layout"), stringProperty(QStringLiteral("opt-bundle or preserve-root."))},
                           {QStringLiteral("opt_directory"), stringProperty(QStringLiteral("Directory name beneath /opt for opt-bundle."))},
                           {QStringLiteral("strip_common_prefix"), booleanProperty(QStringLiteral("Strip the inspected common archive prefix."))},
                           {QStringLiteral("binary_destination"), stringProperty(QStringLiteral("Standalone ELF destination below /usr/bin."))}},
                          {QStringLiteral("release_id"), QStringLiteral("layout")}), write),
        tool(QStringLiteral("upsert_launcher"), QStringLiteral("Add or update a normal Guided command/launcher mapped from an inspected payload path."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("source_path"), stringProperty(QStringLiteral("Exact inspected payload path."))},
                           {QStringLiteral("command_name"), stringProperty(QStringLiteral("Exposed command name."))},
                           {QStringLiteral("destination"), stringProperty(QStringLiteral("Absolute install destination, normally /usr/bin/name."))},
                           {QStringLiteral("kind"), stringProperty(QStringLiteral("symlink or wrapper."))},
                           {QStringLiteral("enabled"), booleanProperty(QStringLiteral("Whether to install the launcher."))}},
                          {QStringLiteral("release_id"), QStringLiteral("source_path"), QStringLiteral("command_name")}), write),
        tool(QStringLiteral("remove_launcher"), QStringLiteral("Remove a Guided command/launcher mapping by its exact inspected source path."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("source_path"), stringProperty(QStringLiteral("Exact launcher source path."))}},
                          {QStringLiteral("release_id"), QStringLiteral("source_path")}), write),
        tool(QStringLiteral("set_apprun"), QStringLiteral("Edit the visible Guided AppRun overlay for an extracted AppImage."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("contents"), stringProperty(QStringLiteral("Complete AppRun script text."))}},
                          {QStringLiteral("release_id"), QStringLiteral("contents")}), write),
        tool(QStringLiteral("upsert_desktop_entry"), QStringLiteral("Add or update an ordinary Guided desktop entry."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("id"), stringProperty(QStringLiteral("Stable desktop entry ID."))},
                           {QStringLiteral("contents"), stringProperty(QStringLiteral("Complete desktop entry text."))},
                           {QStringLiteral("source_path"), stringProperty(QStringLiteral("Optional inspected source path."))},
                           {QStringLiteral("destination"), stringProperty(QStringLiteral("Install destination."))},
                           {QStringLiteral("enabled"), booleanProperty(QStringLiteral("Whether to install the entry."))}},
                          {QStringLiteral("release_id"), QStringLiteral("id"), QStringLiteral("contents")}), write),
        tool(QStringLiteral("delete_desktop_entry"), QStringLiteral("Delete a Guided desktop-entry configuration by stable ID."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("id"), stringProperty(QStringLiteral("Stable desktop entry ID."))}},
                          {QStringLiteral("release_id"), QStringLiteral("id")}), write),
        tool(QStringLiteral("set_release_icon"), QStringLiteral("Import a local PNG, SVG, or XPM through the normal release-icon API and apply it to Guided desktop entries."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("path"), stringProperty(QStringLiteral("Absolute path to the icon file."))}},
                          {QStringLiteral("release_id"), QStringLiteral("path")}), annotations(false, false, false, false)),
        tool(QStringLiteral("set_apprun_review"), QStringLiteral("Keep/acknowledge or restore the inspected AppRun using the same Guided review states as the GUI."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("action"), stringProperty(QStringLiteral("keep-original, acknowledge-current, or restore-original."))}},
                          {QStringLiteral("release_id"), QStringLiteral("action")}), write),
        tool(QStringLiteral("set_script_finding"), QStringLiteral("Set a supported lifecycle responsibility decision for an inspected vendor-script finding."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("evidence_fingerprint"), stringProperty(QStringLiteral("Exact finding fingerprint."))},
                           {QStringLiteral("disposition"), stringProperty(QStringLiteral("handled-by-pacsmith, handled-by-arch, lifecycle-required, not-applicable, or unresolved."))}},
                          {QStringLiteral("release_id"), QStringLiteral("evidence_fingerprint"), QStringLiteral("disposition")}), write),
        tool(QStringLiteral("write_lifecycle_script"), QStringLiteral("Validate and write the ordinary Arch lifecycle .install file for this release."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("file_name"), stringProperty(QStringLiteral("Basename ending in .install."))},
                           {QStringLiteral("contents"), stringProperty(QStringLiteral("Complete lifecycle script text."))}},
                          {QStringLiteral("release_id"), QStringLiteral("file_name"), QStringLiteral("contents")}), write),
        tool(QStringLiteral("remove_lifecycle_script"), QStringLiteral("Remove the ordinary Arch lifecycle .install file from the current release."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), write),
        tool(QStringLiteral("acknowledge_lifecycle_script"), QStringLiteral("Acknowledge the current validated lifecycle script's exact contents using the same review state as the GUI."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), write),
        tool(QStringLiteral("acknowledge_vendor_script"), QStringLiteral("Acknowledge an inspected vendor maintainer script's exact contents."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("name"), stringProperty(QStringLiteral("Exact maintainer-script name."))}},
                          {QStringLiteral("release_id"), QStringLiteral("name")}), write),
        tool(QStringLiteral("import_repository_signing_key"),
             QStringLiteral("Download a vendor OpenPGP public key over HTTPS, inspect its fingerprint, and after explicit confirmation pin it as this release's trusted APT/RPM repository signing key. This is the same Fetch & Review path as the GUI. Never download the key with curl or another external tool."),
             objectSchema({{QStringLiteral("release_id"), release},
                           {QStringLiteral("url"), stringProperty(QStringLiteral("HTTPS URL of the vendor OpenPGP public key, such as keys.asc or a .gpg keyring."))}},
                          {QStringLiteral("release_id"), QStringLiteral("url")}),
             annotations(false, true, false, true)),
        tool(QStringLiteral("set_update_configuration"), QStringLiteral("Edit the normal deterministic update-source configuration for a release."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("strategy"), stringProperty(QStringLiteral("manual, direct-url, apt-repository, rpm-repository, or github-release."))},
                           {QStringLiteral("url"), stringProperty(QStringLiteral("Source or repository URL."))},
                           {QStringLiteral("direct_url_full_check_interval_hours"), integerProperty(QStringLiteral("Hours between automatic full-content checks when a Direct URL exposes no usable validator; zero means manual only."))},
                           {QStringLiteral("github_owner"), stringProperty(QStringLiteral("GitHub owner."))},
                           {QStringLiteral("github_repository"), stringProperty(QStringLiteral("GitHub repository."))},
                           {QStringLiteral("github_asset_regex"), stringProperty(QStringLiteral("Persistent asset-selection regular expression."))},
                           {QStringLiteral("github_include_prereleases"), booleanProperty(QStringLiteral("Include GitHub prereleases."))},
                           {QStringLiteral("apt_suite"), stringProperty(QStringLiteral("APT suite."))},
                           {QStringLiteral("apt_component"), stringProperty(QStringLiteral("APT component."))},
                           {QStringLiteral("apt_architecture"), stringProperty(QStringLiteral("APT architecture."))},
                           {QStringLiteral("apt_package"), stringProperty(QStringLiteral("APT package name."))},
                           {QStringLiteral("rpm_architecture"), stringProperty(QStringLiteral("RPM architecture."))},
                           {QStringLiteral("rpm_package"), stringProperty(QStringLiteral("RPM package name."))},
                           {QStringLiteral("signing_keyring"), stringProperty(QStringLiteral("Existing project-local trusted signing-key path."))},
                           {QStringLiteral("trusted_signing_fingerprint"), stringProperty(QStringLiteral("Pinned fingerprint for that existing key."))}},
                          {QStringLiteral("release_id"), QStringLiteral("strategy")}), write),
        tool(QStringLiteral("set_recipe_mode"), QStringLiteral("Switch this release between constrained Guided mode and Custom PKGBUILD mode."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("mode"), stringProperty(QStringLiteral("guided or custom."))}},
                          {QStringLiteral("release_id"), QStringLiteral("mode")}), write),
        tool(QStringLiteral("write_pkgbuild"), QStringLiteral("Write the current release's Custom PKGBUILD after mandatory human confirmation. A PKGBUILD is shell code that executes during the package build. This never edits historical releases."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("contents"), stringProperty(QStringLiteral("Complete PKGBUILD text. Preserve pacsmith.vars and _PACSMITH_* release variables."))}},
                          {QStringLiteral("release_id"), QStringLiteral("contents")}), sensitiveWrite),
        tool(QStringLiteral("write_custom_support_file"), QStringLiteral("Write a text support file owned by the current Custom PKGBUILD release. It copies forward with the recipe."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("name"), stringProperty(QStringLiteral("Safe basename; PacSmith-owned filenames are rejected."))},
                           {QStringLiteral("contents"), stringProperty(QStringLiteral("Complete text contents."))}},
                          {QStringLiteral("release_id"), QStringLiteral("name"), QStringLiteral("contents")}), write),
        tool(QStringLiteral("delete_custom_support_file"), QStringLiteral("Delete a text support file from this Custom PKGBUILD release."),
             objectSchema({{QStringLiteral("release_id"), release}, {QStringLiteral("name"), stringProperty(QStringLiteral("Safe support-file basename."))}},
                          {QStringLiteral("release_id"), QStringLiteral("name")}), annotations(false, true, true, false)),
        tool(QStringLiteral("start_build"), QStringLiteral("Start the same unprivileged PacSmith build job available to GUI/CLI users."),
             objectSchema({{QStringLiteral("release_id"), release}}, {QStringLiteral("release_id")}), action),
        tool(QStringLiteral("get_build_job"), QStringLiteral("Get current status and structured result for a PacSmith build or preparation job."),
             objectSchema({{QStringLiteral("job_id"), stringProperty(QStringLiteral("PacSmith job UUID."))}}, {QStringLiteral("job_id")}), read),
        tool(QStringLiteral("get_build_job_log"), QStringLiteral("Read a PacSmith job log from a byte offset without scraping the GUI."),
             objectSchema({{QStringLiteral("job_id"), stringProperty(QStringLiteral("PacSmith job UUID."))},
                           {QStringLiteral("after"), integerProperty(QStringLiteral("Byte offset; defaults to zero."))}}, {QStringLiteral("job_id")}), read),
        tool(QStringLiteral("cancel_build_job"), QStringLiteral("Cancel a running PacSmith build or preparation job."),
             objectSchema({{QStringLiteral("job_id"), stringProperty(QStringLiteral("PacSmith job UUID."))}}, {QStringLiteral("job_id")}), annotations(false, true, true, false)),
        tool(QStringLiteral("download_artifact"), QStringLiteral("Export a PacSmith artifact to a new local file for an explicit user-facing purpose. Do not use this to inspect package contents; use get_payload and get_payload_file_inspection."),
             objectSchema({{QStringLiteral("artifact_id"), stringProperty(QStringLiteral("Opaque artifact UUID returned by release/build tools."))},
                           {QStringLiteral("destination"), stringProperty(QStringLiteral("Absolute path that must not already exist."))}},
                          {QStringLiteral("artifact_id"), QStringLiteral("destination")}), annotations(false, false, false, false)),
        tool(QStringLiteral("reanalyze_release"), QStringLiteral("Reset this release's maintained setup and re-run deterministic inspection of its stored artifact. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("project_name"), projectName}, {QStringLiteral("release_name"), releaseName}},
                          {QStringLiteral("project_name"), QStringLiteral("release_name")}), sensitive),
        tool(QStringLiteral("configure_project_repository"), QStringLiteral("Configure this project's publication, package name, and automatic promotion policy. Repository channels are system-wide. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("project_name"), projectName}, {QStringLiteral("publish"), booleanProperty(QStringLiteral("Whether successful builds publish to unstable."))},
                           {QStringLiteral("automatic_soak"), booleanProperty(QStringLiteral("Whether unstable builds automatically promote after the soak period."))},
                           {QStringLiteral("soak_seconds_override"), integerProperty(QStringLiteral("Project-specific soak duration in seconds. Use -1 to inherit the library-wide default, 0 for immediate promotion, or a positive duration."))},
                           {QStringLiteral("package_name_override"), stringProperty(QStringLiteral("Optional published package name override."))}},
                          {QStringLiteral("project_name"), QStringLiteral("publish")}), sensitive),
        tool(QStringLiteral("promote_repository_package"), QStringLiteral("Promote this project's current unstable package into the system-wide Stable channel. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("project_name"), projectName}}, {QStringLiteral("project_name")}), sensitive),
        tool(QStringLiteral("delete_release"), QStringLiteral("Permanently delete a release. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("project_name"), projectName}, {QStringLiteral("release_name"), releaseName}},
                          {QStringLiteral("project_name"), QStringLiteral("release_name")}), sensitive),
        tool(QStringLiteral("delete_project"), QStringLiteral("Permanently delete a project and its releases. PacSmith always elicits explicit human consent."),
             objectSchema({{QStringLiteral("project_name"), projectName}}, {QStringLiteral("project_name")}), sensitive),
    };
    // Harnesses can preflight any mutating tool before PacSmith receives the call, so the
    // input schema itself must carry a target a person recognizes rather than only a UUID.
    for (qsizetype index = 0; index < catalog.size(); ++index) {
        auto entry = catalog.at(index).toObject();
        if (entry.value(QStringLiteral("annotations")).toObject()
                .value(QStringLiteral("readOnlyHint")).toBool()) {
            continue;
        }
        auto schema = entry.value(QStringLiteral("inputSchema")).toObject();
        auto properties = schema.value(QStringLiteral("properties")).toObject();
        auto required = schema.value(QStringLiteral("required")).toArray();
        const auto replaceRequired = [&](const QString &oldName,
                                         const QStringList &replacementNames) {
            QJsonArray next;
            for (const auto &item : required) {
                if (item.toString() == oldName) {
                    for (const auto &replacement : replacementNames) next.append(replacement);
                } else {
                    next.append(item);
                }
            }
            required = next;
        };
        if (properties.contains(QStringLiteral("release_id"))) {
            properties.remove(QStringLiteral("release_id"));
            properties.insert(QStringLiteral("project_name"), projectName);
            properties.insert(QStringLiteral("release_name"), releaseName);
            replaceRequired(QStringLiteral("release_id"),
                            {QStringLiteral("project_name"), QStringLiteral("release_name")});
        }
        if (properties.contains(QStringLiteral("project"))) {
            properties.remove(QStringLiteral("project"));
            properties.insert(QStringLiteral("project_name"), projectName);
            replaceRequired(QStringLiteral("project"), {QStringLiteral("project_name")});
        }
        if (properties.contains(QStringLiteral("existing_project"))) {
            properties.remove(QStringLiteral("existing_project"));
            properties.insert(QStringLiteral("existing_project_name"),
                              stringProperty(QStringLiteral("Optional human-readable existing PacSmith display name or Arch package name.")));
            replaceRequired(QStringLiteral("existing_project"),
                            {QStringLiteral("existing_project_name")});
        }
        schema.insert(QStringLiteral("properties"), properties);
        if (required.isEmpty()) schema.remove(QStringLiteral("required"));
        else schema.insert(QStringLiteral("required"), required);
        entry.insert(QStringLiteral("inputSchema"), schema);
        catalog[index] = entry;
    }
    return catalog;
}

bool locateRelease(LibraryClient &library, const QString &releaseId, Project *project,
                   PackageRelease **release, QString *error) {
    const auto projects = library.list(error);
    for (const auto &summary : projects) {
        if (summary.release(releaseId) == nullptr) continue;
        auto loaded = library.load(summary.id, error);
        *project = std::move(*loaded);
        *release = project->release(releaseId);
        return *release != nullptr;
    }
    if (error != nullptr && error->isEmpty()) *error = QStringLiteral("release not found");
    return false;
}

std::optional<Project> loadNamedProject(LibraryClient &library, const QString &name,
                                        QString *error) {
    const auto projects = library.list(error);
    QList<const Project *> matches;
    for (const auto &candidate : projects) {
        if (candidate.displayName.compare(name, Qt::CaseInsensitive) == 0 ||
            candidate.archPackageName.compare(name, Qt::CaseInsensitive) == 0) {
            matches.append(&candidate);
        }
    }
    if (matches.isEmpty()) {
        if (error != nullptr && error->isEmpty()) {
            *error = QStringLiteral("project_name must be a display name or Arch package name from list_projects");
        }
        return std::nullopt;
    }
    if (matches.size() > 1) {
        if (error != nullptr) {
            *error = QStringLiteral("project_name is ambiguous; use the unique Arch package name from list_projects");
        }
        return std::nullopt;
    }
    return library.load(matches.first()->id, error);
}

bool locateNamedRelease(LibraryClient &library, const QString &projectName,
                        const QString &releaseName, Project *project,
                        PackageRelease **release, QString *error) {
    auto loaded = loadNamedProject(library, projectName, error);
    if (!loaded) return false;
    *project = std::move(*loaded);
    QList<PackageRelease *> matches;
    for (auto &candidate : project->releases) {
        if (candidate.debian.version.compare(releaseName, Qt::CaseInsensitive) == 0 ||
            candidate.originalSourceFilename.compare(releaseName, Qt::CaseInsensitive) == 0) {
            matches.append(&candidate);
        }
    }
    if (matches.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("release_name must be a version or source filename from list_projects");
        }
        return false;
    }
    if (matches.size() > 1) {
        if (error != nullptr) {
            *error = QStringLiteral("release_name is ambiguous; use the unique source filename from list_projects");
        }
        return false;
    }
    *release = matches.first();
    return true;
}

QString projectLabel(const Project &project) {
    if (project.displayName.compare(project.archPackageName, Qt::CaseInsensitive) == 0) {
        return project.displayName;
    }
    return QStringLiteral("%1 (%2)").arg(project.displayName, project.archPackageName);
}

QString releaseLabel(const Project &project, const PackageRelease &release) {
    const auto name = release.debian.version.isEmpty() ? release.originalSourceFilename
                                                       : release.debian.version;
    return QStringLiteral("%1, release %2").arg(projectLabel(project), name);
}

QJsonObject releaseSummary(const PackageRelease &release) {
    return {{QStringLiteral("release_id"), release.id},
            {QStringLiteral("release_name"), release.debian.version.isEmpty()
                                                ? release.originalSourceFilename
                                                : release.debian.version},
            {QStringLiteral("version"), release.debian.version},
            {QStringLiteral("source_filename"), release.originalSourceFilename},
            {QStringLiteral("state"), releaseStateName(release.state)},
            {QStringLiteral("source_type"), sourcePackageTypeName(release.sourceType)},
            {QStringLiteral("source_sha256"), release.sourceSha256},
            {QStringLiteral("recipe_mode"), release.pkgbuildManuallyModified ? QStringLiteral("custom")
                                                                              : QStringLiteral("guided")},
            {QStringLiteral("build_status"), buildStatusName(release.buildStatus)}};
}

QJsonArray releaseSummaries(const QList<PackageRelease> &releases) {
    QJsonArray result;
    for (const auto &release : releases) result.append(releaseSummary(release));
    return result;
}

QJsonObject releaseIssueReport(const Project &project, const PackageRelease &release) {
    const auto issues = releaseReviewIssues(release);
    const bool hasSuccessfulBuild = release.buildStatus == BuildStatus::Succeeded ||
                                    !release.builtArtifactIds.isEmpty();
    QJsonArray issueValues;
    qsizetype blocking = 0;
    for (const auto &issue : issues) {
        if (issue.blocksBuild) ++blocking;
        issueValues.append(QJsonObject{{QStringLiteral("code"), issue.code},
                                       {QStringLiteral("category"), issue.category},
                                       {QStringLiteral("summary"), issue.summary},
                                       {QStringLiteral("subject"), issue.subject},
                                       {QStringLiteral("remediation"), issue.remediation},
                                       {QStringLiteral("blocks_build"), issue.blocksBuild}});
    }
    return {{QStringLiteral("project_name"), projectLabel(project)},
            {QStringLiteral("release_name"), release.debian.version.isEmpty()
                                                 ? release.originalSourceFilename
                                                 : release.debian.version},
            {QStringLiteral("release_id"), release.id},
            {QStringLiteral("release_state"), releaseStateName(release.state)},
            {QStringLiteral("build_status"), buildStatusName(release.buildStatus)},
            {QStringLiteral("has_successful_build"), hasSuccessfulBuild},
            {QStringLiteral("review_complete"), issues.isEmpty()},
            {QStringLiteral("maintenance_complete"), issues.isEmpty() && hasSuccessfulBuild},
            {QStringLiteral("remaining_issue_count"), issues.size()},
            {QStringLiteral("build_blocking_issue_count"), blocking},
            {QStringLiteral("issues"), issueValues},
            {QStringLiteral("completion_guidance"),
             !issues.isEmpty()
                 ? QStringLiteral("Do not claim this release is fully reviewed until these current-state issues are resolved. A successful build alone is not completion.")
                 : hasSuccessfulBuild
                     ? QStringLiteral("PacSmith has no remaining structured review issues and retains a successful build for this release.")
                     : QStringLiteral("Structured review is complete, but this release does not yet retain a successful build.")}};
}

bool saveReleaseProject(LibraryClient &library, Project &project, QString *error) {
    for (auto &release : project.releases) {
        if (release.pkgbuildManuallyModified) continue;
        release.generatedPkgbuild = PkgbuildGenerator::generate(release);
        release.generatedPkgbuildSha256 = QString::fromLatin1(
            QCryptographicHash::hash(release.generatedPkgbuild.toUtf8(),
                                     QCryptographicHash::Sha256).toHex());
    }
    return library.save(project, error);
}

QStringList argumentStringList(const QJsonObject &args, const QString &name, QString *error) {
    QStringList result;
    for (const auto &item : args.value(name).toArray()) {
        if (!item.isString()) {
            if (error != nullptr) *error = QStringLiteral("%1 must contain only strings").arg(name);
            return {};
        }
        const auto value = item.toString().trimmed();
        if (!value.isEmpty() && !result.contains(value)) result.append(value);
    }
    return result;
}

QString argumentString(const QJsonObject &args, const QString &name) {
    return args.value(name).toString();
}

QJsonArray stringArray(const QStringList &values) {
    QJsonArray result;
    for (const auto &value : values) result.append(value);
    return result;
}

QJsonObject librarySettingsJson(const LibrarySettings &settings) {
    return {{QStringLiteral("revision"), settings.revision},
            {QStringLiteral("updates_enabled"), settings.updatesEnabled},
            {QStringLiteral("daily"), settings.updatesDaily},
            {QStringLiteral("weekday"), settings.weekDay},
            {QStringLiteral("hour"), settings.localTime.hour()},
            {QStringLiteral("minute"), settings.localTime.minute()},
            {QStringLiteral("automatically_prepare"), settings.automaticallyPrepare},
            {QStringLiteral("retention_versions"), settings.retentionVersions},
            {QStringLiteral("build_parallelism"), settings.buildParallelism},
            {QStringLiteral("available_build_cores"), settings.availableBuildCores}};
}

QJsonObject repoSettingsJson(const RepoSettings &settings) {
    return {{QStringLiteral("revision"), settings.revision},
            {QStringLiteral("enabled"), settings.enabled},
            {QStringLiteral("listen_hosts"), stringArray(settings.listenHosts)},
            {QStringLiteral("listen_port"), settings.listenPort},
            {QStringLiteral("advertised_url"), settings.advertisedUrl},
            {QStringLiteral("stable_enabled"), settings.stableEnabled},
            {QStringLiteral("soak_seconds"), settings.soakSeconds},
            {QStringLiteral("package_name_prefix"), settings.packageNamePrefix},
            {QStringLiteral("trust_mode"), settings.trustMode},
            {QStringLiteral("signing_initialized"), settings.signingInitialized},
            {QStringLiteral("certified"), settings.certified},
            {QStringLiteral("fingerprint"), settings.fingerprint},
            {QStringLiteral("root_fingerprint"), settings.rootFingerprint},
            {QStringLiteral("keyring_version"), settings.keyringVersion},
            {QStringLiteral("keyring_package"), settings.keyringPackage},
            {QStringLiteral("keyring_url"), settings.keyringUrl},
            {QStringLiteral("bound"), stringArray(settings.bound)},
            {QStringLiteral("certification_help"), settings.certificationHelp},
            {QStringLiteral("certification_commands"), settings.certificationCommands}};
}

QJsonObject jobJson(const JobStatus &job) {
    return {{QStringLiteral("id"), job.id}, {QStringLiteral("kind"), job.kind},
            {QStringLiteral("status"), job.status}, {QStringLiteral("project_id"), job.projectId},
            {QStringLiteral("release_id"), job.releaseId}, {QStringLiteral("error"), job.error},
            {QStringLiteral("result"), job.result}};
}

}

Server::Server(LibraryClient client) : library_(std::move(client)) {}

QJsonArray Server::toolCatalog() { return tools(); }

const ConnectionConfig &Server::connectionConfig() const noexcept { return library_.config(); }

int Server::run() {
    while (true) {
        const auto request = readMessage();
        if (!request) return 0;
        if (!request->contains(QStringLiteral("id"))) {
            if (request->value(QStringLiteral("method")).toString() == QStringLiteral("notifications/initialized")) {
                initialized_ = true;
            }
            continue;
        }
        if (!writeMessage(handleRequest(*request))) return 1;
    }
}

std::optional<QJsonObject> Server::readMessage() {
    auto &input = inputFile();
    while (!input.atEnd()) {
        const auto line = input.readLine();
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line, &error);
        if (error.error == QJsonParseError::NoError && document.isObject()) return document.object();
        QTextStream(stderr) << "pacsmith mcp: ignored invalid JSON-RPC message: " << error.errorString() << '\n';
    }
    return std::nullopt;
}

bool Server::writeMessage(const QJsonObject &message) {
    auto body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    body.append('\n');
    auto &output = outputFile();
    return output.write(body) == body.size() && output.flush();
}

QJsonObject Server::handleRequest(const QJsonObject &request) {
    const auto id = request.value(QStringLiteral("id"));
    const auto method = request.value(QStringLiteral("method")).toString();
    if (method == QStringLiteral("initialize")) {
        const auto params = request.value(QStringLiteral("params")).toObject();
        const auto requested = params.value(QStringLiteral("protocolVersion")).toString();
        static const QSet<QString> supported{QStringLiteral("2025-03-26"), QStringLiteral("2025-06-18"),
                                             QStringLiteral("2025-11-25")};
        if (supported.contains(requested)) protocolVersion_ = requested;
        const auto capabilities = params.value(QStringLiteral("capabilities")).toObject();
        if (capabilities.contains(QStringLiteral("elicitation"))) {
            const auto elicitation = capabilities.value(QStringLiteral("elicitation")).toObject();
            elicitationSupported_ = elicitation.isEmpty() || elicitation.contains(QStringLiteral("form"));
        }
        return rpcResult(id, QJsonObject{
            {QStringLiteral("protocolVersion"), protocolVersion_},
            {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
            {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("pacsmith")},
                                                        {QStringLiteral("version"), QStringLiteral(PACSMITH_VERSION)}}},
            {QStringLiteral("instructions"), QStringLiteral("PacSmith is a packaging workbench. Use domain tools; preserve pacsmith.vars in Custom PKGBUILDs. Sensitive destructive, system, trust, credential, publication, and automation changes enforce user confirmation via elicitation.")},
        });
    }
    if (method == QStringLiteral("ping")) return rpcResult(id, QJsonObject{});
    if (method == QStringLiteral("tools/list")) {
        return rpcResult(id, QJsonObject{{QStringLiteral("tools"), toolCatalog()}});
    }
    if (method == QStringLiteral("tools/call")) {
        if (!initialized_) return rpcError(id, -32002, QStringLiteral("MCP initialization is not complete"));
        return callTool(id, request.value(QStringLiteral("params")).toObject());
    }
    return rpcError(id, -32601, QStringLiteral("method not found"));
}

bool Server::confirm(const QString &toolName, const QString &target, QString *error) {
    return confirm(toolName, target, {}, error);
}

bool Server::confirm(const QString &toolName, const QString &target, const QString &message,
                     QString *error) {
    if (PermissionPolicy::level(toolName) != PermissionLevel::MandatoryConfirmation) return true;
    if (!PermissionPolicy::canProceedToConfirmation(toolName, elicitationSupported_, error)) return false;
    const auto requestId = QStringLiteral("pacsmith-confirm-%1").arg(++serverRequestId_);
    const auto request = QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), requestId},
        {QStringLiteral("method"), QStringLiteral("elicitation/create")},
        {QStringLiteral("params"), QJsonObject{
            {QStringLiteral("mode"), QStringLiteral("form")},
            {QStringLiteral("message"), message.isEmpty()
                                            ? PermissionPolicy::confirmationMessage(toolName, target)
                                            : message},
            {QStringLiteral("requestedSchema"), objectSchema(
                {{QStringLiteral("confirm"), booleanProperty(QStringLiteral("Confirm this exact PacSmith operation."))}},
                {QStringLiteral("confirm")})},
        }},
    };
    if (!writeMessage(request)) {
        if (error != nullptr) *error = QStringLiteral("Could not request explicit confirmation");
        return false;
    }
    while (const auto response = readMessage()) {
        if (response->value(QStringLiteral("id")).toString() != requestId) continue;
        if (response->contains(QStringLiteral("error"))) {
            if (error != nullptr) *error = QStringLiteral("The MCP client could not present PacSmith's mandatory confirmation");
            return false;
        }
        const auto result = response->value(QStringLiteral("result")).toObject();
        const auto accepted = result.value(QStringLiteral("action")).toString() == QStringLiteral("accept") &&
                              result.value(QStringLiteral("content")).toObject().value(QStringLiteral("confirm")).toBool(false);
        if (!accepted && error != nullptr) *error = QStringLiteral("The user declined or canceled the required confirmation");
        return accepted;
    }
    if (error != nullptr) *error = QStringLiteral("The MCP client disconnected before confirmation");
    return false;
}

QJsonObject Server::callTool(const QJsonValue &id, const QJsonObject &params) {
    const auto name = params.value(QStringLiteral("name")).toString();
    const auto args = params.value(QStringLiteral("arguments")).toObject();
    QString error;
    const auto fail = [&](const QString &message) { return toolError(id, message); };

    if (name == QStringLiteral("list_projects")) {
        const auto query = argumentString(args, QStringLiteral("query"));
        QJsonArray result;
        for (const auto &project : library_.list(&error)) {
            const auto haystack = project.displayName + QLatin1Char('\n') + project.archPackageName +
                                  QLatin1Char('\n') + project.vendorName + QLatin1Char('\n') + project.sourceIdentity;
            if (!query.isEmpty() && !haystack.contains(query, Qt::CaseInsensitive)) continue;
            result.append(QJsonObject{{QStringLiteral("id"), project.id},
                                      {QStringLiteral("display_name"), project.displayName},
                                      {QStringLiteral("arch_package_name"), project.archPackageName},
                                      {QStringLiteral("vendor_name"), project.vendorName},
                                      {QStringLiteral("source_identity"), project.sourceIdentity},
                                      {QStringLiteral("releases"), releaseSummaries(project.releases)}});
        }
        if (!error.isEmpty()) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("check_updates")) {
        QString diagnosticText;
        QTextStream diagnostics(&diagnosticText);
        const auto requestedProject = argumentString(args, QStringLiteral("project_name"));
        UpdateCheckBatchResult batch;
        if (requestedProject.isEmpty()) {
            batch = UpdateCheckRunner::runAll(library_, diagnostics);
            if (!batch.error.isEmpty()) return fail(batch.error);
        } else {
            const auto project = loadNamedProject(library_, requestedProject, &error);
            if (!project) return fail(error);
            const auto checked = UpdateCheckRunner::run(
                library_, *project, diagnostics, true);
            batch.checks.append(checked);
            batch.exitCode = checked.exitCode;
        }
        QJsonArray checks;
        for (const auto &checked : batch.checks) {
            checks.append(QJsonObject{{QStringLiteral("project_id"), checked.projectId},
                                      {QStringLiteral("status"), checked.status},
                                      {QStringLiteral("message"), checked.message},
                                      {QStringLiteral("detected_version"), checked.detectedVersion},
                                      {QStringLiteral("prepared"), checked.prepared}});
        }
        const auto result = QJsonObject{{QStringLiteral("exit_code"), batch.exitCode},
                                        {QStringLiteral("checks"), checks},
                                        {QStringLiteral("diagnostics"), diagnosticText.trimmed()}};
        return toolResult(id, result);
    }
    if (name == QStringLiteral("prepare_release")) {
        Project preparedProject;
        PackageRelease *discovered = nullptr;
        if (!locateNamedRelease(library_, argumentString(args, QStringLiteral("project_name")),
                                argumentString(args, QStringLiteral("release_name")),
                                &preparedProject, &discovered, &error)) {
            return fail(error);
        }
        const auto releaseId = discovered->id;
        QString diagnosticsText;
        QTextStream diagnostics(&diagnosticsText);
        const auto imported = UpdateCheckRunner::prepareDiscovered(library_, preparedProject, releaseId, diagnostics, &error);
        if (!imported) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("project_id"), imported->project.id},
                                          {QStringLiteral("release_id"), imported->releaseId},
                                          {QStringLiteral("diagnostics"), diagnosticsText.trimmed()}});
    }
    if (name == QStringLiteral("get_connection_status")) {
        const auto &config = library_.config();
        QJsonObject result{{QStringLiteral("mode"), config.mode == ConnectionConfig::Mode::Remote
                                                        ? QStringLiteral("remote") : QStringLiteral("local")},
                           {QStringLiteral("summary"), config.summary()},
                           {QStringLiteral("config_path"), ConnectionConfig::configPath()},
                           {QStringLiteral("reachable"), library_.reachable(&error)}};
        if (config.mode == ConnectionConfig::Mode::Remote) {
            result.insert(QStringLiteral("url"), config.remoteUrl.toString());
            result.insert(QStringLiteral("server_ca_path"), config.serverCaPath);
            result.insert(QStringLiteral("client_certificate_path"), config.clientCertPath);
        } else {
            result.insert(QStringLiteral("socket_path"), config.socketPath);
        }
        if (!error.isEmpty()) result.insert(QStringLiteral("connection_error"), error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("get_library_settings")) {
        const auto settings = library_.librarySettings(&error);
        return settings ? toolResult(id, librarySettingsJson(*settings)) : fail(error);
    }
    if (name == QStringLiteral("set_library_settings")) {
        auto settings = library_.librarySettings(&error);
        if (!settings) return fail(error);
        if (!confirm(name, QStringLiteral("library update, retention, and build settings"), &error)) return fail(error);
        if (args.contains(QStringLiteral("updates_enabled"))) settings->updatesEnabled = args.value(QStringLiteral("updates_enabled")).toBool();
        if (args.contains(QStringLiteral("daily"))) settings->updatesDaily = args.value(QStringLiteral("daily")).toBool();
        if (args.contains(QStringLiteral("weekday"))) settings->weekDay = args.value(QStringLiteral("weekday")).toInt();
        if (args.contains(QStringLiteral("hour")) || args.contains(QStringLiteral("minute"))) {
            settings->localTime = QTime(args.contains(QStringLiteral("hour")) ? args.value(QStringLiteral("hour")).toInt() : settings->localTime.hour(),
                                        args.contains(QStringLiteral("minute")) ? args.value(QStringLiteral("minute")).toInt() : settings->localTime.minute());
        }
        if (args.contains(QStringLiteral("automatically_prepare"))) settings->automaticallyPrepare = args.value(QStringLiteral("automatically_prepare")).toBool();
        if (args.contains(QStringLiteral("retention_versions"))) settings->retentionVersions = args.value(QStringLiteral("retention_versions")).toInt();
        if (args.contains(QStringLiteral("build_parallelism"))) settings->buildParallelism = args.value(QStringLiteral("build_parallelism")).toInt();
        if (settings->weekDay < 1 || settings->weekDay > 7 || !settings->localTime.isValid() ||
            settings->retentionVersions < -1 || settings->buildParallelism < 1 ||
            settings->buildParallelism > settings->availableBuildCores) {
            return fail(QStringLiteral("Invalid schedule, retention, or build parallelism value"));
        }
        const auto saved = library_.saveLibrarySettings(*settings, &error);
        return saved ? toolResult(id, librarySettingsJson(*saved)) : fail(error);
    }
    if (name == QStringLiteral("get_client_preferences")) {
        const AppSettingsStore store;
        const auto settings = store.load(&error);
        if (!error.isEmpty()) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("keep_in_tray"), settings.updates.keepInTray},
                                          {QStringLiteral("start_at_login"), settings.updates.startAtLogin},
                                          {QStringLiteral("start_minimized"), settings.updates.startMinimized},
                                          {QStringLiteral("autostart_path"), BackgroundUpdateManager::autostartPath()}});
    }
    if (name == QStringLiteral("set_client_preferences")) {
        if (!confirm(name, QStringLiteral("this desktop session"), &error)) return fail(error);
        const AppSettingsStore store;
        auto settings = store.load(&error);
        if (!error.isEmpty()) return fail(error);
        if (args.contains(QStringLiteral("keep_in_tray"))) settings.updates.keepInTray = args.value(QStringLiteral("keep_in_tray")).toBool();
        if (args.contains(QStringLiteral("start_at_login"))) settings.updates.startAtLogin = args.value(QStringLiteral("start_at_login")).toBool();
        if (args.contains(QStringLiteral("start_minimized"))) settings.updates.startMinimized = args.value(QStringLiteral("start_minimized")).toBool();
        if (settings.updates.startMinimized && (!settings.updates.startAtLogin || !settings.updates.keepInTray)) {
            return fail(QStringLiteral("start_minimized requires both start_at_login and keep_in_tray"));
        }
        const auto guiExecutable = QCoreApplication::applicationDirPath() + QStringLiteral("/pacsmith-gui");
        if (!store.save(settings, &error) ||
            !BackgroundUpdateManager::apply(settings.updates, guiExecutable, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("keep_in_tray"), settings.updates.keepInTray},
                                          {QStringLiteral("start_at_login"), settings.updates.startAtLogin},
                                          {QStringLiteral("start_minimized"), settings.updates.startMinimized}});
    }
    if (name == QStringLiteral("get_repository_configuration")) {
        const auto settings = library_.repoSettings(&error);
        return settings ? toolResult(id, repoSettingsJson(*settings)) : fail(error);
    }
    if (name == QStringLiteral("set_repository_configuration")) {
        auto settings = library_.repoSettings(&error);
        if (!settings) return fail(error);
        if (!confirm(name, QStringLiteral("global PacSmith repository"), &error)) return fail(error);
        if (args.contains(QStringLiteral("enabled"))) settings->enabled = args.value(QStringLiteral("enabled")).toBool();
        if (args.contains(QStringLiteral("listen_hosts"))) {
            settings->listenHosts.clear();
            for (const auto &entry : args.value(QStringLiteral("listen_hosts")).toArray()) settings->listenHosts.append(entry.toString().trimmed());
        }
        if (args.contains(QStringLiteral("listen_port"))) settings->listenPort = args.value(QStringLiteral("listen_port")).toInt();
        if (args.contains(QStringLiteral("advertised_url"))) settings->advertisedUrl = argumentString(args, QStringLiteral("advertised_url")).trimmed();
        if (args.contains(QStringLiteral("stable_enabled"))) settings->stableEnabled = args.value(QStringLiteral("stable_enabled")).toBool();
        if (args.contains(QStringLiteral("soak_seconds"))) settings->soakSeconds = args.value(QStringLiteral("soak_seconds")).toInteger();
        if (args.contains(QStringLiteral("package_name_prefix"))) settings->packageNamePrefix = argumentString(args, QStringLiteral("package_name_prefix")).trimmed();
        if (args.contains(QStringLiteral("trust_mode"))) settings->trustMode = argumentString(args, QStringLiteral("trust_mode")).trimmed();
        if (settings->listenPort < 1 || settings->listenPort > 65535 || settings->soakSeconds < 0 ||
            (settings->trustMode != QStringLiteral("direct") && settings->trustMode != QStringLiteral("root-certified"))) {
            return fail(QStringLiteral("Invalid repository port, soak duration, or trust mode"));
        }
        const auto saved = library_.saveRepoSettings(*settings, &error);
        return saved ? toolResult(id, repoSettingsJson(*saved)) : fail(error);
    }
    if (name == QStringLiteral("initialize_repository_signing")) {
        if (!confirm(name, QStringLiteral("global PacSmith repository"), &error)) return fail(error);
        const auto saved = library_.initRepoSigning(&error);
        return saved ? toolResult(id, repoSettingsJson(*saved)) : fail(error);
    }
    if (name == QStringLiteral("download_repository_public_key")) {
        const auto destination = argumentString(args, QStringLiteral("destination"));
        if (!QFileInfo(destination).isAbsolute() || QFileInfo::exists(destination)) return fail(QStringLiteral("destination must be an absolute path that does not already exist"));
        if (!library_.downloadRepoPublicKey(destination, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("destination"), destination}, {QStringLiteral("downloaded"), true}});
    }
    if (name == QStringLiteral("upload_repository_root_key") || name == QStringLiteral("upload_repository_certified_key")) {
        if (!confirm(name, QStringLiteral("global PacSmith repository trust"), &error)) return fail(error);
        const auto key = argumentString(args, QStringLiteral("public_key"));
        if (!key.contains(QStringLiteral("BEGIN PGP PUBLIC KEY BLOCK"))) return fail(QStringLiteral("public_key must be an armored OpenPGP public key"));
        const auto saved = name == QStringLiteral("upload_repository_root_key")
            ? library_.uploadRepoRootKey(key, &error) : library_.uploadRepoCertifiedKey(key, &error);
        return saved ? toolResult(id, repoSettingsJson(*saved)) : fail(error);
    }
    if (name == QStringLiteral("get_repository_bootstrap_script")) {
        auto channel = argumentString(args, QStringLiteral("channel"));
        if (channel.isEmpty()) channel = QStringLiteral("stable");
        if (channel != QStringLiteral("stable") && channel != QStringLiteral("unstable")) return fail(QStringLiteral("channel must be stable or unstable"));
        const auto script = library_.repoBootstrapScript(channel, &error);
        return script ? toolResult(id, QJsonObject{{QStringLiteral("channel"), channel}, {QStringLiteral("script"), *script},
                                                    {QStringLiteral("warning"), QStringLiteral("Review this script; MCP does not execute or install trust configuration.")}}) : fail(error);
    }
    if (name == QStringLiteral("get_server_status")) {
        const auto info = library_.serverInfo(&error);
        if (!info) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("fingerprint"), info->fingerprint},
                                          {QStringLiteral("fingerprint_sha256"), info->fingerprintSha256},
                                          {QStringLiteral("secret_backend"), info->secretBackend},
                                          {QStringLiteral("pki_ready"), info->pkiReady},
                                          {QStringLiteral("listen"), QJsonObject{{QStringLiteral("enabled"), info->listen.enabled},
                                                                                  {QStringLiteral("port"), info->listen.port},
                                                                                  {QStringLiteral("hosts"), stringArray(info->listen.hosts)},
                                                                                  {QStringLiteral("bound"), stringArray(info->listen.bound)}}}});
    }
    if (name == QStringLiteral("set_remote_listening")) {
        if (!confirm(name, QStringLiteral("pacsmithd remote listener"), &error)) return fail(error);
        ListenSettings settings;
        settings.enabled = args.value(QStringLiteral("enabled")).toBool();
        settings.port = args.contains(QStringLiteral("port")) ? args.value(QStringLiteral("port")).toInt() : 8443;
        for (const auto &entry : args.value(QStringLiteral("hosts")).toArray()) settings.hosts.append(entry.toString().trimmed());
        if (settings.hosts.isEmpty()) settings.hosts.append(QStringLiteral("0.0.0.0"));
        if (settings.port < 1 || settings.port > 65535) return fail(QStringLiteral("port must be between 1 and 65535"));
        const auto saved = library_.saveListen(settings, &error);
        return saved ? toolResult(id, QJsonObject{{QStringLiteral("enabled"), saved->enabled}, {QStringLiteral("port"), saved->port},
                                                  {QStringLiteral("hosts"), stringArray(saved->hosts)}, {QStringLiteral("bound"), stringArray(saved->bound)}}) : fail(error);
    }
    if (name == QStringLiteral("list_remote_clients")) {
        QJsonArray result;
        const auto clients = library_.clients(&error);
        if (!error.isEmpty()) return fail(error);
        for (const auto &client : clients) result.append(QJsonObject{{QStringLiteral("id"), client.id}, {QStringLiteral("name"), client.name},
                                                                    {QStringLiteral("certificate_sha256"), client.certSha256}, {QStringLiteral("revoked"), client.revoked}});
        return toolResult(id, result);
    }
    if (name == QStringLiteral("list_pending_registrations")) {
        QJsonArray result;
        const auto registrations = library_.pendingRegistrations(&error);
        if (!error.isEmpty()) return fail(error);
        for (const auto &registration : registrations) result.append(QJsonObject{{QStringLiteral("id"), registration.id}, {QStringLiteral("name"), registration.name},
                                                                                  {QStringLiteral("status"), registration.status}, {QStringLiteral("client_id"), registration.clientId},
                                                                                  {QStringLiteral("certificate_pem"), registration.certPem}});
        return toolResult(id, result);
    }
    if (name == QStringLiteral("approve_remote_registration") || name == QStringLiteral("reject_remote_registration")) {
        const auto registrationId = argumentString(args, QStringLiteral("registration_id"));
        if (!confirm(name, registrationId, &error)) return fail(error);
        const auto ok = name == QStringLiteral("approve_remote_registration")
            ? library_.approveRegistration(registrationId, &error) : library_.rejectRegistration(registrationId, &error);
        return ok ? toolResult(id, QJsonObject{{QStringLiteral("registration_id"), registrationId}, {QStringLiteral("completed"), true}}) : fail(error);
    }
    if (name == QStringLiteral("revoke_remote_client")) {
        const auto clientId = argumentString(args, QStringLiteral("client_id"));
        if (!confirm(name, clientId, &error)) return fail(error);
        return library_.revokeClient(clientId, &error)
            ? toolResult(id, QJsonObject{{QStringLiteral("client_id"), clientId}, {QStringLiteral("revoked"), true}}) : fail(error);
    }
    if (name == QStringLiteral("get_github_credential_status")) {
        const auto status = library_.credentialStatus(QStringLiteral("github.token"), &error);
        return status ? toolResult(id, QJsonObject{{QStringLiteral("configured"), status->configured}, {QStringLiteral("backend"), status->backend}}) : fail(error);
    }
    if (name == QStringLiteral("set_github_credential")) {
        if (!confirm(name, QStringLiteral("GitHub credential"), &error)) return fail(error);
        const auto token = argumentString(args, QStringLiteral("token"));
        if (token.trimmed().isEmpty()) return fail(QStringLiteral("token cannot be empty"));
        return library_.setCredential(QStringLiteral("github.token"), token, &error)
            ? toolResult(id, QJsonObject{{QStringLiteral("configured"), true}}) : fail(error);
    }
    if (name == QStringLiteral("delete_github_credential")) {
        if (!confirm(name, QStringLiteral("GitHub credential"), &error)) return fail(error);
        return library_.deleteCredential(QStringLiteral("github.token"), &error)
            ? toolResult(id, QJsonObject{{QStringLiteral("configured"), false}}) : fail(error);
    }
    if (name == QStringLiteral("list_harness_profiles")) {
        const AppSettingsStore store;
        const auto settings = store.load(&error);
        if (!error.isEmpty()) return fail(error);
        QJsonArray result;
        for (const auto &profile : settings.harnessProfiles) {
            QJsonArray arguments;
            for (const auto &argument : profile.arguments) arguments.append(argument);
            result.append(QJsonObject{{QStringLiteral("name"), profile.name},
                                      {QStringLiteral("executable"), profile.executable},
                                      {QStringLiteral("arguments"), arguments},
                                      {QStringLiteral("default"), profile.isDefault}});
        }
        return toolResult(id, result);
    }
    if (name == QStringLiteral("upsert_harness_profile")) {
        HarnessProfile profile;
        profile.name = argumentString(args, QStringLiteral("name"));
        profile.executable = argumentString(args, QStringLiteral("executable"));
        for (const auto &argument : args.value(QStringLiteral("arguments")).toArray()) {
            if (!argument.isString()) return fail(QStringLiteral("Every harness argument must be a string"));
            profile.arguments.append(argument.toString());
        }
        profile.isDefault = args.value(QStringLiteral("default")).toBool(false);
        const AppSettingsStore store;
        if (!store.upsertHarnessProfile(profile, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("name"), profile.name},
                                          {QStringLiteral("configured"), true},
                                          {QStringLiteral("default"), profile.isDefault}});
    }
    if (name == QStringLiteral("remove_harness_profile")) {
        const auto profileName = argumentString(args, QStringLiteral("name"));
        const AppSettingsStore store;
        if (!store.removeHarnessProfile(profileName, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("name"), profileName},
                                          {QStringLiteral("removed"), true}});
    }
    if (name == QStringLiteral("set_default_harness_profile")) {
        const auto profileName = argumentString(args, QStringLiteral("name"));
        const AppSettingsStore store;
        if (!store.setDefaultHarnessProfile(profileName, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("name"), profileName},
                                          {QStringLiteral("default"), true}});
    }
    if (name == QStringLiteral("get_build_job")) {
        const auto job = library_.getJob(argumentString(args, QStringLiteral("job_id")), &error);
        return job ? toolResult(id, jobJson(*job)) : fail(error);
    }
    if (name == QStringLiteral("get_build_job_log")) {
        qint64 offset = 0;
        const auto after = args.value(QStringLiteral("after")).toInteger(0);
        const auto log = library_.jobLog(argumentString(args, QStringLiteral("job_id")), after, &offset, &error);
        if (!error.isEmpty()) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("log"), log}, {QStringLiteral("next_offset"), offset}});
    }
    if (name == QStringLiteral("cancel_build_job")) {
        const auto jobId = argumentString(args, QStringLiteral("job_id"));
        if (!library_.cancelJob(jobId, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("job_id"), jobId}, {QStringLiteral("canceled"), true}});
    }
    if (name == QStringLiteral("download_artifact")) {
        const auto destination = argumentString(args, QStringLiteral("destination"));
        if (!QFileInfo(destination).isAbsolute() || QFileInfo::exists(destination)) return fail(QStringLiteral("destination must be an absolute path that does not already exist"));
        if (!library_.downloadArtifact(argumentString(args, QStringLiteral("artifact_id")), destination, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("destination"), destination}, {QStringLiteral("downloaded"), true}});
    }
    if (name == QStringLiteral("get_project")) {
        const auto project = library_.load(argumentString(args, QStringLiteral("project")), &error);
        if (!project) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("id"), project->id},
                                          {QStringLiteral("revision"), project->revision},
                                          {QStringLiteral("display_name"), project->displayName},
                                          {QStringLiteral("arch_package_name"), project->archPackageName},
                                          {QStringLiteral("vendor_name"), project->vendorName},
                                          {QStringLiteral("source_identity"), project->sourceIdentity},
                                          {QStringLiteral("installed_version"), project->installedVersion},
                                          {QStringLiteral("installed_release_id"), project->installedReleaseId},
                                          {QStringLiteral("externally_installed"), project->externallyInstalled},
                                          {QStringLiteral("history"), project->toJson().value(QStringLiteral("history"))},
                                          {QStringLiteral("releases"), releaseSummaries(project->releases)}});
    }

    Project project;
    PackageRelease *release = nullptr;
    auto releaseId = argumentString(args, QStringLiteral("release_id"));
    const bool namedRelease = args.contains(QStringLiteral("release_name"));
    if (namedRelease) {
        if (!locateNamedRelease(library_, argumentString(args, QStringLiteral("project_name")),
                                argumentString(args, QStringLiteral("release_name")),
                                &project, &release, &error)) {
            return fail(error);
        }
        releaseId = release->id;
    }
    const auto needsRelease = name.startsWith(QStringLiteral("get_")) ||
                              name.startsWith(QStringLiteral("set_")) ||
                              name.startsWith(QStringLiteral("write_")) ||
                              name == QStringLiteral("read_custom_support_file") ||
                              name == QStringLiteral("upsert_launcher") ||
                              name == QStringLiteral("upsert_desktop_entry") ||
                              name == QStringLiteral("add_runtime_dependency") ||
                              name == QStringLiteral("remove_runtime_dependency") ||
                              name == QStringLiteral("remove_launcher") ||
                              name == QStringLiteral("delete_desktop_entry") ||
                              name == QStringLiteral("remove_lifecycle_script") ||
                              name == QStringLiteral("acknowledge_lifecycle_script") ||
                              name == QStringLiteral("acknowledge_vendor_script") ||
                              name == QStringLiteral("delete_custom_support_file") ||
                              name == QStringLiteral("import_repository_signing_key") ||
                              name == QStringLiteral("start_build") ||
                              name == QStringLiteral("reanalyze_release") ||
                              name == QStringLiteral("delete_release");
    if (needsRelease && !namedRelease && !releaseId.isEmpty() &&
        !locateRelease(library_, releaseId, &project, &release, &error)) {
        return fail(error);
    }

    if (name == QStringLiteral("get_release")) {
        auto result = releaseSummary(*release);
        result.insert(QStringLiteral("project_id"), project.id);
        result.insert(QStringLiteral("revision"), release->revision);
        result.insert(QStringLiteral("display_name"), release->displayName);
        result.insert(QStringLiteral("arch_package_name"), release->archPackageName);
        result.insert(QStringLiteral("vendor_metadata"), release->debian.toJson());
        result.insert(QStringLiteral("acquisition"), release->acquisition.toJson());
        result.insert(QStringLiteral("source_filename"), release->originalSourceFilename);
        result.insert(QStringLiteral("source_artifact_id"), release->sourceArtifactId);
        result.insert(QStringLiteral("icon_artifact_id"), release->iconArtifactId);
        QJsonArray built;
        for (const auto &artifact : release->builtArtifactIds) built.append(artifact);
        result.insert(QStringLiteral("built_artifact_ids"), built);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("get_release_issues")) {
        return toolResult(id, releaseIssueReport(project, *release));
    }
    if (name == QStringLiteral("get_dependencies")) {
        return toolResult(id, QJsonObject{
            {QStringLiteral("vendor_findings"), release->toJson().value(QStringLiteral("dependencies"))},
            {QStringLiteral("additional_runtime_dependencies"),
             stringArray(release->packageMetadata.additionalDependencies)}});
    }
    if (name == QStringLiteral("get_package_metadata")) {
        return toolResult(id, release->packageMetadata.toJson());
    }
    if (name == QStringLiteral("get_payload")) {
        return toolResult(id, QJsonObject{{QStringLiteral("findings"), release->toJson().value(QStringLiteral("payload"))},
                                          {QStringLiteral("dispositions"), release->toJson().value(QStringLiteral("payloadRules"))}});
    }
    if (name == QStringLiteral("get_payload_file_inspection")) {
        const auto inspection = library_.inspectPayloadFile(
            release->id, argumentString(args, QStringLiteral("path")), &error);
        return inspection ? toolResult(id, *inspection) : fail(error);
    }
    if (name == QStringLiteral("get_lifecycle")) {
        return toolResult(id, QJsonObject{{QStringLiteral("vendor_scripts"), release->toJson().value(QStringLiteral("maintainerScripts"))},
                                          {QStringLiteral("findings"), release->toJson().value(QStringLiteral("scriptFindings"))},
                                          {QStringLiteral("arch_lifecycle"), release->lifecycleScript.toJson()}});
    }
    if (name == QStringLiteral("get_install_configuration")) return toolResult(id, release->installMapping.toJson());
    if (name == QStringLiteral("get_update_configuration")) {
        return toolResult(id, QJsonObject{{QStringLiteral("acquisition"), release->acquisition.toJson()},
                                          {QStringLiteral("update"), release->update.toJson()}});
    }
    if (name == QStringLiteral("get_recipe")) {
        const auto pkgbuild = library_.readPkgbuild(*release, &error);
        if (!pkgbuild) return fail(error);
        const auto vars = library_.readIdentityVariables(*release, &error);
        if (!vars) return fail(error);
        QJsonArray files;
        for (auto iterator = release->customFiles.cbegin(); iterator != release->customFiles.cend(); ++iterator) files.append(iterator.key());
        return toolResult(id, QJsonObject{{QStringLiteral("mode"), release->pkgbuildManuallyModified ? QStringLiteral("custom") : QStringLiteral("guided")},
                                          {QStringLiteral("PKGBUILD"), *pkgbuild},
                                          {QStringLiteral("pacsmith.vars"), *vars},
                                          {QStringLiteral("custom_support_files"), files},
                                          {QStringLiteral("lifecycle_file"), release->lifecycleScript.fileName}});
    }
    if (name == QStringLiteral("read_custom_support_file")) {
        const auto fileName = argumentString(args, QStringLiteral("name"));
        if (!release->customFiles.contains(fileName)) return fail(QStringLiteral("custom support file not found"));
        const auto contents = library_.readFile(release->id, fileName, &error);
        return contents ? toolResult(id, QJsonObject{{QStringLiteral("name"), fileName}, {QStringLiteral("contents"), *contents}}) : fail(error);
    }
    if (name == QStringLiteral("get_build_results")) {
        return toolResult(id, QJsonObject{{QStringLiteral("status"), buildStatusName(release->buildStatus)},
                                          {QStringLiteral("automatic"), release->automaticBuild},
                                          {QStringLiteral("last_log"), release->lastBuildLog},
                                          {QStringLiteral("builds"), release->toJson().value(QStringLiteral("builds"))},
                                          {QStringLiteral("built_artifact_ids"), release->toJson().value(QStringLiteral("builtArtifactIds"))}});
    }
    if (name == QStringLiteral("get_repository_state")) {
        const auto loaded = library_.load(argumentString(args, QStringLiteral("project")), &error);
        if (!loaded) return fail(error);
        const auto state = library_.projectRepo(loaded->id, &error);
        if (!state) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("project_id"), loaded->id},
                                          {QStringLiteral("publish"), state->publish},
                                          {QStringLiteral("stable_channel_enabled"), state->stableChannelEnabled},
                                          {QStringLiteral("automatic_soak"), state->automaticSoak},
                                          {QStringLiteral("soak_seconds_override"), state->soakSecondsOverride},
                                          {QStringLiteral("library_soak_seconds"), state->librarySoakSeconds},
                                          {QStringLiteral("effective_soak_seconds"), state->effectiveSoakSeconds},
                                          {QStringLiteral("effective_package_name"), state->effectivePackageName},
                                          {QStringLiteral("published_package_name"), state->publishedPackageName},
                                          {QStringLiteral("has_unstable"), state->hasUnstable},
                                          {QStringLiteral("has_stable"), state->hasStable}});
    }
    if (name == QStringLiteral("import_artifact")) {
        const auto path = argumentString(args, QStringLiteral("path"));
        if (!QFileInfo(path).isAbsolute() || !QFileInfo(path).isFile()) return fail(QStringLiteral("path must be an absolute regular file"));
        ImportOptions options;
        const auto existingProjectName = argumentString(args, QStringLiteral("existing_project_name"));
        if (!existingProjectName.isEmpty()) {
            const auto existing = loadNamedProject(library_, existingProjectName, &error);
            if (!existing) return fail(error);
            options.existingProjectId = existing->id;
        }
        options.acquisition.kind = AcquisitionKind::LocalFile;
        options.acquisition.canonicalIdentity = argumentString(args, QStringLiteral("canonical_identity"));
        const auto imported = library_.importSource(path, options, &error);
        if (!imported) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("project_id"), imported->project.id},
                                          {QStringLiteral("release_id"), imported->releaseId}});
    }
    if (name == QStringLiteral("import_github_release") ||
        name == QStringLiteral("import_direct_url")) {
        QString existingProjectId;
        const auto existingProjectName =
            argumentString(args, QStringLiteral("existing_project_name"));
        if (!existingProjectName.isEmpty()) {
            const auto existing = loadNamedProject(library_, existingProjectName, &error);
            if (!existing) return fail(error);
            existingProjectId = existing->id;
        }
        const QUrl url(argumentString(args, QStringLiteral("url")), QUrl::StrictMode);
        std::optional<RemoteArtifactImportResult> imported;
        if (name == QStringLiteral("import_github_release")) {
            AppSettingsStore settingsStore;
            const auto settings = settingsStore.load();
            const auto source = settings.credentialSources.value(
                QStringLiteral("github"), CredentialSource::Environment);
            CredentialStore credentials(settingsStore.ageSecretsPath());
            auto token = credentials.load(QStringLiteral("github"), source, nullptr)
                             .value_or(QString{});
            imported = RemoteImportService::importGitHub(
                library_, url, argumentString(args, QStringLiteral("asset_regex")),
                args.value(QStringLiteral("include_prereleases")).toBool(false),
                existingProjectId, token, &error);
            token.fill(QChar::Null);
        } else {
            imported = RemoteImportService::importDirectUrl(
                library_, url, existingProjectId, &error);
        }
        if (!imported) return fail(error);
        const auto *importedRelease = imported->imported.project.release(
            imported->imported.releaseId);
        return toolResult(id, QJsonObject{
            {QStringLiteral("project_id"), imported->imported.project.id},
            {QStringLiteral("project_name"), imported->imported.project.displayName},
            {QStringLiteral("release_id"), imported->imported.releaseId},
            {QStringLiteral("release_name"),
             importedRelease == nullptr ? imported->source.detectedVersion
                                        : importedRelease->debian.version},
            {QStringLiteral("source_filename"),
             importedRelease == nullptr ? imported->source.filename
                                        : importedRelease->originalSourceFilename},
            {QStringLiteral("source_sha256"),
             importedRelease == nullptr ? imported->source.sha256
                                        : importedRelease->sourceSha256},
            {QStringLiteral("project_created"), imported->imported.projectCreated},
            {QStringLiteral("duplicate"), imported->imported.duplicate},
            {QStringLiteral("github_tag"), imported->source.tag},
            {QStringLiteral("publisher_digest"), imported->source.publisherDigest},
        });
    }
    if (name == QStringLiteral("update_project_metadata")) {
        auto loaded = loadNamedProject(library_, argumentString(args, QStringLiteral("project_name")), &error);
        if (!loaded) return fail(error);
        if (args.contains(QStringLiteral("display_name"))) loaded->displayName = argumentString(args, QStringLiteral("display_name"));
        if (args.contains(QStringLiteral("arch_package_name"))) {
            const auto packageName = argumentString(args, QStringLiteral("arch_package_name")).trimmed();
            const auto validationError = DomainValidation::archPackageName(packageName);
            if (!validationError.isEmpty()) return fail(validationError);
            loaded->archPackageName = packageName;
        }
        if (args.contains(QStringLiteral("vendor_name"))) loaded->vendorName = argumentString(args, QStringLiteral("vendor_name"));
        if (loaded->displayName.trimmed().isEmpty()) return fail(QStringLiteral("display_name cannot be empty"));
        for (auto &item : loaded->releases) {
            item.displayName = loaded->displayName;
            item.archPackageName = loaded->archPackageName;
            item.vendorName = loaded->vendorName;
        }
        if (!saveReleaseProject(library_, *loaded, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("project_id"), loaded->id}, {QStringLiteral("revision"), loaded->revision}});
    }
    if (name == QStringLiteral("set_dependency_mapping")) {
        const auto expression = argumentString(args, QStringLiteral("dependency"));
        const auto status = argumentString(args, QStringLiteral("status")).toLower();
        auto iterator = std::find_if(release->dependencies.begin(), release->dependencies.end(),
                                     [&](const auto &item) { return item.rawExpression == expression; });
        if (iterator == release->dependencies.end()) return fail(QStringLiteral("dependency finding not found"));
        if (status == QStringLiteral("resolved")) iterator->status = MappingStatus::Resolved;
        else if (status == QStringLiteral("ignored")) iterator->status = MappingStatus::Ignored;
        else if (status == QStringLiteral("bundled")) iterator->status = MappingStatus::Bundled;
        else if (status == QStringLiteral("provided")) iterator->status = MappingStatus::Provided;
        else if (status == QStringLiteral("unresolved")) iterator->status = MappingStatus::Unresolved;
        else return fail(QStringLiteral("invalid dependency status"));
        iterator->archPackage = status == QStringLiteral("resolved") ? argumentString(args, QStringLiteral("arch_package")) : QString{};
        if (status == QStringLiteral("resolved") && iterator->archPackage.trimmed().isEmpty()) return fail(QStringLiteral("arch_package is required for resolved dependencies"));
        iterator->ignored = iterator->status == MappingStatus::Ignored;
        iterator->bundled = iterator->status == MappingStatus::Bundled;
        iterator->provided = iterator->status == MappingStatus::Provided;
        iterator->userOverride = true;
        iterator->mappingSource = QStringLiteral("User-directed MCP edit");
        const auto result = iterator->toJson();
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("set_package_metadata")) {
        auto &metadata = release->packageMetadata;
        if (args.contains(QStringLiteral("description"))) {
            metadata.description = argumentString(args, QStringLiteral("description")).trimmed();
        }
        if (args.contains(QStringLiteral("homepage"))) {
            const auto value = argumentString(args, QStringLiteral("homepage")).trimmed();
            const QUrl url(value, QUrl::StrictMode);
            if (!value.isEmpty() &&
                (!url.isValid() || (url.scheme() != QStringLiteral("https") &&
                                    url.scheme() != QStringLiteral("http")))) {
                return fail(QStringLiteral("homepage must be an absolute HTTP or HTTPS URL"));
            }
            metadata.homepage = value;
        }
        const auto setRelations = [&](const QString &field, QStringList *destination) -> bool {
            if (!args.contains(field)) return true;
            const auto values = argumentStringList(args, field, &error);
            if (!error.isEmpty()) return false;
            for (const auto &value : values) {
                const auto validationError = DomainValidation::packageRelation(value);
                if (!validationError.isEmpty()) {
                    error = QStringLiteral("%1: %2").arg(value, validationError);
                    return false;
                }
            }
            *destination = values;
            return true;
        };
        if (!setRelations(QStringLiteral("provides"), &metadata.provides) ||
            !setRelations(QStringLiteral("conflicts"), &metadata.conflicts)) {
            return fail(error);
        }
        if (args.contains(QStringLiteral("licenses"))) {
            const auto licenses = argumentStringList(args, QStringLiteral("licenses"), &error);
            if (!error.isEmpty()) return fail(error);
            if (licenses.isEmpty()) return fail(QStringLiteral("licenses must contain at least one value"));
            for (const auto &license : licenses) {
                if (license.size() > 256 || license.contains(QLatin1Char('\n')) ||
                    license.contains(QChar(QChar::Null))) {
                    return fail(QStringLiteral("license expressions must be printable single-line values of at most 256 characters"));
                }
            }
            metadata.licenses = licenses;
        }
        const auto result = metadata.toJson();
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("add_runtime_dependency") ||
        name == QStringLiteral("remove_runtime_dependency")) {
        const auto dependency = argumentString(args, QStringLiteral("arch_package")).trimmed();
        const auto validationError = DomainValidation::packageRelation(dependency);
        if (!validationError.isEmpty()) return fail(validationError);
        auto &dependencies = release->packageMetadata.additionalDependencies;
        if (name == QStringLiteral("add_runtime_dependency")) {
            if (!dependencies.contains(dependency)) dependencies.append(dependency);
        } else {
            if (!dependencies.removeOne(dependency)) {
                return fail(QStringLiteral("explicit runtime dependency not found"));
            }
        }
        const auto result = QJsonObject{
            {QStringLiteral("arch_package"), dependency},
            {QStringLiteral("configured"), name == QStringLiteral("add_runtime_dependency")},
            {QStringLiteral("additional_runtime_dependencies"), stringArray(dependencies)}};
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("set_payload_disposition")) {
        const auto path = argumentString(args, QStringLiteral("path"));
        const auto disposition = argumentString(args, QStringLiteral("disposition")).toLower();
        const auto entry = std::find_if(release->payload.cbegin(), release->payload.cend(), [&](const auto &item) { return item.path == path; });
        if (entry == release->payload.cend()) return fail(QStringLiteral("payload path not found"));
        auto rule = std::find_if(release->payloadRules.begin(), release->payloadRules.end(), [&](const auto &item) { return item.path == path; });
        if (disposition == QStringLiteral("clear")) {
            if (rule != release->payloadRules.end()) release->payloadRules.erase(rule);
        } else if (disposition == QStringLiteral("keep") || disposition == QStringLiteral("exclude")) {
            PayloadRule value;
            value.path = path;
            value.excluded = disposition == QStringLiteral("exclude");
            value.reason = QStringLiteral("User-directed MCP payload decision");
            value.userDecision = true;
            value.acknowledgedFingerprint = PayloadReview::fingerprint(*release, path);
            if (rule == release->payloadRules.end()) release->payloadRules.append(value);
            else *rule = value;
        } else return fail(QStringLiteral("invalid payload disposition"));
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("disposition"), disposition}});
    }
    if (name == QStringLiteral("set_install_layout")) {
        const auto layout = argumentString(args, QStringLiteral("layout"));
        if (layout == QStringLiteral("opt-bundle")) release->installMapping.archiveLayout = ArchiveLayout::OptBundle;
        else if (layout == QStringLiteral("preserve-root")) release->installMapping.archiveLayout = ArchiveLayout::PreserveRoot;
        else return fail(QStringLiteral("invalid layout"));
        if (args.contains(QStringLiteral("opt_directory"))) release->installMapping.optDirectory = argumentString(args, QStringLiteral("opt_directory")).trimmed();
        if (release->installMapping.archiveLayout == ArchiveLayout::OptBundle) {
            const auto validationError = DomainValidation::optDirectory(release->installMapping.optDirectory);
            if (!validationError.isEmpty()) return fail(validationError);
        }
        if (args.contains(QStringLiteral("strip_common_prefix"))) release->installMapping.stripCommonPrefix = args.value(QStringLiteral("strip_common_prefix")).toBool();
        if (args.contains(QStringLiteral("binary_destination"))) {
            if (release->sourceType != SourcePackageType::ElfBinary) return fail(QStringLiteral("binary_destination is only valid for standalone ELF releases"));
            const auto destination = argumentString(args, QStringLiteral("binary_destination")).trimmed();
            const auto validationError = DomainValidation::command(QFileInfo(destination).fileName(), destination);
            if (!validationError.isEmpty()) return fail(validationError);
            release->installMapping.binaryDestination = destination;
        }
        const auto result = release->installMapping.toJson();
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("upsert_launcher")) {
        const auto source = argumentString(args, QStringLiteral("source_path"));
        if (std::none_of(release->payload.cbegin(), release->payload.cend(), [&](const auto &item) { return item.path == source; })) return fail(QStringLiteral("launcher source is not an inspected payload path"));
        auto iterator = std::find_if(release->installMapping.launchers.begin(), release->installMapping.launchers.end(), [&](const auto &item) { return item.sourcePath == source; });
        LauncherMapping launcher;
        if (iterator != release->installMapping.launchers.end()) launcher = *iterator;
        launcher.sourcePath = source;
        launcher.commandName = argumentString(args, QStringLiteral("command_name"));
        launcher.destination = argumentString(args, QStringLiteral("destination"));
        if (launcher.destination.isEmpty()) launcher.destination = QStringLiteral("/usr/bin/") + launcher.commandName;
        auto kind = argumentString(args, QStringLiteral("kind"));
        if (kind.isEmpty()) kind = QStringLiteral("symlink");
        if (kind != QStringLiteral("symlink") && kind != QStringLiteral("wrapper")) return fail(QStringLiteral("kind must be symlink or wrapper"));
        const auto validationError = DomainValidation::command(launcher.commandName, launcher.destination);
        if (!validationError.isEmpty()) return fail(validationError);
        launcher.kind = kind == QStringLiteral("wrapper") ? LauncherKind::Wrapper : LauncherKind::Symlink;
        launcher.enabled = !args.contains(QStringLiteral("enabled")) || args.value(QStringLiteral("enabled")).toBool();
        launcher.provenance.origin = ValueOrigin::User;
        if (iterator == release->installMapping.launchers.end()) release->installMapping.launchers.append(launcher);
        else *iterator = launcher;
        const auto result = launcher.toJson();
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("remove_launcher")) {
        const auto source = argumentString(args, QStringLiteral("source_path"));
        const auto before = release->installMapping.launchers.size();
        release->installMapping.launchers.removeIf([&](const auto &launcher) { return launcher.sourcePath == source; });
        if (release->installMapping.launchers.size() == before) return fail(QStringLiteral("launcher not found"));
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("source_path"), source}, {QStringLiteral("removed"), true}});
    }
    if (name == QStringLiteral("set_apprun")) {
        if (release->sourceType != SourcePackageType::AppImage || !release->installMapping.appRun.script) return fail(QStringLiteral("release has no editable text AppRun"));
        const auto contents = argumentString(args, QStringLiteral("contents"));
        const auto validationError = DomainValidation::appRun(contents);
        if (!validationError.isEmpty()) return fail(validationError);
        release->installMapping.appRun.contents = contents;
        release->installMapping.appRun.userModified = contents != release->installMapping.appRun.originalContents;
        release->installMapping.appRun.acknowledge();
        release->installMapping.appRun.provenance.origin = ValueOrigin::User;
        release->installMapping.appRun.provenance.userApproved = true;
        release->installMapping.appRun.provenance.timestamp = QDateTime::currentDateTimeUtc();
        const auto result = release->installMapping.appRun.toJson();
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("upsert_desktop_entry")) {
        const auto entryId = argumentString(args, QStringLiteral("id"));
        auto iterator = std::find_if(release->installMapping.desktopEntries.begin(), release->installMapping.desktopEntries.end(), [&](const auto &item) { return item.id == entryId; });
        DesktopEntryConfiguration entry;
        if (iterator != release->installMapping.desktopEntries.end()) entry = *iterator;
        entry.id = entryId;
        entry.contents = argumentString(args, QStringLiteral("contents"));
        if (args.contains(QStringLiteral("source_path"))) entry.sourcePath = argumentString(args, QStringLiteral("source_path"));
        if (args.contains(QStringLiteral("destination"))) entry.destination = argumentString(args, QStringLiteral("destination"));
        if (entry.destination.isEmpty()) entry.destination = QStringLiteral("/usr/share/applications/") + entryId + QStringLiteral(".desktop");
        if (!entry.sourcePath.isEmpty() && std::none_of(release->payload.cbegin(), release->payload.cend(), [&](const auto &item) { return item.path == entry.sourcePath; })) return fail(QStringLiteral("desktop entry source is not an inspected payload path"));
        const auto validationError = DomainValidation::desktopEntry(entry.contents, entry.destination);
        if (!validationError.isEmpty()) return fail(validationError);
        entry.enabled = !args.contains(QStringLiteral("enabled")) || args.value(QStringLiteral("enabled")).toBool();
        entry.userModified = true;
        entry.provenance.origin = ValueOrigin::User;
        if (iterator == release->installMapping.desktopEntries.end()) release->installMapping.desktopEntries.append(entry);
        else *iterator = entry;
        const auto result = entry.toJson();
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("delete_desktop_entry")) {
        const auto entryId = argumentString(args, QStringLiteral("id"));
        const auto before = release->installMapping.desktopEntries.size();
        release->installMapping.desktopEntries.removeIf([&](const auto &entry) { return entry.id == entryId; });
        if (release->installMapping.desktopEntries.size() == before) return fail(QStringLiteral("desktop entry not found"));
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("id"), entryId}, {QStringLiteral("removed"), true}});
    }
    if (name == QStringLiteral("set_release_icon")) {
        const auto path = argumentString(args, QStringLiteral("path"));
        const QFileInfo info(path);
        const auto suffix = info.suffix().toLower();
        if (!info.isAbsolute() || !info.isFile() || info.size() <= 0 || info.size() > 4 * 1024 * 1024 ||
            (suffix != QStringLiteral("png") && suffix != QStringLiteral("svg") && suffix != QStringLiteral("xpm"))) {
            return fail(QStringLiteral("path must be an absolute PNG, SVG, or XPM file no larger than 4 MiB"));
        }
        if (suffix == QStringLiteral("svg")) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return fail(file.errorString());
            const auto prefix = file.read(4096).toLower();
            if (!prefix.contains("<svg") || prefix.contains("<!entity") || prefix.contains("<!doctype")) return fail(QStringLiteral("SVG header is invalid or declares external entities"));
        }
        auto &icon = release->installMapping.icon;
        icon.sourceKind = IconSourceKind::LocalFile;
        icon.sourcePath = path;
        icon.format = suffix;
        icon.iconName = release->archPackageName;
        icon.missing = false;
        icon.provenance.origin = ValueOrigin::User;
        icon.provenance.userApproved = true;
        icon.provenance.timestamp = QDateTime::currentDateTimeUtc();
        applyDesktopIconName(release->installMapping.desktopEntries, icon.iconName);
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        release = project.release(releaseId);
        if (release == nullptr || !library_.setReleaseIcon(*release, path, &error)) return fail(error);
        return toolResult(id, release->installMapping.icon.toJson());
    }
    if (name == QStringLiteral("set_apprun_review")) {
        auto &appRun = release->installMapping.appRun;
        if (release->sourceType != SourcePackageType::AppImage || !appRun.script) return fail(QStringLiteral("release has no editable text AppRun"));
        const auto actionName = argumentString(args, QStringLiteral("action"));
        if (actionName == QStringLiteral("keep-original")) {
            if (!appRun.originalContents.isEmpty()) appRun.contents = appRun.originalContents;
            appRun.userModified = false;
            appRun.acknowledge();
        } else if (actionName == QStringLiteral("acknowledge-current")) {
            const auto validationError = DomainValidation::appRun(appRun.contents);
            if (!validationError.isEmpty()) return fail(validationError);
            appRun.acknowledge();
        } else if (actionName == QStringLiteral("restore-original")) {
            if (appRun.originalContents.isEmpty()) return fail(QStringLiteral("no original AppRun was retained"));
            appRun.contents = appRun.originalContents;
            appRun.userModified = false;
            appRun.acknowledgedFingerprint.clear();
        } else return fail(QStringLiteral("action must be keep-original, acknowledge-current, or restore-original"));
        appRun.provenance.origin = ValueOrigin::User;
        appRun.provenance.userApproved = true;
        appRun.provenance.timestamp = QDateTime::currentDateTimeUtc();
        const auto result = appRun.toJson();
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("set_script_finding")) {
        const auto fingerprint = argumentString(args, QStringLiteral("evidence_fingerprint"));
        const auto dispositionName = argumentString(args, QStringLiteral("disposition"));
        static const QSet<QString> dispositions{
            QStringLiteral("handled-by-pacsmith"), QStringLiteral("handled-by-arch"),
            QStringLiteral("lifecycle-required"), QStringLiteral("not-applicable"),
            QStringLiteral("unresolved")};
        if (!dispositions.contains(dispositionName)) return fail(QStringLiteral("invalid script finding disposition"));
        auto iterator = std::find_if(release->scriptFindings.begin(), release->scriptFindings.end(), [&](const auto &item) { return item.evidenceFingerprint == fingerprint; });
        if (iterator == release->scriptFindings.end()) return fail(QStringLiteral("script finding not found"));
        iterator->disposition = scriptDispositionFromName(dispositionName);
        iterator->provenance.origin = ValueOrigin::User;
        const auto result = iterator->toJson();
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("write_lifecycle_script")) {
        const auto fileName = argumentString(args, QStringLiteral("file_name"));
        if (!fileName.endsWith(QStringLiteral(".install"))) return fail(QStringLiteral("file_name must end in .install"));
        if (!library_.writeFile(release->id, fileName, argumentString(args, QStringLiteral("contents")), release->revision, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("release_id"), release->id}, {QStringLiteral("file_name"), fileName}});
    }
    if (name == QStringLiteral("remove_lifecycle_script")) {
        const auto removedReleaseId = release->id;
        if (!library_.removeLifecycle(project, *release, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("release_id"), removedReleaseId},
                                          {QStringLiteral("removed"), true}});
    }
    if (name == QStringLiteral("acknowledge_lifecycle_script")) {
        if (release->lifecycleScript.contents.isEmpty() || !release->lifecycleScript.validationPassed) return fail(QStringLiteral("lifecycle script must exist and pass validation before acknowledgement"));
        release->lifecycleScript.acknowledge();
        release->lifecycleScript.provenance.origin = ValueOrigin::User;
        release->lifecycleScript.provenance.userApproved = true;
        release->lifecycleScript.provenance.timestamp = QDateTime::currentDateTimeUtc();
        const auto result = release->lifecycleScript.toJson();
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("acknowledge_vendor_script")) {
        const auto scriptName = argumentString(args, QStringLiteral("name"));
        auto iterator = std::find_if(release->maintainerScripts.begin(), release->maintainerScripts.end(), [&](const auto &script) { return script.name == scriptName; });
        if (iterator == release->maintainerScripts.end()) return fail(QStringLiteral("vendor maintainer script not found"));
        iterator->acknowledge();
        const auto result = iterator->toJson();
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("import_repository_signing_key")) {
        if (release == nullptr) return fail(QStringLiteral("release not found"));
        const QUrl url(argumentString(args, QStringLiteral("url")), QUrl::StrictMode);
        if (!isAcceptableRepositoryKeyUrl(url)) {
            return fail(QStringLiteral("signing-key URL must be a valid HTTPS URL without credentials or a fragment"));
        }
        if (!PermissionPolicy::canProceedToConfirmation(name, elicitationSupported_, &error)) return fail(error);
        QString downloadError;
        const auto downloaded = downloadRepositorySigningKey(url, &downloadError);
        if (!downloaded) return fail(downloadError);
        QString inspectionError;
        const auto inspection = RepositoryTrust::inspectKey(downloaded->contents, &inspectionError);
        if (!inspection || inspection->fingerprints.isEmpty()) {
            return fail(inspectionError.isEmpty() ? QStringLiteral("Downloaded file is not an OpenPGP public key")
                                                  : inspectionError);
        }
        const auto resolvedNotice = downloaded->requestedUrl == downloaded->resolvedUrl
            ? QString{}
            : QStringLiteral("\nThe download redirected to %1.").arg(downloaded->resolvedUrl.toDisplayString());
        const auto message = QStringLiteral(
            "Trust and pin this repository signing key for PacSmith release %1?\n"
            "The HTTPS URL identifies where the bytes came from; the pinned fingerprint is what protects future APT and RPM checks.%2\n\n"
            "Requested URL: %3\nResolved URL: %4\nSHA256: %5\nOpenPGP fingerprint(s):\n%6")
                                 .arg(releaseLabel(project, *release), resolvedNotice,
                                      downloaded->requestedUrl.toString(),
                                      downloaded->resolvedUrl.toString(), inspection->sha256,
                                      inspection->fingerprints.join(QLatin1Char('\n')));
        if (!confirm(name, releaseLabel(project, *release), message, &error)) return fail(error);
        QString importError;
        auto key = RepositoryTrust::importUserKey(library_.releasePath(*release), downloaded->contents,
                                                  downloaded->requestedUrl.toString(), &importError);
        if (!key || key->fingerprints.isEmpty()) return fail(importError);
        const auto storedPath = QDir(QString::fromUtf8(library_.releasePath(*release).string().c_str()))
                                    .filePath(key->relativePath);
        QFile stored(storedPath);
        if (stored.open(QIODevice::ReadOnly)) key->contents = stored.readAll();
        const auto duplicate = std::find_if(release->update.signingKeys.begin(),
                                            release->update.signingKeys.end(),
                                            [&](const auto &candidate) { return candidate.sha256 == key->sha256; });
        if (duplicate == release->update.signingKeys.end()) {
            release->update.signingKeys.append(*key);
        } else {
            if (duplicate->contents.isEmpty()) duplicate->contents = key->contents;
            duplicate->trusted = true;
        }
        release->update.aptSigningKeyring = key->relativePath;
        release->update.trustedSigningFingerprint = key->fingerprints.first();
        release->fieldProvenance.insert(QStringLiteral("update.aptSigningKeyring"), key->provenance);
        release->fieldProvenance.insert(QStringLiteral("update.trustedSigningFingerprint"), key->provenance);
        release->history.append({QDateTime::currentDateTimeUtc(), QStringLiteral("update-key"),
                                 QStringLiteral("Trusted repository key %1 downloaded from %2")
                                     .arg(key->fingerprints.first(), downloaded->requestedUrl.toString())});
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, QJsonObject{
            {QStringLiteral("project_name"), projectLabel(project)},
            {QStringLiteral("release_name"), release->debian.version.isEmpty()
                                                 ? release->originalSourceFilename
                                                 : release->debian.version},
            {QStringLiteral("url"), downloaded->requestedUrl.toString()},
            {QStringLiteral("resolved_url"), downloaded->resolvedUrl.toString()},
            {QStringLiteral("sha256"), inspection->sha256},
            {QStringLiteral("fingerprints"), stringArray(key->fingerprints)},
            {QStringLiteral("signing_keyring"), release->update.aptSigningKeyring},
            {QStringLiteral("trusted_signing_fingerprint"), release->update.trustedSigningFingerprint},
        });
    }
    if (name == QStringLiteral("set_update_configuration")) {
        const auto strategy = argumentString(args, QStringLiteral("strategy"));
        const auto previousStrategy = release->update.strategy;
        const auto previousUrl = release->update.url;
        if (strategy == QStringLiteral("manual")) release->update.strategy = UpdateStrategy::Manual;
        else if (strategy == QStringLiteral("direct-url")) release->update.strategy = UpdateStrategy::DirectUrl;
        else if (strategy == QStringLiteral("apt-repository")) release->update.strategy = UpdateStrategy::AptRepository;
        else if (strategy == QStringLiteral("rpm-repository")) release->update.strategy = UpdateStrategy::RpmRepository;
        else if (strategy == QStringLiteral("github-release")) release->update.strategy = UpdateStrategy::GitHubRelease;
        else return fail(QStringLiteral("invalid update strategy"));
        const auto assign = [&](const QString &key, QString &field) { if (args.contains(key)) field = argumentString(args, key); };
        assign(QStringLiteral("url"), release->update.url);
        assign(QStringLiteral("github_owner"), release->update.githubOwner);
        assign(QStringLiteral("github_repository"), release->update.githubRepository);
        assign(QStringLiteral("github_asset_regex"), release->update.githubAssetRegex);
        assign(QStringLiteral("apt_suite"), release->update.aptSuite);
        assign(QStringLiteral("apt_component"), release->update.aptComponent);
        assign(QStringLiteral("apt_architecture"), release->update.aptArchitecture);
        assign(QStringLiteral("apt_package"), release->update.aptPackageName);
        assign(QStringLiteral("rpm_architecture"), release->update.rpmArchitecture);
        assign(QStringLiteral("rpm_package"), release->update.rpmPackageName);
        assign(QStringLiteral("signing_keyring"), release->update.aptSigningKeyring);
        assign(QStringLiteral("trusted_signing_fingerprint"), release->update.trustedSigningFingerprint);
        if (args.contains(QStringLiteral("direct_url_full_check_interval_hours"))) {
            release->update.directUrlFullCheckIntervalHours =
                std::max(0, args.value(QStringLiteral("direct_url_full_check_interval_hours")).toInt());
        }
        if (args.contains(QStringLiteral("github_include_prereleases"))) release->update.githubIncludePrereleases = args.value(QStringLiteral("github_include_prereleases")).toBool();
        if (release->update.strategy == UpdateStrategy::DirectUrl &&
            (previousStrategy != UpdateStrategy::DirectUrl || previousUrl != release->update.url)) {
            release->update.directUrlEtag.clear();
            release->update.directUrlLastModified.clear();
            release->update.directUrlContentLength = -1;
            release->update.directUrlVendorValidatorName.clear();
            release->update.directUrlVendorValidator.clear();
            release->update.directUrlLastSha256.clear();
            release->update.directUrlLastFullCheck = {};
        }
        const auto validationError = DomainValidation::updateConfiguration(release->update);
        if (!validationError.isEmpty()) return fail(validationError);
        if ((release->update.strategy == UpdateStrategy::AptRepository || release->update.strategy == UpdateStrategy::RpmRepository) &&
            (!release->update.aptSigningKeyring.isEmpty() || !release->update.trustedSigningFingerprint.isEmpty())) {
            const auto key = std::find_if(release->update.signingKeys.cbegin(), release->update.signingKeys.cend(), [&](const auto &candidate) {
                return candidate.relativePath == release->update.aptSigningKeyring && candidate.trusted &&
                       candidate.fingerprints.contains(release->update.trustedSigningFingerprint, Qt::CaseInsensitive);
            });
            if (key == release->update.signingKeys.cend()) return fail(QStringLiteral("signing_keyring and fingerprint must identify an existing trusted key already imported through PacSmith's normal signing-key review"));
        }
        const auto result = release->update.toJson();
        if (!saveReleaseProject(library_, project, &error)) return fail(error);
        return toolResult(id, result);
    }
    if (name == QStringLiteral("set_recipe_mode")) {
        const auto mode = argumentString(args, QStringLiteral("mode"));
        const auto ok = mode == QStringLiteral("custom") ? library_.activateCustomPkgbuild(project, *release, &error)
                      : mode == QStringLiteral("guided") ? library_.activateGuidedPkgbuild(project, *release, &error)
                                                          : false;
        if (!ok) return fail(error.isEmpty() ? QStringLiteral("mode must be guided or custom") : error);
        return toolResult(id, QJsonObject{{QStringLiteral("release_id"), releaseId}, {QStringLiteral("mode"), mode}});
    }
    if (name == QStringLiteral("write_pkgbuild")) {
        const auto contents = argumentString(args, QStringLiteral("contents"));
        const auto validation = PkgbuildGenerator::validate(contents);
        if (!confirm(name, releaseLabel(project, *release), &error)) return fail(error);
        if (!library_.saveCustomPkgbuild(project, *release, contents, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("release_id"), releaseId},
                                          {QStringLiteral("mode"), QStringLiteral("custom")},
                                          {QStringLiteral("static_validation"), validation}});
    }
    if (name == QStringLiteral("write_custom_support_file")) {
        if (!release->pkgbuildManuallyModified) return fail(QStringLiteral("switch this release to Custom PKGBUILD mode first"));
        const auto fileName = argumentString(args, QStringLiteral("name"));
        if (!library_.writeFile(releaseId, fileName, argumentString(args, QStringLiteral("contents")), release->revision, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("release_id"), releaseId}, {QStringLiteral("name"), fileName}});
    }
    if (name == QStringLiteral("delete_custom_support_file")) {
        const auto fileName = argumentString(args, QStringLiteral("name"));
        if (!library_.deleteFile(releaseId, fileName, release->revision, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("release_id"), releaseId}, {QStringLiteral("deleted"), fileName}});
    }
    if (name == QStringLiteral("start_build")) {
        const auto job = library_.startBuild(releaseId, &error);
        if (!job) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("job_id"), job->id}, {QStringLiteral("status"), job->status}});
    }
    if (name == QStringLiteral("reanalyze_release")) {
        if (!confirm(name, releaseLabel(project, *release), &error)) return fail(error);
        const auto result = library_.reanalyzeRelease(releaseId, &error);
        if (!result) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("project_id"), result->project.id}, {QStringLiteral("release_id"), result->releaseId}});
    }
    if (name == QStringLiteral("configure_project_repository")) {
        auto loaded = loadNamedProject(library_, argumentString(args, QStringLiteral("project_name")), &error);
        if (!loaded) return fail(error);
        if (!confirm(name, projectLabel(*loaded), &error)) return fail(error);
        const auto current = library_.projectRepo(loaded->id, &error);
        if (!current) return fail(error);
        const auto automaticSoak = current->stableChannelEnabled &&
            (args.contains(QStringLiteral("automatic_soak"))
                 ? args.value(QStringLiteral("automatic_soak")).toBool()
                 : current->automaticSoak);
        const auto soakSecondsOverride = args.contains(QStringLiteral("soak_seconds_override"))
            ? args.value(QStringLiteral("soak_seconds_override")).toInteger()
            : current->soakSecondsOverride;
        const auto packageNameOverride = args.contains(QStringLiteral("package_name_override"))
            ? argumentString(args, QStringLiteral("package_name_override"))
            : current->packageNameOverride;
        const auto saved = library_.saveProjectRepo(loaded->id, args.value(QStringLiteral("publish")).toBool(),
                                                    automaticSoak,
                                                    soakSecondsOverride, packageNameOverride,
                                                    loaded->revision, &error);
        if (!saved) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("project_id"), loaded->id},
                                          {QStringLiteral("publish"), saved->publish},
                                          {QStringLiteral("stable_channel_enabled"), saved->stableChannelEnabled},
                                          {QStringLiteral("automatic_soak"), saved->automaticSoak},
                                          {QStringLiteral("soak_seconds_override"), saved->soakSecondsOverride},
                                          {QStringLiteral("effective_soak_seconds"), saved->effectiveSoakSeconds}});
    }
    if (name == QStringLiteral("promote_repository_package")) {
        auto loaded = loadNamedProject(library_, argumentString(args, QStringLiteral("project_name")), &error);
        if (!loaded) return fail(error);
        if (!confirm(name, projectLabel(*loaded), &error)) return fail(error);
        const auto saved = library_.promoteProjectRepo(loaded->id, &error);
        if (!saved) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("project_id"), loaded->id}, {QStringLiteral("promoted"), true}});
    }
    if (name == QStringLiteral("delete_release")) {
        if (!confirm(name, releaseLabel(project, *release), &error)) return fail(error);
        const auto deletedVersion = release->debian.version.isEmpty()
                                        ? release->originalSourceFilename
                                        : release->debian.version;
        if (!library_.deleteRelease(project, releaseId, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("deleted_project"), project.displayName},
                                          {QStringLiteral("deleted_release"), deletedVersion}});
    }
    if (name == QStringLiteral("delete_project")) {
        auto loaded = loadNamedProject(library_, argumentString(args, QStringLiteral("project_name")), &error);
        if (!loaded) return fail(error);
        if (!confirm(name, projectLabel(*loaded), &error)) return fail(error);
        if (!library_.deleteProject(loaded->id, &error)) return fail(error);
        return toolResult(id, QJsonObject{{QStringLiteral("deleted_project"), loaded->displayName},
                                          {QStringLiteral("arch_package_name"), loaded->archPackageName}});
    }
    return fail(QStringLiteral("unknown tool"));
}

} // namespace pacsmith::mcp
