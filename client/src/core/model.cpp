#include "core/model.hpp"

#include "core/apt_repository.hpp"
#include "core/rpm_repository.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

namespace pacsmith {
namespace {

QString dateToString(const QDateTime &date) {
    return date.isValid() ? date.toUTC().toString(Qt::ISODateWithMs) : QString{};
}

QDateTime dateFromString(const QJsonValue &value) {
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

QJsonObject stringMapToJson(const QMap<QString, QString> &map) {
    QJsonObject result;
    for (auto iterator = map.cbegin(); iterator != map.cend(); ++iterator) {
        result.insert(iterator.key(), iterator.value());
    }
    return result;
}

QMap<QString, QString> stringMapFromJson(const QJsonObject &object) {
    QMap<QString, QString> result;
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        result.insert(iterator.key(), iterator.value().toString());
    }
    return result;
}

QJsonObject provenanceMapToJson(const QMap<QString, FieldProvenance> &map) {
    QJsonObject result;
    for (auto iterator = map.cbegin(); iterator != map.cend(); ++iterator) {
        result.insert(iterator.key(), iterator.value().toJson());
    }
    return result;
}

QMap<QString, FieldProvenance> provenanceMapFromJson(const QJsonObject &object) {
    QMap<QString, FieldProvenance> result;
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (iterator.value().isObject()) {
            result.insert(iterator.key(), FieldProvenance::fromJson(iterator.value().toObject()));
        }
    }
    return result;
}

template <typename T>
QJsonArray valueListToJson(const QList<T> &values) {
    QJsonArray result;
    for (const auto &value : values) {
        result.append(value.toJson());
    }
    return result;
}

template <typename T>
QList<T> valueListFromJson(const QJsonValue &value) {
    QList<T> result;
    for (const auto &entry : value.toArray()) {
        if (entry.isObject()) {
            result.append(T::fromJson(entry.toObject()));
        }
    }
    return result;
}

QJsonArray stringsToJson(const QStringList &values) {
    QJsonArray result;
    for (const auto &value : values) {
        result.append(value);
    }
    return result;
}

QStringList stringsFromJson(const QJsonValue &value) {
    QStringList result;
    for (const auto &entry : value.toArray()) {
        result.append(entry.toString());
    }
    return result;
}

QStringList splitMetadataField(const QString &value) {
    QStringList result;
    for (const auto &part : value.split(QRegularExpression(QStringLiteral("[,\\n]")),
                                       Qt::SkipEmptyParts)) {
        const auto item = part.trimmed();
        if (!item.isEmpty() && !result.contains(item)) result.append(item);
    }
    return result;
}

} // namespace

QJsonObject FieldProvenance::toJson() const {
    return {{QStringLiteral("origin"), valueOriginName(origin)},
            {QStringLiteral("provider"), provider},
            {QStringLiteral("model"), model},
            {QStringLiteral("sourceFingerprint"), sourceFingerprint},
            {QStringLiteral("rationale"), rationale},
            {QStringLiteral("timestamp"), dateToString(timestamp)},
            {QStringLiteral("userApproved"), userApproved}};
}

FieldProvenance FieldProvenance::fromJson(const QJsonObject &object) {
    FieldProvenance result;
    result.origin = valueOriginFromName(object.value(QStringLiteral("origin")).toString());
    result.provider = object.value(QStringLiteral("provider")).toString();
    result.model = object.value(QStringLiteral("model")).toString();
    result.sourceFingerprint = object.value(QStringLiteral("sourceFingerprint")).toString();
    result.rationale = object.value(QStringLiteral("rationale")).toString();
    result.timestamp = dateFromString(object.value(QStringLiteral("timestamp")));
    result.userApproved = object.value(QStringLiteral("userApproved")).toBool();
    return result;
}

QJsonObject DebianMetadata::toJson() const {
    return {{QStringLiteral("package"), package},
            {QStringLiteral("version"), version},
            {QStringLiteral("architecture"), architecture},
            {QStringLiteral("maintainer"), maintainer},
            {QStringLiteral("description"), description},
            {QStringLiteral("homepage"), homepage},
            {QStringLiteral("depends"), depends},
            {QStringLiteral("preDepends"), preDepends},
            {QStringLiteral("recommends"), recommends},
            {QStringLiteral("suggests"), suggests},
            {QStringLiteral("conflicts"), conflicts},
            {QStringLiteral("provides"), provides},
            {QStringLiteral("rawFields"), stringMapToJson(rawFields)}};
}

DebianMetadata DebianMetadata::fromJson(const QJsonObject &object) {
    DebianMetadata result;
    result.package = object.value(QStringLiteral("package")).toString();
    result.version = object.value(QStringLiteral("version")).toString();
    result.architecture = object.value(QStringLiteral("architecture")).toString();
    result.maintainer = object.value(QStringLiteral("maintainer")).toString();
    result.description = object.value(QStringLiteral("description")).toString();
    result.homepage = object.value(QStringLiteral("homepage")).toString();
    result.depends = object.value(QStringLiteral("depends")).toString();
    result.preDepends = object.value(QStringLiteral("preDepends")).toString();
    result.recommends = object.value(QStringLiteral("recommends")).toString();
    result.suggests = object.value(QStringLiteral("suggests")).toString();
    result.conflicts = object.value(QStringLiteral("conflicts")).toString();
    result.provides = object.value(QStringLiteral("provides")).toString();
    result.rawFields = stringMapFromJson(object.value(QStringLiteral("rawFields")).toObject());
    return result;
}

QJsonObject PackageMetadata::toJson() const {
    return {{QStringLiteral("description"), description},
            {QStringLiteral("homepage"), homepage},
            {QStringLiteral("licenses"), stringsToJson(licenses)},
            {QStringLiteral("provides"), stringsToJson(provides)},
            {QStringLiteral("conflicts"), stringsToJson(conflicts)},
            {QStringLiteral("additionalDependencies"),
             stringsToJson(additionalDependencies)}};
}

PackageMetadata PackageMetadata::fromJson(const QJsonObject &object) {
    PackageMetadata result;
    result.description = object.value(QStringLiteral("description")).toString();
    result.homepage = object.value(QStringLiteral("homepage")).toString();
    result.licenses = stringsFromJson(object.value(QStringLiteral("licenses")));
    result.provides = stringsFromJson(object.value(QStringLiteral("provides")));
    result.conflicts = stringsFromJson(object.value(QStringLiteral("conflicts")));
    result.additionalDependencies =
        stringsFromJson(object.value(QStringLiteral("additionalDependencies")));
    return result;
}

QJsonObject SourceAcquisition::toJson() const {
    return {{QStringLiteral("kind"), acquisitionKindName(kind)},
            {QStringLiteral("canonicalIdentity"), canonicalIdentity},
            {QStringLiteral("originalUrl"), originalUrl},
            {QStringLiteral("githubOwner"), githubOwner},
            {QStringLiteral("githubRepository"), githubRepository},
            {QStringLiteral("githubReleaseId"), QString::number(githubReleaseId)},
            {QStringLiteral("githubTag"), githubTag},
            {QStringLiteral("githubPrerelease"), githubPrerelease},
            {QStringLiteral("githubAssetId"), QString::number(githubAssetId)},
            {QStringLiteral("githubAssetName"), githubAssetName},
            {QStringLiteral("publisherDigest"), publisherDigest},
            {QStringLiteral("publisherVerified"), publisherVerified}};
}

SourceAcquisition SourceAcquisition::fromJson(const QJsonObject &object) {
    SourceAcquisition result;
    result.kind = acquisitionKindFromName(object.value(QStringLiteral("kind")).toString());
    result.canonicalIdentity = object.value(QStringLiteral("canonicalIdentity")).toString();
    result.originalUrl = object.value(QStringLiteral("originalUrl")).toString();
    result.githubOwner = object.value(QStringLiteral("githubOwner")).toString();
    result.githubRepository = object.value(QStringLiteral("githubRepository")).toString();
    result.githubReleaseId = object.value(QStringLiteral("githubReleaseId")).toString().toLongLong();
    result.githubTag = object.value(QStringLiteral("githubTag")).toString();
    result.githubPrerelease = object.value(QStringLiteral("githubPrerelease")).toBool();
    result.githubAssetId = object.value(QStringLiteral("githubAssetId")).toString().toLongLong();
    result.githubAssetName = object.value(QStringLiteral("githubAssetName")).toString();
    result.publisherDigest = object.value(QStringLiteral("publisherDigest")).toString();
    result.publisherVerified = object.value(QStringLiteral("publisherVerified")).toBool();
    return result;
}

QJsonObject LauncherMapping::toJson() const {
    return {{QStringLiteral("enabled"), enabled},
            {QStringLiteral("sourcePath"), sourcePath},
            {QStringLiteral("commandName"), commandName},
            {QStringLiteral("destination"), destination},
            {QStringLiteral("kind"), kind == LauncherKind::Wrapper
                                         ? QStringLiteral("wrapper")
                                         : QStringLiteral("symlink")},
            {QStringLiteral("sourceFingerprint"), sourceFingerprint},
            {QStringLiteral("missing"), missing},
            {QStringLiteral("provenance"), provenance.toJson()}};
}

