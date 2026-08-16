#include "core/source_analyzer.hpp"

#include "core/apt_sources.hpp"
#include "core/path_safety.hpp"
#include "core/pkgbuild_generator.hpp"
#include "core/project_store.hpp"
#include "core/rpm_analyzer.hpp"
#include "core/script_evidence.hpp"

#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTemporaryDir>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>

namespace pacsmith {
namespace {

struct ArchiveDeleter {
    void operator()(archive *value) const { archive_read_free(value); }
};
using ArchivePointer = std::unique_ptr<archive, ArchiveDeleter>;

QString qPath(const std::filesystem::path &path) {
    return QString::fromUtf8(path.string().c_str());
}

ArchivePointer openArchive(const std::filesystem::path &path, QString *error) {
    ArchivePointer reader(archive_read_new());
    archive_read_support_filter_all(reader.get());
    archive_read_support_format_all(reader.get());
    if (archive_read_open_filename(reader.get(), path.c_str(), 64 * 1024) != ARCHIVE_OK) {
        if (error != nullptr) *error = QString::fromUtf8(archive_error_string(reader.get()));
        return {};
    }
    return reader;
}

QByteArray readEntry(archive *reader, const qsizetype maximum, QString *error) {
    QByteArray result;
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = archive_read_data(reader, buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (error != nullptr) *error = QString::fromUtf8(archive_error_string(reader));
            return {};
        }
        if (result.size() + count > maximum) {
            if (error != nullptr) *error = QStringLiteral("Archive metadata exceeds the safety limit");
            return {};
        }
        result.append(buffer.data(), count);
    }
    return result;
}

QStringList fields(const QByteArray &contents, const QByteArray &name) {
    const auto prefix = name + QByteArrayLiteral(" = ");
    QStringList result;
    for (const auto &line : contents.split('\n')) {
        if (line.startsWith(prefix)) result.append(QString::fromUtf8(line.sliced(prefix.size())).trimmed());
    }
    return result;
}

QString firstField(const QByteArray &contents, const QByteArray &name) {
    const auto values = fields(contents, name);
    return values.isEmpty() ? QString{} : values.first();
}

QString strippedFilename(QString name) {
    static const QStringList suffixes{QStringLiteral(".pkg.tar.zst"), QStringLiteral(".pkg.tar.xz"),
                                      QStringLiteral(".pkg.tar.gz"), QStringLiteral(".tar.zst"),
                                      QStringLiteral(".tar.xz"), QStringLiteral(".tar.bz2"),
                                      QStringLiteral(".tar.lz4"), QStringLiteral(".tar.gz"),
                                      QStringLiteral(".tbz2"), QStringLiteral(".tgz"),
                                      QStringLiteral(".tar"), QStringLiteral(".zip"),
                                      QStringLiteral(".7z")};
    if (name.endsWith(QStringLiteral(".AppImage"), Qt::CaseInsensitive)) {
        name.chop(9);
        return name;
    }
    for (const auto &suffix : suffixes) {
        if (name.endsWith(suffix, Qt::CaseInsensitive)) {
            name.chop(suffix.size());
            break;
        }
    }
    return name;
}

void inferNameVersion(const std::filesystem::path &path, DebianMetadata &metadata) {
    auto base = strippedFilename(QFileInfo(qPath(path)).fileName());
    static const QRegularExpression platformSuffix(
        QStringLiteral(R"([_-](?:linux[_-])?(?:x86_64|amd64|aarch64|arm64)$)"),
        QRegularExpression::CaseInsensitiveOption);
    base.remove(platformSuffix);
    static const QRegularExpression simple(
        QStringLiteral(R"(^(.+?)[-_]v?([0-9][A-Za-z0-9.+~_-]*)$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = simple.match(base);
    metadata.package = PkgbuildGenerator::sanitizePackageName(
        match.hasMatch() ? match.captured(1) : base);
    metadata.version = match.hasMatch() ? match.captured(2) : QStringLiteral("1.0.0");
    metadata.architecture = QSysInfo::currentCpuArchitecture() == QStringLiteral("x86_64")
                                ? QStringLiteral("amd64")
                                : QSysInfo::currentCpuArchitecture();
    metadata.description = metadata.package;
}

QString dependencyName(const QString &expression) {
    static const QRegularExpression name(QStringLiteral(R"(^\s*([A-Za-z0-9@._+\-]+))"));
    return name.match(expression).captured(1);
}

bool isDesktopEntry(const QString &path, const qint64 size) {
    return size >= 0 && size <= 256 * 1024 &&
           path.endsWith(QStringLiteral(".desktop"));
}

QString desktopEntryValue(const QString &contents, const QString &key) {
    const QRegularExpression expression(
        QStringLiteral("(?m)^%1=(.*)$").arg(QRegularExpression::escape(key)));
    return expression.match(contents).captured(1).trimmed();
}

bool appImageDesktopCandidatePath(const QString &path, const qint64 size) {
    if (!isDesktopEntry(path, size)) return false;
    if (!path.contains(QLatin1Char('/'))) return true;
    const auto prefix = QStringLiteral("usr/share/applications/");
    return path.startsWith(prefix) && !path.sliced(prefix.size()).contains(QLatin1Char('/'));
}

bool appImageApplicationDesktopEntry(const QString &contents) {
    return desktopEntryValue(contents, QStringLiteral("Type")) == QStringLiteral("Application") &&
           !desktopEntryValue(contents, QStringLiteral("Exec")).isEmpty();
}

QString desktopEntryCommand(const QString &contents) {
    auto exec = desktopEntryValue(contents, QStringLiteral("Exec"));
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

int appImageDesktopScore(const DesktopEntryConfiguration &desktop,
                         const QString &packageName) {
    const bool topLevel = !desktop.sourcePath.contains(QLatin1Char('/'));
    const auto noDisplay = desktopEntryValue(desktop.contents, QStringLiteral("NoDisplay"))
                               .compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
    const auto command = desktopEntryCommand(desktop.contents);
    const auto icon = desktopEntryValue(desktop.contents, QStringLiteral("Icon"));
    const auto name = desktopEntryValue(desktop.contents, QStringLiteral("Name"));
    int score = topLevel ? 10000 : 100;
    score += noDisplay ? -5000 : 1000;
    for (const auto &value : {desktop.id, command, icon, name}) {
        if (!packageName.isEmpty() && value.contains(packageName, Qt::CaseInsensitive)) {
            score += 500;
        }
    }
    if (!command.isEmpty()) score += 250;
    return score;
}

bool isIconCandidate(const QString &path, const qint64 size) {
    if (size <= 0 || size > 4 * 1024 * 1024) return false;
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QStringLiteral("png") && suffix != QStringLiteral("svg") &&
        suffix != QStringLiteral("xpm")) return false;
    return true;
}

bool isDirectOptApplication(const QString &path) {
    const auto components = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return components.size() == 3 && components.first() == QStringLiteral("opt");
}

bool likelyUserCommand(const QString &path) {
    const auto info = QFileInfo(path);
    const auto name = info.fileName().toLower();
    if (name.isEmpty() || name.endsWith(QStringLiteral(".so")) ||
        name.contains(QStringLiteral(".so.")) || name.endsWith(QStringLiteral(".dll")) ||
        name.endsWith(QStringLiteral(".dylib")) || name.contains(QStringLiteral("debug")) ||
        name.contains(QStringLiteral("crashpad")) || name.contains(QStringLiteral("sandbox")) ||
        name == QStringLiteral("apprun")) {
        return false;
    }
    return isDirectOptApplication(path) || path.startsWith(QStringLiteral("bin/")) ||
           path.startsWith(QStringLiteral("usr/bin/")) ||
           path.startsWith(QStringLiteral("usr/local/bin/")) ||
           !path.contains(QLatin1Char('/'));
}

quint32 bigEndian32(const QByteArray &data, const qsizetype offset) {
    return (static_cast<quint32>(static_cast<unsigned char>(data.at(offset))) << 24U) |
           (static_cast<quint32>(static_cast<unsigned char>(data.at(offset + 1))) << 16U) |
           (static_cast<quint32>(static_cast<unsigned char>(data.at(offset + 2))) << 8U) |
           static_cast<quint32>(static_cast<unsigned char>(data.at(offset + 3)));
}

bool validIconContents(const QString &path, const QByteArray &contents) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("png")) {
        static const QByteArray signature("\x89PNG\r\n\x1a\n", 8);
        if (contents.size() < 24 || !contents.startsWith(signature)) return false;
        const auto width = bigEndian32(contents, 16);
        const auto height = bigEndian32(contents, 20);
        return width > 0 && height > 0 && width <= 2048 && height <= 2048 &&
               static_cast<quint64>(width) * static_cast<quint64>(height) <=
                   4U * 1024U * 1024U;
    }
    if (suffix == QStringLiteral("xpm")) {
        return contents.size() <= 1024 * 1024 && contents.left(256).contains("/* XPM */");
    }
    if (suffix == QStringLiteral("svg")) {
        const auto prefix = contents.left(4096).trimmed().toLower();
        return contents.size() <= 2 * 1024 * 1024 && prefix.contains("<svg") &&
               !prefix.contains("<!entity") && !prefix.contains("<!doctype");
    }
    return false;
}

void collectDesktopIconReferences(const QByteArray &contents, QSet<QString> &references) {
    for (auto line : contents.split('\n')) {
        line = line.trimmed();
        if (!line.startsWith("Icon=")) continue;
        auto value = QString::fromUtf8(line.mid(5)).trimmed();
        if (value.startsWith(QLatin1Char('/'))) value.remove(0, 1);
        const auto safe = PathSafety::normalizedArchivePath(value);
        if (value.contains(QLatin1Char('/')) && !safe) continue;
        if (safe) value = *safe;
        if (value.isEmpty()) continue;
        references.insert(value);
        const QFileInfo info(value);
        references.insert(info.fileName());
        references.insert(info.completeBaseName());
    }
}

struct IconCandidate {
    QString path;
    QByteArray contents;
};

int iconScore(const IconCandidate &candidate, const QSet<QString> &references,
              const QString &packageName) {
    const QFileInfo info(candidate.path);
    const auto fileName = info.fileName();
    const auto stem = info.completeBaseName();
    int score = 0;
    if (references.contains(candidate.path) || references.contains(fileName) ||
        references.contains(stem)) score += 100000;
    if (candidate.path.startsWith(QStringLiteral("usr/share/icons/"))) score += 10000;
    const auto suffix = info.suffix().toLower();
    score += suffix == QStringLiteral("png") ? 3000
           : suffix == QStringLiteral("svg") ? 2000 : 1000;
    static const QRegularExpression dimensions(QStringLiteral(R"(/(\d+)x(\d+)/apps/)"));
    const auto match = dimensions.match(candidate.path);
    if (match.hasMatch()) {
        score += std::min(std::min(match.captured(1).toInt(), match.captured(2).toInt()), 512);
    }
    if (!packageName.isEmpty() && stem.contains(packageName, Qt::CaseInsensitive)) score += 500;
    return score;
}

QString normalizedDesktopContents(QString contents, const QString &command,
                                  const QString &iconName) {
    QStringList lines = contents.replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
                            .split(QLatin1Char('\n'));
    bool hasExec = false;
    bool hasIcon = false;
    for (auto &line : lines) {
        if (line.startsWith(QStringLiteral("Exec=")) && !command.isEmpty()) {
            const auto value = line.mid(5).trimmed();
            const auto firstSpace = value.indexOf(QRegularExpression(QStringLiteral("\\s")));
            const auto arguments = firstSpace < 0 ? QString{} : value.mid(firstSpace);
            line = QStringLiteral("Exec=%1%2").arg(command, arguments);
            hasExec = true;
        } else if (line.startsWith(QStringLiteral("Icon=")) && !iconName.isEmpty()) {
            line = QStringLiteral("Icon=%1").arg(iconName);
            hasIcon = true;
        }
    }
    if (!hasExec && !command.isEmpty()) lines.append(QStringLiteral("Exec=%1").arg(command));
    if (!hasIcon && !iconName.isEmpty()) lines.append(QStringLiteral("Icon=%1").arg(iconName));
    return lines.join(QLatin1Char('\n'));
}

QString exclusionRoot(const QString &path) {
    for (const auto &root : {QStringLiteral("etc/apt"),
                             QStringLiteral("usr/share/keyrings"),
                             QStringLiteral("usr/share/lintian"),
                             QStringLiteral("etc/yum.repos.d"),
                             QStringLiteral("etc/dnf"),
                             QStringLiteral("etc/zypp"),
                             QStringLiteral("etc/pki/rpm-gpg"),
                             QStringLiteral("etc/rpm"),
                             QStringLiteral("usr/lib/sysimage/rpm"),
                             QStringLiteral("var/lib/rpm")}) {
        if (path == root || path.startsWith(root + QLatin1Char('/'))) return root;
    }
    return path;
}

void appendReviewRule(SourceAnalysis &result, const PayloadEntry &entry,
                      QSet<QString> &rulePaths) {
    if (!entry.requiresReview) return;
    const bool excluded = PathSafety::isForeignPackageManagerPath(entry.path);
    const auto path = excluded ? exclusionRoot(entry.path) : entry.path;
    if (rulePaths.contains(path)) return;
    PayloadRule rule;
    rule.path = path;
    rule.reason = entry.reviewReason;
    rule.excluded = excluded;
    result.payloadRules.append(rule);
    rulePaths.insert(path);
}

bool inspectReviewEntry(archive *reader, PayloadEntry &entry, QByteArray *captured,
                        QString *error) {
    constexpr qint64 maximumReviewFile = 64LL * 1024 * 1024;
    constexpr qsizetype previewLimit = 1024 * 1024;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray preview;
    qint64 total = 0;
    bool truncated = false;
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        const auto count = archive_read_data(reader, buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (error != nullptr) *error = QString::fromUtf8(archive_error_string(reader));
            return false;
        }
        total += count;
        if (total > maximumReviewFile) {
            if (error != nullptr) {
                *error = QStringLiteral("Review-sensitive archive entry exceeds 64 MiB: %1")
                             .arg(entry.path);
            }
            return false;
        }
        const auto length = static_cast<qsizetype>(count);
        hash.addData(QByteArrayView(buffer.data(), length));
        const auto available = previewLimit - preview.size();
        if (available > 0) preview.append(buffer.data(), std::min(available, length));
        if (length > available) truncated = true;
    }
    entry.contentSha256 = QString::fromLatin1(hash.result().toHex());
    entry.previewTruncated = truncated;
    if (captured != nullptr) *captured = preview;
    if (!preview.contains('\0')) {
        QStringDecoder decoder(QStringDecoder::Utf8);
        entry.textPreview = decoder.decode(preview);
        if (decoder.hasError()) entry.textPreview.clear();
    }
    return true;
}