LauncherMapping LauncherMapping::fromJson(const QJsonObject &object) {
    LauncherMapping result;
    result.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    result.sourcePath = object.value(QStringLiteral("sourcePath")).toString();
    result.commandName = object.value(QStringLiteral("commandName")).toString();
    result.destination = object.value(QStringLiteral("destination")).toString();
    result.kind = object.value(QStringLiteral("kind")).toString() == QStringLiteral("wrapper")
        ? LauncherKind::Wrapper : LauncherKind::Symlink;
    result.sourceFingerprint = object.value(QStringLiteral("sourceFingerprint")).toString();
    result.missing = object.value(QStringLiteral("missing")).toBool();
    result.provenance = FieldProvenance::fromJson(
        object.value(QStringLiteral("provenance")).toObject());
    return result;
}

QJsonObject DesktopEntryConfiguration::toJson() const {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("enabled"), enabled},
            {QStringLiteral("sourcePath"), sourcePath},
            {QStringLiteral("destination"), destination},
            {QStringLiteral("contents"), contents},
            {QStringLiteral("sourceSha256"), sourceSha256},
            {QStringLiteral("originalContentsSha256"), originalContentsSha256},
            {QStringLiteral("generated"), generated},
            {QStringLiteral("userModified"), userModified},
            {QStringLiteral("missing"), missing},
            {QStringLiteral("provenance"), provenance.toJson()}};
}

DesktopEntryConfiguration DesktopEntryConfiguration::fromJson(const QJsonObject &object) {
    DesktopEntryConfiguration result;
    result.id = object.value(QStringLiteral("id")).toString();
    result.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    result.sourcePath = object.value(QStringLiteral("sourcePath")).toString();
    result.destination = object.value(QStringLiteral("destination")).toString();
    result.contents = object.value(QStringLiteral("contents")).toString();
    result.sourceSha256 = object.value(QStringLiteral("sourceSha256")).toString();
    result.originalContentsSha256 =
        object.value(QStringLiteral("originalContentsSha256")).toString();
    result.generated = object.value(QStringLiteral("generated")).toBool();
    result.userModified = object.value(QStringLiteral("userModified")).toBool();
    result.missing = object.value(QStringLiteral("missing")).toBool();
    result.provenance = FieldProvenance::fromJson(
        object.value(QStringLiteral("provenance")).toObject());
    return result;
}

QString desktopEntryField(const QString &contents, const QString &key) {
    const QRegularExpression expression(
        QStringLiteral("(?m)^%1=(.*)$").arg(QRegularExpression::escape(key)));
    return expression.match(contents).captured(1).trimmed();
}

QString withDesktopEntryField(QString contents, const QString &key, const QString &value) {
    contents.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    auto lines = contents.split(QLatin1Char('\n'));
    const auto prefix = key + QLatin1Char('=');
    bool replaced = false;
    for (auto &line : lines) {
        if (!line.startsWith(prefix)) continue;
        line = prefix + value;
        replaced = true;
    }
    if (!replaced) {
        while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
        lines.append(prefix + value);
        lines.append(QString{});
    }
    return lines.join(QLatin1Char('\n'));
}

bool IconConfiguration::isConfigured() const {
    return !missing && !sha256.isEmpty();
}

QString IconConfiguration::installedPath() const {
    if (iconName.isEmpty()) return {};
    auto extension = format.toLower();
    if (extension.isEmpty()) extension = QFileInfo(projectPath).suffix().toLower();
    if (extension.isEmpty()) extension = QFileInfo(sourcePath).suffix().toLower();
    if (extension.isEmpty()) return {};
    const auto directory = extension == QStringLiteral("svg")
        ? QStringLiteral("/usr/share/icons/hicolor/scalable/apps")
        : QStringLiteral("/usr/share/pixmaps");
    return directory + QLatin1Char('/') + iconName + QLatin1Char('.') + extension;
}

int applyDesktopIconName(QList<DesktopEntryConfiguration> &entries, const QString &iconName) {
    if (iconName.isEmpty()) return 0;
    int updated = 0;
    const auto now = QDateTime::currentDateTimeUtc();
    for (auto &desktop : entries) {
        if (!desktop.enabled) continue;
        if (desktopEntryField(desktop.contents, QStringLiteral("Icon")) == iconName) continue;
        desktop.contents = withDesktopEntryField(desktop.contents, QStringLiteral("Icon"), iconName);
        desktop.userModified = true;
        desktop.provenance.origin = ValueOrigin::User;
        desktop.provenance.userApproved = true;
        desktop.provenance.timestamp = now;
        ++updated;
    }
    return updated;
}

QString desktopEntryCommand(const QString &contents) {
    auto exec = desktopEntryField(contents, QStringLiteral("Exec"));
    if (exec.isEmpty()) return {};
    QString executable;
    if (exec.startsWith(QLatin1Char('"'))) {
        const auto closing = exec.indexOf(QLatin1Char('"'), 1);
        if (closing <= 1) return {};
        executable = exec.sliced(1, closing - 1);
    } else {
        const auto whitespace = exec.indexOf(QRegularExpression(QStringLiteral("\\s")));
        executable = whitespace < 0 ? exec : exec.first(whitespace);
    }
    auto command = QFileInfo(executable).fileName();
    static const QRegularExpression safeName(QStringLiteral("^[A-Za-z0-9@._+\\-]+$"));
    if (!safeName.match(command).hasMatch()) return {};
    const auto lower = command.toLower();
    if (lower == QStringLiteral("env") || lower == QStringLiteral("sh") ||
        lower == QStringLiteral("bash") || lower == QStringLiteral("gio") ||
        lower == QStringLiteral("gapplication") || lower.startsWith(QStringLiteral("dbus-")) ||
        lower.startsWith(QStringLiteral("python"))) {
        return {};
    }
    return command;
}

QString preferredDisplayName(const DebianMetadata &metadata,
                             const QList<DesktopEntryConfiguration> &desktopEntries) {
    QString fallbackDesktop;
    for (const auto &desktop : desktopEntries) {
        if (!desktop.enabled) continue;
        const auto name = desktopEntryField(desktop.contents, QStringLiteral("Name"));
        if (name.isEmpty() || name.size() > 80) continue;
        if (fallbackDesktop.isEmpty()) fallbackDesktop = name;
        if (!metadata.package.isEmpty() &&
            (name.contains(metadata.package, Qt::CaseInsensitive) ||
             desktop.id.contains(metadata.package, Qt::CaseInsensitive) ||
             desktop.sourcePath.contains(metadata.package, Qt::CaseInsensitive))) {
            return name;
        }
    }
    if (!fallbackDesktop.isEmpty()) return fallbackDesktop;
    if (!metadata.package.isEmpty()) return metadata.package;
    return metadata.description.section(QLatin1Char('\n'), 0, 0).trimmed();
}

QJsonObject IconConfiguration::toJson() const {
    QString kind;
    switch (sourceKind) {
    case IconSourceKind::None: kind = QStringLiteral("none"); break;
    case IconSourceKind::Payload: kind = QStringLiteral("payload"); break;
    case IconSourceKind::LocalFile: kind = QStringLiteral("local-file"); break;
    case IconSourceKind::RemoteUrl: kind = QStringLiteral("remote-url"); break;
    }
    return {{QStringLiteral("sourceKind"), kind},
            {QStringLiteral("sourcePath"), sourcePath},
            {QStringLiteral("sourceUrl"), sourceUrl},
            {QStringLiteral("projectPath"), projectPath},
            {QStringLiteral("sha256"), sha256},
            {QStringLiteral("format"), format},
            {QStringLiteral("width"), dimensions.width()},
            {QStringLiteral("height"), dimensions.height()},
            {QStringLiteral("iconName"), iconName},
            {QStringLiteral("missing"), missing},
            {QStringLiteral("provenance"), provenance.toJson()}};
}

IconConfiguration IconConfiguration::fromJson(const QJsonObject &object) {
    IconConfiguration result;
    const auto kind = object.value(QStringLiteral("sourceKind")).toString();
    if (kind == QStringLiteral("payload")) result.sourceKind = IconSourceKind::Payload;
    else if (kind == QStringLiteral("local-file")) result.sourceKind = IconSourceKind::LocalFile;
    else if (kind == QStringLiteral("remote-url")) result.sourceKind = IconSourceKind::RemoteUrl;
    result.sourcePath = object.value(QStringLiteral("sourcePath")).toString();
    result.sourceUrl = object.value(QStringLiteral("sourceUrl")).toString();
    result.projectPath = object.value(QStringLiteral("projectPath")).toString();
    result.sha256 = object.value(QStringLiteral("sha256")).toString();
    result.format = object.value(QStringLiteral("format")).toString();
    result.dimensions = QSize(object.value(QStringLiteral("width")).toInt(),
                              object.value(QStringLiteral("height")).toInt());
    result.iconName = object.value(QStringLiteral("iconName")).toString();
    result.missing = object.value(QStringLiteral("missing")).toBool();
    result.provenance = FieldProvenance::fromJson(
        object.value(QStringLiteral("provenance")).toObject());
    return result;
}

QString AppRunConfiguration::contentFingerprint() const {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(contents.toUtf8());
    return QString::fromLatin1(hash.result().toHex());
}

bool AppRunConfiguration::requiresReview() const {
    if (!present || !script || userModified) return false;
    return acknowledgedFingerprint != contentFingerprint();
}

void AppRunConfiguration::acknowledge() {
    acknowledgedFingerprint = contentFingerprint();
}

QJsonObject AppRunConfiguration::toJson() const {
    return {{QStringLiteral("present"), present},
            {QStringLiteral("script"), script},
            {QStringLiteral("contents"), contents},
            {QStringLiteral("originalContents"), originalContents},
            {QStringLiteral("originalContentsSha256"), originalContentsSha256},
            {QStringLiteral("acknowledgedFingerprint"), acknowledgedFingerprint},
            {QStringLiteral("userModified"), userModified},
            {QStringLiteral("reviewReason"), reviewReason},
            {QStringLiteral("provenance"), provenance.toJson()}};
}

AppRunConfiguration AppRunConfiguration::fromJson(const QJsonObject &object) {
    AppRunConfiguration result;
    result.present = object.value(QStringLiteral("present")).toBool();
    result.script = object.value(QStringLiteral("script")).toBool();
    result.contents = object.value(QStringLiteral("contents")).toString();
    result.originalContents = object.value(QStringLiteral("originalContents")).toString();
    result.originalContentsSha256 =
        object.value(QStringLiteral("originalContentsSha256")).toString();
    result.acknowledgedFingerprint =
        object.value(QStringLiteral("acknowledgedFingerprint")).toString();
    result.userModified = object.value(QStringLiteral("userModified")).toBool();
    result.reviewReason = object.value(QStringLiteral("reviewReason")).toString();
    result.provenance = FieldProvenance::fromJson(
        object.value(QStringLiteral("provenance")).toObject());
    return result;
}

QJsonObject InstallMapping::toJson() const {
    return {{QStringLiteral("archiveLayout"),
             archiveLayout == ArchiveLayout::PreserveRoot ? QStringLiteral("preserve-root")
                                                          : QStringLiteral("opt-bundle")},
            {QStringLiteral("optDirectory"), optDirectory},
            {QStringLiteral("commonPrefix"), commonPrefix},
            {QStringLiteral("stripCommonPrefix"), stripCommonPrefix},
            {QStringLiteral("appImageOffset"), QString::number(appImageOffset)},
            {QStringLiteral("binarySourcePath"), binarySourcePath},
            {QStringLiteral("binaryDestination"), binaryDestination},
            {QStringLiteral("executableLinks"), stringsToJson(executableLinks)},
            {QStringLiteral("launchers"), valueListToJson(launchers)},
            {QStringLiteral("desktopEntries"), valueListToJson(desktopEntries)},
            {QStringLiteral("icon"), icon.toJson()},
            {QStringLiteral("appRun"), appRun.toJson()}};
}

InstallMapping InstallMapping::fromJson(const QJsonObject &object) {
    InstallMapping result;
    result.archiveLayout = object.value(QStringLiteral("archiveLayout")).toString() ==
                                   QStringLiteral("preserve-root")
                               ? ArchiveLayout::PreserveRoot
                               : ArchiveLayout::OptBundle;
    result.optDirectory = object.value(QStringLiteral("optDirectory")).toString();
    result.commonPrefix = object.value(QStringLiteral("commonPrefix")).toString();
    result.stripCommonPrefix = object.value(QStringLiteral("stripCommonPrefix")).toBool();
    result.appImageOffset = object.value(QStringLiteral("appImageOffset")).toString().toLongLong();
    result.binarySourcePath = object.value(QStringLiteral("binarySourcePath")).toString();
    result.binaryDestination = object.value(QStringLiteral("binaryDestination")).toString();
    result.executableLinks = stringsFromJson(object.value(QStringLiteral("executableLinks")));
    result.launchers = valueListFromJson<LauncherMapping>(object.value(QStringLiteral("launchers")));
    result.desktopEntries = valueListFromJson<DesktopEntryConfiguration>(
        object.value(QStringLiteral("desktopEntries")));
    result.icon = IconConfiguration::fromJson(object.value(QStringLiteral("icon")).toObject());
    result.appRun = AppRunConfiguration::fromJson(object.value(QStringLiteral("appRun")).toObject());
    // Migrate the original single-launcher representation without changing
    // older, user-readable project files in place until the next save.
    if (result.launchers.isEmpty() && !result.binarySourcePath.isEmpty() &&
        !result.binaryDestination.isEmpty()) {
        LauncherMapping launcher;
        launcher.sourcePath = result.binarySourcePath;
        launcher.commandName = QFileInfo(result.binaryDestination).fileName();
        launcher.destination = result.binaryDestination;
        result.launchers.append(launcher);
    }
    return result;
}

QJsonObject DependencyMapping::toJson() const {
    QJsonArray alternativesJson;
    for (const auto &alternative : alternatives) {
        alternativesJson.append(QJsonObject{
            {QStringLiteral("packageName"), alternative.packageName},
            {QStringLiteral("versionOperator"), alternative.versionOperator},
            {QStringLiteral("version"), alternative.version}});
    }
    return {{QStringLiteral("rawExpression"), rawExpression},
            {QStringLiteral("alternatives"), alternativesJson},
            {QStringLiteral("archPackage"), archPackage},
            {QStringLiteral("status"), mappingStatusName(status)},
            {QStringLiteral("mappingSource"), mappingSource},
            {QStringLiteral("confidence"), confidence},
            {QStringLiteral("userOverride"), userOverride},
            {QStringLiteral("ignored"), ignored},
            {QStringLiteral("bundled"), bundled},
            {QStringLiteral("provided"), provided}};
}

DependencyMapping DependencyMapping::fromJson(const QJsonObject &object) {
    DependencyMapping result;
    result.rawExpression = object.value(QStringLiteral("rawExpression")).toString();
    for (const auto &value : object.value(QStringLiteral("alternatives")).toArray()) {
        const auto alternativeObject = value.toObject();
        result.alternatives.append({alternativeObject.value(QStringLiteral("packageName")).toString(),
                                    alternativeObject.value(QStringLiteral("versionOperator")).toString(),
                                    alternativeObject.value(QStringLiteral("version")).toString()});
    }
    result.archPackage = object.value(QStringLiteral("archPackage")).toString();
    result.status = mappingStatusFromName(object.value(QStringLiteral("status")).toString());
    result.mappingSource = object.value(QStringLiteral("mappingSource")).toString();
    result.confidence = object.value(QStringLiteral("confidence")).toDouble();
    result.userOverride = object.value(QStringLiteral("userOverride")).toBool();
    result.ignored = object.value(QStringLiteral("ignored")).toBool();
    result.bundled = object.value(QStringLiteral("bundled")).toBool();
    result.provided = object.value(QStringLiteral("provided")).toBool();
    return result;
}

QJsonObject MaintainerScript::toJson() const {
    return {{QStringLiteral("name"), name},
            {QStringLiteral("contents"), contents},
            {QStringLiteral("acknowledgedFingerprint"), acknowledgedFingerprint},
            {QStringLiteral("requiresReview"), requiresReview()}};
}

MaintainerScript MaintainerScript::fromJson(const QJsonObject &object) {
    return {object.value(QStringLiteral("name")).toString(),
            object.value(QStringLiteral("contents")).toString(),
            object.value(QStringLiteral("acknowledgedFingerprint")).toString()};
}

QString MaintainerScript::contentFingerprint() const {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(name.toUtf8());
    hash.addData(QByteArrayView{"\0", 1});
    hash.addData(contents.toUtf8());
    return QString::fromLatin1(hash.result().toHex());
}

bool MaintainerScript::requiresReview() const {
    return acknowledgedFingerprint != contentFingerprint();
}

void MaintainerScript::acknowledge() {
    acknowledgedFingerprint = contentFingerprint();
}

bool ScriptFinding::requiresReview() const {
    return disposition == ScriptDisposition::LifecycleRequired ||
           disposition == ScriptDisposition::Unresolved;
}

QJsonObject ScriptFinding::toJson() const {
    return {{QStringLiteral("scriptName"), scriptName},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("summary"), summary},
            {QStringLiteral("evidence"), evidence},
            {QStringLiteral("evidenceFingerprint"), evidenceFingerprint},
            {QStringLiteral("disposition"), scriptDispositionName(disposition)},
            {QStringLiteral("provenance"), provenance.toJson()}};
}

ScriptFinding ScriptFinding::fromJson(const QJsonObject &object) {
    ScriptFinding result;
    result.scriptName = object.value(QStringLiteral("scriptName")).toString();
    result.kind = object.value(QStringLiteral("kind")).toString();
    result.summary = object.value(QStringLiteral("summary")).toString();
    result.evidence = object.value(QStringLiteral("evidence")).toString();
    result.evidenceFingerprint = object.value(QStringLiteral("evidenceFingerprint")).toString();
    result.disposition = scriptDispositionFromName(object.value(QStringLiteral("disposition")).toString());
    result.provenance = FieldProvenance::fromJson(object.value(QStringLiteral("provenance")).toObject());
    return result;
}

QJsonObject PayloadEntry::toJson() const {
    return {{QStringLiteral("path"), path},
            {QStringLiteral("type"), type},
            {QStringLiteral("symlinkTarget"), symlinkTarget},
            {QStringLiteral("size"), QString::number(size)},
            {QStringLiteral("requiresReview"), requiresReview},
            {QStringLiteral("reviewReason"), reviewReason},
            {QStringLiteral("contentSha256"), contentSha256},
            {QStringLiteral("textPreview"), textPreview},
            {QStringLiteral("previewTruncated"), previewTruncated},
            {QStringLiteral("executable"), executable}};
}