bool isRepositoryKeyPath(const QString &path) {
    return path.startsWith(QStringLiteral("usr/share/keyrings/")) ||
           path.startsWith(QStringLiteral("etc/pki/rpm-gpg/"));
}

bool shouldInspectRepositoryContents(const QString &path, const qint64 size) {
    if (size < 0 || size > 1024 * 1024) return false;
    return path.startsWith(QStringLiteral("etc/apt/")) ||
           path.startsWith(QStringLiteral("etc/yum.repos.d/")) ||
           path.startsWith(QStringLiteral("etc/dnf/")) ||
           path.startsWith(QStringLiteral("etc/zypp/")) ||
           path.endsWith(QStringLiteral(".sources")) ||
           path.endsWith(QStringLiteral(".list")) ||
           path.endsWith(QStringLiteral(".repo"));
}

void deduplicateEvidence(SourceAnalysis &result) {
    QList<AptRepositoryCandidate> apt;
    QSet<QString> aptSeen;
    for (const auto &candidate : std::as_const(result.aptCandidates)) {
        const auto identity = candidate.displayText() + QLatin1Char('\n') + candidate.sourcePath;
        if (!aptSeen.contains(identity)) {
            aptSeen.insert(identity);
            apt.append(candidate);
        }
    }
    result.aptCandidates = apt;

    QList<RpmRepositoryCandidate> rpm;
    QSet<QString> rpmSeen;
    for (const auto &candidate : std::as_const(result.rpmCandidates)) {
        const auto identity = candidate.baseUrl + QLatin1Char('\n') + candidate.architecture;
        if (!rpmSeen.contains(identity)) {
            rpmSeen.insert(identity);
            rpm.append(candidate);
        }
    }
    result.rpmCandidates = rpm;

    QList<ExtractedSigningKey> keys;
    QSet<QString> keySeen;
    for (const auto &key : std::as_const(result.signingKeys)) {
        const auto identity = QString::fromLatin1(
            QCryptographicHash::hash(key.contents, QCryptographicHash::Sha256).toHex());
        if (!keySeen.contains(identity)) {
            keySeen.insert(identity);
            keys.append(key);
        }
    }
    result.signingKeys = keys;
}