PayloadEntry PayloadEntry::fromJson(const QJsonObject &object) {
    PayloadEntry result;
    result.path = object.value(QStringLiteral("path")).toString();
    result.type = object.value(QStringLiteral("type")).toString();
    result.symlinkTarget = object.value(QStringLiteral("symlinkTarget")).toString();
    result.size = object.value(QStringLiteral("size")).isString()
                      ? object.value(QStringLiteral("size")).toString().toLongLong()
                      : object.value(QStringLiteral("size")).toInteger();
    result.requiresReview = object.value(QStringLiteral("requiresReview")).toBool();
    if (result.type == QStringLiteral("directory")) result.requiresReview = false;
    result.reviewReason = object.value(QStringLiteral("reviewReason")).toString();
    result.contentSha256 = object.value(QStringLiteral("contentSha256")).toString();
    result.textPreview = object.value(QStringLiteral("textPreview")).toString();
    result.previewTruncated = object.value(QStringLiteral("previewTruncated")).toBool();
    result.executable = object.value(QStringLiteral("executable")).toBool();
    return result;
}

QJsonObject PayloadRule::toJson() const {
    return {{QStringLiteral("path"), path},
            {QStringLiteral("excluded"), excluded},
            {QStringLiteral("reason"), reason},
            {QStringLiteral("userDecision"), userDecision},
            {QStringLiteral("acknowledgedFingerprint"), acknowledgedFingerprint}};
}

PayloadRule PayloadRule::fromJson(const QJsonObject &object) {
    return {object.value(QStringLiteral("path")).toString(),
            object.value(QStringLiteral("excluded")).toBool(),
            object.value(QStringLiteral("reason")).toString(),
            object.value(QStringLiteral("userDecision")).toBool(),
            object.value(QStringLiteral("acknowledgedFingerprint")).toString()};
}

QString AptRepositoryCandidate::displayText() const {
    QString result = uri;
    if (!suite.isEmpty()) result += QStringLiteral("  ") + suite;
    if (!components.isEmpty()) result += QStringLiteral("  ") + components.join(QLatin1Char(' '));
    return result;
}

QJsonObject AptRepositoryCandidate::toJson() const {
    return {{QStringLiteral("uri"), uri},
            {QStringLiteral("suite"), suite},
            {QStringLiteral("components"), stringsToJson(components)},
            {QStringLiteral("architectures"), stringsToJson(architectures)},
            {QStringLiteral("signedBy"), signedBy},
            {QStringLiteral("sourcePath"), sourcePath}};
}

AptRepositoryCandidate AptRepositoryCandidate::fromJson(const QJsonObject &object) {
    return {object.value(QStringLiteral("uri")).toString(),
            object.value(QStringLiteral("suite")).toString(),
            stringsFromJson(object.value(QStringLiteral("components"))),
            stringsFromJson(object.value(QStringLiteral("architectures"))),
            object.value(QStringLiteral("signedBy")).toString(),
            object.value(QStringLiteral("sourcePath")).toString()};
}

QString RpmRepositoryCandidate::displayText() const {
    return architecture.isEmpty() ? baseUrl
                                  : QStringLiteral("%1  [%2]").arg(baseUrl, architecture);
}

QJsonObject RpmRepositoryCandidate::toJson() const {
    return {{QStringLiteral("baseUrl"), baseUrl},
            {QStringLiteral("architecture"), architecture},
            {QStringLiteral("keyUrls"), stringsToJson(keyUrls)},
            {QStringLiteral("sourcePath"), sourcePath}};
}

RpmRepositoryCandidate RpmRepositoryCandidate::fromJson(const QJsonObject &object) {
    return {object.value(QStringLiteral("baseUrl")).toString(),
            object.value(QStringLiteral("architecture")).toString(),
            stringsFromJson(object.value(QStringLiteral("keyUrls"))),
            object.value(QStringLiteral("sourcePath")).toString()};
}

QJsonObject RepositorySigningKey::toJson() const {
    auto json = QJsonObject{{QStringLiteral("relativePath"), relativePath},
                            {QStringLiteral("sha256"), sha256},
                            {QStringLiteral("fingerprints"), stringsToJson(fingerprints)},
                            {QStringLiteral("sourcePath"), sourcePath},
                            {QStringLiteral("sourceFingerprint"), sourceFingerprint},
                            {QStringLiteral("trusted"), trusted},
                            {QStringLiteral("artifactId"), artifactId},
                            {QStringLiteral("provenance"), provenance.toJson()}};
    if (!contents.isEmpty()) {
        json.insert(QStringLiteral("contents"), QString::fromLatin1(contents.toBase64()));
    }
    return json;
}

RepositorySigningKey RepositorySigningKey::fromJson(const QJsonObject &object) {
    RepositorySigningKey result;
    result.relativePath = object.value(QStringLiteral("relativePath")).toString();
    result.sha256 = object.value(QStringLiteral("sha256")).toString();
    result.fingerprints = stringsFromJson(object.value(QStringLiteral("fingerprints")));
    result.sourcePath = object.value(QStringLiteral("sourcePath")).toString();
    result.sourceFingerprint = object.value(QStringLiteral("sourceFingerprint")).toString();
    result.trusted = object.value(QStringLiteral("trusted")).toBool();
    result.provenance = FieldProvenance::fromJson(object.value(QStringLiteral("provenance")).toObject());
    result.artifactId = object.value(QStringLiteral("artifactId")).toString();
    if (result.artifactId.isEmpty()) {
        result.artifactId = object.value(QStringLiteral("artifact_id")).toString();
    }
    const auto encoded = object.value(QStringLiteral("contents")).toString();
    if (!encoded.isEmpty()) result.contents = QByteArray::fromBase64(encoded.toLatin1());
    return result;
}

QString ArchLifecycleScript::contentFingerprint() const {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(fileName.toUtf8());
    hash.addData(QByteArrayView{"\0", 1});
    hash.addData(contents.toUtf8());
    return QString::fromLatin1(hash.result().toHex());
}

bool ArchLifecycleScript::requiresAcknowledgement() const {
    return !contents.isEmpty() && acknowledgedFingerprint != contentFingerprint();
}

void ArchLifecycleScript::acknowledge() {
    acknowledgedFingerprint = contentFingerprint();
}

void ArchLifecycleScript::bindRequiredFindings(const QList<ScriptFinding> &findings) {
    if (contents.isEmpty() || !validationPassed) return;
    for (const auto &finding : findings) {
        if (finding.disposition != ScriptDisposition::LifecycleRequired ||
            finding.evidenceFingerprint.isEmpty()) {
            continue;
        }
        if (!sourceFingerprints.contains(finding.evidenceFingerprint)) {
            sourceFingerprints.append(finding.evidenceFingerprint);
        }
    }
}

QJsonObject ArchLifecycleScript::toJson() const {
    return {{QStringLiteral("fileName"), fileName},
            {QStringLiteral("contents"), contents},
            {QStringLiteral("acknowledgedFingerprint"), acknowledgedFingerprint},
            {QStringLiteral("sourceFingerprints"), stringsToJson(sourceFingerprints)},
            {QStringLiteral("validationMessage"), validationMessage},
            {QStringLiteral("validationPassed"), validationPassed},
            {QStringLiteral("manuallyModified"), manuallyModified},
            {QStringLiteral("provenance"), provenance.toJson()}};
}

ArchLifecycleScript ArchLifecycleScript::fromJson(const QJsonObject &object) {
    ArchLifecycleScript result;
    result.fileName = object.value(QStringLiteral("fileName")).toString();
    result.contents = object.value(QStringLiteral("contents")).toString();
    result.acknowledgedFingerprint = object.value(QStringLiteral("acknowledgedFingerprint")).toString();
    result.sourceFingerprints = stringsFromJson(object.value(QStringLiteral("sourceFingerprints")));
    result.validationMessage = object.value(QStringLiteral("validationMessage")).toString();
    result.validationPassed = object.value(QStringLiteral("validationPassed")).toBool();
    result.manuallyModified = object.value(QStringLiteral("manuallyModified")).toBool();
    result.provenance = FieldProvenance::fromJson(object.value(QStringLiteral("provenance")).toObject());
    return result;
}

QJsonObject AiChangeRecord::toJson() const {
    return {{QStringLiteral("timestamp"), dateToString(timestamp)},
            {QStringLiteral("field"), field},
            {QStringLiteral("previousValue"), previousValue},
            {QStringLiteral("newValue"), newValue},
            {QStringLiteral("provider"), provider},
            {QStringLiteral("model"), model},
            {QStringLiteral("rationale"), rationale}};
}

AiChangeRecord AiChangeRecord::fromJson(const QJsonObject &object) {
    return {dateFromString(object.value(QStringLiteral("timestamp"))),
            object.value(QStringLiteral("field")).toString(),
            object.value(QStringLiteral("previousValue")).toString(),
            object.value(QStringLiteral("newValue")).toString(),
            object.value(QStringLiteral("provider")).toString(),
            object.value(QStringLiteral("model")).toString(),
            object.value(QStringLiteral("rationale")).toString()};
}

QJsonObject UpdateConfiguration::toJson() const {
    return {{QStringLiteral("strategy"), updateStrategyName(strategy)},
            {QStringLiteral("url"), url},
            {QStringLiteral("aptSuite"), aptSuite},
            {QStringLiteral("aptComponent"), aptComponent},
            {QStringLiteral("aptArchitecture"), aptArchitecture},
            {QStringLiteral("aptPackageName"), aptPackageName},
            {QStringLiteral("aptSigningKeyring"), aptSigningKeyring},
            {QStringLiteral("rpmArchitecture"), rpmArchitecture},
            {QStringLiteral("rpmPackageName"), rpmPackageName},
            {QStringLiteral("trustedSigningFingerprint"), trustedSigningFingerprint},
            {QStringLiteral("detectedVersion"), detectedVersion},
            {QStringLiteral("detectedFilename"), detectedFilename},
            {QStringLiteral("detectedSha256"), detectedSha256},
            {QStringLiteral("detectedUrl"), detectedUrl},
            {QStringLiteral("lastChecked"), dateToString(lastChecked)},
            {QStringLiteral("lastCheckMessage"), lastCheckMessage},
            {QStringLiteral("signatureVerified"), signatureVerified},
            {QStringLiteral("directUrlEtag"), directUrlEtag},
            {QStringLiteral("directUrlLastModified"), directUrlLastModified},
            {QStringLiteral("directUrlContentLength"), QString::number(directUrlContentLength)},
            {QStringLiteral("directUrlVendorValidatorName"), directUrlVendorValidatorName},
            {QStringLiteral("directUrlVendorValidator"), directUrlVendorValidator},
            {QStringLiteral("directUrlLastSha256"), directUrlLastSha256},
            {QStringLiteral("directUrlLastFullCheck"), dateToString(directUrlLastFullCheck)},
            {QStringLiteral("directUrlFullCheckIntervalHours"), directUrlFullCheckIntervalHours},
            {QStringLiteral("detectedCandidates"), stringsToJson(detectedCandidates)},
            {QStringLiteral("aptCandidates"), valueListToJson(aptCandidates)},
            {QStringLiteral("rpmCandidates"), valueListToJson(rpmCandidates)},
            {QStringLiteral("signingKeys"), valueListToJson(signingKeys)},
            {QStringLiteral("githubOwner"), githubOwner},
            {QStringLiteral("githubRepository"), githubRepository},
            {QStringLiteral("githubAssetRegex"), githubAssetRegex},
            {QStringLiteral("githubIncludePrereleases"), githubIncludePrereleases},
            {QStringLiteral("githubEtag"), githubEtag},
            {QStringLiteral("githubReleaseId"), QString::number(githubReleaseId)},
            {QStringLiteral("githubAssetId"), QString::number(githubAssetId)},
            {QStringLiteral("githubTag"), githubTag},
            {QStringLiteral("githubPublisherDigest"), githubPublisherDigest}};
}

UpdateConfiguration UpdateConfiguration::fromJson(const QJsonObject &object) {
    UpdateConfiguration result;
    result.strategy = updateStrategyFromName(object.value(QStringLiteral("strategy")).toString());
    result.url = object.value(QStringLiteral("url")).toString();
    result.aptSuite = object.value(QStringLiteral("aptSuite")).toString();
    result.aptComponent = object.value(QStringLiteral("aptComponent")).toString();
    result.aptArchitecture = object.value(QStringLiteral("aptArchitecture")).toString();
    result.aptPackageName = object.value(QStringLiteral("aptPackageName")).toString();
    result.aptSigningKeyring = object.value(QStringLiteral("aptSigningKeyring")).toString();
    result.rpmArchitecture = object.value(QStringLiteral("rpmArchitecture")).toString();
    result.rpmPackageName = object.value(QStringLiteral("rpmPackageName")).toString();
    result.trustedSigningFingerprint = object.value(QStringLiteral("trustedSigningFingerprint")).toString();
    result.detectedVersion = object.value(QStringLiteral("detectedVersion")).toString();
    result.detectedFilename = object.value(QStringLiteral("detectedFilename")).toString();
    result.detectedSha256 = object.value(QStringLiteral("detectedSha256")).toString();
    result.detectedUrl = object.value(QStringLiteral("detectedUrl")).toString();
    result.lastChecked = dateFromString(object.value(QStringLiteral("lastChecked")));
    result.lastCheckMessage = object.value(QStringLiteral("lastCheckMessage")).toString();
    result.signatureVerified = object.value(QStringLiteral("signatureVerified")).toBool();
    result.directUrlEtag = object.value(QStringLiteral("directUrlEtag")).toString();
    result.directUrlLastModified = object.value(QStringLiteral("directUrlLastModified")).toString();
    result.directUrlContentLength = object.value(QStringLiteral("directUrlContentLength"))
                                        .toString(QStringLiteral("-1")).toLongLong();
    result.directUrlVendorValidatorName =
        object.value(QStringLiteral("directUrlVendorValidatorName")).toString();
    result.directUrlVendorValidator =
        object.value(QStringLiteral("directUrlVendorValidator")).toString();
    result.directUrlLastSha256 = object.value(QStringLiteral("directUrlLastSha256")).toString();
    result.directUrlLastFullCheck =
        dateFromString(object.value(QStringLiteral("directUrlLastFullCheck")));
    result.directUrlFullCheckIntervalHours = std::max(
        0, object.value(QStringLiteral("directUrlFullCheckIntervalHours")).toInt(24));
    result.detectedCandidates = stringsFromJson(object.value(QStringLiteral("detectedCandidates")));
    result.aptCandidates = valueListFromJson<AptRepositoryCandidate>(object.value(QStringLiteral("aptCandidates")));
    result.rpmCandidates = valueListFromJson<RpmRepositoryCandidate>(object.value(QStringLiteral("rpmCandidates")));
    result.signingKeys = valueListFromJson<RepositorySigningKey>(object.value(QStringLiteral("signingKeys")));
    result.githubOwner = object.value(QStringLiteral("githubOwner")).toString();
    result.githubRepository = object.value(QStringLiteral("githubRepository")).toString();
    result.githubAssetRegex = object.value(QStringLiteral("githubAssetRegex")).toString();
    result.githubIncludePrereleases = object.value(QStringLiteral("githubIncludePrereleases")).toBool();
    result.githubEtag = object.value(QStringLiteral("githubEtag")).toString();
    result.githubReleaseId = object.value(QStringLiteral("githubReleaseId")).toString().toLongLong();
    result.githubAssetId = object.value(QStringLiteral("githubAssetId")).toString().toLongLong();
    result.githubTag = object.value(QStringLiteral("githubTag")).toString();
    result.githubPublisherDigest = object.value(QStringLiteral("githubPublisherDigest")).toString();
    return result;
}

QJsonObject HistoryEntry::toJson() const {
    return {{QStringLiteral("timestamp"), dateToString(timestamp)},
            {QStringLiteral("event"), event},
            {QStringLiteral("detail"), detail}};
}

HistoryEntry HistoryEntry::fromJson(const QJsonObject &object) {
    return {dateFromString(object.value(QStringLiteral("timestamp"))),
            object.value(QStringLiteral("event")).toString(),
            object.value(QStringLiteral("detail")).toString()};
}

QJsonObject PackageArtifact::toJson() const {
    return {{QStringLiteral("relativePath"), relativePath},
            {QStringLiteral("sha256"), sha256},
            {QStringLiteral("packageName"), packageName},
            {QStringLiteral("packageVersion"), packageVersion},
            {QStringLiteral("architecture"), architecture},
            {QStringLiteral("size"), size},
            {QStringLiteral("createdAt"), dateToString(createdAt)}};
}

PackageArtifact PackageArtifact::fromJson(const QJsonObject &object) {
    return {object.value(QStringLiteral("relativePath")).toString(),
            object.value(QStringLiteral("sha256")).toString(),
            object.value(QStringLiteral("packageName")).toString(),
            object.value(QStringLiteral("packageVersion")).toString(),
            object.value(QStringLiteral("architecture")).toString(),
            object.value(QStringLiteral("size")).toInteger(),
            dateFromString(object.value(QStringLiteral("createdAt")))};
}

QJsonObject BuildRecord::toJson() const {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("status"), buildStatusName(status)},
            {QStringLiteral("log"), log},
            {QStringLiteral("artifacts"), valueListToJson(artifacts)},
            {QStringLiteral("startedAt"), dateToString(startedAt)},
            {QStringLiteral("finishedAt"), dateToString(finishedAt)}};
}

BuildRecord BuildRecord::fromJson(const QJsonObject &object) {
    BuildRecord result;
    result.id = object.value(QStringLiteral("id")).toString();
    const auto status = object.value(QStringLiteral("status")).toString();
    if (status == QStringLiteral("building")) result.status = BuildStatus::Building;
    else if (status == QStringLiteral("succeeded")) result.status = BuildStatus::Succeeded;
    else if (status == QStringLiteral("failed")) result.status = BuildStatus::Failed;
    else if (status == QStringLiteral("canceled")) result.status = BuildStatus::Canceled;
    result.log = object.value(QStringLiteral("log")).toString();
    result.artifacts = valueListFromJson<PackageArtifact>(object.value(QStringLiteral("artifacts")));
    result.startedAt = dateFromString(object.value(QStringLiteral("startedAt")));
    result.finishedAt = dateFromString(object.value(QStringLiteral("finishedAt")));
    return result;
}