std::optional<SourceAnalysis> analyzeArchive(const std::filesystem::path &path,
                                             const bool archPackage,
                                             QString *error,
                                             const ImportProgressCallback &progress) {
    auto reader = openArchive(path, error);
    if (!reader) return std::nullopt;
    SourceAnalysis result;
    result.type = archPackage ? SourcePackageType::ArchPackage : SourcePackageType::Archive;
    QByteArray pkginfo;
    QByteArray installScript;
    QSet<QString> paths;
    bool recognizedRoot = false;
    qsizetype entries = 0;
    QSet<QString> desktopIconReferences;
    QSet<QString> rulePaths;
    QList<IconCandidate> iconCandidates;
    qsizetype capturedIconBytes = 0;
    archive_entry *entry = nullptr;
    while (archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK) {
        const auto rawPath = QString::fromUtf8(archive_entry_pathname(entry));
        const auto normalized = PathSafety::normalizedArchivePath(rawPath);
        if (!normalized || normalized->isEmpty()) {
            if (error != nullptr) *error = QStringLiteral("Unsafe archive path: %1").arg(rawPath);
            return std::nullopt;
        }
        if (paths.contains(*normalized)) {
            if (error != nullptr) *error = QStringLiteral("Duplicate archive path: %1").arg(*normalized);
            return std::nullopt;
        }
        paths.insert(*normalized);
        const auto fileType = archive_entry_filetype(entry);
        if (fileType == AE_IFCHR || fileType == AE_IFBLK || fileType == AE_IFIFO || fileType == AE_IFSOCK) {
            if (error != nullptr) *error = QStringLiteral("Special device entry is not permitted: %1").arg(*normalized);
            return std::nullopt;
        }
        if (fileType == AE_IFLNK) {
            const auto target = QString::fromUtf8(archive_entry_symlink(entry));
            if (!PathSafety::safeSymlinkTarget(*normalized, target)) {
                if (error != nullptr) *error = QStringLiteral("Unsafe symlink %1 -> %2").arg(*normalized, target);
                return std::nullopt;
            }
        }
        const auto hardlink = archive_entry_hardlink(entry);
        if (hardlink != nullptr && !PathSafety::normalizedArchivePath(QString::fromUtf8(hardlink))) {
            if (error != nullptr) *error = QStringLiteral("Unsafe hard link in %1").arg(*normalized);
            return std::nullopt;
        }
        if (*normalized == QStringLiteral(".PKGINFO")) {
            pkginfo = readEntry(reader.get(), 1024 * 1024, error);
            if (pkginfo.isEmpty() && archive_entry_size(entry) > 0) return std::nullopt;
            continue;
        }
        if (*normalized == QStringLiteral(".INSTALL")) {
            installScript = readEntry(reader.get(), 4 * 1024 * 1024, error);
            if (installScript.isEmpty() && archive_entry_size(entry) > 0) return std::nullopt;
            continue;
        }
        if (normalized->startsWith(QStringLiteral("usr/")) || normalized->startsWith(QStringLiteral("etc/")) ||
            normalized->startsWith(QStringLiteral("opt/")) || normalized->startsWith(QStringLiteral("var/"))) {
            recognizedRoot = true;
        }
        PayloadEntry payload;
        payload.path = *normalized;
        payload.size = std::max<qint64>(0, archive_entry_size(entry));
        payload.type = fileType == AE_IFDIR ? QStringLiteral("directory")
                     : fileType == AE_IFLNK ? QStringLiteral("symlink") : QStringLiteral("file");
        if (fileType == AE_IFLNK) payload.symlinkTarget = QString::fromUtf8(archive_entry_symlink(entry));
        payload.reviewReason = PathSafety::reviewReason(payload.path);
        payload.requiresReview = !payload.reviewReason.isEmpty() && payload.type != QStringLiteral("directory");
        const auto permissions = archive_entry_perm(entry);
        if ((permissions & 06000) != 0) {
            if (!payload.reviewReason.isEmpty()) payload.reviewReason += QStringLiteral("; ");
            payload.reviewReason += QStringLiteral("Set-user-ID or set-group-ID permission requires review");
            payload.requiresReview = true;
        }
        if (payload.type == QStringLiteral("file") &&
            (archive_entry_perm(entry) & 0111) != 0 && likelyUserCommand(payload.path)) {
            if (result.installMapping.binarySourcePath.isEmpty()) {
                result.installMapping.binarySourcePath = payload.path;
            }
            LauncherMapping launcher;
            launcher.enabled = !isDirectOptApplication(payload.path);
            launcher.sourcePath = payload.path;
            launcher.commandName = QFileInfo(payload.path).fileName();
            launcher.destination = QStringLiteral("/usr/bin/%1").arg(launcher.commandName);
            launcher.provenance.origin = ValueOrigin::Deterministic;
            launcher.provenance.rationale = launcher.enabled
                ? QStringLiteral("Executable command detected in the inspected payload")
                : QStringLiteral(
                      "Direct /opt application executable detected; it is available for explicit command exposure");
            result.installMapping.launchers.append(launcher);
        }
        result.payload.append(payload);
        appendReviewRule(result, payload, rulePaths);
        if (payload.requiresReview && payload.type == QStringLiteral("file")) {
            QByteArray captured;
            if (!inspectReviewEntry(reader.get(), result.payload.last(), &captured, error)) {
                return std::nullopt;
            }
            const auto &reviewed = result.payload.last();
            if (!reviewed.textPreview.isEmpty() && !reviewed.previewTruncated) {
                const auto evidence = ScriptEvidenceAnalyzer::analyze(
                    {{QStringLiteral("payload/%1").arg(reviewed.path), reviewed.textPreview, {}}});
                result.rpmCandidates.append(evidence.rpmCandidates);
                result.aptCandidates.append(evidence.aptCandidates);
                result.signingKeys.append(evidence.signingKeys);
                result.updateCandidates.append(PathSafety::urlsFromText(reviewed.textPreview));
            }
            if (shouldInspectRepositoryContents(reviewed.path, reviewed.size) &&
                !reviewed.previewTruncated) {
                result.aptCandidates.append(
                    AptSourcesParser::parse(QByteArrayView(captured), reviewed.path));
            }
            if (isRepositoryKeyPath(reviewed.path) && !reviewed.previewTruncated &&
                !captured.isEmpty()) {
                result.signingKeys.append(
                    {captured, reviewed.path, reviewed.contentSha256});
            }
        } else if (payload.type == QStringLiteral("file") &&
                   (isDesktopEntry(payload.path, payload.size) ||
                    isIconCandidate(payload.path, payload.size))) {
            const auto contents = readEntry(reader.get(), 4 * 1024 * 1024, error);
            if (contents.isEmpty() && payload.size > 0) return std::nullopt;
            if (isDesktopEntry(payload.path, payload.size)) {
                collectDesktopIconReferences(contents, desktopIconReferences);
                DesktopEntryConfiguration desktop;
                desktop.id = QFileInfo(payload.path).completeBaseName();
                desktop.sourcePath = payload.path;
                desktop.destination = QStringLiteral("/usr/share/applications/%1")
                                          .arg(QFileInfo(payload.path).fileName());
                desktop.contents = QString::fromUtf8(contents);
                desktop.sourceSha256 = sha256Hex(contents);
                desktop.originalContentsSha256 = desktop.sourceSha256;
                desktop.provenance.origin = ValueOrigin::Deterministic;
                desktop.provenance.rationale = QStringLiteral(
                    "Desktop entry detected in the inspected payload");
                result.installMapping.desktopEntries.append(desktop);
            }
            constexpr qsizetype totalIconLimit = 32 * 1024 * 1024;
            if (isIconCandidate(payload.path, payload.size) && iconCandidates.size() < 128 &&
                contents.size() <= totalIconLimit - capturedIconBytes &&
                validIconContents(payload.path, contents)) {
                capturedIconBytes += contents.size();
                iconCandidates.append({payload.path, contents});
            }
        } else {
            archive_read_data_skip(reader.get());
        }
        ++entries;
        if (progress && entries % 250 == 0) progress({ImportStage::ReadingPayloadArchive, entries});
    }
    if (archPackage) {
        if (pkginfo.isEmpty()) {
            if (error != nullptr) *error = QStringLiteral("Arch package does not contain .PKGINFO");
            return std::nullopt;
        }
        result.metadata.package = firstField(pkginfo, QByteArrayLiteral("pkgname"));
        const auto fullVersion = firstField(pkginfo, QByteArrayLiteral("pkgver"));
        const auto dash = fullVersion.lastIndexOf(QLatin1Char('-'));
        result.metadata.version = dash > 0 ? fullVersion.left(dash) : fullVersion;
        result.upstreamArchPkgrel = dash > 0 ? fullVersion.mid(dash + 1) : QStringLiteral("1");
        result.metadata.architecture = firstField(pkginfo, QByteArrayLiteral("arch"));
        result.metadata.description = firstField(pkginfo, QByteArrayLiteral("pkgdesc"));
        result.metadata.homepage = firstField(pkginfo, QByteArrayLiteral("url"));
        result.metadata.maintainer = firstField(pkginfo, QByteArrayLiteral("packager"));
        result.metadata.depends = fields(pkginfo, QByteArrayLiteral("depend")).join(QStringLiteral(", "));
        result.metadata.conflicts = fields(pkginfo, QByteArrayLiteral("conflict")).join(QStringLiteral(", "));
        result.metadata.provides = fields(pkginfo, QByteArrayLiteral("provides")).join(QStringLiteral(", "));
        for (const auto &line : pkginfo.split('\n')) {
            const auto separator = line.indexOf(QByteArrayLiteral(" = "));
            if (separator <= 0) continue;
            const auto key = QString::fromUtf8(line.first(separator));
            const auto value = QString::fromUtf8(line.sliced(separator + 3));
            auto &stored = result.metadata.rawFields[key];
            if (!stored.isEmpty()) stored += QLatin1Char('\n');
            stored += value;
        }
        for (const auto &depend : fields(pkginfo, QByteArrayLiteral("depend"))) {
            const auto name = dependencyName(depend);
            if (name.isEmpty()) continue;
            DependencyMapping mapping;
            mapping.rawExpression = depend;
            mapping.archPackage = name;
            mapping.status = MappingStatus::Resolved;
            mapping.mappingSource = QStringLiteral("upstream Arch package metadata");
            mapping.confidence = 1.0;
            result.dependencies.append(mapping);
        }
        if (!installScript.isEmpty()) {
            MaintainerScript script{QStringLiteral(".INSTALL"), QString::fromUtf8(installScript), {}};
            result.maintainerScripts.append(script);
            ScriptFinding finding;
            finding.scriptName = script.name;
            finding.kind = QStringLiteral("arch-install-script");
            finding.summary = QStringLiteral("Upstream Arch lifecycle script requires review before translation or reuse.");
            finding.evidenceFingerprint = script.contentFingerprint();
            finding.disposition = ScriptDisposition::Unresolved;
            result.scriptFindings.append(finding);
        }
        result.installMapping.archiveLayout = ArchiveLayout::PreserveRoot;
    } else {
        inferNameVersion(path, result.metadata);
        result.installMapping.archiveLayout = recognizedRoot ? ArchiveLayout::PreserveRoot
                                                             : ArchiveLayout::OptBundle;
        result.installMapping.optDirectory = result.metadata.package;
        if (!result.installMapping.binarySourcePath.isEmpty()) {
            const auto name = QFileInfo(result.installMapping.binarySourcePath).fileName();
            result.installMapping.binaryDestination = QStringLiteral("/usr/bin/%1").arg(name);
            result.installMapping.executableLinks.append(name);
        }
        QString commonPrefix;
        bool sharedPrefix = !result.payload.isEmpty();
        for (const auto &payload : std::as_const(result.payload)) {
            const auto slash = payload.path.indexOf(QLatin1Char('/'));
            const auto prefix = slash < 0 ? payload.path : payload.path.left(slash);
            if (commonPrefix.isEmpty()) commonPrefix = prefix;
            else if (prefix != commonPrefix) {
                sharedPrefix = false;
                break;
            }
        }
        if (sharedPrefix && !commonPrefix.isEmpty()) {
            result.installMapping.commonPrefix = commonPrefix;
            result.installMapping.stripCommonPrefix =
                result.installMapping.archiveLayout == ArchiveLayout::OptBundle;
        }
    }
    if (!iconCandidates.isEmpty()) {
        const auto selected = std::max_element(
            iconCandidates.cbegin(), iconCandidates.cend(), [&](const auto &left, const auto &right) {
                return iconScore(left, desktopIconReferences, result.metadata.package) <
                       iconScore(right, desktopIconReferences, result.metadata.package);
            });
        result.icon = ExtractedSourceIcon{selected->path, selected->contents};
        result.installMapping.icon.sourceKind = IconSourceKind::Payload;
        result.installMapping.icon.sourcePath = selected->path;
        result.installMapping.icon.sha256 = sha256Hex(selected->contents);
        result.installMapping.icon.format = QFileInfo(selected->path).suffix().toLower();
        result.installMapping.icon.iconName = result.metadata.package;
        result.installMapping.icon.provenance.origin = ValueOrigin::Deterministic;
        result.installMapping.icon.provenance.rationale = QStringLiteral(
            "Best matching icon selected from inspected payload and desktop references");
    }
    if (!archPackage && result.installMapping.archiveLayout == ArchiveLayout::OptBundle) {
        const auto command = result.installMapping.launchers.isEmpty()
            ? QString{} : result.installMapping.launchers.first().commandName;
        const auto iconName = result.installMapping.icon.iconName;
        for (auto &desktop : result.installMapping.desktopEntries) {
            desktop.contents = normalizedDesktopContents(desktop.contents, command, iconName);
        }
    }
    return result;
}