QJsonObject PackageRelease::toJson() const {
    QJsonObject customFilesObject;
    for (auto iterator = customFiles.cbegin(); iterator != customFiles.cend(); ++iterator) {
        customFilesObject.insert(iterator.key(), iterator.value());
    }
    return {{QStringLiteral("formatVersion"), formatVersion},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("id"), id},
            {QStringLiteral("projectId"), projectId},
            {QStringLiteral("displayName"), displayName},
            {QStringLiteral("iconPath"), iconPath},
            {QStringLiteral("iconSourcePath"), iconSourcePath},
            {QStringLiteral("iconSha256"), iconSha256},
            {QStringLiteral("archPackageName"), archPackageName},
            {QStringLiteral("sourceType"), sourcePackageTypeName(sourceType)},
            {QStringLiteral("acquisition"), acquisition.toJson()},
            {QStringLiteral("installMapping"), installMapping.toJson()},
            {QStringLiteral("originalSourceFilename"), originalSourceFilename},
            {QStringLiteral("sourceUrl"), sourceUrl},
            {QStringLiteral("sourceSha256"), sourceSha256},
            {QStringLiteral("vendorName"), vendorName},
            {QStringLiteral("archPkgrel"), archPkgrel},
            {QStringLiteral("archPkgrelOverride"), archPkgrelOverride},
            {QStringLiteral("debian"), debian.toJson()},
            {QStringLiteral("packageMetadata"), packageMetadata.toJson()},
            {QStringLiteral("dependencies"), valueListToJson(dependencies)},
            {QStringLiteral("maintainerScripts"), valueListToJson(maintainerScripts)},
            {QStringLiteral("scriptFindings"), valueListToJson(scriptFindings)},
            {QStringLiteral("payload"), valueListToJson(payload)},
            {QStringLiteral("payloadRules"), valueListToJson(payloadRules)},
            {QStringLiteral("generatedPkgbuild"), generatedPkgbuild},
            {QStringLiteral("generatedPkgbuildSha256"), generatedPkgbuildSha256},
            {QStringLiteral("pkgbuildManuallyModified"), pkgbuildManuallyModified},
            {QStringLiteral("customPkgbuild"), customPkgbuild},
            {QStringLiteral("customFiles"), customFilesObject},
            {QStringLiteral("lifecycleScript"), lifecycleScript.toJson()},
            {QStringLiteral("fieldProvenance"), provenanceMapToJson(fieldProvenance)},
            {QStringLiteral("aiChanges"), valueListToJson(aiChanges)},
            {QStringLiteral("update"), update.toJson()},
            {QStringLiteral("buildStatus"), buildStatusName(buildStatus)},
            {QStringLiteral("automaticBuild"), automaticBuild},
            {QStringLiteral("state"), releaseStateName(state)},
            {QStringLiteral("lastBuildLog"), lastBuildLog},
            {QStringLiteral("producedPackages"), stringsToJson(producedPackages)},
            {QStringLiteral("builds"), valueListToJson(builds)},
            {QStringLiteral("history"), valueListToJson(history)},
            {QStringLiteral("createdAt"), dateToString(createdAt)},
            {QStringLiteral("modifiedAt"), dateToString(modifiedAt)}};
}

PackageRelease PackageRelease::fromJson(const QJsonObject &object) {
    PackageRelease result;
    result.formatVersion = object.value(QStringLiteral("formatVersion")).toInt(1);
    result.revision = object.value(QStringLiteral("revision")).toInteger(1);
    result.id = object.value(QStringLiteral("id")).toString();
    result.projectId = object.value(QStringLiteral("projectId")).toString();
    result.displayName = object.value(QStringLiteral("displayName")).toString();
    result.iconPath = object.value(QStringLiteral("iconPath")).toString();
    result.iconSourcePath = object.value(QStringLiteral("iconSourcePath")).toString();
    result.iconSha256 = object.value(QStringLiteral("iconSha256")).toString();
    result.archPackageName = object.value(QStringLiteral("archPackageName")).toString();
    result.sourceType = sourcePackageTypeFromName(object.value(QStringLiteral("sourceType")).toString());
    result.acquisition = SourceAcquisition::fromJson(object.value(QStringLiteral("acquisition")).toObject());
    result.installMapping = InstallMapping::fromJson(object.value(QStringLiteral("installMapping")).toObject());
    result.originalSourceFilename = object.value(QStringLiteral("originalSourceFilename")).toString();
    result.sourceUrl = object.value(QStringLiteral("sourceUrl")).toString();
    result.sourceSha256 = object.value(QStringLiteral("sourceSha256")).toString();
    result.vendorName = object.value(QStringLiteral("vendorName")).toString();
    result.archPkgrel = std::max(1, object.value(QStringLiteral("archPkgrel")).toInt(1));
    result.archPkgrelOverride = object.value(QStringLiteral("archPkgrelOverride")).toString();
    result.debian = DebianMetadata::fromJson(object.value(QStringLiteral("debian")).toObject());
    if (object.contains(QStringLiteral("packageMetadata"))) {
        result.packageMetadata = PackageMetadata::fromJson(
            object.value(QStringLiteral("packageMetadata")).toObject());
    } else {
        result.packageMetadata.description = result.debian.description;
        result.packageMetadata.homepage = result.debian.homepage;
        result.packageMetadata.licenses = {QStringLiteral("custom:vendor")};
        result.packageMetadata.provides = splitMetadataField(result.debian.provides);
        result.packageMetadata.conflicts = splitMetadataField(result.debian.conflicts);
    }
    result.dependencies = valueListFromJson<DependencyMapping>(object.value(QStringLiteral("dependencies")));
    result.maintainerScripts = valueListFromJson<MaintainerScript>(object.value(QStringLiteral("maintainerScripts")));
    result.scriptFindings = valueListFromJson<ScriptFinding>(object.value(QStringLiteral("scriptFindings")));
    result.payload = valueListFromJson<PayloadEntry>(object.value(QStringLiteral("payload")));
    result.payloadRules = valueListFromJson<PayloadRule>(object.value(QStringLiteral("payloadRules")));
    result.generatedPkgbuild = object.value(QStringLiteral("generatedPkgbuild")).toString();
    result.generatedPkgbuildSha256 = object.value(QStringLiteral("generatedPkgbuildSha256")).toString();
    result.pkgbuildManuallyModified = object.value(QStringLiteral("pkgbuildManuallyModified")).toBool();
    result.customPkgbuild = object.value(QStringLiteral("customPkgbuild")).toString();
    const auto customFiles = object.value(QStringLiteral("customFiles")).toObject();
    for (auto iterator = customFiles.constBegin(); iterator != customFiles.constEnd(); ++iterator) {
        if (iterator.value().isString()) result.customFiles.insert(iterator.key(), iterator.value().toString());
    }
    result.lifecycleScript = ArchLifecycleScript::fromJson(object.value(QStringLiteral("lifecycleScript")).toObject());
    result.lifecycleScript.bindRequiredFindings(result.scriptFindings);
    result.fieldProvenance = provenanceMapFromJson(object.value(QStringLiteral("fieldProvenance")).toObject());
    result.aiChanges = valueListFromJson<AiChangeRecord>(object.value(QStringLiteral("aiChanges")));
    result.update = UpdateConfiguration::fromJson(object.value(QStringLiteral("update")).toObject());
    const auto build = object.value(QStringLiteral("buildStatus")).toString();
    if (build == QStringLiteral("building")) {
        result.buildStatus = BuildStatus::Building;
    } else if (build == QStringLiteral("succeeded")) {
        result.buildStatus = BuildStatus::Succeeded;
    } else if (build == QStringLiteral("failed")) {
        result.buildStatus = BuildStatus::Failed;
    } else if (build == QStringLiteral("canceled")) {
        result.buildStatus = BuildStatus::Canceled;
    }
    result.automaticBuild = object.value(QStringLiteral("automaticBuild")).toBool();
    result.lastBuildLog = object.value(QStringLiteral("lastBuildLog")).toString();
    result.producedPackages = stringsFromJson(object.value(QStringLiteral("producedPackages")));
    result.state = releaseStateFromName(object.value(QStringLiteral("state")).toString());
    result.builds = valueListFromJson<BuildRecord>(object.value(QStringLiteral("builds")));
    result.builtArtifactIds = stringsFromJson(object.value(QStringLiteral("builtArtifactIds")));
    result.sourceArtifactId = object.value(QStringLiteral("sourceArtifactId")).toString();
    result.iconArtifactId = object.value(QStringLiteral("iconArtifactId")).toString();
    result.history = valueListFromJson<HistoryEntry>(object.value(QStringLiteral("history")));
    result.createdAt = dateFromString(object.value(QStringLiteral("createdAt")));
    result.modifiedAt = dateFromString(object.value(QStringLiteral("modifiedAt")));
    return result;
}

PackageRelease *Project::release(const QString &releaseId) {
    const auto iterator = std::find_if(releases.begin(), releases.end(), [&](const auto &candidate) {
        return candidate.id == releaseId;
    });
    return iterator == releases.end() ? nullptr : &*iterator;
}

const PackageRelease *Project::release(const QString &releaseId) const {
    const auto iterator = std::find_if(releases.cbegin(), releases.cend(), [&](const auto &candidate) {
        return candidate.id == releaseId;
    });
    return iterator == releases.cend() ? nullptr : &*iterator;
}

PackageRelease *Project::newestRelease() {
    if (releases.isEmpty()) return nullptr;
    return &*std::max_element(releases.begin(), releases.end(), [](const auto &left, const auto &right) {
        const auto version = compareReleaseVersions(left, right);
        return version == 0 ? left.createdAt < right.createdAt : version < 0;
    });
}

const PackageRelease *Project::newestRelease() const {
    if (releases.isEmpty()) return nullptr;
    return &*std::max_element(releases.cbegin(), releases.cend(), [](const auto &left, const auto &right) {
        const auto version = compareReleaseVersions(left, right);
        return version == 0 ? left.createdAt < right.createdAt : version < 0;
    });
}

PackageRelease *Project::installedRelease() { return release(installedReleaseId); }
const PackageRelease *Project::installedRelease() const { return release(installedReleaseId); }

PackageRelease *Project::activeTrackingRelease() {
    if (auto *installed = installedRelease(); installed != nullptr) return installed;
    PackageRelease *newest = nullptr;
    for (auto &candidate : releases) {
        if (candidate.state == ReleaseState::Discovered ||
            candidate.state == ReleaseState::Preparing) {
            continue;
        }
        if (newest == nullptr ||
            compareReleaseVersions(candidate, *newest) > 0 ||
            (compareReleaseVersions(candidate, *newest) == 0 &&
             candidate.createdAt > newest->createdAt)) {
            newest = &candidate;
        }
    }
    return newest;
}

const PackageRelease *Project::activeTrackingRelease() const {
    if (const auto *installed = installedRelease(); installed != nullptr) return installed;
    const PackageRelease *newest = nullptr;
    for (const auto &candidate : releases) {
        if (candidate.state == ReleaseState::Discovered ||
            candidate.state == ReleaseState::Preparing) {
            continue;
        }
        if (newest == nullptr ||
            compareReleaseVersions(candidate, *newest) > 0 ||
            (compareReleaseVersions(candidate, *newest) == 0 &&
             candidate.createdAt > newest->createdAt)) {
            newest = &candidate;
        }
    }
    return newest;
}

bool Project::hasAvailableUpdate() const {
    const auto *installed = installedRelease();
    if (installedVersion.isEmpty() || installed == nullptr) return false;
    const auto installedVendorVersion = installed->debian.version;
    if (installedVendorVersion.isEmpty()) return false;
    return std::any_of(releases.cbegin(), releases.cend(), [&](const auto &candidate) {
        if (candidate.id == installed->id || candidate.debian.version.isEmpty()) return false;
        return comparePackageVersions(candidate.sourceType, candidate.debian.version,
                                      installedVendorVersion) > 0;
    });
}

bool Project::ownsInstalledPackage() const {
    return !installedVersion.isEmpty() && !externallyInstalled;
}

QJsonObject Project::toJson() const {
    QJsonArray releaseIds;
    for (const auto &item : releases) releaseIds.append(item.id);
    return {{QStringLiteral("formatVersion"), formatVersion},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("id"), id},
            {QStringLiteral("displayName"), displayName},
            {QStringLiteral("archPackageName"), archPackageName},
            {QStringLiteral("vendorName"), vendorName},
            {QStringLiteral("sourceIdentity"), sourceIdentity},
            {QStringLiteral("iconPath"), iconPath},
            {QStringLiteral("iconSourcePath"), iconSourcePath},
            {QStringLiteral("iconSha256"), iconSha256},
            {QStringLiteral("releaseIds"), releaseIds},
            {QStringLiteral("installedVersion"), installedVersion},
            {QStringLiteral("installedReleaseId"), installedReleaseId},
            {QStringLiteral("externallyInstalled"), externallyInstalled},
            {QStringLiteral("autoBuildPolicy"), autoBuildPolicyName(autoBuildPolicy)},
            {QStringLiteral("compileCachePolicy"), compileCachePolicyName(compileCachePolicy)},
            {QStringLiteral("history"), valueListToJson(history)},
            {QStringLiteral("createdAt"), dateToString(createdAt)},
            {QStringLiteral("modifiedAt"), dateToString(modifiedAt)}};
}

Project Project::fromJson(const QJsonObject &object) {
    Project result;
    result.formatVersion = object.value(QStringLiteral("formatVersion")).toInt(4);
    result.revision = object.value(QStringLiteral("revision")).toInteger(1);
    result.id = object.value(QStringLiteral("id")).toString();
    result.displayName = object.value(QStringLiteral("displayName")).toString();
    result.archPackageName = object.value(QStringLiteral("archPackageName")).toString();
    result.vendorName = object.value(QStringLiteral("vendorName")).toString();
    result.sourceIdentity = object.value(QStringLiteral("sourceIdentity")).toString();
    result.iconPath = object.value(QStringLiteral("iconPath")).toString();
    result.iconSourcePath = object.value(QStringLiteral("iconSourcePath")).toString();
    result.iconSha256 = object.value(QStringLiteral("iconSha256")).toString();
    result.installedVersion = object.value(QStringLiteral("installedVersion")).toString();
    result.installedReleaseId = object.value(QStringLiteral("installedReleaseId")).toString();
    result.externallyInstalled = object.value(QStringLiteral("externallyInstalled")).toBool();
    result.autoBuildPolicy = autoBuildPolicyFromName(
        object.value(QStringLiteral("autoBuildPolicy")).toString());
    result.compileCachePolicy = compileCachePolicyFromName(
        object.value(QStringLiteral("compileCachePolicy")).toString());
    result.history = valueListFromJson<HistoryEntry>(object.value(QStringLiteral("history")));
    result.createdAt = dateFromString(object.value(QStringLiteral("createdAt")));
    result.modifiedAt = dateFromString(object.value(QStringLiteral("modifiedAt")));
    result.releases = valueListFromJson<PackageRelease>(object.value(QStringLiteral("releases")));
    result.repository = ProjectRepository::fromJson(object.value(QStringLiteral("repository")).toObject());
    return result;
}

QString autoBuildPolicyName(const AutoBuildPolicy policy) {
    switch (policy) {
    case AutoBuildPolicy::Never: return QStringLiteral("never");
    case AutoBuildPolicy::Ai: return QStringLiteral("ai");
    case AutoBuildPolicy::ReviewFree: return QStringLiteral("review_free");
    }
    return QStringLiteral("review_free");
}

AutoBuildPolicy autoBuildPolicyFromName(const QString &name) {
    if (name == QStringLiteral("never")) return AutoBuildPolicy::Never;
    if (name == QStringLiteral("ai")) return AutoBuildPolicy::Ai;
    return AutoBuildPolicy::ReviewFree;
}

QString compileCachePolicyName(const CompileCachePolicy policy) {
    switch (policy) {
    case CompileCachePolicy::ClearAfterSuccess: return QStringLiteral("clear_after_success");
    case CompileCachePolicy::Disabled: return QStringLiteral("disabled");
    case CompileCachePolicy::Reuse: return QStringLiteral("reuse");
    }
    return QStringLiteral("reuse");
}

CompileCachePolicy compileCachePolicyFromName(const QString &name) {
    if (name == QStringLiteral("clear_after_success")) {
        return CompileCachePolicy::ClearAfterSuccess;
    }
    if (name == QStringLiteral("disabled")) return CompileCachePolicy::Disabled;
    return CompileCachePolicy::Reuse;
}

RepoPackageRef RepoPackageRef::fromJson(const QJsonObject &object) {
    RepoPackageRef result;
    result.pkgname = object.value(QStringLiteral("pkgname")).toString();
    result.arch = object.value(QStringLiteral("arch")).toString();
    result.epoch = object.value(QStringLiteral("epoch")).toInteger();
    result.pkgver = object.value(QStringLiteral("pkgver")).toString();
    result.pkgrel = object.value(QStringLiteral("pkgrel")).toString();
    result.version = object.value(QStringLiteral("version")).toString();
    result.filename = object.value(QStringLiteral("filename")).toString();
    result.artifactId = object.value(QStringLiteral("artifact_id")).toString();
    result.signatureArtifactId = object.value(QStringLiteral("signature_artifact_id")).toString();
    result.releaseId = object.value(QStringLiteral("release_id")).toString();
    return result;
}

RepoSoakStatus RepoSoakStatus::fromJson(const QJsonObject &object) {
    RepoSoakStatus result;
    result.pkgname = object.value(QStringLiteral("pkgname")).toString();
    result.arch = object.value(QStringLiteral("arch")).toString();
    result.pkgver = object.value(QStringLiteral("pkgver")).toString();
    result.pkgrel = object.value(QStringLiteral("pkgrel")).toString();
    result.version = object.value(QStringLiteral("version")).toString();
    result.status = object.value(QStringLiteral("status")).toString();
    result.startedAt = object.value(QStringLiteral("soak_started_at")).toString();
    result.eligibleAt = object.value(QStringLiteral("eligible_at")).toString();
    result.artifactId = object.value(QStringLiteral("artifact_id")).toString();
    result.releaseId = object.value(QStringLiteral("release_id")).toString();
    return result;
}

ProjectRepository ProjectRepository::fromJson(const QJsonObject &object) {
    ProjectRepository result;
    result.revision = object.value(QStringLiteral("revision")).toInteger();
    result.publish = object.value(QStringLiteral("publish")).toBool();
    result.stableChannelEnabled = object.value(QStringLiteral("stable_channel_enabled")).toBool();
    result.automaticSoak = object.value(QStringLiteral("automatic_soak")).toBool();
    result.soakSecondsOverride = object.value(QStringLiteral("soak_seconds_override")).toInteger(-1);
    result.librarySoakSeconds = object.value(QStringLiteral("library_soak_seconds")).toInteger();
    result.effectiveSoakSeconds = object.value(QStringLiteral("effective_soak_seconds")).toInteger();
    result.originalPackageName = object.value(QStringLiteral("original_package_name")).toString();
    result.archPackageName = object.value(QStringLiteral("arch_package_name")).toString();
    result.prefixDefault = object.value(QStringLiteral("prefix_default")).toString();
    result.packageNameOverride = object.value(QStringLiteral("package_name_override")).toString();
    result.effectivePackageName = object.value(QStringLiteral("effective_package_name")).toString();
    result.publishedPackageName = object.value(QStringLiteral("published_package_name")).toString();
    result.pkgnameChangeWarning = object.value(QStringLiteral("pkgname_change_warning")).toBool();
    result.reserved = object.value(QStringLiteral("reserved")).toBool();
    const auto unstable = object.value(QStringLiteral("unstable"));
    if (unstable.isObject()) {
        result.hasUnstable = true;
        result.unstable = RepoPackageRef::fromJson(unstable.toObject());
    }
    const auto stable = object.value(QStringLiteral("stable"));
    if (stable.isObject()) {
        result.hasStable = true;
        result.stable = RepoPackageRef::fromJson(stable.toObject());
    }
    result.soaks = valueListFromJson<RepoSoakStatus>(object.value(QStringLiteral("soaks")));
    return result;
}