std::optional<SourceAnalysis> analyzeElf(const std::filesystem::path &path, QString *error) {
    QFile file(qPath(path));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return std::nullopt;
    }
    const auto header = file.read(64);
    if (header.size() < 20 || !header.startsWith(QByteArrayView{"\x7f" "ELF", 4})) {
        if (error != nullptr) *error = QStringLiteral("Source is not a usable ELF executable");
        return std::nullopt;
    }
    const bool littleEndian = static_cast<unsigned char>(header.at(5)) == 1U;
    const auto first = static_cast<unsigned char>(header.at(18));
    const auto second = static_cast<unsigned char>(header.at(19));
    const auto machine = littleEndian ? static_cast<unsigned>(first | (second << 8U))
                                      : static_cast<unsigned>((first << 8U) | second);
    SourceAnalysis result;
    result.type = SourcePackageType::ElfBinary;
    inferNameVersion(path, result.metadata);
    result.metadata.architecture = machine == 62U ? QStringLiteral("amd64")
                                   : machine == 183U ? QStringLiteral("arm64")
                                                     : QStringLiteral("unknown");
    if (result.metadata.architecture == QStringLiteral("unknown")) {
        if (error != nullptr) *error = QStringLiteral("Unsupported ELF machine type %1").arg(machine);
        return std::nullopt;
    }
    const auto filename = QFileInfo(qPath(path)).fileName();
    result.installMapping.binarySourcePath = filename;
    result.installMapping.binaryDestination = QStringLiteral("/usr/bin/%1").arg(result.metadata.package);
    LauncherMapping launcher;
    launcher.sourcePath = filename;
    launcher.commandName = result.metadata.package;
    launcher.destination = result.installMapping.binaryDestination;
    launcher.sourceFingerprint = sha256Hex(header);
    launcher.provenance.origin = ValueOrigin::Deterministic;
    launcher.provenance.rationale = QStringLiteral("Standalone executable selected as the package command");
    result.installMapping.launchers.append(std::move(launcher));
    result.payload.append({filename, QStringLiteral("file"), {}, file.size(), false, {}, {}, {}, false});
    return result;
}