QString mappingStatusName(const MappingStatus status) {
    switch (status) {
    case MappingStatus::Resolved: return QStringLiteral("Resolved");
    case MappingStatus::Ignored: return QStringLiteral("Ignored");
    case MappingStatus::Bundled: return QStringLiteral("Bundled");
    case MappingStatus::Provided: return QStringLiteral("Provided");
    case MappingStatus::Unresolved: return QStringLiteral("Unresolved");
    }
    return QStringLiteral("Unresolved");
}

QString buildStatusName(const BuildStatus status) {
    switch (status) {
    case BuildStatus::NeverBuilt: return QStringLiteral("never-built");
    case BuildStatus::Building: return QStringLiteral("building");
    case BuildStatus::Succeeded: return QStringLiteral("succeeded");
    case BuildStatus::Failed: return QStringLiteral("failed");
    case BuildStatus::Canceled: return QStringLiteral("canceled");
    }
    return QStringLiteral("never-built");
}

QString releaseStateName(const ReleaseState state) {
    switch (state) {
    case ReleaseState::Discovered: return QStringLiteral("discovered");
    case ReleaseState::Preparing: return QStringLiteral("preparing");
    case ReleaseState::NeedsReview: return QStringLiteral("needs-review");
    case ReleaseState::Ready: return QStringLiteral("ready");
    case ReleaseState::Built: return QStringLiteral("built");
    }
    return QStringLiteral("needs-review");
}

ReleaseState releaseStateFromName(const QString &name) {
    if (name == QStringLiteral("discovered")) return ReleaseState::Discovered;
    if (name == QStringLiteral("preparing")) return ReleaseState::Preparing;
    if (name == QStringLiteral("ready")) return ReleaseState::Ready;
    if (name == QStringLiteral("built")) return ReleaseState::Built;
    return ReleaseState::NeedsReview;
}

QString updateStrategyName(const UpdateStrategy strategy) {
    switch (strategy) {
    case UpdateStrategy::Manual: return QStringLiteral("Manual");
    case UpdateStrategy::DirectUrl: return QStringLiteral("Direct URL");
    case UpdateStrategy::AptRepository: return QStringLiteral("APT repository");
    case UpdateStrategy::RpmRepository: return QStringLiteral("RPM repository");
    case UpdateStrategy::GitHubRelease: return QStringLiteral("GitHub releases");
    }
    return QStringLiteral("Manual");
}

MappingStatus mappingStatusFromName(const QString &name) {
    const auto normalized = name.toLower();
    if (normalized == QStringLiteral("resolved")) return MappingStatus::Resolved;
    if (normalized == QStringLiteral("ignored")) return MappingStatus::Ignored;
    if (normalized == QStringLiteral("bundled")) return MappingStatus::Bundled;
    if (normalized == QStringLiteral("provided")) return MappingStatus::Provided;
    return MappingStatus::Unresolved;
}

UpdateStrategy updateStrategyFromName(const QString &name) {
    if (name.compare(QStringLiteral("Direct URL"), Qt::CaseInsensitive) == 0) {
        return UpdateStrategy::DirectUrl;
    }
    if (name.compare(QStringLiteral("APT repository"), Qt::CaseInsensitive) == 0) {
        return UpdateStrategy::AptRepository;
    }
    if (name.compare(QStringLiteral("RPM repository"), Qt::CaseInsensitive) == 0) {
        return UpdateStrategy::RpmRepository;
    }
    if (name.compare(QStringLiteral("GitHub releases"), Qt::CaseInsensitive) == 0) {
        return UpdateStrategy::GitHubRelease;
    }
    return UpdateStrategy::Manual;
}

QString sourcePackageTypeName(const SourcePackageType type) {
    switch (type) {
    case SourcePackageType::Unknown: return QStringLiteral("not-inspected");
    case SourcePackageType::Debian: return QStringLiteral("deb");
    case SourcePackageType::Rpm: return QStringLiteral("rpm");
    case SourcePackageType::ArchPackage: return QStringLiteral("arch-package");
    case SourcePackageType::Archive: return QStringLiteral("archive");
    case SourcePackageType::AppImage: return QStringLiteral("appimage");
    case SourcePackageType::ElfBinary: return QStringLiteral("elf-binary");
    }
    return QStringLiteral("not-inspected");
}

SourcePackageType sourcePackageTypeFromName(const QString &name) {
    if (name == QStringLiteral("not-inspected")) return SourcePackageType::Unknown;
    if (name == QStringLiteral("rpm")) return SourcePackageType::Rpm;
    if (name == QStringLiteral("arch-package")) return SourcePackageType::ArchPackage;
    if (name == QStringLiteral("archive")) return SourcePackageType::Archive;
    if (name == QStringLiteral("appimage")) return SourcePackageType::AppImage;
    if (name == QStringLiteral("elf-binary")) return SourcePackageType::ElfBinary;
    return SourcePackageType::Debian;
}

QString acquisitionKindName(const AcquisitionKind kind) {
    switch (kind) {
    case AcquisitionKind::LocalFile: return QStringLiteral("local-file");
    case AcquisitionKind::DirectUrl: return QStringLiteral("direct-url");
    case AcquisitionKind::AptRepository: return QStringLiteral("apt-repository");
    case AcquisitionKind::RpmRepository: return QStringLiteral("rpm-repository");
    case AcquisitionKind::GitHubRelease: return QStringLiteral("github-release");
    }
    return QStringLiteral("local-file");
}

AcquisitionKind acquisitionKindFromName(const QString &name) {
    if (name == QStringLiteral("direct-url")) return AcquisitionKind::DirectUrl;
    if (name == QStringLiteral("apt-repository")) return AcquisitionKind::AptRepository;
    if (name == QStringLiteral("rpm-repository")) return AcquisitionKind::RpmRepository;
    if (name == QStringLiteral("github-release")) return AcquisitionKind::GitHubRelease;
    return AcquisitionKind::LocalFile;
}

QString valueOriginName(const ValueOrigin origin) {
    switch (origin) {
    case ValueOrigin::Deterministic: return QStringLiteral("deterministic");
    case ValueOrigin::Ai: return QStringLiteral("ai");
    case ValueOrigin::User: return QStringLiteral("user");
    case ValueOrigin::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

ValueOrigin valueOriginFromName(const QString &name) {
    if (name == QStringLiteral("deterministic")) return ValueOrigin::Deterministic;
    if (name == QStringLiteral("ai")) return ValueOrigin::Ai;
    if (name == QStringLiteral("user")) return ValueOrigin::User;
    return ValueOrigin::Unknown;
}

QString scriptDispositionName(const ScriptDisposition disposition) {
    switch (disposition) {
    case ScriptDisposition::HandledByPacSmith: return QStringLiteral("handled-by-pacsmith");
    case ScriptDisposition::HandledByArch: return QStringLiteral("handled-by-arch");
    case ScriptDisposition::LifecycleRequired: return QStringLiteral("lifecycle-required");
    case ScriptDisposition::NotApplicable: return QStringLiteral("not-applicable");
    case ScriptDisposition::Unresolved: return QStringLiteral("unresolved");
    }
    return QStringLiteral("unresolved");
}

QString scriptDispositionLabel(const ScriptDisposition disposition) {
    switch (disposition) {
    case ScriptDisposition::HandledByPacSmith: return QStringLiteral("Handled by PacSmith");
    case ScriptDisposition::HandledByArch: return QStringLiteral("Handled by Arch");
    case ScriptDisposition::LifecycleRequired: return QStringLiteral("Lifecycle script");
    case ScriptDisposition::NotApplicable: return QStringLiteral("Not applicable");
    case ScriptDisposition::Unresolved: return QStringLiteral("Unresolved");
    }
    return QStringLiteral("Unresolved");
}

ScriptDisposition scriptDispositionFromName(const QString &name) {
    if (name == QStringLiteral("handled-by-pacsmith")) return ScriptDisposition::HandledByPacSmith;
    if (name == QStringLiteral("handled-by-arch")) return ScriptDisposition::HandledByArch;
    if (name == QStringLiteral("lifecycle-required")) return ScriptDisposition::LifecycleRequired;
    if (name == QStringLiteral("not-applicable")) return ScriptDisposition::NotApplicable;
    return ScriptDisposition::Unresolved;
}

int comparePackageVersions(const SourcePackageType sourceType,
                           const QString &left, const QString &right) {
    return sourceType == SourcePackageType::Rpm ? RpmVersion::compare(left, right)
                                                : DebianVersion::compare(left, right);
}

int compareReleaseVersions(const PackageRelease &left, const PackageRelease &right) {
    const auto type = left.sourceType == SourcePackageType::Rpm ||
                      right.sourceType == SourcePackageType::Rpm
        ? SourcePackageType::Rpm : left.sourceType;
    return comparePackageVersions(type, left.debian.version, right.debian.version);
}

} // namespace pacsmith