std::optional<qint64> appImageSquashfsOffset(const std::filesystem::path &path,
                                             QString *error) {
    QFile file(qPath(path));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return std::nullopt;
    }
    constexpr qint64 scanLimit = 128LL * 1024 * 1024;
    constexpr qint64 chunkSize = 1024 * 1024;
    QByteArray overlap;
    qint64 position = 0;
    while (position < std::min(file.size(), scanLimit)) {
        const auto chunk = file.read(std::min(chunkSize, scanLimit - position));
        if (chunk.isEmpty()) break;
        const auto combined = overlap + chunk;
        qsizetype from = 0;
        while ((from = combined.indexOf(QByteArrayLiteral("hsqs"), from)) >= 0) {
            const auto candidate = position - overlap.size() + from;
            // SquashFS images are normally aligned, and rejecting implausibly
            // early/unaligned occurrences avoids treating arbitrary data in the
            // ELF runtime as the filesystem superblock.
            if (candidate >= 4096 && candidate % 4 == 0) return candidate;
            ++from;
        }
        overlap = combined.right(3);
        position += chunk.size();
    }
    if (error != nullptr) {
        *error = QStringLiteral(
            "The Type 2 AppImage marker was present, but no bounded SquashFS payload was found");
    }
    return std::nullopt;
}

std::optional<SourceAnalysis> analyzeAppImage(const std::filesystem::path &path,
                                              QString *error,
                                              const ImportProgressCallback &progress) {
    const auto unsquashfs = QStandardPaths::findExecutable(QStringLiteral("unsquashfs"));
    if (unsquashfs.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "Type 2 AppImage inspection requires /usr/bin/unsquashfs from Arch's squashfs-tools package");
        }
        return std::nullopt;
    }
    const auto offset = appImageSquashfsOffset(path, error);
    if (!offset) return std::nullopt;

    QProcess probe;
    probe.setProgram(unsquashfs);
    probe.setArguments({QStringLiteral("-s"), QStringLiteral("-o"),
                        QString::number(*offset), qPath(path)});
    probe.start();
    if (!probe.waitForStarted(5000) || !probe.waitForFinished(30000) ||
        probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not validate the AppImage SquashFS payload: %1")
                         .arg(QString::fromUtf8(probe.readAllStandardError()).trimmed());
        }
        return std::nullopt;
    }

    QTemporaryDir directory(QStringLiteral("pacsmith-appimage-XXXXXX"));
    if (!directory.isValid()) {
        if (error != nullptr) *error = QStringLiteral("Could not create a private AppImage inspection directory");
        return std::nullopt;
    }
    if (progress) progress({ImportStage::ReadingPayloadArchive, 0});
    QProcess extract;
    extract.setProgram(unsquashfs);
    extract.setArguments({QStringLiteral("-no-progress"), QStringLiteral("-no-xattrs"),
                          QStringLiteral("-o"), QString::number(*offset),
                          QStringLiteral("-d"), directory.path(), qPath(path)});
    extract.start();
    if (!extract.waitForStarted(5000) || !extract.waitForFinished(120000) ||
        extract.exitStatus() != QProcess::NormalExit || extract.exitCode() != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Static AppImage extraction failed: %1")
                         .arg(QString::fromUtf8(extract.readAllStandardError()).trimmed());
        }
        return std::nullopt;
    }

    SourceAnalysis result;
    result.type = SourcePackageType::AppImage;
    inferNameVersion(path, result.metadata);
    result.installMapping.archiveLayout = ArchiveLayout::OptBundle;
    result.installMapping.optDirectory = result.metadata.package;
    result.installMapping.appImageOffset = *offset;
    QSet<QString> desktopReferences;
    QList<IconCandidate> icons;
    bool hasAppRun = false;
    qint64 expandedSize = 0;
    qsizetype entries = 0;
    std::error_code filesystemError;
    const std::filesystem::path root(directory.path().toUtf8().constData());
    for (std::filesystem::recursive_directory_iterator iterator(
             root, std::filesystem::directory_options::skip_permission_denied, filesystemError), end;
         iterator != end && !filesystemError; iterator.increment(filesystemError)) {
        if (++entries > 100000) {
            if (error != nullptr) *error = QStringLiteral("AppImage contains more than 100,000 entries");
            return std::nullopt;
        }
        const auto relative = iterator->path().lexically_relative(root).generic_string();
        const auto safe = PathSafety::normalizedArchivePath(QString::fromUtf8(relative));
        if (!safe || safe->isEmpty()) {
            if (error != nullptr) *error = QStringLiteral("Unsafe AppImage path: %1").arg(QString::fromUtf8(relative));
            return std::nullopt;
        }
        const auto status = iterator->symlink_status(filesystemError);
        if (filesystemError) break;
        PayloadEntry payload;
        payload.path = *safe;
        if (std::filesystem::is_directory(status)) payload.type = QStringLiteral("directory");
        else if (std::filesystem::is_symlink(status)) {
            payload.type = QStringLiteral("symlink");
            const auto target = std::filesystem::read_symlink(iterator->path(), filesystemError).generic_string();
            const auto targetText = QString::fromUtf8(target);
            if (filesystemError ||
                !PathSafety::safeAppImageSymlinkTarget(payload.path, targetText)) {
                if (error != nullptr) {
                    *error = QStringLiteral("Unsafe AppImage symlink: %1 -> %2")
                                 .arg(payload.path, targetText);
                }
                return std::nullopt;
            }
            payload.symlinkTarget = targetText;
        } else if (std::filesystem::is_regular_file(status)) {
            payload.type = QStringLiteral("file");
            payload.size = static_cast<qint64>(iterator->file_size(filesystemError));
            if (filesystemError) break;
            expandedSize += payload.size;
            if (expandedSize > 32LL * 1024 * 1024 * 1024) {
                if (error != nullptr) *error = QStringLiteral("AppImage expands beyond the 32 GiB safety limit");
                return std::nullopt;
            }
        } else {
            if (error != nullptr) *error = QStringLiteral("AppImage contains unsupported special entry: %1").arg(payload.path);
            return std::nullopt;
        }
        result.payload.append(payload);

        if (payload.type == QStringLiteral("file")) {
            const QFileInfo info(qPath(iterator->path()));
            const bool executable = info.permissions().testAnyFlags(
                QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
            if (payload.path == QStringLiteral("AppRun") && executable) hasAppRun = true;
            if (appImageDesktopCandidatePath(payload.path, payload.size) ||
                isIconCandidate(payload.path, payload.size)) {
                QFile file(info.absoluteFilePath());
                if (file.open(QIODevice::ReadOnly)) {
                    const auto contents = file.read(4 * 1024 * 1024 + 1);
                    if (appImageDesktopCandidatePath(payload.path, payload.size) &&
                        contents.size() <= 256 * 1024 && !contents.contains('\0') &&
                        appImageApplicationDesktopEntry(QString::fromUtf8(contents))) {
                        collectDesktopIconReferences(contents, desktopReferences);
                        DesktopEntryConfiguration desktop;
                        desktop.id = QFileInfo(payload.path).completeBaseName();
                        desktop.enabled = !payload.path.contains(QLatin1Char('/')) &&
                            desktopEntryValue(QString::fromUtf8(contents), QStringLiteral("NoDisplay"))
                                .compare(QStringLiteral("true"), Qt::CaseInsensitive) != 0;
                        desktop.sourcePath = payload.path;
                        desktop.destination = QStringLiteral("/usr/share/applications/%1")
                                                  .arg(QFileInfo(payload.path).fileName());
                        desktop.contents = QString::fromUtf8(contents);
                        desktop.sourceSha256 = sha256Hex(contents);
                        desktop.originalContentsSha256 = desktop.sourceSha256;
                        desktop.provenance.origin = ValueOrigin::Deterministic;
                        desktop.provenance.rationale = QStringLiteral(
                            "Application desktop entry detected at an AppDir integration path");
                        const auto duplicate = std::find_if(
                            result.installMapping.desktopEntries.begin(),
                            result.installMapping.desktopEntries.end(),
                            [&desktop](const auto &candidate) {
                                return candidate.destination == desktop.destination;
                            });
                        if (duplicate == result.installMapping.desktopEntries.end()) {
                            result.installMapping.desktopEntries.append(desktop);
                        } else if (!desktop.sourcePath.contains(QLatin1Char('/')) &&
                                   duplicate->sourcePath.contains(QLatin1Char('/'))) {
                            *duplicate = desktop;
                        }
                    }
                    if (icons.size() < 128 && validIconContents(payload.path, contents)) {
                        icons.append({payload.path, contents});
                    }
                }
            }
        }
        if (progress && entries % 250 == 0) {
            progress({ImportStage::ReadingPayloadArchive, entries});
        }
    }
    if (filesystemError) {
        if (error != nullptr) *error = QString::fromStdString(filesystemError.message());
        return std::nullopt;
    }
    if (!hasAppRun) {
        if (error != nullptr) *error = QStringLiteral("AppImage payload has no executable AppRun entry point");
        return std::nullopt;
    }
    if (!result.installMapping.desktopEntries.isEmpty() &&
        std::none_of(result.installMapping.desktopEntries.cbegin(),
                     result.installMapping.desktopEntries.cend(),
                     [](const auto &desktop) { return desktop.enabled; })) {
        const auto primary = std::max_element(
            result.installMapping.desktopEntries.begin(),
            result.installMapping.desktopEntries.end(),
            [&result](const auto &left, const auto &right) {
                return appImageDesktopScore(left, result.metadata.package) <
                       appImageDesktopScore(right, result.metadata.package);
            });
        primary->enabled = true;
    }
    if (!icons.isEmpty()) {
        const auto selected = std::max_element(
            icons.cbegin(), icons.cend(), [&](const auto &left, const auto &right) {
                return iconScore(left, desktopReferences, result.metadata.package) <
                       iconScore(right, desktopReferences, result.metadata.package);
            });
        result.icon = ExtractedSourceIcon{selected->path, selected->contents};
        auto &icon = result.installMapping.icon;
        icon.sourceKind = IconSourceKind::Payload;
        icon.sourcePath = selected->path;
        icon.sha256 = sha256Hex(selected->contents);
        icon.format = QFileInfo(selected->path).suffix().toLower();
        icon.iconName = result.metadata.package;
        icon.provenance.origin = ValueOrigin::Deterministic;
    }
    QString command;
    QString primaryDesktopPath;
    const auto primaryDesktop = std::max_element(
        result.installMapping.desktopEntries.cbegin(),
        result.installMapping.desktopEntries.cend(),
        [&result](const auto &left, const auto &right) {
            const auto leftScore = left.enabled
                ? appImageDesktopScore(left, result.metadata.package)
                : std::numeric_limits<int>::min();
            const auto rightScore = right.enabled
                ? appImageDesktopScore(right, result.metadata.package)
                : std::numeric_limits<int>::min();
            return leftScore < rightScore;
        });
    if (primaryDesktop != result.installMapping.desktopEntries.cend()) {
        primaryDesktopPath = primaryDesktop->sourcePath;
    }
    const auto commandDesktop = std::max_element(
        result.installMapping.desktopEntries.cbegin(),
        result.installMapping.desktopEntries.cend(),
        [&result](const auto &left, const auto &right) {
            const auto leftScore = desktopEntryCommand(left.contents).isEmpty()
                ? std::numeric_limits<int>::min()
                : appImageDesktopScore(left, result.metadata.package);
            const auto rightScore = desktopEntryCommand(right.contents).isEmpty()
                ? std::numeric_limits<int>::min()
                : appImageDesktopScore(right, result.metadata.package);
            return leftScore < rightScore;
        });
    if (commandDesktop != result.installMapping.desktopEntries.cend()) {
        command = desktopEntryCommand(commandDesktop->contents);
    }
    if (command.isEmpty()) command = result.metadata.package;
    result.installMapping.desktopEntries.removeIf(
        [&command, &primaryDesktopPath](const auto &desktop) {
        if (desktop.sourcePath == primaryDesktopPath) return false;
        const auto candidateCommand = desktopEntryCommand(desktop.contents);
        return candidateCommand.isEmpty() || candidateCommand != command;
    });
    LauncherMapping launcher;
    launcher.enabled = true;
    launcher.sourcePath = QStringLiteral("AppRun");
    launcher.commandName = command;
    launcher.destination = QStringLiteral("/usr/bin/%1").arg(command);
    launcher.kind = LauncherKind::Wrapper;
    launcher.provenance.origin = ValueOrigin::Deterministic;
    launcher.provenance.rationale = QStringLiteral(
        "PacSmith host command launches the AppImage-provided AppRun entry point");
    result.installMapping.launchers.append(launcher);
    result.installMapping.binarySourcePath = launcher.sourcePath;
    result.installMapping.binaryDestination = launcher.destination;
    for (auto &desktop : result.installMapping.desktopEntries) {
        desktop.contents = normalizedDesktopContents(
            desktop.contents, command, result.installMapping.icon.iconName);
    }
    return result;
}

} // namespace

std::optional<SourcePackageType> SourceAnalyzer::detect(const std::filesystem::path &path,
                                                        QString *error) {
    QFile file(qPath(path));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return std::nullopt;
    }
    const auto magic = file.read(12);
    const auto fileName = QFileInfo(qPath(path)).fileName();
    const bool appImageMagic = magic.size() >= 11 && magic.startsWith(QByteArrayView{"\x7f" "ELF", 4}) &&
                               magic.at(8) == 'A' && magic.at(9) == 'I' &&
                               (magic.at(10) == '\x01' || magic.at(10) == '\x02');
    if (appImageMagic) {
        if (magic.at(10) == '\x02') return SourcePackageType::AppImage;
        if (error != nullptr) *error = QStringLiteral("Type 1 AppImages are not supported; PacSmith currently decomposes Type 2 SquashFS AppImages only");
        return std::nullopt;
    }
    if (fileName.endsWith(QStringLiteral(".AppImage"), Qt::CaseInsensitive)) {
        if (error != nullptr) *error = QStringLiteral("The .AppImage extension is present, but the file has no valid AppImage type marker");
        return std::nullopt;
    }
    if (magic.startsWith(QByteArrayView{"\x7f" "ELF", 4})) return SourcePackageType::ElfBinary;
    if (magic.startsWith(QByteArrayLiteral("!<arch>\n"))) return SourcePackageType::Debian;
    if (magic.size() >= 4 &&
        static_cast<unsigned char>(magic.at(0)) == 0xedU &&
        static_cast<unsigned char>(magic.at(1)) == 0xabU &&
        static_cast<unsigned char>(magic.at(2)) == 0xeeU &&
        static_cast<unsigned char>(magic.at(3)) == 0xdbU) {
        return SourcePackageType::Rpm;
    }
    QString archiveDiagnostic;
    auto reader = openArchive(path, &archiveDiagnostic);
    if (!reader) {
        if (error != nullptr) {
            *error = QStringLiteral("Artifact '%1' is not a recognized DEB, RPM, Arch package, Type 2 AppImage, supported archive, or standalone ELF file. Header bytes: %2.%3")
                         .arg(fileName, QString::fromLatin1(magic.toHex(' ')),
                              archiveDiagnostic.isEmpty()
                                  ? QString{} : QStringLiteral(" libarchive: %1").arg(archiveDiagnostic));
        }
        return std::nullopt;
    }
    archive_entry *entry = nullptr;
    while (archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK) {
        const auto normalized = PathSafety::normalizedArchivePath(
            QString::fromUtf8(archive_entry_pathname(entry)));
        if (normalized && *normalized == QStringLiteral(".PKGINFO")) {
            return SourcePackageType::ArchPackage;
        }
        archive_read_data_skip(reader.get());
    }
    return SourcePackageType::Archive;
}

std::optional<SourceAnalysis> SourceAnalyzer::analyze(const std::filesystem::path &path,
                                                      QString *error,
                                                      const ImportProgressCallback &progress) {
    const auto type = detect(path, error);
    if (!type) return std::nullopt;
    if (*type == SourcePackageType::Debian) {
        if (error != nullptr) *error = QStringLiteral("DEB sources are handled by DebAnalyzer");
        return std::nullopt;
    }
    if (*type == SourcePackageType::AppImage) return analyzeAppImage(path, error, progress);
    if (*type == SourcePackageType::ElfBinary) return analyzeElf(path, error);
    auto result = analyzeArchive(path, *type == SourcePackageType::ArchPackage, error, progress);
    if (!result) return std::nullopt;
    if (*type == SourcePackageType::Rpm) {
        const auto rpm = RpmAnalyzer::analyzeHeader(path, error);
        if (!rpm) return std::nullopt;
        result->type = SourcePackageType::Rpm;
        result->metadata = rpm->metadata;
        result->dependencies = rpm->dependencies;
        result->maintainerScripts = rpm->maintainerScripts;
        result->scriptFindings = rpm->scriptFindings;
        for (auto &entry : result->payload) {
            const auto capability = rpm->fileCapabilities.value(entry.path);
            if (capability.isEmpty()) continue;
            if (!entry.reviewReason.isEmpty()) entry.reviewReason += QStringLiteral("; ");
            entry.reviewReason += QStringLiteral("Linux file capabilities '%1' require review")
                                      .arg(capability);
            entry.requiresReview = true;
            const auto existingRule = std::find_if(
                result->payloadRules.cbegin(), result->payloadRules.cend(),
                [&](const auto &rule) { return rule.path == entry.path; });
            if (existingRule == result->payloadRules.cend()) {
                result->payloadRules.append(
                    {entry.path, false, entry.reviewReason, false, {}});
            }
        }
        const auto scriptEvidence = ScriptEvidenceAnalyzer::analyze(result->maintainerScripts);
        result->rpmCandidates.append(scriptEvidence.rpmCandidates);
        result->aptCandidates.append(scriptEvidence.aptCandidates);
        result->signingKeys.append(scriptEvidence.signingKeys);
        for (const auto &script : std::as_const(result->maintainerScripts)) {
            result->updateCandidates.append(PathSafety::urlsFromText(script.contents));
        }
        result->installMapping.archiveLayout = ArchiveLayout::PreserveRoot;
    }
    result->updateCandidates.removeDuplicates();
    result->updateCandidates.sort();
    deduplicateEvidence(*result);
    return result;
}

} // namespace pacsmith
